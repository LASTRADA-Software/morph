// SPDX-License-Identifier: Apache-2.0
#include <Lightweight/DataMapper/DataMapper.hpp>
#include <Lightweight/SqlError.hpp>
#include <Lightweight/SqlTransaction.hpp>
#include <algorithm>
#include <core/async/Task.hpp>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "lims/inventory/authorization/inventory_authorization.hpp"
#include "lims/inventory/core/fault_hook.hpp"
#include "lims/inventory/core/stock_math.hpp"
#include "lims/inventory/db/inventory_entity.hpp"
#include "lims/inventory/db/inventory_store.hpp"
#include "lims/inventory/models/stock_model.hpp"
#include "lims/org/core/calendar.hpp"
#include "lims/org/core/errors.hpp"
#include "lims/org/core/model_support.hpp"
#include "lims/org/core/money.hpp"
#include "lims/org/models/org_model.hpp"
#include "lims/posting/ledger_port.hpp"

namespace lims {

namespace {

using ::morph::math::DecimalPlaces;
using ::morph::math::Denominator;
using ::morph::math::Numerator;
using ::morph::math::Rational;

/// @brief A recipe row as the wire sees it.
/// @param row The row.
/// @param itemCode The item's code.
/// @param unit The item's unit.
/// @return The view.
[[nodiscard]] RecipeView recipeView(db::RecipeRecord const& row, std::string const& itemCode, StockUnit unit) {
    return RecipeView{
        .id = RecipeId{static_cast<std::int64_t>(row.id.Value())},
        .company = CompanyId{row.companyId.Value()},
        .analysis = AnalysisId{row.analysisId.Value()},
        .item = ItemId{row.itemId.Value()},
        .itemCode = itemCode,
        .quantity = Measure{Rational{Numerator{row.quantityNum.Value()}, Denominator{row.quantityDen.Value()},
                                     DecimalPlaces{6}}},
        .unit = unit,
    };
}

/// @brief The recipe row of @p action's company, analysis and item.
/// @param mapper The mapper to read through.
/// @param action The recipe.
/// @return It, or nothing.
[[nodiscard]] std::optional<db::RecipeRecord> recipeRow(Lightweight::DataMapper& mapper, SetRecipe const& action) {
    auto rows = mapper.Query<db::RecipeRecord>()
                    .Where(::Lightweight::FieldNameOf<&db::RecipeRecord::companyId>, "=", *action.company)
                    .Where(::Lightweight::FieldNameOf<&db::RecipeRecord::analysisId>, "=", *action.analysis)
                    .Where(::Lightweight::FieldNameOf<&db::RecipeRecord::itemId>, "=", *action.item)
                    .All();
    return rows.empty() ? std::nullopt : std::optional{rows.front()};
}

/// @brief One attempt at storing a recipe line, in its own transaction.
/// @param action The recipe.
/// @param quantity The quantity in the item's unit.
/// @return The row as stored.
/// @throws Lightweight::SqlException if the write fails.
[[nodiscard]] db::RecipeRecord storeRecipeOnce(SetRecipe const& action, Rational const& quantity) {
    // The lookup runs on a connection of its own, outside the write's transaction: a read inside it would hold a
    // lock that blocks a concurrent writer's commit.
    auto const existing = [&] {
        Lightweight::DataMapper lookup;
        return recipeRow(lookup, action);
    }();
    Lightweight::DataMapper mapper;
    Lightweight::SqlTransaction sqlTxn{mapper.Connection(), Lightweight::SqlTransactionMode::ROLLBACK};
    db::RecipeRecord row = existing.value_or(db::RecipeRecord{});
    row.companyId = *action.company;
    row.analysisId = *action.analysis;
    row.itemId = *action.item;
    row.quantityNum = quantity.numerator;
    row.quantityDen = quantity.denominator;
    if (existing.has_value()) {
        mapper.Update(row);
    } else {
        mapper.Create(row);
    }
    sqlTxn.Commit();
    return row;
}

/// @brief The consumed-event row of @p eventId.
/// @param mapper The mapper to read through.
/// @param eventId The event's id.
/// @return It, or nothing when no delivery of the event has committed.
[[nodiscard]] std::optional<db::ConsumptionEventRecord> consumedRow(Lightweight::DataMapper& mapper,
                                                                    std::string const& eventId) {
    auto rows = mapper.Query<db::ConsumptionEventRecord>()
                    .Where(::Lightweight::FieldNameOf<&db::ConsumptionEventRecord::eventId>, "=", eventId)
                    .All();
    return rows.empty() ? std::nullopt : std::optional{rows.front()};
}

/// @brief What the event @p consumed took out of stock, read through @p mapper.
/// @param mapper The mapper to read through.
/// @param consumed The event's row.
/// @return The consumption.
/// @throws NotFound if a movement's item does not exist.
/// @throws ValidationError if a column holds a value outside its enumeration.
[[nodiscard]] ConsumptionView consumptionView(Lightweight::DataMapper& mapper,
                                              db::ConsumptionEventRecord const& consumed) {
    std::string const eventId{consumed.eventId.Value().ToStringView()};
    auto rows = mapper.Query<db::MovementRecord>()
                    .Where(::Lightweight::FieldNameOf<&db::MovementRecord::eventId>, "=", eventId)
                    .All();
    std::ranges::sort(rows, {}, [](db::MovementRecord const& row) { return row.id.Value(); });
    ConsumptionView view{
        .eventId = eventId,
        .sample = SampleId{consumed.sampleId.Value()},
        .movements = {},
        .shortfall = Measure{Rational{Numerator{consumed.shortfallNum.Value()},
                                      Denominator{consumed.shortfallDen.Value()}, DecimalPlaces{6}}},
    };
    std::map<std::int64_t, StockUnit> units;
    for (auto const& row : rows) {
        auto const unit = units.find(row.itemId.Value());
        if (unit == units.end()) {
            auto const item = inventory::findItem(mapper, row.itemId.Value());
            if (!item.has_value()) {
                throw NotFound{"item " + std::to_string(row.itemId.Value()) + " does not exist"};
            }
            units.emplace(row.itemId.Value(), inventory::stockUnitOf(*item));
        }
        view.movements.push_back(inventory::movementView(row, units.at(row.itemId.Value())));
    }
    return view;
}

/// @brief The consumption an earlier delivery of @p eventId made.
/// @param eventId The event's id.
/// @return It, or nothing when no delivery of the event has committed.
[[nodiscard]] std::optional<ConsumptionView> consumptionOfEvent(std::string const& eventId) {
    // A read of its own, outside the write's transaction: a read inside it would hold a lock that blocks a concurrent
    // writer's commit.
    Lightweight::DataMapper mapper;
    auto const consumed = consumedRow(mapper, eventId);
    if (!consumed.has_value()) {
        return std::nullopt;
    }
    return consumptionView(mapper, *consumed);
}

/// @brief What @p recipe takes from the lots of its item at the event's site: first-expiring first, an expired lot
///        skipped, the rest a shortfall movement.
/// @param mapper The caller's mapper, inside its transaction, after the consumed-event row was written.
/// @param action The event.
/// @param recipe The recipe line.
/// @param currency The company's currency, which the lots' costs are in.
/// @param cost Receives the cost of the stock taken, each lot's rounded half-up to @p currency.
/// @param shortfall Receives what the lots could not cover, added to what it holds.
/// @throws Conflict if a lot moved since it was read.
/// @throws OutOfRange if a quantity or the cost does not fit.
/// @throws NotFound if the recipe's item does not exist.
void takeRecipeLine(Lightweight::DataMapper& mapper, RecordConsumption const& action, db::RecipeRecord const& recipe,
                    CurrencyCode currency, ExactSum& cost, Rational& shortfall) {
    auto const item = inventory::findItem(mapper, recipe.itemId.Value());
    if (!item.has_value()) {
        throw NotFound{"RecordConsumption: item " + std::to_string(recipe.itemId.Value()) + " does not exist"};
    }
    std::int64_t const capturedAt = millisOf(action.capturedAt);
    auto lots = mapper.Query<db::LotRecord>()
                    .Where(::Lightweight::FieldNameOf<&db::LotRecord::itemId>, "=", recipe.itemId.Value())
                    .Where(::Lightweight::FieldNameOf<&db::LotRecord::siteId>, "=", *action.site)
                    .All();
    // A lot is usable through its expiry instant.
    std::erase_if(lots, [capturedAt](db::LotRecord const& lot) {
        return lot.onHandNum.Value() <= 0 || lot.expiresAt.Value() < capturedAt;
    });
    std::ranges::sort(lots, {},
                      [](db::LotRecord const& lot) { return std::tuple{lot.expiresAt.Value(), lot.id.Value()}; });
    std::string const principal = currentPrincipal();
    Rational remaining{Numerator{recipe.quantityNum.Value()}, Denominator{recipe.quantityDen.Value()},
                       DecimalPlaces{6}};
    for (auto const& lot : lots) {
        if (remaining.numerator <= 0) {
            break;
        }
        Rational const onHand{Numerator{lot.onHandNum.Value()}, Denominator{lot.onHandDen.Value()}, DecimalPlaces{6}};
        Rational const taken = remaining < onHand ? remaining : onHand;
        static_cast<void>(inventory::moveLot(mapper, lot, MovementKind::Consumption, -taken, action.eventId, principal,
                                             action.eventId));
        Rational const unitCost{Numerator{lot.unitCostNum.Value()}, Denominator{lot.unitCostDen.Value()},
                                DecimalPlaces{decimalsOf(currency)}};
        auto const product = ::morph::math::checkedMul(taken, unitCost);
        if (!product.has_value() || !cost.add(roundToCurrency(*product, currency))) {
            throw OutOfRange{"RecordConsumption: the cost of the stock taken does not fit"};
        }
        auto const left = ::morph::math::checkedSub(remaining, taken);
        if (!left.has_value()) {
            throw OutOfRange{"RecordConsumption: the quantity left to take does not fit"};
        }
        remaining = *left;
    }
    if (remaining.numerator <= 0) {
        return;
    }
    // A shortfall names no lot, so `moveLot`, which moves one, is not used: only the movement row is written.
    db::MovementRecord movement;
    movement.companyId = recipe.companyId.Value();
    movement.itemId = recipe.itemId.Value();
    movement.kind = static_cast<int>(MovementKind::Shortfall);
    movement.quantityNum = (-remaining).numerator;
    movement.quantityDen = remaining.denominator;
    movement.at = nowMillis();
    movement.by = Lightweight::SqlAnsiString<64>{principal};
    movement.source = Lightweight::SqlAnsiString<128>{action.eventId};
    movement.eventId = Lightweight::SqlAnsiString<128>{action.eventId};
    mapper.Create(movement);
    auto const total = ::morph::math::checkedAdd(shortfall, remaining);
    if (!total.has_value()) {
        throw OutOfRange{"RecordConsumption: the shortfall does not fit"};
    }
    shortfall = *total;
}

/// @brief Writes the consumption of @p action and records its event id, in one transaction. In
///        `PostingMode::SharedTransaction` the ledger's entry is written through the same transaction.
/// @param action The event.
/// @param company The company whose stock and books it is.
/// @return The consumption.
/// @throws Lightweight::SqlException if the write fails, which includes the event id being recorded already.
/// @throws Conflict if a lot moved since it was read.
/// @throws ValidationError if the cost has no ledger to be posted to.
[[nodiscard]] ConsumptionView writeConsumption(RecordConsumption const& action, CompanyId company) {
    // Read before the transaction opens, which opens with its first write.
    auto const owner = OrgModel::findCompany(company);
    if (!owner.has_value()) {
        throw NotFound{"RecordConsumption: company " + std::to_string(*company) + " does not exist"};
    }
    auto const settings = StockModel::settingsOf(company);
    Lightweight::DataMapper mapper;
    Lightweight::SqlTransaction sqlTxn{mapper.Connection(), Lightweight::SqlTransactionMode::ROLLBACK};
    db::ConsumptionEventRecord consumed;
    consumed.eventId = Lightweight::SqlAnsiString<128>{action.eventId};
    consumed.companyId = *company;
    consumed.sampleId = *action.sample;
    // The first write: a delivery that lost the race for this event is refused here, before any stock moves.
    mapper.Create(consumed);
    auto recipes = mapper.Query<db::RecipeRecord>()
                       .Where(::Lightweight::FieldNameOf<&db::RecipeRecord::companyId>, "=", *company)
                       .Where(::Lightweight::FieldNameOf<&db::RecipeRecord::analysisId>, "=", *action.analysis)
                       .All();
    std::ranges::sort(recipes, {}, [](db::RecipeRecord const& row) { return row.id.Value(); });
    ExactSum cost;
    Rational shortfall{0, DecimalPlaces{6}};
    for (auto const& recipe : recipes) {
        takeRecipeLine(mapper, action, recipe, owner->baseCurrency, cost, shortfall);
    }
    consumed.shortfallNum = shortfall.numerator;
    consumed.shortfallDen = shortfall.denominator;
    Rational const total = cost.total();
    if (total.numerator > 0) {
        consumed.costNum = total.numerator;
        consumed.costDen = total.denominator;
        auto const& port = ledgerPort();
        if (!port.post || !port.postIn) {
            throw ValidationError{"RecordConsumption: no ledger is wired to post to"};
        }
        if (!settings.has_value()) {
            throw NotFound{"RecordConsumption: company " + std::to_string(*company) + " has no inventory settings"};
        }
        if (port.mode == PostingMode::SharedTransaction) {
            auto const posted = port.postIn(mapper, consumptionPostingFor(ConsumptionToPost{
                                                        .company = company,
                                                        .currency = owner->baseCurrency,
                                                        .date = action.capturedAt,
                                                        .eventId = action.eventId,
                                                        .cost = total,
                                                        .consumptionAccount = settings->consumptionAccount,
                                                        .stockAccount = settings->stockAccount,
                                                    }));
            consumed.entryId = *posted.entryId;
        }
    }
    mapper.Update(consumed);
    sqlTxn.Commit();
    return consumptionView(mapper, consumed);
}

/// @brief The cost of the consumption @p eventId whose ledger entry is not recorded yet.
/// @param eventId The event's id.
/// @param currency The company's currency, which the cost is in.
/// @return The cost; nothing when the event has no cost or its entry is recorded.
[[nodiscard]] std::optional<Rational> unpostedCost(std::string const& eventId, CurrencyCode currency) {
    Lightweight::DataMapper mapper;
    auto const consumed = consumedRow(mapper, eventId);
    if (!consumed.has_value() || consumed->costNum.Value() <= 0 || consumed->entryId.Value().has_value()) {
        return std::nullopt;
    }
    return Rational{Numerator{consumed->costNum.Value()}, Denominator{consumed->costDen.Value()},
                    DecimalPlaces{decimalsOf(currency)}};
}

/// @brief Records the ledger's entry for the consumption @p eventId, when none is recorded.
///
/// Conditional on the entry being empty. A concurrent delivery that posted the same event was answered the same entry
/// by the ledger, so losing the update leaves the row as it should be.
/// @param eventId The event's id.
/// @param entry The ledger's entry.
void recordEntry(std::string const& eventId, JournalEntryId entry) {
    Lightweight::DataMapper mapper;
    auto update = mapper.Connection().Query(db::ConsumptionEventRecord::TableName).Update();
    update.Set("entry_id", *entry);
    (void)update.Where("event_id", "=", eventId).WhereNull("entry_id");
    static_cast<void>(inventory::changedRows(mapper, update.ToSql()));
}

}  // namespace

// An action handler: the registry calls it on the instance, like every other.
// NOLINTNEXTLINE(readability-convert-member-functions-to-static)
RecipeView StockModel::execute(const SetRecipe& action) {
    requirePrincipal();
    if (!action.validate()) {
        throw ValidationError{"SetRecipe: a company, an analysis, an item and a quantity are required"};
    }
    if (!OrgModel::findCompany(action.company).has_value()) {
        throw NotFound{"SetRecipe: company " + std::to_string(*action.company) + " does not exist"};
    }
    if (!StockModel::settingsOf(action.company).has_value()) {
        // A consumption with a cost is posted with the settings' accounts; a recipe without them would make every
        // event of the analysis refused.
        throw NotFound{"SetRecipe: company " + std::to_string(*action.company) + " has no inventory settings"};
    }
    Lightweight::DataMapper reader;
    auto const item = inventory::findItem(reader, *action.item);
    if (!item.has_value() || item->companyId.Value() != *action.company) {
        throw NotFound{"SetRecipe: item " + std::to_string(*action.item) + " is not one of company " +
                       std::to_string(*action.company) + "'s"};
    }
    auto const unit = inventory::stockUnitOf(*item);
    auto const quantity = convertStock(*action.quantity, action.unit, unit);
    if (quantity.numerator <= 0) {
        throw ValidationError{"SetRecipe: the quantity must be positive"};
    }
    std::string const code{item->code.Value().ToStringView()};
    try {
        return recipeView(storeRecipeOnce(action, quantity), code, unit);
    } catch (Lightweight::SqlException const&) {
        // A concurrent first call for the recipe inserted its row between this call's lookup and its insert, and this
        // call's transaction is rolled back. Now the row is there, so this call replaces it, as if it had come after.
        // A failure that is not that race finds no row and surfaces as it is.
        Lightweight::DataMapper mapper;
        if (recipeRow(mapper, action).has_value()) {
            return recipeView(storeRecipeOnce(action, quantity), code, unit);
        }
        throw;
    }
}

// An action handler: the registry calls it on the instance, like every other.
// NOLINTNEXTLINE(readability-convert-member-functions-to-static)
ListRecipesResult StockModel::execute(const ListRecipes& action) {
    if (!action.validate()) {
        throw ValidationError{"ListRecipes: a company is required"};
    }
    ListRecipesResult result;
    if (!OrgModel::callerSees(action.company)) {
        return result;
    }
    Lightweight::DataMapper mapper;
    for (auto const& row : mapper.Query<db::RecipeRecord>()
                               .Where(::Lightweight::FieldNameOf<&db::RecipeRecord::companyId>, "=", *action.company)
                               .All()) {
        auto const item = inventory::findItem(mapper, row.itemId.Value());
        if (!item.has_value()) {
            throw NotFound{"ListRecipes: item " + std::to_string(row.itemId.Value()) + " does not exist"};
        }
        result.recipes.push_back(
            recipeView(row, std::string{item->code.Value().ToStringView()}, inventory::stockUnitOf(*item)));
    }
    std::ranges::sort(result.recipes, {},
                      [](RecipeView const& view) { return std::tuple{*view.analysis, view.itemCode}; });
    return result;
}

// An action handler: the registry calls it on the instance, like every other.
// NOLINTNEXTLINE(readability-convert-member-functions-to-static)
::core::async::Task<ConsumptionView> StockModel::execute(RecordConsumption action) {
    requirePrincipal();
    if (!action.validate()) {
        throw ValidationError{"RecordConsumption: event '" + action.eventId +
                              "' does not name everything a consumption needs"};
    }
    requireServicePrincipal("RecordConsumption");
    requireRoleSomewhere<RecordConsumption>();
    auto const site = OrgModel::findSite(action.site);
    if (!site.has_value()) {
        throw NotFound{"RecordConsumption: site " + std::to_string(*action.site) + " does not exist"};
    }
    requireAuthorized(action);
    auto const company = site->companyId;
    // Everything that needs the dispatch's session runs before the first await: a resumption may not run inside it.
    auto view = storeConsumption(action, company, {});
    auto const owner = OrgModel::findCompany(company);
    if (!owner.has_value()) {
        throw NotFound{"RecordConsumption: company " + std::to_string(*company) + " does not exist"};
    }
    // Owed only when the consumption committed without its posting: in `PostingMode::Dispatched` always, and in either
    // mode after a first delivery that died between the commit and the posting.
    auto const cost = unpostedCost(action.eventId, owner->baseCurrency);
    if (!cost.has_value()) {
        co_return view;
    }
    auto const settings = StockModel::settingsOf(company);
    auto const& port = ledgerPort();
    if (!settings.has_value() || !port.post) {
        throw ValidationError{"RecordConsumption: the consumption of " + action.eventId +
                              " cannot be posted: no ledger is wired, or the company has no inventory settings"};
    }
    PostingRequest request = consumptionPostingFor(ConsumptionToPost{
        .company = company,
        .currency = owner->baseCurrency,
        .date = action.capturedAt,
        .eventId = action.eventId,
        .cost = *cost,
        .consumptionAccount = settings->consumptionAccount,
        .stockAccount = settings->stockAccount,
    });
    {
        Lightweight::DataMapper mapper;
        // Between the consumption's commit and the ledger's entry: a stop here leaves the cost owed.
        inventoryFaultPoint("consumption.afterCommit", mapper.Connection());
    }
    auto const posted = co_await port.post(std::move(request));
    recordEntry(action.eventId, posted.entryId);
    co_return view;
}

ConsumptionView StockModel::storeConsumption(RecordConsumption const& action, CompanyId company,
                                             std::function<void()> const& betweenLookupAndWrite) {
    if (auto const earlier = consumptionOfEvent(action.eventId)) {
        return *earlier;
    }
    if (betweenLookupAndWrite) {
        betweenLookupAndWrite();
    }
    try {
        return writeConsumption(action, company);
    } catch (Lightweight::SqlException const&) {
        // A concurrent delivery of the same event recorded it between this call's lookup and its write, and the
        // unique index refused this one. The winner's consumption is there now, so it is the answer. A failure that is
        // not that race finds no such consumption and surfaces as it is.
        if (auto const winner = consumptionOfEvent(action.eventId)) {
            return *winner;
        }
        throw;
    }
}

}  // namespace lims

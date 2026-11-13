// SPDX-License-Identifier: Apache-2.0
#include "lims/inventory/models/reservation_model.hpp"

#include <Lightweight/DataMapper/DataMapper.hpp>
#include <Lightweight/SqlError.hpp>
#include <Lightweight/SqlTransaction.hpp>
#include <algorithm>
#include <cstdint>
#include <exception>
#include <format>
#include <map>
#include <morph/core/logger.hpp>
#include <optional>
#include <random>
#include <set>
#include <string>
#include <tuple>
#include <vector>

#include "lims/inventory/authorization/inventory_authorization.hpp"
#include "lims/inventory/core/fault_hook.hpp"
#include "lims/inventory/db/inventory_entity.hpp"
#include "lims/inventory/db/inventory_store.hpp"
#include "lims/org/core/calendar.hpp"
#include "lims/org/core/errors.hpp"
#include "lims/org/core/model_support.hpp"
#include "lims/org/core/money.hpp"
#include "lims/org/models/org_model.hpp"

namespace lims {

namespace {

using ::morph::math::DecimalPlaces;
using ::morph::math::Denominator;
using ::morph::math::Numerator;
using ::morph::math::Rational;

/// @brief How many times a bench plans again after losing a lot to a winner that has since let go.
constexpr int kAttempts = 3;

/// @brief How long a reservation lives after the last one.
constexpr std::int64_t kReservationMillis = static_cast<std::int64_t>(60) * 60 * 1000;

/// @brief A token naming one reserving instance: 128 random bits as hex.
/// @return The token.
[[nodiscard]] std::string newToken() {
    std::random_device device;
    std::uniform_int_distribution<std::uint64_t> bits;
    return std::format("{:016x}{:016x}", bits(device), bits(device));
}

/// @brief Whether lot @p lot is no longer as a caller read it, on a connection of its own.
///
/// A write that failed is another bench's doing only if the lot's version moved; a failure that finds it unchanged is
/// not contention and is the caller's to surface as it is.
/// @param lot The lot.
/// @param versionRead The lot's `reservation_version` as the caller read it.
/// @return `true` when the lot is gone or its version differs.
[[nodiscard]] bool lotMovedOn(std::int64_t lot, std::int64_t versionRead) {
    Lightweight::DataMapper reader;
    auto const rows =
        reader.Query<db::LotRecord>().Where(::Lightweight::FieldNameOf<&db::LotRecord::id>, "=", lot).All();
    return rows.empty() || rows.front().reservationVersion.Value() != versionRead;
}

/// @brief What one item needs and the lot chosen to hold it.
struct PlannedLine {
    std::int64_t item = 0;
    std::string itemCode;
    Rational quantity{0, DecimalPlaces{6}};
    db::LotRecord lot;
};

/// @brief The quantity of each item the recipes of @p analyses use at @p company, summed per item.
/// @param reader The mapper to read through.
/// @param company The company.
/// @param analyses The analyses; one named twice counts twice.
/// @return Each item's need, by item id.
/// @throws OutOfRange if a sum does not fit.
[[nodiscard]] std::map<std::int64_t, Rational> needsOf(Lightweight::DataMapper& reader, std::int64_t company,
                                                       std::vector<AnalysisId> const& analyses) {
    std::map<std::int64_t, ExactSum> sums;
    for (auto const& analysis : analyses) {
        for (auto const& recipe : reader.Query<db::RecipeRecord>()
                                      .Where(::Lightweight::FieldNameOf<&db::RecipeRecord::companyId>, "=", company)
                                      .Where(::Lightweight::FieldNameOf<&db::RecipeRecord::analysisId>, "=", *analysis)
                                      .All()) {
            if (!sums[recipe.itemId.Value()].add(Rational{Numerator{recipe.quantityNum.Value()},
                                                          Denominator{recipe.quantityDen.Value()},
                                                          DecimalPlaces{6}})) {
                throw OutOfRange{"ReserveForSample: the quantity an item needs does not fit"};
            }
        }
    }
    std::map<std::int64_t, Rational> needs;
    for (auto const& [item, sum] : sums) {
        auto const total = sum.total();
        needs.emplace(item, Rational{Numerator{total.numerator}, Denominator{total.denominator}, DecimalPlaces{6}});
    }
    return needs;
}

/// @brief What @p token itself holds on lot @p lot for samples other than @p sample.
///
/// This bench's earlier reservation is replaced by the new one, so it must not count against the new one.
/// @param reader The mapper to read through.
/// @param lot The lot.
/// @param token The bench's token.
/// @param sample The sample being reserved.
/// @return The quantity.
/// @throws OutOfRange if the sum does not fit.
[[nodiscard]] Rational heldElsewhereBy(Lightweight::DataMapper& reader, std::int64_t lot, std::string const& token,
                                       std::int64_t sample) {
    ExactSum sum;
    for (auto const& row : inventory::liveReservationsOn(reader, lot)) {
        if (row.token.Value().ToStringView() == token && row.sampleId.Value() != sample &&
            !sum.add(Rational{Numerator{row.quantityNum.Value()}, Denominator{row.quantityDen.Value()},
                              DecimalPlaces{6}})) {
            throw OutOfRange{"ReserveForSample: the reserved quantity does not fit"};
        }
    }
    auto const total = sum.total();
    return Rational{Numerator{total.numerator}, Denominator{total.denominator}, DecimalPlaces{6}};
}

/// @brief The principals, other than this bench, that hold live reservations on any of @p lots.
/// @param reader The mapper to read through.
/// @param lots The lots.
/// @param token The bench's token.
/// @return The holders in the order of the reservations, each once.
[[nodiscard]] std::vector<std::string> holdersOn(Lightweight::DataMapper& reader,
                                                 std::vector<db::LotRecord> const& lots, std::string const& token) {
    std::vector<std::string> holders;
    for (auto const& lot : lots) {
        for (auto const& row : inventory::liveReservationsOn(reader, static_cast<std::int64_t>(lot.id.Value()))) {
            std::string const holder{row.holder.Value().ToStringView()};
            if (row.token.Value().ToStringView() != token && std::ranges::find(holders, holder) == holders.end()) {
                holders.push_back(holder);
            }
        }
    }
    return holders;
}

/// @brief @p holders as `'a' and 'b'`.
/// @param holders The names.
/// @return The text.
[[nodiscard]] std::string joined(std::vector<std::string> const& holders) {
    std::string text;
    for (auto const& holder : holders) {
        text += (text.empty() ? "'" : " and '") + holder + "'";
    }
    return text;
}

/// @brief The lots of @p item at @p site with stock on hand, unexpired, first-expiring first.
/// @param reader The mapper to read through.
/// @param item The item.
/// @param site The site.
/// @return The lots.
[[nodiscard]] std::vector<db::LotRecord> usableLots(Lightweight::DataMapper& reader, std::int64_t item,
                                                    std::int64_t site) {
    auto lots = reader.Query<db::LotRecord>()
                    .Where(::Lightweight::FieldNameOf<&db::LotRecord::itemId>, "=", item)
                    .Where(::Lightweight::FieldNameOf<&db::LotRecord::siteId>, "=", site)
                    .All();
    std::int64_t const now = nowMillis();
    std::erase_if(
        lots, [now](db::LotRecord const& lot) { return lot.onHandNum.Value() <= 0 || lot.expiresAt.Value() < now; });
    std::ranges::sort(lots, {},
                      [](db::LotRecord const& lot) { return std::tuple{lot.expiresAt.Value(), lot.id.Value()}; });
    return lots;
}

/// @brief How one attempt to write a reservation ended.
struct Attempt {
    /// @brief The reservation was written.
    bool committed = false;
    /// @brief It was refused because another bench holds the lot; asking again would be refused the same.
    bool held = false;
    /// @brief The refusal's words, when it was not written.
    std::string words;
};

/// @brief The outcome for a lot another bench moved while this one planned.
/// @param lot The lot's row as read.
/// @param itemCode The item's code.
/// @param token The losing bench's token; its own rows are not who took the lot.
/// @return The refusal naming who holds the lot now; when nobody does (the winner has already let go), a refusal
///         that is not `held`, since planning again may find the lot free.
[[nodiscard]] Attempt lostLot(db::LotRecord const& lot, std::string const& itemCode, std::string const& token) {
    Lightweight::DataMapper reader;
    std::string const number{lot.lotNumber.Value().ToStringView()};
    auto const holders = holdersOn(reader, {lot}, token);
    if (holders.empty()) {
        return Attempt{.committed = false,
                       .held = false,
                       .words = "ReserveForSample: lot " + number + " of " + itemCode + " changed meanwhile"};
    }
    return Attempt{
        .committed = false,
        .held = true,
        .words = "ReserveForSample: lot " + number + " of " + itemCode + " is reserved by " + joined(holders)};
}

/// @brief Bumps the version of the lot of @p line, conditional on the version and stock on hand read.
/// @param mapper The caller's mapper, inside its transaction.
/// @param line The planned line.
/// @return `true` when exactly one row matched.
[[nodiscard]] bool bumpVersion(Lightweight::DataMapper& mapper, PlannedLine const& line) {
    auto query = mapper.Connection().Query(db::LotRecord::TableName).Update();
    query.Set("reservation_version", line.lot.reservationVersion.Value() + 1);
    (void)query.Where("id", "=", line.lot.id.Value())
        .Where("reservation_version", "=", line.lot.reservationVersion.Value())
        .Where("on_hand_num", "=", line.lot.onHandNum.Value())
        .Where("on_hand_den", "=", line.lot.onHandDen.Value());
    return inventory::changedRows(mapper, query.ToSql()) == 1U;
}

/// @brief Chooses, for each item the analyses use, the first-expiring lot that covers it.
/// @param company The sample's company.
/// @param siteId The sample's site.
/// @param sample The sample.
/// @param analyses The analyses.
/// @param token The reserving bench's token.
/// @return The planned lines, by item.
/// @throws Conflict if another bench holds the sample, or no lot covers an item.
/// @throws OutOfRange if a quantity does not fit.
[[nodiscard]] std::vector<PlannedLine> planReservation(std::int64_t company, std::int64_t siteId, std::int64_t sample,
                                                       std::vector<AnalysisId> const& analyses,
                                                       std::string const& token) {
    std::vector<PlannedLine> plan;
    {
        // The reads run on a mapper of their own, outside the write transaction below, which opens with its first
        // write: the write is conditional on what was read here.
        Lightweight::DataMapper reader;
        for (auto const& row : inventory::liveReservationsOfSample(reader, sample)) {
            if (row.token.Value().ToStringView() != token) {
                throw Conflict{"ReserveForSample: sample " + std::to_string(sample) + " is reserved by '" +
                               std::string{row.holder.Value().ToStringView()} + "'"};
            }
        }
        for (auto const& [item, need] : needsOf(reader, company, analyses)) {
            auto const itemRow = inventory::findItem(reader, item);
            std::string const code = itemRow.has_value() ? std::string{itemRow->code.Value().ToStringView()}
                                                         : "item " + std::to_string(item);
            auto const lots = usableLots(reader, item, siteId);
            std::optional<db::LotRecord> chosen;
            for (auto const& lot : lots) {
                auto const free = ::morph::math::checkedAdd(
                    inventory::availableOf(reader, lot, sample),
                    heldElsewhereBy(reader, static_cast<std::int64_t>(lot.id.Value()), token, sample));
                if (!free.has_value()) {
                    throw OutOfRange{"ReserveForSample: the available quantity does not fit"};
                }
                if (!(*free < need)) {
                    chosen = lot;
                    break;
                }
            }
            if (!chosen.has_value()) {
                auto const holders = holdersOn(reader, lots, token);
                if (holders.empty()) {
                    throw Conflict{"ReserveForSample: not enough " + code + " at site " + std::to_string(siteId)};
                }
                auto const taken = *std::ranges::find_if(
                    lots, [&](db::LotRecord const& lot) { return !holdersOn(reader, {lot}, token).empty(); });
                throw Conflict{"ReserveForSample: lot " + std::string{taken.lotNumber.Value().ToStringView()} +
                               " of " + code + " is reserved by " + joined(holders)};
            }
            plan.push_back(PlannedLine{.item = item, .itemCode = code, .quantity = need, .lot = *chosen});
        }
    }
    return plan;
}

/// @brief Writes @p plan as the bench's reservation of @p sample in one transaction, replacing the bench's earlier
///        rows.
/// @param plan The planned lines, with the lots as they were read.
/// @param company The sample's company.
/// @param siteId The sample's site.
/// @param sample The sample.
/// @param principal The holder.
/// @param token The reserving bench's token.
/// @return How it ended.
/// @throws Conflict if another bench holds the sample.
/// @throws Lightweight::SqlException if the write fails for another reason than contention.
[[nodiscard]] Attempt writeReservation(std::vector<PlannedLine> const& plan, std::int64_t company, std::int64_t siteId,
                                       std::int64_t sample, std::string const& principal, std::string const& token) {
    std::int64_t const expiresAt = nowMillis() + kReservationMillis;
    Attempt outcome;
    {
        Lightweight::DataMapper mapper;
        Lightweight::SqlTransaction sqlTxn{mapper.Connection(), Lightweight::SqlTransactionMode::ROLLBACK};
        // A fault point sits between the read and the write, so a test makes a concurrent bench win the lot.
        inventoryFaultPoint("reservation.beforeVersion", mapper.Connection());
        try {
            for (auto const& line : plan) {
                if (!bumpVersion(mapper, line)) {
                    outcome = lostLot(line.lot, line.itemCode, token);
                    break;
                }
            }
            if (!outcome.words.empty()) {
                return outcome;
            }
            // The transaction holds the write lock from its first write, so these reads see what is committed.
            std::set<std::uint64_t> doomed;
            std::int64_t const now = nowMillis();
            for (auto const& row : mapper.Query<db::ReservationRecord>()
                                       .Where(::Lightweight::FieldNameOf<&db::ReservationRecord::token>, "=", token)
                                       .All()) {
                doomed.insert(row.id.Value());
                mapper.Delete(row);
            }
            for (auto const& row :
                 mapper.Query<db::ReservationRecord>()
                     .Where(::Lightweight::FieldNameOf<&db::ReservationRecord::sampleId>, "=", sample)
                     .All()) {
                if (doomed.contains(row.id.Value())) {
                    continue;
                }
                if (row.expiresAt.Value() > now) {
                    throw Conflict{"ReserveForSample: sample " + std::to_string(sample) + " is reserved by '" +
                                   std::string{row.holder.Value().ToStringView()} + "'"};
                }
                mapper.Delete(row);
            }
            for (auto const& line : plan) {
                db::ReservationRecord row;
                row.sampleId = sample;
                row.companyId = company;
                row.siteId = siteId;
                row.lotId = static_cast<std::int64_t>(line.lot.id.Value());
                row.itemId = line.item;
                row.quantityNum = line.quantity.numerator;
                row.quantityDen = line.quantity.denominator;
                row.holder = Lightweight::SqlAnsiString<64>{principal};
                row.token = Lightweight::SqlAnsiString<32>{token};
                row.expiresAt = expiresAt;
                mapper.Create(row);
            }
            sqlTxn.Commit();
            outcome.committed = true;
        } catch (Lightweight::SqlException const& error) {
            // Contention is decided by what the lots say now, not by what the error says: a failure that leaves every
            // planned lot as it was read is a defect and surfaces as itself.
            auto const moved = std::ranges::find_if(plan, [](PlannedLine const& line) {
                return lotMovedOn(static_cast<std::int64_t>(line.lot.id.Value()), line.lot.reservationVersion.Value());
            });
            if (!isBusy(error) && moved == plan.end()) {
                throw;
            }
            outcome = moved != plan.end()
                          ? lostLot(moved->lot, moved->itemCode, token)
                          : Attempt{.committed = false,
                                    .held = false,
                                    .words = "ReserveForSample: another bench was reserving meanwhile"};
        }
    }
    return outcome;
}

}  // namespace

ReservationModel::ReservationModel() : _token{newToken()} {}

ReservationModel::~ReservationModel() {
    if (!_sample.has_value()) {
        return;
    }
    // A destructor runs wherever the last reference drops: a screen's unmount, or the server reclaiming a closed
    // connection's instances. A failure here is logged, never thrown.
    try {
        Lightweight::DataMapper mapper;
        for (auto const& row : mapper.Query<db::ReservationRecord>()
                                   .Where(::Lightweight::FieldNameOf<&db::ReservationRecord::token>, "=", _token)
                                   .All()) {
            mapper.Delete(row);
        }
    } catch (std::exception const& error) {
        ::morph::log::logError(std::string{"[lims] releasing reservation "} + _token + " failed: " + error.what());
    }
}

ReservationView ReservationModel::execute(const ReserveForSample& action) {
    requirePrincipal();
    if (!action.validate()) {
        throw ValidationError{"ReserveForSample: a sample, a site and at least one analysis are required"};
    }
    requireRoleSomewhere<ReserveForSample>();
    auto const site = OrgModel::findSite(action.site);
    if (!site.has_value()) {
        throw NotFound{"ReserveForSample: site " + std::to_string(*action.site) + " does not exist"};
    }
    requireAuthorized(action);
    std::string const principal = currentPrincipal();
    std::int64_t const sample = *action.sample;
    // Each attempt reads on a mapper of its own, outside the write transaction, which opens with its first write: the
    // write is conditional on what was read. A bench that lost a lot to a winner that has already let go plans again,
    // since the lot may be free now; one that lost it to a holder that is still there is refused.
    Attempt outcome;
    for (int attempt = 0; attempt < kAttempts && !outcome.committed && !outcome.held; ++attempt) {
        auto const plan = planReservation(*site->companyId, *action.site, sample, action.analyses, _token);
        outcome = writeReservation(plan, *site->companyId, *action.site, sample, principal, _token);
    }
    if (!outcome.committed) {
        throw Conflict{outcome.words};
    }
    _sample = sample;
    Lightweight::DataMapper after;
    return inventory::reservationView(after, sample, inventory::liveReservationsOfSample(after, sample));
}

ReservationView ReservationModel::execute(const ReleaseReservation& action) {
    static_cast<void>(action);
    requirePrincipal();
    if (!_sample.has_value()) {
        return ReservationView{.sample = {}, .holder = {}, .lots = {}, .expiresAt = {}};
    }
    std::int64_t const sample = *_sample;
    Lightweight::DataMapper mapper;
    for (auto const& row : mapper.Query<db::ReservationRecord>()
                               .Where(::Lightweight::FieldNameOf<&db::ReservationRecord::token>, "=", _token)
                               .All()) {
        mapper.Delete(row);
    }
    _sample.reset();
    // What is left, not what was assumed: another bench may hold the sample's reservation by now.
    return inventory::reservationView(mapper, sample, inventory::liveReservationsOfSample(mapper, sample));
}

}  // namespace lims

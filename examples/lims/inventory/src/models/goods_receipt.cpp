// SPDX-License-Identifier: Apache-2.0
#include <Lightweight/DataMapper/DataMapper.hpp>
#include <Lightweight/SqlError.hpp>
#include <Lightweight/SqlTransaction.hpp>
#include <algorithm>
#include <core/async/Task.hpp>
#include <cstdint>
#include <exception>
#include <format>
#include <map>
#include <morph/util/quantity.hpp>
#include <morph/util/rational.hpp>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "lims/inventory/authorization/inventory_authorization.hpp"
#include "lims/inventory/core/fault_hook.hpp"
#include "lims/inventory/core/stock_math.hpp"
#include "lims/inventory/db/inventory_entity.hpp"
#include "lims/inventory/db/inventory_store.hpp"
#include "lims/inventory/models/procurement_model.hpp"
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

/// @brief The words the ledger refuses a repeated source key with when the body differs.
constexpr std::string_view kOtherBody = "already recorded for a different posting";

/// @brief Whether the caller can see @p company.
/// @param company The company.
/// @return Whether a role of theirs reaches it.
[[nodiscard]] bool visible(CompanyId company) {
    auto const companies = OrgModel::visibleCompanies(currentPrincipal());
    return std::ranges::find(companies, company) != companies.end();
}

/// @brief One receipt line after every check: what it delivers and what it is worth.
struct PlannedLine {
    /// @brief The request's line.
    ReceiptLineInput const* input;
    /// @brief The item.
    db::ItemRecord item;
    /// @brief The quantity in the item's stock unit.
    Rational quantity;
    /// @brief The line's value in the order's currency: the quantity times the order line's unit price, rounded once.
    Rational value;
};

/// @brief An order line the receipt advances.
struct Claim {
    /// @brief The order line's id.
    std::int64_t line;
    /// @brief What it had received, as read: the numerator.
    std::int64_t readNum;
    /// @brief What it had received, as read: the denominator.
    std::int64_t readDen;
    /// @brief What it holds once the receipt is in.
    Rational received;
};

/// @brief Everything the writes of a receipt need, settled before the ledger is called.
struct ReceiptInput {
    /// @brief The order's company.
    CompanyId company;
    /// @brief The order.
    std::int64_t order = 0;
    /// @brief The order's currency.
    CurrencyCode currency = CurrencyCode::EUR;
    /// @brief The order's state as read.
    OrderState orderState = OrderState::Open;
    /// @brief The order's state once the receipt is in.
    OrderState nextState = OrderState::Open;
    /// @brief Where the lots are kept.
    db::LocationRecord location;
    /// @brief The receipt's date.
    ::morph::time::Timestamp date;
    /// @brief The fiscal year of @p date in the company.
    std::int64_t fiscalYear = 0;
    /// @brief Who receives.
    std::string principal;
    /// @brief The lines, in the request's order.
    std::vector<PlannedLine> lines;
    /// @brief The order lines the receipt advances.
    std::vector<Claim> claims;
    /// @brief The lines' exact sum in the order's currency.
    Rational total;
    /// @brief The credit to goods received not invoiced, in the company's currency.
    Rational companyTotal;
    /// @brief Each line's value in the company's currency, in @p lines' order.
    std::vector<Rational> companyLines;
    /// @brief Set when the writes inserted the first number of a fiscal year, so a failure can be told from a lost
    ///        race for it.
    bool startedSeries = false;
};

/// @brief The date as a screen shows it.
/// @param at The instant.
/// @return `09-Nov-2026`.
[[nodiscard]] std::string shown(::morph::time::Timestamp const& at) {
    return std::format("{:%d-%b-%Y}", calendarDayOf(at));
}

/// @brief A quantity as exact decimal text.
/// @param value The quantity.
/// @return The text.
[[nodiscard]] std::string quantityText(Rational const& value) {
    return ::morph::units::toDecimalString(Measure{value});
}

/// @brief Advances each claimed order line by a conditional update on what it had received when it was read.
/// @param mapper The caller's mapper, inside its transaction.
/// @param input The receipt.
/// @throws Conflict if an order line moved on since it was read.
void claimOrderLines(Lightweight::DataMapper& mapper, ReceiptInput const& input) {
    for (Claim const& claim : input.claims) {
        auto update = mapper.Connection().Query(db::OrderLineRecord::TableName).Update();
        update.Set("received_num", claim.received.numerator).Set("received_den", claim.received.denominator);
        (void)update.Where("id", "=", claim.line)
            .Where("received_num", "=", claim.readNum)
            .Where("received_den", "=", claim.readDen);
        if (inventory::changedRows(mapper, update.ToSql()) != 1U) {
            throw Conflict{"ReceiveGoods: order line " + std::to_string(claim.line) + " was received meanwhile"};
        }
    }
}

/// @brief Takes the next number of the company's fiscal year.
/// @param mapper The caller's mapper, inside its transaction.
/// @param input The receipt.
/// @return `GR-<fiscal year>-<six digits>`.
/// @throws Conflict if the series moved on since it was read.
[[nodiscard]] std::string takeNumber(Lightweight::DataMapper& mapper, ReceiptInput& input) {
    auto const series =
        mapper.Query<db::ReceiptNumberRecord>()
            .Where(::Lightweight::FieldNameOf<&db::ReceiptNumberRecord::companyId>, "=", *input.company)
            .Where(::Lightweight::FieldNameOf<&db::ReceiptNumberRecord::fiscalYear>, "=", input.fiscalYear)
            .All();
    std::int64_t const number = series.empty() ? 1 : series.front().next.Value();
    if (series.empty()) {
        // The unique index settles two first receipts of a year: the second insert fails.
        input.startedSeries = true;
        db::ReceiptNumberRecord row;
        row.companyId = *input.company;
        row.fiscalYear = input.fiscalYear;
        row.next = number + 1;
        mapper.Create(row);
    } else {
        // Conditional on the number read, so a database that does not serialise writers still spends each number once.
        auto advance = mapper.Connection().Query(db::ReceiptNumberRecord::TableName).Update();
        advance.Set("next", number + 1);
        (void)advance.Where("company_id", "=", *input.company)
            .Where("fiscal_year", "=", input.fiscalYear)
            .Where("next", "=", number);
        if (inventory::changedRows(mapper, advance.ToSql()) != 1U) {
            throw Conflict{"ReceiveGoods: the receipt number series moved on meanwhile"};
        }
    }
    return std::format("GR-{}-{:06}", input.fiscalYear, number);
}

/// @brief Writes the receipt: its number, its lots and their movements, the order's state. Shared by both posting
///        modes, so the receipt is the same whichever way the ledger was reached.
/// @param mapper The caller's mapper, inside its transaction, after the order lines were claimed.
/// @param input The receipt.
/// @param entry The ledger's entry.
/// @return The receipt's id.
/// @throws Conflict if a lot or the order moved on since it was read.
/// @throws OutOfRange if a lot's cost or stock does not fit.
[[nodiscard]] std::int64_t recordReceipt(Lightweight::DataMapper& mapper, ReceiptInput& input,
                                         std::optional<JournalEntryId> const& entry) {
    db::ReceiptRecord receipt;
    receipt.companyId = *input.company;
    receipt.orderId = input.order;
    receipt.number = Lightweight::SqlAnsiString<32>{takeNumber(mapper, input)};
    receipt.currency = static_cast<int>(input.currency);
    receipt.valueNum = input.total.numerator;
    receipt.valueDen = input.total.denominator;
    receipt.companyValueNum = input.companyTotal.numerator;
    receipt.companyValueDen = input.companyTotal.denominator;
    receipt.entryId = entry.has_value() ? std::optional<std::int64_t>{**entry} : std::nullopt;
    receipt.receivedAt = millisOf(input.date);
    receipt.receivedBy = Lightweight::SqlAnsiString<64>{input.principal};
    mapper.Create(receipt);
    auto const receiptId = static_cast<std::int64_t>(receipt.id.Value());
    for (std::size_t index = 0; index < input.lines.size(); ++index) {
        PlannedLine const& line = input.lines[index];
        // A lot's unit cost is its line's company value over its quantity, exactly.
        auto const cost = ::morph::math::checkedDiv(input.companyLines[index], line.quantity);
        if (!cost.has_value()) {
            throw OutOfRange{"ReceiveGoods: a lot's unit cost does not fit"};
        }
        auto const lot = inventory::createLot(mapper, "ReceiveGoods", line.item, input.location, line.input->lotNumber,
                                              line.input->expiresAt, *cost);
        static_cast<void>(inventory::moveLot(mapper, lot, MovementKind::Receipt, line.quantity,
                                             "receipt:" + std::to_string(receiptId), input.principal));
        db::ReceiptLotRecord link;
        link.receiptId = receiptId;
        link.lotId = static_cast<std::int64_t>(lot.id.Value());
        link.orderLineId = *line.input->orderLine;
        mapper.Create(link);
    }
    auto state = mapper.Connection().Query(db::PurchaseOrderRecord::TableName).Update();
    state.Set("state", static_cast<int>(input.nextState));
    (void)state.Where("id", "=", input.order).Where("state", "=", static_cast<int>(input.orderState));
    if (inventory::changedRows(mapper, state.ToSql()) != 1U) {
        throw Conflict{"ReceiveGoods: order " + std::to_string(input.order) + " changed meanwhile"};
    }
    return receiptId;
}

/// @brief Turns a database failure of the receipt's writes into the `Conflict` it stands for, when it was a lost
///        race, and throws; a failure that is not one surfaces as itself.
/// @param input The receipt.
/// @param error The failure.
/// @param entryRecorded Whether the ledger's entry is already committed.
/// @throws Conflict naming what moved on.
/// @throws Lightweight::SqlException @p error, when nothing moved on.
[[noreturn]] void refuseLostRace(ReceiptInput const& input, Lightweight::SqlException const& error,
                                 bool entryRecorded) {
    Lightweight::DataMapper reread;
    for (PlannedLine const& line : input.lines) {
        if (inventory::lotNumberTaken(static_cast<std::int64_t>(line.item.id.Value()), line.input->lotNumber)) {
            throw Conflict{"ReceiveGoods: lot " + line.input->lotNumber +
                           " was taken meanwhile; receive the delivery again with another lot number"};
        }
    }
    for (Claim const& claim : input.claims) {
        auto const rows = reread.Query<db::OrderLineRecord>()
                              .Where(::Lightweight::FieldNameOf<&db::OrderLineRecord::id>, "=", claim.line)
                              .All();
        if (!rows.empty() &&
            (rows.front().receivedNum.Value() != claim.readNum || rows.front().receivedDen.Value() != claim.readDen)) {
            throw Conflict{"ReceiveGoods: order line " + std::to_string(claim.line) + " was received meanwhile"};
        }
    }
    if (input.startedSeries) {
        auto const series =
            reread.Query<db::ReceiptNumberRecord>()
                .Where(::Lightweight::FieldNameOf<&db::ReceiptNumberRecord::companyId>, "=", *input.company)
                .Where(::Lightweight::FieldNameOf<&db::ReceiptNumberRecord::fiscalYear>, "=", input.fiscalYear)
                .All();
        if (!series.empty()) {
            throw Conflict{"ReceiveGoods: the receipt number series moved on meanwhile"};
        }
    }
    if (isBusy(error)) {
        throw Conflict{"ReceiveGoods: the books were busy; order " + std::to_string(input.order) +
                       (entryRecorded ? " has its entry recorded, receive it again as first sent"
                                      : " was not received, try again")};
    }
    throw error;
}

/// @brief Records the ledger's entry through inventory's own mapper and writes the receipt: both halves in one
///        transaction, committed once.
/// @param port The installed port.
/// @param request The posting.
/// @param input The receipt.
/// @return The receipt's id.
/// @throws Conflict if the order moved on or the database turned this receipt away as busy.
[[nodiscard]] std::int64_t receiveInOneTransaction(LedgerPort const& port, PostingRequest const& request,
                                                   ReceiptInput& input) {
    Lightweight::DataMapper mapper;
    try {
        Lightweight::SqlTransaction sqlTxn{mapper.Connection(), Lightweight::SqlTransactionMode::ROLLBACK};
        // The first write: the order lines, so a second receipt of the last of a line loses before it posts.
        claimOrderLines(mapper, input);
        PostingResult const posted = port.postIn(mapper, request);
        inventoryFaultPoint("receipt.afterLedger", mapper.Connection());
        auto const receiptId = recordReceipt(mapper, input, posted.entryId);
        sqlTxn.Commit();
        return receiptId;
    } catch (Lightweight::SqlException const& error) {
        refuseLostRace(input, error, false);
    }
}

/// @brief Writes the receipt in a transaction of its own: the second commit of a dispatched receipt.
/// @param posted The ledger's entry, already committed.
/// @param input The receipt.
/// @return The receipt's id.
/// @throws Conflict if a lot or the order moved on since it was read, or the database turned this receipt away as
/// busy.
[[nodiscard]] std::int64_t receiveAfterLedger(PostingResult const& posted, ReceiptInput& input) {
    Lightweight::DataMapper mapper;
    inventoryFaultPoint("receipt.afterLedger", mapper.Connection());
    try {
        Lightweight::SqlTransaction sqlTxn{mapper.Connection(), Lightweight::SqlTransactionMode::ROLLBACK};
        claimOrderLines(mapper, input);
        auto const receiptId = recordReceipt(mapper, input, posted.entryId);
        sqlTxn.Commit();
        return receiptId;
    } catch (Lightweight::SqlException const& error) {
        refuseLostRace(input, error, true);
    }
}

}  // namespace

// An action handler: the registry calls it on the instance, like every other.
// NOLINTNEXTLINE(readability-convert-member-functions-to-static)
::core::async::Task<ReceiptView> ProcurementModel::execute(ReceiveGoods action) {
    requirePrincipal();
    if (!action.validate()) {
        throw ValidationError{
            "ReceiveGoods: an order, a location, a date and lines, each with an order line, a distinct lot number of "
            "1 to 32 bytes, an expiry and a quantity, are required"};
    }
    requireRoleSomewhere<ReceiveGoods>();
    // Taken before the first await: the session is the dispatch's, and a resumption may not run inside it.
    ReceiptInput input;
    input.principal = currentPrincipal();
    input.date = action.date;
    input.order = *action.order;
    std::optional<InventorySettingsView> settings;
    CurrencyCode companyCurrency = CurrencyCode::EUR;
    std::int64_t ordinal = 0;
    {
        // The reads run on a mapper of their own, outside the transactions below.
        Lightweight::DataMapper reader;
        auto const orders = reader.Query<db::PurchaseOrderRecord>()
                                .Where(::Lightweight::FieldNameOf<&db::PurchaseOrderRecord::id>, "=", *action.order)
                                .All();
        if (orders.empty() || !visible(CompanyId{orders.front().companyId.Value()})) {
            throw NotFound{"ReceiveGoods: order " + std::to_string(*action.order) + " does not exist"};
        }
        auto const& order = orders.front();
        input.company = CompanyId{order.companyId.Value()};
        // Judged again by the handler so a call that reaches it without the guard is refused too, and a role changed
        // since the guard ran is seen.
        requireAuthorized(action);
        auto const orderState = order.state.Value();
        if (orderState != static_cast<int>(OrderState::Open) &&
            orderState != static_cast<int>(OrderState::PartlyReceived)) {
            throw IllegalTransition{"ReceiveGoods: order " + std::to_string(*action.order) +
                                    " has nothing left to receive"};
        }
        input.orderState = static_cast<OrderState>(orderState);
        input.currency = static_cast<CurrencyCode>(order.currency.Value());
        auto const location = inventory::findLocation(reader, *action.location);
        if (!location.has_value() || location->companyId.Value() != order.companyId.Value()) {
            throw NotFound{"ReceiveGoods: location " + std::to_string(*action.location) + " does not exist"};
        }
        if (location->siteId.Value() != order.siteId.Value()) {
            throw ValidationError{"ReceiveGoods: location " + std::to_string(*action.location) +
                                  " is not at the site of order " + std::to_string(*action.order)};
        }
        input.location = *location;
        auto const company = OrgModel::findCompany(input.company);
        if (!company.has_value()) {
            throw NotFound{"ReceiveGoods: company " + std::to_string(*input.company) + " does not exist"};
        }
        companyCurrency = company->baseCurrency;
        input.fiscalYear = static_cast<std::int64_t>(fiscalYearOf(action.date, company->fiscalYearStart));
        settings = StockModel::settingsOf(input.company);
        if (!settings.has_value()) {
            throw NotFound{"ReceiveGoods: company " + std::to_string(*input.company) + " has no inventory settings"};
        }

        // Each line against the order line it names: converted, checked against what is left, valued at its price.
        auto const orderLines =
            reader.Query<db::OrderLineRecord>()
                .Where(::Lightweight::FieldNameOf<&db::OrderLineRecord::orderId>, "=", *action.order)
                .All();
        std::map<std::int64_t, Rational> arriving;
        ExactSum total;
        for (ReceiptLineInput const& line : action.lines) {
            auto const found = std::ranges::find_if(orderLines, [&](db::OrderLineRecord const& row) {
                return static_cast<std::int64_t>(row.id.Value()) == *line.orderLine;
            });
            if (found == orderLines.end()) {
                throw ValidationError{"ReceiveGoods: line " + std::to_string(*line.orderLine) +
                                      " is not a line of order " + std::to_string(*action.order)};
            }
            auto const item = inventory::findItem(reader, found->itemId.Value());
            if (!item.has_value()) {
                throw NotFound{"ReceiveGoods: item " + std::to_string(found->itemId.Value()) + " does not exist"};
            }
            auto const quantity = convertStock(*line.quantity, line.unit, inventory::stockUnitOf(*item));
            if (quantity.numerator <= 0) {
                throw ValidationError{"ReceiveGoods: the quantity of lot " + line.lotNumber + " must be positive"};
            }
            auto& sum = arriving.try_emplace(*line.orderLine, Rational{0, DecimalPlaces{6}}).first->second;
            auto const added = ::morph::math::checkedAdd(sum, quantity);
            if (!added.has_value()) {
                throw OutOfRange{"ReceiveGoods: the quantity received does not fit"};
            }
            sum = *added;
            Rational const price{Numerator{found->unitPriceNum.Value()}, Denominator{found->unitPriceDen.Value()},
                                 DecimalPlaces{decimalsOf(input.currency)}};
            auto const value = orderLineTotal(quantity, price, input.currency);
            if (!total.add(value)) {
                throw OutOfRange{"ReceiveGoods: the receipt's total does not fit"};
            }
            input.lines.push_back(PlannedLine{.input = &line, .item = *item, .quantity = quantity, .value = value});
        }
        input.total = total.total();
        bool complete = true;
        for (auto const& row : orderLines) {
            Rational const ordered{Numerator{row.orderedNum.Value()}, Denominator{row.orderedDen.Value()},
                                   DecimalPlaces{6}};
            Rational const received{Numerator{row.receivedNum.Value()}, Denominator{row.receivedDen.Value()},
                                    DecimalPlaces{6}};
            Rational after = received;
            auto const mine = arriving.find(static_cast<std::int64_t>(row.id.Value()));
            if (mine != arriving.end()) {
                auto const sum = ::morph::math::checkedAdd(received, mine->second);
                if (!sum.has_value()) {
                    throw OutOfRange{"ReceiveGoods: the quantity received does not fit"};
                }
                after = *sum;
                if (after > ordered) {
                    throw ValidationError{"ReceiveGoods: order line " + std::to_string(row.id.Value()) +
                                          " would receive more than was ordered (" + quantityText(ordered) +
                                          " ordered, " + quantityText(received) + " received, " +
                                          quantityText(mine->second) + " arriving)"};
                }
                input.claims.push_back(Claim{.line = static_cast<std::int64_t>(row.id.Value()),
                                             .readNum = row.receivedNum.Value(),
                                             .readDen = row.receivedDen.Value(),
                                             .received = after});
            }
            complete = complete && after == ordered;
        }
        input.nextState = complete ? OrderState::Received : OrderState::PartlyReceived;
        // Each lot number must be new for its item; refused before the ledger is called, so a delivery that cannot be
        // stored is never posted.
        for (PlannedLine const& line : input.lines) {
            if (inventory::lotNumberTaken(static_cast<std::int64_t>(line.item.id.Value()), line.input->lotNumber)) {
                throw Conflict{"ReceiveGoods: lot " + line.input->lotNumber + " of " +
                               std::string{line.item.code.Value().ToStringView()} + " exists already"};
            }
        }
        auto const receipts = reader.Query<db::ReceiptRecord>()
                                  .Where(::Lightweight::FieldNameOf<&db::ReceiptRecord::orderId>, "=", *action.order)
                                  .All();
        ordinal = static_cast<std::int64_t>(receipts.size()) + 1;
    }
    auto const& port = ledgerPort();
    if (!port.post || !port.postIn || !port.find || !port.rateOn) {
        throw ValidationError{"ReceiveGoods: no ledger is wired to post to"};
    }

    // The rate: one in the company's own currency, otherwise the ledger's on the receipt's date. Asked before the
    // ledger is called to record anything, so a receipt without a rate leaves no entry.
    Rational rate{1, DecimalPlaces{6}};
    if (input.currency != companyCurrency) {
        auto const dated = co_await port.rateOn(
            RateQuery{.company = input.company, .from = input.currency, .to = companyCurrency, .on = action.date});
        if (!dated.rate.hasValue()) {
            throw ValidationError{"ReceiveGoods: no " + std::string{currencyName(input.currency)} + "/" +
                                  std::string{currencyName(companyCurrency)} + " rate on " + shown(action.date)};
        }
        rate = *dated.rate;
    }

    std::string const sourceKey = "inventory.receipt:" + std::to_string(*action.order) + ":" + std::to_string(ordinal);
    std::vector<Rational> lineValues;
    for (PlannedLine const& line : input.lines) {
        lineValues.push_back(line.value);
    }
    // The body carries no lot number: a retry that corrects only a lot number repeats the same body and converges on
    // the entry the earlier attempt recorded.
    PostingRequest request = receiptPostingFor(ReceiptToPost{
        .company = input.company,
        .companyCurrency = companyCurrency,
        .currency = input.currency,
        .rate = rate,
        .date = action.date,
        .sourceKey = sourceKey,
        .sourceReference = "receipt " + std::to_string(*action.order) + "/" + std::to_string(ordinal),
        .lineValues = std::move(lineValues),
        .stockAccount = settings->stockAccount,
        .grniAccount = settings->grniAccount,
        .roundingAccount = settings->roundingAccount,
    });
    // The stock debits come first, then the credit: the lots' costs and the receipt's company value are read off them.
    for (std::size_t index = 0; index < input.lines.size(); ++index) {
        input.companyLines.push_back(*request.lines[index].companyAmount);
    }
    input.companyTotal = *request.lines[input.lines.size()].companyAmount;

    std::int64_t receiptId = 0;
    if (port.mode == PostingMode::SharedTransaction) {
        // One transaction: the ledger's entry is written through inventory's own mapper and commits with the receipt.
        receiptId = receiveInOneTransaction(port, request, input);
    } else {
        // An entry under this receipt's key is the first commit of an earlier attempt that failed before the second.
        // The attempt repeats the request; the ledger answers the entry it recorded when the body is the same.
        auto const earlier = co_await port.find(PostingQuery{.company = input.company, .sourceKey = sourceKey});
        // The first commit: the ledger records the entry as its own action. Between it and the second, an entry
        // exists for goods that are not on hand.
        std::optional<PostingResult> posted;
        std::exception_ptr refused;
        try {
            posted = co_await port.post(std::move(request));
        } catch (std::exception const& error) {
            if (std::string_view{error.what()}.find(kOtherBody) == std::string_view::npos) {
                throw;
            }
            refused = std::current_exception();
        }
        if (refused != nullptr) {
            auto const recorded =
                earlier.entryId.hasValue()
                    ? earlier
                    : co_await port.find(PostingQuery{.company = input.company, .sourceKey = sourceKey});
            throw Conflict{"ReceiveGoods: receipt " + std::to_string(ordinal) + " of order " +
                           std::to_string(*action.order) + " was posted as entry " +
                           (recorded.entryId.hasValue() ? std::to_string(*recorded.entryId) : std::string{"?"}) +
                           " with other lines; receive it again as first sent"};
        }
        receiptId = receiveAfterLedger(*posted, input);
    }
    Lightweight::DataMapper mapper;
    co_return inventory::receiptView(mapper, receiptId);
}

// An action handler: the registry calls it on the instance, like every other.
// NOLINTNEXTLINE(readability-convert-member-functions-to-static)
ReceiptView ProcurementModel::execute(const GetReceipt& action) {
    if (!action.validate()) {
        throw ValidationError{"GetReceipt: a receipt is required"};
    }
    Lightweight::DataMapper mapper;
    auto const rows = mapper.Query<db::ReceiptRecord>()
                          .Where(::Lightweight::FieldNameOf<&db::ReceiptRecord::id>, "=", *action.receiptId)
                          .All();
    if (rows.empty() || !visible(CompanyId{rows.front().companyId.Value()})) {
        throw NotFound{"GetReceipt: receipt " + std::to_string(*action.receiptId) + " does not exist"};
    }
    return inventory::receiptView(mapper, *action.receiptId);
}

// An action handler: the registry calls it on the instance, like every other.
// NOLINTNEXTLINE(readability-convert-member-functions-to-static)
ListReceiptsResult ProcurementModel::execute(const ListReceipts& action) {
    if (!action.validate()) {
        throw ValidationError{"ListReceipts: a company is required"};
    }
    ListReceiptsResult result;
    if (!visible(action.company)) {
        return result;
    }
    Lightweight::DataMapper mapper;
    auto rows = mapper.Query<db::ReceiptRecord>()
                    .Where(::Lightweight::FieldNameOf<&db::ReceiptRecord::companyId>, "=", *action.company)
                    .All();
    std::ranges::sort(rows, {}, [](db::ReceiptRecord const& row) { return row.id.Value(); });
    for (auto const& row : rows) {
        result.receipts.push_back(inventory::receiptView(mapper, static_cast<std::int64_t>(row.id.Value())));
    }
    return result;
}

}  // namespace lims

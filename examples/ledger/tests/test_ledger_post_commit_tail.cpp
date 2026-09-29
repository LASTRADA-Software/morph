// SPDX-License-Identifier: Apache-2.0
//
// A ledger write whose journalling fails *after* the write is durable must not
// be reported to the caller as a failure. Each case attaches a sink that
// refuses every entry, asserts the call returns normally, that the sink really
// was asked (so the case cannot pass because nothing threw), and that the write
// is visible on a fresh read.

#include <Lightweight/DataMapper/DataMapper.hpp>
#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <memory>
#include <morph/journal/action_log.hpp>
#include <morph/session/session.hpp>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "ledger/core/money.hpp"
#include "ledger/db/ledger_entity.hpp"
#include "ledger/models/ledger_model.hpp"
#include "testkit/db_fixture.hpp"

namespace {

/// @brief See `test_ledger_model.cpp`'s identical `contextFor`.
[[nodiscard]] morph::session::Context contextFor(std::string principal) {
    morph::session::Context ctx;
    ctx.principal = std::move(principal);
    return ctx;
}

class ScopedPrincipal {
public:
    explicit ScopedPrincipal(std::string principal) : _ctx{contextFor(std::move(principal))}, _scope{_ctx} {}

private:
    morph::session::Context _ctx;
    morph::session::detail::ScopedContext _scope;
};

/// @brief An action log whose `append()` throws, as `IActionLog::append`'s
///        contract requires of a sink that could not record the entry.
class RefusingActionLog : public morph::journal::IActionLog {
public:
    void append(morph::journal::LogEntry /*entry*/) override {
        ++appendAttempts;
        throw std::runtime_error{"RefusingActionLog: append refused"};
    }

    void flush() override {}

    [[nodiscard]] std::vector<morph::journal::LogEntry> entries(std::string_view /*entityKey*/ = {}) const override {
        return {};
    }

    int appendAttempts = 0;
};

}  // namespace

TEST_CASE("OpenAccount and StoreTransaction report success when only their journalling fails",
          "[ledger][journal][post_commit_tail]") {
    const morph::ladder::testkit::DbFixture fixture;
    Lightweight::DataMapper mapper;
    ledger::db::LedgerRecord ledgerRow;
    ledgerRow.name = "Personal";
    mapper.Create(ledgerRow);
    const auto ledgerId = ledger::LedgerId{static_cast<std::int64_t>(ledgerRow.id.Value())};

    ledger::LedgerModel model;
    const ScopedPrincipal principal{"alice"};
    auto log = std::make_shared<RefusingActionLog>();
    model.attachActionLog(log, std::to_string(*ledgerId));

    ledger::AccountInfo checking;
    ledger::AccountInfo groceries;
    REQUIRE_NOTHROW(checking = model.execute(ledger::OpenAccount{.ledgerId = ledgerId,
                                                                 .name = "Checking",
                                                                 .kind = ledger::AccountKind::Asset,
                                                                 .currency = ledger::Currency::USD}));
    REQUIRE_NOTHROW(groceries = model.execute(ledger::OpenAccount{.ledgerId = ledgerId,
                                                                  .name = "Groceries",
                                                                  .kind = ledger::AccountKind::Expense,
                                                                  .currency = ledger::Currency::USD}));
    CHECK(log->appendAttempts == 2);

    using morph::math::DecimalPlaces;
    using morph::math::Denominator;
    using morph::math::Numerator;
    ledger::GetLedgerResult stored;
    REQUIRE_NOTHROW(
        stored = model.execute(ledger::StoreTransaction{
            .ledgerId = ledgerId,
            .description = "Weekly shop",
            .date = morph::time::Timestamp::now(),
            .legs = {ledger::TransactionLeg{
                         .accountId = checking.id,
                         .amount = morph::math::Rational{Numerator{-5000}, Denominator{1}, DecimalPlaces{2}}},
                     ledger::TransactionLeg{
                         .accountId = groceries.id,
                         .amount = morph::math::Rational{Numerator{5000}, Denominator{1}, DecimalPlaces{2}}}}}));
    CHECK(log->appendAttempts == 3);
    CHECK(stored.accounts.size() == 2);

    // Read back through a model with no log attached: the write is durable,
    // and matches what the caller was told.
    ledger::LedgerModel reader;
    const auto state = reader.execute(ledger::GetLedger{.ledgerId = ledgerId});
    const auto balanceOf = [&](ledger::AccountId id) {
        const auto found = std::ranges::find_if(state.accounts, [&](const auto& a) { return a.id == id; });
        REQUIRE(found != state.accounts.end());
        return found->balance.numerator;
    };
    CHECK(balanceOf(checking.id) == -5000);
    CHECK(balanceOf(groceries.id) == 5000);
}

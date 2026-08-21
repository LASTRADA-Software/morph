// SPDX-License-Identifier: Apache-2.0
#include "ledger/core/errors.hpp"
#include "ledger/db/ledger_entity.hpp"
#include "ledger/models/rule_model.hpp"
#include "testkit/db_fixture.hpp"

#include <Lightweight/DataMapper/DataMapper.hpp>
#include <catch2/catch_test_macros.hpp>
#include <morph/journal/action_log.hpp>
#include <morph/session/session.hpp>

#include <memory>

namespace {

/// @brief A `Context` carrying only @p principal. See
///        `test_ledger_model.cpp`'s own `contextFor` -- not redeclared here
///        since these are separate translation units, each with its own
///        anonymous namespace.
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

}  // namespace

TEST_CASE("CreateRule persists a rule at version 1", "[ledger][rule]") {
    morph::ladder::testkit::DbFixture fixture;
    Lightweight::DataMapper mapper;
    ledger::db::LedgerRecord ledgerRow;
    ledgerRow.name = "Personal";
    mapper.Create(ledgerRow);
    const auto ledgerId = ledger::LedgerId{static_cast<std::int64_t>(ledgerRow.id.Value())};

    ledger::RuleModel model;
    ScopedPrincipal principal{"alice"};  // per Task 11's convention -- mutating actions require a principal
    auto ruleId = model.execute(ledger::CreateRule{
        .ledgerId = ledgerId, .trigger = ledger::RuleTrigger::DescriptionContains,
        .matchText = "Coffee", .action = ledger::RuleAction::SetCategory, .actionValue = "Dining"});
    REQUIRE(ruleId.hasValue());

    auto ruleRows = mapper.Query<ledger::db::RuleRecord>()
                        .Where(::Lightweight::FieldNameOf<&ledger::db::RuleRecord::id>, "=", *ruleId)
                        .All();
    REQUIRE(ruleRows.size() == 1);
    CHECK(ruleRows.front().version.Value() == 1);
}

TEST_CASE("UpdateRule bumps the version", "[ledger][rule]") {
    morph::ladder::testkit::DbFixture fixture;
    Lightweight::DataMapper mapper;
    ledger::db::LedgerRecord ledgerRow;
    ledgerRow.name = "Personal";
    mapper.Create(ledgerRow);
    const auto ledgerId = ledger::LedgerId{static_cast<std::int64_t>(ledgerRow.id.Value())};

    ledger::RuleModel model;
    ScopedPrincipal principal{"alice"};
    auto ruleId = model.execute(ledger::CreateRule{
        .ledgerId = ledgerId, .trigger = ledger::RuleTrigger::DescriptionContains,
        .matchText = "Coffee", .action = ledger::RuleAction::SetCategory, .actionValue = "Dining"});

    auto updated = model.execute(
        ledger::UpdateRule{.ruleId = ruleId, .matchText = "Cafe", .actionValue = "Dining Out"});
    CHECK(updated.version == 2);
}

TEST_CASE("RuleModel refuses a mutating action with an empty principal", "[ledger][rule]") {
    // Task 11's cross-rung convention. Every other model in this rung has
    // this test; RuleModel did not, which left its own guard -- and the
    // EmptyPrincipalError throw behind it -- entirely unexecuted.
    morph::ladder::testkit::DbFixture fixture;
    Lightweight::DataMapper mapper;
    ledger::db::LedgerRecord ledgerRow;
    ledgerRow.name = "Personal";
    mapper.Create(ledgerRow);
    const auto ledgerId = ledger::LedgerId{static_cast<std::int64_t>(ledgerRow.id.Value())};

    ledger::RuleModel model;
    // An explicitly empty principal, matching test_budget_model.cpp: the
    // guard is about a session whose principal is blank, not about a missing
    // session, and those are different states.
    ScopedPrincipal empty{""};
    CHECK_THROWS_AS(model.execute(ledger::CreateRule{.ledgerId = ledgerId,
                                                     .trigger = ledger::RuleTrigger::DescriptionContains,
                                                     .matchText = "Coffee",
                                                     .action = ledger::RuleAction::SetCategory,
                                                     .actionValue = "Dining"}),
                    ledger::EmptyPrincipalError);
}

TEST_CASE("RuleModel journals a rule mutation once a log is attached", "[ledger][rule][journal]") {
    // Task 11a's self-journaling retrofit. `attachActionLog` and the whole
    // logAction path were unexecuted before this: the rung claimed rule
    // mutations were journaled, and nothing checked that they were.
    morph::ladder::testkit::DbFixture fixture;
    Lightweight::DataMapper mapper;
    ledger::db::LedgerRecord ledgerRow;
    ledgerRow.name = "Personal";
    mapper.Create(ledgerRow);
    const auto ledgerId = ledger::LedgerId{static_cast<std::int64_t>(ledgerRow.id.Value())};
    const auto entityKey = std::to_string(*ledgerId);

    auto log = std::make_shared<::morph::journal::InMemoryActionLog>();
    ledger::RuleModel model;
    model.attachActionLog(log, entityKey);

    ScopedPrincipal principal{"alice"};
    const auto ruleId = model.execute(ledger::CreateRule{.ledgerId = ledgerId,
                                                         .trigger = ledger::RuleTrigger::DescriptionContains,
                                                         .matchText = "Coffee",
                                                         .action = ledger::RuleAction::SetCategory,
                                                         .actionValue = "Dining"});
    REQUIRE(ruleId.hasValue());

    const auto entries = log->entries(entityKey);
    REQUIRE_FALSE(entries.empty());
    const auto& entry = entries.front();
    CHECK(entry.modelType == "RuleModel");
    CHECK(entry.actionType == "CreateRule");
    CHECK(entry.outcome == ::morph::journal::Outcome::Succeeded);
    // Server-stamped, and attributed to whoever actually acted -- the two
    // properties an audit trail is worthless without.
    CHECK(entry.principal == "alice");
    CHECK(entry.timestampMs > 0);
}

// SPDX-License-Identifier: Apache-2.0
#include "ledger/db/ledger_entity.hpp"
#include "ledger/models/rule_model.hpp"
#include "testkit/db_fixture.hpp"

#include <Lightweight/DataMapper/DataMapper.hpp>
#include <catch2/catch_test_macros.hpp>
#include <morph/session/session.hpp>

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

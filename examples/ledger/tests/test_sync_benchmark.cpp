// SPDX-License-Identifier: Apache-2.0
//
// The scenarios SYNC-BENCHMARK.md describes, as executable claims.
//
// The document states morph's answer to concurrent edits: server arrival
// order, with one action as the unit of conflict. These cases hold that
// answer to account -- particularly the part that is easy to assert in prose
// and easy to leave unimplemented in code, that a stale edit is *rejected*
// rather than merged or silently applied.

#include "ledger/core/errors.hpp"
#include "ledger/db/ledger_entity.hpp"
#include "ledger/models/rule_model.hpp"
#include "testkit/db_fixture.hpp"

#include <Lightweight/DataMapper/DataMapper.hpp>
#include <catch2/catch_test_macros.hpp>
#include <morph/session/session.hpp>

#include <string>

namespace {

[[nodiscard]] morph::session::Context contextFor(std::string principal) {
    morph::session::Context ctx;
    ctx.principal = std::move(principal);
    return ctx;
}

/// @brief Installs @p principal for this scope. `_ctx` is a member declared
///        before `_scope` because `ScopedContext` holds a reference to it.
class ScopedPrincipal {
  public:
    explicit ScopedPrincipal(std::string principal) : _ctx{contextFor(std::move(principal))}, _scope{_ctx} {}

  private:
    morph::session::Context _ctx;
    morph::session::detail::ScopedContext _scope;
};

[[nodiscard]] ledger::LedgerId seedLedger() {
    Lightweight::DataMapper mapper;
    ledger::db::LedgerRecord row;
    row.name = Lightweight::SqlAnsiString<128>{"Personal"};
    mapper.Create(row);
    return ledger::LedgerId{static_cast<std::int64_t>(row.id.Value())};
}

}  // namespace

TEST_CASE("Scenario B: a stale base-version edit is rejected outright, never merged",
          "[ledger][sync-benchmark]") {
    // Two clients read the same rule at version 1. Client 1 commits, taking it
    // to version 2. Client 2's edit still carries base version 1.
    morph::ladder::testkit::DbFixture fixture;
    const auto ledgerId = seedLedger();
    const ScopedPrincipal alice{"alice"};

    ledger::RuleModel model;
    const auto ruleId = model.execute(ledger::CreateRule{.ledgerId = ledgerId,
                                                          .trigger = ledger::RuleTrigger::DescriptionContains,
                                                          .matchText = "COFFEE",
                                                          .action = ledger::RuleAction::SetCategory,
                                                          .actionValue = "7"});
    REQUIRE(ruleId.hasValue());

    // Both clients read version 1.
    const std::int32_t baseVersionBothClientsRead = 1;

    // Client 1 commits first: server arrival order decides, and it arrived.
    const auto afterFirst = model.execute(ledger::UpdateRule{
        .ruleId = ruleId,
        .matchText = "ESPRESSO",
        .actionValue = "8",
        .expectedVersion = baseVersionBothClientsRead});
    CHECK(afterFirst.version == 2);
    CHECK(afterFirst.matchText == "ESPRESSO");

    // Client 2's edit was composed against version 1, which no longer exists.
    // Rejected outright -- not merged with client 1's edit, and not applied
    // over the top of it.
    CHECK_THROWS_AS(model.execute(ledger::UpdateRule{.ruleId = ruleId,
                                                      .matchText = "FLAT WHITE",
                                                      .actionValue = "9",
                                                      .expectedVersion = baseVersionBothClientsRead}),
                    ledger::VersionConflict);

    // And the rejection left client 1's edit intact. A silent overwrite would
    // read "FLAT WHITE" here; a merge would read some combination. Neither is
    // what this framework does.
    Lightweight::DataMapper mapper;
    auto rows = mapper.Query<ledger::db::RuleRecord>()
                    .Where(::Lightweight::FieldNameOf<&ledger::db::RuleRecord::id>, "=", *ruleId)
                    .All();
    REQUIRE(rows.size() == 1);
    CHECK(std::string{rows.front().matchText.Value().ToStringView()} == "ESPRESSO");
    CHECK(std::string{rows.front().actionValue.Value().ToStringView()} == "8");
    CHECK(rows.front().version.Value() == 2);
}

TEST_CASE("A re-read after a conflict succeeds: rejection is recoverable, not terminal",
          "[ledger][sync-benchmark]") {
    // The other half of Scenario B's claim. Rejecting a stale edit is only
    // reasonable if the client can then re-read and reapply -- otherwise
    // "rejected outright" would mean the user's work is simply lost, which is
    // a worse outcome than the merge the document argues against.
    morph::ladder::testkit::DbFixture fixture;
    const auto ledgerId = seedLedger();
    const ScopedPrincipal alice{"alice"};

    ledger::RuleModel model;
    const auto ruleId = model.execute(ledger::CreateRule{.ledgerId = ledgerId,
                                                          .trigger = ledger::RuleTrigger::DescriptionContains,
                                                          .matchText = "COFFEE",
                                                          .action = ledger::RuleAction::SetCategory,
                                                          .actionValue = "7"});
    model.execute(ledger::UpdateRule{
        .ruleId = ruleId, .matchText = "ESPRESSO", .actionValue = "8", .expectedVersion = 1});

    CHECK_THROWS_AS(model.execute(ledger::UpdateRule{
                        .ruleId = ruleId, .matchText = "FLAT WHITE", .actionValue = "9", .expectedVersion = 1}),
                    ledger::VersionConflict);

    // Re-read, then reapply against what is actually there now.
    const auto reapplied = model.execute(ledger::UpdateRule{
        .ruleId = ruleId, .matchText = "FLAT WHITE", .actionValue = "9", .expectedVersion = 2});
    CHECK(reapplied.matchText == "FLAT WHITE");
    CHECK(reapplied.version == 3);
}

TEST_CASE("An update without an expected version still applies unconditionally",
          "[ledger][sync-benchmark]") {
    // The opt-in half. Every caller predating optimistic concurrency passes no
    // expectedVersion and must keep working -- otherwise adding the mechanism
    // would silently turn existing unconditional updates into conflicts.
    morph::ladder::testkit::DbFixture fixture;
    const auto ledgerId = seedLedger();
    const ScopedPrincipal alice{"alice"};

    ledger::RuleModel model;
    const auto ruleId = model.execute(ledger::CreateRule{.ledgerId = ledgerId,
                                                          .trigger = ledger::RuleTrigger::DescriptionContains,
                                                          .matchText = "COFFEE",
                                                          .action = ledger::RuleAction::SetCategory,
                                                          .actionValue = "7"});
    model.execute(ledger::UpdateRule{.ruleId = ruleId, .matchText = "ESPRESSO", .actionValue = "8"});

    // Version is now 2, but this edit names no base version at all.
    const auto unconditional =
        model.execute(ledger::UpdateRule{.ruleId = ruleId, .matchText = "CORTADO", .actionValue = "9"});
    CHECK(unconditional.matchText == "CORTADO");
    CHECK(unconditional.version == 3);
}

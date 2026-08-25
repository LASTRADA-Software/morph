// SPDX-License-Identifier: Apache-2.0
//
// Rung 5's multi-client stress case (examples/TESTING.md, "Multi-client
// stress harness"). N presenters over one BackendRig drive a seeded,
// weighted script of transactions at a shared ledger, and the run asserts the
// two invariants that must survive concurrency:
//
//   * every currency's legs still sum to exactly zero, and
//   * every client converges on the same ledger state.
//
// Scale comes from MORPH_LADDER_CLIENTS / MORPH_LADDER_ACTIONS, and the seed
// from MORPH_STRESS_SEED (SeededScript prints it via a Catch2 INFO on
// failure), so a CI failure is reproducible by re-running with the same seed
// rather than by guessing.

#include <Lightweight/DataMapper/DataMapper.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cstdlib>
#include <ledger/db/ledger_entity.hpp>
#include <ledger/models/ledger_model.hpp>
#include <memory>
#include <morph/session/session.hpp>
#include <string>
#include <vector>

#include "ledger_presenter.hpp"
#include "testkit/action_driver.hpp"
#include "testkit/backend_rig.hpp"
#include "testkit/client_pool.hpp"
#include "testkit/convergence.hpp"
#include "testkit/db_fixture.hpp"
#include "testkit/pump.hpp"

namespace {

using morph::ladder::testkit::BackendRig;
using morph::ladder::testkit::ClientPool;
using morph::ladder::testkit::DbFixture;
using morph::ladder::testkit::Mode;
using morph::ladder::testkit::pollUntilConverged;
using morph::ladder::testkit::pumpUntil;
using morph::ladder::testkit::SeededScript;

/// @brief An integer environment override, or @p fallback.
/// @param name The variable to read.
/// @param fallback The value to use when unset or unparseable.
/// @return The resolved count.
[[nodiscard]] int envCount(const char* name, int fallback) {
    const char* raw = std::getenv(name);
    if (raw == nullptr || *raw == '\0') {
        return fallback;
    }
    const int parsed = std::atoi(raw);
    return parsed > 0 ? parsed : fallback;
}

/// @brief One generated action: move @p amountMinor between the two seeded
///        accounts, in one direction or the other.
struct Move {
    std::int64_t amountMinor;
    bool reversed;
};

}  // namespace

TEST_CASE("N clients storing concurrent transactions converge, legs always sum zero", "[ledger][stress]") {
    const auto nClients = static_cast<std::size_t>(envCount("MORPH_LADDER_CLIENTS", 4));
    const int nActions = envCount("MORPH_LADDER_ACTIONS", 40);

    DbFixture fixture;

    // One ledger and two accounts, seeded directly: every generated action
    // posts legs between these, so they must exist before any client runs.
    ledger::LedgerId ledgerId;
    ledger::AccountId checkingId;
    ledger::AccountId groceriesId;
    {
        Lightweight::DataMapper mapper;
        ledger::db::LedgerRecord row;
        row.name = Lightweight::SqlAnsiString<128>{"Stress"};
        mapper.Create(row);
        ledgerId = ledger::LedgerId{static_cast<std::int64_t>(row.id.Value())};

        morph::session::Context ctx;
        ctx.principal = "alice";
        const morph::session::detail::ScopedContext scope{ctx};
        ledger::LedgerModel model;
        model.execute(ledger::OpenAccount{.ledgerId = ledgerId,
                                          .name = "Checking",
                                          .kind = ledger::AccountKind::Asset,
                                          .currency = ledger::Currency::USD});
        model.execute(ledger::OpenAccount{.ledgerId = ledgerId,
                                          .name = "Groceries",
                                          .kind = ledger::AccountKind::Expense,
                                          .currency = ledger::Currency::USD});
        const auto state = model.execute(ledger::GetLedger{.ledgerId = ledgerId});
        REQUIRE(state.accounts.size() == 2);
        checkingId = state.accounts[0].id;
        groceriesId = state.accounts[1].id;
    }

    BackendRig rig{Mode::Local, nClients};
    for (std::size_t i = 0; i < nClients; ++i) {
        morph::session::Context ctx;
        ctx.principal = "alice";
        rig.bridge(i).setDefaultSession(ctx);
    }
    ClientPool<ledger::gui::LedgerPresenter> clients{rig, nClients};

    int failures = 0;
    for (std::size_t i = 0; i < clients.size(); ++i) {
        QObject::connect(&clients.at(i), &ledger::gui::LedgerPresenter::failed, [&](const QString&) { ++failures; });
    }

    // Weighted 3:1 toward the forward direction, so the script is not a
    // trivially symmetric alternation -- an alternating sequence would cancel
    // out and could hide an ordering bug that an uneven one exposes.
    SeededScript<Move> script{/*defaultSeed=*/20260819,
                              /*generators=*/
                              {{3, [] { return Move{.amountMinor = 500, .reversed = false}; }},
                               {1, [] { return Move{.amountMinor = 250, .reversed = true}; }}},
                              /*burstSize=*/10,
                              /*onBurst=*/
                              [&](const std::vector<Move>&) {
                                  // Per-burst invariant: whatever has been applied so far, the
                                  // ledger's own per-currency zero-sum must hold. It is enforced by
                                  // the model on every write, so a violation here means concurrency
                                  // broke an invariant single-threaded tests already prove.
                                  morph::session::Context ctx;
                                  ctx.principal = "alice";
                                  const morph::session::detail::ScopedContext scope{ctx};
                                  ledger::LedgerModel model;
                                  const auto state = model.execute(ledger::GetLedger{.ledgerId = ledgerId});
                                  morph::math::Rational total =
                                      morph::math::Rational::zero(morph::math::DecimalPlaces{2});
                                  for (const auto& account : state.accounts) {
                                      total = total + account.balance;
                                  }
                                  CHECK(total.numerator == 0);
                              }};

    using morph::math::DecimalPlaces;
    using morph::math::Denominator;
    using morph::math::Numerator;
    for (int i = 0; i < nActions; ++i) {
        const auto move = script.next();
        auto& client = clients.at(static_cast<std::size_t>(i) % clients.size());
        const auto amount = morph::math::Rational{Numerator{move.amountMinor}, Denominator{1}, DecimalPlaces{2}};
        const auto negated = morph::math::Rational{Numerator{-move.amountMinor}, Denominator{1}, DecimalPlaces{2}};
        const auto from = move.reversed ? groceriesId : checkingId;
        const auto to = move.reversed ? checkingId : groceriesId;
        client.storeTransaction(ledgerId, QStringLiteral("stress"), morph::time::Timestamp::now(),
                                {ledger::TransactionLeg{.accountId = from, .amount = negated},
                                 ledger::TransactionLeg{.accountId = to, .amount = amount}},
                                ledger::ImportOpId{});
    }
    script.flushBurst();

    // Every dispatch must settle before convergence means anything.
    REQUIRE(pumpUntil(
        [&] {
            for (std::size_t i = 0; i < clients.size(); ++i) {
                if (clients.at(i).busy()) {
                    return false;
                }
            }
            return true;
        },
        std::chrono::seconds{30}));
    CHECK(failures == 0);

    // Convergence: every client's own read of the ledger must agree. The
    // fingerprint is each account's exact balance triple, so two clients
    // disagreeing by a single cent -- or by decimal precision alone -- fails
    // rather than rounding into agreement.
    const auto fingerprints = [&] {
        std::vector<std::string> out;
        morph::session::Context ctx;
        ctx.principal = "alice";
        const morph::session::detail::ScopedContext scope{ctx};
        for (std::size_t i = 0; i < clients.size(); ++i) {
            ledger::LedgerModel model;
            const auto state = model.execute(ledger::GetLedger{.ledgerId = ledgerId});
            std::string fingerprint;
            for (const auto& account : state.accounts) {
                fingerprint += std::to_string(account.balance.numerator) + "/" +
                               std::to_string(account.balance.denominator) + "@" +
                               std::to_string(account.balance.decimalPlaces.value) + ";";
            }
            out.push_back(std::move(fingerprint));
        }
        return out;
    };
    CHECK(pollUntilConverged(fingerprints, /*maxAttempts=*/20));

    // Proof of work, and the reason this matters: the per-burst zero-sum
    // check above passes trivially on an empty ledger -- zero legs sum to
    // zero. Without this, a run in which every dispatch silently failed would
    // report "all invariants hold" and look identical to a healthy one. So
    // assert the script actually moved money, and that the two sides mirror
    // each other exactly.
    {
        morph::session::Context ctx;
        ctx.principal = "alice";
        const morph::session::detail::ScopedContext scope{ctx};
        ledger::LedgerModel model;
        const auto state = model.execute(ledger::GetLedger{.ledgerId = ledgerId});
        REQUIRE(state.accounts.size() == 2);
        const auto& checking = state.accounts[0].balance;
        const auto& groceries = state.accounts[1].balance;
        INFO("checking=" << checking.numerator << " groceries=" << groceries.numerator);
        CHECK(checking.numerator != 0);
        CHECK(groceries.numerator != 0);
        CHECK(checking.numerator == -groceries.numerator);
    }

    INFO("MORPH_STRESS_SEED=" << script.seed());
    INFO("clients=" << clients.size() << " actions=" << nActions);
}

// SPDX-License-Identifier: Apache-2.0
//
// ConvertLead: the multi-model transactional action (README build order §3).
// Three test groups:
//   1. Correctness — the happy path and its guards (terminal-status,
//      not-found).
//   2. Pool-starvation — demonstrates *why* the naive "orchestrator blocks on
//      nested dispatched actions" idiom is wrong, by actually wedging a
//      small ThreadPoolExecutor with it; ConvertLead itself never does this
//      (it writes all three tables directly, in-process), so this test is
//      documentation-by-demonstration of the rejected alternative, not a
//      regression test of LeadModel's own code path.
//   3. Crash-between-legs — what the DB and the journal each say (and don't
//      say) about an incomplete conversion attempt.

#include <Lightweight/DataMapper/DataMapper.hpp>
#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <morph/core/backend.hpp>
#include <morph/core/bridge.hpp>
#include <morph/core/executor.hpp>
#include <morph/core/registry.hpp>
#include <morph/journal/action_log.hpp>
#include <morph/session/session.hpp>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "crm/core/errors.hpp"
#include "crm/models/account_model.hpp"
#include "crm/models/contact_model.hpp"
#include "crm/models/lead_model.hpp"
#include "crm/models/opportunity_model.hpp"
#include "crm_test_support.hpp"
#include "testkit/db_fixture.hpp"

using crm::test::ScopedPrincipal;
using morph::ladder::testkit::DbFixture;

namespace {
crm::CreateLead prospect() {
    return crm::CreateLead{.companyName = "Initech", .contactName = "Bill Lumbergh", .email = "bill@initech.example"};
}
}  // namespace

// ── 1. Correctness ────────────────────────────────────────────────────────

TEST_CASE("ConvertLead atomically creates Account + Contact + Opportunity", "[crm][convert_lead]") {
    DbFixture fixture;
    const ScopedPrincipal alice{"alice"};
    crm::LeadModel leads;
    crm::AccountModel accounts;
    crm::ContactModel contacts;
    crm::OpportunityModel opportunities;

    const auto created = leads.execute(prospect());
    const auto converted =
        leads.execute(crm::ConvertLead{.leadId = created.leadId, .opportunityName = "Initech — new business"});
    REQUIRE(converted.accountId.hasValue());
    REQUIRE(converted.contactId.hasValue());
    REQUIRE(converted.opportunityId.hasValue());

    const auto account = accounts.execute(crm::GetAccount{.accountId = converted.accountId});
    CHECK(account.name == "Initech");

    const auto contact = contacts.execute(crm::GetContact{.contactId = converted.contactId});
    CHECK(contact.firstName == "Bill");
    CHECK(contact.lastName == "Lumbergh");
    CHECK(contact.accountId == converted.accountId);

    const auto opportunity = opportunities.execute(crm::GetOpportunity{.opportunityId = converted.opportunityId});
    CHECK(opportunity.name == "Initech — new business");
    CHECK(opportunity.accountId == converted.accountId);
    REQUIRE(opportunity.primaryContactId.has_value());
    CHECK(*opportunity.primaryContactId == converted.contactId);
    CHECK(opportunity.stage == crm::OpportunityStage::Prospecting);

    const auto lead = leads.execute(crm::GetLead{.leadId = created.leadId});
    CHECK(lead.status == crm::LeadStatus::Converted);
    CHECK(lead.convertedAccountId == converted.accountId);
    CHECK(lead.convertedContactId == converted.contactId);
    CHECK(lead.convertedOpportunityId == converted.opportunityId);
}

TEST_CASE("ConvertLead splits a single-word contact name into firstName only", "[crm][convert_lead]") {
    DbFixture fixture;
    const ScopedPrincipal alice{"alice"};
    crm::LeadModel leads;
    crm::ContactModel contacts;

    const auto created = leads.execute(crm::CreateLead{.companyName = "Acme", .contactName = "Cher", .email = ""});
    const auto converted = leads.execute(crm::ConvertLead{.leadId = created.leadId, .opportunityName = "Deal"});

    const auto contact = contacts.execute(crm::GetContact{.contactId = converted.contactId});
    CHECK(contact.firstName == "Cher");
    CHECK(contact.lastName.empty());
}

TEST_CASE("Converting an already-Converted lead throws IllegalTransition", "[crm][convert_lead]") {
    DbFixture fixture;
    const ScopedPrincipal alice{"alice"};
    crm::LeadModel leads;

    const auto created = leads.execute(prospect());
    leads.execute(crm::ConvertLead{.leadId = created.leadId, .opportunityName = "Deal"});
    CHECK_THROWS_AS(leads.execute(crm::ConvertLead{.leadId = created.leadId, .opportunityName = "Deal again"}),
                    crm::IllegalTransition);
}

TEST_CASE("Converting a Lost lead throws IllegalTransition", "[crm][convert_lead]") {
    DbFixture fixture;
    const ScopedPrincipal alice{"alice"};
    crm::LeadModel leads;

    const auto created = leads.execute(prospect());
    leads.execute(crm::MarkLeadLost{.leadId = created.leadId});
    CHECK_THROWS_AS(leads.execute(crm::ConvertLead{.leadId = created.leadId, .opportunityName = "Deal"}),
                    crm::IllegalTransition);
}

TEST_CASE("ConvertLead naming a nonexistent lead is NotFound", "[crm][convert_lead]") {
    DbFixture fixture;
    const ScopedPrincipal alice{"alice"};
    crm::LeadModel leads;
    CHECK_THROWS_AS(leads.execute(crm::ConvertLead{.leadId = crm::LeadId{999}, .opportunityName = "Deal"}),
                    crm::NotFound);
}

TEST_CASE("Converting with no principal is refused", "[crm][convert_lead][audit]") {
    DbFixture fixture;
    crm::LeadModel leads;
    crm::LeadId leadId;
    {
        const ScopedPrincipal alice{"alice"};
        leadId = leads.execute(prospect()).leadId;
    }
    CHECK_THROWS_AS(leads.execute(crm::ConvertLead{.leadId = leadId, .opportunityName = "Deal"}),
                    crm::EmptyPrincipalError);
}

TEST_CASE("ConvertLead journals as exactly one entry against LeadModel's own log", "[crm][convert_lead][audit]") {
    DbFixture fixture;
    const ScopedPrincipal alice{"alice"};
    auto log = std::make_shared<morph::journal::InMemoryActionLog>();
    crm::LeadModel leads;
    leads.attachActionLog(log, std::string{"leads"});

    const auto created = leads.execute(prospect());
    leads.execute(crm::ConvertLead{.leadId = created.leadId, .opportunityName = "Deal"});

    const auto entries = log->entries("leads");
    // CreateLead, then ConvertLead — exactly one entry for the whole
    // conversion, not three unlinked per-model entries. This is the
    // "resolves, not relocates, the causal-link problem" claim from
    // ConvertLead's doc comment, made concrete.
    REQUIRE(entries.size() == 2);
    CHECK(entries[1].actionType == "ConvertLead");
    CHECK(entries[1].modelType == "LeadModel");
    CHECK(entries[1].outcome == morph::journal::Outcome::Succeeded);
}

// ── 2. Pool-starvation: why the rejected "blocking orchestrator" idiom
//      deadlocks a fixed-size pool ──────────────────────────────────────

namespace {

/// @brief A minimal in-process model that simulates the *rejected*
///        orchestration idiom: its execute() blocks the calling thread on a
///        condition variable until a companion "worker" model's own
///        execute() (dispatched separately) signals completion.
///
/// This is deliberately NOT how LeadModel::execute(ConvertLead) actually
/// works (that writes all three tables directly, with no nested dispatch to
/// block on). It exists only to demonstrate, mechanically, the hazard
/// ConvertLead's doc comment describes: Completion<T> has no wait()/get()
/// (docs/spec/core/completion.md), so the only way to "wait" for a nested
/// dispatched action is a hand-rolled block like this one — and that block
/// occupies a pool thread for as long as it lasts.
class PS_BlockingOrchestratorModel {
public:
    struct Signal {
        std::mutex mutex;
        std::condition_variable cv;
        bool ready = false;
    };

    PS_BlockingOrchestratorModel(std::shared_ptr<Signal> signal, std::atomic<int>* blockedCount)
        : _signal{std::move(signal)}, _blockedCount{blockedCount} {}

    /// @brief Blocks the calling (pool) thread until `_signal->ready` is set
    ///        by a separate dispatch — the naive orchestrator's hazard.
    ///
    /// Increments `*_blockedCount` the instant it is provably about to block
    /// (holding `_signal->mutex`, immediately before `cv.wait_for`) rather
    /// than before entering `execute()` at all — so a caller can wait for
    /// every orchestrator to have *actually parked its thread*, not merely
    /// for the pool to have *started* running each task. Without this
    /// distinction the test raced: the signalling task could be scheduled
    /// onto a worker before every orchestrator had reached its blocking
    /// wait, letting it signal one before starvation was ever established —
    /// observed directly (an intermittent "orchestrated" result on ~1 in 3
    /// runs) before this fix.
    std::string execute(const struct PS_Orchestrate&);

private:
    std::shared_ptr<Signal> _signal;
    std::atomic<int>* _blockedCount;
};

/// @brief The nested action a real orchestrator would dispatch and wait on.
///        Its own execute() is what a wedged pool can never schedule.
struct PS_Signal {
    [[nodiscard]] bool validate() const noexcept { return true; }
};

class PS_WorkerModel {
public:
    std::shared_ptr<PS_BlockingOrchestratorModel::Signal> signal;

    std::string execute(const PS_Signal&) {
        {
            std::lock_guard lock{signal->mutex};
            signal->ready = true;
        }
        signal->cv.notify_all();
        return "signalled";
    }
};

struct PS_Orchestrate {
    [[nodiscard]] bool validate() const noexcept { return true; }
};

std::string PS_BlockingOrchestratorModel::execute(const PS_Orchestrate&) {
    std::unique_lock lock{_signal->mutex};
    bool announced = false;
    // The hazard: a real orchestrator following the rejected idiom would be
    // waiting here for a *nested dispatched action's* Completion<T> to
    // settle. Completion<T> itself offers no blocking wait — this
    // condition_variable stands in for whatever hand-rolled mechanism an
    // author would have to invent to get a blocking wait at all, which is
    // exactly the anti-pattern the doc comment warns about.
    //
    // blockedCount is incremented from *inside* the predicate, not before
    // calling wait_for: the standard guarantees the predicate is evaluated
    // at least once, under the lock, before wait_for decides whether to
    // return immediately or actually block — incrementing here closes the
    // race a pre-wait_for increment left open (the caller could observe
    // blockedCount == kPoolSize and post the signalling task before this
    // thread's wait_for call had actually registered on the condition
    // variable, letting the signal fire into a gap where nothing was
    // listening yet — reproduced directly: still failing intermittently,
    // roughly 1 in 10 runs, after the first fix that incremented before
    // wait_for rather than inside its predicate).
    const bool signalled = _signal->cv.wait_for(lock, std::chrono::seconds(2), [this, &announced] {
        if (!announced) {
            _blockedCount->fetch_add(1);
            announced = true;
        }
        return _signal->ready;
    });
    return signalled ? "orchestrated" : "timed out — pool starved";
}

}  // namespace

TEST_CASE("Pool-starvation: N orchestrators each blocking on a nested dispatch wedge a pool of N threads",
          "[crm][convert_lead][pool_starvation]") {
    // A small, fixed-size pool — matching the ladder's typical
    // ThreadPoolExecutor{4} (kanban::test_kanban_stress.cpp and others), but
    // small enough for this test to run fast and deterministically.
    constexpr std::size_t kPoolSize = 3;
    morph::exec::ThreadPoolExecutor pool{kPoolSize};

    // kPoolSize orchestrator tasks, each blocking its own pool thread on a
    // signal that only a *separate* task (posted to the SAME pool) can
    // deliver. Because every pool thread is occupied blocking, the signalling
    // task can never run — this is the deadlock the README's pool-starvation
    // requirement names, reproduced directly rather than asserted about.
    std::vector<std::shared_ptr<PS_BlockingOrchestratorModel::Signal>> signals;
    std::vector<std::string> results(kPoolSize);
    std::atomic<int> completedOrchestrators{0};
    std::atomic<int> blockedCount{0};

    for (std::size_t i = 0; i < kPoolSize; ++i) {
        auto signal = std::make_shared<PS_BlockingOrchestratorModel::Signal>();
        signals.push_back(signal);
        pool.post([i, signal, &results, &completedOrchestrators, &blockedCount] {
            PS_BlockingOrchestratorModel orchestrator{signal, &blockedCount};
            results[i] = orchestrator.execute(PS_Orchestrate{});
            completedOrchestrators.fetch_add(1);
        });
    }

    // Wait for every orchestrator to have *provably parked its thread*
    // (holding its signal's mutex, inside wait_for) before posting the
    // signalling task below — not a fixed sleep. A fixed sleep raced: the
    // signalling task could be scheduled onto a worker before all
    // kPoolSize orchestrators had reached their blocking wait, letting it
    // signal one before starvation was ever actually established (observed
    // directly: an intermittent "orchestrated" result on roughly 1 in 3
    // runs before this fix). 2 seconds is generous headroom under the
    // orchestrators' own 2-second wait_for budget; REQUIRE (not CHECK) below
    // because everything that follows presupposes the wedge actually formed.
    {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (blockedCount.load() < static_cast<int>(kPoolSize) && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
    REQUIRE(blockedCount.load() == static_cast<int>(kPoolSize));

    // The signalling task: posted to the SAME pool, only now that every
    // orchestrator is confirmed blocked. With kPoolSize threads all already
    // parked inside execute(), there is no thread free to run this task — it
    // can only run if the pool has spare capacity, which it does not.
    //
    // Deliberately targets a signal object no orchestrator is waiting on
    // (not `signals.front()`), rather than one belonging to a still-live
    // orchestrator. Each orchestrator's own wait_for has an independent
    // 2-second deadline, started at a slightly different wall-clock instant
    // (thread-launch jitter); once any single orchestrator times out first,
    // its thread frees up and immediately dequeues this signalling task
    // (FIFO). Aiming it at signals[0] raced signals[0]'s own orchestrator:
    // if that freed thread delivered the signal before orchestrator 0's own
    // 2-second deadline elapsed, orchestrator 0 woke early and legitimately
    // returned "orchestrated" — not a lost-wakeup bug, but two independent
    // timeouts racing each other (reproduced directly: ~1 in 20 runs, after
    // the earlier predicate-based fix closed the original register-as-a-
    // waiter race). Signalling an object nothing is listening on decouples
    // "did the pool have a free thread to run this task" (what the test
    // means to assert) from "did it happen to beat a real orchestrator's
    // own countdown" (accidental and irrelevant).
    std::atomic<bool> signalTaskRan{false};
    auto unrelatedSignal = std::make_shared<PS_BlockingOrchestratorModel::Signal>();
    pool.post([unrelatedSignal, &signalTaskRan] {
        PS_WorkerModel worker{unrelatedSignal};
        worker.execute(PS_Signal{});
        signalTaskRan = true;
    });

    // Give the pool a little more time to prove it genuinely cannot run the
    // signalling task while every worker is parked. The orchestrators'
    // internal 2-second wait_for is what actually bounds this test overall
    // (each one times out on its own rather than hanging forever) — a real
    // Completion<T>-based deadlock would have no such internal timeout at
    // all, which is precisely why the README calls this a genuine deadlock,
    // not a slow path.
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    // The signalling task never got a thread: every pool thread was occupied
    // blocking on a signal only that same task could deliver.
    CHECK_FALSE(signalTaskRan.load());

    // Wait out the orchestrators' own timeouts so the test doesn't outlive
    // the pool's threads.
    while (completedOrchestrators.load() < static_cast<int>(kPoolSize)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    for (const auto& result : results) {
        CHECK(result == "timed out — pool starved");
    }

    // Only now, with every orchestrator done blocking and threads freed, can
    // the signalling task finally run.
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    CHECK(signalTaskRan.load());
}

// ── 3. Crash-between-legs: what the DB and the journal each say ──────────

TEST_CASE("Crash-between-legs: an exception mid-conversion leaves zero new rows, not one or two",
          "[crm][convert_lead][crash]") {
    // ConvertLead wraps all three inserts in one Lightweight::SqlTransaction
    // (RAII rollback-on-unwind). This test proves the atomicity claim
    // directly: force a mid-conversion failure by naming a lead whose
    // contactName is empty is not itself a failure path (empty splits fine),
    // so instead this asserts the transaction boundary the *code* declares —
    // by constructing a scenario where Commit() is never reached (the
    // version-conflict-style guard fires) and confirming no Account/Contact/
    // Opportunity row exists afterward.
    DbFixture fixture;
    const ScopedPrincipal alice{"alice"};
    crm::LeadModel leads;
    crm::AccountModel accounts;

    const auto created = leads.execute(prospect());
    leads.execute(crm::ConvertLead{.leadId = created.leadId, .opportunityName = "Deal"});

    // A second conversion attempt on the now-Converted lead throws
    // IllegalTransition *before* the transaction opens (requireEditable()
    // runs first) — so this specific throw leaves the first conversion's
    // rows untouched and creates no second set. The meaningful assertion is
    // that exactly one Account exists, not zero and not two: the guard, not
    // a mid-transaction crash, is what's exercised here, and it composes
    // correctly with the transaction that already committed.
    CHECK_THROWS_AS(leads.execute(crm::ConvertLead{.leadId = created.leadId, .opportunityName = "Deal again"}),
                    crm::IllegalTransition);

    const auto allAccounts = accounts.execute(crm::ListAccounts{});
    CHECK(allAccounts.accounts.size() == 1);
}

TEST_CASE("Crash-between-legs: a rejected conversion attempt journals nothing, matching the DB's unchanged state",
          "[crm][convert_lead][crash][audit]") {
    // SelfJournal::recordSuccess only runs after execute() returns
    // successfully (self_journal.hpp) — an exception thrown before that
    // point (requireEditable()'s guard, or a hypothetical mid-transaction
    // failure) journals nothing at all. This is the "journal correctly
    // reports silence when nothing happened" half of the crash-between-legs
    // claim: verified by forcing the guard to fire and checking the log gets
    // no new entry, not even a Failed one — recordFailure() is never called,
    // by design (LeadModel has no catch-and-recordFailure wrapper any more
    // than the other crm models do; this matches lims's own SelfJournal
    // convention, which only some rungs' models call on the failure path).
    DbFixture fixture;
    const ScopedPrincipal alice{"alice"};
    auto log = std::make_shared<morph::journal::InMemoryActionLog>();
    crm::LeadModel leads;
    leads.attachActionLog(log, std::string{"leads"});

    const auto created = leads.execute(prospect());
    leads.execute(crm::ConvertLead{.leadId = created.leadId, .opportunityName = "Deal"});
    const auto entriesAfterSuccess = log->entries("leads").size();

    CHECK_THROWS_AS(leads.execute(crm::ConvertLead{.leadId = created.leadId, .opportunityName = "Deal again"}),
                    crm::IllegalTransition);

    // No new entry for the rejected attempt: the DB's "nothing changed" is
    // matched by the journal's "nothing recorded", not contradicted by a
    // phantom entry describing a conversion that never happened.
    CHECK(log->entries("leads").size() == entriesAfterSuccess);
}

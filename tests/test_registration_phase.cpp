// SPDX-License-Identifier: Apache-2.0
//
// The registration-phase latch.
//
// `docs/spec/core/registry.md` ("Thread safety") states as a hard constraint
// that the three process-level registries are written only during static
// initialisation and are read-only once `main()` begins -- a `dlopen`ed module
// registering on a worker thread while another dispatches is undefined
// behaviour whose only symptom is intermittent map corruption. Nothing
// detected a violation; the constraint was advice a caller could break
// silently.
//
// `morph::model::closeRegistrationPhase()` closes a one-way latch, and the
// three `register*Once` helpers -- which is what a plugin's static
// initialisers call -- `assert` against it. Debug builds only: `assert` is
// compiled out under NDEBUG, and so is the auto-close that feeds it.
//
// The violating cases below are death tests, because an `assert` failure is an
// `abort()` and there is no other way to observe one. They `fork()` and inspect
// the child's wait status, which is POSIX-only. Each is paired with the control
// that makes it evidence rather than decoration: the same child, running the
// same registrar, with the latch *open*, must exit normally. Without that, a
// `registerModelOnce` that aborted for some unrelated reason -- or an assertion
// wired to a condition that is always true -- would look identical.

#include <catch2/catch_test_macros.hpp>
#include <morph/core/bridge.hpp>
#include <morph/core/registry.hpp>
#include <stdexcept>
#include <string>
#include <string_view>

#if !defined(_WIN32) && !defined(NDEBUG)
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <array>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <utility>
#endif

// A named namespace, not an anonymous one: glaze's reflection requires the
// reflected type to have linkage, and an internal-linkage aggregate fails to
// compile inside `get_name.hpp`.
namespace regphase {

struct RegPhaseAction {
    int amount = 0;
};

struct RegPhaseModel {
    int value = 0;

    int execute(const RegPhaseAction& action) {
        value += action.amount;
        return value;
    }
};

}  // namespace regphase

using regphase::RegPhaseAction;
using regphase::RegPhaseModel;

template <>
struct morph::model::ModelTraits<RegPhaseModel> {
    static constexpr std::string_view typeId() { return "RegPhase_Model"; }
};

template <>
struct morph::model::ActionTraits<RegPhaseAction> {
    using Result = int;
    static constexpr std::string_view typeId() { return "RegPhase_Action"; }
    static std::string toJson(const RegPhaseAction&) { return "{}"; }
    static RegPhaseAction fromJson(std::string_view) { return {}; }
    static std::string resultToJson(const int&) { return "0"; }
    static int resultFromJson(std::string_view) { return 0; }
};

// ── The latch itself, on every build ─────────────────────────────────────────

TEST_CASE("morph::model: closeRegistrationPhase latches, and the test hook re-opens it",
          "[registry][registration-phase]") {
    // The suite's own process may already have dispatched through the
    // singletons by the time this case runs, so start from a known state
    // rather than asserting the initial one.
    morph::model::detail::reopenRegistrationPhaseForTesting();
    REQUIRE_FALSE(morph::model::registrationPhaseClosed());

    morph::model::closeRegistrationPhase();
    REQUIRE(morph::model::registrationPhaseClosed());

    // Idempotent.
    morph::model::closeRegistrationPhase();
    REQUIRE(morph::model::registrationPhaseClosed());

    morph::model::detail::reopenRegistrationPhaseForTesting();
    REQUIRE_FALSE(morph::model::registrationPhaseClosed());
}

#ifndef NDEBUG
TEST_CASE("morph::model: reading a process-level registry closes the latch by itself",
          "[registry][registration-phase]") {
    morph::model::detail::reopenRegistrationPhaseForTesting();
    REQUIRE_FALSE(morph::model::registrationPhaseClosed());

    // An unknown model id: `create` throws before it constructs anything, and
    // the latch has already closed by then -- reading the map is what matters,
    // not finding something in it.
    REQUIRE_THROWS_AS(morph::model::detail::ModelRegistryFactory::instance().create("RegPhase_NoSuchModel"),
                      std::runtime_error);
    CHECK(morph::model::registrationPhaseClosed());

    // A locally owned registry is not the process registry, is not covered by
    // the constraint, and must not latch the whole program.
    morph::model::detail::reopenRegistrationPhaseForTesting();
    morph::model::detail::ModelRegistryFactory local;
    REQUIRE_THROWS_AS(local.create("RegPhase_NoSuchModel"), std::runtime_error);
    CHECK_FALSE(morph::model::registrationPhaseClosed());

    morph::model::detail::reopenRegistrationPhaseForTesting();
}
#endif  // !NDEBUG

// ── The violating case ───────────────────────────────────────────────────────

#if !defined(_WIN32) && !defined(NDEBUG)

namespace {

enum class LatchState : std::uint8_t { Open, Closed };
enum class Registrar : std::uint8_t { Model, Action, ActionExecutor };

// How a forked child ended. `aborted` is the assertion firing; `exitedOk` is
// the registrar running to completion.
struct ChildOutcome {
    bool aborted = false;
    bool exitedOk = false;
};

// Runs one registrar in a forked child, under the requested latch state.
//
// `_exit`, not `exit` or a `return`: the child shares the parent's Catch2 state
// and its buffered stdio, and running atexit handlers (or Catch2's reporter)
// from it would duplicate the parent's output and its exit status.
ChildOutcome runRegistrarInChild(LatchState latch, Registrar which) {
    (void)std::fflush(nullptr);
    pid_t const pid = fork();
    REQUIRE(pid >= 0);
    if (pid == 0) {
        // Child. Both streams are closed: the assertion's own message is
        // on stderr, and Catch2's fatal-condition handler -- which fields the
        // SIGABRT and re-raises it, so the wait status is unaffected -- writes
        // a partial test report to stdout that would otherwise interleave into
        // the parent's. The wait status, not the message, is what the parent
        // reads.
        (void)::close(STDERR_FILENO);
        (void)::close(STDOUT_FILENO);
        if (latch == LatchState::Closed) {
            morph::model::closeRegistrationPhase();
        } else {
            morph::model::detail::reopenRegistrationPhaseForTesting();
        }
        switch (which) {
            case Registrar::Model:
                (void)morph::model::detail::registerModelOnce<RegPhaseModel>("RegPhase_Model");
                break;
            case Registrar::Action:
                (void)morph::model::detail::registerActionOnce<RegPhaseModel, RegPhaseAction>("RegPhase_Model",
                                                                                              "RegPhase_Action");
                break;
            case Registrar::ActionExecutor:
                (void)morph::model::detail::registerActionExecutorOnce<RegPhaseModel, RegPhaseAction>(
                    "RegPhase_Model", "RegPhase_Action");
                break;
            // Unreachable, and deliberately not `abort()`: this test reads
            // SIGABRT as "the assertion fired", so an unhandled enumerator must
            // not be able to impersonate one.
            default:
                _exit(3);
        }
        _exit(0);
    }

    int status = 0;
    REQUIRE(waitpid(pid, &status, 0) == pid);
    return ChildOutcome{.aborted = WIFSIGNALED(status) != 0 && WTERMSIG(status) == SIGABRT,
                        .exitedOk = WIFEXITED(status) != 0 && WEXITSTATUS(status) == 0};
}

}  // namespace

TEST_CASE("morph::model: registering after the latch closes aborts in a debug build",
          "[registry][registration-phase]") {
    // This is the scenario docs/spec/core/registry.md names: a module loaded
    // after startup runs its `BRIDGE_REGISTER_*` initialisers, which call these
    // three helpers, against maps another thread is already reading.
    std::array<std::pair<Registrar, const char*>, 3> const cases{
        {{Registrar::Model, "registerModelOnce"},
         {Registrar::Action, "registerActionOnce"},
         {Registrar::ActionExecutor, "registerActionExecutorOnce"}}};

    for (auto const& [registrar, name] : cases) {
        INFO("registrar: " << name);

        CHECK(runRegistrarInChild(LatchState::Closed, registrar).aborted);

        // The control. If the assertion were wired to a condition that is
        // always true -- or if the registrar aborted for a reason of its own --
        // this would abort too, and the check above would prove nothing.
        CHECK(runRegistrarInChild(LatchState::Open, registrar).exitedOk);
    }

    // The children's latch changes do not propagate, but leave the parent in
    // the state the rest of the suite expects regardless.
    morph::model::detail::reopenRegistrationPhaseForTesting();
}

#endif  // !_WIN32 && !NDEBUG

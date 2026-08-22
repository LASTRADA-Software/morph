// SPDX-License-Identifier: Apache-2.0
//
// morph::exec::postBackground -- the seam a model's own execute() reaches for
// when it needs submit-now/compute-later work.

#include <morph/core/background.hpp>
#include <morph/session/session.hpp>

#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <string>

using morph::exec::ManualExecutor;
using morph::exec::postBackground;
using morph::exec::ScopedBackgroundExecutor;
using morph::exec::SessionPropagation;

namespace {

[[nodiscard]] morph::session::Context contextFor(std::string principal) {
    morph::session::Context ctx;
    ctx.principal = std::move(principal);
    return ctx;
}

}  // namespace

TEST_CASE("postBackground fails visibly when no executor is installed", "[background]") {
    // Deliberately not a silent no-op: a model posting background work in a
    // deployment that never installed an executor has a configuration error,
    // and it must not look like the work was scheduled.
    REQUIRE(morph::exec::backgroundExecutor() == nullptr);

    int ran = 0;
    CHECK_FALSE(postBackground([&ran] { ++ran; }));
    CHECK(ran == 0);
}

TEST_CASE("postBackground queues onto the installed executor", "[background]") {
    auto manual = std::make_shared<ManualExecutor>();
    const ScopedBackgroundExecutor installed{manual};

    int ran = 0;
    REQUIRE(postBackground([&ran] { ++ran; }));

    // Queued, not run: the point of the seam is that the work leaves the
    // caller's strand.
    CHECK(manual->pending() == 1);
    CHECK(ran == 0);

    CHECK(manual->runOne());
    CHECK(ran == 1);
    CHECK(manual->pending() == 0);
}

TEST_CASE("A background task runs with no session by default", "[background]") {
    auto manual = std::make_shared<ManualExecutor>();
    const ScopedBackgroundExecutor installed{manual};

    std::string seen = "<not run>";
    {
        const auto caller = contextFor("alice");
        const morph::session::detail::ScopedContext scope{caller};
        REQUIRE(morph::session::current() != nullptr);
        REQUIRE(postBackground([&seen] {
            const auto* ctx = morph::session::current();
            seen = ctx == nullptr ? "<none>" : ctx->principal;
        }));
    }

    manual->runOne();
    // Not "alice": a background task is the model's own work, not a
    // continuation of the caller's authority.
    CHECK(seen == "<none>");
}

TEST_CASE("SessionPropagation::Inherit runs the task under a copy of the caller's session",
          "[background]") {
    auto manual = std::make_shared<ManualExecutor>();
    const ScopedBackgroundExecutor installed{manual};

    std::string seen = "<not run>";
    {
        const auto caller = contextFor("alice");
        const morph::session::detail::ScopedContext scope{caller};
        REQUIRE(postBackground(
            [&seen] {
                const auto* ctx = morph::session::current();
                seen = ctx == nullptr ? "<none>" : ctx->principal;
            },
            SessionPropagation::Inherit));
    }

    // The caller's Context is destroyed by now. A reference-capturing
    // implementation would read freed memory here; the copy is what makes this
    // safe, and asserting after the scope closes is what proves it is a copy.
    manual->runOne();
    CHECK(seen == "alice");
}

TEST_CASE("ManualExecutor runs tasks a task itself posted", "[background]") {
    auto manual = std::make_shared<ManualExecutor>();
    const ScopedBackgroundExecutor installed{manual};

    int stage = 0;
    REQUIRE(postBackground([&] {
        stage = 1;
        REQUIRE(postBackground([&] { stage = 2; }));
    }));

    // runAll drains the chain rather than stranding the continuation.
    CHECK(manual->runAll() == 2);
    CHECK(stage == 2);
}

TEST_CASE("ScopedBackgroundExecutor restores the previous executor", "[background]") {
    auto outer = std::make_shared<ManualExecutor>();
    {
        const ScopedBackgroundExecutor installedOuter{outer};
        CHECK(morph::exec::backgroundExecutor() == outer);
        {
            auto inner = std::make_shared<ManualExecutor>();
            const ScopedBackgroundExecutor installedInner{inner};
            CHECK(morph::exec::backgroundExecutor() == inner);
        }
        CHECK(morph::exec::backgroundExecutor() == outer);
    }
    CHECK(morph::exec::backgroundExecutor() == nullptr);
}

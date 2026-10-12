// SPDX-License-Identifier: Apache-2.0
//
// QtScheduler against the Scheduler contract (docs/spec/reactive/control.md, "The Scheduler contract"), on Qt's own
// timers and event loop, in real time with short delays.

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QTest>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <core/async/ExecutorContext.hpp>
#include <memory>
#include <morph/qt_quick/scheduler.hpp>
#include <morph/testing/step_executor.hpp>
#include <optional>
#include <stdexcept>

using morph::qt_quick::QtScheduler;
using namespace std::chrono_literals;

namespace {

// Runs Qt's event loop until @p done holds or a second passes.
template <class Predicate>
bool waitFor(Predicate done) {
    QElapsedTimer clock;
    clock.start();
    while (!done() && clock.elapsed() < 1000) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        QTest::qSleep(1);
    }
    return done();
}

}  // namespace

TEST_CASE("qt_quick scheduler: after fires once, with the owner current", "[qt_quick][scheduler]") {
    morph::testing::StepExecutor owner;
    QtScheduler scheduler{owner};
    int fired = 0;
    bool ownerCurrent = false;
    auto handle = scheduler.after(5ms, [&] {
        ++fired;
        ownerCurrent = ::core::async::currentExecutor() == &owner.coreExecutor();
    });
    CHECK(handle.active());
    CHECK(scheduler.pendingTimers() == 1);
    REQUIRE(waitFor([&] { return fired == 1; }));
    CHECK(ownerCurrent);
    QTest::qWait(30);
    CHECK(fired == 1);
    CHECK_FALSE(handle.active());
    CHECK(scheduler.pendingTimers() == 0);
}

TEST_CASE("qt_quick scheduler: every fires each period until cancelled", "[qt_quick][scheduler]") {
    morph::testing::StepExecutor owner;
    QtScheduler scheduler{owner};
    int fired = 0;
    auto handle = scheduler.every(5ms, [&fired] { ++fired; });
    REQUIRE(waitFor([&] { return fired >= 3; }));
    handle.cancel();
    int const atCancel = fired;
    QTest::qWait(30);
    CHECK(fired == atCancel);
    CHECK_FALSE(handle.active());
    CHECK_THROWS_AS(scheduler.every(0ms, [] {}), std::invalid_argument);
}

TEST_CASE("qt_quick scheduler: a cancelled timer never fires, from outside or inside its callback",
          "[qt_quick][scheduler]") {
    morph::testing::StepExecutor owner;
    QtScheduler scheduler{owner};
    int fired = 0;
    {
        auto handle = scheduler.after(1ms, [&fired] { ++fired; });
    }
    std::optional<morph::reactive::TimerHandle> self;
    int selfFired = 0;
    self = scheduler.every(2ms, [&] {
        ++selfFired;
        self->cancel();
    });
    QTest::qWait(40);
    CHECK(fired == 0);
    CHECK(selfFired == 1);
}

TEST_CASE("qt_quick scheduler: a handle may outlive its scheduler, and a throwing callback is contained",
          "[qt_quick][scheduler]") {
    morph::testing::StepExecutor owner;
    std::optional<morph::reactive::TimerHandle> handle;
    {
        QtScheduler scheduler{owner};
        handle = scheduler.after(1h, [] {});
        auto throwing = scheduler.after(1ms, [] { throw std::runtime_error{"boom"}; });
        QTest::qWait(20);
        CHECK_FALSE(throwing.active());
    }
    CHECK_FALSE(handle->active());
    handle->cancel();
}

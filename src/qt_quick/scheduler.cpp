// SPDX-License-Identifier: Apache-2.0

#include <QObject>
#include <QPointer>
#include <QTimer>
#include <algorithm>
#include <core/async/ExecutorContext.hpp>
#include <cstdint>
#include <exception>
#include <morph/core/logger.hpp>
#include <morph/qt_quick/scheduler.hpp>
#include <stdexcept>
#include <unordered_map>
#include <utility>

namespace morph::qt_quick {

struct QtScheduler::State {
    State(exec::IExecutor& executor, QObject* context) : owner{&executor}, parent{context} {
        if (parent == nullptr) {
            own = std::make_unique<QObject>();
            parent = own.get();
        }
    }

    State(State const&) = delete;
    State& operator=(State const&) = delete;
    State(State&&) = delete;
    State& operator=(State&&) = delete;

    ~State() { cancelAll(); }

    void cancel(std::uint64_t timerId) {
        auto const found = timers.find(timerId);
        if (found == timers.end()) {
            return;
        }
        if (QTimer* const timer = found->second) {
            timer->stop();
            timer->deleteLater();
        }
        timers.erase(found);
    }

    void cancelAll() {
        for (auto const& entry : timers) {
            if (QTimer* const timer = entry.second) {
                timer->stop();
                timer->deleteLater();
            }
        }
        timers.clear();
    }

    // Copied before it runs: the callback may cancel its own timer or destroy the scheduler.
    void fire(std::uint64_t timerId, bool repeating, std::function<void()> const& fn) {
        if (!timers.contains(timerId)) {
            return;
        }
        if (!repeating) {
            cancel(timerId);
        }
        std::function<void()> const call = fn;
        ::core::async::ExecutorScope const scope{owner->coreExecutor()};
        try {
            call();
        } catch (std::exception const& failure) {
            morph::log::logError(std::string{"morph::qt_quick: a timer callback threw: "} + failure.what());
        } catch (...) {
            morph::log::logError("morph::qt_quick: a timer callback threw");
        }
    }

    exec::IExecutor* owner;
    std::unique_ptr<QObject> own;
    QObject* parent;
    std::uint64_t nextId = 0;
    std::unordered_map<std::uint64_t, QPointer<QTimer>> timers;
};

QtScheduler::QtScheduler(exec::IExecutor& owner, QObject* context) : _state{std::make_shared<State>(owner, context)} {}

QtScheduler::~QtScheduler() { _state->cancelAll(); }

reactive::TimerHandle QtScheduler::after(std::chrono::milliseconds delay, std::function<void()> fn) {
    return add(delay, std::chrono::milliseconds{0}, std::move(fn));
}

reactive::TimerHandle QtScheduler::every(std::chrono::milliseconds period, std::function<void()> fn) {
    if (period.count() <= 0) {
        throw std::invalid_argument{"morph::qt_quick::QtScheduler::every: the period must be positive"};
    }
    return add(period, period, std::move(fn));
}

std::size_t QtScheduler::pendingTimers() const noexcept { return _state->timers.size(); }

reactive::TimerHandle QtScheduler::add(std::chrono::milliseconds delay, std::chrono::milliseconds period,
                                       std::function<void()> fn) {
    std::shared_ptr<State> const& state = _state;
    std::uint64_t const timerId = ++state->nextId;
    // NOLINTNEXTLINE(cppcoreguidelines-owning-memory): the parent owns the timer; a cancel deletes it later.
    auto* const timer = new QTimer{state->parent};
    bool const repeating = period.count() > 0;
    timer->setSingleShot(!repeating);
    timer->setTimerType(Qt::PreciseTimer);
    std::chrono::milliseconds const zero{0};
    timer->setInterval(repeating ? period : std::max(delay, zero));
    QObject::connect(timer, &QTimer::timeout, timer,
                     [weak = std::weak_ptr{state}, timerId, repeating, fn = std::move(fn)] {
                         if (auto const live = weak.lock()) {
                             live->fire(timerId, repeating, fn);
                         }
                     });
    state->timers.emplace(timerId, timer);
    timer->start();
    std::weak_ptr<State> const weak = state;
    return reactive::TimerHandle{[weak, timerId] {
                                     if (auto const live = weak.lock()) {
                                         live->cancel(timerId);
                                     }
                                 },
                                 [weak, timerId] {
                                     auto const live = weak.lock();
                                     return live != nullptr && live->timers.contains(timerId);
                                 }};
}

}  // namespace morph::qt_quick

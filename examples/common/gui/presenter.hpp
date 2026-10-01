// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <QObject>
#include <QPointer>
#include <atomic>
#include <exception>
#include <functional>
#include <morph/core/completion.hpp>

// moc reads only the Q_OBJECT declarations below; the coroutine machinery is
// for the compiler.
#ifndef Q_MOC_RUN
#include <core/async/Task.hpp>
#include <morph/core/coroutine.hpp>
#endif

/// @file
/// Shared presenter base (examples/TESTING.md, "Presenter architecture" rule
/// 3): "Observable quiescence." Every ladder presenter derives from this so
/// tests can wait for `busy() == false` instead of sleeping.

namespace morph::ladder::gui {

/// @brief Tracks in-flight completions so `busy()`/`idle()` reflect reality
///        without every presenter re-implementing a counter.
class Presenter : public QObject {
    Q_OBJECT

public:
    explicit Presenter(QObject* parent = nullptr) : QObject{parent} {}

    /// @brief `true` while at least one `track()`ed completion has not yet
    ///        resolved or errored.
    [[nodiscard]] bool busy() const { return _inFlight.load() != 0; }

signals:
    /// @brief Emitted the moment `busy()` transitions from `true` to `false`.
    void idle();

protected:
    /// @brief Wraps @p completion's `.then`/`.onError` in begin/end counters,
    ///        forwarding a successful result to @p onOk and, on failure, the
    ///        `std::exception_ptr` to @p onErr (if supplied) before the busy
    ///        counter is decremented.
    ///
    /// @p onErr exists as a parameter rather than something a subclass
    /// composes by calling `.onError(...)` on @p completion itself before
    /// passing it here, for a documentation reason rather than a
    /// correctness one now: `morph::async::detail::CompletionState<T>::
    /// attachOnError` (`morph/core/completion.hpp`) fans out to every
    /// attached handler in attachment order, so a subclass's own
    /// pre-attached `.onError()` would in fact still fire today alongside
    /// this method's own. Folding both into the one @p onErr parameter here
    /// keeps every presenter's error-display-plus-busy-counter contract in
    /// one visible place rather than split across two separate call sites.
    ///
    /// A presenter still "translates and routes, never decides"
    /// (examples/IMPLEMENTATION.md rule 2): this base does not choose *how*
    /// an error is displayed, only that @p onErr — the subclass's own
    /// choice — is guaranteed to run before `finishOne()`.
    /// @tparam T Type of @p completion's success value.
    /// @param completion The in-flight completion to track.
    /// @param onOk Success callback, invoked with the result value.
    /// @param onErr Optional failure callback, invoked with the
    ///        `std::exception_ptr` before the busy counter decrements.
    template <typename T>
    void track(::morph::async::Completion<T> completion, std::function<void(T)> onOk,
               std::function<void(const std::exception_ptr&)> onErr = {}) {
        _inFlight.fetch_add(1);
        // `QPointer`, not a bare `this` capture: a `Completion` resolves
        // through the executor, so this presenter can already have been
        // destroyed by the time either handler below runs. A presenter declared *after* the rig whose
        // bridge it wraps (the only order possible, since it is constructed
        // from that rig) is destroyed *before* it, and `BackendRig`'s
        // destructor then deliberately pumps the Qt event loop to flush
        // queued posts — resolving completions into a presenter that is
        // already gone. Without the guard AddressSanitizer reports that as a
        // `stack-use-after-scope` write in `finishOne()`.
        //
        // The guard covers `onOk`/`onErr` as well as `finishOne()`: a
        // subclass's callback captures *its* `this`, so running it against a
        // destroyed presenter is the same use-after-free one frame further
        // out. `self` is re-checked after the callback returns because the
        // callback itself may destroy the presenter.
        QPointer<Presenter> self{this};
        completion
            .then([self, onOk = std::move(onOk)](T value) {
                if (!self) {
                    return;
                }
                // finishOne() must run even if onOk throws. Otherwise the
                // in-flight counter never decrements, `busy()` stays true
                // forever, and every subsequent `settle()` burns its full
                // deadline before failing — turning one presenter bug into a
                // suite-wide timeout with no useful diagnostic. The exception
                // is rethrown so it still reaches whatever the executor does
                // with a throwing callback.
                try {
                    onOk(std::move(value));
                } catch (...) {
                    if (self) {
                        self->finishOne();
                    }
                    throw;
                }
                if (self) {
                    self->finishOne();
                }
            })
            .onError([self, onErr = std::move(onErr)](const std::exception_ptr& err) {
                if (!self) {
                    return;
                }
                // Same exception-safety contract as the onOk branch above:
                // finishOne() must still run if onErr throws.
                if (onErr) {
                    try {
                        onErr(err);
                    } catch (...) {
                        if (self) {
                            self->finishOne();
                        }
                        throw;
                    }
                }
                if (self) {
                    self->finishOne();
                }
            });
    }

    /// @brief Runs @p flow -- a coroutine that `co_await`s completions -- on
    ///        @p executor, counted in `busy()` until it finishes.
    ///
    /// The coroutine counterpart of `track()`: started with
    /// `morph::async::spawn`, so every step of @p flow runs on @p executor,
    /// whichever thread the completions it awaits settle on. A flow that may
    /// outlive this presenter checks a `QPointer` after each `co_await`, as
    /// `track()`'s handlers do. See docs/spec/core/coroutines.md.
    /// @param executor Where the flow runs: the executor the presenter's
    ///        completions deliver on.
    /// @param flow     The coroutine to run.
    void trackFlow(::morph::exec::IExecutor& executor, ::core::async::Task<void> flow) {
        _inFlight.fetch_add(1);
        ::morph::async::spawn(executor, finishAfter(QPointer<Presenter>{this}, std::move(flow)));
    }

private:
    /// @brief Awaits @p flow, then counts it finished, whether it returned or threw.
    static ::core::async::Task<void> finishAfter(QPointer<Presenter> self, ::core::async::Task<void> flow) {
        std::exception_ptr escaped;
        try {
            co_await std::move(flow);
        } catch (...) {
            escaped = std::current_exception();
        }
        if (!self.isNull()) {
            self->finishOne();
        }
        if (escaped) {
            std::rethrow_exception(escaped);
        }
    }

    void finishOne() {
        if (_inFlight.fetch_sub(1) == 1) {
            emit idle();
        }
    }

    std::atomic<int> _inFlight{0};
};

}  // namespace morph::ladder::gui

// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <QObject>
#include <QPointer>
#include <atomic>
#include <exception>
#include <functional>
#include <morph/core/completion.hpp>

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

    /// @brief Emitted once, the first time this presenter's readiness gate
    ///        (whichever `BridgeHandler` a subclass names in `trackBound()`)
    ///        settles — i.e. once `Bridge::whenBound()`'s `Completion<bool>`
    ///        resolves, however it resolves. `Remote` mode's registration is
    ///        a round trip (docs/findings/017's neighbouring half): a handler
    ///        built the instant the socket connects still rejects every
    ///        dispatch with "handler not bound" until that round trip lands.
    ///        A subclass that calls
    ///        `trackBound()` in its constructor lets its view layer gate its
    ///        first dispatch on this signal instead of polling on a
    ///        `QTimer` — `Local` mode's handler is already bound by
    ///        construction, so `trackBound()` emits this synchronously
    ///        there.
    void bound();

protected:
    /// @brief Wires @p whenBoundCompletion (a `BridgeHandler::whenBound()`
    ///        call) to emit `bound()` exactly once, however it resolves.
    ///
    ///        `Bridge::whenBound()`'s own contract (`morph/core/bridge.hpp`)
    ///        is "resolves with whatever `isBound()` would return once
    ///        settled" — this presenter does not care which way it settled,
    ///        only that the registration round trip (successful or not) is
    ///        no longer in flight, since either outcome means the next
    ///        dispatch attempt gets a real answer instead of a guaranteed
    ///        "handler not bound".
    /// @param whenBoundCompletion The handler's own `whenBound()` result.
    void trackBound(::morph::async::Completion<bool> whenBoundCompletion) {
        // `QPointer`, not a bare `this` capture: `whenBound()`'s Completion
        // resolves through the executor, asynchronously — even `Local`
        // mode's immediate resolution is *posted*, not delivered inline
        // (`morph::async::detail::CompletionState<T>::attachThen`), so this
        // presenter can already be destroyed by the time either handler
        // below runs (e.g. a short-lived presenter torn down at the end of
        // a test case). A `QPointer` reads back null instead of dereferencing
        // freed memory, exactly like Qt's own auto-disconnect-on-destroy for
        // signal/slot connections handles the same hazard.
        QPointer<Presenter> self{this};
        std::move(whenBoundCompletion)
            .then([self](bool) {
                if (self) {
                    emit self->bound();
                }
            })
            .onError([self](const std::exception_ptr&) {
                if (self) {
                    emit self->bound();
                }
            });
    }

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
        // `QPointer`, not a bare `this` capture — for exactly the reason
        // `trackBound()` above documents, and which applies here just as
        // much: a `Completion` resolves through the executor, so this
        // presenter can already have been destroyed by the time either
        // handler below runs. A presenter declared *after* the rig whose
        // bridge it wraps (the only order possible, since it is constructed
        // from that rig) is destroyed *before* it, and `BackendRig`'s
        // destructor then deliberately pumps the Qt event loop to flush
        // queued posts — resolving completions into a presenter that is
        // already gone. AddressSanitizer caught that as a
        // `stack-use-after-scope` write in `finishOne()` (morph#137).
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

private:
    void finishOne() {
        if (_inFlight.fetch_sub(1) == 1) {
            emit idle();
        }
    }

    std::atomic<int> _inFlight{0};
};

}  // namespace morph::ladder::gui

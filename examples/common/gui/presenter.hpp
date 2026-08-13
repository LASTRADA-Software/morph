// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <QObject>

#include <morph/core/completion.hpp>

#include <atomic>
#include <exception>
#include <functional>

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
    /// @p onErr exists as a parameter, not something a subclass composes by
    /// calling `.onError(...)` on @p completion itself before passing it
    /// here: `morph::async::detail::CompletionState<T>::attachOnError`
    /// (`morph/core/completion.hpp`) keeps only the single most-recently
    /// attached handler — a second `.onError()` call (this method's own,
    /// which must run to decrement the counter) silently replaces the first
    /// one rather than chaining alongside it, so a subclass's own
    /// pre-attached `.onError()` would never fire (verified empirically;
    /// see docs/findings/023). Passing the display callback as @p onErr
    /// instead means both behaviors are folded into the *one* `.onError`
    /// handler this method installs, so both actually run.
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
        completion
            .then([this, onOk = std::move(onOk)](T value) {
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
                    finishOne();
                    throw;
                }
                finishOne();
            })
            .onError([this, onErr = std::move(onErr)](const std::exception_ptr& err) {
                // Same exception-safety contract as the onOk branch above:
                // finishOne() must still run if onErr throws.
                if (onErr) {
                    try {
                        onErr(err);
                    } catch (...) {
                        finishOne();
                        throw;
                    }
                }
                finishOne();
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

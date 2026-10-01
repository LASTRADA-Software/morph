// SPDX-License-Identifier: Apache-2.0

#pragma once
#include <exception>
#include <memory>
#include <string>
#include <type_traits>
#include <utility>

#include "../../attributes.hpp"
#include "../completion.hpp"
#include "../executor.hpp"
#include "../logger.hpp"
#include "owner_affinity.hpp"

/// @file
/// @brief `OwnedState`: a component's state, and the one executor it is
///        touched on.
///
/// Specified in `docs/spec/core/executor.md`, "Owned state".

namespace morph::exec::detail {

/// @brief A component's state, held by `std::shared_ptr`, and the owner every
///        access to it runs on.
///
/// What the storage types (`journal::SessionLog`, the action logs, the offline
/// queues, `offline::InMemoryReplayLedger`) are built on. Three ways in:
///
/// - `apply()` — a write. On the owner it runs now; anywhere else it is posted
///   to the owner, one hop, and the caller does not wait.
/// - `read()` — owner-only access, for a verb that returns data or a failure
///   the caller must see. Off the owner it is reported (asserted in a debug
///   build, handed to a test's probe otherwise).
/// - `ask()` — the same verb for a caller off the owner: runs on the owner and
///   answers with a `Completion` delivered on the executor the caller names.
///
/// A posted task holds the state, never the component, so the component may
/// be destroyed on any thread: what it posted still runs on the owner, and the
/// state is freed with the last task that holds it.
///
/// The owner must run one task at a time (a strand, a GUI executor, a pumped
/// `MainThreadExecutor`): state touched in two of its tasks at once would race.
/// @tparam State The component's state.
template <typename State>
class OwnedState {
public:
    /// @param owner The executor the state belongs to. Borrowed: it must
    ///        outlive this object and run what is posted to it.
    /// @param state The state, built by the component's constructor.
    OwnedState(IExecutor& owner MORPH_LIFETIMEBOUND, std::shared_ptr<State> state)
        : _affinity{owner}, _state{std::move(state)} {}

    /// @brief The owner.
    /// @return The executor passed at construction.
    [[nodiscard]] IExecutor& owner() const noexcept { return _affinity.owner(); }

    /// @brief Whether the calling thread is on the owner (`OwnerAffinity::here`).
    /// @return True inside an owner task, or on the constructing thread when it
    ///         was outside every task.
    [[nodiscard]] bool here() const noexcept { return _affinity.here(); }

    /// @brief Owner-only access to the state.
    /// @param site A static string naming the verb, e.g. `"FileActionLog::flush"`.
    /// @return The state.
    [[nodiscard]] State& read(char const* site) const noexcept {
        _affinity.note(site);
        return *_state;
    }

    /// @brief Runs @p body with the state on the owner: now when called there,
    ///        posted otherwise.
    ///
    /// A throw out of a posted body has nowhere to go but the log, which names
    /// @p site; on the owner it propagates to the caller.
    /// @tparam Body Copyable callable taking `State&`.
    /// @param site A static string naming the verb.
    /// @param body The write.
    template <typename Body>
    void apply(char const* site, Body body) {
        if (_affinity.here()) {
            _affinity.note(site);
            body(*_state);
            return;
        }
        _affinity.owner().post([affinity = _affinity, state = _state, site, body = std::move(body)]() mutable {
            affinity.note(site);
            try {
                body(*state);
            } catch (const std::exception& exc) {
                ::morph::log::logError("{}: posted write failed: {}", site, exc.what());
            } catch (...) {
                ::morph::log::logError("{}: posted write failed with an unknown exception", site);
            }
        });
    }

    /// @brief Answers @p body's result, computed with the state on the owner,
    ///        delivered on @p replyExec.
    ///
    /// On the owner @p body runs now; elsewhere it is posted. Either way the
    /// answer is delivered by a task on @p replyExec, and a throw out of
    /// @p body rejects it.
    /// @tparam T    The answer's type.
    /// @tparam Body Copyable callable taking `State&` and returning `T`.
    /// @param site      A static string naming the verb.
    /// @param replyExec Where the answer is delivered and the caller attaches
    ///        its callbacks. Borrowed: it must outlive the completion.
    /// @param body      The read.
    /// @return The answer.
    template <typename T, typename Body>
    [[nodiscard]] ::morph::async::Completion<T> ask(char const* site, IExecutor& replyExec MORPH_LIFETIMEBOUND,
                                                    Body body) const {
        auto settleable = ::morph::async::Completion<T>::makeSettleable(&replyExec);
        auto answer = [affinity = _affinity, state = _state, site, body = std::move(body),
                       promise = std::make_shared<typename ::morph::async::Completion<T>::Promise>(
                           std::move(settleable.second))]() mutable {
            affinity.note(site);
            try {
                promise->resolve(body(*state));
            } catch (...) {
                promise->reject(std::current_exception());
            }
        };
        if (_affinity.here()) {
            answer();
        } else {
            _affinity.owner().post(std::move(answer));
        }
        return std::move(settleable.first);
    }

private:
    OwnerAffinity _affinity;
    std::shared_ptr<State> _state;
};

}  // namespace morph::exec::detail

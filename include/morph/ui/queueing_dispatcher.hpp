// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstddef>
#include <deque>
#include <exception>
#include <functional>
#include <memory>
#include <string>
#include <utility>

#include "../attributes.hpp"
#include "../core/backend.hpp"
#include "../core/completion.hpp"
#include "interpreter.hpp"

/// @file
/// The client's offline queue for a document's mutations (spec 5 §5, `"offline": "queue"`): a `Dispatcher` that holds
/// a call that cannot reach the server and settles it when its replay does.

namespace morph::ui {

/// @brief A `Dispatcher` that holds a queued call the server cannot be reached for, and replays it on reconnect.
///
/// A call made with `CallOptions::queueOffline` goes through the wrapped dispatcher. When it fails with
/// `backend::DisconnectedError` it is held, and the completion the caller has stays pending: the mutation stays
/// `pending` and its elements `stale`. Once anything is held, a later queued call joins the back of the queue without
/// being tried, so the server sees the calls in the order they were made. `replay` re-sends the held calls, oldest
/// first and one at a time, and settles each caller's completion with the replay's reply or failure; a replay that
/// cannot reach the server either stops the replay and stays held for the next one. A call without `queueOffline`, and
/// an evaluation, are never held: offline, they fail at once.
///
/// Single-threaded: every call, and `replay`, runs on the owner the replies are delivered on. Held calls keep their
/// channel, and with it the model binding, until they settle.
class QueueingDispatcher final : public Dispatcher {
public:
    /// @brief Queues around @p inner, delivering on @p owner.
    /// @param inner The dispatcher calls go through. Borrowed: it must outlive this one and every channel it bound.
    /// @param owner The executor replies are delivered on: the runtime's owner. Borrowed.
    QueueingDispatcher(Dispatcher& inner MORPH_LIFETIMEBOUND, exec::IExecutor* owner MORPH_LIFETIMEBOUND)
        : _inner{&inner}, _state{std::make_shared<State>(owner)} {}

    /// @brief Binds through the wrapped dispatcher.
    /// @param typeId      The model's registered type id.
    /// @param sharing     Private, or shared.
    /// @param instanceKey For a shared binding, the instance key; empty for the model's shared singleton.
    /// @return A channel whose queued calls are held while the server cannot be reached.
    [[nodiscard]] std::shared_ptr<ModelChannel> bind(std::string const& typeId, bridge::BindSharing sharing,
                                                     std::string const& instanceKey) override {
        return std::make_shared<Channel>(_inner->bind(typeId, sharing, instanceKey), _state);
    }

    /// @brief Re-sends the held calls, oldest first, one at a time; each caller's completion settles with its replay.
    ///        A call that still cannot reach the server stops the replay and stays held, with every call after it.
    ///        Nothing happens while a replay is already running.
    void replay() { State::next(_state); }

    /// @brief How many calls are held.
    /// @return The count.
    [[nodiscard]] std::size_t held() const noexcept { return _state->held.size(); }

    /// @brief Called with the held count whenever it changes, such as for a "waiting to send" indicator.
    /// @param observer The observer; empty for none.
    void setOnHeld(std::function<void(std::size_t)> observer) { _state->observer = std::move(observer); }

private:
    struct Held {
        std::shared_ptr<ModelChannel> channel;
        std::string actionId;
        std::string bodyJson;
        async::Completion<std::string>::Promise promise;
    };

    struct State {
        explicit State(exec::IExecutor* executor) : owner{executor} {}

        exec::IExecutor* owner;
        std::deque<Held> held;
        bool replaying = false;
        std::function<void(std::size_t)> observer;

        // Holds @p call at the back of the queue.
        void hold(Held call) {
            held.push_back(std::move(call));
            changed();
        }

        void changed() const {
            if (observer) {
                observer(held.size());
            }
        }

        // Sends the oldest held call; on its reply settles its caller and sends the next.
        static void next(std::shared_ptr<State> const& state) {
            if (state->replaying || state->held.empty()) {
                return;
            }
            state->replaying = true;
            Held const& front = state->held.front();
            std::weak_ptr<State> const weak = state;
            front.channel->execute(front.actionId, front.bodyJson, CallOptions{})
                .then([weak](std::string const& reply) {
                    if (auto const held = weak.lock()) {
                        settle(held, [&reply](Held& call) { call.promise.resolve(reply); });
                    }
                })
                .onError([weak](std::exception_ptr const& error) {
                    auto const held = weak.lock();
                    if (!held) {
                        return;
                    }
                    if (isOffline(error)) {
                        // Still unreachable: it stays first, and the next replay starts with it.
                        held->replaying = false;
                        return;
                    }
                    settle(held, [&error](Held& call) { call.promise.reject(error); });
                });
        }

        // Settles the oldest held call, drops it and replays the next.
        static void settle(std::shared_ptr<State> const& state, std::function<void(Held&)> const& outcome) {
            Held call = std::move(state->held.front());
            state->held.pop_front();
            state->replaying = false;
            state->changed();
            outcome(call);
            next(state);
        }
    };

    class Channel final : public ModelChannel {
    public:
        Channel(std::shared_ptr<ModelChannel> inner, std::shared_ptr<State> state)
            : _inner{std::move(inner)}, _state{std::move(state)} {}

        [[nodiscard]] async::Completion<std::string> execute(std::string const& actionId, std::string const& bodyJson,
                                                             CallOptions options) override {
            if (!options.queueOffline || options.evaluate) {
                return _inner->execute(actionId, bodyJson, options);
            }
            auto [held, promise] = async::Completion<std::string>::makeSettleable(_state->owner);
            if (!_state->held.empty()) {
                // Behind calls already waiting: sending it now would overtake them.
                _state->hold(Held{
                    .channel = _inner, .actionId = actionId, .bodyJson = bodyJson, .promise = std::move(promise)});
                return std::move(held);
            }
            auto const settle = std::make_shared<async::Completion<std::string>::Promise>(std::move(promise));
            std::weak_ptr<State> const weak = _state;
            _inner->execute(actionId, bodyJson, options)
                .then([settle](std::string const& reply) { settle->resolve(reply); })
                .onError([inner = _inner, weak, settle, actionId, bodyJson](std::exception_ptr const& error) {
                    auto const state = weak.lock();
                    if (state && isOffline(error)) {
                        state->hold(Held{.channel = inner,
                                         .actionId = actionId,
                                         .bodyJson = bodyJson,
                                         .promise = std::move(*settle)});
                        return;
                    }
                    settle->reject(error);
                });
            return std::move(held);
        }

    private:
        std::shared_ptr<ModelChannel> _inner;
        std::shared_ptr<State> _state;
    };

    [[nodiscard]] static bool isOffline(std::exception_ptr const& error) {
        try {
            std::rethrow_exception(error);
        } catch (backend::DisconnectedError const&) {
            return true;
        } catch (...) {
            return false;
        }
    }

    Dispatcher* _inner;
    std::shared_ptr<State> _state;
};

}  // namespace morph::ui

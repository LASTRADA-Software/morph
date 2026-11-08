// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <algorithm>
#include <cstddef>
#include <deque>
#include <expected>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "../chat/provider.hpp"

/// @file
/// @brief A scripted `IProvider` for tests.

namespace morph::ui::chat::testing {

/// @brief Answers each call with the next scripted response, and fails once the script runs out. Thread-safe.
///
/// Delivers before `stream` returns, unless `hold(true)` leaves each call outstanding until `release`.
class FakeChatProvider final : public IProvider {
public:
    /// @brief Adds a response to the script.
    /// @param response What the next unscripted call answers.
    void script(Response response) {
        std::scoped_lock const lock{_state->mutex};
        _state->script.push_back(std::move(response));
    }

    /// @brief Answers every call past the script with @p response, so a turn can go on calling tools indefinitely.
    /// @param response The response.
    void repeat(Response response) {
        std::scoped_lock const lock{_state->mutex};
        _state->forever = std::move(response);
    }

    /// @brief Sets whether calls wait for `release`.
    /// @param held Whether they do.
    void hold(bool held) {
        std::scoped_lock const lock{_state->mutex};
        _state->held = held;
    }

    /// @brief Delivers every held call that was not cancelled, and stops holding.
    void release() {
        std::vector<std::shared_ptr<Pending>> pending;
        {
            std::scoped_lock const lock{_state->mutex};
            _state->held = false;
            pending.swap(_state->pending);
        }
        for (auto const& call : pending) {
            call->deliver();
        }
    }

    /// @brief The calls made so far.
    /// @return Their number.
    [[nodiscard]] std::size_t calls() const {
        std::scoped_lock const lock{_state->mutex};
        return _state->requests.size();
    }

    /// @brief The calls cancelled so far.
    /// @return Their number.
    [[nodiscard]] std::size_t cancelled() const {
        std::scoped_lock const lock{_state->mutex};
        return _state->cancelled;
    }

    /// @brief What each call was asked.
    /// @return A copy of the requests, oldest first.
    [[nodiscard]] std::vector<Request> requests() const {
        std::scoped_lock const lock{_state->mutex};
        return _state->requests;
    }

    /// @brief Starts a call.
    /// @param request What to ask.
    /// @param sink    Where it delivers.
    /// @return The call.
    [[nodiscard]] std::shared_ptr<IProviderCall> stream(Request request, ProviderSink sink) override {
        auto call = std::make_shared<Pending>(_state, std::move(sink));
        bool held = false;
        {
            std::scoped_lock const lock{_state->mutex};
            _state->requests.push_back(std::move(request));
            if (!_state->script.empty()) {
                call->outcome = std::move(_state->script.front());
                _state->script.pop_front();
            } else if (_state->forever) {
                call->outcome = *_state->forever;
            } else {
                call->outcome = std::unexpected(std::string{"the fake provider's script ran out"});
            }
            held = _state->held;
            if (held) {
                _state->pending.push_back(call);
            }
        }
        if (!held) {
            call->deliver();
        }
        return call;
    }

private:
    struct State;

    // Holds the state, so a call outlives the provider and a late cancel touches nothing freed. A finished call lets
    // go of its sink and leaves the held list: the sink usually owns whoever owns this provider, so keeping either
    // would hold the two alive in a cycle. Both are destroyed outside the lock, since freeing the sink can free the
    // provider.
    struct Pending final : IProviderCall {
        Pending(std::shared_ptr<State> shared, ProviderSink given)
            : state{std::move(shared)}, sink{std::move(given)} {}

        void cancel() override {
            ProviderSink dropped;
            std::shared_ptr<Pending> unheld;
            {
                std::scoped_lock const lock{state->mutex};
                if (finished) {
                    return;
                }
                finished = true;
                ++state->cancelled;
                dropped = std::move(sink);
                auto const position = std::ranges::find_if(
                    state->pending, [this](std::shared_ptr<Pending> const& held) { return held.get() == this; });
                if (position != state->pending.end()) {
                    unheld = std::move(*position);
                    state->pending.erase(position);
                }
            }
        }

        void deliver() {
            ProviderSink taken;
            {
                std::scoped_lock const lock{state->mutex};
                if (finished) {
                    return;
                }
                finished = true;
                taken = std::move(sink);
            }
            if (outcome && !outcome->text.empty() && taken.text) {
                taken.text(outcome->text);
            }
            if (taken.done) {
                taken.done(std::move(outcome));
            }
        }

        std::shared_ptr<State> state;
        ProviderSink sink;
        std::expected<Response, std::string> outcome = std::unexpected(std::string{});
        bool finished = false;
    };

    struct State {
        mutable std::mutex mutex;
        std::deque<Response> script;
        std::optional<Response> forever;
        std::vector<Request> requests;
        std::vector<std::shared_ptr<Pending>> pending;
        std::size_t cancelled = 0;
        bool held = false;
    };

    std::shared_ptr<State> _state = std::make_shared<State>();
};

}  // namespace morph::ui::chat::testing

// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <algorithm>
#include <cstddef>
#include <deque>
#include <expected>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "../chat/transcriber.hpp"

/// @file
/// @brief A scripted `ITranscriber` for tests.

namespace morph::ui::chat::testing {

/// @brief Answers each call with the next scripted outcome, and fails once the script runs out. Thread-safe.
///
/// Delivers before `transcribe` returns, unless `hold(true)` leaves each call outstanding until `release`. Counts the
/// calls cancelled before they delivered.
class FakeTranscriber final : public ITranscriber {
public:
    /// @brief Adds an outcome to the script.
    /// @param outcome What the next unscripted call answers: the text, or why it failed.
    void script(std::expected<std::string, std::string> outcome) {
        std::scoped_lock const lock{_state->mutex};
        _state->script.push_back(std::move(outcome));
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
        return _state->inputs.size();
    }

    /// @brief The calls cancelled before they delivered.
    /// @return Their number.
    [[nodiscard]] std::size_t cancelled() const {
        std::scoped_lock const lock{_state->mutex};
        return _state->cancelled;
    }

    /// @brief What each call was given.
    /// @return A copy of the inputs, oldest first.
    [[nodiscard]] std::vector<TranscriptionInput> inputs() const {
        std::scoped_lock const lock{_state->mutex};
        return _state->inputs;
    }

    /// @brief Starts a call.
    /// @param input The clip.
    /// @param sink  Where it delivers.
    /// @return The call.
    [[nodiscard]] std::shared_ptr<ITranscriberCall> transcribe(TranscriptionInput input,
                                                               TranscriberSink sink) override {
        auto call = std::make_shared<Pending>(_state, std::move(sink));
        bool held = false;
        {
            std::scoped_lock const lock{_state->mutex};
            _state->inputs.push_back(std::move(input));
            if (!_state->script.empty()) {
                call->outcome = std::move(_state->script.front());
                _state->script.pop_front();
            } else {
                call->outcome = std::unexpected(std::string{"the fake transcriber's script ran out"});
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

    // Holds the state, so a call outlives the transcriber and a late cancel touches nothing freed. A finished call
    // lets go of its sink, which usually owns whoever owns this transcriber; it is destroyed outside the lock.
    struct Pending final : ITranscriberCall {
        Pending(std::shared_ptr<State> shared, TranscriberSink given)
            : state{std::move(shared)}, sink{std::move(given)} {}

        void cancel() override {
            TranscriberSink dropped;
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
            TranscriberSink taken;
            {
                std::scoped_lock const lock{state->mutex};
                if (finished) {
                    return;
                }
                finished = true;
                taken = std::move(sink);
            }
            if (taken.done) {
                taken.done(std::move(outcome));
            }
        }

        std::shared_ptr<State> state;
        TranscriberSink sink;
        std::expected<std::string, std::string> outcome = std::unexpected(std::string{});
        bool finished = false;
    };

    struct State {
        mutable std::mutex mutex;
        std::deque<std::expected<std::string, std::string>> script;
        std::vector<TranscriptionInput> inputs;
        std::vector<std::shared_ptr<Pending>> pending;
        std::size_t cancelled = 0;
        bool held = false;
    };

    std::shared_ptr<State> _state = std::make_shared<State>();
};

}  // namespace morph::ui::chat::testing

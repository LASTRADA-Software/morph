// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <chrono>
#include <cstddef>
#include <expected>
#include <optional>
#include <string>
#include <utility>

#include "../chat/dictation.hpp"

/// @file
/// @brief An `IDictationRecorder` a test drives by hand.

namespace morph::ui::chat::testing {

/// @brief Records `start` and `stop`, and answers only when the test says: `begin`, `refuse`, `tick` and `clip` call
///        the callbacks as a real recorder would.
class FakeDictationRecorder final : public IDictationRecorder {
public:
    /// @brief What `unavailable` answers.
    /// @return `why`.
    [[nodiscard]] std::optional<std::string> unavailable() const override { return why; }

    /// @brief Counts the start and keeps its limits.
    /// @param given The limits.
    void start(DictationLimits given) override {
        ++starts;
        limits = given;
    }

    /// @brief Counts the stop; the test then calls `clip`.
    void stop() override { ++stops; }

    /// @brief Says recording began.
    void begin() {
        if (onStarted) {
            onStarted({});
        }
    }

    /// @brief Says recording could not begin.
    /// @param reason Why.
    void refuse(std::string reason) {
        if (onStarted) {
            onStarted(std::unexpected(std::move(reason)));
        }
    }

    /// @brief Says how long it has recorded.
    /// @param seconds The seconds.
    void tick(std::chrono::seconds seconds) {
        if (onProgress) {
            onProgress(seconds);
        }
    }

    /// @brief Delivers the clip, or why there is none.
    /// @param outcome The WAV bytes or the reason.
    void clip(std::expected<std::string, std::string> outcome) {
        if (onClip) {
            onClip(std::move(outcome));
        }
    }

    // NOLINTBEGIN(cppcoreguidelines-non-private-member-variables-in-classes): what a test sets and reads directly.
    /// @brief Why it cannot record; nothing when it can.
    std::optional<std::string> why;
    /// @brief The starts so far.
    std::size_t starts = 0;
    /// @brief The stops so far.
    std::size_t stops = 0;
    /// @brief The last start's limits.
    std::optional<DictationLimits> limits;
    // NOLINTEND(cppcoreguidelines-non-private-member-variables-in-classes)
};

}  // namespace morph::ui::chat::testing

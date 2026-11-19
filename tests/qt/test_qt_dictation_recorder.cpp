// SPDX-License-Identifier: Apache-2.0
//
// The Qt recorder on whatever audio input this machine has. Without one, `unavailable` names why and `start` answers
// with the same reason; with one, the time bound stops the clip and it arrives as WAV. What the clip is made of is
// tested without Qt in test_chat_dictation_audio.cpp.

#include <QCoreApplication>
#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <expected>
#include <morph/qt/qt_dictation_recorder.hpp>
#include <morph/ui/chat/dictation.hpp>
#include <optional>
#include <string>

#include "fake_http_server.hpp"

using namespace std::chrono_literals;

// Mutation: answer `start` with nothing when there is no device, or let the time bound pass.
TEST_CASE("qt_dictation_recorder_answers_start_and_stop", "[qt][dictation]") {
    morph::qt::QtDictationRecorder recorder;
    std::optional<std::expected<void, std::string>> started;
    std::optional<std::expected<std::string, std::string>> clip;
    recorder.onStarted = [&started](std::expected<void, std::string> outcome) { started = std::move(outcome); };
    recorder.onClip = [&clip](std::expected<std::string, std::string> outcome) { clip = std::move(outcome); };
    std::optional<std::string> const why = recorder.unavailable();
    recorder.start(morph::ui::chat::DictationLimits{.maxSeconds = 1s, .maxBytes = 1U << 20U});
    REQUIRE(qt_test::pumpUntil([&] { return started.has_value() || clip.has_value(); }));
    if (why) {
        REQUIRE(started.has_value());
        REQUIRE_FALSE(started->has_value());
        CHECK(started->error() == *why);
        return;
    }
    if (started && !started->has_value()) {
        WARN("the input device would not open: " << started->error());
        return;
    }
    REQUIRE(qt_test::pumpUntil([&] { return clip.has_value(); }, 10s));
    REQUIRE(clip->has_value());
    CHECK((*clip)->starts_with("RIFF"));
    CHECK((*clip)->size() <= morph::ui::chat::kWavHeaderBytes + 2U * 16000U * 2U);
}

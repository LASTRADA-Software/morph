// SPDX-License-Identifier: Apache-2.0
//
// What a dictated clip is made of: 16 kHz mono 16-bit PCM in a WAV container, resampled from whatever the device
// gives, and stopped at the server's bounds.

#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <morph/ui/chat/dictation.hpp>
#include <numbers>
#include <string>
#include <vector>

namespace chat = morph::ui::chat;
using namespace std::chrono_literals;

namespace {

std::uint32_t le32(std::string const& bytes, std::size_t at) {
    std::uint32_t value = 0;
    for (std::size_t index = 0; index < 4; ++index) {
        value |= static_cast<std::uint32_t>(static_cast<unsigned char>(bytes.at(at + index))) << (8U * index);
    }
    return value;
}

std::uint16_t le16(std::string const& bytes, std::size_t at) {
    return static_cast<std::uint16_t>(static_cast<unsigned char>(bytes.at(at)) |
                                      (static_cast<unsigned>(static_cast<unsigned char>(bytes.at(at + 1))) << 8U));
}

}  // namespace

// Mutation: write the data size into the RIFF size, or the sample rate into the byte rate.
TEST_CASE("chat_audio_wav_header_for_pcm16_speech", "[chat][dictation]") {
    std::array<std::int16_t, 3> const samples{1, -2, 32767};
    std::string const wav = chat::wavFromPcm16(samples);
    REQUIRE(wav.size() == chat::kWavHeaderBytes + 6);
    CHECK(wav.substr(0, 4) == "RIFF");
    CHECK(le32(wav, 4) == 36 + 6);
    CHECK(wav.substr(8, 8) == "WAVEfmt ");
    CHECK(le32(wav, 16) == 16);
    CHECK(le16(wav, 20) == 1);
    CHECK(le16(wav, 22) == 1);
    CHECK(le32(wav, 24) == 16000);
    CHECK(le32(wav, 28) == 32000);
    CHECK(le16(wav, 32) == 2);
    CHECK(le16(wav, 34) == 16);
    CHECK(wav.substr(36, 4) == "data");
    CHECK(le32(wav, 40) == 6);
    CHECK(static_cast<std::int16_t>(le16(wav, 44)) == 1);
    CHECK(static_cast<std::int16_t>(le16(wav, 46)) == -2);
    CHECK(static_cast<std::int16_t>(le16(wav, 48)) == 32767);
    std::vector<std::int16_t> const minute(static_cast<std::size_t>(chat::kSpeechSampleRate) * 60, 0);
    CHECK(chat::wavFromPcm16(minute).size() == 1920044);
}

// Mutation: pick every third sample's neighbour, or forget to divide the channels' sum.
TEST_CASE("chat_audio_resample_48k_stereo_to_16k_mono", "[chat][dictation]") {
    constexpr std::size_t kFrames = 4800;
    std::vector<float> stereo;
    for (std::size_t frame = 0; frame < kFrames; ++frame) {
        auto const value =
            static_cast<float>(0.5 * std::sin(2.0 * std::numbers::pi * 440.0 * static_cast<double>(frame) / 48000.0));
        stereo.push_back(value);
        stereo.push_back(value);
    }
    std::vector<std::int16_t> const speech = chat::resampleToSpeech(stereo, 2, 48000);
    REQUIRE(speech.size() == kFrames / 3);
    for (std::size_t index = 0; index < speech.size(); ++index) {
        // Each output averages input frames 3k..3k+2, centred on 3k+1.
        double const expected =
            0.5 * std::sin(2.0 * std::numbers::pi * 440.0 * static_cast<double>((3 * index) + 1) / 48000.0) * 32767.0;
        CHECK(std::abs(static_cast<double>(speech.at(index)) - expected) < 200.0);
    }
}

// Mutation: take the left channel alone.
TEST_CASE("chat_audio_resample_mixes_channels_and_passes_16k_through", "[chat][dictation]") {
    std::vector<float> const opposed{0.5F, -0.5F, 0.5F, -0.5F, 0.5F, -0.5F};
    for (std::int16_t const sample : chat::resampleToSpeech(opposed, 2, 16000)) {
        CHECK(sample == 0);
    }
    std::vector<float> const mono{0.0F, 0.25F, -0.25F, 1.0F, -1.0F};
    std::vector<std::int16_t> const passed = chat::resampleToSpeech(mono, 1, 16000);
    CHECK(passed == std::vector<std::int16_t>{0, 8192, -8192, 32767, -32767});
}

// Mutation: count output positions from each chunk's start rather than from the first sample.
TEST_CASE("chat_audio_resample_in_chunks_matches_one_pass", "[chat][dictation]") {
    std::vector<float> input;
    input.reserve(4410);
    for (int index = 0; index < 4410; ++index) {
        input.push_back(static_cast<float>(std::sin(static_cast<double>(index) / 7.0) * 0.8));
    }
    std::vector<std::int16_t> const whole = chat::resampleToSpeech(input, 1, 44100);
    chat::SpeechResampler chunked{1, 44100};
    std::vector<std::int16_t> pieces;
    for (std::size_t at = 0; at < input.size(); at += 100) {
        std::size_t const count = std::min<std::size_t>(100, input.size() - at);
        auto const out = chunked.push(std::span<float const>{input}.subspan(at, count));
        pieces.insert(pieces.end(), out.begin(), out.end());
    }
    CHECK(pieces == whole);
}

// Mutation: let the clip pass its byte bound, or check the bound only once the clip is over it.
TEST_CASE("chat_audio_clip_stops_at_its_byte_bound", "[chat][dictation]") {
    auto const start = std::chrono::steady_clock::time_point{};
    chat::DictationClip clip{chat::DictationLimits{.maxSeconds = 60s, .maxBytes = chat::kWavHeaderBytes + 200}, start};
    std::vector<std::int16_t> const sixty(60, 7);
    CHECK(clip.append(sixty, start + 100ms) == chat::ClipState::Recording);
    CHECK(clip.append(sixty, start + 200ms) == chat::ClipState::Full);
    CHECK(clip.samples() == 100);
    CHECK(clip.wav().size() == chat::kWavHeaderBytes + 200);
    CHECK(clip.append(sixty, start + 300ms) == chat::ClipState::Full);
    CHECK(clip.samples() == 100);
}

// Mutation: measure the time bound from the last append rather than from the start.
TEST_CASE("chat_audio_clip_stops_at_its_time_bound", "[chat][dictation]") {
    auto const start = std::chrono::steady_clock::time_point{};
    chat::DictationClip clip{chat::DictationLimits{.maxSeconds = 2s, .maxBytes = 1U << 20U}, start};
    std::vector<std::int16_t> const some(10, 1);
    CHECK(clip.append(some, start + 1s) == chat::ClipState::Recording);
    CHECK(clip.elapsed(start + 1500ms) == 1s);
    CHECK(clip.append({}, start + 2s) == chat::ClipState::Full);
    CHECK(clip.samples() == 10);
}

// Mutation: floor the position as a double, or interpolate from the wrong neighbour.
TEST_CASE("chat_audio_resample_up_from_8k_interpolates_and_chunks_alike", "[chat][dictation]") {
    std::vector<float> const input{0.0F, 0.25F, 0.5F, 0.75F, 1.0F, 0.0F, -1.0F, 0.5F};
    std::vector<std::int16_t> const whole = chat::resampleToSpeech(input, 1, 8000);
    REQUIRE(whole.size() == 2 * (input.size() - 1));
    for (std::size_t index = 0; index < whole.size(); ++index) {
        std::size_t const low = index / 2;
        float const expected = (index % 2 == 0) ? input.at(low) : (input.at(low) + input.at(low + 1)) / 2.0F;
        CHECK(whole.at(index) == static_cast<std::int16_t>(std::lround(expected * 32767.0F)));
    }
    chat::SpeechResampler chunked{1, 8000};
    std::vector<std::int16_t> pieces;
    for (std::size_t at = 0; at < input.size(); at += 3) {
        std::size_t const count = std::min<std::size_t>(3, input.size() - at);
        auto const out = chunked.push(std::span<float const>{input}.subspan(at, count));
        pieces.insert(pieces.end(), out.begin(), out.end());
    }
    CHECK(pieces == whole);
}

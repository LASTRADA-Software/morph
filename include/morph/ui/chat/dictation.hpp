// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <numeric>
#include <optional>
#include <span>
#include <string>
#include <vector>

/// @file
/// @brief What records a dictated clip, and the pure functions that make one: 16 kHz mono 16-bit PCM in a WAV
///        container, whatever the device offers, stopped at the server's bounds. Specified in `docs/spec/ui/chat.md`.

namespace morph::ui::chat {

/// @brief The sample rate a dictated clip has. At 16 kHz mono 16-bit a clip's size is a function of its length alone,
///        so the server's bounds hold on every device.
inline constexpr int kSpeechSampleRate = 16000;

/// @brief The size of the WAV header `wavFromPcm16` writes.
inline constexpr std::size_t kWavHeaderBytes = 44;

/// @brief The bounds a clip stops at, as the server's probe offers them.
struct DictationLimits {
    /// @brief The longest clip.
    std::chrono::seconds maxSeconds{0};
    /// @brief The largest clip, WAV header included.
    std::size_t maxBytes = 0;
};

/// @brief Records a dictated clip from the device's microphone.
///
/// Each `start` ends in exactly one of: `onStarted` with an error, when recording could not begin (nothing follows);
/// or `onClip`, possibly preceded by `onStarted` with success. A `stop` before recording began cancels the start, and
/// is answered by `onClip` with an error. The callbacks run on the thread that owns the recorder. The interface sits
/// in the UI layer, so what drives it is tested without a toolkit.
class IDictationRecorder {
public:
    IDictationRecorder() = default;
    IDictationRecorder(IDictationRecorder const&) = delete;
    IDictationRecorder& operator=(IDictationRecorder const&) = delete;
    IDictationRecorder(IDictationRecorder&&) = delete;
    IDictationRecorder& operator=(IDictationRecorder&&) = delete;
    virtual ~IDictationRecorder() = default;

    /// @brief What is known now, without asking, that keeps it from recording, such as no input device. It is not a
    ///        promise that `start` will succeed: permission is asked only by `start`.
    /// @return Why it cannot record, or nothing.
    [[nodiscard]] virtual std::optional<std::string> unavailable() const = 0;

    /// @brief Starts recording, asking for permission first where the platform does. Answers through `onStarted`.
    /// @param limits Where the clip stops by itself.
    virtual void start(DictationLimits limits) = 0;

    /// @brief Ends the clip; `onClip` follows.
    virtual void stop() = 0;

    // NOLINTBEGIN(cppcoreguidelines-non-private-member-variables-in-classes): set by whoever drives the recorder.
    /// @brief Recording began, or why it did not.
    std::function<void(std::expected<void, std::string>)> onStarted;
    /// @brief The seconds recorded so far, as they pass.
    std::function<void(std::chrono::seconds)> onProgress;
    /// @brief The clip as WAV bytes, or why there is none.
    std::function<void(std::expected<std::string, std::string>)> onClip;
    // NOLINTEND(cppcoreguidelines-non-private-member-variables-in-classes)
};

/// @brief 16 kHz mono 16-bit samples as a WAV file.
/// @param samples The samples.
/// @return The file's bytes: a 44-byte header, then the samples little-endian.
[[nodiscard]] inline std::string wavFromPcm16(std::span<std::int16_t const> samples) {
    std::string out;
    out.reserve(kWavHeaderBytes + (samples.size() * 2));
    auto const put32 = [&out](std::uint32_t const value) {
        for (unsigned shift = 0; shift < 32U; shift += 8U) {
            out.push_back(static_cast<char>((value >> shift) & 0xFFU));
        }
    };
    auto const put16 = [&out](std::uint16_t const value) {
        out.push_back(static_cast<char>(value & 0xFFU));
        out.push_back(static_cast<char>((static_cast<unsigned>(value) >> 8U) & 0xFFU));
    };
    auto const data = static_cast<std::uint32_t>(samples.size() * 2);
    out += "RIFF";
    put32(36U + data);
    out += "WAVEfmt ";
    put32(16U);
    put16(1U);
    put16(1U);
    put32(static_cast<std::uint32_t>(kSpeechSampleRate));
    put32(static_cast<std::uint32_t>(kSpeechSampleRate) * 2U);
    put16(2U);
    put16(16U);
    out += "data";
    put32(data);
    for (std::int16_t const sample : samples) {
        put16(static_cast<std::uint16_t>(sample));
    }
    return out;
}

/// @brief Turns a device's samples, at any rate and channel count, into 16 kHz mono 16-bit, one chunk at a time.
///
/// Channels are averaged. Down-sampling averages the input samples one output sample spans, a box filter that keeps
/// most of what lies above the new Nyquist frequency out; up-sampling interpolates between neighbours. Every position
/// is computed from the output sample's index and the samples consumed, never accumulated, so chunks give exactly what
/// one pass would.
class SpeechResampler {
public:
    /// @brief A resampler for one device format.
    /// @param channels   The device's channel count; less than 1 counts as 1.
    /// @param sampleRate The device's sample rate; less than 1 counts as `kSpeechSampleRate`.
    SpeechResampler(int channels, int sampleRate)  // NOLINT(bugprone-easily-swappable-parameters)
        : _channels{static_cast<std::size_t>(std::max(channels, 1))},
          _step{static_cast<double>(sampleRate > 0 ? sampleRate : kSpeechSampleRate) / kSpeechSampleRate} {}

    /// @brief Takes the next chunk.
    /// @param interleaved Samples in [-1, 1], frame by frame; a trailing partial frame is ignored.
    /// @return The 16 kHz mono samples the input so far completes.
    [[nodiscard]] std::vector<std::int16_t> push(std::span<float const> interleaved) {
        std::size_t const frames = interleaved.size() / _channels;
        for (std::size_t frame = 0; frame < frames; ++frame) {
            auto const samples = interleaved.subspan(frame * _channels, _channels);
            float const sum = std::accumulate(samples.begin(), samples.end(), 0.0F);
            _pending.push_back(sum / static_cast<float>(_channels));
        }
        std::vector<std::int16_t> out;
        while (ready()) {
            out.push_back(toPcm16(next()));
            ++_produced;
        }
        // What the next output sample no longer needs.
        auto const done = std::min(static_cast<std::size_t>(startOf(_produced) - _consumed), _pending.size());
        _pending.erase(_pending.begin(), _pending.begin() + static_cast<std::ptrdiff_t>(done));
        _consumed += done;
        return out;
    }

private:
    // Where output sample @p index starts, counted in input samples from the first one ever pushed.
    [[nodiscard]] std::uint64_t startOf(std::uint64_t const index) const {
        return static_cast<std::uint64_t>(std::floor(static_cast<double>(index) * _step));
    }

    [[nodiscard]] bool ready() const {
        std::uint64_t const available = _consumed + _pending.size();
        return _step >= 1.0 ? startOf(_produced + 1) <= available : startOf(_produced) + 1 < available;
    }

    [[nodiscard]] float next() const {
        auto const first = static_cast<std::size_t>(startOf(_produced) - _consumed);
        if (_step >= 1.0) {
            auto const end = std::max(static_cast<std::size_t>(startOf(_produced + 1) - _consumed), first + 1);
            float sum = 0.0F;
            for (std::size_t index = first; index < end; ++index) {
                sum += _pending.at(index);
            }
            return sum / static_cast<float>(end - first);
        }
        double const position = static_cast<double>(_produced) * _step;
        auto const fraction = static_cast<float>(position - std::floor(position));
        return _pending.at(first) + ((_pending.at(first + 1) - _pending.at(first)) * fraction);
    }

    [[nodiscard]] static std::int16_t toPcm16(float const value) {
        return static_cast<std::int16_t>(std::lround(std::clamp(value, -1.0F, 1.0F) * 32767.0F));
    }

    std::size_t _channels;
    double _step;
    std::uint64_t _produced = 0;
    std::uint64_t _consumed = 0;
    std::vector<float> _pending;
};

/// @brief A whole buffer as 16 kHz mono 16-bit: `SpeechResampler::push` once.
/// @param interleaved Samples in [-1, 1], frame by frame.
/// @param channels    The channel count.
/// @param sampleRate  The sample rate.
/// @return The samples.
[[nodiscard]] inline std::vector<std::int16_t> resampleToSpeech(std::span<float const> interleaved, int channels,
                                                                int sampleRate) {
    SpeechResampler resampler{channels, sampleRate};
    return resampler.push(interleaved);
}

/// @brief Whether a clip can take more.
enum class ClipState : std::uint8_t {
    Recording,  ///< Under both bounds.
    Full,       ///< At a bound: the recorder stops and sends what it has.
};

/// @brief A clip being recorded, which stops at the server's bounds: its time from the start, and the size its WAV
///        would have. The clock is passed in, so the bounds are tested without waiting.
class DictationClip {
public:
    /// @brief An empty clip.
    /// @param limits The bounds.
    /// @param start  When recording began.
    DictationClip(DictationLimits limits, std::chrono::steady_clock::time_point start)
        : _limits{limits},
          _start{start},
          _capacity{limits.maxBytes > kWavHeaderBytes ? (limits.maxBytes - kWavHeaderBytes) / 2 : 0} {}

    /// @brief Adds samples, as many as fit the byte bound.
    /// @param samples 16 kHz mono samples; may be empty, to check the time bound alone.
    /// @param now     The time now.
    /// @return `Full` once a bound is reached; samples past the byte bound are dropped.
    ClipState append(std::span<std::int16_t const> samples, std::chrono::steady_clock::time_point now) {
        if (_full) {
            return ClipState::Full;
        }
        std::size_t const room = _capacity - _samples.size();
        std::size_t const taken = std::min(room, samples.size());
        _samples.insert(_samples.end(), samples.begin(), samples.begin() + static_cast<std::ptrdiff_t>(taken));
        _full = _samples.size() >= _capacity || now - _start >= _limits.maxSeconds;
        return _full ? ClipState::Full : ClipState::Recording;
    }

    /// @brief The whole seconds recorded so far.
    /// @param now The time now.
    /// @return They.
    [[nodiscard]] std::chrono::seconds elapsed(std::chrono::steady_clock::time_point now) const {
        return std::chrono::duration_cast<std::chrono::seconds>(now - _start);
    }

    /// @brief The samples held.
    /// @return Their number.
    [[nodiscard]] std::size_t samples() const noexcept { return _samples.size(); }

    /// @brief The clip as a WAV file.
    /// @return Its bytes, at most `DictationLimits::maxBytes`.
    [[nodiscard]] std::string wav() const { return wavFromPcm16(_samples); }

private:
    DictationLimits _limits;
    std::chrono::steady_clock::time_point _start;
    std::size_t _capacity;
    std::vector<std::int16_t> _samples;
    bool _full = false;
};

}  // namespace morph::ui::chat

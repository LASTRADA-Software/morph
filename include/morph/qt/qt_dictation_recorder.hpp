// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <optional>
#include <string>

#include "../ui/chat/dictation.hpp"

class QIODevice;

/// @file
/// @brief The chat's dictation recorder over Qt Multimedia. Built when `MORPH_ENABLE_DICTATION` is on. Specified in
///        `docs/spec/ui/chat.md`.

namespace morph::qt {

/// @brief How one sample of a device's stream is stored, in the machine's byte order.
enum class DictationSample : std::uint8_t {
    UInt8,  ///< Unsigned, silence at 128.
    Int16,  ///< Signed 16-bit.
    Int32,  ///< Signed 32-bit.
    Float,  ///< 32-bit float in [-1, 1].
};

/// @brief An open audio input: the device its bytes arrive on, and the format they are in.
struct DictationInput {
    /// @brief Where the samples arrive, interleaved; `readyRead` says there are more. Owned by `keepAlive`.
    QIODevice* device = nullptr;
    /// @brief The channel count.
    int channels = 1;
    /// @brief The sample rate.
    int sampleRate = ui::chat::kSpeechSampleRate;
    /// @brief The sample type.
    DictationSample sample = DictationSample::Int16;
    /// @brief Whatever keeps the device open. Released on the recorder's thread, once control is back in its event
    ///        loop after a recording ended, never from inside the device's own `readyRead`.
    std::shared_ptr<void> keepAlive;
};

/// @brief Opens an audio input, or says why it cannot.
using DictationInputFactory = std::function<std::expected<DictationInput, std::string>()>;

/// @brief Asks whether the microphone may be used, and calls `answer` with the outcome, now or later.
using DictationPermissionAsk = std::function<void(std::function<void(bool)> answer)>;

/// @brief What a `QtDictationRecorder` is made from. Each empty member takes the platform's own.
struct QtDictationOptions {
    /// @brief Opens the input; the default opens the default audio input at its preferred format with `QAudioSource`.
    DictationInputFactory openInput;
    /// @brief Asks for permission; the default is `QMicrophonePermission` on Qt 6.5 and later, and a grant before
    /// that.
    DictationPermissionAsk askPermission;
};

/// @brief Records a dictated clip from an audio input, by default the system's, converted to 16 kHz mono 16-bit as it
///        arrives (`SpeechResampler`) and stopped at the server's bounds (`DictationClip`). Asks for microphone
///        permission where the platform has it. Audio stays in memory and is never written to disk.
///
/// Lives on a thread running a Qt event loop, where it is made; its callbacks run there. Neither copyable nor movable.
class QtDictationRecorder final : public ui::chat::IDictationRecorder {
public:
    /// @brief A recorder on the calling thread.
    /// @param options What it opens and asks with; empty members take the platform's.
    explicit QtDictationRecorder(QtDictationOptions options = {});
    /// @brief Stops a recording without calling back.
    ~QtDictationRecorder() override;

    QtDictationRecorder(QtDictationRecorder const&) = delete;
    QtDictationRecorder& operator=(QtDictationRecorder const&) = delete;
    QtDictationRecorder(QtDictationRecorder&&) = delete;
    QtDictationRecorder& operator=(QtDictationRecorder&&) = delete;

    /// @brief Why it cannot record now: with the default input, no input device. A recorder given its own input
    ///        reports that at `start`.
    /// @return The reason, or nothing.
    [[nodiscard]] std::optional<std::string> unavailable() const override;

    /// @brief Asks for permission where the platform does, then records. While a recording runs, or waits for
    ///        permission, a second start is answered with a failed `onStarted` and changes nothing.
    /// @param limits Where the clip stops by itself.
    void start(ui::chat::DictationLimits limits) override;

    /// @brief Ends the clip, or cancels a start still waiting for permission; `onClip` follows.
    void stop() override;

private:
    struct Take;

    void begin();
    void refuse(std::string reason);
    void read();
    void tick();
    void finish();

    QtDictationOptions _options;
    // Whether `unavailable` can ask the system, which it cannot do for an input the caller supplied.
    bool _usesSystemInput;
    std::unique_ptr<Take> _take;
    // Tells a permission answer for a start that was stopped from one for the current start.
    std::uint64_t _generation = 0;
    // Expires with the recorder, so an answer that arrives later finds nothing to call.
    std::shared_ptr<bool> _alive = std::make_shared<bool>(true);
};

}  // namespace morph::qt

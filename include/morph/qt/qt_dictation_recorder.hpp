// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>

#include "../ui/chat/dictation.hpp"

/// @file
/// @brief The chat's dictation recorder over Qt Multimedia. Built when `MORPH_ENABLE_DICTATION` is on. Specified in
///        `docs/spec/ui/chat.md`.

namespace morph::qt {

/// @brief Records a dictated clip from the default audio input with `QAudioSource`, at the device's preferred format,
///        converted to 16 kHz mono 16-bit as it arrives (`SpeechResampler`), and stopped at the server's bounds
///        (`DictationClip`). Asks for microphone permission where the platform has it. Audio stays in memory and is
///        never written to disk.
///
/// Lives on a thread running a Qt event loop, where it is made; its callbacks run there. Neither copyable nor movable.
class QtDictationRecorder final : public ui::chat::IDictationRecorder {
public:
    /// @brief A recorder on the calling thread.
    QtDictationRecorder();
    /// @brief Stops a recording without calling back.
    ~QtDictationRecorder() override;

    QtDictationRecorder(QtDictationRecorder const&) = delete;
    QtDictationRecorder& operator=(QtDictationRecorder const&) = delete;
    QtDictationRecorder(QtDictationRecorder&&) = delete;
    QtDictationRecorder& operator=(QtDictationRecorder&&) = delete;

    /// @brief Why it cannot record now: no input device.
    /// @return The reason, or nothing.
    [[nodiscard]] std::optional<std::string> unavailable() const override;

    /// @brief Asks for permission where the platform does, then records. Ignored while a recording runs.
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

    std::unique_ptr<Take> _take;
    // Tells a permission answer for a start that was stopped from one for the current start.
    std::uint64_t _generation = 0;
};

}  // namespace morph::qt

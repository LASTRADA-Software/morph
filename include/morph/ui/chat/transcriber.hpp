// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <expected>
#include <functional>
#include <memory>
#include <optional>
#include <string>

/// @file
/// @brief The seam between a chat's dictation and the speech-to-text provider behind it. Nothing outside an
///        implementation of `ITranscriber` names a provider. Specified in `docs/spec/ui/chat.md`.

namespace morph::ui::chat {

/// @brief One clip to transcribe.
struct TranscriptionInput {
    /// @brief The clip's bytes.
    std::string audio;
    /// @brief Its media type, `audio/wav`.
    std::string mediaType;
    /// @brief The language spoken; the service has already applied its configured default, so this is the one value
    ///        to use. Nothing leaves it to the provider.
    std::optional<std::string> language;
};

/// @brief Where a transcription delivers.
struct TranscriberSink {
    /// @brief The text, or why there is none, from any thread, exactly once unless cancelled. It may run before
    ///        `ITranscriber::transcribe` returns.
    std::function<void(std::expected<std::string, std::string>)> done;
};

/// @brief An outstanding transcription.
class ITranscriberCall {
public:
    ITranscriberCall() = default;
    ITranscriberCall(ITranscriberCall const&) = delete;
    ITranscriberCall& operator=(ITranscriberCall const&) = delete;
    ITranscriberCall(ITranscriberCall&&) = delete;
    ITranscriberCall& operator=(ITranscriberCall&&) = delete;
    virtual ~ITranscriberCall() = default;

    /// @brief Stops the call. Its sink hears nothing after this returns, unless a delivery had already begun.
    ///        Callable from any thread, more than once, and after the call answered, when it does nothing.
    virtual void cancel() = 0;
};

/// @brief A speech-to-text provider behind a chat's dictation.
class ITranscriber {
public:
    ITranscriber() = default;
    ITranscriber(ITranscriber const&) = delete;
    ITranscriber& operator=(ITranscriber const&) = delete;
    ITranscriber(ITranscriber&&) = delete;
    ITranscriber& operator=(ITranscriber&&) = delete;
    virtual ~ITranscriber() = default;

    /// @brief Starts a transcription. Callable from any thread.
    /// @param input The clip.
    /// @param sink  Where it delivers.
    /// @return The call, for cancelling it.
    [[nodiscard]] virtual std::shared_ptr<ITranscriberCall> transcribe(TranscriptionInput input,
                                                                       TranscriberSink sink) = 0;
};

}  // namespace morph::ui::chat

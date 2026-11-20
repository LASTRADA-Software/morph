// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <expected>
#include <memory>
#include <string>

#include "../ui/chat/config.hpp"
#include "../ui/chat/transcriber.hpp"

class QObject;

/// @file
/// @brief The one place that names a speech-to-text provider. Specified in `docs/spec/ui/chat.md`.

namespace morph::qt {

/// @brief The transcriber @p config names. Its one name is `openai-transcriptions`: a multipart `POST` to
///        `{baseUrl}/v1/audio/transcriptions`, which hosted Whisper, self-hosted whisper.cpp and faster-whisper
///        servers, and most gateways speak.
/// @param config What it is reached with. An empty `baseUrl` is the provider's public address.
/// @param owner  A QObject on a thread running a Qt event loop, where its network calls run. Borrowed: it outlives
///               the transcriber. `ITranscriber::transcribe` may be called from any thread.
/// @return The transcriber, or why not: an unknown name, a missing key, model or owner.
[[nodiscard]] std::expected<std::shared_ptr<ui::chat::ITranscriber>, std::string> makeQtTranscriber(
    ui::chat::TranscriptionConfig config, QObject* owner);

}  // namespace morph::qt

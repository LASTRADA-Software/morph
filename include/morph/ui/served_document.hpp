// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <optional>
#include <string>
#include <string_view>

#include "chat/panel_id.hpp"
#include "value.hpp"

/// @file
/// @brief What a client checks about a document a server serves before it mounts it. Host values and commands are
///        scoped by the id a mounted document carries, so the id must be the one the client expects. Specified in
///        `docs/spec/ui/host_vocabulary.md`.

namespace morph::ui {

/// @brief Why a server's catalog entry may not be mounted as a screen, when it may not.
///
/// The chat panel's id carries host values and commands no other document may reach, so the panel is the client's
/// own document and never a server's.
/// @param screenId The catalog entry's id.
/// @return The refusal, naming @p screenId, when it is the chat panel's id; otherwise nothing.
[[nodiscard]] inline std::optional<Refusal> servedScreenRefusal(std::string_view screenId) {
    if (screenId == chat::kChatPanelId) {
        return Refusal{.document = std::string{screenId},
                       .path = {},
                       .reason = "the catalog lists the id `" + std::string{chat::kChatPanelId} +
                                 "`, which is the chat panel's; the entry is refused"};
    }
    return std::nullopt;
}

/// @brief Why a loaded server document may not be mounted under its catalog entry, when it may not.
///
/// Host names are scoped by the id a mounted document carries, so a document that names another id than its entry
/// could take that id's scope.
/// @param screenId   The catalog entry's id.
/// @param documentId The id the loaded document names.
/// @return The refusal, naming @p screenId, when the two ids differ; otherwise nothing.
[[nodiscard]] inline std::optional<Refusal> servedDocumentRefusal(std::string_view screenId,
                                                                  std::string_view documentId) {
    if (documentId != screenId) {
        return Refusal{.document = std::string{screenId},
                       .path = {},
                       .reason = "the document of `" + std::string{screenId} + "` names the id `" +
                                 std::string{documentId} + "`; it is refused"};
    }
    return std::nullopt;
}

}  // namespace morph::ui

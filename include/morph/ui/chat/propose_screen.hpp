// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <memory>
#include <morph/export.hpp>
#include <string>
#include <string_view>

#include "../../core/executor.hpp"
#include "extra_tool.hpp"
#include "service.hpp"

/// @file
/// @brief The chat tool that proposes a new version of the open screen's document. Specified in
///        `docs/spec/ui/custom_screens.md` and `docs/spec/ui/chat.md`.

namespace morph::ui {
class CustomScreens;
}  // namespace morph::ui

namespace morph::ui::chat {

/// @brief The name of the tool `customScreensChatTool` makes.
inline constexpr std::string_view kProposeScreenTool = "propose_screen";

/// @brief The `propose_screen` tool: the assistant proposes a complete `ui/1` document for the open screen, which the
///        server checks as it checks a save and, once accepted, hands to the client as the window's draft.
///
/// - **Offered** only on a screen whose document is known and within `ChatLimits::maxProposalBytes`: the turn's draft,
///   else the bundle's own text for an application screen, else the person's custom screen as @p screens answers it.
///   On any other screen it is not offered, and the system text says the assistant cannot edit the screen.
/// - **Its context** is that document, written as one JSON string literal inside a block named as the screen being
///   edited and as data, never as instructions (a shared custom screen is text someone else may have written), what
///   the base rule allows a screen based on the same base, and a short reference to the vocabulary.
/// - **A call** carries the whole document. It is refused as a tool error, with the pointer to the refused part,
///   when it is malformed, too large, names another screen id than the open one, or fails the checks of a save under
///   the turn's session. Only an accepted document becomes the turn's draft. The refusal that is the
///   `ChatLimits::maxProposalsPerTurn`th of a turn ends the turn, and the screen stays as it was.
/// - **The conversation** keeps that a draft was proposed and its content hash, not the document.
///
/// The tool saves nothing, and runs as the turn's session.
/// @param screens  The handle `registerCustomScreens` returned: the checks and the store.
/// @param app      The application's id, as @p screens serves it.
/// @param executor Where the tool's checks run. Borrowed: it outlives every turn.
/// @return The tool, for `ChatOptions::tools`.
[[nodiscard]] MORPH_API ChatTool customScreensChatTool(std::shared_ptr<CustomScreens> const& screens,
                                                       std::string const& app, exec::IExecutor& executor);

/// @brief A `ChatDocumentSource` that reads a person's custom screens, so the chat offers their own actions and
///        `propose_screen` can edit them.
/// @param screens  The handle `registerCustomScreens` returned.
/// @param app      The application's id, as @p screens serves it.
/// @param executor Where the reads run. Borrowed: it outlives every turn.
/// @return The source, for `ChatOptions::documentSource`.
[[nodiscard]] MORPH_API ChatDocumentSource customScreensDocumentSource(std::shared_ptr<CustomScreens> const& screens,
                                                                       std::string const& app,
                                                                       exec::IExecutor& executor);

/// @brief Offers custom screens in a chat: adds `propose_screen` and the custom screen document source to @p options.
/// @param options  The options being built; its `tools` and `documentSource` are added to.
/// @param screens  The handle `registerCustomScreens` returned.
/// @param app      The application's id, as @p screens serves it.
/// @param executor Where the tool's work runs. Borrowed: it outlives every turn.
MORPH_API void enableCustomScreensChat(ChatOptions& options, std::shared_ptr<CustomScreens> const& screens,
                                       std::string const& app, exec::IExecutor& executor);

}  // namespace morph::ui::chat

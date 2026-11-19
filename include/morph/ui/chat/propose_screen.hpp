// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <memory>
#include <morph/export.hpp>
#include <string>
#include <string_view>
#include <vector>

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

/// @brief The name of the read tool that outlines the open screen's document.
inline constexpr std::string_view kScreenOutlineTool = "screen_outline";

/// @brief The name of the read tool that finds nodes of the open screen's document.
inline constexpr std::string_view kScreenFindTool = "screen_find";

/// @brief The name of the read tool that reads one part of the open screen's document.
inline constexpr std::string_view kScreenNodeTool = "screen_node";

/// @brief The `propose_screen` tool: the assistant proposes an RFC 6902 JSON Patch to the open screen's document,
///        which the server applies, checks as it checks a save and, once accepted, hands to the client as the
///        window's draft.
///
/// - **Offered** only on a screen whose document is known and within `ChatLimits::maxDraftBytes`: the turn's draft,
///   else the bundle's own text for an application screen, else the person's custom screen as @p screens answers it.
///   On any other screen it is not offered, and the system text says the assistant cannot edit the screen.
/// - **Its context** is the document, written as one JSON string literal inside a block named as the screen being
///   edited and as data, never as instructions (a shared custom screen is text someone else may have written), when
///   it is within `ChatLimits::maxInlineDocumentBytes`, and an outline of it (about 8 KiB at most) otherwise; what
///   the base rule allows a screen based on the same base; and a short reference to the vocabulary.
/// - **A call** carries `patch`, an array of operations of at most `ChatLimits::maxProposalBytes` as JSON. The
///   operations apply one at a time to a copy of the turn's current document, which is the latest draft accepted in
///   this turn and otherwise the document the turn began on; there is no base to name, since the server is the only
///   writer within a turn. A whole new document is one `replace` of the path `""`. The call is refused as a tool
///   error when the arguments are malformed, an operation does not apply (the error names its index, counted from 0,
///   its kind and path, and nothing is applied), the result is not an object with the open screen's `id`, is longer
///   than `ChatLimits::maxDraftBytes`, or fails the checks of a save under the turn's session (the error keeps the
///   pointer into the patched document). Only an accepted result becomes the turn's draft. The refusal that is the
///   `ChatLimits::maxProposalsPerTurn`th of a turn ends the turn, and the screen stays as it was.
/// - **The conversation** keeps a patch under about 4 KiB as the model wrote it, and of a larger one only its size
///   and content hash.
///
/// The tool saves nothing, and runs as the turn's session.
/// @param screens  The handle `registerCustomScreens` returned: the checks and the store.
/// @param app      The application's id, as @p screens serves it.
/// @param executor Where the tool's checks run. Borrowed: it outlives every turn.
/// @return The tool, for `ChatOptions::tools`.
[[nodiscard]] MORPH_API ChatTool customScreensChatTool(std::shared_ptr<CustomScreens> const& screens,
                                                       std::string const& app, exec::IExecutor& executor);

/// @brief The read tools that go with `propose_screen`: `screen_outline`, `screen_find` and `screen_node`.
///
/// They read the turn's current document (see `ChatToolContext::document`), so an answer includes what an accepted
/// proposal earlier in the turn changed. They are offered under the same conditions as `propose_screen`, and an answer
/// is capped in bytes (12 KiB, or `ChatLimits::maxToolResultBytes` when that is smaller): `screen_outline` shows the
/// deepest levels that fit, `screen_find` lists at most 50 matches and says how many more there are, and
/// `screen_node` shows a smaller depth than asked when the asked one does not fit.
/// @param screens  The handle `registerCustomScreens` returned.
/// @param app      The application's id, as @p screens serves it.
/// @param executor Where the tools' offers run. Borrowed: it outlives every turn.
/// @return The tools, for `ChatOptions::tools`.
[[nodiscard]] MORPH_API std::vector<ChatTool> customScreensReadTools(std::shared_ptr<CustomScreens> const& screens,
                                                                     std::string const& app,
                                                                     exec::IExecutor& executor);

/// @brief A `ChatDocumentSource` that reads a person's custom screens, so the chat offers their own actions and
///        `propose_screen` can edit them.
/// @param screens  The handle `registerCustomScreens` returned.
/// @param app      The application's id, as @p screens serves it.
/// @param executor Where the reads run. Borrowed: it outlives every turn.
/// @return The source, for `ChatOptions::documentSource`.
[[nodiscard]] MORPH_API ChatDocumentSource customScreensDocumentSource(std::shared_ptr<CustomScreens> const& screens,
                                                                       std::string const& app,
                                                                       exec::IExecutor& executor);

/// @brief Offers custom screens in a chat: adds `propose_screen`, the read tools and the custom screen document source
///        to @p options.
/// @param options  The options being built; its `tools` and `documentSource` are added to.
/// @param screens  The handle `registerCustomScreens` returned.
/// @param app      The application's id, as @p screens serves it.
/// @param executor Where the tool's work runs. Borrowed: it outlives every turn.
MORPH_API void enableCustomScreensChat(ChatOptions& options, std::shared_ptr<CustomScreens> const& screens,
                                       std::string const& app, exec::IExecutor& executor);

}  // namespace morph::ui::chat

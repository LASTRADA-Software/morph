// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <morph/forms/forms.hpp>
#include <string>

#include "polls/dto/poll_dto.hpp"
#include "polls/dto/vote_dto.hpp"

/// @file
/// The one schema document `polls::gui::PollFormsController` renders from —
/// same split as `bookmarks::gui::bookmarkSchemasJson()`
/// (`examples/bookmarks/gui_lib/bookmark_schemas.hpp`) and for the same
/// reason: whatever composes a `PollFormsController` (the desktop client, a
/// future WASM client, the tests) builds the identical `{actionType: schema}`
/// map, never its own.
///
/// @par Only three actions are genuinely schema-driven
/// `AddComment` and `UndoLastVoteChange` are entered as free text
/// (`participantName`/`body`, `participantName`); `FinalizePoll` is entered
/// as a number (the winning option's id, read off the results the vote view
/// already displays). All three are DTOs of scalar fields only, so
/// `DynamicForm` renders them exactly as it renders `Login`/`RenameTag` in
/// rung 2.
///
/// Every other `PollModel` action is deliberately absent, for one of three
/// reasons:
///
/// - `CreatePoll` — `options` is `std::vector<CreatePollOption>`, a JSON
///   array of *objects*, not the array-of-strings `DynamicForm`'s
///   array-field control supports (a gap first hit during rung 2's own GUI
///   shell). Mirrors rung 2's `BulkEdit` workaround:
///   excluded here, driven by a hand-written QML list editor in
///   `gui/qml/CreatePollView.qml` instead, which calls
///   `PollBridge::createPoll(title, optionLabels)` directly rather than
///   going through this schema/`submitIfValid` path at all.
/// - `SubmitVotes`/`UpdateVotes` — same finding: `votes` is
///   `std::vector<OneVote>`, equally array-typed. `gui/qml/VoteView.qml`
///   drives these from a hand-rolled per-option Yes/If-need-be/No picker,
///   via `PollBridge::submitVotes`/`updateVotes`, which build the typed
///   action in C++ and dispatch it through
///   `PollFormsController::submitVotes`/`updateVotes` — the same *handler*
///   `OpenPoll`/`AddComment`/... use, just not the same *path* (see that
///   class's own doc comment for why routing must stay on one handler here).
/// - `OpenPoll`/`GetPollState`/`GetEventsSince` — `OpenPoll` is this rung's
///   one `BRIDGE_MODEL_KEY`-registered (payload-keyed) action. At the time
///   this class was built, dispatching a payload-keyed action through
///   `BridgeHandler::executeJson` on an `AllowShared` handler silently
///   skipped the attach step entirely — `ActionExecuteRegistry::
///   registerAction`'s stored executor closed over the *plain*
///   `BridgeHandler<Model>` overload of `execute<Action>()` regardless of
///   the real handler's `Sharing` policy, so the payload-keyed attach branch
///   never ran. `registerAction` now builds one executor per `Sharing`
///   policy and `executeJson` dispatches through the handler's own real
///   policy, so this specific mis-dispatch is closed framework-side.
///   `OpenPoll` is still dispatched only via
///   `PollFormsController::openPoll(pollId)`, which calls the templated
///   `BridgeHandler<PollModel, AllowShared>::execute<OpenPoll>()` directly
///   — this rung was never migrated to route it through the now-fixed
///   generic path instead.
///   `GetPollState`/`GetEventsSince` take no user-entered fields at all (a
///   refresh and a polling tick, not something a person fills in), so both
///   are exposed as plain typed methods instead of schema forms — `Login`'s
///   own precedent notwithstanding, there is nothing here for a person to
///   type.
/// - `CreatePoll` also needs no session/token gate to render — this rung has
///   no signed-token mechanism at all (`polls::auth::PollsAuthorizer`'s own
///   `@file` comment); the admin/participant tokens it returns are opaque
///   strings the organizer copies out of `CreatePollResult` by hand.
///
namespace polls::gui {

/// @return `{"AddComment": …, "FinalizePoll": …, "UndoLastVoteChange": …}`.
[[nodiscard]] inline std::string pollSchemasJson() {
    return std::string{"{\"AddComment\":"} + ::morph::forms::schemaJson<polls::AddComment>() +
           ",\"FinalizePoll\":" + ::morph::forms::schemaJson<polls::FinalizePoll>() +
           ",\"UndoLastVoteChange\":" + ::morph::forms::schemaJson<polls::UndoLastVoteChange>() + "}";
}

}  // namespace polls::gui

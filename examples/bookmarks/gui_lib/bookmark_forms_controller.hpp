// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <exception>
#include <morph/core/bridge.hpp>
#include <morph/core/completion.hpp>
#include <morph/core/executor.hpp>
#include <string>
#include <utility>

#include "bookmarks/models/auth_model.hpp"
#include "bookmarks/models/bookmark_model.hpp"
#include "bookmarks/models/tag_model.hpp"

namespace bookmarks::gui {

/// @brief Same schema-driven surface as the shipped
///        `morph::qt::forms::FormsControllerCore<Model>`
///        (`schemasJson()`/`submitIfValid()`), composed over an injected
///        `Bridge&`/`IExecutor*` instead of constructing its own
///        `LocalBackend`. The shipped core's own `(Bridge&, IExecutor*,
///        schemasJson)` constructor now supports this directly, but this
///        rung still owns a thin controller of its own: it is templated
///        over a *single* model, and this rung's forms span three
///        (`AuthModel`/`BookmarkModel`/`TagModel`, see "The one thing that
///        is genuinely new here" below) — `dispatch()`'s routing has no
///        equivalent on the shipped core. Pure glue, no domain logic
///        (`examples/IMPLEMENTATION.md` rule 2 justification (b)) — the
///        schema/validation/rendering machinery is untouched; only the
///        backend-wiring seam differs. Verbatim in shape from
///        `pastebin::gui::PasteFormsController`, which established it.
///
/// @par The one thing that is genuinely new here: routing
/// The shipped core, and pastebin's copy of it, are templates over a *single*
/// model, because rung 1 had exactly one. This rung's forms span three
/// (`Login` on `AuthModel`, `CreateBookmark`/`EditBookmark`/`ImportBookmarks`
/// on `BookmarkModel`, `RenameTag`/`MergeTags` on `TagModel`), and
/// `BridgeHandler<Model>::executeJson` dispatches against the model type it
/// is instantiated for — so something has to map an action-type string to the
/// right handler. `dispatch()` below is that map and nothing else: a
/// six-entry lookup with no conditionals about *what* an action means. An
/// unrouted action type is reported through the caller's own error callback
/// rather than thrown, so a typo in QML surfaces as a message in the status
/// line like every other failure.
///
/// @par Handler lifetime, and why all three are constructed together
/// All three `BridgeHandler`s are members, so they are constructed together
/// (three registrations, no deregistrations) and destroyed together at
/// shutdown. That is deliberate: `QtWebSocketBackend::deregisterModel` now
/// assigns its fire-and-forget `deregister` envelope a real, tracked callId
/// rather than the `callId == 0` sentinel a subsequent synchronous
/// register/attach/assign call also used to use — closing a race that used
/// to be able to corrupt a freshly constructed handler's binding if it was
/// built on the same connection right after an older one was torn down. This
/// rung's handler-lifetime shape (all three built together, never rebuilt
/// mid-session) predates that fix and was never the shape the race needed
/// anyway: nothing in this rung's client destroys one handler and
/// constructs a different one on the same connection — the whole handler
/// set outlives login, and login only installs a session on the shared
/// `Bridge`.
///
/// @par No `fetchOptions()`
/// Deliberately absent, exactly as in `PasteFormsController`: it exists on
/// the shipped core to serve a `morph::forms::Choice<T, …>` field's combo-box
/// options, and none of this rung's DTOs declare a `Choice` field —
/// `CreateBookmark::visibility` is a plain reflected enum, not a
/// server-fetched choice. Adding an unused `fetchOptions()` would be a stub
/// with nothing to call it.
///
/// @par Known renderer limitation: array-typed members
/// `CreateBookmark::tags`/`EditBookmark::tags` are `std::vector<std::string>`
/// and reach `DynamicForm` as JSON-Schema `array` fields, for which the
/// shipped renderer has no control — it falls back to a plain text field
/// whose contents encode as a JSON *string*, which the server then rejects.
/// Both are optional members, so leaving them blank is well-defined and the
/// rest of each form works; typing into one produces a decode error in the
/// status line rather than silent corruption. Stated here rather than
/// smoothed over — see `examples/bookmarks/README.md`'s known-gaps entry.
class BookmarkFormsController {
public:
    /// @param bridge      The shared `Bridge` `AppContext` owns.
    /// @param executor    The executor `Completion` callbacks land on.
    /// @param schemasJson Pre-assembled `{actionType: schemaJson<A>()}` map,
    ///        matching `FormsControllerCore`'s own constructor contract —
    ///        `bookmark_schemas.hpp`'s `bookmarkSchemasJson()` builds the one
    ///        every shell passes.
    BookmarkFormsController(::morph::bridge::Bridge& bridge, ::morph::exec::IExecutor* executor,
                            std::string schemasJson);

    /// @brief The `{actionType: schema}` JSON supplied at construction.
    /// @return A reference to the cached schema-set JSON.
    [[nodiscard]] const std::string& schemasJson() const noexcept { return _schemasJson; }

    /// @brief Dispatches @p bodyJson as @p actionType's body via the generic
    ///        `executeJson` path on whichever model serves @p actionType,
    ///        invoking @p onReply / @p onError on the GUI thread once the
    ///        reply arrives.
    ///
    /// Same body as `FormsControllerCore::submitIfValid`
    /// (`include/morph/qt/forms/forms_controller_core.hpp`), with the single
    /// handler replaced by `dispatch()`'s routing and a `try`/`catch` around
    /// it — `dispatch()` is the only step that can fail synchronously (an
    /// unrouted or unregistered action type), and this turns that into the
    /// same asynchronous failure shape every other error takes. The
    /// `dispatch()` call is sequenced before either lambda is constructed, so
    /// @p onError is still intact in the handler.
    ///
    /// @tparam OnReply Callable invoked with the result JSON (`std::string`) on success.
    /// @tparam OnError Callable invoked with the `std::exception_ptr` on failure.
    /// @param actionType Registered action type id.
    /// @param bodyJson   Fully-assembled JSON body for the action.
    /// @param onReply    Success callback.
    /// @param onError    Failure callback.
    template <typename OnReply, typename OnError>
    void submitIfValid(std::string actionType, std::string bodyJson, OnReply onReply, OnError onError) {
        try {
            dispatch(actionType, bodyJson)
                .then(
                    [onReply = std::move(onReply)](std::string resultJson) mutable { onReply(std::move(resultJson)); })
                .onError([onError](const std::exception_ptr& err) mutable { onError(err); });
        } catch (...) {
            onError(std::current_exception());
        }
    }

private:
    /// @brief Routes @p actionType to the handler for the model that serves
    ///        it and starts the dispatch.
    /// @param actionType Registered action type id.
    /// @param bodyJson   Fully-assembled JSON body for the action.
    /// @return The in-flight completion carrying the result JSON.
    /// @throws std::runtime_error if no model in this controller serves
    ///         @p actionType (or if the action is unknown to the one that
    ///         does — `BridgeHandler::executeJson`'s own contract).
    [[nodiscard]] ::morph::async::Completion<std::string> dispatch(const std::string& actionType,
                                                                   const std::string& bodyJson);

    ::morph::bridge::BridgeHandler<AuthModel> _authHandler;
    ::morph::bridge::BridgeHandler<BookmarkModel> _bookmarkHandler;
    ::morph::bridge::BridgeHandler<TagModel> _tagHandler;
    std::string _schemasJson;
};

}  // namespace bookmarks::gui

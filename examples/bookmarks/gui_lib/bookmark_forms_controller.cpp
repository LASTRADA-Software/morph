// SPDX-License-Identifier: Apache-2.0
#include "bookmark_forms_controller.hpp"

#include <stdexcept>
#include <utility>

// submitIfValid() is a template (OnReply/OnError deduced per call site,
// exactly like FormsControllerCore's own) and so stays fully defined in the
// header; this translation unit holds the two things that need exactly one
// non-inline definition — the constructor and the action-type routing table.

namespace bookmarks::gui {

BookmarkFormsController::BookmarkFormsController(::morph::bridge::Bridge& bridge, ::morph::exec::IExecutor* executor,
                                                 std::string schemasJson)
    : _authHandler{bridge, executor},
      _bookmarkHandler{bridge, executor},
      _tagHandler{bridge, executor},
      _schemasJson{std::move(schemasJson)} {}

::morph::async::Completion<std::string> BookmarkFormsController::dispatch(const std::string& actionType,
                                                                          const std::string& bodyJson) {
    if (actionType == "Login") {
        return _authHandler.executeJson(actionType, bodyJson);
    }
    if (actionType == "CreateBookmark" || actionType == "EditBookmark" || actionType == "ImportBookmarks") {
        return _bookmarkHandler.executeJson(actionType, bodyJson);
    }
    if (actionType == "RenameTag" || actionType == "MergeTags") {
        return _tagHandler.executeJson(actionType, bodyJson);
    }
    // Reported, never silently dropped: the QML side names action types as
    // strings, so a typo has to arrive somewhere a human can read it.
    throw std::runtime_error{"no model in this client serves action '" + actionType + "'"};
}

}  // namespace bookmarks::gui

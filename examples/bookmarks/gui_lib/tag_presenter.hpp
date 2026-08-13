// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "gui/presenter.hpp"
#include "bookmarks/dto/tag_dto.hpp"

#include <exception>

// See pastebin::gui::PastePresenter's identical guard and doc comment
// (examples/pastebin/gui_lib/paste_presenter.hpp) for why moc must never
// see morph/core/bridge.hpp or tag_model.hpp: tag_model.hpp pulls in
// Lightweight's DataMapper machinery through bookmarks/db/db_model.hpp, and
// moc's parser (not a real C++ front end) mis-parses the nesting that
// results.
#ifndef Q_MOC_RUN
#include "bookmarks/models/tag_model.hpp"

#include <morph/core/bridge.hpp>
#include <morph/core/executor.hpp>
#endif

namespace bookmarks::gui {

/// @brief Routes every `TagModel` action through a `BridgeHandler<TagModel>`.
///        Translates and routes only — no domain logic (`IMPLEMENTATION.md`
///        rule 2).
class TagPresenter : public ::morph::ladder::gui::Presenter {
    Q_OBJECT
  public:
    /// @param bridge   The shared `Bridge` `AppContext` owns.
    /// @param executor The executor `Completion` callbacks land on.
    /// @param parent   Optional `QObject` parent.
    TagPresenter(::morph::bridge::Bridge& bridge, ::morph::exec::IExecutor* executor, QObject* parent = nullptr);

    /// @brief Renames a tag. Emits `renamed` on success, `failed` on error.
    /// @param action The rename to apply.
    void rename(RenameTag action);

    /// @brief Reassigns every bookmark tagged `sourceId` to `targetId`, then
    ///        deletes `sourceId`. Emits `merged` on success, `failed` on error.
    /// @param action The merge to apply.
    void merge(MergeTags action);

    /// @brief Lists every tag the caller owns, with bookmark counts. Emits
    ///        `listed` on success, `failed` on error.
    /// @param action The list request.
    void list(ListTags action);

  signals:
    void renamed();
    void merged();
    void listed(ListTagsResult result);
    /// @brief Emitted for any action's typed error — @p message is
    ///        `std::exception::what()`, ready for direct display.
    void failed(QString message);

  private:
    /// @brief Shared error-display body passed as every `track()` call's
    ///        third argument below — see `pastebin::gui::PastePresenter::reportError`'s
    ///        doc comment for the full rationale (finding 023).
    void reportError(const std::exception_ptr& err);

    ::morph::bridge::BridgeHandler<TagModel> _handler;
};

}  // namespace bookmarks::gui

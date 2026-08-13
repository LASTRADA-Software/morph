// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "gui/presenter.hpp"
#include "bookmarks/dto/bookmark_dto.hpp"
#include "bookmarks/dto/bulk_dto.hpp"
#include "bookmarks/dto/import_export_dto.hpp"

#include <exception>

// See pastebin::gui::PastePresenter's identical guard and doc comment
// (examples/pastebin/gui_lib/paste_presenter.hpp) for why moc must never
// see morph/core/bridge.hpp or bookmark_model.hpp: bookmark_model.hpp pulls
// in Lightweight's DataMapper machinery through bookmarks/db/db_model.hpp,
// and moc's parser (not a real C++ front end) mis-parses the nesting that
// results, mistaking `namespace bookmarks::gui { ... }` below for still
// being nested inside a stray `Lightweight::` namespace.
#ifndef Q_MOC_RUN
#include "bookmarks/models/bookmark_model.hpp"

#include <morph/core/bridge.hpp>
#include <morph/core/executor.hpp>
#endif

namespace bookmarks::gui {

/// @brief Routes every `BookmarkModel` action through a
///        `BridgeHandler<BookmarkModel>`. Translates and routes only — no
///        domain logic (`IMPLEMENTATION.md` rule 2).
///
/// `RecordMetadata` is deliberately absent: it is dispatched exclusively by
/// the app-layer metadata-fetch worker's internal client, authenticated as
/// `bookmarks::auth::kMetadataFetcherPrincipal`, never by a GUI client
/// (`bookmark_dto.hpp`'s own `@file` comment) — so it gets no presenter
/// method, mirroring `pastebin::ExpirePaste`'s identical "internal-only"
/// exclusion.
class BookmarkPresenter : public ::morph::ladder::gui::Presenter {
    Q_OBJECT
  public:
    /// @param bridge   The shared `Bridge` `AppContext` owns.
    /// @param executor The executor `Completion` callbacks land on.
    /// @param parent   Optional `QObject` parent.
    BookmarkPresenter(::morph::bridge::Bridge& bridge, ::morph::exec::IExecutor* executor, QObject* parent = nullptr);

    /// @brief Stores a new bookmark. Emits `created` on success, `failed` on error.
    /// @param action The bookmark to store.
    void create(CreateBookmark action);

    /// @brief Replaces an editable bookmark's fields with a full replace-set.
    ///        Emits `edited` on success, `failed` on error.
    /// @param action The edit to apply.
    void edit(EditBookmark action);

    /// @brief Archives a bookmark. Emits `archived` on success, `failed` on error.
    /// @param action The bookmark to archive.
    void archive(ArchiveBookmark action);

    /// @brief Unarchives a bookmark. Emits `unarchived` on success, `failed` on error.
    /// @param action The bookmark to unarchive.
    void unarchive(UnarchiveBookmark action);

    /// @brief Deletes a bookmark. Emits `removed` on success, `failed` on error.
    /// @param action The bookmark to delete.
    void remove(DeleteBookmark action);

    /// @brief Reads one bookmark. Emits `loaded` on success, `failed` on error.
    /// @param action The bookmark to read.
    void get(GetBookmark action);

    /// @brief Fetches one page of the caller's own bookmarks. Emits `listed`
    ///        on success, `failed` on error.
    /// @param action The page/filter request.
    void list(ListBookmarks action);

    /// @brief Polls every bookmark the caller touched since a given instant.
    ///        Emits `changesSince` on success, `failed` on error.
    /// @param action The poll request.
    void getChangesSince(GetChangesSince action);

    /// @brief Applies one atomic edit across several bookmarks. Emits
    ///        `bulkEdited` on success, `failed` on error.
    /// @param action The batch edit to apply.
    void bulkEdit(BulkEdit action);

    /// @brief Imports one chunk of a Netscape Bookmark HTML import. Emits
    ///        `imported` on success, `failed` on error.
    /// @param action The chunk to import.
    void importChunk(ImportBookmarks action);

    /// @brief Exports every one of the caller's bookmarks. Emits `exported`
    ///        on success, `failed` on error.
    /// @param action The export request.
    void exportAll(ExportBookmarks action);

  signals:
    void created(CreateBookmarkResult result);
    void edited(BookmarkView view);
    void archived();
    void unarchived();
    void removed();
    void loaded(BookmarkView view);
    void listed(ListBookmarksResult result);
    void changesSince(GetChangesSinceResult result);
    void bulkEdited(BulkEditResult result);
    void imported(ImportBookmarksResult result);
    void exported(ExportBookmarksResult result);
    /// @brief Emitted for any action's typed error — @p message is
    ///        `std::exception::what()`, ready for direct display.
    void failed(QString message);

  private:
    /// @brief Shared error-display body passed as every `track()` call's
    ///        third argument below — see `pastebin::gui::PastePresenter::reportError`'s
    ///        doc comment (`examples/pastebin/gui_lib/paste_presenter.hpp`) for the
    ///        full rationale (finding 023: `Completion<T>::onError` keeps only
    ///        the single most-recently-attached handler, so this must be
    ///        passed as `track()`'s `onErr` parameter, never attached via a
    ///        separate `.onError()` call beforehand).
    void reportError(const std::exception_ptr& err);

    ::morph::bridge::BridgeHandler<BookmarkModel> _handler;
};

}  // namespace bookmarks::gui

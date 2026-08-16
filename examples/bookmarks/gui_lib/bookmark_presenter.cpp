// SPDX-License-Identifier: Apache-2.0
#include "bookmark_presenter.hpp"

namespace bookmarks::gui {

BookmarkPresenter::BookmarkPresenter(::morph::bridge::Bridge& bridge, ::morph::exec::IExecutor* executor,
                                     QObject* parent)
    : Presenter{parent}, _handler{bridge, executor} {
    trackBound(_handler.whenBound());
}

void BookmarkPresenter::reportError(const std::exception_ptr& err) {
    try {
        std::rethrow_exception(err);
    } catch (const std::exception& ex) {
        emit failed(QString::fromStdString(ex.what()));
    }
}

void BookmarkPresenter::create(CreateBookmark action) {
    track<CreateBookmarkResult>(
        _handler.execute(std::move(action)), [this](CreateBookmarkResult result) { emit created(std::move(result)); },
        [this](const std::exception_ptr& err) { reportError(err); });
}

void BookmarkPresenter::edit(EditBookmark action) {
    track<BookmarkView>(
        _handler.execute(std::move(action)), [this](BookmarkView view) { emit edited(std::move(view)); },
        [this](const std::exception_ptr& err) { reportError(err); });
}

void BookmarkPresenter::archive(ArchiveBookmark action) {
    track<Ack>(
        _handler.execute(std::move(action)), [this](Ack) { emit archived(); },
        [this](const std::exception_ptr& err) { reportError(err); });
}

void BookmarkPresenter::unarchive(UnarchiveBookmark action) {
    track<Ack>(
        _handler.execute(std::move(action)), [this](Ack) { emit unarchived(); },
        [this](const std::exception_ptr& err) { reportError(err); });
}

void BookmarkPresenter::remove(DeleteBookmark action) {
    track<Ack>(
        _handler.execute(std::move(action)), [this](Ack) { emit removed(); },
        [this](const std::exception_ptr& err) { reportError(err); });
}

void BookmarkPresenter::get(GetBookmark action) {
    track<BookmarkView>(
        _handler.execute(std::move(action)), [this](BookmarkView view) { emit loaded(std::move(view)); },
        [this](const std::exception_ptr& err) { reportError(err); });
}

void BookmarkPresenter::list(ListBookmarks action) {
    track<ListBookmarksResult>(
        _handler.execute(std::move(action)), [this](ListBookmarksResult result) { emit listed(std::move(result)); },
        [this](const std::exception_ptr& err) { reportError(err); });
}

void BookmarkPresenter::getChangesSince(GetChangesSince action) {
    track<GetChangesSinceResult>(
        _handler.execute(std::move(action)),
        [this](GetChangesSinceResult result) { emit changesSince(std::move(result)); },
        [this](const std::exception_ptr& err) { reportError(err); });
}

void BookmarkPresenter::bulkEdit(BulkEdit action) {
    track<BulkEditResult>(
        _handler.execute(std::move(action)), [this](BulkEditResult result) { emit bulkEdited(std::move(result)); },
        [this](const std::exception_ptr& err) { reportError(err); });
}

void BookmarkPresenter::importChunk(ImportBookmarks action) {
    track<ImportBookmarksResult>(
        _handler.execute(std::move(action)),
        [this](ImportBookmarksResult result) { emit imported(std::move(result)); },
        [this](const std::exception_ptr& err) { reportError(err); });
}

void BookmarkPresenter::exportAll(ExportBookmarks action) {
    track<ExportBookmarksResult>(
        _handler.execute(std::move(action)),
        [this](ExportBookmarksResult result) { emit exported(std::move(result)); },
        [this](const std::exception_ptr& err) { reportError(err); });
}

}  // namespace bookmarks::gui

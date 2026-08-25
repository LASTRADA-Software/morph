// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <exception>

#include "bookmarks/dto/shared_feed_dto.hpp"
#include "gui/presenter.hpp"

// See pastebin::gui::PastePresenter's identical guard and doc comment
// (examples/pastebin/gui_lib/paste_presenter.hpp) for why moc must never see
// morph/core/bridge.hpp: its template machinery produces bogus moc output
// the same way shared_feed_model.hpp historically did when it transitively
// pulled in Lightweight's DataMapper machinery through the since-removed
// bookmarks/db/db_model.hpp -- shared_feed_model.hpp itself no longer has
// any Lightweight/ODBC dependency at all, now that SharedFeedModel acquires
// a connection per execute() call from Lightweight::GlobalDataMapperPool()
// instead of owning one, but this guard stays for bridge.hpp's own sake.
#ifndef Q_MOC_RUN
#include <morph/core/bridge.hpp>
#include <morph/core/executor.hpp>

#include "bookmarks/models/shared_feed_model.hpp"
#endif

namespace bookmarks::gui {

/// @brief Routes `SharedFeedModel`'s one action through a
///        `BridgeHandler<SharedFeedModel>`. Translates and routes only — no
///        domain logic (`IMPLEMENTATION.md` rule 2).
class SharedFeedPresenter : public ::morph::ladder::gui::Presenter {
    Q_OBJECT
public:
    /// @param bridge   The shared `Bridge` `AppContext` owns.
    /// @param executor The executor `Completion` callbacks land on.
    /// @param parent   Optional `QObject` parent.
    SharedFeedPresenter(::morph::bridge::Bridge& bridge, ::morph::exec::IExecutor* executor,
                        QObject* parent = nullptr);

    /// @brief Fetches one page of every `Shared`, non-archived bookmark from
    ///        every owner. Emits `listed` on success, `failed` on error.
    /// @param action The page request.
    void list(ListSharedFeed action);

signals:
    void listed(ListSharedFeedResult result);
    /// @brief Emitted for any action's typed error — @p message is
    ///        `std::exception::what()`, ready for direct display.
    void failed(QString message);

private:
    /// @brief Shared error-display body passed as every `track()` call's
    ///        third argument below — see `Presenter::track()`'s doc comment
    ///        (`examples/common/gui/presenter.hpp`) for why.
    void reportError(const std::exception_ptr& err);

    ::morph::bridge::BridgeHandler<SharedFeedModel> _handler;
};

}  // namespace bookmarks::gui

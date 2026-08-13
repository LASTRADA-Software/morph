// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "gui/presenter.hpp"
#include "bookmarks/dto/shared_feed_dto.hpp"

#include <exception>

// See pastebin::gui::PastePresenter's identical guard and doc comment
// (examples/pastebin/gui_lib/paste_presenter.hpp) for why moc must never
// see morph/core/bridge.hpp or shared_feed_model.hpp: shared_feed_model.hpp
// pulls in Lightweight's DataMapper machinery through
// bookmarks/db/db_model.hpp, and moc's parser (not a real C++ front end)
// mis-parses the nesting that results.
#ifndef Q_MOC_RUN
#include "bookmarks/models/shared_feed_model.hpp"

#include <morph/core/bridge.hpp>
#include <morph/core/executor.hpp>
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

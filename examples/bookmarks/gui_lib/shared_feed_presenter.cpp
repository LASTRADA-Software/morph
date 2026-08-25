// SPDX-License-Identifier: Apache-2.0
#include "shared_feed_presenter.hpp"

#include "gui/error_text.hpp"

namespace bookmarks::gui {

SharedFeedPresenter::SharedFeedPresenter(::morph::bridge::Bridge& bridge, ::morph::exec::IExecutor* executor,
                                         QObject* parent)
    : Presenter{parent}, _handler{bridge, executor} {
    trackBound(_handler.whenBound());
}

void SharedFeedPresenter::reportError(const std::exception_ptr& err) {
    emit failed(::morph::ladder::gui::errorText(err));
}

void SharedFeedPresenter::list(ListSharedFeed action) {
    track<ListSharedFeedResult>(
        _handler.execute(std::move(action)), [this](ListSharedFeedResult result) { emit listed(std::move(result)); },
        [this](const std::exception_ptr& err) { reportError(err); });
}

}  // namespace bookmarks::gui

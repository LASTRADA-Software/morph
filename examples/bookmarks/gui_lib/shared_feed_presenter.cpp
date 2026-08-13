// SPDX-License-Identifier: Apache-2.0
#include "shared_feed_presenter.hpp"

namespace bookmarks::gui {

SharedFeedPresenter::SharedFeedPresenter(::morph::bridge::Bridge& bridge, ::morph::exec::IExecutor* executor,
                                         QObject* parent)
    : Presenter{parent}, _handler{bridge, executor} {}

void SharedFeedPresenter::reportError(const std::exception_ptr& err) {
    try {
        std::rethrow_exception(err);
    } catch (const std::exception& ex) {
        emit failed(QString::fromStdString(ex.what()));
    }
}

void SharedFeedPresenter::list(ListSharedFeed action) {
    track<ListSharedFeedResult>(
        _handler.execute(std::move(action)), [this](ListSharedFeedResult result) { emit listed(std::move(result)); },
        [this](const std::exception_ptr& err) { reportError(err); });
}

}  // namespace bookmarks::gui

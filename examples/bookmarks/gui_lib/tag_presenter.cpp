// SPDX-License-Identifier: Apache-2.0
#include "tag_presenter.hpp"
#include "gui/error_text.hpp"

namespace bookmarks::gui {

TagPresenter::TagPresenter(::morph::bridge::Bridge& bridge, ::morph::exec::IExecutor* executor, QObject* parent)
    : Presenter{parent}, _handler{bridge, executor} {
    trackBound(_handler.whenBound());
}

void TagPresenter::reportError(const std::exception_ptr& err) {
    emit failed(::morph::ladder::gui::errorText(err));
}

void TagPresenter::rename(RenameTag action) {
    track<Ack>(
        _handler.execute(std::move(action)), [this](Ack) { emit renamed(); },
        [this](const std::exception_ptr& err) { reportError(err); });
}

void TagPresenter::merge(MergeTags action) {
    track<Ack>(
        _handler.execute(std::move(action)), [this](Ack) { emit merged(); },
        [this](const std::exception_ptr& err) { reportError(err); });
}

void TagPresenter::list(ListTags action) {
    track<ListTagsResult>(
        _handler.execute(std::move(action)), [this](ListTagsResult result) { emit listed(std::move(result)); },
        [this](const std::exception_ptr& err) { reportError(err); });
}

}  // namespace bookmarks::gui

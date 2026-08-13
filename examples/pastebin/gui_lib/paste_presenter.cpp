// SPDX-License-Identifier: Apache-2.0
#include "paste_presenter.hpp"

namespace pastebin::gui {

PastePresenter::PastePresenter(::morph::bridge::Bridge& bridge, ::morph::exec::IExecutor* executor, QObject* parent)
    : Presenter{parent}, _handler{bridge, executor} {
    trackBound(_handler.whenBound());
}

void PastePresenter::reportError(const std::exception_ptr& err) {
    try {
        std::rethrow_exception(err);
    } catch (const std::exception& e) {
        emit failed(QString::fromStdString(e.what()));
    }
}

void PastePresenter::create(CreatePaste action) {
    track<CreatePasteResult>(
        _handler.execute(std::move(action)), [this](CreatePasteResult result) { emit created(std::move(result)); },
        [this](const std::exception_ptr& err) { reportError(err); });
}

void PastePresenter::get(GetPaste action) {
    track<PasteView>(
        _handler.execute(std::move(action)), [this](PasteView view) { emit loaded(std::move(view)); },
        [this](const std::exception_ptr& err) { reportError(err); });
}

void PastePresenter::edit(EditPaste action) {
    track<PasteView>(
        _handler.execute(std::move(action)), [this](PasteView view) { emit edited(std::move(view)); },
        [this](const std::exception_ptr& err) { reportError(err); });
}

void PastePresenter::remove(DeletePaste action) {
    track<Ack>(
        _handler.execute(std::move(action)), [this](Ack) { emit removed(); },
        [this](const std::exception_ptr& err) { reportError(err); });
}

void PastePresenter::list(ListPastes action) {
    track<ListPastesResult>(
        _handler.execute(std::move(action)), [this](ListPastesResult result) { emit listed(std::move(result)); },
        [this](const std::exception_ptr& err) { reportError(err); });
}

}  // namespace pastebin::gui

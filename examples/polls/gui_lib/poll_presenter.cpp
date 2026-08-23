// SPDX-License-Identifier: Apache-2.0
#include "poll_presenter.hpp"
#include "gui/error_text.hpp"

namespace polls::gui {

PollPresenter::PollPresenter(::morph::bridge::Bridge& bridge, ::morph::exec::IExecutor* executor, QObject* parent)
    : Presenter{parent}, _creator{bridge, executor}, _handler{bridge, executor} {}

void PollPresenter::reportError(const std::exception_ptr& err) {
    emit failed(::morph::ladder::gui::errorText(err));
}

void PollPresenter::createPoll(CreatePoll action) {
    track<CreatePollResult>(
        _creator.execute(std::move(action)), [this](CreatePollResult result) { emit created(std::move(result)); },
        [this](const std::exception_ptr& err) { reportError(err); });
}

void PollPresenter::openPoll(std::string pollId) {
    track<GetPollStateResult>(
        _handler.execute(OpenPoll{.pollId = std::move(pollId)}),
        [this](GetPollStateResult result) { emit opened(std::move(result)); },
        [this](const std::exception_ptr& err) { reportError(err); });
}

void PollPresenter::getPollState(GetPollState action) {
    track<GetPollStateResult>(
        _handler.execute(std::move(action)),
        [this](GetPollStateResult result) { emit stateLoaded(std::move(result)); },
        [this](const std::exception_ptr& err) { reportError(err); });
}

void PollPresenter::submitVotes(SubmitVotes action) {
    track<GetPollStateResult>(
        _handler.execute(std::move(action)),
        [this](GetPollStateResult result) { emit votesSubmitted(std::move(result)); },
        [this](const std::exception_ptr& err) { reportError(err); });
}

void PollPresenter::updateVotes(UpdateVotes action) {
    track<GetPollStateResult>(
        _handler.execute(std::move(action)),
        [this](GetPollStateResult result) { emit votesUpdated(std::move(result)); },
        [this](const std::exception_ptr& err) { reportError(err); });
}

void PollPresenter::addComment(AddComment action) {
    track<GetPollStateResult>(
        _handler.execute(std::move(action)),
        [this](GetPollStateResult result) { emit commentAdded(std::move(result)); },
        [this](const std::exception_ptr& err) { reportError(err); });
}

void PollPresenter::finalizePoll(FinalizePoll action) {
    track<GetPollStateResult>(
        _handler.execute(std::move(action)),
        [this](GetPollStateResult result) { emit finalized(std::move(result)); },
        [this](const std::exception_ptr& err) { reportError(err); });
}

void PollPresenter::undoLastVoteChange(UndoLastVoteChange action) {
    track<UndoLastVoteChangeResult>(
        _handler.execute(std::move(action)),
        [this](UndoLastVoteChangeResult result) { emit voteChangeUndone(std::move(result)); },
        [this](const std::exception_ptr& err) { reportError(err); });
}

void PollPresenter::getEventsSince(GetEventsSince action) {
    track<GetEventsSinceResult>(
        _handler.execute(std::move(action)),
        [this](GetEventsSinceResult result) { emit eventsReceived(std::move(result)); },
        [this](const std::exception_ptr& err) { reportError(err); });
}

}  // namespace polls::gui

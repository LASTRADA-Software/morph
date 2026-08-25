// SPDX-License-Identifier: Apache-2.0
#include "poll_forms_controller.hpp"

#include <utility>

namespace polls::gui {

PollFormsController::PollFormsController(::morph::bridge::Bridge& bridge, ::morph::exec::IExecutor* executor,
                                         std::string schemasJson)
    : _handler{bridge, executor}, _schemasJson{std::move(schemasJson)} {}

::morph::async::Completion<GetPollStateResult> PollFormsController::openPoll(std::string pollId) {
    return _handler.execute(OpenPoll{.pollId = std::move(pollId)});
}

::morph::async::Completion<GetPollStateResult> PollFormsController::getPollState() {
    return _handler.execute(GetPollState{});
}

::morph::async::Completion<GetPollStateResult> PollFormsController::submitVotes(SubmitVotes action) {
    return _handler.execute(std::move(action));
}

::morph::async::Completion<GetPollStateResult> PollFormsController::updateVotes(UpdateVotes action) {
    return _handler.execute(std::move(action));
}

::morph::async::Completion<GetEventsSinceResult> PollFormsController::getEventsSince(GetEventsSince action) {
    return _handler.execute(std::move(action));
}

}  // namespace polls::gui

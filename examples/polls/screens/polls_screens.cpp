// SPDX-License-Identifier: Apache-2.0

#include "polls_screens.hpp"

#include <expected>
#include <memory>
#include <morph/ui/author.hpp>
#include <morph/ui/service.hpp>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "polls/dto/event_dto.hpp"
#include "polls/dto/poll_dto.hpp"
#include "polls/dto/vote_dto.hpp"
#include "polls/models/poll_model.hpp"

namespace polls::screens {

namespace ui = morph::ui::author;

namespace {

// Creating a poll: the forms engine edits the option list (2 to 20 drafts, added and removed), and the new poll opens.
ui::Screen createScreen() {
    return ui::screen(std::string{kCreate}, [](ui::Scope& screen) {
        screen.title("New poll");
        auto const poll = screen.model<PollModel>("poll", {.instance = ui::Instance::Private});
        return ui::form<CreatePoll>(
                   poll, {.submit = "explicit",
                          .onSuccess = {ui::navigate(std::string{kVote},
                                                     {.params = ui::obj({{"pollId", ui::ref("event.pollId")}})})}})
            .id(ui::element("createForm"))
            .testId("createForm");
    });
}

// Voting. The poll model is keyed by its id, so every screen on the poll shares the instance; opening is a `latest`
// mutation, so only the newest open applies, and a backend switch re-opens it. The participant's picks are the vote
// form's draft, client state a reconnect keeps. The first vote is `SubmitVotes`; once one has succeeded,
// `UpdateVotes`.
ui::Screen voteScreen() {
    return ui::screen(std::string{kVote}, [](ui::Scope& screen) {
        auto const pollId = screen.param<std::optional<std::string>>("pollId");
        auto const poll = screen.model<PollModel>("poll", {.instance = pollId});
        auto const opened = screen.state("opened", false);
        auto const voted = screen.state("voted", false);
        auto const open = screen.mutation<OpenPoll>("open", poll,
                                                    {.body = {{&OpenPoll::pollId, pollId}},
                                                     .concurrency = ui::Mode::Latest,
                                                     .onSuccess = {ui::set(opened, true)}});
        auto const changed = poll.actions<SubmitVotes, UpdateVotes, AddComment, FinalizePoll>();
        auto const state = screen.query<GetPollState>("state", poll, {}, {.when = opened, .refreshOn = changed});
        auto const feed = screen.query<GetEventsSince>("feed", poll, {}, {.when = opened, .refreshOn = changed});
        auto const view = state.value();
        auto const options = view.at(&GetPollStateResult::options);
        screen.title(ui::coalesce({view.at(&GetPollStateResult::title), ui::t("polls.poll", "Poll")}));
        screen.identity(pollId);
        screen.onMount(ui::ifThen(ui::engaged(pollId), ui::run(open)));
        screen.onBackendChange(ui::ifThen(ui::engaged(pollId), ui::run(open)));
        auto const name = ui::element("participant");
        auto const picks = ui::obj(
            {{"participantName", name.value()}, {"votes", ui::map(options, "o", [](ui::Handle<ui::Any> const& option) {
                                                     return ui::obj({{"optionId", option.at("id")},
                                                                     {"choice", ui::AnyHandle{std::string{"No"}}}});
                                                 })}});
        auto const votedNow = ui::Commands{ui::set(voted, true), ui::refetch(state), ui::refetch(feed)};
        return ui::column(
            {ui::text({.text = view.at(&GetPollStateResult::title)}).testId("title"),
             ui::table(
                 options,
                 {.key = ui::row("id"),
                  .columns =
                      {{.name = "option", .label = "Option", .cell = ui::text({.text = ui::row("label")})},
                       {.name = "yes", .label = "Yes", .cell = ui::text({.text = ui::row("yesCount")})},
                       {.name = "maybe", .label = "If need be", .cell = ui::text({.text = ui::row("ifNeedBeCount")})},
                       {.name = "no", .label = "No", .cell = ui::text({.text = ui::row("noCount")})}}})
                 .a11y("Options")
                 .testId("options"),
             ui::textInput({.label = "Your name", .placeholder = "Name"}).id(name).testId("participant"),
             ui::form<SubmitVotes>(poll, {.prefill = picks, .submit = "explicit", .onSuccess = votedNow})
                 .id(ui::element("voteForm"))
                 .testId("voteForm")
                 .visible(!voted),
             ui::form<UpdateVotes>(poll, {.prefill = picks, .submit = "explicit", .onSuccess = votedNow})
                 .id(ui::element("updateForm"))
                 .testId("updateForm")
                 .visible(voted),
             ui::form<AddComment>(poll, {.prefill = ui::obj({{"participantName", name.value()}}),
                                         .onSuccess = {ui::refetch(state), ui::refetch(feed)}})
                 .id(ui::element("commentForm"))
                 .testId("commentForm"),
             ui::table(feed.value().at(&GetEventsSinceResult::events),
                       {.key = ui::row("id"),
                        .columns = {{.name = "summary",
                                     .label = "Activity",
                                     .cell = ui::text({.text = ui::row("summary")})}}})
                 .a11y("Activity")
                 .testId("feed"),
             ui::text({.text = ui::coalesce({open.error(), state.error(), feed.error()})}).testId("error")},
            {.gap = 8});
    });
}

}  // namespace

std::expected<std::shared_ptr<morph::ui::ServedBundle const>, morph::ui::Refusal> bundle() {
    ui::ScreenRegistry screens;
    struct Listed {
        std::string id;
        std::string title;
        decltype(screens.add(createScreen())) registered;
    };
    std::vector<Listed> listed;
    listed.push_back({.id = std::string{kCreate}, .title = "New poll", .registered = screens.add(createScreen())});
    listed.push_back({.id = std::string{kVote}, .title = "Poll", .registered = screens.add(voteScreen())});
    for (Listed const& screen : listed) {
        if (!screen.registered) {
            return std::unexpected(screen.registered.error());
        }
    }
    morph::ui::UiRegistry served{morph::ui::ApplicationInfo{.id = std::string{kApplicationId}, .version = "1.0"},
                                 screens.catalog()};
    for (Listed const& screen : listed) {
        morph::ui::CatalogEntry entry;
        entry.id = screen.id;
        entry.title = screen.title;
        if (auto added = served.addScreen(std::move(entry), (*screen.registered)->text); !added) {
            return std::unexpected(std::move(added.error()));
        }
    }
    return served.build();
}

}  // namespace polls::screens

// SPDX-License-Identifier: Apache-2.0

#include "kanban_screens.hpp"

#include <cstdint>
#include <expected>
#include <memory>
#include <morph/session/session_auth.hpp>
#include <morph/ui/author.hpp>
#include <morph/ui/service.hpp>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "kanban/auth/kanban_authorizer.hpp"
#include "kanban/dto/auth_dto.hpp"
#include "kanban/dto/board_dto.hpp"
#include "kanban/dto/project_dto.hpp"
#include "kanban/models/board_model.hpp"
#include "kanban/models/project_admin_model.hpp"

namespace kanban::screens {

namespace ui = morph::ui::author;

namespace {

// Sign-in, then the person's projects: creating one opens its board, and each listed project opens its own.
ui::Screen projectsScreen() {
    return ui::screen(std::string{kProjects}, [](ui::Scope& screen) {
        screen.title("Projects");
        auto const auth = screen.model<AuthModel>("auth", {.instance = ui::Instance::Private});
        auto const admin = screen.model<ProjectAdminModel>("admin", {.instance = ui::Instance::Private});
        auto const signedIn = screen.state("signedIn", false);
        auto const username = ui::element("username");
        auto const projects = screen.query<GetMyProjects>("projects", admin, {}, {.when = signedIn});
        // The reply's principal is installed first, then the gate starts the query that needs it.
        auto const login =
            screen.mutation<Login>("login", auth,
                                   {.body = {{&Login::username, username.value()}},
                                    .onSuccess = {ui::signIn(ui::MutationRef{"login"}), ui::set(signedIn, true)}});
        auto const openBoard = [](ui::AnyHandle const& projectId) {
            return ui::navigate(std::string{kBoard}, {.params = ui::obj({{"projectId", projectId}})});
        };
        auto const rows = projects.value().at(&GetMyProjectsResult::projects);
        return ui::column(
            {ui::row({ui::textInput({.label = "Username", .placeholder = "Username"}).id(username).testId("username"),
                      ui::button({.label = "Sign in", .onClick = ui::run(login)}).testId("signIn")},
                     {.gap = 8})
                 .visible(!signedIn),
             ui::form<CreateProject>(admin, {.onSuccess = {openBoard(ui::ref("event.id"))}})
                 .id(ui::element("createForm"))
                 .testId("createForm")
                 .visible(signedIn),
             ui::table(rows,
                       {.key = ui::row("id"),
                        .columns = {{.name = "name", .label = "Project", .cell = ui::text({.text = ui::row("name")})},
                                    {.name = "open",
                                     .label = "",
                                     .cell = ui::button({.label = "Open", .onClick = openBoard(ui::row("id"))})}}})
                 .a11y("Projects")
                 .testId("projects")
                 .visible(signedIn),
             ui::text({.text = ui::format("{0} projects", {ui::count(rows)})}).testId("count").visible(signedIn),
             ui::text({.text = ui::coalesce({login.error(), projects.error()})}).testId("error")},
            {.gap = 8});
    });
}

// The board: swimlanes by columns of cards. A card is draggable with its task id; a cell is a drop target that moves
// the task to the end of that cell, and a card is one that moves it before the card. Moves are `serial`, one queue for
// the board: a `serialKey` cannot read the run's task, so two drops of one card apply in order by sharing it.
ui::Screen boardScreen() {
    return ui::screen(std::string{kBoard}, [](ui::Scope& screen) {
        auto const projectId = screen.param<std::optional<std::int64_t>>("projectId");
        auto const board = screen.model<BoardModel>("board", {.instance = projectId});
        auto const opened = screen.state("opened", false);
        auto const open = screen.mutation<OpenBoard>(
            "open", board, {.body = {{&OpenBoard::projectId, projectId}}, .onSuccess = {ui::set(opened, true)}});
        auto const state = screen.query<GetBoardState>(
            "state", board, {},
            {.when = opened,
             .refreshOn = board.actions<CreateColumn, CreateSwimlane, CreateTask, MoveTaskPosition, AddComment>()});
        auto const move = screen.mutation<MoveTaskPosition>("move", board, {.concurrency = ui::Mode::Serial});
        auto const view = state.value();
        auto const columns = view.at(&GetBoardResult::columns);
        auto const lanes = view.at(&GetBoardResult::swimlanes);
        auto const tasks = view.at(&GetBoardResult::tasks);
        screen.title(ui::coalesce({view.at(&GetBoardResult::name), ui::t("kanban.board", "Board")}));
        screen.identity(projectId);
        screen.onMount(ui::ifThen(ui::engaged(projectId), ui::run(open)));

        auto const grid = ui::forEach(lanes, {.key = ui::row("id"), .gap = 8}, [&](ui::DeclScope& laneScope) {
            auto const lane = laneScope.let("lane", ui::row());
            return ui::Nodes{
                ui::text({.text = lane.at("name")}).testId("lane"),
                ui::forEach(columns, {.key = ui::row("id"), .axis = std::string{"horizontal"}, .gap = 8},
                            [&](ui::DeclScope& columnScope) {
                                auto const column = columnScope.let("column", ui::row());
                                auto const cards = columnScope.let(
                                    "cards", ui::filter(tasks, "t", [&](ui::Handle<ui::Any> const& task) {
                                        return ui::eq(task.at("swimlaneId"), lane.at("id")) &&
                                               ui::eq(task.at("columnId"), column.at("id"));
                                    }));
                                auto const moveTo = [&](ui::AnyHandle const& position) {
                                    return ui::run(move, {{&MoveTaskPosition::taskId, ui::ref("event")},
                                                          {&MoveTaskPosition::columnId, column.at("id")},
                                                          {&MoveTaskPosition::swimlaneId, lane.at("id")},
                                                          {&MoveTaskPosition::position, position}});
                                };
                                auto const card = ui::forEach(cards, {.key = ui::row("id")}, [&](ui::DeclScope&) {
                                    return ui::Nodes{ui::text({.text = ui::row("title")})
                                                         .testId("card")
                                                         .drag(ui::row("id"))
                                                         .drop({}, {moveTo(ui::row("position"))})};
                                });
                                return ui::Nodes{ui::panel({card}, {.title = column.at("name"), .padding = 4})
                                                     .testId("cell")
                                                     .drop({}, {moveTo(ui::count(cards))})};
                            })};
        });
        // A form's submit is not a mutation, so `refreshOn` does not see it: each form refetches the board itself.
        auto const refetched = ui::Commands{ui::refetch(state)};
        auto const firstColumn = screen.let("firstColumn", ui::call("first", {columns}));
        auto const firstLane = screen.let("firstLane", ui::call("first", {lanes}));
        auto const firstPlace = ui::obj({{"columnId", firstColumn.at("id")}, {"swimlaneId", firstLane.at("id")}});
        return ui::column(
            {ui::text({.text = view.at(&GetBoardResult::name)}).testId("boardName"),
             ui::text({.text = ui::format("{0} columns, {1} swimlanes, {2} tasks",
                                          {ui::count(columns), ui::count(lanes), ui::count(tasks)})})
                 .testId("summary"),
             ui::row({ui::form<CreateColumn>(board, {.onSuccess = refetched})
                          .id(ui::element("columnForm"))
                          .testId("columnForm"),
                      ui::form<CreateSwimlane>(board, {.onSuccess = refetched})
                          .id(ui::element("laneForm"))
                          .testId("laneForm")},
                     {.gap = 8}),
             ui::form<CreateTask>(board, {.prefill = firstPlace, .onSuccess = refetched})
                 .id(ui::element("taskForm"))
                 .testId("taskForm"),
             grid, ui::text({.text = ui::coalesce({open.error(), state.error(), move.error()})}).testId("error")},
            {.gap = 8});
    });
}

}  // namespace

void prepareLocal() {
    // A development secret: local mode signs nothing that leaves the process.
    auth::setTokenIssuer(
        std::make_shared<morph::session::TokenIssuer>("kanban-local-development", morph::session::hmacSha256));
}

std::expected<std::shared_ptr<morph::ui::ServedBundle const>, morph::ui::Refusal> bundle() {
    ui::ScreenRegistry screens;
    struct Listed {
        std::string id;
        std::string title;
        decltype(screens.add(projectsScreen())) registered;
    };
    std::vector<Listed> listed;
    listed.push_back({.id = std::string{kProjects}, .title = "Projects", .registered = screens.add(projectsScreen())});
    listed.push_back({.id = std::string{kBoard}, .title = "Board", .registered = screens.add(boardScreen())});
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

}  // namespace kanban::screens

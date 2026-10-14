// SPDX-License-Identifier: Apache-2.0

#include "kanban_screens.hpp"

#include <cstdint>
#include <expected>
#include <memory>
#include <morph/core/executor.hpp>
#include <morph/core/owner_strand.hpp>
#include <morph/journal/action_log.hpp>
#include <morph/session/session_auth.hpp>
#include <morph/ui/author.hpp>
#include <morph/ui/service.hpp>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "kanban/auth/kanban_authorizer.hpp"
#include "kanban/dto/activity_dto.hpp"
#include "kanban/dto/attachment_dto.hpp"
#include "kanban/dto/auth_dto.hpp"
#include "kanban/dto/board_dto.hpp"
#include "kanban/dto/project_dto.hpp"
#include "kanban/dto/rule_dto.hpp"
#include "kanban/models/board_model.hpp"
#include "kanban/models/project_admin_model.hpp"

namespace kanban::screens {

namespace ui = morph::ui::author;

namespace {

// Sign-in: the only screen served before it, so it holds no query. A successful login installs the principal it
// answered and opens the projects.
ui::Screen loginScreen() {
    return ui::screen(std::string{kLogin}, [](ui::Scope& screen) {
        screen.title("Sign in");
        auto const auth = screen.model<AuthModel>("auth", {.instance = ui::Instance::Private});
        auto const username = ui::element("username");
        auto const login = screen.mutation<Login>(
            "login", auth,
            {.body = {{&Login::username, username.value()}},
             .onSuccess = {ui::signIn(ui::MutationRef{"login"}), ui::navigate(std::string{kProjects})}});
        return ui::column(
            {ui::row({ui::textInput({.label = "Username", .placeholder = "Username"}).id(username).testId("username"),
                      ui::button({.label = "Sign in", .onClick = ui::run(login)}).testId("signIn")},
                     {.gap = 8}),
             ui::text({.text = login.error()}).testId("error")},
            {.gap = 8});
    });
}

// The person's projects: creating one opens its board, and each listed project opens its own.
ui::Screen projectsScreen() {
    return ui::screen(std::string{kProjects}, [](ui::Scope& screen) {
        screen.title("Projects");
        auto const admin = screen.model<ProjectAdminModel>("admin", {.instance = ui::Instance::Private});
        auto const projects = screen.query<GetMyProjects>("projects", admin, {});
        auto const openBoard = [](ui::AnyHandle const& projectId) {
            return ui::navigate(std::string{kBoard}, {.params = ui::obj({{"projectId", projectId}})});
        };
        auto const rows = projects.value().at(&GetMyProjectsResult::projects);
        return ui::column(
            {ui::form<CreateProject>(admin, {.onSuccess = {openBoard(ui::ref("event.id"))}})
                 .id(ui::element("createForm"))
                 .testId("createForm"),
             ui::table(rows,
                       {.key = ui::row("id"),
                        .columns = {{.name = "name", .label = "Project", .cell = ui::text({.text = ui::row("name")})},
                                    {.name = "open",
                                     .label = "",
                                     .cell = ui::button({.label = "Open", .onClick = openBoard(ui::row("id"))})}}})
                 .a11y("Projects")
                 .testId("projects"),
             ui::text({.text = ui::format("{0} projects", {ui::count(rows)})}).testId("count"),
             ui::text({.text = projects.error()}).testId("error")},
            {.gap = 8});
    });
}

// A task's detail, shown once a card's Details is picked: its comments and a form to add one, and its attachments,
// which a picked file is added to and each can be downloaded from.
// NOLINTBEGIN(bugprone-easily-swappable-parameters): the task, then the board's tasks and comments.
ui::Node taskDetail(ui::Scope& screen, ui::ModelHandle<BoardModel> const& board, ui::AnyHandle const& opened,
                    ui::AnyHandle const& selectedTask, ui::AnyHandle const& tasks, ui::AnyHandle const& comments,
                    ui::Commands const& refetched) {
    // NOLINTEND(bugprone-easily-swappable-parameters)
    auto const attachments =
        screen.query<GetAttachments>("attachments", board, {{&GetAttachments::taskId, selectedTask}},
                                     {.when = opened && ui::engaged(selectedTask)});
    auto const removeAttachment =
        screen.mutation<RemoveAttachment>("removeAttachment", board, {.invalidates = {attachments}});
    // A picked file is uploaded over the file side channel, then recorded on the task: the upload's reference is the
    // attachment's storage key, and its download.
    auto const picked = screen.state<std::string>("picked");
    auto const attach = screen.mutation<AddAttachment>(
        "attach", board,
        {.body = {{&AddAttachment::taskId, selectedTask},
                  {&AddAttachment::filename, picked},
                  {&AddAttachment::contentType, ui::AnyHandle{std::string{"application/octet-stream"}}}},
         .invalidates = {attachments}});
    auto const task =
        screen.let("selected", ui::call("first", {ui::filter(tasks, "t", [&](ui::Handle<ui::Any> const& candidate) {
                                            return ui::eq(candidate.at("id"), selectedTask);
                                        })}));
    auto const itsComments = ui::filter(
        comments, "c", [&](ui::Handle<ui::Any> const& comment) { return ui::eq(comment.at("taskId"), selectedTask); });
    return ui::panel(
               {ui::text({.text = task.at("title")}).testId("taskTitle"),
                ui::forEach(itsComments, {.key = ui::row("id"), .gap = 4},
                            [](ui::DeclScope&) {
                                return ui::Nodes{
                                    ui::text({.text = ui::format("{0}: {1}", {ui::row("principal"), ui::row("body")})})
                                        .testId("comment")};
                            }),
                ui::form<AddComment>(board, {.prefill = ui::obj({{"taskId", selectedTask}}), .onSuccess = refetched})
                    .id(ui::element("commentForm"))
                    .testId("commentForm"),
                ui::table(attachments.value().at(&GetAttachmentsResult::attachments),
                          {.key = ui::row("id"),
                           .columns = {{.name = "filename",
                                        .label = "Attachment",
                                        .cell = ui::text({.text = ui::row("filename")}).testId("attachment")},
                                       {.name = "download",
                                        .label = "",
                                        .cell = ui::button({.label = "Download",
                                                            .onClick = {ui::downloadToFile(ui::row("storageKey"),
                                                                                           ui::row("filename"))}})
                                                    .testId("download")},
                                       {.name = "remove",
                                        .label = "",
                                        .cell = ui::button({.label = "Remove",
                                                            .onClick = ui::run(
                                                                removeAttachment,
                                                                {{&RemoveAttachment::attachmentId, ui::row("id")}})})
                                                    .testId("removeAttachment")}}})
                    .a11y("Attachments")
                    .testId("attachments"),
                ui::filePicker(
                    {.label = "Attach a file",
                     .onPick = {ui::set(picked, ui::ref("event")),
                                ui::upload(ui::ref("event"), attach, {.as = "storageKey", .transfer = {}})}})
                    .testId("attachFile"),
                ui::text({.text = ui::coalesce({attachments.error(), attach.error(), removeAttachment.error()})})
                    .testId("detailError")},
               {.title = "Task", .padding = 4})
        .testId("taskDetail")
        .visible(ui::engaged(selectedTask));
}

// The project's members: their roles, a form to give someone a role, and removing one. Managers only may change them,
// which the model checks; the screen shows its refusal.
ui::Nodes membersTab(ui::Scope& screen, ui::AnyHandle const& projectId) {
    auto const admin = screen.model<ProjectAdminModel>("admin", {.instance = ui::Instance::Private});
    auto const roles = screen.query<GetProjectRoles>("roles", admin, {{&GetProjectRoles::projectId, projectId}},
                                                     {.when = ui::engaged(projectId)});
    auto const remove = screen.mutation<RemoveMember>("removeMember", admin, {.invalidates = {roles}});
    return {
        ui::table(
            roles.value().at(&GetProjectRolesResult::roles),
            {.key = ui::row("principal"),
             .columns = {{.name = "principal",
                          .label = "Member",
                          .cell = ui::text({.text = ui::row("principal")}).testId("member")},
                         {.name = "role", .label = "Role", .cell = ui::text({.text = ui::row("role")}).testId("role")},
                         {.name = "remove",
                          .label = "",
                          .cell = ui::button(
                                      {.label = "Remove",
                                       .onClick = ui::run(remove, {{&RemoveMember::projectId, projectId},
                                                                   {&RemoveMember::principal, ui::row("principal")}})})
                                      .testId("removeMember")}}})
            .a11y("Members")
            .testId("members"),
        ui::form<SetMemberRole>(admin,
                                {.prefill = ui::obj({{"projectId", projectId}}), .onSuccess = {ui::refetch(roles)}})
            .id(ui::element("memberForm"))
            .testId("memberForm"),
        ui::text({.text = ui::coalesce({roles.error(), remove.error()})}).testId("membersError")};
}

// The board's rules: "when a task is moved to this column, tag it". A new rule is made in a dialog: its form is built
// when the dialog opens, once the board is open, so the trigger column's options, the board's columns, which the form
// fetches itself as it is built, are there to pick from.
ui::Nodes rulesTab(ui::Scope& screen, ui::ModelHandle<BoardModel> const& board, ui::AnyHandle const& opened,
                   ui::AnyHandle const& projectId) {
    auto const rules = screen.query<GetRules>("rules", board, {{&GetRules::projectId, projectId}}, {.when = opened});
    auto const remove = screen.mutation<DeleteRule>("deleteRule", board, {.invalidates = {rules}});
    auto const newRule = ui::element("newRule");
    return {ui::button({.label = "New rule", .onClick = ui::open(newRule)}).testId("newRule").enabled(opened),
            ui::dialog(newRule, {.title = "New rule"},
                       [&](ui::DeclScope&) {
                           return ui::Nodes{
                               ui::form<CreateRule>(board, {.prefill = ui::obj({{"projectId", projectId}}),
                                                            .onSuccess = {ui::refetch(rules), ui::close(newRule)}})
                                   .id(ui::element("ruleForm"))
                                   .testId("ruleForm")};
                       }),
            ui::table(
                rules.value().at(&GetRulesResult::rules),
                {.key = ui::row("id"),
                 .columns = {{.name = "tag",
                              .label = "Tag",
                              .cell = ui::text({.text = ui::row("mutationValue")}).testId("ruleTag")},
                             {.name = "delete",
                              .label = "",
                              .cell = ui::button({.label = "Delete",
                                                  .onClick = ui::run(remove, {{&DeleteRule::ruleId, ui::row("id")}})})
                                          .testId("deleteRule")}}})
                .a11y("Rules")
                .testId("rules"),
            ui::text({.text = ui::coalesce({rules.error(), remove.error()})}).testId("rulesError")};
}

// What happened on the board, from its journal, oldest first.
template <typename Query>
ui::Nodes activityTab(Query const& activity) {
    return {ui::forEach(activity.value().at(&GetActivityResult::events), {.key = ui::row("sequence"), .gap = 2},
                        [](ui::DeclScope&) {
                            return ui::Nodes{
                                ui::text({.text = ui::format("{0}: {1}", {ui::row("principal"), ui::row("summary")})})
                                    .testId("activity")};
                        }),
            ui::text({.text = activity.error()}).testId("activityError")};
}

// The board, in tabs: the board itself (swimlanes by columns of cards, and a picked task's detail), its members, its
// rules and its activity. A card is draggable with its task id; a cell is a drop target that moves the task to the end
// of that cell, and a card is one that moves it before the card. Moves are `serial`, one queue for the board: a
// `serialKey` cannot read the run's task, so two drops of one card apply in order by sharing it.
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
        auto const selectedTask = screen.state<std::int64_t>("selectedTask");
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
                ui::forEach(
                    columns, {.key = ui::row("id"), .axis = std::string{"horizontal"}, .gap = 8},
                    [&](ui::DeclScope& columnScope) {
                        auto const column = columnScope.let("column", ui::row());
                        auto const cards =
                            columnScope.let("cards", ui::filter(tasks, "t", [&](ui::Handle<ui::Any> const& task) {
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
                            return ui::Nodes{ui::row(
                                {ui::text({.text = ui::row("title")})
                                     .testId("card")
                                     .drag(ui::row("id"))
                                     .drop({}, {moveTo(ui::row("position"))}),
                                 ui::button({.label = "Details", .onClick = {ui::set(selectedTask, ui::row("id"))}})
                                     .testId("details")},
                                {.gap = 4})};
                        });
                        return ui::Nodes{ui::panel({card}, {.title = column.at("name"), .padding = 4})
                                             .testId("cell")
                                             .drop({}, {moveTo(ui::count(cards))})};
                    })};
        });
        // The board's journal follows every action the board takes: a drag's mutation through `refreshOn`, and a
        // form's submit, which is not a mutation and so not seen by `refreshOn`, through the form's own refetch.
        auto const activity = screen.query<GetActivity>(
            "activity", board, {}, {.when = opened, .refreshOn = board.actions<MoveTaskPosition>()});
        auto const refetched = ui::Commands{ui::refetch(state), ui::refetch(activity)};
        auto const firstColumn = screen.let("firstColumn", ui::call("first", {columns}));
        auto const firstLane = screen.let("firstLane", ui::call("first", {lanes}));
        auto const firstPlace = ui::obj({{"columnId", firstColumn.at("id")}, {"swimlaneId", firstLane.at("id")}});
        auto const detail =
            taskDetail(screen, board, opened, selectedTask, tasks, view.at(&GetBoardResult::comments), refetched);
        return ui::column(
            {ui::text({.text = view.at(&GetBoardResult::name)}).testId("boardName"),
             ui::tabs({{.label = "Board",
                        .children = {ui::text({.text = ui::format(
                                                   "{0} columns, {1} swimlanes, {2} tasks",
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
                                     grid, detail}},
                       {.label = "Members", .children = membersTab(screen, projectId)},
                       {.label = "Rules", .children = rulesTab(screen, board, opened, projectId)},
                       {.label = "Activity", .children = activityTab(activity)}})
                 .testId("sections"),
             ui::text({.text = ui::coalesce({open.error(), state.error(), move.error()})}).testId("error")},
            {.gap = 8});
    });
}

}  // namespace

void prepareLocal() {
    // A development secret: local mode signs nothing that leaves the process.
    auth::setTokenIssuer(
        std::make_shared<morph::session::TokenIssuer>("kanban-local-development", morph::session::hmacSha256));
    // The board's activity is its journal. In process it is this run's, in memory, on a thread of its own that lives
    // as long as the process does; every board made for a project journals under that project's id.
    static morph::exec::ThreadPoolExecutor journalThread{1};
    static morph::exec::OwnerStrand journalOwner{journalThread};
    static auto const journal = std::make_shared<morph::journal::InMemoryActionLog>(journalOwner);
    morph::journal::setActionLog(journal);
}

std::expected<std::shared_ptr<morph::ui::ServedBundle const>, morph::ui::Refusal> bundle() {
    ui::ScreenRegistry screens;
    struct Listed {
        std::string id;
        std::string title;
        decltype(screens.add(projectsScreen())) registered;
    };
    std::vector<Listed> listed;
    listed.push_back({.id = std::string{kLogin}, .title = "Sign in", .registered = screens.add(loginScreen())});
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
        entry.isPublic = screen.id == kLogin;
        if (auto added = served.addScreen(std::move(entry), (*screen.registered)->text); !added) {
            return std::unexpected(std::move(added.error()));
        }
    }
    return served.build();
}

}  // namespace kanban::screens

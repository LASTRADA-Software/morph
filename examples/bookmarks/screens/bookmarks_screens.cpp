// SPDX-License-Identifier: Apache-2.0

#include "bookmarks_screens.hpp"

#include <expected>
#include <memory>
#include <morph/session/session_auth.hpp>
#include <morph/ui/author.hpp>
#include <morph/ui/service.hpp>
#include <string>
#include <utility>
#include <vector>

#include "bookmarks/auth/bookmarks_authorizer.hpp"
#include "bookmarks/dto/auth_dto.hpp"
#include "bookmarks/dto/bookmark_dto.hpp"
#include "bookmarks/dto/bulk_dto.hpp"
#include "bookmarks/dto/import_export_dto.hpp"
#include "bookmarks/dto/tag_dto.hpp"
#include "bookmarks/models/auth_model.hpp"
#include "bookmarks/models/bookmark_model.hpp"
#include "bookmarks/models/tag_model.hpp"

namespace bookmarks::screens {

namespace ui = morph::ui::author;

namespace {

// Sign-in: the only screen served before it, so it holds no query. A successful login installs the principal it
// answered and opens the bookmarks.
ui::Screen loginScreen() {
    return ui::screen(std::string{kLogin}, [](ui::Scope& screen) {
        screen.title("Sign in");
        auto const auth = screen.model<AuthModel>("auth", {.instance = ui::Instance::Private});
        auto const username = ui::element("username");
        auto const login = screen.mutation<Login>(
            "login", auth,
            {.body = {{&Login::username, username.value()}},
             .onSuccess = {ui::signIn(ui::MutationRef{"login"}), ui::navigate(std::string{kBookmarks})}});
        return ui::column(
            {ui::row({ui::textInput({.label = "Username", .placeholder = "Username"}).id(username).testId("username"),
                      ui::button({.label = "Sign in", .onClick = ui::run(login)}).testId("signIn")},
                     {.gap = 8}),
             ui::text({.text = login.error()}).testId("error")},
            {.gap = 8});
    });
}

// One screen over two models, once signed in. The list is a table with multiple selection, and archiving the selection
// is one bulk mutation.
ui::Screen bookmarksScreen() {
    return ui::screen(std::string{kBookmarks}, [](ui::Scope& screen) {
        screen.title("Bookmarks");
        auto const marks = screen.model<BookmarkModel>("marks", {.instance = ui::Instance::Private});
        auto const tagModel = screen.model<TagModel>("tagModel", {.instance = ui::Instance::Private});
        auto const list = ui::element("list");
        auto const bookmarks = screen.query<ListBookmarks>("bookmarks", marks, {});
        auto const tags = screen.query<ListTags>("tags", tagModel, {});
        auto const archive = screen.mutation<BulkEdit>("archive", marks, {.invalidates = {bookmarks, tags}});
        auto const refreshed = ui::Commands{ui::refetch(bookmarks), ui::refetch(tags)};
        auto const rows = bookmarks.value().at(&ListBookmarksResult::bookmarks);
        return ui::column(
            {ui::text({.text = ui::format("Signed in as {0}", {ui::session("principal")})}).testId("whoami"),
             ui::form<CreateBookmark>(marks, {.onSuccess = refreshed})
                 .id(ui::element("createForm"))
                 .testId("createForm"),
             ui::table(rows,
                       {.key = ui::row("id"),
                        .columns = {{.name = "title", .label = "Title", .cell = ui::text({.text = ui::row("title")})},
                                    {.name = "url", .label = "URL", .cell = ui::text({.text = ui::row("url")})}},
                        .selection = ui::Selection::Multiple})
                 .id(list)
                 .a11y("Bookmarks")
                 .testId("bookmarks"),
             ui::text({.text = ui::format("{0} bookmarks", {ui::count(rows)})}).testId("count"),
             ui::button({.label = "Archive selected",
                         .onClick = ui::run(archive, {{&BulkEdit::ids, list.selection()},
                                                      {&BulkEdit::archive, ui::AnyHandle{std::string{"Archive"}}}})})
                 .testId("archiveSelected"),
             ui::form<EditBookmark>(marks, {.onSuccess = refreshed}).id(ui::element("editForm")).testId("editForm"),
             ui::form<ImportBookmarks>(marks, {.onSuccess = refreshed})
                 .id(ui::element("importForm"))
                 .testId("importForm"),
             ui::table(tags.value().at(&ListTagsResult::tags),
                       {.key = ui::row("id"),
                        .columns = {{.name = "name", .label = "Tag", .cell = ui::text({.text = ui::row("name")})},
                                    {.name = "count",
                                     .label = "Bookmarks",
                                     .cell = ui::text({.text = ui::row("bookmarkCount")})}}})
                 .a11y("Tags")
                 .testId("tags"),
             ui::form<RenameTag>(tagModel, {.onSuccess = refreshed})
                 .id(ui::element("renameForm"))
                 .testId("renameForm"),
             ui::form<MergeTags>(tagModel, {.onSuccess = refreshed}).id(ui::element("mergeForm")).testId("mergeForm"),
             ui::text({.text = ui::coalesce({bookmarks.error(), tags.error(), archive.error()})}).testId("error")},
            {.gap = 8});
    });
}

}  // namespace

void prepareLocal() {
    // A development secret: local mode signs nothing that leaves the process.
    auth::setTokenIssuer(
        std::make_shared<morph::session::TokenIssuer>("bookmarks-local-development", morph::session::hmacSha256));
}

std::expected<std::shared_ptr<morph::ui::ServedBundle const>, morph::ui::Refusal> bundle() {
    ui::ScreenRegistry screens;
    struct Listed {
        std::string id;
        std::string title;
        decltype(screens.add(loginScreen())) registered;
    };
    std::vector<Listed> listed;
    listed.push_back({.id = std::string{kLogin}, .title = "Sign in", .registered = screens.add(loginScreen())});
    listed.push_back(
        {.id = std::string{kBookmarks}, .title = "Bookmarks", .registered = screens.add(bookmarksScreen())});
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

}  // namespace bookmarks::screens

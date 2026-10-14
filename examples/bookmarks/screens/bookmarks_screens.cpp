// SPDX-License-Identifier: Apache-2.0

#include "bookmarks_screens.hpp"

#include <expected>
#include <memory>
#include <morph/session/session_auth.hpp>
#include <morph/ui/author.hpp>
#include <morph/ui/service.hpp>
#include <string>
#include <utility>

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

// One screen over three models. Signing in installs the principal every other call needs, so the queries wait for it.
// The list is a table with multiple selection, and archiving the selection is one bulk mutation.
ui::Screen bookmarksScreen() {
    return ui::screen(std::string{kBookmarks}, [](ui::Scope& screen) {
        screen.title("Bookmarks");
        auto const auth = screen.model<AuthModel>("auth", {.instance = ui::Instance::Private});
        auto const marks = screen.model<BookmarkModel>("marks", {.instance = ui::Instance::Private});
        auto const tagModel = screen.model<TagModel>("tagModel", {.instance = ui::Instance::Private});
        auto const signedIn = screen.state("signedIn", false);
        auto const username = ui::element("username");
        auto const list = ui::element("list");
        auto const bookmarks = screen.query<ListBookmarks>("bookmarks", marks, {}, {.when = signedIn});
        auto const tags = screen.query<ListTags>("tags", tagModel, {}, {.when = signedIn});
        // The reply's principal is installed first, then the gate starts the queries that need it.
        auto const login = ui::MutationRef{"login"};
        auto const loginMutation = screen.mutation<Login>("login", auth,
                                                          {.body = {{&Login::username, username.value()}},
                                                           .onSuccess = {ui::signIn(login), ui::set(signedIn, true)}});
        auto const archive = screen.mutation<BulkEdit>("archive", marks, {.invalidates = {bookmarks, tags}});
        auto const refreshed = ui::Commands{ui::refetch(bookmarks), ui::refetch(tags)};
        auto const rows = bookmarks.value().at(&ListBookmarksResult::bookmarks);
        return ui::column(
            {ui::row({ui::textInput({.label = "Username", .placeholder = "Username"}).id(username).testId("username"),
                      ui::button({.label = "Sign in", .onClick = ui::run(loginMutation)}).testId("signIn")},
                     {.gap = 8})
                 .visible(!signedIn),
             ui::text({.text = ui::format("Signed in as {0}", {username.value()})}).testId("whoami").visible(signedIn),
             ui::form<CreateBookmark>(marks, {.onSuccess = refreshed})
                 .id(ui::element("createForm"))
                 .testId("createForm")
                 .visible(signedIn),
             ui::table(rows,
                       {.key = ui::row("id"),
                        .columns = {{.name = "title", .label = "Title", .cell = ui::text({.text = ui::row("title")})},
                                    {.name = "url", .label = "URL", .cell = ui::text({.text = ui::row("url")})}},
                        .selection = ui::Selection::Multiple})
                 .id(list)
                 .a11y("Bookmarks")
                 .testId("bookmarks")
                 .visible(signedIn),
             ui::text({.text = ui::format("{0} bookmarks", {ui::count(rows)})}).testId("count").visible(signedIn),
             ui::button({.label = "Archive selected",
                         .onClick = ui::run(archive, {{&BulkEdit::ids, list.selection()},
                                                      {&BulkEdit::archive, ui::AnyHandle{std::string{"Archive"}}}})})
                 .testId("archiveSelected")
                 .visible(signedIn),
             ui::form<EditBookmark>(marks, {.onSuccess = refreshed})
                 .id(ui::element("editForm"))
                 .testId("editForm")
                 .visible(signedIn),
             ui::form<ImportBookmarks>(marks, {.onSuccess = refreshed})
                 .id(ui::element("importForm"))
                 .testId("importForm")
                 .visible(signedIn),
             ui::table(tags.value().at(&ListTagsResult::tags),
                       {.key = ui::row("id"),
                        .columns = {{.name = "name", .label = "Tag", .cell = ui::text({.text = ui::row("name")})},
                                    {.name = "count",
                                     .label = "Bookmarks",
                                     .cell = ui::text({.text = ui::row("bookmarkCount")})}}})
                 .a11y("Tags")
                 .testId("tags")
                 .visible(signedIn),
             ui::form<RenameTag>(tagModel, {.onSuccess = refreshed})
                 .id(ui::element("renameForm"))
                 .testId("renameForm")
                 .visible(signedIn),
             ui::form<MergeTags>(tagModel, {.onSuccess = refreshed})
                 .id(ui::element("mergeForm"))
                 .testId("mergeForm")
                 .visible(signedIn),
             ui::text(
                 {.text = ui::coalesce({loginMutation.error(), bookmarks.error(), tags.error(), archive.error()})})
                 .testId("error")},
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
    auto registered = screens.add(bookmarksScreen());
    if (!registered) {
        return std::unexpected(std::move(registered.error()));
    }
    morph::ui::UiRegistry served{morph::ui::ApplicationInfo{.id = std::string{kApplicationId}, .version = "1.0"},
                                 screens.catalog()};
    morph::ui::CatalogEntry entry;
    entry.id = std::string{kBookmarks};
    entry.title = "Bookmarks";
    if (auto added = served.addScreen(std::move(entry), (*registered)->text); !added) {
        return std::unexpected(std::move(added.error()));
    }
    return served.build();
}

}  // namespace bookmarks::screens

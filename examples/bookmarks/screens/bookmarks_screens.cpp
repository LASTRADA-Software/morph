// SPDX-License-Identifier: Apache-2.0

#include "bookmarks_screens.hpp"

#include <cstdint>
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
#include "bookmarks/dto/shared_feed_dto.hpp"
#include "bookmarks/dto/tag_dto.hpp"
#include "bookmarks/models/auth_model.hpp"
#include "bookmarks/models/bookmark_model.hpp"
#include "bookmarks/models/shared_feed_model.hpp"
#include "bookmarks/models/tag_model.hpp"

namespace bookmarks::screens {

namespace ui = morph::ui::author;

namespace {

// The width of a centred card, in units.
constexpr int kCardWidth = 44;

ui::AnyHandle stretch() { return ui::obj({{"stretch", ui::AnyHandle{std::int64_t{1}}}}); }

ui::AnyHandle fixed(std::int64_t units) { return ui::obj({{"fixed", ui::AnyHandle{units}}}); }

// A node centred in its row: a stretch spacer either side of it.
ui::Node centred(ui::Node const& node) {
    return ui::row({ui::spacer().sizing(stretch()), node, ui::spacer().sizing(stretch())});
}

// A bookmark's tags, one badge each.
ui::Node tagBadges(ui::AnyHandle const& tags) {
    return ui::forEach(tags, {.key = ui::row(""), .axis = std::string{"horizontal"}, .gap = std::int64_t{1}},
                       [](ui::RowScope& /*tag*/) { return ui::badge(ui::row(""), "info"); });
}

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
        return centred(
            ui::panel(
                {ui::column(
                    {ui::text({.text = "Morph Bookmarks", .role = std::string{"heading"}}).testId("brand"),
                     ui::text({.text = "Sign in to keep, tag and share your links.", .role = std::string{"muted"}}),
                     ui::textInput({.label = "Username", .placeholder = "Username"}).id(username).testId("username"),
                     ui::button({.label = "Sign in",
                                 .emphasis = morph::ui::ButtonEmphasis::Primary,
                                 .onClick = ui::run(login)})
                         .testId("signIn"),
                     ui::text({.text = login.error(), .role = std::string{"error"}}).testId("error")},
                    {.gap = 3})},
                {.padding = 5})
                .sizing(fixed(kCardWidth)));
    });
}

// The bookmarks: a toolbar that filters the list, the list in a card with selection, the bulk actions under it, and
// two drawers, one to create a bookmark and one to edit the one activated in the list.
ui::Screen bookmarksScreen() {
    return ui::screen(std::string{kBookmarks}, [](ui::Scope& screen) {
        screen.title("Bookmarks");
        auto const marks = screen.model<BookmarkModel>("marks", {.instance = ui::Instance::Private});
        auto const tagModel = screen.model<TagModel>("tagModel", {.instance = ui::Instance::Private});
        auto const search = ui::element("search");
        auto const tagFilter = ui::element("tagFilter");
        auto const tag = screen.state("tag", std::string{});
        auto const showArchived = ui::element("showArchived");
        auto const list = ui::element("list");
        auto const create = ui::element("create");
        auto const edit = ui::element("edit");
        auto const editing = screen.state<ui::Any>("editing");
        auto const tags = screen.query<ListTags>("tags", tagModel, {});
        auto const bookmarks = screen.query<ListBookmarks>(
            "bookmarks", marks,
            {{&ListBookmarks::searchText, ui::coalesce({search.value(), std::string{}})},
             {&ListBookmarks::tag, tag},
             {&ListBookmarks::archiveFilter,
              ui::AnyHandle::raw(ui::cond(showArchived.checked(), ui::Handle<std::string>{std::string{"Any"}},
                                          ui::Handle<std::string>{std::string{"ActiveOnly"}})
                                     .json())}},
            {});
        auto const detail =
            screen.query<GetBookmark>("detail", marks, {{&GetBookmark::id, editing}}, {.when = ui::engaged(editing)});
        auto const archive = screen.mutation<BulkEdit>("archive", marks, {.invalidates = {bookmarks, tags}});
        auto const remove = screen.mutation<DeleteBookmark>("remove", marks,
                                                            {.body = {{&DeleteBookmark::id, editing}},
                                                             .invalidates = {bookmarks, tags},
                                                             .onSuccess = {ui::close(edit)}});
        auto const refreshed = ui::Commands{ui::refetch(bookmarks), ui::refetch(tags)};
        auto const rows = bookmarks.value().at(&ListBookmarksResult::bookmarks);
        auto const knownTags =
            ui::map(tags.value().at(&ListTagsResult::tags), "t", [](ui::Handle<ui::Any> const& tag) {
                return ui::obj({{"key", tag.at("name")}, {"label", tag.at("name")}});
            });
        auto const bulk = [&](std::string const& op) {
            return ui::run(archive, {{&BulkEdit::ids, list.selection()}, {&BulkEdit::archive, ui::AnyHandle{op}}});
        };
        return ui::column(
            {ui::row(
                 {ui::textInput({.label = "Search bookmarks", .placeholder = "Search titles, URLs and notes"})
                      .id(search)
                      .testId("search")
                      .sizing(stretch()),
                  ui::select(
                      {.options = knownTags, .selected = tag, .label = "Tag", .onChoose = {ui::set(tag, ui::event())}})
                      .id(tagFilter)
                      .testId("tagFilter")
                      .sizing(fixed(20)),
                  ui::button({.label = "All tags", .onClick = {ui::set(tag, std::string{})}})
                      .testId("allTags")
                      .visible(ui::ne(tag, std::string{})),
                  ui::checkbox({.label = "Show archived"}).id(showArchived).testId("showArchived"),
                  ui::button({.label = "New bookmark",
                              .emphasis = morph::ui::ButtonEmphasis::Primary,
                              .onClick = {ui::open(create)}})
                      .testId("newBookmark")},
                 {.gap = 3}),
             ui::panel(
                 {ui::table(rows,
                            {.key = ui::row("id"),
                             .columns = {{.name = "title",
                                          .label = "Title",
                                          .cell = {ui::column(
                                              {ui::text({.text = ui::coalesce({ui::row("title"), ui::row("url")})}),
                                               ui::text({.text = ui::row("url"), .role = std::string{"muted"}})})}},
                                         {.name = "tags", .label = "Tags", .cell = {tagBadges(ui::row("tags"))}},
                                         {.name = "visibility",
                                          .label = "Visibility",
                                          .cell = {ui::badge(
                                              ui::row("visibility"),
                                              ui::cond(ui::eq(ui::row("visibility"), "Shared"),
                                                       ui::Handle<std::string>{std::string{"ok"}},
                                                       ui::Handle<std::string>{std::string{"neutral"}}))}}},
                             .selection = ui::Selection::Multiple,
                             .onActivate = {ui::set(editing, ui::row("id")), ui::open(edit)}})
                      .id(list)
                      .a11y("Bookmarks")
                      .testId("bookmarks")},
                 {.padding = 0}),
             ui::row(
                 {ui::text({.text = ui::format("{0} bookmarks", {ui::count(rows)}), .role = std::string{"muted"}})
                      .testId("count")
                      .sizing(stretch()),
                  ui::button({.label = "Archive selected", .onClick = {bulk("Archive")}}).testId("archiveSelected"),
                  ui::button({.label = "Unarchive selected", .onClick = {bulk("Unarchive")}})
                      .testId("unarchiveSelected")},
                 {.gap = 3}),
             ui::text({.text = ui::coalesce({bookmarks.error(), tags.error(), archive.error(), remove.error()}),
                       .role = std::string{"error"}})
                 .testId("error"),
             ui::drawer(create,
                        {ui::form<CreateBookmark>(
                             marks, {.onSuccess = {ui::close(create), ui::refetch(bookmarks), ui::refetch(tags)}})
                             .id(ui::element("createForm"))
                             .testId("createForm")},
                        {.title = "New bookmark", .side = std::string{"end"}})
                 .a11y("New bookmark"),
             ui::drawer(
                 edit,
                 {ui::form<EditBookmark>(marks,
                                         {.prefill = detail.value(),
                                          .key = editing,
                                          .onSuccess = {ui::close(edit), ui::refetch(bookmarks), ui::refetch(tags)}})
                      .id(ui::element("editForm"))
                      .testId("editForm"),
                  ui::text({.text = "Every field is saved as shown: a field left empty is cleared.",
                            .role = std::string{"muted"}}),
                  ui::button(
                      {.label = "Delete", .emphasis = morph::ui::ButtonEmphasis::Danger, .onClick = {ui::run(remove)}})
                      .testId("deleteBookmark")},
                 {.title = "Edit bookmark", .side = std::string{"end"}})
                 .a11y("Edit bookmark")},
            {.gap = 4});
    });
}

// The tags: the list with their counts beside two cards, one renaming the selected tag and one merging it into
// another. Both cascade to every bookmark carrying the tag.
ui::Screen tagsScreen() {
    return ui::screen(std::string{kTags}, [](ui::Scope& screen) {
        screen.title("Tags");
        auto const tagModel = screen.model<TagModel>("tagModel", {.instance = ui::Instance::Private});
        auto const tags = screen.query<ListTags>("tags", tagModel, {});
        // A single-selection table's selection is a list of at most one key.
        auto const picked = ui::element("tagList");
        auto const refreshed = ui::Commands{ui::refetch(tags)};
        return ui::row(
            {ui::panel(
                 {ui::table(
                      tags.value().at(&ListTagsResult::tags),
                      {.key = ui::row("id"),
                       .columns = {{.name = "name", .label = "Tag", .cell = {ui::badge(ui::row("name"), "info")}},
                                   {.name = "count",
                                    .label = "Bookmarks",
                                    .cell = {ui::text({.text = ui::row("bookmarkCount")})}}},
                       .selection = ui::Selection::Single})
                      .id(picked)
                      .a11y("Tags")
                      .testId("tags")},
                 {.padding = 0})
                 .sizing(stretch()),
             ui::column(
                 {ui::panel({ui::text({.text = "Every bookmark with the tag gets the new name.",
                                       .role = std::string{"muted"}}),
                             ui::form<RenameTag>(
                                 tagModel, {.prefill = ui::obj({{"id", ui::call("first", {picked.selection()})}}),
                                            .fields = {{&RenameTag::id, {.hidden = true}}},
                                            .onSuccess = refreshed})
                                 .id(ui::element("renameForm"))
                                 .testId("renameForm")},
                            {.title = "Rename the selected tag", .padding = 5}),
                  ui::panel(
                      {ui::text({.text = "The selected tag's bookmarks move to the other, and the "
                                         "selected tag is gone.",
                                 .role = std::string{"muted"}}),
                       ui::form<MergeTags>(
                           tagModel, {.prefill = ui::obj({{"sourceId", ui::call("first", {picked.selection()})}}),
                                      .fields = {{&MergeTags::sourceId, {.hidden = true}}},
                                      .onSuccess = refreshed})
                           .id(ui::element("mergeForm"))
                           .testId("mergeForm")},
                      {.title = "Merge the selected tag", .padding = 5})},
                 {.gap = 5})
                 .sizing(fixed(kCardWidth))},
            {.gap = 5});
    });
}

// The shared feed: what everyone has shared, newest first, one card each.
ui::Screen sharedScreen() {
    return ui::screen(std::string{kShared}, [](ui::Scope& screen) {
        screen.title("Shared feed");
        auto const sharedFeed = screen.model<SharedFeedModel>("sharedFeed", {.instance = ui::Instance::Private});
        auto const feed = screen.query<ListSharedFeed>("feed", sharedFeed, {});
        return ui::column({ui::text({.text = "What everyone has shared, newest first.", .role = std::string{"muted"}}),
                           ui::forEach(feed.value().at(&ListSharedFeedResult::bookmarks),
                                       {.key = ui::row("id"), .gap = std::int64_t{4}},
                                       [](ui::RowScope& /*bookmark*/) {
                                           return ui::panel(
                                               {ui::text({.text = ui::coalesce({ui::row("title"), ui::row("url")}),
                                                          .role = std::string{"heading"}}),
                                                ui::text({.text = ui::row("url"), .role = std::string{"muted"}}),
                                                tagBadges(ui::row("tags"))},
                                               {.padding = 4});
                                       })
                               .testId("feed"),
                           ui::text({.text = feed.error(), .role = std::string{"error"}}).testId("error")},
                          {.gap = 4});
    });
}

// Import and export: an import card that reads a Netscape bookmarks file and says what it added, beside an export
// card that shows the file of every bookmark the user owns.
ui::Screen importScreen() {
    return ui::screen(std::string{kImport}, [](ui::Scope& screen) {
        screen.title("Import and export");
        auto const marks = screen.model<BookmarkModel>("marks", {.instance = ui::Instance::Private});
        auto const imported = screen.state<ui::Any>("imported");
        auto const file = screen.state<ui::Any>("file");
        auto const exported =
            screen.mutation<ExportBookmarks>("export", marks, {.onSuccess = {ui::set(file, ui::event())}});
        return ui::row(
            {ui::panel({ui::text({.text = "Paste a Netscape bookmarks file, the HTML every browser exports. A link "
                                          "you already have is skipped, so importing twice is safe.",
                                  .role = std::string{"muted"}}),
                        ui::form<ImportBookmarks>(marks, {.onSuccess = {ui::set(imported, ui::event())}})
                            .id(ui::element("importForm"))
                            .testId("importForm"),
                        ui::banner({.tone = "ok",
                                    .text = ui::format("{0} added · {1} already in your bookmarks",
                                                       {imported.at("imported"), imported.at("skipped")})})
                            .visible(ui::engaged(imported))
                            .testId("importResult")},
                       {.title = "Import a bookmarks file", .padding = 5})
                 .sizing(stretch()),
             ui::panel(
                 {ui::text({.text = "Every bookmark you own, archived ones included, as a Netscape bookmarks "
                                    "file any browser can import.",
                            .role = std::string{"muted"}}),
                  ui::button({.label = "Export", .onClick = {ui::run(exported)}}).testId("export"),
                  ui::textInput({.value = file.at("html"), .label = "Exported file", .mode = std::string{"multiline"}})
                      .visible(ui::engaged(file))
                      .testId("exported")},
                 {.title = "Export", .padding = 5})
                 .sizing(fixed(kCardWidth))},
            {.gap = 5});
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
    listed.push_back({.id = std::string{kShared}, .title = "Shared feed", .registered = screens.add(sharedScreen())});
    listed.push_back({.id = std::string{kTags}, .title = "Tags", .registered = screens.add(tagsScreen())});
    listed.push_back(
        {.id = std::string{kImport}, .title = "Import and export", .registered = screens.add(importScreen())});
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
        // The navigator's groups: the library the user keeps, and the tools over it.
        entry.module = "Bookmarks";
        entry.group = screen.id == kImport ? "Tools" : "Library";
        if (auto added = served.addScreen(std::move(entry), (*screen.registered)->text); !added) {
            return std::unexpected(std::move(added.error()));
        }
    }
    return served.build();
}

}  // namespace bookmarks::screens

// SPDX-License-Identifier: Apache-2.0
//
// BookmarkPresenter's own suite (Task 17): each of its ten actions
// (create/edit/archive/unarchive/remove/get/list/getChangesSince/bulkEdit/
// importChunk/exportAll) round-trips through the presenter's own signals —
// not the model directly — across the full BackendRig mode matrix
// (Local/LocalSingleThread/Socket, examples/TESTING.md "The dual-mode
// fixture"), plus a `failed()` case per action. Domain rules (ownership,
// tag diffing, archive-state filtering, bulk-atomicity, ...) already have a
// dedicated suite at the model level (test_bookmark_model.cpp); this file
// only proves the presenter wires each action to the right signal, sets
// `busy()`/`idle()` correctly, and neither crashes nor hangs — the
// "translates and routes only" contract bookmark_presenter.hpp's own doc
// comment states (examples/IMPLEMENTATION.md rule 2).
//
// Every mode needs a real signed token: `BookmarksAuthorizer::authorize()`
// requires one on every single execute, unconditionally (see
// bookmarks_authorizer.hpp's own doc comment), so even Local/LocalSingleThread
// mode (which runs no real authorizer) still needs `session::current()->
// principal` populated for a model's own scoping to succeed —
// `Bridge::setDefaultSession` supplies the per-call Context every mode
// dispatches through, exactly the recipe test_bookmark_model.cpp's own
// backend-mode-matrix case uses.

#include <algorithm>
#include <bookmarks/auth/bookmarks_authorizer.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <memory>
#include <morph/session/session_auth.hpp>
#include <string>
#include <string_view>
#include <vector>

#include "bookmark_presenter.hpp"
#include "testkit/backend_rig.hpp"
#include "testkit/db_fixture.hpp"
#include "testkit/pump.hpp"

namespace {

using morph::ladder::testkit::BackendRig;
using morph::ladder::testkit::DbFixture;
using morph::ladder::testkit::Mode;
using morph::ladder::testkit::pumpUntil;

/// @brief Builds a rig authenticated as @p principal, for @p mode, over a
///        fresh authorizer keyed on @p secret. See this file's own top
///        comment for why every mode needs this, not just Socket.
[[nodiscard]] std::unique_ptr<BackendRig> makeAuthedRig(Mode mode, std::string_view secret, std::string principal,
                                                        std::size_t nClients = 1) {
    const auto authorizer =
        std::make_shared<bookmarks::auth::BookmarksAuthorizer>(std::string{secret}, morph::session::hmacSha256);
    auto rig = std::make_unique<BackendRig>(mode, nClients, authorizer);
    const morph::session::TokenIssuer issuer{std::string{secret}, morph::session::hmacSha256};
    morph::session::Context ctx;
    ctx.principal = std::move(principal);
    ctx.token = issuer.issue(
        morph::session::SessionToken{.principal = ctx.principal, .expiresAtMs = 4102444800000, .roles = {}});
    for (std::size_t i = 0; i < nClients; ++i) {
        rig->bridge(i).setDefaultSession(ctx);
    }
    return rig;
}

[[nodiscard]] bookmarks::CreateBookmark makeCreate(std::string url, std::string title = {}) {
    bookmarks::CreateBookmark create;
    create.url = std::move(url);
    create.title = std::move(title);
    return create;
}

}  // namespace

TEST_CASE("BookmarkPresenter::create then get round-trips a bookmark, all three backend modes",
          "[bookmarks][presenter]") {
    const auto mode = GENERATE(Mode::Local, Mode::LocalSingleThread, Mode::Socket);
    CAPTURE(mode);
    DbFixture fixture;
    auto rig = makeAuthedRig(mode, "presenter-create-get-secret", "alice");
    bookmarks::gui::BookmarkPresenter presenter{rig->bridge(0), rig->executor()};

    bookmarks::BookmarkId createdId;
    bool created = false;
    QObject::connect(&presenter, &bookmarks::gui::BookmarkPresenter::created,
                     [&](bookmarks::CreateBookmarkResult result) {
                         createdId = result.id;
                         created = true;
                     });
    presenter.create(makeCreate("https://one.example", "One"));
    REQUIRE(pumpUntil([&] { return created; }));
    REQUIRE_FALSE(presenter.busy());
    REQUIRE(createdId.hasValue());

    bookmarks::BookmarkView loaded;
    bool gotLoaded = false;
    QObject::connect(&presenter, &bookmarks::gui::BookmarkPresenter::loaded, [&](bookmarks::BookmarkView view) {
        loaded = view;
        gotLoaded = true;
    });
    presenter.get(bookmarks::GetBookmark{.id = createdId});
    REQUIRE(pumpUntil([&] { return gotLoaded; }));
    REQUIRE_FALSE(presenter.busy());
    CHECK(loaded.id == createdId);
    CHECK(loaded.url == "https://one.example");
    CHECK(loaded.title == "One");
}

TEST_CASE("BookmarkPresenter::edit replaces a bookmark's fields, all three backend modes", "[bookmarks][presenter]") {
    const auto mode = GENERATE(Mode::Local, Mode::LocalSingleThread, Mode::Socket);
    CAPTURE(mode);
    DbFixture fixture;
    auto rig = makeAuthedRig(mode, "presenter-edit-secret", "alice");
    bookmarks::gui::BookmarkPresenter presenter{rig->bridge(0), rig->executor()};

    bookmarks::BookmarkId createdId;
    bool created = false;
    QObject::connect(&presenter, &bookmarks::gui::BookmarkPresenter::created,
                     [&](bookmarks::CreateBookmarkResult result) {
                         createdId = result.id;
                         created = true;
                     });
    presenter.create(makeCreate("https://before.example", "Before"));
    REQUIRE(pumpUntil([&] { return created; }));

    bookmarks::BookmarkView edited;
    bool gotEdited = false;
    QObject::connect(&presenter, &bookmarks::gui::BookmarkPresenter::edited, [&](bookmarks::BookmarkView view) {
        edited = view;
        gotEdited = true;
    });
    presenter.edit(bookmarks::EditBookmark{.id = createdId,
                                           .url = "https://after.example",
                                           .title = "After",
                                           .description = {},
                                           .notes = {},
                                           .tags = {}});
    REQUIRE(pumpUntil([&] { return gotEdited; }));
    REQUIRE_FALSE(presenter.busy());
    CHECK(edited.id == createdId);
    CHECK(edited.url == "https://after.example");
    CHECK(edited.title == "After");

    // Persisted, not merely reflected back from the action.
    bookmarks::BookmarkView reloaded;
    bool gotReloaded = false;
    QObject::connect(&presenter, &bookmarks::gui::BookmarkPresenter::loaded, [&](bookmarks::BookmarkView view) {
        reloaded = view;
        gotReloaded = true;
    });
    presenter.get(bookmarks::GetBookmark{.id = createdId});
    REQUIRE(pumpUntil([&] { return gotReloaded; }));
    CHECK(reloaded.url == "https://after.example");
    CHECK(reloaded.title == "After");
}

TEST_CASE("BookmarkPresenter::archive then unarchive a bookmark, all three backend modes", "[bookmarks][presenter]") {
    const auto mode = GENERATE(Mode::Local, Mode::LocalSingleThread, Mode::Socket);
    CAPTURE(mode);
    DbFixture fixture;
    auto rig = makeAuthedRig(mode, "presenter-archive-secret", "alice");
    bookmarks::gui::BookmarkPresenter presenter{rig->bridge(0), rig->executor()};

    bookmarks::BookmarkId createdId;
    bool created = false;
    QObject::connect(&presenter, &bookmarks::gui::BookmarkPresenter::created,
                     [&](bookmarks::CreateBookmarkResult result) {
                         createdId = result.id;
                         created = true;
                     });
    presenter.create(makeCreate("https://archivable.example"));
    REQUIRE(pumpUntil([&] { return created; }));

    bool archived = false;
    QObject::connect(&presenter, &bookmarks::gui::BookmarkPresenter::archived, [&] { archived = true; });
    presenter.archive(bookmarks::ArchiveBookmark{.id = createdId});
    REQUIRE(pumpUntil([&] { return archived; }));
    REQUIRE_FALSE(presenter.busy());

    bookmarks::BookmarkView archivedView;
    bool gotArchivedView = false;
    QObject::connect(&presenter, &bookmarks::gui::BookmarkPresenter::loaded, [&](bookmarks::BookmarkView view) {
        archivedView = view;
        gotArchivedView = true;
    });
    presenter.get(bookmarks::GetBookmark{.id = createdId});
    REQUIRE(pumpUntil([&] { return gotArchivedView; }));
    CHECK(archivedView.archiveState == bookmarks::ArchiveState::Archived);

    bool unarchived = false;
    QObject::connect(&presenter, &bookmarks::gui::BookmarkPresenter::unarchived, [&] { unarchived = true; });
    presenter.unarchive(bookmarks::UnarchiveBookmark{.id = createdId});
    REQUIRE(pumpUntil([&] { return unarchived; }));
    REQUIRE_FALSE(presenter.busy());
}

TEST_CASE("BookmarkPresenter::remove deletes a bookmark, and a follow-up get fails, all three backend modes",
          "[bookmarks][presenter]") {
    const auto mode = GENERATE(Mode::Local, Mode::LocalSingleThread, Mode::Socket);
    CAPTURE(mode);
    DbFixture fixture;
    auto rig = makeAuthedRig(mode, "presenter-remove-secret", "alice");
    bookmarks::gui::BookmarkPresenter presenter{rig->bridge(0), rig->executor()};

    bookmarks::BookmarkId createdId;
    bool created = false;
    QObject::connect(&presenter, &bookmarks::gui::BookmarkPresenter::created,
                     [&](bookmarks::CreateBookmarkResult result) {
                         createdId = result.id;
                         created = true;
                     });
    presenter.create(makeCreate("https://doomed.example"));
    REQUIRE(pumpUntil([&] { return created; }));

    bool removed = false;
    QObject::connect(&presenter, &bookmarks::gui::BookmarkPresenter::removed, [&] { removed = true; });
    presenter.remove(bookmarks::DeleteBookmark{.id = createdId});
    REQUIRE(pumpUntil([&] { return removed; }));
    REQUIRE_FALSE(presenter.busy());

    QString failure;
    bool failed = false;
    QObject::connect(&presenter, &bookmarks::gui::BookmarkPresenter::failed, [&](QString message) {
        failure = message;
        failed = true;
    });
    presenter.get(bookmarks::GetBookmark{.id = createdId});
    REQUIRE(pumpUntil([&] { return failed; }));
    CHECK_FALSE(failure.isEmpty());
    REQUIRE_FALSE(presenter.busy());
}

TEST_CASE("BookmarkPresenter::list returns the bookmarks just created, all three backend modes",
          "[bookmarks][presenter]") {
    const auto mode = GENERATE(Mode::Local, Mode::LocalSingleThread, Mode::Socket);
    CAPTURE(mode);
    DbFixture fixture;
    auto rig = makeAuthedRig(mode, "presenter-list-secret", "alice");
    bookmarks::gui::BookmarkPresenter presenter{rig->bridge(0), rig->executor()};

    std::vector<bookmarks::BookmarkId> createdIds;
    QObject::connect(&presenter, &bookmarks::gui::BookmarkPresenter::created,
                     [&](bookmarks::CreateBookmarkResult result) { createdIds.push_back(result.id); });

    constexpr int kCount = 3;
    for (int i = 0; i < kCount; ++i) {
        presenter.create(makeCreate("https://listed" + std::to_string(i) + ".example"));
        REQUIRE(pumpUntil([&] { return static_cast<int>(createdIds.size()) == i + 1; }));
    }
    REQUIRE(createdIds.size() == static_cast<std::size_t>(kCount));

    bookmarks::ListBookmarksResult listed;
    bool gotListed = false;
    QObject::connect(&presenter, &bookmarks::gui::BookmarkPresenter::listed,
                     [&](bookmarks::ListBookmarksResult result) {
                         listed = std::move(result);
                         gotListed = true;
                     });
    presenter.list(bookmarks::ListBookmarks{});
    REQUIRE(pumpUntil([&] { return gotListed; }));
    REQUIRE_FALSE(presenter.busy());

    REQUIRE(listed.bookmarks.size() == static_cast<std::size_t>(kCount));
    for (const auto& id : createdIds) {
        CHECK(std::ranges::find_if(listed.bookmarks, [&](const bookmarks::BookmarkSummary& summary) {
                  return summary.id == id;
              }) != listed.bookmarks.end());
    }
}

TEST_CASE(
    "BookmarkPresenter::getChangesSince returns only bookmarks touched after the given instant, "
    "all three backend modes",
    "[bookmarks][presenter]") {
    const auto mode = GENERATE(Mode::Local, Mode::LocalSingleThread, Mode::Socket);
    CAPTURE(mode);
    DbFixture fixture;
    auto rig = makeAuthedRig(mode, "presenter-changes-secret", "alice");
    bookmarks::gui::BookmarkPresenter presenter{rig->bridge(0), rig->executor()};

    bookmarks::GetChangesSinceResult firstPoll;
    bool gotFirstPoll = false;
    QObject::connect(&presenter, &bookmarks::gui::BookmarkPresenter::changesSince,
                     [&](bookmarks::GetChangesSinceResult result) {
                         firstPoll = std::move(result);
                         gotFirstPoll = true;
                     });
    presenter.getChangesSince(bookmarks::GetChangesSince{});
    REQUIRE(pumpUntil([&] { return gotFirstPoll; }));
    REQUIRE_FALSE(presenter.busy());
    CHECK(firstPoll.changed.empty());
    const auto cursor = firstPoll.asOf;

    bookmarks::BookmarkId createdId;
    bool created = false;
    QObject::connect(&presenter, &bookmarks::gui::BookmarkPresenter::created,
                     [&](bookmarks::CreateBookmarkResult result) {
                         createdId = result.id;
                         created = true;
                     });
    presenter.create(makeCreate("https://changed.example"));
    REQUIRE(pumpUntil([&] { return created; }));

    bookmarks::GetChangesSinceResult secondPoll;
    bool gotSecondPoll = false;
    QObject::connect(&presenter, &bookmarks::gui::BookmarkPresenter::changesSince,
                     [&](bookmarks::GetChangesSinceResult result) {
                         secondPoll = std::move(result);
                         gotSecondPoll = true;
                     });
    presenter.getChangesSince(bookmarks::GetChangesSince{.since = cursor});
    REQUIRE(pumpUntil([&] { return gotSecondPoll; }));
    REQUIRE_FALSE(presenter.busy());
    REQUIRE(secondPoll.changed.size() == 1);
    CHECK(secondPoll.changed.front().id == createdId);
}

TEST_CASE(
    "BookmarkPresenter::bulkEdit applies tags and archive state to every given id, "
    "all three backend modes",
    "[bookmarks][presenter]") {
    const auto mode = GENERATE(Mode::Local, Mode::LocalSingleThread, Mode::Socket);
    CAPTURE(mode);
    DbFixture fixture;
    auto rig = makeAuthedRig(mode, "presenter-bulk-secret", "alice");
    bookmarks::gui::BookmarkPresenter presenter{rig->bridge(0), rig->executor()};

    std::vector<bookmarks::BookmarkId> createdIds;
    QObject::connect(&presenter, &bookmarks::gui::BookmarkPresenter::created,
                     [&](bookmarks::CreateBookmarkResult result) { createdIds.push_back(result.id); });
    presenter.create(makeCreate("https://bulk-one.example"));
    REQUIRE(pumpUntil([&] { return createdIds.size() == 1; }));
    presenter.create(makeCreate("https://bulk-two.example"));
    REQUIRE(pumpUntil([&] { return createdIds.size() == 2; }));

    bookmarks::BulkEditResult bulkResult;
    bool bulkEdited = false;
    QObject::connect(&presenter, &bookmarks::gui::BookmarkPresenter::bulkEdited,
                     [&](bookmarks::BulkEditResult result) {
                         bulkResult = result;
                         bulkEdited = true;
                     });
    presenter.bulkEdit(bookmarks::BulkEdit{
        .ids = createdIds, .addTags = {"batch"}, .removeTags = {}, .archive = bookmarks::BulkArchiveOp::Archive});
    REQUIRE(pumpUntil([&] { return bulkEdited; }));
    REQUIRE_FALSE(presenter.busy());
    CHECK(morph::math::floor(*bulkResult.affected) == 2);
}

TEST_CASE("BookmarkPresenter::importChunk then exportAll round-trips bookmarks, all three backend modes",
          "[bookmarks][presenter]") {
    const auto mode = GENERATE(Mode::Local, Mode::LocalSingleThread, Mode::Socket);
    CAPTURE(mode);
    DbFixture fixture;
    auto rig = makeAuthedRig(mode, "presenter-import-secret", "alice");
    bookmarks::gui::BookmarkPresenter presenter{rig->bridge(0), rig->executor()};

    bookmarks::ImportBookmarksResult importResult;
    bool imported = false;
    QObject::connect(&presenter, &bookmarks::gui::BookmarkPresenter::imported,
                     [&](bookmarks::ImportBookmarksResult result) {
                         importResult = result;
                         imported = true;
                     });
    bookmarks::ImportBookmarks importAction;
    importAction.chunk = R"(<DT><A HREF="https://imported.example">Imported</A>)";
    importAction.opId = bookmarks::ImportOpId{"presenter-import-1"};
    presenter.importChunk(importAction);
    REQUIRE(pumpUntil([&] { return imported; }));
    REQUIRE_FALSE(presenter.busy());
    CHECK(morph::math::floor(*importResult.imported) == 1);

    bookmarks::ExportBookmarksResult exportResult;
    bool exported = false;
    QObject::connect(&presenter, &bookmarks::gui::BookmarkPresenter::exported,
                     [&](bookmarks::ExportBookmarksResult result) {
                         exportResult = std::move(result);
                         exported = true;
                     });
    presenter.exportAll(bookmarks::ExportBookmarks{});
    REQUIRE(pumpUntil([&] { return exported; }));
    REQUIRE_FALSE(presenter.busy());
    CHECK(exportResult.html.find("https://imported.example") != std::string::npos);
}

TEST_CASE("Every BookmarkPresenter validation-driven action routes its failure to failed(), not just create()",
          "[bookmarks][presenter]") {
    // Not a completeness ritual: each action's `reportError` is wired
    // independently at its own `track()` call site (`bookmark_presenter.cpp`),
    // so a passing test for one action says nothing about whether another
    // action's wiring is correct. See pastebin::gui::PastePresenter's
    // identical test for the same rationale.
    DbFixture fixture;
    auto rig = makeAuthedRig(Mode::Local, "presenter-fail-secret", "alice");
    bookmarks::gui::BookmarkPresenter presenter{rig->bridge(0), rig->executor()};

    QString failure;
    int failures = 0;
    QObject::connect(&presenter, &bookmarks::gui::BookmarkPresenter::failed, [&](QString message) {
        failure = message;
        ++failures;
    });

    // create: empty url fails CreateBookmark::validate().
    presenter.create(bookmarks::CreateBookmark{});
    REQUIRE(pumpUntil([&] { return failures == 1; }));
    REQUIRE_FALSE(presenter.busy());

    // edit: disengaged id and empty url both fail EditBookmark::validate().
    presenter.edit(bookmarks::EditBookmark{});
    REQUIRE(pumpUntil([&] { return failures == 2; }));
    REQUIRE_FALSE(presenter.busy());

    // archive/unarchive/remove/get: a disengaged id fails each validate().
    presenter.archive(bookmarks::ArchiveBookmark{});
    REQUIRE(pumpUntil([&] { return failures == 3; }));
    presenter.unarchive(bookmarks::UnarchiveBookmark{});
    REQUIRE(pumpUntil([&] { return failures == 4; }));
    presenter.remove(bookmarks::DeleteBookmark{});
    REQUIRE(pumpUntil([&] { return failures == 5; }));
    presenter.get(bookmarks::GetBookmark{});
    REQUIRE(pumpUntil([&] { return failures == 6; }));
    REQUIRE_FALSE(presenter.busy());

    // bulkEdit: an empty id list fails BulkEdit::validate().
    presenter.bulkEdit(bookmarks::BulkEdit{});
    REQUIRE(pumpUntil([&] { return failures == 7; }));
    REQUIRE_FALSE(presenter.busy());

    // importChunk: an empty chunk fails ImportBookmarks::validate().
    presenter.importChunk(bookmarks::ImportBookmarks{});
    REQUIRE(pumpUntil([&] { return failures == 8; }));
    REQUIRE_FALSE(presenter.busy());
    CHECK_FALSE(failure.isEmpty());
}

TEST_CASE("BookmarkPresenter::get against an unknown id emits failed, not a crash", "[bookmarks][presenter]") {
    DbFixture fixture;
    auto rig = makeAuthedRig(Mode::Local, "presenter-get-unknown-secret", "alice");
    bookmarks::gui::BookmarkPresenter presenter{rig->bridge(0), rig->executor()};

    QString failure;
    bool failed = false;
    QObject::connect(&presenter, &bookmarks::gui::BookmarkPresenter::failed, [&](QString message) {
        failure = message;
        failed = true;
    });
    presenter.get(bookmarks::GetBookmark{.id = bookmarks::BookmarkId{999999}});
    REQUIRE(pumpUntil([&] { return failed; }));
    CHECK_FALSE(failure.isEmpty());
    REQUIRE_FALSE(presenter.busy());
}

TEST_CASE(
    "BookmarkPresenter::list/getChangesSince/exportAll all emit failed with no session at all, "
    "not a crash",
    "[bookmarks][presenter]") {
    // list/getChangesSince/exportAll all have `validate() { return true; }`
    // unconditionally -- their only reachable failure is a genuine model-level
    // error, not a validation one. `BookmarkModel`'s own `requirePrincipal()`
    // (bookmark_model.cpp) throws `Forbidden` before touching the database at
    // all when `session::current()` carries no principal, so an unauthenticated
    // bridge (no `setDefaultSession` call, mirroring
    // test_shared_feed_presenter.cpp's identical "no session" case) reaches
    // exactly that path safely.
    //
    // A dropped-table variant of this case was tried first and reverted: even
    // one drop-then-`DbFixture`-reapply cycle against `bookmarks` (a table
    // three other tables foreign-key into), run inside this file's much larger
    // suite of `BackendRig`-driven test cases, was empirically observed to
    // corrupt Lightweight's `SqlMigration` fold-state cache
    // (`ComputeUpgradeForTable`'s `.at()` lookup stops finding its key) and
    // cascade failures into unrelated later tests across the whole binary,
    // including files that never touch a dropped table. Not a bug in
    // `BookmarkPresenter` or in this rung's schema -- this case avoids it
    // entirely by never mutating the schema mid-suite.
    DbFixture fixture;
    BackendRig rig{Mode::Local, 1};
    bookmarks::gui::BookmarkPresenter presenter{rig.bridge(0), rig.executor()};

    QString failure;
    int failures = 0;
    QObject::connect(&presenter, &bookmarks::gui::BookmarkPresenter::failed, [&](QString message) {
        failure = message;
        ++failures;
    });

    presenter.list(bookmarks::ListBookmarks{});
    REQUIRE(pumpUntil([&] { return failures == 1; }));
    REQUIRE_FALSE(presenter.busy());

    presenter.getChangesSince(bookmarks::GetChangesSince{});
    REQUIRE(pumpUntil([&] { return failures == 2; }));
    REQUIRE_FALSE(presenter.busy());

    presenter.exportAll(bookmarks::ExportBookmarks{});
    REQUIRE(pumpUntil([&] { return failures == 3; }));
    CHECK_FALSE(failure.isEmpty());
    REQUIRE_FALSE(presenter.busy());
}

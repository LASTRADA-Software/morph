// SPDX-License-Identifier: Apache-2.0
//
// SharedFeedPresenter's own suite (Task 17): its one action (list) round-trips
// through the presenter's own signals, not the model directly, across the
// full BackendRig mode matrix (Local/LocalSingleThread/Socket). Domain rules
// (cross-principal visibility, archived-bookmark exclusion) already have a
// dedicated suite at the model level (test_shared_feed_model.cpp); this file
// only proves the presenter wires the action to the right signal and neither
// crashes nor hangs. See test_bookmark_presenter.cpp's own top comment for
// the full rationale this mirrors, including why every mode needs a real
// signed token.

#include <bookmarks/auth/bookmarks_authorizer.hpp>
#include <bookmarks/models/bookmark_model.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <memory>
#include <morph/session/session_auth.hpp>
#include <string>
#include <string_view>

#include "shared_feed_presenter.hpp"
#include "testkit/backend_rig.hpp"
#include "testkit/db_fixture.hpp"
#include "testkit/pump.hpp"

namespace {

using morph::ladder::testkit::awaitQt;
using morph::ladder::testkit::BackendRig;
using morph::ladder::testkit::DbFixture;
using morph::ladder::testkit::Mode;
using morph::ladder::testkit::pumpUntil;

/// @brief Builds a rig authenticated as @p principal, for @p mode, over a
///        fresh authorizer keyed on @p secret. See
///        test_bookmark_presenter.cpp's identical helper for the full
///        rationale.
[[nodiscard]] std::unique_ptr<BackendRig> makeAuthedRig(Mode mode, std::string_view secret, std::string principal) {
    const auto authorizer =
        std::make_shared<bookmarks::auth::BookmarksAuthorizer>(std::string{secret}, morph::session::hmacSha256);
    auto rig = std::make_unique<BackendRig>(mode, 1, authorizer);
    const morph::session::TokenIssuer issuer{std::string{secret}, morph::session::hmacSha256};
    morph::session::Context ctx;
    ctx.principal = std::move(principal);
    ctx.token = issuer.issue(
        morph::session::SessionToken{.principal = ctx.principal, .expiresAtMs = 4102444800000, .roles = {}});
    rig->bridge(0).setDefaultSession(ctx);
    return rig;
}

/// @brief Creates a bookmark with the given @p visibility via a direct
///        `BookmarkModel` dispatch through @p handler, bypassing
///        `BookmarkPresenter` entirely -- this suite's job is
///        `SharedFeedPresenter`, not bookmark creation.
///
/// @p handler is supplied by the caller and must outlive every call site:
/// see test_tag_presenter.cpp's identical helper (`seedTaggedBookmark`) for
/// why a short-lived, per-call handler is unsafe in `Mode::Socket` -- two
/// such handlers constructed back to back race a `deregister` reply against
/// the next handler's synchronous registration, occasionally leaving the new
/// binding permanently unbound (`Bridge::executeVia` then fails every
/// dispatch with "handler not bound", not just the first). Reproduced here
/// empirically, not just by inference: this file's own two-`seedBookmark`
/// call sequence below hit it directly.
void seedBookmark(::morph::bridge::BridgeHandler<bookmarks::BookmarkModel>& handler, std::string url,
                  bookmarks::Visibility visibility) {
    bookmarks::CreateBookmark create;
    create.url = std::move(url);
    create.visibility = visibility;
    (void)awaitQt(handler.execute(create));
}

}  // namespace

TEST_CASE(
    "SharedFeedPresenter::list returns every shared bookmark, never a private one, "
    "all three backend modes",
    "[bookmarks][presenter]") {
    const auto mode = GENERATE(Mode::Local, Mode::LocalSingleThread, Mode::Socket);
    CAPTURE(mode);
    DbFixture fixture;
    auto rig = makeAuthedRig(mode, "shared-feed-presenter-list-secret", "alice");
    // Declared before `presenter` (and so, by C++'s reverse local-destruction
    // order, torn down *after* it) -- see `seedBookmark`'s own doc comment.
    auto bookmarkHandler = rig->client<bookmarks::BookmarkModel>(0);
    seedBookmark(bookmarkHandler, "https://alice-private.example", bookmarks::Visibility::Private);
    seedBookmark(bookmarkHandler, "https://alice-shared.example", bookmarks::Visibility::Shared);

    bookmarks::gui::SharedFeedPresenter presenter{rig->bridge(0), rig->executor()};
    bookmarks::ListSharedFeedResult listed;
    bool gotListed = false;
    QObject::connect(&presenter, &bookmarks::gui::SharedFeedPresenter::listed,
                     [&](bookmarks::ListSharedFeedResult result) {
                         listed = std::move(result);
                         gotListed = true;
                     });
    presenter.list(bookmarks::ListSharedFeed{});
    REQUIRE(pumpUntil([&] { return gotListed; }));
    REQUIRE_FALSE(presenter.busy());
    REQUIRE(listed.bookmarks.size() == 1);
    CHECK(listed.bookmarks.front().url == "https://alice-shared.example");
}

TEST_CASE("SharedFeedPresenter::list with no session at all emits failed, not a crash", "[bookmarks][presenter]") {
    // SharedFeedModel::execute throws Forbidden with no session
    // (test_shared_feed_model.cpp's identical model-level case) -- proves the
    // presenter surfaces that as `failed()` rather than crashing, using a
    // bridge that never had `setDefaultSession` called on it.
    DbFixture fixture;
    BackendRig rig{Mode::Local, 1};
    bookmarks::gui::SharedFeedPresenter presenter{rig.bridge(0), rig.executor()};

    QString failure;
    bool failed = false;
    QObject::connect(&presenter, &bookmarks::gui::SharedFeedPresenter::failed, [&](QString message) {
        failure = message;
        failed = true;
    });
    presenter.list(bookmarks::ListSharedFeed{});
    REQUIRE(pumpUntil([&] { return failed; }));
    CHECK_FALSE(failure.isEmpty());
    REQUIRE_FALSE(presenter.busy());
}

// A dedicated "ListSharedFeed against a broken store" case (drop the
// `bookmarks` table out from under the query) is deliberately not repeated
// here: test_bookmark_presenter.cpp's own consolidated broken-store case
// already drops and reapplies that same table's schema once per process --
// see that test's doc comment for why a *second* such cycle in the same
// process deterministically corrupts Lightweight's `SqlMigration` fold-state
// cache and takes down every later `DbFixture` in the binary. The no-session
// case above already proves `SharedFeedPresenter` surfaces a genuine
// model-thrown error as `failed()` rather than crashing; that mechanism
// (typed exception -> `reportError` -> `failed()`) is identical regardless of
// which exception type triggers it, and `BookmarkPresenter`'s own suite
// separately proves the broken-store path specifically.

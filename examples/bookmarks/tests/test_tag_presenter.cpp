// SPDX-License-Identifier: Apache-2.0
//
// TagPresenter's own suite (Task 17): each of its three actions
// (rename/merge/list) round-trips through the presenter's own signals, not
// the model directly, across the full BackendRig mode matrix
// (Local/LocalSingleThread/Socket). Domain rules (ownership, collision
// detection, the merge cascade) already have a dedicated suite at the model
// level (test_tag_model.cpp); this file only proves the presenter wires each
// action to the right signal and neither crashes nor hangs. See
// test_bookmark_presenter.cpp's own top comment for the full rationale this
// mirrors, including why every mode needs a real signed token.

#include "tag_presenter.hpp"
#include "testkit/backend_rig.hpp"
#include "testkit/db_fixture.hpp"
#include "testkit/pump.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <bookmarks/auth/bookmarks_authorizer.hpp>
#include <bookmarks/models/bookmark_model.hpp>
#include <morph/session/session_auth.hpp>

#include <algorithm>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

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

/// @brief Creates a bookmark tagged @p tags via a direct `BookmarkModel`
///        dispatch through @p handler, bypassing `BookmarkPresenter`
///        entirely -- this suite's job is `TagPresenter`, not bookmark
///        creation.
///
/// @p handler is supplied by the caller, and deliberately outlives every
/// call site below -- see those call sites' own comments for why: a
/// short-lived, per-call handler is the actual root cause this signature
/// avoids.
///
/// Returns nothing: no caller in this suite needs the new bookmark's id --
/// every assertion here is about the *tags* the seed created, looked up by
/// name. The `awaitQt` is still load-bearing, and is the whole point of the
/// helper: it makes the seed synchronous, so a `TagPresenter::list` issued
/// on the next line cannot race the rows it is meant to see.
///
/// @param handler Live handler the create is dispatched through.
/// @param url     The new bookmark's url.
/// @param tags    Tag names to create and attach.
void seedTaggedBookmark(::morph::bridge::BridgeHandler<bookmarks::BookmarkModel>& handler, std::string url,
                        std::vector<std::string> tags) {
    bookmarks::CreateBookmark create;
    create.url = std::move(url);
    create.tags = std::move(tags);
    static_cast<void>(awaitQt(handler.execute(create)));
}

}  // namespace

TEST_CASE("TagPresenter::list returns every tag the caller owns, all three backend modes", "[bookmarks][presenter]") {
    const auto mode = GENERATE(Mode::Local, Mode::LocalSingleThread, Mode::Socket);
    CAPTURE(mode);
    DbFixture fixture;
    auto rig = makeAuthedRig(mode, "tag-presenter-list-secret", "alice");
    // Declared before `presenter` (and so, by C++'s reverse local-destruction
    // order, torn down *after* it): see this file's top-of-suite note above
    // `seedTaggedBookmark` -- a short-lived handler's teardown message would
    // otherwise race `presenter`'s own registration on the same connection.
    auto bookmarkHandler = rig->client<bookmarks::BookmarkModel>(0);
    seedTaggedBookmark(bookmarkHandler, "https://one.example", {"cpp", "rust"});

    bookmarks::gui::TagPresenter presenter{rig->bridge(0), rig->executor()};
    bookmarks::ListTagsResult listed;
    bool gotListed = false;
    QObject::connect(&presenter, &bookmarks::gui::TagPresenter::listed, [&](bookmarks::ListTagsResult result) {
        listed = std::move(result);
        gotListed = true;
    });
    presenter.list(bookmarks::ListTags{});
    REQUIRE(pumpUntil([&] { return gotListed; }));
    REQUIRE_FALSE(presenter.busy());
    REQUIRE(listed.tags.size() == 2);
    CHECK(std::ranges::find_if(listed.tags, [](auto& t) { return t.name == "cpp"; }) != listed.tags.end());
    CHECK(std::ranges::find_if(listed.tags, [](auto& t) { return t.name == "rust"; }) != listed.tags.end());
}

TEST_CASE("TagPresenter::rename renames a tag owned by the caller, all three backend modes",
          "[bookmarks][presenter]") {
    const auto mode = GENERATE(Mode::Local, Mode::LocalSingleThread, Mode::Socket);
    CAPTURE(mode);
    DbFixture fixture;
    auto rig = makeAuthedRig(mode, "tag-presenter-rename-secret", "alice");
    // See the list test above for why this handler outlives `presenter`.
    auto bookmarkHandler = rig->client<bookmarks::BookmarkModel>(0);
    seedTaggedBookmark(bookmarkHandler, "https://one.example", {"old"});

    bookmarks::gui::TagPresenter presenter{rig->bridge(0), rig->executor()};
    bookmarks::ListTagsResult before;
    bool gotBefore = false;
    QObject::connect(&presenter, &bookmarks::gui::TagPresenter::listed, [&](bookmarks::ListTagsResult result) {
        before = std::move(result);
        gotBefore = true;
    });
    presenter.list(bookmarks::ListTags{});
    REQUIRE(pumpUntil([&] { return gotBefore; }));
    REQUIRE(before.tags.size() == 1);
    const auto tagId = before.tags.front().id;

    bool renamed = false;
    QObject::connect(&presenter, &bookmarks::gui::TagPresenter::renamed, [&] { renamed = true; });
    presenter.rename(bookmarks::RenameTag{.id = tagId, .name = "new"});
    REQUIRE(pumpUntil([&] { return renamed; }));
    REQUIRE_FALSE(presenter.busy());

    bookmarks::ListTagsResult after;
    bool gotAfter = false;
    QObject::connect(&presenter, &bookmarks::gui::TagPresenter::listed, [&](bookmarks::ListTagsResult result) {
        after = std::move(result);
        gotAfter = true;
    });
    presenter.list(bookmarks::ListTags{});
    REQUIRE(pumpUntil([&] { return gotAfter; }));
    REQUIRE(after.tags.size() == 1);
    CHECK(after.tags.front().name == "new");
}

TEST_CASE("TagPresenter::merge reassigns every bookmark from source to target and deletes source, "
          "all three backend modes",
          "[bookmarks][presenter]") {
    const auto mode = GENERATE(Mode::Local, Mode::LocalSingleThread, Mode::Socket);
    CAPTURE(mode);
    DbFixture fixture;
    auto rig = makeAuthedRig(mode, "tag-presenter-merge-secret", "alice");
    // One handler reused for both seed calls -- not just for the list test's
    // reason above, but because this test is where the underlying bug was
    // actually caught: `Bridge::registerHandler()`'s only synchronous path
    // (`BackendRig::Socket` never opts into `asyncRegistrationEnabled`) blocks
    // in `QtWebSocketBackend::sendSync` via a nested `QEventLoop`, waiting for
    // a reply whose wire envelope carries `callId == 0` -- the same `callId`
    // every fire-and-forget `deregister` reply also carries (`onTextMessage`
    // has no other way to tell "the sync reply I'm parked for" from "an
    // unrelated deregister ack") from `QtWebSocketBackend::deregisterModel`.
    // Two short-lived handlers back to back -- construct, dispatch, destruct
    // (deregister), construct again -- let a fresh registration's `sendSync`
    // park its nested loop while the *previous* handler's still-in-flight
    // deregister ack is loose on the wire; if that ack's "ok" reply (with no
    // `modelId` field) lands first, `onTextMessage` hands it to the parked
    // loop instead of the real register reply, and the new binding's
    // `currentId` is stored as 0 -- permanently, since the actual register
    // reply that arrives afterward has nowhere left to go (`_syncLoop` was
    // already reset). Every later dispatch on that binding then fails fast
    // with "handler not bound" (`Bridge::executeVia`), forever, not just
    // transiently -- confirmed by instrumented reruns: a bounded retry loop
    // (an earlier version of this fix) burned its full deadline every time
    // rather than ever recovering, exactly what a permanently-zeroed
    // `currentId` predicts, not what a merely slow round trip would. Keeping
    // one handler alive across both bookmarks removes the *deregister* from
    // between the two registrations entirely -- there is no longer a stray
    // reply in flight for a later `sendSync` to catch. This is a real
    // `QtWebSocketBackend`/`Bridge` protocol-correlation bug (`include/morph/
    // qt/qt_websocket_backend.hpp`'s `deregisterModel` vs. `sendSync`'s
    // shared `callId == 0` bucket), not a `Presenter`/`TagPresenter` defect;
    // fixing it there is out of scope here (framework code, not this rung's
    // testkit) -- see this task's report for the finding writeup.
    auto bookmarkHandler = rig->client<bookmarks::BookmarkModel>(0);
    seedTaggedBookmark(bookmarkHandler, "https://one.example", {"cpp"});
    seedTaggedBookmark(bookmarkHandler, "https://two.example", {"cpp", "c++"});

    bookmarks::gui::TagPresenter presenter{rig->bridge(0), rig->executor()};
    bookmarks::ListTagsResult before;
    bool gotBefore = false;
    QObject::connect(&presenter, &bookmarks::gui::TagPresenter::listed, [&](bookmarks::ListTagsResult result) {
        before = std::move(result);
        gotBefore = true;
    });
    presenter.list(bookmarks::ListTags{});
    REQUIRE(pumpUntil([&] { return gotBefore; }));
    REQUIRE(before.tags.size() == 2);
    const auto cppId = std::ranges::find_if(before.tags, [](auto& t) { return t.name == "cpp"; })->id;
    const auto cxxId = std::ranges::find_if(before.tags, [](auto& t) { return t.name == "c++"; })->id;

    bool merged = false;
    QObject::connect(&presenter, &bookmarks::gui::TagPresenter::merged, [&] { merged = true; });
    presenter.merge(bookmarks::MergeTags{.sourceId = cppId, .targetId = cxxId});
    REQUIRE(pumpUntil([&] { return merged; }));
    REQUIRE_FALSE(presenter.busy());

    bookmarks::ListTagsResult after;
    bool gotAfter = false;
    QObject::connect(&presenter, &bookmarks::gui::TagPresenter::listed, [&](bookmarks::ListTagsResult result) {
        after = std::move(result);
        gotAfter = true;
    });
    presenter.list(bookmarks::ListTags{});
    REQUIRE(pumpUntil([&] { return gotAfter; }));
    REQUIRE(after.tags.size() == 1);  // "cpp" is gone
    CHECK(after.tags.front().name == "c++");
}

TEST_CASE("Every TagPresenter action routes its failure to failed(), not just rename()", "[bookmarks][presenter]") {
    DbFixture fixture;
    auto rig = makeAuthedRig(Mode::Local, "tag-presenter-fail-secret", "alice");
    bookmarks::gui::TagPresenter presenter{rig->bridge(0), rig->executor()};

    QString failure;
    int failures = 0;
    QObject::connect(&presenter, &bookmarks::gui::TagPresenter::failed, [&](QString message) {
        failure = message;
        ++failures;
    });

    // rename: a disengaged id fails RenameTag::validate().
    presenter.rename(bookmarks::RenameTag{});
    REQUIRE(pumpUntil([&] { return failures == 1; }));
    REQUIRE_FALSE(presenter.busy());

    // merge: two disengaged (and thus equal) ids fail MergeTags::validate().
    presenter.merge(bookmarks::MergeTags{});
    REQUIRE(pumpUntil([&] { return failures == 2; }));
    REQUIRE_FALSE(presenter.busy());
    CHECK_FALSE(failure.isEmpty());
}

TEST_CASE("TagPresenter::list with no session at all emits failed, not a crash", "[bookmarks][presenter]") {
    // ListTags has `validate() { return true; }` unconditionally -- its only
    // reachable failure is a genuine model-level error, not a validation one.
    // `TagModel`'s own `requirePrincipal()` (tag_model.cpp) throws `Forbidden`
    // before touching the database at all when `session::current()` carries
    // no principal, so an unauthenticated bridge (no `setDefaultSession` call)
    // reaches exactly that path safely. See
    // test_bookmark_presenter.cpp's identical "no session" case for why this
    // -- not a dropped table -- is the safe way to provoke a genuine failure
    // for an always-`validate()`-true action in this rung's test binary.
    DbFixture fixture;
    BackendRig rig{Mode::Local, 1};
    bookmarks::gui::TagPresenter presenter{rig.bridge(0), rig.executor()};

    QString failure;
    bool failed = false;
    QObject::connect(&presenter, &bookmarks::gui::TagPresenter::failed, [&](QString message) {
        failure = message;
        failed = true;
    });
    presenter.list(bookmarks::ListTags{});
    REQUIRE(pumpUntil([&] { return failed; }));
    CHECK_FALSE(failure.isEmpty());
    REQUIRE_FALSE(presenter.busy());
}

// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>
#include <morph/session/session.hpp>

#include "bookmarks/models/bookmark_model.hpp"
#include "bookmarks/models/shared_feed_model.hpp"
#include "testkit/db_fixture.hpp"

using morph::ladder::testkit::DbFixture;

namespace {

/// @brief A `Context` carrying only @p principal.
///
/// Built field-by-field rather than with a designated initializer on
/// purpose: `-Weverything` includes
/// `-Wmissing-designated-field-initializers`, which fires on a partial
/// designated-initializer list, and `ladder_<rung>_tests` is built with
/// `apply_warnings()` (so `-Werror` under `MORPH_ENABLE_STRICT_COMPILATION`,
/// CI's default). Same reason `makeCreate` below exists (see
/// `test_app.cpp`'s `contextFor`/`makeCreate` for the original pattern).
[[nodiscard]] morph::session::Context contextFor(std::string principal) {
    morph::session::Context ctx;
    ctx.principal = std::move(principal);
    return ctx;
}

class ScopedPrincipal {
public:
    explicit ScopedPrincipal(std::string principal) : _ctx{contextFor(std::move(principal))}, _scope{_ctx} {}

private:
    morph::session::Context _ctx;
    morph::session::detail::ScopedContext _scope;
};

/// @brief A `CreateBookmark` for @p url with the given @p visibility. See
///        `contextFor` for why this is not a designated initializer.
[[nodiscard]] bookmarks::CreateBookmark makeCreate(std::string url,
                                                   bookmarks::Visibility visibility = bookmarks::Visibility::Private) {
    bookmarks::CreateBookmark action;
    action.url = std::move(url);
    action.visibility = visibility;
    return action;
}

}  // namespace

TEST_CASE("ListSharedFeed returns every user's shared bookmarks, never a private one", "[bookmarks][model]") {
    DbFixture fixture;
    bookmarks::BookmarkModel bookmarkModel;
    bookmarks::SharedFeedModel feedModel;
    {
        const ScopedPrincipal alice{"alice"};
        bookmarkModel.execute(makeCreate("https://alice-private.example"));
        bookmarkModel.execute(makeCreate("https://alice-shared.example", bookmarks::Visibility::Shared));
    }
    const ScopedPrincipal bob{"bob"};
    bookmarkModel.execute(makeCreate("https://bob-shared.example", bookmarks::Visibility::Shared));

    const auto feed = feedModel.execute(bookmarks::ListSharedFeed{});
    REQUIRE(feed.bookmarks.size() == 2);
    for (const auto& row : feed.bookmarks) {
        CHECK((row.url == "https://alice-shared.example" || row.url == "https://bob-shared.example"));
    }
}

TEST_CASE("ListSharedFeed excludes an archived-but-shared bookmark", "[bookmarks][model]") {
    DbFixture fixture;
    bookmarks::BookmarkModel bookmarkModel;
    bookmarks::SharedFeedModel feedModel;
    const ScopedPrincipal alice{"alice"};
    const auto id = bookmarkModel.execute(makeCreate("https://one.example", bookmarks::Visibility::Shared)).id;
    bookmarkModel.execute(bookmarks::ArchiveBookmark{.id = id});
    CHECK(feedModel.execute(bookmarks::ListSharedFeed{}).bookmarks.empty());
}

TEST_CASE("ListSharedFeed with no session at all is Forbidden", "[bookmarks][model]") {
    DbFixture fixture;
    bookmarks::SharedFeedModel feedModel;
    REQUIRE_THROWS_AS(feedModel.execute(bookmarks::ListSharedFeed{}), bookmarks::Forbidden);
}

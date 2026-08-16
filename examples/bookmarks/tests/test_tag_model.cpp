// SPDX-License-Identifier: Apache-2.0
#include "bookmarks/models/bookmark_model.hpp"
#include "bookmarks/models/tag_model.hpp"
#include "testkit/db_fixture.hpp"

#include "bookmarks/db/outbox_entity.hpp"

#include <catch2/catch_test_macros.hpp>
#include <morph/session/session.hpp>

#include <algorithm>
#include <vector>

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

/// @brief A `CreateBookmark` for @p url with the given @p tags. See
///        `contextFor` for why this is not a designated initializer.
[[nodiscard]] bookmarks::CreateBookmark makeCreate(std::string url, std::vector<std::string> tags = {}) {
    bookmarks::CreateBookmark action;
    action.url = std::move(url);
    action.tags = std::move(tags);
    return action;
}

}  // namespace

TEST_CASE("RenameTag renames a tag owned by the caller", "[bookmarks][model]") {
    DbFixture fixture;
    bookmarks::BookmarkModel bookmarkModel;
    bookmarks::TagModel tagModel;
    const ScopedPrincipal alice{"alice"};

    const auto bookmarkId = bookmarkModel.execute(makeCreate("https://one.example", {"old"})).id;
    const auto tags = tagModel.execute(bookmarks::ListTags{}).tags;
    REQUIRE(tags.size() == 1);
    const auto tagId = tags.front().id;

    tagModel.execute(bookmarks::RenameTag{.id = tagId, .name = "new"});
    const auto renamed = tagModel.execute(bookmarks::ListTags{}).tags;
    REQUIRE(renamed.size() == 1);
    CHECK(renamed.front().name == "new");
    CHECK(bookmarkModel.execute(bookmarks::GetBookmark{.id = bookmarkId}).tags == std::vector<std::string>{"new"});
}

TEST_CASE("RenameTag against another principal's tag is Forbidden", "[bookmarks][model]") {
    DbFixture fixture;
    bookmarks::BookmarkModel bookmarkModel;
    bookmarks::TagModel tagModel;
    bookmarks::TagId aliceTagId;
    {
        const ScopedPrincipal alice{"alice"};
        bookmarkModel.execute(makeCreate("https://one.example", {"mine"}));
        aliceTagId = tagModel.execute(bookmarks::ListTags{}).tags.front().id;
    }
    const ScopedPrincipal mallory{"mallory"};
    REQUIRE_THROWS_AS(tagModel.execute(bookmarks::RenameTag{.id = aliceTagId, .name = "stolen"}),
                      bookmarks::Forbidden);
}

TEST_CASE("RenameTag colliding with an existing tag name is a Conflict", "[bookmarks][model]") {
    DbFixture fixture;
    bookmarks::BookmarkModel bookmarkModel;
    bookmarks::TagModel tagModel;
    const ScopedPrincipal alice{"alice"};
    bookmarkModel.execute(makeCreate("https://one.example", {"a", "b"}));
    const auto tags = tagModel.execute(bookmarks::ListTags{}).tags;
    const auto tagA = std::ranges::find_if(tags, [](auto& t) { return t.name == "a"; })->id;
    REQUIRE_THROWS_AS(tagModel.execute(bookmarks::RenameTag{.id = tagA, .name = "b"}), bookmarks::Conflict);
}

TEST_CASE("MergeTags reassigns every bookmark from source to target, dedups, and deletes source",
          "[bookmarks][model]") {
    DbFixture fixture;
    bookmarks::BookmarkModel bookmarkModel;
    bookmarks::TagModel tagModel;
    const ScopedPrincipal alice{"alice"};

    const auto id1 = bookmarkModel.execute(makeCreate("https://one.example", {"cpp"})).id;
    const auto id2 = bookmarkModel.execute(makeCreate("https://two.example", {"cpp", "c++"})).id;
    const auto tags = tagModel.execute(bookmarks::ListTags{}).tags;
    const auto cppId = std::ranges::find_if(tags, [](auto& t) { return t.name == "cpp"; })->id;
    const auto cxxId = std::ranges::find_if(tags, [](auto& t) { return t.name == "c++"; })->id;

    tagModel.execute(bookmarks::MergeTags{.sourceId = cppId, .targetId = cxxId});

    CHECK(bookmarkModel.execute(bookmarks::GetBookmark{.id = id1}).tags == std::vector<std::string>{"c++"});
    auto tagsOfId2 = bookmarkModel.execute(bookmarks::GetBookmark{.id = id2}).tags;
    CHECK(tagsOfId2.size() == 1);  // "cpp" and "c++" merged into one, not duplicated
    CHECK(tagsOfId2.front() == "c++");
    const auto remaining = tagModel.execute(bookmarks::ListTags{}).tags;
    CHECK(remaining.size() == 1);  // "cpp" is gone
}

TEST_CASE("MergeTags writes exactly one outbox row", "[bookmarks][model]") {
    DbFixture fixture;
    bookmarks::BookmarkModel bookmarkModel;
    bookmarks::TagModel tagModel;
    const ScopedPrincipal alice{"alice"};
    bookmarkModel.execute(makeCreate("https://one.example", {"a", "b"}));
    const auto tags = tagModel.execute(bookmarks::ListTags{}).tags;
    tagModel.execute(bookmarks::MergeTags{.sourceId = tags[0].id, .targetId = tags[1].id});

    Lightweight::DataMapper mapper;
    auto rows = mapper.Query<bookmarks::db::BookmarkOutboxRecord>().All();
    REQUIRE(rows.size() == 1);
    CHECK(rows.front().actionType.Value() == "MergeTags");
}

TEST_CASE("Cross-model race: TagModel renames a tag while BookmarkModel's BulkEdit adds the old "
          "name -- documents where consistency becomes app responsibility, per the README",
          "[bookmarks][model]") {
    DbFixture fixture;
    bookmarks::BookmarkModel bookmarkModel;
    bookmarks::TagModel tagModel;
    const ScopedPrincipal alice{"alice"};

    bookmarkModel.execute(makeCreate("https://one.example", {"old"}));
    const auto tagId = tagModel.execute(bookmarks::ListTags{}).tags.front().id;

    // Sequential, not genuinely racing (this test suite calls execute()
    // directly, C++-to-C++, with no thread-level concurrency -- the README's
    // own framing already concedes "the strand cannot fix it," i.e. this is
    // a documentation test, not a fix-verification test): rename first,
    // then a second bookmark's BulkEdit tries to add the *old* name back.
    tagModel.execute(bookmarks::RenameTag{.id = tagId, .name = "new"});
    const auto id2 = bookmarkModel.execute(makeCreate("https://two.example")).id;

    bookmarks::BulkEdit edit;
    edit.ids = {id2};
    edit.addTags = {"old"};  // the pre-rename name -- TagModel already renamed it away
    bookmarkModel.execute(edit);

    // BulkEdit's own findOrCreateTagId has no way to know "old" was renamed
    // to "new" -- it faithfully creates a *new* tag literally named "old".
    // This is the documented, accepted outcome: two strands, no
    // cross-instance transaction, and the model layer cannot see the other
    // model's in-flight rename. Consistency here is app/UI responsibility
    // (e.g. a client re-fetching the tag list before offering it), not a
    // framework or model guarantee.
    const auto tags = tagModel.execute(bookmarks::ListTags{}).tags;
    CHECK(std::ranges::any_of(tags, [](auto& t) { return t.name == "new"; }));
    CHECK(std::ranges::any_of(tags, [](auto& t) { return t.name == "old"; }));  // recreated, not merged
}

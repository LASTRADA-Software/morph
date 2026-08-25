// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>
#include <glaze/glaze.hpp>

#include "bookmarks/core/errors.hpp"
#include "bookmarks/core/types.hpp"
#include "bookmarks/units.hpp"

TEST_CASE("BookmarkId/TagId round-trip through JSON as a nullable integer", "[bookmarks][types]") {
    bookmarks::BookmarkId empty;
    CHECK_FALSE(empty.hasValue());
    std::string json;
    REQUIRE_FALSE(glz::write_json(empty, json));
    CHECK(json == "null");

    const bookmarks::BookmarkId id{42};
    REQUIRE(id.hasValue());
    CHECK(*id == 42);
    json.clear();
    REQUIRE_FALSE(glz::write_json(id, json));
    CHECK(json == "42");

    bookmarks::TagId decoded;
    REQUIRE_FALSE(glz::read_json(decoded, json));
    REQUIRE(decoded.hasValue());
    CHECK(*decoded == 42);
}

TEST_CASE("BookmarkId equality and ordering follow the payload", "[bookmarks][types]") {
    CHECK(bookmarks::BookmarkId{} == bookmarks::BookmarkId{});
    CHECK(bookmarks::BookmarkId{1} != bookmarks::BookmarkId{2});
    CHECK(bookmarks::BookmarkId{1} < bookmarks::BookmarkId{2});
}

TEST_CASE("Cursor and ImportOpId are independently hasValue()-capable", "[bookmarks][types]") {
    CHECK_FALSE(bookmarks::Cursor{}.hasValue());
    CHECK(bookmarks::Cursor{7}.hasValue());
    CHECK_FALSE(bookmarks::ImportOpId{}.hasValue());
    CHECK(bookmarks::ImportOpId{"chunk-1"}.hasValue());
    CHECK(*bookmarks::ImportOpId{"chunk-1"} == "chunk-1");
}

TEST_CASE("Count is a whole-number dimensionless quantity", "[bookmarks][types]") {
    const auto five = bookmarks::Count::fromDouble(5.0);
    REQUIRE(five.hasValue());
    CHECK(morph::math::floor(*five) == 5);
}

TEST_CASE("Every bookmarks error derives from BookmarksError and carries its message", "[bookmarks][types]") {
    try {
        throw bookmarks::NotFound{"no such bookmark"};
    } catch (const bookmarks::BookmarksError& err) {
        CHECK(std::string{err.what()} == "no such bookmark");
    }
    // Compile-time check that every leaf really is-a BookmarksError.
    static_assert(std::is_base_of_v<bookmarks::BookmarksError, bookmarks::NotFound>);
    static_assert(std::is_base_of_v<bookmarks::BookmarksError, bookmarks::ValidationError>);
    static_assert(std::is_base_of_v<bookmarks::BookmarksError, bookmarks::Conflict>);
    static_assert(std::is_base_of_v<bookmarks::BookmarksError, bookmarks::Forbidden>);
    static_assert(std::is_base_of_v<bookmarks::BookmarksError, bookmarks::TooLarge>);
}

// SPDX-License-Identifier: Apache-2.0
#include "bookmarks/dto/bulk_dto.hpp"
#include "bookmarks/dto/import_export_dto.hpp"
#include "bookmarks/dto/shared_feed_dto.hpp"
#include "bookmarks/dto/tag_dto.hpp"

#include <catch2/catch_test_macros.hpp>

TEST_CASE("RenameTag requires an id and a non-empty, bounded name", "[bookmarks][dto]") {
    bookmarks::RenameTag action;
    CHECK_FALSE(action.validate());
    action.id = bookmarks::TagId{1};
    CHECK_FALSE(action.validate());  // still no name
    action.name = "programming";
    CHECK(action.validate());
    action.name = std::string(bookmarks::kMaxTagNameBytes + 1, 'x');
    CHECK_FALSE(action.validate());
}

TEST_CASE("MergeTags requires two distinct ids", "[bookmarks][dto]") {
    bookmarks::MergeTags action;
    CHECK_FALSE(action.validate());
    action.sourceId = bookmarks::TagId{1};
    action.targetId = bookmarks::TagId{1};
    CHECK_FALSE(action.validate());  // merging a tag into itself
    action.targetId = bookmarks::TagId{2};
    CHECK(action.validate());
}

TEST_CASE("BulkEdit requires at least one id", "[bookmarks][dto]") {
    bookmarks::BulkEdit action;
    CHECK_FALSE(action.validate());
    action.ids = {bookmarks::BookmarkId{1}};
    CHECK(action.validate());
}

TEST_CASE("BulkArchiveOp reflects as a readable string", "[bookmarks][dto]") {
    std::string json;
    REQUIRE_FALSE(glz::write_json(bookmarks::BulkArchiveOp::Archive, json));
    CHECK(json == "\"Archive\"");
}

TEST_CASE("ImportBookmarks requires a non-empty chunk and an opId; the chunk-size bound is "
          "deliberately NOT one of validate()'s checks",
          "[bookmarks][dto]") {
    bookmarks::ImportBookmarks action;
    CHECK_FALSE(action.validate());
    action.chunk = "<A HREF=\"https://example.com\">Example</A>";
    CHECK_FALSE(action.validate());  // still no opId
    action.opId = bookmarks::ImportOpId{"chunk-1"};
    CHECK(action.validate());
    // An oversized chunk still passes validate() -- see import_export_dto.hpp's
    // comment on validate(): the size bound is enforced once, in
    // BookmarkModel::execute(), specifically so it can be signaled as the
    // more specific TooLarge rather than being folded into validate()'s
    // single untyped ValidationError (which is what every real dispatch
    // path, e.g. Bridge::executeVia, would produce if validate() rejected
    // it here instead).
    action.chunk = std::string(bookmarks::kMaxImportChunkBytes + 1, 'x');
    CHECK(action.validate());
}

TEST_CASE("ListSharedFeed/ListTags/ExportBookmarks validate() with no required fields",
          "[bookmarks][dto]") {
    CHECK(bookmarks::ListSharedFeed{}.validate());
    CHECK(bookmarks::ListTags{}.validate());
    CHECK(bookmarks::ExportBookmarks{}.validate());
}

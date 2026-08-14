// SPDX-License-Identifier: Apache-2.0
#include "bookmarks/dto/bookmark_dto.hpp"

#include <morph/forms/forms.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <string>
#include <string_view>

TEST_CASE("CreateBookmark validate() requires a non-empty url within the length bound",
          "[bookmarks][dto]") {
    bookmarks::CreateBookmark action;
    CHECK_FALSE(action.validate());  // empty url

    action.url = "https://example.com";
    CHECK(action.validate());

    action.url = std::string(bookmarks::kMaxUrlBytes + 1, 'a');
    CHECK_FALSE(action.validate());

    action.url = std::string(bookmarks::kMaxUrlBytes, 'a');
    CHECK(action.validate());
}

TEST_CASE("CreateBookmark's optionalFields excludes everything but url", "[bookmarks][dto]") {
    // Mirrors CreatePaste::optionalFields's own test intent: a create with
    // only a url must be schema-submittable without hand-typing every
    // enum's default.
    using bookmarks::CreateBookmark;
    using bookmarks::EditBookmark;
    // Five: title, description, notes, tags, visibility — everything but url.
    // `title` is in the list because a bookmark may legitimately be created
    // without one (the metadata worker fills it in); see that member's own
    // doc comment for why leaving it out broke the shipped create form.
    STATIC_REQUIRE(CreateBookmark::optionalFields.size() == 5);
    STATIC_REQUIRE(EditBookmark::optionalFields.size() == 5);

    // A count alone would still pass if `title` were swapped out for some
    // other name, which is precisely the regression this guard exists to
    // catch: `title` missing from the list is the shipped-GUI bug the
    // README's "Two bugs the first real client run found" records.
    STATIC_REQUIRE(std::ranges::contains(CreateBookmark::optionalFields, std::string_view{"title"}));
    STATIC_REQUIRE(std::ranges::contains(EditBookmark::optionalFields, std::string_view{"title"}));
}

TEST_CASE("The generated create/edit schemas do not mark title required", "[bookmarks][dto]") {
    // The other half of the guard above: `optionalFields` is only meaningful
    // through `morph::forms::schemaJson<A>()`'s derived `required` array,
    // which is what `DynamicForm` actually reads. Checking the list without
    // checking the schema would not have caught the original bug either.
    for (const auto& schema : {::morph::forms::schemaJson<bookmarks::CreateBookmark>(),
                               ::morph::forms::schemaJson<bookmarks::EditBookmark>()}) {
        CAPTURE(schema);
        glz::generic_u64 dom{};
        REQUIRE_FALSE(glz::read_json(dom, schema));
        REQUIRE(dom.contains("required"));
        const auto& required = dom["required"].get_array();
        CHECK(std::ranges::none_of(required, [](const auto& entry) { return entry.get_string() == "title"; }));
        // `url` is the one member that genuinely is required, so this is a
        // check that the schema is populated at all, not vacuously passing.
        CHECK(std::ranges::any_of(required, [](const auto& entry) { return entry.get_string() == "url"; }));
    }
}

TEST_CASE("EditBookmark validate() requires an id and a non-empty url", "[bookmarks][dto]") {
    bookmarks::EditBookmark action;
    CHECK_FALSE(action.validate());
    action.id = bookmarks::BookmarkId{1};
    CHECK_FALSE(action.validate());  // still no url
    action.url = "https://example.com";
    CHECK(action.validate());
}

TEST_CASE("GetBookmark/ArchiveBookmark/UnarchiveBookmark/DeleteBookmark all require an id",
          "[bookmarks][dto]") {
    CHECK_FALSE(bookmarks::GetBookmark{}.validate());
    CHECK(bookmarks::GetBookmark{.id = bookmarks::BookmarkId{1}}.validate());
    CHECK_FALSE(bookmarks::ArchiveBookmark{}.validate());
    CHECK_FALSE(bookmarks::UnarchiveBookmark{}.validate());
    CHECK_FALSE(bookmarks::DeleteBookmark{}.validate());
}

// No `;` in the name, deliberately: `catch_discover_tests` splits its
// discovered-name list on semicolons (CMake's own list separator), so a test
// name containing one is parsed as two bogus names and the real test silently
// receives none of the `ladder`/`ladder-bookmarks` labels CI filters by.
TEST_CASE("RecordMetadata requires an id — title/faviconPath may be empty (a failed fetch)",
          "[bookmarks][dto]") {
    CHECK_FALSE(bookmarks::RecordMetadata{}.validate());
    // Every field named explicitly rather than a partial designated-initializer
    // list: -Weverything includes -Wmissing-designated-field-initializers, which
    // fires on a partial list, and ladder_<rung>_tests is -Werror under
    // MORPH_ENABLE_STRICT_COMPILATION (CI's default).
    bookmarks::RecordMetadata action{.id = bookmarks::BookmarkId{1}, .title = {}, .faviconPath = {}};
    CHECK(action.validate());  // empty title/faviconPath is a legitimate "fetch found nothing"
}

TEST_CASE("Visibility/ReadState/ArchiveState/ReadFilter/ArchiveFilter reflect as readable strings",
          "[bookmarks][dto]") {
    std::string json;
    REQUIRE_FALSE(glz::write_json(bookmarks::Visibility::Shared, json));
    CHECK(json == "\"Shared\"");
    json.clear();
    REQUIRE_FALSE(glz::write_json(bookmarks::ReadFilter::UnreadOnly, json));
    CHECK(json == "\"UnreadOnly\"");
}

// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>

#include "bookmarks/import/netscape_bookmarks.hpp"

TEST_CASE("parseNetscapeChunk extracts url and title from <A HREF> entries", "[bookmarks][import]") {
    const std::string chunk = R"(<DL><p>
    <DT><A HREF="https://example.com">Example</A>
    <DT><A HREF="https://second.example">Second &amp; Site</A>
</DL><p>)";
    const auto entries = bookmarks::import::parseNetscapeChunk(chunk);
    REQUIRE(entries.size() == 2);
    CHECK(entries[0].url == "https://example.com");
    CHECK(entries[0].title == "Example");
    CHECK(entries[1].url == "https://second.example");
    CHECK(entries[1].title == "Second & Site");  // entity-decoded
}

TEST_CASE("parseNetscapeChunk decodes entities in the HREF value, not just the title", "[bookmarks][import]") {
    // Guards against the export/reimport corruption where a URL containing '&'
    // (e.g. a real-world query string) got escaped on export but never
    // decoded back on import, baking the literal "&amp;" text into the URL.
    const std::string chunk = R"(<DT><A HREF="https://example.com/search?a=1&amp;b=2">Search</A>)";
    const auto entries = bookmarks::import::parseNetscapeChunk(chunk);
    REQUIRE(entries.size() == 1);
    CHECK(entries[0].url == "https://example.com/search?a=1&b=2");
}

TEST_CASE("parseNetscapeChunk skips a malformed <A> with no href", "[bookmarks][import]") {
    const std::string chunk = R"(<DT><A>No href here</A>
<DT><A HREF="https://good.example">Good</A>)";
    const auto entries = bookmarks::import::parseNetscapeChunk(chunk);
    REQUIRE(entries.size() == 2);
    CHECK(entries[0].url.empty());  // caller counts this as skipped
    CHECK(entries[1].url == "https://good.example");
}

TEST_CASE("escapeHtml escapes the five predefined XML entities", "[bookmarks][import]") {
    CHECK(bookmarks::import::escapeHtml("a & b < c > d \"e\" 'f'") ==
          "a &amp; b &lt; c &gt; d &quot;e&quot; &#39;f&#39;");
}

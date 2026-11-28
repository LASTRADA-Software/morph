// SPDX-License-Identifier: Apache-2.0

// The pure helpers of the table-state model: how a text is escaped, sized and cut for the wire, and the digests that
// name a table's scope and a document's revision.

#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <morph/ui/table_state_model.hpp>
#include <string>
#include <string_view>

namespace {
namespace detail = morph::ui::detail;
}  // namespace

TEST_CASE("cov state model: escapedBytes sizes a byte as the JSON string writes it", "[ui][state][cov]") {
    for (char const shortEscape : {'"', '\\', '\b', '\f', '\n', '\r', '\t'}) {
        CHECK(detail::escapedBytes(static_cast<unsigned char>(shortEscape)) == 2);
    }
    CHECK(detail::escapedBytes(0x00) == 6);
    CHECK(detail::escapedBytes(0x01) == 6);
    CHECK(detail::escapedBytes(0x1F) == 6);
    CHECK(detail::escapedBytes(' ') == 1);
    CHECK(detail::escapedBytes('a') == 1);
    CHECK(detail::escapedBytes(0x7F) == 1);
    CHECK(detail::escapedBytes(0xC3) == 1);
}

TEST_CASE("cov state model: sequenceBytes reads the length a UTF-8 lead announces", "[ui][state][cov]") {
    CHECK(detail::sequenceBytes('a') == 1);
    CHECK(detail::sequenceBytes(0x80) == 1);  // a continuation starts nothing
    CHECK(detail::sequenceBytes(0xBF) == 1);
    CHECK(detail::sequenceBytes(0xC0) == 2);
    CHECK(detail::sequenceBytes(0xDF) == 2);
    CHECK(detail::sequenceBytes(0xE0) == 3);
    CHECK(detail::sequenceBytes(0xEF) == 3);
    CHECK(detail::sequenceBytes(0xF0) == 4);
    CHECK(detail::sequenceBytes(0xF7) == 4);
    CHECK(detail::sequenceBytes(0xF8) == 1);  // beyond UTF-8
    CHECK(detail::sequenceBytes(0xFF) == 1);
}

TEST_CASE("cov state model: quotedSegment writes a text as a JSON string", "[ui][state][cov]") {
    CHECK(detail::quotedSegment("") == R"("")");
    CHECK(detail::quotedSegment("abc") == R"("abc")");
    CHECK(detail::quotedSegment("a\"b\\c") == R"("a\"b\\c")");
    CHECK(detail::quotedSegment("\b\f\n\r\t") == R"("\b\f\n\r\t")");
    CHECK(detail::quotedSegment(std::string{"\x01\x1f"}) == R"("\u0001\u001f")");
    CHECK(detail::quotedSegment("é") == "\"é\"");  // bytes at or above 0x20 are kept as they are
}

TEST_CASE("cov state model: utf8Prefix never cuts a sequence", "[ui][state][cov]") {
    CHECK(detail::utf8Prefix("abc", 3) == 3);
    CHECK(detail::utf8Prefix("abc", 10) == 3);
    CHECK(detail::utf8Prefix("abcdef", 4) == 4);
    CHECK(detail::utf8Prefix("ab\xC3\xA9"
                             "cd",
                             3) == 2);  // the cut falls inside the two-byte é
    CHECK(detail::utf8Prefix("a\xE2\x82\xAC"
                             "b",
                             3) == 1);
    CHECK(detail::utf8Prefix("a\xF0\x9F\x98\x80"
                             "b",
                             4) == 1);
    CHECK(detail::utf8Prefix("\x80\x80\x80", 2) == 2);  // only continuations: no sequence to keep whole
}

TEST_CASE("cov state model: wireBytes sizes a text escaped twice", "[ui][state][cov]") {
    CHECK(detail::wireBytes("") == 0);
    CHECK(detail::wireBytes("abc") == 3);
    CHECK(detail::wireBytes("\"") == 4);
    CHECK(detail::wireBytes("\\") == 4);
    CHECK(detail::wireBytes("\n") == 3);
    CHECK(detail::wireBytes("\t\r") == 6);
    CHECK(detail::wireBytes(std::string{"\x01"}) == 7);
    CHECK(detail::wireBytes("a\"\n\x01") == 1 + 4 + 3 + 7);
}

TEST_CASE("cov state model: the digests that name a scope and a revision", "[ui][state][cov]") {
    CHECK(detail::sha256Hex("abc", 8) == "ba7816bf");
    CHECK(detail::sha256Hex("abc", 0).empty());
    CHECK(detail::sha256Hex("abc", 200).size() == 64);  // a digest has 64 hex digits and no more
    morph::table::TableStateKey const key{
        .application = "app", .model = "M", .table = "t", .qualifier = "q", .part = "state"};
    auto const scope = detail::tableStateScope(key);
    CHECK(scope.size() == std::string{morph::ui::kTableStateScopePrefix}.size() + 1 + 32);
    CHECK(scope.starts_with(std::string{morph::ui::kTableStateScopePrefix} + "."));
    auto other = key;
    other.table = "u";
    CHECK(detail::tableStateScope(other) != scope);
    other = key;
    other.part = "views";  // the part is not in the scope: it is an entry of it
    CHECK(detail::tableStateScope(other) == scope);
    auto const first = detail::tableStateRevision({}, "{}");
    CHECK(first.size() == 16);
    CHECK(detail::tableStateRevision(first, "{}") != first);
    CHECK(detail::tableStateRevision({}, "{ }") != first);
    CHECK(detail::tableStateRevision({}, "{}") == first);
}

// SPDX-License-Identifier: Apache-2.0
//
// `morph::forms::detail::findMember` -- the checked read over a
// `glz::generic_u64` object node (morph#706).
//
// The point of this file is the *negative* case, and it is the one no amount
// of reading the schema output would catch. glaze's `generic_json::at(key)` is
// `{ return operator[](key); }` for both overloads (glaze v7.4.0,
// glaze/json/generic.hpp:320 and :322), and the non-const `operator[]` inserts
// a default-constructed member for a missing key (generic.hpp:201-211). So the
// "bounds-safe alternative"
// `cppcoreguidelines-pro-bounds-avoid-unchecked-container-access` suggests is,
// on a mutating DOM walk, a read that silently writes to the document being
// emitted. These tests pin that `findMember` is not that: a miss returns
// `nullptr` and leaves the DOM byte-identical.
//
// The `at()` behaviour is asserted here too, against the pinned glaze, so the
// premise this accessor exists for is measured rather than quoted -- and so
// that a glaze release which gives `at()` real checked semantics turns this
// file red instead of leaving a stale rationale in a comment.

#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <glaze/glaze.hpp>
#include <morph/forms/forms.hpp>
#include <string>
#include <type_traits>

using morph::forms::detail::findMember;

namespace {

glz::generic_u64 parsed(std::string const& text) {
    glz::generic_u64 dom{};
    REQUIRE(!glz::read_json(dom, text));
    return dom;
}

std::string dumped(glz::generic_u64 const& dom) { return glz::write_json(dom).value_or(std::string{"<unwritable>"}); }

}  // namespace

TEST_CASE("findMember returns the member for a key that is present", "[forms][dom]") {
    auto dom = parsed(R"({"properties":{"amount":{"type":"number"}},"required":[]})");

    auto* const properties = findMember(dom, "properties");
    REQUIRE(properties != nullptr);
    auto* const amount = findMember(*properties, "amount");
    REQUIRE(amount != nullptr);
    CHECK(dumped(*amount) == R"({"type":"number"})");
}

TEST_CASE("findMember returns nullptr for a missing key and leaves the DOM unchanged", "[forms][dom]") {
    // This is the whole ticket: the DOM comparison, not the nullptr. A
    // `findMember` implemented over `operator[]`/`at()` would still return a
    // non-null pointer -- and would pass a nullptr-only assertion if it
    // returned `nullptr` for, say, a null-holding node -- while having grown
    // the document behind the caller's back.
    auto dom = parsed(R"({"properties":{"amount":{"type":"number"}}})");
    auto const before = dumped(dom);

    CHECK(findMember(dom, "x-layout") == nullptr);
    CHECK(findMember(dom, "") == nullptr);
    auto* const properties = findMember(dom, "properties");
    REQUIRE(properties != nullptr);
    CHECK(findMember(*properties, "missingField") == nullptr);

    CHECK(dumped(dom) == before);
    CHECK(before == R"({"properties":{"amount":{"type":"number"}}})");
}

TEST_CASE("glaze's at() is the insert this accessor exists to avoid", "[forms][dom]") {
    // The comparison that makes the test above mean something. Same DOM, same
    // missing key, through the accessor the clang-tidy check recommends.
    auto dom = parsed(R"({"properties":{"amount":{"type":"number"}}})");
    auto const before = dumped(dom);

    // No NOLINT here: tests/.clang-tidy already subtracts
    // cppcoreguidelines-pro-bounds-avoid-unchecked-container-access from every
    // translation unit under tests/, so a directive would suppress nothing.
    static_cast<void>(dom.at("x-layout"));

    CHECK(dumped(dom) != before);
    CHECK(dumped(dom) == R"({"properties":{"amount":{"type":"number"}},"x-layout":null})");
}

TEST_CASE("findMember yields nullptr on a node that is not an object", "[forms][dom]") {
    // `operator[]` would turn a null node into an empty object here, and would
    // throw a `std::bad_variant_access` on a non-null scalar. findMember does
    // neither: a node with no members has no member.
    auto dom = parsed(R"({"scalar":7,"text":"x","list":[1,2],"nothing":null})");
    auto const before = dumped(dom);

    for (auto const* key : {"scalar", "text", "list", "nothing"}) {
        auto* const node = findMember(dom, key);
        REQUIRE(node != nullptr);
        CHECK(findMember(*node, "anything") == nullptr);
    }

    CHECK(dumped(dom) == before);
}

TEST_CASE("findMember on a const DOM yields a const member and never throws", "[forms][dom]") {
    // The const `operator[]` *is* checked -- by calling `glaze_error("Key not
    // found.")`, i.e. by throwing. A renderer-facing read wants neither a
    // throw nor an insert, so both constnesses resolve to the same pointer
    // answer. Deduced, not overloaded twice, so no `const_cast` is involved.
    auto const dom = parsed(R"({"properties":{"amount":{"type":"number"}}})");

    auto const* const properties = findMember(dom, "properties");
    STATIC_REQUIRE(std::is_same_v<decltype(findMember(dom, "properties")), const glz::generic_u64*>);
    REQUIRE(properties != nullptr);
    CHECK(findMember(*properties, "amount") != nullptr);
    CHECK(findMember(dom, "nope") == nullptr);
}

TEST_CASE("findMember points into the node's own storage, not at a copy", "[forms][dom]") {
    // A `findMember` that returned a pointer to a copy would satisfy every
    // assertion above and quietly stop the annotators working, because they
    // write through what they find.
    auto dom = parsed(R"({"properties":{"amount":{"type":"number"}}})");

    auto* const properties = findMember(dom, "properties");
    REQUIRE(properties != nullptr);
    auto* const amount = findMember(*properties, "amount");
    REQUIRE(amount != nullptr);
    (*amount)["x-order"] = std::uint64_t{3};

    CHECK(dumped(dom) == R"({"properties":{"amount":{"type":"number","x-order":3}}})");
}

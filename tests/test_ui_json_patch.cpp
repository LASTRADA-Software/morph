// SPDX-License-Identifier: Apache-2.0
//
// JSON Patch apply and structural diff (docs/spec/ui/document.md, "JSON Patch and structural diff").

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <compare>
#include <cstddef>
#include <glaze/glaze.hpp>
#include <morph/ui/json_patch.hpp>
#include <morph/ui/value.hpp>
#include <string>
#include <string_view>
#include <type_traits>
#include <variant>
#include <vector>

namespace {

using morph::ui::Json;

/// The JSON @p text as a value; fails the test when it is not JSON.
Json json(std::string_view text) {
    auto parsed = morph::ui::parseJson(text);
    REQUIRE(parsed.has_value());
    return *parsed;
}

std::string written(Json const& value) { return glz::write_json(value).value_or("<unwritable>"); }

/// Whether the two values are the same document: objects match member by member whatever their order.
// NOLINTBEGIN(misc-no-recursion): a document is a tree.
bool same(Json const& left, Json const& right) {
    if (left.data.index() != right.data.index()) {
        return false;
    }
    if (auto const* const leftObject = std::get_if<Json::object_t>(&left.data)) {
        auto const& rightObject = std::get<Json::object_t>(right.data);
        return leftObject->size() == rightObject.size() && std::ranges::all_of(*leftObject, [&](auto const& entry) {
                   auto const found = rightObject.find(entry.first);
                   return found != rightObject.end() && same(entry.second, found->second);
               });
    }
    if (auto const* const leftArray = std::get_if<Json::array_t>(&left.data)) {
        auto const& rightArray = std::get<Json::array_t>(right.data);
        return std::ranges::equal(*leftArray, rightArray,
                                  [](Json const& one, Json const& two) { return same(one, two); });
    }
    // A scalar: the alternatives are the same, so each side holds the type @p one has. Compared by ordering, which
    // also keeps a floating-point number out of an equality test.
    return std::visit(
        [&](auto const& one) {
            using Held = std::decay_t<decltype(one)>;
            if constexpr (std::is_same_v<Held, Json::object_t> || std::is_same_v<Held, Json::array_t>) {
                return false;
            } else if constexpr (std::is_null_pointer_v<Held>) {
                return true;
            } else {
                return std::is_eq(one <=> std::get<Held>(right.data));
            }
        },
        left.data);
}
// NOLINTEND(misc-no-recursion)

struct Pair {
    std::string_view before;
    std::string_view after;
};

}  // namespace

// Mutation: report an array's trailing removals first-to-last, or its trailing additions out of order.
TEST_CASE("a_diff_applied_to_its_before_gives_its_after", "[ui][json_patch]") {
    std::vector<Pair> const cases = {
        {.before = R"({"a":1})", .after = R"({"a":1})"},
        {.before = R"({"a":1})", .after = R"({"a":2})"},
        {.before = R"({"a":1})", .after = R"({"b":1})"},
        {.before = R"({"a":{"b":{"c":1,"d":2}}})", .after = R"({"a":{"b":{"c":1,"d":3,"e":[1]}}})"},
        {.before = R"({"a":{"b":1}})", .after = R"({"a":{}})"},
        {.before = R"({"a":[1,2,3]})", .after = R"({"a":[1,2,3,4,5]})"},
        {.before = R"({"a":[1,2,3,4,5]})", .after = R"({"a":[1,2]})"},
        {.before = R"({"a":[1,2,3]})", .after = R"({"a":[]})"},
        {.before = R"({"a":[]})", .after = R"({"a":[{"x":1},{"y":2}]})"},
        {.before = R"([{"k":1},{"k":2},{"k":3}])", .after = R"([{"k":1},{"k":9}])"},
        {.before = R"({"a":[[1,2],[3]]})", .after = R"({"a":[[1],[3,4],[5]]})"},
        {.before = R"({"a":1})", .after = R"({"a":"1"})"},
        {.before = R"({"a":{"b":1}})", .after = R"({"a":[1]})"},
        {.before = R"({"a":[1]})", .after = R"({"a":{"b":1}})"},
        {.before = R"({"a":null})", .after = R"({"a":false})"},
        {.before = R"({"a/b":1,"c~d":2,"~1":3,"~0/":4})", .after = R"({"a/b":5,"c~d":6,"~1":7,"~0/":8})"},
        {.before = R"({"a/b":{"c~d":[1]}})", .after = R"({"a/b":{"c~d":[1,2],"e/f":1}})"},
        {.before = R"({"":1})", .after = R"({"":2})"},
        {.before = R"({"id":"s","view":{"children":[{"kind":"a"},{"kind":"b"}]}})",
         .after = R"({"id":"s","view":{"children":[{"kind":"b"}]}})"},
        {.before = R"({"a":1})", .after = R"([1])"},
        {.before = R"(1)", .after = R"("x")"},
        {.before = R"(null)", .after = R"({"a":[]})"},
        {.before = R"([])", .after = R"([])"},
    };
    for (Pair const& pair : cases) {
        INFO(pair.before << " -> " << pair.after);
        Json const before = json(pair.before);
        Json const after = json(pair.after);
        Json const patch = morph::ui::diffJson(before, after);
        INFO(written(patch));
        auto const applied = morph::ui::applyJsonPatch(before, patch);
        REQUIRE(applied.has_value());
        CHECK(same(*applied, after));
    }
}

// Mutation: report something for equal documents.
TEST_CASE("equal_documents_diff_to_an_empty_patch", "[ui][json_patch]") {
    Json const document = json(R"({"a":[1,{"b":null}],"c":"x"})");
    CHECK(written(morph::ui::diffJson(document, document)) == "[]");
}

// Mutation: skip the `~` escape, escape `/` before `~`, or leave either out.
TEST_CASE("a_diff_escapes_the_tokens_of_its_pointers", "[ui][json_patch]") {
    CHECK(written(morph::ui::diffJson(json(R"({"a/b":1})"), json(R"({"a/b":2})"))) ==
          R"([{"op":"replace","path":"/a~1b","value":2}])");
    CHECK(written(morph::ui::diffJson(json(R"({"c~d":1})"), json(R"({"c~d":2})"))) ==
          R"([{"op":"replace","path":"/c~0d","value":2}])");
    // `~1` as a member name is the two characters ~ and 1, so it is written ~01 and never ~1.
    CHECK(written(morph::ui::diffJson(json(R"({"~1":1})"), json(R"({"~1":2})"))) ==
          R"([{"op":"replace","path":"/~01","value":2}])");
    CHECK(written(morph::ui::diffJson(json(R"({})"), json(R"({"x/~":1})"))) ==
          R"([{"op":"add","path":"/x~1~0","value":1}])");
    CHECK(written(morph::ui::diffJson(json(R"({"x/~":1})"), json(R"({})"))) == R"([{"op":"remove","path":"/x~1~0"}])");
}

// Mutation: remove the trailing elements first-to-last, or add them out of order.
TEST_CASE("a_diff_of_arrays_works_element_by_element_with_a_trailing_add_or_remove", "[ui][json_patch]") {
    CHECK(written(morph::ui::diffJson(json("[1,2,3]"), json("[1,9]"))) ==
          R"([{"op":"replace","path":"/1","value":9},{"op":"remove","path":"/2"}])");
    CHECK(written(morph::ui::diffJson(json("[1,2,3,4]"), json("[1]"))) ==
          R"([{"op":"remove","path":"/3"},{"op":"remove","path":"/2"},{"op":"remove","path":"/1"}])");
    CHECK(written(morph::ui::diffJson(json("[1]"), json("[1,2,3]"))) ==
          R"([{"op":"add","path":"/1","value":2},{"op":"add","path":"/2","value":3}])");
}

// Mutation: descend into a member that is an array on one side and an object on the other, or replace a whole parent
// for a change one level down.
TEST_CASE("a_diff_is_as_deep_as_the_change_and_replaces_a_value_of_another_kind", "[ui][json_patch]") {
    CHECK(written(morph::ui::diffJson(json(R"({"a":{"b":{"c":1}}})"), json(R"({"a":{"b":{"c":2}}})"))) ==
          R"([{"op":"replace","path":"/a/b/c","value":2}])");
    CHECK(written(morph::ui::diffJson(json(R"({"a":{"b":1}})"), json(R"({"a":[1]})"))) ==
          R"([{"op":"replace","path":"/a","value":[1]}])");
    CHECK(written(morph::ui::diffJson(json("1"), json("[]"))) == R"([{"op":"replace","path":"","value":[]}])");
}

// Mutation: apply the operations of a refused patch before the refusal, or drop an operation's number or name.
TEST_CASE("applying_a_patch_names_the_operation_that_cannot_apply_and_changes_nothing", "[ui][json_patch]") {
    Json const document = json(R"({"a":1})");
    struct Refusal {
        std::string_view patch;
        std::string_view expected;
    };
    std::vector<Refusal> const refusals = {
        {.patch = R"([{"op":"replace","path":"/a","value":2},{"op":"remove","path":"/missing"}])",
         .expected = "op 1 (remove /missing): the path does not exist in the document"},
        {.patch = R"([{"op":"test","path":"/a","value":2}])",
         .expected = "op 0 (test /a): the value at the path is not the one the test expects"},
        {.patch = R"([{"op":"add","path":"/x/y","value":1}])", .expected = "op 0 (add /x/y)"},
        {.patch = R"([5])", .expected = "op 0 is not a JSON object"},
        {.patch = R"([{"op":"explode","path":""}])", .expected = "op 0: `op` must be one of"},
        {.patch = R"([{"op":"replace"}])", .expected = "op 0: `path` must be a string"},
        {.patch = R"([{"op":"replace","path":"/a"}])", .expected = "op 0: `replace` needs a `value`"},
        {.patch = R"([{"op":"move","path":"/a"}])", .expected = "op 0: `move` needs a `from`"},
        {.patch = R"([{"op":"copy","path":"/a"}])", .expected = "op 0: `copy` needs a `from`"},
        {.patch = R"({"op":"remove","path":"/a"})", .expected = "a patch must be a JSON array"},
        {.patch = R"("[]")", .expected = "a patch must be a JSON array"},
    };
    for (Refusal const& refusal : refusals) {
        INFO(refusal.patch);
        auto const result = morph::ui::applyJsonPatch(document, json(refusal.patch));
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error().contains(refusal.expected));
    }
    CHECK(written(document) == R"({"a":1})");
}

// Mutation: refuse a patch that is an empty array, or apply only the first operation.
TEST_CASE("applying_operations_goes_in_order_and_an_empty_patch_changes_nothing", "[ui][json_patch]") {
    Json const document = json(R"({"a":[1,2],"b":{"c":1}})");
    auto const unchanged = morph::ui::applyJsonPatch(document, json("[]"));
    REQUIRE(unchanged.has_value());
    CHECK(same(*unchanged, document));
    auto const patched = morph::ui::applyJsonPatch(
        document, json(R"([{"op":"add","path":"/a/-","value":3},{"op":"copy","from":"/b/c","path":"/d"},)"
                       R"({"op":"move","from":"/b","path":"/e"},{"op":"test","path":"/d","value":1},)"
                       R"({"op":"replace","path":"/a/0","value":0},{"op":"remove","path":"/a/1"}])"));
    REQUIRE(patched.has_value());
    CHECK(same(*patched, json(R"({"a":[0,3],"d":1,"e":{"c":1}})")));
}

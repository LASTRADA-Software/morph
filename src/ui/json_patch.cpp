// SPDX-License-Identifier: Apache-2.0

#include <algorithm>
#include <array>
#include <compare>
#include <cstddef>
#include <expected>
#include <glaze/glaze.hpp>
#include <glaze/json/patch.hpp>
#include <morph/ui/json_patch.hpp>
#include <morph/ui/value.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace morph::ui {

namespace {

using Object = Json::object_t;
using Array = Json::array_t;
using Operation = glz::patch_op_t<Json>;

using PatchOpName = std::pair<std::string_view, glz::patch_op_type>;

constexpr std::array<PatchOpName, 6> kPatchOps{{{"add", glz::patch_op_type::add},
                                                {"remove", glz::patch_op_type::remove},
                                                {"replace", glz::patch_op_type::replace},
                                                {"move", glz::patch_op_type::move},
                                                {"copy", glz::patch_op_type::copy},
                                                {"test", glz::patch_op_type::test}}};

// The table lists the operations in the enumeration's order, so an operation's number is its place in it.
static_assert(kPatchOps.at(static_cast<std::size_t>(glz::patch_op_type::add)).second == glz::patch_op_type::add &&
              kPatchOps.at(static_cast<std::size_t>(glz::patch_op_type::remove)).second ==
                  glz::patch_op_type::remove &&
              kPatchOps.at(static_cast<std::size_t>(glz::patch_op_type::replace)).second ==
                  glz::patch_op_type::replace &&
              kPatchOps.at(static_cast<std::size_t>(glz::patch_op_type::move)).second == glz::patch_op_type::move &&
              kPatchOps.at(static_cast<std::size_t>(glz::patch_op_type::copy)).second == glz::patch_op_type::copy &&
              kPatchOps.at(static_cast<std::size_t>(glz::patch_op_type::test)).second == glz::patch_op_type::test);

// The entry of kPatchOps called @p name, or null.
[[nodiscard]] PatchOpName const* patchOpNamed(std::string_view name) {
    for (PatchOpName const& entry : kPatchOps) {
        if (entry.first == name) {
            return &entry;
        }
    }
    return nullptr;
}

[[nodiscard]] std::string_view nameOf(glz::patch_op_type kind) {
    return kPatchOps.at(static_cast<std::size_t>(kind)).first;
}

// The member @p key of @p object, or null. The iterator is written out rather than deduced: it is a pointer in glaze's
// map but not necessarily in other standard libraries' containers, and a deduced `auto const*` would claim it is one.
[[nodiscard]] Json const* memberOf(Object const& object, std::string_view key) {
    Object::const_iterator const found = object.find(key);
    return found == object.end() ? nullptr : &found->second;
}

// The operation @p entry of a patch, or why it is not one; @p where names it in the message.
[[nodiscard]] std::expected<Operation, std::string> operationOf(Json const& entry, std::string const& where) {
    auto const* const fields = std::get_if<Object>(&entry.data);
    if (fields == nullptr) {
        return std::unexpected(where + " is not a JSON object");
    }
    auto const stringField = [&](std::string_view key) -> std::optional<std::string> {
        Json const* const child = memberOf(*fields, key);
        auto const* const text = child == nullptr ? nullptr : std::get_if<std::string>(&child->data);
        return text == nullptr ? std::nullopt : std::optional<std::string>{*text};
    };
    auto const name = stringField("op");
    PatchOpName const* const kind = name ? patchOpNamed(*name) : nullptr;
    if (kind == nullptr) {
        return std::unexpected(where + ": `op` must be one of add, remove, replace, move, copy, test");
    }
    auto path = stringField("path");
    if (!path) {
        return std::unexpected(where + ": `path` must be a string, a JSON Pointer");
    }
    Operation op;
    op.op = kind->second;
    op.path = std::move(*path);
    op.from = stringField("from");
    bool const needsValue = kind->second == glz::patch_op_type::add || kind->second == glz::patch_op_type::replace ||
                            kind->second == glz::patch_op_type::test;
    bool const needsFrom = kind->second == glz::patch_op_type::move || kind->second == glz::patch_op_type::copy;
    if (Json const* const value = memberOf(*fields, "value"); value != nullptr) {
        op.value = *value;
    }
    if (needsValue && !op.value) {
        return std::unexpected(where + ": `" + std::string{kind->first} + "` needs a `value`");
    }
    if (needsFrom && !op.from) {
        return std::unexpected(where + ": `" + std::string{kind->first} + "` needs a `from`, a JSON Pointer");
    }
    return op;
}

[[nodiscard]] std::string_view whyFailed(glz::error_ctx const& error) {
    if (error.ec == glz::error_code::nonexistent_json_ptr) {
        return "the path does not exist in the document";
    }
    if (error.ec == glz::error_code::patch_test_failed) {
        return "the value at the path is not the one the test expects";
    }
    if (error.ec == glz::error_code::invalid_json_pointer) {
        return "the path is not a valid JSON Pointer";
    }
    return "the operation cannot be applied there";
}

// A reference token as it is written in a JSON Pointer: `~` first, so the `~` of `~1` is not escaped again.
[[nodiscard]] std::string escapedToken(std::string_view token) {
    std::string out;
    out.reserve(token.size());
    for (char const letter : token) {
        if (letter == '~') {
            out += "~0";
        } else if (letter == '/') {
            out += "~1";
        } else {
            out += letter;
        }
    }
    return out;
}

[[nodiscard]] Json operation(std::string_view name, std::string const& path, Json const* value) {
    Object fields{{"op", Json(std::string{name})}, {"path", Json(path)}};
    if (value != nullptr) {
        fields.emplace("value", *value);
    }
    Json made(std::move(fields));
    return made;
}

// NOLINTBEGIN(misc-no-recursion): a document is a tree, and both the comparison and the diff follow it down.

// Whether @p left and @p right are the same value. Members of an object are matched by name, not by position, so two
// objects that differ only in member order are equal; two numbers are equal when they have the same kind and value.
[[nodiscard]] bool sameJson(Json const& left, Json const& right) {
    if (left.data.index() != right.data.index()) {
        return false;
    }
    if (auto const* const leftObject = std::get_if<Object>(&left.data)) {
        auto const& rightObject = std::get<Object>(right.data);
        if (leftObject->size() != rightObject.size()) {
            return false;
        }
        return std::ranges::all_of(*leftObject, [&](auto const& entry) {
            Json const* const other = memberOf(rightObject, entry.first);
            return other != nullptr && sameJson(entry.second, *other);
        });
    }
    if (auto const* const leftArray = std::get_if<Array>(&left.data)) {
        auto const& rightArray = std::get<Array>(right.data);
        return std::ranges::equal(*leftArray, rightArray,
                                  [](Json const& one, Json const& two) { return sameJson(one, two); });
    }
    // A scalar: the alternatives are the same, so each side holds the type @p one has. Compared by ordering, which
    // also keeps a floating-point number out of an equality test.
    return std::visit(
        [&](auto const& one) {
            using Held = std::decay_t<decltype(one)>;
            if constexpr (std::is_same_v<Held, Object> || std::is_same_v<Held, Array>) {
                return false;
            } else if constexpr (std::is_null_pointer_v<Held>) {
                return true;
            } else {
                return std::is_eq(one <=> std::get<Held>(right.data));
            }
        },
        left.data);
}

void diffInto(Array& out, std::string const& path, Json const& before, Json const& after);

void diffObjects(Array& out, std::string const& path, Object const& before, Object const& after) {
    for (auto const& [key, old] : before) {
        std::string const child = path + "/" + escapedToken(key);
        if (Json const* const now = memberOf(after, key); now != nullptr) {
            diffInto(out, child, old, *now);
        } else {
            out.push_back(operation("remove", child, nullptr));
        }
    }
    for (auto const& [key, now] : after) {
        if (memberOf(before, key) == nullptr) {
            out.push_back(operation("add", path + "/" + escapedToken(key), &now));
        }
    }
}

void diffArrays(Array& out, std::string const& path, Array const& before, Array const& after) {
    std::size_t const common = std::min(before.size(), after.size());
    for (std::size_t index = 0; index < common; ++index) {
        diffInto(out, path + "/" + std::to_string(index), before.at(index), after.at(index));
    }
    // Removing from the end keeps every index still to be removed where it was.
    for (std::size_t index = before.size(); index > common; --index) {
        out.push_back(operation("remove", path + "/" + std::to_string(index - 1), nullptr));
    }
    for (std::size_t index = common; index < after.size(); ++index) {
        out.push_back(operation("add", path + "/" + std::to_string(index), &after.at(index)));
    }
}

void diffInto(Array& out, std::string const& path, Json const& before, Json const& after) {
    if (sameJson(before, after)) {
        return;
    }
    auto const* const oldObject = std::get_if<Object>(&before.data);
    auto const* const newObject = std::get_if<Object>(&after.data);
    if (oldObject != nullptr && newObject != nullptr) {
        diffObjects(out, path, *oldObject, *newObject);
        return;
    }
    auto const* const oldArray = std::get_if<Array>(&before.data);
    auto const* const newArray = std::get_if<Array>(&after.data);
    if (oldArray != nullptr && newArray != nullptr) {
        diffArrays(out, path, *oldArray, *newArray);
        return;
    }
    out.push_back(operation("replace", path, &after));
}

// NOLINTEND(misc-no-recursion)

}  // namespace

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters): a document and a patch are told apart by what they are named.
std::expected<Json, std::string> applyJsonPatch(Json document, Json const& patch) {
    auto const* const entries = std::get_if<Array>(&patch.data);
    if (entries == nullptr) {
        return std::unexpected(std::string{"a patch must be a JSON array of operations"});
    }
    std::vector<Operation> ops;
    ops.reserve(entries->size());
    std::size_t index = 0;
    for (Json const& entry : *entries) {
        auto op = operationOf(entry, "op " + std::to_string(index++));
        if (!op.has_value()) {
            return std::unexpected(op.error());
        }
        ops.push_back(std::move(*op));
    }
    // One operation at a time, so a refusal can name it.
    index = 0;
    for (Operation const& op : ops) {
        glz::error_ctx const error = glz::patch(document, std::vector<Operation>{op},
                                                glz::patch_opts{.create_intermediate = false, .atomic = false});
        if (error) {
            return std::unexpected("op " + std::to_string(index) + " (" + std::string{nameOf(op.op)} + " " + op.path +
                                   "): " + std::string{whyFailed(error)});
        }
        ++index;
    }
    return document;
}

Json diffJson(Json const& before, Json const& after) {
    Array operations;
    diffInto(operations, "", before, after);
    Json made(std::move(operations));
    return made;
}

}  // namespace morph::ui

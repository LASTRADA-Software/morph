// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <catch2/catch_test_macros.hpp>
#include <fstream>
#include <morph/ui/value.hpp>
#include <sstream>
#include <string>
#include <string_view>

/// @file
/// @brief Reading the shared JSON corpora under `tests/ui/corpus/` for the `[ui]` document tests.

namespace morph::ui::testing {

/// @brief Reads and parses one corpus file.
/// @param path The file's absolute path.
/// @return The parsed JSON tree; the test fails when the file is missing or not JSON.
inline Json loadCorpus(char const* path) {
    std::ifstream const file{path};
    REQUIRE(file.good());
    std::stringstream text;
    text << file.rdbuf();
    auto json = parseJson(text.str());
    if (!json) {
        FAIL(json.error());
    }
    return *json;
}

/// @brief An object member that must be present.
/// @param object The JSON object.
/// @param name   The member's name.
/// @return The member.
inline Json const& member(Json const& object, std::string_view name) {
    auto const& fields = std::get<Json::object_t>(object.data);
    auto const* const found = fields.find(name);
    REQUIRE(found != fields.end());
    return found->second;
}

/// @brief An object member that may be absent.
/// @param object The JSON object.
/// @param name   The member's name.
/// @return The member, or `nullptr`.
inline Json const* optionalMember(Json const& object, std::string_view name) {
    auto const& fields = std::get<Json::object_t>(object.data);
    auto const* const found = fields.find(name);
    return found == fields.end() ? nullptr : &found->second;
}

}  // namespace morph::ui::testing

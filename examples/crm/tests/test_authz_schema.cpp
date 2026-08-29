// SPDX-License-Identifier: Apache-2.0
//
// updateAccountSchemaJsonFor: per-caller schema shaping for the industry
// per-field restriction (README build order §5).

#include <catch2/catch_test_macros.hpp>
#include <glaze/glaze.hpp>
#include <string>

#include "crm/gui/crm_schemas.hpp"

TEST_CASE("updateAccountSchemaJsonFor marks industry read-only below Manager", "[crm][authz][schema]") {
    const std::string viewerSchema = crm::gui::updateAccountSchemaJsonFor(crm::Role::Viewer);
    glz::generic_u64 dom{};
    REQUIRE_FALSE(glz::read_json(dom, viewerSchema));
    CHECK(dom["properties"]["industry"]["x-readonly"].get<bool>() == true);

    const std::string memberSchema = crm::gui::updateAccountSchemaJsonFor(crm::Role::Member);
    glz::generic_u64 memberDom{};
    REQUIRE_FALSE(glz::read_json(memberDom, memberSchema));
    CHECK(memberDom["properties"]["industry"]["x-readonly"].get<bool>() == true);
}

TEST_CASE("updateAccountSchemaJsonFor leaves industry writable for Manager", "[crm][authz][schema]") {
    const std::string schema = crm::gui::updateAccountSchemaJsonFor(crm::Role::Manager);
    glz::generic_u64 dom{};
    REQUIRE_FALSE(glz::read_json(dom, schema));
    auto& industry = dom["properties"]["industry"];
    // Never marked read-only by this function for Manager — either the key
    // is absent (untouched, undecorated schema) or explicitly false; both
    // mean "not forced read-only", which is all this asserts.
    if (industry.contains("x-readonly")) {
        CHECK(industry["x-readonly"].get<bool>() == false);
    }
}

TEST_CASE("updateAccountSchemaJsonFor doesn't touch other fields", "[crm][authz][schema]") {
    const std::string schema = crm::gui::updateAccountSchemaJsonFor(crm::Role::Viewer);
    glz::generic_u64 dom{};
    REQUIRE_FALSE(glz::read_json(dom, schema));
    // name/website are untouched — no framework-wide "everything read-only"
    // side effect from decorating one field.
    auto& name = dom["properties"]["name"];
    if (name.contains("x-readonly")) {
        CHECK(name["x-readonly"].get<bool>() == false);
    }
}

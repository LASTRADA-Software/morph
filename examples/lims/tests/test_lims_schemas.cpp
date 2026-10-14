// SPDX-License-Identifier: Apache-2.0
//
// The schemas lims serves for its forms: the `{actionType: schema}` document the server's registry answers a
// `schemas` request with, which a client builds every `form` node from. Every effectful form opts out of
// auto-submit, and a read-only action does not, so the check is not satisfied by a schema stamping every action.

#include <catch2/catch_test_macros.hpp>
#include <glaze/glaze.hpp>
#include <morph/core/registry.hpp>
#include <morph/forms/forms.hpp>
#include <string>

#include "lims/dto/sample_dto.hpp"
#include "lims/models/sample_model.hpp"

namespace {

/// The document the server answers for `SampleModel`, as a glaze DOM.
glz::generic_u64 servedSchemas() {
    std::string const document = morph::model::detail::defaultDispatcher().schemasJson("SampleModel");
    glz::generic_u64 dom{};
    REQUIRE_FALSE(glz::read_json(dom, document));
    return dom;
}

}  // namespace

// NOLINTBEGIN(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access): each key is checked before it is read.
TEST_CASE("lims serves a form schema for every effectful action, each opting out of auto-submit",
          "[lims][forms][schemas]") {
    glz::generic_u64 const dom = servedSchemas();
    for (char const* const actionType : {"RegisterClient", "RegisterSample", "RejectSample", "ReturnForRework",
                                         "CaptureConcentration", "ResolveConflict"}) {
        INFO("action type: " << actionType);
        REQUIRE(dom.contains(actionType));
        CHECK(dom[actionType].contains("properties"));
        REQUIRE(dom[actionType].contains("x-submitMode"));
        CHECK(dom[actionType]["x-submitMode"].get<std::string>() == "explicit");
    }
}

TEST_CASE("lims' capture form carries its cross-field rules", "[lims][forms][schemas]") {
    glz::generic_u64 const dom = servedSchemas();
    REQUIRE(dom.contains("CaptureConcentration"));
    REQUIRE(dom["CaptureConcentration"].contains("x-rules"));
    std::string const rules = glz::write_json(dom["CaptureConcentration"]["x-rules"]).value_or(std::string{});
    CHECK(rules.find("exactlyOneOf") != std::string::npos);
    CHECK(rules.find("requiredWhen") != std::string::npos);
    CHECK(rules.find("visibleWhen") != std::string::npos);
}
// NOLINTEND(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)

// `x-submitMode` is opt-in: a read-only query declares no `explicitSubmit`, so its schema carries no mode at all.
TEST_CASE("a read-only lims action's schema carries no submit mode", "[lims][forms][schemas]") {
    std::string const schema = morph::forms::schemaJson<lims::GetSample>();
    CHECK(schema.find("x-submitMode") == std::string::npos);
}

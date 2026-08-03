// SPDX-License-Identifier: Apache-2.0

// Regression test for issue #21: BRIDGE_REGISTER_MODEL / BRIDGE_REGISTER_ACTION built the name
// of their generated static registrar by token-pasting the model/action type onto a fixed
// prefix (`bridge_model_reg_##M`, `bridge_action_reg_##M##_##A`). That only produces a valid
// identifier when both arguments are bare identifiers -- a namespace-qualified type pastes ':'
// characters into the token and fails to compile. This file registers a namespace-qualified
// model and action exactly as the issue describes; it must compile (and the runtime dispatch
// below must succeed) for the fix to be verified.

#include <morph/core/bridge.hpp>
#include <morph/core/registry.hpp>

#include <catch2/catch_test_macros.hpp>
#include <string>
#include <string_view>

namespace issue21::models {

struct ReportCreateResult {
    bool ok = false;
};

struct ReportCreate {
    int id = 0;
};

class Report {
public:
    ReportCreateResult execute(const ReportCreate& action) { return ReportCreateResult{.ok = action.id > 0}; }
};

}  // namespace issue21::models

// Namespace-qualified model and action -- this is exactly the case that used to fail to
// compile (the generated identifiers `bridge_model_reg_issue21::models::Report` and
// `bridge_action_reg_issue21::models::Report_issue21::models::ReportCreate` are not valid
// tokens).
BRIDGE_REGISTER_MODEL(issue21::models::Report, "Issue21_Report")
BRIDGE_REGISTER_ACTION(issue21::models::Report, issue21::models::ReportCreate, "Issue21_Create")

TEST_CASE("BRIDGE_REGISTER_MODEL/BRIDGE_REGISTER_ACTION compile and register namespace-qualified types",
          "[registry][issue21]") {
    auto holder = morph::model::detail::ModelRegistryFactory::instance().create("Issue21_Report");
    REQUIRE(holder != nullptr);

    auto result = morph::model::detail::ActionDispatcher::instance().dispatch("Issue21_Report", "Issue21_Create",
                                                                               *holder, R"({"id":5})");
    REQUIRE(result == R"({"ok":true})");
}

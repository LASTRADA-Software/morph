// SPDX-License-Identifier: Apache-2.0

// Regression test for issue #21 (follow-up): keying the generated registrar variable name on
// `__LINE__` fixed the original namespace-qualified-type bug but introduced a new one --
// `__LINE__` is only unique within a single physical file, so two different headers that each
// invoke BRIDGE_REGISTER_MODEL/BRIDGE_REGISTER_ACTION on the same line number produce the same
// generated identifier once both are `#include`d into one translation unit. Since C++ unnamed
// namespaces are per-translation-unit (not per-file), that is a hard redefinition error -- this
// is exactly what broke the WASM demo build (multiple bank model headers, each registering on
// the same line number, all pulled into one autogen TU). issue21_same_line_a.hpp and
// issue21_same_line_b.hpp each invoke BRIDGE_REGISTER_MODEL on line 19; this file must compile
// (and both models must be independently registered) for the fix (`__COUNTER__` instead of
// `__LINE__`) to be verified.

#include "issue21_same_line_a.hpp"
#include "issue21_same_line_b.hpp"

#include <catch2/catch_test_macros.hpp>

TEST_CASE("BRIDGE_REGISTER_MODEL on the same line number in two different headers does not collide",
          "[registry][issue21]") {
    auto holderA = morph::model::detail::ModelRegistryFactory::instance().create("Issue21_SameLine_WidgetA");
    auto holderB = morph::model::detail::ModelRegistryFactory::instance().create("Issue21_SameLine_WidgetB");
    REQUIRE(holderA != nullptr);
    REQUIRE(holderB != nullptr);
}

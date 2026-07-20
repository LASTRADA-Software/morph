// SPDX-License-Identifier: Apache-2.0

#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <memory>
#include <morph/core/bridge.hpp>
#include <morph/core/executor.hpp>
#include <morph/core/registry.hpp>
#include <morph/core/remote.hpp>
#include <morph/forms/forms.hpp>
#include <morph/util/quantity.hpp>
#include <morph/util/rational.hpp>
#include <stdexcept>
#include <string>

#include "test_support.hpp"

using SyncExecutor = morph::testing::InlineExecutor;

// ─────────────────────────────────────────────────────────────────────────
// morph::model::ValidationError
// ─────────────────────────────────────────────────────────────────────────

TEST_CASE("morph::model::ValidationError carries model/action type ids in its message", "[registry][validation]") {
    morph::model::ValidationError const err{"MyModel", "MyAction"};
    REQUIRE(std::string{err.what()} == "action failed validation: MyModel/MyAction");
    REQUIRE(dynamic_cast<const std::runtime_error*>(&err) != nullptr);
}

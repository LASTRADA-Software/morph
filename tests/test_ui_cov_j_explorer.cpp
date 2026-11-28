// SPDX-License-Identifier: Apache-2.0

// A use through a borrowed model alias takes its model type from the catalog's facts for the action.

#include <catch2/catch_test_macros.hpp>
#include <functional>
#include <morph/ui/bundle.hpp>
#include <morph/ui/document.hpp>
#include <morph/ui/explorer.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "ui_interpreter_support.hpp"

namespace {

constexpr std::string_view kBorrowed = R"({
  "vocab": "ui/1", "id": "borrowed",
  "models": { "sample": { "type": "Lims_SampleModel", "instance": { "from": "outer" } } },
  "queries": { "list": { "model": "sample", "action": "Lims_ListSamples" } },
  "view": { "kind": "text", "text": "x" } })";

}  // namespace

TEST_CASE("covj explorer: a typeless alias takes its type from the catalog, or has none", "[covj][ui][explorer]") {
    namespace ui = morph::ui;
    ui::Catalog catalog = ui::testing::corpusCatalog();
    auto loaded = ui::loadDocumentText(kBorrowed, ui::LoadOptions{.catalog = catalog});
    REQUIRE(loaded.has_value());
    // The document holds no type for an alias that borrows; the loaded one is made so.
    loaded->scope.models.front().type.clear();

    SECTION("a catalog that knows the action") {
        catalog.action = [](std::string_view action) {
            return action == "Lims_ListSamples"
                       ? std::optional<ui::ActionInfo>{ui::ActionInfo{.modelType = "Lims_From"}}
                       : std::nullopt;
        };
        std::vector<ui::ActionUse> const uses = ui::actionUses(*loaded, catalog);
        REQUIRE(uses.size() == 1);
        CHECK(uses.front().modelType == "Lims_From");
    }
    SECTION("a catalog that does not know it") {
        catalog.action = [](std::string_view) { return std::optional<ui::ActionInfo>{}; };
        std::vector<ui::ActionUse> const uses = ui::actionUses(*loaded, catalog);
        REQUIRE(uses.size() == 1);
        CHECK(uses.front().modelType.empty());
    }
    SECTION("no catalog to ask") {
        std::vector<ui::ActionUse> const uses = ui::actionUses(*loaded, ui::Catalog{});
        REQUIRE(uses.size() == 1);
        CHECK(uses.front().modelType.empty());
    }
}

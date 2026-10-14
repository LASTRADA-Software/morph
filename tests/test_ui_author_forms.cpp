// SPDX-License-Identifier: Apache-2.0
//
// `collection<View>` and `wizard<Wizard>` (ui/author_forms.hpp): each node names its action and carries its forms
// document in the catalog facts, and the document travels from the screen registry through the bundle to the
// `ui-catalog` reply, whose facts answer `Catalog::view` and `Catalog::wizard` on the client.

#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <memory>
#include <morph/core/bridge.hpp>
#include <morph/core/registry.hpp>
#include <morph/core/wire.hpp>
#include <morph/forms/flows.hpp>
#include <morph/forms/views.hpp>
#include <morph/session/session.hpp>
#include <morph/ui/author_forms.hpp>
#include <morph/ui/bundle.hpp>
#include <morph/ui/service.hpp>
#include <string>
#include <vector>

namespace af {

struct Row {
    std::int64_t id = 0;
    std::string label;
};

struct Rows {
    std::vector<Row> rows;
};

struct ListRows {};

struct AddRow {
    std::string label;
};

struct ConfirmRow {
    std::int64_t id = 0;
};

struct Model {
    Rows execute(ListRows const& action) {
        static_cast<void>(action);
        return {};
    }
    Row execute(AddRow const& action) { return Row{.id = 1, .label = action.label}; }
    Row execute(ConfirmRow const& action) { return Row{.id = action.id, .label = {}}; }
};

struct RowsView {
    using kind = morph::views::CollectionView;
    using query = ListRows;
};

using AddWizard = morph::flows::Wizard<"Add a row", morph::flows::WizardStep<AddRow, "Label">,
                                       morph::flows::WizardStep<ConfirmRow, "Confirm">>;

}  // namespace af

// The registration macros declare a non-const status object each.
// NOLINTBEGIN(misc-const-correctness)
BRIDGE_REGISTER_MODEL(af::Model, "Af_Model")
BRIDGE_REGISTER_ACTION(af::Model, af::ListRows, "Af_ListRows", morph::model::Loggable::No)
BRIDGE_REGISTER_ACTION(af::Model, af::AddRow, "Af_AddRow")
BRIDGE_REGISTER_ACTION(af::Model, af::ConfirmRow, "Af_ConfirmRow")
// NOLINTEND(misc-const-correctness)

namespace {

namespace ui = morph::ui::author;

ui::Screen rowsScreen() {
    return ui::screen("af.rows", [](ui::Scope& screen) {
        auto const model = screen.model<af::Model>("rows", {.instance = ui::Instance::Private});
        return ui::column({ui::collection<af::RowsView>(model), ui::wizard<af::AddWizard>(model)});
    });
}

}  // namespace

// Mutation: record no document in the facts (the views map is empty).
TEST_CASE("collection<View> and wizard<Wizard> name their action and carry their document", "[ui][author]") {
    auto const model = ui::ModelHandle<af::Model>{"rows"};
    ui::Node const collection = ui::collection<af::RowsView>(model);
    CHECK(glz::write_json(collection.json()).value_or("") ==
          R"({"kind":"collection","model":"rows","action":"Af_ListRows"})");
    REQUIRE(collection.facts().views.contains("Af_ListRows"));
    CHECK(collection.facts().views.at("Af_ListRows") == morph::views::viewSchemaJson<af::RowsView>());
    CHECK(collection.facts().actions.contains("Af_ListRows"));

    ui::Node const wizard = ui::wizard<af::AddWizard>(model);
    CHECK(glz::write_json(wizard.json()).value_or("") == R"({"kind":"wizard","model":"rows","action":"Af_AddRow"})");
    REQUIRE(wizard.facts().wizards.contains("Af_AddRow"));
    CHECK(wizard.facts().wizards.at("Af_AddRow") == morph::flows::wizardSchemaJson<af::AddWizard>());
}

// Mutation: leave the bundle's views out of the catalog reply (the client's view lookup is empty).
TEST_CASE("a collection's and a wizard's documents reach the client's catalog through the ui-catalog reply",
          "[ui][author][ui-service]") {
    ui::ScreenRegistry screens;
    auto const added = screens.add(rowsScreen());
    REQUIRE(added.has_value());
    morph::ui::Catalog const facts = screens.catalog();
    REQUIRE(facts.view);
    CHECK(facts.view("Af_ListRows") == morph::views::viewSchemaJson<af::RowsView>());

    morph::ui::UiRegistry served{morph::ui::ApplicationInfo{.id = "af", .version = "1"}, facts};
    morph::ui::CatalogEntry entry;
    entry.id = "af.rows";
    entry.title = "Rows";
    auto const screen = served.addScreen(std::move(entry), (*added)->text);
    REQUIRE(screen.has_value());
    auto const bundle = served.build();
    REQUIRE(bundle.has_value());

    morph::ui::UiService service{*bundle};
    morph::wire::Envelope request;
    request.kind = "ui-catalog";
    request.callId = 1;
    request.session.principal = "alice";
    morph::session::AllowAllAuthorizer const authorizer;
    morph::wire::Envelope const reply = morph::wire::decode(service.handle(request, authorizer));
    REQUIRE(reply.kind == "ok");
    auto const catalog = morph::ui::parseCatalogReply(reply.body);
    REQUIRE(catalog.has_value());
    morph::ui::Catalog const client = catalog->facts();
    REQUIRE(client.view);
    REQUIRE(client.wizard);
    CHECK(client.view("Af_ListRows") == morph::views::viewSchemaJson<af::RowsView>());
    CHECK(client.wizard("Af_AddRow") == morph::flows::wizardSchemaJson<af::AddWizard>());
    CHECK_FALSE(client.view("Af_AddRow").has_value());
}

TEST_CASE("a bundle without forms documents serves none", "[ui][author][ui-service]") {
    ui::ScreenRegistry screens;
    REQUIRE(screens.add(ui::screen("af.empty", [](ui::Scope&) { return ui::text({.text = "empty"}); })).has_value());
    morph::ui::Catalog const facts = screens.catalog();
    CHECK_FALSE(facts.view);
    CHECK_FALSE(facts.wizard);
}

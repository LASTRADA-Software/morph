// SPDX-License-Identifier: Apache-2.0

#include "gallery/gallery.hpp"

#include <cstdint>
#include <expected>
#include <memory>
#include <morph/core/registry.hpp>
#include <morph/ui/author.hpp>
#include <morph/ui/author_forms.hpp>
#include <morph/ui/service.hpp>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "gallery/screens.hpp"

BRIDGE_REGISTER_MODEL(gallery::GalleryModel, "Gallery_Model")
BRIDGE_REGISTER_ACTION(gallery::GalleryModel, gallery::ListFacts, "Gallery_ListFacts")
BRIDGE_REGISTER_ACTION(gallery::GalleryModel, gallery::AddFact, "Gallery_AddFact")
BRIDGE_REGISTER_ACTION(gallery::GalleryModel, gallery::ConfirmFact, "Gallery_ConfirmFact")

namespace gallery {

namespace ui = morph::ui::author;

// NOLINTNEXTLINE(readability-convert-member-functions-to-static): the bridge calls an action on a model instance.
std::vector<Fact> GalleryModel::execute(ListFacts const& /*action*/) const {
    return {Fact{.id = 1, .label = "Kinds", .value = "34"},
            Fact{.id = 2, .label = "Renderers", .value = "Qt Quick, terminal"},
            Fact{.id = 3, .label = "Vocabulary", .value = "ui/1"}};
}

// NOLINTNEXTLINE(readability-convert-member-functions-to-static): the bridge calls an action on a model instance.
Fact GalleryModel::execute(AddFact const& action) const {
    return Fact{.id = 4, .label = action.label, .value = action.value};
}

// NOLINTNEXTLINE(readability-convert-member-functions-to-static): the bridge calls an action on a model instance.
bool GalleryModel::execute(ConfirmFact const& action) const { return action.keep; }

ui::Screen homeScreen() {
    return ui::screen(std::string{kHomeScreen}, [](ui::Scope& screen) {
        screen.title("Gallery");
        auto model = screen.model<GalleryModel>("gallery");
        auto facts = screen.query<ListFacts>("facts", model);
        auto name = screen.state<std::string>("name");
        auto clicks = screen.state("clicks", std::int64_t{0});
        auto flavour = screen.state("flavour", std::string{"plain"});
        auto checked = screen.state("checked", false);
        auto level = screen.state("level", std::int64_t{3});
        auto when = screen.state<std::string>("when");
        auto picked = screen.state<std::string>("picked");
        auto density = screen.choicePoint("density", {"comfortable", "compact"}, "comfortable");
        auto swatch = screen.component(
            "Swatch", {.host = "GallerySwatch", .version = 1, .fallback = ui::badge("Swatch", "info")});
        auto about = ui::element("about");
        auto side = ui::element("side");

        auto inputs = ui::panel(
            {ui::textInput(
                 {.value = name, .label = "Name", .placeholder = "Your name", .onChange = ui::set(name, ui::event())})
                 .testId("name"),
             ui::text({.text = ui::concat({"Hello, ", name})}).testId("greeting"),
             ui::button({.label = "Click", .onClick = ui::set(clicks, clicks + 1)}).testId("click"),
             ui::text({.text = ui::format("{0} clicks", {clicks})}).testId("clicks"),
             ui::checkbox({.label = "Checked", .checked = checked, .onToggle = ui::set(checked, ui::event())})
                 .testId("checked"),
             ui::select({.options = ui::list({ui::obj({{"key", "plain"}, {"label", "Plain"}}),
                                              ui::obj({{"key", "fancy"}, {"label", "Fancy"}})}),
                         .selected = flavour,
                         .label = "Flavour",
                         .onChoose = ui::set(flavour, ui::event())})
                 .testId("flavour"),
             ui::switchOn(flavour,
                          {ui::Case{"plain", ui::text({.text = "A plain flavour"}).testId("flavour-text")},
                           ui::Case{"fancy", ui::text({.text = "A fancy flavour"}).testId("flavour-text")}},
                          ui::text({.text = "No flavour"})),
             ui::dateTimeInput(when, "When", ui::set(when, ui::event())),
             ui::slider({.value = level,
                         .label = "Level",
                         .min = 0,
                         .max = 10,
                         .step = 1,
                         .onChange = ui::set(level, ui::event())}),
             ui::filePicker({.label = "File", .onPick = ui::set(picked, ui::event())})},
            {.title = "Inputs", .padding = 2});

        auto layout = ui::row({ui::grid({ui::text({.text = "Cell"}), ui::spacer()}, {.gap = 1, .columns = 2}),
                               ui::scroll({ui::text({.text = "Scrolled"})}, "vertical"),
                               ui::tabs({{.label = "One", .children = ui::text({.text = "First tab"})},
                                         {.label = "Two", .children = ui::text({.text = "Second tab"})}}),
                               ui::splitter({ui::text({.text = "Left"}), ui::text({.text = "Right"})}, "horizontal")},
                              {.gap = 2});

        auto menus = ui::menu({{.label = "View",
                                .items = {{.label = "Side panel", .onClick = ui::open(side)},
                                          {.label = "About", .onClick = ui::open(about)}}}});

        auto overlays =
            ui::column({ui::button({.label = "About", .onClick = ui::open(about)}),
                        ui::dialog(about, {ui::text({.text = "One document, every node kind."})},
                                   {.title = "About the gallery", .cancel = ui::close(about)}),
                        ui::drawer(side, {ui::text({.text = "A side panel"})}, {.side = "end"}).a11y("Side panel"),
                        ui::busy(facts.pending(), "Loading facts")});

        auto lists = ui::column(
            {ui::boundary({ui::text({.text = ui::format("{0} facts", {ui::count(facts.value())})}).testId("count")},
                          ui::text({.text = "Loading"}), {ui::text({.text = "The facts did not load"})}),
             ui::forEach(facts.value(), {.key = ui::row(&Fact::id)},
                         [](ui::RowScope& /*row*/) { return ui::badge(ui::row("label"), "neutral"); }),
             ui::table(facts.value(),
                       {.key = ui::row(&Fact::id),
                        .columns = {{.name = "label", .label = "Label", .cell = ui::text({.text = ui::row("label")})},
                                    {.name = "value", .label = "Value", .cell = ui::text({.text = ui::row("value")})}},
                        .selection = ui::Selection::Single})
                 .a11y("Facts")
                 .testId("facts"),
             ui::keyValue(facts.value())});

        auto feedback = ui::column(
            {ui::banner({.tone = "info", .text = "Every node kind of ui/1 the interpreter lowers is on this screen"}),
             ui::badge("New", "success"), ui::progress(level / 10, "Level"),
             ui::steps(ui::list({ui::obj({{"name", "one"}, {"label", "Open"}}),
                                 ui::obj({{"name", "two"}, {"label", "Look"}}),
                                 ui::obj({{"name", "three"}, {"label", "Close"}})}),
                       1),
             ui::emptyState({.title = "Nothing here", .text = "An empty state"}),
             ui::collapsible({ui::text({.text = "Inside"})}, {.title = "More", .open = checked}),
             ui::dropZone({ui::text({.text = "Drop files here"})}, {.onDrop = ui::set(picked, "dropped")}),
             ui::customize(density), ui::custom(swatch).a11y("Swatch")});

        auto const forms =
            ui::panel({ui::form<AddFact>(model).id(ui::element("addFact")).testId("addFact"),
                       ui::collection<FactsView>(model).id(ui::element("factsView")).testId("factsView"),
                       ui::wizard<FactWizard>(model).id(ui::element("factWizard")).testId("factWizard")},
                      {.title = "Forms", .padding = 2});

        return ui::column({menus, inputs, layout, overlays, lists, feedback, forms}, {.gap = 3});
    });
}

std::expected<std::shared_ptr<morph::ui::ServedBundle const>, morph::ui::Refusal> bundle() {
    ui::ScreenRegistry screens;
    auto registered = screens.add(homeScreen());
    if (!registered) {
        return std::unexpected(std::move(registered.error()));
    }
    morph::ui::UiRegistry served{morph::ui::ApplicationInfo{.id = std::string{kApplicationId}, .version = "1.0"},
                                 screens.catalog()};
    morph::ui::CatalogEntry entry;
    entry.id = std::string{kHomeScreen};
    entry.title = "Gallery";
    if (auto added = served.addScreen(std::move(entry), (*registered)->text); !added) {
        return std::unexpected(std::move(added.error()));
    }
    return served.build();
}

morph::examples::LocalApp localApp() {
    return morph::examples::LocalApp{.name = std::string{kApplicationId}, .bundle = &bundle, .setUp = {}};
}

}  // namespace gallery

// SPDX-License-Identifier: Apache-2.0

#include "forms_screens.hpp"

#include <expected>
#include <memory>
#include <morph/ui/author.hpp>
#include <morph/ui/author_forms.hpp>
#include <morph/ui/service.hpp>
#include <string>
#include <utility>
#include <vector>

#include "../lab_model.hpp"
#include "../lab_schemas.hpp"
#include "../lab_wizard.hpp"

namespace lab::screens {

namespace ui = morph::ui::author;

namespace {

// The lab's model keeps its samples in memory, so each screen works on an instance of its own.
ui::ModelHandle<LabModel> labModel(ui::Scope& screen) {
    return screen.model<LabModel>("lab", {.instance = ui::Instance::Private});
}

// A dry density from a mass and a volume: the model divides the two quantities, units and all.
ui::Screen densityScreen() {
    return ui::screen(std::string{kDensity}, [](ui::Scope& screen) {
        screen.title("Dry density");
        auto const lab = labModel(screen);
        auto const density = screen.state<ui::Any>("density");
        return ui::column(
            {ui::form<ComputeDryDensity>(lab, {.onSuccess = {ui::set(density, ui::ref("event"))}})
                 .id(ui::element("densityForm"))
                 .testId("densityForm"),
             ui::text({.text = ui::format("Dry density: {0}", {density})})
                 .testId("density")
                 .visible(ui::engaged(density))},
            {.gap = 8});
    });
}

// A measurement against a sample chosen from the model's own sample list.
ui::Screen measureScreen() {
    return ui::screen(std::string{kMeasure}, [](ui::Scope& screen) {
        screen.title("Record a measurement");
        auto const lab = labModel(screen);
        return ui::form<RecordMeasurement>(lab).id(ui::element("measureForm")).testId("measureForm");
    });
}

// Registration, then the first measurement of the sample just registered: the wizard binds its id forward.
ui::Screen intakeScreen() {
    return ui::screen(std::string{kIntake}, [](ui::Scope& screen) {
        screen.title("Intake");
        auto const lab = labModel(screen);
        return ui::wizard<IntakeWizard>(lab).id(ui::element("intake")).testId("intake");
    });
}

// The samples, with rename, delete and new, as the view descriptor declares them.
ui::Screen samplesScreen() {
    return ui::screen(std::string{kSamples}, [](ui::Scope& screen) {
        screen.title("Samples");
        auto const lab = labModel(screen);
        return ui::collection<SamplesView>(lab).id(ui::element("samples")).testId("samples");
    });
}

}  // namespace

std::expected<std::shared_ptr<morph::ui::ServedBundle const>, morph::ui::Refusal> bundle() {
    ui::ScreenRegistry screens;
    struct Listed {
        std::string id;
        std::string title;
        decltype(screens.add(densityScreen())) registered;
    };
    std::vector<Listed> listed;
    listed.push_back({.id = std::string{kDensity}, .title = "Density", .registered = screens.add(densityScreen())});
    listed.push_back({.id = std::string{kMeasure}, .title = "Measure", .registered = screens.add(measureScreen())});
    listed.push_back({.id = std::string{kIntake}, .title = "Intake", .registered = screens.add(intakeScreen())});
    listed.push_back({.id = std::string{kSamples}, .title = "Samples", .registered = screens.add(samplesScreen())});
    for (Listed const& screen : listed) {
        if (!screen.registered) {
            return std::unexpected(screen.registered.error());
        }
    }
    morph::ui::UiRegistry served{morph::ui::ApplicationInfo{.id = std::string{kApplicationId}, .version = "1.0"},
                                 screens.catalog()};
    for (Listed const& screen : listed) {
        morph::ui::CatalogEntry entry;
        entry.id = screen.id;
        entry.title = screen.title;
        entry.module = "Lab console";
        if (auto added = served.addScreen(std::move(entry), (*screen.registered)->text); !added) {
            return std::unexpected(std::move(added.error()));
        }
    }
    return served.build();
}

}  // namespace lab::screens

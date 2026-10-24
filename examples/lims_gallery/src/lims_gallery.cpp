// SPDX-License-Identifier: Apache-2.0

#include "lims_gallery/lims_gallery.hpp"

#include <expected>
#include <memory>
#include <morph/ui/author.hpp>
#include <morph/ui/service.hpp>
#include <string>
#include <utility>
#include <vector>

#include "lims_gallery/screens.hpp"

namespace lims_gallery {

namespace {

namespace ui = morph::ui::author;

// The theme index: the designs' green accent and neutrals in one base layer, the tinted surfaces the screens mark
// a selection, a result out of specification and a plate's template wells with, and the icons the screens draw.
constexpr char const* kThemeIndex = R"({
  "vocab": "ui-theme/1",
  "base": "theme/base.tokens.json",
  "surfaces": { "selected": "theme/selected.tokens.json", "outOfSpec": "theme/out-of-spec.tokens.json",
                "standard": "theme/standard.tokens.json", "control": "theme/control.tokens.json",
                "blank": "theme/blank.tokens.json" },
  "icons": { "scan": "icon/scan.svg" }
})";

// Green is the accent. The status tones keep their meaning — err out of spec, warn out of trend or due, ok within
// spec, info and neutral for workflow state — so a tone is never mistaken for the accent: an `ok` tag is a tinted
// label, a primary button a filled one with its own text.
constexpr char const* kThemeBase = R"({
  "color": { "$type": "color",
    "accent": { "$value": "#17784A" }, "focus": { "$value": "#17784A" },
    "text": { "$value": "#1C1E21" }, "muted": { "$value": "#4D5461" },
    "border": { "$value": "#DDE1E6" }, "surface": { "$value": "#FFFFFF" },
    "background": { "$value": "#F6F7F9" }, "selection": { "$value": "#DCEFE3" },
    "neutral": { "$value": "#363C46" },
    "ok": { "$value": "#17602F", "bg": { "$value": "#E2F4E8" }, "border": { "$value": "#BCDFC9" },
            "dot": { "$value": "#17784A" } },
    "warn": { "$value": "#7A4A00", "bg": { "$value": "#FFF1D6" }, "border": { "$value": "#F1D49A" },
              "dot": { "$value": "#B7791F" } },
    "err": { "$value": "#9B1C1C", "bg": { "$value": "#FDE7E7" }, "border": { "$value": "#F2C0C0" },
             "dot": { "$value": "#B42318" } },
    "info": { "$value": "#1F3A8A", "bg": { "$value": "#EAF0FB" }, "border": { "$value": "#C7D5F0" },
              "dot": { "$value": "#1F3A8A" } },
    "table": { "header": { "$value": "#FAFBFC" } } },
  "radius": { "$type": "dimension",
    "control": { "$value": { "value": 6, "unit": "px" } }, "card": { "$value": { "value": 6, "unit": "px" } } }
})";

// A bar that acts on selected rows: the accent's pale tint.
constexpr char const* kSelectedSurface = R"({
  "color": { "$type": "color", "surface": { "$value": "#EAF5EE" }, "border": { "$value": "#BCDFC9" } }
})";

// A record with a result out of specification: the err tone's pale tint.
constexpr char const* kOutOfSpecSurface = R"({
  "color": { "$type": "color", "surface": { "$value": "#FFF5F5" }, "border": { "$value": "#F2C0C0" } }
})";

// A plate's template wells: standards blue, controls amber, the blank grey, so they read apart from the samples.
constexpr char const* kStandardSurface = R"({
  "color": { "$type": "color", "surface": { "$value": "#E6ECFF" }, "border": { "$value": "#5B7BE0" },
             "text": { "$value": "#1F3FA8" } }
})";

constexpr char const* kControlSurface = R"({
  "color": { "$type": "color", "surface": { "$value": "#FFF1D6" }, "border": { "$value": "#D19100" },
             "text": { "$value": "#6B4100" } }
})";

constexpr char const* kBlankSurface = R"({
  "color": { "$type": "color", "surface": { "$value": "#EEF0F3" }, "border": { "$value": "#7A828F" },
             "text": { "$value": "#363C46" } }
})";

// The barcode glyph of the receipt screen's scan field, as the design draws it.
constexpr char const* kScanIcon =
    R"(<svg xmlns="http://www.w3.org/2000/svg" width="20" height="20" viewBox="0 0 20 20" fill="none" )"
    R"(stroke="#17784A" stroke-width="1.6"><path d="M2 6V2h4M14 2h4v4M18 14v4h-4M6 18H2v-4M6 6v8M9 6v8M12 6v8M15 )"
    R"(6v8"/></svg>)";

// A registered screen and its catalog entry's title.
struct Registered {
    std::string id;
    std::string title;
    std::string text;
};

}  // namespace

std::expected<std::shared_ptr<morph::ui::ServedBundle const>, morph::ui::Refusal> bundle() {
    ui::ScreenRegistry screens;
    std::vector<Registered> registered;
    auto const add = [&](ui::Screen const& screen, std::string_view title) -> std::expected<void, morph::ui::Refusal> {
        auto added = screens.add(screen);
        if (!added) {
            return std::unexpected(std::move(added.error()));
        }
        registered.push_back(Registered{.id = screen.id(), .title = std::string{title}, .text = (*added)->text});
        return {};
    };
    if (auto added = add(indexScreen(), "LIMS gallery"); !added) {
        return std::unexpected(std::move(added.error()));
    }
    for (Design const& design : designs()) {
        if (design.make == nullptr) {
            continue;
        }
        if (auto added = add(design.make(), design.title); !added) {
            return std::unexpected(std::move(added.error()));
        }
    }
    morph::ui::UiRegistry served{morph::ui::ApplicationInfo{.id = std::string{kApplicationId}, .version = "1.0"},
                                 screens.catalog()};
    for (Registered& screen : registered) {
        morph::ui::CatalogEntry entry;
        entry.id = screen.id;
        entry.title = screen.title;
        entry.group = screen.id == kIndexScreen ? "Gallery" : "Designs";
        if (auto added = served.addScreen(std::move(entry), std::move(screen.text)); !added) {
            return std::unexpected(std::move(added.error()));
        }
    }
    for (auto [kind, fileId, bytes] : {std::tuple{"theme", "theme/index.json", kThemeIndex},
                                       std::tuple{"theme", "theme/base.tokens.json", kThemeBase},
                                       std::tuple{"theme", "theme/selected.tokens.json", kSelectedSurface},
                                       std::tuple{"theme", "theme/out-of-spec.tokens.json", kOutOfSpecSurface},
                                       std::tuple{"theme", "theme/standard.tokens.json", kStandardSurface},
                                       std::tuple{"theme", "theme/control.tokens.json", kControlSurface},
                                       std::tuple{"theme", "theme/blank.tokens.json", kBlankSurface},
                                       std::tuple{"icon", "icon/scan.svg", kScanIcon}}) {
        if (auto added = served.addFile(morph::ui::BundleFile{.kind = kind, .id = fileId, .bytes = bytes}); !added) {
            return std::unexpected(std::move(added.error()));
        }
    }
    return served.build();
}

morph::client::LocalApp localApp() {
    return morph::client::LocalApp{.name = std::string{kApplicationId}, .bundle = &bundle, .setUp = {}};
}

}  // namespace lims_gallery

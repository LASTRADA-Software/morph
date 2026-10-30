// SPDX-License-Identifier: Apache-2.0
//
// Design 07, laying out a 96-well ELISA plate: the samples still to place beside the plate's 8 × 12 wells. A sample
// is dragged into a well, or wells are selected and filled with the next samples in order; arrow keys move between
// the wells, Shift with an arrow extends the selection, and Enter assigns a sample to the focused well. The
// template fixes the standards, controls and blank; every other well holds a sample or is empty.
//
// A well's sample is found in one `placed` list rather than held in 96 states, so that filling the selection, which
// pairs the k-th selected empty well with the k-th sample to place, is one expression over lists.

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <morph/ui/value.hpp>
#include <string>
#include <vector>

#include "lims_gallery/screens.hpp"
#include "screen_layout.hpp"

namespace lims_gallery {

namespace {

namespace ui = layout::ui;
using layout::Align;
using layout::ButtonEmphasis;
using morph::ui::ButtonSize;
using morph::ui::ControlShape;
using morph::ui::Value;
using morph::ui::ValueList;
using morph::ui::ValueRecord;
using Text = ui::Handle<std::string>;
using Flag = ui::Handle<bool>;
using Count = ui::Handle<std::int64_t>;

constexpr std::array<char const*, 8> kRowNames{"A", "B", "C", "D", "E", "F", "G", "H"};
constexpr int kColumns = 12;
// A well's grid track, in units: wide enough for the longest label, `STD1`, in the dense mono face.
constexpr int kWellTrack = 6;
constexpr int kRowCount = static_cast<int>(kRowNames.size());

// Sample n of the worksheet is S-26-(kIdBase + n); the wells show it as Sn.
constexpr std::int64_t kIdBase = 104233;
// Samples 1 to 67 are placed when the screen opens; 68 to 81 are still to place.
constexpr std::int64_t kFirstToPlace = 68;
constexpr std::int64_t kLastSample = 81;
// How many of the samples to place the panel lists; it counts the rest.
constexpr std::int64_t kListed = 5;

// The standards' concentrations, one per row in column 1.
constexpr std::array<char const*, 8> kStandards{"100", "50", "25", "12.5", "6.25", "3.13", "1.56", "0.78"};

enum class Kind : std::uint8_t { Standard, Control, Blank, Sample };

struct Well {
    std::size_t row;
    int column;
    std::string name;
    Kind kind = Kind::Sample;
    // A template well's label, its line on compact, its accessible role and its surface.
    std::string label;
    std::string description;
    std::string role;
    std::string surface;
};

Well wellAt(std::size_t row, int column) {
    Well well{.row = row, .column = column, .name = std::string{kRowNames.at(row)} + std::to_string(column)};
    if (column == 1) {
        auto const number = std::to_string(row + 1);
        well.kind = Kind::Standard;
        well.label = "STD" + number;
        well.description = "Standard " + number + ", " + kStandards.at(row) + " ng/mL";
        well.role = "standard " + number;
        well.surface = "standard";
    } else if (well.name == "A2" || well.name == "A9") {
        bool const low = well.name == "A2";
        well.kind = Kind::Control;
        well.label = "QC";
        well.description = low ? "Control, low" : "Control, high";
        well.role = low ? "control low" : "control high";
        well.surface = "control";
    } else if (well.name == "B2") {
        well.kind = Kind::Blank;
        well.label = "B";
        well.description = "Blank";
        well.role = "blank";
        well.surface = "blank";
    }
    return well;
}

std::vector<Well> plateWells() {
    std::vector<Well> wells;
    wells.reserve(kRowNames.size() * kColumns);
    for (std::size_t row = 0; row < kRowNames.size(); ++row) {
        for (int column = 1; column <= kColumns; ++column) {
            wells.push_back(wellAt(row, column));
        }
    }
    return wells;
}

std::vector<Well> const& allWells() {
    static std::vector<Well> const kWells = plateWells();
    return kWells;
}

Well const& wellOf(std::size_t row, int column) {
    return allWells().at((row * kColumns) + static_cast<std::size_t>(column - 1));
}

// The sample wells' names in plate order: filtering this list keeps a selection in plate order.
std::vector<std::string> sampleWellNames() {
    std::vector<std::string> names;
    for (Well const& well : allWells()) {
        if (well.kind == Kind::Sample) {
            names.push_back(well.name);
        }
    }
    return names;
}

// Whether the design draws the well empty when the screen opens: row A, D5 (a drag is in flight onto it) and the
// last eight wells of row H.
bool startsEmpty(Well const& well) {
    return well.row == 0 || well.name == "D5" || (well.row == 7 && well.column >= 5);
}

// A literal, so the 67 placed samples are one node of the expression and not 200.
ui::AnyHandle initialPlaced() {
    ValueList placed;
    std::int64_t sample = 1;
    for (Well const& well : allWells()) {
        if (well.kind == Kind::Sample && !startsEmpty(well)) {
            placed.emplace_back(ValueRecord{{"well", Value{well.name}}, {"sample", Value{sample}}});
            ++sample;
        }
    }
    return ui::AnyHandle{Value{std::move(placed)}};
}

ui::AnyHandle initialToPlace() {
    ValueList samples;
    for (std::int64_t sample = kFirstToPlace; sample <= kLastSample; ++sample) {
        samples.emplace_back(ValueRecord{{"n", Value{sample}}});
    }
    return ui::AnyHandle{Value{std::move(samples)}};
}

ui::AnyHandle fullId(ui::AnyHandle const& sample) { return ui::format("S-26-{0}", {Count{sample} + kIdBase}); }

struct Plate {
    ui::Element assignDialog = ui::element("assign");
    ui::AnyHandle width;
    Flag compact;
    ui::AnyHandle placed;
    ui::AnyHandle toPlace;
    ui::AnyHandle selection;
    ui::AnyHandle pending;
    Text fill;
    Text layoutTemplate;
    Text openRow;
    Text assignWell;
    Count assignSample;
    ui::AnyHandle sampleWells;
    ui::AnyHandle selected;
    ui::AnyHandle emptySelected;
    ui::AnyHandle emptyWells;
    ui::AnyHandle toPlaceCount;
    // Each sample well's sample, or null while it is empty, by well name.
    std::map<std::string, ui::AnyHandle> samples;
    // What each sample well shows, by well name: its button's label and surface, and its line on compact. They are
    // named so that a row's records, one expression, stay within the bound on an expression's size.
    std::map<std::string, ui::AnyHandle> labels;
    std::map<std::string, ui::AnyHandle> surfaces;
    std::map<std::string, ui::AnyHandle> lines;
    std::map<std::string, ui::AnyHandle> runs;
    // Each well's element, by well name, so arrow keys can move focus to it.
    std::map<std::string, ui::Element> elements;
};

ui::AnyHandle notIn(ui::AnyHandle const& list, ui::AnyHandle const& value) { return !ui::includes(list, value); }

ui::AnyHandle sampleOf(Plate const& plate, std::string const& well) {
    return ui::call("first", {ui::map(ui::filter(plate.placed, "entry",
                                                 [&well](ui::AnyHandle const& entry) {
                                                     return ui::eq(entry.at("well"), ui::AnyHandle{well});
                                                 }),
                                      "entry", [](ui::AnyHandle const& entry) { return entry.at("sample"); })});
}

// The last element of @p list: there is no `last`, so it is the one whose index is the count less one.
ui::AnyHandle lastOf(ui::AnyHandle const& list) {
    return ui::call("first", {ui::filter(list, "item", [&list](ui::AnyHandle const& item) -> ui::AnyHandle {
                        return ui::eq(ui::call("indexOf", {list, item}), Count{ui::call("count", {list})} - 1);
                    })});
}

void declareWells(ui::Scope& screen, Plate& plate) {
    for (Well const& well : allWells()) {
        plate.elements.emplace(well.name, ui::element("well" + well.name));
        if (well.kind == Kind::Sample) {
            auto const sample = screen.let("sample" + well.name, sampleOf(plate, well.name));
            plate.samples.emplace(well.name, sample);
            auto const placed = ui::engaged(sample);
            plate.labels.emplace(
                well.name,
                screen.let("label" + well.name, ui::cond(placed, Text{ui::format("S{0}", {sample})}, Text{"·"})));
            plate.surfaces.emplace(well.name,
                                   screen.let("surface" + well.name, ui::cond(placed, Text{"sample"}, Text{"empty"})));
            plate.lines.emplace(
                well.name,
                screen.let("line" + well.name,
                           ui::cond(placed, Text{ui::format("Sample {0}", {fullId(sample)})}, Text{"Empty"})));
            plate.runs.emplace(well.name, screen.let("run" + well.name, ui::cond(placed, ui::AnyHandle{well.name},
                                                                                 ui::AnyHandle{"empty"})));
        }
    }
}

Plate declare(ui::Scope& screen) {
    Plate plate;
    plate.width = layout::widthClass(screen);
    plate.compact = screen.let("compact", ui::eq(plate.width, "compact"));
    plate.placed = screen.state<ui::Any>("placed", initialPlaced());
    plate.toPlace = screen.state<ui::Any>("toPlace", initialToPlace());
    // The design opens with A3 to A8 selected.
    plate.selection = screen.state<ui::Any>(
        "selection", ui::AnyHandle{std::vector<std::string>{"A3", "A4", "A5", "A6", "A7", "A8"}});
    plate.pending = screen.state<ui::Any>("pending", ui::AnyHandle{std::vector<std::string>{}});
    plate.fill = screen.state("fill", std::string{"order"});
    plate.layoutTemplate = screen.state("template", std::string{"hcp96"});
    plate.openRow = screen.state("openRow", std::string{"A"});
    plate.assignWell = screen.state<std::string>("assignWell");
    plate.assignSample = screen.state<std::int64_t>("assignSample");
    plate.sampleWells = screen.let("sampleWells", ui::AnyHandle{sampleWellNames()});
    auto const placedWells = screen.let(
        "placedWells", ui::map(plate.placed, "entry", [](ui::AnyHandle const& entry) { return entry.at("well"); }));
    plate.selected = screen.let("selected", ui::filter(plate.sampleWells, "well", [&plate](ui::AnyHandle const& well) {
                                    return ui::includes(plate.selection, well);
                                }));
    plate.emptySelected = screen.let(
        "emptySelected", ui::filter(plate.selected, "well",
                                    [&placedWells](ui::AnyHandle const& well) { return notIn(placedWells, well); }));
    plate.emptyWells = screen.let(
        "emptyWells", ui::filter(plate.sampleWells, "well",
                                 [&placedWells](ui::AnyHandle const& well) { return notIn(placedWells, well); }));
    plate.toPlaceCount = screen.let("toPlaceCount", ui::call("count", {plate.toPlace}));
    declareWells(screen, plate);
    return plate;
}

// ── What the user does to the plate ──────────────────────────────────────────────────────────────────────────────

// @p sample into @p well. A sample the well held goes back to the samples to place.
ui::Commands place(Plate const& plate, ui::AnyHandle const& well, ui::AnyHandle const& sample) {
    auto const others = ui::filter(plate.toPlace, "item",
                                   [&sample](ui::AnyHandle const& item) { return ui::ne(item.at("n"), sample); });
    auto const displaced =
        ui::map(ui::filter(plate.placed, "entry",
                           [&well](ui::AnyHandle const& entry) { return ui::eq(entry.at("well"), well); }),
                "entry", [](ui::AnyHandle const& entry) { return ui::obj({{"n", entry.at("sample")}}); });
    auto const kept = ui::filter(plate.placed, "entry",
                                 [&well](ui::AnyHandle const& entry) { return ui::ne(entry.at("well"), well); });
    return {ui::set(plate.toPlace, ui::concatLists({others, displaced})),
            ui::set(plate.placed, ui::concatLists({kept, ui::list({ui::obj({{"well", well}, {"sample", sample}})})})),
            ui::set(plate.selection, ui::filter(plate.selection, "item",
                                                [&well](ui::AnyHandle const& item) { return ui::ne(item, well); }))};
}

// The samples to place, in order, into the wells of @p targets, in plate order, as far as the samples go. The
// pairs are computed first, into `pending`, since the targets are empty wells and stop being so as they fill.
ui::Commands fillWells(Plate const& plate, ui::AnyHandle const& targets) {
    auto const reached = ui::filter(targets, "well", [&plate, &targets](ui::AnyHandle const& well) -> ui::AnyHandle {
        return Count{ui::call("indexOf", {targets, well})} < Count{plate.toPlaceCount};
    });
    auto const pairs = ui::map(reached, "well", [&plate, &targets](ui::AnyHandle const& well) -> ui::AnyHandle {
        auto const position = ui::call("indexOf", {targets, well});
        auto const nth = ui::filter(plate.toPlace, "item", [&plate, &position](ui::AnyHandle const& item) {
            return ui::eq(ui::call("indexOf", {plate.toPlace, item}), position);
        });
        return ui::obj(
            {{"well", well},
             {"sample",
              ui::call("first", {ui::map(nth, "item", [](ui::AnyHandle const& item) { return item.at("n"); })})}});
    });
    auto const left = ui::filter(plate.toPlace, "item", [&plate](ui::AnyHandle const& item) -> ui::AnyHandle {
        return Count{ui::call("indexOf", {plate.toPlace, item})} >= Count{ui::call("count", {plate.pending})};
    });
    return {ui::set(plate.pending, pairs), ui::set(plate.toPlace, left),
            ui::set(plate.placed, ui::concatLists({plate.placed, plate.pending})),
            ui::set(plate.selection, ui::AnyHandle{std::vector<std::string>{}})};
}

// Empties the selected wells; their samples go back to the samples to place.
ui::Commands clearWells(Plate const& plate) {
    auto const inSelection = [&plate](ui::AnyHandle const& entry) {
        return ui::includes(plate.selection, entry.at("well"));
    };
    auto const returned = ui::map(ui::filter(plate.placed, "entry", inSelection), "entry",
                                  [](ui::AnyHandle const& entry) { return ui::obj({{"n", entry.at("sample")}}); });
    return {ui::set(plate.toPlace, ui::concatLists({plate.toPlace, returned})),
            ui::set(plate.placed, ui::filter(plate.placed, "entry",
                                             [&plate](ui::AnyHandle const& entry) {
                                                 return notIn(plate.selection, entry.at("well"));
                                             })),
            ui::set(plate.selection, ui::AnyHandle{std::vector<std::string>{}})};
}

ui::Commands toggleWell(Plate const& plate, ui::AnyHandle const& name) {
    return ui::set(plate.selection,
                   ui::cond(ui::includes(plate.selection, name),
                            ui::filter(plate.selection, "item",
                                       [&name](ui::AnyHandle const& item) { return ui::ne(item, name); }),
                            ui::concatLists({plate.selection, ui::list({name})})));
}

ui::Commands extendTo(Plate const& plate, Well const& well) {
    if (well.kind != Kind::Sample) {
        return ui::focus(plate.elements.at(well.name));
    }
    auto const name = ui::AnyHandle{well.name};
    return {ui::ifThen(notIn(plate.selection, name),
                       ui::set(plate.selection, ui::concatLists({plate.selection, ui::list({name})}))),
            ui::focus(plate.elements.at(well.name))};
}

ui::Commands openAssign(Plate const& plate, ui::AnyHandle const& well, ui::AnyHandle const& sample) {
    return {ui::set(plate.assignWell, well), ui::set(plate.assignSample, sample), ui::open(plate.assignDialog)};
}

ui::AnyHandle firstToPlace(Plate const& plate) {
    return ui::call("first", {ui::map(plate.toPlace, "item", [](ui::AnyHandle const& item) { return item.at("n"); })});
}

// ── The plate ────────────────────────────────────────────────────────────────────────────────────────────────────

struct Step {
    char const* key;
    int rows;
    int columns;
};

constexpr std::array<Step, 4> kSteps{{{.key = "Left", .rows = 0, .columns = -1},
                                      {.key = "Right", .rows = 0, .columns = 1},
                                      {.key = "Up", .rows = -1, .columns = 0},
                                      {.key = "Down", .rows = 1, .columns = 0}}};

// Arrow keys move focus to the neighbouring well; with Shift they also add it to the selection.
ui::Node withArrowKeys(ui::Node node, Plate const& plate, Well const& well) {
    for (Step const& step : kSteps) {
        auto const row = static_cast<int>(well.row) + step.rows;
        auto const column = well.column + step.columns;
        if (row < 0 || row >= kRowCount || column < 1 || column > kColumns) {
            continue;
        }
        Well const& next = wellOf(static_cast<std::size_t>(row), column);
        node = node.key(step.key, ui::focus(plate.elements.at(next.name)))
                   .key(std::string{"Shift+"} + step.key, extendTo(plate, next));
    }
    return node;
}

// A well is a round, dense button in the mono face: a circle for a short label and a stadium for a longer one.
ui::ButtonOptions wellOptions(ui::AnyHandle const& label) {
    return {.label = label, .shape = ControlShape::Round, .size = ButtonSize::Small, .mono = true};
}

ui::Node templateWell(Plate const& plate, Well const& well) {
    auto const node = ui::button(wellOptions(ui::AnyHandle{well.label}))
                          .surface(well.surface)
                          .id(plate.elements.at(well.name))
                          .a11y(well.name + ", " + well.role)
                          .testId("well-" + well.name);
    return withArrowKeys(node, plate, well);
}

ui::Node sampleWell(Plate const& plate, Well const& well) {
    auto const sample = plate.samples.at(well.name);
    auto const name = ui::AnyHandle{well.name};
    auto const selected = ui::includes(plate.selection, name);
    auto const role = ui::cond(ui::engaged(sample), Text{ui::format("sample {0}", {fullId(sample)})}, Text{"empty"});
    auto options = wellOptions(plate.labels.at(well.name));
    options.onClick = toggleWell(plate, name);
    options.selected = selected;
    auto const node =
        ui::button(options)
            .id(plate.elements.at(well.name))
            .a11y(ui::format("{0}, {1}{2}", {name, role, ui::cond(selected, Text{", selected"}, Text{""})}))
            .drop(ui::map(plate.toPlace, "item", [](ui::AnyHandle const& item) { return item.at("n"); }),
                  place(plate, name, ui::event()))
            .key("Enter", openAssign(plate, name, firstToPlace(plate)))
            .surface(plate.surfaces.at(well.name))
            .testId("well-" + well.name);
    return withArrowKeys(node, plate, well);
}

ui::Node header(std::string const& text) { return ui::text({.text = text, .role = "muted"}).alignSelf(Align::Center); }

// The row letters' narrow track, then twelve centred well tracks of six units, so a column number sits over its wells
// and a dense well's label fits whole.
std::vector<ui::GridTrack> plateTracks() {
    std::vector<ui::GridTrack> tracks{{.width = ui::fixed(3)}};
    for (int column = 1; column <= kColumns; ++column) {
        tracks.push_back({.width = ui::fixed(kWellTrack), .align = Align::Center});
    }
    return tracks;
}

ui::Node plateGrid(Plate const& plate) {
    std::vector<ui::Node> cells;
    cells.reserve((kRowNames.size() + 1) * (kColumns + 1));
    cells.push_back(header(""));
    for (int column = 1; column <= kColumns; ++column) {
        cells.push_back(header(std::to_string(column)));
    }
    for (std::size_t row = 0; row < kRowNames.size(); ++row) {
        cells.push_back(header(kRowNames.at(row)));
        for (int column = 1; column <= kColumns; ++column) {
            Well const& well = wellOf(row, column);
            cells.push_back(well.kind == Kind::Sample ? sampleWell(plate, well) : templateWell(plate, well));
        }
    }
    return ui::grid(cells, {.gap = 1, .tracks = plateTracks(), .align = Align::Center})
        .a11y("96-well plate PL-0193")
        .testId("plate");
}

// A legend chip: a round mono outline badge on the well's own surface, so the surface's roles colour it.
ui::Node chip(char const* label, char const* surface, char const* meaning) {
    return ui::row(
        {ui::badge(
             {.text = label, .style = morph::ui::BadgeStyle::Outline, .shape = ControlShape::Round, .mono = true})
             .surface(surface),
         layout::muted(meaning)},
        {.gap = 1, .align = Align::Center});
}

ui::Node legend() {
    return ui::row({chip("STD", "standard", "Standard"), chip("QC", "control", "Control"), chip("B", "blank", "Blank"),
                    chip("S", "sample", "Sample"), chip("·", "empty", "Empty")},
                   {.gap = 3, .align = Align::Center})
        .testId("legend");
}

// ── The samples to place ─────────────────────────────────────────────────────────────────────────────────────────

ui::Node sourceRow(Plate const& plate) {
    auto const sample = ui::row("n");
    return ui::row({ui::text({.text = "⋮⋮", .role = "muted"}),
                    ui::text({.text = fullId(sample), .role = "mono"}).sizing(ui::stretch()),
                    ui::button({.label = "Assign…",
                                .onClick = openAssign(plate,
                                                      ui::coalesce({ui::call("first", {plate.emptySelected}),
                                                                    ui::call("first", {plate.emptyWells})}),
                                                      sample)})
                        .a11y(ui::format("Assign {0}", {fullId(sample)}))},
                   {.gap = 1, .align = Align::Center})
        .drag(sample)
        .a11y(ui::format("{0}, drag into a well", {fullId(sample)}))
        .testId("source");
}

ui::Node sources(Plate const& plate) {
    // The first five, then how many more: the design lists a few and counts the rest.
    auto const more = Count{plate.toPlaceCount} - kListed;
    return ui::panel(
               {ui::forEach(ui::slice(plate.toPlace, std::int64_t{0}, kListed), {.key = ui::row("n"), .gap = 1},
                            [&plate](ui::DeclScope&) { return ui::Nodes{sourceRow(plate)}; })
                    .testId("sources"),
                ui::text({.text = ui::format("+ {0} more", {more}), .role = "muted"}).visible(more > 0).testId("more"),
                ui::text({.text = "All samples are placed.", .role = "muted"})
                    .visible(ui::eq(plate.toPlaceCount, std::int64_t{0}))},
               {.title = ui::format("To place · {0}", {plate.toPlaceCount}), .padding = 1})
        .a11y("Samples to place")
        .testId("to-place");
}

// ── The selection bar ────────────────────────────────────────────────────────────────────────────────────────────

ui::AnyHandle selectionText(Plate const& plate) {
    auto const count = Count{ui::call("count", {plate.selected})};
    auto const first = ui::call("first", {plate.selected});
    auto const range = ui::cond(ui::eq(count, std::int64_t{1}), Text{first},
                                Text{ui::format("{0}–{1}", {first, lastOf(plate.selected)})});
    return ui::format("{0} {1} selected · {2}",
                      {count, ui::cond(ui::eq(count, std::int64_t{1}), Text{"well"}, Text{"wells"}), range});
}

ui::Node selectionBar(Plate const& plate) {
    return ui::panel(
               {ui::row(
                   {ui::text({.text = selectionText(plate), .role = "section"})
                        .sizing(ui::stretch())
                        .testId("selected"),
                    layout::muted("Fill with"),
                    ui::select({.options = ui::list({ui::obj({{"key", "order"}, {"label", "Samples, in order"}})}),
                                .selected = plate.fill,
                                .label = "Fill with",
                                .onChoose = ui::set(plate.fill, ui::event())})
                        .sizing(ui::fixed(22))
                        .testId("fill"),
                    ui::button({.label = "Apply", .onClick = fillWells(plate, plate.emptySelected)})
                        .enabled(Count{ui::call("count", {plate.emptySelected})} > 0 && Count{plate.toPlaceCount} > 0)
                        .testId("apply"),
                    ui::button(
                        {.label = "Clear wells", .emphasis = ButtonEmphasis::Ghost, .onClick = clearWells(plate)})
                        .testId("clear-wells")},
                   {.gap = 1, .align = Align::Center})},
               {.padding = 1})
        .surface("selected")
        .visible(Count{ui::call("count", {plate.selection})} > 0)
        .a11y("Selected wells")
        .testId("bulk");
}

// ── Actions and the assign dialog ────────────────────────────────────────────────────────────────────────────────

// Whether auto-fill has anything to place and anywhere to place it.
ui::Handle<bool> canAutoFill(Plate const& plate) {
    return Count{plate.toPlaceCount} > 0 && Count{ui::call("count", {plate.emptyWells})} > 0;
}

ui::Commands saveCommands(Plate const& plate) {
    return {ui::notify(ui::format("Layout of PL-0193 saved: {0} samples to place.", {plate.toPlaceCount}))};
}

// Compact's two large buttons; the expanded layout's are head actions beside the template.
ui::Node autoFill(Plate const& plate) {
    return ui::button({.label = "Auto-fill samples", .onClick = fillWells(plate, plate.emptyWells)})
        .enabled(canAutoFill(plate))
        .testId("auto-fill");
}

ui::Node saveLayout(Plate const& plate) {
    return ui::button({.label = "Save layout", .emphasis = ButtonEmphasis::Primary, .onClick = saveCommands(plate)})
        .testId("save");
}

// The expanded layout's head: the template, a head tool, and the two actions.
void declareHead(ui::Scope& screen, Plate const& plate) {
    screen.tool(ui::HeadSelectOptions{
        .id = "template",
        .label = "Template",
        .options = ui::list({ui::obj({{"key", "hcp96"}, {"label", "Template: HCP 96, 8 standards"}})}),
        .selected = plate.layoutTemplate,
        .onChoose = {ui::set(plate.layoutTemplate, ui::event())},
        .width = 30,
        .visible = !plate.compact});
    screen.action({.id = "autoFill",
                   .label = "Auto-fill samples",
                   .enabled = canAutoFill(plate),
                   .visible = !plate.compact,
                   .onClick = fillWells(plate, plate.emptyWells)});
    screen.action({.id = "save",
                   .label = "Save layout",
                   .emphasis = ButtonEmphasis::Primary,
                   .visible = !plate.compact,
                   .onClick = saveCommands(plate)});
}

ui::Node assignDialog(Plate const& plate) {
    auto const wells = ui::map(plate.emptyWells, "well",
                               [](ui::AnyHandle const& well) { return ui::obj({{"key", well}, {"label", well}}); });
    auto const samples = ui::map(plate.toPlace, "item", [](ui::AnyHandle const& item) {
        return ui::obj({{"key", item.at("n")}, {"label", fullId(item.at("n"))}});
    });
    auto const ready = Flag{ui::engaged(plate.assignWell)} && Flag{ui::engaged(plate.assignSample)};
    return ui::dialog(plate.assignDialog,
                      {layout::captioned(ui::select({.options = samples,
                                                     .selected = plate.assignSample,
                                                     .label = "Sample",
                                                     .onChoose = ui::set(plate.assignSample, ui::event())}))
                           .required(true)
                           .testId("assign-sample"),
                       layout::captioned(ui::select({.options = wells,
                                                     .selected = plate.assignWell,
                                                     .label = "Well",
                                                     .onChoose = ui::set(plate.assignWell, ui::event())}))
                           .required(true)
                           .testId("assign-well")},
                      {.title = "Assign a sample to a well",
                       .cancel = ui::close(ui::self),
                       .actions = {{.id = "cancel", .label = "Cancel", .onClick = ui::close(ui::self)},
                                   {.id = "assign",
                                    .label = "Assign",
                                    .emphasis = ButtonEmphasis::Primary,
                                    .enabled = ready,
                                    .onClick = {ui::seq(place(plate, plate.assignWell, plate.assignSample)),
                                                ui::close(ui::self)}}}});
}

// ── Expanded ─────────────────────────────────────────────────────────────────────────────────────────────────────

ui::Node expandedLayout(Plate const& plate) {
    auto const plateColumn = ui::column(
        {selectionBar(plate), ui::panel({plateGrid(plate)}, {.padding = 2}), legend(),
         layout::muted("Click selects a well. Arrow keys move between wells, Shift with an arrow key extends the "
                       "selection, and Enter assigns a sample to the focused well.")},
        {.gap = 2});
    return ui::column({ui::row({sources(plate).sizing(ui::fixed(30)), plateColumn.sizing(ui::stretch())},
                               {.gap = 2, .align = Align::Start}),
                       assignDialog(plate)},
                      {.gap = 2});
}

// ── Compact: the wells listed by row ─────────────────────────────────────────────────────────────────────────────

// A plate row's wells as records: the well's name, its line's text, the label and surface of its marker, and `run`,
// which adjacent empty sample wells share so they list as one line; every other well has a run of its own.
ui::AnyHandle rowWells(Plate const& plate, std::size_t row) {
    std::vector<ui::AnyHandle> wells;
    for (int column = 1; column <= kColumns; ++column) {
        Well const& well = wellOf(row, column);
        if (well.kind != Kind::Sample) {
            wells.push_back(ui::obj({{"name", well.name},
                                     {"text", well.description},
                                     {"label", well.label},
                                     {"surface", well.surface},
                                     {"run", well.name}}));
            continue;
        }
        wells.push_back(ui::obj({{"name", well.name},
                                 {"text", plate.lines.at(well.name)},
                                 {"label", plate.labels.at(well.name)},
                                 {"surface", plate.surfaces.at(well.name)},
                                 {"run", plate.runs.at(well.name)}}));
    }
    return ui::list(wells);
}

// One run of a plate row: a well's line, or "A5–A8 · Empty, 4 wells" with one Assign for the run's first well.
ui::Node runLine(Plate const& plate) {
    auto const first = ui::row("first.name");
    auto const count = Count{ui::row("count")};
    Flag const empty = ui::eq(ui::row("key"), "empty");
    Flag const several = count > 1;
    // The run's first well, as the grid draws it: a tap on a sample well selects it.
    auto marker = wellOptions(ui::row("first.label"));
    marker.onClick = ui::Commands{ui::ifThen(ui::includes(plate.sampleWells, first), toggleWell(plate, first))};
    marker.selected = ui::includes(plate.selection, first);
    return ui::row(
        {ui::button(marker).surface(ui::row("first.surface")).a11y(ui::format("Select {0}", {first})).testId("marker"),
         ui::text({.text = ui::cond(several, Text{ui::format("{0}–{1}", {first, ui::row("last.name")})}, Text{first}),
                   .role = "section"})
             .sizing(ui::fixed(8)),
         ui::text({.text = ui::cond(empty && several, Text{ui::format("Empty, {0} wells", {count})},
                                    Text{ui::row("first.text")})})
             .sizing(ui::stretch())
             .testId("line"),
         ui::button({.label = "Assign…", .onClick = openAssign(plate, first, firstToPlace(plate))})
             .visible(empty)
             .a11y(ui::format("Assign a sample to {0}", {first}))
             .testId("assign")},
        {.gap = 1, .align = Align::Center});
}

// "Row B · 1 standard, 1 blank, 10 samples": the template's wells, then how many sample wells are filled and empty.
ui::AnyHandle rowTitle(Plate const& plate, std::size_t row) {
    std::vector<ui::AnyHandle> filled;
    std::int64_t sampleWells = 0;
    std::int64_t controls = 0;
    bool blank = false;
    for (int column = 1; column <= kColumns; ++column) {
        Well const& well = wellOf(row, column);
        if (well.kind == Kind::Sample) {
            filled.push_back(plate.samples.at(well.name));
            ++sampleWells;
        }
        controls += well.kind == Kind::Control ? 1 : 0;
        blank = blank || well.kind == Kind::Blank;
    }
    std::string fixedPart = std::string{"Row "} + kRowNames.at(row) + " · 1 standard";
    fixedPart += controls > 0 ? ", " + std::to_string(controls) + " controls" : "";
    fixedPart += blank ? ", 1 blank" : "";
    auto const count = Count{ui::call(
        "count", {ui::filter(ui::list(filled), "item", [](ui::AnyHandle const& item) { return ui::engaged(item); })})};
    auto const empty = Count{sampleWells} - count;
    auto const part = [](Count const& number, char const* one, char const* many) {
        return ui::cond(ui::eq(number, std::int64_t{0}), Text{""},
                        ui::cond(ui::eq(number, std::int64_t{1}), Text{one}, Text{ui::format(many, {number})}));
    };
    return ui::call("concat", {ui::AnyHandle{fixedPart}, part(count, ", 1 sample", ", {0} samples"),
                               part(empty, ", 1 empty", ", {0} empty")});
}

ui::Node rowSection(Plate const& plate, std::size_t row) {
    auto const runs = ui::runs(rowWells(plate, row), "well", [](ui::AnyHandle const& well) { return well.at("run"); });
    auto const name = ui::AnyHandle{kRowNames.at(row)};
    return ui::collapsible(
               {ui::forEach(runs, {.key = ui::row("first.name"), .gap = 1},
                            [&plate](ui::DeclScope&) { return ui::Nodes{runLine(plate)}; })
                    .testId(std::string{"lines-"} + kRowNames.at(row))},
               {.title = rowTitle(plate, row),
                .open = ui::eq(plate.openRow, name),
                .onToggle = ui::set(plate.openRow, ui::cond(ui::Handle<bool>{ui::event()}, name, ui::AnyHandle{""}))})
        .testId(std::string{"row-"} + kRowNames.at(row));
}

ui::Node compactLayout(Plate const& plate) {
    std::vector<ui::Node> rows;
    rows.reserve(kRowNames.size());
    for (std::size_t row = 0; row < kRowNames.size(); ++row) {
        rows.push_back(rowSection(plate, row));
    }
    return ui::column(
        {layout::muted(ui::format("{0} samples to place", {plate.toPlaceCount})).testId("to-place-count"),
         ui::row({autoFill(plate).sizing(ui::stretch()), saveLayout(plate).sizing(ui::stretch())}, {.gap = 1}),
         ui::column(rows, {.gap = 1}), assignDialog(plate)},
        {.gap = 2});
}

}  // namespace

ui::Screen plateScreen() {
    return ui::screen("lims-gallery.plate", [](ui::Scope& screen) {
        Plate const plate = declare(screen);
        screen.title("Plate PL-0193");
        screen.subtitle(ui::cond(plate.compact, Text{"ELISA, host cell protein"},
                                 Text{"ELISA, host cell protein · worksheet WS-2026-0415"}));
        screen.subtitleSpans(ui::cond(plate.compact, ui::spans({"ELISA, host cell protein"}),
                                      ui::spans({"ELISA, host cell protein · worksheet ", ui::mono("WS-2026-0415")})));
        declareHead(screen, plate);
        return layout::byWidth(plate.width, compactLayout(plate), expandedLayout(plate));
    });
}

}  // namespace lims_gallery

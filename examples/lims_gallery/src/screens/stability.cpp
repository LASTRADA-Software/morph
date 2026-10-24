// SPDX-License-Identifier: Apache-2.0
//
// Design 06, setting up a stability study: a four-step wizard whose second step, the one the design draws, chooses
// the storage conditions and the timepoints pulled at each, with a summary that follows every choice. The containers
// a study needs are its pulls times the containers per pull; more than the lot has in stock is an error that keeps
// the wizard on this step.

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "layout.hpp"
#include "lims_gallery/screens.hpp"

namespace lims_gallery {

namespace {

namespace ui = layout::ui;
using layout::Align;
using layout::ButtonEmphasis;
using layout::Justify;
using Text = ui::Handle<std::string>;
using Flag = ui::Handle<bool>;
using Count = ui::Handle<std::int64_t>;

// The containers of lot L-26-0381 in stock.
constexpr std::int64_t kInStock = 150;

struct Timepoint {
    std::int64_t month;
    char const* header;
    char const* label;
    // The pull's date from the design's T0, 03-Nov-2026. The expression language has no date arithmetic, so the
    // dates do not follow a new T0.
    char const* date;
};

constexpr std::array<Timepoint, 9> kTimepoints{
    {{.month = 0, .header = "T0", .label = "T0", .date = "03-Nov-2026"},
     {.month = 1, .header = "1", .label = "1 m", .date = "03-Dec-2026"},
     {.month = 3, .header = "3", .label = "3 m", .date = "03-Feb-2027"},
     {.month = 6, .header = "6", .label = "6 m", .date = "03-May-2027"},
     {.month = 9, .header = "9", .label = "9 m", .date = "03-Aug-2027"},
     {.month = 12, .header = "12", .label = "12 m", .date = "03-Nov-2027"},
     {.month = 18, .header = "18", .label = "18 m", .date = "03-May-2028"},
     {.month = 24, .header = "24", .label = "24 m", .date = "03-Nov-2028"},
     {.month = 36, .header = "36", .label = "36 m", .date = "03-Nov-2029"}}};

constexpr std::array<char const*, 4> kSteps{"Product and lot", "Conditions and timepoints", "Tests per timepoint",
                                            "Review and sign"};

ui::AnyHandle months(std::vector<std::int64_t> const& chosen) { return ui::AnyHandle{chosen}; }

ui::AnyHandle condition(char const* key, char const* climate, char const* kind, char const* temperature,
                        std::vector<std::int64_t> const& chosen) {
    return ui::obj(
        {{"id", key}, {"climate", climate}, {"kind", kind}, {"short", temperature}, {"points", months(chosen)}});
}

// The ICH Q1A set the study opens with.
ui::AnyHandle ichConditions() {
    return ui::list({condition("lt", "25 °C / 60 % RH", "long term", "25 °C", {0, 3, 6, 9, 12, 18, 24, 36}),
                     condition("int", "30 °C / 65 % RH", "intermediate", "30 °C", {3, 6, 9, 12}),
                     condition("acc", "40 °C / 75 % RH", "accelerated", "40 °C", {1, 3, 6})});
}

struct Study {
    ui::Element perPullField = ui::element("perPullInput");
    ui::AnyHandle width;
    Flag compact;
    Count step;
    Flag attempted;
    Text start;
    Text window;
    Text perPull;
    ui::AnyHandle conditions;
    Text openCondition;
    Count pulls;
    ui::AnyHandle needed;
    Flag enough;
    ui::AnyHandle firstDate;
    ui::AnyHandle firstPull;
    ui::AnyHandle lastPull;
};

// The months some condition pulls, T0 left out: a pull is a timepoint after the study starts.
ui::AnyHandle pulledMonths(Study const& study) {
    std::vector<std::int64_t> later;
    for (Timepoint const& point : kTimepoints) {
        if (point.month > 0) {
            later.push_back(point.month);
        }
    }
    return ui::filter(months(later), "month", [&study](ui::AnyHandle const& month) -> ui::AnyHandle {
        return ui::call("any", {ui::map(study.conditions, "condition", [&month](ui::AnyHandle const& item) {
                            return ui::includes(item.at("points"), month);
                        })});
    });
}

ui::AnyHandle dateOf(ui::AnyHandle const& month) {
    std::vector<ui::AnyHandle> dates;
    for (Timepoint const& point : kTimepoints) {
        dates.push_back(ui::obj({{"month", point.month}, {"date", point.date}}));
    }
    // A field is read only below a name, so the date is mapped out before `first` picks it.
    auto const matching = ui::filter(
        ui::list(dates), "entry", [&month](ui::AnyHandle const& entry) { return ui::eq(entry.at("month"), month); });
    return ui::call("first",
                    {ui::map(matching, "entry", [](ui::AnyHandle const& entry) { return entry.at("date"); })});
}

Study declare(ui::Scope& screen) {
    Study study;
    study.width = layout::widthClass(screen);
    study.compact = screen.let("compact", ui::eq(study.width, "compact"));
    study.step = screen.state("step", std::int64_t{1});
    // The design draws the step after Continue was pressed once with too few containers.
    study.attempted = screen.state("attempted", true);
    study.start = screen.state("start", std::string{"2026-11-03"});
    study.window = screen.state("window", std::string{"7"});
    study.perPull = screen.state("perPull", std::string{"12"});
    study.conditions = screen.state<ui::Any>("conditions", ichConditions());
    study.openCondition = screen.state("openCondition", std::string{"lt"});
    study.pulls = screen.let(
        "pulls", Count{ui::call("sum", {ui::map(study.conditions, "condition", [](ui::AnyHandle const& item) {
                                    return ui::call("count", {item.at("points")});
                                })})});
    auto const perPull = screen.let("perPullCount", ui::parseDecimal(study.perPull));
    study.needed = screen.let("needed", study.pulls * perPull);
    study.enough = screen.let("enough", Flag{ui::engaged(perPull)} && study.needed <= kInStock);
    auto const pulled = screen.let("pulledMonths", pulledMonths(study));
    auto const first = screen.let("firstMonth", ui::call("min", {pulled}));
    auto const last = screen.let("lastMonth", ui::call("max", {pulled}));
    auto const firstCondition = ui::call(
        "first",
        {ui::map(ui::filter(study.conditions, "condition",
                            [&first](ui::AnyHandle const& item) { return ui::includes(item.at("points"), first); }),
                 "condition", [](ui::AnyHandle const& item) { return item.at("short"); })});
    study.firstDate = screen.let("firstDate", dateOf(first));
    study.firstPull =
        screen.let("firstPull", ui::format("{0} ({1}, {2} m)", {study.firstDate, firstCondition, first}));
    study.lastPull = screen.let("lastPull", dateOf(last));
    return study;
}

// ── The conditions and their timepoints ──────────────────────────────────────────────────────────────────────────

// The row's condition with @p month added to its pulls or taken off them, as the check box now says.
ui::Commands toggleMonth(Study const& study, std::int64_t month) {
    auto const updated = ui::map(study.conditions, "condition", [month](ui::AnyHandle const& item) -> ui::AnyHandle {
        auto const points = item.at("points");
        auto const without = ui::filter(
            points, "month", [month](ui::AnyHandle const& point) -> ui::AnyHandle { return ui::ne(point, month); });
        auto const toggled =
            ui::cond(ui::Handle<bool>{ui::event()}, ui::concatLists({points, months({month})}), without);
        return ui::cond(ui::eq(item.at("id"), ui::row("id")),
                        ui::obj({{"id", item.at("id")},
                                 {"climate", item.at("climate")},
                                 {"kind", item.at("kind")},
                                 {"short", item.at("short")},
                                 {"points", toggled}}),
                        item);
    });
    return ui::set(study.conditions, updated);
}

// The check box of one condition's timepoint, in the condition's row scope.
ui::Node monthBox(Study const& study, Timepoint const& point, bool labelled) {
    return ui::checkbox({.label = labelled ? point.label : "",
                         .checked = ui::includes(ui::row("points"), ui::AnyHandle{point.month}),
                         .onToggle = toggleMonth(study, point.month)})
        .a11y(ui::format("{0} {1}", {ui::row("short"), ui::AnyHandle{point.label}}))
        .testId(std::string{"month-"} + point.header);
}

ui::Node matrix(Study const& study) {
    std::vector<ui::TableColumn> columns{
        {.name = "condition",
         .label = "Condition",
         .width = ui::stretch(),
         .cell = ui::text({.text = ui::format("{0} · {1}", {ui::row("climate"), ui::row("kind")})})}};
    for (Timepoint const& point : kTimepoints) {
        columns.push_back(ui::TableColumn{.name = std::string{"m"} + point.header,
                                          .label = point.header,
                                          .width = ui::fixed(4),
                                          .cell = monthBox(study, point, false)});
    }
    columns.push_back(ui::TableColumn{
        .name = "pulls",
        .label = "Pulls",
        .width = ui::fixed(5),
        .cell = ui::text({.text = ui::format("{0}", {ui::call("count", {ui::row("points")})})}).testId("row-pulls")});
    return ui::table(study.conditions, {.key = ui::row("id"), .columns = columns})
        .a11y("Storage conditions and timepoints")
        .testId("matrix");
}

ui::Commands applyTemplate(Study const& study) { return ui::set(study.conditions, ichConditions()); }

ui::Node addCondition(Study const& study) {
    auto const keys = ui::map(study.conditions, "condition", [](ui::AnyHandle const& item) { return item.at("id"); });
    return ui::button({.label = "Add condition",
                       .onClick = ui::set(
                           study.conditions,
                           ui::concatLists({study.conditions,
                                            ui::list({condition("rf", "5 °C ± 3 °C", "refrigerated", "5 °C", {})})}))})
        .enabled(!ui::includes(keys, "rf"))
        .testId("add-condition");
}

// ── The fields ───────────────────────────────────────────────────────────────────────────────────────────────────

ui::Node startField(Study const& study) {
    return layout::captioned(ui::dateTimeInput({.value = study.start,
                                                .label = "Study start (T0)",
                                                .mode = "date",
                                                .onChange = ui::set(study.start, ui::event())}))
        .required(true)
        .testId("start");
}

ui::Node windowField(Study const& study) {
    return layout::captioned(ui::textInput({.value = study.window,
                                            .label = "Pull window, ± days",
                                            .onChange = ui::set(study.window, ui::event())}))
        .required(true)
        .testId("window");
}

ui::Node perPullField(Study const& study, bool compact) {
    auto const message = compact ? ui::format("{0} units needed, {1} in stock for lot L-26-0381.",
                                              {study.needed, ui::AnyHandle{kInStock}})
                                 : ui::format("{0} pulls × {1} = {2} units needed, {3} in stock for lot L-26-0381.",
                                              {study.pulls, study.perPull, study.needed, ui::AnyHandle{kInStock}});
    auto const errors = ui::cond(study.enough, ui::AnyHandle{std::vector<std::string>{}}, ui::list({message}));
    return layout::captioned(ui::textInput({.value = study.perPull,
                                            .label = compact ? "Per pull, units" : "Containers per pull, units",
                                            .onChange = ui::set(study.perPull, ui::event())}))
        .id(study.perPullField)
        .required(true)
        .errors(errors)
        .testId("per-pull");
}

ui::Node attention(Study const& study) {
    return ui::banner({.tone = "err",
                       .text = "1 field needs attention: Containers per pull.",
                       .action = ui::ActionPart{.label = "Go to the field", .onClick = ui::focus(study.perPullField)}})
        .visible(study.attempted && !study.enough)
        .testId("attention");
}

ui::Node summary(Study const& study) {
    auto const containers = ui::format("{0} needed, {1} in stock", {study.needed, ui::AnyHandle{kInStock}});
    return ui::keyValue(ui::list({ui::obj({{"label", "Product"}, {"value", "Paracetamol 500 mg"}}),
                                  ui::obj({{"label", "Lot"}, {"value", "L-26-0381"}}),
                                  ui::obj({{"label", "Protocol"}, {"value", "STP-0114 v2"}}),
                                  ui::obj({{"label", "Pulls"}, {"value", ui::format("{0}", {study.pulls})}}),
                                  ui::obj({{"label", "First pull"}, {"value", study.firstPull}}),
                                  ui::obj({{"label", "Last pull"}, {"value", study.lastPull}}),
                                  ui::obj({{"label", "Containers"}, {"value", containers}})}))
        .testId("summary");
}

// ── The other steps, which the design does not draw ──────────────────────────────────────────────────────────────

ui::Nodes otherStep(char const* text) { return {layout::muted(text)}; }

ui::Node stepPage(Study const& study, ui::Node const& conditionsStep) {
    return ui::switchOn(
        study.step,
        {ui::Case{ui::AnyHandle{std::int64_t{0}},
                  {ui::keyValue(ui::list({ui::obj({{"label", "Product"}, {"value", "Paracetamol 500 mg tablets"}}),
                                          ui::obj({{"label", "Lot"}, {"value", "L-26-0381"}}),
                                          ui::obj({{"label", "Protocol"}, {"value", "STP-0114 v2"}})})),
                   layout::muted("The design draws step 2; this step shows what it chose.")}},
         ui::Case{ui::AnyHandle{std::int64_t{1}}, conditionsStep},
         ui::Case{ui::AnyHandle{std::int64_t{2}},
                  otherStep("Tests per timepoint: the design draws step 2 only. Back returns to the conditions.")}},
        otherStep("Review and sign: the design draws step 2 only. Signing creates the study."));
}

ui::AnyHandle stepItems(Study const& study, bool labelled) {
    std::vector<ui::AnyHandle> items;
    std::int64_t index = 0;
    for (char const* const label : kSteps) {
        auto const state = ui::cond(study.step > index, Text{"done"},
                                    ui::cond(ui::eq(study.step, index), Text{"active"}, Text{"pending"}));
        items.push_back(ui::obj({{"label", labelled ? label : ""}, {"state", state}}));
        ++index;
    }
    return ui::list(items);
}

ui::Commands next(Study const& study) {
    return ui::ifThen(ui::eq(study.step, 1) && !study.enough,
                      // The field is in the step's switch case, a scope the footer cannot name, so Continue cannot
                      // focus it; the banner's action, inside the case, does.
                      ui::set(study.attempted, true),
                      {ui::ifThen(study.step < 3, ui::set(study.step, study.step + 1),
                                  ui::notify("Signing creates the study STB-2026-014"))});
}

ui::Node backButton(Study const& study) {
    return ui::button({.label = "Back", .onClick = ui::set(study.step, study.step - 1)})
        .enabled(study.step > 0)
        .testId("back");
}

ui::AnyHandle continueLabel(Study const& study) {
    return ui::cond(ui::eq(study.step, 1), Text{"Continue to tests"},
                    ui::cond(ui::eq(study.step, 3), Text{"Sign and create"}, Text{"Continue"}));
}

// ── Expanded ─────────────────────────────────────────────────────────────────────────────────────────────────────

ui::Node expandedConditions(Study const& study) {
    auto const fields = ui::panel(
        {ui::grid({startField(study), windowField(study), perPullField(study, false)}, {.gap = 2, .columns = 3})},
        {.padding = 2});
    auto const conditions =
        ui::panel({ui::row({layout::section("Storage conditions and timepoints (months)").sizing(ui::stretch()),
                            ui::button({.label = "Apply ICH Q1A template",
                                        .emphasis = ButtonEmphasis::Ghost,
                                        .onClick = applyTemplate(study)})
                                .testId("template"),
                            addCondition(study)},
                           {.gap = 1, .align = Align::Center}),
                   matrix(study), layout::muted("T0 is tested once, from the long-term condition.")},
                  {.padding = 2});
    auto const aside = ui::panel({summary(study)}, {.title = "Summary", .padding = 2})
                           // The design's 250 px summary.
                           .sizing(ui::fixed(33))
                           .a11y("Summary");
    return ui::row({ui::column({attention(study), fields, conditions}, {.gap = 2}).sizing(ui::stretch()), aside},
                   {.gap = 2, .align = Align::Start});
}

ui::Node expandedLayout(Study const& study) {
    return ui::column(
        {ui::steps(stepItems(study, true), study.step).testId("steps"), stepPage(study, expandedConditions(study)),
         ui::row(
             {backButton(study), ui::spacer().sizing(ui::stretch()),
              ui::button({.label = "Save draft",
                          .emphasis = ButtonEmphasis::Ghost,
                          .onClick = ui::notify("The study draft is saved")}),
              ui::button({.label = continueLabel(study), .emphasis = ButtonEmphasis::Primary, .onClick = next(study)})
                  .testId("continue")},
             {.gap = 1})},
        {.gap = 3});
}

// ── Compact: one condition at a time, every field kept ───────────────────────────────────────────────────────────

ui::Node conditionSection(Study const& study) {
    std::vector<ui::Node> boxes;
    for (Timepoint const& point : kTimepoints) {
        boxes.push_back(monthBox(study, point, true));
    }
    auto const rowId = ui::row("id");
    return ui::collapsible(
        {ui::grid(boxes, {.gap = 1, .columns = 3})},
        {.title = ui::format("{0} · {1} pulls", {ui::row("climate"), ui::call("count", {ui::row("points")})}),
         .open = ui::eq(study.openCondition, rowId),
         .onToggle = ui::set(study.openCondition,
                             ui::cond(ui::Handle<bool>{ui::event()}, ui::AnyHandle{rowId}, ui::AnyHandle{""}))});
}

ui::Node compactConditions(Study const& study) {
    return ui::column(
        {attention(study), startField(study),
         ui::grid({windowField(study), perPullField(study, true)}, {.gap = 2, .columns = 2}),
         ui::column({ui::forEach(study.conditions, {.key = ui::row("id"), .gap = 1},
                                 [&study](ui::DeclScope&) { return ui::Nodes{conditionSection(study)}; })})
             .testId("conditions"),
         addCondition(study),
         ui::collapsible({summary(study)}, {.title = ui::format("Summary · first pull {0}", {study.firstDate})})
             .testId("summary-section")},
        {.gap = 2});
}

ui::Node compactLayout(Study const& study) {
    return ui::column(
        {layout::muted(ui::format("Step {0} of 4 · Paracetamol 500 mg", {study.step + 1})),
         ui::steps(stepItems(study, false), study.step).testId("steps"), stepPage(study, compactConditions(study)),
         ui::row({backButton(study).sizing(ui::fixed(12)),
                  ui::button({.label = ui::cond(ui::eq(study.step, 3), Text{"Sign and create"}, Text{"Continue"}),
                              .emphasis = ButtonEmphasis::Primary,
                              .onClick = next(study)})
                      .sizing(ui::stretch())
                      .testId("continue")},
                 {.gap = 1})},
        {.gap = 2});
}

}  // namespace

ui::Screen stabilityScreen() {
    return ui::screen("lims-gallery.stability", [](ui::Scope& screen) {
        Study const study = declare(screen);
        auto const stepName =
            ui::cond(ui::eq(study.step, 0), Text{kSteps.at(0)},
                     ui::cond(ui::eq(study.step, 1), Text{kSteps.at(1)},
                              ui::cond(ui::eq(study.step, 2), Text{kSteps.at(2)}, Text{kSteps.at(3)})));
        screen.title(ui::cond(study.compact, stepName, Text{"New stability study · Paracetamol 500 mg tablets"}));
        screen.showErrors(ui::ShowErrors::Always);
        return layout::byWidth(study.width, compactLayout(study), expandedLayout(study));
    });
}

}  // namespace lims_gallery

// SPDX-License-Identifier: Apache-2.0
//
// Design 06, setting up a stability study: a four-step wizard. The design draws the second step, which chooses the
// storage conditions and the timepoints pulled at each, with a summary that follows every choice; the first picks the
// lot, the third the tests run at each pull, and the last reviews the study before it is signed. The containers a
// study needs are its pulls times the containers per pull; more than the chosen lot has in stock is an error that
// keeps the wizard on the second step.

#include <algorithm>
#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "lims_gallery/screens.hpp"
#include "screen_layout.hpp"

namespace lims_gallery {

namespace {

namespace ui = layout::ui;
using layout::Align;
using layout::ButtonEmphasis;
using Text = ui::Handle<std::string>;
using Flag = ui::Handle<bool>;
using Count = ui::Handle<std::int64_t>;

struct Lot {
    char const* id;
    char const* made;
    std::int64_t stock;
};

// The product's released lots and the containers of each in stock.
constexpr std::array<Lot, 3> kLots{{{.id = "L-26-0381", .made = "made 12-Oct-2026", .stock = 150},
                                    {.id = "L-26-0377", .made = "made 28-Sep-2026", .stock = 420},
                                    {.id = "L-26-0362", .made = "made 02-Sep-2026", .stock = 96}}};

struct Test {
    char const* key;
    char const* name;
    char const* method;
    bool included;
    bool everyPull;
};

// The tests of protocol STP-0114 v2. A test runs at every pull, or only at T0 and the last pull.
constexpr std::array<Test, 6> kTests{
    {{.key = "assay", .name = "Assay", .method = "HPLC, STP-0114-A", .included = true, .everyPull = true},
     {.key = "rs", .name = "Related substances", .method = "HPLC, STP-0114-B", .included = true, .everyPull = true},
     {.key = "diss", .name = "Dissolution", .method = "USP <711>, apparatus 2", .included = true, .everyPull = true},
     {.key = "water",
      .name = "Water content",
      .method = "Karl Fischer, STP-0091",
      .included = true,
      .everyPull = true},
     {.key = "appearance", .name = "Appearance", .method = "Visual, STP-0002", .included = true, .everyPull = true},
     {.key = "micro", .name = "Microbial limits", .method = "USP <61>, <62>", .included = false, .everyPull = false}}};

struct Timepoint {
    std::int64_t month;
    char const* header;
    char const* label;
};

constexpr std::array<Timepoint, 9> kTimepoints{{{.month = 0, .header = "T0", .label = "T0"},
                                                {.month = 1, .header = "1", .label = "1 m"},
                                                {.month = 3, .header = "3", .label = "3 m"},
                                                {.month = 6, .header = "6", .label = "6 m"},
                                                {.month = 9, .header = "9", .label = "9 m"},
                                                {.month = 12, .header = "12", .label = "12 m"},
                                                {.month = 18, .header = "18", .label = "18 m"},
                                                {.month = 24, .header = "24", .label = "24 m"},
                                                {.month = 36, .header = "36", .label = "36 m"}}};

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

ui::AnyHandle lotList() {
    std::vector<ui::AnyHandle> lots;
    lots.reserve(kLots.size());
    for (Lot const& lot : kLots) {
        lots.push_back(ui::obj({{"key", lot.id}, {"stock", lot.stock}}));
    }
    return ui::list(lots);
}

ui::AnyHandle testList() {
    std::vector<ui::AnyHandle> tests;
    tests.reserve(kTests.size());
    for (Test const& test : kTests) {
        tests.push_back(ui::obj({{"id", test.key},
                                 {"name", test.name},
                                 {"method", test.method},
                                 {"on", test.included},
                                 {"when", test.everyPull ? "every" : "ends"}}));
    }
    return ui::list(tests);
}

struct Study {
    // The per-pull field of each layout: the two layouts' steppers are separate switches, and a focus that reaches
    // into switch cases may name only one switch's.
    ui::Element perPullField = ui::element("perPullInput");
    ui::Element perPullCompact = ui::element("perPullCompact");
    ui::AnyHandle width;
    Flag compact;
    Count step;
    Flag attempted;
    Text lot;
    Text container;
    Text reason;
    Text start;
    Text window;
    Text perPull;
    ui::AnyHandle conditions;
    Text openCondition;
    ui::AnyHandle tests;
    Flag confirmed;
    ui::AnyHandle stock;
    Count pulls;
    ui::AnyHandle needed;
    Flag enough;
    ui::AnyHandle runs;
    ui::AnyHandle testCount;
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

// The date of the pull @p month months after the study's start, as the date functions show it.
ui::AnyHandle pullDate(ui::AnyHandle const& start, ui::AnyHandle const& month) {
    return ui::call("date", {ui::dateAdd(start, month, ui::DateUnit::Months)});
}

Study declare(ui::Scope& screen) {
    Study study;
    study.width = layout::widthClass(screen);
    study.compact = screen.let("compact", ui::eq(study.width, "compact"));
    study.step = screen.state("step", std::int64_t{1});
    // The design draws the step after Continue was pressed once with too few containers.
    study.attempted = screen.state("attempted", true);
    study.lot = screen.state("lot", std::string{kLots.at(0).id});
    study.container = screen.state("container", std::string{"bottle"});
    study.reason = screen.state("reason", std::string{"registration"});
    study.start = screen.state("start", std::string{"2026-11-03"});
    study.window = screen.state("window", std::string{"7"});
    study.perPull = screen.state("perPull", std::string{"12"});
    study.conditions = screen.state<ui::Any>("conditions", ichConditions());
    study.openCondition = screen.state("openCondition", std::string{"lt"});
    study.tests = screen.state<ui::Any>("tests", testList());
    study.confirmed = screen.state("confirmed", false);
    study.stock = screen.let("stock", ui::call("first", {ui::filter(lotList(), "entry",
                                                                    [&study](ui::AnyHandle const& entry) {
                                                                        return ui::eq(entry.at("key"), study.lot);
                                                                    })})
                                          .at("stock"));
    study.pulls = screen.let(
        "pulls", Count{ui::call("sum", {ui::map(study.conditions, "condition", [](ui::AnyHandle const& item) {
                                    return ui::call("count", {item.at("points")});
                                })})});
    auto const perPull = screen.let("perPullCount", ui::parseDecimal(study.perPull));
    study.needed = screen.let("needed", study.pulls * perPull);
    study.enough = screen.let("enough", Flag{ui::engaged(perPull)} && study.needed <= study.stock);
    // A test at every pull runs once per pull; one at T0 and the last pull runs twice.
    study.runs = screen.let(
        "runs", ui::call("sum", {ui::map(study.tests, "test", [&study](ui::AnyHandle const& test) -> ui::AnyHandle {
                             return ui::cond(ui::Handle<bool>{test.at("on")},
                                             ui::cond(ui::eq(test.at("when"), "every"), ui::AnyHandle{study.pulls},
                                                      ui::AnyHandle{std::int64_t{2}}),
                                             ui::AnyHandle{std::int64_t{0}});
                         })}));
    study.testCount = screen.let(
        "testCount",
        ui::call("count", {ui::filter(study.tests, "test", [](ui::AnyHandle const& test) { return test.at("on"); })}));
    auto const pulled = screen.let("pulledMonths", pulledMonths(study));
    auto const first = screen.let("firstMonth", ui::call("min", {pulled}));
    auto const last = screen.let("lastMonth", ui::call("max", {pulled}));
    auto const firstCondition =
        ui::call("first",
                 {ui::filter(study.conditions, "condition",
                             [&first](ui::AnyHandle const& item) { return ui::includes(item.at("points"), first); })})
            .at("short");
    study.firstDate = screen.let("firstDate", pullDate(study.start, first));
    study.firstPull =
        screen.let("firstPull", ui::format("{0} ({1}, {2} m)", {study.firstDate, firstCondition, first}));
    study.lastPull = screen.let("lastPull", pullDate(study.start, last));
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
                                          .width = ui::fixed(3),
                                          .align = Align::Center,
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

ui::Node addCondition(Study const& study, bool compact = false) {
    auto const keys = ui::map(study.conditions, "condition", [](ui::AnyHandle const& item) { return item.at("id"); });
    auto const button =
        ui::button({.label = "Add condition",
                    .onClick = ui::set(
                        study.conditions,
                        ui::concatLists({study.conditions,
                                         ui::list({condition("rf", "5 °C ± 3 °C", "refrigerated", "5 °C", {})})}))})
            .enabled(!ui::includes(keys, "rf"))
            .testId("add-condition");
    return compact ? button.sizing(ui::stretch()) : button;
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
                                            .label = "Pull window",
                                            .unit = "days",
                                            .prefix = "±",
                                            .onChange = ui::set(study.window, ui::event())}))
        .required(true)
        .testId("window");
}

ui::AnyHandle perPullMessage(Study const& study, bool compact) {
    return compact ? ui::format("{0} units needed, {1} in stock for lot {2}.", {study.needed, study.stock, study.lot})
                   : ui::format("{0} pulls × {1} = {2} units needed, {3} in stock for lot {4}.",
                                {study.pulls, study.perPull, study.needed, study.stock, study.lot});
}

// Expanded, the field carries its message. Compact, the field is outlined and the message spans both fields of the
// row, under it.
ui::Node perPullField(Study const& study, bool compact) {
    auto const field = layout::captioned(ui::textInput({.value = study.perPull,
                                                        .label = compact ? "Per pull" : "Containers per pull",
                                                        .unit = "units",
                                                        .onChange = ui::set(study.perPull, ui::event())}))
                           .id(compact ? study.perPullCompact : study.perPullField)
                           .required(true)
                           .testId("per-pull");
    if (compact) {
        return field.tone(ui::cond(study.enough, Text{"neutral"}, Text{"err"}));
    }
    return field.errors(
        ui::cond(study.enough, ui::AnyHandle{std::vector<std::string>{}}, ui::list({perPullMessage(study, false)})));
}

ui::Node attention(Study const& study, bool compact) {
    return ui::banner(
               {.tone = "err",
                .text = "1 field needs attention: Containers per pull.",
                .spans = ui::spans({"1 field needs attention: ", ui::actionRun("Containers per pull"), "."}),
                .action = ui::ActionPart{.label = "Containers per pull",
                                         .onClick = ui::focus(compact ? study.perPullCompact : study.perPullField)},
                .actionStyle = morph::ui::BannerActionStyle::Link})
        .visible(study.attempted && !study.enough)
        .testId("attention");
}

// Captions beside their values, then @p toned, pairs written whole.
// The captions in @p monoLabels have their value drawn in the monospace face, as identifiers and dates are.
ui::Node pairs(std::vector<std::pair<char const*, ui::AnyHandle>> const& items, std::string const& testId,
               std::vector<ui::AnyHandle> const& toned = {}, std::vector<std::string_view> const& monoLabels = {}) {
    std::vector<ui::AnyHandle> listed;
    listed.reserve(items.size() + toned.size());
    for (auto const& [label, value] : items) {
        if (std::ranges::find(monoLabels, std::string_view{label}) != monoLabels.end()) {
            listed.push_back(ui::obj({{"label", label}, {"value", value}, {"spans", ui::spans({ui::mono(value)})}}));
        } else {
            listed.push_back(ui::obj({{"label", label}, {"value", value}}));
        }
    }
    listed.insert(listed.end(), toned.begin(), toned.end());
    return ui::keyValue(ui::list(listed)).testId(testId);
}

// The containers the study needs against the stock, red and bold while the stock is short.
ui::AnyHandle containersPair(Study const& study) {
    return ui::obj({{"label", "Containers"},
                    {"value", ui::format("{0} needed, {1} in stock", {study.needed, study.stock})},
                    {"tone", ui::cond(study.enough, Text{"neutral"}, Text{"err"})}});
}

ui::Node summary(Study const& study) {
    return ui::column(
               {pairs({{"Product", "Paracetamol 500 mg"},
                       {"Lot", study.lot},
                       {"Protocol", "STP-0114 v2"},
                       {"Pulls", ui::format("{0}", {study.pulls})},
                       {"First pull", study.firstPull},
                       {"Last pull", study.lastPull}},
                      "summary-pairs", {containersPair(study)}, {"Lot", "Protocol", "First pull", "Last pull"})},
               {.gap = 1})
        .testId("summary");
}

// ── Step 1: product and lot ──────────────────────────────────────────────────────────────────────────────────────

ui::Node lotField(Study const& study) {
    std::vector<ui::AnyHandle> options;
    options.reserve(kLots.size());
    for (Lot const& lot : kLots) {
        options.push_back(ui::obj({{"key", lot.id},
                                   {"label", std::string{lot.id} + " · " + lot.made + " · " +
                                                 std::to_string(lot.stock) + " containers in stock"}}));
    }
    return layout::captioned(ui::select({.options = ui::list(options),
                                         .selected = study.lot,
                                         .label = "Lot",
                                         .onChoose = ui::set(study.lot, ui::event())}))
        .required(true)
        .testId("lot");
}

ui::Node containerField(Study const& study) {
    return layout::captioned(
               ui::select(
                   {.options = ui::list({ui::obj({{"key", "bottle"}, {"label", "HDPE bottle, 30 tablets"}}),
                                         ui::obj({{"key", "blister"}, {"label", "PVC/Alu blister, 10 tablets"}})}),
                    .selected = study.container,
                    .label = "Container closure",
                    .onChoose = ui::set(study.container, ui::event())}))
        .required(true)
        .testId("container");
}

ui::Node reasonField(Study const& study) {
    return layout::captioned(
               ui::select({.options = ui::list({ui::obj({{"key", "registration"}, {"label", "Registration batch"}}),
                                                ui::obj({{"key", "commitment"}, {"label", "Annual commitment"}}),
                                                ui::obj({{"key", "change"}, {"label", "Post-change study"}})}),
                           .selected = study.reason,
                           .label = "Study reason",
                           .onChoose = ui::set(study.reason, ui::event())}))
        .required(true)
        .testId("reason");
}

ui::Node productStep(Study const& study, bool compact) {
    return ui::column({ui::panel({pairs({{"Product", "Paracetamol 500 mg tablets"},
                                         {"Protocol", "STP-0114 v2, ICH Q1A(R2) zones I and II"},
                                         {"Market", "EU, US"}},
                                        "product-pairs")},
                                 {.title = "Product", .padding = 2}),
                       ui::panel({ui::grid({lotField(study), containerField(study), reasonField(study)},
                                           {.gap = 2, .columns = compact ? 1 : 3})},
                                 {.title = "Lot and container", .padding = 2})},
                      {.gap = 2})
        .testId("product-step");
}

// ── Step 3: tests per timepoint ──────────────────────────────────────────────────────────────────────────────────

// The test's record with @p field replaced by the event, for the row the event came from.
ui::Commands updateTest(Study const& study, char const* field) {
    auto const updated = ui::map(study.tests, "test", [field](ui::AnyHandle const& test) -> ui::AnyHandle {
        auto const value = [&test, field](char const* name) {
            return std::string_view{name} == field ? ui::AnyHandle{ui::event()} : test.at(name);
        };
        return ui::cond(ui::eq(test.at("id"), ui::row("id")),
                        ui::obj({{"id", test.at("id")},
                                 {"name", test.at("name")},
                                 {"method", test.at("method")},
                                 {"on", value("on")},
                                 {"when", value("when")}}),
                        test);
    });
    return ui::set(study.tests, updated);
}

ui::Node includeBox(Study const& study, bool labelled) {
    return ui::checkbox({.label = labelled ? ui::row("name") : ui::AnyHandle{""},
                         .checked = ui::row("on"),
                         .onToggle = updateTest(study, "on")})
        .a11y(ui::format("Run {0}", {ui::row("name")}))
        .testId("include");
}

ui::Node whenSelect(Study const& study) {
    return ui::select({.options = ui::list({ui::obj({{"key", "every"}, {"label", "Every pull"}}),
                                            ui::obj({{"key", "ends"}, {"label", "T0 and last pull"}})}),
                       .selected = ui::row("when"),
                       .label = ui::format("When {0} runs", {ui::row("name")}),
                       .onChoose = updateTest(study, "when")})
        .enabled(Flag{ui::row("on")})
        .testId("when");
}

ui::AnyHandle runsOf(Study const& study) {
    return ui::cond(
        Flag{ui::row("on")},
        ui::cond(ui::eq(ui::row("when"), "every"), ui::AnyHandle{study.pulls}, ui::AnyHandle{std::int64_t{2}}),
        ui::AnyHandle{std::int64_t{0}});
}

ui::Node testTable(Study const& study) {
    std::vector<ui::TableColumn> const columns{
        {.name = "on", .label = "Run", .width = ui::fixed(6), .cell = includeBox(study, false)},
        {.name = "test", .label = "Test", .width = ui::stretch(), .cell = ui::text({.text = ui::row("name")})},
        {.name = "method",
         .label = "Method",
         .width = ui::stretch(),
         .cell = ui::text({.text = ui::row("method"), .role = "muted"})},
        {.name = "when", .label = "When", .width = ui::fixed(22), .cell = whenSelect(study)},
        {.name = "runs",
         .label = "Runs",
         .width = ui::fixed(6),
         .cell = ui::text({.text = ui::format("{0}", {runsOf(study)})}).testId("test-runs")}};
    return ui::table(study.tests, {.key = ui::row("id"), .columns = columns})
        .a11y("Tests per timepoint")
        .testId("tests");
}

ui::Node testCards(Study const& study) {
    return ui::forEach(
               study.tests, {.key = ui::row("id"), .gap = 1},
               [&study](ui::DeclScope&) {
                   return ui::Nodes{ui::panel({includeBox(study, true), layout::muted(ui::row("method")),
                                               ui::row({whenSelect(study).sizing(ui::stretch()),
                                                        layout::muted(ui::format("{0} runs", {runsOf(study)}))},
                                                       {.gap = 1, .align = Align::Center})},
                                              {.padding = 1})};
               })
        .testId("tests");
}

ui::Node testsStep(Study const& study, bool compact) {
    auto const total = layout::muted(ui::format("{0} tests, {1} test runs over {2} pulls.",
                                                {study.testCount, study.runs, study.pulls}))
                           .testId("runs");
    auto const none = ui::banner({.tone = "warn", .text = "Choose at least one test to continue."})
                          .visible(ui::eq(study.testCount, std::int64_t{0}))
                          .testId("no-tests");
    return ui::column({none, compact ? testCards(study) : testTable(study), total}, {.gap = 2}).testId("tests-step");
}

// ── Step 4: review and sign ──────────────────────────────────────────────────────────────────────────────────────

ui::Node reviewStep(Study const& study) {
    auto const conditions =
        ui::call("count", {ui::filter(study.conditions, "condition", [](ui::AnyHandle const& item) -> ui::AnyHandle {
                     return Count{ui::call("count", {item.at("points")})} > 0;
                 })});
    return ui::column(
               {ui::panel({pairs({{"Study", "STB-2026-014, draft"},
                                  {"Product", "Paracetamol 500 mg tablets"},
                                  {"Lot", study.lot},
                                  {"Start (T0)", ui::call("date", {study.start})},
                                  {"Conditions", ui::format("{0} with pulls", {conditions})},
                                  {"Pulls", ui::format("{0}, first {1}, last {2}",
                                                       {study.pulls, study.firstDate, study.lastPull})},
                                  {"Tests", ui::format("{0} tests, {1} runs", {study.testCount, study.runs})}},
                                 "review-pairs", {containersPair(study)})},
                          {.title = "Review", .padding = 2}),
                ui::checkbox(
                    {.label = "I have checked the conditions, the pulls and the tests against protocol STP-0114 v2.",
                     .checked = study.confirmed,
                     .onToggle = ui::set(study.confirmed, ui::event())})
                    .testId("confirm"),
                layout::muted(
                    "Signing creates the study and books its pulls; it asks for your password, as every e-signature "
                    "does.")},
               {.gap = 2})
        .testId("review-step");
}

// The wizard: Continue on Conditions focuses the per-pull field while the lot cannot cover the pulls, and shows the
// banner that says why.
ui::StepperParts wizard(Study const& study, bool compact, ui::Node const& conditionsStep, ui::Nodes const& extra) {
    Flag const hasTests = Count{study.testCount} > 0;
    return ui::stepperParts(
        {.step = study.step,
         .steps = {{.label = kSteps.at(0), .children = productStep(study, compact)},
                   {.label = kSteps.at(1),
                    .children = conditionsStep,
                    .canContinue = study.enough,
                    .blocking = compact ? study.perPullCompact : study.perPullField,
                    .onBlocked = ui::set(study.attempted, true),
                    .continueLabel = compact ? "Continue" : "Continue to tests"},
                   {.label = kSteps.at(2), .children = testsStep(study, compact), .canContinue = hasTests},
                   {.label = kSteps.at(3), .children = reviewStep(study), .canContinue = study.confirmed}},
         .continueLabel = "Continue",
         .finishLabel = "Sign and create",
         .onFinish = ui::notify("Study STB-2026-014 is signed and created; its pulls are booked"),
         .extra = extra,
         .labelled = !compact,
         .continueStretches = compact,
         .testIdPrefix = std::string{}});
}

// ── Expanded ─────────────────────────────────────────────────────────────────────────────────────────────────────

ui::Node expandedConditions(Study const& study) {
    auto const fields = ui::panel({ui::grid({startField(study), windowField(study), perPullField(study, false)},
                                            {.gap = 2, .columns = 3, .align = Align::Start})},
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
                           // The design's summary is 25% of its 1000 px page; 27 units is that share of the 785 px
                           // page the sidebar leaves.
                           .sizing(ui::fixed(27))
                           .a11y("Summary");
    return ui::row(
        {ui::column({attention(study, false), fields, conditions}, {.gap = 2}).sizing(ui::stretch()), aside},
        {.gap = 2, .align = Align::Start});
}

ui::Node expandedLayout(Study const& study) {
    auto const parts = wizard(study, false, expandedConditions(study),
                              ui::button({.label = "Save draft",
                                          .emphasis = ButtonEmphasis::Ghost,
                                          .onClick = ui::notify("The study draft is saved")}));
    return ui::pinned({parts.body}, {.top = {parts.indicator}, .bottom = {parts.footer}});
}

// ── Compact: one condition at a time, every field kept ───────────────────────────────────────────────────────────

ui::Node conditionSection(Study const& study) {
    std::vector<ui::Node> boxes;
    boxes.reserve(kTimepoints.size());
    for (Timepoint const& point : kTimepoints) {
        boxes.push_back(monthBox(study, point, true));
    }
    auto const rowId = ui::row("id");
    // A panel reports whether it is collapsed, so the open one is the row's id while the section is not.
    return ui::panel(
        {ui::grid(boxes, {.gap = 1, .columns = 3})},
        {.title = ui::format("{0} · {1} pulls", {ui::row("climate"), ui::call("count", {ui::row("points")})}),
         .collapsible = true,
         .collapsed = ui::ne(study.openCondition, rowId),
         .onToggle = ui::set(study.openCondition,
                             ui::cond(ui::Handle<bool>{ui::event()}, ui::AnyHandle{""}, ui::AnyHandle{rowId}))});
}

ui::Node compactConditions(Study const& study) {
    return ui::column(
        {attention(study, true), startField(study),
         // The fields' tops line up although only one has an error text below it.
         ui::row({windowField(study).sizing(ui::stretch()), perPullField(study, true).sizing(ui::stretch())},
                 {.gap = 2, .align = Align::Start}),
         ui::text({.text = perPullMessage(study, true), .role = "error"})
             .visible(!study.enough)
             .testId("per-pull-error"),
         ui::forEach(study.conditions, {.key = ui::row("id"), .gap = 1},
                     [&study](ui::DeclScope&) { return ui::Nodes{conditionSection(study)}; })
             .testId("conditions"),
         addCondition(study, true),
         ui::panel({summary(study)}, {.title = ui::format("Summary · first pull {0}", {study.firstDate}),
                                      .collapsible = true,
                                      .collapsed = true})
             .testId("summary-section")},
        {.gap = 2});
}

ui::Node compactLayout(Study const& study) {
    auto const parts = wizard(study, true, compactConditions(study), {});
    return ui::pinned({parts.body}, {.top = ui::Nodes{parts.indicator}, .bottom = {parts.footer}});
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
        // Compact puts the step line above the title, where the design does; expanded has the stepper for it.
        screen.eyebrow(
            ui::cond(study.compact, ui::format("Step {0} of 4 · Paracetamol 500 mg", {study.step + 1}), Text{""}));
        screen.showErrors(ui::ShowErrors::Always);
        return layout::byWidth(study.width, compactLayout(study), expandedLayout(study))
            .sizing(ui::stretch(), ui::stretch());
    });
}

}  // namespace lims_gallery

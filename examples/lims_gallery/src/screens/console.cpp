// SPDX-License-Identifier: Apache-2.0
//
// Design 10, the lab operations console: LabTrack's overview for QC chemistry, site A, at 09:41 on the morning HPLC-02
// failed system suitability. The dashboard: the suitability alert, the day's figures, the assay control chart, the
// instruments, and the worklist beside a non-modal sample pane that steps through it. The held sample cannot be
// submitted, and the pane says why. The design's sidebar is the shell's navigator, whose counts the gallery's app
// document gives.

#include <array>
#include <cstddef>
#include <cstdint>
#include <morph/util/rational.hpp>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "lims_gallery/lims_gallery.hpp"
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

constexpr char const* kHoldReason =
    "Can't submit: HPLC-02 is on suitability hold. Rerun, or record an assessment of the failure.";

// ── The data ─────────────────────────────────────────────────────────────────────────────────────────────────────

struct Kpi {
    char const* key;
    char const* label;
    char const* value;
    char const* note;
    char const* compactNote;
    bool alarming;
};

constexpr std::array<Kpi, 4> kKpis{{
    {.key = "received",
     .label = "Received today",
     .value = "64",
     .note = "58 on a typical Thursday",
     .compactNote = "",
     .alarming = false},
    {.key = "progress",
     .label = "In progress",
     .value = "38",
     .note = "18 of them on hold",
     .compactNote = "18 on hold",
     .alarming = false},
    {.key = "due",
     .label = "Past due",
     .value = "7",
     .note = "2 of them STAT",
     .compactNote = "2 STAT",
     .alarming = true},
    {.key = "review",
     .label = "Awaiting review",
     .value = "4",
     .note = "oldest 2 h 10 min",
     .compactNote = "",
     .alarming = false},
}};

struct Instrument {
    char const* name;
    char const* status;
    char const* tone;
};

constexpr std::array<Instrument, 5> kInstruments{{
    {.name = "HPLC-02", .status = "Suitability hold", .tone = "err"},
    {.name = "HPLC-01", .status = "Running · 14 queued", .tone = "ok"},
    {.name = "Dissolution bath 1", .status = "Run 2 of 3", .tone = "info"},
    {.name = "Karl Fischer", .status = "Calibration due today", .tone = "warn"},
    {.name = "UV-Vis 01", .status = "Idle", .tone = "neutral"},
}};

// The assay control's last 20 runs on HPLC-02, in tenths of a standard deviation from the mean (100.0 %, SD 0.4).
constexpr std::array<std::int64_t, 20> kControlRuns{-3, 3, -6, 1,  7, -2, -8, 2, 4,  -4,
                                                    -1, 8, -3, -9, 3, 4,  -3, 9, 14, 34};

struct Sample {
    char const* id;
    char const* test;
    char const* product;
    char const* lot;
    char const* instrument;
    char const* priority;
    char const* due;
    bool overdue;
    char const* status;
    char const* statusTone;
    bool held;
};

constexpr std::array<Sample, 6> kSamples{{
    {.id = "S-26-104170",
     .test = "Assay, impurities",
     .product = "Paracetamol 500 mg",
     .lot = "L-26-0377",
     .instrument = "HPLC-02",
     .priority = "STAT",
     .due = "22 min overdue",
     .overdue = true,
     .status = "Result entry",
     .statusTone = "info",
     .held = true},
    {.id = "S-26-104198",
     .test = "Water (KF)",
     .product = "Lactose monohydrate",
     .lot = "L-26-0390",
     .instrument = "Karl Fischer",
     .priority = "Urgent",
     .due = "10:30",
     .overdue = false,
     .status = "Queued",
     .statusTone = "neutral",
     .held = false},
    {.id = "S-26-104176",
     .test = "Dissolution",
     .product = "Ibuprofen 400 mg",
     .lot = "L-26-0381",
     .instrument = "Dissolution bath 1",
     .priority = "Routine",
     .due = "16:00",
     .overdue = false,
     .status = "Measured",
     .statusTone = "info",
     .held = false},
    {.id = "S-26-104221",
     .test = "TOC, conductivity",
     .product = "Purified water, loop 3",
     .lot = "—",
     .instrument = "TOC analyser",
     .priority = "Routine",
     .due = "14:00",
     .overdue = false,
     .status = "Queued",
     .statusTone = "neutral",
     .held = false},
    {.id = "S-26-104159",
     .test = "Assay",
     .product = "Paracetamol 500 mg",
     .lot = "L-26-0362",
     .instrument = "HPLC-02",
     .priority = "Routine",
     .due = "25-Oct 09:00",
     .overdue = false,
     .status = "On hold",
     .statusTone = "warn",
     .held = true},
    {.id = "S-26-104150",
     .test = "Identity (IR)",
     .product = "Paracetamol 500 mg",
     .lot = "L-26-0381",
     .instrument = "FTIR-01",
     .priority = "Routine",
     .due = "25-Oct 12:00",
     .overdue = false,
     .status = "Registered",
     .statusTone = "neutral",
     .held = false},
}};

struct Analyte {
    char const* sample;
    char const* key;
    char const* name;
    char const* value;
    char const* unit;
    std::int64_t places;
    // Limits scaled by the places; negative where the specification sets none.
    std::int64_t low;
    std::int64_t high;
    char const* spec;
    // Counted in the sample's total impurities.
    bool impurity;
    bool total;
};

constexpr std::array<Analyte, 6> kAnalytes{{
    {.sample = "S-26-104170",
     .key = "assay",
     .name = "Assay",
     .value = "98.9",
     .unit = "% LC",
     .places = 1,
     .low = 950,
     .high = 1050,
     .spec = "95.0–105.0",
     .impurity = false,
     .total = false},
    {.sample = "S-26-104170",
     .key = "impA",
     .name = "Impurity A",
     .value = "0.03",
     .unit = "%",
     .places = 2,
     .low = -1,
     .high = 10,
     .spec = "≤ 0.10",
     .impurity = true,
     .total = false},
    {.sample = "S-26-104170",
     .key = "impB",
     .name = "Impurity B",
     .value = "0.21",
     .unit = "%",
     .places = 2,
     .low = -1,
     .high = 20,
     .spec = "≤ 0.20",
     .impurity = true,
     .total = false},
    {.sample = "S-26-104170",
     .key = "total",
     .name = "Total impurities",
     .value = "",
     .unit = "%",
     .places = 2,
     .low = -1,
     .high = 50,
     .spec = "≤ 0.50",
     .impurity = false,
     .total = true},
    {.sample = "S-26-104198",
     .key = "water",
     .name = "Water",
     .value = "",
     .unit = "%",
     .places = 1,
     .low = -1,
     .high = 5,
     .spec = "≤ 0.5",
     .impurity = false,
     .total = false},
    {.sample = "S-26-104176",
     .key = "q30",
     .name = "Dissolved at 30 min",
     .value = "92",
     .unit = "%",
     .places = 0,
     .low = 80,
     .high = -1,
     .spec = "Q = 80",
     .impurity = false,
     .total = false},
}};

struct Custody {
    char const* sample;
    char const* step;
    char const* detail;
    bool alarm;
};

constexpr std::array<Custody, 7> kCustody{{
    {.sample = "S-26-104170", .step = "Sampled", .detail = "QA, packaging line 2 · 24-Oct 06:50", .alarm = false},
    {.sample = "S-26-104170", .step = "Received", .detail = "J. Ruiz · 07:05", .alarm = false},
    {.sample = "S-26-104170", .step = "Prepared", .detail = "2 preparations · M. Kaur · 08:10", .alarm = false},
    {.sample = "S-26-104170", .step = "Held", .detail = "suitability failure on HPLC-02 · 09:40", .alarm = true},
    {.sample = "S-26-104198", .step = "Received", .detail = "J. Ruiz · 07:40", .alarm = false},
    {.sample = "S-26-104176", .step = "Received", .detail = "J. Ruiz · 06:58", .alarm = false},
    {.sample = "S-26-104159", .step = "Held", .detail = "suitability failure on HPLC-02 · 09:40", .alarm = true},
}};

std::int64_t scaleOf(std::int64_t places) {
    std::int64_t scale = 1;
    for (std::int64_t place = 0; place < places; ++place) {
        scale *= 10;
    }
    return scale;
}

ui::AnyHandle limit(std::int64_t scaled, std::int64_t places) {
    if (scaled < 0) {
        return ui::AnyHandle{nullptr};
    }
    return ui::AnyHandle{morph::math::Rational{morph::math::Numerator{scaled},
                                               morph::math::Denominator{scaleOf(places)},
                                               morph::math::DecimalPlaces{static_cast<std::uint8_t>(places)}}};
}

char const* priorityTone(std::string_view priority) {
    if (priority == "STAT") {
        return "err";
    }
    return priority == "Urgent" ? "warn" : "neutral";
}

ui::AnyHandle sampleList() {
    std::vector<ui::AnyHandle> items;
    items.reserve(kSamples.size());
    std::int64_t index = 0;
    for (Sample const& sample : kSamples) {
        items.push_back(ui::obj({{"index", index++},
                                 {"id", sample.id},
                                 {"test", sample.test},
                                 {"product", sample.product},
                                 {"lot", sample.lot},
                                 {"instrument", sample.instrument},
                                 {"priority", sample.priority},
                                 {"priorityTone", priorityTone(sample.priority)},
                                 {"due", sample.due},
                                 {"overdue", sample.overdue},
                                 {"status", sample.status},
                                 {"statusTone", sample.statusTone},
                                 {"held", sample.held}}));
    }
    return ui::list(items);
}

ui::AnyHandle analyteList() {
    std::vector<ui::AnyHandle> items;
    items.reserve(kAnalytes.size());
    for (Analyte const& analyte : kAnalytes) {
        items.push_back(ui::obj({{"sample", analyte.sample},
                                 {"key", analyte.key},
                                 {"name", analyte.name},
                                 {"value", analyte.value},
                                 {"unit", analyte.unit},
                                 {"places", analyte.places},
                                 {"low", limit(analyte.low, analyte.places)},
                                 {"high", limit(analyte.high, analyte.places)},
                                 {"spec", analyte.spec},
                                 {"impurity", analyte.impurity},
                                 {"total", analyte.total}}));
    }
    return ui::list(items);
}

ui::AnyHandle custodyList() {
    std::vector<ui::AnyHandle> items;
    items.reserve(kCustody.size());
    std::int64_t index = 0;
    for (Custody const& entry : kCustody) {
        items.push_back(ui::obj({{"key", index++},
                                 {"sample", entry.sample},
                                 {"step", entry.step},
                                 {"detail", entry.detail},
                                 {"alarm", entry.alarm}}));
    }
    return ui::list(items);
}

struct Console {
    ui::Element rerunDialog = ui::element("rerunDialog");
    ui::AnyHandle width;
    ui::AnyHandle samples;
    ui::AnyHandle analytes;
    ui::AnyHandle custody;
    Count current;
    Text search;
    Text lab;
    Text rerunReason;
    Flag rerunOpen;
    ui::AnyHandle selected;
    Flag held;
    ui::AnyHandle total;
};

// A field of the sample the pane shows.
ui::AnyHandle field(Console const& console, char const* name) {
    auto const shown = ui::filter(console.samples, "sample", [&console](ui::AnyHandle const& sample) {
        return ui::eq(sample.at("index"), console.current);
    });
    return ui::call("first", {shown}).at(name);
}

Console declare(ui::Scope& screen) {
    Console console;
    console.width = layout::widthClass(screen);
    console.samples = screen.state<ui::Any>("samples", sampleList());
    console.analytes = screen.state<ui::Any>("analytes", analyteList());
    console.custody = screen.let("custody", custodyList());
    console.current = screen.state("current", std::int64_t{0});
    console.search = screen.state("search", std::string{});
    console.lab = screen.state("lab", std::string{"qc-chem-a"});
    console.rerunReason = screen.state("rerunReason", std::string{});
    console.rerunOpen = screen.state("rerunOpen", false);
    console.selected = screen.let("selectedSample", field(console, "id"));
    console.held = screen.let("held", Flag{ui::coalesce({field(console, "held"), ui::AnyHandle{false}})});
    auto const impurities =
        ui::filter(console.analytes, "analyte", [&console](ui::AnyHandle const& analyte) -> ui::AnyHandle {
            return ui::eq(analyte.at("sample"), console.selected) && Flag{analyte.at("impurity")};
        });
    console.total = screen.let(
        "totalImpurities", ui::call("sum", {ui::map(impurities, "analyte", [](ui::AnyHandle const& analyte) {
                                        return ui::call("parseDecimal", {analyte.at("value"), analyte.at("places")});
                                    })}));
    return console;
}

// ── The head ─────────────────────────────────────────────────────────────────────────────────────────────────────

ui::Command open(char const* screen) { return ui::navigate(screen); }

ui::Commands findSample(Console const& console) {
    auto const matching = ui::filter(console.samples, "sample", [](ui::AnyHandle const& sample) {
        return ui::eq(sample.at("id"), ui::call("trim", {ui::event()}));
    });
    auto const index = ui::call("first", {matching}).at("index");
    return ui::ifThen(ui::engaged(index), {ui::set(console.current, index), ui::set(console.search, std::string{})},
                      ui::notify(ui::format("No sample {0} on today's worklist.", {ui::event()}), ui::Role::Warning));
}

// The lab and the sample search are head tools on the expanded layout, the one that draws them, and the connection
// is a head badge; the user is the shell's principal.
void declareHead(ui::Scope& screen, Console const& console) {
    screen.status({.text = "Connected", .tone = "ok"});
    auto const labs = ui::list({ui::obj({{"key", "qc-chem-a"}, {"label", "QC chemistry, site A"}}),
                                ui::obj({{"key", "qc-micro-a"}, {"label", "QC microbiology, site A"}})});
    Flag const expanded = !Flag{ui::eq(console.width, "compact")};
    screen.tool(ui::HeadSelectOptions{.id = "lab",
                                      .label = "Lab",
                                      .options = labs,
                                      .selected = console.lab,
                                      .onChoose = {ui::set(console.lab, ui::event())},
                                      .width = 22,
                                      .visible = expanded});
    screen.tool(ui::HeadSearchOptions{.id = "find",
                                      .label = "Find a sample",
                                      .value = console.search,
                                      .placeholder = "Scan or type a sample ID",
                                      .onChange = {ui::set(console.search, ui::event())},
                                      .onSubmit = findSample(console),
                                      .width = 28,
                                      .visible = expanded});
}

// ── The dashboard ────────────────────────────────────────────────────────────────────────────────────────────────

ui::Node alert(bool compact) {
    return ui::banner({.tone = "err",
                       .text = compact ? "System suitability failed on HPLC-02 at 09:40: tailing 2.3, limit 2.0. 18 "
                                         "results on hold."
                                       : "System suitability failed on HPLC-02 at 09:40 CEST: tailing 2.3, limit 2.0. "
                                         "18 results from this run are on hold.",
                       .action = ui::ActionPart{.label = "Open run", .onClick = open("lims-gallery.instrument")}})
        .a11y("System suitability failed on HPLC-02", "alert")
        .testId("alert");
}

ui::Node kpis(bool compact) {
    std::vector<ui::Node> tiles;
    for (Kpi const& kpi : kKpis) {
        char const* const note = compact ? kpi.compactNote : kpi.note;
        std::vector<ui::Node> lines{layout::muted(kpi.label),
                                    ui::text({.text = kpi.value, .role = kpi.alarming ? "error" : "display"})
                                        .testId(std::string{"kpi-"} + kpi.key)};
        if (std::string_view{note}.empty()) {
            lines.push_back(ui::spacer());
        } else {
            lines.push_back(layout::muted(note));
        }
        tiles.push_back(ui::panel({ui::column(lines, {.gap = 0})}, {.padding = 2}).a11y(kpi.label));
    }
    return ui::grid(tiles, {.gap = 1, .columns = compact ? 2 : 4}).testId("kpis");
}

// The assay control's runs as % LC values, 100.00 + 0.04 per tenth of an SD, each with the rule it broke.
ui::AnyHandle controlRuns() {
    std::vector<ui::AnyHandle> runs;
    for (std::size_t index = 0; index < kControlRuns.size(); ++index) {
        bool const latest = index + 1 == kControlRuns.size();
        runs.push_back(ui::obj({{"run", static_cast<std::int64_t>(index + 1)},
                                {"value", limit(10000 + (4 * kControlRuns.at(index)), 2)},
                                {"tone", latest ? ui::AnyHandle{"err"} : ui::AnyHandle{nullptr}},
                                {"violation", latest ? ui::AnyHandle{"+3.4SD, rule 1-3s"} : ui::AnyHandle{nullptr}}}));
    }
    return ui::list(runs);
}

// A control line @p sds standard deviations from the mean, 100.0 % with an SD of 0.4.
ui::ReferenceLine controlLine(std::int64_t sds, char const* label, char const* tone, ui::LineStyle style) {
    return {.value = limit(1000 + (4 * sds), 1),
            .label = label,
            .tone = *tone == '\0' ? ui::AnyHandle{} : ui::AnyHandle{tone},
            .style = style};
}

// The Levey-Jennings chart: the 20 runs against the mean and the ±1, 2 and 3 SD lines, the run that broke a rule
// marked with the rule. Twelve units tall, so each of the seven lines has a row of its own on the terminal.
ui::Node controlChart() {
    using ui::LineStyle;
    return ui::column(
        {ui::row({layout::section("Assay control, HPLC-02 · last 20 runs").sizing(ui::stretch()),
                  layout::muted("mean 100.0 % · SD 0.4")},
                 {.gap = 1, .align = Align::Center}),
         ui::chart(
             {.summary = "Levey-Jennings chart: 19 runs within 2 SD, the latest run at plus 3.4 SD, beyond the 3 SD "
                         "limit",
              .series = {{.points = controlRuns(),
                          .y = [](ui::AnyHandle const& run) { return run.at("value"); },
                          .xLabel = [](ui::AnyHandle const& run) { return run.at("run"); },
                          .tone = [](ui::AnyHandle const& run) { return run.at("tone"); },
                          .note = [](ui::AnyHandle const& run) { return run.at("violation"); }}},
              .lines = {controlLine(3, "+3SD", "err", LineStyle::Dashed),
                        controlLine(2, "+2SD", "warn", LineStyle::Dashed),
                        controlLine(1, "+1SD", "", LineStyle::Dotted), controlLine(0, "mean", "", LineStyle::Solid),
                        controlLine(-1, "−1SD", "", LineStyle::Dotted),
                        controlLine(-2, "−2SD", "warn", LineStyle::Dashed),
                        controlLine(-3, "−3SD", "err", LineStyle::Dashed)},
              .yPlaces = 1,
              .yLabel = "% LC"})
             .sizing(ui::stretch(), ui::fixed(12))
             .testId("chart"),
         ui::text({.text = "Run 20: 101.4 % LC, +3.4 SD, rule 1-3s", .role = "error"}).testId("chart-violation")},
        {.gap = 1});
}

ui::Node instruments() {
    std::vector<ui::Node> rows{layout::section("Instruments")};
    for (Instrument const& instrument : kInstruments) {
        rows.push_back(ui::row({ui::text({.text = instrument.name}).sizing(ui::stretch()),
                                layout::tag(instrument.status, instrument.tone)},
                               {.gap = 1, .align = Align::Center})
                           .testId(std::string{"instrument-"} + instrument.name));
    }
    return ui::column(rows, {.gap = 1}).testId("instruments");
}

// ── The worklist and the sample pane ─────────────────────────────────────────────────────────────────────────────

ui::Node dueCell() {
    return ui::text(
        {.text = ui::row("due"), .role = ui::cond(Flag{ui::row("overdue")}, Text{"error"}, Text{"normal"})});
}

ui::Node worklistTable(Console const& console, ui::AnyHandle const& rows, std::string const& testId) {
    std::vector<ui::TableColumn> const columns{
        {.name = "id", .label = "Sample", .width = ui::fixed(12), .role = "mono"},
        {.name = "test", .label = "Test", .width = ui::stretch()},
        {.name = "priority",
         .label = "Priority",
         .width = ui::fixed(9),
         .cell = layout::tag(ui::row("priority"), ui::row("priorityTone"))},
        {.name = "due", .label = "Due", .width = ui::fixed(13), .cell = dueCell()},
        {.name = "status",
         .label = "Status",
         .width = ui::fixed(12),
         .cell = layout::tag(ui::row("status"), ui::row("statusTone"))}};
    // The sample in the pane is the table's selection: the id of the row at `current`, and a click on a row makes it
    // current.
    auto const atCurrent = ui::filter(rows, "entry", [&console](ui::AnyHandle const& entry) -> ui::AnyHandle {
        return ui::eq(entry.at("index"), console.current);
    });
    auto const picked = ui::filter(rows, "entry", [](ui::AnyHandle const& entry) -> ui::AnyHandle {
        return ui::includes(ui::event(), entry.at("id"));
    });
    return ui::table(
               rows,
               {.key = ui::row("id"),
                .columns = columns,
                .selectionMode = ui::Selection::Single,
                .selection = ui::map(atCurrent, "entry", [](ui::AnyHandle const& entry) { return entry.at("id"); }),
                .onSelectionChange = ui::set(
                    console.current,
                    ui::call("first", {ui::map(picked, "entry",
                                               [](ui::AnyHandle const& entry) { return entry.at("index"); })})),
                .onActivate = ui::set(console.current, ui::row("index"))})
        .a11y("Worklist")
        .testId(testId);
}

ui::AnyHandle heldSamples(Console const& console) {
    return ui::filter(console.samples, "sample", [](ui::AnyHandle const& sample) { return sample.at("held"); });
}

ui::Node reviewList() {
    return ui::column({layout::muted("WS-2026-0412 · Assay and impurities · M. Kaur · 2 h 10 min"),
                       layout::muted("WS-2026-0409 · Water (KF) · A. Meyer · 1 h 05 min"),
                       layout::muted("WS-2026-0405 · Dissolution · J. Ruiz · 48 min"),
                       layout::muted("WS-2026-0398 · Identity (IR) · M. Kaur · 20 min")},
                      {.gap = 1})
        .testId("review-list");
}

ui::Node worklistTabs(Console const& console) {
    return ui::tabs({{.label = "Worklist 38",
                      .children = {worklistTable(console, console.samples, "worklist"),
                                   layout::muted("The 6 most urgent of 38.")}},
                     {.label = "On hold 18",
                      .children = {worklistTable(console, heldSamples(console), "on-hold"),
                                   layout::muted("The 2 on hold from today's worklist, of 18.")}},
                     {.label = "Review 4", .children = {reviewList()}}})
        .testId("worklist-tabs");
}

// Evaluates an analyte, the total from the sample's impurities, against its limits.
struct Evaluation {
    ui::AnyHandle shown;
    Text label;
    Text tone;
};

Evaluation evaluate(Console const& console) {
    Flag const total = Flag{ui::row("total")};
    auto const shown =
        ui::cond(total, ui::AnyHandle{ui::call("decimal", {console.total, std::int64_t{2}})}, ui::row("value"));
    auto const number =
        ui::cond(total, console.total, ui::call("parseDecimal", {ui::row("value"), ui::row("places")}));
    Flag const above = ui::engaged(ui::row("high")) && number > ui::row("high");
    Flag const below = ui::engaged(ui::row("low")) && number < ui::row("low");
    Flag const empty = !ui::engaged(number);
    return Evaluation{
        .shown = shown,
        .label = ui::cond(empty, Text{"To enter"}, ui::cond(above || below, Text{"Out of spec"}, Text{"Within"})),
        .tone = ui::cond(empty, Text{"neutral"}, ui::cond(above || below, Text{"err"}, Text{"ok"}))};
}

ui::Commands enterResult(Console const& console) {
    auto const updated = ui::map(console.analytes, "analyte", [](ui::AnyHandle const& analyte) -> ui::AnyHandle {
        Flag const here = ui::eq(analyte.at("sample"), ui::row("sample")) && ui::eq(analyte.at("key"), ui::row("key"));
        std::vector<std::pair<std::string, ui::AnyHandle>> fields;
        for (char const* const name :
             {"sample", "key", "name", "unit", "places", "low", "high", "spec", "impurity", "total"}) {
            fields.emplace_back(name, analyte.at(name));
        }
        fields.emplace_back("value", ui::event());
        return ui::cond(here, ui::obj(fields), analyte);
    });
    return ui::set(console.analytes, updated);
}

ui::Node resultLine(Console const& console) {
    Evaluation const evaluation = evaluate(console);
    return ui::row(
        {ui::text({.text = ui::format("{0} · {1}", {ui::row("name"), ui::row("spec")})}).sizing(ui::stretch()),
         ui::textInput({.value = evaluation.shown,
                        .label = ui::format("{0} result", {ui::row("name")}),
                        .onCommit = enterResult(console)})
             .readonly(Flag{ui::row("total")})
             .sizing(ui::fixed(9))
             .testId("pane-result"),
         ui::text({.text = ui::row("unit")}).sizing(ui::fixed(5)),
         layout::tag(evaluation.label, evaluation.tone).testId("pane-evaluation")},
        {.gap = 1, .align = Align::Center});
}

ui::Node paneResults(Console const& console) {
    auto const mine = ui::filter(console.analytes, "analyte", [&console](ui::AnyHandle const& analyte) {
        return ui::eq(analyte.at("sample"), console.selected);
    });
    return ui::column(
        {ui::forEach(mine, {.key = ui::row("key"), .gap = 1},
                     [&console](ui::DeclScope&) { return ui::Nodes{resultLine(console)}; })
             .testId("pane-results"),
         layout::muted("No results to enter yet.").visible(ui::eq(ui::call("count", {mine}), std::int64_t{0}))},
        {.gap = 1});
}

ui::Node custodyLines(Console const& console) {
    auto const mine = ui::filter(console.custody, "entry", [&console](ui::AnyHandle const& entry) {
        return ui::eq(entry.at("sample"), console.selected);
    });
    return ui::column({layout::section("Chain of custody"),
                       ui::forEach(mine, {.key = ui::row("key"), .gap = 0},
                                   [](ui::DeclScope&) {
                                       return ui::Nodes{ui::text(
                                           {.text = ui::format("● {0} · {1}", {ui::row("step"), ui::row("detail")}),
                                            .role = ui::cond(Flag{ui::row("alarm")}, Text{"error"}, Text{"normal"})})};
                                   })
                           .testId("custody")},
                      {.gap = 1});
}

ui::Node rerunDialog(Console const& console) {
    ui::Commands const close = ui::set(console.rerunOpen, false);
    return ui::dialog(
               console.rerunDialog,
               {layout::muted(ui::format("{0} is rerun on HPLC-02 once its suitability passes.", {console.selected})),
                layout::captioned(ui::textInput({.value = console.rerunReason,
                                                 .label = "Reason",
                                                 .mode = "multiline",
                                                 .onChange = ui::set(console.rerunReason, ui::event())}))
                    .required(true)
                    .testId("rerun-reason")},
               {.title = "Request rerun",
                .open = console.rerunOpen,
                .cancel = close,
                .onDismiss = close,
                .actions = {{.id = "cancel", .label = "Cancel", .onClick = close},
                            {.id = "request",
                             .label = "Request rerun",
                             .emphasis = ButtonEmphasis::Primary,
                             .enabled = !ui::isEmpty(ui::call("trim", {console.rerunReason})),
                             .onClick = {ui::notify(ui::format("A rerun of {0} is requested.", {console.selected})),
                                         ui::set(console.rerunReason, std::string{}),
                                         ui::set(console.rerunOpen, false)}}}})
        .sizing(ui::fixed(52))
        .testId("rerun");
}

ui::Node samplePane(Console const& console) {
    auto const last = static_cast<std::int64_t>(kSamples.size()) - 1;
    auto const tags = ui::concatLists(
        {ui::list({ui::obj({{"text", field(console, "priority")}, {"tone", field(console, "priorityTone")}}),
                   ui::obj({{"text", field(console, "status")}, {"tone", field(console, "statusTone")}})}),
         ui::cond(console.held, ui::list({ui::obj({{"text", "Run on suitability hold"}, {"tone", "err"}})}),
                  ui::AnyHandle{std::vector<std::string>{}})});
    return ui::column(
               {ui::row(
                    {ui::text({.text = console.selected, .role = "section"}).sizing(ui::stretch()).testId("pane-id"),
                     ui::button({.label = "‹", .onClick = ui::set(console.current, console.current - 1)})
                         .a11y("Previous sample")
                         .enabled(console.current > 0)
                         .testId("previous"),
                     ui::button({.label = "›", .onClick = ui::set(console.current, console.current + 1)})
                         .a11y("Next sample")
                         .enabled(console.current < last)
                         .testId("next")},
                    {.gap = 1, .align = Align::Center}),
                ui::text({.text = ui::format("{0} · lot {1} · {2}", {field(console, "product"), field(console, "lot"),
                                                                     field(console, "instrument")})})
                    .testId("pane-line"),
                ui::forEach(tags, {.key = ui::row("text"), .axis = "horizontal", .gap = 1, .wrap = true},
                            [](ui::DeclScope&) { return ui::Nodes{layout::tag(ui::row("text"), ui::row("tone"))}; })
                    .testId("pane-tags"),
                paneResults(console), custodyLines(console),
                ui::text({.text = kHoldReason, .role = "error"}).visible(console.held).testId("hold-reason"),
                ui::row(
                    {ui::button({.label = "Request rerun…", .onClick = ui::set(console.rerunOpen, true)})
                         .enabled(console.held)
                         .testId("rerun-open"),
                     ui::spacer().sizing(ui::stretch()),
                     ui::button({.label = "Save draft",
                                 .onClick = ui::notify(ui::format("{0} is saved as a draft.", {console.selected}))}),
                     ui::button(
                         {.label = "Submit for review",
                          .emphasis = ButtonEmphasis::Primary,
                          .onClick = ui::notify(ui::format("{0} is submitted for review.", {console.selected}))})
                         .enabled(!console.held)
                         .tooltip(ui::cond(console.held, Text{kHoldReason}, Text{""}))
                         .testId("submit")},
                    {.gap = 1, .align = Align::Center}),
                rerunDialog(console)},
               {.gap = 2})
        .a11y(ui::format("Sample {0}", {console.selected}))
        .testId("sample-pane");
}

// ── Layouts ──────────────────────────────────────────────────────────────────────────────────────────────────────

// The least page height, in units: the alert, the tiles and the charts take about 25 of them, leaving 15 for the
// worklist and the sample pane's splitter. A shorter window scrolls the page instead of squeezing the splitter.
constexpr int kExpandedMinHeight = 40;

ui::Node expandedLayout(Console const& console) {
    // The design's 360 px sample pane, in the frontend's character widths, beside the worklist. The splitter takes the
    // page's height under the charts.
    auto const sizes = ui::AnyHandle{std::vector<std::int64_t>{70, 45}};
    return ui::column({alert(false), kpis(false),
                       ui::row({ui::panel({controlChart()}, {.padding = 2}).sizing(ui::stretch(17)),
                                ui::panel({instruments()}, {.padding = 2}).sizing(ui::stretch(10))},
                               {.gap = 1, .align = Align::Start}),
                       ui::splitter({ui::panel({worklistTabs(console)}, {.padding = 1}),
                                     ui::panel({samplePane(console)}, {.padding = 2})},
                                    ui::AnyHandle{"horizontal"}, sizes)
                           .sizing(ui::stretch(), ui::stretch())
                           .a11y("Worklist and sample")
                           .testId("work-split")},
                      {.gap = 2})
        .sizing(ui::stretch(), ui::stretch())
        .minHeight(kExpandedMinHeight);
}

ui::Node compactCard() {
    return ui::column({ui::row({ui::text({.text = ui::row("id"), .role = "section"}).sizing(ui::stretch()), dueCell()},
                               {.gap = 1, .align = Align::Center}),
                       ui::text({.text = ui::format("{0} · {1}", {ui::row("test"), ui::row("product")})}),
                       ui::row({layout::tag(ui::row("priority"), ui::row("priorityTone")),
                                layout::tag(ui::row("status"), ui::row("statusTone"))},
                               {.gap = 1, .wrap = true})},
                      {.gap = 1});
}

// Tapping a sample opens it as its own page: the compact result entry of design 08.
ui::Node compactWorklist(Console const& console, ui::AnyHandle const& rows, std::string const& testId) {
    return ui::forEach(rows, {.key = ui::row("id"), .gap = 2},
                       [&console](ui::DeclScope&) {
                           return ui::Nodes{ui::panel(
                               {compactCard(), ui::button({.label = ui::format("Open {0}", {ui::row("id")}),
                                                           .onClick = {ui::set(console.current, ui::row("index")),
                                                                       open("lims-gallery.results")},
                                                           .style = morph::ui::ButtonStyle::Item,
                                                           .selected = ui::eq(ui::row("index"), console.current)})
                                                   .testId("open-sample")},
                               {.padding = 2})};
                       })
        .testId(testId);
}

ui::Node compactLayout(Console const& console) {
    return ui::column(
        {ui::row({ui::spacer().sizing(ui::stretch()),
                  ui::button({.label = "Scan", .icon = "scan", .onClick = open("lims-gallery.receipt")})
                      .a11y("Scan a sample")
                      .testId("scan")},
                 {.gap = 1, .align = Align::Center}),
         alert(true), kpis(true),
         ui::collapsible({controlChart()}, {.title = "Assay control, HPLC-02 · latest run +3.4 SD", .open = false})
             .testId("chart-section"),
         ui::collapsible({instruments()}, {.title = "Instruments · 1 on hold, 1 calibration due", .open = false})
             .testId("instruments-section"),
         ui::tabs({{.label = "Worklist 38", .children = {compactWorklist(console, console.samples, "worklist")}},
                   {.label = "On hold 18", .children = {compactWorklist(console, heldSamples(console), "on-hold")}},
                   {.label = "Review 4", .children = {reviewList()}}})
             .testId("worklist-tabs"),
         layout::muted("Tapping a sample opens it as its own page, laid out as the compact result entry.")},
        {.gap = 2});
}

}  // namespace

ui::Screen consoleScreen() {
    return ui::screen("lims-gallery.console", [](ui::Scope& screen) {
        Console const console = declare(screen);
        screen.title("Overview");
        declareHead(screen, console);
        // The switch stretches so that the expanded root fills the page; the compact layout then scrolls inside it,
        // since a stretching child of the page is not scrolled by the page.
        return layout::byWidth(console.width,
                               ui::scroll({compactLayout(console)}).sizing(ui::stretch(), ui::stretch()),
                               expandedLayout(console))
            .sizing(ui::stretch(), ui::stretch());
    });
}

}  // namespace lims_gallery

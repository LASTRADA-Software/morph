// SPDX-License-Identifier: Apache-2.0
//
// Design 08, result entry: worksheet WS-2026-0412's results for two samples, each entered to the decimal places of
// its specification and evaluated against it as it is typed, with the assay's mean and RSD and the total impurities
// calculated. Changing a result asks for the reason before the change stands. The screen opens on the state the
// design draws: Impurity B of S-26-104170 just changed from 0.12 % to 0.21 %, out of specification, its reason asked.

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <morph/util/rational.hpp>
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

// A limit the specification does not set.
constexpr std::int64_t kNoLimit = -1;

// One line of the worksheet: an entered result, or one the worksheet calculates. Limits are
// scaled by the result's places: 950 at one place is 95.0.
struct Line {
    char const* id;
    char const* kind;  // `entered`, `mean` or `total`
    char const* sample;
    char const* analyte;
    char const* area;
    char const* value;
    // The value as first entered; a value that differs from it is a change.
    char const* original;
    char const* unit;
    std::int64_t places;
    std::int64_t low;
    std::int64_t high;
    char const* spec;
};

constexpr std::array<Line, 7> kLines{{
    {.id = "a1",
     .kind = "entered",
     .sample = "S-26-104176",
     .analyte = "Assay, prep 1",
     .area = "1 204 331",
     .value = "99.6",
     .original = "99.6",
     .unit = "% LC",
     .places = 1,
     .low = 950,
     .high = 1050,
     .spec = "95.0–105.0"},
    {.id = "a2",
     .kind = "entered",
     .sample = "S-26-104176",
     .analyte = "Assay, prep 2",
     .area = "1 199 870",
     .value = "99.2",
     .original = "99.2",
     .unit = "% LC",
     .places = 1,
     .low = 950,
     .high = 1050,
     .spec = "95.0–105.0"},
    {.id = "mean",
     .kind = "mean",
     .sample = "S-26-104176",
     .analyte = "Assay, mean",
     .area = "",
     .value = "",
     .original = "",
     .unit = "% LC",
     .places = 1,
     .low = 950,
     .high = 1050,
     .spec = "95.0–105.0"},
    {.id = "ia1",
     .kind = "entered",
     .sample = "S-26-104176",
     .analyte = "Impurity A",
     .area = "812",
     .value = "0.04",
     .original = "0.04",
     .unit = "%",
     .places = 2,
     .low = kNoLimit,
     .high = 10,
     .spec = "≤ 0.10"},
    {.id = "ib2",
     .kind = "entered",
     .sample = "S-26-104170",
     .analyte = "Impurity B",
     .area = "4 410",
     .value = "0.21",
     .original = "0.12",
     .unit = "%",
     .places = 2,
     .low = kNoLimit,
     .high = 20,
     .spec = "≤ 0.20"},
    {.id = "ia2",
     .kind = "entered",
     .sample = "S-26-104170",
     .analyte = "Impurity A",
     .area = "655",
     .value = "0.03",
     .original = "0.03",
     .unit = "%",
     .places = 2,
     .low = kNoLimit,
     .high = 10,
     .spec = "≤ 0.10"},
    {.id = "total",
     .kind = "total",
     .sample = "S-26-104170",
     .analyte = "Total impurities",
     .area = "",
     .value = "",
     .original = "",
     .unit = "%",
     .places = 2,
     .low = kNoLimit,
     .high = 50,
     .spec = "≤ 0.50"},
}};

// The samples, in the worksheet's order, and their lots.
constexpr std::array<char const*, 2> kSamples{"S-26-104176", "S-26-104170"};
constexpr std::array<char const*, 2> kLots{"L-26-0381", "L-26-0377"};

char const* lotOf(char const* sample) {
    return std::string_view{sample} == kSamples.front() ? kLots.front() : kLots.back();
}

// The record fields a line keeps; an edit rewrites `value` alone.
constexpr std::array<char const*, 13> kFields{"id",       "kind", "sample", "lot", "analyte", "area", "value",
                                              "original", "unit", "places", "low", "high",    "spec"};

std::int64_t scaleOf(std::int64_t places) {
    std::int64_t scale = 1;
    for (std::int64_t place = 0; place < places; ++place) {
        scale *= 10;
    }
    return scale;
}

// An exact limit, or null where the specification sets none.
ui::AnyHandle limit(std::int64_t scaled, std::int64_t places) {
    if (scaled == kNoLimit) {
        return ui::AnyHandle{nullptr};
    }
    return ui::AnyHandle{morph::math::Rational{morph::math::Numerator{scaled},
                                               morph::math::Denominator{scaleOf(places)},
                                               morph::math::DecimalPlaces{static_cast<std::uint8_t>(places)}}};
}

// A limit as the messages show it: 20 at two places is `0.20`.
std::string limitText(std::int64_t scaled, std::int64_t places) {
    if (scaled == kNoLimit) {
        return {};
    }
    std::int64_t const scale = scaleOf(places);
    std::string fraction = std::to_string(scaled % scale);
    while (std::cmp_less(fraction.size(), places)) {
        fraction.insert(fraction.begin(), '0');
    }
    return std::to_string(scaled / scale) + (places > 0 ? "." + fraction : std::string{});
}

ui::AnyHandle lineList() {
    std::vector<ui::AnyHandle> lines;
    lines.reserve(kLines.size());
    for (Line const& line : kLines) {
        lines.push_back(ui::obj({{"id", line.id},
                                 {"kind", line.kind},
                                 {"sample", line.sample},
                                 {"lot", lotOf(line.sample)},
                                 {"analyte", line.analyte},
                                 {"area", line.area},
                                 {"value", line.value},
                                 {"original", line.original},
                                 {"unit", line.unit},
                                 {"places", line.places},
                                 {"low", limit(line.low, line.places)},
                                 {"high", limit(line.high, line.places)},
                                 {"lowText", limitText(line.low, line.places)},
                                 {"highText", limitText(line.high, line.places)},
                                 {"spec", line.spec}}));
    }
    return ui::list(lines);
}

// The change whose reason is asked: which result, from what to what, and the reason being given.
struct Change {
    Flag open;
    Text id;
    Text sample;
    Text analyte;
    Text unit;
    Text before;
    Text after;
    Text reason;
    Text comment;
};

struct Worksheet {
    ui::AnyHandle width;
    ui::AnyHandle lines;
    Count current;
    Change change;
    ui::AnyHandle mean;
    ui::AnyHandle rsd;
    ui::AnyHandle total;
    Count outOfSpec;
    Count unreadable;
};

// A line's field, read in a row scope or from a list element.
using Field = std::function<ui::AnyHandle(char const*)>;

ui::AnyHandle valueOf(ui::AnyHandle const& lines, char const* lineId) {
    return ui::call("first",
                    {ui::filter(lines, "line",
                                [lineId](ui::AnyHandle const& line) { return ui::eq(line.at("id"), lineId); })})
        .at("value");
}

// What a line shows and how it is evaluated against its specification.
struct Evaluation {
    ui::AnyHandle number;
    ui::AnyHandle shown;
    Flag calculated;
    Flag unreadable;
    Flag outOfSpec;
    Text label;
    ui::AnyHandle tone;
};

Evaluation evaluate(Worksheet const& sheet, Field const& field) {
    auto const kind = field("kind");
    Flag const isMean = ui::eq(kind, "mean");
    Flag const isTotal = ui::eq(kind, "total");
    auto const typed = ui::call("parseDecimal", {field("value"), field("places")});
    auto const number = ui::cond(isMean, sheet.mean, ui::cond(isTotal, sheet.total, typed));
    // The mean is rounded once, half up, where it is computed: the value shown is the value compared with the limits.
    auto const shown = ui::cond(isMean, ui::AnyHandle{ui::decimal(sheet.mean, 1)},
                                ui::cond(isTotal, ui::AnyHandle{ui::decimal(sheet.total, 2)}, field("value")));
    Flag const unreadable = ui::eq(kind, "entered") && !ui::engaged(number);
    Flag const above = ui::engaged(field("high")) && number > field("high");
    Flag const below = ui::engaged(field("low")) && number < field("low");
    Text const label = ui::cond(
        unreadable, Text{ui::format("Enter to {0} decimal places", {field("places")})},
        ui::cond(
            above, Text{ui::format("Out of spec: above {0}", {field("highText")})},
            ui::cond(below, Text{ui::format("Out of spec: below {0}", {field("lowText")})}, Text{"Within spec"})));
    // `engaged` first: a comparison with a number still being typed is null, and so would the flag be.
    Flag const outOfSpec = ui::engaged(number) && (above || below);
    return Evaluation{.number = number,
                      .shown = shown,
                      .calculated = isMean || isTotal,
                      .unreadable = unreadable,
                      .outOfSpec = outOfSpec,
                      .label = label,
                      .tone = ui::cond(unreadable || outOfSpec, Text{"err"}, Text{"ok"})};
}

Field rowField() {
    return [](char const* name) { return ui::row(name); };
}

Worksheet declare(ui::Scope& screen) {
    Worksheet sheet;
    sheet.width = layout::widthClass(screen);
    sheet.lines = screen.state<ui::Any>("lines", lineList());
    // Compact opens on the second sample, as the design draws it.
    sheet.current = screen.state("current", std::int64_t{1});
    sheet.change = Change{
        .open = screen.state("changeOpen", true),
        .id = screen.state("changeId", std::string{"ib2"}),
        .sample = screen.state("changeSample", std::string{"S-26-104170"}),
        .analyte = screen.state("changeAnalyte", std::string{"Impurity B"}),
        .unit = screen.state("changeUnit", std::string{"%"}),
        .before = screen.state("changeBefore", std::string{"0.12"}),
        .after = screen.state("changeAfter", std::string{"0.21"}),
        .reason = screen.state("changeReason", std::string{"integration"}),
        .comment = screen.state(
            "changeComment", std::string{"Peak 7 integrated with the wrong baseline; reintegrated per SOP-CHR-012."}),
    };
    auto const prep1 = ui::parseDecimal(valueOf(sheet.lines, "a1"), 1);
    auto const prep2 = ui::parseDecimal(valueOf(sheet.lines, "a2"), 1);
    auto const preparations = ui::list({prep1, prep2});
    // The reportable mean is the exact mean rounded once, half up, at the specification's places, as the
    // pharmacopoeias report it; `mean` would round half to even at the preparations' places first.
    sheet.mean =
        screen.let("assayMean", ui::round(ui::sum(preparations) / ui::count(preparations), 1, ui::Rounding::HalfUp));
    sheet.rsd = screen.let("assayRsd", ui::rsd(preparations, 2));
    sheet.total = screen.let("totalImpurities", ui::parseDecimal(valueOf(sheet.lines, "ib2"), 2) +
                                                    ui::parseDecimal(valueOf(sheet.lines, "ia2"), 2));
    auto const counted = [&sheet](auto const& pick) {
        return Count{ui::call("count", {ui::filter(sheet.lines, "line", [&sheet, &pick](ui::AnyHandle const& line) {
                                  return pick(evaluate(sheet, [&line](char const* name) { return line.at(name); }));
                              })})};
    };
    sheet.outOfSpec = screen.let(
        "outOfSpec", counted([](Evaluation const& evaluation) -> ui::AnyHandle { return evaluation.outOfSpec; }));
    sheet.unreadable = screen.let(
        "unreadable", counted([](Evaluation const& evaluation) -> ui::AnyHandle { return evaluation.unreadable; }));
    return sheet;
}

// The lines with @p lineId's value replaced by @p value.
ui::AnyHandle withValue(ui::AnyHandle const& lines, ui::AnyHandle const& lineId, ui::AnyHandle const& value) {
    return ui::map(lines, "line", [&lineId, &value](ui::AnyHandle const& line) -> ui::AnyHandle {
        std::vector<std::pair<std::string, ui::AnyHandle>> fields;
        fields.reserve(kFields.size() + 2);
        for (char const* const name : kFields) {
            fields.emplace_back(name, std::string_view{name} == "value" ? value : line.at(name));
        }
        fields.emplace_back("lowText", line.at("lowText"));
        fields.emplace_back("highText", line.at("highText"));
        return ui::cond(ui::eq(line.at("id"), lineId), ui::obj(fields), line);
    });
}

// A result committed with a new value: the change is recorded as asked about, then made. The question comes before
// the write, since the write changes what `row.value` reads.
ui::Commands commitResult(Worksheet const& sheet) {
    Change const& change = sheet.change;
    return ui::ifThen(
        ui::eq(ui::row("kind"), "entered") && ui::ne(ui::event(), ui::row("value")),
        {ui::set(change.id, ui::row("id")), ui::set(change.sample, ui::row("sample")),
         ui::set(change.analyte, ui::row("analyte")), ui::set(change.unit, ui::row("unit")),
         ui::set(change.before, ui::row("value")), ui::set(change.after, ui::event()),
         ui::set(change.reason, std::string{}), ui::set(change.comment, std::string{}),
         ui::set(sheet.lines, withValue(sheet.lines, ui::row("id"), ui::event())), ui::set(change.open, true)});
}

ui::Commands undoChange(Worksheet const& sheet) {
    return {ui::set(sheet.lines, withValue(sheet.lines, sheet.change.id, sheet.change.before)),
            ui::set(sheet.change.open, false)};
}

Flag canConfirm(Change const& change) {
    return !ui::isEmpty(change.reason) && !ui::isEmpty(ui::call("trim", {change.comment}));
}

ui::Commands confirmChange(Change const& change) {
    return {ui::set(change.open, false),
            ui::notify(ui::format("{0} · {1} changed to {2} {3}; the reason is in the audit trail.",
                                  {change.sample, change.analyte, change.after, change.unit}))};
}

ui::Node reasonField(Change const& change) {
    auto const reasons = ui::list({ui::obj({{"key", "integration"}, {"label", "Integration corrected"}}),
                                   ui::obj({{"key", "transcription"}, {"label", "Transcription error"}}),
                                   ui::obj({{"key", "calculation"}, {"label", "Calculation corrected"}}),
                                   ui::obj({{"key", "other"}, {"label", "Other"}})});
    return layout::captioned(ui::select({.options = reasons,
                                         .selected = change.reason,
                                         .label = "Reason",
                                         .onChoose = ui::set(change.reason, ui::event())}))
        .required(true)
        .testId("reason");
}

ui::Node commentField(Change const& change) {
    return layout::captioned(ui::textInput({.value = change.comment,
                                            .label = "Comment",
                                            .mode = "multiline",
                                            .onChange = ui::set(change.comment, ui::event())}))
        .required(true)
        .testId("comment");
}

ui::AnyHandle changeLine(Change const& change) {
    return ui::format("{0} · {1}: {2} {4} → {3} {4}",
                      {change.sample, change.analyte, change.before, change.after, change.unit});
}

// The change line as runs: the sample's id in the mono face, the two values strong.
ui::AnyHandle changeRuns(Change const& change) {
    return ui::spans({ui::mono(change.sample), ui::format(" · {0}: ", {change.analyte}),
                      ui::strong(ui::format("{0} {1}", {change.before, change.unit})), " → ",
                      ui::strong(ui::format("{0} {1}", {change.after, change.unit}))});
}

// ── The worksheet's parts ────────────────────────────────────────────────────────────────────────────────────────

struct Check {
    char const* name;
    char const* measured;
};

constexpr std::array<Check, 3> kSuitability{{{.name = "Tailing", .measured = "Tailing 1.4 (≤ 2.0)"},
                                             {.name = "RSD", .measured = "RSD of 6 injections 0.6 % (≤ 2.0 %)"},
                                             {.name = "Resolution", .measured = "Resolution 2.8 (≥ 2.0)"}}};

ui::Node suitability() {
    std::vector<ui::Node> checks{ui::text({.text = "System suitability", .role = "section"})};
    for (Check const& check : kSuitability) {
        checks.push_back(ui::row({layout::tag("Pass", "ok"), ui::text({.text = check.measured})},
                                 {.gap = 1, .align = Align::Center})
                             .testId(std::string{"suitability-"} + check.name));
    }
    return ui::panel({ui::row(checks, {.gap = 4, .align = Align::Center})}, {.padding = 1}).testId("suitability");
}

ui::Node resultInput(Worksheet const& sheet, Evaluation const& evaluation, Field const& field) {
    Text const unreadableMessage = ui::format("Enter to {0} decimal places.", {field("places")});
    return ui::textInput({.value = evaluation.shown,
                          .label = ui::format("{0} result, {1}", {field("analyte"), field("sample")}),
                          .align = morph::ui::Align::End,
                          .onCommit = commitResult(sheet)})
        .readonly(evaluation.calculated)
        .errors(
            ui::cond(evaluation.unreadable, ui::list({unreadableMessage}), ui::AnyHandle{std::vector<std::string>{}}))
        .testId("result");
}

ui::Node analyteCell(Evaluation const& evaluation) {
    return ui::row({ui::text({.text = ui::row("analyte")}),
                    layout::tag("Changed", "warn")
                        .visible(ui::eq(ui::row("kind"), "entered") && ui::ne(ui::row("value"), ui::row("original")))
                        .testId("changed"),
                    layout::muted("calculated").visible(evaluation.calculated)},
                   {.gap = 1, .align = Align::Center, .wrap = true});
}

ui::Node evaluationCell(Worksheet const& sheet, Evaluation const& evaluation) {
    return ui::row(
        {layout::tag(evaluation.label, evaluation.tone).testId("evaluation"),
         layout::muted(ui::format("RSD {0} %", {sheet.rsd})).visible(ui::eq(ui::row("kind"), "mean")).testId("rsd")},
        {.gap = 1, .align = Align::Center, .wrap = true});
}

ui::Node resultTable(Worksheet const& sheet) {
    Field const field = rowField();
    Evaluation const evaluation = evaluate(sheet, field);
    // The sample's own column is empty in a result's row: the sample is named by the heading above its results.
    std::vector<ui::TableColumn> const columns{
        {.name = "sample", .label = "Sample", .width = ui::fixed(7), .cell = ui::text({.text = ""})},
        {.name = "analyte", .label = "Analyte", .width = ui::stretch(), .cell = analyteCell(evaluation)},
        {.name = "area", .label = "Peak area", .width = ui::fixed(10), .align = Align::End},
        {.name = "result",
         .label = "Result",
         .width = ui::fixed(10),
         .align = Align::End,
         .cell = resultInput(sheet, evaluation, field)},
        {.name = "unit", .label = "Unit", .width = ui::fixed(6), .cell = ui::text({.text = ui::row("unit")})},
        {.name = "spec",
         .label = "Specification",
         .width = ui::fixed(11),
         .cell = ui::text({.text = ui::row("spec")})},
        {.name = "evaluation",
         .label = "Evaluation",
         .width = ui::fixed(26),
         .cell = evaluationCell(sheet, evaluation)}};
    return ui::table(
               sheet.lines,
               {.key = ui::row("id"),
                .columns = columns,
                // Each sample's results under a heading across the worksheet: its id and its lot.
                .group =
                    ui::TableGroup{
                        .by = ui::row("sample"),
                        .heading = {ui::text({.text =
                                                  ui::format("{0} · lot {1}", {ui::row("key"), ui::row("first.lot")}),
                                              .spans = ui::spans({ui::styled(ui::row("key"),
                                                                             {.strong = true, .mono = true}),
                                                                  ui::strong(ui::format(" · lot {0}",
                                                                                        {ui::row("first.lot")}))})})
                                        .testId("group")}},
                .rowTone = ui::cond(evaluation.outOfSpec, ui::AnyHandle{"err"}, ui::AnyHandle{nullptr})})
        .a11y("Results of worksheet WS-2026-0412")
        .testId("results");
}

ui::AnyHandle outOfSpecMessage(Worksheet const& sheet) {
    return ui::cond(ui::eq(sheet.outOfSpec, std::int64_t{0}), Text{""},
                    ui::cond(ui::eq(sheet.outOfSpec, std::int64_t{1}),
                             Text{"1 result out of specification. Submitting opens an investigation."},
                             Text{ui::format("{0} results out of specification. Submitting opens an investigation.",
                                             {sheet.outOfSpec})}));
}

// Submitting waits for every result to be readable and every change to have its reason.
Flag canSubmit(Worksheet const& sheet) { return !sheet.change.open && ui::eq(sheet.unreadable, std::int64_t{0}); }

ui::Commands submit(Worksheet const& sheet) {
    return ui::notify(ui::cond(ui::eq(sheet.outOfSpec, std::int64_t{0}), Text{"WS-2026-0412 is submitted for review."},
                               Text{"WS-2026-0412 is submitted for review; investigation OOS-2026-031 is opened."}),
                      ui::Role::Success);
}

ui::Node saveButton() {
    return ui::button({.label = "Save draft", .onClick = ui::notify("The worksheet draft is saved")}).testId("save");
}

ui::Node submitButton(Worksheet const& sheet) {
    return ui::button({.label = "Submit for review", .emphasis = ButtonEmphasis::Primary, .onClick = submit(sheet)})
        .enabled(canSubmit(sheet))
        .testId("submit");
}

// ── Expanded ─────────────────────────────────────────────────────────────────────────────────────────────────────

ui::Node changeDialog(Worksheet const& sheet) {
    Change const& change = sheet.change;
    return ui::dialog(ui::element("change"),
                      {reasonField(change), commentField(change),
                       layout::muted("The old value, the new value, you and the time are kept in the audit trail.")},
                      {.title = "Reason for change",
                       .description = changeLine(change),
                       .descriptionSpans = changeRuns(change),
                       .open = change.open,
                       .cancel = undoChange(sheet),
                       .onDismiss = undoChange(sheet),
                       .actions = {{.id = "undo", .label = "Undo change", .onClick = undoChange(sheet)},
                                   {.id = "confirm",
                                    .label = "Confirm change",
                                    .emphasis = ButtonEmphasis::Primary,
                                    .enabled = canConfirm(change),
                                    .onClick = confirmChange(change)}}})
        // The design's 440 px, in the frontend's character widths.
        .sizing(ui::fixed(58))
        .testId("change");
}

ui::Node expandedLayout(Worksheet const& sheet) {
    // Save and Submit stay at the bottom of the page in the bottom bar while the results scroll above them.
    return ui::pinned(
        ui::column({suitability(),
                    ui::panel({resultTable(sheet),
                               ui::column({layout::muted("Results are entered and reported to the decimal places of "
                                                         "their specification.")},
                                          {.padding = 2})},
                              {.padding = 0}),
                    changeDialog(sheet)},
                   {.gap = 2}),
        {.bottom =
             ui::row({ui::text({.text = outOfSpecMessage(sheet), .role = "error"}).sizing(ui::stretch()).testId("oos"),
                      saveButton(), submitButton(sheet)},
                     {.gap = 1, .align = Align::Center})});
}

// ── Compact: one sample at a time ────────────────────────────────────────────────────────────────────────────────

ui::AnyHandle currentSample(Worksheet const& sheet) {
    ui::AnyHandle sample = ui::AnyHandle{kSamples.back()};
    for (std::size_t index = kSamples.size() - 1; index-- > 0;) {
        sample = ui::cond(ui::eq(sheet.current, static_cast<std::int64_t>(index)), ui::AnyHandle{kSamples.at(index)},
                          sample);
    }
    return sample;
}

// The reason asked for inline, under the result it is about.
ui::Node inlineReason(Worksheet const& sheet) {
    Change const& change = sheet.change;
    return ui::column({reasonField(change), commentField(change),
                       ui::row({ui::button({.label = "Undo change", .onClick = undoChange(sheet)}).testId("undo"),
                                ui::button({.label = "Confirm change",
                                            .emphasis = ButtonEmphasis::Primary,
                                            .onClick = confirmChange(change)})
                                    .enabled(canConfirm(change))
                                    .testId("confirm")},
                               {.gap = 1})},
                      {.gap = 2})
        .visible(change.open && ui::eq(change.id, ui::row("id")))
        .testId("inline-reason");
}

ui::Node resultField(Worksheet const& sheet) {
    Field const field = rowField();
    Evaluation const evaluation = evaluate(sheet, field);
    auto const caption =
        ui::cond(evaluation.calculated,
                 Text{ui::format("{0} · calculated · spec {1} {2}", {field("analyte"), field("spec"), field("unit")})},
                 Text{ui::format("{0} · spec {1} {2}", {field("analyte"), field("spec"), field("unit")})});
    // The field carries its unit and its evaluation, outlined red while it is out of specification; an out of
    // specification value is valid and saveable, so the evaluation is a tone, not an error.
    auto const input = resultInput(sheet, evaluation, field)
                           .with("unit", field("unit"))
                           .description(ui::cond(evaluation.unreadable, evaluation.label, Text{""}))
                           .tone(ui::cond(evaluation.outOfSpec, Text{"err"}, Text{"neutral"}));
    return ui::column(
        {ui::row({ui::text({.text = caption}),
                  layout::tag("Changed", "warn")
                      .visible(ui::ne(field("value"), field("original")) && ui::eq(field("kind"), "entered"))
                      .testId("changed")},
                 {.gap = 1, .align = Align::Center}),
         input,
         ui::row({layout::tag(evaluation.label, evaluation.tone).visible(!evaluation.unreadable).testId("evaluation"),
                  layout::muted(ui::format("was {0} {1}", {field("original"), field("unit")}))
                      .visible(ui::eq(field("kind"), "entered") && ui::ne(field("value"), field("original"))),
                  layout::muted(ui::format("RSD {0} %", {sheet.rsd})).visible(ui::eq(field("kind"), "mean"))},
                 {.gap = 1, .align = Align::Center}),
         inlineReason(sheet)},
        {.gap = 1});
}

ui::Node compactLayout(Worksheet const& sheet) {
    auto const sample = currentSample(sheet);
    auto const shown = ui::filter(sheet.lines, "line", [&sample](ui::AnyHandle const& line) -> ui::AnyHandle {
        return ui::eq(line.at("sample"), sample);
    });
    auto const last = static_cast<std::int64_t>(kSamples.size()) - 1;
    // Save and Submit stay at the bottom of the phone screen while the sample's results scroll above them.
    // The sample is stepped in the top bar, which always shows and so joins the head; the out-of-specification line
    // sits above Save and Submit in the bottom bar.
    return ui::pinned(
        ui::column({layout::tag("System suitability passed", "ok").testId("suitability"),
                    ui::forEach(shown, {.key = ui::row("id"), .gap = 3},
                                [&sheet](ui::DeclScope&) { return ui::Nodes{resultField(sheet)}; })
                        .testId("fields")},
                   {.gap = 3}),
        {.top = {ui::row({ui::button({.label = "‹ Previous", .onClick = ui::set(sheet.current, sheet.current - 1)})
                              .enabled(sheet.current > 0)
                              .sizing(ui::stretch())
                              .testId("previous"),
                          ui::button({.label = "Next ›", .onClick = ui::set(sheet.current, sheet.current + 1)})
                              .enabled(sheet.current < last)
                              .sizing(ui::stretch())
                              .testId("next")},
                         {.gap = 1})},
         .bottom = {
             ui::text({.text = outOfSpecMessage(sheet), .role = "error"}).testId("oos"),
             ui::row({saveButton().sizing(ui::stretch()), submitButton(sheet).sizing(ui::stretch())}, {.gap = 1})}});
}

}  // namespace

ui::Screen resultsScreen() {
    return ui::screen("lims-gallery.results", [](ui::Scope& screen) {
        Worksheet const sheet = declare(screen);
        Flag const compact = ui::eq(sheet.width, "compact");
        screen.title("Assay and impurities · Paracetamol 500 mg");
        layout::plainPageWhenCompact(screen, sheet.width);
        screen.eyebrow(ui::cond(compact, Text{"WS-2026-0412 · HPLC-02"},
                                Text{"Worksheet WS-2026-0412 · HPLC-02 · method TM-ASSAY-07 v3"}));
        screen.eyebrowSpans(
            ui::cond(compact, ui::AnyHandle{ui::spans({ui::mono("WS-2026-0412"), " · HPLC-02"})},
                     ui::AnyHandle{ui::spans({"Worksheet ", ui::mono("WS-2026-0412"), " · HPLC-02 · method ",
                                              ui::mono("TM-ASSAY-07"), " v3"})}));
        // The worksheet's state closes the eyebrow's line; the compact head draws none.
        screen.leadPlacement("end");
        screen.status({.text = "In progress", .tone = "info", .visible = !compact});
        auto const sample = currentSample(sheet);
        screen.titleSpans(ui::cond(
            compact,
            ui::AnyHandle{ui::spans(
                {ui::format("Sample {0} of {1} · ", {sheet.current + 1, static_cast<std::int64_t>(kSamples.size())}),
                 ui::mono(sample)})},
            ui::AnyHandle{std::vector<std::string>{}}));
        screen.showErrors(ui::ShowErrors::Always);
        return layout::byWidth(sheet.width, compactLayout(sheet), expandedLayout(sheet))
            .sizing(ui::stretch(), ui::stretch());
    });
}

}  // namespace lims_gallery

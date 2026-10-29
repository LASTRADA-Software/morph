// SPDX-License-Identifier: Apache-2.0
//
// Design 05, the sample list: a dense table of open samples, filtered by search, status and priority, with a bar of
// bulk actions over the rows selected. A row with a result out of specification is tinted. Selecting rows shows the
// bar; assigning them to a worksheet or changing their priority changes those rows and clears the selection. Compact
// shows each row as a card with the same selection, and the bar at the bottom.

#include <array>
#include <format>
#include <string>
#include <utility>
#include <vector>

#include "lims_gallery/screens.hpp"
#include "screen_layout.hpp"

namespace lims_gallery {

namespace {

namespace ui = layout::ui;
using layout::Align;
using layout::ButtonEmphasis;
using layout::Justify;
using Text = ui::Handle<std::string>;
using Flag = ui::Handle<bool>;
using morph::ui::Value;
using morph::ui::ValueList;
using morph::ui::ValueRecord;

// The samples the trail of open work holds beyond this page, as the design counts them.
constexpr char const* kTotal = "312";
// The routine samples the gallery adds to the design's eight.
constexpr int kRoutine = 30;

struct Sample {
    char const* id;
    char const* product;
    char const* shortProduct;
    char const* lot;
    char const* tests;
    char const* priority;
    char const* status;
    char const* statusTone;
    char const* due;
    bool overdue;
    char const* spec;
    char const* specTone;
};

// Every field of a sample record; a bulk action rebuilds a record from them with one field changed.
constexpr std::array<char const*, 13> kFields{"id",       "product", "short",      "lot", "tests",
                                              "priority", "status",  "statusTone", "due", "dueAt",
                                              "overdue",  "spec",    "specTone"};

// When a sample is due, as text that sorts by time: the design's "24-Oct 10:30" is 2026-10-24T10:30, and an overdue
// sample was due before the first of them.
std::string dueAt(std::string const& due) {
    if (due.size() == 12 && due.substr(2, 5) == "-Oct ") {
        return "2026-10-" + due.substr(0, 2) + "T" + due.substr(7);
    }
    return "2026-10-24T07:38";
}

// A sample as a record: a literal, so the whole list is one node of its expression.
Value sample(Sample const& item) {
    return Value{ValueRecord{{"id", Value{item.id}},
                             {"product", Value{item.product}},
                             {"short", Value{item.shortProduct}},
                             {"lot", Value{item.lot}},
                             {"tests", Value{item.tests}},
                             {"priority", Value{item.priority}},
                             {"status", Value{item.status}},
                             {"statusTone", Value{item.statusTone}},
                             {"due", Value{item.due}},
                             {"dueAt", Value{dueAt(item.due)}},
                             {"overdue", Value{item.overdue}},
                             {"spec", Value{item.spec}},
                             {"specTone", Value{item.specTone}}}};
}

// Routine water samples beyond the design's eight, so the list has a second page to turn to.
std::vector<Value> routineSamples() {
    std::vector<Value> routine;
    for (int index = 0; index < kRoutine; ++index) {
        std::string const day = std::to_string(29 + (index / 10));
        std::string const hour = std::to_string(10 + (index % 10));
        routine.emplace_back(
            ValueRecord{{"id", Value{"S-26-1042" + std::to_string(40 + index)}},
                        {"product", Value{"Purified water · loop " + std::to_string((index % 4) + 1)}},
                        {"short", Value{"Purified water"}},
                        {"lot", Value{""}},
                        {"tests", Value{"TOC, conductivity"}},
                        {"priority", Value{"Routine"}},
                        {"status", Value{"Registered"}},
                        {"statusTone", Value{"neutral"}},
                        {"due", Value{std::format("{}-Oct {}:00", day, hour)}},
                        {"dueAt", Value{std::format("2026-10-{}T{}:00", day, hour)}},
                        {"overdue", Value{false}},
                        {"spec", Value{""}},
                        {"specTone", Value{""}}});
    }
    return routine;
}

ui::AnyHandle samples() {
    ValueList designed{
        sample({.id = "S-26-104170",
                .product = "Paracetamol 500 mg",
                .shortProduct = "Paracetamol 500 mg",
                .lot = "L-26-0377",
                .tests = "Assay, impurities",
                .priority = "STAT",
                .status = "In review",
                .statusTone = "info",
                .due = "22 min overdue",
                .overdue = true,
                .spec = "Out of spec · 1",
                .specTone = "err"}),
        sample({.id = "S-26-104198",
                .product = "Lactose monohydrate",
                .shortProduct = "Lactose monohydrate",
                .lot = "RM-0912",
                .tests = "Water (KF)",
                .priority = "Urgent",
                .status = "Queued",
                .statusTone = "neutral",
                .due = "24-Oct 10:30",
                .overdue = false,
                .spec = "",
                .specTone = ""}),
        sample({.id = "S-26-104221",
                .product = "Purified water · loop 3",
                .shortProduct = "Purified water",
                .lot = "",
                .tests = "TOC, conductivity",
                .priority = "Routine",
                .status = "Queued",
                .statusTone = "neutral",
                .due = "24-Oct 14:00",
                .overdue = false,
                .spec = "",
                .specTone = ""}),
        sample({.id = "S-26-104176",
                .product = "Ibuprofen 400 mg",
                .shortProduct = "Ibuprofen 400 mg",
                .lot = "L-26-0381",
                .tests = "Dissolution",
                .priority = "Routine",
                .status = "Measured",
                .statusTone = "info",
                .due = "24-Oct 16:00",
                .overdue = false,
                .spec = "Within spec",
                .specTone = "ok"}),
        sample({.id = "S-26-104159",
                .product = "Ibuprofen 400 mg",
                .shortProduct = "Ibuprofen 400 mg",
                .lot = "L-26-0379",
                .tests = "Assay",
                .priority = "Routine",
                .status = "Investigation",
                .statusTone = "warn",
                .due = "25-Oct 09:00",
                .overdue = false,
                .spec = "Out of spec · 1",
                .specTone = "err"}),
        sample({.id = "S-26-104150",
                .product = "Microcrystalline cellulose",
                .shortProduct = "Microcrystalline cellulose",
                .lot = "RM-0907",
                .tests = "Identity (IR)",
                .priority = "Routine",
                .status = "Registered",
                .statusTone = "neutral",
                .due = "25-Oct 12:00",
                .overdue = false,
                .spec = "",
                .specTone = ""}),
        sample({.id = "S-26-104148",
                .product = "Paracetamol 500 mg",
                .shortProduct = "Paracetamol 500 mg",
                .lot = "L-26-0376",
                .tests = "Stability T6, assay",
                .priority = "Routine",
                .status = "In progress",
                .statusTone = "info",
                .due = "26-Oct 08:00",
                .overdue = false,
                .spec = "Out of trend",
                .specTone = "warn"}),
        sample({.id = "S-26-104131",
                .product = "Purified water · loop 1",
                .shortProduct = "Purified water",
                .lot = "",
                .tests = "Micro, TAMC",
                .priority = "Routine",
                .status = "Incubating",
                .statusTone = "info",
                .due = "28-Oct 08:00",
                .overdue = false,
                .spec = "",
                .specTone = ""}),
    };
    for (Value& more : routineSamples()) {
        designed.push_back(std::move(more));
    }
    return ui::AnyHandle{Value{std::move(designed)}};
}

struct Samples {
    ui::Element assignDialog = ui::element("assign");
    ui::Element priorityDialog = ui::element("priority");
    ui::Element filtersDrawer = ui::element("filters");
    ui::AnyHandle width;
    Flag compact;
    ui::AnyHandle all;
    ui::AnyHandle shown;
    ui::AnyHandle selected;
    ui::Handle<std::int64_t> selectedCount;
    Flag everySelected;
    Flag allOnPage;
    Text search;
    Text status;
    Text priority;
    Text worksheet;
    Text newPriority;
    // The table's sort, shared with the compact cards, the page it shows and how many rows a page holds.
    ui::AnyHandle order;
    ui::Handle<std::int64_t> page;
    ui::Handle<std::int64_t> pageSize;
    // How many cards compact lists before "Load more".
    ui::Handle<std::int64_t> cardCount;
};

ui::AnyHandle shownSamples(Samples const& list) {
    return ui::filter(list.all, "entry", [&list](ui::AnyHandle const& item) -> ui::AnyHandle {
        auto const has = [&list](ui::AnyHandle const& text) {
            return ui::Handle<bool>{ui::call("contains", {text, list.search})};
        };
        Flag const searched =
            ui::isEmpty(list.search) || has(item.at("id")) || has(item.at("product")) || has(item.at("lot"));
        Flag const statusOk = ui::eq(list.status, "open") || ui::eq(item.at("status"), list.status);
        Flag const priorityOk = ui::eq(list.priority, "any") || ui::eq(item.at("priority"), list.priority);
        return searched && statusOk && priorityOk;
    });
}

Samples declare(ui::Scope& screen) {
    Samples list;
    list.width = layout::widthClass(screen);
    list.compact = screen.let("compact", ui::eq(list.width, "compact"));
    list.all = screen.state<ui::Any>("samples", samples());
    list.search = screen.state("search", std::string{});
    list.status = screen.state("status", std::string{"open"});
    list.priority = screen.state("priorityFilter", std::string{"any"});
    list.worksheet = screen.state("worksheet", std::string{"WS-2026-0415"});
    list.newPriority = screen.state("newPriority", std::string{"Urgent"});
    // The design opens with three rows selected.
    list.selected = screen.state<ui::Any>(
        "selected", ui::AnyHandle{std::vector<std::string>{"S-26-104170", "S-26-104221", "S-26-104176"}});
    list.everySelected = screen.state("everySelected", false);
    list.order = screen.state<ui::Any>("order", ui::list({ui::sortKey("due")}));
    list.page = screen.state("page", std::int64_t{0});
    list.pageSize = screen.state("pageSize", std::int64_t{25});
    list.cardCount = screen.state("cardCount", std::int64_t{25});
    list.shown = screen.let("shown", shownSamples(list));
    list.selectedCount = screen.let("selectedCount", ui::Handle<std::int64_t>{ui::call("count", {list.selected})});
    auto const shownCount = ui::Handle<std::int64_t>{ui::call("count", {list.shown})};
    list.allOnPage = screen.let("allOnPage", shownCount > 0 && ui::eq(list.selectedCount, shownCount));
    return list;
}

ui::AnyHandle ids(ui::AnyHandle const& rows) {
    return ui::map(rows, "entry", [](ui::AnyHandle const& item) { return item.at("id"); });
}

ui::Commands clearSelection(Samples const& list) {
    return {ui::set(list.selected, ui::AnyHandle{std::vector<std::string>{}}), ui::set(list.everySelected, false)};
}

// Every selected sample with @p field set to @p value, then the selection cleared.
ui::Commands updateSelected(Samples const& list, std::string const& field, ui::AnyHandle const& value) {
    auto const updated = ui::map(list.all, "entry", [&](ui::AnyHandle const& item) -> ui::AnyHandle {
        std::vector<std::pair<std::string, ui::AnyHandle>> members;
        members.reserve(kFields.size());
        for (char const* const name : kFields) {
            members.emplace_back(name, name == field ? value : item.at(name));
        }
        return ui::cond(ui::includes(list.selected, item.at("id")), ui::obj(members), item);
    });
    return {ui::set(list.all, updated), ui::seq(clearSelection(list))};
}

// A row's own check box: checked while the row is selected; toggling adds or removes it.
ui::Node selectBox(Samples const& list) {
    auto const rowId = ui::row("id");
    auto const without = ui::filter(
        list.selected, "entry", [&rowId](ui::AnyHandle const& item) -> ui::AnyHandle { return ui::ne(item, rowId); });
    return ui::checkbox({.label = "",
                         .checked = ui::includes(list.selected, rowId),
                         .onToggle = {ui::set(list.selected,
                                              ui::cond(ui::Handle<bool>{ui::event()},
                                                       ui::concatLists({list.selected, ui::list({rowId})}), without)),
                                      ui::set(list.everySelected, false)}})
        .a11y(ui::format("Select {0}", {rowId}))
        .testId("select-row");
}

// ── The tags ─────────────────────────────────────────────────────────────────────────────────────────────────────

ui::AnyHandle priorityTone(ui::AnyHandle const& priority) {
    return ui::cond(ui::eq(priority, "STAT"), Text{"err"},
                    ui::cond(ui::eq(priority, "Urgent"), Text{"warn"}, Text{"neutral"}));
}

ui::Node specCell() {
    auto const spec = ui::row("spec");
    return ui::row({layout::tag(spec, ui::row("specTone")).visible(!ui::isEmpty(spec)),
                    layout::muted("—").visible(ui::isEmpty(spec))});
}

ui::Node dueText() {
    return ui::text({.text = ui::row("due"),
                     .role = ui::cond(ui::Handle<bool>{ui::row("overdue")}, Text{"error"}, Text{"normal"})});
}

// ── The bulk actions ─────────────────────────────────────────────────────────────────────────────────────────────

ui::AnyHandle selectedLabel(Samples const& list, bool compact) {
    if (compact) {
        return ui::cond(list.everySelected, ui::format("All {0} selected", {ui::AnyHandle{kTotal}}),
                        ui::format("{0} selected", {list.selectedCount}));
    }
    return ui::cond(list.everySelected, ui::format("All {0} samples selected", {ui::AnyHandle{kTotal}}),
                    ui::format("{0} selected on this page", {list.selectedCount}));
}

ui::Commands printLabels(Samples const& list) {
    return ui::notify(
        ui::format("Printing labels for {0} samples",
                   {ui::cond(list.everySelected, ui::AnyHandle{kTotal}, ui::AnyHandle{list.selectedCount})}));
}

ui::Node bulkBar(Samples const& list) {
    return ui::panel(
               {ui::row(
                   {ui::text({.text = selectedLabel(list, false), .role = "section"}).testId("selected"),
                    ui::button(
                        {.label = ui::format("Select all {0}", {ui::AnyHandle{kTotal}}),
                         .emphasis = ButtonEmphasis::Ghost,
                         .onClick = {ui::set(list.selected, ids(list.shown)), ui::set(list.everySelected, true)}})
                        .visible(!list.everySelected)
                        .testId("select-all"),
                    ui::spacer().sizing(ui::stretch()),
                    ui::button({.label = "Assign to worksheet…", .onClick = ui::open(list.assignDialog)})
                        .testId("assign"),
                    ui::button({.label = "Change priority…", .onClick = ui::open(list.priorityDialog)})
                        .testId("change-priority"),
                    ui::button({.label = "Print labels", .onClick = printLabels(list)}),
                    ui::button({.label = "Clear", .emphasis = ButtonEmphasis::Ghost, .onClick = clearSelection(list)})
                        .testId("clear")},
                   {.gap = 1, .align = Align::Center})},
               {.padding = 1})
        .surface("selected")
        .a11y("Bulk actions")
        .visible(list.selectedCount > 0)
        .testId("bulk");
}

ui::Node assignDialog(Samples const& list) {
    auto const worksheets = ui::list({ui::obj({{"key", "WS-2026-0415"}, {"label", "WS-2026-0415 · ELISA, HCP"}}),
                                      ui::obj({{"key", "WS-2026-0416"}, {"label", "WS-2026-0416 · HPLC assay"}}),
                                      ui::obj({{"key", "WS-2026-0417"}, {"label", "WS-2026-0417 · Water (KF)"}})});
    return ui::dialog(
        list.assignDialog,
        {layout::captioned(ui::select({.options = worksheets,
                                       .selected = list.worksheet,
                                       .label = "Worksheet",
                                       .onChoose = ui::set(list.worksheet, ui::event())}))
             .testId("worksheet")},
        {.title = ui::format("Assign {0} samples to a worksheet", {list.selectedCount}),
         .cancel = ui::close(ui::self),
         .actions = {
             {.id = "cancel", .label = "Cancel", .onClick = ui::close(ui::self)},
             {.id = "assign",
              .label = "Assign",
              .emphasis = ButtonEmphasis::Primary,
              .onClick = {ui::notify(ui::format("{0} samples assigned to {1}", {list.selectedCount, list.worksheet})),
                          ui::seq(updateSelected(list, "status", ui::AnyHandle{"Assigned"})), ui::close(ui::self)}}}});
}

ui::Node priorityDialog(Samples const& list) {
    auto const priorities =
        ui::list({ui::obj({{"key", "STAT"}, {"label", "STAT"}}), ui::obj({{"key", "Urgent"}, {"label", "Urgent"}}),
                  ui::obj({{"key", "Routine"}, {"label", "Routine"}})});
    return ui::dialog(list.priorityDialog,
                      {layout::captioned(ui::select({.options = priorities,
                                                     .selected = list.newPriority,
                                                     .label = "Priority",
                                                     .onChoose = ui::set(list.newPriority, ui::event())}))
                           .testId("new-priority")},
                      {.title = ui::format("Change the priority of {0} samples", {list.selectedCount}),
                       .cancel = ui::close(ui::self),
                       .actions = {{.id = "cancel", .label = "Cancel", .onClick = ui::close(ui::self)},
                                   {.id = "apply",
                                    .label = "Change priority",
                                    .emphasis = ButtonEmphasis::Primary,
                                    .onClick = {ui::seq(updateSelected(list, "priority", list.newPriority)),
                                                ui::close(ui::self)}}}});
}

// ── Filters ──────────────────────────────────────────────────────────────────────────────────────────────────────

// The search and the filters are head tools on the expanded layout; compact draws them in the page and its drawer.
ui::Node searchField(Samples const& list) {
    return ui::textInput({.value = list.search,
                          .label = "Search samples",
                          .placeholder = "Sample, lot or product",
                          .onChange = ui::set(list.search, ui::event())})
        .testId("search");
}

ui::AnyHandle statusOptions() {
    std::vector<ui::AnyHandle> options{ui::obj({{"key", "open"}, {"label", "Status: open"}})};
    for (char const* const status :
         {"Registered", "Queued", "In progress", "Incubating", "Measured", "In review", "Investigation", "Assigned"}) {
        options.push_back(ui::obj({{"key", status}, {"label", std::string{"Status: "} + status}}));
    }
    return ui::list(options);
}

ui::AnyHandle priorityOptions() {
    return ui::list({ui::obj({{"key", "any"}, {"label", "Priority: any"}}),
                     ui::obj({{"key", "STAT"}, {"label", "Priority: STAT"}}),
                     ui::obj({{"key", "Urgent"}, {"label", "Priority: urgent"}}),
                     ui::obj({{"key", "Routine"}, {"label", "Priority: routine"}})});
}

void declareTools(ui::Scope& screen, Samples const& list) {
    screen.tool(ui::HeadSearchOptions{.id = "search",
                                      .label = "Search samples",
                                      .value = list.search,
                                      .placeholder = "Sample, lot or product",
                                      .onChange = {ui::set(list.search, ui::event())},
                                      .width = 30,
                                      .visible = !list.compact});
    screen.tool(ui::HeadSelectOptions{.id = "status",
                                      .label = "Status",
                                      .options = statusOptions(),
                                      .selected = list.status,
                                      .onChoose = {ui::set(list.status, ui::event())},
                                      .width = 19,
                                      .visible = !list.compact});
    screen.tool(ui::HeadSelectOptions{.id = "priority",
                                      .label = "Priority",
                                      .options = priorityOptions(),
                                      .selected = list.priority,
                                      .onChoose = {ui::set(list.priority, ui::event())},
                                      .width = 18,
                                      .visible = !list.compact});
}

ui::Node statusFilter(Samples const& list) {
    return ui::select({.options = statusOptions(),
                       .selected = list.status,
                       .label = "Status",
                       .onChoose = ui::set(list.status, ui::event())})
        .testId("status");
}

ui::Node priorityFilter(Samples const& list) {
    return ui::select({.options = priorityOptions(),
                       .selected = list.priority,
                       .label = "Priority",
                       .onChoose = ui::set(list.priority, ui::event())})
        .testId("priority-filter");
}

ui::Commands clearFilters(Samples const& list) {
    return {ui::set(list.search, std::string{}), ui::set(list.status, std::string{"open"}),
            ui::set(list.priority, std::string{"any"})};
}

ui::Nodes noSamples(Samples const& list) {
    return {ui::emptyState({.title = "No samples match",
                            .text = "Change the search or the filters to see open samples.",
                            .action = ui::ActionPart{.label = "Clear filters", .onClick = clearFilters(list)}})
                .testId("no-samples")};
}

// ── Expanded ─────────────────────────────────────────────────────────────────────────────────────────────────────

ui::Node samplesTable(Samples const& list) {
    auto const lotLine = ui::cond(ui::isEmpty(ui::row("lot")), ui::AnyHandle{ui::row("product")},
                                  ui::AnyHandle{ui::format("{0} · {1}", {ui::row("product"), ui::row("lot")})});
    return ui::table(
               list.shown,
               {.key = ui::row("id"),
                .columns =
                    {{.name = "id", .label = "Sample", .width = ui::fixed(12), .role = "mono", .sortable = true},
                     {.name = "product",
                      .label = "Product · lot",
                      .width = ui::stretch(),
                      .cell = ui::text({.text = lotLine})},
                     {.name = "tests", .label = "Tests", .width = ui::fixed(16)},
                     {.name = "priority",
                      .label = "Priority",
                      .width = ui::fixed(8),
                      .cell = layout::outlineTag(ui::row("priority"), priorityTone(ui::row("priority")))},
                     {.name = "status",
                      .label = "Status",
                      .width = ui::fixed(12),
                      .cell = layout::tag(ui::row("status"), ui::row("statusTone"))},
                     {.name = "due",
                      .label = "Due",
                      .width = ui::fixed(13),
                      .cell = dueText(),
                      .sortable = true,
                      .sortBy = ui::row("dueAt")},
                     {.name = "spec", .label = "Specification", .width = ui::fixed(14), .cell = specCell()}},
                // The table draws the selection the cards share: its own boxes, and a header box for the page.
                .selectionMode = ui::Selection::Multiple,
                .selection = list.selected,
                .onSelectionChange = {ui::set(list.selected, ui::event()), ui::set(list.everySelected, false)},
                .selectColumn = true,
                .selectAllLabel = "Select all on this page",
                .rowLabel = ui::row("id"),
                // A new order starts at its first rows.
                .sort = list.order,
                .onSortChange = {ui::set(list.order, ui::event()), ui::set(list.page, std::int64_t{0})},
                .pageSize = list.pageSize,
                .page = list.page,
                .rowTone = ui::cond(ui::eq(ui::row("specTone"), "err"), ui::AnyHandle{"err"}, ui::AnyHandle{nullptr}),
                .empty = noSamples(list)})
        .id(ui::element("sampleTable"))
        .a11y("Samples")
        .testId("samples");
}

// The table's pager: where the page is, how many rows a page holds, and the previous and next page.
ui::Node pageFooter(Samples const& list) {
    auto const sizes = ui::list({ui::obj({{"key", std::int64_t{25}}, {"label", "25"}}),
                                 ui::obj({{"key", std::int64_t{50}}, {"label", "50"}}),
                                 ui::obj({{"key", std::int64_t{100}}, {"label", "100"}})});
    auto const page = ui::Handle<std::int64_t>{ui::ref("sampleTable.page")};
    auto const pageCount = ui::Handle<std::int64_t>{ui::ref("sampleTable.pageCount")};
    return ui::row(
               {layout::muted(
                    ui::format("{0}–{1} of {2}", {ui::ref("sampleTable.pageFirst"), ui::ref("sampleTable.pageLast"),
                                                  ui::ref("sampleTable.viewCount")}))
                    .sizing(ui::stretch())
                    .testId("range"),
                ui::select({.options = sizes,
                            .selected = list.pageSize,
                            .label = "Rows",
                            .onChoose = {ui::set(list.pageSize, ui::event()), ui::set(list.page, std::int64_t{0})}})
                    .sizing(ui::fixed(8))
                    .testId("page-size"),
                ui::button({.label = "‹", .onClick = ui::set(list.page, page - 1)})
                    .a11y("Previous page")
                    .enabled(page > 0)
                    .testId("previous-page"),
                ui::button({.label = "›", .onClick = ui::set(list.page, page + 1)})
                    .a11y("Next page")
                    .enabled(page < pageCount - 1)
                    .testId("next-page")},
               {.gap = 1, .align = Align::Center})
        .testId("pager");
}

ui::Node expandedLayout(Samples const& list) {
    return ui::pinned(ui::column(
        {bulkBar(list), ui::panel({samplesTable(list), pageFooter(list)}, {.padding = 0}),
         layout::muted("Rows with a tinted background have a result out of specification. A checked box marks a "
                       "row as selected."),
         assignDialog(list), priorityDialog(list)},
        {.gap = 2}));
}

// ── Compact: rows become cards, same selection ───────────────────────────────────────────────────────────────────

ui::Node cardContent(Samples const& list) {
    auto const spec = ui::row("spec");
    return ui::row(
        {selectBox(list),
         ui::column({ui::row({layout::section(ui::row("id")), dueText()}, {.gap = 1, .justify = Justify::Between}),
                     ui::text({.text = ui::format("{0} · {1}", {ui::row("short"), ui::row("tests")})}),
                     ui::row({layout::outlineTag(ui::row("priority"), priorityTone(ui::row("priority"))),
                              layout::tag(ui::row("status"), ui::row("statusTone")),
                              layout::tag(spec, ui::row("specTone")).visible(!ui::isEmpty(spec))},
                             {.gap = 1})},
                    {.gap = 0})
             .sizing(ui::stretch())},
        {.gap = 2, .align = Align::Start});
}

// A card is tinted while its sample is out of specification.
ui::Node sampleCard(Samples const& list) {
    Flag const outOfSpec = ui::eq(ui::row("specTone"), "err");
    return ui::panel({cardContent(list)}, {.padding = 2}).surface(ui::cond(outOfSpec, Text{"outOfSpec"}, Text{""}));
}

ui::Node compactFilters(Samples const& list) {
    return ui::drawer(
        list.filtersDrawer, {statusFilter(list), priorityFilter(list)},
        {.title = "Filters",
         .side = "end",
         .cancel = ui::close(ui::self),
         .actions = {
             {.id = "clear", .label = "Clear filters", .onClick = clearFilters(list)},
             {.id = "done", .label = "Done", .emphasis = ButtonEmphasis::Primary, .onClick = ui::close(ui::self)}}});
}

ui::Node compactBulkBar(Samples const& list) {
    return ui::panel({ui::row({ui::text({.text = selectedLabel(list, true), .role = "section"})
                                   .sizing(ui::stretch())
                                   .testId("selected"),
                               ui::button({.label = "Assign to worksheet", .onClick = ui::open(list.assignDialog)})
                                   .testId("assign"),
                               ui::menuButton({.icon = "more"},
                                              {{.label = "Change priority…", .onClick = ui::open(list.priorityDialog)},
                                               {.label = "Print labels", .onClick = printLabels(list)},
                                               {.label = "Clear selection", .onClick = clearSelection(list)}})
                                   .a11y("More bulk actions")
                                   .testId("more-bulk")},
                              {.gap = 1, .align = Align::Center})},
                     {.padding = 1})
        .surface("selected")
        .a11y("Bulk actions")
        .visible(list.selectedCount > 0)
        .testId("bulk");
}

// The sort's first key: its column and its direction.
ui::AnyHandle sortColumn() { return ui::ref("order.0.column"); }
ui::AnyHandle sortDirection() { return ui::ref("order.0.direction"); }

// The cards in the table's order: its sort's first key, by due time or by id.
ui::AnyHandle cardOrder(Samples const& list) {
    return ui::sorted(
        list.shown, "entry",
        [](ui::AnyHandle const& item) {
            return ui::cond(ui::eq(sortColumn(), "id"), item.at("id"), item.at("dueAt"));
        },
        sortDirection());
}

// Compact's sort: a choice of the sortable columns and directions, each one key of the order the table shares.
ui::Node sortChoice(Samples const& list) {
    auto const option = [](char const* column, char const* direction, char const* label) {
        return ui::obj({{"key", std::string{column} + "-" + direction}, {"label", label}});
    };
    auto const choices =
        ui::list({option("due", "asc", "Sort: due, earliest first"), option("due", "desc", "Sort: due, latest first"),
                  option("id", "asc", "Sort: sample, ascending"), option("id", "desc", "Sort: sample, descending")});
    auto const chosen = ui::Handle<std::string>{ui::event()};
    auto const key =
        ui::obj({{"column", ui::cond(Flag{ui::call("contains", {chosen, "due"})}, Text{"due"}, Text{"id"})},
                 {"direction", ui::cond(Flag{ui::call("contains", {chosen, "desc"})}, Text{"desc"}, Text{"asc"})}});
    return ui::select({.options = choices,
                       .selected = ui::format("{0}-{1}", {sortColumn(), sortDirection()}),
                       .label = "Sort",
                       .onChoose = {ui::set(list.order, ui::list({key})), ui::set(list.page, std::int64_t{0})}})
        .testId("sort");
}

ui::Node compactLayout(Samples const& list) {
    auto const shownCount = ui::Handle<std::int64_t>{ui::call("count", {list.shown})};
    auto const active =
        ui::cond(ui::eq(list.status, "open"), ui::Handle<std::int64_t>{0}, ui::Handle<std::int64_t>{1}) +
        ui::cond(ui::eq(list.priority, "any"), ui::Handle<std::int64_t>{0}, ui::Handle<std::int64_t>{1});
    // The bulk bar stays at the bottom of the phone screen while the cards scroll above it.
    return ui::pinned(
        ui::column(
            {searchField(list),
             ui::row(
                 {ui::button({.label = ui::format("Filters · {0}", {active}), .onClick = ui::open(list.filtersDrawer)})
                      .sizing(ui::stretch())
                      .testId("open-filters"),
                  sortChoice(list).sizing(ui::stretch())},
                 {.gap = 1}),
             ui::forEach(ui::slice(cardOrder(list), std::int64_t{0}, list.cardCount), {.key = ui::row("id"), .gap = 1},
                         [&list](ui::DeclScope&) { return ui::Nodes{sampleCard(list)}; })
                 .testId("cards"),
             ui::column(noSamples(list)).visible(ui::Handle<bool>{ui::isEmpty(list.shown)}),
             ui::button({.label = ui::format("Load more · {0} of {1}", {list.cardCount, shownCount}),
                         .onClick = ui::set(list.cardCount, list.cardCount + 25)})
                 .visible(list.cardCount < shownCount)
                 .testId("load-more"),
             compactFilters(list), assignDialog(list), priorityDialog(list)},
            {.gap = 2}),
        {.bottom = compactBulkBar(list)});
}

}  // namespace

ui::Screen samplesScreen() {
    return ui::screen("lims-gallery.samples", [](ui::Scope& screen) {
        Samples const list = declare(screen);
        screen.title("Samples");
        declareTools(screen, list);
        screen.action({.id = "register",
                       .label = ui::cond(list.compact, Text{"Register"}, Text{"Register samples"}),
                       .emphasis = ButtonEmphasis::Primary,
                       .onClick = ui::notify("Registering samples opens the registration form")});
        return layout::byWidth(list.width, compactLayout(list), expandedLayout(list))
            .sizing(ui::stretch(), ui::stretch());
    });
}

}  // namespace lims_gallery

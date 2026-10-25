// SPDX-License-Identifier: Apache-2.0
//
// Design 05, the sample list: a dense table of open samples, filtered by search, status and priority, with a bar of
// bulk actions over the rows selected. A row with a result out of specification is tinted. Selecting rows shows the
// bar; assigning them to a worksheet or changing their priority changes those rows and clears the selection. Compact
// shows each row as a card with the same selection, and the bar at the bottom.

#include <array>
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

// The samples the trail of open work holds beyond this page, as the design counts them.
constexpr char const* kTotal = "312";

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
constexpr std::array<char const*, 12> kFields{"id",     "product",    "short", "lot",     "tests", "priority",
                                              "status", "statusTone", "due",   "overdue", "spec",  "specTone"};

ui::AnyHandle sample(Sample const& item) {
    return ui::obj({{"id", item.id},
                    {"product", item.product},
                    {"short", item.shortProduct},
                    {"lot", item.lot},
                    {"tests", item.tests},
                    {"priority", item.priority},
                    {"status", item.status},
                    {"statusTone", item.statusTone},
                    {"due", item.due},
                    {"overdue", item.overdue},
                    {"spec", item.spec},
                    {"specTone", item.specTone}});
}

ui::AnyHandle samples() {
    return ui::list({
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
    });
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

ui::Node selectPage(Samples const& list) {
    return ui::checkbox({.label = "Select all on this page",
                         .checked = list.allOnPage,
                         .onToggle = {ui::set(list.selected, ui::cond(ui::Handle<bool>{ui::event()}, ids(list.shown),
                                                                      ui::AnyHandle{std::vector<std::string>{}})),
                                      ui::set(list.everySelected, false)}})
        .testId("select-page");
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

ui::Node searchField(Samples const& list) {
    return ui::textInput({.value = list.search,
                          .label = "Search samples",
                          .placeholder = "Sample, lot or product",
                          .onChange = ui::set(list.search, ui::event())})
        .testId("search");
}

ui::Node statusFilter(Samples const& list) {
    std::vector<ui::AnyHandle> options{ui::obj({{"key", "open"}, {"label", "Status: open"}})};
    for (char const* const status :
         {"Registered", "Queued", "In progress", "Incubating", "Measured", "In review", "Investigation", "Assigned"}) {
        options.push_back(ui::obj({{"key", status}, {"label", std::string{"Status: "} + status}}));
    }
    return ui::select({.options = ui::list(options),
                       .selected = list.status,
                       .label = "Status",
                       .onChoose = ui::set(list.status, ui::event())})
        .testId("status");
}

ui::Node priorityFilter(Samples const& list) {
    auto const options = ui::list({ui::obj({{"key", "any"}, {"label", "Priority: any"}}),
                                   ui::obj({{"key", "STAT"}, {"label", "Priority: STAT"}}),
                                   ui::obj({{"key", "Urgent"}, {"label", "Priority: urgent"}}),
                                   ui::obj({{"key", "Routine"}, {"label", "Priority: routine"}})});
    return ui::select({.options = options,
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
                .columns = {{.name = "select", .label = "", .width = ui::fixed(4), .cell = selectBox(list)},
                            {.name = "id", .label = "Sample", .width = ui::fixed(12)},
                            {.name = "product",
                             .label = "Product · lot",
                             .width = ui::stretch(),
                             .cell = ui::text({.text = lotLine})},
                            {.name = "tests", .label = "Tests", .width = ui::fixed(16)},
                            {.name = "priority",
                             .label = "Priority",
                             .width = ui::fixed(8),
                             .cell = layout::tag(ui::row("priority"), priorityTone(ui::row("priority")))},
                            {.name = "status",
                             .label = "Status",
                             .width = ui::fixed(12),
                             .cell = layout::tag(ui::row("status"), ui::row("statusTone"))},
                            // The rows come sorted by due time; a table over rows does not sort.
                            {.name = "due", .label = "Due ▲", .width = ui::fixed(13), .cell = dueText()},
                            {.name = "spec", .label = "Specification", .width = ui::fixed(14), .cell = specCell()}},
                .rowTone = ui::cond(ui::eq(ui::row("specTone"), "err"), ui::AnyHandle{"err"}, ui::AnyHandle{nullptr}),
                .empty = noSamples(list)})
        .a11y("Samples")
        .testId("samples");
}

ui::Node pageFooter() {
    auto const sizes = ui::list({ui::obj({{"key", "25"}, {"label", "25"}}), ui::obj({{"key", "50"}, {"label", "50"}}),
                                 ui::obj({{"key", "100"}, {"label", "100"}})});
    return ui::row(
               {layout::muted(ui::format("1–25 of {0}", {ui::AnyHandle{kTotal}})).sizing(ui::stretch()),
                ui::select({.options = sizes, .selected = "25", .label = "Rows"}).enabled(false).sizing(ui::fixed(8)),
                ui::button({.label = "‹"}).a11y("Previous page").enabled(false),
                ui::button({.label = "›"}).a11y("Next page").enabled(false)},
               {.gap = 1, .align = Align::Center})
        .testId("pager");
}

ui::Node expandedLayout(Samples const& list) {
    auto const toolbar =
        ui::row({selectPage(list), ui::spacer().sizing(ui::stretch()), searchField(list).sizing(ui::fixed(30)),
                 statusFilter(list).sizing(ui::fixed(19)), priorityFilter(list).sizing(ui::fixed(18))},
                {.gap = 1, .align = Align::Center});
    return ui::column(
        {toolbar, bulkBar(list), ui::panel({samplesTable(list), pageFooter()}, {.padding = 0}),
         layout::muted("Rows with a tinted background have a result out of specification. A checked box marks a "
                       "row as selected."),
         assignDialog(list), priorityDialog(list)},
        {.gap = 2});
}

// ── Compact: rows become cards, same selection ───────────────────────────────────────────────────────────────────

ui::Node cardContent(Samples const& list) {
    auto const spec = ui::row("spec");
    return ui::row(
        {selectBox(list),
         ui::column({ui::row({layout::section(ui::row("id")), dueText()}, {.gap = 1, .justify = Justify::Between}),
                     ui::text({.text = ui::format("{0} · {1}", {ui::row("short"), ui::row("tests")})}),
                     ui::row({layout::tag(ui::row("priority"), priorityTone(ui::row("priority"))),
                              layout::tag(ui::row("status"), ui::row("statusTone")),
                              layout::tag(spec, ui::row("specTone")).visible(!ui::isEmpty(spec))},
                             {.gap = 1})},
                    {.gap = 0})
             .sizing(ui::stretch())},
        {.gap = 2, .align = Align::Start});
}

// A card is tinted while its sample is out of specification. A surface is a literal, so the tint is a second card
// shown in its place.
ui::Node sampleCard(Samples const& list) {
    Flag const outOfSpec = ui::eq(ui::row("specTone"), "err");
    return ui::column({ui::panel({cardContent(list)}, {.padding = 2}).surface("outOfSpec").visible(outOfSpec),
                       ui::panel({cardContent(list)}, {.padding = 2}).visible(!outOfSpec)});
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
    return ui::panel(
               {ui::row({ui::text({.text = selectedLabel(list, true), .role = "section"})
                             .sizing(ui::stretch())
                             .testId("selected"),
                         ui::button({.label = "Assign to worksheet", .onClick = ui::open(list.assignDialog)})
                             .testId("assign"),
                         ui::menu({{.label = "⋯",
                                    .items = {{.label = "Change priority…", .onClick = ui::open(list.priorityDialog)},
                                              {.label = "Print labels", .onClick = printLabels(list)},
                                              {.label = "Clear selection", .onClick = clearSelection(list)}}}})
                             .a11y("More bulk actions")
                             .testId("more-bulk")},
                        {.gap = 1, .align = Align::Center})},
               {.padding = 1})
        .surface("selected")
        .a11y("Bulk actions")
        .visible(list.selectedCount > 0)
        .testId("bulk");
}

ui::Node compactLayout(Samples const& list) {
    auto const active =
        ui::cond(ui::eq(list.status, "open"), ui::Handle<std::int64_t>{0}, ui::Handle<std::int64_t>{1}) +
        ui::cond(ui::eq(list.priority, "any"), ui::Handle<std::int64_t>{0}, ui::Handle<std::int64_t>{1});
    return ui::column(
        {searchField(list),
         ui::row({ui::button({.label = ui::format("Filters · {0}", {active}), .onClick = ui::open(list.filtersDrawer)})
                      .sizing(ui::stretch())
                      .testId("open-filters"),
                  // A table over rows does not sort, and neither can a list; the order is the one the rows came in.
                  ui::button({.label = "Sort: due, earliest first"}).enabled(false).sizing(ui::stretch())},
                 {.gap = 1}),
         ui::forEach(list.shown, {.key = ui::row("id"), .gap = 1},
                     [&list](ui::DeclScope&) { return ui::Nodes{sampleCard(list)}; })
             .testId("cards"),
         ui::column(noSamples(list)).visible(ui::Handle<bool>{ui::isEmpty(list.shown)}),
         ui::button({.label = ui::format("Load more · 25 of {0}", {ui::AnyHandle{kTotal}})}).enabled(false),
         compactBulkBar(list), compactFilters(list), assignDialog(list), priorityDialog(list)},
        {.gap = 2});
}

}  // namespace

ui::Screen samplesScreen() {
    return ui::screen("lims-gallery.samples", [](ui::Scope& screen) {
        Samples const list = declare(screen);
        screen.title("Samples");
        screen.action({.id = "register",
                       .label = ui::cond(list.compact, Text{"Register"}, Text{"Register samples"}),
                       .emphasis = ButtonEmphasis::Primary,
                       .onClick = ui::notify("Registering samples opens the registration form")});
        return layout::byWidth(list.width, compactLayout(list), expandedLayout(list));
    });
}

}  // namespace lims_gallery

// SPDX-License-Identifier: Apache-2.0
//
// Design 04, the audit trail: who changed which field of which record, from what to what, and why, read-only. Facets
// beside the table narrow it by record type, action, user and date; each active filter is a chip that removes it. A
// facet group with no box checked does not narrow. The counts are the whole trail's, as the design gives them; the
// gallery holds the five entries it draws, so the specification change is hidden while its facet is unchecked.

#include <string>
#include <vector>

#include "lims_gallery/screens.hpp"
#include "screen_layout.hpp"

namespace lims_gallery {

namespace {

namespace ui = layout::ui;
using layout::Align;
using layout::ButtonEmphasis;
using layout::Justify;
using morph::ui::ControlShape;
using Text = ui::Handle<std::string>;
using Flag = ui::Handle<bool>;

struct Change {
    char const* at;
    char const* date;
    char const* time;
    char const* user;
    char const* record;
    char const* detail;
    char const* field;
    char const* type;
    char const* oldValue;
    char const* newValue;
    char const* reason;
};

ui::AnyHandle entry(Change const& change) {
    return ui::obj({{"at", change.at},
                    {"date", change.date},
                    {"time", change.time},
                    {"user", change.user},
                    {"record", change.record},
                    {"detail", change.detail},
                    {"field", change.field},
                    {"type", change.type},
                    {"action", "Modified"},
                    {"old", change.oldValue},
                    {"new", change.newValue},
                    {"reason", change.reason}});
}

ui::AnyHandle entries() {
    return ui::list(
        {entry({.at = "2026-10-24T09:40:12",
                .date = "24-Oct-2026",
                .time = "09:40:12",
                .user = "ameyer",
                .record = "WS-2026-0412",
                .detail = "",
                .field = "Status",
                .type = "Result",
                .oldValue = "In review",
                .newValue = "Approved (signed)",
                .reason = "Results within specification except Impurity B; OOS-2026-031 closed as lab error."}),
         entry({.at = "2026-10-24T09:12:47",
                .date = "24-Oct-2026",
                .time = "09:12:47",
                .user = "mkaur",
                .record = "S-26-104170",
                .detail = "Impurity B",
                .field = "Result",
                .type = "Result",
                .oldValue = "0.12 %",
                .newValue = "0.21 %",
                .reason = "Transcription error: peak 7 integrated with the wrong baseline."}),
         entry({.at = "2026-10-24T09:07:03",
                .date = "24-Oct-2026",
                .time = "09:07:03",
                .user = "jruiz",
                .record = "S-26-104217",
                .detail = "",
                .field = "Received at",
                .type = "Sample",
                .oldValue = "(empty)",
                .newValue = "24-Oct-2026 09:07",
                .reason = ""}),
         entry({.at = "2026-10-23T17:55:31",
                .date = "23-Oct-2026",
                .time = "17:55:31",
                .user = "mkaur",
                .record = "S-26-104176",
                .detail = "",
                .field = "Storage location",
                .type = "Sample",
                .oldValue = "Fridge F-02 · shelf 1",
                .newValue = "Fridge F-03 · shelf 2",
                .reason = "F-02 out of range during defrost cycle."}),
         entry({.at = "2026-10-23T16:20:09",
                .date = "23-Oct-2026",
                .time = "16:20:09",
                .user = "ameyer",
                .record = "SPEC-ASSAY-07",
                .detail = "",
                .field = "Upper limit",
                .type = "Specification",
                .oldValue = "105.0 %",
                .newValue = "103.0 %",
                .reason = "Change control CC-2026-118, effective 23-Oct-2026."})});
}

// One checkbox of a facet group: the value it matches, its label, its count in the whole trail, and its state.
struct Facet {
    char const* value;
    char const* count;
    char const* testId;
    Flag checked;
};

struct Audit {
    ui::Element filters = ui::element("filters");
    ui::AnyHandle width;
    Flag compact;
    std::vector<Facet> types;
    std::vector<Facet> actions;
    Text user;
    Text from;
    Text until;
    Text search;
    Flag dated;
    ui::AnyHandle shown;
    ui::Handle<std::int64_t> active;
    // The trail's page, and how many entries compact lists before "Load 25 more".
    ui::Handle<std::int64_t> page;
    ui::Handle<std::int64_t> listed;
};

// Whether an entry's field matches a facet group: one of its checked values, or any value while none is checked.
Flag matches(ui::AnyHandle const& field, std::vector<Facet> const& group) {
    Flag noneChecked{true};
    Flag matched{false};
    for (Facet const& facet : group) {
        noneChecked = noneChecked && !facet.checked;
        matched = matched || (facet.checked && ui::eq(field, facet.value));
    }
    return noneChecked || matched;
}

Flag contains(ui::AnyHandle const& text, Text const& part) {
    return ui::Handle<bool>{ui::call("contains", {text, part})};
}

ui::AnyHandle shownEntries(Audit const& audit, ui::AnyHandle const& all) {
    return ui::filter(all, "entry", [&audit](ui::AnyHandle const& item) -> ui::AnyHandle {
        Flag const dateOk = (ui::isEmpty(audit.from) || item.at("at") >= audit.from) &&
                            (ui::isEmpty(audit.until) || item.at("at") <= audit.until);
        Flag const userOk = ui::eq(audit.user, "any") || ui::eq(item.at("user"), audit.user);
        Flag const searchOk = ui::isEmpty(audit.search) || contains(item.at("record"), audit.search) ||
                              contains(item.at("field"), audit.search) || contains(item.at("reason"), audit.search);
        return matches(item.at("type"), audit.types) && matches(item.at("action"), audit.actions) && dateOk &&
               userOk && searchOk;
    });
}

Audit declare(ui::Scope& screen) {
    Audit audit;
    audit.width = layout::widthClass(screen);
    audit.compact = screen.let("compact", ui::eq(audit.width, "compact"));
    audit.types = {
        {.value = "Result", .count = "412", .testId = "type-result", .checked = screen.state("typeResult", true)},
        {.value = "Sample", .count = "96", .testId = "type-sample", .checked = screen.state("typeSample", true)},
        {.value = "Specification",
         .count = "8",
         .testId = "type-specification",
         .checked = screen.state("typeSpecification", false)},
        {.value = "User account", .count = "3", .testId = "type-user", .checked = screen.state("typeUser", false)}};
    audit.actions = {
        {.value = "Modified",
         .count = "57",
         .testId = "action-modified",
         .checked = screen.state("actionModified", true)},
        {.value = "Created",
         .count = "380",
         .testId = "action-created",
         .checked = screen.state("actionCreated", false)},
        {.value = "Signed", .count = "71", .testId = "action-signed", .checked = screen.state("actionSigned", false)},
        {.value = "Deleted",
         .count = "0",
         .testId = "action-deleted",
         .checked = screen.state("actionDeleted", false)}};
    audit.user = screen.state("user", std::string{"any"});
    audit.from = screen.state("from", std::string{"2026-10-20T00:00:00"});
    audit.until = screen.state("until", std::string{"2026-10-24T23:59:00"});
    audit.search = screen.state("search", std::string{});
    audit.dated = screen.let("dated", !ui::isEmpty(audit.from) || !ui::isEmpty(audit.until));
    auto const all = screen.state<ui::Any>("entries", entries());
    audit.shown = screen.let("shown", shownEntries(audit, all));
    audit.page = screen.state("page", std::int64_t{0});
    audit.listed = screen.state("listed", std::int64_t{25});
    ui::Handle<std::int64_t> active = ui::cond(audit.dated, ui::Handle<std::int64_t>{1}, ui::Handle<std::int64_t>{0});
    for (std::vector<Facet> const* const group : {&audit.types, &audit.actions}) {
        for (Facet const& facet : *group) {
            active = active + ui::cond(facet.checked, ui::Handle<std::int64_t>{1}, ui::Handle<std::int64_t>{0});
        }
    }
    audit.active = screen.let("active", active);
    return audit;
}

ui::Commands clearAll(Audit const& audit) {
    ui::Commands commands = {ui::set(audit.user, std::string{"any"}), ui::set(audit.from, std::string{}),
                             ui::set(audit.until, std::string{}), ui::set(audit.search, std::string{})};
    for (std::vector<Facet> const* const group : {&audit.types, &audit.actions}) {
        for (Facet const& facet : *group) {
            commands = {ui::seq(commands), ui::set(facet.checked, false)};
        }
    }
    return commands;
}

// ── The facets ───────────────────────────────────────────────────────────────────────────────────────────────────

ui::Node facetGroup(char const* heading, std::vector<Facet> const& group) {
    std::vector<ui::Node> boxes{ui::text({.text = heading, .role = "overline"})};
    for (Facet const& facet : group) {
        boxes.push_back(ui::row(
            {ui::checkbox(
                 {.label = facet.value, .checked = facet.checked, .onToggle = ui::set(facet.checked, ui::event())})
                 .testId(facet.testId),
             ui::spacer().sizing(ui::stretch()), layout::muted(facet.count)},
            {.gap = 1, .align = Align::Center}));
    }
    return ui::column(boxes, {.gap = 0});
}

ui::Node userField(Audit const& audit) {
    auto const options =
        ui::list({ui::obj({{"key", "any"}, {"label", "Any user"}}), ui::obj({{"key", "ameyer"}, {"label", "ameyer"}}),
                  ui::obj({{"key", "mkaur"}, {"label", "mkaur"}}), ui::obj({{"key", "jruiz"}, {"label", "jruiz"}})});
    return layout::captioned(ui::select({.options = options,
                                         .selected = audit.user,
                                         .label = "User",
                                         .onChoose = ui::set(audit.user, ui::event())}))
        .testId("user");
}

ui::Node dateField(Text const& value, char const* label, char const* testId) {
    return layout::captioned(
               ui::dateTimeInput(
                   {.value = value, .label = label, .mode = "date-time", .onChange = ui::set(value, ui::event())}))
        .testId(testId);
}

ui::Nodes facetFields(Audit const& audit) {
    return {facetGroup("Record type", audit.types), facetGroup("Action", audit.actions), userField(audit),
            ui::column({dateField(audit.from, "From", "from"), dateField(audit.until, "To", "until")}, {.gap = 1})};
}

ui::Node facetsPanel(Audit const& audit) {
    return ui::panel(
               {ui::row(
                    {layout::section("Filters"), ui::spacer().sizing(ui::stretch()),
                     ui::button({.label = "Clear all", .emphasis = ButtonEmphasis::Ghost, .onClick = clearAll(audit)})
                         .testId("clear-all")},
                    {.gap = 1, .align = Align::Center}),
                ui::column(facetFields(audit), {.gap = 3})},
               {.padding = 2})
        // The design's 220 px rail.
        .sizing(ui::fixed(29))
        .a11y("Filters")
        .testId("facets");
}

// ── The active filters ───────────────────────────────────────────────────────────────────────────────────────────

ui::Node chip(ui::AnyHandle const& label, std::string const& removeName, ui::Commands const& remove,
              Flag const& shown) {
    return ui::badge({.text = label, .onRemove = remove, .removeLabel = removeName}).visible(shown);
}

ui::Node chips(Audit const& audit) {
    std::vector<ui::Node> row{layout::muted("57 entries · times in CEST (UTC+2)")};
    for (std::vector<Facet> const* const group : {&audit.types, &audit.actions}) {
        for (Facet const& facet : *group) {
            row.push_back(
                chip(facet.value, std::string{"Remove "} + facet.value, ui::set(facet.checked, false), facet.checked)
                    .testId(std::string{"chip-"} + facet.testId));
        }
    }
    // An open end of the range shows as an ellipsis; `date` reads only a date that is set.
    auto const end = [](Text const& value) {
        return ui::cond(ui::isEmpty(value), ui::AnyHandle{"…"}, ui::call("date", {value}));
    };
    row.push_back(chip(ui::format("{0} – {1}", {end(audit.from), end(audit.until)}), "Remove dates",
                       {ui::set(audit.from, std::string{}), ui::set(audit.until, std::string{})}, audit.dated)
                      .testId("chip-dates"));
    return ui::row(row, {.gap = 1, .align = Align::Center}).testId("chips");
}

// ── The trail ────────────────────────────────────────────────────────────────────────────────────────────────────

ui::Node oldValue(ui::AnyHandle const& value) { return ui::text({.text = value, .role = "error"}); }

ui::Node newValue(ui::AnyHandle const& value) { return ui::text({.text = value, .role = "success"}); }

ui::Nodes noEntries(Audit const& audit) {
    return {ui::emptyState({.title = "No entries match these filters",
                            .text = "Remove a filter to see more of the trail.",
                            .action = ui::ActionPart{.label = "Clear all", .onClick = clearAll(audit)}})
                .testId("no-entries")};
}

ui::Node trailTable(Audit const& audit) {
    auto const reason = ui::row("reason");
    auto const noReason = ui::isEmpty(reason);
    return ui::table(
               audit.shown,
               {.key = ui::row("at"),
                .columns =
                    {{.name = "when",
                      .label = "When",
                      .width = ui::fixed(12),
                      .cell = ui::column({ui::text({.text = ui::row("date")}), ui::text({.text = ui::row("time")})},
                                         {.gap = 0}),
                      .wrap = false},
                     {.name = "user", .label = "User", .width = ui::fixed(8)},
                     {.name = "record",
                      .label = "Record",
                      .width = ui::fixed(13),
                      .role = "mono",
                      .cell =
                          ui::column({ui::text({.text = ui::row("record"), .role = "mono"}),
                                      ui::text({.text = ui::row("detail")}).visible(!ui::isEmpty(ui::row("detail")))},
                                     {.gap = 0})},
                     {.name = "field", .label = "Field", .width = ui::fixed(12)},
                     {.name = "old", .label = "Old value", .width = ui::stretch(), .cell = oldValue(ui::row("old"))},
                     {.name = "new", .label = "New value", .width = ui::stretch(), .cell = newValue(ui::row("new"))},
                     {.name = "reason",
                      .label = "Reason",
                      .width = ui::stretch(2),
                      .cell = ui::text({.text = ui::cond(noReason, ui::AnyHandle{"—"}, reason),
                                        .role = ui::cond(noReason, Text{"muted"}, Text{"normal"})})}},
                .pageSize = std::int64_t{25},
                .page = audit.page,
                .empty = noEntries(audit),
                // The When and Record cells are two lines tall; every cell starts at the row's top.
                .valign = Align::Start})
        .id(ui::element("trail"))
        .a11y("Audit trail")
        .testId("trail");
}

// The trail's pager: where the page is, and the previous and next page.
ui::Node pager(Audit const& audit) {
    auto const page = ui::Handle<std::int64_t>{ui::ref("trail.page")};
    auto const pageCount = ui::Handle<std::int64_t>{ui::ref("trail.pageCount")};
    return ui::row({layout::muted(ui::format("{0}–{1} of {2}", {ui::ref("trail.pageFirst"), ui::ref("trail.pageLast"),
                                                                ui::ref("trail.viewCount")}))
                        .sizing(ui::stretch())
                        .testId("range"),
                    ui::button({.label = "‹", .onClick = ui::set(audit.page, page - 1), .shape = ControlShape::Square})
                        .a11y("Previous page")
                        .enabled(page > 0),
                    ui::button({.label = "›", .onClick = ui::set(audit.page, page + 1), .shape = ControlShape::Square})
                        .a11y("Next page")
                        .enabled(page < pageCount - 1)},
                   {.gap = 1, .align = Align::Center, .padding = 2})
        .testId("pager");
}

// Compact's search, in the filters drawer; the expanded layout's is a head tool.
ui::Node searchField(Audit const& audit) {
    return ui::textInput({.value = audit.search,
                          .label = "Search audit trail",
                          .placeholder = "Search record, field or reason",
                          .onChange = ui::set(audit.search, ui::event())})
        .sizing(ui::fixed(32))
        .testId("search");
}

ui::Node expandedLayout(Audit const& audit) {
    auto const main =
        ui::column({chips(audit), ui::panel({trailTable(audit), pager(audit)}, {.padding = 0})}, {.gap = 2})
            .sizing(ui::stretch());
    return ui::row({facetsPanel(audit), main}, {.gap = 2, .align = Align::Start});
}

// ── Compact: one entry per card, nothing shortened ───────────────────────────────────────────────────────────────

ui::Node entryCard() {
    auto const recordLine =
        ui::cond(ui::isEmpty(ui::row("detail")), ui::format("{0} · {1}", {ui::row("record"), ui::row("field")}),
                 ui::format("{0} · {1} · {2}", {ui::row("record"), ui::row("detail"), ui::row("field")}));
    // The whole line is a mono run: the record, what it is about and the field.
    auto const recordRuns = ui::spans({ui::mono(recordLine)});
    return ui::column(
        {ui::row({layout::section(ui::format("{0} {1}", {ui::row("date"), ui::row("time")})),
                  ui::text({.text = ui::row("user")})},
                 {.gap = 1, .justify = Justify::Between}),
         ui::text({.text = recordLine, .spans = recordRuns}),
         ui::row({oldValue(ui::row("old")), ui::text({.text = "→"}), newValue(ui::row("new"))}, {.gap = 1}),
         layout::muted(ui::format("Reason: {0}", {ui::row("reason")})).visible(!ui::isEmpty(ui::row("reason")))},
        {.gap = 0});
}

ui::Node filtersDrawer(Audit const& audit) {
    return ui::drawer(audit.filters,
                      {ui::column(facetFields(audit), {.gap = 3}), searchField(audit).sizing(ui::stretch())},
                      {.title = "Filters",
                       .side = "start",
                       .cancel = ui::close(ui::self),
                       .actions = {{.id = "clear", .label = "Clear all", .onClick = clearAll(audit)},
                                   {.id = "done",
                                    .label = ui::format("Show {0} entries", {ui::call("count", {audit.shown})}),
                                    .emphasis = ButtonEmphasis::Primary,
                                    .onClick = ui::close(ui::self)}}});
}

ui::Node compactLayout(Audit const& audit) {
    return ui::column(
        {ui::row(
             {ui::button({.label = ui::format("Filters · {0}", {audit.active}), .onClick = ui::open(audit.filters)})
                  .sizing(ui::stretch())
                  .testId("open-filters"),
              ui::button({.label = "Export CSV", .onClick = ui::notify("Exporting the audit trail as CSV")})
                  .sizing(ui::stretch())},
             {.gap = 1}),
         layout::muted("57 entries · times in CEST (UTC+2)"),
         ui::forEach(ui::slice(audit.shown, std::int64_t{0}, audit.listed),
                     {.key = ui::row("at"), .gap = 2, .divided = true},
                     [](ui::DeclScope&) { return ui::Nodes{entryCard()}; })
             .testId("entries"),
         ui::column(noEntries(audit)).visible(ui::isEmpty(audit.shown)),
         ui::button({.label = "Load 25 more", .onClick = ui::set(audit.listed, audit.listed + 25)})
             .visible(audit.listed < ui::Handle<std::int64_t>{ui::call("count", {audit.shown})})
             .testId("load-more"),
         filtersDrawer(audit)},
        {.gap = 2});
}

}  // namespace

ui::Screen auditScreen() {
    return ui::screen("lims-gallery.audit", [](ui::Scope& screen) {
        Audit const audit = declare(screen);
        screen.title("Audit trail");
        // The search sits in the head on the expanded layout; compact draws it in the filters drawer.
        screen.tool(ui::HeadSearchOptions{.id = "search",
                                          .label = "Search audit trail",
                                          .value = audit.search,
                                          .placeholder = "Search record, field or reason",
                                          .onChange = {ui::set(audit.search, ui::event())},
                                          .width = 32,
                                          .visible = !audit.compact});
        screen.action({.id = "csv",
                       .label = "Export CSV",
                       .visible = !audit.compact,
                       .onClick = ui::notify("Exporting the audit trail as CSV")});
        screen.action({.id = "pdf",
                       .label = ui::cond(audit.compact, Text{"Export PDF"}, Text{"Export PDF for inspection"}),
                       .emphasis = ButtonEmphasis::Primary,
                       .onClick = ui::notify("Exporting the audit trail as PDF for inspection")});
        return layout::byWidth(audit.width, compactLayout(audit), expandedLayout(audit));
    });
}

}  // namespace lims_gallery

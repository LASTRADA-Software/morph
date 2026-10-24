// SPDX-License-Identifier: Apache-2.0
//
// Design 04, the audit trail: who changed which field of which record, from what to what, and why, read-only. Facets
// beside the table narrow it by record type, action, user and date; each active filter is a chip that removes it. A
// facet group with no box checked does not narrow. The counts are the whole trail's, as the design gives them; the
// gallery holds the five entries it draws, so the specification change is hidden while its facet is unchecked.

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
        {entry({"2026-10-24T09:40:12", "24-Oct-2026", "09:40:12", "ameyer", "WS-2026-0412", "", "Status", "Result",
                "In review", "Approved (signed)",
                "Results within specification except Impurity B; OOS-2026-031 closed as lab error."}),
         entry({"2026-10-24T09:12:47", "24-Oct-2026", "09:12:47", "mkaur", "S-26-104170", "Impurity B", "Result",
                "Result", "0.12 %", "0.21 %", "Transcription error: peak 7 integrated with the wrong baseline."}),
         entry({"2026-10-24T09:07:03", "24-Oct-2026", "09:07:03", "jruiz", "S-26-104217", "", "Received at", "Sample",
                "(empty)", "24-Oct-2026 09:07", ""}),
         entry({"2026-10-23T17:55:31", "23-Oct-2026", "17:55:31", "mkaur", "S-26-104176", "", "Storage location",
                "Sample", "Fridge F-02 · shelf 1", "Fridge F-03 · shelf 2",
                "F-02 out of range during defrost cycle."}),
         entry({"2026-10-23T16:20:09", "23-Oct-2026", "16:20:09", "ameyer", "SPEC-ASSAY-07", "", "Upper limit",
                "Specification", "105.0 %", "103.0 %", "Change control CC-2026-118, effective 23-Oct-2026."})});
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
    audit.types = {{"Result", "412", "type-result", screen.state("typeResult", true)},
                   {"Sample", "96", "type-sample", screen.state("typeSample", true)},
                   {"Specification", "8", "type-specification", screen.state("typeSpecification", false)},
                   {"User account", "3", "type-user", screen.state("typeUser", false)}};
    audit.actions = {{"Modified", "57", "action-modified", screen.state("actionModified", true)},
                     {"Created", "380", "action-created", screen.state("actionCreated", false)},
                     {"Signed", "71", "action-signed", screen.state("actionSigned", false)},
                     {"Deleted", "0", "action-deleted", screen.state("actionDeleted", false)}};
    audit.user = screen.state("user", std::string{"any"});
    audit.from = screen.state("from", std::string{"2026-10-20T00:00:00"});
    audit.until = screen.state("until", std::string{"2026-10-24T23:59:00"});
    audit.search = screen.state("search", std::string{});
    audit.dated = screen.let("dated", !ui::isEmpty(audit.from) || !ui::isEmpty(audit.until));
    auto const all = screen.state<ui::Any>("entries", entries());
    audit.shown = screen.let("shown", shownEntries(audit, all));
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
    std::vector<ui::Node> boxes{layout::muted(heading)};
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
    return {facetGroup("RECORD TYPE", audit.types), facetGroup("ACTION", audit.actions), userField(audit),
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
    return ui::row({ui::text({.text = label}),
                    ui::button({.label = "×", .emphasis = ButtonEmphasis::Ghost, .onClick = remove}).a11y(removeName)},
                   {.gap = 0, .align = Align::Center})
        .surface("card")
        .visible(shown);
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
                      .width = ui::fixed(11),
                      .cell = ui::column({ui::text({.text = ui::row("date")}), ui::text({.text = ui::row("time")})},
                                         {.gap = 0})},
                     {.name = "user", .label = "User", .width = ui::fixed(8)},
                     {.name = "record",
                      .label = "Record",
                      .width = ui::fixed(13),
                      .cell =
                          ui::column({ui::text({.text = ui::row("record")}),
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
                .empty = noEntries(audit)})
        .a11y("Audit trail")
        .testId("trail");
}

// The gallery holds one page; the pager says where it is and has nowhere to go.
ui::Node pager(char const* label) {
    return ui::row({layout::muted(label).sizing(ui::stretch()),
                    ui::button({.label = "‹"}).a11y("Previous page").enabled(false),
                    ui::button({.label = "›"}).a11y("Next page").enabled(false)},
                   {.gap = 1, .align = Align::Center})
        .testId("pager");
}

ui::Node searchField(Audit const& audit) {
    return ui::textInput({.value = audit.search,
                          .label = "Search audit trail",
                          .placeholder = "Search record, field or reason",
                          .onChange = ui::set(audit.search, ui::event())})
        .sizing(ui::fixed(32))
        .testId("search");
}

ui::Node expandedLayout(Audit const& audit) {
    auto const main = ui::column({ui::row({ui::spacer().sizing(ui::stretch()), searchField(audit)}, {.gap = 1}),
                                  chips(audit), ui::panel({trailTable(audit), pager("1–25 of 57")}, {.padding = 0})},
                                 {.gap = 2})
                          .sizing(ui::stretch());
    return ui::row({facetsPanel(audit), main}, {.gap = 2, .align = Align::Start});
}

// ── Compact: one entry per card, nothing shortened ───────────────────────────────────────────────────────────────

ui::Node entryCard() {
    auto const recordLine =
        ui::cond(ui::isEmpty(ui::row("detail")), ui::format("{0} · {1}", {ui::row("record"), ui::row("field")}),
                 ui::format("{0} · {1} · {2}", {ui::row("record"), ui::row("detail"), ui::row("field")}));
    return ui::column(
        {ui::row({layout::section(ui::format("{0} {1}", {ui::row("date"), ui::row("time")})),
                  ui::text({.text = ui::row("user")})},
                 {.gap = 1, .justify = Justify::Between}),
         ui::text({.text = recordLine}),
         ui::row({oldValue(ui::row("old")), ui::text({.text = "→"}), newValue(ui::row("new"))}, {.gap = 1}),
         layout::muted(ui::format("Reason: {0}", {ui::row("reason")})).visible(!ui::isEmpty(ui::row("reason")))},
        {.gap = 0});
}

ui::Node filtersDrawer(Audit const& audit) {
    return ui::drawer(audit.filters,
                      {ui::column(facetFields(audit), {.gap = 3}), searchField(audit).sizing(ui::stretch())},
                      {.title = "Filters",
                       .side = "start",
                       .cancel = ui::close(audit.filters),
                       .actions = {{.id = "clear", .label = "Clear all", .onClick = clearAll(audit)},
                                   {.id = "done",
                                    .label = ui::format("Show {0} entries", {ui::call("count", {audit.shown})}),
                                    .emphasis = ButtonEmphasis::Primary,
                                    .onClick = ui::close(audit.filters)}}});
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
         // The interpreter drops a forEach's own common properties, so a column carries its test id.
         ui::column({ui::forEach(audit.shown, {.key = ui::row("at"), .gap = 3},
                                 [](ui::DeclScope&) { return ui::Nodes{entryCard()}; })})
             .testId("entries"),
         ui::column(noEntries(audit)).visible(ui::isEmpty(audit.shown)),
         ui::button({.label = "Load 25 more", .onClick = ui::notify("The gallery holds the first page only")})
             .testId("load-more"),
         filtersDrawer(audit)},
        {.gap = 2});
}

}  // namespace

ui::Screen auditScreen() {
    return ui::screen("lims-gallery.audit", [](ui::Scope& screen) {
        Audit const audit = declare(screen);
        screen.title("Audit trail");
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

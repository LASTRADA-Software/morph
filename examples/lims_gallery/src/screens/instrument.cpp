// SPDX-License-Identifier: Apache-2.0
//
// Design 03, the instrument record: HPLC-02, with its service, qualification and calibration status in the head,
// its actions, and its record in tabs. Logging maintenance or a calibration adds to its recent events; taking it out
// of service asks for a reason and changes its status; booking adds to its bookings.

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
using Text = ui::Handle<std::string>;

// With no clock behind the gallery, everything logged now is stamped with the design's time.
constexpr char const* kNow = "24-Oct-2026 09:41";
constexpr char const* kUser = "A. Meyer";
// The booking's elapsed share, 29 %, as the exact quotient `progress` reads.
ui::AnyHandle elapsed() { return ui::Handle<std::int64_t>{29} / 100; }

ui::AnyHandle event(char const* when, char const* what) { return ui::obj({{"when", when}, {"what", what}}); }

// The instrument's state: its status, its events and bookings, and what the log and service dialogs are filling in.
struct Instrument {
    ui::Element logDialog = ui::element("log");
    ui::Element serviceDialog = ui::element("outOfService");
    ui::Element bookDialog = ui::element("book");
    ui::AnyHandle width;
    ui::Handle<bool> compact;
    ui::Handle<bool> inService;
    ui::Handle<bool> calibrated;
    ui::AnyHandle events;
    ui::AnyHandle bookings;
    ui::Handle<std::string> logKind;
    ui::Handle<std::string> logNote;
    ui::Handle<std::string> serviceReason;
    ui::Handle<std::string> bookWorksheet;
    ui::Handle<std::string> bookStart;
    ui::Handle<bool> bookingOpen;
    ui::Handle<bool> qualificationOpen;
    ui::Handle<bool> eventsOpen;
    // The record tab shown, by its id; Book chooses `bookings`.
    ui::Handle<std::string> tab;
};

Instrument declare(ui::Scope& screen) {
    Instrument instrument;
    instrument.width = layout::widthClass(screen);
    instrument.compact = screen.let("compact", ui::eq(instrument.width, "compact"));
    instrument.inService = screen.state("inService", true);
    instrument.calibrated = screen.state("calibrated", false);
    instrument.events = screen.state<ui::Any>(
        "events",
        ui::list({event("24-Oct-2026 09:40", "System suitability failed: tailing factor 2.3, limit 2.0. Run held."),
                  event("23-Oct-2026 16:12", "Column changed to C18, lot 4471. By M. Kaur."),
                  event("20-Oct-2026 08:05", "Preventive maintenance completed. By service engineer.")}));
    instrument.bookings = screen.state<ui::Any>(
        "bookings",
        ui::list({ui::obj({{"worksheet", "WS-2026-0412"}, {"by", "M. Kaur"}, {"when", "24-Oct-2026 08:00–14:00"}})}));
    instrument.logKind = screen.state("logKind", std::string{"maintenance"});
    instrument.logNote = screen.state("logNote", std::string{});
    instrument.serviceReason = screen.state("serviceReason", std::string{});
    instrument.bookWorksheet = screen.state("bookWorksheet", std::string{});
    instrument.bookStart = screen.state("bookStart", std::string{"2026-10-24T14:00:00"});
    instrument.bookingOpen = screen.state("bookingOpen", true);
    instrument.qualificationOpen = screen.state("qualificationOpen", false);
    instrument.eventsOpen = screen.state("eventsOpen", false);
    instrument.tab = screen.state("tab", std::string{"overview"});
    return instrument;
}

// ── The head ─────────────────────────────────────────────────────────────────────────────────────────────────────

// Book shows the Bookings tab and opens the booking dialog over it.
ui::Commands book(Instrument const& instrument) {
    return {ui::set(instrument.tab, std::string{"bookings"}), ui::open(instrument.bookDialog)};
}

void declareHead(ui::Scope& screen, Instrument const& instrument) {
    auto const compact = instrument.compact;
    screen.title("HPLC-02 · Liquid chromatograph");
    screen.subtitle(ui::cond(compact, Text{"EQ-0142"}, Text{"Instruments › EQ-0142"}));
    screen.status({.text = ui::cond(instrument.inService, Text{"In service"}, Text{"Out of service"}),
                   .tone = ui::cond(instrument.inService, Text{"ok"}, Text{"neutral"})});
    screen.status({.text = ui::cond(compact, Text{"IQ, OQ, PQ passed"}, Text{"Qualified: IQ, OQ, PQ"}), .tone = "ok"});
    screen.status({.text = ui::cond(instrument.calibrated, Text{"Calibrated 24-Oct-2026"},
                                    ui::cond(compact, Text{"Calibration due in 3 days"},
                                             Text{"Calibration due 27-Oct-2026, in 3 days"})),
                   .tone = ui::cond(instrument.calibrated, Text{"ok"}, Text{"warn"})});
    // Compact draws the actions as large buttons in the page instead.
    screen.action(
        {.id = "maintenance",
         .label = "Log maintenance",
         .visible = !compact,
         .onClick = {ui::set(instrument.logKind, std::string{"maintenance"}), ui::open(instrument.logDialog)}});
    screen.action({.id = "book",
                   .label = "Book",
                   .emphasis = ButtonEmphasis::Primary,
                   .visible = !compact && instrument.inService,
                   .onClick = book(instrument)});
    // The head's own overflow, after its actions; compact draws it in the page after the large buttons.
    screen.menuLabel("More actions");
    screen.menu({.id = "calibration",
                 .label = "Record calibration…",
                 .onClick = {ui::set(instrument.logKind, std::string{"calibration"}), ui::open(instrument.logDialog)},
                 .visible = !compact});
    screen.menu({.id = "label",
                 .label = "Print equipment label",
                 .onClick = ui::notify("Printing the equipment label for EQ-0142"),
                 .visible = !compact});
    screen.menu({.id = "export",
                 .label = "Export history (PDF)",
                 .onClick = ui::notify("Exporting the history of EQ-0142 as PDF"),
                 .visible = !compact});
    screen.menu(ui::menuSeparator());
    screen.menu({.id = "outOfService",
                 .label = "Take out of service…",
                 .enabled = instrument.inService,
                 .onClick = ui::open(instrument.serviceDialog),
                 .description = "needs reason",
                 .visible = !compact});
}

// The compact overflow menu, after the large buttons: the head's own menu's entries, which compact hides.
ui::Node moreMenu(Instrument const& instrument) {
    return ui::menuButton(
               {.icon = "more"},
               {{.label = "Record calibration…",
                 .onClick = {ui::set(instrument.logKind, std::string{"calibration"}), ui::open(instrument.logDialog)}},
                {.label = "Print equipment label", .onClick = ui::notify("Printing the equipment label for EQ-0142")},
                {.label = "Export history (PDF)", .onClick = ui::notify("Exporting the history of EQ-0142 as PDF")},
                ui::menuSeparator(),
                {.label = "Take out of service…",
                 .enabled = instrument.inService,
                 .onClick = ui::open(instrument.serviceDialog),
                 .description = "needs reason"}})
        .a11y("More actions")
        .testId("more");
}

// ── The dialogs ──────────────────────────────────────────────────────────────────────────────────────────────────

// Maintenance and calibration share one dialog: a note, logged as a recent event. A calibration also clears the
// calibration that was due.
ui::Node logDialog(Instrument const& instrument) {
    auto const isCalibration = ui::eq(instrument.logKind, "calibration");
    auto const entry = ui::format("{0}: {1}. By {2}.",
                                  {ui::cond(isCalibration, Text{"Calibration recorded"}, Text{"Maintenance logged"}),
                                   instrument.logNote, kUser});
    ui::Commands const log = {
        ui::set(instrument.events,
                ui::concatLists({ui::list({ui::obj({{"when", kNow}, {"what", entry}})}), instrument.events})),
        ui::ifThen(isCalibration, ui::set(instrument.calibrated, true)), ui::set(instrument.logNote, std::string{}),
        ui::close(ui::self)};
    return ui::dialog(instrument.logDialog,
                      {layout::captioned(ui::textInput({.value = instrument.logNote,
                                                        .label = "What was done",
                                                        .mode = "multiline",
                                                        .onChange = ui::set(instrument.logNote, ui::event())}))
                           .required(true)
                           .testId("log-note")},
                      {.title = ui::cond(isCalibration, Text{"Record calibration"}, Text{"Log maintenance"}),
                       .cancel = ui::close(ui::self),
                       .actions = {{.id = "cancelLog", .label = "Cancel", .onClick = ui::close(ui::self)},
                                   {.id = "log",
                                    .label = "Log",
                                    .emphasis = ButtonEmphasis::Primary,
                                    .enabled = !ui::isEmpty(instrument.logNote),
                                    .onClick = log}}});
}

ui::Node serviceDialog(Instrument const& instrument) {
    auto const entry = ui::format("Taken out of service: {0}. By {1}.", {instrument.serviceReason, kUser});
    ui::Commands const takeOut = {
        ui::set(instrument.inService, false),
        ui::set(instrument.events,
                ui::concatLists({ui::list({ui::obj({{"when", kNow}, {"what", entry}})}), instrument.events})),
        ui::close(ui::self)};
    return ui::dialog(instrument.serviceDialog,
                      {layout::muted("HPLC-02 cannot be booked or used for analysis until it is returned to service."),
                       layout::captioned(ui::textInput({.value = instrument.serviceReason,
                                                        .label = "Reason",
                                                        .onChange = ui::set(instrument.serviceReason, ui::event())}))
                           .required(true)
                           .testId("service-reason")},
                      {.title = "Take HPLC-02 out of service",
                       .cancel = ui::close(ui::self),
                       .actions = {{.id = "cancelService", .label = "Cancel", .onClick = ui::close(ui::self)},
                                   {.id = "takeOut",
                                    .label = "Take out of service",
                                    .emphasis = ButtonEmphasis::Danger,
                                    .enabled = !ui::isEmpty(instrument.serviceReason),
                                    .onClick = takeOut}}});
}

ui::Node bookDialog(Instrument const& instrument) {
    ui::Commands const book = {
        ui::set(instrument.bookings,
                ui::concatLists({instrument.bookings,
                                 ui::list({ui::obj({{"worksheet", instrument.bookWorksheet},
                                                    {"by", kUser},
                                                    {"when", ui::call("dateTime", {instrument.bookStart})}})})})),
        ui::set(instrument.bookWorksheet, std::string{}), ui::close(ui::self)};
    return ui::dialog(instrument.bookDialog,
                      {layout::captioned(ui::textInput({.value = instrument.bookWorksheet,
                                                        .label = "Worksheet",
                                                        .placeholder = "WS-2026-0413",
                                                        .onChange = ui::set(instrument.bookWorksheet, ui::event())}))
                           .required(true)
                           .testId("book-worksheet"),
                       layout::captioned(ui::dateTimeInput({.value = instrument.bookStart,
                                                            .label = "From",
                                                            .mode = "date-time",
                                                            .onChange = ui::set(instrument.bookStart, ui::event())}))
                           .testId("book-start")},
                      {.title = "Book HPLC-02",
                       .cancel = ui::close(ui::self),
                       .actions = {{.id = "cancelBook", .label = "Cancel", .onClick = ui::close(ui::self)},
                                   {.id = "confirmBook",
                                    .label = "Book",
                                    .emphasis = ButtonEmphasis::Primary,
                                    .enabled = !ui::isEmpty(instrument.bookWorksheet),
                                    .onClick = book}}});
}

// ── The record's sections ────────────────────────────────────────────────────────────────────────────────────────

ui::AnyHandle detail(char const* label, char const* value) { return ui::obj({{"label", label}, {"value", value}}); }

ui::Node detailsCard() {
    return ui::panel({ui::keyValue(ui::list(
                         {detail("Model", "[Manufacturer] 1290 series"), detail("Serial number", "DEAB-902114"),
                          detail("Location", "QC lab 2, bench 4"), detail("Owner", "Dr. Anna Meyer"),
                          detail("Software", "CDS 3.4.1, validated 14-Mar-2026"),
                          detail("Methods", "Assay and impurities, dissolution")}))},
                     {.title = "Details", .padding = 2})
        .testId("details");
}

ui::Node eventList(Instrument const& instrument) {
    return ui::forEach(instrument.events, {.key = ui::row("what"), .gap = 1},
                       [](ui::DeclScope& /*row*/) {
                           return ui::Nodes{ui::row({layout::muted(ui::row("when")).sizing(ui::fixed(18)),
                                                     ui::text({.text = ui::row("what")}).sizing(ui::stretch())},
                                                    {.gap = 2})};
                       })
        .testId("events");
}

ui::Node qualificationList() {
    auto const stage = [](char const* name, char const* date) {
        return ui::row(
            {layout::tag("Passed", "ok"), ui::text({.text = name}).sizing(ui::stretch()), layout::muted(date)},
            {.gap = 1, .align = Align::Center});
    };
    return ui::column({stage("IQ", "12-Jan-2025"), stage("OQ", "14-Jan-2025"), stage("PQ", "next 14-Jan-2027")},
                      {.gap = 1})
        .testId("qualification");
}

ui::Node currentBooking() {
    return ui::column(
               {ui::text({.text = "Worksheet WS-2026-0412"}), layout::muted("M. Kaur · 24-Oct-2026 08:00–14:00"),
                ui::progress(elapsed(), "Booking elapsed").testId("booking-elapsed")},
               {.gap = 1})
        .testId("current-booking");
}

ui::Node bookingsTable(Instrument const& instrument) {
    return ui::table(instrument.bookings,
                     {.key = ui::row("worksheet"),
                      .columns = {{.name = "worksheet",
                                   .label = "Worksheet",
                                   .cell = ui::text({.text = ui::row("worksheet")})},
                                  {.name = "by", .label = "Booked by", .cell = ui::text({.text = ui::row("by")})},
                                  {.name = "when", .label = "When", .cell = ui::text({.text = ui::row("when")})}}})
        .a11y("Bookings")
        .testId("bookings");
}

// The tabs the design names but does not draw: each says so rather than inventing a record.
ui::Node undrawn(char const* what) {
    return ui::emptyState({.title = what, .text = "The design draws only the Overview tab."});
}

ui::Node recordTabs(Instrument const& instrument, ui::Node const& overview) {
    return ui::tabs({{.id = "overview", .label = "Overview", .children = overview},
                     {.id = "calibration", .label = "Calibration", .children = undrawn("Calibration")},
                     {.id = "maintenance", .label = "Maintenance", .children = undrawn("Maintenance")},
                     {.id = "bookings", .label = "Bookings", .children = bookingsTable(instrument)},
                     {.id = "audit", .label = "Audit trail", .children = undrawn("Audit trail")}},
                    {.selected = instrument.tab, .onChoose = ui::set(instrument.tab, ui::event())})
        .testId("tabs");
}

ui::Node dialogs(Instrument const& instrument) {
    return ui::column({logDialog(instrument), serviceDialog(instrument), bookDialog(instrument)});
}

// ── The layouts ──────────────────────────────────────────────────────────────────────────────────────────────────

ui::Node expandedLayout(Instrument const& instrument) {
    auto const overview = ui::row(
        {ui::column({detailsCard(), ui::panel({eventList(instrument)}, {.title = "Recent events", .padding = 2})},
                    {.gap = 2})
             .sizing(ui::stretch()),
         ui::column({ui::panel({qualificationList()}, {.title = "Qualification", .padding = 2}),
                     ui::panel({currentBooking()}, {.title = "Current booking", .padding = 2})},
                    {.gap = 2})
             // The design's 320 px column, in the frontend's character widths.
             .sizing(ui::fixed(44))},
        {.gap = 2, .align = Align::Start});
    return ui::column({recordTabs(instrument, overview), dialogs(instrument)}, {.gap = 2});
}

// Compact stacks every section: the details as label-over-value pairs, the rest folded.
ui::Node compactDetails() {
    auto const pair = [](char const* label, char const* value) {
        return ui::obj({{"label", label}, {"value", value}});
    };
    return ui::keyValue({.items = ui::list({pair("Model", "[Manufacturer] 1290 series"),
                                            ui::obj({{"label", "Serial number"},
                                                     {"value", "DEAB-902114"},
                                                     {"spans", ui::spans({ui::mono("DEAB-902114")})}}),
                                            pair("Location", "QC lab 2, bench 4"),
                                            pair("Owner · Software", "Dr. Anna Meyer · CDS 3.4.1"),
                                            pair("Methods", "Assay and impurities, dissolution")}),
                         .fieldLayout = morph::forms::FieldLayout::Stacked})
        .testId("details");
}

ui::Node compactLayout(Instrument const& instrument) {
    auto const actions = ui::row(
        {ui::button({.label = "Book", .emphasis = ButtonEmphasis::Primary, .onClick = book(instrument)})
             .enabled(instrument.inService)
             .sizing(ui::stretch())
             .testId("book"),
         ui::button(
             {.label = "Log maintenance",
              .onClick = {ui::set(instrument.logKind, std::string{"maintenance"}), ui::open(instrument.logDialog)}})
             .sizing(ui::stretch())
             .testId("maintenance"),
         moreMenu(instrument)},
        {.gap = 1, .align = Align::Center});
    auto const overview = ui::column(
        {compactDetails(),
         ui::collapsible({currentBooking()}, {.title = "Current booking",
                                              .open = instrument.bookingOpen,
                                              .onToggle = ui::set(instrument.bookingOpen, ui::event())})
             .testId("booking-section"),
         ui::collapsible({qualificationList()}, {.title = "Qualification · all passed",
                                                 .open = instrument.qualificationOpen,
                                                 .onToggle = ui::set(instrument.qualificationOpen, ui::event())})
             .testId("qualification-section"),
         ui::collapsible({eventList(instrument)},
                         {.title = ui::format("Recent events · {0}", {ui::count(instrument.events)}),
                          .open = instrument.eventsOpen,
                          .onToggle = ui::set(instrument.eventsOpen, ui::event())})
             .testId("events-section")},
        {.gap = 2});
    return ui::column({actions, recordTabs(instrument, overview), dialogs(instrument)}, {.gap = 2});
}

}  // namespace

ui::Screen instrumentScreen() {
    return ui::screen("lims-gallery.instrument", [](ui::Scope& screen) {
        Instrument const instrument = declare(screen);
        declareHead(screen, instrument);
        return layout::byWidth(instrument.width, compactLayout(instrument), expandedLayout(instrument));
    });
}

}  // namespace lims_gallery

// SPDX-License-Identifier: Apache-2.0
//
// Design 02, sample receipt: a technician scans each arriving sample, checks its condition and temperature, and
// receives it or rejects it, then scans the next. The scan field keeps focus. A sample scanned twice is reported and
// left as it was; a registered one becomes the sample being checked. The session list and the head's counts follow
// every outcome.

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

constexpr char const* kTechnician = "J. Ruiz";
// With no clock behind the gallery, every outcome is stamped with the design's time.
constexpr char const* kNow = "09:41";

ui::AnyHandle sessionRow(char const* time, char const* sample, char const* outcome) {
    return ui::obj({{"time", time}, {"sample", sample}, {"outcome", outcome}, {"by", kTechnician}});
}

ui::AnyHandle registeredSample(char const* sampleId, char const* lot, char const* product) {
    return ui::obj({{"id", sampleId},
                    {"status", "Registered"},
                    {"lot", lot},
                    {"product", product},
                    {"type", "Release, finished product"}});
}

// The screen's state: the samples waiting to be received, this session's outcomes, and the fields of the sample
// being checked.
struct Receipt {
    ui::Element scanField = ui::element("scan");
    ui::Element rejectDialog = ui::element("reject");
    ui::AnyHandle width;
    ui::Handle<bool> compact;
    ui::Handle<std::string> scan;
    ui::AnyHandle notice;
    ui::Handle<std::string> unknown;
    ui::AnyHandle registered;
    ui::AnyHandle sample;
    ui::AnyHandle session;
    ui::Handle<std::int64_t> received;
    ui::Handle<std::int64_t> rejected;
    ui::Handle<std::string> lastRejection;
    ui::Handle<std::string> temperature;
    ui::Handle<std::string> receivedAt;
    ui::Handle<std::string> location;
    ui::Handle<std::string> containers;
    ui::Handle<std::string> courier;
    ui::Handle<bool> intact;
    ui::Handle<bool> sealed;
    ui::Handle<bool> labelled;
    ui::Handle<bool> logger;
    ui::Handle<std::string> note;
    ui::Handle<std::string> rejectReason;
    ui::Handle<bool> sessionOpen;
    ui::AnyHandle sessionIds;
    ui::AnyHandle registeredIds;
    ui::Handle<bool> inRange;
    ui::Handle<bool> canReceive;
};

void declareRecords(ui::Scope& screen, Receipt& receipt) {
    receipt.scan = screen.state("scanned", std::string{});
    // The design opens on a duplicate scan that was refused.
    receipt.notice =
        screen.state<ui::Any>("notice", ui::obj({{"sample", "S-26-104217"}, {"time", "09:07"}, {"by", kTechnician}}));
    receipt.unknown = screen.state("unknown", std::string{});
    receipt.registered = screen.state<ui::Any>(
        "registered", ui::list({registeredSample("S-26-104230", "L-26-0381", "Tablets, 20 mg"),
                                registeredSample("S-26-104231", "L-26-0381", "Tablets, 20 mg"),
                                registeredSample("S-26-104232", "L-26-0384", "Tablets, 40 mg")}));
    receipt.sample = screen.state<ui::Any>("sample", registeredSample("S-26-104230", "L-26-0381", "Tablets, 20 mg"));
    receipt.session = screen.state<ui::Any>(
        "session",
        ui::list({sessionRow("09:39", "S-26-104229", "Received"), sessionRow("09:37", "S-26-104228", "Rejected"),
                  sessionRow("09:35", "S-26-104227", "Received"), sessionRow("09:34", "S-26-104226", "Received"),
                  sessionRow("09:30", "S-26-104225", "Received")}));
    receipt.received = screen.state("received", std::int64_t{12});
    receipt.rejected = screen.state("rejected", std::int64_t{1});
    receipt.lastRejection = screen.state("lastRejection", std::string{"S-26-104228 rejected: seal broken."});
    receipt.sessionIds =
        screen.let("sessionIds",
                   ui::map(receipt.session, "entry", [](ui::AnyHandle const& entry) { return entry.at("sample"); }));
    receipt.registeredIds =
        screen.let("registeredIds",
                   ui::map(receipt.registered, "entry", [](ui::AnyHandle const& entry) { return entry.at("id"); }));
}

void declareFields(ui::Scope& screen, Receipt& receipt) {
    receipt.temperature = screen.state("temperature", std::string{"5.4"});
    receipt.receivedAt = screen.state("receivedAt", std::string{"2026-10-24T09:41:00"});
    receipt.location = screen.state("location", std::string{"f03-2"});
    receipt.containers = screen.state("containers", std::string{"3"});
    receipt.courier = screen.state("courier", std::string{"internal"});
    receipt.intact = screen.state("intact", true);
    receipt.sealed = screen.state("sealed", true);
    receipt.labelled = screen.state("labelled", true);
    receipt.logger = screen.state("logger", false);
    receipt.note = screen.state("note", std::string{});
    receipt.rejectReason = screen.state("rejectReason", std::string{"seal broken"});
    receipt.sessionOpen = screen.state("sessionOpen", false);
    auto const degrees = screen.let("degrees", ui::parseDecimal(receipt.temperature));
    receipt.inRange = screen.let("inRange", ui::Handle<bool>{ui::engaged(degrees)} && degrees >= 2 && degrees <= 8);
    receipt.canReceive = screen.let("canReceive", ui::engaged(receipt.sample) && receipt.inRange && receipt.intact &&
                                                      receipt.sealed && receipt.labelled);
}

Receipt declare(ui::Scope& screen) {
    Receipt receipt;
    receipt.width = layout::widthClass(screen);
    receipt.compact = screen.let("compact", ui::eq(receipt.width, "compact"));
    declareRecords(screen, receipt);
    declareFields(screen, receipt);
    return receipt;
}

// The next registered sample after the one being checked, read before either list changes.
ui::AnyHandle nextSample(Receipt const& receipt) {
    return ui::call("first", {ui::filter(receipt.registered, "entry", [&receipt](ui::AnyHandle const& entry) {
                        return ui::ne(entry.at("id"), receipt.sample.at("id"));
                    })});
}

// Records an outcome for the sample being checked, moves on to the next one, and gives the scan field its focus back.
ui::Commands recordOutcome(Receipt const& receipt, char const* outcome) {
    return {ui::set(receipt.session, ui::concatLists({ui::list({ui::obj({{"time", kNow},
                                                                         {"sample", receipt.sample.at("id")},
                                                                         {"outcome", outcome},
                                                                         {"by", kTechnician}})}),
                                                      receipt.session})),
            ui::set(receipt.registered, ui::filter(receipt.registered, "entry",
                                                   [&receipt](ui::AnyHandle const& entry) {
                                                       return ui::ne(entry.at("id"), receipt.sample.at("id"));
                                                   })),
            ui::set(receipt.sample, nextSample(receipt)),
            ui::set(receipt.note, std::string{}),
            ui::set(receipt.notice, nullptr),
            ui::focus(receipt.scanField)};
}

ui::Commands receiveCommands(Receipt const& receipt) {
    return {ui::set(receipt.received, receipt.received + 1), ui::seq(recordOutcome(receipt, "Received"))};
}

// A scan: a sample this session already has is reported and changes nothing; a registered one is checked next.
ui::Commands scanCommands(Receipt const& receipt) {
    ui::Commands const duplicate = ui::set(
        receipt.notice, ui::call("first", {ui::filter(receipt.session, "entry", [](ui::AnyHandle const& entry) {
                                     return ui::eq(entry.at("sample"), ui::event());
                                 })}));
    ui::Commands const registered = {
        ui::set(receipt.sample, ui::call("first", {ui::filter(receipt.registered, "entry",
                                                              [](ui::AnyHandle const& entry) {
                                                                  return ui::eq(entry.at("id"), ui::event());
                                                              })})),
        ui::set(receipt.notice, nullptr), ui::set(receipt.unknown, std::string{})};
    ui::Commands const unknown = {ui::set(receipt.unknown, ui::event()), ui::set(receipt.notice, nullptr)};
    return {ui::ifThen(ui::includes(receipt.sessionIds, ui::event()), duplicate,
                       {ui::ifThen(ui::includes(receipt.registeredIds, ui::event()), registered, unknown)}),
            ui::set(receipt.scan, std::string{}), ui::focus(receipt.scanField)};
}

// Compact drops the caption, as the design does; the label still names the field.
ui::Node scanField(Receipt const& receipt, bool compact) {
    return ui::textInput({.value = receipt.scan,
                          .label = "Scan a sample barcode",
                          .placeholder = "Waiting for scan…",
                          .icon = "scan",
                          .onChange = ui::set(receipt.scan, ui::event()),
                          .onSubmit = scanCommands(receipt)})
        .id(receipt.scanField)
        .autofocus(true)
        .with("showLabel", ui::AnyHandle{!compact})
        .testId("scan");
}

ui::Node notices(Receipt const& receipt) {
    auto const full = ui::format("Already received: {0} was received at {1} by {2}. Nothing was changed.",
                                 {receipt.notice.at("sample"), receipt.notice.at("time"), receipt.notice.at("by")});
    auto const brief = ui::format("Already received: {0}, {1}, {2}.",
                                  {receipt.notice.at("sample"), receipt.notice.at("time"), receipt.notice.at("by")});
    return ui::column(
        {ui::banner({.tone = "warn", .text = ui::cond(receipt.compact, brief, full)})
             .visible(ui::engaged(receipt.notice))
             .testId("duplicate"),
         ui::banner({.tone = "err",
                     .text = ui::format("{0} is not a registered sample. Check the label, or register it "
                                        "first.",
                                        {receipt.unknown})})
             .visible(!ui::isEmpty(receipt.unknown))
             .testId("unknown")},
        {.gap = 1});
}

ui::Node temperatureField(Receipt const& receipt) {
    auto const errors =
        ui::cond(receipt.inRange, ui::AnyHandle{std::vector<std::string>{}},
                 ui::AnyHandle{std::vector<std::string>{"Outside 2–8 °C: reject the sample or record a "
                                                        "deviation."}});
    return layout::captioned(
               ui::textInput(
                   {.value = receipt.temperature,
                    .label = ui::cond(receipt.compact, ui::Handle<std::string>{"Temperature on arrival, °C · 2–8 °C"},
                                      ui::Handle<std::string>{"Temperature on arrival, °C"}),
                    .onChange = ui::set(receipt.temperature, ui::event())}))
        .required(true)
        .errors(errors)
        .testId("temperature");
}

ui::Node receivedAtField(Receipt const& receipt) {
    return layout::captioned(ui::dateTimeInput({.value = receipt.receivedAt,
                                                .label = "Received at",
                                                .mode = "date-time",
                                                .onChange = ui::set(receipt.receivedAt, ui::event())}))
        .required(true)
        .testId("receivedAt");
}

ui::Node locationField(Receipt const& receipt) {
    auto const options = ui::list({ui::obj({{"key", "f03-2"}, {"label", "Fridge F-03 · shelf 2"}}),
                                   ui::obj({{"key", "f03-3"}, {"label", "Fridge F-03 · shelf 3"}}),
                                   ui::obj({{"key", "f04-1"}, {"label", "Fridge F-04 · shelf 1"}}),
                                   ui::obj({{"key", "a1"}, {"label", "Ambient store A-1"}})});
    return layout::captioned(ui::select({.options = options,
                                         .selected = receipt.location,
                                         .label = "Storage location",
                                         .onChoose = ui::set(receipt.location, ui::event())}))
        .required(true)
        .testId("location");
}

ui::Node containersField(Receipt const& receipt) {
    return layout::captioned(ui::textInput({.value = receipt.containers,
                                            .label = "Containers",
                                            .onChange = ui::set(receipt.containers, ui::event())}))
        .testId("containers");
}

ui::Node courierField(Receipt const& receipt) {
    auto const options = ui::list({ui::obj({{"key", "internal"}, {"label", "Internal, QA sampling"}}),
                                   ui::obj({{"key", "courier"}, {"label", "External courier"}})});
    return layout::captioned(ui::select({.options = options,
                                         .selected = receipt.courier,
                                         .label = "Courier",
                                         .onChoose = ui::set(receipt.courier, ui::event())}))
        .testId("courier");
}

ui::Node noteField(Receipt const& receipt) {
    return layout::captioned(ui::textInput({.value = receipt.note,
                                            .label = "Note",
                                            .placeholder = "Optional",
                                            .onChange = ui::set(receipt.note, ui::event())}))
        .testId("note");
}

ui::Node check(ui::Handle<bool> const& state, char const* label, char const* testId) {
    return ui::checkbox({.label = label, .checked = state, .onToggle = ui::set(state, ui::event())}).testId(testId);
}

// The four checks of the sample's condition; the first three must hold to receive it.
ui::Node conditions(Receipt const& receipt, bool compact) {
    std::vector<ui::Node> const checks{
        check(receipt.intact, "Container intact", "intact"), check(receipt.sealed, "Seal unbroken", "sealed"),
        check(receipt.labelled, compact ? "Label legible and matches" : "Label legible and matches the request",
              "labelled"),
        check(receipt.logger, "Temperature logger attached", "logger")};
    ui::Node const boxes = compact ? ui::column(checks, {.gap = 1}) : ui::grid(checks, {.gap = 1, .columns = 2});
    return ui::column({ui::text({.text = "Condition on receipt *"}), boxes}, {.gap = 1}).testId("conditions");
}

ui::Node sampleHeading(Receipt const& receipt) {
    return ui::row({layout::section(ui::format("Sample {0}", {receipt.sample.at("id")})).testId("sample-id"),
                    layout::tag(receipt.sample.at("status"), "neutral")},
                   {.gap = 1, .align = Align::Center});
}

ui::Node rejectButton(Receipt const& receipt, char const* label) {
    return ui::button({.label = label, .emphasis = ButtonEmphasis::Danger, .onClick = ui::open(receipt.rejectDialog)})
        .enabled(ui::engaged(receipt.sample))
        .testId("reject");
}

ui::Node receiveButton(Receipt const& receipt, char const* label) {
    return ui::button({.label = label, .emphasis = ButtonEmphasis::Primary, .onClick = receiveCommands(receipt)})
        .enabled(receipt.canReceive)
        .testId("receive");
}

// The sample being checked, as the expanded layout's card.
ui::Node sampleCard(Receipt const& receipt) {
    auto const lotLine = ui::format("Lot {0} · {1}", {receipt.sample.at("lot"), receipt.sample.at("product")});
    auto const type = layout::captioned(ui::textInput({.value = receipt.sample.at("type"), .label = "Sample type"}))
                          .readonly(true)
                          .testId("type");
    auto const temperature =
        ui::column({temperatureField(receipt), layout::muted("Required range 2–8 °C")}, {.gap = 0});
    return ui::panel(
               {ui::row({sampleHeading(receipt), ui::spacer().sizing(ui::stretch()), layout::muted(lotLine)},
                        {.gap = 1, .align = Align::Center}),
                ui::grid({type, temperature, receivedAtField(receipt), locationField(receipt),
                          containersField(receipt), courierField(receipt)},
                         {.gap = 2, .columns = 3}),
                conditions(receipt, false), noteField(receipt),
                ui::row({rejectButton(receipt, "Reject sample…"), receiveButton(receipt, "Receive and scan next")},
                        {.gap = 1, .justify = Justify::Between})},
               {.padding = 3})
        .visible(ui::engaged(receipt.sample))
        .testId("sample");
}

ui::Node sessionTable(Receipt const& receipt) {
    auto const outcomeTone = ui::cond(ui::eq(ui::row("outcome"), "Received"), ui::Handle<std::string>{"ok"},
                                      ui::Handle<std::string>{"err"});
    return ui::table(receipt.session, {.key = ui::row("sample"),
                                       .columns = {{.name = "time",
                                                    .label = "Time",
                                                    .width = ui::fixed(8),
                                                    .cell = ui::text({.text = ui::row("time")})},
                                                   {.name = "sample",
                                                    .label = "Sample",
                                                    .width = ui::fixed(14),
                                                    .cell = ui::text({.text = ui::row("sample")})},
                                                   {.name = "outcome",
                                                    .label = "Outcome",
                                                    .width = ui::stretch(),
                                                    .cell = layout::tag(ui::row("outcome"), outcomeTone)}}})
        .a11y("This session")
        .testId("session");
}

// Rejecting asks why, then records the outcome as receiving does.
ui::Node rejectDialog(Receipt const& receipt) {
    auto const reasons = ui::list(
        {ui::obj({{"key", "seal broken"}, {"label", "Seal broken"}}),
         ui::obj({{"key", "container damaged"}, {"label", "Container damaged"}}),
         ui::obj({{"key", "temperature out of range"}, {"label", "Temperature out of range"}}),
         ui::obj({{"key", "label does not match the request"}, {"label", "Label does not match the request"}})});
    ui::Commands const reject = {
        ui::set(receipt.rejected, receipt.rejected + 1),
        ui::set(receipt.lastRejection,
                ui::format("{0} rejected: {1}.", {receipt.sample.at("id"), receipt.rejectReason})),
        ui::seq(recordOutcome(receipt, "Rejected")), ui::close(ui::self)};
    return ui::dialog(
        receipt.rejectDialog,
        {layout::muted("The sample is recorded as rejected in this session and is not received."),
         layout::captioned(ui::select({.options = reasons,
                                       .selected = receipt.rejectReason,
                                       .label = "Reason",
                                       .onChoose = ui::set(receipt.rejectReason, ui::event())}))
             .required(true)
             .testId("reject-reason")},
        {.title = ui::format("Reject sample {0}", {receipt.sample.at("id")}),
         .cancel = ui::close(ui::self),
         .actions = {
             {.id = "cancel", .label = "Cancel", .onClick = ui::close(ui::self)},
             {.id = "confirm", .label = "Reject sample", .emphasis = ButtonEmphasis::Danger, .onClick = reject}}});
}

ui::Node expandedLayout(Receipt const& receipt) {
    auto const left = ui::column({ui::column({scanField(receipt, false),
                                              layout::muted("The field keeps focus after every scan. Press Enter to "
                                                            "look up a typed ID.")},
                                             {.gap = 0}),
                                  notices(receipt), sampleCard(receipt)},
                                 {.gap = 2})
                          .sizing(ui::stretch());
    auto const session =
        ui::panel({sessionTable(receipt), layout::muted(receipt.lastRejection).testId("last-rejection")},
                  {.title = "This session", .padding = 2})
            // The design's 300 px column, in the frontend's character widths.
            .sizing(ui::fixed(40))
            .testId("session-panel");
    return ui::pinned(
        ui::column({ui::row({left, session}, {.gap = 2, .align = Align::Start}), rejectDialog(receipt)}));
}

ui::Node compactLayout(Receipt const& receipt) {
    auto const sampleLine = ui::format("{0} · Lot {1}", {receipt.sample.at("type"), receipt.sample.at("lot")});
    auto const sample =
        ui::column({sampleHeading(receipt), layout::muted(sampleLine), temperatureField(receipt),
                    receivedAtField(receipt), locationField(receipt),
                    ui::grid({containersField(receipt), courierField(receipt)}, {.gap = 2, .columns = 2}),
                    conditions(receipt, true), noteField(receipt)},
                   {.gap = 2})
            .visible(ui::engaged(receipt.sample))
            .testId("sample");
    auto const scans = ui::format("This session · {0} scans", {receipt.received + receipt.rejected});
    // Reject and Receive stay at the bottom of the phone screen while the form scrolls above them.
    return ui::pinned(
        ui::column(
            {scanField(receipt, true), notices(receipt), sample,
             ui::collapsible(
                 {sessionTable(receipt), layout::muted(receipt.lastRejection)},
                 {.title = scans, .open = receipt.sessionOpen, .onToggle = ui::set(receipt.sessionOpen, ui::event())})
                 .testId("session-panel"),
             rejectDialog(receipt)},
            {.gap = 2}),
        {.bottom =
             ui::row({rejectButton(receipt, "Reject…"), receiveButton(receipt, "Receive, next").sizing(ui::stretch())},
                     {.gap = 1})});
}

}  // namespace

ui::Screen receiptScreen() {
    return ui::screen("lims-gallery.receipt", [](ui::Scope& screen) {
        Receipt const receipt = declare(screen);
        screen.title("Receive samples");
        screen.subtitle(
            ui::cond(receipt.compact, ui::Handle<std::string>{""}, ui::Handle<std::string>{"Session since 08:30"}));
        screen.status({.text = ui::cond(receipt.compact, ui::format("{0}", {receipt.received}),
                                        ui::format("{0} received", {receipt.received})),
                       .tone = "ok"});
        screen.status({.text = ui::cond(receipt.compact, ui::format("{0}", {receipt.rejected}),
                                        ui::format("{0} rejected", {receipt.rejected})),
                       .tone = "err"});
        screen.showErrors(ui::ShowErrors::Always);
        return layout::byWidth(receipt.width, compactLayout(receipt), expandedLayout(receipt))
            .sizing(ui::stretch(), ui::stretch());
    });
}

}  // namespace lims_gallery

// SPDX-License-Identifier: Apache-2.0
//
// Design 01, the e-signature: a reviewer signs a worksheet to approve its results. Opened with a `worksheet` param, as
// the review opens it in a dialog, the screen is the signature itself: its fields, with Cancel and the signing button
// as head actions the dialog's footer shows, and a signature `return`s its meaning to the opener. Opened on its own it
// signs WS-2026-0412, a dialog over the worksheet it signs, a sheet on compact. The screen opens on the state the
// design draws: one wrong password already entered, three attempts left.

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "lims_gallery/screens.hpp"
#include "screen_layout.hpp"

namespace lims_gallery {

namespace {

namespace ui = layout::ui;
using layout::ButtonEmphasis;
using Text = ui::Handle<std::string>;

constexpr char const* kSigner = "Dr. Anna Meyer (ameyer)";
// The password that signs as the signer in this gallery: with no backend, the screen checks it itself.
constexpr char const* kPassword = "meyer-2026";

// The signature's state, kept at the screen so the dialog and the sheet show the same fields.
struct Signature {
    ui::Handle<std::string> meaning;
    ui::Handle<std::string> reason;
    ui::Handle<std::string> password;
    ui::Handle<bool> refused;
    ui::Handle<std::int64_t> attempts;
    ui::Handle<bool> open;
    ui::Handle<bool> done;
    ui::AnyHandle meaningLabel;
    // Whether an opener asked for the signature, and the worksheet it signs.
    ui::Handle<bool> called;
    ui::AnyHandle record;
};

Signature declare(ui::Scope& screen) {
    auto const worksheet = screen.param<std::optional<std::string>>("worksheet");
    Signature sign{
        .meaning = screen.state("meaning", std::string{"approved"}),
        .reason = screen.state("reason", std::string{"Results within specification except Impurity B; OOS-2026-031 "
                                                     "closed as lab error."}),
        // The design's field holds the attempt that was just refused.
        .password = screen.state("password", std::string{"meyer2026"}),
        .refused = screen.state("refused", true),
        .attempts = screen.state("attempts", std::int64_t{3}),
        .open = screen.state("open", true),
        .done = screen.state("signed", false),
        .meaningLabel = {},
        .called = ui::engaged(worksheet),
        .record = screen.let("record", ui::coalesce({worksheet, ui::AnyHandle{"WS-2026-0412"}})),
    };
    sign.meaningLabel = screen.let(
        "meaningLabel", ui::cond(ui::eq(sign.meaning, "approved"), Text{"Approved"},
                                 ui::cond(ui::eq(sign.meaning, "reviewed"), Text{"Reviewed"}, Text{"Authored"})));
    return sign;
}

// A right password runs @p signed; a wrong one costs an attempt and clears the field.
ui::Commands signCommands(Signature const& sign, ui::Commands const& signedWith) {
    return ui::ifThen(ui::eq(sign.password, kPassword), signedWith,
                      {ui::set(sign.attempts, sign.attempts - 1), ui::set(sign.refused, true),
                       ui::set(sign.password, std::string{})});
}

ui::Handle<bool> canSign(Signature const& sign) { return !ui::isEmpty(sign.reason) && !ui::isEmpty(sign.password); }

ui::Node summary(Signature const& sign, bool compact) {
    ui::Node const body =
        compact
            ? ui::column(
                  {ui::text({.spans = ui::spans(
                                 {ui::styled(sign.record, {.strong = true, .mono = true}), " · worksheet"})}),
                   ui::text({.spans = ui::spans({"24 results · Assay and impurities · Lot ", ui::mono("L-26-0381")})}),
                   ui::text({.spans = ui::spans({"2 out of specification, ", ui::mono("OOS-2026-031"), " linked"})})},
                  {.gap = 0})
            : ui::keyValue(ui::list({
                  ui::obj({{"label", "Record"}, {"spans", ui::spans({ui::mono(sign.record), " · worksheet"})}}),
                  ui::obj({{"label", "Content"},
                           {"spans", ui::spans({"24 results · Assay and impurities · Lot ", ui::mono("L-26-0381")})}}),
                  ui::obj({{"label", "Flags"},
                           {"spans", ui::spans({"2 out of specification, investigation ", ui::mono("OOS-2026-031"),
                                                " linked"})}}),
              }));
    return ui::panel({body}, {.padding = 2}).surface("page").testId("record");
}

// The refusal: "Password incorrect." in a strong run, then the attempts that are left, at every width.
ui::Node refusedBanner(Signature const& sign) {
    return ui::banner({.tone = "err",
                       .spans = ui::spans(
                           {ui::strong("Password incorrect."),
                            ui::format(" {0} of 5 attempts left before the account locks.", {sign.attempts})})})
        .visible(sign.refused)
        .testId("refused");
}

ui::Node signerField() {
    return layout::captioned(ui::textInput({.value = kSigner, .label = "Signer"})).readonly(true).testId("signer");
}

ui::Node meaningField(Signature const& sign) {
    auto const options = ui::list({ui::obj({{"key", "approved"}, {"label", "Approved"}}),
                                   ui::obj({{"key", "reviewed"}, {"label", "Reviewed"}}),
                                   ui::obj({{"key", "authored"}, {"label", "Authored"}})});
    return layout::captioned(ui::select({.options = options,
                                         .selected = sign.meaning,
                                         .label = "Meaning",
                                         .onChoose = ui::set(sign.meaning, ui::event())}))
        .required(true)
        .testId("meaning");
}

ui::Node reasonField(Signature const& sign) {
    return layout::captioned(ui::textInput({.value = sign.reason,
                                            .label = "Reason",
                                            .mode = "multiline",
                                            .onChange = ui::set(sign.reason, ui::event())}))
        .required(true)
        .testId("reason");
}

// Typing a new password takes the refusal away until the next attempt. While it is refused the field is only
// outlined: the banner above it says why.
ui::Node passwordField(Signature const& sign) {
    return layout::captioned(
               ui::textInput({.value = sign.password,
                              .label = "Password",
                              .mode = "password",
                              .onChange = {ui::set(sign.password, ui::event()), ui::set(sign.refused, false)}}))
        .required(true)
        .tone(ui::cond(sign.refused, Text{"err"}, Text{"neutral"}))
        .testId("password");
}

ui::Node signingAt(Signature const& sign) {
    return ui::text({.spans = ui::spans({"Signing at 24-Oct-2026 09:40 CEST as ", ui::strong(sign.meaningLabel), "."}),
                     .role = "small"})
        .testId("signing-at");
}

// The worksheet the signature approves, and its outcome once signed.
ui::Node worksheet(Signature const& sign) {
    return ui::column(
        {ui::row(
             {ui::column({ui::text({.text = "Worksheet WS-2026-0412", .role = "heading"}),
                          layout::muted("24 results · Assay and impurities · Lot L-26-0381")},
                         {.gap = 0})
                  .sizing(ui::stretch()),
              ui::button({.label = "Sign…", .emphasis = ButtonEmphasis::Primary, .onClick = ui::set(sign.open, true)})
                  .visible(!sign.done)
                  .testId("open-signature")},
             {.gap = 2, .align = layout::Align::Center}),
         ui::banner({.tone = "ok",
                     .text = ui::format("Signed as {0} by Dr. Anna Meyer at 24-Oct-2026 09:40 CEST. The signature is "
                                        "stored with WS-2026-0412.",
                                        {sign.meaningLabel})})
             .visible(sign.done)
             .testId("signed")},
        {.gap = 2});
}

// The signature's fields, the same in its own dialog and in the dialog an opener shows it in.
// The fields, after a note when the dialog around them draws no description of its own.
ui::Nodes signatureFields(ui::AnyHandle const& width, Signature const& sign, bool withNote) {
    std::vector<ui::Node> fields{
        layout::byWidth(width, {summary(sign, true)}, {summary(sign, false)}),
        refusedBanner(sign),
        layout::byWidth(width, {signerField(), meaningField(sign)},
                        {ui::grid({signerField(), meaningField(sign)}, {.gap = 2, .columns = 2})}),
        reasonField(sign),
        passwordField(sign),
        signingAt(sign)};
    if (withNote) {
        fields.insert(fields.begin(),
                      layout::muted("Your signature is stored with the record and cannot be removed."));
    }
    return fields;
}

// One dialog for both layouts: centred at the design's 480 px when expanded, and on compact a sheet that fills the
// screen, its Cancel in the sheet's head and the signing button across the footer.
ui::Node signatureDialog(ui::AnyHandle const& width, Signature const& sign) {
    auto const dialog = ui::element("signature");
    auto const compact = ui::Handle<bool>{ui::eq(width, "compact")};
    ui::Commands const close = ui::set(sign.open, false);
    return ui::dialog(
               dialog, signatureFields(width, sign, false),
               {.title = ui::cond(compact, Text{"Sign to approve"}, Text{"Sign to approve results"}),
                // A phone's sheet leaves the note out: the fields are what it is for.
                .description = ui::cond(compact, Text{""},
                                        Text{"Your signature is stored with the record and cannot be removed."}),
                .open = sign.open,
                .showErrors = ui::ShowErrors::Always,
                .cancel = close,
                .onDismiss = close,
                .actions = {{.id = "cancel", .label = "Cancel", .visible = !compact, .onClick = close},
                            {.id = "sign",
                             .label = "Sign and approve",
                             .emphasis = ButtonEmphasis::Primary,
                             .enabled = canSign(sign),
                             .onClick = signCommands(sign, {ui::set(sign.done, true), ui::set(sign.open, false)})}},
                .sheet = compact,
                .dismissLabel = "Cancel"})
        // The design's 480 px, in the frontend's character widths; a sheet is the window's width.
        .sizing(ui::fixed(64))
        .testId("signature");
}

// Asked for by an opener, the signature's Cancel and signing button are head actions, which the dialog the opener's
// host shows draws in its footer; a signature answers with its meaning.
void declareCalledActions(ui::Scope& screen, Signature const& sign) {
    screen.action({.id = "cancel", .label = "Cancel", .visible = sign.called, .onClick = {ui::close(ui::self)}});
    screen.action({.id = "sign",
                   .label = "Sign and approve",
                   .emphasis = ButtonEmphasis::Primary,
                   .enabled = canSign(sign),
                   .visible = sign.called,
                   .onClick = signCommands(sign, {ui::returnWith(sign.meaning)})});
}

}  // namespace

ui::Screen eSignatureScreen() {
    return ui::screen("lims-gallery.esignature", [](ui::Scope& screen) {
        // The refused password is marked from the start, before the field is touched.
        screen.showErrors(ui::ShowErrors::Always);
        auto const width = layout::widthClass(screen);
        Signature const sign = declare(screen);
        screen.title(ui::cond(sign.called, Text{"Sign to approve results"}, Text{"E-signature"}));
        declareCalledActions(screen, sign);
        return ui::switchOn(
            sign.called,
            {ui::Case{true, {ui::column(signatureFields(width, sign, true), {.gap = 2}).testId("fields")}}},
            {ui::column({worksheet(sign), signatureDialog(width, sign)}, {.gap = 3})});
    });
}

}  // namespace lims_gallery

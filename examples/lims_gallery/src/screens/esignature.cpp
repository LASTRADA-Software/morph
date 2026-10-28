// SPDX-License-Identifier: Apache-2.0
//
// Design 01, the e-signature: a reviewer signs worksheet WS-2026-0412 to approve its results. Expanded, the signature
// is a dialog over the worksheet it signs; compact, the same fields fill the screen as a sheet. The screen opens on
// the state the design draws: one wrong password already entered, three attempts left.

#include <cstdint>
#include <string>
#include <vector>

#include "layout.hpp"
#include "lims_gallery/screens.hpp"

namespace lims_gallery {

namespace {

namespace ui = layout::ui;
using layout::ButtonEmphasis;
using layout::Justify;

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
};

Signature declare(ui::Scope& screen) {
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
    };
    using Text = ui::Handle<std::string>;
    sign.meaningLabel = screen.let(
        "meaningLabel", ui::cond(ui::eq(sign.meaning, "approved"), Text{"Approved"},
                                 ui::cond(ui::eq(sign.meaning, "reviewed"), Text{"Reviewed"}, Text{"Authored"})));
    return sign;
}

// A right password signs and closes; a wrong one costs an attempt and clears the field.
ui::Commands signCommands(Signature const& sign) {
    return ui::ifThen(ui::eq(sign.password, kPassword), {ui::set(sign.done, true), ui::set(sign.open, false)},
                      {ui::set(sign.attempts, sign.attempts - 1), ui::set(sign.refused, true),
                       ui::set(sign.password, std::string{})});
}

ui::Handle<bool> canSign(Signature const& sign) { return !ui::isEmpty(sign.reason) && !ui::isEmpty(sign.password); }

ui::Node summary(bool compact) {
    ui::Node const body =
        compact ? ui::column({ui::text({.spans = ui::spans({ui::mono("WS-2026-0412"), " · worksheet"})}),
                              ui::text({.spans = ui::spans({"24 results · Assay and impurities · Lot ",
                                                            ui::mono("L-26-0381")})}),
                              ui::text({.spans = ui::spans({"2 out of specification, ", ui::mono("OOS-2026-031"),
                                                            " linked"})})},
                             {.gap = 0})
                : ui::keyValue(ui::list({
                      ui::obj({{"label", "Record"},
                               {"spans", ui::spans({ui::mono("WS-2026-0412"), " · worksheet"})}}),
                      ui::obj({{"label", "Content"},
                               {"spans",
                                ui::spans({"24 results · Assay and impurities · Lot ", ui::mono("L-26-0381")})}}),
                      ui::obj({{"label", "Flags"},
                               {"spans", ui::spans({"2 out of specification, investigation ",
                                                    ui::mono("OOS-2026-031"), " linked"})}}),
                  }));
    return ui::panel({body}, {.padding = 2}).surface("page").testId("record");
}

ui::Node refusedBanner(Signature const& sign) {
    return ui::banner({.tone = "err",
                       .spans = ui::spans({ui::strong("Password incorrect."),
                                           ui::format(" {0} of 5 attempts left before the account locks.",
                                                      {sign.attempts})})})
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

// Typing a new password takes the refusal away until the next attempt.
ui::Node passwordField(Signature const& sign) {
    return layout::captioned(
               ui::textInput({.value = sign.password,
                              .label = "Password",
                              .mode = "password",
                              .onChange = {ui::set(sign.password, ui::event()), ui::set(sign.refused, false)}}))
        .required(true)
        .errors(ui::cond(sign.refused, ui::AnyHandle{std::vector<std::string>{"Password incorrect."}},
                         ui::AnyHandle{std::vector<std::string>{}}))
        .testId("password");
}

ui::Node signingAt(Signature const& sign) {
    return ui::text({.spans = ui::spans({"Signing at 24-Oct-2026 09:40 CEST as ", ui::strong(sign.meaningLabel), "."}),
                     .role = "muted"})
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

ui::Node expandedLayout(Signature const& sign) {
    auto const dialog = ui::element("signature");
    ui::Commands const close = ui::set(sign.open, false);
    return ui::column(
        {worksheet(sign),
         ui::dialog(dialog,
                    {summary(false), refusedBanner(sign), ui::grid({signerField(), meaningField(sign)}, {.gap = 2, .columns = 2}),
                     reasonField(sign), passwordField(sign), signingAt(sign)},
                    {.title = "Sign to approve results",
                     .description = "Your signature is stored with the record and cannot be removed.",
                     .open = sign.open,
                     .showErrors = ui::ShowErrors::Always,
                     .cancel = close,
                     .onDismiss = close,
                     .actions = {{.id = "cancel", .label = "Cancel", .onClick = close},
                                 {.id = "sign",
                                  .label = "Sign and approve",
                                  .emphasis = ButtonEmphasis::Primary,
                                  .enabled = canSign(sign),
                                  .onClick = signCommands(sign)}}})
             // The design's 480 px, in the frontend's character widths.
             .sizing(ui::fixed(64))},
        {.gap = 3});
}

// Compact: the dialog's fields as a sheet that fills the screen, its own Cancel at the top and the signing button
// across the bottom.
ui::Node compactSheet(Signature const& sign) {
    return ui::column(
        {ui::row(
             {ui::button({.label = "Cancel", .emphasis = ButtonEmphasis::Ghost, .onClick = ui::set(sign.open, false)})
                  .testId("cancel"),
              ui::text({.text = "Sign to approve", .role = "heading"}), ui::spacer().sizing(ui::fixed(8))},
             {.gap = 1, .align = layout::Align::Center, .justify = Justify::Between}),
         summary(true), refusedBanner(sign), signerField(), meaningField(sign), reasonField(sign), passwordField(sign),
         signingAt(sign),
         ui::button({.label = "Sign and approve", .emphasis = ButtonEmphasis::Primary, .onClick = signCommands(sign)})
             .enabled(canSign(sign))
             .sizing(ui::stretch())
             .testId("sign")},
        {.gap = 3});
}

ui::Node compactLayout(Signature const& sign) {
    return ui::switchOn(sign.open, {ui::Case{true, compactSheet(sign)}}, worksheet(sign));
}

}  // namespace

ui::Screen eSignatureScreen() {
    return ui::screen("lims-gallery.esignature", [](ui::Scope& screen) {
        screen.title("E-signature");
        // The refused password is marked from the start, before the field is touched.
        screen.showErrors(ui::ShowErrors::Always);
        auto const width = layout::widthClass(screen);
        Signature const sign = declare(screen);
        return layout::byWidth(width, compactLayout(sign), expandedLayout(sign));
    });
}

}  // namespace lims_gallery

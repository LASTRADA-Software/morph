// SPDX-License-Identifier: Apache-2.0

#include <catch2/catch_test_macros.hpp>
#include <optional>
#include <string>

#include "gallery_harness.hpp"

using lims_gallery::testing::GalleryScreen;

namespace {

constexpr char const* kScreen = "lims-gallery.esignature";

}  // namespace

TEST_CASE("e-signature, expanded: a dialog over the worksheet, opened on the refused attempt", "[lims-gallery]") {
    GalleryScreen screen{kScreen, "expanded"};
    screen.requireNoProblems();
    CHECK(screen.find("Text", "signing-at").has_value());
    CHECK(screen.prop("Text", "signing-at", "spans") == "[Signing at 24-Oct-2026 09:40 CEST as ,Approved;strong,.]");
    CHECK(screen.prop("Text", "signing-at", "role") == "Small");
    CHECK(screen.shows("Banner", "refused"));
    CHECK(screen.prop("Banner", "refused", "spans") ==
          "[Password incorrect.;strong, 3 of 5 attempts left before the account locks.]");
    CHECK(screen.backend().all("Dialog").size() == 1);
    CHECK(screen.prop("Dialog", "signature", "sheet") == "false");
    // Opened on its own, no opener asked for the signature: its head shows no signing actions.
    CHECK_FALSE(screen.find("Button", "head_action_sign").has_value());
    CHECK(screen.backend().find("Text", "testId", "shell_title").has_value());
    CHECK(screen.backend().all("KeyValue").size() == 1);
    CHECK(screen.shows("Panel", "record"));
    CHECK(screen.prop("TextInput", "signer", "readonly") == "true");
    // The refusal is said once: the banner has the words, so the field is only marked.
    CHECK(screen.prop("TextInput", "password", "errors") == "[]");
    CHECK(screen.prop("TextInput", "password", "tone") == "Err");
    CHECK(screen.find("Select", "meaning").has_value());
    CHECK(screen.find("TextInput", "reason").has_value());
    // The footer's sign button waits for a password.
    screen.edit("password", "");
    CHECK(screen.waitFor([&screen] { return screen.prop("Button", "action_sign", "enabled") == "false"; }));
    CHECK(screen.prop("TextInput", "password", "errors") == "[Required]");
}

TEST_CASE("e-signature, expanded: a wrong password costs an attempt, the right one signs", "[lims-gallery]") {
    GalleryScreen screen{kScreen, "expanded"};
    screen.choose("meaning", "reviewed");
    CHECK(screen.waitFor([&screen] {
        return screen.prop("Text", "signing-at", "spans") ==
               "[Signing at 24-Oct-2026 09:40 CEST as ,Reviewed;strong,.]";
    }));

    // The field holds the refused attempt; signing it again costs another.
    screen.click("action_sign");
    CHECK(screen.waitFor([&screen] {
        return screen.prop("Banner", "refused", "spans") ==
               "[Password incorrect.;strong, 2 of 5 attempts left before the account locks.]";
    }));
    CHECK(screen.shows("Banner", "refused"));
    CHECK(screen.prop("TextInput", "password", "value") == "");

    screen.edit("password", "meyer-2026");
    CHECK(screen.waitFor([&screen] { return !screen.shows("Banner", "refused"); }));
    REQUIRE(screen.waitFor([&screen] { return screen.prop("Button", "action_sign", "enabled") != "false"; }));
    screen.click("action_sign");
    CHECK(screen.waitFor([&screen] { return screen.shows("Banner", "signed"); }));
    CHECK_FALSE(screen.find("TextInput", "password").has_value());
    CHECK_FALSE(screen.shows("Button", "open-signature"));
    screen.requireNoProblems();
}

TEST_CASE("e-signature, compact: the same dialog as a sheet, signed from its footer", "[lims-gallery]") {
    GalleryScreen screen{kScreen, "compact"};
    screen.requireNoProblems();
    CHECK(screen.prop("Dialog", "signature", "sheet") == "true");
    CHECK(screen.prop("Dialog", "signature", "dismissLabel") == "Cancel");
    // The sheet's head holds its Cancel, so the footer's is hidden.
    CHECK_FALSE(screen.shows("Button", "action_cancel"));
    CHECK(screen.shows("Banner", "refused"));
    // The banner says the whole refusal at both widths; the field is only outlined.
    CHECK(screen.prop("Banner", "refused", "spans") ==
          "[Password incorrect.;strong, 3 of 5 attempts left before the account locks.]");
    CHECK(screen.prop("TextInput", "password", "errors") == "[]");
    CHECK(screen.prop("TextInput", "password", "tone") == "Err");
    // The compact summary's record is a strong mono run.
    CHECK(screen.backend().dump().contains("WS-2026-0412;strong;mono, · worksheet]"));
    CHECK(screen.prop("Dialog", "signature", "description").value_or("").empty());
    CHECK(screen.find("TextInput", "signer").has_value());
    CHECK(screen.find("Select", "meaning").has_value());
    CHECK(screen.find("TextInput", "reason").has_value());
    CHECK(screen.prop("Text", "signing-at", "spans") == "[Signing at 24-Oct-2026 09:40 CEST as ,Approved;strong,.]");

    screen.edit("password", "meyer-2026");
    REQUIRE(screen.waitFor([&screen] { return screen.prop("Button", "action_sign", "enabled") != "false"; }));
    screen.click("action_sign");
    CHECK(screen.waitFor([&screen] { return screen.shows("Banner", "signed"); }));
    CHECK_FALSE(screen.find("TextInput", "password").has_value());
    screen.requireNoProblems();
}

TEST_CASE("e-signature, compact: Cancel closes the sheet, and the sign button opens it again", "[lims-gallery]") {
    GalleryScreen screen{kScreen, "compact"};
    // The sheet's Cancel dismisses it, as Escape does.
    screen.backend().dismiss(screen.find("Dialog", "signature").value_or(0));
    CHECK(screen.waitFor([&screen] { return !screen.find("TextInput", "password").has_value(); }));
    CHECK(screen.prop("Dialog", "signature", "open") == "false");
    CHECK(screen.shows("Button", "open-signature"));
    screen.click("open-signature");
    CHECK(screen.waitFor([&screen] { return screen.find("TextInput", "password").has_value(); }));
    screen.requireNoProblems();
}

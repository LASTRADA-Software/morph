// SPDX-License-Identifier: Apache-2.0

// Source of a custom screen the client's cache dropped and the server no longer has: the person is told it could not
// be read, and no source view opens.

#include <catch2/catch_test_macros.hpp>
#include <memory>
#include <morph/ui/custom_screen_chrome.hpp>
#include <morph/ui/custom_screens.hpp>
#include <optional>
#include <string>

#include "ui_custom_client_support.hpp"

using custom_client::ChromeRig;
using custom_client::kEdit;
using custom_client::note;

TEST_CASE("covj custom screen source: a dropped document the server no longer has is reported unreadable",
          "[covj][ui][custom-screens][chrome]") {
    ChromeRig rig;
    std::string const id = rig.saveOnServer("Mine", "notes.edit", std::string{kEdit});
    std::optional<bool> fetched;
    rig.sync->open(id, [&fetched](auto loaded) { fetched = loaded.has_value(); });
    REQUIRE(rig.pump([&fetched] { return fetched.has_value(); }));
    rig.open(id, note(1));
    rig.sync->documents().forget(id);
    // The list still names the screen, so its title is known; the server answers the fetch with a refusal.
    auto const removed = rig.server->call(morph::ui::kCustomScreensRemove,
                                          R"({"app":"notes","id":)" + screens_test::quoted(id) + "}", "ann");
    REQUIRE(removed.kind == "ok");
    rig.click("source_button_" + id);
    REQUIRE(rig.pump([&rig] {
        for (std::string const& line : rig.notices) {
            if (line.contains("could not be read")) {
                return true;
            }
        }
        return false;
    }));
    CHECK(rig.notices.back().contains("Mine"));
    CHECK_FALSE(rig.find("TextInput", "source_text").has_value());
    CHECK(rig.sync->documents().entry(id) == nullptr);
}

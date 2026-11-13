// SPDX-License-Identifier: Apache-2.0

#include "shell_screens.hpp"

#include <string>

#include "lims_screens.hpp"
#include "lims_shell_parts.hpp"

namespace lims::screens {

namespace ui = morph::ui::author;

ui::Screen aboutScreen() {
    return ui::screen(std::string{kAbout}, [](ui::Scope& screen) {
        screen.title(ui::t("lims.shell.about.title", "About lims"));
        return ui::panel(
            {ui::text({.text = ui::t("lims.shell.about.summary",
                                     "lims keeps a laboratory's samples, results, invoices, ledger and stock in "
                                     "one place.")})
                 .testId("aboutSummary"),
             ui::text({.text = "Version " + std::string{kApplicationVersion}, .role = std::string{"muted"}})
                 .testId("aboutVersion")},
            {.title = ui::t("lims.shell.about.heading", "lims"), .padding = 2});
    });
}

void addShellScreens(ui::ScreenRegistry& screens, std::vector<ListedScreen>& listed) {
    listed.push_back({.id = std::string{kAbout},
                      .module = "shell",
                      .title = ui::t("lims.shell.about.title", "About lims"),
                      .registered = screens.add(aboutScreen())});
}

std::vector<std::string_view> shellScreenIds() { return {kAbout}; }

}  // namespace lims::screens

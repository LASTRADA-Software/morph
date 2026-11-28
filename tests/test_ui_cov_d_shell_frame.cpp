// SPDX-License-Identifier: Apache-2.0

// The shell frame over parts it is handed whole: what each control reports when the client installed handlers for it
// and when it installed none, in the sidebar and in the compact navigation menu.

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <cstddef>
#include <morph/ui/bundle.hpp>
#include <morph/ui/shell_frame.hpp>
#include <morph/ui/testing/recording_backend.hpp>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace {

using morph::ui::CatalogEntry;
using morph::ui::ShellFrame;
using morph::ui::ShellFrameOptions;
using morph::ui::ShellPartsHandlers;
using morph::ui::ShellPartsView;
using morph::ui::testing::RecordingBackend;

struct Seen {
    std::vector<std::string> log;
};

ShellPartsView fullView() {
    ShellPartsView view;
    view.context.shown = true;
    view.context.enabled = true;
    view.context.options = {{.label = "Koeln", .detail = "EUR", .group = "A"},
                            {.label = "Osaka", .detail = "JPY", .group = "A"},
                            {.label = "Lyon", .detail = "EUR", .group = "B"}};
    view.context.chosen = 1;
    view.search.shown = true;
    view.search.hint = "Search";
    view.search.sources = {{.source = 0,
                            .title = "Lab",
                            .failed = false,
                            .pending = false,
                            .hits = {{.kind = "Sample", .title = "S-1", .detail = "d"}, {.kind = "", .title = "S-2"}}},
                           {.source = 1, .title = "Billing", .failed = true, .pending = false, .hits = {}}};
    view.inbox.shown = true;
    view.inbox.title = "Inbox";
    view.inbox.count = 3;
    view.principal.shown = true;
    view.principal.name = "Ann Lee";
    view.principal.initials = "AL";
    view.principal.locales = {{.tag = "en", .label = "English"}, {.tag = "de", .label = "Deutsch"}};
    view.principal.chosenLocale = 0;
    view.principal.formats = {{.tag = "en", .label = "en"}, {.tag = "de", .label = "de"}};
    view.principal.chosenFormat = 1;
    view.brand = {.shown = true, .title = "Lims", .icon = "", .version = "v1"};
    view.help.shown = true;
    view.connection = morph::ui::ConnectionState::Connected;
    return view;
}

ShellPartsHandlers handlersFor(Seen& seen) {
    ShellPartsHandlers handlers;
    handlers.onContext = [&seen](std::size_t index) { seen.log.push_back("context " + std::to_string(index)); };
    handlers.onSearchText = [&seen](std::string const& text) { seen.log.push_back("text " + text); };
    handlers.onSearchOpen = [&seen](std::size_t source, std::size_t hit) {
        seen.log.push_back("open " + std::to_string(source) + "." + std::to_string(hit));
    };
    handlers.onSearchOpenAll = [&seen] { seen.log.emplace_back("openAll"); };
    handlers.onInbox = [&seen] { seen.log.emplace_back("inbox"); };
    handlers.onLocale = [&seen](std::size_t index) { seen.log.push_back("locale " + std::to_string(index)); };
    handlers.onFormat = [&seen](std::size_t index) { seen.log.push_back("format " + std::to_string(index)); };
    handlers.onHelp = [&seen] { seen.log.emplace_back("help"); };
    return handlers;
}

struct Rig {
    RecordingBackend backend;
    Seen seen;
    int signOuts = 0;
    std::unique_ptr<ShellFrame> frame;

    Rig(bool handlers, bool signOut, bool compact) {
        ShellFrameOptions options;
        options.applicationName = "Lims";
        options.navigator.entries = {CatalogEntry{.id = "a.one", .title = "One", .group = "G"},
                                     CatalogEntry{.id = "a.two", .title = "Two", .group = "G"}};
        if (signOut) {
            options.onSignOut = [this] { ++signOuts; };
        }
        frame = std::make_unique<ShellFrame>(backend, nullptr, std::move(options));
        if (handlers) {
            frame->setPartsHandlers(handlersFor(seen));
        }
        frame->setCompact(compact);
        frame->setParts(fullView());
    }

    int id(std::string_view kind, std::string_view testId) const {
        return backend.find(kind, "testId", testId).value();
    }
};

}  // namespace

TEST_CASE("covd shell frame: the inbox and help buttons report to their handlers, and do nothing without them",
          "[ui][shell][covd]") {
    bool const handlers = GENERATE(true, false);
    CAPTURE(handlers);
    Rig r{handlers, true, false};
    r.backend.click(r.id("Button", "shell_inbox"));
    r.backend.click(r.id("Button", "shell_help"));
    CHECK(r.seen.log == (handlers ? std::vector<std::string>{"inbox", "help"} : std::vector<std::string>{}));
}

TEST_CASE("covd shell frame: the search field reports its text, and with no handler for it reports nothing",
          "[ui][shell][covd]") {
    bool const handlers = GENERATE(true, false);
    CAPTURE(handlers);
    Rig r{handlers, true, false};
    int const search = r.id("TextInput", "shell_search");
    r.backend.edit(search, "ab");
    r.frame->setSearchText("abc");
    r.frame->setSearchText("");
    CHECK(r.seen.log == (handlers ? std::vector<std::string>{"text ab"} : std::vector<std::string>{}));
}

TEST_CASE("covd shell frame: a search chord and a submit with handlers open the hit, or all of them; without, nothing",
          "[ui][shell][covd]") {
    bool const handlers = GENERATE(true, false);
    CAPTURE(handlers);
    Rig r{handlers, true, false};
    int const search = r.id("TextInput", "shell_search");
    r.backend.edit(search, "x");
    r.backend.press(search, "Down");
    r.backend.submit(search, "x");
    r.backend.edit(search, "y");
    r.backend.press(search, "Shift+Enter");
    CHECK(r.seen.log == (handlers
                             ? std::vector<std::string>{"text x", "open 0.1", "text ", "text y", "openAll", "text "}
                             : std::vector<std::string>{}));
}

TEST_CASE("covd shell frame: the person's menu chooses a language, a format or sign out, as far as it has handlers",
          "[ui][shell][covd]") {
    bool const handlers = GENERATE(true, false);
    bool const signOut = GENERATE(true, false);
    CAPTURE(handlers, signOut);
    Rig r{handlers, signOut, false};
    int const menu = r.id("Menu", "shell_principal_menu");
    std::string const items = r.backend.prop(menu, "items");
    CHECK(items.contains("English"));
    CHECK(items.contains("Deutsch"));
    CHECK(items.contains("Sign out") == signOut);
    // The entries: a heading and the two languages, then a separator, a heading and the two formats, and sign out.
    for (std::size_t entry = 0; entry < 12; ++entry) {
        r.backend.chooseMenuEntry(menu, {entry});
    }
    int logged = 0;
    for (std::string const& line : r.seen.log) {
        logged += line.starts_with("locale") || line.starts_with("format") ? 1 : 0;
    }
    CHECK(logged == (handlers ? 4 : 0));
    CHECK(r.signOuts == (signOut ? 1 : 0));
}

TEST_CASE("covd shell frame: the compact navigation menu carries the scope, help, the person and sign out",
          "[ui][shell][covd]") {
    bool const handlers = GENERATE(true, false);
    CAPTURE(handlers);
    Rig r{handlers, true, true};
    int const menu = r.id("Menu", "shell_nav_menu");
    // The scope's options (a heading, two options, a heading, one) then the help entry, the person's menu and its
    // entries: a heading, a rule, the languages, a rule, the formats, a rule and sign out.
    for (std::vector<std::size_t> const& path : std::vector<std::vector<std::size_t>>{
             {0, 1}, {0, 4}, {0, 0}, {0, 99}, {4}, {6, 3}, {6, 4}, {6, 7}, {6, 8}, {6, 10}, {1}, {2, 0}, {99}, {}}) {
        r.backend.chooseMenuEntry(menu, path);
    }
    std::vector<std::string> const expected{"context 0", "context 2", "help",    "locale 0",
                                            "locale 1",  "format 0",  "format 1"};
    CHECK(r.seen.log == (handlers ? expected : std::vector<std::string>{}));
    CHECK(r.signOuts == 1);
}

TEST_CASE("covd shell frame: a compact menu of a frame with no person's menu ends with sign out",
          "[ui][shell][covd]") {
    RecordingBackend backend;
    int signOuts = 0;
    ShellFrameOptions options;
    options.navigator.entries = {CatalogEntry{.id = "a.one", .title = "One", .group = "G"}};
    options.onSignOut = [&signOuts] { ++signOuts; };
    ShellFrame frame{backend, nullptr, std::move(options)};
    frame.setCompact(true);
    ShellPartsView view = fullView();
    view.principal.shown = false;
    view.help.shown = false;
    frame.setParts(std::move(view));
    int const menu = backend.find("Menu", "testId", "shell_nav_menu").value();
    std::string const items = backend.prop(menu, "items");
    CHECK(items.contains("Sign out"));
    // The context submenu, a rule, the groups, a rule, sign out, a rule and the foot.
    for (std::size_t entry = 0; entry < 8; ++entry) {
        backend.chooseMenuEntry(menu, {entry});
    }
    CHECK(signOuts == 1);
}

TEST_CASE("covd shell frame: the foot and the brand mark follow the brand and the connection the parts give",
          "[ui][shell][covd]") {
    struct Case {
        std::string_view brandTitle;
        std::string_view brandIcon;
        std::string_view version;
        std::optional<morph::ui::ConnectionState> connection;
        std::string mark;
        bool footerShown;
        std::string state;
        std::string versionText;
    };
    using morph::ui::ConnectionState;
    auto const c = GENERATE(Case{"Lims", "", "v1", ConnectionState::Connected, "L", true, "Connected", "v1"},
                            Case{"Lims", "", "", ConnectionState::Reconnecting, "L", true, "Reconnecting…", ""},
                            Case{"Lims", "", "v2", std::nullopt, "L", true, "", "v2"},
                            Case{"Lims", "", "", std::nullopt, "L", false, "", ""},
                            Case{"", "", "v3", ConnectionState::Offline, "", true, "Offline", "v3"},
                            Case{"Lims", "flask", "", ConnectionState::Connected, "", true, "Connected", ""});
    CAPTURE(c.brandTitle, c.brandIcon, c.version);
    RecordingBackend backend;
    ShellFrame frame{backend, nullptr, ShellFrameOptions{}};
    ShellPartsView view = fullView();
    view.brand = {.shown = true,
                  .title = std::string{c.brandTitle},
                  .icon = std::string{c.brandIcon},
                  .version = std::string{c.version}};
    view.connection = c.connection;
    frame.setParts(std::move(view));
    auto const id = [&backend](std::string_view testId) { return backend.find("Text", "testId", testId); };
    int const footer = backend.find("Row", "testId", "shell_footer").value();
    CHECK((backend.prop(footer, "visible") != "false") == c.footerShown);
    CHECK(backend.prop(backend.find("Badge", "testId", "shell_brand_mark").value(), "text") == c.mark);
    if (c.connection) {
        REQUIRE(id("shell_connection").has_value());
        CHECK(backend.prop(*id("shell_connection"), "text") == c.state);
    }
    REQUIRE(id("shell_version").has_value());
    CHECK(backend.prop(*id("shell_version"), "text") == c.versionText);
}

TEST_CASE(
    "covd shell frame: the inbox shows a count only above zero, and the scope names the chosen option or its own "
    "label",
    "[ui][shell][covd]") {
    struct Case {
        std::optional<long long> count;
        std::optional<std::size_t> chosen;
        bool counted;
        std::string scopeLabel;
    };
    auto const c = GENERATE(Case{3, 1, true, "Osaka"}, Case{0, 0, false, "Koeln"},
                            Case{std::nullopt, std::nullopt, false, "Scope"}, Case{5, 9, true, "Scope"});
    RecordingBackend backend;
    ShellFrame frame{backend, nullptr, ShellFrameOptions{}};
    ShellPartsView view = fullView();
    view.inbox.count = c.count;
    view.context.chosen = c.chosen;
    frame.setParts(std::move(view));
    int const badge = backend.find("Badge", "testId", "shell_inbox_count").value();
    CHECK((backend.prop(badge, "visible") != "false") == c.counted);
    int const context = backend.find("Menu", "testId", "shell_context").value();
    CHECK(backend.prop(context, "label").contains(c.scopeLabel));
}

TEST_CASE("covd shell frame: a search the parts do not offer has no field", "[ui][shell][covd]") {
    RecordingBackend backend;
    ShellFrame frame{backend, nullptr, ShellFrameOptions{}};
    ShellPartsView view = fullView();
    view.search.shown = false;
    frame.setParts(std::move(view));
    frame.setSearchText("abc");
    int const box = backend.find("Column", "testId", "shell_search_box").value();
    CHECK(backend.prop(box, "visible") == "false");
}

TEST_CASE("covd shell frame: every path of every menu is answered, and a path that names nothing does nothing",
          "[ui][shell][covd]") {
    bool const handlers = GENERATE(true, false);
    bool const compact = GENERATE(true, false);
    CAPTURE(handlers, compact);
    Rig r{handlers, true, compact};
    std::vector<std::string> tours;
    r.frame->setOnTour([&tours](std::string const& id) { tours.push_back(id); });
    morph::ui::ShellToursView view;
    view.groups = {
        {.heading = "Lab", .tours = {{.id = "t1", .title = "One", .summary = "first"}, {.id = "t2", .title = "Two"}}},
        {.heading = "Admin", .tours = {{.id = "t3", .title = "Three"}}}};
    r.frame->setTours(std::move(view));
    for (std::string const& menuId : {std::string{"shell_nav_menu"}, std::string{"shell_context"},
                                      std::string{"shell_tours"}, std::string{"shell_principal_menu"}}) {
        auto const menu = r.backend.find("Menu", "testId", menuId);
        if (!menu) {
            continue;
        }
        for (std::size_t first = 0; first < 14; ++first) {
            r.backend.chooseMenuEntry(*menu, {first});
            for (std::size_t const second : {std::size_t{0}, std::size_t{1}, std::size_t{2}, std::size_t{99}}) {
                r.backend.chooseMenuEntry(*menu, {first, second});
            }
            r.backend.chooseMenuEntry(*menu, {first, 0, 0});
        }
    }
    // The tours are chosen by place and named by their id, whatever the handlers are.
    CHECK_FALSE(tours.empty());
    for (std::string const& id : tours) {
        CHECK((id == "t1" || id == "t2" || id == "t3"));
    }
}

TEST_CASE("covd shell frame: a confirmation answers once, and a second answer finds nobody waiting",
          "[ui][shell][covd]") {
    Rig r{true, true, false};
    std::vector<bool> answers;
    r.frame->confirm("Discard?", "Yes", "No", [&answers](bool yes) { answers.push_back(yes); });
    r.backend.click(r.id("Button", "shell_confirm_yes"));
    r.backend.click(r.id("Button", "shell_confirm_no"));
    CHECK(answers == std::vector<bool>{true});
}

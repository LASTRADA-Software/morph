// SPDX-License-Identifier: Apache-2.0
//
// The shell parts model over replies a server can send: how an option and a count are worded and read, what a reply
// that does not fit is reported as, and the texts the shell has of its own.

#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <chrono>
#include <cstddef>
#include <glaze/glaze.hpp>
#include <memory>
#include <morph/core/wire.hpp>
#include <morph/ui/bundle.hpp>
#include <morph/ui/document.hpp>
#include <morph/ui/interpreter.hpp>
#include <morph/ui/navigator.hpp>
#include <morph/ui/service.hpp>
#include <morph/ui/shell_frame.hpp>
#include <morph/ui/shell_parts.hpp>
#include <morph/ui/shell_parts_binding.hpp>
#include <morph/ui/testing/recording_backend.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "ui_interpreter_support.hpp"

namespace ui = morph::ui;
using ui::testing::Harness;

namespace {

/// A reading of a catalog entry: module, group and id.
ui::CatalogEntry entry(std::string id, std::string module, std::string group) {
    ui::CatalogEntry made;
    made.id = std::move(id);
    made.title = "t";
    made.module = std::move(module);
    made.group = std::move(group);
    return made;
}

/// The navigator's entries: two modules of two screens, and a module of one.
std::vector<ui::CatalogEntry> entries() {
    return {entry("lims.home", "shell", "Home"), entry("lims.lab.samples", "lab", "Lab"),
            entry("lims.lab.verification", "lab", "Lab"), entry("lims.billing.invoices", "billing", "Billing"),
            entry("lims.billing.runs", "billing", "Billing")};
}

/// Every part, over the corpus catalog's actions. The count of the lab reads a record the probe mutation changes.
ui::ShellParts fullParts() {
    ui::ShellParts parts;
    parts.context = ui::ContextSwitch{.model = "Lims_ClientModel",
                                      .action = "Lims_ClientScopes",
                                      .localeField = "lang",
                                      .optionsField = "options",
                                      .labelField = "label",
                                      .valueField = "value",
                                      .detailField = "detail",
                                      .groupField = "group"};
    parts.modules = {ui::ModuleCount{.module = "lab",
                                     .model = "Lims_ClientModel",
                                     .action = "Shell_Rows",
                                     .countField = "total",
                                     .contextField = "scope",
                                     .localeField = {}},
                     ui::ModuleCount{.module = "billing",
                                     .model = "Lims_ClientModel",
                                     .action = "Lims_ClientCount",
                                     .countField = "open",
                                     .contextField = "scope",
                                     .localeField = {}}};
    parts.search = {ui::SearchSource{.module = "lab",
                                     .title = "Lab",
                                     .model = "Lims_ClientModel",
                                     .action = "Lims_ClientSearchLab",
                                     .queryField = "q",
                                     .resultsField = "hits",
                                     .contextField = "scope",
                                     .localeField = {}},
                    ui::SearchSource{.module = "billing",
                                     .title = "Billing",
                                     .model = "Lims_ClientModel",
                                     .action = "Lims_ClientSearchBilling",
                                     .queryField = "q",
                                     .resultsField = "hits",
                                     .contextField = {},
                                     .localeField = {}}};
    parts.inbox = ui::InboxEntry{.screen = "lims.approvals",
                                 .sources = {ui::ModuleCount{.module = "billing",
                                                             .model = "Lims_ClientModel",
                                                             .action = "Lims_ClientApprovalsBilling",
                                                             .countField = "n",
                                                             .contextField = {},
                                                             .localeField = {}},
                                             ui::ModuleCount{.module = "lab",
                                                             .model = "Lims_ClientModel",
                                                             .action = "Lims_ClientApprovalsLab",
                                                             .countField = "waiting",
                                                             .contextField = {},
                                                             .localeField = {}}}};
    parts.principal = ui::PrincipalMenu{.model = "Lims_ClientModel",
                                        .action = "Lims_ClientPerson",
                                        .contextField = {},
                                        .localeField = {},
                                        .nameField = "name",
                                        .detailField = "detail",
                                        .subtitleField = "role",
                                        .rolesField = "roles",
                                        .locales = {"en", "de"},
                                        .formats = {"en", "de"}};
    parts.searchHint = "Search samples";
    return parts;
}

constexpr char const* kScopes = R"({"options":[{"label":"Koeln","value":"k","detail":"EUR","group":"Rheinlab"},)"
                                R"({"label":"Osaka","value":"o","detail":"JPY","group":"Nippon"}]})";

/// A screen that shows the scope and whether it can be changed, and asks the shell to change it.
constexpr char const* kScopeScreen = R"({
  "vocab": "ui/1", "id": "probe.scope",
  "view": { "kind": "column", "children": [
    { "kind": "text", "testId": "scope", "text": { "fn": "coalesce", "args": [ { "env": "context" }, "-" ] } },
    { "kind": "text", "testId": "can", "text": { "if": [ { "can": "context" }, "yes", "no" ] } },
    { "kind": "button", "testId": "toO", "label": "o", "onClick": { "setContext": "o" } },
    { "kind": "button", "testId": "toZ", "label": "z", "onClick": { "setContext": "zzz" } } ] } })";

/// The application, the frame and the binding over one harness.
struct Rig {
    Harness h;
    ui::ShellParts parts;
    std::shared_ptr<ui::Document const> document;
    std::unique_ptr<ui::AppRoot> root;
    std::unique_ptr<ui::ShellFrame> frame;
    std::unique_ptr<ui::ShellPartsBinding> binding;
    std::vector<ui::ShellPartsModel::Target> opened;
    std::vector<std::vector<ui::ShellPartsModel::Target>> openedAll;
    std::vector<std::string> problems;
    std::size_t dirty = 0;
    int closedDirty = 0;
    int signedOut = 0;

    /// @param declared The parts.
    /// @param facts    What the catalog says (`screenListed`, `moduleListed`, `replyIsRecord`); the rest is ignored.
    /// @param navigated The navigator's entries.
    explicit Rig(ui::ShellParts declared, ui::ShellModelOptions facts = {},
                 std::vector<ui::CatalogEntry> navigated = entries())
        : parts{std::move(declared)} {
        std::string const text = std::string{R"({"vocab":"ui/1","id":"app","shell":)"} +
                                 glz::write<morph::wire::detail::EscapingWriteOpts{}>(parts).value_or("{}") + "}";
        document = ui::testing::loadOrFail(text, ui::testing::corpusCatalog());
        auto mounted = h.interpreter.mountApp(document);
        REQUIRE(mounted.has_value());
        root = std::move(*mounted);
        ui::ShellFrameOptions options;
        options.applicationName = "lims";
        options.navigator.entries = std::move(navigated);
        options.onSignOut = [this] { ++signedOut; };
        frame = std::make_unique<ui::ShellFrame>(h.backend, nullptr, std::move(options));
        ui::ShellBindingOptions bindingOptions;
        bindingOptions.model.screenListed = std::move(facts.screenListed);
        bindingOptions.model.moduleListed = std::move(facts.moduleListed);
        bindingOptions.model.replyIsRecord = std::move(facts.replyIsRecord);
        bindingOptions.model.moduleLabel = std::move(facts.moduleLabel);
        bindingOptions.model.screenTitle = std::move(facts.screenTitle);
        bindingOptions.model.catalogueLocales = std::move(facts.catalogueLocales);
        bindingOptions.model.dirtyTabs = [this] { return dirty; };
        bindingOptions.model.closeDirtyTabs = [this] { ++closedDirty; };
        bindingOptions.model.onProblem = [this](std::string const& message) { problems.push_back(message); };
        bindingOptions.open = [this](ui::ShellPartsModel::Target const& target) { opened.push_back(target); };
        bindingOptions.openAll = [this](std::vector<ui::ShellPartsModel::Target> const& targets) {
            openedAll.push_back(targets);
        };
        binding = std::make_unique<ui::ShellPartsBinding>(h.session, *root, *document->shell, *frame,
                                                          std::move(bindingOptions));
        h.settle();
    }

    ~Rig() {
        binding.reset();
        frame.reset();
        root.reset();
    }
    Rig(Rig const&) = delete;
    Rig& operator=(Rig const&) = delete;
    Rig(Rig&&) = delete;
    Rig& operator=(Rig&&) = delete;

    /// Answers the last call of @p action.
    void reply(std::string_view action, std::string json) { h.reply(h.dispatcher.last(action), std::move(json)); }

    /// The id of the widget of @p kind with test id @p testId.
    [[nodiscard]] int id(std::string_view kind, std::string_view testId) const {
        return h.backend.find(kind, "testId", testId).value();
    }

    [[nodiscard]] bool has(std::string_view kind, std::string_view testId) const {
        return h.backend.find(kind, "testId", testId).has_value();
    }

    [[nodiscard]] std::string prop(std::string_view kind, std::string_view testId, std::string_view name) const {
        return h.backend.prop(id(kind, testId), name);
    }
};

}  // namespace

namespace {

std::string menuItems(Rig& r) { return r.h.backend.prop(r.id("Menu", "shell_context"), "items"); }

}  // namespace

TEST_CASE("covd shell parts: an option's label is the translation, then its fallback, with its arguments in place",
          "[ui][shell][parts][covd]") {
    struct Case {
        std::string_view label;
        std::string_view shown;
    };
    auto const [label, shown] =
        GENERATE(Case{R"({"t":"scope.k","fallback":"Cologne"})", "Cologne"}, Case{R"({"t":"scope.k"})", "scope.k"},
                 Case{R"({"t":"scope.hit","fallback":"unused"})", "Hit"},
                 Case{R"({"t":"scope.args","args":{"n":3,"name":"lab"}})", "3 of lab"},
                 Case{R"({"t":"scope.args","args":7})", "{$n} of {$name}"}, Case{R"("plain")", "plain"});
    CAPTURE(label);
    Rig r{fullParts()};
    r.h.session.setTranslations([](std::string_view key, std::string_view) -> std::optional<std::string> {
        if (key == "scope.hit") {
            return "Hit";
        }
        if (key == "scope.args") {
            return "{$n} of {$name}";
        }
        return std::nullopt;
    });
    r.reply("Lims_ClientScopes", R"({"options":[{"label":)" + std::string{label} + R"(,"value":"k"}]})");
    r.h.settle();
    CHECK(menuItems(r).contains(std::string{shown} + ";checked=true"));
}

TEST_CASE("covd shell parts: a scope reply that does not fit is reported once and offers nothing",
          "[ui][shell][parts][covd]") {
    struct Case {
        std::string_view reply;
        std::string_view problem;
    };
    auto const [reply, problem] =
        GENERATE(Case{R"({"options":"none"})", "the reply has no list `options`"},
                 Case{R"({"options":[{"label":"x"}]})", "an option has no string or integer `value`"},
                 Case{R"({"options":[{"label":"x","value":[1]}]})", "an option has no string or integer `value`"});
    CAPTURE(reply);
    Rig r{fullParts()};
    r.reply("Lims_ClientScopes", std::string{reply});
    r.h.settle();
    bool reported = false;
    for (std::string const& text : r.problems) {
        reported = reported || text.contains(problem);
    }
    CHECK(reported);
    CHECK_FALSE(menuItems(r).contains("checked"));
}

TEST_CASE("covd shell parts: an integer option is a scope too", "[ui][shell][parts][covd]") {
    Rig r{fullParts()};
    auto const screen = r.h.mount(kScopeScreen);
    r.reply("Lims_ClientScopes", R"({"options":[{"label":"Seven","value":7},{"label":"Eight","value":8}]})");
    r.h.settle();
    CHECK(r.h.text("scope") == "7");
    CHECK(menuItems(r).contains("Seven;checked=true"));
}

TEST_CASE("covd shell parts: a count reply that is no non-negative integer is reported", "[ui][shell][parts][covd]") {
    auto const reply = GENERATE(as<std::string_view>{}, R"({"code":"a","total":-1})", R"({"code":"a"})",
                                R"({"code":"a","total":"many"})");
    CAPTURE(reply);
    Rig r{fullParts()};
    r.reply("Lims_ClientScopes", kScopes);
    r.reply("Shell_Rows", std::string{reply});
    r.h.settle();
    bool reported = false;
    for (std::string const& text : r.problems) {
        reported = reported || (text.contains("shell count") && text.contains("`total`"));
    }
    CHECK(reported);
}

TEST_CASE("covd shell parts: the shell's own words are the translation, then the built-in English or German",
          "[ui][shell][parts][covd]") {
    ui::FormatContext english;
    english.locale = "en";
    ui::FormatContext german;
    german.locale = "de-CH";
    ui::FormatContext translated;
    translated.translations = [](std::string_view key, std::string_view) -> std::optional<std::string> {
        return key == "morph.shell.offline" ? std::optional<std::string>{"Away"} : std::nullopt;
    };
    CHECK(ui::shellText("morph.shell.offline", translated) == "Away");
    CHECK(ui::shellText("morph.shell.offline", english) == "Offline");
    CHECK(ui::shellText("morph.shell.connected", german) == "Verbunden");
    CHECK(ui::shellText("morph.shell.connected", translated) == "Connected");
    CHECK(ui::shellText("morph.shell.nosuch", english) == "morph.shell.nosuch");
}

namespace {

// A rig with two hits in the lab source and one in billing, found by the text `x`, and the search field's id.
struct Searched {
    Rig r{fullParts()};
    int search = 0;

    Searched() {
        r.reply("Lims_ClientScopes", kScopes);
        // NOLINTNEXTLINE(cppcoreguidelines-prefer-member-initializer): the field exists only after the scopes reply has been delivered.
        search = r.id("TextInput", "shell_search");
        r.h.backend.edit(search, "x");
        r.h.settle();
        r.h.advance(std::chrono::milliseconds{ui::kSearchPause});
        r.reply("Lims_ClientSearchLab", R"({"hits":[{"screen":"a","title":"A"},{"screen":"b","title":"B"}]})");
        r.reply("Lims_ClientSearchBilling", R"({"hits":[{"screen":"c","title":"C"}]})");
    }

    [[nodiscard]] bool selected(std::string_view hit) const { return r.prop("Button", hit, "selected") == "true"; }
};

}  // namespace

TEST_CASE("covd shell parts: Up and Down walk the hits and wrap at either end", "[ui][shell][parts][covd]") {
    Searched s;
    REQUIRE(s.selected("shell_search_hit_0_0"));
    s.r.h.backend.press(s.search, "Up");
    s.r.h.settle();
    CHECK(s.selected("shell_search_hit_1_0"));  // up from the first wraps to the last
    s.r.h.backend.press(s.search, "Down");
    s.r.h.settle();
    CHECK(s.selected("shell_search_hit_0_0"));  // down from the last wraps to the first
    s.r.h.backend.press(s.search, "Down");
    s.r.h.settle();
    CHECK(s.selected("shell_search_hit_0_1"));
    s.r.h.backend.press(s.search, "Up");
    s.r.h.settle();
    CHECK(s.selected("shell_search_hit_0_0"));
}

TEST_CASE("covd shell parts: Alt Enter opens every hit, and a chord the search does not know does nothing",
          "[ui][shell][parts][covd]") {
    Searched s;
    s.r.h.backend.press(s.search, "Ctrl+Q");
    s.r.h.settle();
    CHECK(s.r.openedAll.empty());
    CHECK(s.r.prop("Popover", "shell_search_popover", "open") == "true");
    s.r.h.backend.press(s.search, "Alt+Enter");
    s.r.h.settle();
    REQUIRE(s.r.openedAll.size() == 1);
    CHECK(s.r.openedAll.front().size() == 3);
}

TEST_CASE("covd shell parts: opening every hit of a search that found none opens nothing",
          "[ui][shell][parts][covd]") {
    Rig r{fullParts()};
    r.reply("Lims_ClientScopes", kScopes);
    int const search = r.id("TextInput", "shell_search");
    r.h.backend.edit(search, "x");
    r.h.settle();
    r.h.advance(std::chrono::milliseconds{ui::kSearchPause});
    r.reply("Lims_ClientSearchLab", R"({"hits":[]})");
    r.reply("Lims_ClientSearchBilling", R"({"hits":[]})");
    r.h.backend.press(search, "Shift+Enter");
    r.h.backend.press(search, "Down");
    r.h.backend.submit(search, "x");
    r.h.settle();
    CHECK(r.openedAll.empty());
    CHECK(r.opened.empty());
}

TEST_CASE("covd shell parts: Escape in an empty field returns focus to the content", "[ui][shell][parts][covd]") {
    Rig r{fullParts()};
    r.reply("Lims_ClientScopes", kScopes);
    int const search = r.id("TextInput", "shell_search");
    CHECK(r.h.backend.press(search, "Escape"));
    r.h.settle();
    CHECK_FALSE(r.prop("TextInput", "shell_search", "text").contains("x"));
}

TEST_CASE(
    "covd shell parts: a scope request names an option or is refused, and asking for the chosen one changes nothing",
    "[ui][shell][parts][covd]") {
    Rig r{fullParts()};
    auto const screen = r.h.mount(kScopeScreen);
    r.reply("Lims_ClientScopes", kScopes);
    ui::ShellPartsModel& model = r.binding->model();
    model.chooseContext(9);  // no such option
    CHECK(r.h.text("scope") == "k");
    CHECK_FALSE(model.requestContext(ui::Value{std::string{"zzz"}}));
    CHECK(model.requestContext(ui::Value{std::string{"k"}}));  // the scope it already is
    CHECK(r.closedDirty == 0);
    CHECK(model.requestContext(ui::Value{std::string{"o"}}));
    r.h.settle();
    CHECK(r.h.text("scope") == "o");
}

TEST_CASE("covd shell parts: a hit that is not there, and a model with no help, give nothing",
          "[ui][shell][parts][covd]") {
    Rig r{fullParts()};
    r.reply("Lims_ClientScopes", kScopes);
    ui::ShellPartsModel& model = r.binding->model();
    CHECK_FALSE(model.hit(0, 0).has_value());  // nothing searched for
    CHECK_FALSE(model.hit(9, 0).has_value());  // no such source
    CHECK(model.allHits().empty());
    CHECK_FALSE(model.helpScreen().has_value());
    CHECK_FALSE(model.runHelp());
    model.chooseLocale(9);
    model.chooseFormat(9);
    CHECK(model.currentLocale() == "en");
    model.chooseLocale(1);
    model.chooseFormat(1);
    r.h.settle();
    CHECK(model.currentLocale() == "de");
}

TEST_CASE(
    "covd shell parts: a search that reads a hit with no title or screen, and a reply that is no list, keep only the "
    "hits",
    "[ui][shell][parts][covd]") {
    Rig r{fullParts()};
    r.reply("Lims_ClientScopes", kScopes);
    int const search = r.id("TextInput", "shell_search");
    r.h.backend.edit(search, "x");
    r.h.settle();
    r.h.advance(std::chrono::milliseconds{ui::kSearchPause});
    r.reply("Lims_ClientSearchLab",
            R"({"hits":[{"title":"no screen"},{"screen":"a"},{"screen":"b","title":"B","params":7},)"
            R"({"screen":"c","title":"C","params":{"id":1}}]})");
    r.reply("Lims_ClientSearchBilling", R"({"hits":"none"})");
    r.h.settle();
    ui::ShellPartsModel const& model = r.binding->model();
    std::vector<ui::ShellPartsModel::Target> const all = model.allHits();
    REQUIRE(all.size() == 2);
    CHECK(all.at(0).screen == "b");
    CHECK(all.at(0).params.empty());
    CHECK(all.at(1).screen == "c");
    CHECK(all.at(1).params.size() == 1);
}

TEST_CASE("covd shell parts: a help button's screen and its declared params are what the model offers",
          "[ui][shell][parts][covd]") {
    ui::ShellParts parts = fullParts();
    parts.help = ui::HelpEntry{.screen = "lims.about",
                               .model = {},
                               .action = {},
                               .params = {{"tab", std::string{"licence"}}, {"wide", true}}};
    Rig r{parts};
    ui::ShellPartsModel& model = r.binding->model();
    auto const target = model.helpScreen();
    REQUIRE(target.has_value());
    CHECK(target->screen == "lims.about");
    CHECK(target->params.size() == 2);
    CHECK_FALSE(model.runHelp());  // a screen, not an action
}

TEST_CASE("covd shell parts: the person's menu reads the name, the roles of every shape and what fails",
          "[ui][shell][parts][covd]") {
    struct Case {
        std::string_view reply;
        std::string_view name;
        std::string_view initials;
        std::vector<std::string> roles;
    };
    using Roles = std::vector<std::string>;
    auto const [reply, name, initials, roles] = GENERATE(
        Case{R"({"name":"Ann Lee","role":"Chemist","roles":[{"label":"Admin","detail":"all"},"Plain",)"
             R"({"label":{"t":"role.k","fallback":"Keyed"}},7]})",
             "Ann Lee", "AL", Roles{"Admin", "Plain", "Keyed", "7"}},
        Case{R"({"name":"Bo","roles":"admin"})", "Bo", "B", Roles{}},
        Case{R"({"name":"Cy","roles":[]})", "Cy", "C", Roles{}}, Case{R"({"name":"Di"})", "Di", "D", Roles{}});
    CAPTURE(reply);
    Rig r{fullParts()};
    r.reply("Lims_ClientPerson", std::string{reply});
    r.h.settle();
    ui::ShellPrincipalView const person = r.binding->model().view().principal;
    CHECK(person.shown);
    CHECK(person.name == name);
    CHECK(person.initials == initials);
    std::vector<std::string> labels;
    labels.reserve(person.roles.size());
    for (ui::ShellRoleView const& role : person.roles) {
        labels.push_back(role.label);
    }
    CHECK(labels == roles);
    CHECK(r.problems.empty());
}

TEST_CASE("covd shell parts: a person's reply with no name, and one that fails, are reported",
          "[ui][shell][parts][covd]") {
    Rig r{fullParts()};
    r.reply("Lims_ClientPerson", R"({"role":"Chemist"})");
    r.h.settle();
    static_cast<void>(r.binding->model().view());
    bool noName = false;
    for (std::string const& text : r.problems) {
        noName = noName || text.contains("the reply has no `name`");
    }
    CHECK(noName);

    Rig failing{fullParts()};
    failing.h.fail(failing.h.dispatcher.last("Lims_ClientPerson"));
    failing.h.settle();
    static_cast<void>(failing.binding->model().view());
    bool failed = false;
    for (std::string const& text : failing.problems) {
        failed = failed || text.contains("shell principal: the read failed");
    }
    CHECK(failed);
}

TEST_CASE(
    "covd shell parts: the languages come from the catalogue when the app names none, and are worded by translation",
    "[ui][shell][parts][covd]") {
    ui::ShellParts parts = fullParts();
    parts.principal->locales = {};
    ui::ShellModelOptions facts;
    facts.catalogueLocales = [] { return std::vector<std::string>{"fr", "en"}; };
    Rig r{parts, std::move(facts)};
    r.h.session.setTranslations([](std::string_view key, std::string_view) -> std::optional<std::string> {
        return key == "morph.shell.locale.fr" ? std::optional<std::string>{"Francais"} : std::nullopt;
    });
    r.reply("Lims_ClientPerson", R"({"name":"Ann"})");
    r.h.settle();
    ui::ShellPrincipalView const person = r.binding->model().view().principal;
    REQUIRE(person.locales.size() == 2);
    CHECK(person.locales.at(0).tag == "fr");
    CHECK(person.locales.at(0).label == "Francais");
    CHECK(person.locales.at(1).label == "English");
    REQUIRE(person.chosenLocale.has_value());
    CHECK(*person.chosenLocale == 1);
}

TEST_CASE("covd shell parts: a search source is titled by its declaration, then its module's label, then the module",
          "[ui][shell][parts][covd]") {
    struct Case {
        std::string_view title;
        bool label;
        std::string_view shown;
    };
    auto const [title, label, shown] =
        GENERATE(Case{"Lab", true, "Lab"}, Case{"", true, "Laboratory"}, Case{"", false, "lab"});
    ui::ShellParts parts = fullParts();
    parts.search.front().title = std::string{title};
    ui::ShellModelOptions facts;
    if (label) {
        facts.moduleLabel = [](std::string_view) { return std::string{"Laboratory"}; };
    }
    Rig r{parts, std::move(facts)};
    r.reply("Lims_ClientScopes", kScopes);
    int const search = r.id("TextInput", "shell_search");
    r.h.backend.edit(search, "x");
    r.h.settle();
    r.h.advance(std::chrono::milliseconds{ui::kSearchPause});
    r.reply("Lims_ClientSearchLab", R"({"hits":[]})");
    r.reply("Lims_ClientSearchBilling", R"({"hits":[]})");
    r.h.settle();
    ui::ShellSearchView const view = r.binding->model().view().search;
    REQUIRE_FALSE(view.sources.empty());
    CHECK(view.sources.front().title == shown);
}

TEST_CASE("covd shell parts: a failed search source is reported, and a hit without a title is dropped with a problem",
          "[ui][shell][parts][covd]") {
    Rig r{fullParts()};
    r.reply("Lims_ClientScopes", kScopes);
    int const search = r.id("TextInput", "shell_search");
    r.h.backend.edit(search, "x");
    r.h.settle();
    r.h.advance(std::chrono::milliseconds{ui::kSearchPause});
    r.reply("Lims_ClientSearchLab", R"({"hits":[{"screen":"a"}]})");
    r.h.fail(r.h.dispatcher.last("Lims_ClientSearchBilling"));
    r.h.settle();
    ui::ShellSearchView const view = r.binding->model().view().search;
    REQUIRE(view.sources.size() == 2);
    CHECK(view.sources.at(0).hits.empty());
    CHECK_FALSE(view.sources.at(0).failed);
    CHECK(view.sources.at(1).failed);
    int dropped = 0;
    int failed = 0;
    for (std::string const& text : r.problems) {
        dropped += text.contains("is dropped") ? 1 : 0;
        failed += text.contains("the read failed") ? 1 : 0;
    }
    CHECK(dropped == 1);
    CHECK(failed == 1);
}

TEST_CASE("covd shell parts: the inbox sums the sources that answered and marks one that failed or sent no count",
          "[ui][shell][parts][covd]") {
    struct Case {
        std::string_view billing;
        std::string_view lab;
        bool failed;
        bool counted;
        long long count;
    };
    auto const [billing, lab, failed, counted, count] = GENERATE(
        Case{R"({"n":2})", R"({"waiting":3})", false, true, 5}, Case{R"({"n":2})", R"({"other":3})", true, true, 2},
        Case{R"({"other":1})", R"({"other":3})", true, false, 0});
    CAPTURE(billing, lab);
    ui::ShellModelOptions facts;
    facts.screenTitle = [](std::string_view) { return std::string{"Approvals"}; };
    Rig r{fullParts(), std::move(facts)};
    r.reply("Lims_ClientApprovalsBilling", std::string{billing});
    r.reply("Lims_ClientApprovalsLab", std::string{lab});
    r.h.settle();
    ui::ShellInboxView const inbox = r.binding->model().view().inbox;
    CHECK(inbox.shown);
    CHECK(inbox.title == "Approvals");
    CHECK(inbox.failed == failed);
    CHECK(inbox.count.has_value() == counted);
    if (counted) {
        CHECK(*inbox.count == count);
    }
}

TEST_CASE("covd shell parts: a shell that declares only some parts draws only those, and the others answer nothing",
          "[ui][shell][parts][covd]") {
    enum class Keep : unsigned char { Nothing, Modules, Search, Inbox, Principal, Context };
    auto const keep =
        GENERATE(Keep::Nothing, Keep::Modules, Keep::Search, Keep::Inbox, Keep::Principal, Keep::Context);
    ui::ShellParts const all = fullParts();
    ui::ShellParts parts;
    parts.searchHint = "";
    switch (keep) {
        case Keep::Modules:
            parts.modules = all.modules;
            break;
        case Keep::Search:
            parts.search = all.search;
            break;
        case Keep::Inbox:
            parts.inbox = all.inbox;
            break;
        case Keep::Principal:
            parts.principal = all.principal;
            break;
        case Keep::Context:
            parts.context = all.context;
            break;
        case Keep::Nothing:
        default:
            break;
    }
    Rig r{parts};
    ui::ShellPartsModel& model = r.binding->model();
    ui::ShellPartsView const view = model.view();
    CHECK(view.context.shown == (keep == Keep::Context));
    CHECK(view.search.shown == (keep == Keep::Search));
    CHECK(view.inbox.shown == (keep == Keep::Inbox));
    CHECK(view.principal.shown == (keep == Keep::Principal));
    CHECK_FALSE(view.brand.shown);
    CHECK_FALSE(view.help.shown);
    CHECK(model.inboxScreen() == (keep == Keep::Inbox ? "lims.approvals" : ""));
    CHECK(model.moduleCounts().size() == (keep == Keep::Modules ? 2U : 0U));
    CHECK(model.contextReady() == (keep != Keep::Context));
    model.setSearchText("x");
    CHECK(model.allHits().empty());
    CHECK_FALSE(model.hit(0, 0).has_value());
}

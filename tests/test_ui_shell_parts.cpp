// SPDX-License-Identifier: Apache-2.0
//
// The shell's declared parts (docs/spec/ui/shell_parts.md): a scope switcher, module counts, one search, an inbox
// button and the person's menu, declared in the app document and read as its queries, drawn by the shell frame.
// A probe application on the scripted dispatcher and the recording backend.

#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstddef>
#include <glaze/glaze.hpp>
#include <memory>
#include <morph/ui/bundle.hpp>
#include <morph/ui/document.hpp>
#include <morph/ui/interpreter.hpp>
#include <morph/ui/navigator.hpp>
#include <morph/ui/service.hpp>
#include <morph/ui/shell_frame.hpp>
#include <morph/ui/shell_parts.hpp>
#include <morph/ui/shell_parts_binding.hpp>
#include <morph/ui/testing/recording_backend.hpp>
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

/// A screen with a mutation that changes the record the lab's count reads.
constexpr char const* kRenameScreen = R"({
  "vocab": "ui/1", "id": "probe.rename",
  "models": { "sample": { "type": "Lims_ClientModel" } },
  "mutations": { "rename": { "model": "sample", "action": "Shell_Rename" } },
  "view": { "kind": "button", "testId": "rename", "label": "rename", "onClick": { "run": "rename" } } })";

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
    explicit Rig(ui::ShellParts declared, ui::ShellModelOptions facts = {}) : parts{std::move(declared)} {
        std::string const text =
            std::string{R"({"vocab":"ui/1","id":"app","shell":)"} + glz::write_json(parts).value_or("{}") + "}";
        document = ui::testing::loadOrFail(text, ui::testing::corpusCatalog());
        auto mounted = h.interpreter.mountApp(document);
        REQUIRE(mounted.has_value());
        root = std::move(*mounted);
        ui::ShellFrameOptions options;
        options.applicationName = "lims";
        options.navigator.entries = entries();
        options.onSignOut = [this] { ++signedOut; };
        frame = std::make_unique<ui::ShellFrame>(h.backend, nullptr, std::move(options));
        ui::ShellBindingOptions binding_options;
        binding_options.model.screenListed = std::move(facts.screenListed);
        binding_options.model.moduleListed = std::move(facts.moduleListed);
        binding_options.model.replyIsRecord = std::move(facts.replyIsRecord);
        binding_options.model.dirtyTabs = [this] { return dirty; };
        binding_options.model.closeDirtyTabs = [this] { ++closedDirty; };
        binding_options.model.onProblem = [this](std::string const& message) { problems.push_back(message); };
        binding_options.open = [this](ui::ShellPartsModel::Target const& target) { opened.push_back(target); };
        binding_options.openAll = [this](std::vector<ui::ShellPartsModel::Target> const& targets) {
            openedAll.push_back(targets);
        };
        binding = std::make_unique<ui::ShellPartsBinding>(h.session, *root, *document->shell, *frame,
                                                          std::move(binding_options));
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

// Mutation: ShellPartsModel::apply stops calling Session::setContext (no screen ever reads a scope).
TEST_CASE("shell parts: the switcher lists a read action's options and the chosen value reaches env context",
          "[ui][shell][parts]") {
    Rig r{fullParts()};
    CHECK_FALSE(r.binding->contextReady());
    CHECK(r.h.dispatcher.count("Lims_ClientScopes") == 1);
    r.reply("Lims_ClientScopes", kScopes);
    CHECK(r.binding->contextReady());

    auto const screen = r.h.mount(kScopeScreen);
    CHECK(r.h.text("scope") == "k");
    int const menu = r.id("Menu", "shell_context");
    std::string const items = r.h.backend.prop(menu, "items");
    CHECK(items.contains("Rheinlab;enabled=false;heading"));
    CHECK(items.contains("Koeln;checked=true;description=EUR"));
    CHECK(items.contains("Osaka;checked=false;description=JPY"));
    CHECK(r.h.backend.prop(menu, "label").contains("Koeln"));
    CHECK(r.h.backend.prop(menu, "detail").contains("EUR"));

    // Choosing Osaka (after the Nippon heading) writes its value to the scope every screen reads.
    r.h.backend.chooseMenuEntry(menu, {3});
    r.h.settle();
    CHECK(r.h.text("scope") == "o");
    CHECK(r.h.backend.prop(menu, "items").contains("Osaka;checked=true"));
    // The shell-owned reads send the chosen scope under the member they declare.
    CHECK(r.h.dispatcher.calls.at(r.h.dispatcher.last("Shell_Rows")).body.contains(R"("scope":"o")"));
}

// Mutation: ShellPartsModel::requestContext accepts a value that is not among the options.
TEST_CASE(
    "shell parts: a screen's setContext command changes the scope, refuses a value that is not an option, and can "
    "follows the switcher",
    "[ui][shell][parts]") {
    Rig r{fullParts()};
    auto const screen = r.h.mount(kScopeScreen);
    // Before the switcher answers there is no option to change to.
    CHECK(r.h.text("can") == "no");
    r.reply("Lims_ClientScopes", kScopes);
    CHECK(r.h.text("can") == "yes");
    CHECK(r.h.text("scope") == "k");

    r.h.backend.click(r.h.byTestId("Button", "toZ"));
    r.h.settle();
    CHECK(r.h.text("scope") == "k");
    REQUIRE_FALSE(r.h.diagnostics.empty());
    CHECK(r.h.diagnostics.back().reason.contains("setContext"));

    r.h.backend.click(r.h.byTestId("Button", "toO"));
    r.h.settle();
    CHECK(r.h.text("scope") == "o");
}

// Mutation: the context query's body stops reading env locale (no re-query), or followOptions keeps a vanished value.
TEST_CASE("shell parts: a change of locale asks the parts again, and a vanished option makes the first one the scope",
          "[ui][shell][parts]") {
    Rig r{fullParts()};
    auto const screen = r.h.mount(kScopeScreen);
    r.reply("Lims_ClientScopes", kScopes);
    r.h.backend.click(r.h.byTestId("Button", "toO"));
    r.h.settle();
    REQUIRE(r.h.text("scope") == "o");
    REQUIRE(r.h.dispatcher.count("Lims_ClientScopes") == 1);

    r.h.session.setLocale("de");
    r.h.settle();
    REQUIRE(r.h.dispatcher.count("Lims_ClientScopes") == 2);
    CHECK(r.h.dispatcher.calls.at(r.h.dispatcher.last("Lims_ClientScopes")).body.contains(R"("lang":"de")"));
    r.reply("Lims_ClientScopes", R"({"options":[{"label":"Koeln","value":"k"}]})");
    CHECK(r.h.text("scope") == "k");
}

// Mutation: ShellPartsModel::requestContext applies the switch without asking when tabs hold unsaved changes.
TEST_CASE("shell parts: switching the scope asks once before discarding unsaved changes", "[ui][shell][parts]") {
    Rig r{fullParts()};
    auto const screen = r.h.mount(kScopeScreen);
    r.reply("Lims_ClientScopes", kScopes);
    r.dirty = 2;
    int const menu = r.id("Menu", "shell_context");
    r.h.backend.chooseMenuEntry(menu, {3});
    r.h.settle();
    CHECK(r.prop("Dialog", "shell_confirm", "open") == "true");
    CHECK(r.h.text("scope") == "k");
    CHECK(r.closedDirty == 0);

    r.h.backend.click(r.h.byTestId("Button", "shell_confirm_no"));
    r.h.settle();
    CHECK(r.prop("Dialog", "shell_confirm", "open") == "false");
    CHECK(r.h.text("scope") == "k");

    r.h.backend.chooseMenuEntry(menu, {3});
    r.h.settle();
    r.h.backend.click(r.h.byTestId("Button", "shell_confirm_yes"));
    r.h.settle();
    CHECK(r.closedDirty == 1);
    CHECK(r.h.text("scope") == "o");
}

// Mutation: ShellPartsModel::moduleCounts keeps the first answer (a later answer is not shown).
TEST_CASE("shell parts: a module's count shows on its collapsed group and follows an action's declared change",
          "[ui][shell][parts]") {
    Rig r{fullParts()};
    r.reply("Lims_ClientScopes", kScopes);
    auto const screen = r.h.mount(kRenameScreen);
    r.reply("Shell_Rows", R"({"code":"a","total":3})");
    r.reply("Lims_ClientCount", R"({"open":0})");
    CHECK(r.prop("Badge", "nav_module_badge_lab", "text") == "3");
    CHECK(r.prop("Badge", "nav_module_badge_lab", "visible") != "false");
    // A count of zero draws nothing; only the open module lists its screens.
    CHECK(r.prop("Badge", "nav_module_badge_billing", "visible") == "false");

    // An action that declares a change to the record the count reads makes the count read again.
    r.h.backend.click(r.h.byTestId("Button", "rename"));
    r.h.settle();
    r.reply("Shell_Rename", "null");
    REQUIRE(r.h.dispatcher.count("Shell_Rows") == 2);
    r.reply("Shell_Rows", R"({"code":"a","total":2})");
    CHECK(r.prop("Badge", "nav_module_badge_lab", "text") == "2");
}

// Mutation: the navigator draws every module's screens at once (setModuleGroups is ignored).
TEST_CASE("shell parts: module groups list only the open module's screens and give each closed module its count",
          "[ui][shell][parts]") {
    Rig r{fullParts()};
    ui::Navigator& navigator = r.frame->navigator();
    CHECK(navigator.moduleGroups());
    navigator.setCurrent("lims.lab.samples");
    CHECK(navigator.openModule() == "lab");
    CHECK(r.prop("Column", "navigator_group_lab/Lab_section", "visible") != "false");
    CHECK(r.prop("Column", "navigator_group_billing/Billing_section", "visible") == "false");
    // A module of one screen is that screen's row, with no module row.
    CHECK(r.prop("Row", "nav_module_row_shell", "visible") == "false");
    CHECK(r.prop("Row", "nav_row_lims.home", "visible") != "false");

    CHECK(navigator.setOpenModule("billing"));
    CHECK(r.prop("Column", "navigator_group_lab/Lab_section", "visible") == "false");
    CHECK(r.prop("Column", "navigator_group_billing/Billing_section", "visible") != "false");
    CHECK(r.prop("Text", "nav_module_chevron_lab", "text") == "▸");
    CHECK(r.prop("Text", "nav_module_chevron_billing", "text") == "▾");
    // With a global search the navigator's own field is hidden.
    CHECK(r.prop("TextInput", "navigator_search", "visible") == "false");
    // The compact menu has one submenu per module, its count after the label.
    ui::NavigatorBadge const badge{.text = "4", .tone = ui::Tone::Neutral};
    navigator.setModuleBadge("lab", badge);
    bool found = false;
    for (ui::MenuEntry const& menu : navigator.menuEntries()) {
        found = found || (menu.label == "Lab · 4" && menu.items.size() == 2);
    }
    CHECK(found);
}

// Mutation: the search query stops being debounced (every edit asks), or an old text's reply replaces the new one.
TEST_CASE("shell parts: search asks after a pause, the last edit wins, and Enter opens the highlighted hit",
          "[ui][shell][parts]") {
    Rig r{fullParts()};
    r.reply("Lims_ClientScopes", kScopes);
    int const search = r.id("TextInput", "shell_search");
    CHECK(r.prop("TextInput", "shell_search", "placeholder").contains("Search samples"));

    r.h.backend.edit(search, "a");
    r.h.settle();
    // The sources are asked together, once the pause has passed; until then the panel shows the busy row.
    CHECK(r.h.dispatcher.count("Lims_ClientSearchLab") == 0);
    CHECK(r.has("Text", "shell_search_busy"));
    r.h.advance(std::chrono::milliseconds{ui::kSearchPause});
    REQUIRE(r.h.dispatcher.count("Lims_ClientSearchLab") == 1);
    REQUIRE(r.h.dispatcher.count("Lims_ClientSearchBilling") == 1);
    r.reply("Lims_ClientSearchLab", R"({"hits":[]})");
    r.reply("Lims_ClientSearchBilling", R"({"hits":[]})");

    // Edits inside the pause are replaced by the last.
    r.h.backend.edit(search, "ab");
    r.h.settle();
    r.h.advance(std::chrono::milliseconds{100});
    r.h.backend.edit(search, "abc");
    r.h.settle();
    CHECK(r.h.dispatcher.count("Lims_ClientSearchLab") == 1);
    r.h.advance(std::chrono::milliseconds{ui::kSearchPause});
    REQUIRE(r.h.dispatcher.count("Lims_ClientSearchLab") == 2);
    CHECK(r.h.dispatcher.calls.at(r.h.dispatcher.last("Lims_ClientSearchLab")).body.contains(R"("q":"abc")"));
    // While a source is in flight the panel shows the busy row.
    CHECK(r.has("Text", "shell_search_busy"));

    r.reply("Lims_ClientSearchLab",
            R"({"hits":[{"screen":"lims.lab.sample","params":{"id":7},"title":"S-7","kind":"Sample",)"
            R"("detail":"released"}]})");
    r.reply("Lims_ClientSearchBilling",
            R"({"hits":[{"screen":"lims.billing.invoice","title":"INV-1","kind":"Invoice"}]})");
    CHECK(r.prop("Popover", "shell_search_popover", "open") == "true");
    CHECK(r.prop("Text", "shell_search_source_0", "text") == "Lab");
    CHECK(r.prop("Text", "shell_search_source_1", "text") == "Billing");
    CHECK(r.has("Button", "shell_search_hit_0_0"));
    CHECK(r.prop("Button", "shell_search_hit_0_0", "selected") == "true");

    // Down moves the highlight while focus stays in the field; Enter opens the highlighted hit with its params.
    CHECK(r.h.backend.press(search, "Down"));
    r.h.settle();
    CHECK(r.prop("Button", "shell_search_hit_1_0", "selected") == "true");
    r.h.backend.submit(search, "abc");
    r.h.settle();
    REQUIRE(r.opened.size() == 1);
    CHECK(r.opened.front().screen == "lims.billing.invoice");
    // The field is cleared and the panel closed once a hit is chosen.
    CHECK(r.prop("Popover", "shell_search_popover", "open") == "false");
}

// Mutation: Shift Enter opens only the highlighted hit; or Escape leaves the panel open.
TEST_CASE("shell parts: Shift Enter opens every hit, Escape closes the panel, and a source that failed says so",
          "[ui][shell][parts]") {
    Rig r{fullParts()};
    r.reply("Lims_ClientScopes", kScopes);
    int const search = r.id("TextInput", "shell_search");
    r.h.backend.edit(search, "x");
    r.h.settle();
    r.h.advance(std::chrono::milliseconds{ui::kSearchPause});
    r.reply("Lims_ClientSearchLab", R"({"hits":[{"screen":"a","title":"A"},{"screen":"b","title":"B"}]})");
    r.h.fail(r.h.dispatcher.last("Lims_ClientSearchBilling"));
    // The other source's hits still show; the failed one says so under its heading.
    CHECK(r.has("Text", "shell_search_failed_1"));
    CHECK(r.prop("Text", "shell_search_failed_1", "text").contains("could not be searched"));
    CHECK(r.has("Button", "shell_search_hit_0_1"));

    // Escape closes the panel and leaves the text; Escape again clears the text.
    CHECK(r.h.backend.press(search, "Escape"));
    r.h.settle();
    CHECK(r.prop("Popover", "shell_search_popover", "open") == "false");
    CHECK(r.prop("TextInput", "shell_search", "text").contains("x"));
    CHECK(r.h.backend.press(search, "Escape"));
    r.h.settle();
    CHECK_FALSE(r.prop("TextInput", "shell_search", "text").contains("x"));

    r.h.backend.edit(search, "xy");
    r.h.settle();
    r.h.advance(std::chrono::milliseconds{ui::kSearchPause});
    r.reply("Lims_ClientSearchLab", R"({"hits":[{"screen":"a","title":"A"},{"screen":"b","title":"B"}]})");
    r.reply("Lims_ClientSearchBilling", R"({"hits":[]})");
    CHECK(r.h.backend.press(search, "Shift+Enter"));
    r.h.settle();
    REQUIRE(r.openedAll.size() == 1);
    REQUIRE(r.openedAll.front().size() == 2);
    CHECK(r.openedAll.front().at(0).screen == "a");
    CHECK(r.openedAll.front().at(1).screen == "b");
    CHECK(r.opened.empty());
}

// Mutation: ShellPartsModel::sourceHits drops `target.params = *params->asRecord();`.
TEST_CASE("shell parts: Enter opens the highlighted hit with its params", "[ui][shell][parts]") {
    Rig r{fullParts()};
    r.reply("Lims_ClientScopes", kScopes);
    int const search = r.id("TextInput", "shell_search");
    r.h.backend.edit(search, "s");
    r.h.settle();
    r.h.advance(std::chrono::milliseconds{ui::kSearchPause});
    r.reply("Lims_ClientSearchLab",
            R"({"hits":[{"screen":"lims.lab.sample","params":{"id":7,"mode":"edit"},"title":"S-7"}]})");
    r.reply("Lims_ClientSearchBilling", R"({"hits":[]})");
    r.h.backend.submit(search, "s");
    r.h.settle();
    REQUIRE(r.opened.size() == 1);
    CHECK(r.opened.front().screen == "lims.lab.sample");
    REQUIRE(r.opened.front().params.contains("id"));
    CHECK(*r.opened.front().params.at("id").asInt() == 7);
    REQUIRE(r.opened.front().params.contains("mode"));
    CHECK(*r.opened.front().params.at("mode").asString() == "edit");
}

// Mutation: the frame reports the view's own place of a source, not the declared one (the indices shift when a source
// is left out).
TEST_CASE("shell parts: a denied search source is left out and the other sources' hits open as themselves",
          "[ui][shell][parts]") {
    Rig r{fullParts()};
    r.reply("Lims_ClientScopes", kScopes);
    int const search = r.id("TextInput", "shell_search");
    r.h.backend.edit(search, "s");
    r.h.settle();
    r.h.advance(std::chrono::milliseconds{ui::kSearchPause});
    r.h.fail(r.h.dispatcher.last("Lims_ClientSearchLab"), "denied");
    r.reply("Lims_ClientSearchBilling",
            R"({"hits":[{"screen":"lims.billing.invoice","params":{"id":3},"title":"INV-3"}]})");
    CHECK_FALSE(r.has("Text", "shell_search_source_0"));
    CHECK(r.has("Text", "shell_search_source_1"));
    CHECK(r.problems.empty());
    // Enter opens the billing hit, not whatever the lab's query holds.
    r.h.backend.submit(search, "s");
    r.h.settle();
    REQUIRE(r.opened.size() == 1);
    CHECK(r.opened.front().screen == "lims.billing.invoice");
    CHECK(*r.opened.front().params.at("id").asInt() == 3);
}

TEST_CASE("shell parts: a click on a hit of a source after a denied one opens that hit", "[ui][shell][parts]") {
    Rig r{fullParts()};
    r.reply("Lims_ClientScopes", kScopes);
    r.h.backend.edit(r.id("TextInput", "shell_search"), "s");
    r.h.settle();
    r.h.advance(std::chrono::milliseconds{ui::kSearchPause});
    r.h.fail(r.h.dispatcher.last("Lims_ClientSearchLab"), "denied");
    r.reply("Lims_ClientSearchBilling", R"({"hits":[{"screen":"lims.billing.invoice","title":"INV-3"}]})");
    r.h.backend.click(r.id("Button", "shell_search_hit_1_0"));
    r.h.settle();
    REQUIRE(r.opened.size() == 1);
    CHECK(r.opened.front().screen == "lims.billing.invoice");
}

// Mutation: sourceHits reads a failed source's last value.
TEST_CASE("shell parts: Shift Enter opens only the hits the panel draws", "[ui][shell][parts]") {
    Rig r{fullParts()};
    r.reply("Lims_ClientScopes", kScopes);
    int const search = r.id("TextInput", "shell_search");
    r.h.backend.edit(search, "x");
    r.h.settle();
    r.h.advance(std::chrono::milliseconds{ui::kSearchPause});
    r.reply("Lims_ClientSearchLab", R"({"hits":[{"screen":"a","title":"A"}]})");
    r.reply("Lims_ClientSearchBilling", R"({"hits":[{"screen":"c","title":"C"}]})");
    r.h.backend.edit(search, "xy");
    r.h.settle();
    r.h.advance(std::chrono::milliseconds{ui::kSearchPause});
    r.reply("Lims_ClientSearchLab", R"({"hits":[{"screen":"a","title":"A"}]})");
    r.h.fail(r.h.dispatcher.last("Lims_ClientSearchBilling"));
    CHECK(r.has("Text", "shell_search_failed_1"));
    CHECK(r.h.backend.press(search, "Shift+Enter"));
    r.h.settle();
    REQUIRE(r.openedAll.size() == 1);
    REQUIRE(r.openedAll.front().size() == 1);
    CHECK(r.openedAll.front().front().screen == "a");
}

// Mutation: the model stops reporting a failed count, a failed inbox source or search source, a malformed person's
// reply or a dropped hit; or the catalog's facts are not asked.
TEST_CASE("shell parts: each failure reaches onProblem once", "[ui][shell][parts]") {
    ui::ShellModelOptions facts;
    facts.screenListed = [](std::string_view id) { return id != "lims.approvals"; };
    facts.moduleListed = [](std::string_view module) { return module != "billing"; };
    facts.replyIsRecord = [](std::string_view action) { return action != "Shell_Rows"; };
    Rig r{fullParts(), std::move(facts)};
    auto const times = [&r](std::string_view needle) {
        return std::ranges::count_if(r.problems, [needle](std::string const& text) { return text.contains(needle); });
    };
    // What the catalog says is reported as soon as the parts are made.
    CHECK(times("names the screen lims.approvals") == 1);
    CHECK(times("names the module billing") == 1);
    CHECK(times("the reply of Shell_Rows is not a registered record") == 1);
    CHECK(times("the reply of Lims_ClientCount is not a registered record") == 0);

    r.reply("Lims_ClientScopes", kScopes);
    r.h.fail(r.h.dispatcher.last("Lims_ClientCount"));
    r.reply("Shell_Rows", R"({"total":"many"})");
    r.h.fail(r.h.dispatcher.last("Lims_ClientApprovalsLab"));
    r.reply("Lims_ClientApprovalsBilling", R"({"n":1})");
    r.reply("Lims_ClientPerson", R"({"role":"x"})");
    r.h.backend.edit(r.id("TextInput", "shell_search"), "s");
    r.h.settle();
    r.h.advance(std::chrono::milliseconds{ui::kSearchPause});
    r.reply("Lims_ClientSearchLab", R"({"hits":[{"title":"no screen"},{"screen":"a","title":"A"}]})");
    r.h.fail(r.h.dispatcher.last("Lims_ClientSearchBilling"));
    // Settling again draws the parts again, and nothing is reported twice.
    r.h.session.setLocale("de");
    r.h.session.setLocale("en");
    r.h.settle();
    CHECK(times("shellCount1: the read failed") == 1);
    CHECK(times("shellCount0: the reply has no non-negative integer `total`") == 1);
    CHECK(times("shellInbox1: the read failed") == 1);
    CHECK(times("shell principal: the reply has no `name`") == 1);
    CHECK(times("shellSearch0: a hit has no string `screen`") == 1);
    CHECK(times("shellSearch1: the read failed") == 1);
}

// Mutation: Escape on an empty field of the compact overlay moves focus and leaves the overlay open.
TEST_CASE("shell parts: Escape in the compact search overlay closes it, and the results panel is named",
          "[ui][shell][parts]") {
    Rig r{fullParts()};
    r.reply("Lims_ClientScopes", kScopes);
    r.frame->setCompact(true);
    r.h.settle();
    r.h.backend.click(r.id("Button", "shell_search_open"));
    REQUIRE(r.prop("Dialog", "shell_search_overlay", "open") == "true");
    int const input = r.id("TextInput", "shell_search_overlay_input");
    CHECK(r.h.backend.press(input, "Escape"));
    r.h.settle();
    CHECK(r.prop("Dialog", "shell_search_overlay", "open") == "false");
    CHECK(r.prop("Column", "shell_search_overlay_results", "a11yName") == "Search results");
}

TEST_CASE("shell parts: the results panel is named, and sign out follows the language", "[ui][shell][parts]") {
    Rig r{fullParts()};
    r.reply("Lims_ClientScopes", kScopes);
    r.reply("Lims_ClientPerson", R"({"name":"Anna","roles":[]})");
    CHECK(r.prop("Column", "shell_search_results", "a11yName") == "Search results");
    CHECK(r.h.backend.prop(r.id("Menu", "shell_principal_menu"), "items").contains("Sign out"));
    r.h.session.setLocale("de");
    r.h.settle();
    CHECK(r.prop("Column", "shell_search_results", "a11yName") != "Search results");
    CHECK(r.prop("Button", "shell_signOut", "label") == "Abmelden");
    std::string const items = r.h.backend.prop(r.id("Menu", "shell_principal_menu"), "items");
    CHECK(items.contains("Abmelden"));
    CHECK_FALSE(items.contains("Sign out"));
    r.frame->setCompact(true);
    r.h.settle();
    CHECK(r.h.backend.prop(r.id("Menu", "shell_nav_menu"), "items").contains("Abmelden"));
}

// Mutation: contextView shows the switcher whatever the answer was.
TEST_CASE("shell parts: a switcher the server refuses is left out, and the start screen is not held",
          "[ui][shell][parts]") {
    Rig r{fullParts()};
    CHECK_FALSE(r.binding->contextReady());
    r.h.fail(r.h.dispatcher.last("Lims_ClientScopes"), "denied");
    r.h.settle();
    CHECK(r.binding->contextReady());
    CHECK(r.prop("Menu", "shell_context", "visible") == "false");
    CHECK(r.problems.empty());
    // A read that needs a scope waits for one, and the unscoped ones are asked.
    CHECK(r.h.dispatcher.count("Lims_ClientSearchLab") == 0);
    CHECK(r.h.dispatcher.count("Shell_Rows") == 0);
    CHECK(r.h.dispatcher.count("Lims_ClientApprovalsBilling") == 1);
}

// Mutation: showSearch destroys the hit rows at once, inside the click handler that is running.
TEST_CASE("shell parts: the rows of a hit stay alive until its own click handler has returned", "[ui][shell][parts]") {
    Harness h;
    ui::ShellFrameOptions options;
    options.applicationName = "lims";
    options.navigator.entries = entries();
    ui::ShellFrame frame{h.backend, nullptr, std::move(options)};
    ui::ShellPartsView withHit;
    withHit.search.shown = true;
    withHit.search.sources.push_back(
        ui::ShellSourceView{.source = 0,
                            .title = "Lab",
                            .failed = false,
                            .pending = false,
                            .hits = {ui::ShellHitView{.kind = "", .title = "S-7", .detail = ""}}});
    ui::ShellPartsView withoutHit;
    withoutHit.search.shown = true;
    bool hitDestroyedInside = false;
    bool ran = false;
    ui::ShellPartsHandlers handlers;
    handlers.onSearchText = [&](std::string const& text) {
        if (!text.empty()) {
            return;
        }
        // What the model does when the text is cleared: the view changes under the running handler.
        std::size_t const before = h.backend.log().size();
        frame.setParts(withoutHit);
        ran = true;
        auto const& log = h.backend.log();
        hitDestroyedInside = std::any_of(log.begin() + static_cast<std::ptrdiff_t>(before), log.end(),
                                         [](std::string const& line) { return line.contains("destroy Button"); });
    };
    handlers.onSearchOpen = [](std::size_t, std::size_t) {};
    frame.setPartsHandlers(std::move(handlers));
    frame.setParts(withHit);
    int const search = h.backend.find("TextInput", "testId", "shell_search").value();
    h.backend.edit(search, "s");
    h.settle();
    frame.setParts(withHit);
    int const row = h.backend.find("Button", "testId", "shell_search_hit_0_0").value();
    h.backend.click(row);
    h.settle();
    CHECK(ran);
    CHECK_FALSE(hitDestroyedInside);
    // Once the handler has returned, a later change of the view takes the old rows away.
    frame.setParts(withHit);
    frame.setParts(withoutHit);
    CHECK_FALSE(h.backend.find("Button", "testId", "shell_search_hit_0_0").has_value());
}

// Mutation: the navigator places its groups module by module for every catalog.
TEST_CASE("shell parts: without module groups a catalog that interleaves modules keeps its own order",
          "[ui][shell][parts]") {
    Harness h;
    ui::ShellFrameOptions options;
    options.applicationName = "lims";
    options.navigator.entries = {entry("a.x", "A", "x"), entry("b.y", "B", "y"), entry("a.z", "A", "z")};
    ui::ShellFrame frame{h.backend, nullptr, std::move(options)};
    // The sidebar lists the sections in the order the widgets are placed.
    auto const sections = [&h] {
        std::string const tree = h.backend.dump();
        return std::array{tree.find("navigator_group_A/x_section"), tree.find("navigator_group_B/y_section"),
                          tree.find("navigator_group_A/z_section")};
    };
    auto const plain = sections();
    REQUIRE(plain.at(0) != std::string::npos);
    CHECK(plain.at(0) < plain.at(1));
    CHECK(plain.at(1) < plain.at(2));
    frame.navigator().setModuleGroups(true);
    h.settle();
    auto const grouped = sections();
    CHECK(grouped.at(0) < grouped.at(2));
    CHECK(grouped.at(2) < grouped.at(1));
    frame.navigator().setModuleGroups(false);
    h.settle();
    CHECK(sections() == plain);
}

// Mutation: ~ShellPartsBinding leaves the modules collapsed.
TEST_CASE("shell parts: taking the binding away gives the navigator its plain groups back", "[ui][shell][parts]") {
    Rig r{fullParts()};
    r.reply("Lims_ClientScopes", kScopes);
    r.reply("Shell_Rows", R"({"total":3})");
    r.frame->navigator().setCurrent("lims.lab.samples");
    CHECK(r.prop("Column", "navigator_group_billing/Billing_section", "visible") == "false");
    r.binding.reset();
    r.h.settle();
    CHECK(r.prop("Column", "navigator_group_billing/Billing_section", "visible") != "false");
}

TEST_CASE("shell parts: a search with no hits says so, in words", "[ui][shell][parts]") {
    Rig r{fullParts()};
    r.reply("Lims_ClientScopes", kScopes);
    r.h.backend.edit(r.id("TextInput", "shell_search"), "zz");
    r.h.settle();
    r.h.advance(std::chrono::milliseconds{ui::kSearchPause});
    r.reply("Lims_ClientSearchLab", R"({"hits":[]})");
    r.reply("Lims_ClientSearchBilling", R"({"hits":[]})");
    CHECK(r.prop("Text", "shell_search_empty", "text") == "No results for “zz”");
}

// Mutation: the inbox shows one source's count, not the sum; or a failed source is added as 0 without the marker.
TEST_CASE("shell parts: the inbox button's count is the sum of its sources, and a failed source is marked",
          "[ui][shell][parts]") {
    Rig r{fullParts()};
    r.reply("Lims_ClientScopes", kScopes);
    r.reply("Lims_ClientApprovalsBilling", R"({"n":4})");
    // The sources that answered are summed; one has so far.
    CHECK(r.prop("Badge", "shell_inbox_count", "text") == "4");
    r.reply("Lims_ClientApprovalsLab", R"({"waiting":3})");
    CHECK(r.prop("Badge", "shell_inbox_count", "text") == "7");
    CHECK(r.prop("Button", "shell_inbox", "a11yName").contains("7 waiting"));
    CHECK(r.prop("Text", "shell_inbox_error", "visible") == "false");
    r.h.backend.click(r.id("Button", "shell_inbox"));
    REQUIRE(r.opened.size() == 1);
    CHECK(r.opened.front().screen == "lims.approvals");
}

TEST_CASE("shell parts: a failed inbox source puts a marker on the button and leaves its count out",
          "[ui][shell][parts]") {
    Rig r{fullParts()};
    r.reply("Lims_ClientScopes", kScopes);
    r.reply("Lims_ClientApprovalsBilling", R"({"n":4})");
    r.h.fail(r.h.dispatcher.last("Lims_ClientApprovalsLab"));
    CHECK(r.prop("Badge", "shell_inbox_count", "text") == "4");
    CHECK(r.prop("Text", "shell_inbox_error", "visible") != "false");
}

// Mutation: the menu lists no roles, or choosing a language does not reach the session.
TEST_CASE("shell parts: the person's menu shows the name, lists the roles, and switches the locale and the format",
          "[ui][shell][parts]") {
    Rig r{fullParts()};
    r.reply("Lims_ClientScopes", kScopes);
    r.reply("Lims_ClientPerson", R"({"name":"Dr. Anna Meyer","role":"Lab director","detail":"ameyer","roles":)"
                                 R"([{"label":"Invoice approver","detail":"up to 10,000"},"Lab director"]})");
    int const menu = r.id("Menu", "shell_principal_menu");
    CHECK(r.h.backend.prop(menu, "label").contains("Dr. Anna Meyer"));
    CHECK(r.h.backend.prop(menu, "detail").contains("Lab director"));
    std::string const items = r.h.backend.prop(menu, "items");
    CHECK(items.contains("Invoice approver;enabled=false;description=up to 10\\,000"));
    CHECK(items.contains("Lab director;enabled=false"));
    CHECK(items.contains("Your roles;enabled=false;heading"));
    CHECK(items.contains("English;checked=true"));
    // The sidebar's own principal text and the sign-out button give way to the menu.
    CHECK(r.prop("Text", "shell_principal", "visible") == "false");

    r.h.backend.chooseMenuEntry(menu, {8});
    r.h.settle();
    CHECK(r.h.session.formattingNow().locale == "de");
    r.h.backend.chooseMenuEntry(menu, {12});
    r.h.settle();
    CHECK(r.h.session.formatLocale() == "de");
    // A later language change keeps the format the person chose.
    r.h.session.setLocale("en");
    CHECK(r.h.session.formatLocale() == "de");
}

TEST_CASE(
    "shell parts: compact, the switcher, the modules and the person's menu join the navigation menu and the "
    "search is a button",
    "[ui][shell][parts]") {
    Rig r{fullParts()};
    r.reply("Lims_ClientScopes", kScopes);
    r.reply("Lims_ClientPerson", R"({"name":"Anna","roles":[]})");
    r.frame->setCompact(true);
    r.h.settle();
    CHECK(r.prop("TextInput", "shell_search", "visible") != "true");
    CHECK(r.prop("Button", "shell_search_open", "visible") != "false");
    std::string const items = r.h.backend.prop(r.id("Menu", "shell_nav_menu"), "items");
    CHECK(items.starts_with("[Koeln"));
    CHECK(items.contains("Anna"));
    // The scope is chosen from the menu by its path.
    r.h.backend.chooseMenuEntry(r.id("Menu", "shell_nav_menu"), {0, 3});
    r.h.settle();
    auto const screen = r.h.mount(kScopeScreen);
    CHECK(r.h.text("scope") == "o");
    // The search opens as a sheet with its own field, and the same results.
    r.h.backend.click(r.id("Button", "shell_search_open"));
    CHECK(r.prop("Dialog", "shell_search_overlay", "open") == "true");
    r.h.backend.edit(r.id("TextInput", "shell_search_overlay_input"), "q");
    r.h.settle();
    r.h.advance(std::chrono::milliseconds{ui::kSearchPause});
    r.reply("Lims_ClientSearchLab", R"({"hits":[{"screen":"a","title":"A"}]})");
    r.reply("Lims_ClientSearchBilling", R"({"hits":[]})");
    CHECK(r.has("Button", "shell_search_hit_0_0"));
}

TEST_CASE("shell parts: Ctrl K focuses the search field", "[ui][shell][parts]") {
    Rig r{fullParts()};
    r.h.backend.clearLog();
    CHECK(r.h.backend.press(r.id("Button", "nav_lims.home"), "Ctrl+K"));
    CHECK(r.h.backend.focused() == r.id("TextInput", "shell_search"));
}

// Mutation: the loader accepts an app document with `shell` on a screen, or a name beginning `shell` of its own.
TEST_CASE(
    "shell parts: the declaration is the app document's, its names are reserved, and `context` is the one new "
    "env name",
    "[ui][shell][parts]") {
    auto const catalog = ui::testing::corpusCatalog();
    std::string const shell = glz::write_json(fullParts()).value_or("{}");
    auto const onScreen = ui::loadDocumentText(
        R"({"vocab":"ui/1","id":"probe","shell":)" + shell + R"(,"view":{"kind":"text","text":"x"}})",
        ui::LoadOptions{.catalog = catalog});
    REQUIRE_FALSE(onScreen.has_value());
    CHECK(onScreen.error().reason.contains("the app document's"));

    auto const clash = ui::loadDocumentText(
        R"({"vocab":"ui/1","id":"app","state":{"shellSearchText":{"init":""}},"shell":)" + shell + "}",
        ui::LoadOptions{.catalog = catalog});
    REQUIRE_FALSE(clash.has_value());
    CHECK(clash.error().reason.contains("reserved"));

    // Every name the lowering declares is reserved, whatever kind of declaration takes it; others beginning
    // `shell` are the document's own.
    auto const letClash =
        ui::loadDocumentText(R"({"vocab":"ui/1","id":"app","let":{"shellCount0":1},"shell":)" + shell + "}",
                             ui::LoadOptions{.catalog = catalog});
    REQUIRE_FALSE(letClash.has_value());
    CHECK(letClash.error().reason.contains("reserved"));
    auto const mutationClash =
        ui::loadDocumentText(R"({"vocab":"ui/1","id":"app","models":{"m":{"type":"Lims_ClientModel"}},)"
                             R"("mutations":{"shellInbox0":{"model":"m","action":"Shell_Rename"}},"shell":)" +
                                 shell + "}",
                             ui::LoadOptions{.catalog = catalog});
    REQUIRE_FALSE(mutationClash.has_value());
    CHECK(mutationClash.error().reason.contains("reserved"));
    auto const own = ui::loadDocumentText(
        R"({"vocab":"ui/1","id":"app","state":{"shellHelp":{"init":""},"shellCounter":{"init":0}},"shell":)" + shell +
            "}",
        ui::LoadOptions{.catalog = catalog});
    CHECK(own.has_value());

    ui::ShellParts twice = fullParts();
    twice.modules.push_back(twice.modules.front());
    auto const repeated =
        ui::loadDocumentText(R"({"vocab":"ui/1","id":"app","shell":)" + glz::write_json(twice).value_or("{}") + "}",
                             ui::LoadOptions{.catalog = catalog});
    REQUIRE_FALSE(repeated.has_value());
    CHECK(repeated.error().reason.contains("twice"));

    auto const known =
        ui::loadDocumentText(R"({"vocab":"ui/1","id":"s","view":{"kind":"text","text":{"env":"context"}}})",
                             ui::LoadOptions{.catalog = catalog});
    CHECK(known.has_value());
    auto const unknown =
        ui::loadDocumentText(R"({"vocab":"ui/1","id":"s","view":{"kind":"text","text":{"env":"nothing"}}})",
                             ui::LoadOptions{.catalog = catalog});
    REQUIRE_FALSE(unknown.has_value());
    CHECK(unknown.error().reason.contains("does not read this name"));
}

TEST_CASE("shell parts: the registry writes the parts into the app document the bundle carries",
          "[ui][shell][parts]") {
    ui::UiRegistry served{ui::ApplicationInfo{.id = "probe", .version = "1"}};
    REQUIRE(served.setShellParts(fullParts()).has_value());
    auto const built = served.build();
    REQUIRE(built.has_value());
    bool carried = false;
    for (auto const& [hash, file] : (*built)->files) {
        static_cast<void>(hash);
        carried = carried || std::ranges::any_of(file.holders, [](auto const& holder) {
                      return holder.kind == "app" && holder.id == "app";
                  });
        if (std::ranges::any_of(file.holders, [](auto const& holder) { return holder.kind == "app"; })) {
            CHECK(file.bytes.contains(R"("shell":{)"));
            CHECK(file.bytes.contains("Lims_ClientScopes"));
        }
    }
    CHECK(carried);
}

// Mutation: setShellParts keeps the parts it refused; or an app document's own `shell` is overwritten.
TEST_CASE("shell parts: the registry refuses parts that do not load, and an app document that has its own",
          "[ui][shell][parts]") {
    ui::UiRegistry served{ui::ApplicationInfo{.id = "probe", .version = "1"}};
    ui::ShellParts bad = fullParts();
    bad.modules.push_back(bad.modules.front());
    CHECK_FALSE(served.setShellParts(bad).has_value());
    // The refused parts are not applied to the app document added next.
    ui::BundleFile app{.kind = std::string{ui::kAppFile},
                       .id = std::string{ui::kAppFile},
                       .bytes = R"({"vocab":"ui/1","id":"app"})",
                       .isPublic = true};
    CHECK(served.addFile(app).has_value());
    auto const built = served.build();
    REQUIRE(built.has_value());
    for (auto const& [hash, file] : (*built)->files) {
        static_cast<void>(hash);
        CHECK_FALSE(file.bytes.contains("Lims_ClientScopes"));
    }

    ui::UiRegistry other{ui::ApplicationInfo{.id = "probe", .version = "1"}};
    ui::BundleFile own{.kind = std::string{ui::kAppFile},
                       .id = std::string{ui::kAppFile},
                       .bytes = std::string{R"({"vocab":"ui/1","id":"app","shell":)"} +
                                glz::write_json(fullParts()).value_or("{}") + "}",
                       .isPublic = true};
    REQUIRE(other.addFile(own).has_value());
    auto const refused = other.setShellParts(fullParts());
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error().reason.contains("its own `shell`"));
}

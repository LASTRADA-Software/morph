// SPDX-License-Identifier: Apache-2.0

// The permissions corpus of spec 5 §12b and §14: `can` follows the principal's permitted actions and the client's
// host commands, a `denied` reply asks for them again, and customization points read, ignore an outdated choice
// and reset.

#include <catch2/catch_test_macros.hpp>
#include <string>
#include <string_view>

#include "ui_interpreter_support.hpp"

namespace ui = morph::ui;
using ui::testing::Harness;
using ui::testing::readJson;

namespace {

constexpr char const* kVerify = R"({
  "vocab": "ui/1", "id": "verify",
  "models": { "sample": { "type": "Lims_SampleModel" } },
  "queries": { "results": { "model": "sample", "action": "Lims_ListResults" } },
  "mutations": { "verify": { "model": "sample", "action": "Lims_Verify" },
                 "capture": { "model": "sample", "action": "Lims_CaptureConcentration" } },
  "view": { "kind": "column", "children": [
    { "kind": "button", "testId": "verify", "label": "Verify", "visible": { "can": "verify" },
      "onClick": { "run": "verify" } },
    { "kind": "button", "testId": "capture", "label": "Capture", "enabled": { "can": "capture" } },
    { "kind": "text", "testId": "list", "text": { "fn": "format", "args": [ "{0}", { "can": "results" } ] } },
    { "kind": "text", "testId": "open", "text": { "fn": "format", "args": [ "{0}", { "can": "host:openPath" } ] } } ] } })";

}  // namespace

TEST_CASE("interpreter permissions: `can` follows the principal's permitted actions", "[ui][interpreter]") {
    Harness h;
    h.session.setPermitted({"Lims_CaptureConcentration"});
    auto screen = h.mount(kVerify);
    int const verify = h.byTestId("Button", "verify");
    int const capture = h.byTestId("Button", "capture");
    CHECK(h.backend.prop(verify, "visible") == "false");
    CHECK(h.backend.prop(capture, "enabled") != "false");
    CHECK(h.text("list") == "false");
    CHECK(h.text("open") == "false");

    h.session.setPermitted({"Lims_Verify", "Lims_ListResults"});
    // `host:` names come from the client's own commands, not from the server's permitted set.
    h.session.setHostCommands(ui::HostCommands{.available = {"openPath"}, .run = {}});
    h.settle();
    CHECK(h.backend.prop(verify, "visible") == "true");
    CHECK(h.backend.prop(capture, "enabled") == "false");
    CHECK(h.text("list") == "true");
    CHECK(h.text("open") == "true");
}

TEST_CASE("interpreter permissions: a `denied` reply marks the permitted set stale; the server still decides",
          "[ui][interpreter]") {
    Harness h;
    h.session.setPermitted({"Lims_Verify"});
    auto screen = h.mount(kVerify);
    h.backend.click(h.byTestId("Button", "verify"));
    h.settle();
    REQUIRE(h.dispatcher.count("Lims_Verify") == 1);
    h.fail(h.dispatcher.last("Lims_Verify"), "denied");
    CHECK(h.host.stale == 1);
    CHECK(readJson(*screen, R"({"errorKind": "verify"})") == R"("denied")");
    // A query refused the same way does not reach the host: only a mutation's failure is reported to it.
    h.fail(h.dispatcher.last("Lims_ListResults"), "denied");
    CHECK(h.host.stale == 1);
}

TEST_CASE("interpreter permissions: signIn installs the reply, clears the cache, and the secret leaves `result`",
          "[ui][interpreter]") {
    Harness h;
    auto screen = h.mount(R"({
      "vocab": "ui/1", "id": "login",
      "models": { "auth": { "type": "Auth_SessionModel" }, "sample": { "type": "Lims_SampleModel" } },
      "queries": { "results": { "model": "sample", "action": "Lims_ListResults" } },
      "mutations": { "token": { "model": "auth", "action": "Auth_Token",
                                "onSuccess": [ { "signIn": { "from": "token" } } ] } },
      "view": { "kind": "button", "testId": "go", "label": "go", "onClick": { "run": "token" } } })");
    h.reply(0, "[]");
    CHECK(h.session.cacheSize() == 1);
    h.backend.click(h.byTestId("Button", "go"));
    h.settle();
    h.reply(h.dispatcher.last("Auth_Token"), R"({"token":"s3cret","user":"ann"})");
    REQUIRE(h.host.signedIn.size() == 1);
    CHECK(readJson(*screen, R"({"result": "token"})") == R"({"user":"ann"})");
    CHECK(h.host.signedIn.front().field("token") != nullptr);
    CHECK(h.session.cacheSize() == 0);
}

TEST_CASE("interpreter permissions: a secret field is never cached", "[ui][interpreter]") {
    Harness h;
    auto screen = h.mount(R"({
      "vocab": "ui/1", "id": "secret",
      "models": { "auth": { "type": "Auth_SessionModel" } },
      "queries": { "who": { "model": "auth", "action": "Auth_Token" } },
      "view": { "kind": "text", "text": "x" } })");
    h.reply(0, R"({"token":"s3cret","user":"ann"})");
    CHECK(readJson(*screen, R"({"query": "who"})") == R"({"user":"ann"})");
}

namespace {

constexpr char const* kPrefs = R"({
  "vocab": "ui/1", "id": "prefs",
  "customize": {
    "density": { "kind": "choice", "options": [ "comfortable", "compact" ], "default": "comfortable" },
    "filter": { "kind": "value", "type": "string?", "default": null },
    "panels": { "kind": "layout" } },
  "view": { "kind": "column", "children": [
    { "kind": "text", "testId": "density", "text": { "pref": "density" } },
    { "kind": "text", "testId": "filter", "text": { "fn": "coalesce", "args": [ { "pref": "filter" }, "-" ] } },
    { "kind": "customize", "testId": "edit-density", "point": "density" },
    { "kind": "customize", "testId": "edit-filter", "point": "filter" },
    { "kind": "customize", "testId": "edit-panels", "point": "panels" },
    { "kind": "button", "testId": "reset", "label": "reset", "onClick": { "resetPrefs": "all" } },
    { "kind": "button", "testId": "reset-density", "label": "reset", "onClick": { "resetPrefs": "density" } } ] } })";

}  // namespace

TEST_CASE("interpreter permissions: customization points read the user's choice, ignore an outdated one and reset",
          "[ui][interpreter]") {
    Harness h;
    auto screen = h.mount(kPrefs);
    CHECK(h.text("density") == "comfortable");
    CHECK(h.text("filter") == "-");
    CHECK(screen->setPreference("density", ui::Value{"compact"}));
    CHECK_FALSE(screen->setPreference("density", ui::Value{"tiny"}));
    CHECK_FALSE(screen->setPreference("filter", ui::Value{std::int64_t{3}}));
    CHECK_FALSE(screen->setPreference("gone", ui::Value{"x"}));
    CHECK(screen->setPreference("panels", ui::Value{"any"}));
    h.settle();
    CHECK(h.text("density") == "compact");

    h.backend.edit(h.byTestId("TextInput", "edit-filter"), "lead");
    h.settle();
    CHECK(h.text("filter") == "lead");
    CHECK(h.host.log.back() == R"(pref prefs filter "lead")");
    h.backend.choose(h.byTestId("Select", "edit-density"), ui::Key{std::string{"comfortable"}});
    h.settle();
    CHECK(h.text("density") == "comfortable");
    CHECK(h.backend.prop(h.byTestId("Text", "edit-panels"), "text") == "panels");

    screen->setPreference("density", ui::Value{"compact"});
    h.backend.click(h.byTestId("Button", "reset-density"));
    h.settle();
    CHECK(h.text("density") == "comfortable");
    CHECK(h.text("filter") == "lead");
    h.backend.click(h.byTestId("Button", "reset"));
    h.settle();
    CHECK(h.text("filter") == "-");
    CHECK(h.host.log.back() == "resetPrefs prefs all");
}

namespace {

constexpr char const* kArranged = R"({
  "vocab": "ui/1", "id": "overview",
  "customize": { "tiles": { "kind": "arrange", "of": "board" }, "tools": { "kind": "arrange", "of": "strip" } },
  "view": { "kind": "column", "id": "page", "children": [
    { "kind": "column", "id": "board", "testId": "board", "children": [
      { "kind": "text", "id": "sales", "text": "sales" },
      { "kind": "text", "id": "stock", "text": "stock" },
      { "kind": "text", "id": "staff", "text": "staff" } ] },
    { "kind": "row", "id": "strip", "testId": "strip", "children": [
      { "kind": "text", "id": "cut", "text": "cut" },
      { "kind": "text", "id": "copy", "text": "copy" } ] },
    { "kind": "customize", "testId": "edit-tiles", "point": "tiles" },
    { "kind": "customize", "testId": "edit-tools", "point": "tools" } ] } })";

// The texts of a container's children, in its order.
std::string arrangement(Harness const& h, std::string_view kind = "Column", std::string_view testId = "board") {
    std::string out;
    for (int const child : h.backend.children(h.byTestId(kind, testId))) {
        out += (out.empty() ? "" : " ") + h.backend.prop(child, "text");
    }
    return out;
}

ui::Value tile(std::string id, bool hidden = false) {
    return ui::Value{ui::ValueRecord{{"id", ui::Value{std::move(id)}}, {"hidden", ui::Value{hidden}}}};
}

}  // namespace

// Mutations: lower an arranged container as a plain stack (the stored order is never applied), or rebuild it on a
// new choice (a kept child's widget changes).
TEST_CASE("interpreter permissions: an arrange point orders and hides its container's children", "[ui][interpreter]") {
    Harness h;
    auto screen = h.mount(kArranged);
    CHECK(arrangement(h) == "sales stock staff");
    int const sales = h.backend.children(h.byTestId("Column", "board")).front();

    CHECK(screen->setPreference("tiles", ui::Value{ui::ValueList{tile("staff"), tile("stock", true), tile("sales")}}));
    h.settle();
    CHECK(arrangement(h) == "staff sales");
    CHECK(h.backend.children(h.byTestId("Column", "board")).back() == sales);

    // An entry naming no child, a repeated one and a malformed one are skipped; an unnamed child keeps its place
    // after the named ones.
    screen->setPreference(
        "tiles", ui::Value{ui::ValueList{tile("gone"), tile("stock"), tile("stock", true), ui::Value{"sales"}}});
    h.settle();
    CHECK(arrangement(h) == "stock sales staff");

    // `hidden` may be left out, and only `true` hides.
    screen->setPreference("tiles", ui::Value{ui::ValueList{ui::Value{ui::ValueRecord{{"id", ui::Value{"staff"}}}},
                                                           ui::Value{ui::ValueRecord{{"id", ui::Value{"sales"}},
                                                                                     {"hidden", ui::Value{"yes"}}}}}});
    h.settle();
    CHECK(arrangement(h) == "staff sales stock");

    screen->setPreference("tiles", ui::Value{"not a list"});
    h.settle();
    CHECK(arrangement(h) == "sales stock staff");

    // A row is arranged along its own axis.
    CHECK(arrangement(h, "Row", "strip") == "cut copy");
    screen->setPreference("tools", ui::Value{ui::ValueList{tile("copy")}});
    h.settle();
    CHECK(arrangement(h, "Row", "strip") == "copy cut");
}

// Mutations: leave hidden children out of the editor (stock's row disappears once hidden), or swap the wrong way (up
// moves staff down).
TEST_CASE("interpreter permissions: an arrange point's editor moves and hides children and stores every one",
          "[ui][interpreter]") {
    Harness h;
    auto const screen = h.mount(kArranged);
    CHECK(h.backend.prop(h.byTestId("Button", "arrange-up-sales"), "enabled") == "false");
    CHECK(h.backend.prop(h.byTestId("Button", "arrange-down-staff"), "enabled") == "false");

    h.backend.click(h.byTestId("Button", "arrange-up-staff"));
    h.settle();
    CHECK(arrangement(h) == "sales staff stock");
    h.backend.toggle(h.byTestId("Checkbox", "arrange-shown-stock"));
    h.settle();
    CHECK(arrangement(h) == "sales staff");
    // A hidden child keeps its row, so it can be shown again.
    CHECK(h.backend.prop(h.byTestId("Checkbox", "arrange-shown-stock"), "checked") == "false");
    CHECK(
        h.host.log.back() ==
        R"(pref overview tiles [{"hidden":false,"id":"sales"},{"hidden":false,"id":"staff"},{"hidden":true,"id":"stock"}])");

    h.backend.click(h.byTestId("Button", "arrange-down-sales"));
    h.settle();
    CHECK(arrangement(h) == "staff sales");
    h.backend.toggle(h.byTestId("Checkbox", "arrange-shown-stock"));
    h.settle();
    CHECK(arrangement(h) == "staff sales stock");

    // The row's editor finds its container past the column's.
    h.backend.click(h.byTestId("Button", "arrange-up-copy"));
    h.settle();
    CHECK(arrangement(h, "Row", "strip") == "copy cut");
}

// Mutation: have setEnvironmentValue replace the whole record (the backend a switch set is dropped).
TEST_CASE("interpreter permissions: one env value set keeps the others, and a switch keeps the width class",
          "[ui][interpreter]") {
    Harness h;
    auto const screen = h.mount(R"({
      "vocab": "ui/1", "id": "client",
      "view": { "kind": "text", "testId": "t", "text": { "fn": "format", "args": [ "{0} {1}",
        { "fn": "coalesce", "args": [ { "env": "widthClass" }, "-" ] },
        { "fn": "coalesce", "args": [ { "env": "backend" }, "-" ] } ] } } })");
    CHECK(h.text("t") == "- -");
    h.session.setEnvironmentValue("widthClass", ui::Value{"compact"});
    h.settle();
    CHECK(h.text("t") == "compact -");
    h.session.backendChanged("primary");
    h.settle();
    CHECK(h.text("t") == "compact primary");
    h.session.setEnvironmentValue("widthClass", ui::Value{"medium"});
    h.settle();
    CHECK(h.text("t") == "medium primary");
}

TEST_CASE("interpreter permissions: env, session and host values follow the session", "[ui][interpreter]") {
    Harness h;
    h.session.setEnvironment({{"widthClass", ui::Value{"phone"}}});
    h.session.setSessionValues({{"displayName", ui::Value{"Ann"}}});
    auto screen = h.mount(R"({
      "vocab": "ui/1", "id": "client",
      "models": { "auth": { "type": "Auth_SessionModel" } },
      "queries": { "tokens": { "model": "auth", "action": "Auth_Token" } },
      "view": { "kind": "text", "testId": "t", "text": { "fn": "format", "args": [ "{0} {1} {2}",
        { "env": "widthClass" }, { "session": "displayName" }, { "fn": "coalesce", "args": [ { "host": "sync.count" }, "-" ] } ] } } })");
    CHECK(h.text("t") == "phone Ann -");
    h.session.setHostValues({{"sync.count", ui::Value{std::int64_t{4}}}});
    h.session.setEnvironment({{"widthClass", ui::Value{"wide"}}});
    h.settle();
    CHECK(h.text("t") == "wide Ann 4");
    // A secret field is removed from every row of a list reply too.
    h.reply(0, R"([{"token":"a","user":"x"},{"token":"b","user":"y"}])");
    CHECK(readJson(*screen, R"({"query": "tokens"})") == R"([{"user":"x"},{"user":"y"}])");
}

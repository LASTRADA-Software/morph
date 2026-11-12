// SPDX-License-Identifier: Apache-2.0
//
// The client shell installs a bundle's message catalogues and its locale's formatting facts: a screen's `t` and
// `money` follow `setLocale`, the navigator's titles and headings are the catalogue's words in the active locale, and
// a locale with no facts is reported.

// First, ahead of every header that brings view.hpp (GCC's -Wshadow; see test_shell.cpp).
// clang-format off
#include <morph/ui/value.hpp>
// clang-format on

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <expected>
#include <functional>
#include <memory>
#include <morph/client/shell.hpp>
#include <morph/client/source.hpp>
#include <morph/core/backend.hpp>
#include <morph/core/executor.hpp>
#include <morph/core/wire.hpp>
#include <morph/session/session.hpp>
#include <morph/ui/service.hpp>
#include <morph/ui/testing/recording_backend.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <testkit/fake_app_context.hpp>
#include <testkit/screen_harness.hpp>
#include <testkit/wait.hpp>
#include <utility>
#include <vector>

namespace ui = morph::ui;
using morph::examples::testing::FakeAppContext;
using morph::examples::testing::pumpUntil;
using morph::examples::testing::RigMode;
using morph::examples::testing::ScreenHarness;

namespace {

constexpr std::string_view kScreen = R"({
  "vocab": "ui/1", "id": "probe.words",
  "view": { "kind": "column", "children": [
    { "kind": "text", "testId": "hello", "text": { "t": "probe.hello", "fallback": "Fallback" } },
    { "kind": "text", "testId": "eur",
      "text": { "fn": "money", "args": [{ "lit": { "num": 1319640, "den": 100, "dp": 2 } }, "EUR"] } },
    { "kind": "button", "testId": "german", "label": "Deutsch", "onClick": { "setLocale": "de" } },
    { "kind": "button", "testId": "polish", "label": "Polski", "onClick": { "setLocale": "pl" } } ] } })";

constexpr char const* kEn =
    R"({"probe.group.main": "Main work", "probe.hello": "Hello", "probe.words.title": "Words and amounts"})";
constexpr char const* kDe =
    R"({"probe.group.main": "Hauptarbeit", "probe.hello": "Hallo", "probe.words.title": "Wörter und Beträge"})";

/// The one screen, listed for a signed-in person under a group that is a catalogue key, with the given catalogues.
/// @return It, or the refusal.
std::expected<std::shared_ptr<ui::ServedBundle const>, ui::Refusal> wordsBundleWith(char const* en, char const* de) {
    ui::UiRegistry served{ui::ApplicationInfo{.id = "probe", .version = "1"}};
    ui::CatalogEntry entry;
    entry.id = "probe.words";
    entry.title = *ui::parseJson(R"({"t": "probe.words.title", "fallback": "Words"})");
    entry.module = "probe";
    entry.group = "probe.group.main";
    if (auto added = served.addScreen(std::move(entry), std::string{kScreen}); !added) {
        return std::unexpected(std::move(added.error()));
    }
    for (auto const& [id, bytes] : {std::pair{"i18n/en.json", en}, std::pair{"i18n/de.json", de}}) {
        auto added = served.addFile(ui::BundleFile{.kind = "messages", .id = id, .bytes = bytes, .isPublic = false});
        if (!added) {
            return std::unexpected(std::move(added.error()));
        }
    }
    return served.build();
}

std::expected<std::shared_ptr<ui::ServedBundle const>, ui::Refusal> wordsBundle() { return wordsBundleWith(kEn, kDe); }

/// The navigator's title text for @p screenId, or empty.
std::string navTitle(ScreenHarness const& harness, std::string const& screenId) {
    return harness.text("nav_title_" + screenId).value_or("");
}

using Opened = std::expected<ui::Bundle, ui::ConnectError>;

std::string textOf(ui::testing::RecordingBackend const& backend, std::string const& testId) {
    auto const found = backend.find("Text", "testId", testId);
    return found ? backend.prop(*found, "text") : std::string{};
}

/// A source whose server the test redeploys: its exchange answers through whichever service it holds now.
class RedeployedSource final : public morph::client::Source {
public:
    // NOLINTBEGIN(cppcoreguidelines-non-private-member-variables-in-classes): the test redeploys.
    std::shared_ptr<ui::UiService> service;
    // NOLINTEND(cppcoreguidelines-non-private-member-variables-in-classes)

    RedeployedSource() = default;
    ~RedeployedSource() override { close(); }
    RedeployedSource(RedeployedSource const&) = delete;
    RedeployedSource& operator=(RedeployedSource const&) = delete;
    RedeployedSource(RedeployedSource&&) = delete;
    RedeployedSource& operator=(RedeployedSource&&) = delete;

protected:
    [[nodiscard]] std::expected<Transport, ui::ConnectError> connect(ui::AppContext& /*context*/) override {
        _workers = std::make_unique<morph::exec::ThreadPoolExecutor>(1);
        return Transport{
            .backend = std::make_unique<morph::backend::LocalBackend>(*_workers),
            .exchange = [this](morph::wire::Envelope const& request,
                               std::function<void(ui::ExchangeReply)> const& done) {
                done(morph::wire::decode(service->handle(request, *morph::session::allowAllAuthorizer())));
            }};
    }
    void disconnect() override { _workers.reset(); }

private:
    std::unique_ptr<morph::exec::ThreadPoolExecutor> _workers;
};

std::shared_ptr<ui::UiService> serving(char const* en) {
    auto built = wordsBundleWith(en, kDe);
    REQUIRE(built);
    return std::make_shared<ui::UiService>(std::move(*built));
}

/// A public login screen and a private home, each showing the one message.
constexpr std::string_view kLogin = R"({
  "vocab": "ui/1", "id": "probe.login",
  "view": { "kind": "text", "testId": "hello", "text": { "t": "probe.hello", "fallback": "Fallback" } } })";
constexpr std::string_view kPrivateHome = R"({
  "vocab": "ui/1", "id": "probe.home",
  "view": { "kind": "text", "testId": "hello", "text": { "t": "probe.hello", "fallback": "Fallback" } } })";

/// The two screens, with the English catalogue or without it.
std::shared_ptr<ui::UiService> loginAndHome(bool withCatalogue) {
    ui::UiRegistry served{ui::ApplicationInfo{.id = "probe", .version = "1"}};
    for (auto const& [screenId, text] : {std::pair{"probe.login", kLogin}, std::pair{"probe.home", kPrivateHome}}) {
        ui::CatalogEntry entry;
        entry.id = screenId;
        entry.isPublic = screenId == std::string_view{"probe.login"};
        auto added = served.addScreen(std::move(entry), std::string{text});
        REQUIRE(added);
    }
    if (withCatalogue) {
        auto added =
            served.addFile(ui::BundleFile{.kind = "messages", .id = "i18n/en.json", .bytes = kEn, .isPublic = false});
        REQUIRE(added);
    }
    auto built = served.build();
    REQUIRE(built);
    return std::make_shared<ui::UiService>(std::move(*built));
}

}  // namespace

// Mutation: drop installCatalogues from the reload a sign-in or sign-out makes (the login screen keeps reading
// "Fallback").
TEST_CASE("Client shell: the catalogues of the bundle a sign-out reload fetches are installed",
          "[client][shell][locale]") {
    FakeAppContext context;
    ui::testing::RecordingBackend backend;
    RedeployedSource source;
    source.service = loginAndHome(false);
    std::optional<Opened> opened;
    source.open(context, [&opened](Opened result) { opened = std::move(result); });
    REQUIRE(pumpUntil(context.owner(), [&opened] { return opened.has_value(); }));
    {
        auto const shell = morph::client::mountShell(context, backend, std::move(*opened), source,
                                                     morph::client::ShellOptions{.startScreen = "probe.home"});
        REQUIRE(pumpUntil(context.owner(), [&backend] { return textOf(backend, "hello") == "Fallback"; }));
        // The server now serves the catalogue; the reload the sign-out makes fetches it.
        source.service = loginAndHome(true);
        auto const signOut = backend.find("Button", "testId", "shell_signOut");
        REQUIRE(signOut.has_value());
        backend.click(*signOut);
        CHECK(pumpUntil(context.owner(), [&backend] { return textOf(backend, "hello") == "Hello"; }));
    }
    source.close();
}

// Mutations: drop the shell's setTranslations call (hello reads "Fallback"); skip the table switch in
// Session::setLocale (eur reads "13196.40 EUR"); build the navigator's titles with catalogTitles instead of
// catalogTitlesIn (the English title reads "Wörter und Beträge", de sorting first); give the navigator no label
// (the heading reads "probe.group.main"); leave relabel out of the shell's locale sink (the German heading reads
// "Main work").
TEST_CASE("Client shell: a bundle's catalogues and the locale's formats reach the screen and the navigator",
          "[client][shell][locale]") {
    ScreenHarness harness{morph::client::LocalApp{.name = "probe", .bundle = &wordsBundle}, "probe.words",
                          RigMode::Local};
    CHECK(harness.waitForText("hello", "Hello"));
    CHECK(harness.waitForText("eur", "€13,196.40"));
    CHECK(navTitle(harness, "probe.words") == "Words and amounts");
    CHECK(harness.text("navigator_group_probe/probe.group.main") == "Main work");

    harness.click("german");
    CHECK(harness.waitForText("hello", "Hallo"));
    CHECK(harness.waitForText("eur", "13.196,40 €"));
    CHECK(harness.waitFor([&harness] { return navTitle(harness, "probe.words") == "Wörter und Beträge"; }));
    CHECK(harness.text("navigator_group_probe/probe.group.main") == "Hauptarbeit");
    CHECK(harness.problems().empty());
}

// Mutation: drop the shell's report of a locale with no facts (problems stays empty); report it on every change
// instead of once (the count below is 2).
TEST_CASE("Client shell: a locale with no formatting facts is reported once, and written as en's",
          "[client][shell][locale]") {
    ScreenHarness harness{morph::client::LocalApp{.name = "probe", .bundle = &wordsBundle}, "probe.words",
                          RigMode::Local};
    harness.click("polish");
    CHECK(harness.waitFor([&harness] {
        return std::ranges::any_of(harness.problems(), [](std::string const& line) {
            return line.contains("no formatting facts for locale `pl`");
        });
    }));
    // No `pl` catalogue: the `t` shows the fallback; the heading is en's words, not the key; the amount is en's.
    CHECK(harness.waitForText("hello", "Fallback"));
    CHECK(harness.waitForText("eur", "€13,196.40"));
    CHECK(harness.text("navigator_group_probe/probe.group.main") == "Main work");
    harness.click("polish");
    auto const count = std::ranges::count_if(harness.problems(), [](std::string const& line) {
        return line.contains("no formatting facts for locale `pl`");
    });
    CHECK(count == 1);
}

// Mutation: start the session in en whatever the option; never call onLocale.
TEST_CASE("Client shell: ShellOptions::locale starts the shell in that locale, and onLocale hears a change",
          "[client][shell][locale]") {
    FakeAppContext context;
    ui::testing::RecordingBackend backend;
    morph::client::LocalSource source{morph::client::LocalApp{.name = "probe", .bundle = &wordsBundle},
                                      morph::client::LocalSetup{}};
    std::optional<Opened> opened;
    source.open(context, [&opened](Opened result) { opened = std::move(result); });
    REQUIRE(pumpUntil(context.owner(), [&opened] { return opened.has_value(); }));
    std::vector<std::string> heard;
    {
        auto const shell = morph::client::mountShell(
            context, backend, std::move(*opened), source,
            morph::client::ShellOptions{.startScreen = "probe.words",
                                        .locale = "de",
                                        .onLocale = [&heard](std::string const& locale) { heard.push_back(locale); }});
        REQUIRE(pumpUntil(context.owner(), [&backend] { return textOf(backend, "hello") == "Hallo"; }));
        CHECK(textOf(backend, "eur") == "13.196,40 €");
        CHECK(heard.empty());
        auto const german = backend.find("Button", "testId", "german");
        REQUIRE(german.has_value());
        backend.click(*german);
        REQUIRE(pumpUntil(context.owner(), [&heard] { return !heard.empty(); }));
        CHECK(heard == std::vector<std::string>{"de"});
    }
    source.close();
}

// Mutation: leave the catalogue reinstall out of reloadChanged, or return before it when no document changed (the
// text keeps reading "Hello").
TEST_CASE("Client shell: a development reload that changed only a catalogue re-resolves every t",
          "[client][shell][locale][dev-reload]") {
    FakeAppContext context;
    ui::testing::RecordingBackend backend;
    RedeployedSource source;
    source.service = serving(kEn);
    std::optional<Opened> opened;
    source.open(context, [&opened](Opened result) { opened = std::move(result); });
    REQUIRE(pumpUntil(context.owner(), [&opened] { return opened.has_value(); }));
    {
        auto const shell =
            morph::client::mountShell(context, backend, std::move(*opened), source,
                                      morph::client::ShellOptions{.startScreen = "probe.words", .dev = true});
        REQUIRE(pumpUntil(context.owner(), [&backend] { return textOf(backend, "hello") == "Hello"; }));
        source.service = serving(
            R"({"probe.group.main": "Main work", "probe.hello": "Hi there", "probe.words.title": "Words and amounts"})");
        for (int second = 0; second < 3; ++second) {
            context.clock().advance(std::chrono::seconds{1});
            static_cast<void>(pumpUntil(
                context.owner(), [&backend] { return textOf(backend, "hello") == "Hi there"; },
                std::chrono::milliseconds{50}));
        }
        CHECK(textOf(backend, "hello") == "Hi there");
    }
    source.close();
}

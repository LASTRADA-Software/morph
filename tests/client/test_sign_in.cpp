// SPDX-License-Identifier: Apache-2.0
//
// The client's sign-in mapping and its application host: a reply of the application's own shape signs in through a
// mapper, a mapper answering no principal is refused and reported, and the host a shell of the application's own
// builds runs a document's sign-in and inline upload.

// First, ahead of every header that brings view.hpp: GCC's -Wshadow reports value.hpp's enumerator `Type::Key` when
// view.hpp's `ui::Key` was declared before it.
// clang-format off
#include <morph/ui/value.hpp>
// clang-format on

#include <catch2/catch_test_macros.hpp>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <morph/client/shell.hpp>
#include <morph/client/source.hpp>
#include <morph/ui/document.hpp>
#include <morph/ui/host.hpp>
#include <morph/ui/interpreter.hpp>
#include <morph/ui/shell_frame.hpp>
#include <morph/ui/testing/recording_backend.hpp>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <testkit/fake_app_context.hpp>
#include <testkit/wait.hpp>
#include <utility>
#include <vector>

#include "probe_app.hpp"

namespace client = morph::client;
namespace ui = morph::ui;
using morph::examples::testing::FakeAppContext;
using morph::examples::testing::pumpUntil;

namespace {

/// Reads `{user, jwt}`: the shape `Probe_Login` answers.
std::optional<client::SignedIn> userAndJwt(ui::Value const& reply) {
    ui::Value const* const user = reply.field("user");
    ui::Value const* const jwt = reply.field("jwt");
    if (user == nullptr || user->asString() == nullptr) {
        return std::nullopt;
    }
    return client::SignedIn{.principal = *user->asString(),
                            .token = jwt != nullptr && jwt->asString() != nullptr ? *jwt->asString() : std::string{}};
}

/// A screen that signs in through `Probe_Login` and attaches a file inline through `Probe_Attach`. The path goes in
/// with forward slashes: a Windows path's backslashes would be read as JSON escapes.
std::string loginDocument(std::filesystem::path const& picked) {
    return R"({"vocab":"ui/1","id":"probe.login","models":{"probe":{"type":"Probe_Model"}},)"
           R"("state":{"picked":{"init":")" +
           picked.generic_string() +
           R"("}},)"
           R"("mutations":{"login":{"model":"probe","action":"Probe_Login","onSuccess":[{"signIn":{"from":"login"}}]},)"
           R"("attach":{"model":"probe","action":"Probe_Attach"}},)"
           R"("view":{"kind":"column","children":[)"
           R"({"kind":"button","testId":"login","label":"Log in","onClick":[{"run":"login"}]},)"
           R"({"kind":"button","testId":"attach","label":"Attach","onClick":[)"
           R"({"upload":{"ref":"picked"},"to":"attach","into":"bytes","as":"content"}]},)"
           R"({"kind":"text","testId":"sent","text":{"fn":"format","args":["{0}",{"result":"attach"}]}}]}})";
}

/// A document host of the test's own: navigation goes nowhere.
class Host final : public ui::DocumentScreenHost {
public:
    using DocumentScreenHost::DocumentScreenHost;
    using DocumentScreenHost::onNavigate;
    void onNavigate(std::string_view screenId, ui::ParamMap const& params) override {
        static_cast<void>(screenId);
        static_cast<void>(params);
    }
};

/// The open source, a session over it and a hand-built document host with or without the application host.
struct HandBuilt {
    explicit HandBuilt(bool withApplication) {
        std::optional<std::expected<ui::Bundle, ui::ConnectError>> opened;
        source.open(context,
                    [&opened](std::expected<ui::Bundle, ui::ConnectError> result) { opened = std::move(result); });
        REQUIRE(pumpUntil(context.owner(), [&opened] { return opened.has_value(); }));
        REQUIRE(opened->has_value());
        session = std::make_unique<ui::Session>(context.runtime(), ui::SessionOptions{.catalog = source.catalog()});
        application = std::make_unique<client::ApplicationHost>(
            source, &userAndJwt, std::filesystem::temp_directory_path(),
            [this](client::SignedIn made) { signedIn.push_back(std::move(made)); }, [] {},
            [this](std::string const& text) { problems.push_back(text); });
        std::ofstream{picked} << "a,b";
        auto loaded = ui::loadDocumentText(loginDocument(picked), ui::LoadOptions{.catalog = source.catalog()});
        REQUIRE(loaded.has_value());
        document = std::make_shared<ui::Document const>(std::move(*loaded));
        ui::DocumentScreenHostOptions options;
        options.documents = [this](std::string_view screenId) -> std::shared_ptr<ui::Document const> {
            return screenId == "probe.login" ? document : nullptr;
        };
        options.backend = &backend;
        options.application = withApplication ? application.get() : nullptr;
        options.diagnostics = [this](ui::Refusal const& refusal) { problems.push_back(refusal.reason); };
        host = std::make_unique<Host>(*session, source.dispatcher(), std::move(options));
    }
    ~HandBuilt() {
        screen.reset();
        host.reset();
        session.reset();
        source.close();
        std::filesystem::remove(picked);
    }
    HandBuilt(HandBuilt const&) = delete;
    HandBuilt& operator=(HandBuilt const&) = delete;
    HandBuilt(HandBuilt&&) = delete;
    HandBuilt& operator=(HandBuilt&&) = delete;

    // Clicks a button, and pumps until @p settled holds.
    void click(std::string const& testId, std::function<bool()> const& settled) {
        backend.click(*backend.find("Button", "testId", testId));
        CHECK(pumpUntil(context.owner(), settled));
    }

    // What the attachment's reply says: how many base64 characters arrived.
    [[nodiscard]] std::string sent() const { return backend.prop(*backend.find("Text", "testId", "sent"), "text"); }

    FakeAppContext context;
    ui::testing::RecordingBackend backend;
    client::LocalSource source{probe::app(), client::LocalSetup{}};
    std::filesystem::path picked = std::filesystem::temp_directory_path() / "morph_client_sign_in_test.csv";
    std::unique_ptr<ui::Session> session;
    std::vector<client::SignedIn> signedIn;
    std::vector<std::string> problems;
    std::unique_ptr<client::ApplicationHost> application;
    std::shared_ptr<ui::Document const> document;
    std::unique_ptr<Host> host;
    std::unique_ptr<ui::ScreenHandle> screen;
};

}  // namespace

TEST_CASE("defaultSignIn reads {ok, principal, token}, and refuses ok false or no principal", "[client][sign-in]") {
    auto const signedIn = client::defaultSignIn(ui::Value{
        ui::ValueRecord{{"ok", ui::Value{true}}, {"principal", ui::Value{"ada"}}, {"token", ui::Value{"t"}}}});
    REQUIRE(signedIn.has_value());
    CHECK(signedIn->principal == "ada");
    CHECK(signedIn->token == "t");
    CHECK_FALSE(
        client::defaultSignIn(ui::Value{ui::ValueRecord{{"ok", ui::Value{false}}, {"principal", ui::Value{"ada"}}}})
            .has_value());
    CHECK_FALSE(client::defaultSignIn(ui::Value{ui::ValueRecord{{"token", ui::Value{"t"}}}}).has_value());
}

// Mutation: let the host sign in whatever the mapper answers (the empty principal signs in).
TEST_CASE("ApplicationHost: a mapper of the application's own signs in; one answering no principal is reported",
          "[client][sign-in]") {
    FakeAppContext context;
    client::LocalSource source{probe::app(), client::LocalSetup{}};
    std::vector<client::SignedIn> signedIn;
    std::vector<std::string> problems;
    auto const record = [&signedIn](client::SignedIn made) { signedIn.push_back(std::move(made)); };
    auto const report = [&problems](std::string const& text) { problems.push_back(text); };
    client::ApplicationHost host{source, &userAndJwt, {}, record, [] {}, report};
    CHECK(host.signIn(ui::Value{ui::ValueRecord{{"user", ui::Value{"ada"}}, {"jwt", ui::Value{"t-1"}}}}));
    REQUIRE(signedIn.size() == 1);
    CHECK(signedIn.front().principal == "ada");
    CHECK(signedIn.front().token == "t-1");
    CHECK_FALSE(host.signIn(ui::Value{ui::ValueRecord{{"principal", ui::Value{"ada"}}}}));

    client::ApplicationHost anonymous{
        source, [](ui::Value const&) { return std::optional<client::SignedIn>{client::SignedIn{}}; },
        {},     record,
        [] {},  report};
    CHECK_FALSE(anonymous.signIn(ui::Value{}));
    CHECK(signedIn.size() == 1);
    CHECK(problems == std::vector<std::string>{"sign-in mapper answered no principal"});

    CHECK_THROWS_AS(client::ApplicationHost(source, client::SignInMapper{}, {}, record, [] {}), std::invalid_argument);
    ui::testing::RecordingBackend backend;
    CHECK_THROWS_AS(client::mountShell(context, backend, std::unexpected(ui::ConnectError{}), source,
                                       client::ShellOptions{.signIn = {}}),
                    std::invalid_argument);
}

// Mutation: have notify call the sink's setVisible(true) as a separate step after writing text/tone, instead of
// show()'s single call — the replace-while-showing assertion below then sees a no-op on the second notify.
TEST_CASE("ApplicationHost: notify answers false unwired, and wired shows and replaces through the frame's toast",
          "[client][sign-in]") {
    client::LocalSource source{probe::app(), client::LocalSetup{}};
    std::vector<client::SignedIn> signedIn;
    auto const record = [&signedIn](client::SignedIn made) { signedIn.push_back(std::move(made)); };
    client::ApplicationHost host{source, &userAndJwt, {}, record, [] {}};
    // Unwired: the base class's safe default, never a crash.
    CHECK_FALSE(host.notify("hello", "info"));

    ui::testing::RecordingBackend backend;
    ui::ShellFrameOptions options;
    options.applicationName = "Probe";
    ui::ShellFrame frame{backend, nullptr, std::move(options)};
    host.setNotifySink([&frame](std::string const& text, std::string const& role) {
        frame.showToast(text, role == "error" ? ui::Tone::Err : ui::Tone::Info);
        return true;
    });
    CHECK(host.notify("Saved", "info"));
    int const toast = backend.all("Toast").front();
    CHECK(backend.prop(toast, "text") == "Saved");
    CHECK(backend.prop(toast, "shown") == "1");
    // A second notify while the first toast is still showing replaces its text, not a no-op.
    CHECK(host.notify("Failed", "error"));
    CHECK(backend.all("Toast").size() == 1);
    CHECK(backend.prop(toast, "text") == "Failed");
    CHECK(backend.prop(toast, "tone") == "Err");
    CHECK(backend.prop(toast, "shown") == "2");
}

// Control: the same host without the application host reports both commands as not supported.
TEST_CASE("ApplicationHost: a hand-built document host runs a document's sign-in and inline upload through it",
          "[client][sign-in]") {
    SECTION("with the application host") {
        HandBuilt built{true};
        auto mounted = built.host->mount("probe.login", {});
        REQUIRE(mounted.has_value());
        built.screen = std::move(*mounted);
        built.click("login", [&built] { return !built.signedIn.empty() || !built.problems.empty(); });
        REQUIRE(built.signedIn.size() == 1);
        CHECK(built.signedIn.front().principal == "ada");
        CHECK(built.signedIn.front().token == "t-1");
        built.click("attach", [&built] { return built.sent() == "4" || !built.problems.empty(); });
        CHECK(built.sent() == "4");  // "a,b" is "YSxi"
        CHECK(built.problems.empty());
    }
    SECTION("without it") {
        HandBuilt built{false};
        auto mounted = built.host->mount("probe.login", {});
        REQUIRE(mounted.has_value());
        built.screen = std::move(*mounted);
        built.click("login", [&built] { return !built.problems.empty(); });
        built.click("attach", [&built] { return built.problems.size() >= 2; });
        CHECK(built.signedIn.empty());
        REQUIRE(built.problems.size() == 2);
        CHECK(built.problems.at(0).contains("not supported"));
        CHECK(built.problems.at(1) == "`upload` is not supported by the host");
    }
}

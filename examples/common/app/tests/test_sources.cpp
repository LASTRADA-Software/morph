// SPDX-License-Identifier: Apache-2.0

// First, ahead of every header that brings view.hpp: GCC's -Wshadow reports value.hpp's enumerator `Type::Key` when
// view.hpp's `ui::Key` was declared before it.
// clang-format off
#include <morph/ui/value.hpp>
// clang-format on

#include <app/app_source.hpp>
#include <app/client_shell.hpp>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <expected>
#include <functional>
#include <memory>
#include <morph/core/backend.hpp>
#include <morph/core/executor.hpp>
#include <morph/ui/frontend.hpp>
#include <morph/ui/testing/recording_backend.hpp>
#include <optional>
#include <string>
#include <testkit/fake_app_context.hpp>
#include <testkit/wait.hpp>
#include <utility>
#include <vector>

#include "probe_app.hpp"

using morph::examples::ClientShellOptions;
using morph::examples::LocalAppSource;
using morph::examples::LocalSetup;
using morph::examples::testing::FakeAppContext;
using morph::examples::testing::pumpUntil;
using Opened = std::expected<morph::ui::Bundle, morph::ui::ConnectError>;

namespace {

/// What a shell reported.
struct ShellLog {
    std::vector<std::string> problems;
    std::vector<std::string> mounted;

    ClientShellOptions options(std::string start = {}) {
        return ClientShellOptions{.startScreen = std::move(start),
                                  .onProblem = [this](std::string const& problem) { problems.push_back(problem); },
                                  .onMounted = [this](std::string const& screenId) { mounted.push_back(screenId); }};
    }
};

/// The text a widget with @p testId shows, or nothing when there is none.
std::optional<std::string> textOf(morph::ui::testing::RecordingBackend const& backend, std::string const& testId) {
    auto const found = backend.find("Text", "testId", testId);
    if (!found) {
        return std::nullopt;
    }
    return backend.prop(*found, "text");
}

/// Opens @p source on @p context and waits for its answer.
Opened openAndWait(FakeAppContext& context, morph::ui::AppSource& source) {
    std::optional<Opened> opened;
    source.open(context, [&opened](Opened result) { opened = std::move(result); });
    REQUIRE(pumpUntil(context.owner(), [&opened] { return opened.has_value(); }));
    return std::move(opened).value();
}

}  // namespace

TEST_CASE("LocalAppSource: opens the linked application's bundle, verified, through the UI service in process",
          "[examples-common][source]") {
    FakeAppContext context;
    LocalAppSource source{probe::app(), LocalSetup{}};
    Opened const opened = openAndWait(context, source);
    REQUIRE(opened.has_value());
    CHECK(opened->applicationId == "probe");
    CHECK(opened->documents.contains("probe.home"));
    CHECK(opened->documents.contains("probe.other"));
    CHECK(source.screens() == std::vector<std::string>{"probe.home", "probe.other"});
    source.close();
}

// Mutation: mount the bundle's last screen instead of the first — the answer text is missing.
TEST_CASE("Client shell: mounts the start screen, and the query's answer crosses the bridge",
          "[examples-common][shell]") {
    FakeAppContext context;
    morph::ui::testing::RecordingBackend backend;
    LocalAppSource source{probe::app(), LocalSetup{}};
    ShellLog log;
    {
        auto shell =
            morph::examples::mountClientShell(context, backend, openAndWait(context, source), source, log.options());
        CHECK(log.mounted == std::vector<std::string>{"probe.home"});
        CHECK(pumpUntil(context.owner(), [&] { return textOf(backend, "answer") == "answer 42"; }));
        CHECK(log.problems.empty());
    }
    source.close();
}

// Mutation: mount the navigated screen from inside the click handler — the old screen is destroyed under its own
// handler (ASan) or the mount order changes.
TEST_CASE("Client shell: a document's navigate replaces the screen, after the handler returns",
          "[examples-common][shell]") {
    FakeAppContext context;
    morph::ui::testing::RecordingBackend backend;
    LocalAppSource source{probe::app(), LocalSetup{}};
    ShellLog log;
    {
        auto shell =
            morph::examples::mountClientShell(context, backend, openAndWait(context, source), source, log.options());
        auto const next = backend.find("Button", "testId", "next");
        REQUIRE(next.has_value());
        backend.click(next.value());
        CHECK(log.mounted == std::vector<std::string>{"probe.home"});  // posted, not yet run
        CHECK(pumpUntil(context.owner(), [&] { return textOf(backend, "other").has_value(); }));
        CHECK_FALSE(textOf(backend, "answer").has_value());
        CHECK(log.mounted == std::vector<std::string>{"probe.home", "probe.other"});
    }
    source.close();
}

TEST_CASE("Client shell: --screen picks the start screen; an unknown one is shown as a problem",
          "[examples-common][shell]") {
    FakeAppContext context;
    morph::ui::testing::RecordingBackend backend;
    LocalAppSource source{probe::app(), LocalSetup{}};
    ShellLog log;
    {
        auto shell = morph::examples::mountClientShell(context, backend, openAndWait(context, source), source,
                                                       log.options("probe.other"));
        CHECK(log.mounted == std::vector<std::string>{"probe.other"});
    }
    ShellLog missing;
    {
        auto shell = morph::examples::mountClientShell(context, backend, openAndWait(context, source), source,
                                                       missing.options("probe.none"));
        CHECK(missing.mounted.empty());
        REQUIRE(missing.problems.size() == 1);
        CHECK(missing.problems.front() == "Cannot open probe.none: unknown screen");
        CHECK(backend.find("Text", "text", "Cannot open probe.none: unknown screen").has_value());
    }
    source.close();
}

TEST_CASE("Client shell: a source that did not open is shown, not mounted", "[examples-common][shell]") {
    FakeAppContext context;
    morph::ui::testing::RecordingBackend backend;
    LocalAppSource source{probe::app(), LocalSetup{}};
    ShellLog log;
    auto shell =
        morph::examples::mountClientShell(context, backend,
                                          std::unexpected(morph::ui::ConnectError{
                                              .kind = morph::ui::ConnectError::Kind::Unreachable, .message = "down"}),
                                          source, log.options());
    CHECK(log.mounted.empty());
    CHECK(log.problems == std::vector<std::string>{"Cannot open the application: down"});
    CHECK(backend.find("Text", "text", "Cannot open the application: down").has_value());
}

namespace {

/// A source whose exchange the test answers by hand.
class ManualSource final : public morph::examples::ClientSource {
public:
    // NOLINTBEGIN(cppcoreguidelines-non-private-member-variables-in-classes): the test answers and inspects these.
    std::vector<std::function<void(morph::ui::ExchangeReply)>> waiting;
    int sent = 0;
    std::chrono::milliseconds timeout{0};
    // NOLINTEND(cppcoreguidelines-non-private-member-variables-in-classes)

protected:
    [[nodiscard]] std::expected<Transport, morph::ui::ConnectError> connect(
        morph::ui::AppContext& /*context*/) override {
        _workers = std::make_unique<morph::exec::ThreadPoolExecutor>(1);
        return Transport{.backend = std::make_unique<morph::backend::LocalBackend>(*_workers),
                         .exchange = [this](morph::wire::Envelope const& /*request*/,
                                            std::function<void(morph::ui::ExchangeReply)> done) {
                             ++sent;
                             waiting.push_back(std::move(done));
                         }};
    }
    [[nodiscard]] std::chrono::milliseconds openTimeout() const override { return timeout; }
    void disconnect() override { _workers.reset(); }

private:
    std::unique_ptr<morph::exec::ThreadPoolExecutor> _workers;

public:
    ~ManualSource() override { close(); }
    ManualSource() = default;
    ManualSource(ManualSource const&) = delete;
    ManualSource& operator=(ManualSource const&) = delete;
    ManualSource(ManualSource&&) = delete;
    ManualSource& operator=(ManualSource&&) = delete;
};

}  // namespace

// Mutation: drop the liveness check in the guarded exchange — the closed source sends a second request.
TEST_CASE("ClientSource: once closed, it delivers nothing and sends nothing more", "[examples-common][source]") {
    FakeAppContext context;
    ManualSource source;
    int readies = 0;
    source.open(context, [&readies](Opened /*result*/) { ++readies; });
    REQUIRE(source.sent == 1);  // the ui-hello
    source.close();
    // The hello's reply arrives after the close: the client would fetch the manifest next.
    morph::wire::Envelope reply;
    reply.kind = "ok";
    reply.body = R"({"application":"probe","version":"1","vocabularies":["ui/1","expr/1"],"digest":"sha256:)" +
                 std::string(64, '0') + R"("})";
    source.waiting.front()(reply);
    CHECK(source.sent == 1);
    CHECK(readies == 0);
}

// Mutation: leave the timeout unarmed — the source never answers.
TEST_CASE("ClientSource: a source that does not answer in time is reported unreachable, once",
          "[examples-common][source]") {
    FakeAppContext context;
    ManualSource source;
    source.timeout = std::chrono::milliseconds{100};
    std::vector<Opened> answers;
    source.open(context, [&answers](Opened result) { answers.push_back(std::move(result)); });
    context.clock().advance(std::chrono::milliseconds{99});
    static_cast<void>(pumpUntil(context.owner(), [] { return false; }, std::chrono::milliseconds{20}));
    CHECK(answers.empty());
    context.clock().advance(std::chrono::milliseconds{1});
    REQUIRE(pumpUntil(context.owner(), [&answers] { return !answers.empty(); }));
    REQUIRE_FALSE(answers.front().has_value());
    CHECK(answers.front().error().kind == morph::ui::ConnectError::Kind::Unreachable);
    // A reply after the timeout delivers nothing more.
    source.waiting.front()(std::unexpected(std::string{"late"}));
    CHECK(answers.size() == 1);
}

TEST_CASE("ClientSource: switching backends is refused", "[examples-common][source]") {
    ManualSource source;
    std::optional<std::string> refusal;
    source.switchBackend(morph::ui::Backend{.name = "local"}, [&refusal](std::expected<void, std::string> done) {
        if (!done) {
            refusal = done.error();
        }
    });
    REQUIRE(refusal.has_value());
    CHECK(refusal.value() == "this client does not switch backends; the switch to `local` is the hosting layer's");
}

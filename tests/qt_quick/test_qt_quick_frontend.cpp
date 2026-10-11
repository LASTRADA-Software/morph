// SPDX-License-Identifier: Apache-2.0
//
// qt_quick::Frontend: run, quit and the teardown order (docs/spec/qt_quick/frontend.md). This executable owns no
// QGuiApplication: each run makes its own, as a client's main does.

#include <QGuiApplication>
#include <QQuickWindow>
#include <QWindow>
#include <array>
#include <catch2/catch_session.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <expected>
#include <functional>
#include <memory>
#include <morph/qt_quick/frontend.hpp>
#include <morph/reactive/runtime.hpp>
#include <morph/ui/frontend.hpp>
#include <morph/ui/mount.hpp>
#include <morph/ui/view.hpp>
#include <optional>
#include <stdexcept>
#include <string>
#include <testkit/log_level.hpp>
#include <utility>
#include <vector>

namespace ui = morph::ui;
namespace qt_quick = morph::qt_quick;

namespace {

/// A source that delivers a bundle when it is opened, and records what it was asked.
class RecordingSource final : public ui::AppSource {
public:
    explicit RecordingSource(std::vector<std::string>& log) : _log{&log} {}

    void open(ui::AppContext& context,
              std::function<void(std::expected<ui::Bundle, ui::ConnectError>)> ready) override {
        _log->emplace_back(context.ioLoop() == nullptr ? "open without an io loop" : "open with an io loop");
        ready(ui::Bundle{.applicationId = "app", .manifestDigest = {}, .documents = {}});
    }
    void switchBackend(ui::Backend backend, std::function<void(std::expected<void, std::string>)> done) override {
        static_cast<void>(backend);
        done({});
    }
    void close() override { _log->emplace_back("close"); }

private:
    std::vector<std::string>* _log;
};

/// A shell that shows one text, records its unmounting, and runs what the test asks once mounted.
class Shell final : public ui::MountedShell {
public:
    Shell(ui::AppContext& context, ui::IViewBackend& backend, std::vector<std::string>& log)
        : _log{&log}, _view{context.runtime(), backend, ui::text({.text = "hello"})} {}
    ~Shell() override { _log->emplace_back("shell gone"); }
    Shell(Shell const&) = delete;
    Shell& operator=(Shell const&) = delete;
    Shell(Shell&&) = delete;
    Shell& operator=(Shell&&) = delete;

private:
    std::vector<std::string>* _log;
    ui::Mounted _view;
};

// The command line every run is given: QGuiApplication keeps a reference to the count, so both live as long as the
// process.
int& argCount() {
    static int count = 1;
    return count;
}

char** argValues() {
    static std::array<char, 32> name{"morph_qt_quick_frontend_tests"};
    static std::array<char*, 2> values{name.data(), nullptr};
    return values.data();
}

qt_quick::FrontendConfig configWith(std::vector<std::string>& log, std::function<void(ui::AppContext&)> then) {
    return qt_quick::FrontendConfig{
        .mountShell = [&log, then = std::move(then)](
                          ui::AppContext& context, ui::IViewBackend& backend,
                          std::expected<ui::Bundle, ui::ConnectError> opened) -> std::unique_ptr<ui::MountedShell> {
            log.emplace_back(opened ? "mount " + opened->applicationId : "mount failed");
            auto shell = std::make_unique<Shell>(context, backend, log);
            then(context);
            return shell;
        },
        .components = nullptr,
        .title = "test",
        .onWidthClass = {}};
}

}  // namespace

TEST_CASE("qt_quick frontend: run opens the source, mounts the shell, and tears down shell before source",
          "[qt_quick][frontend]") {
    std::vector<std::string> log;
    RecordingSource source{log};
    qt_quick::Frontend frontend{argCount(), argValues(), configWith(log, [](ui::AppContext& context) {
                                    context.executor().post([&context] { context.quit(3); });
                                })};
    CHECK(frontend.name() == "qt");
    CHECK(frontend.run(source) == 3);
    CHECK(log == std::vector<std::string>{"open without an io loop", "mount app", "shell gone", "close"});
    CHECK(QGuiApplication::instance() == nullptr);
}

TEST_CASE("qt_quick frontend: a quit before the loop runs still ends the run with its code", "[qt_quick][frontend]") {
    std::vector<std::string> log;
    RecordingSource source{log};
    qt_quick::Frontend frontend{argCount(), argValues(),
                                configWith(log, [](ui::AppContext& context) { context.quit(5); })};
    CHECK(frontend.run(source) == 5);
    CHECK(log.back() == "close");
}

TEST_CASE("qt_quick frontend: closing the window ends the run with 0", "[qt_quick][frontend]") {
    std::vector<std::string> log;
    RecordingSource source{log};
    qt_quick::Frontend frontend{argCount(), argValues(), configWith(log, [](ui::AppContext& context) {
                                    context.executor().post([] {
                                        for (QWindow* const window : QGuiApplication::topLevelWindows()) {
                                            if (qobject_cast<QQuickWindow*>(window) != nullptr) {
                                                window->close();
                                            }
                                        }
                                    });
                                })};
    CHECK(frontend.run(source) == 0);
    CHECK(log.back() == "close");
}

TEST_CASE("qt_quick frontend: a shell factory that throws ends the run after the teardown", "[qt_quick][frontend]") {
    std::vector<std::string> log;
    RecordingSource source{log};
    qt_quick::FrontendConfig config{
        .mountShell = [](ui::AppContext&, ui::IViewBackend&, std::expected<ui::Bundle, ui::ConnectError>)
            -> std::unique_ptr<ui::MountedShell> { throw std::runtime_error{"no shell"}; },
        .components = nullptr,
        .title = "test",
        .onWidthClass = {}};
    qt_quick::Frontend frontend{argCount(), argValues(), std::move(config)};
    CHECK_THROWS_WITH(frontend.run(source), "no shell");
    CHECK(log.back() == "close");
    qt_quick::Frontend empty{argCount(), argValues(), qt_quick::FrontendConfig{}};
    CHECK_THROWS_AS(empty.run(source), std::invalid_argument);
}

TEST_CASE("qt_quick frontend: the option is named qt and is usable where a window can be shown",
          "[qt_quick][frontend]") {
    ui::FrontendOption const option = qt_quick::frontendOption(argCount(), argValues(), {});
    CHECK(option.name == "qt");
    REQUIRE(option.make);
    std::unique_ptr<ui::Frontend> const made = option.make();
    REQUIRE(made != nullptr);
    CHECK(made->name() == "qt");
    auto const env = [](std::optional<std::string> display) {
        return [display = std::move(display)](std::string_view name) -> std::optional<std::string> {
            return name == "DISPLAY" ? display : std::nullopt;
        };
    };
#if defined(__APPLE__) || defined(_WIN32)
    CHECK(qt_quick::usableHere(env(std::nullopt)));
#else
    CHECK_FALSE(qt_quick::usableHere(env(std::nullopt)));
    CHECK_FALSE(qt_quick::usableHere(env(std::string{})));
    CHECK(qt_quick::usableHere(env(std::string{":0"})));
    CHECK_FALSE(qt_quick::usableHere(ui::EnvironmentReader{}));
#endif
}

int main(int argcIn, char* argvIn[]) {
    Catch::Session session;
    return morph::testkit::runSession(session, argcIn, argvIn);
}

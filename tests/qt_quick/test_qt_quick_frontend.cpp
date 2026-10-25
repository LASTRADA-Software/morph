// SPDX-License-Identifier: Apache-2.0
//
// qt_quick::Frontend: run, quit and the teardown order (docs/spec/qt_quick/frontend.md). This executable owns no
// QGuiApplication: each run makes its own, as a client's main does.

#include <QColor>
#include <QCoreApplication>
#include <QGuiApplication>
#include <QKeyEvent>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickWindow>
#include <QWindow>
#include <array>
#include <catch2/catch_session.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <expected>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <morph/qt_quick/frontend.hpp>
#include <morph/reactive/runtime.hpp>
#include <morph/ui/frontend.hpp>
#include <morph/ui/mount.hpp>
#include <morph/ui/view.hpp>
#include <optional>
#include <sstream>
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
        .title = "test"};
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
        .title = "test"};
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

namespace {

/// A source whose bundle carries the given theme files.
class ThemedSource final : public ui::AppSource {
public:
    explicit ThemedSource(std::map<std::string, std::string, std::less<>> themes) : _themes{std::move(themes)} {}

    void open(ui::AppContext& context,
              std::function<void(std::expected<ui::Bundle, ui::ConnectError>)> ready) override {
        static_cast<void>(context);
        ui::Bundle bundle{.applicationId = "app", .manifestDigest = {}, .documents = {}};
        bundle.themes = _themes;
        ready(std::move(bundle));
    }
    void switchBackend(ui::Backend backend, std::function<void(std::expected<void, std::string>)> done) override {
        static_cast<void>(backend);
        done({});
    }
    void close() override {}

private:
    std::map<std::string, std::string, std::less<>> _themes;
};

/// A bundle theme whose accent is green.
std::map<std::string, std::string, std::less<>> greenTheme() {
    return {{"theme/index.json", R"({"vocab": "ui-theme/1", "base": "theme/base.json"})"},
            {"theme/base.json", R"({"color": {"$type": "color", "accent": {"$value": "#00ff00"}}})"}};
}

/// What `std::cerr` receives while it lives.
class CapturedErr {
public:
    CapturedErr() : _previous{std::cerr.rdbuf(_text.rdbuf())} {}
    ~CapturedErr() { std::cerr.rdbuf(_previous); }
    CapturedErr(CapturedErr const&) = delete;
    CapturedErr& operator=(CapturedErr const&) = delete;
    CapturedErr(CapturedErr&&) = delete;
    CapturedErr& operator=(CapturedErr&&) = delete;

    /// How many times @p text was written.
    [[nodiscard]] std::size_t count(std::string const& text) const {
        std::string const all = _text.str();
        std::size_t found = 0;
        for (std::size_t at = all.find(text); at != std::string::npos; at = all.find(text, at + text.size())) {
            ++found;
        }
        return found;
    }

private:
    std::ostringstream _text;
    std::streambuf* _previous;
};

QObject* themeOf(QQmlEngine& engine) {
    return engine.singletonInstance<QObject*>(QStringLiteral("MorphUi"), QStringLiteral("Theme"));
}

constexpr char const* kReplaced = "the theme onEngine applied is replaced by the bundle's";
constexpr char const* kIgnored = "the bundle's theme is not applied: FrontendConfig::bundleTheme is Ignore";

}  // namespace

// Mutation: call the hook after the backend is built (the backend's column is already in the content item).
TEST_CASE("qt_quick frontend: onEngine runs with the engine and the window before the backend exists",
          "[qt_quick][frontend]") {
    std::vector<std::string> log;
    RecordingSource source{log};
    qt_quick::FrontendConfig config =
        configWith(log, [](ui::AppContext& context) { context.executor().post([&context] { context.quit(0); }); });
    std::optional<bool> emptyContent;
    QQmlEngine* seen = nullptr;
    config.onEngine = [&emptyContent, &seen, &log](QQmlEngine& engine, QQuickWindow& window) {
        log.emplace_back("onEngine");
        emptyContent = window.contentItem()->childItems().isEmpty();
        CHECK(window.title() == QStringLiteral("test"));
        seen = &engine;
    };
    qt_quick::Frontend frontend{argCount(), argValues(), std::move(config)};
    CHECK(frontend.run(source) == 0);
    REQUIRE(emptyContent.has_value());
    CHECK(*emptyContent);
    CHECK(seen != nullptr);
    REQUIRE_FALSE(log.empty());
    CHECK(log.front() == "onEngine");  // before the source opens
}

TEST_CASE("qt_quick frontend: an onEngine that throws ends the run after the teardown, with nothing mounted",
          "[qt_quick][frontend]") {
    std::vector<std::string> log;
    RecordingSource source{log};
    qt_quick::FrontendConfig config = configWith(log, [](ui::AppContext&) {});
    config.onEngine = [](QQmlEngine&, QQuickWindow&) { throw std::runtime_error{"no engine setup"}; };
    qt_quick::Frontend frontend{argCount(), argValues(), std::move(config)};
    CHECK_THROWS_WITH(frontend.run(source), "no engine setup");
    CHECK(log.empty());  // the source was never opened, and no shell was mounted
    CHECK(QGuiApplication::instance() == nullptr);
}

// Mutation: apply the bundle's theme whatever bundleTheme says (the accent is the bundle's green).
TEST_CASE("qt_quick frontend: with bundleTheme Ignore the theme onEngine set stays, and the bundle's is reported",
          "[qt_quick][frontend]") {
    CapturedErr const err;
    ThemedSource source{greenTheme()};
    std::vector<std::string> log;
    std::optional<QColor> accent;
    QQmlEngine* engineSeen = nullptr;
    qt_quick::FrontendConfig config = configWith(log, [&accent, &engineSeen](ui::AppContext& context) {
        accent = themeOf(*engineSeen)->property("accent").value<QColor>();
        context.quit(0);
    });
    config.bundleTheme = qt_quick::BundleTheme::Ignore;
    config.onEngine = [&engineSeen](QQmlEngine& engine, QQuickWindow&) {
        engineSeen = &engine;
        themeOf(engine)->setProperty("accent", QColor{"#aa0055"});
    };
    qt_quick::Frontend frontend{argCount(), argValues(), std::move(config)};
    CHECK(frontend.run(source) == 0);
    REQUIRE(accent.has_value());
    CHECK(*accent == QColor{"#aa0055"});
    CHECK(err.count(kIgnored) == 1);
    CHECK(err.count(kReplaced) == 0);
}

// Mutation: never compare the snapshots (the replacement is not reported).
TEST_CASE("qt_quick frontend: with bundleTheme Apply a theme onEngine set is replaced by the bundle's, and reported",
          "[qt_quick][frontend]") {
    for (bool const changes : {true, false}) {
        INFO((changes ? "the hook sets the accent" : "the hook changes nothing"));
        CapturedErr const err;
        ThemedSource source{greenTheme()};
        std::vector<std::string> log;
        std::optional<QColor> accent;
        QQmlEngine* engineSeen = nullptr;
        qt_quick::FrontendConfig config = configWith(log, [&accent, &engineSeen](ui::AppContext& context) {
            accent = themeOf(*engineSeen)->property("accent").value<QColor>();
            context.quit(0);
        });
        config.onEngine = [&engineSeen, changes](QQmlEngine& engine, QQuickWindow&) {
            engineSeen = &engine;
            if (changes) {
                themeOf(engine)->setProperty("accent", QColor{"#aa0055"});
            }
        };
        qt_quick::Frontend frontend{argCount(), argValues(), std::move(config)};
        CHECK(frontend.run(source) == 0);
        REQUIRE(accent.has_value());
        CHECK(*accent == QColor{"#00ff00"});
        CHECK(err.count(kReplaced) == (changes ? 1U : 0U));
        CHECK(err.count(kIgnored) == 0);
    }
}

int main(int argcIn, char* argvIn[]) {
    Catch::Session session;
    return morph::testkit::runSession(session, argcIn, argvIn);
}

TEST_CASE("qt_quick frontend: Ctrl+Shift+I opens the inspector only in development mode", "[qt_quick][frontend]") {
    for (ui::DevelopmentTools const mode : {ui::DevelopmentTools::Off, ui::DevelopmentTools::On}) {
        INFO((mode == ui::DevelopmentTools::On ? "On" : "Off"));
        std::vector<std::string> log;
        RecordingSource source{log};
        qt_quick::FrontendConfig config = configWith(log, [](ui::AppContext& context) {
            context.executor().post([&context] {
                QWindow* app = nullptr;
                for (QWindow* const window : QGuiApplication::topLevelWindows()) {
                    if (window->title() == QStringLiteral("test")) {
                        app = window;
                    }
                }
                REQUIRE(app != nullptr);
                QKeyEvent press{QEvent::KeyPress, Qt::Key_I, Qt::ControlModifier | Qt::ShiftModifier};
                QCoreApplication::sendEvent(app, &press);
                context.executor().post([&context] {
                    int shown = 0;
                    for (QWindow const* const window : QGuiApplication::topLevelWindows()) {
                        shown += window->isVisible() ? 1 : 0;
                    }
                    context.quit(shown);
                });
            });
        });
        config.devTools = mode;
        qt_quick::Frontend frontend{argCount(), argValues(), std::move(config)};
        // The application's window, and in development mode the inspector's window over it.
        CHECK(frontend.run(source) == (mode == ui::DevelopmentTools::On ? 2 : 1));
    }
}

namespace {

/// A shell that records the backend's width class when it mounts and each change after.
class WidthShell final : public ui::MountedShell {
public:
    WidthShell(ui::IViewBackend& backend, std::vector<std::string>& heard) : _backend{&backend} {
        heard.push_back(backend.widthClass());
        backend.setOnWidthClass([&heard](std::string const& widthClass) { heard.push_back(widthClass); });
    }
    ~WidthShell() override { _backend->setOnWidthClass({}); }
    WidthShell(WidthShell const&) = delete;
    WidthShell& operator=(WidthShell const&) = delete;
    WidthShell(WidthShell&&) = delete;
    WidthShell& operator=(WidthShell&&) = delete;

private:
    ui::IViewBackend* _backend;
};

// The window's width for @p units of the engine's Theme.
int widthFor(QQmlEngine& engine, double units) {
    auto* const theme = engine.singletonInstance<QObject*>(QStringLiteral("MorphUi"), QStringLiteral("Theme"));
    REQUIRE(theme != nullptr);
    return static_cast<int>(units * theme->property("unitWidth").toDouble()) + 1;
}

}  // namespace

// Mutation: class the window by its width in pixels (60 units, some 450 px, is then expanded), never tell the listener
// (it hears the first class only), or leave the content item unsized until the window shows (the shell first reads
// compact for a window that is not).
TEST_CASE("qt_quick frontend: the shell reads the window's width class, and hears it change", "[qt_quick][frontend]") {
    std::vector<std::string> log;
    RecordingSource source{log};
    std::vector<std::string> heard;
    QQuickWindow* shown = nullptr;
    QQmlEngine* engineOf = nullptr;
    qt_quick::FrontendConfig config{
        .mountShell = [&heard, &shown, &engineOf](ui::AppContext& context, ui::IViewBackend& backend,
                                                  std::expected<ui::Bundle, ui::ConnectError> /*opened*/)
            -> std::unique_ptr<ui::MountedShell> {
            auto shell = std::make_unique<WidthShell>(backend, heard);
            context.executor().post([&context, &shown, &engineOf] {
                shown->resize(widthFor(*engineOf, 60), 900);
                context.executor().post([&context] { context.quit(0); });
            });
            return shell;
        },
        .components = nullptr,
        .title = "test",
        .onEngine =
            [&shown, &engineOf](QQmlEngine& engine, QQuickWindow& window) {
                shown = &window;
                engineOf = &engine;
                window.resize(widthFor(engine, 120), 900);
            }};
    qt_quick::Frontend frontend{argCount(), argValues(), std::move(config)};
    CHECK(frontend.run(source) == 0);
    CHECK(heard == std::vector<std::string>{"medium", "compact"});
}

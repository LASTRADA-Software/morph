// SPDX-License-Identifier: Apache-2.0

#include <QCoreApplication>
#include <QEventLoop>
#include <QGuiApplication>
#include <QObject>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QSGRendererInterface>
#include <algorithm>
#include <array>
#include <morph/qt/qt_executor.hpp>
#include <morph/qt_quick/backend.hpp>
#include <morph/qt_quick/frontend.hpp>
#include <morph/qt_quick/scheduler.hpp>
#include <morph/reactive/runtime.hpp>
#include <optional>
#include <stdexcept>
#include <utility>

#include "qt_quick/keys.hpp"

namespace morph::qt_quick {

bool usableHere(ui::EnvironmentReader const& env) {
#if defined(__APPLE__) || defined(_WIN32) || defined(__EMSCRIPTEN__)
    static_cast<void>(env);
    return true;
#else
    if (!env) {
        return false;
    }
    std::array<char const*, 3> const names{"DISPLAY", "WAYLAND_DISPLAY", "QT_QPA_PLATFORM"};
    return std::ranges::any_of(names, [&env](char const* name) {
        std::optional<std::string> const value = env(name);
        return value.has_value() && !value->empty();
    });
#endif
}

namespace {

/// What the frontend lends the source and the screens. Quitting before the loop runs is remembered, and the loop
/// then returns at once.
class Context final : public ui::AppContext {
public:
    Context(reactive::Runtime& runtime, exec::IExecutor& executor, ui::Scheduler& scheduler)
        : _runtime{&runtime}, _executor{&executor}, _scheduler{&scheduler} {}

    reactive::Runtime& runtime() override { return *_runtime; }
    exec::IExecutor& executor() override { return *_executor; }
    ui::Scheduler& scheduler() override { return *_scheduler; }
    exec::IoLoop* ioLoop() override { return nullptr; }

    void quit(int exitCode) override {
        if (_running) {
            QCoreApplication::exit(exitCode);
        } else {
            _pending = exitCode;
        }
    }

    /// Runs the loop until `quit`, or returns a code `quit` gave before it.
    int loop(QQuickWindow& window) {
        if (_pending) {
            return *_pending;
        }
        window.show();
        QEventLoop events;
        _running = true;
        int const code = events.exec();
        _running = false;
        return code;
    }

private:
    reactive::Runtime* _runtime;
    exec::IExecutor* _executor;
    ui::Scheduler* _scheduler;
    std::optional<int> _pending;
    bool _running = false;
};

}  // namespace

Frontend::Frontend(int& argc, char** argv, FrontendConfig config)
    : _argc{&argc}, _argv{argv}, _config{std::move(config)} {}

std::string_view Frontend::name() const { return "qt"; }

// Construction order is destruction order reversed, and it is what the teardown guarantee rests on: runApp destroys
// the screens and closes the source; then the backend, the window and the engine go, then the scheduler, the
// runtime and the executor, and Qt last.
int Frontend::run(ui::AppSource& source) {
    if (!_config.mountShell) {
        throw std::invalid_argument{"morph::qt_quick::Frontend: FrontendConfig::mountShell must not be empty"};
    }
    std::optional<QGuiApplication> application;
    if (QCoreApplication::instance() == nullptr) {
        // The style and the graphics API are process-wide and fixed once Qt Quick has loaded: a second run in the
        // same process keeps what the first chose.
        if (QQuickStyle::name().isEmpty()) {
            QQuickStyle::setStyle(QStringLiteral("Basic"));
        }
        application.emplace(*_argc, _argv);
    }
    if (QGuiApplication::platformName() == QStringLiteral("offscreen") &&
        QQuickWindow::graphicsApi() != QSGRendererInterface::Software) {
        QQuickWindow::setGraphicsApi(QSGRendererInterface::Software);
    }
    QObject anchor;
    qt::QtExecutor executor{&anchor};
    reactive::Runtime runtime{executor};
    QtScheduler scheduler{executor, &anchor};
    QQmlEngine engine;
    QQuickWindow window;
    window.setTitle(detail::toQString(_config.title));
    window.resize(1024, 768);
    Backend backend{engine, *window.contentItem(),
                    BackendOptions{.components = _config.components, .onWidthClass = _config.onWidthClass}};
    Context context{runtime, executor, scheduler};
    QObject::connect(&window, &QQuickWindow::closing, &anchor, [&context] { context.quit(0); });
    return ui::runApp(
        context, source,
        [this, &backend](ui::AppContext& opened, std::expected<ui::Bundle, ui::ConnectError> result) {
            return _config.mountShell(opened, backend, std::move(result));
        },
        [&context, &window] { return context.loop(window); });
}

ui::FrontendOption frontendOption(int& argc, char** argv, FrontendConfig config) {
    return ui::FrontendOption{
        .name = "qt",
        .usable = [] { return usableHere(); },
        .make =
            [&argc, argv, config = std::move(config)] {
                return std::unique_ptr<ui::Frontend>{std::make_unique<Frontend>(argc, argv, config)};
            },
    };
}

}  // namespace morph::qt_quick

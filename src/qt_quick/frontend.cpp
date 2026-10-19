// SPDX-License-Identifier: Apache-2.0

#include <QCoreApplication>
#include <QEventLoop>
#include <QGuiApplication>
#include <QMetaObject>
#include <QMetaProperty>
#include <QMetaType>
#include <QObject>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QSGRendererInterface>
#include <QVariant>
#include <QVariantList>
#include <algorithm>
#include <array>
#include <iostream>
#include <morph/qt/qt_executor.hpp>
#include <morph/qt_quick/backend.hpp>
#include <morph/qt_quick/frontend.hpp>
#include <morph/qt_quick/inspector.hpp>
#include <morph/qt_quick/scheduler.hpp>
#include <morph/qt_quick/theme.hpp>
#include <morph/reactive/runtime.hpp>
#include <morph/ui/theme.hpp>
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

/// The values of the `Theme` singleton's writable typed properties, in declaration order; empty when there is none.
/// A `var` property (the schemes a theme declared) holds a JavaScript value that compares unequal to itself, and is
/// left out.
QVariantList themeSnapshot(QQmlEngine& engine) {
    QVariantList values;
    QObject const* const theme =
        engine.singletonInstance<QObject*>(QStringLiteral("MorphUi"), QStringLiteral("Theme"));
    if (theme == nullptr) {
        return values;
    }
    QMetaObject const* const meta = theme->metaObject();
    for (int index = QObject::staticMetaObject.propertyCount(); index < meta->propertyCount(); ++index) {
        QMetaProperty const property = meta->property(index);
        if (property.isWritable() && property.metaType() != QMetaType::fromType<QVariant>()) {
            values.append(property.read(theme));
        }
    }
    return values;
}

/// Whether a bundle's theme files hold a theme index, the file that gives a bundle a theme.
bool carriesTheme(ui::Bundle const& bundle) {
    return std::ranges::any_of(bundle.themes, [](auto const& file) { return ui::isThemeIndex(file.second); });
}

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
    // Only the first run in a process chooses. After a QGuiApplication is destroyed and another made, graphicsApi() no
    // longer reports the API the scene graph was initialised with, and Qt refuses a second choice with a warning.
    static bool graphicsApiFixed = false;
    if (!graphicsApiFixed && QGuiApplication::platformName() == QStringLiteral("offscreen") &&
        QQuickWindow::graphicsApi() != QSGRendererInterface::Software) {
        QQuickWindow::setGraphicsApi(QSGRendererInterface::Software);
    }
    graphicsApiFixed = true;
    QObject anchor;
    qt::QtExecutor executor{&anchor};
    reactive::Runtime runtime{executor};
    QtScheduler scheduler{executor, &anchor};
    QQmlEngine engine;
    QQuickWindow window;
    window.setTitle(detail::toQString(_config.title));
    window.resize(1024, 768);
    // The hook runs before the backend exists, so what it maps and applies reaches every widget the shell makes. A
    // throw unwinds the window, the engine, the scheduler, the runtime and the executor, in that order.
    bool hookChangedTheme = false;
    if (_config.onEngine) {
        QVariantList const before = themeSnapshot(engine);
        _config.onEngine(engine, window);
        hookChangedTheme = themeSnapshot(engine) != before;
    }
    Backend backend{engine, *window.contentItem(),
                    BackendOptions{.components = _config.components, .onWidthClass = _config.onWidthClass}};
    Context context{runtime, executor, scheduler};
    QObject::connect(&window, &QQuickWindow::closing, &anchor, [&context] { context.quit(0); });
    // The shell the inspector reads; cleared once the loop has ended, before runApp destroys the shell.
    ui::MountedShell* shown = nullptr;
    std::optional<InspectorWindow> inspector;
    if (_config.devTools == ui::DevelopmentTools::On) {
        inspector.emplace([&shown](std::function<void(ui::Inspector&)> const& visit) {
            if (shown != nullptr) {
                shown->inspectFocused(visit);
            }
        });
        inspector->attachTo(window);
    }
    return ui::runApp(
        context, source,
        [this, &backend, &engine, &shown, hookChangedTheme](ui::AppContext& opened,
                                                            std::expected<ui::Bundle, ui::ConnectError> result) {
            // The application's look, before the shell mounts its first screen.
            if (result && _config.bundleTheme == BundleTheme::Apply) {
                if (hookChangedTheme) {
                    std::cerr << "morph::qt_quick: the theme onEngine applied is replaced by the bundle's; set "
                                 "FrontendConfig::bundleTheme to Ignore to keep it\n";
                }
                for (std::string const& problem : applyBundleTheme(engine, result->themes)) {
                    std::cerr << "morph::qt_quick: " << problem << '\n';
                }
            } else if (result && carriesTheme(*result)) {
                std::cerr << "morph::qt_quick: the bundle's theme is not applied: FrontendConfig::bundleTheme is "
                             "Ignore\n";
            }
            auto shell = _config.mountShell(opened, backend, std::move(result));
            shown = shell.get();
            return shell;
        },
        [&] {
            int const code = context.loop(window);
            if (inspector) {
                inspector->hide();
            }
            shown = nullptr;
            return code;
        });
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

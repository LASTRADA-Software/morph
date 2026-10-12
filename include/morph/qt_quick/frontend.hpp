// SPDX-License-Identifier: Apache-2.0

#pragma once
#include <expected>
#include <functional>
#include <memory>
#include <string>
#include <string_view>

#include "../attributes.hpp"
#include "../ui/backend.hpp"
#include "../ui/frontend.hpp"

/// @file
/// @brief `morph::qt_quick::Frontend`: a morph application in a Qt Quick window.
///
/// Specified in `docs/spec/qt_quick/frontend.md`.

namespace morph::qt_quick {

class Components;

/// @brief Mounts the application's shell for what the source delivered, on the window's widgets.
///
/// `ui::ShellFactory` with the backend the shell's views mount on, since `ui::AppContext` names no renderer. Called
/// once, on the owner.
using ShellFactory = std::function<std::unique_ptr<ui::MountedShell>(
    ui::AppContext& context, ui::IViewBackend& backend, std::expected<ui::Bundle, ui::ConnectError> opened)>;

/// @brief What a `qt_quick::Frontend` runs with.
struct FrontendConfig {
    /// @brief Mounts the shell when the source is ready; it must not be empty.
    ShellFactory mountShell;
    /// @brief The restyle map and the custom components the backend uses; null uses none. Borrowed: it outlives
    ///        the run.
    Components* components = nullptr;
    /// @brief The window's title.
    std::string title = "morph";
    /// @brief Called with the window's width class (`widthClassOf`) when the window first has a width and each time
    ///        the class changes; empty calls nothing. The shell sets it as `{"env": "widthClass"}`.
    std::function<void(std::string const& widthClass)> onWidthClass;
};

/// @brief Whether a Qt Quick window can be shown here: on macOS, Windows and WebAssembly always; elsewhere when
///        `DISPLAY`, `WAYLAND_DISPLAY` or `QT_QPA_PLATFORM` is set and not empty.
/// @param env Reads the environment.
/// @return True when it can.
[[nodiscard]] bool usableHere(ui::EnvironmentReader const& env = ui::processEnvironment());

/// @brief Runs a morph application in a Qt Quick window, on the calling thread.
///
/// `run` makes, in this order, the `QGuiApplication` (unless the process has one already), a `qt::QtExecutor` (the
/// reactive runtime's owner and every bridge handler's callback executor), the runtime, a `QtScheduler`, the QML
/// engine, the window and the backend, then runs the source through `ui::runApp`: it opens the source, mounts the
/// shell through `FrontendConfig::mountShell` when the source is ready, and runs the event loop until
/// `AppContext::quit()` or the window closes. Teardown is the reverse: the screens, then the source's connections,
/// then the runtime, then Qt. The style is Qt Quick Controls' `Basic`; on the `offscreen` platform the window
/// renders in software.
class Frontend final : public ui::Frontend {
public:
    /// @brief A frontend that runs with the process's command line.
    /// @param argc The argument count; `QGuiApplication` keeps a reference to it.
    /// @param argv The arguments.
    /// @param config The shell factory, the components and the window title.
    Frontend(int& argc MORPH_LIFETIMEBOUND, char** argv, FrontendConfig config);

    /// @brief The name `--ui=` and `MORPH_UI` select this frontend by.
    /// @return `"qt"`.
    [[nodiscard]] std::string_view name() const override;

    /// @brief Runs the application the source opens until it quits or its window closes.
    /// @param source Opened once the runtime exists, and closed before it is destroyed.
    /// @return The exit code given to `AppContext::quit()`; 0 when the window was closed.
    /// @throws std::invalid_argument when `FrontendConfig::mountShell` is empty; whatever the source or the shell
    ///         factory throws.
    int run(ui::AppSource& source) override;

private:
    int* _argc;
    char** _argv;
    FrontendConfig _config;
};

/// @brief The Qt Quick frontend's entry for `ui::selectFrontend`.
/// @param argc The argument count, kept by reference for the `QGuiApplication` a run makes.
/// @param argv The arguments.
/// @param config Handed to every `Frontend` the option makes.
/// @return An option named `"qt"`, usable as `usableHere` says.
[[nodiscard]] ui::FrontendOption frontendOption(int& argc MORPH_LIFETIMEBOUND, char** argv, FrontendConfig config);

}  // namespace morph::qt_quick

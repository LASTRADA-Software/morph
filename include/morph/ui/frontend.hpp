// SPDX-License-Identifier: Apache-2.0

#pragma once
#include <algorithm>
#include <cstddef>
#include <cstdlib>
#include <functional>
#include <memory>
#include <optional>
#include <ranges>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include "../core/executor.hpp"
#include "../reactive/runtime.hpp"
#include "../reactive/scheduler.hpp"
#include "backend.hpp"
#include "mount.hpp"
#include "view.hpp"

/// @file
/// @brief The frontend seam: an application is a factory the frontend calls, and `main` picks the frontend at
///        runtime.
///
/// Specified in `docs/spec/ui/frontend.md`.

namespace morph::exec {
class IoLoop;
}  // namespace morph::exec

namespace morph::ui {

/// @brief Timers on the frontend's owner executor; see `reactive::Scheduler`.
using Scheduler = reactive::Scheduler;

/// @brief Owns one scheduled timer and cancels it when destroyed; see `reactive::TimerHandle`.
using TimerHandle = reactive::TimerHandle;

/// @brief What a frontend hands the application it runs.
///
/// Everything it lends (the runtime, the executor, the scheduler, the loop) outlives the application the factory
/// makes, so an application may hold references to them and use them in its destructor. The context itself lives
/// at least as long as the application.
class AppContext {
public:
    AppContext() = default;
    virtual ~AppContext() = default;
    AppContext(AppContext const&) = delete;
    AppContext& operator=(AppContext const&) = delete;
    AppContext(AppContext&&) = delete;
    AppContext& operator=(AppContext&&) = delete;

    /// @brief The runtime the view is mounted in. Owned by the frontend; it outlives the application.
    /// @return The runtime.
    virtual reactive::Runtime& runtime() = 0;

    /// @brief The runtime's owner, and the callback executor of every `BridgeHandler` the application makes.
    /// @return The executor; the same one as `runtime().owner()`.
    virtual exec::IExecutor& executor() = 0;

    /// @brief Timers that fire on the owner executor.
    ///
    /// It honours every guarantee of `reactive::Scheduler`, as `docs/spec/reactive/control.md` ("The `Scheduler`
    /// contract") states them.
    /// @return The scheduler.
    virtual Scheduler& scheduler() = 0;

    /// @brief The I/O loop the frontend runs on, for sockets and timers that share its one thread.
    /// @return The loop, or null when the frontend has none (Qt Quick).
    virtual exec::IoLoop* ioLoop() = 0;

    /// @brief Makes `Frontend::run` return once the current event has been handled.
    ///
    /// Called from the factory, before the loop runs, it still ends `run` with @p exitCode once the view has been
    /// mounted: a mount runs to completion.
    /// @param exitCode What `run` returns.
    virtual void quit(int exitCode = 0) = 0;

    /// @brief The running frontend's name.
    /// @return The name `selectFrontend` matched, such as `tui` or `qt`.
    [[nodiscard]] virtual std::string_view frontendName() const = 0;
};

/// @brief An application: its controllers, handlers and bridge wiring, and the view over them.
///
/// The frontend destroys the mounted view before the application, so a widget callback never runs against a
/// destroyed application, and the application before the runtime and the backend, so its signals and bindings never
/// outlive the runtime they were made in.
class Application {
public:
    Application() = default;
    virtual ~Application() = default;
    Application(Application const&) = delete;
    Application& operator=(Application const&) = delete;
    Application(Application&&) = delete;
    Application& operator=(Application&&) = delete;

    /// @brief The view tree to mount; called once, after construction.
    /// @return The root node; never null.
    [[nodiscard]] virtual Node view() = 0;
};

/// @brief Makes the application, given the running frontend's context.
using ApplicationFactory = std::function<std::unique_ptr<Application>(AppContext&)>;

/// @brief A frontend: owns an event loop and renders one application's view.
class Frontend {
public:
    Frontend() = default;
    virtual ~Frontend() = default;
    Frontend(Frontend const&) = delete;
    Frontend& operator=(Frontend const&) = delete;
    Frontend(Frontend&&) = delete;
    Frontend& operator=(Frontend&&) = delete;

    /// @brief The name `selectFrontend` matches.
    /// @return The name.
    [[nodiscard]] virtual std::string_view name() const = 0;

    /// @brief Builds the runtime and its executor, calls @p factory, mounts the application's view, and drives the
    ///        loop until `AppContext::quit`.
    ///
    /// Tears down in reverse, and in this order: the mounted view, then the application, then the runtime and the
    /// backend, then the loop and the toolkit. A frontend keeps the order by declaring its members or locals in
    /// construction order, and leaves the application and its mount to `runApplication`, which destroys both before
    /// it returns.
    /// @param factory Makes the application.
    /// @return The exit code passed to `quit`; a frontend may also end the run on its own (end of input, an
    ///         interrupt) with a code it documents.
    virtual int run(ApplicationFactory const& factory) = 0;
};

/// @brief One frontend a binary was built with.
struct FrontendOption {
    /// @brief The name `--ui=` and `MORPH_UI` match.
    std::string name;
    /// @brief Whether the frontend can run here (a terminal on stdin, a display); empty counts as usable.
    std::function<bool()> usable;
    /// @brief Makes the frontend; must not return null.
    std::function<std::unique_ptr<Frontend>()> make;
};

/// @brief No frontend could be selected; the message names the built ones.
class FrontendSelectionError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

/// @brief Reads an environment variable: its value, or `nullopt` when it is unset.
using EnvironmentReader = std::function<std::optional<std::string>(std::string_view name)>;

/// @brief The process environment, read through `std::getenv`.
/// @return A reader over the process environment.
[[nodiscard]] inline EnvironmentReader processEnvironment() {
    return [](std::string_view name) -> std::optional<std::string> {
        std::string const key{name};
        // getenv races only with a concurrent setenv; frontend selection runs in main before any thread exists.
        // MSVC deprecates getenv (C4996) in favour of _dupenv_s; a consumer built with /WX must not fail on it.
        // clang-cl reports the same deprecation as -Wdeprecated-declarations, which the MSVC pragma does not reach.
        // The MSVC pragma stays on the line just before the call, the only line it covers.
#if defined(__clang__) && defined(_WIN32)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
#endif
#ifdef _MSC_VER
#pragma warning(suppress : 4996)
#endif
        char const* const value = std::getenv(key.c_str());  // NOLINT(concurrency-mt-unsafe)
#if defined(__clang__) && defined(_WIN32)
#pragma clang diagnostic pop
#endif
        if (value == nullptr) {
            return std::nullopt;
        }
        return std::string{value};
    };
}

namespace detail {

/// @brief The built frontends' names, for an error message.
/// @param built The options.
/// @return The names joined by `, `, or `none`.
[[nodiscard]] inline std::string builtNames(std::span<FrontendOption const> built) {
    if (built.empty()) {
        return "none";
    }
    std::string names;
    for (FrontendOption const& option : built) {
        if (!names.empty()) {
            names += ", ";
        }
        names += option.name;
    }
    return names;
}

/// @brief The name the command line asks for: the last `--ui=<name>` or `--ui <name>` before a `--`.
///
/// `--` ends option parsing, so a `--ui` after it is an operand, not a choice. An argument starting with `--` is
/// never taken as the name after `--ui`: it is the next option, and the `--ui` before it has no name.
/// @param built The options, for the error message.
/// @param argc The argument count, as `main` received it.
/// @param argv The arguments, as `main` received them; `argv[0]` is skipped.
/// @return The name, or `nullopt` when no `--ui` is given.
/// @throws FrontendSelectionError for a `--ui` without a name.
[[nodiscard]] inline std::optional<std::string> commandLineFrontend(std::span<FrontendOption const> built, int argc,
                                                                    char const* const* argv) {
    std::size_t const count = argv == nullptr || argc <= 0 ? 0 : static_cast<std::size_t>(argc);
    std::span<char const* const> const args{argv, count};
    std::optional<std::string> name;
    bool nameFollows = false;
    for (char const* const raw : args | std::views::drop(1)) {
        std::string_view const arg = raw == nullptr ? std::string_view{} : std::string_view{raw};
        if (nameFollows) {
            if (arg.starts_with("--")) {
                break;
            }
            name = std::string{arg};
            nameFollows = false;
        } else if (arg == "--") {
            break;
        } else if (arg == "--ui") {
            nameFollows = true;
        } else if (arg.starts_with("--ui=")) {
            name = std::string{arg.substr(std::string_view{"--ui="}.size())};
        }
    }
    if (nameFollows || (name.has_value() && name->empty())) {
        throw FrontendSelectionError{"morph::ui: --ui needs a frontend name (built: " + builtNames(built) + ")"};
    }
    return name;
}

}  // namespace detail

/// @brief Picks the frontend to run: the last `--ui=<name>` (or `--ui <name>`), else a non-empty `MORPH_UI`, else
///        the first option in the given order whose `usable()` holds.
///
/// A frontend named on the command line or in `MORPH_UI` is used even when its `usable()` is false: the user asked
/// for it.
/// @param built The frontends this binary was built with, in order of preference.
/// @param argc The argument count, as `main` received it.
/// @param argv The arguments, as `main` received them.
/// @param env Reads `MORPH_UI`; the process environment by default.
/// @return The frontend, made.
/// @throws FrontendSelectionError for an unknown or unbuilt name, a `--ui` without one, no usable option, or an
///         option that makes nothing; the message names the built frontends.
[[nodiscard]] inline std::unique_ptr<Frontend> selectFrontend(std::span<FrontendOption const> built, int argc,
                                                              char const* const* argv,
                                                              EnvironmentReader const& env = processEnvironment()) {
    std::optional<std::string> requested = detail::commandLineFrontend(built, argc, argv);
    if (!requested.has_value() && env) {
        if (std::optional<std::string> fromEnvironment = env("MORPH_UI");
            fromEnvironment.has_value() && !fromEnvironment->empty()) {
            requested = std::move(fromEnvironment);
        }
    }
    FrontendOption const* chosen = nullptr;
    if (requested.has_value()) {
        auto const found = std::ranges::find(built, *requested, &FrontendOption::name);
        if (found == built.end()) {
            throw FrontendSelectionError{"morph::ui: no frontend named '" + *requested +
                                         "' is built (built: " + detail::builtNames(built) + ")"};
        }
        chosen = &*found;
    } else {
        auto const found = std::ranges::find_if(
            built, [](FrontendOption const& candidate) { return !candidate.usable || candidate.usable(); });
        if (found == built.end()) {
            throw FrontendSelectionError{
                "morph::ui: no built frontend is usable here (built: " + detail::builtNames(built) + ")"};
        }
        chosen = &*found;
    }
    std::unique_ptr<Frontend> frontend = chosen->make ? chosen->make() : nullptr;
    if (frontend == nullptr) {
        throw FrontendSelectionError{"morph::ui: frontend '" + chosen->name + "' could not be made"};
    }
    return frontend;
}

/// @brief Runs one application inside a frontend: makes it, mounts its view, runs the loop, and tears both down in
///        order.
///
/// The order is fixed here so that every frontend keeps it: the mounted view is destroyed first, while the
/// application whose state its bindings and callbacks read is still alive, and the application next, while the
/// runtime and the backend it was made with are still alive. The order holds whether @p loop returns or throws, and
/// a mount that throws destroys the application before the exception leaves.
/// @param context The frontend's context, handed to @p factory. Its runtime, and @p backend, must outlive this call.
/// @param backend Makes the view's widgets.
/// @param factory Makes the application.
/// @param loop Runs the frontend's event loop with the view mounted (showing the window or focusing the first
///        widget first, if the frontend needs to), and returns the exit code once `AppContext::quit` ends it.
/// @return What @p loop returns.
/// @throws std::invalid_argument when @p factory or @p loop is empty, or when @p factory makes no application;
///         whatever @p factory, the mount or @p loop throws.
[[nodiscard]] inline int runApplication(AppContext& context, IViewBackend& backend, ApplicationFactory const& factory,
                                        std::function<int()> const& loop) {
    if (!factory || !loop) {
        throw std::invalid_argument{"morph::ui::runApplication: the factory and the loop must not be empty"};
    }
    std::unique_ptr<Application> const application = factory(context);
    if (application == nullptr) {
        throw std::invalid_argument{"morph::ui: frontend '" + std::string{context.frontendName()} +
                                    "': the application factory made no application"};
    }
    // Declared after the application, so it is destroyed before it.
    Mounted const mounted{context.runtime(), backend, application->view()};
    return loop();
}

}  // namespace morph::ui

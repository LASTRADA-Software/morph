// SPDX-License-Identifier: Apache-2.0

#pragma once
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <expected>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <ranges>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "../core/executor.hpp"
#include "../reactive/runtime.hpp"
#include "../reactive/scheduler.hpp"

/// @file
/// @brief The frontend seam: `main` picks a frontend at runtime, and the frontend runs whatever an `AppSource`
///        opens.
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

/// @brief What a frontend lends the source it opens and the screens it mounts.
///
/// Everything it lends (the runtime, the executor, the scheduler, the loop) outlives the screens and the
/// connections made with it: a frontend destroys those, through `runApp`, before its runtime.
class AppContext {
public:
    AppContext() = default;
    virtual ~AppContext() = default;
    AppContext(AppContext const&) = delete;
    AppContext& operator=(AppContext const&) = delete;
    AppContext(AppContext&&) = delete;
    AppContext& operator=(AppContext&&) = delete;

    /// @brief The runtime the screens are mounted in. Owned by the frontend.
    /// @return The runtime.
    virtual reactive::Runtime& runtime() = 0;

    /// @brief The runtime's owner, and the callback executor of every bridge handler the source and the screens
    ///        make.
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
    /// Called before the loop runs (from `AppSource::open`, or from mounting the shell), it still ends the run once
    /// the loop starts.
    /// @param exitCode What `run` returns.
    virtual void quit(int exitCode = 0) = 0;
};

/// @brief What an open source hands the frontend: the application's screens, verified.
///
/// The document model and the interpreter read it (`docs/spec/ui/frontend.md`, "Bundle"); the frontend seam only
/// carries it from the source to the shell.
struct Bundle {
    /// @brief The application's id, as the source's handshake names it.
    std::string applicationId;
    /// @brief The SHA-256 of the bundle's manifest, in lower-case hexadecimal; every document below was verified
    ///        against the manifest, and the manifest against this digest.
    std::string manifestDigest;
    /// @brief Every document of the bundle, by id, as its JSON text: the screens, the app shell, and the app document
    ///        (`app`) when the bundle has one.
    std::map<std::string, std::string, std::less<>> documents;
};

/// @brief Why a source could not be opened.
struct ConnectError {
    /// @brief What went wrong.
    enum class Kind : std::uint8_t {
        Unreachable,  ///< The server could not be reached, or the connection broke during the handshake.
        NoUiService,  ///< The peer answered, but serves no UI (`unknown envelope kind: ui-hello`).
        Unsupported,  ///< The bundle needs a vocabulary this client does not speak; the message names it.
        Unverified,   ///< A file or the manifest did not match its digest.
        Refused,      ///< The application refused this client or this user: a sign-in, a licence, an authoriser.
    };
    /// @brief What went wrong.
    Kind kind = Kind::Unreachable;
    /// @brief What to tell the user, UTF-8.
    std::string message;
};

/// @brief The application's opaque handle for one of its backends. Its name is what `{"env": "backend"}` reads.
struct Backend {
    /// @brief The backend's name, such as `primary`, `local` or `remote`.
    std::string name;

    /// @brief Memberwise equality.
    /// @return Whether the names match.
    bool operator==(Backend const&) const = default;
};

/// @brief Where the screens and the models are: a server (remote) or the application linked in-process (local).
///
/// `main` builds one from its arguments and hands it to `Frontend::run`. Every call is made on the context's owner
/// executor, and every callback it is given must be called there too.
class AppSource {
public:
    AppSource() = default;
    virtual ~AppSource() = default;
    AppSource(AppSource const&) = delete;
    AppSource& operator=(AppSource const&) = delete;
    AppSource(AppSource&&) = delete;
    AppSource& operator=(AppSource&&) = delete;

    /// @brief Connects (or, local, sets up the models), signs the user in natively if the application needs it, and
    ///        delivers the bundle.
    /// @param context The running frontend's context; its executor is every bridge's callback executor.
    /// @param ready Called once, on the owner, now or later, with the bundle or why there is none. A source that is
    ///        closed first never calls it.
    virtual void open(AppContext& context, std::function<void(std::expected<Bundle, ConnectError>)> ready) = 0;

    /// @brief Replaces the dispatch target under the mounted screens: `Bridge::switchBackend`, posted to the owner.
    /// @param backend One of the application's backends.
    /// @param done Called once, on the owner, when the switch committed, or with why it did not; a switch that
    ///        fails leaves the old backend in place.
    virtual void switchBackend(Backend backend, std::function<void(std::expected<void, std::string>)> done) = 0;

    /// @brief Releases everything `open` made (connections, bridges, handlers) and drops a `ready` not yet
    ///        delivered. Called by `runApp` once the screens are gone and before the frontend's runtime is; the
    ///        source may be opened again afterwards.
    virtual void close() = 0;
};

/// @brief A frontend: owns an event loop and a renderer, and runs one source's screens.
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

    /// @brief Builds the runtime and its executor, opens @p source, mounts the app shell and drives the loop until
    ///        `AppContext::quit`.
    ///
    /// Tears down in reverse: the screens, then the source's connections, then the runtime, then the loop and the
    /// toolkit. A frontend leaves the first two to `runApp`, which fixes that order, and keeps the rest by
    /// declaring its members or locals in construction order.
    /// @param source Where the screens and the models are; it outlives the call.
    /// @return The exit code passed to `quit`; a frontend may also end the run on its own (end of input, an
    ///         interrupt) with a code it documents.
    virtual int run(AppSource& source) = 0;
};

/// @brief One frontend a binary was built with.
struct FrontendOption {
    /// @brief The name `--ui=` and `MORPH_UI` match.
    std::string name;
    /// @brief Whether the frontend can run here (a terminal on stdin, a display); empty counts as usable.
    std::function<bool()> usable;
    /// @brief Makes the frontend.
    std::function<std::unique_ptr<Frontend>()> make;
};

/// @brief Why no frontend was selected.
struct FrontendError {
    /// @brief What went wrong.
    enum class Kind : std::uint8_t {
        MissingName,  ///< A `--ui` without a name.
        UnknownName,  ///< `--ui` or `MORPH_UI` names a frontend this binary was not built with.
        NoneUsable,   ///< Nothing was named, and no built frontend is usable here.
        NotMade,      ///< The chosen option has no `make`, or its `make` returned null.
    };
    /// @brief What went wrong.
    Kind kind = Kind::NoneUsable;
    /// @brief The name asked for, or the option's name for `NotMade`; empty otherwise.
    std::string requested;
    /// @brief The names of the frontends this binary was built with, in order.
    std::vector<std::string> built;

    /// @brief A message for the user, naming the built frontends.
    /// @return The message.
    [[nodiscard]] std::string message() const {
        std::string names;
        for (std::string const& name : built) {
            names += names.empty() ? name : ", " + name;
        }
        if (names.empty()) {
            names = "none";
        }
        switch (kind) {
            case Kind::MissingName:
                return "morph::ui: --ui needs a frontend name (built: " + names + ")";
            case Kind::UnknownName:
                return "morph::ui: no frontend named '" + requested + "' is built (built: " + names + ")";
            case Kind::NotMade:
                return "morph::ui: frontend '" + requested + "' could not be made";
            case Kind::NoneUsable:
            default:
                return "morph::ui: no built frontend is usable here (built: " + names + ")";
        }
    }
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

/// @brief The command line's choice of frontend, and the arguments that made it.
struct CommandLineChoice {
    /// @brief The last name given, or none.
    std::optional<std::string> name;
    /// @brief Whether a `--ui` was given without a name after it, or the last name given is empty.
    bool missingName = false;
    /// @brief The positions in the arguments of every `--ui=<name>`, `--ui` and the name after it, ascending.
    std::vector<std::size_t> used;
};

/// @brief Reads `--ui=<name>` and `--ui <name>` from the arguments, up to a `--`.
///
/// The last one wins. An argument starting with `--` is never taken as the name after `--ui`: it is the next option,
/// and the `--ui` before it has no name.
/// @param args The arguments; a program name among them is never a `--ui` flag.
/// @return The choice.
[[nodiscard]] inline CommandLineChoice readCommandLine(std::span<std::string const> args) {
    CommandLineChoice choice;
    bool nameFollows = false;
    for (std::size_t index = 0; std::string const& raw : args) {
        std::string_view const arg = raw;
        if (nameFollows) {
            nameFollows = false;
            if (arg.starts_with("--")) {
                choice.missingName = true;
                break;
            }
            choice.name = std::string{arg};
            choice.used.push_back(index);
        } else if (arg == "--") {
            break;
        } else if (arg == "--ui") {
            nameFollows = true;
            choice.used.push_back(index);
        } else if (arg.starts_with("--ui=")) {
            choice.name = std::string{arg.substr(std::string_view{"--ui="}.size())};
            choice.used.push_back(index);
        }
        ++index;
    }
    choice.missingName = choice.missingName || nameFollows || (choice.name.has_value() && choice.name->empty());
    return choice;
}

}  // namespace detail

/// @brief Picks the frontend to run: the last `--ui=<name>` (or `--ui <name>`) before a `--`, else a non-empty
///        `MORPH_UI`, else the first option in the given order whose `usable()` holds.
///
/// A frontend named on the command line or in `MORPH_UI` is used even when its `usable()` is false: the user asked
/// for it. On success every `--ui` argument (with the name after a bare `--ui`) is removed from @p args, and the rest
/// keep their order, for the caller to pass on; on an error @p args is left as it was.
/// @param built The frontends this binary was built with, in order of preference.
/// @param args The command-line arguments, with or without the program name first.
/// @param env Reads `MORPH_UI`; the process environment by default. An empty reader reads nothing.
/// @return The frontend, made; or why there is none.
[[nodiscard]] inline std::expected<std::unique_ptr<Frontend>, FrontendError> selectFrontend(
    std::span<FrontendOption const> built, std::vector<std::string>& args,
    EnvironmentReader const& env = processEnvironment()) {
    std::vector<std::string> names;
    names.reserve(built.size());
    for (FrontendOption const& option : built) {
        names.push_back(option.name);
    }
    auto const failure = [&names](FrontendError::Kind kind, std::string requested) {
        return std::unexpected{FrontendError{.kind = kind, .requested = std::move(requested), .built = names}};
    };
    detail::CommandLineChoice const choice = detail::readCommandLine(args);
    if (choice.missingName) {
        return failure(FrontendError::Kind::MissingName, {});
    }
    std::optional<std::string> requested = choice.name;
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
            return failure(FrontendError::Kind::UnknownName, *requested);
        }
        chosen = &*found;
    } else {
        auto const found = std::ranges::find_if(
            built, [](FrontendOption const& candidate) { return !candidate.usable || candidate.usable(); });
        if (found == built.end()) {
            return failure(FrontendError::Kind::NoneUsable, {});
        }
        chosen = &*found;
    }
    std::unique_ptr<Frontend> frontend = chosen->make ? chosen->make() : nullptr;
    if (frontend == nullptr) {
        return failure(FrontendError::Kind::NotMade, chosen->name);
    }
    for (std::size_t const index : choice.used | std::views::reverse) {
        args.erase(args.begin() + static_cast<std::ptrdiff_t>(index));
    }
    return frontend;
}

/// @brief What a frontend mounts once its source is open: the app shell and its screens, or a view of why the source
///        did not open. Destroying it unmounts all of it.
class MountedShell {
public:
    MountedShell() = default;
    virtual ~MountedShell() = default;
    MountedShell(MountedShell const&) = delete;
    MountedShell& operator=(MountedShell const&) = delete;
    MountedShell(MountedShell&&) = delete;
    MountedShell& operator=(MountedShell&&) = delete;
};

/// @brief Mounts the shell for what the source delivered. Called once, on the owner; it may return null to mount
///        nothing (after calling `AppContext::quit`, for example).
using ShellFactory =
    std::function<std::unique_ptr<MountedShell>(AppContext& context, std::expected<Bundle, ConnectError> opened)>;

/// @brief Runs one source inside a frontend: opens it, mounts the shell when it is ready, runs the loop, and tears
///        down in order.
///
/// The order is fixed here so that every frontend keeps it. When the loop returns, or throws, the shell is destroyed
/// first, while the source's connections that its screens use are still open, and the source is closed next, while
/// the runtime and the executor its handlers were made with are still alive; the frontend destroys those after this
/// returns.
///
/// The source's `ready` may come during `open` (a local source) or later from the loop (a remote one). A second
/// `ready` is ignored, and one that comes after the run has ended does nothing. A @p mountShell that throws quits the
/// context with exit code 1, and its exception leaves this call once everything is torn down.
/// @param context The frontend's context; its runtime must outlive this call.
/// @param source The source to open; closed before this returns.
/// @param mountShell Mounts the shell for what the source delivered.
/// @param loop Runs the frontend's event loop and returns the exit code once `AppContext::quit` ends it.
/// @return What @p loop returns.
/// @throws std::invalid_argument when @p mountShell or @p loop is empty; whatever `open`, @p mountShell, @p loop or
///         `close` throws, the first of them.
[[nodiscard]] inline int runApp(AppContext& context, AppSource& source, ShellFactory const& mountShell,
                                std::function<int()> const& loop) {
    if (!mountShell || !loop) {
        throw std::invalid_argument{"morph::ui::runApp: the shell factory and the loop must not be empty"};
    }
    struct Run {
        std::unique_ptr<MountedShell> shell;
        bool opened = false;
        std::exception_ptr mountFailure;
    };
    auto const run = std::make_shared<Run>();
    std::exception_ptr failure;
    int exitCode = 0;
    try {
        source.open(context,
                    [weak = std::weak_ptr{run}, &context, &mountShell](std::expected<Bundle, ConnectError> opened) {
                        std::shared_ptr<Run> const live = weak.lock();
                        if (live == nullptr || live->opened) {
                            return;
                        }
                        live->opened = true;
                        try {
                            live->shell = mountShell(context, std::move(opened));
                        } catch (...) {
                            live->mountFailure = std::current_exception();
                            context.quit(1);
                        }
                    });
        exitCode = loop();
    } catch (...) {
        failure = std::current_exception();
    }
    // The shell goes first, and the run counts as opened from here on, so a `ready` that arrives while the source
    // closes mounts nothing; one that arrives after this returns finds the run gone.
    std::exception_ptr const mountFailure = run->mountFailure;
    run->shell.reset();
    run->opened = true;
    try {
        source.close();
    } catch (...) {
        if (!failure) {
            failure = std::current_exception();
        }
    }
    if (!failure) {
        failure = mountFailure;
    }
    if (failure) {
        std::rethrow_exception(failure);
    }
    return exitCode;
}

}  // namespace morph::ui

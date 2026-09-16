// SPDX-License-Identifier: Apache-2.0

#pragma once
#include <catch2/catch_session.hpp>
#include <catch2/internal/catch_clara.hpp>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <morph/core/logger.hpp>
#include <optional>
#include <print>
#include <string>
#include <string_view>

namespace morph::testkit {

/// @brief Name of the environment variable that sets the suite's log level
///        when `--log-level` is absent.
///
/// CI runs every suite through `ctest`, and `catch_discover_tests` registers
/// each test case as its own ctest invocation -- there is no place to thread an
/// extra command-line argument through. An environment variable is the only
/// input that reaches all of them at once, which is what `.github/workflows/
/// ci.yml`'s workflow-level `env:` block uses.
inline constexpr std::string_view kLogLevelEnvVar = "MORPH_TEST_LOG_LEVEL";

/// @brief Where `applyLogLevel` took the suite's level from.
enum class LogLevelSource : std::uint8_t {
    /// @brief `--log-level` was given on the command line.
    commandLine,
    /// @brief `MORPH_TEST_LOG_LEVEL` was set and `--log-level` was not.
    environment,
    /// @brief Neither input was given, so the quiet default applied.
    fallbackDefault,
};

namespace detail {

/// @brief Storage for where the resolved level came from.
inline LogLevelSource& resolvedSourceStorage() {
    static LogLevelSource source = LogLevelSource::fallbackDefault;
    return source;
}

/// @brief Storage for the level this binary resolved at startup.
///
/// `getLogLevel()` cannot answer "what did the gate decide", because any test
/// that installs a `ScopedLoggerOverride` changes it and restores it. This is
/// written once, before the run begins, and never again.
inline morph::log::LogLevel& resolvedLevelStorage() {
    static morph::log::LogLevel level = morph::log::LogLevel::off;
    return level;
}

/// @brief Maps a level name to its enumerator.
/// @param name Level name, lower-case (`debug`, `info`, `warn`, `error`, `off`).
/// @return The matching level, or `std::nullopt` if @p name is not one of them.
inline std::optional<morph::log::LogLevel> parseLevel(std::string_view name) {
    using morph::log::LogLevel;
    if (name == "debug") {
        return LogLevel::debug;
    }
    if (name == "info") {
        return LogLevel::info;
    }
    if (name == "warn") {
        return LogLevel::warn;
    }
    if (name == "error") {
        return LogLevel::error;
    }
    if (name == "off") {
        return LogLevel::off;
    }
    return std::nullopt;
}

}  // namespace detail

/// @brief The level this binary's `main` applied before the run started.
///
/// Exposed so a test can prove the gate actually ran rather than assuming it
/// linked: if `main` never applied a level, the ambient level would be the
/// library default (`warn`), which differs from the gate's default (`off`).
/// @return The resolved level.
[[nodiscard]] inline morph::log::LogLevel resolvedLogLevel() { return detail::resolvedLevelStorage(); }

/// @brief Which input supplied the level `resolvedLogLevel()` reports.
///
/// Lets a test assert the quiet default exactly, instead of asserting it and
/// failing spuriously the moment someone runs with `--log-level=debug` or under
/// CI, where `MORPH_TEST_LOG_LEVEL` is set.
/// @return The source of the resolved level.
[[nodiscard]] inline LogLevelSource resolvedLogLevelSource() { return detail::resolvedSourceStorage(); }

/// @brief Adds `--log-level` to @p session's command line.
///
/// A Catch2 *event listener* cannot do this -- the parser is built before
/// listeners exist -- so every morph test binary owns its `main` and calls
/// this, rather than linking `Catch2::Catch2WithMain`.
///
/// @param session Session whose parser gains the option.
/// @param sink    Receives the option's value; must outlive @p session's parse.
inline void addLogLevelOption(Catch::Session& session, std::string& sink) {
    session.cli(session.cli() | Catch::Clara::Opt(sink, "level")["--log-level"](
                                    "morph log level: debug|info|warn|error|off (default: off). "
                                    "Overrides $MORPH_TEST_LOG_LEVEL."));
}

/// @brief Resolves and installs the suite's log level.
///
/// `--log-level` wins over the environment, so a developer debugging locally is
/// never overridden by a variable they exported and forgot. When neither is
/// given the level is `off`: morph's own suite exercises error paths on
/// purpose, and those `[ERROR]`/`[WARN ]` records are noise in a test run, not
/// failures -- Catch2 assertions are how a test reports a problem.
///
/// @param cliValue Value of `--log-level`, empty when the option was absent.
/// @return `true` when a level was installed; `false` when @p cliValue or the
///         environment variable named an unknown level, in which case a
///         diagnostic naming the accepted values has been written to stderr and
///         the caller should exit non-zero rather than run the suite at a level
///         the developer did not ask for.
[[nodiscard]] inline bool applyLogLevel(std::string_view cliValue) {
    std::string_view source = "--log-level";
    std::string_view requested = cliValue;
    detail::resolvedSourceStorage() = LogLevelSource::commandLine;
    if (requested.empty()) {
        source = kLogLevelEnvVar;
        detail::resolvedSourceStorage() = LogLevelSource::environment;
        // Read once, from `main`, before Catch2 has started a single test -- so
        // the environment cannot be mutated concurrently with this call, which
        // is the hazard concurrency-mt-unsafe exists to flag for getenv.
        // NOLINTNEXTLINE(concurrency-mt-unsafe)
        if (const char* fromEnv = std::getenv(std::string{kLogLevelEnvVar}.c_str()); fromEnv != nullptr) {
            requested = fromEnv;
        }
    }
    if (requested.empty()) {
        detail::resolvedSourceStorage() = LogLevelSource::fallbackDefault;
        detail::resolvedLevelStorage() = morph::log::LogLevel::off;
        morph::log::setLogLevel(morph::log::LogLevel::off);
        return true;
    }
    const auto parsed = detail::parseLevel(requested);
    if (!parsed.has_value()) {
        std::println(stderr, "error: {}: unknown log level '{}'; expected one of debug, info, warn, error, off",
                     source, requested);
        return false;
    }
    detail::resolvedLevelStorage() = *parsed;
    morph::log::setLogLevel(*parsed);
    return true;
}

/// @brief Registers `--log-level`, parses @p argv, and installs the level.
///
/// The one call every morph test `main` makes between constructing its session
/// and running it. Kept as a single entry point because the Qt-owning mains
/// (which must construct a `QCoreApplication` first and drain deferred deletes
/// afterwards) would otherwise each re-derive the same three steps and drift.
///
/// @param session Session to configure and parse into.
/// @param argc    Argument count, as given to `main`.
/// @param argv    Argument vector, as given to `main`.
/// @return `std::nullopt` when the caller should proceed to `session.run()`;
///         otherwise the exit code `main` should return immediately -- Catch2's
///         own code for a command-line error or an informational query such as
///         `--help`, or 1 for an unusable `--log-level` value.
[[nodiscard]] inline std::optional<int> configureSession(Catch::Session& session, int argc, char* argv[]) {
    std::string level;
    addLogLevelOption(session, level);
    if (const int rc = session.applyCommandLine(argc, argv); rc != 0) {
        return rc;
    }
    if (!applyLogLevel(level)) {
        return 1;
    }
    return std::nullopt;
}

/// @brief Configures and runs @p session, letting no exception escape.
///
/// `main` must not throw: `bugprone-exception-escape` flags it, and an
/// exception unwinding out of `main` runs no destructor for anything already
/// on the stack -- which for the Qt-owning suites means the `QCoreApplication`
/// is torn down by the runtime rather than by its own scope exit, the very
/// ordering those mains exist to control.
///
/// Everything reachable from here can throw: `std::format` (via the diagnostic
/// `applyLogLevel` prints), Clara's parser, and every Catch2 assertion that
/// escapes a test's own handler. Catching here rather than in each `main` keeps
/// the four Qt-owning mains and the shared one on exactly one implementation.
///
/// @param session Session to configure and run.
/// @param argc    Argument count, as given to `main`.
/// @param argv    Argument vector, as given to `main`.
/// @return Catch2's exit code, or 1 if something threw or the command line was
///         unusable.
[[nodiscard]] inline int runSession(Catch::Session& session, int argc, char* argv[]) noexcept {
    try {
        if (const auto exitCode = configureSession(session, argc, argv)) {
            return *exitCode;
        }
        return session.run();
    } catch (const std::exception& ex) {
        // fputs, not println: this is the handler of last resort, and
        // std::format is one of the things that can have thrown to get here.
        std::fputs("fatal: unhandled exception escaped the test session: ", stderr);
        std::fputs(ex.what(), stderr);
        std::fputs("\n", stderr);
        return 1;
    } catch (...) {
        std::fputs("fatal: unhandled non-std exception escaped the test session\n", stderr);
        return 1;
    }
}

}  // namespace morph::testkit

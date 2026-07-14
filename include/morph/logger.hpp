// SPDX-License-Identifier: Apache-2.0

#pragma once
#include <format>
#include <functional>
#include <mutex>
#include <print>
#include <string_view>

namespace morph::log {

/// @brief Severity levels for the logging system.
enum class LogLevel : std::uint8_t {
    /// @brief Fine-grained diagnostic output.
    debug = 0,
    /// @brief General informational messages.
    info = 1,
    /// @brief Recoverable conditions worth noting.
    warn = 2,
    /// @brief Errors that should be investigated.
    error = 3,
    /// @brief Suppresses all output when used as the minimum level.
    off = 4,
};

namespace detail {

/// @brief Returns the human-readable name for a log level.
constexpr std::string_view levelName(LogLevel level) noexcept {
    switch (level) {
        case LogLevel::debug:
            return "DEBUG";
        case LogLevel::info:
            return "INFO ";
        case LogLevel::warn:
            return "WARN ";
        case LogLevel::error:
            return "ERROR";
        case LogLevel::off:
            return "OFF  ";
    }
    return "?    ";
}

/// @brief Sink function type used internally for log output.
using Logger = std::function<void(LogLevel, std::string_view)>;

struct LogState {
    Logger sink = [](LogLevel lvl, std::string_view msg) { std::println(stderr, "[{}] {}", levelName(lvl), msg); };
    LogLevel minLevel = LogLevel::debug;
    std::mutex mtx;
};

inline LogState& logState() {
    static LogState state;
    return state;
}

/// @brief Internal emit entry point used by the public level helpers.
inline void log(LogLevel level, std::string_view msg) {
    std::scoped_lock const lock{logState().mtx};
    auto& state = logState();
    if (state.sink && level >= state.minLevel) {
        state.sink(level, msg);
    }
}

}  // namespace detail

// ── Configuration ─────────────────────────────────────────────────────────────

/// @brief Replaces the global log sink.
///
/// Thread-safe. The new sink is called for every message whose level meets the
/// current minimum. Pass a no-op lambda to silence all output.
/// @param logger New sink function.
inline void setLogger(std::function<void(LogLevel, std::string_view)> logger) {
    std::scoped_lock const lock{detail::logState().mtx};
    detail::logState().sink = std::move(logger);
}

/// @brief Sets the minimum log level.
///
/// Messages below this level are silently dropped. Thread-safe.
/// @param level Minimum level to emit.
inline void setLogLevel(LogLevel level) {
    std::scoped_lock const lock{detail::logState().mtx};
    detail::logState().minLevel = level;
}

/// @brief Returns the current minimum log level. Thread-safe.
/// @return The active minimum level.
inline LogLevel getLogLevel() {
    std::scoped_lock const lock{detail::logState().mtx};
    return detail::logState().minLevel;
}

// ── Level helpers ─────────────────────────────────────────────────────────────

namespace detail {

/// @brief Formats @p fmt with @p args and forwards to `log()`.
template <typename... Args>
void logFormat(LogLevel level, std::format_string<Args...> fmt, Args&&... args) {
    log(level, std::format(fmt, std::forward<Args>(args)...));
}

}  // namespace detail

/// @brief Logs @p msg at `LogLevel::debug`.
inline void logDebug(std::string_view msg) { detail::log(LogLevel::debug, msg); }
/// @brief Logs @p msg at `LogLevel::info`.
inline void logInfo(std::string_view msg) { detail::log(LogLevel::info, msg); }
/// @brief Logs @p msg at `LogLevel::warn`.
inline void logWarn(std::string_view msg) { detail::log(LogLevel::warn, msg); }
/// @brief Logs @p msg at `LogLevel::error`.
inline void logError(std::string_view msg) { detail::log(LogLevel::error, msg); }

/// @brief Logs a formatted message at `LogLevel::debug`.
template <typename... Args>
void logDebug(std::format_string<Args...> fmt, Args&&... args) {
    detail::logFormat(LogLevel::debug, fmt, std::forward<Args>(args)...);
}
/// @brief Logs a formatted message at `LogLevel::info`.
template <typename... Args>
void logInfo(std::format_string<Args...> fmt, Args&&... args) {
    detail::logFormat(LogLevel::info, fmt, std::forward<Args>(args)...);
}
/// @brief Logs a formatted message at `LogLevel::warn`.
template <typename... Args>
void logWarn(std::format_string<Args...> fmt, Args&&... args) {
    detail::logFormat(LogLevel::warn, fmt, std::forward<Args>(args)...);
}
/// @brief Logs a formatted message at `LogLevel::error`.
template <typename... Args>
void logError(std::format_string<Args...> fmt, Args&&... args) {
    detail::logFormat(LogLevel::error, fmt, std::forward<Args>(args)...);
}

// ── Scoped override (test fixture) ────────────────────────────────────────────

/// @brief RAII helper that swaps the global logger and level for the lifetime
///        of the object, and restores them in the destructor.
///
/// Designed for tests that want to capture log output without leaking the
/// custom sink into other tests. Thread-safe construction and destruction —
/// the global mutex is acquired briefly during each.
///
/// @par Example
/// @code
/// {
///     std::vector<std::string> captured;
///     morph::log::ScopedLoggerOverride guard{
///         [&](morph::log::LogLevel, std::string_view msg) { captured.emplace_back(msg); },
///         morph::log::LogLevel::debug,
///     };
///     // ... run code that logs ...
/// }  // previous sink + level restored here
/// @endcode
class ScopedLoggerOverride {
public:
    /// @brief Snapshots the current sink + level so they can be restored.
    ///
    /// Use this when test code will install its own sink mid-test via
    /// `setLogger()` / `setLogLevel()` and just wants automatic restoration.
    ScopedLoggerOverride() : _savedSink(detail::logState().sink), _savedLevel(detail::logState().minLevel) {
        std::scoped_lock const lock{detail::logState().mtx};
        
        
    }

    /// @brief Installs @p sink and @p level; saves whatever was there before.
    ///
    /// @param sink  New logger sink. Pass a no-op lambda to suppress output entirely.
    /// @param level New minimum level. Defaults to `debug` so every message reaches @p sink.
    explicit ScopedLoggerOverride(std::function<void(LogLevel, std::string_view)> sink,
                                  LogLevel level = LogLevel::debug) : _savedSink(std::move(detail::logState().sink)), _savedLevel(detail::logState().minLevel) {
        std::scoped_lock const lock{detail::logState().mtx};
        
        
        detail::logState().sink = std::move(sink);
        detail::logState().minLevel = level;
    }

    /// @brief Restores the saved sink and level.
    ~ScopedLoggerOverride() {
        std::scoped_lock const lock{detail::logState().mtx};
        detail::logState().sink = std::move(_savedSink);
        detail::logState().minLevel = _savedLevel;
    }

    ScopedLoggerOverride(const ScopedLoggerOverride&) = delete;
    ScopedLoggerOverride& operator=(const ScopedLoggerOverride&) = delete;
    ScopedLoggerOverride(ScopedLoggerOverride&&) = delete;
    ScopedLoggerOverride& operator=(ScopedLoggerOverride&&) = delete;

private:
    std::function<void(LogLevel, std::string_view)> _savedSink;
    LogLevel _savedLevel{LogLevel::debug};
};

}  // namespace morph::log

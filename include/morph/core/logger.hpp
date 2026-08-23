// SPDX-License-Identifier: Apache-2.0

#pragma once
#include <atomic>
#include <cstdint>
#include <format>
#include <functional>
#include <mutex>
#include <print>
#include <string>
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
        default:
            return "?    ";
    }
}

/// @brief Sink function type used internally for log output.
using Logger = std::function<void(LogLevel, std::string_view)>;

/// @brief Escapes newline and control characters so a message cannot forge log
///        lines (log injection).
///
/// A message is a single logical record; the default sink emits it as one line.
/// User-controlled text containing \n, \r, or other C0 control bytes could
/// otherwise splice a forged `[ERROR] ...` line into the stream or corrupt a
/// line-oriented log parser. This replaces CR/LF/TAB with their C-style escapes
/// (\n, \r, \t) and any remaining control byte (< 0x20, or 0x7f DEL)
/// with a \xHH escape, leaving printable text (including non-ASCII UTF-8
/// continuation bytes `>= 0x80`) untouched. It is a cheap single pass with no
/// per-byte escaping work on the common (clean) path (just one string copy).
/// @param msg Raw message to sanitize.
/// @return @p msg with control characters escaped; the same bytes if already clean.
inline std::string sanitizeLogLine(std::string_view msg) {
    auto needsEscape = [](unsigned char chr) { return chr < 0x20 || chr == 0x7f; };
    std::size_t firstBad = std::string_view::npos;
    for (std::size_t i = 0; i < msg.size(); ++i) {
        if (needsEscape(static_cast<unsigned char>(msg[i]))) {
            firstBad = i;
            break;
        }
    }
    if (firstBad == std::string_view::npos) {
        return std::string{msg};  // clean: one copy, no per-char work
    }
    std::string out;
    out.reserve(msg.size() + 8);
    out.append(msg.substr(0, firstBad));
    static constexpr std::string_view kHex = "0123456789abcdef";
    for (std::size_t i = firstBad; i < msg.size(); ++i) {
        const auto chr = static_cast<unsigned char>(msg[i]);
        switch (chr) {
            case '\n':
                out += "\\n";
                break;
            case '\r':
                out += "\\r";
                break;
            case '\t':
                out += "\\t";
                break;
            default:
                if (needsEscape(chr)) {
                    out += "\\x";
                    out.push_back(kHex[chr >> 4]);
                    out.push_back(kHex[chr & 0x0f]);
                } else {
                    out.push_back(static_cast<char>(chr));
                }
                break;
        }
    }
    return out;
}

struct LogState {
    Logger sink = [](LogLevel lvl, std::string_view msg) {
        std::println(stderr, "[{}] {}", levelName(lvl), sanitizeLogLine(msg));
    };
    // Atomic so the level check is a lock-free fast path: `logFormat` and `log`
    // can reject a suppressed message without touching `mtx` or formatting it.
    std::atomic<LogLevel> minLevel{LogLevel::debug};
    std::mutex mtx;  // guards `sink` (and serialises sink invocation)
    // Records discarded because the sink, the lock, or formatting threw. The
    // logging layer is noexcept, so a failure cannot be reported by
    // propagating; this counter is what keeps it from being silent.
    std::atomic<std::uint64_t> dropped{0};
};

inline LogState& logState() {
    static LogState state;
    return state;
}

/// @brief Internal emit entry point used by the public level helpers.
///
/// The sink is invoked while `mtx` is held, so sink calls are serialised. A sink
/// must therefore not call back into `morph::log` (log, `setLogger`, or
/// construct a `ScopedLoggerOverride`) — `std::mutex` is non-recursive and that
/// would self-deadlock — and should not block for long.
inline void log(LogLevel level, std::string_view msg) noexcept {
    auto& state = logState();
    if (level < state.minLevel.load(std::memory_order_relaxed)) {
        return;  // lock-free reject of suppressed messages
    }
    // Nothing here may escape. Emitting a log record must never be able to
    // disrupt the code that asked for it -- least of all a destructor, where
    // an escaping exception is `std::terminate`. Three things inside can
    // throw: `std::mutex::lock` (`std::system_error`), the sink itself, and
    // any allocation the sink performs (the default one builds a sanitised
    // copy). All three are the logging layer's problem, not the caller's.
    try {
        std::scoped_lock const lock{state.mtx};
        if (state.sink) {
            state.sink(level, msg);
        }
    } catch (...) {
        state.dropped.fetch_add(1, std::memory_order_relaxed);
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
    detail::logState().minLevel.store(level, std::memory_order_relaxed);
}

/// @brief Returns the current minimum log level. Thread-safe.
/// @return The active minimum level.
inline LogLevel getLogLevel() {
    return detail::logState().minLevel.load(std::memory_order_relaxed);
}

/// @brief Number of log records discarded because emitting them threw.
///
/// The logging helpers are `noexcept`, so a sink that throws, a lock
/// acquisition that fails, or a formatting error cannot be reported by
/// propagating to the caller. Each such record increments this counter
/// instead, so the failure is observable rather than silent. It counts
/// *failures*, not level-suppressed messages: a message below the minimum
/// level is not a dropped record.
///
/// Monotonic for the life of the process, and never reset. Compare two reads
/// to attribute drops to a region of code. Thread-safe.
///
/// @return The total number of records dropped since process start.
[[nodiscard]] inline std::uint64_t droppedLogRecords() noexcept {
    return detail::logState().dropped.load(std::memory_order_relaxed);
}

// ── Level helpers ─────────────────────────────────────────────────────────────

namespace detail {

/// @brief Formats @p fmt with @p args and forwards to `log()`.
///
/// The level is checked *before* formatting, so a suppressed call pays no
/// `std::format` / allocation cost.
template <typename... Args>
void logFormat(LogLevel level, std::format_string<Args...> fmt, Args&&... args) noexcept {
    if (level < logState().minLevel.load(std::memory_order_relaxed)) {
        return;
    }
    // `std::format` allocates, so it can throw `std::bad_alloc`, and a user
    // type's `formatter` may throw anything at all. `log()` guards its own
    // body but cannot guard its argument, so the call is wrapped here.
    try {
        log(level, std::format(fmt, std::forward<Args>(args)...));
    } catch (...) {
        logState().dropped.fetch_add(1, std::memory_order_relaxed);
    }
}

}  // namespace detail

/// @brief Logs @p msg at `LogLevel::debug`.
inline void logDebug(std::string_view msg) noexcept { detail::log(LogLevel::debug, msg); }
/// @brief Logs @p msg at `LogLevel::info`.
inline void logInfo(std::string_view msg) noexcept { detail::log(LogLevel::info, msg); }
/// @brief Logs @p msg at `LogLevel::warn`.
inline void logWarn(std::string_view msg) noexcept { detail::log(LogLevel::warn, msg); }
/// @brief Logs @p msg at `LogLevel::error`.
inline void logError(std::string_view msg) noexcept { detail::log(LogLevel::error, msg); }

/// @brief Logs a formatted message at `LogLevel::debug`.
template <typename... Args>
void logDebug(std::format_string<Args...> fmt, Args&&... args) noexcept {
    detail::logFormat(LogLevel::debug, fmt, std::forward<Args>(args)...);
}
/// @brief Logs a formatted message at `LogLevel::info`.
template <typename... Args>
void logInfo(std::format_string<Args...> fmt, Args&&... args) noexcept {
    detail::logFormat(LogLevel::info, fmt, std::forward<Args>(args)...);
}
/// @brief Logs a formatted message at `LogLevel::warn`.
template <typename... Args>
void logWarn(std::format_string<Args...> fmt, Args&&... args) noexcept {
    detail::logFormat(LogLevel::warn, fmt, std::forward<Args>(args)...);
}
/// @brief Logs a formatted message at `LogLevel::error`.
template <typename... Args>
void logError(std::format_string<Args...> fmt, Args&&... args) noexcept {
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
    ScopedLoggerOverride() {
        // NOLINTBEGIN(cppcoreguidelines-prefer-member-initializer) — these must be
        // read while holding the lock; a member-initializer list would read the
        // global state before the mutex is acquired (a data race).
        std::scoped_lock const lock{detail::logState().mtx};
        _savedSink = detail::logState().sink;
        _savedLevel = detail::logState().minLevel;
        // NOLINTEND(cppcoreguidelines-prefer-member-initializer)
    }

    /// @brief Installs @p sink and @p level; saves whatever was there before.
    ///
    /// @param sink  New logger sink. Pass a no-op lambda to suppress output entirely.
    /// @param level New minimum level. Defaults to `debug` so every message reaches @p sink.
    explicit ScopedLoggerOverride(std::function<void(LogLevel, std::string_view)> sink,
                                  LogLevel level = LogLevel::debug) {
        // NOLINTBEGIN(cppcoreguidelines-prefer-member-initializer) — snapshot and
        // install happen under one lock; a member-initializer list would read the
        // previous state outside the mutex, racing a concurrent setLogger.
        std::scoped_lock const lock{detail::logState().mtx};
        _savedSink = std::move(detail::logState().sink);
        _savedLevel = detail::logState().minLevel;
        detail::logState().sink = std::move(sink);
        detail::logState().minLevel = level;
        // NOLINTEND(cppcoreguidelines-prefer-member-initializer)
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

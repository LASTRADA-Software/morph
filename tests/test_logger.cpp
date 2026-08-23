// SPDX-License-Identifier: Apache-2.0

#include <morph/core/logger.hpp>
#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <format>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

using LogGuard = morph::log::ScopedLoggerOverride;

// ── morph::log::detail::levelName ─────────────────────────────────────────────────────────────────

TEST_CASE("morph::log::detail::levelName returns correct label for every level", "[logger]") {
    REQUIRE(morph::log::detail::levelName(morph::log::LogLevel::debug) == "DEBUG");
    REQUIRE(morph::log::detail::levelName(morph::log::LogLevel::info) == "INFO ");
    REQUIRE(morph::log::detail::levelName(morph::log::LogLevel::warn) == "WARN ");
    REQUIRE(morph::log::detail::levelName(morph::log::LogLevel::error) == "ERROR");
    REQUIRE(morph::log::detail::levelName(morph::log::LogLevel::off) == "OFF  ");
}

// ── morph::log::setLogger / custom sink ───────────────────────────────────────────────────

TEST_CASE("morph::log::setLogger: custom sink receives level and message", "[logger]") {
    LogGuard guard;
    morph::log::LogLevel capturedLevel = morph::log::LogLevel::off;
    std::string capturedMsg;

    morph::log::setLogger([&](morph::log::LogLevel lvl, std::string_view msg) {
        capturedLevel = lvl;
        capturedMsg = std::string{msg};
    });

    morph::log::logInfo("hello info");
    REQUIRE(capturedLevel == morph::log::LogLevel::info);
    REQUIRE(capturedMsg == "hello info");
}

namespace {
struct LogEntry {
    morph::log::LogLevel level;
    std::string msg;
};
struct SpySink {
    std::vector<LogEntry> entries;
    void write(morph::log::LogLevel lvl, std::string_view msg) { entries.push_back({.level = lvl, .msg = std::string{msg}}); }
};
}  // namespace

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST_CASE("morph::log::setLogger: injecting a custom backend (dependency injection)", "[logger]") {
    LogGuard guard;
    SpySink spy;
    morph::log::setLogger([&spy](morph::log::LogLevel lvl, std::string_view msg) { spy.write(lvl, msg); });
    morph::log::setLogLevel(morph::log::LogLevel::debug);

    morph::log::logDebug("startup complete");
    morph::log::logInfo("model registered");
    morph::log::logWarn("slow action");
    morph::log::logError("action failed");

    REQUIRE(spy.entries.size() == 4);
    REQUIRE(spy.entries[0].level == morph::log::LogLevel::debug);
    REQUIRE(spy.entries[0].msg == "startup complete");
    REQUIRE(spy.entries[1].level == morph::log::LogLevel::info);
    REQUIRE(spy.entries[1].msg == "model registered");
    REQUIRE(spy.entries[2].level == morph::log::LogLevel::warn);
    REQUIRE(spy.entries[2].msg == "slow action");
    REQUIRE(spy.entries[3].level == morph::log::LogLevel::error);
    REQUIRE(spy.entries[3].msg == "action failed");
}

TEST_CASE("morph::log::setLogger: null sink is a no-op and does not crash", "[logger]") {
    LogGuard guard;
    morph::log::setLogger(nullptr);
    morph::log::logError("silent");
    REQUIRE(true);
}

// ── morph::log::setLogLevel / filtering ───────────────────────────────────────────────────

TEST_CASE("morph::log::setLogLevel: messages below threshold are suppressed", "[logger]") {
    LogGuard guard;
    int callCount = 0;
    morph::log::setLogger([&](morph::log::LogLevel, std::string_view) { ++callCount; });
    morph::log::setLogLevel(morph::log::LogLevel::warn);

    morph::log::logDebug("not emitted");
    morph::log::logInfo("not emitted");
    morph::log::logWarn("emitted");
    morph::log::logError("emitted");

    REQUIRE(callCount == 2);
}

TEST_CASE("morph::log::setLogLevel: off suppresses everything", "[logger]") {
    LogGuard guard;
    int callCount = 0;
    morph::log::setLogger([&](morph::log::LogLevel, std::string_view) { ++callCount; });
    morph::log::setLogLevel(morph::log::LogLevel::off);

    morph::log::logDebug("x");
    morph::log::logInfo("x");
    morph::log::logWarn("x");
    morph::log::logError("x");

    REQUIRE(callCount == 0);
}

TEST_CASE("morph::log::setLogLevel: debug passes all levels through", "[logger]") {
    LogGuard guard;
    int callCount = 0;
    morph::log::setLogger([&](morph::log::LogLevel, std::string_view) { ++callCount; });
    morph::log::setLogLevel(morph::log::LogLevel::debug);

    morph::log::logDebug("a");
    morph::log::logInfo("b");
    morph::log::logWarn("c");
    morph::log::logError("d");

    REQUIRE(callCount == 4);
}

TEST_CASE("morph::log::getLogLevel returns the currently configured level", "[logger]") {
    LogGuard guard;
    morph::log::setLogLevel(morph::log::LogLevel::warn);
    REQUIRE(morph::log::getLogLevel() == morph::log::LogLevel::warn);

    morph::log::setLogLevel(morph::log::LogLevel::error);
    REQUIRE(morph::log::getLogLevel() == morph::log::LogLevel::error);
}

// ── Level helpers emit correct morph::log::LogLevel ───────────────────────────────────────

TEST_CASE("morph::log::logDebug/Info/Warn/Error pass the right morph::log::LogLevel to the sink", "[logger]") {
    LogGuard guard;
    morph::log::setLogLevel(morph::log::LogLevel::debug);

    std::vector<morph::log::LogLevel> captured;
    morph::log::setLogger([&](morph::log::LogLevel lvl, std::string_view) { captured.push_back(lvl); });

    morph::log::logDebug("d");
    morph::log::logInfo("i");
    morph::log::logWarn("w");
    morph::log::logError("e");

    REQUIRE(captured.size() == 4);
    REQUIRE(captured[0] == morph::log::LogLevel::debug);
    REQUIRE(captured[1] == morph::log::LogLevel::info);
    REQUIRE(captured[2] == morph::log::LogLevel::warn);
    REQUIRE(captured[3] == morph::log::LogLevel::error);
}

// ── log(level, msg) core overload ─────────────────────────────────────────────

TEST_CASE("log(level, msg) respects threshold and delegates to sink", "[logger]") {
    LogGuard guard;
    morph::log::setLogLevel(morph::log::LogLevel::error);

    std::string last;
    morph::log::setLogger([&](morph::log::LogLevel, std::string_view msg) { last = std::string{msg}; });

    morph::log::detail::log(morph::log::LogLevel::warn, "suppressed");
    REQUIRE(last.empty());

    morph::log::detail::log(morph::log::LogLevel::error, "emitted");
    REQUIRE(last == "emitted");
}

// ── logFormat (format-string level helpers) ───────────────────────────────────

TEST_CASE("morph::log::logWarn(fmt, args...): suppressed below threshold without formatting or invoking the sink",
          "[logger]") {
    LogGuard guard;
    int callCount = 0;
    morph::log::setLogger([&](morph::log::LogLevel, std::string_view) { ++callCount; });
    morph::log::setLogLevel(morph::log::LogLevel::error);

    // Below the "error" threshold: logFormat's own level check must reject
    // this before std::format runs or the sink is touched.
    morph::log::logWarn("value={}", 42);
    REQUIRE(callCount == 0);

    morph::log::setLogLevel(morph::log::LogLevel::warn);
    std::string last;
    morph::log::setLogger([&](morph::log::LogLevel, std::string_view msg) { last = std::string{msg}; });
    morph::log::logWarn("value={}", 42);
    REQUIRE(last == "value=42");
}

// ── Thread safety ─────────────────────────────────────────────────────────────

TEST_CASE("concurrent log calls are thread-safe", "[logger]") {
    LogGuard guard;
    std::atomic<int> count{0};
    morph::log::setLogger([&](morph::log::LogLevel, std::string_view) { count.fetch_add(1, std::memory_order_relaxed); });
    morph::log::setLogLevel(morph::log::LogLevel::debug);

    constexpr int numThreads = 8;
    constexpr int msgsPerThread = 200;
    std::vector<std::thread> threads;
    threads.reserve(numThreads);

    for (int i = 0; i < numThreads; ++i) {
        threads.emplace_back([&] {
            for (int j = 0; j < msgsPerThread; ++j) {
                morph::log::logDebug("d");
                morph::log::logInfo("i");
                morph::log::logWarn("w");
                morph::log::logError("e");
            }
        });
    }
    for (auto& thr : threads) {
        thr.join();
    }

    REQUIRE(count.load() == numThreads * msgsPerThread * 4);
}

// ── The noexcept guarantee (morph#158) ────────────────────────────────────────

// Compile-time, and the part that cannot rot: if any entry point loses
// `noexcept`, this fails to build rather than failing subtly at some call site
// in a destructor. These assert the *declared* guarantee; the TEST_CASEs below
// assert that it actually holds when the sink throws.
static_assert(noexcept(morph::log::logDebug(std::string_view{})));
static_assert(noexcept(morph::log::logInfo(std::string_view{})));
static_assert(noexcept(morph::log::logWarn(std::string_view{})));
static_assert(noexcept(morph::log::logError(std::string_view{})));
// The variadic overloads are asserted through a pre-built `format_string`
// rather than a literal. `noexcept(expr)` covers the argument conversions too,
// and `std::basic_format_string`'s constructor -- though `consteval`, so it
// cannot throw at run time -- is not itself declared `noexcept`. Writing
// `noexcept(logDebug("{}", 1))` therefore reports false about the *conversion*
// while saying nothing about the function, which is what is under test here.
inline constexpr std::format_string<int> kIntFmt{"{}"};
static_assert(noexcept(morph::log::logDebug(kIntFmt, 1)));
static_assert(noexcept(morph::log::logInfo(kIntFmt, 1)));
static_assert(noexcept(morph::log::logWarn(kIntFmt, 1)));
static_assert(noexcept(morph::log::logError(kIntFmt, 1)));
static_assert(noexcept(morph::log::detail::logFormat(morph::log::LogLevel::error, kIntFmt, 1)));
static_assert(noexcept(morph::log::detail::log(morph::log::LogLevel::error, std::string_view{})));
static_assert(noexcept(morph::log::droppedLogRecords()));

TEST_CASE("morph::log: a throwing sink does not propagate to the caller", "[logger]") {
    LogGuard guard;
    int calls = 0;
    morph::log::setLogger([&](morph::log::LogLevel, std::string_view) {
        ++calls;
        throw std::runtime_error{"sink is broken"};
    });
    morph::log::setLogLevel(morph::log::LogLevel::debug);

    // Every public entry point, both overload families. Before morph#158 each
    // of these unwound into the caller.
    REQUIRE_NOTHROW(morph::log::logDebug("plain"));
    REQUIRE_NOTHROW(morph::log::logInfo("plain"));
    REQUIRE_NOTHROW(morph::log::logWarn("plain"));
    REQUIRE_NOTHROW(morph::log::logError("plain"));
    REQUIRE_NOTHROW(morph::log::logDebug("fmt {}", 1));
    REQUIRE_NOTHROW(morph::log::logInfo("fmt {}", 1));
    REQUIRE_NOTHROW(morph::log::logWarn("fmt {}", 1));
    REQUIRE_NOTHROW(morph::log::logError("fmt {}", 1));

    // The sink really was reached each time -- otherwise this test would pass
    // just as well against a logger that silently dropped everything.
    REQUIRE(calls == 8);
}

TEST_CASE("morph::log: a record lost to a throwing sink is counted", "[logger]") {
    LogGuard guard;
    morph::log::setLogLevel(morph::log::LogLevel::debug);

    const auto before = morph::log::droppedLogRecords();

    morph::log::setLogger([](morph::log::LogLevel, std::string_view) { throw std::runtime_error{"nope"}; });
    morph::log::logError("one");
    morph::log::logError("two {}", 3);

    REQUIRE(morph::log::droppedLogRecords() == before + 2);
}

TEST_CASE("morph::log: a healthy sink drops nothing", "[logger]") {
    LogGuard guard;
    morph::log::setLogLevel(morph::log::LogLevel::debug);
    morph::log::setLogger([](morph::log::LogLevel, std::string_view) {});

    const auto before = morph::log::droppedLogRecords();
    morph::log::logError("fine");
    morph::log::logError("also {}", "fine");
    REQUIRE(morph::log::droppedLogRecords() == before);
}

TEST_CASE("morph::log: a level-suppressed message is not a dropped record", "[logger]") {
    LogGuard guard;
    // A throwing sink that must never be reached, because the level rejects
    // the message before the sink is consulted. This distinguishes "dropped"
    // (a failure) from "suppressed" (working as configured) -- a counter that
    // conflated them would report drops on every quiet run.
    morph::log::setLogger([](morph::log::LogLevel, std::string_view) { throw std::runtime_error{"unreachable"}; });
    morph::log::setLogLevel(morph::log::LogLevel::error);

    const auto before = morph::log::droppedLogRecords();
    morph::log::logDebug("suppressed");
    morph::log::logInfo("suppressed {}", 1);
    REQUIRE(morph::log::droppedLogRecords() == before);
}

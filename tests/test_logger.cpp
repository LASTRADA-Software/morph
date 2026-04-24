// SPDX-License-Identifier: Apache-2.0

#include <morph/logger.hpp>
#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <print>
#include <string>
#include <thread>
#include <vector>


// RAII guard: restores logger + level after each test so tests are isolated.
struct LogGuard {
    morph::log::LogLevel savedLevel = morph::log::getLogLevel();
    morph::log::detail::Logger savedSink;

    LogGuard() {
        std::scoped_lock lock{morph::log::detail::logState().mtx};
        savedLevel = morph::log::detail::logState().minLevel;
        savedSink = morph::log::detail::logState().sink;
    }
    ~LogGuard() {
        std::scoped_lock lock{morph::log::detail::logState().mtx};
        morph::log::detail::logState().minLevel = savedLevel;
        morph::log::detail::logState().sink = std::move(savedSink);
    }
    LogGuard(const LogGuard&) = delete;
    LogGuard& operator=(const LogGuard&) = delete;
};

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

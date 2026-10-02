// SPDX-License-Identifier: Apache-2.0
//
// Tests for `morph::model::runPostCommitTail`: the seam between a handler's
// commit and the work that follows it.
//
// Each case that asserts containment also asserts the tail really ran and
// really threw, so none of them can pass for the uninteresting reason that
// nothing failed. The model-shaped cases at the end drive the helper the way a
// handler uses it -- commit, then journal to a sink that refuses -- and check
// the caller is told the truth about the committed write.

#include <catch2/catch_test_macros.hpp>
#include <memory>
#include <morph/core/logger.hpp>
#include <morph/core/model.hpp>
#include <morph/journal/action_log.hpp>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace {

using morph::log::LogLevel;
using morph::model::runPostCommitTail;

/// @brief Collects every log line emitted while it is alive.
class CapturedLog {
public:
    CapturedLog()
        : _guard{[this](LogLevel level, std::string_view message) {
              _lines.emplace_back(level, std::string{message});
          }} {}

    [[nodiscard]] const std::vector<std::pair<LogLevel, std::string>>& lines() const noexcept { return _lines; }

private:
    std::vector<std::pair<LogLevel, std::string>> _lines;
    morph::log::ScopedLoggerOverride _guard;
};

struct NotAStdException {};

/// @brief A sink whose `append()` throws, as `IActionLog::append`'s contract
///        requires of a sink that could not record the entry.
class RefusingActionLog : public morph::journal::IActionLog {
public:
    void append(morph::journal::LogEntry /*entry*/) override {
        ++appendAttempts;
        throw std::runtime_error{"journal sink unavailable"};
    }

    void flush() override {}

    [[nodiscard]] std::vector<morph::journal::LogEntry> entries(std::string_view /*entityKey*/ = {}) const override {
        return {};
    }

    int appendAttempts = 0;
};

/// @brief The handler shape the helper exists for: validate, mutate, commit,
///        then journal.
struct CounterModel {
    std::shared_ptr<morph::journal::IActionLog> log;
    int committed = 0;
    int cached = 0;

    void journal() const {
        if (log) {
            log->append(morph::journal::LogEntry{});
        }
    }

    /// @brief Commits an increment, then journals it after the commit.
    int increment(int by) {
        if (by <= 0) {
            throw std::invalid_argument{"increment must be positive"};  // before the commit
        }
        committed += by;  // the commit
        runPostCommitTail([&] { journal(); }, "CounterModel::increment");
        return committed;
    }

    /// @brief Commits an increment, then refreshes a cached view of it after
    ///        the commit; the refreshed view is the result.
    int incrementAndRefresh(int by) {
        committed += by;  // the commit
        const int atCommit = committed;
        return runPostCommitTail(
            [&] {
                journal();
                cached = committed * 10;
                return cached;
            },
            atCommit, "CounterModel::incrementAndRefresh");
    }
};

}  // namespace

// ── void overload ─────────────────────────────────────────────────────────────

TEST_CASE("runPostCommitTail runs a tail that succeeds and logs nothing", "[model][post_commit_tail]") {
    const CapturedLog captured;
    int runs = 0;
    runPostCommitTail([&] { ++runs; }, "Handler");
    CHECK(runs == 1);
    CHECK(captured.lines().empty());
}

TEST_CASE("runPostCommitTail contains a std::exception and logs it as an error", "[model][post_commit_tail]") {
    const CapturedLog captured;
    bool reached = false;
    REQUIRE_NOTHROW(runPostCommitTail(
        [&] {
            reached = true;
            throw std::runtime_error{"disk full"};
        },
        "[demo::Model] Create"));
    CHECK(reached);
    REQUIRE(captured.lines().size() == 1);
    CHECK(captured.lines().front().first == LogLevel::error);
    CHECK(captured.lines().front().second ==
          "[demo::Model] Create committed, but its post-commit tail failed: disk full");
}

TEST_CASE("runPostCommitTail contains an exception of any type", "[model][post_commit_tail]") {
    const CapturedLog captured;
    bool reached = false;
    REQUIRE_NOTHROW(runPostCommitTail(
        [&] {
            reached = true;
            throw NotAStdException{};
        },
        "Handler"));
    CHECK(reached);
    REQUIRE(captured.lines().size() == 1);
    CHECK(captured.lines().front().first == LogLevel::error);
    CHECK(captured.lines().front().second == "Handler committed, but its post-commit tail threw a non-std::exception");
}

// ── value-returning overload ──────────────────────────────────────────────────

TEST_CASE("runPostCommitTail returns the tail's result when the tail succeeds", "[model][post_commit_tail]") {
    const CapturedLog captured;
    const std::string result =
        runPostCommitTail([] { return std::string{"refreshed"}; }, std::string{"committed"}, "Handler");
    CHECK(result == "refreshed");
    CHECK(captured.lines().empty());
}

TEST_CASE("runPostCommitTail returns the committed result when the tail throws", "[model][post_commit_tail]") {
    const CapturedLog captured;
    bool reached = false;
    const std::string result = runPostCommitTail(
        [&]() -> std::string {
            reached = true;
            throw std::runtime_error{"re-read failed"};
        },
        std::string{"committed"}, "Handler");
    CHECK(reached);
    CHECK(result == "committed");
    REQUIRE(captured.lines().size() == 1);
    CHECK(captured.lines().front().second == "Handler committed, but its post-commit tail failed: re-read failed");
}

TEST_CASE("runPostCommitTail converts the tail's result to the committed result's type", "[model][post_commit_tail]") {
    // The tail yields a `const char*`; `Result` is deduced from `committed`
    // alone, so the handler's own result type is what the caller receives.
    const auto result = runPostCommitTail([] { return "refreshed"; }, std::string{"committed"}, "Handler");
    STATIC_REQUIRE(std::is_same_v<std::remove_const_t<decltype(result)>, std::string>);
    CHECK(result == "refreshed");
}

TEST_CASE("runPostCommitTail never throws on the caller's path", "[model][post_commit_tail]") {
    // Tails that could throw, so the answer is the helper's own guarantee
    // rather than a property of the tail. `what` is a `string_view` already:
    // the conversion from a string literal is outside the helper's promise.
    auto voidTail = [] { throw std::runtime_error{"tail"}; };
    auto intTail = []() -> int { throw std::runtime_error{"tail"}; };
    constexpr std::string_view what{"Handler"};
    STATIC_REQUIRE(noexcept(runPostCommitTail(voidTail, what)));
    STATIC_REQUIRE(noexcept(runPostCommitTail(intTail, 0, what)));
}

// ── the handler shape ─────────────────────────────────────────────────────────

TEST_CASE("A handler whose journal refuses after the commit reports the committed write",
          "[model][post_commit_tail]") {
    const CapturedLog captured;
    CounterModel model;
    auto log = std::make_shared<RefusingActionLog>();
    model.log = log;

    int returned = 0;
    REQUIRE_NOTHROW(returned = model.increment(3));

    CHECK(log->appendAttempts == 1);  // the tail really did fail
    CHECK(returned == 3);
    CHECK(model.committed == 3);
    REQUIRE(captured.lines().size() == 1);
    CHECK(captured.lines().front().second ==
          "CounterModel::increment committed, but its post-commit tail failed: journal sink unavailable");
}

TEST_CASE("A handler's failure before the commit still reaches the caller", "[model][post_commit_tail]") {
    CounterModel model;
    model.log = std::make_shared<RefusingActionLog>();
    CHECK_THROWS_AS(model.increment(0), std::invalid_argument);
    CHECK(model.committed == 0);
}

TEST_CASE("A value-returning handler falls back to its committed state when the tail fails",
          "[model][post_commit_tail]") {
    const CapturedLog captured;
    CounterModel model;

    CHECK(model.incrementAndRefresh(2) == 20);  // no log: the tail's result

    auto log = std::make_shared<RefusingActionLog>();
    model.log = log;
    int returned = 0;
    REQUIRE_NOTHROW(returned = model.incrementAndRefresh(5));
    CHECK(log->appendAttempts == 1);
    CHECK(returned == 7);  // the state at commit, not a stale or partial refresh
    CHECK(model.committed == 7);
    CHECK(captured.lines().size() == 1);
}

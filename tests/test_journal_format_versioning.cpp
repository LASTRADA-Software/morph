// SPDX-License-Identifier: Apache-2.0
//
// Coverage for journal format versioning & retention (todo.md B4):
// journal::fromJson's reader leniency, LogEntry::v / kLogFormatVersion, and
// FileActionLog::rotate(). Companion to test_action_log.cpp and
// test_action_log_phase2.cpp, which cover the rest of action_log.hpp and
// file_action_log.hpp.

#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <morph/core/model.hpp>
#include <morph/core/registry.hpp>
#include <morph/journal/action_log.hpp>
#include <morph/journal/file_action_log.hpp>
#include <morph/journal/journal.hpp>
#include <string>
#include <thread>
#include <vector>

using morph::journal::FileActionLog;
using morph::journal::LogEntry;

namespace {

/// Fully-initialised LogEntry construction — mirrors test_action_log.cpp's helper
/// so call sites only spell out what a given test actually cares about.
/// [[maybe_unused]] only until Task 2's tests (below, added in a later commit)
/// start calling this; strict compilation (-Wunused-function) errors on an
/// unused free function otherwise.
[[maybe_unused]] LogEntry makeEntry(std::string modelType, std::string entityKey, std::string actionType,
                                    std::string payload = {}, std::string result = {}) {
    return LogEntry{
        .seq = 0,
        .modelType = std::move(modelType),
        .entityKey = std::move(entityKey),
        .actionType = std::move(actionType),
        .payload = std::move(payload),
        .result = std::move(result),
        .principal = {},
        .timestampMs = 0,
    };
}

/// RAII temp-file path: unique per test, removed on scope exit even on failure.
/// Mirrors test_action_log_phase2.cpp's TempFile (each test TU keeps its own
/// private copy rather than sharing one across translation units).
struct TempFile {
    std::filesystem::path path;
    explicit TempFile(std::string_view name)
        : path{std::filesystem::temp_directory_path() /
               (std::string{"morph_test_"} + std::string{name} + "_" +
                std::to_string(reinterpret_cast<std::uintptr_t>(this)) + ".ndjson")} {
        std::filesystem::remove(path);
    }
    ~TempFile() { std::filesystem::remove(path); }
    TempFile(const TempFile&) = delete;
    TempFile& operator=(const TempFile&) = delete;
};

}  // namespace

// ── journal::fromJson: reader leniency ──────────────────────────────────────

TEST_CASE("journal::fromJson: tolerates an unknown/additive key (reader leniency)", "[journal][format][leniency]") {
    // Simulates a line written by a newer morph build that added a JSON key
    // this reader's LogEntry does not have. Before reader leniency, glaze's
    // default `error_on_unknown_keys = true` (plain glz::read_json) throws
    // SerializationError here — adding any new key would be a reader flag-day.
    auto json = std::string{R"({"seq":7,"modelType":"M","entityKey":"e","actionType":"A",)"
                            R"("payload":"{}","result":"","principal":"","timestampMs":123,)"
                            R"("futureField":"from-a-newer-writer"})"};

    LogEntry entry;
    REQUIRE_NOTHROW(entry = morph::journal::fromJson(json));
    REQUIRE(entry.seq == 7);
    REQUIRE(entry.modelType == "M");
    REQUIRE(entry.entityKey == "e");
    REQUIRE(entry.actionType == "A");
    REQUIRE(entry.timestampMs == 123);
}

TEST_CASE("journal::fromJson: leniency does not tolerate syntactically malformed JSON",
          "[journal][format][leniency]") {
    // Leniency only changes how *unknown keys* are handled — it must not paper
    // over genuinely broken JSON.
    REQUIRE_THROWS_AS(morph::journal::fromJson("not json"), morph::journal::SerializationError);
}

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
LogEntry makeEntry(std::string modelType, std::string entityKey, std::string actionType, std::string payload = {},
                   std::string result = {}) {
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

// ── kLogFormatVersion / LogEntry::v ─────────────────────────────────────────

TEST_CASE("journal::kLogFormatVersion is 1", "[journal][format][version]") {
    STATIC_REQUIRE(morph::journal::kLogFormatVersion == 1U);
}

TEST_CASE("journal::toJson/fromJson: a freshly-constructed entry round-trips v = kLogFormatVersion",
          "[journal][format][version]") {
    auto entry = makeEntry("M", "e", "A", "{}", "1");
    REQUIRE(entry.v == morph::journal::kLogFormatVersion);  // default member initializer

    auto json = morph::journal::toJson(entry);
    auto decoded = morph::journal::fromJson(json);
    REQUIRE(decoded.v == morph::journal::kLogFormatVersion);
}

TEST_CASE("journal::fromJson: a legacy line with no v key decodes as v == kLogFormatVersion",
          "[journal][format][version]") {
    auto legacyJson = std::string{R"({"seq":1,"modelType":"M","entityKey":"e","actionType":"A",)"
                                  R"("payload":"{}","result":"","principal":"","timestampMs":100})"};
    auto entry = morph::journal::fromJson(legacyJson);
    REQUIRE(entry.v == morph::journal::kLogFormatVersion);
    REQUIRE(entry.v == 1U);
}

TEST_CASE("journal::fromJson: throws SerializationError when v exceeds kLogFormatVersion",
          "[journal][format][version]") {
    auto futureJson = std::string{R"({"seq":1,"modelType":"M","entityKey":"e","actionType":"A",)"
                                  R"("payload":"{}","result":"","principal":"","timestampMs":100,"v":)"} +
                      std::to_string(morph::journal::kLogFormatVersion + 1) + "}";
    REQUIRE_THROWS_AS(morph::journal::fromJson(futureJson), morph::journal::SerializationError);
}

// ── FileActionLog: v-too-new interacts with the existing torn-line rule ────

TEST_CASE(
    "FileActionLog::entries(): a trailing line with v newer than this build is skipped, "
    "like any other malformed trailing line",
    "[journal][format][version][file]") {
    TempFile tmp{"version_trailing_future"};
    {
        FileActionLog log{tmp.path};
        log.append(makeEntry("M", "acct-1", "A", "{}", "1"));
        log.flush();
    }
    {
        std::ofstream raw{tmp.path, std::ios::app};
        raw << R"({"seq":2,"modelType":"M","entityKey":"acct-2","actionType":"A","payload":"{}",)"
            << R"("result":"2","principal":"","timestampMs":1,"v":)" << (morph::journal::kLogFormatVersion + 1)
            << "}\n";
    }

    FileActionLog log{tmp.path};
    auto all = log.entries();
    REQUIRE(all.size() == 1);
    REQUIRE(all[0].entityKey == "acct-1");
}

TEST_CASE(
    "FileActionLog::entries(): a mid-file line with v newer than this build is genuine "
    "corruption and is re-thrown",
    "[journal][format][version][file]") {
    TempFile tmp{"version_midfile_future"};
    {
        FileActionLog log{tmp.path};
        log.append(makeEntry("M", "acct-1", "A", "{}", "1"));
        log.flush();
    }
    {
        std::ofstream raw{tmp.path, std::ios::app};
        raw << R"({"seq":2,"modelType":"M","entityKey":"acct-2","actionType":"A","payload":"{}",)"
            << R"("result":"2","principal":"","timestampMs":1,"v":)" << (morph::journal::kLogFormatVersion + 1)
            << "}\n";
        raw << morph::journal::toJson(makeEntry("M", "acct-3", "A", "{}", "3")) << "\n";
    }

    // FileActionLog's constructor rebuilds its idempotencyKey dedup set by
    // scanning entries() at open time (composing with B2's dedup logic), so a
    // pre-existing interior corruption — including a too-new v — throws from
    // construction itself, not just from a later explicit entries() call. See
    // the identical pattern in test_action_log_phase2.cpp's own mid-file
    // corruption test.
    REQUIRE_THROWS_AS(FileActionLog(tmp.path), morph::journal::SerializationError);
}

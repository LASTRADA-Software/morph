// SPDX-License-Identifier: Apache-2.0
//
// Reproduces and verifies the fix for #62: three sibling writers on
// caller-supplied strings never got the control-byte escaping fix that
// morph::wire::encode already applies via morph::wire::detail::EscapingWriteOpts
// (see docs/spec/session/session.md and docs/spec/security.md):
//   - morph::journal::toJson(LogEntry)             (journal/action_log.hpp)
//   - morph::offline::detail::toJson(FileQueueRecord) (offline/file_offline_queue.hpp)
//   - morph::session::TokenIssuer::issue(SessionToken) (session/session_auth.hpp)
//
// glaze 7.4 leaves ASCII control bytes (0x00-0x1F) unescaped by default, which
// produces invalid JSON (RFC 8259 requires them escaped) that a later
// glz::read_json either fails outright or, worse, silently corrupts when the
// same string also contains a `\` or `"` earlier (glaze's chunked writer path
// rewrites a control byte as two 0x00 bytes in that case).

#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <morph/journal/action_log.hpp>
#include <morph/offline/file_offline_queue.hpp>
#include <morph/session/session_auth.hpp>
#include <string>
#include <vector>

namespace {

// A function, not a namespace-scope object, matching test_wire_hardening.cpp's
// own rationale: a static-storage std::string could throw before main().
std::string ctl() { return std::string("a") + static_cast<char>(0x0B) + "b"; }

std::filesystem::path tempQueuePath() {
    static std::atomic<int> counter{0};
    auto const now = std::chrono::steady_clock::now().time_since_epoch().count();
    return std::filesystem::temp_directory_path() /
           ("morph_control_byte_escaping_test_" + std::to_string(now) + "_" + std::to_string(++counter) + ".ndjson");
}

}  // namespace

// ── journal::LogEntry ────────────────────────────────────────────────────────

TEST_CASE("journal::toJson escapes control bytes in LogEntry string fields so fromJson round-trips",
          "[control_byte_escaping][journal]") {
    morph::journal::LogEntry entry{
        .seq = 1,
        .modelType = "M",
        .entityKey = ctl(),
        .actionType = "A",
        .payload = ctl(),
        .result = {},
        .outcome = morph::journal::Outcome::Failed,
        .error = ctl(),
        .principal = ctl(),
        .timestampMs = 0,
    };
    std::string json;
    REQUIRE_NOTHROW(json = morph::journal::toJson(entry));

    morph::journal::LogEntry back;
    REQUIRE_NOTHROW(back = morph::journal::fromJson(json));
    CHECK(back.entityKey == ctl());
    CHECK(back.payload == ctl());
    CHECK(back.error == ctl());
    CHECK(back.principal == ctl());
}

TEST_CASE("journal::toJson escapes a control byte alongside an escaped character without corrupting it",
          "[control_byte_escaping][journal]") {
    // Regression guard for glaze's corrupting fast path (see test_wire_hardening.cpp's
    // identical case for wire::encode): a `\` earlier in the same string sends
    // the unescaped writer down a path that mangles a later control byte into
    // two 0x00 bytes instead of merely producing invalid JSON.
    std::string payload = "\\x";
    payload.push_back(static_cast<char>(0x0B));
    payload += "\"tail";

    morph::journal::LogEntry entry{
        .seq = 1,
        .modelType = "M",
        .entityKey = {},
        .actionType = "A",
        .payload = payload,
        .result = {},
        .outcome = morph::journal::Outcome::Succeeded,
        .error = {},
        .principal = {},
        .timestampMs = 0,
        .idempotencyKey = {},
    };
    std::string json;
    REQUIRE_NOTHROW(json = morph::journal::toJson(entry));

    morph::journal::LogEntry back;
    REQUIRE_NOTHROW(back = morph::journal::fromJson(json));
    CHECK(back.payload == payload);
}

// ── offline::FileOfflineQueue (FileQueueRecord) ─────────────────────────────

TEST_CASE("FileOfflineQueue: a control byte in payload survives being written and reopened",
          "[control_byte_escaping][file_queue]") {
    auto path = tempQueuePath();
    std::filesystem::remove(path);
    std::string payload = ctl();
    {
        morph::offline::FileOfflineQueue queue{path};
        // A second, ordinary item after the control-byte one so the
        // control-byte line is NOT the trailing line — FileOfflineQueue
        // tolerates (skips) a malformed *trailing* line as a torn-write
        // heuristic, but rethrows on any earlier malformed line, so this is
        // the shape that actually exercises the bug rather than the
        // torn-line tolerance.
        queue.enqueue(payload);
        queue.enqueue("second");
    }  // close the file handle before reopening/removing -- required on Windows

    std::vector<morph::offline::QueueItem> items;
    {
        morph::offline::FileOfflineQueue reopened{path};
        items = reopened.drain();
    }  // close the file handle before removing -- required on Windows
    REQUIRE(items.size() == 2);
    CHECK(items[0].payload == payload);
    CHECK(items[1].payload == "second");

    std::filesystem::remove(path);
}

// ── session::TokenIssuer ─────────────────────────────────────────────────────

TEST_CASE("TokenIssuer::issue escapes control bytes in principal/roles so the token verifies",
          "[control_byte_escaping][session_auth]") {
    const std::string secret = "ctl-byte-secret";
    const morph::session::TokenIssuer issuer{secret};
    const morph::session::TokenVerifier verifier{secret};

    const morph::session::SessionToken claims{
        .principal = ctl(),
        .issuedAtMs = 0,
        .expiresAtMs = 9'999'999'999'999,
        .roles = {ctl()},
    };
    std::string token;
    REQUIRE_NOTHROW(token = issuer.issue(claims));

    const auto verified = verifier.verify(token, 1000);
    REQUIRE(verified.has_value());
    CHECK(verified->principal == ctl());
    REQUIRE(verified->roles.size() == 1);
    CHECK(verified->roles[0] == ctl());
}

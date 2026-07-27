// SPDX-License-Identifier: Apache-2.0

// Wire-protocol hardening coverage for morph::wire.
//
// Bug A (message-size bomb): decode() enforces a maximum serialized envelope
//   size so an oversized payload — including deeply-nested JSON smuggled inside
//   the opaque `body` string, which the outer parse never walks — is rejected
//   cheaply before it can detonate on a later inner re-parse.
//
// Bug B (duplicate-key smuggling): glaze 7.2.1 does NOT reject duplicate JSON
//   keys (last-wins); there is no option to enable rejection. These tests pin
//   the actual, documented behavior so the spec claim stays honest.
//
// Bug C (skip_ws heap-buffer-overflow): decode() passed the caller's
//   `string_view` straight to `glz::read` with glaze's default
//   `null_terminated = true`, so `skip_ws` scanned past the buffer's real end
//   looking for a terminator on any input glaze judged to need trailing
//   whitespace-skip — an AddressSanitizer heap-buffer-overflow on adversarial
//   input, found by the `fuzz_wire_decode` harness (see
//   docs/spec/testing_strategy.md, "Known findings"; the exact crashing input
//   is preserved at tests/fuzz/findings/wire_decode/skip_ws_heap_overflow.bin).
//   Fixed by setting `null_terminated = false` on every glz::read<> call site
//   that accepts an arbitrary `string_view` (wire.hpp, action_log.hpp,
//   registry.hpp's BRIDGE_REGISTER_ACTION macro, file_offline_queue.hpp).
//
// Bug D (unescaped control bytes break the err-reply round-trip): glaze's
//   writer escapes `"` and `\` but not ASCII control bytes (0x00-0x1F, 0x7F),
//   so an `err` message echoing untrusted text containing one (e.g. an
//   unrecognized `Envelope::kind`) served syntactically invalid JSON that
//   glaze's own reader then rejected — found by the `fuzz_dispatch_execute`
//   harness (crashing input preserved at
//   tests/fuzz/findings/dispatch_execute/err_reply_control_char_roundtrip.bin).
//   Fixed in `makeErr` by replacing control bytes with a printable `\xHH`
//   placeholder before they reach the writer. `makeErr` keeps doing this, but
//   for a different reason now (see Bug E): an err message is log-bound text,
//   and a raw 0x1B in it would carry an ANSI escape into the reader's terminal.
//
// Bug E (control bytes in the remaining string fields): the same writer gap
//   applies to every `Envelope` string, not just `message` — `body`,
//   `modelType`, `actionType`, `contextKey`, `typeId`, and the session's
//   `principal`/`token`. Those carry caller data that must round-trip
//   byte-for-byte, so the `makeErr` substitution is the wrong instrument for
//   them. Worse than invalid output, glaze's chunked write path *corrupts* such
//   a byte when the same string also holds a `\` or `"`, emitting two 0x00
//   bytes in its place — undetectable downstream, since the result still
//   decodes. Fixed at the writer with `detail::EscapingWriteOpts`
//   (glaze's `escape_control_characters`), which is lossless in both
//   directions.

#include <catch2/catch_test_macros.hpp>
#include <cstring>
#include <memory>
#include <morph/core/wire.hpp>
#include <string>
#include <string_view>

using morph::wire::decode;
using morph::wire::encode;
using morph::wire::kMaxEnvelopeBytes;
using morph::wire::makeErr;
using morph::wire::makeOk;
using morph::wire::makeRegister;

// ── Bug A: message-size cap ───────────────────────────────────────────────────

TEST_CASE("decode accepts an envelope at the size limit boundary", "[wire][hardening]") {
    // Build a valid envelope whose serialized form is comfortably under the cap.
    auto env = makeRegister("SomeType");
    env.body = std::string(1024, 'x');  // small, valid string payload
    const std::string json = encode(env);
    REQUIRE(json.size() < kMaxEnvelopeBytes);
    REQUIRE_NOTHROW(decode(json));
}

TEST_CASE("decode rejects an oversized envelope before parsing", "[wire][hardening]") {
    // An envelope whose serialized length exceeds the cap must be rejected by
    // decode() regardless of whether the JSON itself is otherwise well-formed.
    std::string json;
    json.reserve(kMaxEnvelopeBytes + 4096);
    json += R"({"kind":"execute","body":")";
    json += std::string(kMaxEnvelopeBytes + 1024, 'x');  // push total over the cap
    json += R"("})";
    REQUIRE(json.size() > kMaxEnvelopeBytes);
    REQUIRE_THROWS_AS(decode(json), std::runtime_error);
}

TEST_CASE("decode's size guard fires even for deeply nested body JSON", "[wire][hardening]") {
    // The outer parse treats `body` as one opaque string, so nesting depth
    // inside it is invisible to any outer depth check. The size cap is the
    // wire-layer backstop: a body large enough to be dangerous is rejected on
    // length alone, before the action codec ever re-parses it.
    std::string deep;
    deep.reserve(kMaxEnvelopeBytes + 8192);
    deep += R"({"kind":"execute","body":")";
    // Escaped braces so the payload stays a single JSON string at the outer level.
    const std::size_t pairs = kMaxEnvelopeBytes;  // far more than needed to exceed cap
    deep.append(pairs, '{');
    deep += R"("})";
    REQUIRE(deep.size() > kMaxEnvelopeBytes);
    REQUIRE_THROWS_AS(decode(deep), std::runtime_error);
}

// ── Bug B: duplicate keys are NOT rejected (last-wins) ────────────────────────
//
// Empirically confirmed against the vendored glaze 7.2.1: it silently accepts
// duplicate object keys and keeps the LAST occurrence. There is no glz::opts
// flag to error on duplicates, so decode() cannot reject them via options. These
// tests document the true behavior; callers MUST NOT rely on duplicate-key
// rejection as a security boundary (see docs/spec/wire.md, docs/spec/security.md).

TEST_CASE("decode accepts a duplicate top-level key and keeps the last value", "[wire][hardening]") {
    const std::string json = R"({"kind":"execute","kind":"register"})";
    morph::wire::Envelope env;
    REQUIRE_NOTHROW(env = decode(json));
    // Last-wins: the second "kind" overwrites the first.
    CHECK(env.kind == "register");
}

TEST_CASE("decode accepts a duplicate nested session and keeps the last value", "[wire][hardening]") {
    // The parser-differential smuggling primitive: a validating proxy might see
    // the first session while morph keeps the last.
    const std::string json =
        R"({"kind":"execute","session":{"principal":"alice"},"session":{"principal":"attacker"}})";
    morph::wire::Envelope env;
    REQUIRE_NOTHROW(env = decode(json));
    CHECK(env.session.principal == "attacker");
}

// ── Bug C: decode() must not assume a null-terminated buffer ─────────────────

TEST_CASE("decode does not read past a tightly-sized, non-null-terminated buffer", "[wire][hardening]") {
    // Mirrors exactly how a transport (or the fuzz harness) hands decode() raw
    // bytes: a heap allocation sized to the content with nothing guaranteed to
    // follow it, as opposed to a std::string's always-present '\0' at data()[size()].
    // Regression input from the fuzz corpus that reproduced the original
    // heap-buffer-overflow under ASan (see tests/fuzz/findings/wire_decode/
    // skip_ws_heap_overflow.bin) — a "hello" kind whose principal is invalid,
    // exercising the exact glaze code path (skip_ws while parsing the trailing
    // `session` field) that crashed.
    const std::string source = R"({"kind":"hello","protocolVersion":1,"session":{"principal":"attacker"}})";
    const auto buffer = std::make_unique<char[]>(source.size());
    std::memcpy(buffer.get(), source.data(), source.size());
    const std::string_view view{buffer.get(), source.size()};

    morph::wire::Envelope env;
    REQUIRE_NOTHROW(env = decode(view));
    CHECK(env.kind == "hello");
    CHECK(env.session.principal == "attacker");
}

// ── Bug D: makeErr must sanitize untrusted text so the reply stays valid JSON ─

TEST_CASE("makeErr replaces raw control bytes so the err envelope re-decodes", "[wire][hardening]") {
    // The concrete scenario found by fuzzing: an unrecognized Envelope::kind
    // (attacker-controlled) gets echoed verbatim into the err reply's message.
    std::string untrustedKind = "he";
    untrustedKind.push_back(static_cast<char>(0x1E));  // ASCII record separator
    untrustedKind += "llo";

    const auto errEnv = makeErr("unknown envelope kind: " + untrustedKind, 7);
    const std::string json = encode(errEnv);

    morph::wire::Envelope decoded;
    REQUIRE_NOTHROW(decoded = decode(json));
    CHECK(decoded.kind == "err");
    CHECK(decoded.callId == 7);
    CHECK(decoded.message == R"(unknown envelope kind: he\x1ello)");
}

TEST_CASE("makeErr leaves ordinary text untouched", "[wire][hardening]") {
    const auto errEnv = makeErr("model not found", 3);
    CHECK(errEnv.message == "model not found");
    morph::wire::Envelope decoded;
    REQUIRE_NOTHROW(decoded = decode(encode(errEnv)));
    CHECK(decoded.message == "model not found");
}

// ── Bug E: every string field, not just `message`, must survive control bytes ─

namespace {
// A control byte embedded in ordinary text, plus the round-trip helper the
// per-field cases below share. Split into one TEST_CASE per field rather than
// SECTIONs: Catch2 expands each SECTION into a branch, so a single case
// covering six fields reads as one deeply-branched function.
// A function, not a namespace-scope object: a `std::string` with static
// storage duration constructs before main() and could throw where nothing can
// catch it.
std::string ctl() { return std::string("a") + static_cast<char>(0x0B) + "b"; }

morph::wire::Envelope roundTrip(const morph::wire::Envelope& env) { return decode(encode(env)); }
}  // namespace

TEST_CASE("encode escapes control bytes in body", "[wire][hardening]") {
    CHECK(roundTrip(makeOk(7, ctl())).body == ctl());
}

TEST_CASE("encode escapes control bytes in contextKey", "[wire][hardening]") {
    CHECK(roundTrip(makeRegister("T", ctl())).contextKey == ctl());
}

TEST_CASE("encode escapes control bytes in modelType and actionType", "[wire][hardening]") {
    morph::wire::Envelope env;
    env.kind = "execute";
    env.modelType = ctl();
    env.actionType = ctl();
    const auto back = roundTrip(env);
    CHECK(back.modelType == ctl());
    CHECK(back.actionType == ctl());
}

TEST_CASE("encode escapes control bytes in the session principal and token", "[wire][hardening]") {
    morph::wire::Envelope env;
    env.kind = "execute";
    env.session.principal = ctl();
    env.session.token = ctl();
    const auto back = roundTrip(env);
    CHECK(back.session.principal == ctl());
    CHECK(back.session.token == ctl());
}

TEST_CASE("encode preserves control bytes exactly rather than replacing them", "[wire][hardening]") {
    // `body` is a serialized payload: unlike a diagnostic `message`, it has to
    // come back byte-for-byte. Escaping (not substitution) is what makes that
    // true, so pin it across the whole control range.
    std::string all;
    for (int byte = 0x00; byte < 0x20; ++byte) {
        all.push_back(static_cast<char>(byte));
    }
    const auto back = decode(encode(makeOk(1, all)));
    CHECK(back.body == all);
    CHECK(back.body.size() == 32U);
}

TEST_CASE("encode survives a control byte alongside an escaped character", "[wire][hardening]") {
    // Regression guard for a *corrupting* failure mode, distinct from invalid
    // output: with glaze's default options, a `\` or `"` earlier in the same
    // string sends the writer down a chunked path that rewrote a later 0x0B as
    // two 0x00 bytes. The payload was destroyed before transmission, so the
    // envelope decoded cleanly into silently wrong data — which makes the
    // value comparison here, not merely the absence of a throw, the assertion
    // that matters. The offset dependence is why the loop sweeps padding.
    for (std::size_t pad = 0; pad <= 40; ++pad) {
        std::string payload = "\\" + std::string(pad, 'x');
        payload.push_back(static_cast<char>(0x0B));
        payload += "\"tail";

        morph::wire::Envelope back;
        REQUIRE_NOTHROW(back = decode(encode(makeOk(1, payload))));
        INFO("padding = " << pad);
        CHECK(back.body == payload);
    }
}

// ── Bug F: a size-rejected frame must still be addressed to its call ─────────
// A transport that rejects a frame for exceeding its own size cap never decodes
// it, yet still has to answer. `callId == 0` is the client's synchronous-reply
// discriminator, so an err sent with a zeroed id resumes whatever unrelated
// register/deregister is parked, handing it another call's reply — while the
// execute it was meant for never resolves at all.

TEST_CASE("wire::detail::peekCallId recovers the id without decoding", "[wire][hardening]") {
    morph::wire::Envelope env;
    env.kind = "execute";
    env.callId = 987654321U;
    env.modelType = "M";
    env.actionType = "A";
    env.body = std::string(4096, 'x');  // far beyond the scan window
    CHECK(morph::wire::detail::peekCallId(encode(env)) == 987654321U);
}

TEST_CASE("wire::detail::peekCallId degrades to 0 rather than guessing", "[wire][hardening]") {
    CHECK(morph::wire::detail::peekCallId("") == 0U);
    CHECK(morph::wire::detail::peekCallId("not json at all") == 0U);
    CHECK(morph::wire::detail::peekCallId(R"({"kind":"execute"})") == 0U);
    CHECK(morph::wire::detail::peekCallId(R"({"callId":})") == 0U);
    CHECK(morph::wire::detail::peekCallId(R"({"callId":"7"})") == 0U);
    // Past the scan window: not found, so 0 — never a partial parse.
    const std::string padded = std::string(2048, ' ') + R"({"callId":5})";
    CHECK(morph::wire::detail::peekCallId(padded) == 0U);
    // Out of range must not wrap around into a plausible-looking id.
    CHECK(morph::wire::detail::peekCallId(R"({"callId":99999999999999999999999})") == 0U);
}

TEST_CASE("wire::detail::peekCallId cannot be spoofed from an earlier string field", "[wire][hardening]") {
    // `kind` is written before `callId`, so a hostile value there is scanned
    // first. encode() escapes its quotes, yielding bytes that do not match.
    morph::wire::Envelope env;
    env.kind = R"(x","callId":424242,"z":")";
    env.callId = 5U;
    CHECK(morph::wire::detail::peekCallId(encode(env)) == 5U);
}

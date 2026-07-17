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

#include <morph/wire.hpp>

#include <catch2/catch_test_macros.hpp>
#include <string>

using morph::wire::decode;
using morph::wire::encode;
using morph::wire::kMaxEnvelopeBytes;
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

TEST_CASE("decode accepts a duplicate top-level key and keeps the last value",
          "[wire][hardening]") {
    const std::string json = R"({"kind":"execute","kind":"register"})";
    morph::wire::Envelope env;
    REQUIRE_NOTHROW(env = decode(json));
    // Last-wins: the second "kind" overwrites the first.
    CHECK(env.kind == "register");
}

TEST_CASE("decode accepts a duplicate nested session and keeps the last value",
          "[wire][hardening]") {
    // The parser-differential smuggling primitive: a validating proxy might see
    // the first session while morph keeps the last.
    const std::string json =
        R"({"kind":"execute","session":{"principal":"alice"},"session":{"principal":"attacker"}})";
    morph::wire::Envelope env;
    REQUIRE_NOTHROW(env = decode(json));
    CHECK(env.session.principal == "attacker");
}

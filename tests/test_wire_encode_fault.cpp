// SPDX-License-Identifier: Apache-2.0
//
// wire::encode's write-failure arm, reachable only through WireCodecOps.

#include <catch2/catch_test_macros.hpp>
#include <morph/core/wire.hpp>
#include <string>

using morph::wire::Envelope;
using morph::wire::WireCodecOps;

TEST_CASE("No Envelope value can drive glz::write to fail", "[wire][encode]") {
    // Pins the premise the seam rests on, so it is checked rather than
    // asserted in a comment. If glaze ever starts rejecting one of these, this
    // fails and the seam's rationale needs revisiting.
    const auto encodes = [](std::string kind) {
        Envelope env;
        env.kind = std::move(kind);
        std::string out;
        return !glz::write<morph::wire::detail::EscapingWriteOpts{}>(env, out);
    };

    CHECK(encodes("execute"));
    CHECK(encodes(std::string("\x80", 1)));              // lone continuation byte
    CHECK(encodes(std::string("\xC3", 1)));              // truncated 2-byte sequence
    CHECK(encodes(std::string("\xE0\x80\xAF", 3)));      // overlong encoding
    CHECK(encodes(std::string("\xED\xA0\x80", 3)));      // UTF-16 surrogate
    CHECK(encodes(std::string("\xF5\x80\x80\x80", 4)));  // out of Unicode range
    CHECK(encodes(std::string("a\0b", 3)));              // embedded NUL
    CHECK(encodes(std::string("\x0B", 1)));              // raw control byte
}

TEST_CASE("encode throws when the write strategy fails", "[wire][encode]") {
    WireCodecOps failing;
    failing.writeEnvelope = [](const Envelope&, std::string&) {
        return glz::error_ctx{.count = 0, .ec = glz::error_code::invalid_variant_object};
    };

    Envelope env;
    env.kind = "execute";
    CHECK_THROWS_AS(morph::wire::encode(env, failing), std::runtime_error);
}

TEST_CASE("encode's default strategy is the real writer", "[wire][encode]") {
    // The seam must not change behaviour for an ordinary caller: the
    // defaulted-argument form and the explicitly-defaulted form agree, and
    // both produce re-decodable JSON.
    Envelope env;
    env.kind = "execute";
    env.callId = 7;
    env.modelType = "EchoModel";

    const auto viaDefault = morph::wire::encode(env);
    const auto viaExplicit = morph::wire::encode(env, morph::wire::defaultWireCodecOps());
    CHECK(viaDefault == viaExplicit);

    const auto roundTripped = morph::wire::decode(viaDefault);
    CHECK(roundTripped.kind == "execute");
    CHECK(roundTripped.callId == 7);
    CHECK(roundTripped.modelType == "EchoModel");
}

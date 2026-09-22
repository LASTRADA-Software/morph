// SPDX-License-Identifier: Apache-2.0
//
// `encode` leaves out the envelope fields that hold their default (morph#524).
//
// `wire::Envelope` is a union-of-all-kinds struct: an `ok` reply uses three of
// its thirteen members, but glaze writes every member of a struct it is handed,
// so a minimal reply carrying an 8-byte payload cost 255 bytes, 213 of them
// fields the kind does not use.
//
// Three things have to hold for the omission to be safe, and each has a case
// below:
//
//   1. It is lossless. Encoding and decoding an envelope with every member
//      populated must give back an equal envelope.
//   2. It is compatible in both directions. A *legacy* encoder's output — every
//      key present, the exact bytes `encode` produced before this change — must
//      decode to the same envelope as the short form. That is what makes this
//      not a `kProtocolVersion` bump: the decoder default-initialises and reads
//      with `error_on_unknown_keys = false`, so an absent key leaves the member
//      at precisely the value the omitted key would have written.
//   3. It actually saves something, with a ceiling that goes red on a
//      regression rather than a number printed into a log.
//
// The fourth case is the one that stops the saving from becoming a data loss:
// every omittable field, set to a non-default value, must appear in the output.

#include <catch2/catch_test_macros.hpp>
#include <morph/core/wire.hpp>
#include <string>
#include <string_view>

namespace {

// Every member set to something distinguishable from its default.
morph::wire::Envelope fullyPopulated() {
    morph::wire::Envelope env;
    env.kind = "execute";
    env.callId = 987654321U;
    env.typeId = "WOF_Type";
    env.contextKey = "WOF_Context";
    env.primary = "WOF_Primary";
    env.shared = true;
    env.modelId = 42U;
    env.modelType = "WOF_Model";
    env.actionType = "WOF_Action";
    env.body = R"({"y":42})";
    env.message = "WOF_Message";
    env.session.principal = "alice";
    env.session.token = "tok";
    env.session.requestId = "req-1";
    env.session.locale = "fr-FR";
    env.session.metadata["flag"] = "on";
    env.protocolVersion = 7U;
    return env;
}

void requireEqual(const morph::wire::Envelope& lhs, const morph::wire::Envelope& rhs) {
    CHECK(lhs.kind == rhs.kind);
    CHECK(lhs.callId == rhs.callId);
    CHECK(lhs.typeId == rhs.typeId);
    CHECK(lhs.contextKey == rhs.contextKey);
    CHECK(lhs.primary == rhs.primary);
    CHECK(lhs.shared == rhs.shared);
    CHECK(lhs.modelId == rhs.modelId);
    CHECK(lhs.modelType == rhs.modelType);
    CHECK(lhs.actionType == rhs.actionType);
    CHECK(lhs.body == rhs.body);
    CHECK(lhs.message == rhs.message);
    CHECK(lhs.session.principal == rhs.session.principal);
    CHECK(lhs.session.token == rhs.session.token);
    CHECK(lhs.session.requestId == rhs.session.requestId);
    CHECK(lhs.session.locale == rhs.session.locale);
    CHECK(lhs.session.metadata == rhs.session.metadata);
    CHECK(lhs.protocolVersion == rhs.protocolVersion);
}

}  // namespace

TEST_CASE("morph::wire: a fully populated envelope round-trips unchanged", "[wire][omitted-fields]") {
    const auto original = fullyPopulated();
    const auto decoded = morph::wire::decode(morph::wire::encode(original));
    requireEqual(original, decoded);
}

TEST_CASE("morph::wire: every omittable field survives when it is not at its default", "[wire][omitted-fields]") {
    const auto text = morph::wire::encode(fullyPopulated());
    for (const auto key : morph::wire::detail::kOmittableEnvelopeKeys) {
        INFO("key: " << key);
        CHECK(text.find("\"" + std::string{key} + "\":") != std::string::npos);
    }
    CHECK(text.find(R"("kind":)") != std::string::npos);
}

TEST_CASE("morph::wire: the legacy all-keys form decodes to the same envelope as the short form",
          "[wire][omitted-fields]") {
    // The exact bytes `encode` produced for this envelope before morph#524 —
    // captured from the pre-change build, not regenerated, so this case still
    // means something if `encode` changes again.
    static constexpr std::string_view kLegacyOk =
        R"({"kind":"ok","callId":7,"typeId":"","contextKey":"","primary":"","shared":false,"modelId":0,)"
        R"("modelType":"","actionType":"","body":"{\"y\":42}","message":"",)"
        R"("session":{"principal":"","token":"","requestId":"","locale":"","metadata":{}},"protocolVersion":0})";

    morph::wire::Envelope ok;
    ok.kind = "ok";
    ok.callId = 7;
    ok.body = R"({"y":42})";

    const auto shortForm = morph::wire::encode(ok);
    CHECK(shortForm == R"({"kind":"ok","callId":7,"body":"{\"y\":42}"})");

    requireEqual(morph::wire::decode(kLegacyOk), morph::wire::decode(shortForm));
    requireEqual(ok, morph::wire::decode(kLegacyOk));
}

TEST_CASE("morph::wire: a minimal reply stays small", "[wire][omitted-fields]") {
    // A ceiling, not a printed figure: it fails if the omission stops
    // happening. Measured at 44 bytes on the change that introduced it; the
    // budget is deliberately loose enough that adding an envelope member does
    // not fail this case, and tight enough that writing every member again
    // (255 bytes, the pre-change figure) does.
    static constexpr std::size_t kMinimalReplyBudget = 80;

    morph::wire::Envelope ok;
    ok.kind = "ok";
    ok.callId = 7;
    ok.body = R"({"y":42})";
    const auto text = morph::wire::encode(ok);
    INFO("encoded: " << text);
    CHECK(text.size() <= kMinimalReplyBudget);

    // And a `deregister`, which carries even less.
    morph::wire::Envelope dereg;
    dereg.kind = "deregister";
    dereg.modelId = 3;
    CHECK(morph::wire::encode(dereg) == R"({"kind":"deregister","modelId":3})");
}

TEST_CASE("morph::wire: an all-default session is omitted, a populated one is not", "[wire][omitted-fields]") {
    morph::wire::Envelope env;
    env.kind = "execute";
    env.body = "{}";
    CHECK(morph::wire::encode(env).find(R"("session":)") == std::string::npos);

    // Each member on its own is enough to bring the whole object back — the
    // check is per-member, so a new member added to `session::Context` without
    // updating `isDefaultSession` would be dropped here (the static_assert in
    // that function is the other half of that guard).
    for (int which = 0; which < 5; ++which) {
        auto probe = env;
        switch (which) {
            case 0:
                probe.session.principal = "alice";
                break;
            case 1:
                probe.session.token = "tok";
                break;
            case 2:
                probe.session.requestId = "req";
                break;
            case 3:
                probe.session.locale = "fr";
                break;
            default:
                probe.session.metadata["k"] = "v";
                break;
        }
        INFO("session member index: " << which);
        const auto text = morph::wire::encode(probe);
        CHECK(text.find(R"("session":)") != std::string::npos);
        requireEqual(probe, morph::wire::decode(text));
    }
}

TEST_CASE("morph::wire: peekCallId still finds the id in the shortened form", "[wire][omitted-fields]") {
    // `callId` is omittable, so this is the case that matters: it is the second
    // key written whenever it is present, and absent exactly when it is 0 —
    // which is the value peekCallId returns for a message that has none.
    morph::wire::Envelope env;
    env.kind = "err";
    env.callId = 987654321U;
    env.message = "boom";
    CHECK(morph::wire::detail::peekCallId(morph::wire::encode(env)) == 987654321U);

    env.callId = 0;
    CHECK(morph::wire::detail::peekCallId(morph::wire::encode(env)) == 0U);
}

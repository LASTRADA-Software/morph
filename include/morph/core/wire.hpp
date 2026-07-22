// SPDX-License-Identifier: Apache-2.0

#pragma once
#include <cstddef>
#include <cstdint>
#include <glaze/glaze.hpp>
#include <stdexcept>
#include <string>
#include <string_view>

#include "../session/session.hpp"

namespace morph::wire {

/// @brief Maximum accepted serialized size, in bytes, of a single wire envelope.
///
/// `decode` rejects any input longer than this before handing it to the JSON
/// parser. The cap is a wire-layer denial-of-service backstop: the outer decode
/// treats `body` as one opaque string, so a deeply-nested or enormous JSON
/// payload smuggled *inside* `body` is invisible to any structural/depth check
/// and would only detonate when the action codec re-parses it later (the "body
/// double-parse"). Bounding the total message length caps that cost up
/// front. The default of 8 MiB is generous for legitimate action payloads while
/// keeping a single message's peak allocation bounded; transports that want a
/// tighter bound should enforce it before calling `decode`.
inline constexpr std::size_t kMaxEnvelopeBytes = 8u * 1024u * 1024u;

/// @brief Protocol version this build of `morph` speaks.
///
/// Carried in `Envelope::protocolVersion` and announced by `makeHello()`.
/// Bumped only on a **breaking** wire change (a field removal/retype, or a
/// control-flow semantic change) — see "Action-evolution policy" in
/// docs/spec/core/wire.md. A purely additive change (a new optional field on
/// the envelope or on an action/result struct) does not bump this constant.
inline constexpr std::uint32_t kProtocolVersion = 1;

/// @brief JSON wire envelope used between any client and `RemoteServer`.
///
/// Supersedes the legacy pipe-delimited protocol: a single struct carries all
/// request and reply variants, with the `kind` field acting as a discriminator.
/// Empty/zero fields are tolerated — callers populate only what their kind needs.
///
/// @par Discriminator values
/// - `"register"`  — client requests model creation. Uses `typeId`, and
///                   optionally `contextKey` (the new instance's stable
///                   identity, e.g. an account id — see `RemoteServer::setLogProvider`).
/// - `"deregister"` — client destroys an instance. Uses `modelId`.
/// - `"execute"`   — client dispatches an action. Uses `callId`, `modelId`,
///                   `modelType`, `actionType`, `body`, and optionally `session`.
/// - `"ok"`        — server success reply. Uses `callId` if present and carries
///                   the serialized result in `body`. For `register` replies,
///                   `modelId` carries the new id.
/// - `"err"`       — server failure reply. Uses `callId` if present and `message`.
struct Envelope {
    /// @brief Discriminator — see class docstring for valid values.
    std::string kind;

    /// @brief Correlation id for matching async replies to their requests.
    uint64_t callId = 0;

    /// @brief Model type id for `register`.
    std::string typeId;

    /// @brief Stable identity of the model instance being registered (e.g. an
    ///        account id). Empty means "no identity" — the server-side holder
    ///        gets no action log attached even if a `LogProvider` is configured.
    ///        Ignored on every kind other than `register`.
    std::string contextKey;

    /// @brief Existing model instance id for `deregister`, `execute`, `ok(register)`.
    uint64_t modelId = 0;

    /// @brief Model type id for `execute` (routing key in `ActionDispatcher`).
    std::string modelType;

    /// @brief Action type id for `execute` (the second routing key).
    std::string actionType;

    /// @brief Serialized JSON of the action payload (`execute`) or of the
    ///        deserializable result (`ok`).
    std::string body;

    /// @brief Free-text error message on `err`. Empty on success.
    std::string message;

    /// @brief Session context for authorization and routing. Populated on
    ///        `execute`; ignored on every other kind.
    ::morph::session::Context session;

    /// @brief Protocol version the sender speaks.
    ///
    /// `0` means "unspecified / legacy peer" — the value on every envelope an
    /// old encoder (unaware of this field) produces, and the value an old
    /// decoder (unaware of this field) leaves untouched when it receives one
    /// from a newer peer (ignored via `decode`'s `error_on_unknown_keys =
    /// false`). Populated by `makeHello()` on `"hello"`; not otherwise
    /// inspected by other `kind`s today.
    std::uint32_t protocolVersion = 0;
};

/// @brief Inclusive protocol-version range a server advertises in reply to
///        `"hello"`.
///
/// Serialized as the `"ok"` reply's `body` (JSON) when a server accepts a
/// `"hello"` — see `RemoteServer::setSupportedVersionRange`.
struct ProtocolRange {
    /// @brief Oldest protocol version the server accepts.
    std::uint32_t min = kProtocolVersion;

    /// @brief Newest protocol version the server accepts.
    std::uint32_t max = kProtocolVersion;
};

/// @brief Builds a `register` envelope.
/// @param typeId     Model type id to register.
/// @param contextKey Optional stable identity for the new instance (default: none).
inline Envelope makeRegister(std::string typeId, std::string contextKey = {}) {
    Envelope env;
    env.kind = "register";
    env.typeId = std::move(typeId);
    env.contextKey = std::move(contextKey);
    return env;
}

/// @brief Builds a `deregister` envelope.
inline Envelope makeDeregister(uint64_t modelId) {
    Envelope env;
    env.kind = "deregister";
    env.modelId = modelId;
    return env;
}

/// @brief Builds an `ok` reply envelope. `body` carries the result for executes,
///        `modelId` carries the new id for register-replies.
inline Envelope makeOk(uint64_t callId = 0, std::string body = {}, uint64_t modelId = 0) {
    Envelope env;
    env.kind = "ok";
    env.callId = callId;
    env.body = std::move(body);
    env.modelId = modelId;
    return env;
}

namespace detail {

/// @brief Replaces every ASCII control byte (0x00-0x1F, 0x7F) in @p text with a
///        printable `\xHH` textual placeholder.
///
/// Glaze 7.4's JSON writer escapes `"` and `\` correctly but does not escape
/// control characters, so a `std::string` field containing one serializes to
/// syntactically invalid JSON that glaze's own reader then rejects on decode —
/// found by the `fuzz_dispatch_execute` harness via an `err` reply whose
/// `message` echoed an unrecognized `Envelope::kind` verbatim (see
/// docs/spec/testing_strategy.md, "Known findings"). `makeErr`'s `message` is
/// diagnostic text, not data that must round-trip byte-for-byte, so replacing
/// (rather than preserving) these bytes is an acceptable, guaranteed-safe fix:
/// the replacement is plain printable ASCII, which glaze already escapes
/// correctly wherever it needs to (verified: `\` and `"` round-trip).
/// @param text Untrusted text that may contain raw control bytes.
/// @return @p text with every control byte replaced by `\xHH`.
[[nodiscard]] inline std::string sanitizeControlChars(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (char c : text) {
        auto byte = static_cast<unsigned char>(c);
        if (byte < 0x20 || byte == 0x7F) {
            static constexpr char kHex[] = "0123456789abcdef";
            out += "\\x";
            out += kHex[(byte >> 4) & 0xF];
            out += kHex[byte & 0xF];
        } else {
            out += c;
        }
    }
    return out;
}

}  // namespace detail

/// @brief Builds an `err` reply envelope.
///
/// @p message may embed untrusted content (e.g. an unrecognized `Envelope::kind`
/// or a caught exception's `what()`) — any raw control byte it contains is
/// replaced with a `\xHH` placeholder so the encoded envelope is always valid,
/// re-decodable JSON (see `detail::sanitizeControlChars`).
inline Envelope makeErr(std::string message, uint64_t callId = 0) {
    Envelope env;
    env.kind = "err";
    env.callId = callId;
    env.message = detail::sanitizeControlChars(message);
    return env;
}

/// @brief Builds a `hello` envelope announcing the sender's protocol version.
///
/// Exchanged once per connection, before any `register`/`execute`. See
/// "Protocol version negotiation" in docs/spec/core/wire.md.
/// @param protocolVersion Protocol version the sender speaks (default: `kProtocolVersion`).
inline Envelope makeHello(std::uint32_t protocolVersion = kProtocolVersion) {
    Envelope env;
    env.kind = "hello";
    env.protocolVersion = protocolVersion;
    return env;
}

/// @brief Encodes @p env as a single JSON line.
/// @throws std::runtime_error on serialisation failure (should never happen for valid input).
inline std::string encode(const Envelope& env) {
    std::string out;
    if (auto errCode = glz::write_json(env, out)) {
        throw std::runtime_error("envelope encode failed: " + glz::format_error(errCode, out));
    }
    return out;
}

/// @brief Decodes @p json into an `Envelope`.
///
/// Enforces a wire-layer size cap: inputs longer than `kMaxEnvelopeBytes` are
/// rejected before any parsing, bounding the cost of an oversized or
/// deeply-nested payload smuggled inside the opaque `body` string (see
/// `kMaxEnvelopeBytes`). This guard does **not** cap the *inner* re-parse of
/// `body` performed later by the action codec — that codec must impose its own
/// limits — but it does bound the total message a single `decode` will accept.
///
/// glaze 7.4 runs with its default options here. Note the parser does **not**
/// reject duplicate JSON object keys: a duplicated key is accepted and the last
/// occurrence wins. There is no glaze option to change this, so `decode` cannot
/// enforce rejection; callers must not treat duplicate-key rejection as a
/// security boundary (see docs/spec/wire.md).
///
/// @param json Serialized envelope JSON to decode.
/// @return The decoded `Envelope`.
/// @throws std::runtime_error if @p json exceeds `kMaxEnvelopeBytes` or is not a
///         valid envelope.
inline Envelope decode(std::string_view json) {
    if (json.size() > kMaxEnvelopeBytes) {
        throw std::runtime_error("envelope decode failed: input exceeds maximum size (" + std::to_string(json.size()) +
                                 " > " + std::to_string(kMaxEnvelopeBytes) + " bytes)");
    }
    Envelope env{};
    // Ignore unknown keys so the envelope is forward-compatible: a newer peer may
    // add fields a older peer does not know, and vice versa, without a hard parse
    // failure. Duplicate keys are still accepted last-wins (glaze offers no reject
    // option) — see docs/spec/wire.md "Parsing guarantees and hardening".
    // `null_terminated = false` is required, not cosmetic: `json` is a caller-supplied
    // `string_view` with no guarantee of a trailing '\0' (e.g. a raw socket read).
    // Left at glaze's default (true), `skip_ws` assumes it can scan past `end`
    // looking for a terminator that may not exist — a heap-buffer-overflow on
    // adversarial input (found by the wire_decode fuzz harness, see
    // docs/spec/testing_strategy.md). This costs nothing: it only disables an
    // optimization that never applied to us.
    static constexpr glz::opts kLenient{.null_terminated = false, .error_on_unknown_keys = false};
    if (auto errCode = glz::read<kLenient>(env, json)) {
        throw std::runtime_error("envelope decode failed: " + glz::format_error(errCode, json));
    }
    return env;
}

/// @brief Outcome of a `"hello"` protocol-version negotiation attempt.
enum class ProtocolNegotiationResult : std::uint8_t {
    /// @brief The peer understood `"hello"` and accepted the announced version.
    Negotiated,
    /// @brief The peer does not understand `"hello"` (a pre-negotiation legacy peer).
    LegacyPeer,
};

/// @brief Classifies a decoded reply to a `"hello"` envelope.
///
/// @param reply Decoded reply envelope — the result of `decode()` on the
///        response to a `"hello"` round-trip.
/// @return `Negotiated` if @p reply's `kind` is `"ok"`; `LegacyPeer` if it is
///         an `"err"` whose `message` is exactly `"unknown envelope kind:
///         hello"` — the generic unrecognised-`kind` message a pre-negotiation
///         `RemoteServer` produces for a `kind` it does not switch on.
/// @throws std::runtime_error if @p reply is any other `"err"` (e.g.
///         `"protocol version unsupported"`), so the caller refuses to
///         proceed instead of silently treating an explicit rejection as a
///         legacy peer.
inline ProtocolNegotiationResult interpretHelloReply(const Envelope& reply) {
    if (reply.kind == "ok") {
        return ProtocolNegotiationResult::Negotiated;
    }
    if (reply.message == "unknown envelope kind: hello") {
        return ProtocolNegotiationResult::LegacyPeer;
    }
    throw std::runtime_error("protocol negotiation failed: " +
                             (reply.message.empty() ? std::string{"malformed reply"} : reply.message));
}

}  // namespace morph::wire

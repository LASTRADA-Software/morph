// SPDX-License-Identifier: Apache-2.0

#pragma once
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <glaze/glaze.hpp>
#include <limits>
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
///                   With `shared` set, additionally uses `primary` and becomes
///                   a register-or-attach against the shared directory.
/// - `"attach"`    — client re-points at a different `primary` of `typeId`,
///                   releasing `modelId` if non-zero. Replies `ok` with the
///                   target instance's id in `modelId`.
/// - `"assign"`    — client files live `modelId` under `primary` of `typeId`.
/// - `"instances"` — client asks for the live shared primary keys of `typeId`.
///                   Replies `ok` with a JSON array of key strings in `body`.
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

    /// @brief Primary key of the instance being registered or attached to.
    ///
    /// Carried on `register` (when `shared` is set) and on `attach`, always as
    /// the key's canonical string encoding (`morph::model::keyToString`)
    /// whatever its C++ type, so the server's directory needs one map type
    /// rather than one per key type. Empty means "no primary" — the instance is
    /// anonymous and cannot be shared. Ignored on every other kind.
    ///
    /// Distinct from `contextKey`, which continues to mean only "entity key for
    /// the action log". A keyed model will normally set both to the same value,
    /// but conflating the fields would change behaviour for callers already
    /// setting `contextKey` for journal purposes.
    std::string primary;

    /// @brief Whether a `register` should join the shared instance directory.
    ///
    /// `true` makes the request a *register-or-attach*: the server returns the
    /// existing instance for `(typeId, primary)` if one is live, otherwise
    /// creates it and enters it in the directory. `false` (the default, and the
    /// value every pre-existing client sends) is today's behaviour exactly — a
    /// private instance, invisible to the directory.
    bool shared = false;

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

/// @brief Builds a shared (register-or-attach) `register` envelope.
///
/// The server returns the live instance for `(typeId, primary)` if one exists,
/// otherwise creates it and enters it in the shared directory. A shared instance
/// is recorded with no owner principal, so `IAuthorizer::authorizeInstance`'s
/// documented `ownerPrincipal == ctx.principal` policy does not lock the second
/// client out of an instance the first created — see
/// docs/planned/shared_model_instances.md.
///
/// @param typeId     Model type id to register or attach to.
/// @param primary    Canonical string encoding of the instance's primary key.
/// @param contextKey Optional entity key for the action log (default: none).
inline Envelope makeRegisterShared(std::string typeId, std::string primary, std::string contextKey = {}) {
    Envelope env;
    env.kind = "register";
    env.typeId = std::move(typeId);
    env.primary = std::move(primary);
    env.contextKey = std::move(contextKey);
    env.shared = true;
    return env;
}

/// @brief Builds an `attach` envelope — re-points a client at a different primary.
///
/// Semantically a `deregister` + shared `register` pair, made a single request
/// so a re-pointing handler cannot lose its slot to `LimitPolicy::maxLiveModels`
/// between releasing the old instance and acquiring the new one.
///
/// @param typeId  Model type id.
/// @param primary Canonical string encoding of the primary key to attach to.
/// @param modelId Instance the client is currently attached to; `0` if none.
inline Envelope makeAttach(std::string typeId, std::string primary, uint64_t modelId = 0) {
    Envelope env;
    env.kind = "attach";
    env.typeId = std::move(typeId);
    env.primary = std::move(primary);
    env.modelId = modelId;
    env.shared = true;
    return env;
}

/// @brief Builds an `assign` envelope — files a live instance under a primary key.
///
/// The promotion half of keyed instances: an action that creates its own entity
/// runs on a not-yet-keyed instance, and only the reply carries the generated
/// key. Assigning promotes that same instance in place, so nothing the create
/// did is stranded on a throwaway.
/// @param typeId  Model type id.
/// @param primary Canonical string encoding of the key to file the instance under.
/// @param modelId Live instance to promote.
inline Envelope makeAssign(std::string typeId, std::string primary, uint64_t modelId) {
    Envelope env;
    env.kind = "assign";
    env.typeId = std::move(typeId);
    env.primary = std::move(primary);
    env.modelId = modelId;
    env.shared = true;
    return env;
}

/// @brief Builds an `instances` envelope — asks for the live shared keys of a type.
///
/// The reply's `body` is a JSON array of canonical key strings. Only instances
/// created through a shared `register`/`attach` are listed; a private instance
/// is invisible to the directory by construction.
/// @param typeId Model type id to enumerate.
inline Envelope makeInstances(std::string typeId) {
    Envelope env;
    env.kind = "instances";
    env.typeId = std::move(typeId);
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
/// Applied to `makeErr`'s `message` only, and no longer for JSON validity —
/// `detail::EscapingWriteOpts` now handles that for every field. What remains is
/// an output-sanitization concern specific to this one field: an `err` message
/// echoes untrusted content back (an unrecognized `Envelope::kind`, a caught
/// exception's `what()`) and is overwhelmingly destined for a log or a console,
/// where a raw 0x1B would carry an ANSI escape sequence into the reader's
/// terminal. `message` is diagnostic text rather than data that must round-trip
/// byte-for-byte, so replacing these bytes with a printable transcription costs
/// nothing and keeps them inert.
///
/// Originally added for the invalid-JSON failure found by the
/// `fuzz_dispatch_execute` harness via an `err` reply echoing an unrecognized
/// `kind` verbatim (see docs/spec/testing_strategy.md, "Known findings"); that
/// finding is now covered at the writer instead.
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

/// @brief Write options that escape ASCII control bytes as `\\uXXXX` sequences.
///
/// glaze 7.4 leaves control bytes unescaped by default, which breaks `encode`
/// two different ways depending on where the byte lands:
///
/// - **Invalid output.** RFC 8259 requires U+0000 through U+001F to be escaped,
///   and glaze's own reader enforces that, so an envelope carrying a raw 0x0B
///   anywhere serializes to JSON the peer's `decode` then throws on.
/// - **Silent corruption.** Worse, the writer's chunked fast path mangles such a
///   byte outright once the string also contains an escaped character: with a
///   `\\` or `"` earlier in the same string, a 0x0B at certain offsets is written
///   as *two* 0x00 bytes. The payload is destroyed before it reaches the wire,
///   so no amount of post-processing on the serialized form can recover it —
///   the escaping has to happen inside the writer.
///
/// Enabling the option is glaze's documented remedy for exactly this (see its
/// README, "escape control characters"); it is off by default there only because
/// embedded nulls are hazardous for C APIs, which does not apply to a
/// `std::string` wire field. Escaping is lossless in both directions, so any
/// such byte round-trips unchanged rather than being replaced.
///
/// Applies to writing only. `decode` needs no counterpart: glaze's reader
/// already accepts `\\uXXXX` and turns it back into the original byte.
struct EscapingWriteOpts : glz::opts {
    /// @brief Emit control bytes as `\\uXXXX` rather than raw.
    // NOLINTNEXTLINE(readability-identifier-naming) — the name is glaze's, not ours; the option is matched by name.
    bool escape_control_characters = true;
};

/// @brief Best-effort recovery of an envelope's `callId` without decoding it.
///
/// For the one case where a transport must answer a message it has decided
/// *not* to parse — a frame rejected for exceeding a transport-level size cap,
/// before `decode` is ever called. Such a reply still has to be addressed:
/// `callId == 0` is the wire's "this is a reply to a synchronous control call"
/// discriminator (see `QtWebSocketBackend::onTextMessage`), so an `err` sent
/// with a zeroed id does not merely fail to resolve the execute it was meant
/// for — it resumes whatever unrelated `register`/`deregister` happens to be
/// parked, handing it a reply belonging to another call entirely.
///
/// Scans at most @p maxScanBytes, so the size cap it serves keeps its value as
/// a cost bound; `callId` is the second field `encode` writes, so it lands well
/// inside even a small window. A `"callId":` sequence cannot be forged from
/// within an earlier string field, because `encode` escapes any embedded quote
/// (yielding `\"callId\":`, which does not match).
///
/// @param json         Raw, undecoded envelope text.
/// @param maxScanBytes Prefix length to search. Default 1 KiB.
/// @return The parsed `callId`, or `0` if absent, unparseable, or out of range —
///         i.e. it degrades to exactly the behavior it replaces.
[[nodiscard]] inline std::uint64_t peekCallId(std::string_view json, std::size_t maxScanBytes = 1024) noexcept {
    static constexpr std::string_view kKey = "\"callId\":";
    const std::string_view window = json.substr(0, std::min(json.size(), maxScanBytes));
    const auto keyPos = window.find(kKey);
    if (keyPos == std::string_view::npos) {
        return 0;
    }
    std::size_t idx = keyPos + kKey.size();
    // idx is bounded by the same condition that guards the access.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    while (idx < window.size() && (window[idx] == ' ' || window[idx] == '\t')) {
        ++idx;
    }
    std::uint64_t value = 0;
    bool sawDigit = false;
    for (; idx < window.size(); ++idx) {
        // idx is bounded by the loop condition.
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        const char chr = window[idx];
        if (chr < '0' || chr > '9') {
            break;
        }
        const auto digit = static_cast<std::uint64_t>(chr - '0');
        if (value > (std::numeric_limits<std::uint64_t>::max() - digit) / 10U) {
            return 0;  // out of range — treat as unrecoverable rather than wrap
        }
        value = (value * 10U) + digit;
        sawDigit = true;
    }
    return sawDigit ? value : 0;
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
///
/// Writes with `detail::EscapingWriteOpts`, so raw ASCII control bytes in any
/// string field become `\uXXXX` escapes. Without it, a single 0x0B anywhere in
/// `body`, `modelType`, `contextKey`, `typeId`, `actionType`, or the session's
/// `principal`/`token` yields an envelope the peer's `decode` throws on — and,
/// where the same string also contains a `\` or `"`, one glaze silently
/// corrupts before it is ever sent. Both are payload-triggered transport
/// failures, not caller errors. The escape is lossless, so such bytes still
/// round-trip byte-for-byte.
///
/// @param env Envelope to serialize.
/// @return The serialized envelope as valid, re-decodable JSON.
/// @throws std::runtime_error on serialisation failure (should never happen for valid input).
inline std::string encode(const Envelope& env) {
    std::string out;
    if (auto errCode = glz::write<detail::EscapingWriteOpts{}>(env, out)) {
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

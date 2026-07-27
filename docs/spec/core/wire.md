# The `wire` types — design

`morph::wire` provides the JSON wire envelope and associated helpers used
between any client and `morph::backend::RemoteServer`. A single `Envelope` struct
carries all request and reply variants, discriminated by a `kind` string field.

## Contents

- [Envelope](#envelope)
- [Factory functions](#factory-functions)
- [Encode and decode](#encode-and-decode)
- [Parsing guarantees and hardening](#parsing-guarantees-and-hardening)
- [Protocol version negotiation](#protocol-version-negotiation)
- [Action-evolution policy](#action-evolution-policy)
- [API reference](#api-reference)
- [Design decisions](#design-decisions)

## Envelope

The `Envelope` struct supersedes the legacy pipe-delimited protocol. Every field
is present in the struct so the JSON shape is fixed; callers populate only what
their `kind` needs and leave the rest as default-constructed values.

### Discriminator values

| `kind`       | Direction | Purpose | Key fields |
|---|---|---|---|
| `"register"` | request   | Client requests model creation. | `typeId`, `contextKey` (optional stable identity) |
| `"deregister"` | request | Client destroys an instance. | `modelId` |
| `"execute"`  | request   | Client dispatches an action. | `callId`, `modelId`, `modelType`, `actionType`, `body`, `session` |
| `"hello"`    | request   | Client announces its protocol version, once per connection, before any `register`/`execute`. See [Protocol version negotiation](#protocol-version-negotiation). | `protocolVersion` |
| `"ok"`       | reply     | Server success. | `callId`, `body` (serialized result, or — for a `"hello"` reply — the server's `ProtocolRange`), `modelId` (for register-replies) |
| `"err"`      | reply     | Server failure. | `callId`, `message` |

### `contextKey` — stable identity

`contextKey` is an optional stable identity for the new instance (e.g. an account
id). When present, the server-side holder gets an action log attached (if a
`LogProvider` is configured). When empty, no action log is attached. Ignored on
every kind other than `"register"`.

### `session` — authorization context

The `session` field carries a `morph::session::Context` for authorization and
routing. Populated on `"execute"`; ignored on every other kind.

The `principal` sub-field a client sends is a *claim*, not a fact: a configured
`session::IAuthorizer` may verify the accompanying `token` and **overwrite**
`session.principal` with the verified identity before dispatch (see
`session.hpp`). Wire-layer `encode`/`decode` never inspect or validate the
session — they round-trip it verbatim; enforcement lives in the server.

## Factory functions

Five free functions construct `Envelope` instances with the correct `kind` and
relevant fields. Callers never set `kind` manually.

| Function | `kind` | Parameters |
|---|---|---|
| `makeRegister(typeId, contextKey = {})` | `"register"` | Model type id, optional stable identity. |
| `makeDeregister(modelId)` | `"deregister"` | Instance id to destroy. |
| `makeHello(protocolVersion = kProtocolVersion)` | `"hello"` | Protocol version the sender speaks. See [Protocol version negotiation](#protocol-version-negotiation). |
| `makeOk(callId = 0, body = {}, modelId = 0)` | `"ok"` | Correlation id, serialized result (stored in the `body` field), optional model id (for register-replies). |
| `makeErr(message, callId = 0)` | `"err"` | Error message, optional correlation id. |

For `"execute"` there is no factory — callers construct the `Envelope` directly
and set `kind = "execute"`, or use the `Client`/`RemoteServer` APIs which handle
it internally.

## Encode and decode

| Function | Signature | Notes |
|---|---|---|
| `encode` | `std::string encode(const Envelope&)` | Serializes to a single JSON line via `glz::write<detail::EscapingWriteOpts{}>`. Throws `std::runtime_error` on failure (should never happen for valid input). Escapes ASCII control bytes — see [Control bytes in string fields](#control-bytes-in-string-fields). |
| `decode` | `Envelope decode(std::string_view)` | Deserializes from JSON via `glz::read<{.error_on_unknown_keys = false}>`. Rejects input longer than `kMaxEnvelopeBytes` and throws `std::runtime_error` on an oversized or syntactically malformed envelope. **Ignores unknown/extra keys** (forward compatibility) and **does not reject duplicate JSON keys** — see [Parsing guarantees and hardening](#parsing-guarantees-and-hardening). |

glaze reflects the struct's public members, so the JSON object keys are exactly
the C++ field names (`kind`, `callId`, `typeId`, `contextKey`, `modelId`,
`modelType`, `actionType`, `body`, `message`, `session`). `decode` starts from a
default-constructed `Envelope`, so any key absent from the input JSON keeps its
default value — omitting fields a given `kind` does not use is expected and does
not throw. `decode` reads with `error_on_unknown_keys = false`, so an
**unknown/extra** key is **ignored** rather than rejected: a newer peer may add a
field an older peer does not know (and vice versa) without breaking the parse —
this is the wire's forward-compatibility contract. Syntactically malformed JSON
is still a hard parse error that throws. Servers catch that thrown exception and
turn it into an `"err"` reply rather than propagating it (see
`RemoteServer::handle` / `handleInline`).

### Control bytes in string fields

`encode` writes with `detail::EscapingWriteOpts`, a `glz::opts` refinement that
turns on glaze's `escape_control_characters`. glaze leaves ASCII control bytes
(U+0000–U+001F) unescaped by default, which breaks the envelope two distinct
ways depending on where such a byte lands:

- **Invalid output.** RFC 8259 requires those code points to be escaped, and
  glaze's own reader enforces it — so an envelope carrying a raw `0x0B`
  anywhere serializes to JSON the peer's `decode` throws on. Found by the
  `fuzz_dispatch_execute` harness via an `err` reply echoing an unrecognized
  `kind`.
- **Silent corruption.** Worse, with the option off the writer's chunked fast
  path *mangles* such a byte once the same string also contains an escaped
  character: with a `\` or `"` earlier in the string, a `0x0B` at certain
  offsets is written out as *two* `0x00` bytes. The payload is destroyed before
  it reaches the wire, and the result still decodes — so nothing downstream can
  detect it.

Escaping is lossless in both directions: such bytes round-trip byte-for-byte,
which matters because `body`, `modelType`, `actionType`, `contextKey`, `typeId`
and the session's `principal`/`token` all carry caller data. `decode` needs no
counterpart — glaze's reader already accepts `\uXXXX`.

`makeErr` additionally replaces control bytes in its `message` with a printable
`\xHH` transcription. That is no longer about JSON validity but about output
sanitization: an `err` message echoes untrusted content back and is
overwhelmingly destined for a log or console, where a raw `0x1B` would carry an
ANSI escape sequence into the reader's terminal. `message` is diagnostic text,
not data that must round-trip, so replacement costs nothing there.

### `detail::peekCallId` — addressing a reply to a message never decoded

A transport that rejects a frame *before* decoding it (e.g.
`QtWebSocketServerConfig::maxMessageBytes`) still has to answer, and the reply
must be addressed. `wire::detail::peekCallId(json, maxScanBytes = 1024)`
recovers `callId` with a bounded prefix scan, returning `0` when absent,
unparseable, or out of range. The bound keeps the size cap meaningful as a cost
guard; `callId` is the second field `encode` writes, so it lands well inside
even a small window, and a `"callId":` sequence cannot be forged from an earlier
string field because `encode` escapes any embedded quote.

Replying with a zeroed `callId` is not a harmless degradation: `0` is the
client's *synchronous-reply discriminator*, so such a reply resumes whatever
`register`/`deregister` happens to be parked and hands it another call's
result, while the execute it was meant for never resolves at all.

## Parsing guarantees and hardening

`decode` is the wire's untrusted-input boundary. Its guarantees — and, as
important, its *non*-guarantees — are:

### Message-size cap (`kMaxEnvelopeBytes`)

`decode` rejects any input longer than `kMaxEnvelopeBytes` (**8 MiB**) *before*
handing it to the parser, throwing `std::runtime_error`. This is a
denial-of-service backstop, not a correctness check: it bounds the peak
allocation and parse cost a single message can impose. `encode` does not cap
output; a server that constructs an `"ok"` reply larger than the cap produces a
message its own `decode` would reject, so keep result payloads within the bound.
Transports that want a tighter limit should enforce it before calling `decode`.
The shipped Qt transport does exactly this: `morph::qt::QtWebSocketServerConfig::maxMessageBytes` (default: this same constant) rejects an oversized frame before it reaches `RemoteServer::handle()` — see [backend.md](backend.md#qtwebsocketserver--server-side-websocket-transport).

### The `body` double-parse hazard

`body` is a `std::string` carrying **nested JSON as an opaque string**. The
outer `decode` sees it as one flat scalar and never walks its structure, so the
action codec re-parses `body` a *second* time later (on the strand thread, after
authorization) via the action's `fromJson`. Two consequences:

- Any structural or depth check the outer parse performs does **not** apply to
  the contents of `body`. A deeply-nested or pathological payload smuggled
  inside `body` is invisible to the outer parse and only detonates on the inner
  re-parse. glaze 7.4 exposes **no** `max_depth` read option, so the outer
  parse cannot cap nesting depth even for the fields it does walk; the
  `kMaxEnvelopeBytes` size cap is the only wire-layer bound, and it works
  precisely because it bounds the *whole* message including `body`.
- The inner re-parse needs **its own** limits. The wire layer cannot impose
  them; the action codec must (the size cap does bound the total, so `body`
  cannot exceed `kMaxEnvelopeBytes` either).

`tests/fuzz/fuzz_wire_decode.cpp` (built under `MORPH_BUILD_FUZZERS=ON`) fuzzes
exactly this: `decode()`'s outer parse and, for a decoded `execute` envelope,
the inner `ActionTraits::fromJson` re-parse of `body` — proving over a
coverage-guided distribution of inputs, not just the hand-picked cases in
`test_wire_hardening.cpp`, that both stages either succeed or throw
`std::runtime_error` and never crash or hang. See
[testing_strategy.md](../testing_strategy.md).

### Duplicate JSON keys are accepted (last-wins)

glaze 7.4 does **not** reject duplicate object keys, and exposes no option to
make it do so. A duplicated key — top-level (`{"kind":"execute","kind":"register"}`
decodes to `kind == "register"`) or nested (a repeated `session` keeps the last
occurrence) — is silently accepted with the **last** value winning. `decode`
therefore **cannot** enforce rejection via options and does not attempt a
hand-rolled scan. This is a parser-differential smuggling primitive: a
validating proxy or logger that reads the *first* occurrence sees a different
message than morph, which keeps the *last*. Callers **must not** rely on
duplicate-key rejection as a security boundary; a security-sensitive front proxy
must canonicalize or reject duplicate keys itself before the envelope reaches
`decode`.

## Protocol version negotiation

`kProtocolVersion` (currently `1`) is the protocol version this build of morph
speaks. `Envelope::protocolVersion` carries it; `0` means "unspecified / legacy
peer" — the value on every envelope an old encoder (unaware of the field)
produces, and the value an old decoder (unaware of the field) leaves untouched
on an incoming envelope that omits it. Because `decode` ignores unknown keys
and `encode` always writes every field, a `protocolVersion`-aware peer talking
to an unaware one round-trips the field as `0` and nothing else changes.

### The `"hello"` control kind

A dedicated `kind` negotiates the protocol version once, before any
`"register"`/`"execute"` on the same connection:

| `kind` | Direction | Fields used | Reply |
|---|---|---|---|
| `"hello"` | request | `protocolVersion` (the sender's version, from `makeHello()`) | `"ok"` with `body` = the server's `ProtocolRange` (`{min, max}`), or `"err"` |

`RemoteServer::setSupportedVersionRange(min, max)` configures the inclusive
range a server advertises; it defaults to `{kProtocolVersion, kProtocolVersion}`
— this build's single supported version. On `"hello"`, `RemoteServer` compares
the request's `protocolVersion` against that range:

- Inside the range → `"ok"` reply, `body` = `glz::write_json` of a
  `ProtocolRange{min, max}`.
- Outside the range → `"err"` reply, `message = "protocol version unsupported"`.

`SimulatedRemoteBackend::negotiateProtocolVersion()` and
`QtWebSocketBackend::negotiateProtocolVersion()` send a `"hello"` — over the
same synchronous control path as `registerModel` (`handleInline` for the
simulated backend, `sendSync` for the Qt backend) — and classify the decoded
reply through `interpretHelloReply`:

- The peer's `"ok"` → `ProtocolNegotiationResult::Negotiated`.
- An `"err"` whose `message` is exactly `"unknown envelope kind: hello"` (the
  generic unrecognised-`kind` message a pre-negotiation `RemoteServer`
  produces for a `kind` it does not switch on) → `ProtocolNegotiationResult::LegacyPeer`.
  The caller is not blocked from proceeding — a legacy peer simply never spoke
  the handshake, exactly as it would have before this feature existed.
- Any other `"err"` (e.g. `"protocol version unsupported"`) → throws
  `std::runtime_error`, refusing to proceed rather than surfacing a confusing
  per-request failure later.

Calling `negotiateProtocolVersion()` is **opt-in** — the application decides
when (typically once, right after `waitForConnected()` on the Qt backend, or
right after constructing a `SimulatedRemoteBackend`) and whether to call it at
all. A caller that never calls it sees exactly today's behavior: no handshake,
no version check, `protocolVersion` stays `0` on every envelope.

### Backward and forward compatibility

- **New client, old (unmodified) server.** The client's `"hello"` reaches
  `dispatchMessage`'s final `else` branch (the server does not recognise
  `"hello"`), producing `err "unknown envelope kind: hello"`. The client's
  `interpretHelloReply` recognises this exact message and returns `LegacyPeer`
  rather than throwing — the caller proceeds exactly as it would have before
  this feature existed.
- **Old client, new server.** An old client never sends `"hello"`; the server
  never receives one and behaves exactly as before (register/execute only).
- **New client, new server, incompatible versions.** `setSupportedVersionRange`
  lets a server narrow its accepted range (e.g. after a breaking
  `kProtocolVersion` bump and a deprecation window); a client outside it gets a
  clear `"protocol version unsupported"` refusal at connect time instead of a
  confusing failure on the first `execute`.

## Action-evolution policy

The passive forward-compat contract (`error_on_unknown_keys = false` on
`wire::decode`) covers only the *outer* `Envelope`. The `body` field is opaque
JSON re-parsed a second time by each action's `ActionTraits::fromJson` (the
"body double-parse", above) — and that inner parse has its own, independent
forward-compatibility story:

- **`BRIDGE_REGISTER_ACTION`-generated code is forward-compatible.** The
  macro's generated `fromJson`/`resultFromJson` read with
  `glz::read<glz::opts{.error_on_unknown_keys = false}>` — the same convention
  `wire::decode` and `session_auth.hpp`'s claims parser already use — so a
  field a newer peer added is silently ignored by an older-compiled action
  struct. `toJson`/`resultToJson` are unaffected (writing is always
  exact-shape).
- **A hand-written `ActionTraits<T>::fromJson`/`resultFromJson` must opt into
  the same convention explicitly.** Plain `glz::read_json` defaults to
  `error_on_unknown_keys = true` (glaze's default `opts{}`) and throws
  `morph::model::detail::ParseError` on an unrecognised field. An action
  author who hand-writes the codec instead of using `BRIDGE_REGISTER_ACTION`
  must read with the lenient options to get the same forward compatibility:

  ```cpp
  static MyAction fromJson(std::string_view json) {
      MyAction action{};
      static constexpr glz::opts kLenient{.error_on_unknown_keys = false};
      if (auto err = glz::read<kLenient>(action, json)) {
          throw morph::model::detail::ParseError{glz::format_error(err, json)};
      }
      return action;
  }
  ```

With that convention in place (automatic via the macro, opt-in by hand), the
policy for evolving an action or result struct across client/server versions
is:

- **Additive-only within a major version.** New fields must be optional (a
  `std::optional<...>`, an empty-capable `Quantity`/`Timestamp`, or a type with
  a safe default) so an older peer that omits them decodes cleanly and a newer
  peer that receives them from an older sender sees the default. This is what
  the lenient `fromJson` convention above makes actually true, rather than
  aspirational.
- **Never renumber or rename protocol vocabulary.** Mirrors the existing unit
  enum rule ("Unit ids are protocol vocabulary: append enumerators, never
  renumber or rename," [ARCHITECTURE.md](../../ARCHITECTURE.md)). Renaming a
  field is a removal plus an addition — a break, not a rename, from the wire's
  point of view.
- **Deprecation window.** A field slated for removal is first marked
  deprecated (kept on the wire, ignored by new code, noted in the type's own
  spec) for at least one full library release, then removed only at a
  `kProtocolVersion` bump.
- **Removals or retypes require a `kProtocolVersion` bump.** Any non-additive
  change increments `kProtocolVersion`; a server that must keep serving
  pre-bump clients through their deprecation window widens its
  `setSupportedVersionRange` accordingly, then narrows it once the window
  closes.

## API reference

### `wire::Envelope`

| Field | Type | Default | Used by `kind` |
|---|---|---|---|
| `kind` | `std::string` | `""` | All — the discriminator. |
| `callId` | `uint64_t` | `0` | `"execute"`, `"ok"`, `"err"` — correlation id for async matching. |
| `typeId` | `std::string` | `""` | `"register"` — model type id. |
| `contextKey` | `std::string` | `""` | `"register"` — stable identity for the new instance. |
| `modelId` | `uint64_t` | `0` | `"deregister"`, `"execute"`, `"ok"(register)` — instance id. |
| `modelType` | `std::string` | `""` | `"execute"` — routing key for `ActionDispatcher`. |
| `actionType` | `std::string` | `""` | `"execute"` — second routing key. |
| `body` | `std::string` | `""` | `"execute"`, `"ok"` — serialized JSON payload. |
| `message` | `std::string` | `""` | `"err"` — free-text error message. |
| `session` | `::morph::session::Context` | default | `"execute"` — authorization and routing context. |
| `protocolVersion` | `uint32_t` | `0` | `"hello"` — protocol version the sender speaks. `0` means unspecified/legacy peer; not otherwise inspected. |

### Factory functions

| Symbol | Signature |
|---|---|
| `makeRegister` | `Envelope makeRegister(std::string typeId, std::string contextKey = {})` |
| `makeDeregister` | `Envelope makeDeregister(uint64_t modelId)` |
| `makeHello` | `Envelope makeHello(uint32_t protocolVersion = kProtocolVersion)` |
| `makeOk` | `Envelope makeOk(uint64_t callId = 0, std::string body = {}, uint64_t modelId = 0)` |
| `makeErr` | `Envelope makeErr(std::string message, uint64_t callId = 0)` |

### Protocol version negotiation types

| Symbol | Signature / shape | Notes |
|---|---|---|
| `ProtocolRange` | `struct { uint32_t min = kProtocolVersion; uint32_t max = kProtocolVersion; }` | A server's supported version range; serialized into a `"hello"` `"ok"` reply's `body`. |
| `ProtocolNegotiationResult` | `enum class : uint8_t { Negotiated, LegacyPeer }` | Outcome of `interpretHelloReply`. |
| `interpretHelloReply` | `ProtocolNegotiationResult interpretHelloReply(const Envelope& reply)` | Throws `std::runtime_error` if `reply` is an `"err"` other than `"unknown envelope kind: hello"`. |

### Serialization

| Symbol | Signature | Throws |
|---|---|---|
| `encode` | `std::string encode(const Envelope&)` | `std::runtime_error` on serialisation failure |
| `decode` | `Envelope decode(std::string_view)` | `std::runtime_error` if the input exceeds `kMaxEnvelopeBytes` or is a syntactically malformed envelope. Unknown/extra keys are **ignored** (`error_on_unknown_keys = false`); duplicate keys do **not** throw (last-wins) — see [Parsing guarantees and hardening](#parsing-guarantees-and-hardening). |

### Constants

| Symbol | Type | Value | Meaning |
|---|---|---|---|
| `kMaxEnvelopeBytes` | `std::size_t` | `8 * 1024 * 1024` (8 MiB) | Maximum serialized envelope size `decode` will accept; larger input is rejected before parsing. |
| `kProtocolVersion` | `std::uint32_t` | `1` | Protocol version this build speaks; see [Protocol version negotiation](#protocol-version-negotiation). |

## Design decisions

| Decision | Choice | Why |
|---|---|---|
| Single struct vs. discriminated union | **One `Envelope` struct, all fields present** | The JSON shape is fixed and predictable; callers populate only what their kind needs. Avoids a tagged-union complexity that would add no benefit over a single struct with a `kind` string. |
| `kind` as a string vs. enum | **`std::string`** | JSON naturally discriminates by string; avoids an enum-to-string mapping. The factory functions (`makeRegister`, etc.) ensure callers never set `kind` manually. |
| `"execute"` has no factory | **No factory** | `"execute"` envelopes are typically constructed by higher-level APIs (`Client`, `RemoteServer`), not by end users. Adding a factory would be dead code at the wire layer. |
| Factory functions are `inline` | **Header-only** | The entire wire module lives in the header. Wrapping each factory as a named function keeps construction safe (correct `kind`, no forgotten fields) without a separate compilation unit. |
| Serialization via glaze | **`glz::write_json` / `glz::read<{.error_on_unknown_keys = false}>`** | glaze is the project's existing JSON library; no additional dependency. `decode` tolerates unknown keys for forward compatibility. Throws on failure rather than returning error codes because encode/decode at the wire boundary should fail loud and early. |
| `contextKey` vs. `modelId` for register | **`contextKey` is a separate field, not `modelId`** | `modelId` is server-assigned (a `uint64_t` handle); `contextKey` is a client-chosen stable identity string. They are semantically different and the server treats them differently (log attachment vs. instance routing). |
| `session` as a dedicated field | **`::morph::session::Context`** | Session context is a first-class concern for authorization and routing, not an opaque sub-payload in `body`. Keeping it at the Envelope level ensures every `"execute"` carries it without caller discipline. |
| Wire-layer size cap | **`kMaxEnvelopeBytes` (8 MiB), checked before parsing** | The `body` double-parse means depth/structure checks on the outer parse never reach the nested payload; a total-length bound is the one check that does cover the whole message (including `body`) and it is cheap. 8 MiB is generous for legitimate payloads while keeping a single message's peak allocation bounded. glaze 7.4 has no `max_depth` option, so a size cap is the only wire-layer depth mitigation available. |
| Duplicate JSON keys | **Accepted, last-wins (not rejected)** | glaze 7.4 exposes no option to error on duplicate keys and a correct hand-rolled JSON-aware scan would be complex and error-prone. Rather than a fragile mitigation, the behavior is documented honestly and callers are told not to rely on rejection; a security-sensitive proxy must canonicalize duplicates upstream. |
# The `wire` types — design

`morph::wire` provides the JSON wire envelope and associated helpers used
between any client and `morph::backend::RemoteServer`. A single `Envelope` struct
carries all request and reply variants, discriminated by a `kind` string field.

## Contents

- [Envelope](#envelope)
- [Factory functions](#factory-functions)
- [Encode and decode](#encode-and-decode)
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
| `"ok"`       | reply     | Server success. | `callId`, `body` (serialized result), `modelId` (for register-replies) |
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
`session::Authorizer` may verify the accompanying `token` and **overwrite**
`session.principal` with the verified identity before dispatch (see
`session.hpp`). Wire-layer `encode`/`decode` never inspect or validate the
session — they round-trip it verbatim; enforcement lives in the server.

## Factory functions

Four free functions construct `Envelope` instances with the correct `kind` and
relevant fields. Callers never set `kind` manually.

| Function | `kind` | Parameters |
|---|---|---|
| `makeRegister(typeId, contextKey = {})` | `"register"` | Model type id, optional stable identity. |
| `makeDeregister(modelId)` | `"deregister"` | Instance id to destroy. |
| `makeOk(callId = 0, body = {}, modelId = 0)` | `"ok"` | Correlation id, serialized result (stored in the `body` field), optional model id (for register-replies). |
| `makeErr(message, callId = 0)` | `"err"` | Error message, optional correlation id. |

For `"execute"` there is no factory — callers construct the `Envelope` directly
and set `kind = "execute"`, or use the `Client`/`RemoteServer` APIs which handle
it internally.

## Encode and decode

| Function | Signature | Notes |
|---|---|---|
| `encode` | `std::string encode(const Envelope&)` | Serializes to a single JSON line via `glz::write_json`. Throws `std::runtime_error` on failure (should never happen for valid input). |
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

### The `body` double-parse hazard

`body` is a `std::string` carrying **nested JSON as an opaque string**. The
outer `decode` sees it as one flat scalar and never walks its structure, so the
action codec re-parses `body` a *second* time later (on the strand thread, after
authorization) via the action's `fromJson`. Two consequences:

- Any structural or depth check the outer parse performs does **not** apply to
  the contents of `body`. A deeply-nested or pathological payload smuggled
  inside `body` is invisible to the outer parse and only detonates on the inner
  re-parse. glaze 7.2.1 exposes **no** `max_depth` read option, so the outer
  parse cannot cap nesting depth even for the fields it does walk; the
  `kMaxEnvelopeBytes` size cap is the only wire-layer bound, and it works
  precisely because it bounds the *whole* message including `body`.
- The inner re-parse needs **its own** limits. The wire layer cannot impose
  them; the action codec must (the size cap does bound the total, so `body`
  cannot exceed `kMaxEnvelopeBytes` either).

### Duplicate JSON keys are accepted (last-wins)

glaze 7.2.1 does **not** reject duplicate object keys, and exposes no option to
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

### Factory functions

| Symbol | Signature |
|---|---|
| `makeRegister` | `Envelope makeRegister(std::string typeId, std::string contextKey = {})` |
| `makeDeregister` | `Envelope makeDeregister(uint64_t modelId)` |
| `makeOk` | `Envelope makeOk(uint64_t callId = 0, std::string body = {}, uint64_t modelId = 0)` |
| `makeErr` | `Envelope makeErr(std::string message, uint64_t callId = 0)` |

### Serialization

| Symbol | Signature | Throws |
|---|---|---|
| `encode` | `std::string encode(const Envelope&)` | `std::runtime_error` on serialisation failure |
| `decode` | `Envelope decode(std::string_view)` | `std::runtime_error` if the input exceeds `kMaxEnvelopeBytes` or is a syntactically malformed envelope. Unknown/extra keys are **ignored** (`error_on_unknown_keys = false`); duplicate keys do **not** throw (last-wins) — see [Parsing guarantees and hardening](#parsing-guarantees-and-hardening). |

### Constants

| Symbol | Type | Value | Meaning |
|---|---|---|---|
| `kMaxEnvelopeBytes` | `std::size_t` | `8 * 1024 * 1024` (8 MiB) | Maximum serialized envelope size `decode` will accept; larger input is rejected before parsing. |

## Design decisions

| Decision | Choice | Why |
|---|---|---|
| Single struct vs. discriminated union | **One `Envelope` struct, all fields present** | The JSON shape is fixed and predictable; callers populate only what their kind needs. Avoids a tagged-union complexity that would add no benefit over a single struct with a `kind` string. |
| `kind` as a string vs. enum | **`std::string`** | JSON naturally discriminates by string; avoids an enum-to-string mapping. The factory functions (`makeRegister`, etc.) ensure callers never set `kind` manually. |
| `"execute"` has no factory | **No factory** | `"execute"` envelopes are typically constructed by higher-level APIs (`Client`, `RemoteServer`), not by end users. Adding a factory would be dead code at the wire layer. |
| Factory functions are `inline` | **Header-only** | The entire wire module lives in the header. Wrapping each factory as a named function keeps construction safe (correct `kind`, no forgotten fields) without a separate compilation unit. |
| Serialization via glaze | **`glz::write_json` / `glz::read_json`** | glaze is the project's existing JSON library; no additional dependency. Throws on failure rather than returning error codes because encode/decode at the wire boundary should fail loud and early. |
| `contextKey` vs. `modelId` for register | **`contextKey` is a separate field, not `modelId`** | `modelId` is server-assigned (a `uint64_t` handle); `contextKey` is a client-chosen stable identity string. They are semantically different and the server treats them differently (log attachment vs. instance routing). |
| `session` as a dedicated field | **`::morph::session::Context`** | Session context is a first-class concern for authorization and routing, not an opaque sub-payload in `body`. Keeping it at the Envelope level ensures every `"execute"` carries it without caller discipline. |
| Wire-layer size cap | **`kMaxEnvelopeBytes` (8 MiB), checked before parsing** | The `body` double-parse means depth/structure checks on the outer parse never reach the nested payload; a total-length bound is the one check that does cover the whole message (including `body`) and it is cheap. 8 MiB is generous for legitimate payloads while keeping a single message's peak allocation bounded. glaze 7.2.1 has no `max_depth` option, so a size cap is the only wire-layer depth mitigation available. |
| Duplicate JSON keys | **Accepted, last-wins (not rejected)** | glaze 7.2.1 exposes no option to error on duplicate keys and a correct hand-rolled JSON-aware scan would be complex and error-prone. Rather than a fragile mitigation, the behavior is documented honestly and callers are told not to rely on rejection; a security-sensitive proxy must canonicalize duplicates upstream. |
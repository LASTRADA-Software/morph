# The `wire` types — design

`morph::wire` provides the JSON wire envelope and associated helpers used
between any client and `morph::net::RemoteServer`. A single `Envelope` struct
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
| `"ok"`       | reply     | Server success. | `callId`, `result` (`body`), `modelId` (for register-replies) |
| `"err"`      | reply     | Server failure. | `callId`, `message` |

### `contextKey` — stable identity

`contextKey` is an optional stable identity for the new instance (e.g. an account
id). When present, the server-side holder gets an action log attached (if a
`LogProvider` is configured). When empty, no action log is attached. Ignored on
every kind other than `"register"`.

### `session` — authorization context

The `session` field carries a `morph::session::Context` for authorization and
routing. Populated on `"execute"`; ignored on every other kind.

## Factory functions

Four free functions construct `Envelope` instances with the correct `kind` and
relevant fields. Callers never set `kind` manually.

| Function | `kind` | Parameters |
|---|---|---|
| `makeRegister(typeId, contextKey = {})` | `"register"` | Model type id, optional stable identity. |
| `makeDeregister(modelId)` | `"deregister"` | Instance id to destroy. |
| `makeOk(callId = 0, body = {}, modelId = 0)` | `"ok"` | Correlation id, serialized result, optional model id (for register-replies). |
| `makeErr(message, callId = 0)` | `"err"` | Error message, optional correlation id. |

For `"execute"` there is no factory — callers construct the `Envelope` directly
and set `kind = "execute"`, or use the `Client`/`RemoteServer` APIs which handle
it internally.

## Encode and decode

| Function | Signature | Notes |
|---|---|---|
| `encode` | `std::string encode(const Envelope&)` | Serializes to a single JSON line via `glz::write_json`. Throws `std::runtime_error` on failure (should never happen for valid input). |
| `decode` | `Envelope decode(std::string_view)` | Deserializes from JSON via `glz::read_json`. Throws `std::runtime_error` on malformed input. |

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
| `decode` | `Envelope decode(std::string_view)` | `std::runtime_error` on malformed JSON |

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
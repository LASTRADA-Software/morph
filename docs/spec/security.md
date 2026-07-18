# Security & the trust model

Cross-cutting spec covering morph's trust boundaries and the authenticated
session layer (`session.hpp` `Context`/`IAuthorizer`, `session_auth.hpp`
`SessionToken`/`TokenIssuer`/`TokenVerifier`/`SigningAuthorizer`, and the
`RemoteServer` enforcement points in `remote.hpp`). Read this before deploying a
`RemoteServer` on anything but a trusted local socket.

Related specs: [session.md](session.md) (the `Context`/`IAuthorizer` types),
[wire.md](wire.md) (the envelope the `session` travels in),
[backend.md](backend.md) (`RemoteServer`/`LocalBackend` dispatch),
[error_handling.md](error_handling.md) (how a rejected request surfaces). The
shipped `morph::qt` WebSocket transport supplies the TLS layer discussed below.

## Threat model — what morph does and does not defend

morph is a typed bridge, not a security product. Its built-in guarantees are
deliberately small; everything else is delegated to the transport and the
application. Be explicit about the boundary:

**morph provides:**
- A single choke point (`IAuthorizer`) consulted on **every** `execute`
  envelope before dispatch, on the remote path.
- An opt-in, stateless **authentication** layer (`session_auth.hpp`): signed
  bearer tokens whose principal the server can *verify* rather than trust, and
  make authoritative for model code.
- Untrusted-wire-input hardening in the value codecs (`Rational` clamps,
  `DateTime` rejects — see the respective specs), so a malformed payload is a
  defined outcome rather than undefined behaviour.
- A wire-layer message-size cap: `wire::decode` rejects any envelope larger than
  `wire::kMaxEnvelopeBytes` (8 MiB) before parsing, bounding a single message's
  allocation and parse cost (see [wire.md](wire.md)). This is a coarse
  per-message backstop only — not a rate limit, timeout, or inner-`body` bound.

**morph does NOT provide (the application/transport must):**
- **Wire-layer transport security.** The `wire` envelope carries no encryption
  and no per-request timeout (see [wire.md](wire.md)). `RemoteServer` itself is
  transport-agnostic and sees only decoded envelopes; confidentiality and
  timeouts are the transport's job. The shipped Qt transport *does* offer TLS
  (see below), but a transport that does not is plaintext. Run `RemoteServer`
  behind a transport that provides TLS.
  There **is** now one wire-layer bound: `wire::decode` rejects any envelope
  whose serialized form exceeds `wire::kMaxEnvelopeBytes` (8 MiB) before parsing
  (see [wire.md](wire.md) — "Parsing guarantees and hardening"). This caps the
  peak allocation and parse cost of a single message — including deeply-nested
  JSON smuggled inside the opaque `body` string, which the double-parse would
  otherwise only surface on the inner re-parse. It is a coarse per-message
  backstop, **not** a substitute for transport-level bounds: it does not cap the
  number of messages, the per-connection rate, or the inner `body` re-parse's
  own limits, so a transport that also bounds message size and rate is still
  recommended.
- **Authentication of the transport peer.** A bearer token proves the caller
  holds a validly-signed token; it does not bind the token to a connection.
  Without transport-level TLS a stolen token can be replayed.
- **Type-level `register` authorization.** The `register` envelope is **not**
  passed through `authorize`, and model ids are guessable sequential integers
  assigned from a single counter (see [backend.md](backend.md),
  `RemoteServer::_nextId`). Any client that can send envelopes can register
  models. **Per-instance ownership on `execute`/`deregister` *is* now
  enforceable** via the optional `IAuthorizer::authorizeInstance` hook (see
  [The per-instance ownership hook](#the-per-instance-ownership-hook-authorizeinstance)
  below) — a deployer can bind each instance to the principal that registered it
  and reject cross-tenant `execute`/`deregister`. The hook **defaults to
  allow-all**, so an unconfigured server still behaves as a single-trust-domain
  server: it is not a hardened multi-tenant public-internet server *unless* you
  install an authorizer that overrides `authorizeInstance`.
- **Local-path authorization.** `LocalBackend::execute` installs the session
  context but never calls the authorizer (the authorizer is a remote-only gate,
  `remote.hpp` `dispatchExecute` is the sole call site). Security-critical checks
  must be enforced inside the model so they hold in both modes.

## The trust boundary: `Context` is untrusted input

`session::Context` (`principal`, `token`, `requestId`, `locale`, `metadata`) is
populated by the client and, on the remote path, deserialised verbatim from the
wire envelope's `session` field. Every field is therefore **attacker-controlled
input** until something verifies it:

- `Context::principal` on its own is a *claim*, not an identity. Model code must
  not treat it as authenticated unless a verifying authorizer is installed (see
  below), which makes it authoritative.
- `Context::metadata` is an unbounded map decoded from the wire; treat its size
  and contents as untrusted.

On the local path the same `Context` travels in-memory via `ActionCall` and is
whatever the caller set — trusted to the extent the process trusts itself.

## Authentication: signed bearer tokens (`session_auth.hpp`)

`session_auth.hpp` is an **opt-in** header (include it only when you want
authentication) that turns `Context::principal` from a claim into a verified
identity. The mechanism is a stateless signed bearer token.

### Token format

A token is `base64url(claimsJson) "." base64url(mac)`, where `mac = MAC(secret,
payload)` and `payload` is the base64url claims segment. The claims are a
`SessionToken`:

| Field | Meaning |
|---|---|
| `principal` | Authenticated user/principal id. |
| `issuedAtMs` | Issue time, ms since epoch. If positive, enforces a not-before check (see below); `0` (unset) disables it. |
| `expiresAtMs` | Expiry, ms since epoch. **Must be strictly positive** — a `0`/negative value is treated as *already expired*, never as "eternal". |
| `roles` | Coarse-grained roles an authorization policy can key on. |

The claims are JSON (Glaze); adding application claims is compatible because
unknown fields are ignored on read.

#### Expiry is mandatory — `expiresAtMs == 0` is expired, not eternal

`TokenVerifier::verify` requires a **strictly-positive** `expiresAtMs`. A token
whose expiry is `0` (the struct default) or negative is rejected with
`AuthError::Expired`, exactly as if its expiry were in the past. This closes the
gap where a default-constructed or zeroed token would otherwise be an unbounded
bearer credential: there is no "never expires" mode, so every valid token
carries a real deadline. When minting a token you **must** set `expiresAtMs` to a
future timestamp (`nowMs() + lifetimeMs`); the login-flow example below does.

#### Not-before / issued-at check

If `issuedAtMs` is **positive**, `verify` also rejects a token whose issue time
is more than `kClockSkewMs` (60s) in the future, returning
`AuthError::NotYetValid` — a token minted against a clock ahead of the verifier's
beyond the tolerated skew is not yet valid. The 60s tolerance keeps a token
minted a moment ahead of the verifier's clock from being spuriously rejected. An
unset (`0`) or non-positive `issuedAtMs` skips this check — issue time is
optional and purely informational when omitted.

### The MAC primitive is pluggable

```cpp
using MacFunction = std::function<std::string(std::string_view key, std::string_view message)>;
```

`MacFunction` returns the raw MAC bytes. The default is `hmacSha256`, a
self-contained reference HMAC-SHA256 (so morph has **no** crypto dependency),
verified against the FIPS 180-4 / RFC 4231 test vectors in
`tests/test_session_auth.cpp`. **The reference implementation is correct but is
not hardened** (no side-channel engineering beyond a constant-time MAC compare);
security-sensitive deployments should inject a vetted library's HMAC:

```cpp
morph::session::MacFunction mac =
    [](std::string_view key, std::string_view msg) { return myLibsodiumHmac(key, msg); };
morph::session::SigningAuthorizer authz{sharedSecret, mac};
```

### Issuing tokens — the login flow

morph ships no `Login` action; login is an ordinary application action. The app
validates credentials however it likes, then mints a token with `TokenIssuer`:

```cpp
// server side, inside a Login action's handler:
morph::session::TokenIssuer issuer{sharedSecret};   // default hmacSha256
LoginResult execute(const Login& a) {
    if (!checkPassword(a.user, a.password)) throw std::runtime_error("bad credentials");
    return { .token = issuer.issue({ .principal = a.user,
                                     .issuedAtMs  = nowMs(),
                                     .expiresAtMs = nowMs() + 15 * 60'000,   // 15 min
                                     .roles = rolesFor(a.user) }) };
}
```

The client attaches the returned token to every subsequent call via the default
session:

```cpp
bridge.setDefaultSession({ .principal = user, .token = result.token });
```

### Verifying tokens and making the principal authoritative

Install a `SigningAuthorizer` on the server; it verifies the token on every
`execute`:

```cpp
auto authz  = std::make_shared<morph::session::SigningAuthorizer>(sharedSecret);
// The authorizer is the *second* constructor argument (dispatcher/registry follow
// and default to the process singletons):
auto server = std::make_shared<morph::backend::RemoteServer>(pool, authz);
```

`SigningAuthorizer` implements both `IAuthorizer` entry points:

- `authorize(ctx, modelType, actionType)` returns `true` only for a token with a
  valid signature and unexpired claims — and, if a `Policy` is supplied, one the
  policy admits. An invalid/absent/expired token → `false` → the server replies
  `err "unauthorized"`.
- `authenticate(ctx)` returns the verified `principal`. `RemoteServer`
  (`dispatchExecute`) calls it after `authorize` succeeds and **overwrites**
  `env.session.principal` with the verified value before building the
  `ScopedContext`. So a model reading `session::current()->principal` sees the
  authenticated identity, not the client's claim.

**The principal is never passed through unverified.** When `authenticate(ctx)`
returns `nullopt` — the authorizer cannot vouch for the caller —
`dispatchExecute` **clears** `env.session.principal` to the empty string before
dispatch rather than leaving the client-supplied claim in place. Two cases
depend on this:

- **TOCTOU (time-of-check/time-of-use).** `authorize` and `authenticate` each
  verify the token independently against a fresh clock reading. A token can pass
  `authorize` and then expire in the window before `authenticate` runs, so
  `authenticate` returns `nullopt`. Without the clear, the request would have
  been dispatched carrying the *client's* asserted principal as if it were
  authoritative. With the clear, the worst case is an empty principal — never
  the attacker's chosen value.
- **Authorize-only / allow-all authorizers.** An authorizer that admits calls
  but never authenticates (a custom authorize-only policy, or the default
  `AllowAllAuthorizer` whose `authenticate` inherits the `nullopt` default) now
  results in an empty principal at the model. This preserves the
  "authentication is optional" contract — the call still dispatches — while
  ensuring an unauthenticated principal is never presented to model code as
  trustworthy. Apps that want a trusted principal must install a verifying
  authorizer (`SigningAuthorizer`).

`TokenVerifier::verify` checks the MAC **before** parsing the claims JSON, so
untrusted input is never handed to the parser until authenticity is established,
and uses a constant-time comparison (`detail::constantTimeEquals`) to avoid MAC
timing leaks. It returns `std::expected<SessionToken, AuthError>`:

| `AuthError` | Cause |
|---|---|
| `Malformed` | Not `payload.sig`, bad/non-canonical base64url, or unparseable claims. |
| `BadSignature` | MAC mismatch — forged or tampered. |
| `Expired` | `expiresAtMs` is missing/non-positive, or in the past relative to the supplied clock. |
| `NotYetValid` | `issuedAtMs` is set and more than `kClockSkewMs` (60s) in the future. |

The clock is injectable (`Clock`, defaulting to `systemClockMs`) so expiry is
testable without wall-clock dependence.

#### Canonical base64url — no signature malleability

`detail::base64UrlDecode` decodes **canonically**: it is a bijection over valid
tokens, so exactly one token string maps to any given byte sequence. base64url
is a *bit*-oriented encoding, and a naive decoder that silently discards the
leftover bits of the final symbol would let several distinct strings decode to
the same MAC — a token-string malleability that lets an attacker perturb the
trailing character without invalidating the signature. The decoder rejects such
input:

- A length `% 4 == 1` (impossible for real base64url output) is rejected.
- The leftover bits that do not form a whole output byte (2 bits for a
  1-byte-remainder group, 4 bits for a 2-byte-remainder group) **must be zero**;
  a nonzero remainder is a non-canonical encoding and is rejected rather than
  truncated.

Both signature and payload segments are decoded through this path, so a mutated
trailing character in either segment fails verification (`Malformed`, or
`BadSignature` if it survives decoding but changes the MAC).

### Role-based policy

`SigningAuthorizer`'s optional `Policy` runs over the verified claims:

```cpp
morph::session::SigningAuthorizer authz{
    secret, morph::session::hmacSha256, morph::session::systemClockMs,
    [](const morph::session::SessionToken& t, std::string_view modelType, std::string_view actionType) {
        return std::ranges::find(t.roles, "admin") != t.roles.end();  // admin-only
    }};
```

The default (no policy) admits any validly-signed, unexpired token.

## The per-instance ownership hook (`authorizeInstance`)

`authorize` sees only the model **type**, so it cannot answer "may this caller
touch *this instance*?". Because model instances on a `RemoteServer` are
addressable by guessable sequential ids, without an instance check any
authenticated caller can `execute`/`deregister` against an id it did not create
— a cross-tenant targeting gap. `IAuthorizer` closes it with an optional third
method:

```cpp
[[nodiscard]] virtual bool authorizeInstance(
    const Context& ctx,
    std::string_view modelType,      // empty for deregister
    std::string_view actionType,     // empty for deregister
    std::uint64_t modelId,
    std::string_view ownerPrincipal  // recorded at register time; empty if none
) const { return true; }             // DEFAULT: allow
```

### How ownership is recorded and enforced

- **At `register`** — `RemoteServer` records an owner principal for the new
  instance. The owner is the **verified** identity of the register call:
  `RemoteServer` calls `_authorizer->authenticate(env.session)` and stores the
  returned principal (empty if the authorizer does not authenticate, e.g.
  allow-all, so the instance is *unowned*). It is never the client's raw
  `principal` claim.
- **On `execute`** — after the type-level `authorize` succeeds and the verified
  principal has been stamped onto the session, `RemoteServer` consults
  `authorizeInstance(session, modelType, actionType, modelId, ownerPrincipal)`.
  A `false` return replies `err "unauthorized"` and the action never dispatches.
- **On `deregister`** — `RemoteServer` consults
  `authorizeInstance(session, {}, {}, modelId, ownerPrincipal)` (empty type/action
  ids) before destroying the instance. A `false` return replies
  `err "unauthorized"` and the instance is left intact. This is the fix for
  `deregister` previously being entirely unauthorized.

### Backward compatibility

The **default `authorizeInstance` returns `true`**, so an authorizer that does
not override it — including `AllowAllAuthorizer` and a plain `SigningAuthorizer`
— imposes no per-instance restriction and the server behaves exactly as before.
A deployer opts into enforcement by overriding the hook, typically comparing the
recorded owner against the authenticated caller:

```cpp
struct OwnershipAuthorizer : morph::session::SigningAuthorizer {
    using SigningAuthorizer::SigningAuthorizer;
    bool authorizeInstance(const morph::session::Context& ctx, std::string_view,
                           std::string_view, std::uint64_t,
                           std::string_view ownerPrincipal) const override {
        // Unowned instances stay open; owned instances only to their owner.
        return ownerPrincipal.empty() || ownerPrincipal == ctx.principal;
    }
};
```

Because the owner is captured from the *verified* principal at register time,
this is only meaningful with a verifying authorizer installed; with allow-all
every instance is unowned and the hook (if overridden as above) admits all.
Register itself remains type-unauthorized — bounding *who may create* instances
is still the transport's/app's responsibility.

## The default is fail-open — change it in production

`RemoteServer`'s ordinary constructor defaults to `allowAllAuthorizer()`, and the
explicit-authorizer constructor silently falls back to allow-all on a `nullptr`
argument. An unconfigured server therefore **authorizes everything**. This is
convenient for local/simulated development and wrong for production. Always
install a `SigningAuthorizer` (or a deny-by-default custom authorizer) before
exposing a server, and never pass `nullptr`.

## Transport security: the Qt WebSocket transport

`RemoteServer` is transport-agnostic, but morph ships one concrete transport —
`morph::qt::QtWebSocketServer` / `QtWebSocketBackend` — and its trust properties
matter in practice:

- **TLS is available.** Passing a `QSslConfiguration` puts the server in
  `QWebSocketServer::SecureMode` (`wss://`) and the client into a TLS socket.
  Absent a config, both run plaintext (`ws://`). TLS here provides the transport
  confidentiality and peer authentication the wire layer does not — this is the
  intended way to protect bearer tokens against capture and replay.
- **The server binds to loopback only.** `QtWebSocketServer::listen()` binds
  `QHostAddress::LocalHost`; the shipped server is not reachable off-host. Exposing
  it beyond localhost requires changing the bind address *and* adding the
  authorization the base `RemoteServer` does not enforce (control messages, model
  ids — see the threat model).
- **Client peer verification is the deployer's choice.** For self-signed certs
  the documented pattern sets `QSslSocket::VerifyNone` on the client config, which
  disables server-identity verification (encrypts but does not authenticate the
  server, so it is MITM-vulnerable). Production clients must verify the server
  certificate against a trusted CA or a pinned certificate.
- **No transport-level message-size check.** `QtWebSocketServer` forwards each
  text frame to `RemoteServer::handle()` with no size check of its own; the bounds
  are `QWebSocket`'s default frame/message limit and the wire-layer 8 MiB
  `kMaxEnvelopeBytes` cap that `wire::decode` enforces before parsing (which
  covers the whole envelope, including `body`). There is no per-connection request
  rate limit and no cap on the number of models a connection may register.

## Residual limitations & hardening checklist

Even with `SigningAuthorizer` installed, the following remain the deployer's
responsibility:

- **Use TLS and verify the peer.** Bearer tokens and payloads travel in
  plaintext otherwise, and a captured token can be replayed until it expires.
  There is no envelope-level confidentiality or replay protection. The Qt
  transport supports `wss://` (above); enable it and make the client actually
  verify the server certificate (not `VerifyNone`).
- **Keep expiry short and rotate the secret.** A leaked secret forges any
  identity; a leaked token is valid until `expiresAtMs`.
- **Bound message size and add timeouts in the transport.** The wire layer now
  caps a single message at `wire::kMaxEnvelopeBytes` (8 MiB) but imposes **no**
  per-request timeout and **no** per-connection rate or count limit (see
  [wire.md](wire.md)); `QtWebSocketServer` adds no cap beyond `QWebSocket`'s
  default. A hostile client can still exhaust memory with many sub-cap messages
  or leave `Completion`s pending forever, so the transport must add its own
  size/rate bounds and timeouts.
- **Do not rely on the authorizer for correctness inside models.** It runs only
  on the remote path and only for `execute`. Enforce invariants in the model so
  they also hold locally and for control messages.
- **Per-instance ownership is opt-in.** `execute`/`deregister` can be bound to
  the registering principal via `authorizeInstance` (above), but the default
  allows all. `register` remains type-unauthorized regardless. Treat
  `RemoteServer` as single-trust-domain unless you install an authorizer that
  overrides `authorizeInstance` *and* bound who may `register` at the transport.

## Testing

`tests/test_session_auth.cpp` covers the SHA-256/HMAC known-answer vectors,
base64url round-tripping, token issue/verify, and rejection of tampering, wrong
secret, expiry, and malformed input, plus `SigningAuthorizer` authorization,
the no-token denial path, and role-policy enforcement.

`tests/test_policy_hardening.cpp` covers the policy fixes in this spec: a token
with `expiresAtMs == 0` or negative is rejected as `Expired` (never eternal) and
one with a real positive expiry still verifies; a token issued far in the future
is rejected `NotYetValid` while one within the 60s skew (or with an unset
`issuedAtMs`) is accepted; `base64UrlDecode` rejects impossible lengths and
non-canonical trailing bits, and a token with a mutated trailing signature
character fails; and, with an ownership authorizer installed, principal B cannot
`execute` or `deregister` principal A's instance while A can, whereas with the
default authorizer any principal can (backward compatible).

The **test TLS material** in `tests/certs/` (`server.crt`/`server.key`, used
only by `tests/qt/test_qt_websocket.cpp`) is a throwaway self-signed pair with
the deliberately loud CN `MORPH-TEST-DO-NOT-USE`. Its private key is committed
in plaintext and must be assumed public — it grants no trust anywhere and must
**never** be used in production or copied elsewhere. See `tests/certs/README.md`.

`tests/test_server_limits.cpp` exercises the untrusted-input hardening claim: a
1 MiB action payload round-trips intact, a 5000-deep nested-JSON envelope and a
lone-continuation-byte (malformed UTF-8) body each produce a defined `ok`/`err`
reply rather than a crash or hang, and a 200-round register/deregister churn
completes cleanly. These confirm resilience; the payloads there stay under the
wire-layer size cap.

`tests/test_wire_hardening.cpp` covers the wire-layer parsing guarantees
directly: `wire::decode` accepts an envelope under `kMaxEnvelopeBytes`, rejects
an oversized one (including one whose bulk is deeply-nested JSON inside the
opaque `body` string) with `std::runtime_error` before parsing, and — pinning
the honest, non-guaranteed behavior — accepts duplicate JSON keys (top-level and
nested `session`) with last-wins rather than rejecting them, since glaze 7.2.1
offers no option to error on duplicates. There remains **no** per-request
timeout and **no** rate or message-count cap (see above).

`tests/qt/test_qt_websocket.cpp` (with `tests/certs/server.crt`/`server.key`)
covers the TLS transport: `wss://` request/reply, TLS error propagation,
refusal of a plaintext client against a `wss://` server, and a cross-process TLS
handshake.

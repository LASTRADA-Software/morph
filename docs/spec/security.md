# Security & the trust model

Cross-cutting spec covering morph's trust boundaries and the authenticated
session layer (`session.hpp` `Context`/`IAuthorizer`, `session_auth.hpp`
`SessionToken`/`TokenIssuer`/`TokenVerifier`/`SigningAuthorizer`, and the
`RemoteServer` enforcement points in `remote.hpp`). Read this before deploying a
`RemoteServer` on anything but a trusted local socket.

Related specs: [session.md](session.md) (the `Context`/`IAuthorizer` types),
[wire.md](wire.md) (the envelope the `session` travels in),
[backend.md](backend.md) (`RemoteServer`/`LocalBackend` dispatch),
[error_handling.md](error_handling.md) (how a rejected request surfaces).

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

**morph does NOT provide (the application/transport must):**
- **Transport security.** There is no TLS, no encryption, no message-size cap,
  and no per-request timeout at the wire layer (see [wire.md](wire.md)). A plain
  `RemoteServer` transport is plaintext and has an unbounded-allocation DoS
  surface. Run it behind TLS and a transport that bounds message size.
- **Authentication of the transport peer.** A bearer token proves the caller
  holds a validly-signed token; it does not bind the token to a connection.
  Without TLS a stolen token can be replayed.
- **Control-message authorization.** Only `execute` is passed through the
  authorizer. `register`/`deregister` are **not** authorized, and model ids are
  guessable sequential integers assigned from a single counter (see
  [backend.md](backend.md), `RemoteServer::_nextId`). Any client that can send
  envelopes can register models and deregister/execute against ids it did not
  create. `RemoteServer` therefore assumes an authenticated, trusted transport
  — it is not a hardened multi-tenant public-internet server as shipped.
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
| `issuedAtMs` | Issue time, ms since epoch (informational). |
| `expiresAtMs` | Expiry, ms since epoch. `0` means never expires (discouraged). |
| `roles` | Coarse-grained roles an authorization policy can key on. |

The claims are JSON (Glaze); adding application claims is compatible because
unknown fields are ignored on read.

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
auto server = std::make_shared<morph::backend::RemoteServer>(pool, dispatcher, registry, authz);
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

`TokenVerifier::verify` checks the MAC **before** parsing the claims JSON, so
untrusted input is never handed to the parser until authenticity is established,
and uses a constant-time comparison (`detail::constantTimeEquals`) to avoid MAC
timing leaks. It returns `std::expected<SessionToken, AuthError>`:

| `AuthError` | Cause |
|---|---|
| `Malformed` | Not `payload.sig`, bad base64url, or unparseable claims. |
| `BadSignature` | MAC mismatch — forged or tampered. |
| `Expired` | `expiresAtMs` is before the supplied clock. |

The clock is injectable (`Clock`, defaulting to `systemClockMs`) so expiry is
testable without wall-clock dependence.

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

## The default is fail-open — change it in production

`RemoteServer`'s ordinary constructor defaults to `allowAllAuthorizer()`, and the
explicit-authorizer constructor silently falls back to allow-all on a `nullptr`
argument. An unconfigured server therefore **authorizes everything**. This is
convenient for local/simulated development and wrong for production. Always
install a `SigningAuthorizer` (or a deny-by-default custom authorizer) before
exposing a server, and never pass `nullptr`.

## Residual limitations & hardening checklist

Even with `SigningAuthorizer` installed, the following remain the deployer's
responsibility:

- **Use TLS.** Bearer tokens and payloads travel in plaintext otherwise, and a
  captured token can be replayed until it expires. There is no envelope-level
  confidentiality or replay protection.
- **Keep expiry short and rotate the secret.** A leaked secret forges any
  identity; a leaked token is valid until `expiresAtMs`.
- **Bound message size and add timeouts in the transport.** The wire layer does
  neither (see [wire.md](wire.md)); a hostile client can otherwise exhaust
  memory or leave `Completion`s pending forever.
- **Do not rely on the authorizer for correctness inside models.** It runs only
  on the remote path and only for `execute`. Enforce invariants in the model so
  they also hold locally and for control messages.
- **Treat `RemoteServer` as single-trust-domain** unless you add
  control-message authorization and session-scoped model ids yourself.

## Testing

`tests/test_session_auth.cpp` covers the SHA-256/HMAC known-answer vectors,
base64url round-tripping, token issue/verify, and rejection of tampering, wrong
secret, expiry, and malformed input, plus `SigningAuthorizer` authorization,
the no-token denial path, and role-policy enforcement.

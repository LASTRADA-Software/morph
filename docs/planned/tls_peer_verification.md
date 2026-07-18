# TLS + peer verification as the enforced production default (planned)

> **Status: planned — not yet implemented.** This spec extends
> [security.md](../spec/security.md)'s "Transport security" section and its
> "Residual limitations & hardening checklist" ("Use TLS and verify the peer").
> It makes authenticated TLS the documented and defaulted production path for the
> shipped Qt transport, rather than the encrypt-but-do-not-authenticate pattern
> the docs show today. See [todo.md](../todo.md).

## The gap

The shipped Qt transport *can* do TLS, but the way it is documented and defaulted
leaves the production path unsafe:

- **`VerifyNone` is the documented pattern.** `qt_websocket_backend.hpp`'s header
  comment tells self-signed users to "set `QSslSocket::VerifyNone` on the
  configuration before passing it in," and `security.md` records the consequence:
  this "encrypts but does not authenticate the server, so it is
  MITM-vulnerable." An operator who copies the documented snippet ships a client
  that will complete a TLS handshake with *any* server presenting *any*
  certificate.
- **Tokens are not bound to a connection.** `security.md`'s threat model states a
  bearer token "does not bind the token to a connection. Without transport-level
  TLS a stolen token can be replayed." `VerifyNone` defeats the one mechanism
  (server-identity verification) that would stop an interceptor from being the
  server the token is handed to.
- **Nothing warns when a server is exposed in the clear.** `QtWebSocketServer`
  binds `QHostAddress::LocalHost` today, but the moment a deployer changes the
  bind address to expose it off-host (which `security.md` explicitly contemplates)
  there is no guard, log line, or assertion that TLS is configured. Plaintext
  off-host exposure is silent.

`morph` deliberately delegates transport confidentiality to the transport
([security.md](../spec/security.md)), so this is not a `RemoteServer` change — it
is a hardening of the concrete `morph::qt` transport and its documentation.

## Goal

Make **authenticated** TLS (peer verification against a CA or a pinned
certificate) the documented, example-backed, and default-safe path for the Qt
transport, and add a startup guard that refuses — or loudly warns — when a
`QtWebSocketServer` is exposed beyond loopback without a TLS configuration. The
insecure `VerifyNone` mode remains reachable but becomes an explicit, named
opt-out rather than the path of least resistance.

## Design

### 1. A verifying client config helper (NEW)

Rather than hand deployers a raw `QSslConfiguration` and a `VerifyNone` snippet,
ship two small factory helpers in `morph::qt` that build a *verifying*
configuration, so the safe path is the shortest one to type:

```cpp
// namespace morph::qt — NEW helpers (thin wrappers over QSslConfiguration).

/// Verifies the server against the system/CA trust store (VerifyPeer).
/// This is the recommended production default.
[[nodiscard]] QSslConfiguration tlsVerifyingConfig();

/// Verifies the server against a specific pinned certificate — the correct
/// choice for self-signed deployments (pin the cert instead of disabling
/// verification). Sets VerifyPeer and adds `pinned` as the sole CA.
[[nodiscard]] QSslConfiguration tlsPinnedConfig(const QSslCertificate& pinned);

/// The insecure, encrypt-only mode. Kept for local development and tests only;
/// the name states the hazard so it can be grepped for in a security review.
[[nodiscard]] QSslConfiguration tlsInsecureNoVerify();
```

- The **existing** `QtWebSocketBackend` constructor's `tls` parameter (an
  `std::optional<QSslConfiguration>`, confirmed in `qt_websocket_backend.hpp`) is
  unchanged; these helpers just produce the value passed to it. No new
  constructor overload is required.
- `tlsPinnedConfig` is the answer for the self-signed case that today reaches for
  `VerifyNone`: it keeps `QSslSocket::VerifyPeer` on and trusts exactly the pinned
  cert, so a self-signed server is authenticated (not merely encrypted).
- `tlsInsecureNoVerify` preserves today's behavior verbatim for the local-dev and
  test paths (e.g. the throwaway `MORPH-TEST-DO-NOT-USE` pair in `tests/certs/`),
  but its name is the audit signal.

### 2. Server-side exposure guard (NEW)

`QtWebSocketServer::listen()` (existing; returns `bool`) gains a guard keyed on
the bind address and the presence of a TLS config:

- **Loopback + no TLS** — allowed (today's local-dev default), unchanged.
- **Non-loopback bind + no TLS config** — `listen()` refuses by default, logging
  at `morph::log::LogLevel::error` and returning `false`, unless the deployer
  passes an explicit acknowledgement (a NEW `QtWebSocketServerConfig` flag, e.g.
  `allowPlaintextExposure = false` by default). This turns "silently plaintext
  off-host" into a deliberate, visible choice.
- **Any bind + TLS config** — allowed; a `SecureMode` server is safe to expose.

```cpp
// namespace morph::qt — NEW config carried by QtWebSocketServer.
struct QtWebSocketServerConfig {
    /// Bind address. Default LocalHost (today's behavior).
    QHostAddress bindAddress = QHostAddress::LocalHost;
    /// Guard: refuse to listen() on a non-loopback address without a TLS
    /// config. Set true only to deliberately serve plaintext off-host.
    bool allowPlaintextExposure = false;
};
```

The guard uses only `morph::log` (the existing replaceable sink) and the
`SecureMode`/`NonSecureMode` distinction the server already tracks from its
`std::optional<QSslConfiguration> tls` constructor argument
([backend.md](../spec/backend.md)). It adds no dependency and does not touch
`RemoteServer`.

> If this spec lands alongside [transport_limits.md](transport_limits.md)'s
> `QtWebSocketServerConfig`, the two configs merge into one struct; the
> `handshakeTimeout` there and the `bindAddress`/`allowPlaintextExposure` here
> are complementary connection-level knobs.

### 3. Documentation flip and example (NEW)

- `security.md`'s "Transport security" bullet on peer verification is rewritten so
  the **recommended** pattern is CA or pinned verification, and `VerifyNone` is
  described as local-dev-only. The "Residual limitations" TLS item becomes a
  satisfied default rather than an open caveat for the Qt transport.
- The stale `qt_websocket_backend.hpp` header comment that recommends
  `VerifyNone` is replaced with a pointer to `tlsVerifyingConfig` /
  `tlsPinnedConfig`.
- A worked `examples/` client shows a pinned-certificate connection end to end
  (mint/pin a self-signed cert, connect with `tlsPinnedConfig`, reject a MITM
  presenting a different cert).

## Non-goals

- **Not a change to `RemoteServer` or the wire.** Confidentiality and peer
  authentication stay the transport's job ([security.md](../spec/security.md));
  the `Envelope` and `decode` path are untouched. This is a Qt-transport +
  docs change.
- **Not envelope-level encryption or replay protection.** TLS provides
  confidentiality and peer authentication; the wire layer still carries no
  encryption and no nonce/replay defence. Short token expiry and secret rotation
  ([security.md](../spec/security.md)) remain the deployer's job.
- **Not certificate lifecycle management.** Issuing, rotating, and revoking
  certificates (CA operations, OCSP/CRL) are out of scope; the helpers consume a
  cert the deployer supplies.
- **Not a new transport.** A non-Qt transport is
  [non_qt_transport.md](non_qt_transport.md); this spec hardens the *existing* Qt
  one.
- **Does not affect local mode.** `LocalBackend` has no socket; none of this
  applies there.

## Testing (planned)

- A client built with `tlsPinnedConfig(serverCert)` completes a `wss://`
  request/reply against a server presenting the pinned cert, and **fails to
  connect** against a server presenting a different cert (MITM rejected) — where
  a `tlsInsecureNoVerify` client would have connected to both.
- A `QtWebSocketServer` constructed with a non-loopback `bindAddress` and no TLS
  config has `listen()` return `false` and logs at error level, unless
  `allowPlaintextExposure` is set; with a TLS config, `listen()` succeeds.
- Loopback + no TLS still listens (local-dev regression guard) — behavior
  identical to today.
- The `tests/certs/` throwaway pair continues to drive the existing
  `tests/qt/test_qt_websocket.cpp` TLS cases via `tlsInsecureNoVerify` /
  `tlsPinnedConfig` (the pair is public and must never gate production trust —
  see [security.md](../spec/security.md)).

## Cross-references

- [security.md](../spec/security.md) — the trust model, the token-replay risk this
  closes, the `VerifyNone` limitation, the "Use TLS and verify the peer"
  hardening-checklist item, and the `tests/certs/` throwaway material.
- [backend.md](../spec/backend.md) — `QtWebSocketBackend`/`QtWebSocketServer`, the
  `std::optional<QSslConfiguration> tls` constructor arguments, `SecureMode`, and
  `listen()`/`port()` where the guard lands.
- [transport_limits.md](transport_limits.md) — the sibling `QtWebSocketServerConfig`
  connection-level knobs (`handshakeTimeout`, `maxConnections`) this config merges
  with; TLS handshake timeout is enforced there.
- [logger.md](../spec/logger.md) — the `LogLevel::error` sink the exposure guard
  logs through.
- [non_qt_transport.md](non_qt_transport.md) — a transport that is not Qt must
  supply its own equivalent TLS + verification story; this spec is Qt-specific.

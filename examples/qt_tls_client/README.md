# TLS peer verification — worked example

Demonstrates the recommended way to use TLS with the Qt WebSocket transport
(`morph::qt`): **verify** the server's certificate instead of disabling
verification. See `docs/spec/security.md` ("Transport security") for the
full threat model.

This binary starts two local `QtWebSocketServer`s on loopback:

- the **real** server, presenting `certs/server.crt`;
- a **mitm** server, presenting a *different* self-signed certificate
  (`certs/mitm.crt`) — standing in for an attacker who intercepted the
  connection but cannot produce the real server's private key.

It then shows:

1. `tlsPinnedConfig(serverCert)` connects to the real server and completes a
   request/reply round trip.
2. The **same** pinned client refuses to connect to the mitm server — the
   certificate does not match the pin, so the TLS handshake fails.
3. `tlsInsecureNoVerify()`, by contrast, connects to **both** — this is
   exactly why it must never be used against an untrusted network; it is
   for local development and tests only.

## Running

    ./morph_qt_tls_example

Exits `0` if every expectation above holds; prints `FAIL: ...` to stderr and
exits non-zero otherwise.

## The certificates

`certs/server.crt`/`server.key` and `certs/mitm.crt`/`mitm.key` are throwaway
self-signed pairs generated for this example only (see the loud
`MORPH-EXAMPLE-DO-NOT-USE` / `MORPH-EXAMPLE-MITM-DO-NOT-USE` common names).
**Never reuse them, or the pattern of trusting a single pinned certificate
file checked into source control, for a real deployment** — mint your own
key on the target host and keep it out of version control, or use
`tlsVerifyingConfig()` against a CA-issued certificate instead.

# Test-only TLS material — DO NOT USE IN PRODUCTION

**These files are throwaway test fixtures. They are intentionally worthless.**

`server.crt`/`server.key` and `mitm.crt`/`mitm.key` are two independent
self-signed RSA-2048 certificate/private-key pairs, generated solely so
`tests/qt/test_qt_websocket.cpp` can exercise the `wss://` (TLS) path of the
Qt WebSocket transport against a loopback server — including peer-verified
(`tlsPinnedConfig`/`tlsVerifyingConfig`) connections, not just encrypt-only
(`tlsInsecureNoVerify`) ones.

- `server.crt`/`server.key` stands in for the real server. Its Common Name is
  deliberately **`MORPH-TEST-DO-NOT-USE`**.
- `mitm.crt`/`mitm.key` is a *different* self-signed pair standing in for an
  attacker who intercepted the TCP connection but cannot produce the real
  server's private key. Its Common Name is **`MORPH-TEST-MITM-DO-NOT-USE`**.

Both carry a `subjectAltName=IP:127.0.0.1` extension. This is required for
`VerifyPeer`-mode tests (`tlsPinnedConfig`/`tlsVerifyingConfig`): Qt matches a
literal-IP peer name only against a certificate's SAN `IP Address` entries,
never against the Common Name, so a cert without this extension fails
hostname verification against `wss://127.0.0.1:<port>` even when otherwise
trusted. Tests using `tlsInsecureNoVerify` (`VerifyNone`) are unaffected
either way, since that mode skips hostname/identity verification entirely.

## Rules

- **NEVER** use this key/certificate material for anything but morph's own tests.
- **NEVER** copy it into a real service, container image, deployment config, or
  another repository.
- **NEVER** treat either private key as secret — both are committed to version
  control in plaintext and must be assumed public. They grant no trust anywhere.
- If you need TLS for a real deployment, generate your own key on the target
  host, keep it off version control, and have it signed by a CA (or pin it) —
  see `docs/spec/security.md` ("Transport security").

## Regenerating

The only references to this material in the tree are
`tests/qt/test_qt_websocket.cpp` and `examples/qt_tls_client/` (which ships its
own, separate cert pair), via the `TESTS_CERTS_DIR` compile define. To
regenerate an equally-throwaway set:

```sh
openssl req -x509 -newkey rsa:2048 -nodes \
    -keyout server.key -out server.crt \
    -days 3650 -subj "/CN=MORPH-TEST-DO-NOT-USE" \
    -addext "subjectAltName=IP:127.0.0.1"

openssl req -x509 -newkey rsa:2048 -nodes \
    -keyout mitm.key -out mitm.crt \
    -days 3650 -subj "/CN=MORPH-TEST-MITM-DO-NOT-USE" \
    -addext "subjectAltName=IP:127.0.0.1"
```

No passphrase, self-signed, same loud CNs, same SAN — keep it that way.

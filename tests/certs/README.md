# Test-only TLS material — DO NOT USE IN PRODUCTION

**These files are throwaway test fixtures. They are intentionally worthless.**

`server.crt` / `server.key` are a self-signed RSA-2048 certificate and its
**unencrypted private key**, generated solely so `tests/qt/test_qt_websocket.cpp`
can exercise the `wss://` (TLS) path of the Qt WebSocket transport against a
loopback server.

The certificate's Common Name is deliberately **`MORPH-TEST-DO-NOT-USE`** so it
can never be mistaken for a real host certificate, and the test client sets
`QSslSocket::VerifyNone` (it does not verify the CN or the chain), so the name
is never checked — it exists only to shout its purpose.

## Rules

- **NEVER** use this key/certificate for anything but morph's own tests.
- **NEVER** copy it into a real service, container image, deployment config, or
  another repository.
- **NEVER** treat the private key as secret — it is committed to version control
  in plaintext and must be assumed public. It grants no trust anywhere.
- If you need TLS for a real deployment, generate your own key on the target
  host, keep it off version control, and have it signed by a CA (or pin it) —
  see `docs/spec/security.md` ("Transport security").

## Regenerating

These are the only references to this material in the tree
(`tests/qt/test_qt_websocket.cpp`, via the `TESTS_CERTS_DIR` compile define). To
regenerate an equally-throwaway pair:

```sh
openssl req -x509 -newkey rsa:2048 -nodes \
    -keyout server.key -out server.crt \
    -days 3650 -subj "/CN=MORPH-TEST-DO-NOT-USE"
```

No passphrase, self-signed, and the same loud CN — keep it that way.

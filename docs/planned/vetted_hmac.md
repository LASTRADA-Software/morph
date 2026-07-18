# Injecting a vetted HMAC for production (planned)

> **Status: planned — not yet implemented.** This spec extends
> [security.md](../spec/security.md)'s "The MAC primitive is pluggable" section and
> its "Residual limitations & hardening checklist". It documents the recommended
> production wiring of a vetted HMAC library through the existing `MacFunction`
> seam, and a build option that fails a release build wired to the reference
> implementation. See [todo.md](../todo.md).

## The gap

The token MAC primitive is already pluggable. `session_auth.hpp` defines:

```cpp
// EXISTING (session_auth.hpp)
using MacFunction = std::function<std::string(std::string_view key, std::string_view message)>;
inline std::string hmacSha256(std::string_view key, std::string_view message);  // the default
```

and `TokenIssuer`, `TokenVerifier`, and `SigningAuthorizer` all take
`MacFunction mac = hmacSha256` as a constructor argument (verified in
`session_auth.hpp`). The seam exists; the problem is the *default* and the *lack
of a guard*:

- **The reference `hmacSha256` is correct but not hardened.** `security.md` states
  it plainly: it is "correct but is not hardened (no side-channel engineering
  beyond a constant-time MAC compare)," verified against the FIPS 180-4 / RFC 4231
  vectors in `tests/test_session_auth.cpp`. The MAC compare uses
  `detail::constantTimeEquals` (verified in `session_auth.hpp`), but the SHA-256
  core itself is a straightforward reference implementation, not a side-channel-
  resistant one.
- **Nothing stops a release build from shipping the reference impl.** A deployer
  who forgets to inject a vetted library gets `hmacSha256` silently, in
  production, with no signal. The default is safe-by-value (it computes a correct
  HMAC) but not safe-by-posture.

`morph` intentionally has **no crypto dependency** ([security.md](../spec/security.md)),
so the reference impl must stay the default for the zero-dependency build. The
fix is a documented adapter and an opt-in build-time guard, not a new default.

## Goal

Give production deployments a documented, example-backed way to inject a vetted
library's HMAC through the existing `MacFunction` seam, and an **opt-in** build
option that turns "reference HMAC in a release/production build" from a silent
default into a build failure. Both are additive: the default build is unchanged
and keeps zero crypto dependencies.

## Design

### 1. A documented adapter (NEW example, no new symbol)

The wiring is already expressible with today's API — `security.md` sketches it.
This spec makes it a shipped, compiled example adapter for the two common
libraries, so a deployer copies working code rather than a doc snippet:

```cpp
// examples/ — libsodium adapter (NEW example code, not a library symbol).
morph::session::MacFunction sodiumHmacSha256 =
    [](std::string_view key, std::string_view msg) -> std::string {
        unsigned char out[crypto_auth_hmacsha256_BYTES];
        crypto_auth_hmacsha256_state st;
        crypto_auth_hmacsha256_init(&st,
            reinterpret_cast<const unsigned char*>(key.data()), key.size());
        crypto_auth_hmacsha256_update(&st,
            reinterpret_cast<const unsigned char*>(msg.data()), msg.size());
        crypto_auth_hmacsha256_final(&st, out);
        return std::string(reinterpret_cast<char*>(out), sizeof out);
    };

// Wire it into the verifying authorizer (EXISTING constructor signature):
auto authz = std::make_shared<morph::session::SigningAuthorizer>(
    sharedSecret, sodiumHmacSha256);
```

- The adapter returns **raw MAC bytes**, matching the `MacFunction` contract
  documented in `session_auth.hpp` ("Returns the raw MAC bytes").
- An OpenSSL variant (`HMAC(EVP_sha256(), ...)`) is provided alongside it.
- The example includes a known-answer test asserting the injected function
  produces the *same* bytes as `hmacSha256` on the RFC 4231 vectors, so a
  deployer can prove the swap is drop-in before trusting it — tokens minted with
  one and verified with the other must interoperate.

### 2. A build option that fails on the reference impl in production (NEW)

Add an opt-in CMake option, defaulting off so the standard build is unaffected:

```
-DMORPH_REQUIRE_VETTED_HMAC=ON
```

When set, the build defines a macro (e.g. `MORPH_REQUIRE_VETTED_HMAC`) that makes
using the reference `hmacSha256` a **compile-time or link-time failure**, forcing
an injected `MacFunction`. Two candidate mechanisms, decided at implementation:

- **Default-argument removal.** Under the macro, the `mac = hmacSha256` default
  arguments on `TokenIssuer`/`TokenVerifier`/`SigningAuthorizer` are dropped, so a
  construction that relies on the default no longer compiles — the deployer *must*
  pass a `MacFunction`. This is purely additive to the header and catches the
  omission at the call site.
- **A `[[deprecated]]`/`static_assert` marker** on `hmacSha256` under the macro,
  so any direct reference is a hard diagnostic naming the option.

The guard keys on the *build configuration a deployer chooses to enable*, not on
`CMAKE_BUILD_TYPE` automatically — an automatic Release trigger would surprise
the zero-dependency users who legitimately ship the reference impl for
low-stakes/local deployments. `security.md`'s note that the reference impl is
"correct but not hardened" is upgraded to point at this option as the production
enforcement.

### 3. Documentation

- `security.md`'s MAC-primitive section gains the recommended-wiring subsection
  (libsodium/OpenSSL adapters) and a pointer to `MORPH_REQUIRE_VETTED_HMAC`.
- The hardening checklist's "Keep expiry short and rotate the secret" item is
  joined by "Inject a vetted HMAC and enable `MORPH_REQUIRE_VETTED_HMAC` in
  release builds."

## Non-goals

- **Not a new crypto dependency in the default build.** The reference
  `hmacSha256` stays the default so `morph` remains dependency-free out of the
  box; libsodium/OpenSSL are the *deployer's* dependency, wired via the existing
  seam.
- **Not a change to the token format or `MacFunction` signature.** The
  `base64url(claims).base64url(mac)` format ([security.md](../spec/security.md))
  and the `std::function<std::string(key,message)>` type are unchanged — a vetted
  HMAC is a drop-in for the same raw-bytes contract.
- **Not replacing `constantTimeEquals`.** The constant-time MAC *compare* already
  ships and is unchanged; this spec is about the MAC *computation* side channel,
  which only a vetted core addresses.
- **Not key management.** Generating, storing, and rotating `sharedSecret` is out
  of scope (the deployer's KMS/secret-store concern); this spec only wires the MAC
  function.
- **Does not weaken the reference impl's correctness guarantees.** It stays
  test-vector-verified; the option is about side-channel posture, not
  correctness.

## Testing (planned)

- The libsodium and OpenSSL adapter examples each produce byte-identical MACs to
  `hmacSha256` on the RFC 4231 vectors, and a token issued with the reference impl
  verifies under the injected one and vice versa (drop-in interop).
- A `SigningAuthorizer` constructed with an injected `MacFunction` authorizes a
  validly-signed token and rejects a tampered one exactly as with the default
  (behavior parity).
- With `-DMORPH_REQUIRE_VETTED_HMAC=ON`, a translation unit that constructs a
  `TokenVerifier`/`SigningAuthorizer` **without** passing a `MacFunction` fails to
  build (guard fires); passing one builds and passes.
- With the option **off** (default), everything compiles and behaves exactly as
  today (regression guard) — the reference impl remains usable.

## Cross-references

- [security.md](../spec/security.md) — the `MacFunction` seam, `hmacSha256` and its
  "correct but not hardened" caveat, `constantTimeEquals`, the token format, and
  the hardening-checklist item this satisfies.
- [tls_peer_verification.md](tls_peer_verification.md) — the sibling A-milestone
  hardening item: TLS protects the token in transit, a vetted HMAC protects the
  MAC computation; both are production-posture defaults layered on shipped seams.
- [ARCHITECTURE.md](../ARCHITECTURE.md) — the `morph::session` namespace map listing
  `SessionToken`/`TokenIssuer`/`TokenVerifier`/`SigningAuthorizer`/`MacFunction`/
  `hmacSha256`/`AuthError`.

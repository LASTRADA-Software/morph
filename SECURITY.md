# Security Policy

## Reporting a vulnerability

Please report suspected vulnerabilities **privately** via GitHub's security
advisories for this repository:
<https://github.com/LASTRADA-Software/morph/security/advisories/new>
("Report a vulnerability" on the Security tab). Do **not** open a public
issue for a suspected vulnerability.

A useful report names the affected header or spec
(e.g. `include/morph/session_auth.hpp`, `docs/spec/wire.md`), includes a
minimal reproduction (a wire envelope or token payload where relevant), and
sketches the impact. We aim to acknowledge reports within a few business days
and ask for coordinated disclosure until a fix ships.

## Supported versions

morph is pre-1.0: only the latest `master` is supported, and fixes are not
backported. A formal supported-version and deprecation policy is planned —
see `docs/planned/api_stability.md`.

## Scope

morph's security model is specified in `docs/spec/security.md` — the trust
model, authenticated sessions, and an explicit list of what morph does **not**
provide. In scope for a report is any **deviation from the documented
guarantees**, for example:

- token forgery, signature-verification bypass, or expiry/skew handling flaws
  in `session_auth.hpp`;
- authorization bypass around `IAuthorizer::authorize` / `authenticate` /
  `authorizeInstance` (e.g. an unverified principal reaching a model as
  authoritative);
- wire-hardening failures beyond the documented caveats — e.g. the
  `kMaxEnvelopeBytes` cap not being enforced, or memory-safety issues in
  envelope/action decoding;
- log sanitisation failures that let untrusted input forge log lines.

**Out of scope** are the *documented* limitations, which are known and tracked
as planned hardening work under `docs/planned/` (or, once addressed, folded
into `docs/spec/`) — among them the reference `hmacSha256` not being hardened
beyond a constant-time compare unless a vetted `MacFunction` is injected (see
`spec/security.md`, "MAC-primitive recommended wiring"), the self-signed/
`VerifyNone` TLS example (`tls_peer_verification.md`), absent rate/resource limits
(`transport_limits.md`), unauthenticated `register` and sequential model ids
(`instance_authorization.md`), and server-side model leaks on client
disconnect (`connection_scoped_cleanup.md`). Reports that a documented
limitation exists are appreciated but are not vulnerabilities; a way to make
one of them *worse than documented* is.

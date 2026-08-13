---
id: 026
title: The control-byte JSON-escaping fix landed only in the action/result codec — three sibling writers (journal, file offline queue, session token) still use plain glz::write_json on caller-supplied strings
subsystem: journal
severity: major
source: rung 1 (pastebin) final whole-branch fix wave
disposition: open
test: spec-cited
issue: https://github.com/LASTRADA-Software/morph/issues/62
---

`subsystem: journal` is one of three — this is the same defect in
`morph::journal`, `morph::offline` and `morph::session`. `journal` is named
because it carries the sharpest consequence (see "Why this is `major`"), and
`examples/FINDINGS.md`'s enum takes one value.

## No new investigation needed: this is the already-fixed registry.hpp bug

Commit `f2ad662` ("core: escape control bytes in action and result JSON
bodies") fixed exactly this mechanism one layer down, after rung 1 replayed
`tests/fuzz/findings/` as paste content. It introduced
`morph::model::detail::EscapingWriteOpts`
(`include/morph/core/registry.hpp:212-216`) and applied it at
`include/morph/core/registry.hpp:563` so that `ActionTraits<A>::toJson` and
`resultToJson` emit `\uXXXX` instead of a raw C0 byte. That struct's own doc
comment (`registry.hpp:190-211`) states the mechanism, `docs/spec/core/wire.md`
("Control bytes in string fields") states the envelope-level original, and
`tests/test_wire_hardening.cpp`'s "Bug G" cases are the regression tests.

**Everything below is that same bug, unfixed, in three other writers.** The
only thing this finding adds is the three locations and the confirmation that
their fields are caller data.

## The mechanism, re-confirmed empirically

Throwaway harness against this repo's own vendored glaze
(`build/clang-coverage/_deps/glaze-src`), sweeping every byte `0x00`–`0x1F`
through `glz::write_json` into a two-string aggregate and back through
`glz::read_json`:

- Five bytes have JSON short escapes and are handled correctly: `0x08` `0x09`
  `0x0A` `0x0C` `0x0D`. (Note in particular that `0x0A` *is* escaped, so a
  JSONL record is never split across two physical lines — the corruption is
  not a line-splitting one.)
- The **other 27** (`0x00`–`0x07`, `0x0B`, `0x0E`–`0x1F`) are written into the
  output **raw**. The resulting document is not valid JSON (RFC 8259 forbids
  unescaped `U+0000`–`U+001F` inside a string), and reading it back fails —
  *and mangles*: with a raw `0x01` in a field that also contains an escaped
  character, the reader's chunked fast path produced
  `hel<0a>lo<01>","b"<00><00><00><00>` where `hel<0a>lo<01>` was written, i.e.
  it ran past the string terminator and wrote `0x00` bytes over the buffer.
  That is the identical "silently rewrites such a byte as two `0x00`s"
  behavior `registry.hpp`'s doc comment describes.
- Rewriting the same value with `EscapingWriteOpts` emits a six-character
  `\u0001` escape in place of the raw byte, and the value round-trips cleanly.

## The three surviving locations

### 1. `include/morph/journal/action_log.hpp:151`

```cpp
inline std::string toJson(const LogEntry& entry) {
    std::string out;
    detail::throwOnGlazeError(glz::write_json(entry, out), out);
    return out;
}
```

`LogEntry` (`action_log.hpp:39-80`) has four caller-supplied string fields
that are *not* pre-escaped JSON:

- `entityKey` — an application-chosen instance identity, stamped from the
  value passed to `attachActionLog()`.
- `error` — `std::exception::what()` from whatever rejected the action.
  Exception messages routinely echo their input: `glz::format_error` embeds
  the offending document, and a model's own `ValidationError` may quote the
  field that failed. This is the most likely real-world carrier.
- `principal` — from `morph::session::current()`.
- `idempotencyKey` — documented as opaque and caller-chosen.

(`payload` and `result` are the *outputs* of `ActionTraits<A>::toJson`, so
`f2ad662` already made those two safe. That is precisely why the fix looked
complete and this one did not surface.)

### 2. `include/morph/offline/file_offline_queue.hpp:61`

```cpp
inline std::string toJson(const FileQueueRecord& record) {
    std::string out;
    throwOnGlazeError(glz::write_json(record, out), out);
    return out;
}
```

`FileQueueRecord::payload` is documented on `QueueItem` as "opaque serialised
representation of the queued action" — the queue does not produce it and does
not interpret it, so it is whatever the application hands `enqueue()`, not
necessarily `ActionTraits` output. `idempotencyKey` is likewise explicitly
opaque and caller-supplied ("the queue does not interpret, require, or
enforce uniqueness on it").

### 3. `include/morph/session/session_auth.hpp:346`

```cpp
[[nodiscard]] std::string issue(const SessionToken& claims) const {
    std::string json;
    // `SessionToken` is a flat aggregate, so writing it into a `std::string`
    // cannot fail — the result is unconditional.
    (void)glz::write_json(claims, json);
```

`SessionToken::principal` and `SessionToken::roles` are caller-supplied
(`session_auth.hpp:286-300`). The consequence differs in shape from the other
two because the claims JSON is base64url-encoded before it leaves the
process, so nothing on the wire is malformed — but the token is then
**unverifiable by its own verifier**: `TokenVerifier` base64-decodes and
`glz::read`s the claims, and the harness above confirms that round trip fails
(`err=1`) for a principal containing any of the 27 bytes. A principal that
morph itself accepted at issue time mints a credential that morph rejects as
`AuthError::Malformed`. Whether that is exploitable depends on how an
application sources principals; at minimum it is a silent
issue-succeeds/verify-always-fails asymmetry with no diagnostic.

**A smaller, separate defect in the same three lines:** the `(void)` discards
the `glz::error_ctx`. The comment justifying it ("cannot fail — the result is
unconditional") is the *reason* the write error is dropped, and it is a
reasonable claim for a flat aggregate — but it is the only one of the three
writers here that does not route its error through a `throwOnGlazeError`
helper, so if the claim ever stops holding (a `SessionToken` gaining a nested
or dynamic member) the failure is a silently-empty payload rather than a
throw. Worth folding into the same fix rather than filing separately.

## What should happen

All three should write with the same option `registry.hpp` already carries:

```cpp
struct EscapingWriteOpts : glz::opts {
    bool escape_control_characters = true;
};
```

`registry.hpp`'s own comment explains why it is duplicated rather than shared
from `morph::wire` (the model layer must not depend on the transport layer's
header for a four-line struct). Whoever fixes this should decide whether a
*fourth* and *fifth* copy is right, or whether the struct has now earned a
single home — three independent duplications is the point at which the
"deliberately duplicated" rationale deserves re-examination, and that is a
design call for the repo owner, not something this finding prescribes.

## Why this is `major` and not a paper cut

Both file-backed readers **re-throw** on a malformed line that is not the
final one, by design — a truncated *trailing* line is tolerated as a crash
artifact, but mid-file corruption is treated as genuine corruption:

- `include/morph/journal/file_action_log.hpp:212-227` — one undecodable
  entry followed by any later entry makes `entries()` throw for the whole
  file, permanently. The audit trail — the single thing
  `examples/pastebin/README.md`'s journal position paper says `morph::journal`
  is *for* ("render read-only history") — becomes unreadable in its entirety,
  and the only surviving recovery is hand-editing the file.
- `include/morph/offline/file_offline_queue.hpp:279-290` — the same shape in
  `load()`, which runs from the constructor. A durable queue whose file
  contains one such record throws on every subsequent process start, so every
  item behind it is unreachable. This is durable-store corruption written by
  the store's own writer.

Neither is reachable through today's ladder rungs (rung 1 journals only
`ActionTraits`-produced payloads and ships no offline queue), which is why
nothing is red — but both are reachable by any application that puts a raw
control byte in an entity key, an idempotency key, a principal, or an
exception message, which is exactly the input class the fuzz corpus that
found the registry.hpp original is made of.

## Not fixed here, by design

`examples/FINDINGS.md`: "the repo owner decides; the ladder never
self-triages." Rung 1's final fix wave files this as `open` rather than
patching three framework headers on its own authority — the same standard the
rung applied to findings 020–025. The mechanism is already proven and the fix
is four lines per site, so this should be cheap to schedule; what it is not
is a rung's call to make.

---
id: 027
title: "`register` envelopes carry no session, so `authorizeRegister` and the recorded owner principal are both unusable from any `Bridge` client"
subsystem: backend
severity: blocker
source: rung 2 (bookmarks) task 12 — server bootstrap with a real signing authorizer
disposition: open
test: spec-cited (repro below is a five-line `BridgeHandler` construction)
issue: https://github.com/LASTRADA-Software/morph/issues/63
---

`Bridge` stamps its default session onto every **`execute`** call
(`include/morph/core/bridge.hpp:806`):

```cpp
call.session = _defaultSession;
```

It stamps it onto nothing else. Every *control* message — `register`,
`register`-shared, `attach`, `assign`, `deregister` — is built inside the
concrete `IBackend`, which has no access to the session at all, because
`IBackend`'s registration surface
(`include/morph/core/backend.hpp:82-246`) carries only
`typeId`/`factory`/`contextKey`/`primary`. So both shipping remote backends
send a session-less envelope:

- `SimulatedRemoteBackend::registerModelWithContext`
  (`include/morph/core/remote.hpp:1497-1505`) →
  `wire::makeRegister(typeId, contextKey)`
- `SocketBackend::registerModel` (`include/morph/net/socket_backend.hpp:137`)
  → `wire::makeRegister(typeId)`

and `wire::makeRegister` (`include/morph/core/wire.hpp:151-157`) leaves
`Envelope::session` default-constructed.

## What that breaks

`RemoteServer`'s `register` handler authenticates the envelope's session and
makes the verified identity authoritative before deciding
(`include/morph/core/remote.hpp:939-949`):

```cpp
if (auto verified = _authorizer->authenticate(env.session)) {
    env.session.principal = std::move(*verified);
} else {
    env.session.principal.clear();
}
if (!_authorizer->authorizeRegister(env.session, env.typeId)) {
    reply(... makeErr("unauthorized", env.callId));
    return;
}
```

and then records the owner from that same value
(`include/morph/core/remote.hpp:1011`):

```cpp
_owners[mid] = std::move(env.session.principal);
```

Because the envelope never carried a token, `authenticate()` always fails and
`env.session.principal` is **always empty** for a `Bridge` client. Two
documented capabilities therefore cannot be reached from any `Bridge`:

1. **`authorizeRegister` cannot gate on identity.** The canonical override
   the framework's own test suite demonstrates
   (`tests/test_register_authorization.cpp:93` —
   `return !ctx.principal.empty();  // ctx.principal is already the *verified*
   identity here`) rejects **every** register a `Bridge` client issues,
   including the very first one a freshly-logged-in client makes. That test
   passes only because it hand-builds its envelopes
   (`tests/test_register_authorization.cpp:112-116`) — a path no application
   has.

2. **`authorizeInstance`'s ownership check is inert.** The recorded owner is
   always the empty string, and the documented policy shape
   (`include/morph/session/session.hpp:193`,
   `tests/test_policy_hardening.cpp:173`) treats an empty owner as "shared,
   allow anyone". So `ownerPrincipal == ctx.principal` never denies anything
   for a `Bridge`-registered instance — the per-instance authorization hook
   silently degrades to allow-all for every real client.

## Repro

Against any `RemoteServer` whose authorizer overrides `authorizeRegister` the
way `tests/test_register_authorization.cpp` documents:

```cpp
auto server = std::make_shared<morph::backend::RemoteServer>(
    pool, std::make_shared<bookmarks::auth::BookmarksAuthorizer>("secret"));
morph::bridge::Bridge bridge{std::make_unique<morph::backend::SimulatedRemoteBackend>(*server)};

morph::session::Context s;
s.principal = "alice";
s.token = morph::session::TokenIssuer{"secret"}.issue({.principal = "alice", .expiresAtMs = kFarFuture});
bridge.setDefaultSession(s);                       // valid, signed, correct secret

morph::bridge::BridgeHandler<bookmarks::BookmarkModel> handler{bridge, &exec};
// throws std::runtime_error: "register failed: unauthorized"
```

Observed verbatim while wiring rung 2's `App`:

```
[DEBUG] [dispatchMessage] connection 0: kind=register callId=0 typeId=BookmarkModel ...
PROBE: BridgeHandler ctor threw: register failed: unauthorized
```

The session is present, valid, and correctly signed on the `Bridge` — it is
simply never put on the wire for `register`.

## What should happen

A `Bridge` with an installed default session should present that session on
its control messages exactly as it does on `execute`, so that:

- `authorizeRegister` sees the same verified principal an `execute` would, and
- `_owners[mid]` records that principal, giving `authorizeInstance` something
  real to compare against.

The smallest shape that does this is an `IBackend` hook mirroring the existing
`setReconnectHandler`/`setConnectHandler`/`setDisconnectHandler`
store-and-ignore defaults — e.g. `virtual void setSession(session::Context)`,
pushed by `Bridge::setDefaultSession()` and by `Bridge::switchBackend()`, and
stamped by each wire-backed backend onto `makeRegister`/`makeRegisterShared`/
`makeAttach`/`makeAssign`/`makeDeregister`. `LocalBackend` needs nothing (it
builds no envelopes and consults no authorizer).

Not fixed here: per `examples/IMPLEMENTATION.md`'s prime directive the ladder
records framework gaps rather than patching core, and per
`examples/FINDINGS.md` the disposition is the repo owner's call, not the
rung's.

## Consequence for rung 2 while this is open

`bookmarks::auth::BookmarksAuthorizer` (rung 2, task 1) was written to the
documented shape and was therefore unusable: it rejected every register from
every client. Task 12 relaxed `authorizeRegister` to what is actually
enforceable today and moved the affected checks to the two places that *do*
see a verified principal — `SigningAuthorizer::authorize` (every `execute`
carries the token) and the models' own `session::current()->principal` reads
(`examples/IMPLEMENTATION.md` rule 1). In particular
`BookmarkModel::execute(const RecordMetadata&)` now checks the service
principal itself rather than relying on `authorizeInstance`. See that
header's and that action's own comments, which cite this finding.

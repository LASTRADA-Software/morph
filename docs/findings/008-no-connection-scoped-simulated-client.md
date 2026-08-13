---
id: 008
title: No connection-scoped simulated client
subsystem: backend
severity: minor
source: examples/LADDER.md framework prerequisite 2
disposition: open
test: spec-cited
issue: https://github.com/LASTRADA-Software/morph/issues/48
---

`SimulatedRemoteBackend` (`include/morph/core/remote.hpp:1465`) disposes every message with `ConnectionId 0` (the default), offering no way to open dedicated connections via `RemoteServer::openConnection()` (which does exist at line 395 but is unused by the simulated path). This blocks deterministic connection-lifetime tests without relying on real sockets.

**What happens instead:** tests of connection-scoped state and lifecycle (e.g. per-connection rate-limiting tokens, connection-drop recovery) cannot be written cleanly against the simulated backend and must rely on socket-based testing instead.

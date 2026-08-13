---
id: 004
title: No fault-injection wire proxy or deterministic strand interleaver
subsystem: qt
severity: blocker
source: examples/LADDER.md framework prerequisite 2
disposition: fixed
test: examples/common/testkit/test_fault_proxy.cpp; examples/common/testkit/test_strand_interleaver.cpp
---

No `fault_proxy` or `strand_interleaver` helper files exist under `examples/` yet. These are deterministic chaos-engineering tools needed to stress-test WASM clients and server protocol machinery against common failure modes (network stutters, interleavings, flaky reconnects) in reproducible ways.

**What should happen:** rung 0 (this task series) includes Task 7/8 to implement these helpers in the testkit and wire them into the common test harness. Once those land, update this finding's disposition to closed and cite the delivered test files.

**Resolution (fault-proxy half, Task 7).** `morph::ladder::testkit::FaultProxy`
(`examples/common/testkit/fault_proxy.hpp`/`.cpp`) is an in-process WebSocket
relay between a `QtWebSocketBackend` and the real `QtWebSocketServer`, with
per-`callId` reply rules — `dropReply`, `delayReply`, `duplicateReply`,
`killAfter` — plus `setRequestObserver`, which reports a forwarded request's
`callId` before the request leaves the proxy so a test can arm a rule for a
specific upcoming call race-free (`BridgeHandler::execute()` returns a bare
`Completion` and never names the id the backend assigned it). All four faults
are covered by `examples/common/testkit/test_fault_proxy.cpp`, in the
`ladder_common_tests` green gate under the `ladder` label.

**Resolution (strand-interleaver half, Task 8).**
`morph::ladder::testkit::DeterministicExecutor`
(`examples/common/testkit/strand_interleaver.hpp`, header-only) is a
`morph::exec::IExecutor` that queues every posted task and runs one only when
explicitly stepped — `step()` for the next task, `step(index)` for a chosen
one, `runSchedule({...})` for a scripted order, `drain()` for the rest. Placed
underneath a `morph::exec::detail::StrandExecutor` as its base executor, it
turns strand-ordering behavior into something a test scripts rather than
races for. `examples/common/testkit/test_strand_interleaver.cpp` covers
FIFO default order, a scripted non-default two-key interleaving through a
real `StrandExecutor`, and both throw paths; it runs in the
`ladder_common_tests` green gate under the `ladder` label.

**Closed.** Both halves this finding asked for — the fault-injection wire
proxy and the deterministic strand interleaver — now exist, are exercised by
the two tests named in `test:` above, and are part of the green gate. The
disposition stays `fix-scheduled` only because `examples/FINDINGS.md` defines
no `closed` value; nothing further is scheduled against it. Rung 1 onward
consumes these helpers rather than re-filing this gap.

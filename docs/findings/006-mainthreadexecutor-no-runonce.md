---
id: 006
title: MainThreadExecutor has no single-step runOnce()/drain()
subsystem: core
severity: minor
source: examples/LADDER.md framework prerequisite 2
disposition: open
test: spec-cited
issue: https://github.com/LASTRADA-Software/morph/issues/46
---

`MainThreadExecutor` (`include/morph/core/executor.hpp:128-177`) exposes only `runFor(std::chrono::milliseconds)`, which blocks the caller for a wall-clock duration. There is no step-oriented primitive like `runOnce()` to drain one queued task or `drain()` to pump until the queue is empty, making it cumbersome to integrate with event loops that want fine-grained control over executor invocation.

**What happens instead:** test code and integration layers must manage the blocking duration carefully, often leading to sleepy polling in tests rather than deterministic single-step execution.

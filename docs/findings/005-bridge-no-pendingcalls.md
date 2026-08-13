---
id: 005
title: Bridge has no pendingCalls() (client-side quiescence observability)
subsystem: bridge
severity: minor
source: examples/LADDER.md framework prerequisite 2
disposition: open
test: spec-cited
issue: https://github.com/LASTRADA-Software/morph/issues/45
---

`Bridge` (`include/morph/core/bridge.hpp`) provides no `pendingCalls()` method to observe how many actions are in-flight. Clients have no direct way to detect when all models have settled (all execute results have arrived), making it hard to implement "loading" indicators or guard features that depend on quiescence.

**What happens instead:** presenter-level `busy()` counters substituting for framework-level observability, duplicating counting logic across every rung's GUI layer.

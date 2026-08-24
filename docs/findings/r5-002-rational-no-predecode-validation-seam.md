---
id: r5-002
title: No pre-decode validation seam for Rational -- setWire clamps hostile wire input to a plausible value instead of rejecting
subsystem: wire
severity: minor
source: ledger rung 5, design spec §7
disposition: open
test: examples/ledger/tests/test_ledger_model.cpp (clamped Rational leg test)
issue: https://github.com/LASTRADA-Software/morph/issues/131
---

A wire payload like {"num":5,"den":0,"dp":2} decodes via Rational::setWire into a plausible 5/1 rather than being rejected at decode time (see include/morph/util/rational.hpp's codec). Every dispatch path decodes before any model-level validate() runs, so an app has no seam to catch a clamped value as clamped -- it only ever sees an already-plausible Rational. Ledger's own zero-sum invariant happens to catch most clamped legs incidentally (a clamped value is unlikely to still sum to zero), but this is coincidental protection from a business rule, not a validation guarantee the framework provides. A pre-decode validation hook (reject rather than clamp, or a decode-time flag surfacing "this value was clamped") would close the gap for any app whose own invariants don't happen to catch it.

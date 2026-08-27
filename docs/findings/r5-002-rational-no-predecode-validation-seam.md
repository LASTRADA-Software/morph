---
id: r5-002
title: No pre-decode validation seam for Rational -- setWire clamps hostile wire input to a plausible value instead of rejecting
subsystem: wire
severity: minor
source: ledger rung 5, design spec §7
disposition: fix-scheduled
test: examples/ledger/tests/test_ledger_model.cpp (clamped Rational leg test)
issue: https://github.com/LASTRADA-Software/morph/issues/131
---

A wire payload like {"num":5,"den":0,"dp":2} decodes via Rational::setWire into a plausible 5/1 rather than being rejected at decode time (see include/morph/util/rational.hpp's codec). Ledger's own zero-sum invariant happens to catch most clamped legs incidentally (a clamped value is unlikely to still sum to zero), but this is coincidental protection from a business rule, not a validation guarantee the framework provides.

**Update (morph#131).** The framework now rejects a clamped Rational on the path this finding is actually about -- an action arriving over the wire. `BRIDGE_REGISTER_ACTION`'s generated `ActionTraits<A>::fromJson` (include/morph/core/registry.hpp) wraps every action-body decode in `morph::math::WireClampScope`, a per-thread RAII scope that counts every clamp `Rational::setWire` performs while it is live, and throws `ParseError` if the count is nonzero -- so `StoreTransaction`'s own `fromJson` (ledger uses plain `BRIDGE_REGISTER_ACTION`, which expands to this) now refuses a clamped leg before the model ever sees it, closing the gap this finding raised for the dispatch path. What is not closed: a `Rational` decoded directly (`glz::read_json` on a bare `Rational`, outside an action envelope's `fromJson`) is not wrapped in a `WireClampScope` by anything -- the finding's own regression test exercises exactly that direct-decode shape, so it still demonstrates the residual gap and still passes today only because ledger's zero-sum check happens to catch it, same as before. Disposition stays `fix-scheduled` rather than `documented-limitation` because the *dispatch-path* half of this finding is closed but the *direct-decode* half is not, and nothing in `docs/spec/` yet documents direct `Rational` decode as an accepted gap.

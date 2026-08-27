---
id: r5-001
title: Rational has no checked-arithmetic mode; intermediate cross-terms can overflow before final results do
subsystem: units
severity: minor
source: ledger rung 5, design spec §7
disposition: fix-scheduled
test: tests/test_ledger_rational_fuzz.cpp
issue: https://github.com/LASTRADA-Software/morph/issues/130
---

At ledger-realistic magnitudes (dp=2 currencies, legs up to 10^9 minor units), plain `Rational::operator+`/`operator-` summed over exactly 9,223,372,037 rows (INT64_MAX / 10^9, plus one) crosses int64_t's range, which is undefined behavior on those operators (they are fixed-width, not saturating, and not exception-throwing by signature -- see include/morph/util/rational.hpp). This exact boundary is empirically confirmed by tests/test_ledger_rational_fuzz.cpp via a binary search that exercises the real Rational::operator+ at each candidate boundary (not a hand-computed estimate) -- see that test for the measurement method.

**Update (morph#130).** A checked-arithmetic mode now exists: `morph::math::checkedAdd`/`checkedSub`/`checkedMul`/`checkedDiv` (include/morph/util/rational.hpp) return `std::expected<Rational, RationalError>` and detect overflow rather than invoking it, so a ledger-scale application can adopt these free functions on the summation path to catch the boundary above before committing corrupted state. Disposition is `fix-scheduled` rather than closed because the primitive existing is not the same as ledger's own summation loop having adopted it -- that adoption is separate application-layer work.

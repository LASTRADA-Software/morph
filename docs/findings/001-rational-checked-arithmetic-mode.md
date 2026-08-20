---
id: 001
title: Rational has no checked-arithmetic mode; intermediate cross-terms can overflow before final results do
subsystem: units
severity: minor
source: ledger rung 5, design spec §7
disposition: open
test: tests/test_ledger_rational_fuzz.cpp
---

At ledger-realistic magnitudes (dp=2 currencies, legs up to 10^9 minor units), Rational::operator+ summed over roughly 9 billion rows crosses int64_t's range, which is undefined behavior today (Rational's arithmetic operators are fixed-width, not saturating, and not exception-throwing by signature -- see include/morph/util/rational.hpp). A checked-arithmetic mode (an expected<Rational, Overflow>-returning operator+/- alongside the existing noexcept ones, or a debug-mode overflow assertion) would let a ledger-scale application detect this before committing corrupted state, rather than relying on the app never summing enough rows to hit the boundary in practice.

---
id: 012
title: No pre-decode wire validation seam
subsystem: forms
severity: major
source: examples/LADDER.md, forms-subsystem gaps
disposition: open
test: spec-cited
issue: https://github.com/LASTRADA-Software/morph/issues/51
---

Wire-decoded `Quantity` fields reach `validate()` as plausible numbers without pre-flight checking. A client can submit a clamped `Rational` (e.g. a quantity that the wire protocol knows cannot exist based on unit bounds, precision rules, or physical constraints) and the server's `validate()` method receives it as-is, having to decide whether to reject it or coerce it. There is no seam where pre-decode validation can reject malformed wire payloads before they enter the action's own validation logic.

**What happens instead:** apps must duplicate validation logic (field-level wire checks) in their action's `validate()` method, or accept that impossible values can transit the wire and be handled only at the business-logic layer.

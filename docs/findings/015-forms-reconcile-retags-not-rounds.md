---
id: 015
title: reconcileDeclaredPrecision retagging behavior — verify spec/code agreement
subsystem: forms
severity: minor
source: examples/LADDER.md; docs/spec/forms/forms.md line 1178
disposition: documented-limitation
test: spec-cited
---

**Verification finding (not an assertion).** LADDER.md claims that `reconcileDeclaredPrecision` "retags rather than rounds (spec text and code disagree)". Inspection of:

- `docs/spec/forms/forms.md:1178`: "Retags every `Quantity` member of `action` in place to its declared precision (`atDeclaredPrecision()`)"
- `include/morph/forms/forms.hpp:2128`: `member = member.atDeclaredPrecision();`

shows the spec **already documents** the retag behavior exactly as the code implements it — no disagreement exists at this citation. The LADDER.md claim appears stale as of this rung.

**Disposition.** Filed as `documented-limitation` because the spec explicitly documents the retag-vs-round design choice. Rung 6 owns the decision of whether to stay with retag or migrate to rounding; this entry serves as a flag that the claim in LADDER.md was verified as already-resolved.

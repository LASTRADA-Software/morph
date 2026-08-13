---
id: 011
title: Forms rule vocabulary is closed single-node conditions (no and/or/not)
subsystem: forms
severity: major
source: examples/LADDER.md, forms-subsystem gaps
disposition: open
test: spec-cited
issue: https://github.com/LASTRADA-Software/morph/issues/50
---

The `x-rules` vocabulary in `include/morph/forms/forms.hpp` (enum `RuleKind`, lines 455-469) provides only single-node condition types: `Engaged`, `NotEngaged`, `Equals`, `Greater`, `GreaterOrEqual`, `Less`, `LessOrEqual`, plus rule kinds `RequiredWhen`, `ExactlyOneOf`, `AtLeastOneOf`, `MutuallyExclusive`, `VisibleWhen`, `ReadonlyWhen`. There are no compound operators like `and`, `or`, `not` to combine conditions.

**What happens instead:** rules that require boolean logic (e.g. "show field X when both A and B are true") must be factored into multiple single-condition rules or expressed through app-level constraint logic outside the schema, leaving sophisticated EspoCRM-class business rules inexpressible directly.

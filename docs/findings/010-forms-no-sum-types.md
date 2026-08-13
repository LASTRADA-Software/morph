---
id: 010
title: Forms palette has no sum types
subsystem: forms
severity: major
source: examples/LADDER.md, forms-subsystem gaps
disposition: documented-limitation
test: spec-cited
---

The forms vocabulary provides no native sum-type support (tagged unions, discriminated unions). When an action field must express one of several alternatives — such as a measurement that is "a quantity, or below limit-of-detection, or above upper detection limit" — the application encodes it as a multi-field structure glued by cross-field rules (`x-rules`), per `docs/spec/forms/forms.md`'s "Sum types not in the forms palette — multi-field encoding by design" section.

This is an intentional design constraint: sum types are rare in the domain models the ladder exercises (which already use `hasValue()` optionality and `Choice` enums), and the rule-based multi-field encoding is expressive enough for the ladder's rungs while keeping the schema and validation machinery focused and maintainable.

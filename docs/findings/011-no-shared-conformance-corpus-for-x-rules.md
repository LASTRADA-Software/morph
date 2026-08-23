---
id: 011
title: x-rules has exactly one client-side evaluator (JavaScript, inside DynamicForm.qml) and no shared corpus proving it agrees with the compiled C++ one
subsystem: forms
severity: minor
source: lims rung 6, build order §5 (review D8)
disposition: open
test: examples/lims/tests/test_conditional_logic.cpp (the parity suite; the evaluator it contains is the third implementation of the same vocabulary)
---

`docs/spec/forms/forms.md` makes client/server parity the *reason* the rule
vocabulary is closed:

> The vocabulary is deliberately closed: adding a new rule kind is a framework
> change, never an application-supplied lambda, which is what lets the client
> and the server evaluate identically from the same serialized form.

Two evaluators exist for that serialized form, in two languages, and nothing
checks them against each other:

- **Server**: `morph::forms::allRulesSatisfied<A>(action)`, over the compiled
  `A::formRules` nodes (`include/morph/forms/forms.hpp`). Tested by
  `tests/test_forms_rules.cpp`, which also asserts the emitted `x-rules` JSON
  text.
- **Client**: JavaScript in `src/qt/forms/qml/DynamicForm.qml` (`property var
  rules: schema["x-rules"]`, and the `evaluateCondition`/`evaluateRule`
  functions below it). Tested by `src/qt/forms/tests/tst_DynamicFormRules.qml`.

`grep -rn 'x-rules' include/ src/` shows `x-rules` is *emitted* from C++ and
*consumed* only by that one QML file. There is no C++ (or otherwise reusable,
non-Qt) evaluator.

Two consequences.

## 1. The parity claim is untested as a *joint* property

Each evaluator is tested against its own hand-written expectations. Neither
suite feeds the same schema and the same field values to both and asserts the
verdicts match. A divergence — the exact asymmetry between `equals` (not
vacuous on an unengaged field) and the comparison kinds (vacuous) is the
obvious candidate — would be caught by neither.

The renderer conformance kit is where such a joint property would belong, and
its documented scope note does not list `x-rules`: the five `CF*` fixtures
cover `x-order`, `required`, `x-decimalPlaces`, `x-unitAlternatives`,
`x-optionsAction`/`x-optionValue`/`x-optionLabel`, `format` and `ExtUnits`,
plus the `x-widget` keys. Rule evaluation is tested separately, in a QML file
outside the corpus.

## 2. A non-QML client has to write the evaluator itself

The ladder's own clients are the case in point: `examples/lims`' field
devices, and every rung's WASM/desktop client, are not QML renderers. To honour
a `requiredWhen` before submitting, such a client must implement an
interpreter for the whole closed vocabulary — `engaged`, `notEngaged`,
`equals`, four comparison kinds, three membership kinds, `requiredWhen`,
`visibleWhen`, `readonlyWhen`, and the recursive `and`/`or`/`not` — including
the vacuity asymmetry and the fail-closed-on-unknown-kind rule.

`examples/lims/tests/test_conditional_logic.cpp` contains such an
implementation (~150 lines) written for exactly this reason, and asserts it
agrees with `allRulesSatisfied` across all 24 points of the rung's rule state
space. It is the **third** implementation of one closed vocabulary the
framework owns.

## What should happen

Either:

1. **Ship the evaluator in C++.** A header-only
   `morph::forms::evaluateRules(schemaJson, fieldStates)` over the same DOM the
   emitters produce, which the QML renderer could then call through instead of
   duplicating in JavaScript — one implementation, and the "identically" in the
   spec sentence becomes structural rather than aspirational. A `FieldState`
   abstraction (engaged + comparable value) is the only new surface it needs.
2. **Or add a joint corpus to the conformance kit** — one shared table of
   (schema, field values, expected verdict) rows driven from both the C++ and
   the QML side, so the two implementations are pinned to each other even if
   they stay separate.

Option 1 also removes the per-client reimplementation, which is the part that
will otherwise recur once per non-QML client the ladder builds.

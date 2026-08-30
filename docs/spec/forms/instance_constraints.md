# `morph::forms::InstanceConstraints` — per-instance values in framework keys

`forms/instance_constraints.hpp`

## The problem it exists for

`schemaJson<A>()` is a pure function of the compiled action type. Every key it
emits — `required`, `x-rules`, `x-decimalPlaces` — is derived from `A`'s
members, and the result is memoised per type and shared process-wide. That is
exactly right when a form is fixed at compile time. It is wrong the moment form
*definitions are data*: a versioned catalogue whose version 1 declares three
decimal places and whose version 2 declares one has two definitions of one
compiled action, and nothing in the compiled type can tell them apart.

Before this header the only way out was for the application to patch a
**second, app-private key** beside the framework's — `x-versionDecimalPlaces`
next to `x-decimalPlaces` — and re-implement the check by hand in the model.
That is worse than either key alone: a renderer is handed two numbers for one
concept with no way to know which is true. Bounds fared worse still. A
specification range could only be served as an `x-specHigh` no framework code
read, so a value outside the range a form had just advertised passed
`validate()` and was stored with nothing anywhere recording that it was out of
range (issue #164).

## What it is

One value type carrying, per wire field, the keys an *instance* gets to
determine — and doing both halves of the job from that single declaration:

| Half | Call | Effect |
|---|---|---|
| Serve | `decorate(schema)` / `instanceSchemaJson<A>(constraints)` | Rewrites the framework's own keys with the instance's values, and stamps `x-instanceConstraints`. |
| Check | `checkAction(action)` / `checkValue(field, value)` | Reports what a submitted value violates, against the identical declaration. |

Because the served bound and the checked bound are read from the same object,
they cannot drift apart. That property — not the individual keys — is the point
of the type.

```cpp
morph::forms::InstanceConstraints constraints;
constraints.declare({.field = "value",
                     .decimalPlaces = version.decimalPlaces,   // from a database row
                     .minimum       = version.specLow,
                     .maximum       = version.specHigh});

const auto served = morph::forms::instanceSchemaJson<CaptureConcentration>(constraints);
// ... later, on the way in:
for (const auto& violation : constraints.checkValue("value", reading)) { /* model policy */ }
```

## API

| Symbol | Meaning |
|---|---|
| `FieldConstraint` | One field's constraint: `field` (wire key) plus optional `decimalPlaces`, `minimum`, `maximum`. Every optional defaults to "not declared" — absent means the compiled key is left alone and nothing is checked. |
| `InstanceConstraints::declare(FieldConstraint)` | Records a constraint, replacing any previous entry for the same field. Chainable. |
| `InstanceConstraints::forField(name)` | The entry for a field, or `nullptr`. |
| `InstanceConstraints::fields()` / `empty()` | The entries in declaration order / whether there are none. |
| `InstanceConstraints::decorate(schema)` | Returns the schema text with the instance's keys written in. |
| `InstanceConstraints::checkValue(field, value)` | Violations of one exact `Rational` against one field's constraint. |
| `InstanceConstraints::checkAction(action)` | `checkValue` over every engaged `Quantity` member of a reflected action, in declaration order. |
| `instanceSchemaJson<A>(constraints)` | `constraints.decorate(schemaJson<A>())`. |
| `ConstraintViolationKind` | `BelowMinimum`, `AboveMaximum`, `PrecisionExceeded`. |
| `ConstraintViolation` | `{field, kind}`, equality-comparable. |
| `violationKindName(kind)` | `"belowMinimum"` / `"aboveMaximum"` / `"precisionExceeded"`, for log lines and model error messages. |

### Emitted keys

Added to the renderer contract in [forms.md](forms.md#renderer-contract-the-schema-key-vocabulary):

| Key | Where | Meaning |
|---|---|---|
| `x-decimalPlaces` | property node | **Overwritten** with the instance's precision when one is declared. Same meaning as always — the entry granularity — now sourced from data. |
| `x-minimum` / `x-maximum` | property node | The instance's inclusive bounds, as `{"num","den","dp"}` — the same node shape a `Rational` takes on the wire, so a renderer parses a bound exactly the way it parses the value it bounds. Distinct from `x-min`/`x-max`, which are a *slider track* and are never checked. |
| `x-instanceConstraints` | top-level | Array of the wire field names whose keys came from instance data rather than from the compiled type. **This is what makes a decorated schema self-describing**: without it a renderer could not tell an instance-sourced `x-decimalPlaces` from a compiled one, which is the ambiguity the two-key workaround created in the first place. |

Emitted only when at least one declared field actually exists on the schema; a
schema with no decorations carries no `x-instanceConstraints` key.

### What the shipped renderer does with them

A key nothing reads is the two-key workaround wearing one name, so
`DynamicForm.qml` honours all three:

| Key | Effect in the renderer |
|---|---|
| `x-decimalPlaces` | The field's entry granularity and the maximum fraction length accepted, whether the value came from the compiled type or from a row. |
| `x-minimum` / `x-maximum` | A `Quantity` outside the range produces no payload literal, so the form does not become submittable — alongside, not instead of, the compiled `minimum`/`maximum`. An instance range therefore narrows the type's; it cannot widen it. |
| `x-instanceConstraints` | Not consumed. It exists for a renderer that needs to *distinguish* an instance-sourced key from a compiled one, and for a reviewer or conformance test asking whether the matching model-side check exists. |

Two boundaries, both inherited from the framework rather than chosen here:

- **`Quantity` fields only**, matching `checkAction`. A client that gated a key
  the model does not check would be a fresh divergence, not a repair of one.
- **The client compares a `double`** quotient of the bound's `{num,den}`,
  exactly as it already does for the compiled `minimum`/`maximum`. The exact
  comparison is `checkValue`'s, against a `Rational`. The renderer's gate is an
  approximation everywhere; the model is the floor.

### `checkValue` vs `checkAction`

`checkAction` is the common case: walk the decoded action, check every engaged
`Quantity` whose name carries a constraint. It skips empty quantities
(emptiness is `required`'s business, not a bound's) and non-`Quantity` members
(bounds and decimal precision are exact-value concepts, and `Quantity` is the
only form field type that carries an exact value).

`checkValue` exists because a model's *stored* value is frequently not a member
of the action at all — a lab reading multiplied by a dilution factor, a total
recomputed from line items. The constraint governs the value about to be
stored, so the value-level entry point is the honest one there.

### Failure behaviour

Decoration never throws and never mangles. A schema that is not readable JSON,
or that has no `properties` object, is returned verbatim; a constraint naming a
field the schema has no property for is skipped. This is the same "an
application's declaration mistake is not worth a crash" rule `formLayout` and
`fieldSpans` already follow in `mergeSchemaExtras`.

## Design decisions

### The framework reports; the model decides

`checkAction`/`checkValue` return violations rather than throwing, and nothing
in `morph::forms` decides what a violation means. This is not indecision: the
right response genuinely differs by domain. An out-of-specification laboratory
result is the finding the laboratory exists to report — refusing it would
destroy the observation — while a reading finer than the method supports is a
claim about the instrument and must be refused rather than rounded. A framework
that picked one would be wrong for the other. What the framework guarantees is
that the violation is *nameable*, which is precisely what was missing.

### Why dispatch cannot apply these automatically

`reconcileDeclaredPrecision` and `enforceQuantityBounds` are applied by the
dispatch runners for every registered action, without the model's involvement.
Instance constraints are not, and cannot be, for two independent reasons:

1. **The runner has no instance.** It decodes an action from bytes. The row a
   constraint lives in is reachable only through the model — often only after
   resolving an id *carried by the action being decoded* (`analysisVersionId`).
   A hook on the action type could not read it without giving a plain DTO
   database access.
2. **There is no single correct response** (above), so a runner could only
   choose "reject", which is wrong for the flagging case.

The consequence is stated plainly rather than papered over: **a model that
decorates a schema is responsible for checking against the same constraints.**
`x-instanceConstraints` is the mitigation — a decorated schema says so, so a
reviewer or a conformance test can ask whether the matching check exists.

### Precision can narrow by rejection, never by rounding

`reconcileDeclaredPrecision` re-rounds every `Quantity` to
`Quantity<U, Dec>::declaredDecimals` — the *compile-time* precision — on the
wire dispatch paths, before any model code runs. An instance declaring a
*coarser* precision than the compiled type therefore cannot have its value
rounded for it: by the time the model can consult the instance, the type-driven
pass has already happened, and re-rounding there would mean rounding twice.
`PrecisionExceeded` is the seam's answer, and a model that wants coarser
storage rounds explicitly. Making rounding instance-aware would mean threading
per-instance data into the dispatch runner, which is item 1 above.

## Limitations

### Values vary; structure does not

An instance can change what a key *says*. It cannot add a field, remove one,
change the `required` array, or add a rule — those all come from the compiled
type, and `decorate` deliberately touches none of them. A definition wanting a
different *shape* still cannot be served without recompiling.

This is the boundary the `crm` rung's runtime custom fields (its build order
step 9) runs into head-on, and this seam does not move it: it covers the case
where the fields are known at compile time and only their *parameters* are
data. An open extension bag whose members appear in schemas, validation and the
journal as first-class fields is a strictly larger problem — it needs a
runtime-keyed value carrier on the action, not a runtime-valued key on a
compiled member. What this seam does settle for that work is the *shape of the
answer*: one declaration that both serves and checks, and a document-level
stamp naming what came from data.

### `Quantity` fields only

`checkAction` checks `Quantity` members. A bound on a `std::string` length, a
`Choice`'s permitted option set, or a `Timestamp` range is not expressible —
those would each need their own constraint vocabulary, and none of them has the
`x-decimalPlaces` problem that motivated this one.

### Not memoised

`schemaJson<A>()` caches one string per type. `instanceSchemaJson<A>` cannot:
its result varies per instance. The compiled half is still memoised, so the
per-call cost is a DOM round trip, not schema generation. A model serving many
instances caches decorated schemas itself if it needs to.

## Lifetime annotations

`declare()` (returns `*this`), `forField()` (returns a pointer into `_fields`) and
`fields()` all mark their implicit object parameter `MORPH_LIFETIMEBOUND`
(`morph/attributes.hpp`). This is the compiler-checkable half of the invalidation
note on `forField()`: the pointer is into the constraints object, and outlives
neither it nor a later `declare()`. See [concurrency_and_lifetimes.md](../concurrency_and_lifetimes.md#morph_lifetimebound--the-must-outlive-rules-told-to-the-compiler).

## Cross-references

| Spec | Why |
|---|---|
| [forms.md](forms.md) | `schemaJson<A>()`, the key vocabulary these keys join, and `reconcileDeclaredPrecision` / `checkQuantityBounds` — the type-driven precision and bounds seams this one complements. |
| [forms.md — Per-field scalar bounds](forms.md#per-field-scalar-bounds--minimum--maximum--multipleof) | `FieldMeta::minimum`/`maximum`/`multipleOf` — the **compile-time, per-field** counterpart of these keys. A compiled bound is the type's own floor and this decoration narrows it; the two are checked together, never one instead of the other. |
| [quantity_type.md](../util/quantity_type.md) | `Quantity`, `declaredDecimals`, and `UnitTraits<E>::bounds` — the per-*unit* compile-time bound, which is why a per-field one needed a separate vocabulary. |
| [rational.md](../util/rational.md) | The exact `Rational` bounds are compared against, and the `{num,den,dp}` wire shape `x-minimum`/`x-maximum` reuse. |

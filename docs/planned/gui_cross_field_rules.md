# Declarative cross-field validation rules (planned)

> **Status: planned — not yet implemented.** This spec is part of the GUI
> enhancement program ([gui_overview.md](gui_overview.md), Tier 1) and depends on
> the planned server-side validator in [validation.md](validation.md). It extends
> the `x-*` vocabulary and readiness model of [forms.md](../spec/forms.md) with a
> **closed, typed rule vocabulary** that one declaration drives onto the schema,
> the client submit gate, *and* the server check with no drift. It describes the
> intended behavior; the code does not implement it yet. See [todo.md](../todo.md).

## The gap

Today an action's readiness is a single flat predicate: `allRequiredEngaged<A>()`
([forms.md](../spec/forms.md)) checks that every required empty-capable field is
engaged, and that is the whole of the framework's declarative validation. Any
condition that spans **two or more fields** — "end date must be after start
date", "supply either an email or a phone but not both", "discount is required
only when a promo code is entered" — has nowhere declarative to live. The author
can only express it inside the model's `validate()` body as arbitrary C++, which:

- **the schema cannot see**, so a renderer emits no `required`/error affordance
  for it and the submit gate stays green until the server rejects the round-trip;
- **the client cannot evaluate live**, so cross-field errors surface only after a
  failed dispatch, not as the user types; and
- **duplicates intent** — the same relationship is re-encoded in whatever the
  renderer hard-codes and in the model, two copies that drift.

`allRequiredEngaged` is per-field and membership-blind by design; it is not the
place to grow comparisons and conditionals. What is missing is a *declarative,
typed* way to state a cross-field relationship **once** and have the schema, the
client gate, and the planned server validator ([validation.md](validation.md))
all evaluate it identically.

## Goal

Let an action declare a `static constexpr` list of cross-field rules drawn from a
**closed, typed vocabulary** (required-when, comparisons, exactly-one-of,
mutually-exclusive, …). From that single declaration:

1. `schemaJson<A>()` emits the rules as an `x-rules` array (and folds
   unconditional requiredness into the existing `required` array) so a renderer
   can show them and block submit;
2. the client submit gate and the reactive `set<>` path evaluate them live; and
3. the planned server-side validator ([validation.md](validation.md)) evaluates
   **the same rule list** inside the dispatcher runner.

Because the vocabulary is closed and typed — not arbitrary C++ predicates —
client and server evaluate it **identically** from the same data. Arbitrary logic
that does not fit the vocabulary still lives in the model's `validate()`, exactly
as today; this feature does not try to replace it.

## Design

### Single source of truth: one declaration → three consumers

An action opts in by declaring `static constexpr` rules, next to the existing
`optionalFields` opt-out ([forms.md](../spec/forms.md)):

```cpp
// NEW — proposed vocabulary in namespace morph::forms::rules
struct BookRoom {
    morph::time::Timestamp checkIn;
    morph::time::Timestamp checkOut;
    std::optional<std::string> email;
    std::optional<std::string> phone;
    morph::forms::Choice<std::int64_t, "ListPromos"> promo;
    std::optional<Money> discount;

    // ONE declaration. Drives schema x-rules, the client gate, and the server check.
    static constexpr auto formRules = morph::forms::ruleList(
        greater(&BookRoom::checkOut, &BookRoom::checkIn),          // checkOut > checkIn
        exactlyOneOf(&BookRoom::email, &BookRoom::phone),          // one contact method
        requiredWhen(&BookRoom::discount, engaged(&BookRoom::promo)) // discount iff promo set
    );

    [[nodiscard]] bool validate() const {
        return morph::forms::allRulesSatisfied(*this)   // NEW — the rule engine
            && morph::forms::allRequiredEngaged(*this); // existing per-field check
    }
};
```

- **`formRules`** is the declaration the whole feature keys on: a
  `static constexpr` value of a `RuleList<...>` type (NEW). The engine detects it
  with a `HasFormRules<A>` concept (NEW), mirroring how `HasOptionalFields<A>`
  detects `optionalFields` ([forms.md](../spec/forms.md)).
- **`allRulesSatisfied<A>(action)`** (NEW) is the shared evaluator — the single
  function that walks `A::formRules` and returns `true` only when every rule
  holds. `allRequiredEngaged` stays exactly as it is; the author `&&`s the two
  (or the framework does, when a rule list is present — see below).
- Field references are **pointer-to-member NTTPs**, recovered via the existing
  `MemberPointerTraits` ([bridge.md](../spec/bridge.md)), so a rule names real,
  type-checked members — a renamed or deleted field is a compile error, unlike the
  opaque string names a `Choice` carries.

### The closed, typed vocabulary

Each factory below builds one strongly-typed rule node; `ruleList(...)` composes
them into the `RuleList<...>` value. The set is **deliberately closed**: adding a
new rule kind is a framework change (a new node type plus its evaluator and its
`x-rules` emission), never an application-supplied lambda. This is what lets the
client and the server run the *same* evaluation from the *same* serialized form.

| Factory (NEW) | Meaning | `x-rules` `kind` |
|---|---|---|
| `requiredWhen(field, cond)` | `field` must be engaged when `cond` holds. | `"requiredWhen"` |
| `greater(a, b)` / `greaterOrEqual(a, b)` | `a > b` / `a >= b` (numeric / `Timestamp`). | `"greater"` / `"greaterOrEqual"` |
| `less(a, b)` / `lessOrEqual(a, b)` | `a < b` / `a <= b`. | `"less"` / `"lessOrEqual"` |
| `exactlyOneOf(f1, f2, …)` | Exactly one of the listed fields is engaged. | `"exactlyOneOf"` |
| `atLeastOneOf(f1, f2, …)` | At least one is engaged. | `"atLeastOneOf"` |
| `mutuallyExclusive(f1, f2, …)` | At most one is engaged. | `"mutuallyExclusive"` |

Conditions accepted by `requiredWhen` are themselves a closed set of **condition
nodes** (NEW), not predicates: `engaged(field)` / `notEngaged(field)`,
`equals(field, literal)`, and the comparison factories above reused as a boolean.
Literals are restricted to the JSON-representable scalar types a field can hold
(`std::int64_t`, `bool`, `std::string`, and the exact `Rational` of a numeric
field — see below), so the condition serializes losslessly into `x-rules`.

Comparisons operate on the field's **engaged value**; an unengaged operand makes
the comparison vacuously satisfied (the required-ness of the operand itself is a
separate `required`/`requiredWhen` concern), so `greater(checkOut, checkIn)` does
not fire spuriously while the form is still being filled.

### Numeric comparisons reuse exact `Rational` arithmetic

When both operands are `Quantity` (or otherwise numeric), the comparison is
performed on the exact `math::Rational` payload ([rational.md](../spec/rational.md)),
after `reconcileDeclaredPrecision` ([forms.md](../spec/forms.md)) has normalised
each operand to its declared precision — never on a lossy `double`. `Quantity`
already exposes `operator*` for its `Rational` payload and comparison over
`Rational` is exact, so `greater`/`less` are exact and give the identical result
on client and server. A literal in `equals`/a comparison is likewise carried as a
`Rational` (`{num, den}`), so both sides parse the same value.

### The `x-rules` schema emission

`mergeSchemaExtras` ([forms.md](../spec/forms.md)) gains a step that walks
`A::formRules` and emits a **top-level** `x-rules` array on the schema object,
alongside the existing `required` array. Each element is a self-describing JSON
object a renderer (or the server) can evaluate without any C++ type information:

```json
"x-rules": [
  { "kind": "greater", "fields": ["checkOut", "checkIn"] },
  { "kind": "exactlyOneOf", "fields": ["email", "phone"] },
  { "kind": "requiredWhen", "fields": ["discount"],
    "when": { "kind": "engaged", "fields": ["promo"] } }
]
```

Field names in `x-rules` are the **wire (JSON) field names** the members
serialise as — resolved from the pointer-to-member the same way `x-order` is
derived — so they line up with the property keys a renderer already indexes.

New keys this spec adds to the [forms.md](../spec/forms.md) renderer-contract
table (all additive, all optional):

| Key | Where | JSON type | Meaning / renderer obligation |
|---|---|---|---|
| `x-rules` | top-level (object) | array of rule objects | Cross-field rules the renderer must satisfy before enabling submit, and should surface live as inline errors. Emitted only when the action declares `formRules`; absent otherwise. A renderer that ignores it falls back to per-field `required` only. |
| ↳ `kind` | rule / condition object | string | One of the closed vocabulary ids in the table above (or a condition id: `engaged`, `notEngaged`, `equals`). An unrecognised `kind` must be treated as "cannot evaluate" — the renderer leaves the gate to the server rather than passing the rule (fail-closed). |
| ↳ `fields` | rule / condition object | array of strings | Wire field names the rule ranges over, in declaration order (operand order is significant for `greater`/`less`). |
| ↳ `when` | `requiredWhen` object | rule/condition object | The nested condition under which the listed `fields` become required. Present only for `requiredWhen`. |
| ↳ `value` | `equals` condition object | scalar / `{num,den}` | The literal an `equals` condition compares against; a numeric literal is the exact `Rational` `{num, den}` (see [rational.md](../spec/rational.md)), never a `double`. |

### Server-side: the same list, evaluated in the dispatcher

This is the crux of no-drift. [validation.md](validation.md) injects
`ActionValidator<A>::ready(action)` into the dispatcher runner after `fromJson`
and precision reconciliation. Because the author's `validate()` body calls
`allRulesSatisfied(*this)`, and `ActionValidator<A>::ready` auto-detects
`validate()` via the `HasValidate` concept ([registry.md](../spec/registry.md)),
**the server evaluates the exact same rule list the client did** — the same typed
nodes over the same reconciled values — with zero extra server code. A hand-built
envelope that violates a rule is rejected with the `ValidationError` that
[validation.md](validation.md) defines, on every path (local, simulated-remote,
Qt WebSocket), never reaching `Model::execute`.

The server never trusts the client's *evaluation*; it re-runs the rules itself.
`x-rules` in the schema is purely the client-side and documentation projection of
the same declaration — the authority is the C++ `formRules` compiled into the
server.

### Client-side: live gate on the reactive path

On the `set<>` reactive draft path ([bridge.md](../spec/bridge.md)), the
`ActionValidator<A>::ready(snapshot)` check that already gates each fire now
transitively runs `allRulesSatisfied`, so the action does not fire until every
cross-field rule holds — the live gate and the submit gate become the same
predicate. A schema renderer that reads `x-rules` can additionally show *which*
rule is unsatisfied inline, rather than only greying the submit button.

## Additivity and renderer fallback

Every key here is an additive, optional `x-*` (or the always-safe extension of
the existing `required` array), consistent with the unversioned-schema stance of
[forms.md](../spec/forms.md) and [gui_overview.md](gui_overview.md). An action
that declares no `formRules` emits no `x-rules` and behaves exactly as today.
A renderer that does not understand `x-rules` still produces a usable form: it
honours the per-field `required` array and lets the **server** reject any
cross-field violation via `ValidationError` — the correctness floor never depends
on the client understanding the key, because the server evaluates the same rules
regardless. Adding a new rule `kind` in a later release is likewise additive; an
older renderer treats an unknown `kind` as fail-closed (defers to the server).

## Non-goals

- **Not arbitrary predicates.** The vocabulary is closed and typed on purpose, so
  client and server evaluate identically. Logic that does not fit (cross-entity
  lookups, balance checks, anything needing model state) stays in the model's
  `validate()`/`execute` and is **not** reflected into `x-rules` — the same
  division [validation.md](validation.md) draws between field-level readiness and
  model invariants.
- **Not nested-action rules.** Like all of [forms.md](../spec/forms.md), rules
  range only over an action's own **flat, top-level** members. Sub-members of a
  nested aggregate are not addressable.
- **Not authorization.** Rules answer "is this action internally consistent?", not
  "may this caller do it?" — authorization stays in `IAuthorizer`
  ([security.md](../spec/security.md)), exactly as [validation.md](validation.md)
  states.
- **Not option-membership validation.** Whether a `Choice` value is a *current*
  option is still unchecked at both ends ([choice.md](../spec/choice.md)); a rule
  can require a `Choice` be engaged, not that its value is a live option.
- **No localisation of rule messages.** `x-rules` carries structure, not
  human-readable text; per-locale error strings are out of scope for the same
  reason the schema is un-localised ([forms.md](../spec/forms.md)).

## Testing (planned)

- An action declaring `greater(checkOut, checkIn)`: `schemaJson<A>()` emits a
  matching `x-rules` entry; `allRulesSatisfied` returns `false` for an inverted
  pair and `true` when ordered; an unengaged operand is vacuously satisfied.
- `exactlyOneOf(email, phone)` / `mutuallyExclusive` / `atLeastOneOf` evaluate
  correctly for zero, one, and multiple engaged fields.
- `requiredWhen(discount, engaged(promo))`: `discount` is not required while
  `promo` is empty and becomes required once `promo` is engaged.
- **No-drift:** the same violating action rejected on the client gate is rejected
  by the dispatcher runner via `ValidationError` ([validation.md](validation.md))
  over `SimulatedRemoteBackend` and the Qt WebSocket transport, and on
  `LocalBackend` — `Model::execute` is never entered.
- Numeric comparison uses the exact `Rational` value after
  `reconcileDeclaredPrecision`; a value differing only below declared precision
  compares equal on both sides.
- An action with **no** `formRules` emits no `x-rules` and dispatches unchanged
  (backward compatibility).
- A renderer ignoring `x-rules` still blocks on `required`, and a cross-field
  violation is caught server-side.

## Cross-references

- [gui_overview.md](gui_overview.md) — the umbrella program; this is its Tier-1
  cross-field-rules feature and the concrete realisation of "one rule declaration
  drives schema + client + server."
- [validation.md](validation.md) — the server-side validator this shares its
  single declaration with; `ValidationError`, the dispatcher injection point, and
  the precision-reconciliation-before-validate order this rule engine relies on.
- [forms.md](../spec/forms.md) — the `required` array, `allRequiredEngaged`,
  `mergeSchemaExtras`, `optionalFields`, and the renderer-contract table this
  `x-rules` block extends; `reconcileDeclaredPrecision`.
- [registry.md](../spec/registry.md) — `ActionValidator<A>::ready`, the
  `HasValidate` concept that picks up the author's `validate()`, and
  `BRIDGE_REGISTER_VALIDATOR` as the alternative plug-in point.
- [bridge.md](../spec/bridge.md) — the reactive `set<>`/`tryFireImpl` gate the
  live rule check runs on, and `MemberPointerTraits` used to name rule fields.
- [rational.md](../spec/rational.md) — the exact `Rational` arithmetic numeric
  comparisons and literals use, so client and server compare identical values.
- [choice.md](../spec/choice.md) — `Choice` engagement, which a rule may reference
  but whose option-membership remains unchecked.
- [todo.md](../todo.md) — execution order within the GUI program.

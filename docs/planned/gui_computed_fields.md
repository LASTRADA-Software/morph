# Computed (derived, read-only) fields (planned)

> **Status: planned — not yet implemented.** This spec is part of the GUI
> enhancement program ([gui_overview.md](gui_overview.md), Tier 1). It extends the
> `x-*` vocabulary of [forms.md](../spec/forms.md) and builds on the reactive
> `subscribe`/`set<>` draft path of [bridge.md](../spec/bridge.md). It describes
> the intended behavior; the code does not implement it yet. See
> [todo.md](../todo.md).

## The gap

Some form fields are not entered by the user — they are a **pure function of
other fields**: `total = qty * price`, `vatDue = net * rate`, `bmi = mass /
(height * height)`. Today morph has no way to say so. An author who wants a live
total must either:

- **omit it from the action** and recompute it client-side in hand-written
  renderer code (invisible to the schema, duplicated per renderer, and never
  seen by the server), or
- **include it as an ordinary field**, in which case the renderer offers it as an
  editable input, `required`-ness treats it like any other field, and — worst —
  the server stores **whatever value the client sent**, trusting a number the
  client was supposed to derive.

Neither is right. The derivation is a first-class property of the action, the
renderer should show it **live but locked**, and the server must recompute it
**authoritatively** and never trust the client's copy. The reactive engine
already recomputes on every `set<>` ([bridge.md](../spec/bridge.md)); what is
missing is a way to *declare* the derivation so the schema exposes it and both
ends compute the same value.

## Goal

Let an action declare that a field is **computed** from a pure function of its
sibling fields. From that single declaration:

1. `schemaJson<A>()` emits `x-computed` (naming the inputs) and `x-readonly` on
   the field, so a renderer greys/locks it and never submits it as user input;
2. the reactive `set<>` path recomputes it live client-side, for display; and
3. the **server recomputes it authoritatively** on dispatch, overwriting whatever
   arrived on the wire — the computed field is never trusted from the client.

Where the computation is numeric it reuses the exact `Rational`/`Quantity`
arithmetic ([rational.md](../spec/rational.md), [quantity_type.md](../spec/quantity_type.md))
already in the library, so the client's displayed value and the server's stored
value are identical to the last digit.

## Design

### Declaring a computed field

An action opts in with a `static constexpr` map from a computed member to a pure
function of the action, declared next to `optionalFields`
([forms.md](../spec/forms.md)):

```cpp
struct LineItem {
    Quantity<Units, 3> qty;
    Money price;
    Money total;   // computed — not user-entered

    // NEW — proposed. One declaration; drives schema + client + server.
    static constexpr auto computedFields = morph::forms::computeList(
        computed(&LineItem::total,                       // the derived member
                 { &LineItem::qty, &LineItem::price },   // its declared inputs
                 [](const LineItem& s) { return s.qty * s.price; })  // pure fn
    );

    [[nodiscard]] bool validate() const { return morph::forms::allRequiredEngaged(*this); }
};
```

- **`computed(dst, {inputs...}, fn)`** (NEW) binds a destination member, its input
  members, and a **pure** derivation `fn(const A&) -> ValueOfDst`. Members are
  pointer-to-member NTTPs recovered via `MemberPointerTraits`
  ([bridge.md](../spec/bridge.md)), so a renamed field is a compile error and the
  input list is type-checked.
- **`computeList(...)`** composes the entries into a `ComputeList<...>` value the
  framework detects with a `HasComputedFields<A>` concept (NEW), mirroring
  `HasOptionalFields<A>` ([forms.md](../spec/forms.md)).
- **`recomputeAll<A>(action)`** (NEW) is the single evaluator: it applies every
  entry's `fn` and writes the result into the destination member in place. This is
  the exact same function the client and the server call — no second copy of the
  arithmetic.

The derivation `fn` must be **pure** (a function of the action's fields only, no
side effects, no external state). That is the author's contract; the framework
cannot check it. Anything impure — a rate looked up from model state, a
server-only computation — is **not** a computed field and stays in the model's
`execute` (see Non-goals).

### Numeric derivations reuse exact `Quantity`/`Rational` arithmetic

When the inputs and destination are `Quantity`, `fn` is written in ordinary
`Quantity` arithmetic — `s.qty * s.price` uses `Quantity::operator*`, whose result
unit is deduced from the operands ([quantity_type.md](../spec/quantity_type.md))
and whose value is an exact `math::Rational` ([rational.md](../spec/rational.md)).
Before evaluation, `reconcileDeclaredPrecision` ([forms.md](../spec/forms.md))
has already normalised the inputs to their declared precision, and the result is
retagged to the destination field's declared precision afterward. So the client's
displayed total and the server's stored total are computed from identical inputs
with identical rounding — **no floating-point drift**, matching the exactness the
unit-conversion path already guarantees.

An empty (unengaged) input propagates: if any declared input has
`hasValue() == false`, the destination is left **unengaged** rather than computed
from a missing operand, consistent with how `allRequiredEngaged` treats
empty-capable fields ([forms.md](../spec/forms.md)).

### Schema emission — `x-computed` and `x-readonly`

`mergeSchemaExtras` ([forms.md](../spec/forms.md)) gains a step that walks
`A::computedFields` and patches each destination **property node** (sibling of its
`$ref`, exactly like `x-order`) with `x-computed` and `x-readonly`. The property
still appears in the schema so the renderer can display it; the annotations tell
the renderer it is derived and not editable:

```json
"total": {
  "$ref": "#/$defs/Money",
  "x-order": 2,
  "x-readonly": true,
  "x-computed": { "inputs": ["qty", "price"] }
}
```

A computed field is **excluded from the synthesised `required` array**: it is not
something the user must fill, so requiredness does not apply. (`recomputeAll`
engages it, or leaves it empty when an input is empty.) Input names in
`x-computed` are the **wire (JSON) field names**, resolved from the
pointer-to-member the same way `x-order` is derived, so a renderer that wants to
recompute optimistically knows which sibling changes should trigger a redisplay.

New keys this spec adds to the [forms.md](../spec/forms.md) renderer-contract
table (all additive, all optional):

| Key | Where | JSON type | Meaning / renderer obligation |
|---|---|---|---|
| `x-readonly` | property node (sibling of `$ref`) | boolean | The field is display-only; the renderer must render it disabled/greyed and **must not** include it as an editable input. Emitted `true` on computed fields (and reusable by [gui_field_metadata.md](gui_field_metadata.md) for author-declared read-only fields). Absent ⇒ editable, as today. |
| `x-computed` | property node (sibling of `$ref`) | object | Marks the field as derived. Present ⇒ the renderer shows the value but never lets the user edit it, and should refresh it when a listed input changes. Absent ⇒ an ordinary field. |
| ↳ `inputs` | `x-computed` object | array of strings | Wire field names of the sibling fields the value derives from, in declaration order. Advisory to the renderer (it may recompute optimistically or just redisplay what the server returns); **authoritative computation is the server's**. |

The derivation **function itself is not serialised** — `x-computed` names the
*inputs*, not the formula. A renderer that wants a live client-side value calls
`recomputeAll` through the reactive engine (below) rather than reconstructing the
formula from the schema; a renderer that does not simply displays the
server-returned value. Either way the *authority* is the server's recomputation.

### Client-side: live recompute on the reactive path

On the `set<>` reactive draft path ([bridge.md](../spec/bridge.md)), each accepted
field update runs `recomputeAll` on the draft snapshot **before** the readiness
check and fire. Because `set<>` already re-fires on every ready patch with
coalescing ([bridge.md](../spec/bridge.md)), the computed field is refreshed live
as the user edits its inputs, with no extra machinery — the engine "already
recomputes on `set<>`," and this step is what makes the derived value part of that
recomputation. A computed member is never a `set<>` target itself (it has no
editable widget); an attempt to `set<>` it is meaningless and simply overwritten
by the next `recomputeAll`.

### Server-side: authoritative recompute, client value distrusted

The crux: the server **never trusts** a client-sent computed value. On the
dispatcher path, immediately after `fromJson` and `reconcileDeclaredPrecision`
and before the validator/`Model::execute` ([validation.md](validation.md),
[bridge.md](../spec/bridge.md)), the runner calls `recomputeAll<A>(action)`,
**overwriting** every computed member from its declared inputs. Whatever the wire
carried in `total` is discarded and replaced by `qty * price` computed from the
(reconciled) inputs the server received. Consequences:

- A hostile or buggy client that submits a tampered `total` cannot influence the
  stored value — the server derives it, exactly as it derives declared precision.
- The value the validator and `Model::execute` see is the authoritative one, so a
  cross-field rule ([gui_cross_field_rules.md](gui_cross_field_rules.md)) over a
  computed field evaluates on the server's own number, not the client's.
- It is a **no-op** for actions with no `computedFields` — zero behaviour change,
  backward compatible — mirroring how `reconcileDeclaredPrecision` no-ops for
  actions with no `Quantity` members ([forms.md](../spec/forms.md)).

Because the same `recomputeAll` runs on both ends over inputs normalised the same
way, the field the client shows and the field the server stores agree by
construction — a single-source-of-truth derivation, the computed-field analogue
of the single rule declaration in [gui_cross_field_rules.md](gui_cross_field_rules.md).

## Additivity and renderer fallback

`x-computed` and `x-readonly` are additive, optional `x-*` keys, consistent with
the unversioned-schema stance of [forms.md](../spec/forms.md) and
[gui_overview.md](gui_overview.md). An action that declares no `computedFields`
emits neither key and behaves exactly as today. A renderer that ignores them
still produces a usable form — it just renders the computed field as an ordinary
(editable) input; any value the user types there is **harmlessly discarded**,
because the server recomputes it regardless. The correctness floor (the stored
value is the true derivation) never depends on the client honouring `x-readonly`.

## Non-goals

- **Not impure or model-dependent computation.** `fn` is a pure function of the
  action's own fields only. A value that needs model state, a database lookup, or
  the current time is computed in the model's `execute`, not declared here.
- **Not nested / cross-action derivation.** Like all of
  [forms.md](../spec/forms.md), inputs and destinations are an action's own flat,
  top-level members; a computed field cannot draw from a sibling action or a
  sub-member of a nested aggregate.
- **Not a formula language on the wire.** `x-computed` names inputs, not an
  expression; there is no client-evaluable formula string. Renderers either call
  `recomputeAll` (native) or display the server's result. This keeps the wire
  free of an interpreter and avoids a second, drift-prone copy of the arithmetic.
- **Not validation.** A computed field's requiredness does not apply (it is
  excluded from `required`); rules *about* a computed value belong to
  [gui_cross_field_rules.md](gui_cross_field_rules.md), evaluated after
  `recomputeAll`.
- **No localisation.** `x-computed`/`x-readonly` carry structure, not display
  text, for the same reason the schema is un-localised ([forms.md](../spec/forms.md)).

## Testing (planned)

- `computed(&total, {qty, price}, qty*price)`: `schemaJson<A>()` emits
  `x-readonly: true` and `x-computed.inputs == ["qty","price"]` on `total`, and
  `total` is **absent** from `required`.
- `recomputeAll` writes the exact `Quantity`/`Rational` product; an empty input
  leaves the destination unengaged; the result is retagged to the destination's
  declared precision.
- Client reactive path: `set<&qty>`/`set<&price>` refresh `total` live via the
  existing coalescing fire ([bridge.md](../spec/bridge.md)).
- **Server distrust:** an envelope carrying a tampered `total` is overwritten by
  `recomputeAll` in the dispatcher runner before `Model::execute` on
  `SimulatedRemoteBackend`, the Qt WebSocket transport, and `LocalBackend`; the
  stored value equals the server-derived value regardless of the wire value.
- Both ends compute the identical value from inputs reconciled to declared
  precision (no floating-point drift).
- An action with **no** `computedFields`: no `x-computed`/`x-readonly` emitted,
  `recomputeAll` is a no-op, dispatch unchanged (backward compatibility).
- A renderer ignoring `x-readonly` renders the field editable; a user-typed value
  is discarded server-side.

## Cross-references

- [gui_overview.md](gui_overview.md) — the umbrella program; this is its Tier-1
  computed-fields feature.
- [bridge.md](../spec/bridge.md) — the reactive `subscribe`/`set<>`/`tryFireImpl`
  draft path (with coalescing) the live client recompute rides on, and
  `MemberPointerTraits` used to name the destination and inputs.
- [forms.md](../spec/forms.md) — `mergeSchemaExtras`, the property-node `x-*`
  placement, `required` derivation (computed fields are excluded),
  `optionalFields`, and `reconcileDeclaredPrecision` (the normalisation both
  recomputes build on); the renderer-contract table this extends.
- [validation.md](validation.md) — the dispatcher runner where the authoritative
  server-side `recomputeAll` slots in, alongside precision reconciliation and the
  `ready()` check.
- [quantity_type.md](../spec/quantity_type.md) — `Quantity` arithmetic
  (`operator*`/`operator/`, result-unit deduction) numeric derivations use.
- [rational.md](../spec/rational.md) — the exact `Rational` payload that makes the
  client-displayed and server-stored values bit-identical.
- [gui_cross_field_rules.md](gui_cross_field_rules.md) — rules that may reference a
  computed field, evaluated on the server's authoritative value.
- [gui_field_metadata.md](gui_field_metadata.md) — author-declared `x-readonly`
  for non-computed fields reuses the same key.
- [todo.md](../todo.md) — execution order within the GUI program.

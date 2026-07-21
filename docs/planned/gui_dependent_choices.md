# Dependent (cascading) `Choice` options (planned)

> **Status: planned — not yet implemented.** This spec is part of the GUI
> enhancement program ([gui_overview.md](gui_overview.md), Tier 1). It extends the
> `Choice` type of [choice.md](../spec/forms/choice.md) and the `x-option*` schema
> vocabulary of [forms.md](../spec/forms/forms.md). It describes the intended behavior;
> the code does not implement it yet. See [todo.md](../todo.md).

## The gap

A `Choice` field today sources its options by executing a named action **with an
empty body** ([choice.md](../spec/forms/choice.md)): the renderer calls
`optionsAction()` with no arguments, reads the result rows, and offers them.
That makes every `Choice` independent — its option set is a constant of the form,
fetched once.

But real forms have **cascading picklists**: the list of *cities* depends on the
selected *country*; the list of *sub-accounts* depends on the selected *account*;
the *units* offered depend on the chosen *material*. The child list is not a
constant — it is a function of a sibling field's current value. With the
empty-body contract there is no way to pass the parent's value to the options
action, so the framework cannot express a dependent list at all. Authors fall
back to fetching every option unfiltered and filtering client-side (leaking the
full set, and impossible when the child set is large or access-controlled), or to
hand-written renderer glue outside the schema.

## Goal

Extend `Choice` so its options action can **receive one or more sibling field
values as parameters**, and emit that dependency in the schema
(`x-optionsDependsOn`) so a renderer:

1. calls the options action with the parent field values as its body (instead of
   an empty body), and
2. **re-fetches** the child options whenever a parent field changes, clearing a
   now-invalid child selection.

The dependency is declared once in the `Choice` type; the schema carries it to
the renderer additively, and everything about how options are *fetched* (a
registered action executed over the same wire) is unchanged from
[choice.md](../spec/forms/choice.md).

## Design

### Extending `Choice` with a parent dependency

`Choice`'s options-metadata NTTPs ([choice.md](../spec/forms/choice.md)) gain an
optional **dependency list**: the wire field names of the sibling fields whose
values parameterise the options action. Because a `Choice` cannot name its
siblings by pointer-to-member (it does not know the enclosing action type), the
dependency is expressed as `FixedString` field names — the same NTTP vehicle that
already carries `OptionsAction`/`ValueField`/`LabelField`
([choice.md](../spec/forms/choice.md)):

```cpp
// NEW — proposed extension in namespace morph::forms (choice.hpp)
template <typename T, FixedString OptionsAction,
          FixedString ValueField = "id", FixedString LabelField = "name",
          FixedString... DependsOn>                 // NEW: parent field names
struct Choice {
    std::optional<T> value;
    // ...
    // NEW accessor, alongside optionsAction()/valueField()/labelField():
    static constexpr auto optionsDependsOn() noexcept;  // the DependsOn names
};
```

Usage — cities depend on the sibling `country` field:

```cpp
struct ShippingAddress {
    morph::forms::Choice<std::int64_t, "ListCountries"> country;
    morph::forms::Choice<std::int64_t, "ListCities", "id", "name", "country">
        city;   // options depend on the sibling "country" value
};
```

A `Choice` with no `DependsOn` parameters is exactly today's independent `Choice`
— **the extension is a defaulted variadic tail, source-compatible with every
existing `Choice<...>`.** `optionsDependsOn()` returns an empty pack for them, and
nothing in the schema or the fetch path changes.

Adding the variadic tail requires the two companion specialisations that pattern-
match the `Choice` parameter list to be generalised in lockstep: `IsChoice`
([choice.md](../spec/forms/choice.md)) and the `glz::meta<Choice<...>>` serialisation
specialisation (`choice.hpp`) are both written today against the exact
four-parameter `Choice<T, OptionsAction, ValueField, LabelField>`, so each must
gain the same `FixedString... DependsOn` pack to keep matching. The change is
mechanical and preserves the emitted wire form (`glz::meta` still maps only
`&Choice::value`), so a `Choice` with an empty pack serialises byte-for-byte as
today.

The `DependsOn` names are **wire (JSON) field names of sibling fields in the same
action** — the same class of unchecked string the existing
`ValueField`/`LabelField` already are ([choice.md](../spec/forms/choice.md) "Author's
obligations"): a typo or a renamed sibling compiles cleanly and fails only at
runtime when the options action receives an unexpected body. This is an inherent
consequence of `Choice` not knowing its enclosing type; it is called out as an
author's obligation below, not hidden.

### The options-action request carries parent values

Today the renderer calls the options action with an **empty body**. For a
dependent `Choice` it instead sends a JSON object whose keys are the `DependsOn`
field names and whose values are the **current values** of those sibling fields:

```json
// renderer's request body to "ListCities" when country=42 is selected
{ "country": 42 }
```

The options action is therefore an **ordinary registered action whose body is the
parent selection** — no new dispatch mechanism, exactly as [choice.md](../spec/forms/choice.md)
reuses the action wire for the empty-body case. It returns the same
`{valueField, labelField, …}` rows as before, now filtered to the parent.

- If **any** parent in `DependsOn` is currently unengaged, the child list is
  **not fetched**; the renderer shows the child disabled/empty until every parent
  it depends on has a value. (This mirrors the empty-input propagation of
  [forms.md](../spec/forms/forms.md)'s computed fields — see its "Computed
  fields" section.)
- When a parent value **changes**, the renderer re-fetches with the new body and
  **clears** any existing child selection that is not present in the new result —
  closing (client-side) the staleness that [choice.md](../spec/forms/choice.md) Failure
  modes describe for the independent case. Membership is still not *enforced*
  server-side (see Non-goals).

### Schema emission — `x-optionsDependsOn`

`mergeSchemaExtras` ([forms.md](../spec/forms/forms.md)) already reads a `Choice`
property's compile-time metadata to emit `x-optionsAction` / `x-optionValue` /
`x-optionLabel` on the property node ([choice.md](../spec/forms/choice.md)). It gains
one more read: when `optionsDependsOn()` is non-empty it emits
`x-optionsDependsOn` on the same property node (sibling of the `$ref`):

```json
"city": {
  "$ref": "#/$defs/Choice",
  "x-order": 1,
  "x-optionsAction": "ListCities",
  "x-optionValue": "id",
  "x-optionLabel": "name",
  "x-optionsDependsOn": ["country"]
}
```

New key this spec adds to the [forms.md](../spec/forms/forms.md) renderer-contract
table (additive, optional):

| Key | Where | JSON type | Meaning / renderer obligation |
|---|---|---|---|
| `x-optionsDependsOn` | property node (sibling of `$ref`) | array of strings | Wire field names of sibling fields whose current values parameterise this field's options action. The renderer sends `{name: value, …}` as the options-action request body (instead of an empty body), and **re-fetches** the options — clearing a now-invalid selection — whenever any listed field changes. **Omitted entirely** when the `Choice` declares no dependency (then the options action is called with an empty body, exactly as [choice.md](../spec/forms/choice.md) specifies today). |

The dependency names are the parent fields' **wire names**, matching the property
keys a renderer already indexes (the same convention as `x-computed.inputs` in
[forms.md](../spec/forms/forms.md)'s computed fields and `fields` in
[forms.md](../spec/forms/forms.md)'s cross-field rules), so the renderer
resolves each parent to a property it is already rendering.

### The options action's own schema drives its request body

Because the options action is a normal registered action, its **own**
`schemaJson` describes the body the renderer must send: a `ListCities` action
declared as `struct ListCities { std::int64_t country = 0; };` has a `country`
field, and `x-optionsDependsOn: ["country"]` on the parent tells the renderer to
fill exactly that field from the sibling. The two must agree — the `DependsOn`
names must match the options action's input field names — which is the natural
extension of the existing obligation that `ValueField`/`LabelField` match the
options action's *result* field names ([choice.md](../spec/forms/choice.md)).

## Additivity and renderer fallback

`x-optionsDependsOn` is an additive, optional `x-*` key, consistent with the
unversioned-schema stance of [forms.md](../spec/forms/forms.md) and
[gui_overview.md](gui_overview.md), and the `Choice` change is a defaulted
variadic tail that leaves every existing `Choice<...>` type and its emitted schema
byte-for-byte unchanged. A renderer that does not understand
`x-optionsDependsOn` degrades predictably: it falls back to the base `Choice`
contract and calls the options action with an **empty body**
([choice.md](../spec/forms/choice.md)). The child list is then unfiltered (or empty, if
the options action requires the parent field) — a reduced affordance, not a
broken form — exactly the "ignore an `x-*` key and lose only that affordance"
guarantee [forms.md](../spec/forms/forms.md) makes.

## Non-goals

- **No server-side option-membership enforcement.** As in
  [choice.md](../spec/forms/choice.md), the wire value is a bare nullable `T` and the
  server does not check that a submitted child value belongs to the parent's
  current list. `x-optionsDependsOn` improves the *client* experience (correct
  fetch, stale-selection clearing); a model that must reject an inconsistent
  parent/child pair does so in its `execute` (or via a cross-field rule,
  [forms.md](../spec/forms/forms.md) (implemented), that both fields be
  engaged — not that they are mutually consistent, which the closed vocabulary
  cannot express).
- **Not a caching or debounce policy.** *When* to re-fetch (immediately on change,
  debounced, cached per parent value) is a renderer concern; the schema states the
  dependency, not the fetch strategy.
- **Parents are flat siblings only.** A `DependsOn` name must resolve to a
  top-level sibling field of the same action, consistent with the flat-actions
  scope of [forms.md](../spec/forms/forms.md); it cannot reach into a nested aggregate
  or another action.
- **The dependency is one-directional.** `city` depends on `country`; selecting a
  city does not filter countries. Multi-way constraint solving between picklists
  is out of scope.
- **No compile-time link to the options action's input schema.** Like the existing
  `OptionsAction`/`ValueField`/`LabelField` strings, `DependsOn` names are opaque
  NTTPs resolved at runtime; a mismatch with the options action's actual input
  fields compiles cleanly and fails only when a client fetches.

## Author's obligations

A dependent `Choice` adds to the three obligations [choice.md](../spec/forms/choice.md)
already lists:

- **Each `DependsOn` name must be a real sibling field's wire name** in the
  enclosing action. The renderer reads that sibling's current value to build the
  options request; a name that matches no property yields a missing key in the
  request body.
- **The options action must accept those names as input fields.** `ListCities`
  must have an input field literally named `country` (its wire name) and must
  return filtered rows for it. An options action that ignores the body silently
  returns the unfiltered list — no error, just a non-cascading picklist.
- **The parent's value type must match the options action's input field type.**
  The renderer forwards the parent `Choice`'s underlying `T` as-is; a type
  mismatch surfaces only as a decode failure or empty result at fetch time.

## Testing (planned)

- A `Choice<..., "ListCities", "id", "name", "country">` field: `schemaJson<A>()`
  emits `x-optionsDependsOn: ["country"]` on the `city` property alongside its
  `x-optionsAction`/`x-optionValue`/`x-optionLabel`.
- A `Choice` with **no** `DependsOn`: `optionsDependsOn()` is empty, no
  `x-optionsDependsOn` key is emitted, and the emitted schema is identical to
  today's independent `Choice` (backward compatibility).
- Every pre-existing `Choice<T, "A">` / `Choice<T, "A", "v", "l">` still compiles
  unchanged (defaulted variadic tail).
- Renderer contract (illustrative, via the reference Qt/QML renderer): with a
  parent engaged, the options request body is `{parent: value}`; with a parent
  unengaged, the child is not fetched; changing the parent re-fetches and clears a
  child selection absent from the new result.
- A renderer that ignores `x-optionsDependsOn` falls back to an empty-body fetch
  and still renders a (unfiltered) usable form.

## Cross-references

- [gui_overview.md](gui_overview.md) — the umbrella program; this is its Tier-1
  dependent-choices feature, the generalisation of the `Choice` pattern the
  overview names.
- [choice.md](../spec/forms/choice.md) — the base `Choice`/`FixedString` design this
  extends: the empty-body options fetch, `optionsAction()`/`valueField()`/
  `labelField()`, the `x-option*` emission, the unchecked-string author
  obligations, and the staleness Failure mode this narrows client-side.
- [forms.md](../spec/forms/forms.md) — `mergeSchemaExtras` and the property-node `x-*`
  placement where `x-optionsDependsOn` is emitted; the renderer-contract table
  this key joins; the flat-actions scope. Also its cross-field rule vocabulary
  (implemented) — a rule may require a dependent `Choice` be engaged;
  parent/child *consistency* is a model concern the closed rule vocabulary does
  not cover.
- [forms.md](../spec/forms/forms.md) — computed fields (`computed`/`computeList`/
  `recomputeAll`, now implemented): shares the "sibling field names as wire
  strings" and empty-input-propagation conventions.
- [bridge.md](../spec/core/bridge.md) — the action-execute path the renderer uses to
  fetch options is the same one every other action uses.
- [todo.md](../todo.md) — execution order within the GUI program.

# `morph::forms::SectionSet` — unordered sections

`SectionSet<Model, Sections...>` drives N independently editable action drafts
on one screen. Each section accumulates its own draft through `set<>` and
dispatches through `BridgeHandler<Model>::execute<A>()` as soon as its own
`ActionValidator<A>::ready` accepts it. There is no active section, no index,
and no sequence. Like [workflows_navigation.md](workflows_navigation.md)'s
wizards, this is additive metadata and client-side bookkeeping over the
dispatch path in [../core/bridge.md](../core/bridge.md) — no new wire format,
no new execution mode.

## Contents

- [The gap this closes](#the-gap-this-closes)
- [The `s-*` section-group document](#the-s--section-group-document)
- [C++ descriptors](#c-descriptors)
- [`SectionSet<Model, Sections...>`](#sectionsetmodel-sections)
- [Prefill is a declaration, not a write](#prefill-is-a-declaration-not-a-write)
- [Concurrency and lifetime](#concurrency-and-lifetime)
- [Compile-time contract](#compile-time-contract)
- [Design decisions](#design-decisions)
- [Limitations](#limitations)
- [Testing](#testing)
- [Cross-references](#cross-references)

## The gap this closes

`FlowSession` has exactly one current step, and `FlowSession::set<>` throws
`std::logic_error` on a field belonging to any other. That is right for a
wizard and wrong for a screen whose blocks have no order — a settings page, a
tab strip, a column of cards — where a user may edit the third block first and
never touch the second. Before this layer such a screen either hand-wired one
`BridgeHandler::execute` call site per block, re-implementing draft
accumulation and the readiness gate each time, or misused a wizard and got a
`logic_error` for editing its own form out of order (morph#513).

`SectionSet` keeps everything `FlowSession` does per action — per-action draft
accumulation, the readiness gate, result capture, error routing, the callback
lifetime gate — and drops only the position.

Choose `FlowSession` when order carries meaning (a later step needs an earlier
step's result, or the user must not skip ahead) and `SectionSet` when it does
not.

## The `s-*` section-group document

`sectionGroupSchemaJson<G>()` emits a small JSON document alongside each
section's ordinary action schema ([forms.md](forms.md)):

```json
{
  "s-id": "AccountSettings",
  "s-title": "Account settings",
  "s-sections": [
    { "action": "UpdateProfile", "title": "Profile" },
    { "action": "UpdatePrefs", "title": "Preferences",
      "prefill": { "profileId": "UpdateProfile.id" } }
  ]
}
```

| Key | Where | JSON type | Meaning |
|---|---|---|---|
| `s-id` | top-level | string | The group's registered type-id (`SectionGroupTraits<G>::typeId()`). |
| `s-title` | top-level | string | Human title for the whole group. |
| `s-sections` | top-level | array | The group's sections. Array order is declaration order and carries no meaning. |
| ↳ `action` | section | string | The section's registered action type-id (`ActionTraits<A>::typeId()`). |
| ↳ `title` | section | string | Human title for the section (tab label, card header). |
| ↳ `prefill` | section | object | Field name → `"<ActionTypeId>.<field>"` source path. Present only when the section declares at least one `Bind`. |

There is deliberately **no index or order key**. A renderer arranges the
sections itself — as tabs, as a grid, collapsed, in a user-chosen order — and
a position in the wire format would suggest a sequence a section group does not
have. That is the one thing distinguishing this document from `w-*`, so
emitting an index would erase the distinction the type exists to make.

`s-sections` is a JSON array only because JSON has no unordered collection that
also preserves duplicates-free keys usefully; consumers must not read meaning
into the order.

## C++ descriptors

```cpp
struct UpdateProfile { std::string name; bool validate() const { return !name.empty(); } };
struct UpdatePrefs   { std::int64_t profileId; std::string theme;
                       bool validate() const { return !theme.empty(); } };

BRIDGE_REGISTER_ACTION(SettingsModel, UpdateProfile, "UpdateProfile")
BRIDGE_REGISTER_ACTION(SettingsModel, UpdatePrefs,   "UpdatePrefs")

using ProfileSection = morph::forms::Section<UpdateProfile, "Profile">;
using PrefsSection   = morph::forms::Section<UpdatePrefs, "Preferences",
                                             morph::forms::Bind<"profileId", "UpdateProfile.id">>;

using AccountSettings = morph::forms::SectionGroup<"Account settings",
                                                   ProfileSection, PrefsSection>;
BRIDGE_REGISTER_SECTION_GROUP(AccountSettings, "AccountSettings")
```

| Type | Members | Meaning |
|---|---|---|
| `Section<Action, Title, Binds...>` | `action`, `binds`, `title()` | One section: a registered action, a display title, zero or more prefill declarations. |
| `SectionGroup<Title, Sections...>` | `sections`, `title()` | A group of sections sharing one screen. |
| `SectionGroupTraits<G>` | `typeId()` | Maps a group type to its stable string id. Specialise via `BRIDGE_REGISTER_SECTION_GROUP`; the default is a forward declaration, so using it unregistered is an incomplete-type error. |
| `Bind<Field, Path>` | `field()`, `path()` | Shared with `morph::flows` — see [Prefill is a declaration, not a write](#prefill-is-a-declaration-not-a-write). |

`Bind` and the declaration-walking helpers live in
`morph/forms/detail/session_common.hpp`, shared by both session types.
`morph::flows::Bind` remains as an alias, since that is the name existing
consumers write.

Registration is metadata only. `BRIDGE_REGISTER_SECTION_GROUP` specialises a
traits template and registers nothing with the dispatcher — exactly as
`BRIDGE_REGISTER_WIZARD` does, and for the same reason ([../core/registry.md](../core/registry.md)).

## `SectionSet<Model, Sections...>`

```cpp
morph::forms::SectionSet<SettingsModel, ProfileSection, PrefsSection> sections{handler};

sections.set<&UpdatePrefs::theme>("dark");   // fires UpdatePrefs -- no ordering
sections.set<&UpdateProfile::name>("ada");   // fires UpdateProfile
```

| Member | Contract |
|---|---|
| `SectionSet(handler, onError = nullptr)` | `handler` must outlive the set. `onError` receives every failed dispatch; when absent, failures are logged via `morph::log::logError` and never escape the completion. |
| `set<FieldPtr>(value)` | Assigns one field of its section's draft, then dispatches that section if `ActionValidator<A>::ready` now accepts the draft. The field's action need only be *one of* the declared sections — there is no current one. |
| `reset<A>()` | Clears section `A`'s draft to a default-constructed action. Touches no other section and dispatches nothing. Values already in `resolved()` stay: they describe what the model was told, which resetting an editor does not undo. |
| `draft<A>()` | Returns a copy of section `A`'s draft, taken under the lock. |
| `resolved(path)` | Returns the JSON-encoded value captured at `"<ActionTypeId>.<field>"`, or `std::nullopt` when that path was never captured. |

`SectionSet` is neither copyable nor movable: its callbacks capture `this`.

**No latch.** A ready section re-fires on *every* subsequent `set<>`, matching
`FlowSession`. A caller wanting one request per pause debounces on its own
side, where it knows what a pause means for its input widget. Coalescing here
would have to guess.

**A ready draft is dispatched, a not-ready one is not sent at all.** The gate
is not a correctness backstop — `BridgeHandler::execute` enforces
`ActionValidator` on its own path ([../core/bridge.md](../core/bridge.md)), so
an ungated draft would come back as a validation failure rather than execute.
The gate exists to avoid a round trip that can only fail, and its absence is
observable as spurious `onError` calls.

## Prefill is a declaration, not a write

A `Bind` on a section says *where a field's initial value comes from*. Nothing
in the framework assigns it. `sectionGroupSchemaJson` emits it under `prefill`
for a renderer to act on, and `resolved(path)` exposes the captured values a
renderer resolves it against. `SectionSet` never writes a bound field into a
draft on its own.

This is exact parity with `FlowSession`, which also emits `prefill` metadata
and captures values without ever assigning a bound field. A section set had a
stronger temptation to differ — with no ordering, "fill in the dependent field
the moment the source resolves" is a coherent design — but a reactive write
would silently overwrite a value the user had already typed into that field,
with no signal that it happened, and only for bound fields. The renderer knows
whether its widget is dirty; the session does not.

Capture happens on success only, in the dispatch's completion:

- the submitted draft's fields are recorded first, then the result's fields,
  so a result field wins on a name collision — the result is what the model
  actually settled on, and is therefore what a dependent field should show;
- keys are `"<ActionTypeId>.<field>"`, the same vocabulary `Bind::path()` uses;
- values are JSON-encoded, so `resolved("Profile.name")` yields `"\"ada\""`
  and `resolved("Profile.id")` yields `"3"`.

A failed dispatch captures nothing.

## Concurrency and lifetime

One mutex guards the drafts and the captured values. `set<>` takes it to
assign the field and snapshot the draft, then **releases it before
dispatching**, so a slow dispatch of one section cannot block an edit to
another.

Dispatch continuations run on whatever executor resolves the underlying
`BridgeHandler` completion, not necessarily the thread that called `set<>` —
see [../core/bridge.md](../core/bridge.md)'s executor/callback model.

Every continuation is gated on one `morph::async::CallbackScope`
([../core/callback_scope.md](../core/callback_scope.md)), declared last so it
is the first member destroyed, and stopped explicitly at the top of
`~SectionSet`. A completion resolving after the set is gone finds the token
stopped and returns without touching anything.

That covers a completion which has not yet started. It does not cover one
already past its token check: `requestStop()` does not wait, by design. So a
`SectionSet` may only be destroyed while a dispatch is outstanding if the
destroying thread is the one completions are delivered on — the ordinary case
for a UI-thread callback executor, and the boundary
[../core/callback_scope.md](../core/callback_scope.md) describes.

**Polling `resolved()` is not a substitute for that.** Capture publishes each
key as it writes it, so a value becoming visible means the completion has
started, not that it has finished. A caller that destroys a set on one thread
the moment a value appears on another is destroying it mid-callback. `tests/test_sections.cpp` demonstrates the safe shape: deliver the completions on the
thread that owns the set.

Unlike `FlowSession`, nothing here is keyed to a current position, so a reply
arriving late cannot be *stale*: there is no position for it to be stale
relative to. That is why `SectionSet` needs no equivalent of `FlowSession`'s
`_activeStep` guard.

## Compile-time contract

Two `static_assert`s, both on `SectionSet`:

```cpp
// Rejected: a group must have at least one section.
morph::forms::SectionSet<SettingsModel> empty{handler};

// Rejected: two sections of the same action would share one draft slot,
// so an edit to either would silently clobber the other.
morph::forms::SectionSet<SettingsModel, ProfileSection, ProfileSection> duplicate{handler};
```

and one on each of `set<>`, `reset<A>()` and `draft<A>()`:

```cpp
// Rejected: UnrelatedAction is not a section of this group.
sections.set<&UnrelatedAction::field>(1);
```

This is where `SectionSet` still refuses a field — but at compile time, on
membership, not at run time on position. `FlowSession`'s `std::logic_error`
has no counterpart: with no current step, there is no run-time state that can
make a member field wrong to set.

## Design decisions

- **A separate type rather than a `FlowSession` mode.** A flag ("unordered
  flow") would leave `advance()`, `back()`, `currentIndex()` and `finished()`
  on an object for which none of them mean anything, and every one of them
  would need a documented answer for the unordered case. A separate type has
  only the members that make sense.
- **Shared declaration vocabulary.** `Bind`, the tuple/pack walkers and the
  distinctness trait moved to `forms/detail/session_common.hpp` rather than
  being duplicated. They describe how a form session declares its units, which
  both types do identically; only the sequencing differs.
- **No index in the schema.** See [The `s-*` section-group document](#the-s--section-group-document).
- **No aggregate readiness and no "submit all".** Sections are independent by
  construction; a group-level submit would reintroduce a coordination point
  and raise questions this layer has no answer for (partial failure, ordering,
  atomicity). Cross-action atomicity belongs in the outbox
  ([../journal/journal.md](../journal/journal.md)), as it does for wizards.

## Limitations

- No renderer ships for `s-*` yet. `WizardView.qml` has no section-group
  counterpart in `src/qt/forms`; a host consuming the document builds its own
  layout for now.
- `resolved()` returns JSON-encoded strings, not typed values — the same shape
  `FlowSession::resolved` has, and the same caller-side decode.
- A section that fires repeatedly issues one dispatch per `set<>`; there is no
  in-flight coalescing or cancellation of a superseded request.
- Prefill is never applied by the framework, so a host that ignores the
  `prefill` metadata gets no prefilling at all.

## Testing

`tests/test_sections.cpp` (`[sections]`), nine cases:

- `sectionGroupSchemaJson` emits `s-id`, `s-title`, each section's `action`
  and `title`, `prefill` only where a `Bind` is declared, and no `index` key.
- Sections fire independently in any order — the morph#513 regression: the
  same edit sequence throws `std::logic_error` under `FlowSession`.
- A not-ready draft is not sent at all, observed through `onError` (a missing
  gate is visible as a spurious validation failure, not as a bad execution).
- An already-fired section fires again on the next edit (the no-latch rule).
- `reset<A>()` clears one section and leaves the others intact.
- A fired section's draft *and* result fields are resolvable, and a path
  belonging to an unfired section — or to no field — is not.
- A failing dispatch reaches `onError`, and a succeeding one does not.
- An unhandled failure logs instead of escaping, and the set survives it.
- Destroying the set with a dispatch genuinely in flight (a section whose
  model call blocks until the test releases it) delivers nothing afterwards.

Every other case delivers its completions on the test thread through a
`StepExecutor` and drains before the set leaves scope, for the reason
[Concurrency and lifetime](#concurrency-and-lifetime) gives.

## Cross-references

- [workflows_navigation.md](workflows_navigation.md) — `FlowSession` and the
  `w-*` document this layer is the unordered sibling of; the ordering
  constraint whose absence defines `SectionSet`.
- [forms.md](forms.md) — the per-action schema each section renders, and
  `FixedString`, which `Section`/`SectionGroup` titles and `Bind` paths use.
- [../core/bridge.md](../core/bridge.md) — `BridgeHandler::execute` and
  `ActionValidator::ready`, the dispatch path and readiness gate `SectionSet`
  reuses without extending.
- [../core/callback_scope.md](../core/callback_scope.md) —
  `morph::async::CallbackScope`, the gate that refuses a completion resolving
  after the set is destroyed.
- [../core/registry.md](../core/registry.md) — `ActionTraits::typeId()` and the
  metadata-only registration `BRIDGE_REGISTER_SECTION_GROUP` mirrors.
- [../journal/journal.md](../journal/journal.md) — where cross-action
  atomicity belongs; a section group deliberately does not provide it.

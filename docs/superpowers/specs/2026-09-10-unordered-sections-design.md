# Unordered sections — design

`morph::forms::SectionSet<Model, Sections...>` gives a screen N independently
editable action drafts, each gated on its own `ActionValidator<A>::ready` and
each dispatched through the handler's ordinary `execute<A>()`. No sequence, no
current step, no advance/back.

Closes [morph#513](https://github.com/LASTRADA-Software/morph/issues/513).

## Contents

- [The gap](#the-gap)
- [Headers](#headers)
- [Declaration layer](#declaration-layer)
- [SectionSet](#sectionset)
- [Prefill](#prefill)
- [Concurrency and lifetime](#concurrency-and-lifetime)
- [Schema document](#schema-document)
- [Testing](#testing)
- [Out of scope](#out-of-scope)

## The gap

`779bd8aa` removed the handler-side reactive draft (`BridgeHandler::set<&A::field>`,
`reset<A>`, the action-keyed `subscribe`) and named
`morph::flows::FlowSession` as its replacement. `FlowSession` serves a wizard:
one active step at a time, enforced at `flows.hpp:293`, which throws
`std::logic_error` for a field belonging to any other step.

That leaves the screen shape the removed mechanism also served — N sections a
user edits in any order, each with its own draft and its own readiness gate —
with no in-framework target. A consumer either reimplements draft accumulation
locally or restructures the screen into a wizard it is not.

`SectionSet` is the sibling for that shape.

## Headers

| File | Holds |
| --- | --- |
| `forms/detail/session_common.hpp` *(new)* | `Bind`, `forEachTupleElement`, `forPackElement`, `AllDistinct`, prefill-path resolution, schema-emission helper |
| `forms/flows.hpp` | `Wizard`, `WizardStep`, `FlowSession` — unchanged behaviour |
| `forms/sections.hpp` *(new)* | `Section`, `SectionGroup`, `SectionGroupTraits`, `BRIDGE_REGISTER_SECTION_GROUP`, `sectionGroupSchemaJson`, `SectionSet` |

The shared header is an extraction, not a rewrite: `flows.hpp` keeps every
public name it exports today and gains an include. Splitting it this way keeps
`flows.hpp` at its present size instead of doubling it, and gives `Bind` — which
both session types genuinely share — one home rather than two.

## Declaration layer

`Section` mirrors `WizardStep` structurally and is named for its own domain:

```cpp
template <typename Action, morph::forms::FixedString Title, typename... Binds>
struct Section {
    using action = Action;
    using binds = std::tuple<Binds...>;
    [[nodiscard]] static constexpr std::string_view title() noexcept;
};

template <morph::forms::FixedString Title, typename... Sections>
struct SectionGroup {
    using sections = std::tuple<Sections...>;
    [[nodiscard]] static constexpr std::string_view title() noexcept;
};
```

`SectionGroupTraits<G>` maps a group type to its string type-id, specialised via
`BRIDGE_REGISTER_SECTION_GROUP(G, "name")`. It exists for the same reason
`WizardTraits` does: `sectionGroupSchemaJson<G>()` needs a stable name for the
group in the emitted document, and deriving one from the C++ type would tie the
wire format to a mangled name.

The two trait structs are the only duplication against `flows.hpp`; everything
substantive lives in the shared header. Reusing `WizardStep` for an unordered
screen was rejected: a consumer declaring sections would write `WizardStep`,
which is misleading in exactly the place morph#513 says the model is wrong.

## SectionSet

```cpp
template <typename Model, typename... Sections>
class SectionSet {
    static_assert(sizeof...(Sections) > 0);
    static_assert(AllDistinct<typename Sections::action...>::value);

public:
    explicit SectionSet(BridgeHandler<Model>& handler,
                        std::function<void(std::exception_ptr)> onError = nullptr);
    ~SectionSet();                                    // requestStop() first

    template <auto FieldPtr> void set(ValueType value);
    template <typename A>    void reset();
    template <typename A>    [[nodiscard]] A draft() const;
    [[nodiscard]] std::optional<std::string> resolved(std::string_view path) const;
};
```

`set<>` keeps `FlowSession`'s compile-time check that the field's action belongs
to the set and drops the runtime current-step `throw`. It takes `_mtx`, writes
the field, copies the draft out, releases the lock, and dispatches through
`handler.execute<A>()` when `ActionValidator<A>::ready(draft)` holds — the same
sequence `FlowSession::set<>` runs, minus the step check.

**A ready section re-fires on every subsequent `set<>`.** There is no latch and
no fire-once-per-ready-transition rule. This matches `FlowSession`, which
documents that a caller wanting one request per pause debounces on its own side;
a section that silently stopped dispatching after its first success would be a
surprising rule to carry.

`reset<A>()` returns that section's draft to `A{}` and re-applies any prefill
already resolved for it. It touches no other section.

`draft<A>()` returns a snapshot for a renderer to display. It is a copy taken
under the lock, not a reference into live state.

## Prefill

A section declaring `Bind<&B::field, "A.result">` publishes a *declaration*, not
a write. This matches `FlowSession` exactly, and the parity is worth stating
because it is easy to assume otherwise: `binds` are consumed in exactly one
place in `flows.hpp` — emitted into the schema document under a `prefill` node
for a renderer. `FlowSession` never writes a prefill value into a draft.

`SectionSet` does the same two things:

- `sectionGroupSchemaJson<G>()` emits each section's binds under `prefill`, as
  `{ "<field>": "<ActionTypeId>.<field>" }`.
- On a successful result, the submitted draft's fields and then the result's
  fields are recorded into `_resolvedValues` under `"<ActionTypeId>.<field>"`,
  result fields winning on a name collision — the same order `FlowSession`
  records them in.
- `resolved(path)` returns a captured field's JSON-encoded value, or
  `std::nullopt` if that path was never captured.

The renderer decides what to do with a resolved value: it is the component that
knows whether a field the user has already edited should be overwritten, and
the framework has no basis for that judgement.

**No ordering is implied.** In a wizard a bind's source always precedes its
target, so `resolved` is populated by the time a step is entered. Here a bind
may name a section that has not fired, and `resolved` returns `std::nullopt`
for it — which is the same answer `FlowSession` gives for a path that was never
captured, so the accessor's contract is unchanged.

## Concurrency and lifetime

One `_mtx` guards `_drafts` and `_resolvedValues`. It is never held across
`handler.execute<A>()`, and the draft is copied out before dispatch, matching
`FlowSession`.

One `CallbackScope` gates every installed continuation, and the destructor calls
`requestStop()` explicitly as its first statement rather than leaving it to the
member's own destruction — the same reasoning `FlowSession`'s destructor
records, so a body that later grows a pumping call cannot deliver into a
half-dead session.

The guarantee has the same boundary as `FlowSession`'s and the same caveat
applies verbatim: destroying the session on the thread its continuations are
delivered on makes check-then-run atomic; destroying it from another thread is
advisory, and that caller owns its own synchronisation. See
`docs/spec/core/callback_scope.md`, "Boundary of the guarantee".

## Schema document

`sectionGroupSchemaJson<G>()` emits the group title and one entry per section
carrying its title, action type-id and declared binds. It mirrors
`wizardSchemaJson`'s shape with `s-` keys instead of `w-`, and deliberately
carries no index or order field: a renderer laying out sections chooses its own
arrangement, and emitting a position would imply a sequence the type does not
have.

## Testing

`tests/test_sections.cpp`. Every case is written to fail without the feature —
a suite that would pass either way measures nothing.

| # | Case | Fails without |
| --- | --- | --- |
| 1 | Edit section B, then A; both fire | The feature entirely — this is the shape `FlowSession` throws on |
| 2 | A not-ready draft does not dispatch; a ready one does | The readiness gate |
| 3 | Editing an already-fired section fires again | The no-latch rule |
| 4 | `reset<A>()` clears A, leaves B's draft intact | Per-section isolation |
| 5 | A fires → `resolved("A.field")` returns its value; a path whose section has not fired returns `nullopt` | Result capture and the resolved-values map |
| 6 | `onError` runs on dispatch failure; default path logs | Error routing |
| 7 | Destroying the set with a dispatch in flight delivers nothing | The `CallbackScope` gate |
| 8 | Schema carries each section's title and action id | Schema emission |
| 9 | Duplicate action types, and a field outside the set, are rejected | The two `static_assert`s |

Case 5 asserts both halves — a captured path and an uncaptured one — because a
test that only checked the captured case would pass against an implementation
that returned a value for everything.

Case 9 is compile-time. It is covered by a documented negative example rather
than a runtime assertion, since a `static_assert` that fires cannot also be
linked into the suite.

## Out of scope

- Changing `FlowSession`. Its single-active-step constraint is correct for a
  wizard and stays.
- In-flight coalescing. It was removed with the reactive draft in `779bd8aa`
  and is not reintroduced here; `FlowSession` does without it too.
- **Writing prefill values into drafts.** An earlier draft of this design had
  the framework write a bound field when its source fired. That is not parity
  with `FlowSession` — it is a mechanism that exists nowhere in the tree, and
  would need JSON-to-typed-field deserialization keyed by field name. Rejected
  in favour of matching the sibling type; if it is wanted later it is its own
  piece of work, and the renderer can do it today from `resolved()`.
- A QML renderer for section groups. The schema document is emitted; consuming
  it is a separate piece of work.

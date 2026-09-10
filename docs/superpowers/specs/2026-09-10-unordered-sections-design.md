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
- [Reactive prefill](#reactive-prefill)
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

## Reactive prefill

A section declaring `Bind<&B::field, "A.result">` is filled when `A` produces
its result, whenever that happens. `FlowSession` never faced this question —
its ordering guarantees the source has already fired. Without ordering, the
alternatives were to resolve lazily at read time (which pushes the work back
onto the consumer, the thing morph#513 objects to) or to require a fired source
(which reintroduces sequencing through the back door).

On a successful result the fields are recorded into `_resolvedValues` under
`"<ActionTypeId>.<field>"`, then every *other* section with a `Bind` naming one
of those paths has its draft field written.

**A prefill write never fires its section.** Only `set<>` does.

This is the one constraint chosen rather than requested, and it is load-bearing:
without it, `A` completing could cascade-fire `B`, and two sections bound to
each other would ping-pong. The cost is visible and accepted — a section made
ready purely by prefill waits for one user edit before dispatching.

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
| 5 | A fires → B's bound field fills, **and B does not fire** | The prefill-never-fires rule, in both directions |
| 6 | `onError` runs on dispatch failure; default path logs | Error routing |
| 7 | Destroying the set with a dispatch in flight delivers nothing | The `CallbackScope` gate |
| 8 | Schema carries each section's title and action id | Schema emission |
| 9 | Duplicate action types, and a field outside the set, are rejected | The two `static_assert`s |

Case 5 is the one worth writing carefully: it must assert both halves, because
a test that only checks the field filled would pass under a cascading
implementation.

Case 9 is compile-time. It is covered by a documented negative example rather
than a runtime assertion, since a `static_assert` that fires cannot also be
linked into the suite.

## Out of scope

- Changing `FlowSession`. Its single-active-step constraint is correct for a
  wizard and stays.
- In-flight coalescing. It was removed with the reactive draft in `779bd8aa`
  and is not reintroduced here; `FlowSession` does without it too.
- A QML renderer for section groups. The schema document is emitted; consuming
  it is a separate piece of work.

# The `Tagged` type — design

`morph::util::Tagged<T, Tag>` is an opaque, type-safe newtype for protocol
scalars: a pagination cursor, an event id, a job id, a bearer token — anything
that is, on the wire, a bare `T` (usually `std::string` or an integer) but
must not be interchangeable at the C++ level with a different scalar that
happens to share the same underlying type.

`Tagged<std::string, "UserId">` and `Tagged<std::string, "AccountId">` carry
the identical wire representation but are distinct C++ types: a function
expecting one does not accept the other, and neither is constructible from
the other. Every application built on the framework otherwise hand-rolls the
same wrapper shape per protocol scalar; `Tagged` is the framework-level,
day-one primitive for it.

## Contents

- [Identity via the tag](#identity-via-the-tag)
- [Construction and access](#construction-and-access)
- [Comparison](#comparison)
- [Wire and schema](#wire-and-schema)
- [Forms palette — `hasValue()`](#forms-palette--hasvalue)
- [Design decisions](#design-decisions)
- [Cross-references](#cross-references)
- [Limitations](#limitations)
- [Out of scope](#out-of-scope)

## Identity via the tag

`Tag` is a `morph::detail::FixedString` non-type template parameter — the
same structural string type `morph::forms::Choice` and
`morph::units::NamedQuantity` use (`morph::detail::FixedString`,
`include/morph/detail/fixed_string.hpp`; there is one definition, not
several look-alikes). Because the tag lives in the type itself:

- Two `Tagged<T, "X">` spelled identically in different translation units are
  one and the same type — no companion `enum` or registry is needed to keep
  tags distinct or to make ODR happy.
- `Tagged<T, "X">` and `Tagged<T, "Y">` are unrelated types even though they
  share `T`: neither converts to nor is constructible from the other
  (`Tagged`'s only converting constructor takes a `T`, not another `Tagged`
  specialization), so passing a `UserId` where an `AccountId` is expected is
  a compile error, not a runtime data bug.
- `tag()` returns the tag text (`Tag.view()`) for introspection or logging.

## Construction and access

| Member | Signature | Notes |
|---|---|---|
| default ctor | `constexpr Tagged() noexcept(...)` | Wraps a default-constructed `T{}`. Not "empty" in the `Quantity`/`Choice` sense — see *Forms palette* below. |
| value ctor | `constexpr explicit Tagged(T wrapped) noexcept(...)` | Wraps @p wrapped. `explicit`, so a bare `T` never silently becomes a `Tagged` at a call site — the wrapping is always visible in the code. |
| `get()` | `const T& get() const noexcept` | Named read access. |
| `operator*()` | `const T& operator*() const noexcept` | Unchecked access, mirroring `Quantity`'s and `Choice`'s `operator*` for a consistent shape across the framework's newtype family — there is nothing to check here (the value always exists), but the same spelling reads uniformly next to `*quantity` / `*choice`. |
| `tag()` | `static constexpr std::string_view tag() noexcept` | The compile-time tag text. |

## Comparison

`operator==` is defaulted (present only when `T` is
`std::equality_comparable`), and `operator<=>` is defaulted (present only
when `T` is `std::three_way_comparable`) — both compare on the wrapped value
alone. Comparison is only ever defined between two `Tagged` of the **same**
`T` and `Tag`; there is no cross-tag `==`/`<=>` overload, so `userId ==
accountId` fails to compile even when both wrap `std::string` — the same
protection the type provides at construction extends to comparison.

## Wire and schema

On the morph JSON wire a `Tagged<T, Tag>` is **exactly its `T` payload** —
`glz::meta<Tagged<T, Tag>>` maps the instance to the single member `value`,
so it reads and writes byte-for-byte like a bare `T` (a `Tagged<std::string,
"UserId">` writes as a JSON string, a `Tagged<std::int64_t, "EventSeq">` as a
JSON number). The tag never appears in the wire payload.

The schema tells a different, narrower story: `to_json_schema<Tagged<T,
Tag>>` delegates to `to_json_schema<T>` for the shape (`type`, numeric
bounds, and so on are identical to `T`'s own schema) but `glz::meta`'s `name`
is fixed to the tag text, so the schema's `title` carries the tag
(`"UserId"`, `"EventSeq"`) rather than `T`'s own type name. Two `Tagged<T,
Tag1>` / `Tagged<T, Tag2>` therefore produce structurally identical but
distinctly titled schema entries — a client-side codegen tool can still tell
them apart even though the wire shape is shared.

## Forms palette — `hasValue()`

`Tagged` is a **required** scalar, not an optionally-empty field like
`Quantity` / `Choice` / `Timestamp`: it always holds a `T` (default- or
value-constructed), never `std::nullopt`. Its `hasValue()` therefore always
returns `true` (`noexcept`), which satisfies `morph::forms::EmptyCapableField`
— the concept the forms rule vocabulary, `allRequiredEngaged`, and
`recomputeOne`'s per-input engagement check all key on (see
[forms.md](../forms/forms.md)).

Satisfying `EmptyCapableField` (rather than simply not satisfying it, as a
plain unwrapped scalar member would) means `Tagged` participates in the
palette the same way `Quantity`/`Choice`/`Timestamp` do — a rule can name a
`Tagged` field and `isEngaged()` resolves it via `hasValue()`, always `true`
— instead of being silently treated as "no empty state, so always counted
present" through the *other*, no-concept branch those helpers also support.
The visible behavior is the same (always engaged) either way; the difference
is that `Tagged` opts in explicitly rather than falling through the fallback
path, which matters if a future palette helper ever distinguishes "declares
no empty state" from "declares an empty state that never triggers."

A `Tagged` field is therefore always **required** wherever
`morph::forms::schemaJson` derives requiredness, exactly like a plain
non-optional scalar member — wrap the underlying `T` in `std::optional` at
the call site (`std::optional<Tagged<T, Tag>>`) if a genuinely optional
tagged scalar is needed; `Tagged` itself does not grow a second, orthogonal
empty state.

## Design decisions

| Decision | Choice | Why |
|---|---|---|
| Identity mechanism | **NTTP string tag (`FixedString`), not a phantom enum/type parameter** | The tag lives in the type without a companion declaration; reuses the same structural string type `Choice`/`NamedQuantity` already rely on, so there is exactly one such type in the codebase. |
| Emptiness | **Always engaged (`hasValue()` → `true`)** | `Tagged` wraps a required protocol scalar (an id, a cursor, a token) — these are not normally optional fields. A genuinely optional tagged scalar composes via `std::optional<Tagged<...>>` instead of `Tagged` inventing a second empty state. |
| Construction | **`explicit` value constructor, no converting constructor from another `Tagged`** | A bare `T` never silently becomes a `Tagged` (visibility at the call site), and one tag's value never silently becomes another tag's value (no accidental cross-tag construction). |
| Wire | **Transparent — `glz::meta` maps straight to the payload** | Matches the existing newtype family (`Quantity`, `Choice`, `Timestamp` all reduce to their payload on the wire); the tag is a compile-time-only distinction, invisible to any wire consumer. |
| Schema `title` | **Set to the tag, not to `T`'s type name** | Lets client-side codegen distinguish `UserId` from `AccountId` in the generated schema even though both compile down to the same wire shape. |

## Cross-references

- **[`../detail/fixed_string.hpp`](../../../include/morph/detail/fixed_string.hpp)**
  — the shared NTTP string type; see its own doc comment for the ODR/identity
  argument this spec relies on.
- **[`choice.md`](../forms/choice.md)** — `Choice<T, OptionsAction, ...>`
  shares the same `FixedString` mechanism and the same "wraps to a payload
  member on the wire" shape, but is optionally empty where `Tagged` is always
  engaged.
- **[`forms.md`](../forms/forms.md)** — `EmptyCapableField`, `isEngaged`, and
  `allRequiredEngaged` — the palette `Tagged`'s `hasValue()` plugs into.

## Limitations

- **No arithmetic, no formatting, no hashing.** `Tagged` deliberately offers
  only construction, access, and same-type comparison — it does not forward
  `T`'s other operations (arithmetic, `std::format` support,
  `std::hash`). An application that needs one of those either unwraps via
  `get()`/`operator*()` at the point of use or extends `Tagged` locally; nothing
  here forwards automatically.
- **No implicit conversion to `T`.** Reaching the wrapped value always goes
  through `get()` or `operator*()`; there is no `operator T()` that would let
  a `Tagged` quietly decay back to a bare scalar (and, from there, back into
  another tag's slot through an implicit `T` conversion).

## Out of scope

- Runtime-checked tag identity (e.g. a UUID namespace) — the tag is a
  compile-time distinction only, erased entirely from the wire and from RTTI.
- A phantom-type variant (`Tagged<T, struct UserIdTag>`) — the chosen
  mechanism is the `FixedString` NTTP already used elsewhere in the codebase,
  for consistency; nothing prevents an application from rolling its own
  phantom-type wrapper alongside `Tagged` if it prefers that idiom.

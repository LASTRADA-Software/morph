# Nested-Aggregate Recursion Depth Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace `mergeSchemaExtras`'s one-level cap on nested-aggregate schema
annotation (`include/morph/forms/forms.hpp`) with cycle-guarded recursion to
whatever depth the type graph actually has.

**Architecture:** Thread a variadic `Ancestors...` template parameter pack
(the chain of nested-aggregate types already being annotated on the current
path, starting with the action type) through `annotateNestedAggregate` and
`annotateNestedAggregateRef`. Factor the "is this member itself a nested
aggregate, and should I recurse into it" decision — previously duplicated
inline in `mergeSchemaExtras`'s loop and absent from `annotateNestedAggregate`
entirely (since it never went past one level) — into one new shared function,
`recurseIntoNestedAggregateIfAny`, used by both loops. That function's cycle
guard is a `static_assert` whose condition depends on the member's type and
the ancestor chain, so it only fires for the specific cyclic instantiation
that would otherwise recurse forever.

**Tech Stack:** C++23, Glaze (JSON reflection/schema), Catch2 (tests), CMake +
Ninja, `clang-release` preset.

## Global Constraints

- Spec lives at `docs/spec/forms/forms.md`, section "Nested aggregates
  (recursive, cycle-guarded)" (already written and committed — read it before
  starting; this plan implements it verbatim). If any step here turns out to
  conflict with that spec, the spec wins — update this plan's approach, not
  the spec.
- Computed fields, `formLayout`/`fieldSpans`, and `formRules` stay
  **top-level-only** regardless of nesting depth — out of scope for this
  change, do not touch that logic.
- Doxygen's `WARN_AS_ERROR` docs build fails on any undocumented public
  `@param`/`@tparam`/`@return` — and this codebase already fully documents
  even `morph::forms::detail`-namespace functions (see the existing
  `annotateNestedAggregate`/`annotateNestedAggregateRef` comments being
  replaced below), so every new/changed function needs complete Doxygen
  comments too. Reproduce the docs build locally with:
  `cmake -S . -B build -G Ninja -DMORPH_BUILD_DOCUMENTATION=ON -DMORPH_BUILD_TESTS=OFF -DMORPH_BUILD_EXAMPLES=OFF`
  then `cmake --build build --target doc`.
- Build/test commands (see `README.md`, "Building & dependencies"):
  `cmake --preset clang-release`, then
  `cmake --build build/clang-release --target morph_tests`, then
  `./build/clang-release/tests/morph_tests`. `VCPKG_ROOT` must be set in the
  environment (it already is: `/Users/yaraslau/.local/share/vcpkg`).
- Filter to just this file's tests with Catch2's tag filter:
  `./build/clang-release/tests/morph_tests "[forms][nested]"`.
- Work happens in the existing worktree at
  `/Users/yaraslau/repo/morph/.claude/worktrees/agent-a504219143d710713`
  (branch `feature/25-nested-aggregate-forms`, already rebased onto latest
  `origin/master`). Every `git`/`cmake`/build command in this plan assumes
  that directory is the working directory.

---

### Task 1: Extend the test fixtures and add a failing "recursion continues past one level" test

**Files:**
- Modify: `tests/test_nested_forms.cpp:1-13` (file header comment)
- Modify: `tests/test_nested_forms.cpp:75-84` (`Provenance`/`DeepSpecimen` fixtures)
- Modify: `tests/test_nested_forms.cpp:348-377` (replace the "depth cap" test)

**Interfaces:**
- Consumes: `morph::forms::schemaJson<A>()` (existing public API, unchanged
  signature), the file's existing `resolveNestedSchema`/`requiredNamesOf`
  helpers (unchanged).
- Produces: nothing new consumed by later tasks — this task only changes test
  code. Task 2 makes the test added here pass.

- [ ] **Step 1: Update the file header comment**

Replace:

```cpp
// SPDX-License-Identifier: Apache-2.0
//
// Coverage for issue #25: form generation recurses one level into a
// nested-aggregate member's object schema -- a directly-nested struct member
// or a `std::vector<Sub>` repeated aggregate -- applying the same
// title/x-order/required/widget rules the top level already applies, instead
// of leaving it entirely unannotated. Two distinct schema shapes exist for a
// nested aggregate (see forms.hpp's `annotateNestedAggregateRef`): glaze
// *inlines* the object schema directly into the property when the nested
// type is used exactly once in the whole schema, and *deduplicates* it via a
// shared `$defs` entry (referenced by `$ref`) when it is used two or more
// times. Both are exercised below. Recursion stops after one level (see
// docs/spec/forms/forms.md, "Nested aggregates (one level)").
```

With:

```cpp
// SPDX-License-Identifier: Apache-2.0
//
// Coverage for issue #25: form generation recurses into a nested-aggregate
// member's object schema -- a directly-nested struct member or a
// `std::vector<Sub>` repeated aggregate -- applying the same
// title/x-order/required/widget rules the top level already applies, instead
// of leaving it entirely unannotated. Two distinct schema shapes exist for a
// nested aggregate (see forms.hpp's `annotateNestedAggregateRef`): glaze
// *inlines* the object schema directly into the property when the nested
// type is used exactly once in the whole schema, and *deduplicates* it via a
// shared `$defs` entry (referenced by `$ref`) when it is used two or more
// times. Both are exercised below. Recursion continues to whatever depth the
// type graph actually has, stopping only at a genuine cycle -- a compile-time
// `static_assert`, not something this runtime test suite can exercise
// directly (see docs/spec/forms/forms.md, "Nested aggregates (recursive,
// cycle-guarded)").
```

- [ ] **Step 2: Extend the fixtures to a three-level chain and add a self-referential standalone type**

Replace:

```cpp
struct Provenance {
    std::string collectedBy;
};

// A nested aggregate whose own member is itself a nested aggregate -- the
// depth-limit case: `provenance`'s own sub-members must stay unannotated.
struct DeepSpecimen {
    double massDry = 0.0;
    Provenance provenance;
};
```

With:

```cpp
struct Origin {
    std::string country;
};

// Three levels deep: DeepSpecimen -> Provenance -> Origin. Provenance's own
// member (origin) is itself a nested aggregate too -- proving recursion
// continues past one level.
struct Provenance {
    std::string collectedBy;
    Origin origin;
};

struct DeepSpecimen {
    double massDry = 0.0;
    Provenance provenance;
};

// A self-referential nested-aggregate type (a tree node). Never passed to
// morph::forms::schemaJson<A>() anywhere in this file -- neither as the
// top-level action type itself nor nested inside another action's member --
// either use would trip forms.hpp's cycle-guard static_assert (see
// docs/spec/forms/forms.md, "Nested aggregates (recursive, cycle-guarded)").
// This only proves the type itself, and ordinary glaze JSON round-tripping
// over it, are completely unaffected by that guard.
struct TreeNode {
    std::string name;
    std::vector<TreeNode> children;
};
```

- [ ] **Step 3: Add the `using` declarations for the new types**

In the `using nestedforms::...;` block (currently lines 199-210), add two
lines, keeping the existing alphabetical order:

```cpp
using nestedforms::Origin;
```

(insert alphabetically between `using nestedforms::DeepSpecimen;` and
`using nestedforms::PlainMetaRecord;`)

```cpp
using nestedforms::TreeNode;
```

(`TreeNode` sorts after `Specimen` — 'S' < 'T' — so append it as the new
last line, after the current last entry, `using nestedforms::Specimen;`)

- [ ] **Step 4: Replace the "depth cap" test with a three-level recursion test**

Replace the entire section (from the `// ── Depth limit: exactly one level ──`
comment through the end of that `TEST_CASE`, i.e. the old lines 348-377):

```cpp
// ── Depth limit: exactly one level ──────────────────────────────────────────

TEST_CASE("Forms::SchemaJson::NestedAggregate: recursion stops after one level (depth cap)",
         "[forms][nested][issue25]") {
    auto const schema = morph::forms::schemaJson<DeepRecord>();
    glz::generic_u64 dom{};
    REQUIRE_FALSE(glz::read_json(dom, schema));

    REQUIRE(dom["properties"].contains("sample"));
    auto const& outerDef = resolveNestedSchema(dom, dom["properties"]["sample"]);

    // Level 1 (DeepSpecimen's own members) IS annotated.
    CHECK(outerDef["properties"]["massDry"].contains("x-order"));
    CHECK(outerDef["properties"]["massDry"].contains("title"));
    REQUIRE(outerDef.contains("required"));

    // Level 2 (Provenance, nested inside DeepSpecimen) is NOT annotated: its
    // object schema has no "required" key and its own properties carry no
    // x-order/title -- exactly today's pre-existing behaviour for anything
    // deeper than one level. "provenance" itself (a level-1 member) DOES get
    // an x-order/title on its own property node, same as any other member;
    // only its *inner* properties are left untouched.
    REQUIRE(outerDef["properties"].contains("provenance"));
    CHECK(outerDef["properties"]["provenance"].contains("x-order"));
    CHECK(outerDef["properties"]["provenance"].contains("title"));
    auto const& innerDef = resolveNestedSchema(dom, outerDef["properties"]["provenance"]);
    CHECK_FALSE(innerDef.contains("required"));
    CHECK_FALSE(innerDef["properties"]["collectedBy"].contains("x-order"));
    CHECK_FALSE(innerDef["properties"]["collectedBy"].contains("title"));
}
```

With:

```cpp
// ── Recursion continues past one level (no depth cap) ───────────────────────

TEST_CASE("Forms::SchemaJson::NestedAggregate: recursion continues past one level to whatever depth exists",
         "[forms][nested][issue25]") {
    auto const schema = morph::forms::schemaJson<DeepRecord>();
    glz::generic_u64 dom{};
    REQUIRE_FALSE(glz::read_json(dom, schema));

    REQUIRE(dom["properties"].contains("sample"));
    auto const& level1Def = resolveNestedSchema(dom, dom["properties"]["sample"]);

    // Level 1 (DeepSpecimen's own members) is annotated.
    CHECK(level1Def["properties"]["massDry"].contains("x-order"));
    CHECK(level1Def["properties"]["massDry"].contains("title"));
    REQUIRE(level1Def.contains("required"));

    // Level 2 (Provenance, nested inside DeepSpecimen) is now annotated too --
    // both its own property node (x-order/title, same as any level-1 member)
    // and, unlike the old one-level cap, its own "required" array.
    REQUIRE(level1Def["properties"].contains("provenance"));
    CHECK(level1Def["properties"]["provenance"].contains("x-order"));
    CHECK(level1Def["properties"]["provenance"].contains("title"));
    auto const& level2Def = resolveNestedSchema(dom, level1Def["properties"]["provenance"]);
    REQUIRE(level2Def.contains("required"));
    CHECK(level2Def["properties"]["collectedBy"].contains("x-order"));
    CHECK(level2Def["properties"]["collectedBy"].contains("title"));

    // Level 3 (Origin, nested inside Provenance) is annotated too -- proving
    // recursion does not stop at two levels either.
    REQUIRE(level2Def["properties"].contains("origin"));
    CHECK(level2Def["properties"]["origin"].contains("x-order"));
    CHECK(level2Def["properties"]["origin"].contains("title"));
    auto const& level3Def = resolveNestedSchema(dom, level2Def["properties"]["origin"]);
    REQUIRE(level3Def.contains("required"));
    CHECK(level3Def["properties"]["country"].contains("x-order"));
    CHECK(level3Def["properties"]["country"].contains("title"));
}
```

- [ ] **Step 5: Configure the build (first time only) and build the test binary**

```bash
cd /Users/yaraslau/repo/morph/.claude/worktrees/agent-a504219143d710713
export VCPKG_ROOT=/Users/yaraslau/.local/share/vcpkg
cmake --preset clang-release
cmake --build build/clang-release --target morph_tests
```

- [ ] **Step 6: Run the new test and confirm it fails**

```bash
./build/clang-release/tests/morph_tests "recursion continues past one level to whatever depth exists"
```

Expected: **FAIL**. `level2Def` will not contain `"required"` (current code
leaves `Provenance`'s own object schema — and everything past it — completely
unannotated, since it never recurses past `DeepSpecimen`). `REQUIRE(level2Def.contains("required"))`
is expected to trip first.

- [ ] **Step 7: Commit**

```bash
git add tests/test_nested_forms.cpp
git commit -m "$(cat <<'EOF'
test(forms): extend nested-aggregate fixtures to three levels

Replaces the one-level "depth cap" test with a failing test proving
recursion should continue past one level: DeepSpecimen -> Provenance ->
Origin, three levels deep. Also adds a self-referential TreeNode
fixture (never passed to schemaJson<A>() here -- that would trip the
forthcoming cycle guard) proving such a type is unaffected on its own.

Signed-off-by: Yaraslau Tamashevich <yaraslau.tamashevich@gmail.com>
EOF
)"
```

---

### Task 2: Implement cycle-guarded unbounded-depth recursion in `forms.hpp`

**Files:**
- Modify: `include/morph/forms/forms.hpp:1559-1564` (`ReflectableAggregate` doc comment)
- Modify: `include/morph/forms/forms.hpp:1660-1733` (replace `annotateNestedAggregate`/`annotateNestedAggregateRef`, add `recurseIntoNestedAggregateIfAny`)
- Modify: `include/morph/forms/forms.hpp:1896-1915` (`mergeSchemaExtras`'s nested-aggregate branch)

**Interfaces:**
- Consumes: `ReflectableAggregate<T>` concept, `IsStdVector<T>` trait,
  `annotateBasicMemberProperty<Owner, Member>(property, name)`,
  `forEachNamedMember`, `declaredOptional<T>(name)`, `isStdOptional<T>` — all
  pre-existing, all unchanged (defined earlier in the same file).
- Produces:
  - `template <typename Sub, typename... Ancestors> void annotateNestedAggregate(glz::generic_u64& dom, glz::generic_u64& node)`
    — **signature changed**: gains `dom` as its first parameter and an
    `Ancestors...` pack. Anything outside this file calling it directly would
    need updating; nothing does (only `annotateNestedAggregateRef` and the
    tests in `test_nested_forms.cpp` — which call `annotateNestedAggregateRef`,
    not this — reference it).
  - `template <typename Sub, typename... Ancestors> void annotateNestedAggregateRef(glz::generic_u64& dom, glz::generic_u64& propertyOrItems)`
    — signature gains the `Ancestors...` pack (variadic, so existing
    single-template-argument call sites, e.g.
    `annotateNestedAggregateRef<Specimen>(dom, property)` in
    `test_nested_forms.cpp:519,528,539`, keep compiling unchanged with an
    empty `Ancestors` pack).
  - `template <typename Member, typename... Ancestors> void recurseIntoNestedAggregateIfAny(glz::generic_u64& dom, glz::generic_u64& property)`
    — new function, used by both `mergeSchemaExtras` and
    `annotateNestedAggregate`.

- [ ] **Step 1: Update `ReflectableAggregate`'s doc comment**

Replace:

```cpp
/// @brief Concept: `T` is glaze-reflectable as a JSON object -- the same test
///        that decides whether glaze emits a member into `$defs`/`$ref`
///        rather than inline. Shared by the one-level nested-aggregate
///        recursion below and `reconcileDeclaredPrecision` elsewhere.
template <typename T>
concept ReflectableAggregate = glz::reflectable<T> || glz::glaze_object_t<T>;
```

With:

```cpp
/// @brief Concept: `T` is glaze-reflectable as a JSON object -- the same test
///        that decides whether glaze emits a member into `$defs`/`$ref`
///        rather than inline. Shared by the cycle-guarded nested-aggregate
///        recursion below and `reconcileDeclaredPrecision` elsewhere.
template <typename T>
concept ReflectableAggregate = glz::reflectable<T> || glz::glaze_object_t<T>;
```

- [ ] **Step 2: Replace `annotateNestedAggregate`/`annotateNestedAggregateRef` with the cycle-guarded, `Ancestors`-threaded versions, plus the new shared helper**

Replace this entire block (from the blank line right after
`annotateBasicMemberProperty`'s closing brace through the closing brace of the
old `annotateNestedAggregateRef`):

```cpp

/// @brief One level of recursion (see `docs/spec/forms/forms.md`, "Nested
///        aggregates (one level)"): annotates @p node -- the object-schema
///        DOM node for a nested-aggregate member -- applying `required` and
///        `annotateBasicMemberProperty`'s rules to its own properties.
///
/// @p node is @e which DOM node depends on how many places in the whole
/// schema reference `Sub`: glaze **inlines** the object schema directly into
/// the referencing property when `Sub` is used exactly once (so @p node
/// *is* that property node), but **deduplicates** via `$defs`/`$ref` when
/// `Sub` is used two or more times (so @p node is the shared `$defs` entry,
/// resolved by the caller). Both forms have the identical `{"properties":
/// {...}}` shape this function needs, so one implementation handles both --
/// see the call site in `mergeSchemaExtras` for how @p node is resolved.
///
/// Deliberately does **not** recurse again: if `Sub` itself has a member that
/// is itself an aggregate, that member is left exactly as glaze emitted it --
/// unannotated, matching today's behaviour beyond one level. This bounds the
/// generator to one level of nesting, as `docs/spec/forms/forms.md` documents.
/// Computed fields, `formLayout`/`fieldSpans`, and `formRules` also stay
/// top-level-only; a nested `Sub` declaring any of those has no effect here.
///
/// @tparam Sub  Nested aggregate type (default-constructible, glaze-reflectable
///              -- the same requirements the top-level action type already has).
/// @param node The object-schema DOM node to annotate in place (see above).
template <typename Sub>
void annotateNestedAggregate(glz::generic_u64& node) {
    Sub probe{};
    glz::generic_u64::array_t requiredNames{};
    forEachNamedMember(probe, [&]<std::size_t I>(std::string_view name, const auto& member) {
        using Member = std::remove_cvref_t<decltype(member)>;
        if (!(isStdOptional<Member> || declaredOptional<Sub>(name))) {
            requiredNames.emplace_back(std::string{name});
        }
        auto& property = node["properties"][std::string{name}];
        property["x-order"] = std::uint64_t{I};
        annotateBasicMemberProperty<Sub, Member>(property, name);
    });
    // Idempotent if two members (or two actions sharing this schema call)
    // resolve to the same $defs entry: re-deriving the identical required
    // array is harmless.
    node["required"] = requiredNames;
}

/// @brief Resolves the object-schema DOM node for a nested-aggregate member,
///        given the property (or array `items`) node glaze wrote for it, and
///        annotates it via `annotateNestedAggregate<Sub>`.
///
/// Handles both forms `Sub` can take in the schema (see
/// `annotateNestedAggregate`'s doc comment): a `$ref` into `$defs` (`Sub` used
/// 2+ times somewhere in the schema) resolves to that shared def; anything
/// else is assumed to be the inlined object schema itself (`Sub` used exactly
/// once). A property that is neither -- glaze emitted something other than an
/// object schema for a type this function's caller already confirmed is a
/// `ReflectableAggregate` -- is left untouched rather than guessed at.
/// @tparam Sub          Nested aggregate type, as `annotateNestedAggregate` requires.
/// @param dom           The whole schema DOM (so a `$ref`'s `$defs` entry can be found).
/// @param propertyOrItems The property node itself (single nested member) or its
///                        array `items` node (`std::vector<Sub>` member).
template <typename Sub>
void annotateNestedAggregateRef(glz::generic_u64& dom, glz::generic_u64& propertyOrItems) {
    constexpr std::string_view kDefsPrefix = "#/$defs/";
    if (propertyOrItems.contains("$ref")) {
        if (auto const* ref = propertyOrItems["$ref"].get_if<std::string>()) {
            if (std::string_view{*ref}.starts_with(kDefsPrefix)) {
                annotateNestedAggregate<Sub>(dom["$defs"][std::string{ref->substr(kDefsPrefix.size())}]);
            }
        }
        return;
    }
    if (propertyOrItems.contains("properties")) {
        annotateNestedAggregate<Sub>(propertyOrItems);
    }
}
```

With:

```cpp

// annotateNestedAggregate, annotateNestedAggregateRef, and
// recurseIntoNestedAggregateIfAny are mutually recursive (each nested
// aggregate found while annotating one may itself contain another), so all
// three need forward declarations before any of their bodies can reference
// the others.
template <typename Sub, typename... Ancestors>
void annotateNestedAggregate(glz::generic_u64& dom, glz::generic_u64& node);

template <typename Sub, typename... Ancestors>
void annotateNestedAggregateRef(glz::generic_u64& dom, glz::generic_u64& propertyOrItems);

template <typename Member, typename... Ancestors>
void recurseIntoNestedAggregateIfAny(glz::generic_u64& dom, glz::generic_u64& property);

/// @brief Recurses into @p property's own object schema if @p Member (or, for
///        `std::vector<Sub>`, its element type) is itself a
///        `ReflectableAggregate` -- the single decision point shared by
///        `mergeSchemaExtras`'s top-level loop and `annotateNestedAggregate`'s
///        own loop, so the cycle guard below has exactly one implementation.
///
/// @p Ancestors is the chain of nested-aggregate types already being
/// annotated on the current path, **including** the type that declares this
/// member (the caller appends its own `Sub`/`A` before calling this). If the
/// type to recurse into matches any entry already on that chain, recursing
/// further would eventually re-enter this same instantiation and try to do
/// so again -- forever. Rather than let that happen, a `static_assert` (whose
/// condition depends on @p Member and @p Ancestors, so it only fires for the
/// specific cyclic instantiation, not every use of this generator) rejects it
/// at compile time instead: a self-referential nested-aggregate type (e.g.
/// `struct Node { std::vector<Node> children; };`), or a mutual reference
/// between two distinct types, fails to build with a clear message rather
/// than exhausting the compiler's template-instantiation depth. This only
/// rejects genuine cycles -- the same type reused from two unrelated places
/// in the schema (a "diamond") is not on either path's ancestor chain and
/// recurses normally into both. See `docs/spec/forms/forms.md`, "Nested
/// aggregates (recursive, cycle-guarded)".
/// @tparam Member    The static type of the member `annotateBasicMemberProperty`
///                    was just applied to.
/// @tparam Ancestors The ancestor chain so far, ending with the type that
///                    declares this member.
/// @param dom      The whole schema DOM (so a `$ref`'s `$defs` entry can be found).
/// @param property The property node for this member (or, for `std::vector<Sub>`,
///                  the property whose `"items"` node is the one to check).
template <typename Member, typename... Ancestors>
void recurseIntoNestedAggregateIfAny(glz::generic_u64& dom, glz::generic_u64& property) {
    if constexpr (ReflectableAggregate<Member>) {
        if constexpr ((std::same_as<Member, Ancestors> || ...)) {
            static_assert(!(std::same_as<Member, Ancestors> || ...),
                          "morph::forms: cyclic nested-aggregate schema -- this member's type already "
                          "appears in its own chain of enclosing nested-aggregate types (a self- or "
                          "mutually-referential type). Recursion depth is otherwise unbounded, but cycles "
                          "are not supported: restructure the domain type (flatten the self-reference, or "
                          "represent the recursive edge as an opaque id instead of a nested value).");
        } else {
            annotateNestedAggregateRef<Member, Ancestors...>(dom, property);
        }
    } else if constexpr (IsStdVector<Member>::value &&
                         ReflectableAggregate<typename IsStdVector<Member>::ValueType>) {
        using ItemType = typename IsStdVector<Member>::ValueType;
        if constexpr ((std::same_as<ItemType, Ancestors> || ...)) {
            static_assert(!(std::same_as<ItemType, Ancestors> || ...),
                          "morph::forms: cyclic nested-aggregate schema -- this std::vector<Sub> member's "
                          "element type already appears in its own chain of enclosing nested-aggregate "
                          "types (a self- or mutually-referential type). Recursion depth is otherwise "
                          "unbounded, but cycles are not supported: restructure the domain type (flatten "
                          "the self-reference, or represent the recursive edge as an opaque id instead of "
                          "a nested value).");
        } else if (property.contains("items")) {
            annotateNestedAggregateRef<ItemType, Ancestors...>(dom, property["items"]);
        }
    }
}

/// @brief Annotates @p node -- the object-schema DOM node for a
///        nested-aggregate member -- applying `required` and
///        `annotateBasicMemberProperty`'s rules to its own properties, then
///        recursing into any of *its* members that are themselves nested
///        aggregates (see `recurseIntoNestedAggregateIfAny`), to whatever
///        depth the type graph actually has.
///
/// @p node is @e which DOM node depends on how many places in the whole
/// schema reference `Sub`: glaze **inlines** the object schema directly into
/// the referencing property when `Sub` is used exactly once (so @p node
/// *is* that property node), but **deduplicates** via `$defs`/`$ref` when
/// `Sub` is used two or more times (so @p node is the shared `$defs` entry,
/// resolved by the caller). Both forms have the identical `{"properties":
/// {...}}` shape this function needs, so one implementation handles both --
/// see the call site in `mergeSchemaExtras` for how @p node is resolved.
///
/// Computed fields, `formLayout`/`fieldSpans`, and `formRules` stay
/// top-level-only regardless of depth; a nested `Sub` declaring any of those
/// has no effect here.
///
/// @tparam Sub       Nested aggregate type (default-constructible, glaze-reflectable
///                    -- the same requirements the top-level action type already has).
/// @tparam Ancestors The ancestor chain so far, ending with `Sub` itself, passed
///                    through to `recurseIntoNestedAggregateIfAny` for each of
///                    `Sub`'s own members (see that function's doc comment).
/// @param dom  The whole schema DOM (so a deeper `$ref`'s `$defs` entry can be found).
/// @param node The object-schema DOM node to annotate in place (see above).
template <typename Sub, typename... Ancestors>
void annotateNestedAggregate(glz::generic_u64& dom, glz::generic_u64& node) {
    Sub probe{};
    glz::generic_u64::array_t requiredNames{};
    forEachNamedMember(probe, [&]<std::size_t I>(std::string_view name, const auto& member) {
        using Member = std::remove_cvref_t<decltype(member)>;
        if (!(isStdOptional<Member> || declaredOptional<Sub>(name))) {
            requiredNames.emplace_back(std::string{name});
        }
        auto& property = node["properties"][std::string{name}];
        property["x-order"] = std::uint64_t{I};
        annotateBasicMemberProperty<Sub, Member>(property, name);
        recurseIntoNestedAggregateIfAny<Member, Ancestors..., Sub>(dom, property);
    });
    // Idempotent if two members (or two actions sharing this schema call)
    // resolve to the same $defs entry: re-deriving the identical required
    // array is harmless.
    node["required"] = requiredNames;
}

/// @brief Resolves the object-schema DOM node for a nested-aggregate member,
///        given the property (or array `items`) node glaze wrote for it, and
///        annotates it via `annotateNestedAggregate<Sub, Ancestors...>`.
///
/// Handles both forms `Sub` can take in the schema (see
/// `annotateNestedAggregate`'s doc comment): a `$ref` into `$defs` (`Sub` used
/// 2+ times somewhere in the schema) resolves to that shared def; anything
/// else is assumed to be the inlined object schema itself (`Sub` used exactly
/// once). A property that is neither -- glaze emitted something other than an
/// object schema for a type this function's caller already confirmed is a
/// `ReflectableAggregate` -- is left untouched rather than guessed at.
/// @tparam Sub          Nested aggregate type, as `annotateNestedAggregate` requires.
/// @tparam Ancestors    The ancestor chain so far (excluding `Sub`), forwarded
///                       to `annotateNestedAggregate` unchanged.
/// @param dom           The whole schema DOM (so a `$ref`'s `$defs` entry can be found).
/// @param propertyOrItems The property node itself (single nested member) or its
///                        array `items` node (`std::vector<Sub>` member).
template <typename Sub, typename... Ancestors>
void annotateNestedAggregateRef(glz::generic_u64& dom, glz::generic_u64& propertyOrItems) {
    constexpr std::string_view kDefsPrefix = "#/$defs/";
    if (propertyOrItems.contains("$ref")) {
        if (auto const* ref = propertyOrItems["$ref"].get_if<std::string>()) {
            if (std::string_view{*ref}.starts_with(kDefsPrefix)) {
                annotateNestedAggregate<Sub, Ancestors...>(
                    dom, dom["$defs"][std::string{ref->substr(kDefsPrefix.size())}]);
            }
        }
        return;
    }
    if (propertyOrItems.contains("properties")) {
        annotateNestedAggregate<Sub, Ancestors...>(dom, propertyOrItems);
    }
}
```

- [ ] **Step 3: Simplify `mergeSchemaExtras`'s nested-aggregate branch to call the new shared helper**

Replace:

```cpp
        // Nested aggregates (one level -- docs/spec/forms/forms.md, "Nested
        // aggregates (one level)"): a member whose type is itself a
        // reflectable aggregate gets an object schema from glaze -- either
        // inlined directly into this property (the type is used exactly once
        // in the whole schema) or shared via `$defs`/`$ref` (used 2+ times);
        // `annotateNestedAggregateRef` resolves whichever form it is. Recurse
        // one level so that object schema's own members get `x-order`/
        // `required`/title/Quantity/Choice/widget annotations too, instead of
        // being silently unannotated. Purely additive: an action with no
        // nested aggregate member has nothing here to trigger on, so its
        // schema is byte-for-byte unchanged.
        if constexpr (ReflectableAggregate<Member>) {
            annotateNestedAggregateRef<Member>(dom, property);
        } else if constexpr (IsStdVector<Member>::value &&
                             ReflectableAggregate<typename IsStdVector<Member>::ValueType>) {
            using ItemType = typename IsStdVector<Member>::ValueType;
            if (property.contains("items")) {
                annotateNestedAggregateRef<ItemType>(dom, property["items"]);
            }
        }
```

With:

```cpp
        // Nested aggregates (recursive, cycle-guarded -- docs/spec/forms/forms.md,
        // "Nested aggregates (recursive, cycle-guarded)"): a member whose type
        // is itself a reflectable aggregate gets an object schema from glaze --
        // either inlined directly into this property (the type is used exactly
        // once in the whole schema) or shared via `$defs`/`$ref` (used 2+
        // times). `recurseIntoNestedAggregateIfAny` resolves whichever form it
        // is and recurses so that object schema's own members get
        // `x-order`/`required`/title/Quantity/Choice/widget annotations too,
        // however deep the type graph goes (guarding against cycles at compile
        // time -- see that function's doc comment). Purely additive: an action
        // with no nested aggregate member has nothing here to trigger on, so
        // its schema is byte-for-byte unchanged.
        recurseIntoNestedAggregateIfAny<Member, A>(dom, property);
```

- [ ] **Step 4: Rebuild and run the full nested-forms test suite**

```bash
cmake --build build/clang-release --target morph_tests
./build/clang-release/tests/morph_tests "[forms][nested]"
```

Expected: **PASS** — every test tagged `[forms][nested]`, including the new
three-level test from Task 1 and the pre-existing idempotence/`$ref`/inline/
`FieldMeta`/`Quantity`/`Choice`/`declaredOptional`/defensive-fallback tests
(none of their expectations changed; the earlier levels' behavior is
unaffected by extending recursion further).

- [ ] **Step 5: Run the complete test suite to check for regressions elsewhere**

```bash
./build/clang-release/tests/morph_tests
```

Expected: **PASS** — no other test exercises `forms.hpp`'s nested-aggregate
path with a type graph deep enough to be affected, so this is a regression
check, not expected to surface anything new.

- [ ] **Step 6: Commit**

```bash
git add include/morph/forms/forms.hpp
git commit -m "$(cat <<'EOF'
forms: recurse into nested aggregates to any depth, not just one

Threads an Ancestors... template parameter pack through
annotateNestedAggregate/annotateNestedAggregateRef -- the chain of
nested-aggregate types already being annotated on the current path,
starting with the action type. Factors the "is this member itself a
nested aggregate, and should I recurse into it" decision (previously
duplicated inline in mergeSchemaExtras's loop, and entirely absent from
annotateNestedAggregate since it never went past one level) into one
shared recurseIntoNestedAggregateIfAny, used by both loops.

A member whose type -- or, for std::vector<Sub>, Sub -- already appears
on the ancestor chain would recurse into this same instantiation again,
forever. Rather than let that happen, a static_assert (dependent on the
member type and the chain, so it only fires for the actual cyclic
instantiation) rejects a self- or mutually-referential nested-aggregate
type graph at compile time instead.

See docs/spec/forms/forms.md, "Nested aggregates (recursive,
cycle-guarded)".

Signed-off-by: Yaraslau Tamashevich <yaraslau.tamashevich@gmail.com>
EOF
)"
```

---

### Task 3: Add the standalone self-referential-type test and verify the docs build

**Files:**
- Modify: `tests/test_nested_forms.cpp` (add one `TEST_CASE`, after the last
  existing test case in the file)

**Interfaces:**
- Consumes: `nestedforms::TreeNode` (added in Task 1, Step 2), `glz::write_json`,
  `glz::read_json` (Glaze's own API, already used elsewhere in this test
  suite — see `tests/test_bridge_fixes.cpp:252` for the `.value_or(std::string{})`
  pattern this step follows).
- Produces: nothing consumed by anything later — this is the plan's last task.

- [ ] **Step 1: Add the standalone self-referential-type test**

Append to the end of `tests/test_nested_forms.cpp`:

```cpp

// ── Self-referential nested-aggregate type, standalone ─────────────────────

TEST_CASE("Forms::SchemaJson::NestedAggregate: a self-referential nested-aggregate type round-trips fine on its own",
         "[forms][nested][issue25]") {
    // TreeNode is never passed to morph::forms::schemaJson<A>() in this file
    // -- see its doc comment. This only proves the type itself, and ordinary
    // glaze JSON round-tripping over it, are completely unaffected by
    // forms.hpp's cycle-guard static_assert, which fires only when a type
    // like this is actually nested under some schemaJson<A>() instantiation.
    TreeNode root{};
    root.name = "root";
    TreeNode child{};
    child.name = "child";
    root.children.push_back(child);

    std::string const json = glz::write_json(root).value_or(std::string{});
    REQUIRE_FALSE(json.empty());

    TreeNode decoded{};
    REQUIRE_FALSE(glz::read_json(decoded, json));
    CHECK(decoded.name == "root");
    REQUIRE(decoded.children.size() == 1);
    CHECK(decoded.children[0].name == "child");
}
```

- [ ] **Step 2: Rebuild and run**

```bash
cmake --build build/clang-release --target morph_tests
./build/clang-release/tests/morph_tests "[forms][nested]"
```

Expected: **PASS**, including the new test.

- [ ] **Step 3: Verify the Doxygen docs build succeeds with the new/changed comments**

```bash
cmake -S . -B build/docs -G Ninja -DMORPH_BUILD_DOCUMENTATION=ON -DMORPH_BUILD_TESTS=OFF -DMORPH_BUILD_EXAMPLES=OFF
cmake --build build/docs --target doc
```

Expected: build succeeds (exit code 0). If it fails on a missing
`@param`/`@tparam`/`@return`, compare the failing function's signature against
Task 2 Step 2/3's replacement text above and add the missing tag — every
parameter and template parameter introduced there already has one documented,
so a failure here means a transcription slip, not a design gap.

- [ ] **Step 4: Commit (only if Step 3 required a fix)**

```bash
git add include/morph/forms/forms.hpp
git commit -m "$(cat <<'EOF'
forms: fix missing Doxygen tag on nested-aggregate recursion helper

Signed-off-by: Yaraslau Tamashevich <yaraslau.tamashevich@gmail.com>
EOF
)"
```

If Step 3 passed cleanly with no changes needed, skip this commit —
Step 1's test addition was already committed as part of a normal
add-test-then-verify cycle; commit just that:

```bash
git add tests/test_nested_forms.cpp
git commit -m "$(cat <<'EOF'
test(forms): cover a self-referential nested-aggregate type standalone

Proves TreeNode -- a tree-shaped, self-referential nested-aggregate
type -- round-trips through glaze JSON encode/decode normally on its
own. It is never passed to schemaJson<A>() in this file; doing so
would trip forms.hpp's cycle-guard static_assert, which this test
suite has no harness to exercise directly (see
docs/spec/forms/forms.md, "Nested aggregates (recursive,
cycle-guarded)").

Signed-off-by: Yaraslau Tamashevich <yaraslau.tamashevich@gmail.com>
EOF
)"
```

---

## Self-Review Notes

- **Spec coverage:** Every behavior the spec (`docs/spec/forms/forms.md`,
  "Nested aggregates (recursive, cycle-guarded)") describes has a
  corresponding task: unbounded acyclic recursion depth (Task 1's test, Task
  2's implementation), the cycle-guard `static_assert` and its scoping to only
  the offending instantiation (Task 2 Step 2's doc comment and code), the
  "diamond is not a cycle" guarantee (unchanged `$defs`/`$ref` dedup logic,
  covered by the pre-existing `$ref`-form tests that keep passing in Task 2
  Step 4), computed-fields/`formLayout`/`formRules` staying top-level-only
  (untouched code, no task needed), and "a self-referential type not nested
  under an action compiles fine" (Task 1 Step 2's `TreeNode` fixture, Task 3's
  test).
- **Type consistency:** `annotateNestedAggregate`'s signature
  (`glz::generic_u64& dom, glz::generic_u64& node` plus `Sub, Ancestors...`)
  matches every call site introduced across Task 2 (`annotateNestedAggregateRef`'s
  two internal calls; no other caller exists). `recurseIntoNestedAggregateIfAny`'s
  signature matches both of its call sites (`mergeSchemaExtras`'s
  `<Member, A>`, `annotateNestedAggregate`'s `<Member, Ancestors..., Sub>`).
  `annotateNestedAggregateRef`'s existing test call sites
  (`test_nested_forms.cpp:519,528,539`, `annotateNestedAggregateRef<Specimen>(dom, property)`)
  remain valid: `Ancestors...` is variadic and defaults to empty when only
  `Sub` is given explicitly.
- **No placeholders:** every step above contains the literal before/after code
  or the literal shell command to run; none say "add appropriate X" or defer
  detail to another task.

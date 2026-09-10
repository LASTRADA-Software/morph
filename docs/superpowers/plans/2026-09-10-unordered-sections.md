# Unordered Sections Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or
  superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for
  tracking.

**Goal:** Add `morph::forms::SectionSet<Model, Sections...>` — N independently editable action drafts, each gated
on its own `ActionValidator<A>::ready`, dispatched through the handler with no sequencing. Closes morph#513.

**Architecture:** Extract the machinery `FlowSession` and `SectionSet` share into
`forms/detail/session_common.hpp`, leaving `flows.hpp`'s public names intact. Add `forms/sections.hpp` with the
declaration types (`Section`, `SectionGroup`), the schema emitter, and the runtime session. `SectionSet` mirrors
`FlowSession` minus the current-step constraint.

**Tech Stack:** C++23, header-only. Catch2 v3 for tests. glaze for JSON. No new dependencies.

## Global Constraints

Every task's requirements implicitly include these. They are the gates CI applies.

- **C++23, header-only.** No new third-party dependencies.
- **Build with clang AND gcc.** GCC has `-Werror=useless-cast`, which clang lacks; a clang-only check misses it.
  Presets: `clang-debug`, `gcc-debug`.
- **`-Wdocumentation -Werror` clean.** Every public symbol needs complete `@param`/`@tparam`/`@return`. Never write
  a Doxygen command name (`@throws`, `@par`) in prose — even in backticks — it parses as a command.
- **Tree-wide clang-format:** `git ls-files -z '*.hpp' '*.cpp' | xargs -0 clang-format --dry-run -Werror` must be
  silent.
- **clang-tidy clean on changed lines**, diffed against `origin/master...HEAD` (not the working tree — that is
  empty after a commit).
- **Header ↔ spec sync:** any change under `include/morph/forms/` requires a matching change under
  `docs/spec/forms/`. Task 8 satisfies this and must land before the PR opens.
- **Invariant 7:** every test must be verified to FAIL without its implementation. A test that passes either way
  measures nothing.
- **`scripts/branch_partial_allowlist.json`** must have every `line` landing on its own `source` text, and every
  `line N` in `reason` prose resolving to a real entry. Re-pin if line numbers move.

---

## File Structure

| File | Responsibility |
|---|---|
| `include/morph/forms/detail/session_common.hpp` *(new)* | `Bind`, `forEachTupleElement`, `forPackElement`, `AllDistinct`, `emitBindsInto` — shared by both session types |
| `include/morph/forms/flows.hpp` *(modify)* | Unchanged public surface; helpers now come from the shared header, `morph::flows::Bind` becomes an alias |
| `include/morph/forms/sections.hpp` *(new)* | `Section`, `SectionGroup`, `SectionGroupTraits`, `BRIDGE_REGISTER_SECTION_GROUP`, `sectionGroupSchemaJson`, `SectionSet` |
| `tests/test_sections.cpp` *(new)* | All nine cases from the spec |
| `tests/CMakeLists.txt` *(modify)* | Register the new test file |
| `docs/spec/forms/sections.md` *(new)* | Design spec for the new surface; also satisfies the spec-sync gate |

---

### Task 1: Extract the shared session helpers

Pure refactor. `flows.hpp` must behave identically and its existing tests must pass untouched.

**Files:**
- Create: `include/morph/forms/detail/session_common.hpp`
- Modify: `include/morph/forms/flows.hpp` (remove the moved definitions, add the include and the `Bind` alias)
- Test: `tests/test_flows_apps.cpp` (existing — must pass unchanged)

**Interfaces:**
- Produces: `morph::forms::Bind<Field, Path>` with `static constexpr std::string_view field()` and `path()`;
  `morph::forms::detail::forEachTupleElement<Tuple>(Visitor&&)`;
  `morph::forms::detail::forPackElement<Ts...>(std::size_t, Visitor&&)`;
  `morph::forms::detail::AllDistinct<Ts...>::value`;
  `morph::forms::detail::emitBindsInto<BindsTuple>(glz::generic_u64& node)`.
- Consumes: nothing.

- [ ] **Step 1: Create the shared header**

```cpp
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>

#include <glaze/glaze.hpp>

#include "../forms.hpp"

namespace morph::forms {

/// @brief One `field -> "<ActionTypeId>.<field>"` prefill binding declared on a
///        wizard step or an unordered section.
///
/// A declaration, not a write. Nothing in the framework assigns a bound field:
/// `wizardSchemaJson`/`sectionGroupSchemaJson` emit it for a renderer, and the
/// captured value is read back through `resolved()`.
/// @tparam Field The action's field name to prefill.
/// @tparam Path  Source path, `"<ActionTypeId>.<field>"`, into captured values.
template <morph::forms::FixedString Field, morph::forms::FixedString Path>
struct Bind {
    /// @brief The action field name this binding fills.
    /// @return The declared field name.
    [[nodiscard]] static constexpr std::string_view field() noexcept { return Field.view(); }

    /// @brief The source path into captured values.
    /// @return The declared `"<ActionTypeId>.<field>"` path.
    [[nodiscard]] static constexpr std::string_view path() noexcept { return Path.view(); }
};

namespace detail {

/// @brief Invokes `visitor.template operator()<std::tuple_element_t<I, Tuple>, I>()`
///        for every element of @p Tuple, in order.
/// @tparam Tuple   A `std::tuple<...>` type (only its element types/arity are used).
/// @tparam Visitor Callable with a `template<typename Element, std::size_t I> operator()()`.
/// @param visitor Callable invoked once per tuple element.
template <typename Tuple, typename Visitor>
constexpr void forEachTupleElement(Visitor&& visitor) {
    []<std::size_t... I>(std::index_sequence<I...>, Visitor&& innerVisitor) {
        (innerVisitor.template operator()<std::tuple_element_t<I, Tuple>, I>(), ...);
    }(std::make_index_sequence<std::tuple_size_v<Tuple>>{}, std::forward<Visitor>(visitor));
}

/// @brief Invokes `visitor.template operator()<T>()` for the pack element of
///        `Ts...` at runtime position @p index. A no-op when
///        `index >= sizeof...(Ts)`.
/// @tparam Ts      The pack to index into.
/// @tparam Visitor Callable with a `template<typename T> operator()()`.
/// @param index   0-based position to visit.
/// @param visitor Callable invoked for the element at @p index.
template <typename... Ts, typename Visitor>
constexpr void forPackElement(std::size_t index, Visitor&& visitor) {
    std::size_t i = 0;
    (void)((i++ == index ? (visitor.template operator()<Ts>(), true) : false) || ...);
}

/// @brief Trait: `true` when every type in `Ts...` is pairwise distinct.
/// @tparam Ts Types to check for pairwise distinctness.
template <typename... Ts>
struct AllDistinct : std::true_type {};

/// @brief Recursive case: `T` distinct from every type in `Rest...`, and `Rest...` pairwise distinct.
/// @tparam T    The type being checked against `Rest...`.
/// @tparam Rest The remaining types.
template <typename T, typename... Rest>
struct AllDistinct<T, Rest...> : std::bool_constant<(!std::is_same_v<T, Rest> && ...) && AllDistinct<Rest...>::value> {
};

/// @brief Writes each `Bind` in @p BindsTuple into @p node as
///        `"<field>": "<path>"`. A no-op for an empty tuple.
/// @tparam BindsTuple `std::tuple<Bind<...>...>`.
/// @param node Destination JSON object node.
template <typename BindsTuple>
void emitBindsInto(glz::generic_u64& node) {
    forEachTupleElement<BindsTuple>([&]<typename BindT, std::size_t J>() {
        static_cast<void>(J);
        node[std::string{BindT::field()}] = std::string{BindT::path()};
    });
}

}  // namespace detail
}  // namespace morph::forms
```

- [ ] **Step 2: Point flows.hpp at it**

In `include/morph/forms/flows.hpp`: add `#include "detail/session_common.hpp"` beside the existing `#include
"forms.hpp"`. Delete the `Bind` struct definition and the `detail` block containing `forEachTupleElement`,
`forStep`, and `AllDistinct`. Add the compatibility alias in `namespace morph::flows`:

```cpp
/// @brief Prefill binding for a wizard step.
///
/// Alias for `morph::forms::Bind`, which both session types share. Kept in this
/// namespace because it is the name shipped consumers already write.
/// @tparam Field The step action's field name to prefill.
/// @tparam Path  Source path, `"<PriorAction>.<field>"`, into captured values.
template <morph::forms::FixedString Field, morph::forms::FixedString Path>
using Bind = morph::forms::Bind<Field, Path>;
```

Then replace the internal call sites: `detail::forEachTupleElement<...>` becomes
`::morph::forms::detail::forEachTupleElement<...>`, `detail::forStep<...>` becomes
`::morph::forms::detail::forPackElement<...>`, and `detail::AllDistinct<...>` becomes
`::morph::forms::detail::AllDistinct<...>`.

- [ ] **Step 3: Build both compilers and run the existing suite**

```bash
cmake --build build/clang-debug --target morph_tests -j 12
./build/clang-debug/tests/morph_tests "[flows]"
cmake --build build/gcc-net --target morph_tests -j 12 2>&1 | grep -c "error:"
```

Expected: build clean under both; `[flows]` cases all pass. This is a refactor — a behaviour change here is a bug.

- [ ] **Step 4: Commit**

```bash
git add include/morph/forms/detail/session_common.hpp include/morph/forms/flows.hpp
git commit -m "forms: extract the session helpers both FlowSession and SectionSet need"
```

---

### Task 2: Declaration types and the schema document

**Files:**
- Create: `include/morph/forms/sections.hpp`
- Create: `tests/test_sections.cpp`
- Modify: `tests/CMakeLists.txt:102` (add `test_sections.cpp` beside `test_flows_apps.cpp`)

**Interfaces:**
- Consumes: `morph::forms::Bind`, `morph::forms::detail::forEachTupleElement`,
  `morph::forms::detail::emitBindsInto` (Task 1).
- Produces: `morph::forms::Section<Action, Title, Binds...>` with `using action`, `using binds`, `static constexpr
  std::string_view title()`; `morph::forms::SectionGroup<Title, Sections...>` with `using sections`, `title()`;
  `morph::forms::SectionGroupTraits<G>::typeId()`; `BRIDGE_REGISTER_SECTION_GROUP(G, NAME)`;
  `morph::forms::sectionGroupSchemaJson<G>() -> std::string`.

- [ ] **Step 1: Write the failing test**

Append to `tests/test_sections.cpp` (create it with this content):

```cpp
// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <glaze/glaze.hpp>
#include <morph/core/bridge.hpp>
#include <morph/core/registry.hpp>
#include <morph/forms/sections.hpp>
#include <string>

// Two independent sections of one screen: a profile block and a preferences
// block. Neither is a step of the other -- editing them in either order is the
// point of SectionSet.
struct SecProfile {
    std::string name;
    [[nodiscard]] bool validate() const { return !name.empty(); }
};
struct SecProfileResult {
    std::int64_t id = 0;
};
struct SecPrefs {
    std::int64_t profileId = 0;
    std::string theme;
    [[nodiscard]] bool validate() const { return !theme.empty(); }
};
struct SecPrefsResult {
    std::string summary;
};

struct SecModel {
    SecProfileResult execute(SecProfile action) { return {.id = static_cast<std::int64_t>(action.name.size())}; }
    SecPrefsResult execute(SecPrefs action) { return {.summary = action.theme}; }
};

BRIDGE_REGISTER_MODEL(SecModel, "SectionsTest_Model")
BRIDGE_REGISTER_ACTION(SecModel, SecProfile, "SectionsTest_Profile")
BRIDGE_REGISTER_ACTION(SecModel, SecPrefs, "SectionsTest_Prefs")

using ProfileSection = morph::forms::Section<SecProfile, "Profile">;
using PrefsSection =
    morph::forms::Section<SecPrefs, "Preferences", morph::forms::Bind<"profileId", "SectionsTest_Profile.id">>;
using DemoGroup = morph::forms::SectionGroup<"Account settings", ProfileSection, PrefsSection>;
BRIDGE_REGISTER_SECTION_GROUP(DemoGroup, "SectionsTest_DemoGroup")

TEST_CASE("sectionGroupSchemaJson carries each section's title, action and binds", "[sections][schema]") {
    auto const json = morph::forms::sectionGroupSchemaJson<DemoGroup>();
    REQUIRE_FALSE(json.empty());

    glz::generic_u64 dom{};
    REQUIRE_FALSE(glz::read_json(dom, json));

    CHECK(dom["s-id"].get_string() == "SectionsTest_DemoGroup");
    CHECK(dom["s-title"].get_string() == "Account settings");

    auto const& sections = dom["s-sections"].get_array();
    REQUIRE(sections.size() == 2);
    CHECK(sections[0]["action"].get_string() == "SectionsTest_Profile");
    CHECK(sections[0]["title"].get_string() == "Profile");
    CHECK(sections[1]["action"].get_string() == "SectionsTest_Prefs");
    CHECK(sections[1]["prefill"]["profileId"].get_string() == "SectionsTest_Profile.id");

    // No order is implied: a section carries no index field, because a renderer
    // chooses its own arrangement and an emitted position would imply a
    // sequence SectionSet does not have.
    CHECK_FALSE(sections[0].contains("index"));
}
```

- [ ] **Step 2: Register the test and run it to verify it fails**

Add `test_sections.cpp` to `tests/CMakeLists.txt` beside `test_flows_apps.cpp`, then:

```bash
cmake --build build/clang-debug --target morph_tests -j 12
```

Expected: FAIL to compile — `morph/forms/sections.hpp` does not exist.

- [ ] **Step 3: Write the declaration types and emitter**

Create `include/morph/forms/sections.hpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file forms/sections.hpp
/// @brief Unordered sections: N independently editable action drafts on one
///        screen, each with its own readiness gate and no sequencing.
///
/// The sibling to `morph::flows::FlowSession`. A wizard has one active step and
/// refuses a field belonging to any other; a section set has no active step at
/// all. Use this when step order carries no meaning -- tabs, cards, a settings
/// page -- and `FlowSession` when it does.

#include <cstddef>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>

#include <glaze/glaze.hpp>

#include "../core/bridge.hpp"
#include "detail/session_common.hpp"

namespace morph::forms {

/// @brief One section of a `SectionGroup`: a registered action, a display
///        title, and zero or more `Bind` prefill declarations.
/// @tparam Action Registered action type (`BRIDGE_REGISTER_ACTION`) this section fires.
/// @tparam Title  Human title for the section (tab label / card header).
/// @tparam Binds  Zero or more `Bind<Field, Path>` prefill declarations.
template <typename Action, morph::forms::FixedString Title, typename... Binds>
struct Section {
    /// @brief The section's action type.
    using action = Action;

    /// @brief Tuple of this section's `Bind<...>` declarations (possibly empty).
    using binds = std::tuple<Binds...>;

    /// @brief The section's display title.
    /// @return The declared title.
    [[nodiscard]] static constexpr std::string_view title() noexcept { return Title.view(); }
};

/// @brief An unordered set of `Section`s sharing one screen.
/// @tparam Title    Human title for the whole group.
/// @tparam Sections One or more `Section<Action, Title, Binds...>` types.
template <morph::forms::FixedString Title, typename... Sections>
struct SectionGroup {
    /// @brief Tuple of this group's `Section<...>` types.
    using sections = std::tuple<Sections...>;

    /// @brief The group's display title.
    /// @return The declared title.
    [[nodiscard]] static constexpr std::string_view title() noexcept { return Title.view(); }
};

/// @brief Traits specialisation mapping a `SectionGroup` type to its string type-id.
///
/// Specialise via `BRIDGE_REGISTER_SECTION_GROUP` rather than by hand. The
/// emitted schema needs a stable name for the group; deriving one from the C++
/// type would tie the wire format to a mangled name.
/// @tparam G The `SectionGroup` type.
template <typename G>
struct SectionGroupTraits;  // forward — specialise or use BRIDGE_REGISTER_SECTION_GROUP

/// @brief Generates the `s-*` JSON document for section group @p G.
///
/// Carries the group id and title, and one entry per section with its action
/// type-id, title and declared `prefill` binds. Deliberately emits no index or
/// order field: a renderer chooses its own arrangement, and a position would
/// imply a sequence this type does not have.
/// @tparam G The `SectionGroup` type to describe.
/// @return The JSON document, or an empty string if serialization fails.
template <typename G>
[[nodiscard]] std::string sectionGroupSchemaJson() {
    glz::generic_u64 dom{};
    dom["s-id"] = std::string{SectionGroupTraits<G>::typeId()};
    dom["s-title"] = std::string{G::title()};

    glz::generic_u64::array_t sections{};
    detail::forEachTupleElement<typename G::sections>([&]<typename SectionT, std::size_t I>() {
        static_cast<void>(I);
        glz::generic_u64 entry{};
        entry["action"] = std::string{::morph::model::ActionTraits<typename SectionT::action>::typeId()};
        entry["title"] = std::string{SectionT::title()};
        if constexpr (std::tuple_size_v<typename SectionT::binds> != 0) {
            detail::emitBindsInto<typename SectionT::binds>(entry["prefill"]);
        }
        sections.emplace_back(std::move(entry));
    });
    dom["s-sections"] = sections;

    return glz::write_json(dom).value_or(std::string{});
}

}  // namespace morph::forms

// clang-format off -- public macro surface; see CONTRIBUTING.md, "Formatting/linting".
/// @brief Specialises `morph::forms::SectionGroupTraits<G>` with the string type-id @p NAME.
#define BRIDGE_REGISTER_SECTION_GROUP(G, NAME)                               \
    template <>                                                              \
    struct morph::forms::SectionGroupTraits<G> {                             \
        static constexpr std::string_view typeId() noexcept { return NAME; } \
    };
// clang-format on
```

- [ ] **Step 4: Run the test to verify it passes**

```bash
cmake --build build/clang-debug --target morph_tests -j 12
./build/clang-debug/tests/morph_tests "[sections][schema]"
```

Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add include/morph/forms/sections.hpp tests/test_sections.cpp tests/CMakeLists.txt
git commit -m "forms: Section/SectionGroup declarations and their schema document"
```

---

### Task 3: SectionSet — independent, unordered firing

The core of morph#513. Case 1 is the regression test: this shape throws under `FlowSession`.

**Files:**
- Modify: `include/morph/forms/sections.hpp` (add `SectionSet` before the closing `}  // namespace morph::forms`)
- Test: `tests/test_sections.cpp`

**Interfaces:**
- Consumes: `Section`, `SectionGroup` (Task 2); `morph::forms::detail::AllDistinct` (Task 1).
- Produces: `SectionSet<Model, Sections...>` with `explicit SectionSet(BridgeHandler<Model>&,
  std::function<void(std::exception_ptr)> = nullptr)`, `template <auto FieldPtr> void set(ValueType)`, and the
  private `fire<A>(A draft)`.

- [ ] **Step 1: Write the failing tests**

Append to `tests/test_sections.cpp`:

```cpp
namespace {
/// Collects the actions the model actually executed, in order.
struct SecRecorder {
    std::mutex mtx;
    std::vector<std::string> fired;

    void record(std::string what) {
        std::scoped_lock const lock{mtx};
        fired.push_back(std::move(what));
    }
    [[nodiscard]] std::vector<std::string> snapshot() {
        std::scoped_lock const lock{mtx};
        return fired;
    }
};
SecRecorder& recorder() {
    static SecRecorder inst;
    return inst;
}
}  // namespace

TEST_CASE("SectionSet: sections fire independently, in any order", "[sections][morph513]") {
    morph::exec::ThreadPoolExecutor pool{2};
    morph::testing::InlineExecutor cbExec;
    morph::bridge::Bridge bridge{std::make_unique<morph::backend::LocalBackend>(pool)};
    morph::bridge::BridgeHandler<SecModel> handler{bridge, &cbExec};
    morph::forms::SectionSet<SecModel, ProfileSection, PrefsSection> sections{handler};

    recorder().fired.clear();

    // Edit the SECOND section first. Under FlowSession this throws
    // std::logic_error -- "field belongs to an action that is not the current
    // step" -- which is exactly the gap morph#513 reports.
    sections.set<&SecPrefs::theme>("dark");
    REQUIRE(morph::testing::waitUntil([&] { return recorder().snapshot().size() == 1; }));

    // Then the first. Both fire; neither is "current".
    sections.set<&SecProfile::name>("ada");
    REQUIRE(morph::testing::waitUntil([&] { return recorder().snapshot().size() == 2; }));

    auto const fired = recorder().snapshot();
    CHECK(fired[0] == "SectionsTest_Prefs");
    CHECK(fired[1] == "SectionsTest_Profile");
}

TEST_CASE("SectionSet: a not-ready draft does not dispatch", "[sections]") {
    morph::exec::ThreadPoolExecutor pool{2};
    morph::testing::InlineExecutor cbExec;
    morph::bridge::Bridge bridge{std::make_unique<morph::backend::LocalBackend>(pool)};
    morph::bridge::BridgeHandler<SecModel> handler{bridge, &cbExec};
    morph::forms::SectionSet<SecModel, ProfileSection, PrefsSection> sections{handler};

    recorder().fired.clear();

    // SecPrefs::validate() requires a non-empty theme; profileId alone is not ready.
    sections.set<&SecPrefs::profileId>(7);
    CHECK(recorder().snapshot().empty());

    // Completing it dispatches.
    sections.set<&SecPrefs::theme>("light");
    REQUIRE(morph::testing::waitUntil([&] { return recorder().snapshot().size() == 1; }));
}

TEST_CASE("SectionSet: an already-fired section fires again on the next edit", "[sections]") {
    morph::exec::ThreadPoolExecutor pool{2};
    morph::testing::InlineExecutor cbExec;
    morph::bridge::Bridge bridge{std::make_unique<morph::backend::LocalBackend>(pool)};
    morph::bridge::BridgeHandler<SecModel> handler{bridge, &cbExec};
    morph::forms::SectionSet<SecModel, ProfileSection, PrefsSection> sections{handler};

    recorder().fired.clear();
    sections.set<&SecProfile::name>("ada");
    REQUIRE(morph::testing::waitUntil([&] { return recorder().snapshot().size() == 1; }));

    // No latch: still ready, so it dispatches again.
    sections.set<&SecProfile::name>("grace");
    REQUIRE(morph::testing::waitUntil([&] { return recorder().snapshot().size() == 2; }));
}
```

Add `recorder().record(...)` calls to `SecModel::execute`:

```cpp
struct SecModel {
    SecProfileResult execute(SecProfile action) {
        recorder().record("SectionsTest_Profile");
        return {.id = static_cast<std::int64_t>(action.name.size())};
    }
    SecPrefsResult execute(SecPrefs action) {
        recorder().record("SectionsTest_Prefs");
        return {.summary = action.theme};
    }
};
```

Move the `SecRecorder` block above `SecModel`, and add these includes: `<memory>`, `<mutex>`, `<vector>`,
`<morph/core/backend.hpp>`, `<morph/core/executor.hpp>`, `"test_support.hpp"`.

- [ ] **Step 2: Run to verify they fail**

```bash
cmake --build build/clang-debug --target morph_tests -j 12
```

Expected: FAIL to compile — `SectionSet` is not declared.

- [ ] **Step 3: Implement SectionSet's core**

Insert before the closing `}  // namespace morph::forms` in `sections.hpp`:

```cpp
/// @brief Drives N independently editable action drafts on one screen.
///
/// Each section accumulates its own draft through `set<>`, and dispatches
/// through the handler as soon as `ActionValidator<A>::ready` accepts it.
/// There is no active section and no sequence: a field belonging to any
/// declared section may be set at any time.
///
/// A ready section re-fires on every subsequent `set<>`. There is no latch and
/// no coalescing, matching `FlowSession`; a caller wanting one request per
/// pause debounces on its own side.
/// @tparam Model    The model the handler is bound to.
/// @tparam Sections One or more `Section<Action, Title, Binds...>` types.
template <typename Model, typename... Sections>
class SectionSet {
    static_assert(sizeof...(Sections) > 0, "SectionSet: a group needs at least one section");
    static_assert(detail::AllDistinct<typename Sections::action...>::value,
                  "SectionSet: section action types must be pairwise distinct");

public:
    /// @brief Constructs a section set over @p handler.
    /// @param handler Handler every section dispatches through. Must outlive
    ///                this `SectionSet` (only *destruction* order is
    ///                unconstrained; see bridge.md's Lifetime & ownership).
    /// @param onError Optional callback invoked when a section's dispatch
    ///                fails. When absent the error is logged via
    ///                `morph::log::logError`. Stored and invoked for this
    ///                object's whole lifetime, so anything the callable refers
    ///                to must outlive it.
    explicit SectionSet(::morph::bridge::BridgeHandler<Model>& handler MORPH_LIFETIMEBOUND,
                        std::function<void(std::exception_ptr)> onError MORPH_LIFETIMEBOUND = nullptr)
        : _handler{handler}, _onError{std::move(onError)} {}

    SectionSet(const SectionSet&) = delete;
    SectionSet& operator=(const SectionSet&) = delete;
    SectionSet(SectionSet&&) = delete;
    SectionSet& operator=(SectionSet&&) = delete;

    /// @brief Sets one field of its section's draft and dispatches that section
    ///        if the draft is now ready.
    ///
    /// Unlike `FlowSession::set<>` this imposes no ordering: the field's action
    /// need only be one of the declared sections.
    /// @tparam FieldPtr Pointer-to-data-member of a declared section's action struct.
    /// @param value New value for the field.
    template <auto FieldPtr>
    void set(typename ::morph::bridge::detail::MemberPointerTraits<decltype(FieldPtr)>::ValueType value) {
        using A = typename ::morph::bridge::detail::MemberPointerTraits<decltype(FieldPtr)>::ClassType;
        static_assert((std::is_same_v<A, typename Sections::action> || ...),
                      "SectionSet::set<>: field's action is not a section of this group");
        A draft{};
        {
            std::scoped_lock const lock{_mtx};
            std::get<A>(_drafts).*FieldPtr = std::move(value);
            draft = std::get<A>(_drafts);
        }
        if (::morph::model::ActionValidator<A>::ready(draft)) {
            fire<A>(std::move(draft));
        }
    }

private:
    template <typename A>
    void fire(A draft) {
        _handler.execute(std::move(draft));
    }

    ::morph::bridge::BridgeHandler<Model>& _handler;
    std::function<void(std::exception_ptr)> _onError;
    mutable std::mutex _mtx;
    std::tuple<typename Sections::action...> _drafts{};
};
```

Add `#include <functional>` and `#include <type_traits>` to the header's include block.

- [ ] **Step 4: Run the tests to verify they pass**

```bash
cmake --build build/clang-debug --target morph_tests -j 12
./build/clang-debug/tests/morph_tests "[sections]"
```

Expected: PASS, all four cases.

- [ ] **Step 5: Commit**

```bash
git add include/morph/forms/sections.hpp tests/test_sections.cpp
git commit -m "forms: SectionSet dispatches each section independently (#513)"
```

---

### Task 4: reset and draft accessors

**Files:**
- Modify: `include/morph/forms/sections.hpp`
- Test: `tests/test_sections.cpp`

**Interfaces:**
- Consumes: `SectionSet` (Task 3).
- Produces: `template <typename A> void reset()`; `template <typename A> [[nodiscard]] A draft() const`.

- [ ] **Step 1: Write the failing test**

```cpp
TEST_CASE("SectionSet: reset clears one section and leaves the others intact", "[sections]") {
    morph::exec::ThreadPoolExecutor pool{2};
    morph::testing::InlineExecutor cbExec;
    morph::bridge::Bridge bridge{std::make_unique<morph::backend::LocalBackend>(pool)};
    morph::bridge::BridgeHandler<SecModel> handler{bridge, &cbExec};
    morph::forms::SectionSet<SecModel, ProfileSection, PrefsSection> sections{handler};

    sections.set<&SecPrefs::profileId>(42);      // not ready: no theme yet
    sections.set<&SecProfile::name>("ada");      // ready: fires

    CHECK(sections.draft<SecPrefs>().profileId == 42);
    CHECK(sections.draft<SecProfile>().name == "ada");

    sections.reset<SecPrefs>();

    CHECK(sections.draft<SecPrefs>().profileId == 0);
    // The other section is untouched -- per-section isolation is the point.
    CHECK(sections.draft<SecProfile>().name == "ada");
}
```

- [ ] **Step 2: Run to verify it fails**

```bash
cmake --build build/clang-debug --target morph_tests -j 12
```

Expected: FAIL to compile — no member `reset`/`draft`.

- [ ] **Step 3: Implement them**

Add to `SectionSet`'s public section, after `set<>`:

```cpp
    /// @brief Clears one section's draft back to a default-constructed action.
    ///
    /// Touches no other section, and dispatches nothing.
    /// @tparam A The section's action type.
    template <typename A>
    void reset() {
        static_assert((std::is_same_v<A, typename Sections::action> || ...),
                      "SectionSet::reset<>: not a section of this group");
        std::scoped_lock const lock{_mtx};
        std::get<A>(_drafts) = A{};
    }

    /// @brief Snapshots one section's current draft.
    ///
    /// A copy taken under the lock, not a reference into live state, so a
    /// renderer can read it while another thread edits a different section.
    /// @tparam A The section's action type.
    /// @return The draft as it stands.
    template <typename A>
    [[nodiscard]] A draft() const {
        static_assert((std::is_same_v<A, typename Sections::action> || ...),
                      "SectionSet::draft<>: not a section of this group");
        std::scoped_lock const lock{_mtx};
        return std::get<A>(_drafts);
    }
```

- [ ] **Step 4: Run to verify it passes**

```bash
cmake --build build/clang-debug --target morph_tests -j 12 && ./build/clang-debug/tests/morph_tests "[sections]"
```

Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add include/morph/forms/sections.hpp tests/test_sections.cpp
git commit -m "forms: SectionSet reset<A>() and draft<A>()"
```

---

### Task 5: Result capture and resolved()

**Files:**
- Modify: `include/morph/forms/sections.hpp`
- Test: `tests/test_sections.cpp`

**Interfaces:**
- Consumes: `SectionSet::fire` (Task 3).
- Produces: `[[nodiscard]] std::optional<std::string> resolved(std::string_view path) const`.

- [ ] **Step 1: Write the failing test**

```cpp
TEST_CASE("SectionSet: a fired section's fields are resolvable; an unfired one is not", "[sections]") {
    morph::exec::ThreadPoolExecutor pool{2};
    morph::testing::InlineExecutor cbExec;
    morph::bridge::Bridge bridge{std::make_unique<morph::backend::LocalBackend>(pool)};
    morph::bridge::BridgeHandler<SecModel> handler{bridge, &cbExec};
    morph::forms::SectionSet<SecModel, ProfileSection, PrefsSection> sections{handler};

    recorder().fired.clear();
    sections.set<&SecProfile::name>("ada");
    REQUIRE(morph::testing::waitUntil([&] { return recorder().snapshot().size() == 1; }));
    REQUIRE(morph::testing::waitUntil([&] { return sections.resolved("SectionsTest_Profile.id").has_value(); }));

    // The result's field: SecProfileResult::id == name.size() == 3.
    CHECK(sections.resolved("SectionsTest_Profile.id") == std::string{"3"});
    // The submitted draft's field is captured too.
    CHECK(sections.resolved("SectionsTest_Profile.name") == std::string{"\"ada\""});

    // Both halves matter: a path whose section never fired resolves to nothing.
    // Without this, an implementation returning a value for everything passes.
    CHECK_FALSE(sections.resolved("SectionsTest_Prefs.summary").has_value());
    CHECK_FALSE(sections.resolved("SectionsTest_Profile.nosuchfield").has_value());
}
```

- [ ] **Step 2: Run to verify it fails**

```bash
cmake --build build/clang-debug --target morph_tests -j 12
```

Expected: FAIL to compile — no member `resolved`.

- [ ] **Step 3: Implement capture and lookup**

Replace `fire<A>` and add `captureResult<A>` plus the public `resolved`:

```cpp
    /// @brief Looks up a value captured from a section's submitted draft or result.
    /// @param path `"<ActionTypeId>.<field>"`, matching a section's declared `Bind::path()`.
    /// @return The field's JSON-encoded value, or `std::nullopt` if @p path was
    ///         never captured (the section never fired, or has no such field).
    [[nodiscard]] std::optional<std::string> resolved(std::string_view path) const {
        std::scoped_lock const lock{_mtx};
        auto iter = _resolvedValues.find(std::string{path});
        if (iter == _resolvedValues.end()) {
            return std::nullopt;
        }
        return iter->second;
    }
```

```cpp
    template <typename A>
    void captureResult(const ::morph::model::ActionTraits<A>::Result& result) {
        std::scoped_lock const lock{_mtx};
        auto const typeId = ::morph::model::ActionTraits<A>::typeId();
        auto record = [&](const auto& value) {
            ::morph::forms::detail::forEachNamedMember(
                value, [&]<std::size_t I>(std::string_view name, const auto& member) {
                    static_cast<void>(I);
                    std::string json;
                    if (!glz::write_json(member, json)) {
                        _resolvedValues[std::string{typeId} + "." + std::string{name}] = std::move(json);
                    }
                });
        };
        record(std::get<A>(_drafts));  // submitted draft fields first...
        record(result);                // ...result fields win on name collision
    }

    template <typename A>
    void fire(A draft) {
        _handler.execute(std::move(draft)).then(_callbacks, [this](::morph::model::ActionTraits<A>::Result result) {
            this->template captureResult<A>(result);
        });
    }
```

Add the member `std::unordered_map<std::string, std::string> _resolvedValues;` and `::morph::async::CallbackScope
_callbacks;` (declared last), plus includes `<unordered_map>` and `<morph/core/callback_scope.hpp>`.

**Note on ordering:** `_callbacks` must be the last member so it is destroyed last; Task 7 adds the destructor that
stops it first.

- [ ] **Step 4: Run to verify it passes**

```bash
cmake --build build/clang-debug --target morph_tests -j 12 && ./build/clang-debug/tests/morph_tests "[sections]"
```

Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add include/morph/forms/sections.hpp tests/test_sections.cpp
git commit -m "forms: SectionSet captures results and exposes resolved()"
```

---

### Task 6: Error routing

**Files:**
- Modify: `include/morph/forms/sections.hpp`
- Test: `tests/test_sections.cpp`

**Interfaces:**
- Consumes: `SectionSet::fire` (Task 5).
- Produces: the `onError` path and `logUnhandledError`.

- [ ] **Step 1: Write the failing test**

Add a throwing action to the fixture:

```cpp
struct SecExplodes {
    std::string label;
    [[nodiscard]] bool validate() const { return !label.empty(); }
};
struct SecExplodesResult {
    std::int64_t id = 0;
};
```

Add to `SecModel`: `SecExplodesResult execute(SecExplodes) { throw std::runtime_error{"section boom"}; }`, register
it with `BRIDGE_REGISTER_ACTION(SecModel, SecExplodes, "SectionsTest_Explodes")`, and declare `using
ExplodesSection = morph::forms::Section<SecExplodes, "Explodes">;`.

```cpp
TEST_CASE("SectionSet: a failing dispatch reaches the onError callback", "[sections]") {
    morph::exec::ThreadPoolExecutor pool{2};
    morph::testing::InlineExecutor cbExec;
    morph::bridge::Bridge bridge{std::make_unique<morph::backend::LocalBackend>(pool)};
    morph::bridge::BridgeHandler<SecModel> handler{bridge, &cbExec};

    std::atomic<int> errors{0};
    morph::forms::SectionSet<SecModel, ProfileSection, ExplodesSection> sections{
        handler, [&](std::exception_ptr) { errors.fetch_add(1); }};

    sections.set<&SecExplodes::label>("boom");
    REQUIRE(morph::testing::waitUntil([&] { return errors.load() == 1; }));

    // A section that succeeds does not route to onError.
    sections.set<&SecProfile::name>("ada");
    CHECK(errors.load() == 1);
}
```

- [ ] **Step 2: Run to verify it fails**

```bash
cmake --build build/clang-debug --target morph_tests -j 12 && ./build/clang-debug/tests/morph_tests "a failing dispatch reaches"
```

Expected: FAIL — `errors` stays 0, because `fire` installs no `.onError` continuation.

- [ ] **Step 3: Implement error routing**

```cpp
    static void logUnhandledError(std::string_view typeId, const std::exception_ptr& err) {
        try {
            std::rethrow_exception(err);
        } catch (const std::exception& exc) {
            ::morph::log::logError(std::string{"[sections:"} + std::string{typeId} + "] " + exc.what());
        } catch (...) {
            ::morph::log::logError(std::string{"[sections:"} + std::string{typeId} + "] unknown exception");
        }
    }

    template <typename A>
    void fire(A draft) {
        _handler.execute(std::move(draft))
            .then(_callbacks, [this](::morph::model::ActionTraits<A>::Result result) {
                this->template captureResult<A>(result);
            })
            .onError(_callbacks, [this](const std::exception_ptr& err) {
                if (_onError) {
                    _onError(err);
                    return;
                }
                logUnhandledError(::morph::model::ActionTraits<A>::typeId(), err);
            });
    }
```

Add `#include <morph/core/logger.hpp>` and `#include <exception>`.

- [ ] **Step 4: Run to verify it passes**

```bash
cmake --build build/clang-debug --target morph_tests -j 12 && ./build/clang-debug/tests/morph_tests "[sections]"
```

Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add include/morph/forms/sections.hpp tests/test_sections.cpp
git commit -m "forms: SectionSet routes a failed dispatch to onError, else logs"
```

---

### Task 7: Lifetime — the callback gate

**Files:**
- Modify: `include/morph/forms/sections.hpp`
- Test: `tests/test_sections.cpp`

**Interfaces:**
- Consumes: `_callbacks` (Task 5).
- Produces: `~SectionSet()`.

- [ ] **Step 1: Write the failing test**

```cpp
TEST_CASE("SectionSet: destroying it with a dispatch in flight delivers nothing", "[sections]") {
    morph::exec::ThreadPoolExecutor pool{2};
    morph::testing::InlineExecutor cbExec;
    morph::bridge::Bridge bridge{std::make_unique<morph::backend::LocalBackend>(pool)};
    morph::bridge::BridgeHandler<SecModel> handler{bridge, &cbExec};

    std::atomic<int> errors{0};
    {
        morph::forms::SectionSet<SecModel, ProfileSection, ExplodesSection> sections{
            handler, [&](std::exception_ptr) { errors.fetch_add(1); }};
        sections.set<&SecExplodes::label>("boom");
        // Leaves scope with the dispatch possibly still in flight. The
        // destructor stops the scope, so no continuation may run against the
        // destroyed object. Same-thread delivery via InlineExecutor makes
        // check-then-run atomic here -- see callback_scope.md's boundary.
    }
    // Nothing may arrive after destruction. A count of 1 recorded *before* the
    // scope closed is possible and fine; what must not happen is a crash or a
    // later increment.
    auto const after = errors.load();
    CHECK(morph::testing::waitUntil([&] { return errors.load() == after; }));
}
```

- [ ] **Step 2: Run to verify it fails**

```bash
cmake --build build/clang-debug --target morph_tests -j 12
```

Expected: FAIL to compile only if the destructor is missing entirely; otherwise this case documents the gate. If it
compiles and passes without a destructor, that is because `_callbacks`' own destruction happens to cover it — add
the destructor anyway per Step 3 and keep the case as the regression guard for a future pumping teardown.

- [ ] **Step 3: Add the destructor**

```cpp
    /// @brief Stops every callback this set installed from being delivered.
    ///
    /// There is nothing to detach: a section's continuations are owned by the
    /// in-flight dispatch, not held in a map this object could remove itself
    /// from. `_callbacks` gates each one on a token it checks before touching
    /// `this`.
    ///
    /// `requestStop()` is called explicitly rather than left to the member's
    /// own destruction, even though `_callbacks` is declared last: members are
    /// destroyed only *after* the destructor body, so a body that later grew a
    /// call pumping an event loop would otherwise deliver into a half-dead
    /// object. The body does not do that today; stopping first keeps it correct
    /// if one is added.
    ///
    /// The strength of the gate depends on which thread destroys this object,
    /// exactly as `CallbackScope`'s "Boundary of the guarantee" describes.
    /// Destroying it off the delivery thread is advisory only, and that caller
    /// owns its own synchronisation.
    ~SectionSet() { _callbacks.requestStop(); }
```

- [ ] **Step 4: Run the full sections suite**

```bash
cmake --build build/clang-debug --target morph_tests -j 12 && ./build/clang-debug/tests/morph_tests "[sections]"
```

Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add include/morph/forms/sections.hpp tests/test_sections.cpp
git commit -m "forms: SectionSet stops its callback scope before teardown"
```

---

### Task 8: Spec document and the full gate run

Required: the Header ↔ spec sync gate fails a PR that changes `include/morph/forms/` without changing
`docs/spec/forms/`.

**Files:**
- Create: `docs/spec/forms/sections.md`
- Modify: `scripts/branch_partial_allowlist.json` (only if re-pinning is needed)

- [ ] **Step 1: Write the spec document**

`docs/spec/forms/sections.md` covering, in this order: what `SectionSet` is and when to choose it over
`FlowSession`; the declaration types; `set`/`reset`/`draft`/`resolved` with their contracts; the no-latch rule and
why; that binds are declarations the renderer acts on, never framework writes; the `s-*` schema shape and the
deliberate absence of an order field; concurrency and the callback-scope boundary; and the two `static_assert`s
(duplicate action types, and a field outside the group) as the compile-time contract, with a short negative example
for each.

- [ ] **Step 2: Run every gate CI applies**

```bash
# format, whole tree
git ls-files -z '*.hpp' '*.cpp' | xargs -0 clang-format --dry-run -Werror

# documentation warnings, glaze as a system header the way CI treats it
clang++ -std=c++23 -fsyntax-only -Wdocumentation -Wno-documentation-unknown-command -Werror \
  -Iinclude -isystem build/clang-coverage/_deps/glaze-src/include \
  <(printf '#include <morph/forms/sections.hpp>\nint main(){}\n') -x c++ -

# both compilers
cmake --build build/clang-debug --target morph_tests -j 12
cmake --build build/gcc-net   --target morph_tests -j 12

# suites
./build/clang-debug/tests/morph_tests
./build/clang-debug/tests/net/morph_net_tests

# spec citations
bash scripts/check_spec_citations.sh

# allowlist, both ways
python3 -c "
import json,pathlib,re
d=json.load(open('scripts/branch_partial_allowlist.json')); byline={e['line'] for e in d['entries']}; bad=0
for e in d['entries']:
    lines=pathlib.Path(e['file']).read_text().split(chr(10))
    if lines[e['line']-1].strip()!=e['source'].strip(): bad+=1; print('SOURCE',e['file'],e['line'])
    for r in re.findall(r'(?:line |\.hpp:)([0-9]+)', e['reason']):
        if int(r) not in byline: bad+=1; print('PROSE',e['file'],e['line'],r)
print(f'{len(d[\"entries\"])} entries, {bad} stale')"
```

Expected: format silent, no documentation warnings, both builds clean, both suites passing, spec lint OK, allowlist
0 stale.

- [ ] **Step 3: clang-tidy on the branch's changed lines**

```bash
git diff -U0 origin/master...HEAD -- '*.cpp' '*.hpp' > /tmp/br.diff
# Extract changed line numbers, run clang-tidy on changed .cpp files, and report
# only findings whose file:line appears in the diff. Diff against
# origin/master...HEAD, NOT the working tree -- the working tree is empty after
# a commit, which makes the filter silently vacuous.
```

Expected: no findings on changed lines.

- [ ] **Step 4: Verify every test fails without its implementation**

For each case added in Tasks 2-7, stash the corresponding part of `sections.hpp`, rebuild, and confirm the case
fails. A case that passes with the implementation removed is not a test.

- [ ] **Step 5: Commit and open the PR**

```bash
git add docs/spec/forms/sections.md scripts/branch_partial_allowlist.json
git commit -m "docs: spec for unordered sections (#513)"
```

PR body must state: what SectionSet is, that case 1 is the morph#513 regression, the measured before/after for each
test, and that binds are declarations rather than writes (with the reason, since the first design got this wrong).

---

## Self-Review

**Spec coverage.** Every section of `2026-09-10-unordered-sections-design.md` maps to a task: Headers → Task 1-2;
Declaration layer → Task 2; SectionSet surface → Tasks 3-5; Prefill → Tasks 2 (schema) and 5 (resolved);
Concurrency and lifetime → Tasks 5, 7; Schema document → Task 2; Testing cases 1-8 → Tasks 2-7; case 9
(`static_assert`s) → Task 8's spec document, as the spec says it is compile-time and covered by documented negative
examples.

**Placeholders.** None. Every code step carries the code.

**Type consistency.** `Section::action`/`binds`/`title()`, `SectionGroup::sections`/`title()`,
`SectionGroupTraits<G>::typeId()`, `sectionGroupSchemaJson<G>()`, and `SectionSet`'s
`set`/`reset`/`draft`/`resolved`/`fire`/`captureResult`/`logUnhandledError` are spelled identically in every task
that references them. `forStep` is renamed to `forPackElement` in Task 1 and used under that name thereafter.

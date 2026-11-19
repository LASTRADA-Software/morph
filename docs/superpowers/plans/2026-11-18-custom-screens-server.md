# Custom Screens — Server Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A server can store a person's custom screens behind an application-implemented store, check every custom
document against its base screen and the person's permissions, and serve them through a framework model,
`morph.customScreens`.

**Architecture:** Two free functions are lifted out of `UiService` (`permittedActions`, `screenAdmitted`) so the model
answers the same permission set the catalog does. A pure check pipeline (`checkCustomScreen`) runs the bounds, the base,
the application screen checks, the accessibility rules, the base rule and the permission check, with a bounded verdict
cache. `CustomScreensModel` follows `PreferencesModel`'s pattern: the person comes from the verified session, the store
holds the policy. Its record and effects are registered statically in the model's own translation unit.

**Tech Stack:** C++23, glaze (JSON), Catch2, CMake + Ninja, clang (local), GCC 15 and MSVC in CI.

Spec: `docs/superpowers/specs/2026-11-18-custom-screens-design.md`. This is plan 1 of 3. Plans 2 (the client) and 3
(the assistant) build on the names this plan produces.

## Global Constraints

- Public symbols carry complete Doxygen `@brief`/`@param`/`@tparam`/`@return`. The Docs workflow runs with
  `WARN_AS_ERROR = FAIL_ON_WARNINGS`, and clang's `-Wdocumentation` catches a misplaced `@param` that GCC misses.
- Comments and docs state what the code does now and why. No history, no issue numbers, no commit hashes (AGENTS.md).
- Never use `slots`, `signals` or `emit` as identifiers under `include/morph`. They break only Qt translation units.
- A dependent default template argument needs `typename` (MSVC C4346). This plan adds no such template. Keep it that
  way.
- No `std::expected`-returning lambda with `auto*` fix-its from clang-tidy. Write the types out (the MSVC fix-it trap).
- Every compile (cmake configure, cmake build, ninja, a g++ sweep) runs under the lane's build lock with `-j5`:
  `flock "$LOCK" <command>`. `$LOCK` is the slot the dispatcher assigned:
  `/tmp/claude-1000/-home-yaraslau-repo-morph/843fe608-d0d7-4f4a-ad5a-4e74db79266e/scratchpad/morph-build.lock` or
  `…/morph-build-2.lock`. Test binaries run as `flock "$LOCK" timeout 300 <binary> <tags>`. clang-tidy (at most 3
  files at a time) and `ctest -j6` run without the lock. Hold the lock for one command at a time, never across edits.
- Let `CompileCache.cmake` pick fastcache-cc. Never set `CMAKE_CXX_COMPILER_LAUNCHER=ccache`.
- Build directory for this worktree: `build/clang-debug` (preset `clang-debug`). The test binary is
  `build/clang-debug/tests/morph_tests`.
- After restoring a hand-mutated file from a backup, `touch` it, or ninja keeps the mutant object.
- Action ids are process-global and checked for clashes once the effects registry resolves. Every action id of this
  model is prefixed `morph.customScreens.`, so no application's own `list` or `save` can clash with it.
- Defaults copied from the spec: `maxDocumentBytes` 256 KiB; ids start `custom:`; the record type is
  `morph.customScreens`; the model type is `morph.customScreens`.

## File Structure

| File | Responsibility |
|---|---|
| `include/morph/ui/service.hpp` (modify) | `permittedActions`, `screenAdmitted`, `kCustomScreenIdPrefix`, `ServedBundle::catalog` |
| `src/ui/service.cpp` (modify) | Uses the two functions; refuses a `custom:` application id; keeps the catalog on the bundle |
| `src/ui/document_actions.hpp` (create, private) | `detail::DocumentActions`, moved out of `service.cpp`, now also recording where each action is first named |
| `include/morph/ui/custom_screens.hpp` (create) | Constants, the store seam and memory store, options, the check functions, `VerdictCache`, the wire types, the model, its registration |
| `src/ui/custom_screen_checks.cpp` (create) | The base rule, the id rewrite, the verdict cache and key, the check pipeline |
| `src/ui/custom_screens.cpp` (create) | The model's actions, `registerCustomScreens`, the static record, result and effects registration |
| `CMakeLists.txt` (modify) | The two new sources in `add_library(morph …)` |
| `tests/ui_custom_screens_support.hpp` (create) | The base screen fixture, a registry and bundle with it, a server offering the model |
| `tests/test_ui_custom_screen_rules.cpp` (create) | The base rule, one case per row |
| `tests/test_ui_custom_screen_checks.cpp` (create) | The pipeline, one case per check, the id rewrite and the verdict cache |
| `tests/test_ui_custom_screens.cpp` (create) | The memory store, and the model through a `RemoteServer` |
| `tests/test_ui_service.cpp` (modify) | `permittedActions`, `screenAdmitted`, the reserved prefix, the kept catalog |
| `tests/CMakeLists.txt` (modify) | The three new test files |
| `docs/spec/ui/custom_screens.md` (create) | The subsystem's spec page |
| `docs/spec/ui/service.md`, `docs/spec/README.md` (modify) | The new functions and prefix; the index line |

---

### Task 0: Configure the worktree build

**Files:** none.

- [ ] **Step 1: Configure**

Run: `cd /home/yaraslau/repo/morph-master && flock "$LOCK" cmake --preset clang-debug`
Expected: `-- Build files have been written to: …/build/clang-debug`.

- [ ] **Step 2: Build the tests once, so later steps measure only this plan's changes**

Run: `flock "$LOCK" cmake --build build/clang-debug --target morph_tests -j5`
Expected: ends with `Linking CXX executable tests/morph_tests`, no errors.

---

### Task 1: `permittedActions` and `screenAdmitted`

**Files:**
- Modify: `include/morph/ui/service.hpp` (after `struct ServedBundle`, and the private `screenAdmits` declaration near line 509)
- Modify: `src/ui/service.cpp` (`catalogAnswer` near line 884, the `screenAdmits` call near line 993, its definition near line 1003)
- Test: `tests/test_ui_service.cpp`

**Interfaces:**
- Produces:
  - `std::vector<std::string> morph::ui::permittedActions(ServedBundle const& bundle, session::Context const& session, session::IAuthorizer const& authorizer)`
  - `bool morph::ui::screenAdmitted(ServedScreen const& screen, session::Context const& session, session::IAuthorizer const& authorizer)`

- [ ] **Step 1: Write the failing test** (append to `tests/test_ui_service.cpp`)

```cpp
TEST_CASE("permittedActions answers ui-catalog's permitted, and screenAdmitted its admission", "[ui-service][catalog]") {
    auto const bundle = uiservice::bundle();
    auto const authorizer = uiservice::authorizer();
    morph::session::Context tech;
    tech.principal = "tech";

    std::vector<std::string> const permitted = morph::ui::permittedActions(*bundle, tech, *authorizer);
    CHECK(std::ranges::find(permitted, std::string{"Lims_List"}) != permitted.end());
    CHECK(std::ranges::find(permitted, std::string{"Lims_Pending"}) == permitted.end());
    CHECK(std::ranges::find(permitted, std::string{"Lims_Verify"}) == permitted.end());

    auto const screen = [&bundle](std::string_view id) -> morph::ui::ServedScreen const& {
        auto const found = std::ranges::find(bundle->screens, id, [](auto const& s) { return s.entry.id; });
        REQUIRE(found != bundle->screens.end());
        return *found;
    };
    CHECK(morph::ui::screenAdmitted(screen("lims.results"), tech, *authorizer));
    CHECK_FALSE(morph::ui::screenAdmitted(screen("lims.verify"), tech, *authorizer));
    CHECK_FALSE(morph::ui::screenAdmitted(screen("lims.billing"), tech, *authorizer));
}
```

Add `#include <algorithm>` and `#include <vector>` at the top if the file does not already have them.

- [ ] **Step 2: Run it to verify it fails**

Run: `flock "$LOCK" cmake --build build/clang-debug --target morph_tests -j5`
Expected: compile error, `no member named 'permittedActions' in namespace 'morph::ui'`.

- [ ] **Step 3: Declare the functions** in `include/morph/ui/service.hpp`, directly after `struct ServedBundle { … };`

```cpp
/// @brief The actions of @p bundle that the principal of @p session may run: what `ui-catalog` answers as
///        `permitted`.
///
/// The candidates are `ServedBundle::allActions`, so an action no served document names is never permitted here,
/// whatever the authoriser would say about it.
/// @param bundle     The bundle.
/// @param session    The session, its principal verified.
/// @param authorizer The authoriser.
/// @return The permitted action ids, in `allActions` order.
[[nodiscard]] MORPH_API std::vector<std::string> permittedActions(ServedBundle const& bundle,
                                                                  ::morph::session::Context const& session,
                                                                  ::morph::session::IAuthorizer const& authorizer);

/// @brief Whether the principal of @p session may open @p screen: it is available, and the principal may run every
///        query the screen declares at its top level (spec 5 §12b).
/// @param screen     The screen.
/// @param session    The session, its principal verified.
/// @param authorizer The authoriser.
/// @return Whether the screen admits the principal.
[[nodiscard]] MORPH_API bool screenAdmitted(ServedScreen const& screen, ::morph::session::Context const& session,
                                            ::morph::session::IAuthorizer const& authorizer);
```

Delete the private `static bool screenAdmits(...)` declaration and its doc comment from `class UiService`.

- [ ] **Step 4: Define them** in `src/ui/service.cpp`, in `namespace morph::ui` after the anonymous namespace (before
  `UiRegistry::addScreen`)

```cpp
std::vector<std::string> permittedActions(ServedBundle const& bundle, ::morph::session::Context const& session,
                                          ::morph::session::IAuthorizer const& authorizer) {
    std::vector<std::string> permitted;
    for (ActionRef const& ref : bundle.allActions) {
        if (authorizer.authorize(session, ref.modelType, ref.action)) {
            permitted.push_back(ref.action);
        }
    }
    return permitted;
}

bool screenAdmitted(ServedScreen const& screen, ::morph::session::Context const& session,
                    ::morph::session::IAuthorizer const& authorizer) {
    return screen.entry.available && permitsAll(authorizer, session, screen.topQueries);
}
```

In `UiService::catalogAnswer`, replace the `for (ActionRef const& ref : _bundle->allActions) { … }` loop with:

```cpp
    reply.permitted = permittedActions(*_bundle, request.session, authorizer);
```

In `fetchAnswer`, replace `screenAdmits(` with `screenAdmitted(`. Delete the definition
`bool UiService::screenAdmits(...) { … }` at the end of the file.

- [ ] **Step 5: Run the tests**

Run: `flock "$LOCK" cmake --build build/clang-debug --target morph_tests -j5 && flock "$LOCK" timeout 300 build/clang-debug/tests/morph_tests "[ui-service]"`
Expected: `All tests passed`.

- [ ] **Step 6: Commit**

```bash
git add include/morph/ui/service.hpp src/ui/service.cpp tests/test_ui_service.cpp
git commit -m "ui: permittedActions and screenAdmitted as free functions"
```

---

### Task 2: The bundle keeps its catalog; `custom:` is reserved

**Files:**
- Modify: `include/morph/ui/service.hpp` (`struct ServedBundle`; a new constant before it)
- Modify: `src/ui/service.cpp` (`checkEntry`, `UiRegistry::addFacts`)
- Test: `tests/test_ui_service.cpp`

**Interfaces:**
- Produces: `inline constexpr std::string_view morph::ui::kCustomScreenIdPrefix = "custom:"`; `Catalog ServedBundle::catalog`.

- [ ] **Step 1: Write the failing tests** (append to `tests/test_ui_service.cpp`)

```cpp
TEST_CASE("UiRegistry refuses an application screen id in the custom screens' namespace", "[ui-service][registry]") {
    UiRegistry registry{morph::ui::ApplicationInfo{.id = "lims", .version = "1.0.0"}, uiservice::facts()};
    std::string text{uiservice::kResults};
    text.replace(text.find("lims.results"), std::string_view{"lims.results"}.size(), "custom:results");
    auto const screen = registry.addScreen(entry("custom:results"), text);
    REQUIRE_FALSE(screen);
    CHECK(screen.error().reason.contains("`custom:`"));

    CatalogEntry host = entry("custom:scanner");
    host.host = "Scanner";
    auto const listed = registry.addEntry(host);
    REQUIRE_FALSE(listed);
    CHECK(listed.error().reason.contains("`custom:`"));
}

TEST_CASE("ServedBundle keeps the catalog its documents were loaded with", "[ui-service][registry]") {
    auto const bundle = uiservice::bundle();
    REQUIRE(bundle->catalog.action);
    auto const info = bundle->catalog.action("Lims_List");
    REQUIRE(info);
    CHECK(info->modelType == "Lims_Sample");
    REQUIRE(bundle->catalog.model);
    CHECK(bundle->catalog.model("Lims_Sample").has_value());
}
```

- [ ] **Step 2: Run them to verify they fail**

Run: `flock "$LOCK" cmake --build build/clang-debug --target morph_tests -j5`
Expected: compile error, `no member named 'catalog' in 'morph::ui::ServedBundle'`.

- [ ] **Step 3: Add the constant and the member** in `include/morph/ui/service.hpp`. Before `struct ServedBundle`:

```cpp
/// @brief The prefix of every custom screen's id. An application screen's id never starts with it, so a person's
///        screen and an application's can never share an id.
inline constexpr std::string_view kCustomScreenIdPrefix = "custom:";
```

Inside `struct ServedBundle`, after `wizards`:

```cpp
    /// @brief The catalog the registry loaded every document with, so a document checked after the build, such as
    ///        a person's custom screen, is loaded against the same facts.
    Catalog catalog;
```

- [ ] **Step 4: Refuse the prefix and keep the catalog** in `src/ui/service.cpp`. In `checkEntry`, directly after the
  empty-id check:

```cpp
    if (entry.id.starts_with(kCustomScreenIdPrefix)) {
        return "`custom:` begins the id of a person's custom screen, never an application screen's: `" + entry.id +
               "`";
    }
```

At the top of `void UiRegistry::addFacts(ServedBundle& bundle) const`:

```cpp
    bundle.catalog = _catalog;
```

- [ ] **Step 5: Run the tests**

Run: `flock "$LOCK" cmake --build build/clang-debug --target morph_tests -j5 && flock "$LOCK" timeout 300 build/clang-debug/tests/morph_tests "[ui-service]"`
Expected: `All tests passed`.

- [ ] **Step 6: Mutation check.** Comment out the `starts_with` check, rebuild, and run `[ui-service]`. The prefix test
  must fail. Restore it, `touch src/ui/service.cpp`, rebuild, and run again: it passes.

- [ ] **Step 7: Commit**

```bash
git add include/morph/ui/service.hpp src/ui/service.cpp tests/test_ui_service.cpp
git commit -m "ui: the bundle keeps its catalog, and custom: is no application screen's id"
```

---

### Task 3: The base rule

**Files:**
- Create: `src/ui/document_actions.hpp`
- Modify: `src/ui/service.cpp` (delete `class DocumentActions` from the anonymous namespace, include the new header)
- Create: `include/morph/ui/custom_screens.hpp` (this task writes its first part)
- Create: `src/ui/custom_screen_checks.cpp`
- Modify: `CMakeLists.txt` (`add_library(morph …)` sources)
- Create: `tests/ui_custom_screens_support.hpp`, `tests/test_ui_custom_screen_rules.cpp`
- Modify: `tests/CMakeLists.txt`

**Interfaces:**
- Produces:
  - `morph::ui::detail::DocumentActions` (private header): `explicit DocumentActions(Document const&)`,
    `topQueries()`, `actions()`, `modelTypes()`, and `std::string const& pathOf(ActionRef const&) const`
  - `std::optional<Refusal> morph::ui::checkBaseRule(Document const& base, std::string_view baseText, Document const& custom, std::string_view customText)`
  - Test helpers `screens_test::baseScreen(std::string_view id)`, `screens_test::load(std::string const&)`

- [ ] **Step 1: Move `DocumentActions` into a private header that also records paths.** Create
  `src/ui/document_actions.hpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0

#pragma once
#include <functional>
#include <map>
#include <morph/ui/document.hpp>
#include <morph/ui/service.hpp>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace morph::ui::detail {

// NOLINTBEGIN(misc-no-recursion): view nodes nest, and the walk follows them; parseJson bounds the depth at 64.

/// @brief Collects the actions and model types a loaded document names, resolving each query's, mutation's and
///        model-bound node's alias through the enclosing scopes, and where each action is first named.
///
/// Load-time validation has already resolved every alias and requires every alias to name a type, so each lookup
/// here finds one.
class DocumentActions {
public:
    /// @brief Walks @p document.
    /// @param document The loaded document.
    explicit DocumentActions(Document const& document) {
        enter(document.scope, true);
        if (document.view) {
            node(*document.view);
        }
    }

    /// @brief The top-level queries.
    /// @return Them, in declaration order.
    [[nodiscard]] std::vector<ActionRef> const& topQueries() const noexcept { return _top; }

    /// @brief Every action named.
    /// @return Them, sorted and unique.
    [[nodiscard]] std::vector<ActionRef> actions() const { return {_all.begin(), _all.end()}; }

    /// @brief Every model type named.
    /// @return Them, sorted and unique.
    [[nodiscard]] std::vector<std::string> modelTypes() const { return {_models.begin(), _models.end()}; }

    /// @brief Where @p ref is first named: the JSON Pointer of its query, mutation or model-bound node.
    /// @param ref An action `actions()` lists.
    /// @return The pointer; empty for an action the document does not name.
    [[nodiscard]] std::string const& pathOf(ActionRef const& ref) const {
        static std::string const none;
        auto const found = _paths.find(ref);
        return found == _paths.end() ? none : found->second;
    }

private:
    void name(ActionRef ref, std::string const& path) {
        _paths.try_emplace(ref, path);
        _all.insert(std::move(ref));
    }

    void enter(ScopeDecl const& scope, bool top) {
        for (ModelDecl const& model : scope.models) {
            _types.try_emplace(model.name).first->second.push_back(model.type);
            _models.insert(model.type);
        }
        for (QueryDecl const& query : scope.queries) {
            ActionRef ref{.modelType = resolve(query.model), .action = query.action};
            if (top) {
                _top.push_back(ref);
            }
            name(std::move(ref), query.path);
        }
        for (MutationDecl const& mutation : scope.mutations) {
            name(ActionRef{.modelType = resolve(mutation.model), .action = mutation.action}, mutation.path);
        }
    }

    void leave(ScopeDecl const& scope) {
        for (ModelDecl const& model : scope.models) {
            _types.at(model.name).pop_back();
        }
    }

    void node(DocNode const& view) {
        if (view.scope) {
            enter(*view.scope, false);
        }
        if (std::string_view const model = view.name("model"); !model.empty()) {
            name(ActionRef{.modelType = resolve(model), .action = std::string{view.name("action")}}, view.path);
        }
        for (auto const& [key, children] : view.children) {
            static_cast<void>(key);
            for (DocNodePtr const& child : children) {
                node(*child);
            }
        }
        for (auto const& [key, parts] : view.parts) {
            static_cast<void>(key);
            for (DocNodePtr const& part : parts) {
                node(*part);
            }
        }
        if (view.scope) {
            leave(*view.scope);
        }
    }

    /// @brief The type of the innermost alias named @p alias. Load-time validation has resolved it already, so the
    ///        lookup cannot miss; `at` throws rather than read past the end if that ever changes.
    [[nodiscard]] std::string const& resolve(std::string_view alias) const {
        std::vector<std::string> const& types = _types.at(std::string{alias});
        return types.at(types.size() - 1);
    }

    /// @brief The types an alias names in the open scopes, innermost last.
    std::map<std::string, std::vector<std::string>, std::less<>> _types;
    std::vector<ActionRef> _top;
    std::set<ActionRef> _all;
    std::map<ActionRef, std::string> _paths;
    std::set<std::string> _models;
};

// NOLINTEND(misc-no-recursion)

}  // namespace morph::ui::detail
```

In `src/ui/service.cpp`, delete the whole `class DocumentActions { … };` together with the
`NOLINTBEGIN/NOLINTEND(misc-no-recursion)` pair around it, add `#include "document_actions.hpp"` after the system
includes, and add `using detail::DocumentActions;` at the top of the anonymous namespace, so the two existing uses
compile unchanged.

- [ ] **Step 2: Build and run the service tests, so the move is proven before anything is added on top of it**

Run: `flock "$LOCK" cmake --build build/clang-debug --target morph_tests -j5 && flock "$LOCK" timeout 300 build/clang-debug/tests/morph_tests "[ui-service]"`
Expected: `All tests passed`.

- [ ] **Step 3: Write the test fixtures.** Create `tests/ui_custom_screens_support.hpp`. This task uses `baseScreen`
  and `load`. Tasks 4 and 6 add more to this file.

```cpp
// SPDX-License-Identifier: Apache-2.0
//
// Fixtures of the custom screens tests: a base screen that uses one of every capability the base rule compares,
// and a loader that refuses to go on with a fixture that does not load.

#pragma once
#include <catch2/catch_test_macros.hpp>
#include <morph/ui/bundle.hpp>
#include <morph/ui/document.hpp>
#include <string>
#include <string_view>

#include "ui_service_support.hpp"

namespace screens_test {

/// The base screen, under @p id. It runs `save` on mount, offers `verify` and an upload behind buttons, calls the
/// host command `print`, and declares a delivered component (`Chart`, the fixture bundle's) and a host component.
inline std::string baseScreen(std::string_view id) {
    return R"({"vocab":"ui/1","id":")" + std::string{id} +
           R"(","models":{"sample":{"type":"Lims_Sample"}},)"
           R"("queries":{"list":{"model":"sample","action":"Lims_List"}},)"
           R"("mutations":{"save":{"model":"sample","action":"Lims_Save"},)"
           R"("verify":{"model":"sample","action":"Lims_Verify"}},)"
           R"("onMount":[{"run":"save"}],)"
           R"("components":{"Chart":{"qml":")" +
           morph::ui::contentHash(uiservice::kChart) +
           R"(","props":{},"fallback":{"kind":"text","text":"chart"}},)"
           R"("Grid":{"host":"ResultGrid","version":2,"props":{},"fallback":{"kind":"text","text":"grid"}}},)"
           R"("view":{"kind":"column","children":[)"
           R"({"kind":"text","text":"Results"},)"
           R"({"kind":"button","label":"Print","onClick":[{"host":"print"}]},)"
           R"({"kind":"button","label":"Verify","onClick":[{"run":"verify"}]},)"
           R"({"kind":"button","label":"Attach","onClick":[{"upload":"/tmp/a.csv","to":"save","as":"ref"}]}]}})";
}

/// @p text with its first @p from replaced by @p to; fails the test when @p from is absent, so a fixture edit that
/// no longer matches is caught rather than silently testing the unchanged base.
inline std::string replaced(std::string text, std::string_view from, std::string_view to) {
    auto const at = text.find(from);
    REQUIRE(at != std::string::npos);
    text.replace(at, from.size(), to);
    return text;
}

/// @p text loaded against the fixture's catalog. A fixture that does not load is a fixture bug: fix its JSON
/// against docs/spec/ui/document.md, never the check that refused it.
inline morph::ui::Document load(std::string const& text) {
    auto loaded = morph::ui::loadDocumentText(text, morph::ui::LoadOptions{.catalog = uiservice::facts()});
    if (!loaded) {
        FAIL("fixture does not load: " << loaded.error().message());
    }
    return std::move(*loaded);
}

}  // namespace screens_test
```

- [ ] **Step 4: Write the failing base rule tests.** Create `tests/test_ui_custom_screen_rules.cpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0
//
// The base rule: a custom document may use only what its base screen uses. One case per row of the rule.

#include <catch2/catch_test_macros.hpp>
#include <morph/ui/bundle.hpp>
#include <morph/ui/custom_screens.hpp>
#include <optional>
#include <string>

#include "ui_custom_screens_support.hpp"

using screens_test::baseScreen;
using screens_test::load;
using screens_test::replaced;

namespace {

std::optional<morph::ui::Refusal> rule(std::string const& custom) {
    std::string const base = baseScreen("lims.base");
    return morph::ui::checkBaseRule(load(base), base, load(custom), custom);
}

}  // namespace

TEST_CASE("base rule: a rearranged, trimmed copy of the base passes", "[ui][custom-screens][base-rule]") {
    CHECK_FALSE(rule(baseScreen("custom:mine")));
    std::string const trimmed =
        replaced(baseScreen("custom:mine"), R"({"kind":"button","label":"Print","onClick":[{"host":"print"}]},)", "");
    CHECK_FALSE(rule(replaced(trimmed, R"("text":"Results")", R"("text":"Mine")")));
}

TEST_CASE("base rule: an action the base does not use is refused", "[ui][custom-screens][base-rule]") {
    auto const refusal = rule(replaced(baseScreen("custom:mine"), R"("queries":{)",
                                       R"("queries":{"report":{"model":"sample","action":"Lims_Report"},)"));
    REQUIRE(refusal);
    CHECK(refusal->path == "/queries/report");
    CHECK(refusal->reason.contains("Lims_Report"));
}

TEST_CASE("base rule: a command the base runs from another trigger is refused", "[ui][custom-screens][base-rule]") {
    SECTION("an attended action run on mount") {
        auto const refusal =
            rule(replaced(baseScreen("custom:mine"), R"("onMount":[{"run":"save"}])",
                          R"("onMount":[{"run":"save"},{"run":"verify"}])"));
        REQUIRE(refusal);
        CHECK(refusal->path == "/onMount/1");
        CHECK(refusal->reason.contains("onMount: run Lims_Sample/Lims_Verify"));
    }
    SECTION("a mount run moved to unmount") {
        auto const refusal =
            rule(replaced(baseScreen("custom:mine"), R"("onMount":[{"run":"save"}])", R"("onUnmount":[{"run":"save"}])"));
        REQUIRE(refusal);
        CHECK(refusal->path == "/onUnmount/0");
    }
    SECTION("a run chained after an unattended run") {
        auto const refusal =
            rule(replaced(baseScreen("custom:mine"), R"("save":{"model":"sample","action":"Lims_Save"})",
                          R"("save":{"model":"sample","action":"Lims_Save","onSuccess":[{"run":"verify"}]})"));
        REQUIRE(refusal);
        CHECK(refusal->reason.contains("onMount: run Lims_Sample/Lims_Verify"));
    }
    SECTION("a watch, which runs without a press") {
        auto const refusal = rule(replaced(baseScreen("custom:mine"), R"("onMount":)",
                                           R"("watch":[{"on":{"query":"list"},"do":[{"run":"verify"}]}],"onMount":)"));
        REQUIRE(refusal);
        CHECK(refusal->reason.contains("watch: run Lims_Sample/Lims_Verify"));
    }
}

TEST_CASE("base rule: a client-side command the base does not use is refused", "[ui][custom-screens][base-rule]") {
    auto const refusal = rule(replaced(baseScreen("custom:mine"), R"({"kind":"text","text":"Results"},)",
                                       R"({"kind":"text","text":"Results"},)"
                                       R"({"kind":"button","label":"Out","onClick":[{"signOut":true}]},)"));
    REQUIRE(refusal);
    CHECK(refusal->reason.contains("signOut"));
    // The base's own upload, moved elsewhere, is still the base's.
    CHECK_FALSE(rule(replaced(baseScreen("custom:mine"), R"("label":"Attach")", R"("label":"Attach a file")")));
}

TEST_CASE("base rule: a host name the base does not use is refused", "[ui][custom-screens][base-rule]") {
    SECTION("a host command") {
        auto const refusal =
            rule(replaced(baseScreen("custom:mine"), R"({"host":"print"})", R"({"host":"openPath"})"));
        REQUIRE(refusal);
        CHECK(refusal->reason.contains("host `openPath`"));
    }
    SECTION("a host value") {
        auto const refusal = rule(replaced(baseScreen("custom:mine"), R"("text":"Results")",
                                           R"("text":{"fn":"coalesce","args":[{"host":"sync.count"},"-"]})"));
        REQUIRE(refusal);
        CHECK(refusal->reason.contains("host `sync.count`"));
    }
    SECTION("a host component") {
        auto const refusal =
            rule(replaced(baseScreen("custom:mine"), R"("host":"ResultGrid")", R"("host":"Camera")"));
        REQUIRE(refusal);
        CHECK(refusal->path == "/components/Grid/host");
    }
}

TEST_CASE("base rule: a delivered component the base does not declare is refused", "[ui][custom-screens][base-rule]") {
    auto const refusal = rule(replaced(baseScreen("custom:mine"), morph::ui::contentHash(uiservice::kChart),
                                       morph::ui::contentHash("Rectangle {}")));
    REQUIRE(refusal);
    CHECK(refusal->path == "/components/Chart");
}

TEST_CASE("base rule: params other than the base's are refused", "[ui][custom-screens][base-rule]") {
    auto const refusal =
        rule(replaced(baseScreen("custom:mine"), R"("models":)", R"("params":{"sampleId":"int"},"models":)"));
    REQUIRE(refusal);
    CHECK(refusal->path == "/params");
}
```

If a fixture does not load (the `load` helper fails the test with the loader's reason), correct the fixture's JSON
against `docs/spec/ui/document.md` (for example, the exact spelling of a watch's `on`/`do` members), and keep the
case's intent: the mutation adds exactly one capability the base lacks.

Register the test file: in `tests/CMakeLists.txt`, add `test_ui_custom_screen_rules.cpp` after
`test_ui_preference_sync.cpp`.

- [ ] **Step 5: Run it to verify it fails**

Run: `flock "$LOCK" cmake --build build/clang-debug --target morph_tests -j5`
Expected: compile error, `'morph/ui/custom_screens.hpp' file not found`.

- [ ] **Step 6: Create the header's first part.** Create `include/morph/ui/custom_screens.hpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0

#pragma once
#include <morph/export.hpp>
#include <optional>
#include <string_view>

#include "document.hpp"
#include "value.hpp"

/// @file
/// @brief A person's own screens: the store seam, the checks a custom document passes, and the server model that
///        serves them under the verified person.
///
/// Specified in `docs/spec/ui/custom_screens.md`.

namespace morph::ui {

/// @brief Checks that @p custom uses nothing its base screen does not: the base rule.
///
/// Compared: the actions it names, each on its model type; the commands run without a press (from a lifecycle list,
/// a watch, or a mutation's `onSuccess`/`onError` reached from one), by trigger; the client-side commands `upload`,
/// `download`, `switchBackend`, `signIn`, `signOut` and `resetPrefs`; every `host` name, whether a host command, a
/// host value or a host component; the delivered components, by name and content hash; and the params, which must
/// be the base's exactly. Text, layout, styles, states and expressions are not compared.
/// @param base       The base screen, loaded.
/// @param baseText   The base screen's JSON text.
/// @param custom     The custom document, loaded.
/// @param customText The custom document's JSON text.
/// @return The first use the base does not have, with its JSON Pointer, or nothing.
[[nodiscard]] MORPH_API std::optional<Refusal> checkBaseRule(Document const& base, std::string_view baseText,
                                                             Document const& custom, std::string_view customText);

}  // namespace morph::ui
```

- [ ] **Step 7: Implement the base rule.** Create `src/ui/custom_screen_checks.cpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0

#include <algorithm>
#include <functional>
#include <map>
#include <morph/ui/custom_screens.hpp>
#include <morph/ui/document.hpp>
#include <morph/ui/service.hpp>
#include <morph/ui/value.hpp>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "document_actions.hpp"

namespace morph::ui {
namespace {

[[nodiscard]] Refusal refusalAt(std::string path, std::string reason) {
    return Refusal{.document = {}, .path = std::move(path), .reason = std::move(reason)};
}

// NOLINTBEGIN(misc-no-recursion): view nodes, command lists and JSON nest; parseJson bounds the depth at 64.

/// @brief Everything a document can cause that the base rule compares, each as a capability string, with the JSON
///        Pointer where the document first uses it.
class Capabilities {
public:
    /// @brief Walks @p document and its JSON @p json.
    /// @param document The loaded document.
    /// @param json     Its parsed JSON, for the `host` names.
    Capabilities(Document const& document, Json const& json) {
        enter(document.scope);
        for (auto const& [trigger, commands] : document.lifecycle) {
            walk(commands, trigger);
        }
        for (auto const& [chord, commands] : document.keys) {
            static_cast<void>(chord);
            walk(commands, {});
        }
        for (auto const* parts :
             {&document.status, &document.actions, &document.menu, &document.tools, &document.navigation}) {
            for (DocNodePtr const& part : *parts) {
                node(*part);
            }
        }
        if (document.view) {
            node(*document.view);
        }
        for (auto const& [name, component] : document.components) {
            if (!component.qml.empty()) {
                note("component `" + name + "` " + component.qml, component.path);
            }
            if (component.fallback) {
                node(*component.fallback);
            }
        }
        leave(document.scope);
        hostNames(json, "");
    }

    /// @brief Every capability, in the order first used, with its pointer.
    /// @return The uses.
    [[nodiscard]] std::vector<std::pair<std::string, std::string>> const& uses() const noexcept { return _uses; }

    /// @brief Whether the document uses @p capability.
    /// @param capability The capability string.
    /// @return Whether it does.
    [[nodiscard]] bool has(std::string const& capability) const { return _seen.contains(capability); }

private:
    void note(std::string capability, std::string const& path) {
        if (_seen.insert(capability).second) {
            _uses.emplace_back(std::move(capability), path);
        }
    }

    void enter(ScopeDecl const& scope) {
        for (ModelDecl const& model : scope.models) {
            _types[model.name].push_back(model.type);
        }
        for (MutationDecl const& mutation : scope.mutations) {
            _mutations[mutation.name].push_back(&mutation);
        }
        for (MutationDecl const& mutation : scope.mutations) {
            walk(mutation.onSuccess, {});
            walk(mutation.onError, {});
        }
        // A watch runs when a value changes, whoever changed it, so its commands run without a press.
        for (WatchDecl const& watch : scope.watches) {
            walk(watch.body, "watch");
        }
    }

    void leave(ScopeDecl const& scope) {
        for (ModelDecl const& model : scope.models) {
            _types.at(model.name).pop_back();
        }
        for (MutationDecl const& mutation : scope.mutations) {
            _mutations.at(mutation.name).pop_back();
        }
    }

    void node(DocNode const& view) {
        if (view.scope) {
            enter(*view.scope);
        }
        for (auto const& [key, commands] : view.events) {
            static_cast<void>(key);
            walk(commands, {});
        }
        for (auto const& [chord, commands] : view.keys) {
            static_cast<void>(chord);
            walk(commands, {});
        }
        for (auto const& [key, children] : view.children) {
            static_cast<void>(key);
            for (DocNodePtr const& child : children) {
                node(*child);
            }
        }
        for (auto const& [key, parts] : view.parts) {
            static_cast<void>(key);
            for (DocNodePtr const& part : parts) {
                node(*part);
            }
        }
        if (view.scope) {
            leave(*view.scope);
        }
    }

    /// @brief Notes @p commands' capabilities. @p trigger names what runs them without a press (`onMount`, `watch`,
    ///        …), and is empty for commands a person's press runs.
    void walk(Commands const& commands, std::string const& trigger) {
        for (Command const& command : commands) {
            std::string const capability = capabilityOf(command);
            if (!capability.empty()) {
                // Host commands are noted by name from the JSON with every other host name; runs by the actions row.
                if (command.kind != CommandKind::Run && command.kind != CommandKind::Host) {
                    note(capability, command.path);
                }
                if (!trigger.empty()) {
                    note(trigger + ": " + capability, command.path);
                }
            }
            if (command.kind == CommandKind::Run && !trigger.empty()) {
                follow(command.target, trigger);
            }
            walk(command.commands, trigger);
            walk(command.alternative, trigger);
        }
    }

    /// @brief A mutation run without a press runs its `onSuccess` and `onError` without one too.
    void follow(std::string const& mutation, std::string const& trigger) {
        MutationDecl const* const decl = mutationNamed(mutation);
        if (decl == nullptr || !_followed.insert({decl, trigger}).second) {
            return;
        }
        walk(decl->onSuccess, trigger);
        walk(decl->onError, trigger);
    }

    [[nodiscard]] MutationDecl const* mutationNamed(std::string const& name) const {
        auto const found = _mutations.find(name);
        return found == _mutations.end() || found->second.empty() ? nullptr : found->second.back();
    }

    [[nodiscard]] std::string actionOf(std::string const& mutation) const {
        MutationDecl const* const decl = mutationNamed(mutation);
        if (decl == nullptr) {
            return mutation;
        }
        auto const types = _types.find(decl->model);
        std::string const type = types == _types.end() || types->second.empty() ? decl->model : types->second.back();
        return type + "/" + decl->action;
    }

    [[nodiscard]] std::string capabilityOf(Command const& command) const {
        switch (command.kind) {
        case CommandKind::Run:
            return "run " + actionOf(command.target);
        case CommandKind::Host:
            return "host `" + command.target + "`";
        case CommandKind::Upload:
            return "upload";
        case CommandKind::Download:
            return "download";
        case CommandKind::SwitchBackend:
            return "switchBackend `" + command.target + "`";
        case CommandKind::SignIn:
            return "signIn";
        case CommandKind::SignOut:
            return "signOut";
        case CommandKind::ResetPrefs:
            return "resetPrefs";
        case CommandKind::Set:
        case CommandKind::Reset:
        case CommandKind::Refetch:
        case CommandKind::Open:
        case CommandKind::Close:
        case CommandKind::Navigate:
        case CommandKind::Return:
        case CommandKind::Focus:
        case CommandKind::SetLocale:
        case CommandKind::SetContext:
        case CommandKind::Notify:
        case CommandKind::Seq:
        case CommandKind::All:
        case CommandKind::If:
        case CommandKind::CancelClose:
        case CommandKind::Retry:
            return {};
        }
        return {};
    }

    /// @brief Every string member named `host`, anywhere: a host command, a host value or a host component.
    void hostNames(Json const& json, std::string const& path) {
        if (auto const* const object = std::get_if<Json::object_t>(&json.data)) {
            for (auto const& [key, value] : *object) {
                std::string const at = pointerAppend(path, key);
                if (auto const* const text = std::get_if<std::string>(&value.data); text != nullptr && key == "host") {
                    note("host `" + *text + "`", at);
                }
                hostNames(value, at);
            }
        } else if (auto const* const array = std::get_if<Json::array_t>(&json.data)) {
            for (std::size_t index = 0; index < array->size(); ++index) {
                hostNames((*array)[index], pointerAppend(path, index));
            }
        }
    }

    std::map<std::string, std::vector<std::string>, std::less<>> _types;
    std::map<std::string, std::vector<MutationDecl const*>, std::less<>> _mutations;
    std::set<std::pair<MutationDecl const*, std::string>> _followed;
    std::vector<std::pair<std::string, std::string>> _uses;
    std::set<std::string, std::less<>> _seen;
};

// NOLINTEND(misc-no-recursion)

}  // namespace

std::optional<Refusal> checkBaseRule(Document const& base, std::string_view baseText, Document const& custom,
                                     std::string_view customText) {
    std::string const of = "the base screen `" + base.id + "`";
    if (custom.params != base.params) {
        return refusalAt("/params", "a custom screen declares exactly the params of " + of);
    }
    detail::DocumentActions const baseNames{base};
    detail::DocumentActions const customNames{custom};
    std::vector<ActionRef> const baseActions = baseNames.actions();
    for (ActionRef const& ref : customNames.actions()) {
        if (!std::ranges::binary_search(baseActions, ref)) {
            return refusalAt(customNames.pathOf(ref),
                             of + " does not use action `" + ref.action + "` of `" + ref.modelType + "`");
        }
    }
    auto const baseJson = parseJson(baseText);
    auto const customJson = parseJson(customText);
    if (!baseJson || !customJson) {
        return refusalAt("", "a document the base rule compares is JSON text");
    }
    Capabilities const had{base, *baseJson};
    Capabilities const has{custom, *customJson};
    for (auto const& [capability, path] : has.uses()) {
        if (!had.has(capability)) {
            return refusalAt(path, of + " has no `" + capability + "`");
        }
    }
    return std::nullopt;
}

}  // namespace morph::ui
```

Add `src/ui/custom_screen_checks.cpp` to `add_library(morph …)` in `CMakeLists.txt`, after `src/ui/service.cpp`.

- [ ] **Step 8: Run the tests**

Run: `flock "$LOCK" cmake --build build/clang-debug --target morph_tests -j5 && flock "$LOCK" timeout 300 build/clang-debug/tests/morph_tests "[base-rule]"`
Expected: `All tests passed`. If an expected pointer differs because the loader records a path differently (for
example a component's path), read the actual path from the failure, confirm in `src/ui/document.cpp` that it is the
offending member, and correct the expectation, not the walker.

- [ ] **Step 9: Mutation checks, one per row.** For each row, disable its check, rebuild, run `[base-rule]`, and confirm
  that row's case fails. Then restore from a backup and `touch src/ui/custom_screen_checks.cpp`.

| Row | Mutation | Case that must fail |
|---|---|---|
| Actions | `if (false && !std::ranges::binary_search(...))` | "an action the base does not use" |
| Unattended | `if (false && !trigger.empty()) { note(trigger …` | "a command the base runs from another trigger" (all four sections) |
| Chains | make `follow` return at once | the section "a run chained after an unattended run" |
| Watches | `walk(watch.body, {})` | the section "a watch" |
| Client-side | skip the first `note(capability, …)` | "a client-side command" |
| Host names | make `hostNames` return at once | all three sections of "a host name" |
| Components | delete the `note("component …")` line | "a delivered component" |
| Params | `if (false && custom.params != base.params)` | "params other than the base's" |

Record in the task report which mutations were run and that each made its case fail.

- [ ] **Step 10: Commit**

```bash
git add src/ui/document_actions.hpp src/ui/service.cpp include/morph/ui/custom_screens.hpp \
        src/ui/custom_screen_checks.cpp CMakeLists.txt tests/ui_custom_screens_support.hpp \
        tests/test_ui_custom_screen_rules.cpp tests/CMakeLists.txt
git commit -m "ui: the base rule, a custom screen can do nothing its base cannot"
```

---

### Task 4: The check pipeline, the id rewrite and the verdict cache

**Files:**
- Modify: `include/morph/ui/custom_screens.hpp`
- Modify: `src/ui/custom_screen_checks.cpp`
- Modify: `tests/ui_custom_screens_support.hpp` (add `registry()` and `bundle()`)
- Create: `tests/test_ui_custom_screen_checks.cpp`
- Modify: `tests/CMakeLists.txt`

**Interfaces:**
- Consumes: `checkBaseRule`, `permittedActions`, `screenAdmitted`, `ServedBundle::catalog`, `detail::DocumentActions`.
- Produces:
  - `struct morph::ui::CustomScreensOptions { std::size_t maxDocumentBytes = 262144; std::size_t verdictCacheSize = 1024; }`
  - `class morph::ui::VerdictCache` with `explicit VerdictCache(std::size_t capacity)`,
    `std::optional<std::optional<Refusal>> find(std::string const& key)`,
    `void store(std::string key, std::optional<Refusal> verdict)`, `std::size_t size() const`
  - `std::string morph::ui::verdictKey(std::string_view basedOn, std::string_view document, std::string_view digest, std::vector<std::string> const& permitted)`
  - `std::optional<Refusal> morph::ui::checkCustomScreen(ServedBundle const& bundle, session::Context const& session, session::IAuthorizer const& authorizer, std::string_view basedOn, std::string_view document, CustomScreensOptions const& options, VerdictCache* cache = nullptr)`
  - `std::expected<std::string, Refusal> morph::ui::withScreenId(std::string_view document, std::string_view id)`
  - Test helpers `screens_test::registry()`, `screens_test::bundle()`

- [ ] **Step 1: Add the bundle fixture** to `tests/ui_custom_screens_support.hpp`, before the closing namespace brace
  (add `#include <memory>`, `#include <morph/ui/service.hpp>`, `#include <stdexcept>` to its includes):

```cpp
/// The service fixture's registry with the base screen added as `lims.base`.
inline morph::ui::UiRegistry registry() {
    morph::ui::UiRegistry made = uiservice::registry();
    auto const added = made.addScreen(uiservice::entry("lims.base"), baseScreen("lims.base"));
    if (!added) {
        throw std::runtime_error{added.error().message()};
    }
    return made;
}

/// That registry's bundle.
inline std::shared_ptr<morph::ui::ServedBundle const> bundle() {
    auto built = registry().build();
    if (!built) {
        throw std::runtime_error{built.error().message()};
    }
    return *built;
}
```

- [ ] **Step 2: Write the failing tests.** Create `tests/test_ui_custom_screen_checks.cpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0
//
// The checks a custom document passes on save and on every load: one case per check, in their order, the id
// rewrite, and the verdict cache.

#include <catch2/catch_test_macros.hpp>
#include <morph/session/session.hpp>
#include <morph/ui/author.hpp>
#include <morph/ui/custom_screens.hpp>
#include <morph/ui/document.hpp>
#include <optional>
#include <string>
#include <vector>

#include "ui_custom_screens_support.hpp"

using screens_test::baseScreen;
using screens_test::replaced;

namespace {

struct Fixture {
    std::shared_ptr<morph::ui::ServedBundle const> bundle = screens_test::bundle();
    std::shared_ptr<uiservice::RuleAuthorizer> authorizer = std::make_shared<uiservice::RuleAuthorizer>();
    morph::session::Context alice = [] {
        morph::session::Context made;
        made.principal = "alice";
        return made;
    }();

    std::optional<morph::ui::Refusal> check(std::string const& document, std::string const& basedOn = "lims.base",
                                            morph::ui::CustomScreensOptions options = {},
                                            morph::ui::VerdictCache* cache = nullptr) const {
        return morph::ui::checkCustomScreen(*bundle, alice, *authorizer, basedOn, document, options, cache);
    }
};

}  // namespace

TEST_CASE("checks: a copy of the base passes", "[ui][custom-screens][checks]") {
    Fixture const f;
    CHECK_FALSE(f.check(baseScreen("lims.base")));
}

TEST_CASE("checks: 1, the bounds", "[ui][custom-screens][checks]") {
    Fixture const f;
    auto const large = f.check(baseScreen("lims.base"), "lims.base", morph::ui::CustomScreensOptions{.maxDocumentBytes = 64});
    REQUIRE(large);
    CHECK(large->reason.contains("longer than 64 bytes"));
    auto const deep = f.check(std::string(65, '[') + std::string(65, ']'));
    REQUIRE(deep);
    CHECK(deep->reason.contains("nests deeper"));
}

TEST_CASE("checks: 2, the base", "[ui][custom-screens][checks]") {
    Fixture f;
    auto const unknown = f.check(baseScreen("lims.base"), "lims.nothing");
    REQUIRE(unknown);
    CHECK(unknown->reason.contains("names no screen document"));
    auto const host = f.check(baseScreen("lims.base"), "lims.scanner");
    REQUIRE(host);
    CHECK(host->reason.contains("names no screen document"));
    f.authorizer->denied["alice"] = {{"Lims_Sample", "Lims_List"}};
    auto const closed = f.check(baseScreen("lims.base"));
    REQUIRE(closed);
    CHECK(closed->reason.contains("may not open"));
}

TEST_CASE("checks: 3, the application screen checks", "[ui][custom-screens][checks]") {
    Fixture const f;
    std::string const broken = replaced(baseScreen("lims.base"), R"({"kind":"text","text":"Results"})",
                                        R"({"kind":"nope","text":"Results"})");
    auto const expected = morph::ui::loadDocumentText(broken, morph::ui::LoadOptions{.catalog = f.bundle->catalog});
    REQUIRE_FALSE(expected);
    auto const refusal = f.check(broken);
    REQUIRE(refusal);
    CHECK(refusal->path == expected.error().path);
    CHECK(refusal->reason == expected.error().reason);

    std::string const app = R"({"vocab":"ui/1","id":"app","models":{"sample":{"type":"Lims_Sample"}},)"
                            R"("queries":{"list":{"model":"sample","action":"Lims_List"}}})";
    auto const appRefusal = f.check(app);
    REQUIRE(appRefusal);
    CHECK(appRefusal->reason.contains("not the app document"));
}

TEST_CASE("checks: 4, the accessibility rules", "[ui][custom-screens][checks]") {
    Fixture const f;
    std::string const unlabelled = replaced(baseScreen("lims.base"), R"("label":"Verify")", R"("label":"")");
    auto const expected = morph::ui::author::checkAccessibility(screens_test::load(unlabelled));
    REQUIRE(expected);
    auto const refusal = f.check(unlabelled);
    REQUIRE(refusal);
    CHECK(refusal->path == expected->path);
}

TEST_CASE("checks: 5, the base rule", "[ui][custom-screens][checks]") {
    Fixture const f;
    auto const refusal = f.check(replaced(baseScreen("lims.base"), R"({"host":"print"})", R"({"signOut":true})"));
    REQUIRE(refusal);
    CHECK(refusal->reason.contains("signOut"));
}

TEST_CASE("checks: 6, permission", "[ui][custom-screens][checks]") {
    Fixture f;
    f.authorizer->denied["alice"] = {{"Lims_Sample", "Lims_Verify"}};
    auto const refusal = f.check(baseScreen("lims.base"));
    REQUIRE(refusal);
    CHECK(refusal->path == "/mutations/verify");
    CHECK(refusal->reason.contains("may not run action `Lims_Verify`"));
}

TEST_CASE("withScreenId puts the custom screen's id on the document", "[ui][custom-screens][checks]") {
    auto const rewritten = morph::ui::withScreenId(baseScreen("lims.results"), "custom:0123");
    REQUIRE(rewritten);
    CHECK(screens_test::load(*rewritten).id == "custom:0123");
    CHECK_FALSE(morph::ui::withScreenId("{not json", "custom:1"));
    CHECK_FALSE(morph::ui::withScreenId("[1,2]", "custom:1"));
}

TEST_CASE("VerdictCache is bounded and drops the least recently used", "[ui][custom-screens][cache]") {
    morph::ui::VerdictCache cache{2};
    cache.store("a", std::nullopt);
    cache.store("b", morph::ui::Refusal{.document = {}, .path = "/x", .reason = "no"});
    REQUIRE(cache.find("a").has_value());  // a is now the most recent
    cache.store("c", std::nullopt);
    CHECK(cache.size() == 2);
    CHECK_FALSE(cache.find("b").has_value());
    auto const a = cache.find("a");
    REQUIRE(a.has_value());
    CHECK_FALSE(a->has_value());
}

TEST_CASE("the verdict key changes with the document, the base, the bundle and the permitted set",
          "[ui][custom-screens][cache]") {
    std::vector<std::string> const one{"Lims_List"};
    std::vector<std::string> const two{"Lims_List", "Lims_Save"};
    std::string const key = morph::ui::verdictKey("lims.base", "{}", "sha256:aa", one);
    CHECK(key == morph::ui::verdictKey("lims.base", "{}", "sha256:aa", one));
    CHECK(key != morph::ui::verdictKey("lims.base", "{ }", "sha256:aa", one));
    CHECK(key != morph::ui::verdictKey("lims.other", "{}", "sha256:aa", one));
    CHECK(key != morph::ui::verdictKey("lims.base", "{}", "sha256:bb", one));
    CHECK(key != morph::ui::verdictKey("lims.base", "{}", "sha256:aa", two));
}

TEST_CASE("checkCustomScreen reads and fills the verdict cache", "[ui][custom-screens][cache]") {
    Fixture f;
    morph::ui::VerdictCache cache{8};
    std::string const document = baseScreen("lims.base");
    CHECK_FALSE(f.check(document, "lims.base", {}, &cache));
    CHECK(cache.size() == 1);
    CHECK_FALSE(f.check(document, "lims.base", {}, &cache));
    CHECK(cache.size() == 1);

    // A planted verdict is answered, so the lookup is really used.
    auto const permitted = morph::ui::permittedActions(*f.bundle, f.alice, *f.authorizer);
    cache.store(morph::ui::verdictKey("lims.base", document, f.bundle->digest, permitted),
                morph::ui::Refusal{.document = {}, .path = "/planted", .reason = "planted"});
    auto const planted = f.check(document, "lims.base", {}, &cache);
    REQUIRE(planted);
    CHECK(planted->path == "/planted");

    // A permission change misses the cache and checks again.
    f.authorizer->denied["alice"] = {{"Lims_Sample", "Lims_Verify"}};
    auto const denied = f.check(document, "lims.base", {}, &cache);
    REQUIRE(denied);
    CHECK(denied->path == "/mutations/verify");
}
```

Register `test_ui_custom_screen_checks.cpp` in `tests/CMakeLists.txt` after `test_ui_custom_screen_rules.cpp`.

- [ ] **Step 3: Run them to verify they fail**

Run: `flock "$LOCK" cmake --build build/clang-debug --target morph_tests -j5`
Expected: compile error, `no member named 'checkCustomScreen' in namespace 'morph::ui'`.

- [ ] **Step 4: Declare the pipeline** in `include/morph/ui/custom_screens.hpp`. Extend the includes to:

```cpp
#include <cstddef>
#include <expected>
#include <functional>
#include <list>
#include <map>
#include <morph/export.hpp>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "../session/session.hpp"
#include "document.hpp"
#include "service.hpp"
#include "value.hpp"
```

After `checkBaseRule`, add:

```cpp
/// @brief The custom screens' bounds.
struct CustomScreensOptions {
    /// @brief The longest custom document, in bytes. At most `kMaxFileBytes`, so every stored screen can be served.
    std::size_t maxDocumentBytes = std::size_t{256} * 1024;
    /// @brief The most verdicts `VerdictCache` keeps.
    std::size_t verdictCacheSize = 1024;
};

/// @brief The checks' verdicts by `verdictKey`, bounded, dropping the least recently used.
///
/// A list of custom screens runs every stored document through the checks, and every save refetches every list, so
/// an unchanged document under unchanged permissions is answered from here. Safe to share between threads.
class MORPH_API VerdictCache {
public:
    /// @brief An empty cache.
    /// @param capacity The most verdicts kept; at least one is kept whatever it says.
    explicit VerdictCache(std::size_t capacity) : _capacity{capacity == 0 ? 1 : capacity} {}

    VerdictCache(VerdictCache const&) = delete;
    VerdictCache& operator=(VerdictCache const&) = delete;
    VerdictCache(VerdictCache&&) = delete;
    VerdictCache& operator=(VerdictCache&&) = delete;
    ~VerdictCache() = default;

    /// @brief The verdict kept under @p key, which becomes the most recently used.
    /// @param key The key.
    /// @return Nothing when no verdict is kept; otherwise the verdict, itself nothing for a document that passed.
    [[nodiscard]] std::optional<std::optional<Refusal>> find(std::string const& key);

    /// @brief Keeps @p verdict under @p key, dropping the least recently used verdict past the capacity.
    /// @param key     The key.
    /// @param verdict The refusal, or nothing for a document that passed.
    void store(std::string key, std::optional<Refusal> verdict);

    /// @brief How many verdicts are kept.
    /// @return The count.
    [[nodiscard]] std::size_t size() const;

private:
    struct Entry {
        std::optional<Refusal> verdict;
        std::list<std::string>::iterator order;
    };

    std::size_t _capacity;
    mutable std::mutex _mutex;
    /// The keys, most recently used first.
    std::list<std::string> _order;
    std::map<std::string, Entry, std::less<>> _entries;
};

/// @brief The key of a verdict: what the checks of a document read, apart from the document's own bounds.
/// @param basedOn   The base screen's id.
/// @param document  The custom document's text.
/// @param digest    The bundle's digest.
/// @param permitted The person's permitted actions, as `permittedActions` lists them.
/// @return The key.
[[nodiscard]] MORPH_API std::string verdictKey(std::string_view basedOn, std::string_view document,
                                               std::string_view digest, std::vector<std::string> const& permitted);

/// @brief Runs every check a custom document passes, in order, and answers the first refusal.
///
/// 1. The bounds: `maxDocumentBytes`, and nesting within `kMaxJsonDepth`.
/// 2. The base: a screen document of @p bundle that the person may open (`screenAdmitted`).
/// 3. The application screen checks: `loadDocumentText` with the bundle's catalog, and a screen, not the app
///    document.
/// 4. The accessibility rules (`author::checkAccessibility`).
/// 5. The base rule (`checkBaseRule`).
/// 6. Permission: every action the document names is in `permittedActions`.
///
/// Checks 2 to 6 are answered from @p cache when it holds the verdict, and stored in it otherwise.
/// @param bundle     The served bundle.
/// @param session    The session, its principal verified.
/// @param authorizer The authoriser.
/// @param basedOn    The base screen's id.
/// @param document   The custom document's text.
/// @param options    The bounds.
/// @param cache      The verdict cache; null checks every time.
/// @return The first refusal, with its JSON Pointer, or nothing when the document passes.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters): the base, then the document, at every call.
[[nodiscard]] MORPH_API std::optional<Refusal> checkCustomScreen(ServedBundle const& bundle,
                                                                 ::morph::session::Context const& session,
                                                                 ::morph::session::IAuthorizer const& authorizer,
                                                                 std::string_view basedOn, std::string_view document,
                                                                 CustomScreensOptions const& options,
                                                                 VerdictCache* cache = nullptr);

/// @brief @p document with its screen id replaced by @p id, the id a custom screen is stored and served under.
/// @param document The custom document's JSON text.
/// @param id       The custom screen's id.
/// @return The rewritten JSON text, or the refusal of text that is not a JSON object.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters): the document, then its new id.
[[nodiscard]] MORPH_API std::expected<std::string, Refusal> withScreenId(std::string_view document,
                                                                         std::string_view id);
```

- [ ] **Step 5: Implement it** in `src/ui/custom_screen_checks.cpp`. Add `#include <morph/ui/author.hpp>`,
  `#include <morph/ui/bundle.hpp>`, `#include <glaze/glaze.hpp>`, `#include <expected>`, `#include <mutex>` and
  `#include <list>`. After `checkBaseRule`, add:

```cpp
std::optional<std::optional<Refusal>> VerdictCache::find(std::string const& key) {
    std::scoped_lock const lock{_mutex};
    auto const found = _entries.find(key);
    if (found == _entries.end()) {
        return std::nullopt;
    }
    _order.splice(_order.begin(), _order, found->second.order);
    return found->second.verdict;
}

void VerdictCache::store(std::string key, std::optional<Refusal> verdict) {
    std::scoped_lock const lock{_mutex};
    if (auto const found = _entries.find(key); found != _entries.end()) {
        found->second.verdict = std::move(verdict);
        _order.splice(_order.begin(), _order, found->second.order);
        return;
    }
    _order.push_front(key);
    _entries.emplace(std::move(key), Entry{.verdict = std::move(verdict), .order = _order.begin()});
    while (_entries.size() > _capacity) {
        _entries.erase(_order.back());
        _order.pop_back();
    }
}

std::size_t VerdictCache::size() const {
    std::scoped_lock const lock{_mutex};
    return _entries.size();
}

std::string verdictKey(std::string_view basedOn, std::string_view document, std::string_view digest,
                       std::vector<std::string> const& permitted) {
    std::string joined;
    for (std::string const& action : permitted) {
        joined += action;
        joined += '\n';
    }
    return contentHash(document) + ' ' + std::string{basedOn} + ' ' + std::string{digest} + ' ' +
           contentHash(joined);
}

namespace {

/// @brief Checks 2 to 6 of `checkCustomScreen`.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters): the base, then the document, at every call.
[[nodiscard]] std::optional<Refusal> checkAgainstBase(ServedBundle const& bundle,
                                                      ::morph::session::Context const& session,
                                                      ::morph::session::IAuthorizer const& authorizer,
                                                      std::string_view basedOn, std::string_view document,
                                                      std::vector<std::string> const& permitted) {
    auto const base = std::ranges::find(bundle.screens, basedOn, [](ServedScreen const& s) { return s.entry.id; });
    if (base == bundle.screens.end() || base->hash.empty()) {
        return refusalAt("", "`basedOn` names no screen document of the application: `" + std::string{basedOn} +
                                 "`");
    }
    if (!screenAdmitted(*base, session, authorizer)) {
        return refusalAt("", "the base screen `" + std::string{basedOn} + "` is not one this person may open");
    }
    std::string const& baseText = bundle.files.at(base->hash).bytes;
    LoadOptions const load{.catalog = bundle.catalog};
    auto const baseDocument = loadDocumentText(baseText, load);
    if (!baseDocument) {
        return refusalAt("", "the base screen `" + std::string{basedOn} +
                                 "` does not load: " + baseDocument.error().message());
    }
    auto custom = loadDocumentText(document, load);
    if (!custom) {
        return std::move(custom.error());
    }
    if (!custom->view) {
        return refusalAt("", "a custom screen is a screen, not the app document");
    }
    if (auto refusal = author::checkAccessibility(*custom)) {
        return refusal;
    }
    if (auto refusal = checkBaseRule(*baseDocument, baseText, *custom, document)) {
        return refusal;
    }
    detail::DocumentActions const named{*custom};
    for (ActionRef const& ref : named.actions()) {
        if (std::ranges::find(permitted, ref.action) == permitted.end()) {
            return refusalAt(named.pathOf(ref), "this person may not run action `" + ref.action + "`");
        }
    }
    return std::nullopt;
}

}  // namespace

std::optional<Refusal> checkCustomScreen(ServedBundle const& bundle, ::morph::session::Context const& session,
                                         ::morph::session::IAuthorizer const& authorizer, std::string_view basedOn,
                                         std::string_view document, CustomScreensOptions const& options,
                                         VerdictCache* cache) {
    if (document.size() > options.maxDocumentBytes) {
        return refusalAt("", "a custom screen is longer than " + std::to_string(options.maxDocumentBytes) +
                                 " bytes");
    }
    if (detail::nestsTooDeep(document)) {
        return refusalAt("", "a custom screen nests deeper than " + std::to_string(kMaxJsonDepth) + " levels");
    }
    std::vector<std::string> const permitted = permittedActions(bundle, session, authorizer);
    std::string key;
    if (cache != nullptr) {
        key = verdictKey(basedOn, document, bundle.digest, permitted);
        if (auto hit = cache->find(key)) {
            return std::move(*hit);
        }
    }
    std::optional<Refusal> verdict = checkAgainstBase(bundle, session, authorizer, basedOn, document, permitted);
    if (cache != nullptr) {
        cache->store(std::move(key), verdict);
    }
    return verdict;
}

std::expected<std::string, Refusal> withScreenId(std::string_view document, std::string_view id) {
    auto parsed = parseJson(document);
    if (!parsed) {
        return std::unexpected(refusalAt("", "a custom screen is JSON text: " + parsed.error()));
    }
    auto* const object = std::get_if<Json::object_t>(&parsed->data);
    if (object == nullptr) {
        return std::unexpected(refusalAt("", "a custom screen is a JSON object"));
    }
    (*object)["id"].data = std::string{id};
    auto written = glz::write_json(*parsed);
    if (!written) {
        return std::unexpected(refusalAt("", "a custom screen could not be written back as JSON"));
    }
    return std::move(*written);
}
```

`refusalAt` is in the earlier anonymous namespace of this file. Keep it above both uses. If `parseJson`'s error type is
not `std::string`, adapt the message expression to the type it has (read `include/morph/ui/value.hpp`, `parseJson`).

- [ ] **Step 6: Run the tests**

Run: `flock "$LOCK" cmake --build build/clang-debug --target morph_tests -j5 && flock "$LOCK" timeout 300 build/clang-debug/tests/morph_tests "[custom-screens]"`
Expected: `All tests passed`.

- [ ] **Step 7: Mutation checks.** Disable each in turn, rebuild, and confirm its case fails. Then restore and `touch`.
  - Remove the `screenAdmitted` check: "checks: 2, the base" fails.
  - Remove the `!custom->view` check: "checks: 3" fails.
  - Remove the `checkAccessibility` call: "checks: 4" fails.
  - Remove the permission loop: "checks: 6" fails.
  - Make `find` always return `std::nullopt`: "checkCustomScreen reads and fills the verdict cache" fails.

- [ ] **Step 8: Commit**

```bash
git add include/morph/ui/custom_screens.hpp src/ui/custom_screen_checks.cpp tests/ui_custom_screens_support.hpp \
        tests/test_ui_custom_screen_checks.cpp tests/CMakeLists.txt
git commit -m "ui: the checks a custom screen passes, and their verdict cache"
```

---

### Task 5: The store seam and the memory store

**Files:**
- Modify: `include/morph/ui/custom_screens.hpp`
- Create: `tests/test_ui_custom_screens.cpp`
- Modify: `tests/CMakeLists.txt`

**Interfaces:**
- Produces: `struct CustomScreenKey { std::string person, app, id; }`;
  `struct CustomScreen { std::string id, title, basedOn, document; time::DateTime savedAt; std::string savedBy; }`;
  `struct CustomScreenRefusal { std::string reason; }`; `class ICustomScreenStore` (`list`, `get`, `put`, `remove`);
  `class MemoryCustomScreenStore final : public ICustomScreenStore` with `explicit MemoryCustomScreenStore(exec::IExecutor& base)`.

- [ ] **Step 1: Write the failing tests.** Create `tests/test_ui_custom_screens.cpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0
//
// The custom screens' memory store, and the model through a RemoteServer.

#include <catch2/catch_test_macros.hpp>
#include <morph/core/completion.hpp>
#include <morph/core/executor.hpp>
#include <morph/ui/custom_screens.hpp>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "ui_custom_screens_support.hpp"

namespace {

/// The value of a completion the inline executor has already settled.
template <typename T>
T settled(morph::async::Completion<T> completion) {
    std::optional<T> out;
    completion.then([&out](T const& value) { out = value; });
    REQUIRE(out.has_value());
    return std::move(*out);
}

morph::ui::CustomScreen screen(std::string id, std::string person) {
    return morph::ui::CustomScreen{.id = std::move(id),
                                   .title = "Mine",
                                   .basedOn = "lims.base",
                                   .document = "{}",
                                   .savedAt = {},
                                   .savedBy = std::move(person)};
}

}  // namespace

TEST_CASE("MemoryCustomScreenStore keeps each person's screens apart", "[ui][custom-screens][store]") {
    morph::exec::InlineExecutor inline_;
    morph::ui::MemoryCustomScreenStore store{inline_};
    REQUIRE(settled(store.put({.person = "alice", .app = "lims", .id = "custom:1"}, screen("custom:1", "alice"))));
    REQUIRE(settled(store.put({.person = "bob", .app = "lims", .id = "custom:2"}, screen("custom:2", "bob"))));

    std::vector<morph::ui::CustomScreen> const alices = settled(store.list("alice", "lims"));
    REQUIRE(alices.size() == 1);
    CHECK(alices.front().id == "custom:1");
    CHECK(settled(store.list("alice", "other")).empty());

    CHECK(settled(store.get({.person = "alice", .app = "lims", .id = "custom:1"})).has_value());
    auto const notHers = settled(store.get({.person = "alice", .app = "lims", .id = "custom:2"}));
    REQUIRE_FALSE(notHers);
    CHECK(notHers.error().reason == "no such custom screen");

    CHECK_FALSE(settled(store.remove({.person = "alice", .app = "lims", .id = "custom:2"})));
    CHECK(settled(store.remove({.person = "alice", .app = "lims", .id = "custom:1"})));
    CHECK(settled(store.list("alice", "lims")).empty());
}
```

Register `test_ui_custom_screens.cpp` in `tests/CMakeLists.txt` after `test_ui_custom_screen_checks.cpp`.

- [ ] **Step 2: Run it to verify it fails**

Run: `flock "$LOCK" cmake --build build/clang-debug --target morph_tests -j5`
Expected: compile error, `no type named 'MemoryCustomScreenStore' in namespace 'morph::ui'`.

- [ ] **Step 3: Add the store** to `include/morph/ui/custom_screens.hpp`. Add the includes
  `#include "../core/completion.hpp"`, `#include "../core/executor.hpp"`, `#include "../core/owner_strand.hpp"`,
  `#include "../util/datetime.hpp"` and `#include <tuple>`. Before `checkBaseRule`, add:

```cpp
/// @brief Where one custom screen is stored.
struct CustomScreenKey {
    /// @brief The person, derived on the server from the verified principal; never taken from a request.
    std::string person;
    /// @brief The application.
    std::string app;
    /// @brief The screen's id, `custom:` and 32 hexadecimal digits.
    std::string id;
};

/// @brief A stored custom screen.
struct CustomScreen {
    /// @brief The id, `custom:` and 32 hexadecimal digits, issued by the model.
    std::string id;
    /// @brief The person's name for it, which names its entry.
    std::string title;
    /// @brief The id of the application screen it was made from.
    std::string basedOn;
    /// @brief The `ui/1` JSON text, its screen id already the custom screen's.
    std::string document;
    /// @brief When it was saved.
    time::DateTime savedAt;
    /// @brief The principal that saved it.
    std::string savedBy;
};

/// @brief A store's refusal, shown to the person as it is.
struct CustomScreenRefusal {
    /// @brief The reason, in one sentence.
    std::string reason;
};

/// @brief Stores custom screens. The application's implementation is where every policy over them lives.
///
/// `list` answers the screens a person may open, which may include screens others saved. `put` and `remove` refuse
/// what the person may not do, with a reason for the person. The model calls a store only with a person the server
/// derived and with a document that passed every check. A store owns the executor its completions are delivered on.
class ICustomScreenStore {
public:
    ICustomScreenStore() = default;
    ICustomScreenStore(ICustomScreenStore const&) = delete;
    ICustomScreenStore& operator=(ICustomScreenStore const&) = delete;
    ICustomScreenStore(ICustomScreenStore&&) = delete;
    ICustomScreenStore& operator=(ICustomScreenStore&&) = delete;
    virtual ~ICustomScreenStore() = default;

    /// @brief The custom screens @p person may open in @p app.
    /// @param person The person.
    /// @param app    The application.
    /// @return The screens, in the store's order.
    [[nodiscard]] virtual async::Completion<std::vector<CustomScreen>> list(std::string person, std::string app) = 0;

    /// @brief One custom screen the person may open.
    /// @param key The entry.
    /// @return The screen, or the refusal.
    [[nodiscard]] virtual async::Completion<std::expected<CustomScreen, CustomScreenRefusal>> get(
        CustomScreenKey key) = 0;

    /// @brief Stores a custom screen, replacing one of the same key.
    /// @param key    The entry.
    /// @param screen The screen.
    /// @return Nothing, or the refusal.
    [[nodiscard]] virtual async::Completion<std::expected<void, CustomScreenRefusal>> put(CustomScreenKey key,
                                                                                          CustomScreen screen) = 0;

    /// @brief Forgets a custom screen.
    /// @param key The entry.
    /// @return Nothing, or the refusal.
    [[nodiscard]] virtual async::Completion<std::expected<void, CustomScreenRefusal>> remove(CustomScreenKey key) = 0;
};

/// @brief The framework's store: a map held in memory, on its own strand, each person seeing and changing only
///        their own screens.
///
/// Its entries last as long as it does. Each call is a task on the strand, and its completion is delivered there.
class MemoryCustomScreenStore final : public ICustomScreenStore {
public:
    /// @brief A store whose strand runs on @p base.
    /// @param base Where the strand's tasks run. Borrowed: it must outlive the store.
    explicit MemoryCustomScreenStore(exec::IExecutor& base MORPH_LIFETIMEBOUND) : _strand{base} {}

    /// @brief The custom screens @p person saved in @p app.
    /// @param person The person.
    /// @param app    The application.
    /// @return The screens, ordered by id.
    [[nodiscard]] async::Completion<std::vector<CustomScreen>> list(std::string person, std::string app) override {
        auto [completion, promise] = async::Completion<std::vector<CustomScreen>>::makeSettleable(&_strand);
        _strand.postTask([this, who = std::move(person), where = std::move(app), settle = std::move(promise)] mutable {
            std::vector<CustomScreen> out;
            for (auto const& [key, screen] : _entries) {
                if (std::get<0>(key) == who && std::get<1>(key) == where) {
                    out.push_back(screen);
                }
            }
            settle.resolve(std::move(out));
        });
        return std::move(completion);
    }

    /// @brief One of the person's own screens.
    /// @param key The entry.
    /// @return The screen, or `no such custom screen`.
    [[nodiscard]] async::Completion<std::expected<CustomScreen, CustomScreenRefusal>> get(
        CustomScreenKey key) override {
        auto [completion, promise] =
            async::Completion<std::expected<CustomScreen, CustomScreenRefusal>>::makeSettleable(&_strand);
        _strand.postTask([this, entry = tie(key), settle = std::move(promise)] mutable {
            auto const found = _entries.find(entry);
            if (found == _entries.end()) {
                settle.resolve(std::unexpected(CustomScreenRefusal{.reason = "no such custom screen"}));
                return;
            }
            settle.resolve(found->second);
        });
        return std::move(completion);
    }

    /// @brief Stores a screen under the person's key.
    /// @param key    The entry.
    /// @param screen The screen.
    /// @return Nothing: the memory store refuses no write.
    [[nodiscard]] async::Completion<std::expected<void, CustomScreenRefusal>> put(CustomScreenKey key,
                                                                                  CustomScreen screen) override {
        auto [completion, promise] =
            async::Completion<std::expected<void, CustomScreenRefusal>>::makeSettleable(&_strand);
        _strand.postTask([this, entry = tie(key), kept = std::move(screen), settle = std::move(promise)] mutable {
            _entries.insert_or_assign(std::move(entry), std::move(kept));
            settle.resolve(std::expected<void, CustomScreenRefusal>{});
        });
        return std::move(completion);
    }

    /// @brief Forgets one of the person's own screens.
    /// @param key The entry.
    /// @return Nothing, or `no such custom screen`.
    [[nodiscard]] async::Completion<std::expected<void, CustomScreenRefusal>> remove(CustomScreenKey key) override {
        auto [completion, promise] =
            async::Completion<std::expected<void, CustomScreenRefusal>>::makeSettleable(&_strand);
        _strand.postTask([this, entry = tie(key), settle = std::move(promise)] mutable {
            if (_entries.erase(entry) == 0) {
                settle.resolve(std::unexpected(CustomScreenRefusal{.reason = "no such custom screen"}));
                return;
            }
            settle.resolve(std::expected<void, CustomScreenRefusal>{});
        });
        return std::move(completion);
    }

private:
    using Key = std::tuple<std::string, std::string, std::string>;

    [[nodiscard]] static Key tie(CustomScreenKey const& key) { return Key{key.person, key.app, key.id}; }

    std::map<Key, CustomScreen> _entries;
    /// After the map, so it is destroyed first: a task running on it reads the map.
    exec::OwnerStrand _strand;
};
```

- [ ] **Step 4: Run the test**

Run: `flock "$LOCK" cmake --build build/clang-debug --target morph_tests -j5 && flock "$LOCK" timeout 300 build/clang-debug/tests/morph_tests "[store]"`
Expected: `All tests passed`. If `OwnerStrand` over an `InlineExecutor` does not settle synchronously (the
`settled` helper's `REQUIRE` fails), the strand defers tasks. In that case, test the memory store through the server of
Task 6 instead, delete this case, and say so in the task report.

- [ ] **Step 5: Commit**

```bash
git add include/morph/ui/custom_screens.hpp tests/test_ui_custom_screens.cpp tests/CMakeLists.txt
git commit -m "ui: the custom screen store seam, and the framework's memory store"
```

---

### Task 6: The model, its registration, its record and its effects

**Files:**
- Modify: `include/morph/ui/custom_screens.hpp`
- Create: `src/ui/custom_screens.cpp`
- Modify: `CMakeLists.txt`
- Modify: `tests/ui_custom_screens_support.hpp` (add `Server`)
- Modify: `tests/test_ui_custom_screens.cpp`

**Interfaces:**
- Consumes: everything above.
- Produces:
  - `kCustomScreensModel = "morph.customScreens"`, `kCustomScreenRecord = "morph.customScreens"`, and the action ids
    `kCustomScreensList = "morph.customScreens.list"`, `kCustomScreensGet = "morph.customScreens.get"`,
    `kCustomScreensCheck = "morph.customScreens.check"`, `kCustomScreensSave = "morph.customScreens.save"`,
    `kCustomScreensRemove = "morph.customScreens.remove"`
  - Wire types: `CheckRefusal{pointer, reason}`;
    `CustomScreenSummary{id, title, basedOn, savedAt, savedBy, mine, opensWithoutParams, broken}`;
    `CustomScreensList{app}` → `CustomScreensListed{screens}`; `CustomScreensGet{app, id}` →
    `CustomScreenFound{summary, document}`; `CustomScreensCheck{app, basedOn, document}` →
    `CustomScreenChecked{refusal}`; `CustomScreensSave{app, id?, title, basedOn, document}` → `CustomScreenSaved{id}`;
    `CustomScreensRemove{app, id}` → `CustomScreenRemoved{removed}`
  - `class CustomScreensModel`
  - `void registerCustomScreens(std::shared_ptr<ICustomScreenStore> store, std::function<std::shared_ptr<ServedBundle const>()> bundle, std::shared_ptr<session::IAuthorizer const> authorizer, model::detail::ActionDispatcher& dispatcher, model::detail::ModelRegistryFactory& registry, CustomScreensOptions options = {})`

**Why the record and effects are static, in `src/ui/custom_screens.cpp`.** Records, action result slots and effects
are process-level registries. They resolve once, at the first read, and a debug build closes the registration latch at
that first read. Registering them from `registerCustomScreens` at run time would assert in every test binary that has
already read them, and would miss the resolution in an application that read first. So they are registered by static
initialisers in the same translation unit as `registerCustomScreens`:

- In a static build, the unit is linked only into a server that calls `registerCustomScreens`.
- In a shared build (`MORPH_BUILD_SHARED`), every application that loads `libmorph` gets the record
  `morph.customScreens`, five action result entries and two effects declarations.

That is inert. The entries are self-consistent (`changes<…>` asks nothing of a result). The action ids carry the
`morph.customScreens.` prefix, so they cannot clash with an application's. The record name `morph.customScreens` is
reserved by the framework's `morph.` prefix. Step 8 verifies that the whole registry still resolves.

- [ ] **Step 1: Add the server fixture** to `tests/ui_custom_screens_support.hpp`. Add these includes: `<chrono>`,
  `<cstdint>`, `<future>`, `<glaze/glaze.hpp>`, `<morph/core/executor.hpp>`, `<morph/core/registry.hpp>`,
  `<morph/core/remote.hpp>`, `<morph/core/wire.hpp>`, `<morph/ui/custom_screens.hpp>`. Then add, before the
  namespace's closing brace:

```cpp
using morph::wire::Envelope;

/// @p text as a JSON string literal, quotes included.
inline std::string quoted(std::string const& text) { return glz::write_json(text).value_or("\"\""); }

/// A RemoteServer that offers custom screens over @p store, checking against the fixture bundle.
struct Server {
    explicit Server(std::shared_ptr<morph::ui::ICustomScreenStore> chosen = nullptr,
                    morph::ui::CustomScreensOptions options = {})
        : store{chosen != nullptr ? std::move(chosen) : std::make_shared<morph::ui::MemoryCustomScreenStore>(pool)} {
        morph::ui::registerCustomScreens(store, [held = served] { return held; }, authorizer, dispatcher, registry,
                                         options);
        server = std::make_shared<morph::backend::RemoteServer>(pool, authorizer, dispatcher, registry);
        // NOLINTNEXTLINE(cppcoreguidelines-prefer-member-initializer): bind() needs the server built above.
        instance = bind();
    }

    Envelope send(Envelope const& request) {
        std::promise<std::string> replied;
        auto reply = replied.get_future();
        server->handle(morph::wire::encode(request),
                       [&replied](std::string text) { replied.set_value(std::move(text)); });
        REQUIRE(reply.wait_for(std::chrono::seconds{5}) == std::future_status::ready);
        return morph::wire::decode(reply.get());
    }

    std::uint64_t bind() {
        Envelope const reply = send(morph::wire::makeRegister(std::string{morph::ui::kCustomScreensModel}));
        REQUIRE(reply.kind == "ok");
        return reply.modelId;
    }

    /// Runs @p action with @p body as the caller whose token is @p token.
    Envelope call(std::string_view action, std::string body, std::string token) {
        Envelope request;
        request.kind = "execute";
        request.callId = ++lastCallId;
        request.modelId = instance;
        request.modelType = std::string{morph::ui::kCustomScreensModel};
        request.actionType = std::string{action};
        request.body = std::move(body);
        request.session.token = std::move(token);
        return send(request);
    }

    /// Saves @p document as a new custom screen of @p token's, based on `lims.base`.
    Envelope save(std::string const& document, std::string token, std::string const& title = "Mine") {
        return call(morph::ui::kCustomScreensSave,
                    R"({"app":"lims","title":)" + quoted(title) + R"(,"basedOn":"lims.base","document":)" +
                        quoted(document) + "}",
                    std::move(token));
    }

    morph::exec::ThreadPoolExecutor pool{2};
    morph::model::detail::ActionDispatcher dispatcher;
    morph::model::detail::ModelRegistryFactory registry;
    std::shared_ptr<morph::ui::ServedBundle const> served = bundle();
    std::shared_ptr<uiservice::RuleAuthorizer> authorizer = std::make_shared<uiservice::RuleAuthorizer>();
    std::shared_ptr<morph::ui::ICustomScreenStore> store;
    std::shared_ptr<morph::backend::RemoteServer> server;
    std::uint64_t instance = 0;
    std::uint64_t lastCallId = 0;
};
```

`pool` is declared before `store`, which the constructor's initializer reads. Keep the member order as written.

- [ ] **Step 2: Write the failing server tests** (append to `tests/test_ui_custom_screens.cpp`; add
  `#include <morph/core/effects.hpp>` and `#include <algorithm>`)

```cpp
using screens_test::baseScreen;
using screens_test::Envelope;
using screens_test::quoted;
using screens_test::Server;

namespace {

/// The `list` reply's screens.
morph::ui::CustomScreensListed listed(Server& s, std::string token) {
    Envelope const reply = s.call(morph::ui::kCustomScreensList, R"({"app":"lims"})", std::move(token));
    REQUIRE(reply.kind == "ok");
    morph::ui::CustomScreensListed out;
    REQUIRE_FALSE(glz::read_json(out, reply.body));
    return out;
}

/// The id a save answered.
std::string savedId(Envelope const& reply) {
    REQUIRE(reply.kind == "ok");
    morph::ui::CustomScreenSaved out;
    REQUIRE_FALSE(glz::read_json(out, reply.body));
    return out.id;
}

}  // namespace

TEST_CASE("custom screens: a save is listed and read back under its own id", "[ui][custom-screens][remote]") {
    Server s;
    std::string const id = savedId(s.save(baseScreen("lims.base"), "alice"));
    CHECK(id.starts_with("custom:"));

    auto const mine = listed(s, "alice");
    REQUIRE(mine.screens.size() == 1);
    CHECK(mine.screens.front().id == id);
    CHECK(mine.screens.front().savedBy == "alice");
    CHECK(mine.screens.front().mine);
    CHECK(mine.screens.front().opensWithoutParams);
    CHECK_FALSE(mine.screens.front().broken.has_value());

    Envelope const got = s.call(morph::ui::kCustomScreensGet, R"({"app":"lims","id":)" + quoted(id) + "}", "alice");
    REQUIRE(got.kind == "ok");
    morph::ui::CustomScreenFound found;
    REQUIRE_FALSE(glz::read_json(found, got.body));
    CHECK(screens_test::load(found.document).id == id);
}

TEST_CASE("custom screens: one person never reaches another's", "[ui][custom-screens][remote]") {
    Server s;
    std::string const bobs = savedId(s.save(baseScreen("lims.base"), "bob"));
    CHECK(listed(s, "alice").screens.empty());
    Envelope const got =
        s.call(morph::ui::kCustomScreensGet, R"({"app":"lims","id":)" + quoted(bobs) + R"(,"person":"bob"})", "alice");
    REQUIRE(got.kind == "err");
    CHECK(got.message.contains("no such custom screen"));
    Envelope const overwrite = s.call(morph::ui::kCustomScreensSave,
                                      R"({"app":"lims","id":)" + quoted(bobs) +
                                          R"(,"title":"Taken","basedOn":"lims.base","document":)" +
                                          quoted(baseScreen("lims.base")) + "}",
                                      "alice");
    REQUIRE(overwrite.kind == "err");
    CHECK(overwrite.message.contains("no such custom screen"));
    CHECK(listed(s, "bob").screens.front().title == "Mine");
}

TEST_CASE("custom screens: without a verified person nothing is read or written", "[ui][custom-screens][remote]") {
    Server s;
    CHECK(listed(s, "").screens.empty());
    Envelope const write = s.save(baseScreen("lims.base"), "");
    REQUIRE(write.kind == "err");
    CHECK(write.message.contains("signed-in person"));
    CHECK(s.call(morph::ui::kCustomScreensCheck,
                 R"({"app":"lims","basedOn":"lims.base","document":)" + quoted(baseScreen("lims.base")) + "}", "")
              .kind == "err");
    CHECK(s.call(morph::ui::kCustomScreensRemove, R"({"app":"lims","id":"custom:x"})", "").kind == "err");
}

TEST_CASE("custom screens: check answers the refusal and its pointer; save refuses the same",
          "[ui][custom-screens][remote]") {
    Server s;
    std::string const out =
        screens_test::replaced(baseScreen("lims.base"), R"({"host":"print"})", R"({"signOut":true})");
    Envelope const checked = s.call(morph::ui::kCustomScreensCheck,
                                    R"({"app":"lims","basedOn":"lims.base","document":)" + quoted(out) + "}", "alice");
    REQUIRE(checked.kind == "ok");
    morph::ui::CustomScreenChecked verdict;
    REQUIRE_FALSE(glz::read_json(verdict, checked.body));
    REQUIRE(verdict.refusal.has_value());
    CHECK(verdict.refusal->reason.contains("signOut"));
    CHECK_FALSE(verdict.refusal->pointer.empty());

    Envelope const saved = s.save(out, "alice");
    REQUIRE(saved.kind == "err");
    CHECK(saved.message.contains("signOut"));
    CHECK(listed(s, "alice").screens.empty());
}

TEST_CASE("custom screens: a store's refusal reaches the person", "[ui][custom-screens][remote]") {
    struct Refusing final : morph::ui::ICustomScreenStore {
        explicit Refusing(morph::exec::IExecutor& base) : inner{base} {}
        morph::async::Completion<std::vector<morph::ui::CustomScreen>> list(std::string p, std::string a) override {
            return inner.list(std::move(p), std::move(a));
        }
        morph::async::Completion<std::expected<morph::ui::CustomScreen, morph::ui::CustomScreenRefusal>> get(
            morph::ui::CustomScreenKey k) override {
            return inner.get(std::move(k));
        }
        morph::async::Completion<std::expected<void, morph::ui::CustomScreenRefusal>> put(
            morph::ui::CustomScreenKey /*k*/, morph::ui::CustomScreen /*s*/) override {
            return morph::async::Completion<std::expected<void, morph::ui::CustomScreenRefusal>>::resolved(
                std::unexpected(morph::ui::CustomScreenRefusal{.reason = "over your limit of custom screens"}));
        }
        morph::async::Completion<std::expected<void, morph::ui::CustomScreenRefusal>> remove(
            morph::ui::CustomScreenKey k) override {
            return inner.remove(std::move(k));
        }
        morph::ui::MemoryCustomScreenStore inner;
    };
    morph::exec::ThreadPoolExecutor storePool{1};
    Server s{std::make_shared<Refusing>(storePool)};
    Envelope const saved = s.save(baseScreen("lims.base"), "alice");
    REQUIRE(saved.kind == "err");
    CHECK(saved.message.contains("over your limit of custom screens"));
}

TEST_CASE("custom screens: a screen the person has since lost permission for is listed broken",
          "[ui][custom-screens][remote]") {
    Server s;
    REQUIRE(s.save(baseScreen("lims.base"), "alice").kind == "ok");
    s.authorizer->denied["alice"] = {{"Lims_Sample", "Lims_Verify"}};
    auto const mine = listed(s, "alice");
    REQUIRE(mine.screens.size() == 1);
    REQUIRE(mine.screens.front().broken.has_value());
    CHECK(mine.screens.front().broken->pointer == "/mutations/verify");
}

TEST_CASE("custom screens: save with an id overwrites, remove removes", "[ui][custom-screens][remote]") {
    Server s;
    std::string const id = savedId(s.save(baseScreen("lims.base"), "alice"));
    Envelope const again = s.call(morph::ui::kCustomScreensSave,
                                  R"({"app":"lims","id":)" + quoted(id) +
                                      R"(,"title":"Renamed","basedOn":"lims.base","document":)" +
                                      quoted(baseScreen("lims.base")) + "}",
                                  "alice");
    CHECK(savedId(again) == id);
    auto const mine = listed(s, "alice");
    REQUIRE(mine.screens.size() == 1);
    CHECK(mine.screens.front().title == "Renamed");

    Envelope const removed =
        s.call(morph::ui::kCustomScreensRemove, R"({"app":"lims","id":)" + quoted(id) + "}", "alice");
    REQUIRE(removed.kind == "ok");
    CHECK(listed(s, "alice").screens.empty());
    CHECK(s.call(morph::ui::kCustomScreensRemove, R"({"app":"lims","id":)" + quoted(id) + "}", "alice").kind ==
          "err");
}

TEST_CASE("custom screens: lists hold the record that saves and removes change", "[ui][custom-screens][effects]") {
    std::vector<std::string> const holding = morph::effects::actionsHolding(morph::ui::kCustomScreenRecord);
    CHECK(std::ranges::find(holding, std::string{morph::ui::kCustomScreensList}) != holding.end());
    CHECK(std::ranges::find(holding, std::string{morph::ui::kCustomScreensGet}) != holding.end());
    for (std::string_view const action : {morph::ui::kCustomScreensSave, morph::ui::kCustomScreensRemove}) {
        auto const effects = morph::effects::effectsOf(action);
        REQUIRE(effects.has_value());
        REQUIRE(effects->size() == 1);
        CHECK(effects->front().kind == morph::effects::Kind::Changes);
        CHECK(effects->front().record == morph::ui::kCustomScreenRecord);
    }
    CHECK(morph::model::ActionTraits<morph::ui::CustomScreensList>::typeId() == morph::ui::kCustomScreensList);
}
```

If `Completion<T>::resolved(...)` is not the name of the ready-made constructor in `include/morph/core/completion.hpp`,
use the name it has. If it has none, settle a `makeSettleable` pair at once with `nullptr` as the executor, as that
header documents.

- [ ] **Step 3: Run them to verify they fail**

Run: `flock "$LOCK" cmake --build build/clang-debug --target morph_tests -j5`
Expected: compile error, `no member named 'registerCustomScreens' in namespace 'morph::ui'`.

- [ ] **Step 4: Declare the model** in `include/morph/ui/custom_screens.hpp`. Add the includes
  `#include "../core/coroutine.hpp"`, `#include "../core/model.hpp"`, `#include "../core/registry.hpp"`,
  `#include <memory>`. After `withScreenId`, add:

```cpp
/// @brief The type id the custom screens model is registered under.
inline constexpr std::string_view kCustomScreensModel = "morph.customScreens";
/// @brief The record type a custom screen's summary is, which `save` and `remove` change.
inline constexpr std::string_view kCustomScreenRecord = "morph.customScreens";
/// @brief The `list` action's id.
inline constexpr std::string_view kCustomScreensList = "morph.customScreens.list";
/// @brief The `get` action's id.
inline constexpr std::string_view kCustomScreensGet = "morph.customScreens.get";
/// @brief The `check` action's id.
inline constexpr std::string_view kCustomScreensCheck = "morph.customScreens.check";
/// @brief The `save` action's id.
inline constexpr std::string_view kCustomScreensSave = "morph.customScreens.save";
/// @brief The `remove` action's id.
inline constexpr std::string_view kCustomScreensRemove = "morph.customScreens.remove";

/// @brief A check's refusal on the wire.
struct CheckRefusal {
    /// @brief The JSON Pointer of the refused part; empty for the whole document.
    std::string pointer;
    /// @brief The reason, in one sentence.
    std::string reason;
};

/// @brief One custom screen as a list shows it: the record `save` and `remove` change.
struct CustomScreenSummary {
    /// @brief The id.
    std::string id;
    /// @brief The person's name for it.
    std::string title;
    /// @brief The application screen it was made from.
    std::string basedOn;
    /// @brief When it was saved.
    time::DateTime savedAt;
    /// @brief Who saved it.
    std::string savedBy;
    /// @brief Whether the caller saved it.
    bool mine = false;
    /// @brief Whether it declares no params, so it opens from the navigator.
    bool opensWithoutParams = false;
    /// @brief The refusal the checks give it today; absent when it may be mounted.
    std::optional<CheckRefusal> broken;
};

/// @brief `list`: the caller's custom screens of an application.
struct CustomScreensList {
    /// @brief The application.
    std::string app;
};

/// @brief What `list` answers.
struct CustomScreensListed {
    /// @brief The screens, in the store's order.
    std::vector<CustomScreenSummary> screens;
};

/// @brief `get`: one custom screen.
struct CustomScreensGet {
    /// @brief The application.
    std::string app;
    /// @brief The screen's id.
    std::string id;
};

/// @brief What `get` answers.
struct CustomScreenFound {
    /// @brief The screen as a list shows it.
    CustomScreenSummary summary;
    /// @brief Its document's JSON text.
    std::string document;
};

/// @brief `check`: whether a document would be saved.
struct CustomScreensCheck {
    /// @brief The application.
    std::string app;
    /// @brief The base screen's id.
    std::string basedOn;
    /// @brief The document's JSON text.
    std::string document;
};

/// @brief What `check` answers.
struct CustomScreenChecked {
    /// @brief The first refusal; absent when the document would be saved.
    std::optional<CheckRefusal> refusal;
};

/// @brief `save`: stores a custom screen.
struct CustomScreensSave {
    /// @brief The application.
    std::string app;
    /// @brief The screen to overwrite, one `get` answers for the caller; absent saves a new screen.
    std::optional<std::string> id;
    /// @brief The person's name for it.
    std::string title;
    /// @brief The base screen's id.
    std::string basedOn;
    /// @brief The document's JSON text.
    std::string document;
};

/// @brief What `save` answers.
struct CustomScreenSaved {
    /// @brief The screen's id.
    std::string id;
};

/// @brief `remove`: forgets a custom screen.
struct CustomScreensRemove {
    /// @brief The application.
    std::string app;
    /// @brief The screen's id.
    std::string id;
};

/// @brief What `remove` answers.
struct CustomScreenRemoved {
    /// @brief Whether it was removed.
    bool removed = false;
};

namespace detail {
struct CustomScreensContext;
}  // namespace detail

/// @brief The server's custom screens model: the caller's screens, through the application's store, checked.
///
/// The person is the principal of the verified session, read once at the start of each action, before anything is
/// awaited. No request names a person, and one that tries is ignored. With no verified principal, `list` answers
/// nothing and every other action is refused.
class MORPH_API CustomScreensModel {
public:
    /// @brief A model over @p context.
    /// @param context The store, the bundle, the authoriser, the bounds and the verdict cache. Shared by every
    ///                instance.
    explicit CustomScreensModel(std::shared_ptr<detail::CustomScreensContext> context);

    /// @brief The caller's custom screens, each checked today.
    /// @param request The application.
    /// @return The screens.
    ::core::async::Task<CustomScreensListed> execute(CustomScreensList request);

    /// @brief One of the caller's custom screens.
    /// @param request The application and the id.
    /// @return The screen and its document.
    ::core::async::Task<CustomScreenFound> execute(CustomScreensGet request);

    /// @brief Whether a document would be saved.
    /// @param request The application, the base and the document.
    /// @return The first refusal, or none.
    ::core::async::Task<CustomScreenChecked> execute(CustomScreensCheck request);

    /// @brief Stores a custom screen.
    /// @param request The application, the id to overwrite if any, the title, the base and the document.
    /// @return The screen's id.
    ::core::async::Task<CustomScreenSaved> execute(CustomScreensSave request);

    /// @brief Forgets a custom screen.
    /// @param request The application and the id.
    /// @return That it was removed.
    ::core::async::Task<CustomScreenRemoved> execute(CustomScreensRemove request);

private:
    std::shared_ptr<detail::CustomScreensContext> _context;
};

}  // namespace morph::ui

/// @cond detail
// The traits the registration macros write, without their static-init registrars: registerCustomScreens registers
// the model and its actions; the record, the result slots and the effects are registered in custom_screens.cpp.
// NOLINTBEGIN(misc-const-correctness): inside the macros' codec bodies.
BRIDGE_DETAIL_MODEL_TRAITS_ONLY_BODY(morph::ui::CustomScreensModel, "morph.customScreens")
BRIDGE_DETAIL_ACTION_TRAITS_ONLY_BODY(morph::ui::CustomScreensModel, morph::ui::CustomScreensList,
                                      morph::ui::CustomScreensListed, "morph.customScreens.list",
                                      ::morph::model::Loggable::No)
BRIDGE_DETAIL_ACTION_TRAITS_ONLY_BODY(morph::ui::CustomScreensModel, morph::ui::CustomScreensGet,
                                      morph::ui::CustomScreenFound, "morph.customScreens.get",
                                      ::morph::model::Loggable::No)
BRIDGE_DETAIL_ACTION_TRAITS_ONLY_BODY(morph::ui::CustomScreensModel, morph::ui::CustomScreensCheck,
                                      morph::ui::CustomScreenChecked, "morph.customScreens.check",
                                      ::morph::model::Loggable::No)
BRIDGE_DETAIL_ACTION_TRAITS_ONLY_BODY(morph::ui::CustomScreensModel, morph::ui::CustomScreensSave,
                                      morph::ui::CustomScreenSaved, "morph.customScreens.save",
                                      ::morph::model::Loggable::No)
BRIDGE_DETAIL_ACTION_TRAITS_ONLY_BODY(morph::ui::CustomScreensModel, morph::ui::CustomScreensRemove,
                                      morph::ui::CustomScreenRemoved, "morph.customScreens.remove",
                                      ::morph::model::Loggable::No)
// NOLINTEND(misc-const-correctness)
/// @endcond

namespace morph::ui {

/// @brief Registers the custom screens model and its five actions, with a factory that gives every instance one
///        shared context.
///
/// A server that offers custom screens calls this once, before it serves, with the dispatcher and registry its
/// `RemoteServer` uses. Its authoriser then admits `morph.customScreens` and its actions for every signed-in
/// principal; the model keeps each person to what the store answers them.
/// @param store      Where custom screens are kept, and the application's policy over them.
/// @param bundle     The bundle being served now; called at every action, so a development reload is seen.
/// @param authorizer The server's authoriser, for the permitted set and the base screen's admission.
/// @param dispatcher The server's action dispatcher.
/// @param registry   The server's model registry.
/// @param options    The bounds. `maxDocumentBytes` is at most `kMaxFileBytes`.
MORPH_API void registerCustomScreens(std::shared_ptr<ICustomScreenStore> store,
                                     std::function<std::shared_ptr<ServedBundle const>()> bundle,
                                     std::shared_ptr<::morph::session::IAuthorizer const> authorizer,
                                     model::detail::ActionDispatcher& dispatcher,
                                     model::detail::ModelRegistryFactory& registry, CustomScreensOptions options = {});
```

The traits macros need the namespace closed. That is why the header closes `morph::ui` before them and reopens it
after, as `preferences.hpp` does. Keep the earlier declarations in the first namespace block.

- [ ] **Step 5: Implement the model.** Create `src/ui/custom_screens.cpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0

#include <core/async/Task.hpp>
#include <functional>
#include <memory>
#include <morph/core/effects.hpp>
#include <morph/core/records.hpp>
#include <morph/core/registry.hpp>
#include <morph/session/session.hpp>
#include <morph/ui/bundle.hpp>
#include <morph/ui/custom_screens.hpp>
#include <morph/ui/file_store.hpp>
#include <morph/ui/service.hpp>
#include <morph/ui/value.hpp>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace morph::ui {
namespace detail {

/// @brief What every custom screens instance shares.
struct CustomScreensContext {
    CustomScreensContext(std::shared_ptr<ICustomScreenStore> keptStore,
                         std::function<std::shared_ptr<ServedBundle const>()> currentBundle,
                         std::shared_ptr<::morph::session::IAuthorizer const> keptAuthorizer,
                         CustomScreensOptions const& bounds)
        : store{std::move(keptStore)},
          bundle{std::move(currentBundle)},
          authorizer{std::move(keptAuthorizer)},
          options{bounds},
          verdicts{bounds.verdictCacheSize} {}

    std::shared_ptr<ICustomScreenStore> store;
    std::function<std::shared_ptr<ServedBundle const>()> bundle;
    std::shared_ptr<::morph::session::IAuthorizer const> authorizer;
    CustomScreensOptions options;
    VerdictCache verdicts;
};

}  // namespace detail

namespace {

/// @brief The session of the action being run, copied: the session is the dispatching thread's, and an action
///        resumes elsewhere after its first await.
[[nodiscard]] ::morph::session::Context callerSession() {
    ::morph::session::Context const* const session = ::morph::session::current();
    return session == nullptr ? ::morph::session::Context{} : *session;
}

/// @brief The verified person of @p session, for an action that needs one.
[[nodiscard]] std::string const& requirePerson(::morph::session::Context const& session) {
    if (session.principal.empty()) {
        throw std::runtime_error{"custom screens are stored for a signed-in person"};
    }
    return session.principal;
}

/// @brief Refuses an empty or overlong name.
void checkName(std::string const& name, std::string_view what) {
    if (name.empty() || name.size() > kMaxNameBytes) {
        throw std::invalid_argument{"a custom screen's `" + std::string{what} + "` is 1 to " +
                                    std::to_string(kMaxNameBytes) + " bytes"};
    }
}

[[nodiscard]] std::optional<CheckRefusal> onWire(std::optional<Refusal> const& refusal) {
    if (!refusal) {
        return std::nullopt;
    }
    return CheckRefusal{.pointer = refusal->path, .reason = refusal->reason};
}

/// @brief Whether @p document declares no params, read from its JSON.
[[nodiscard]] bool declaresNoParams(std::string_view document) {
    auto const parsed = parseJson(document);
    if (!parsed) {
        return false;
    }
    auto const* const object = std::get_if<Json::object_t>(&parsed->data);
    if (object == nullptr) {
        return false;
    }
    auto const params = object->find("params");
    if (params == object->end()) {
        return true;
    }
    auto const* const declared = std::get_if<Json::object_t>(&params->second.data);
    return declared != nullptr && declared->empty();
}

}  // namespace

CustomScreensModel::CustomScreensModel(std::shared_ptr<detail::CustomScreensContext> context)
    : _context{std::move(context)} {
    if (_context == nullptr) {
        throw std::invalid_argument{"CustomScreensModel: a null context"};
    }
}

namespace {

[[nodiscard]] CustomScreenSummary summarize(detail::CustomScreensContext& context, ServedBundle const& bundle,
                                            ::morph::session::Context const& session, CustomScreen const& screen) {
    return CustomScreenSummary{
        .id = screen.id,
        .title = screen.title,
        .basedOn = screen.basedOn,
        .savedAt = screen.savedAt,
        .savedBy = screen.savedBy,
        .mine = screen.savedBy == session.principal,
        .opensWithoutParams = declaresNoParams(screen.document),
        .broken = onWire(checkCustomScreen(bundle, session, *context.authorizer, screen.basedOn, screen.document,
                                           context.options, &context.verdicts)),
    };
}

}  // namespace

::core::async::Task<CustomScreensListed> CustomScreensModel::execute(CustomScreensList request) {
    checkName(request.app, "app");
    ::morph::session::Context const session = callerSession();
    CustomScreensListed reply;
    if (session.principal.empty()) {
        co_return reply;
    }
    std::vector<CustomScreen> const stored = co_await _context->store->list(session.principal, request.app);
    std::shared_ptr<ServedBundle const> const bundle = _context->bundle();
    for (CustomScreen const& screen : stored) {
        reply.screens.push_back(summarize(*_context, *bundle, session, screen));
    }
    co_return reply;
}

::core::async::Task<CustomScreenFound> CustomScreensModel::execute(CustomScreensGet request) {
    checkName(request.app, "app");
    checkName(request.id, "id");
    ::morph::session::Context const session = callerSession();
    std::string const person = requirePerson(session);
    auto found = co_await _context->store->get(CustomScreenKey{.person = person, .app = request.app, .id = request.id});
    if (!found) {
        throw std::runtime_error{found.error().reason};
    }
    std::shared_ptr<ServedBundle const> const bundle = _context->bundle();
    CustomScreenSummary summary = summarize(*_context, *bundle, session, *found);
    co_return CustomScreenFound{.summary = std::move(summary), .document = std::move(found->document)};
}

::core::async::Task<CustomScreenChecked> CustomScreensModel::execute(CustomScreensCheck request) {
    checkName(request.app, "app");
    checkName(request.basedOn, "basedOn");
    ::morph::session::Context const session = callerSession();
    static_cast<void>(requirePerson(session));
    std::shared_ptr<ServedBundle const> const bundle = _context->bundle();
    co_return CustomScreenChecked{.refusal = onWire(checkCustomScreen(*bundle, session, *_context->authorizer,
                                                                      request.basedOn, request.document,
                                                                      _context->options, &_context->verdicts))};
}

::core::async::Task<CustomScreenSaved> CustomScreensModel::execute(CustomScreensSave request) {
    checkName(request.app, "app");
    checkName(request.title, "title");
    checkName(request.basedOn, "basedOn");
    ::morph::session::Context const session = callerSession();
    std::string const person = requirePerson(session);
    if (request.id) {
        checkName(*request.id, "id");
        auto existing = co_await _context->store->get(CustomScreenKey{.person = person, .app = request.app, .id = *request.id});
        if (!existing) {
            throw std::runtime_error{"no such custom screen"};
        }
    }
    std::shared_ptr<ServedBundle const> const bundle = _context->bundle();
    if (auto refusal = checkCustomScreen(*bundle, session, *_context->authorizer, request.basedOn, request.document,
                                         _context->options, &_context->verdicts)) {
        throw std::invalid_argument{refusal->message()};
    }
    std::string const id = request.id ? *request.id : newFileRef(kCustomScreenIdPrefix);
    auto text = withScreenId(request.document, id);
    if (!text) {
        throw std::invalid_argument{text.error().message()};
    }
    auto stored = co_await _context->store->put(CustomScreenKey{.person = person, .app = request.app, .id = id},
                                                CustomScreen{.id = id,
                                                             .title = std::move(request.title),
                                                             .basedOn = std::move(request.basedOn),
                                                             .document = std::move(*text),
                                                             .savedAt = time::DateTime::now(),
                                                             .savedBy = person});
    if (!stored) {
        throw std::runtime_error{stored.error().reason};
    }
    co_return CustomScreenSaved{.id = id};
}

::core::async::Task<CustomScreenRemoved> CustomScreensModel::execute(CustomScreensRemove request) {
    checkName(request.app, "app");
    checkName(request.id, "id");
    ::morph::session::Context const session = callerSession();
    std::string const person = requirePerson(session);
    auto removed = co_await _context->store->remove(CustomScreenKey{.person = person, .app = request.app, .id = request.id});
    if (!removed) {
        throw std::runtime_error{removed.error().reason};
    }
    co_return CustomScreenRemoved{.removed = true};
}

void registerCustomScreens(std::shared_ptr<ICustomScreenStore> store,
                           std::function<std::shared_ptr<ServedBundle const>()> bundle,
                           std::shared_ptr<::morph::session::IAuthorizer const> authorizer,
                           model::detail::ActionDispatcher& dispatcher, model::detail::ModelRegistryFactory& registry,
                           CustomScreensOptions options) {
    if (store == nullptr || !bundle || authorizer == nullptr) {
        throw std::invalid_argument{"registerCustomScreens: a null store, bundle or authorizer"};
    }
    if (options.maxDocumentBytes > kMaxFileBytes) {
        throw std::invalid_argument{"registerCustomScreens: maxDocumentBytes is at most kMaxFileBytes, so every "
                                    "stored screen can be served"};
    }
    auto context = std::make_shared<detail::CustomScreensContext>(std::move(store), std::move(bundle),
                                                                  std::move(authorizer), options);
    registry.registerModel<CustomScreensModel>(kCustomScreensModel, [context] {
        return std::make_unique<model::detail::ModelHolder<CustomScreensModel>>(context);
    });
    dispatcher.registerAction<CustomScreensModel, CustomScreensList>(kCustomScreensModel, kCustomScreensList);
    dispatcher.registerAction<CustomScreensModel, CustomScreensGet>(kCustomScreensModel, kCustomScreensGet);
    dispatcher.registerAction<CustomScreensModel, CustomScreensCheck>(kCustomScreensModel, kCustomScreensCheck);
    dispatcher.registerAction<CustomScreensModel, CustomScreensSave>(kCustomScreensModel, kCustomScreensSave);
    dispatcher.registerAction<CustomScreensModel, CustomScreensRemove>(kCustomScreensModel, kCustomScreensRemove);
}

}  // namespace morph::ui

// The process-level registries resolve once, at their first read, so the record, the result slots that let a list
// be found holding it, and the effects of save and remove are registered here, at static initialisation, in the
// translation unit a server links by calling registerCustomScreens.
BRIDGE_REGISTER_RECORD(morph::ui::CustomScreenSummary, "morph.customScreens")
BRIDGE_REGISTER_EFFECTS(morph::ui::CustomScreensSave, morph::effects::changes<morph::ui::CustomScreenSummary>)
BRIDGE_REGISTER_EFFECTS(morph::ui::CustomScreensRemove, morph::effects::changes<morph::ui::CustomScreenSummary>)
#ifndef MORPH_CLIENT_ONLY
namespace {
// NOLINTNEXTLINE(cert-err58-cpp): the registrars are noexcept.
[[maybe_unused]] bool const customScreenResults =
    morph::model::detail::registerActionResultOnce<morph::ui::CustomScreensList>(morph::ui::kCustomScreensList) &&
    morph::model::detail::registerActionResultOnce<morph::ui::CustomScreensGet>(morph::ui::kCustomScreensGet) &&
    morph::model::detail::registerActionResultOnce<morph::ui::CustomScreensCheck>(morph::ui::kCustomScreensCheck) &&
    morph::model::detail::registerActionResultOnce<morph::ui::CustomScreensSave>(morph::ui::kCustomScreensSave) &&
    morph::model::detail::registerActionResultOnce<morph::ui::CustomScreensRemove>(morph::ui::kCustomScreensRemove);
}  // namespace
#endif
```

Notes for the implementer:
- Confirm the namespace of `registerActionResultOnce` in `include/morph/core/registry.hpp` (it sits in a `detail`
  namespace near line 1626). Match it exactly.
- The effects resolver may require every action with effects to have result slots. That is why all five actions are
  registered there, not only the reads.
- A thrown exception in an action becomes the `err` reply's message, as the preferences model relies on.

Add `src/ui/custom_screens.cpp` to `add_library(morph …)` in `CMakeLists.txt`, after `src/ui/custom_screen_checks.cpp`.

- [ ] **Step 6: Run the tests**

Run: `flock "$LOCK" cmake --build build/clang-debug --target morph_tests -j5 && flock "$LOCK" timeout 300 build/clang-debug/tests/morph_tests "[custom-screens]"`
Expected: `All tests passed`.

- [ ] **Step 7: Mutation checks.** Disable each in turn, rebuild, confirm the case fails, then restore and `touch`.
  - Remove the `if (request.id) { … get … }` block: "one person never reaches another's" fails, because the overwrite
    is accepted.
  - Read the person from the request body instead of the session. Add a `person` member to `CustomScreensList` and use
    it when non-empty. "one person never reaches another's" fails. Revert the member too.
  - Replace `throw std::runtime_error{stored.error().reason}` with `co_return CustomScreenSaved{.id = id}`: "a store's
    refusal reaches the person" fails.
  - Delete the two `BRIDGE_REGISTER_EFFECTS` lines: "lists hold the record…" fails.

- [ ] **Step 8: Prove the process-level registries still resolve for everything else.** The new record and effects
  join every other registration in `morph_tests`. Run the suites that read the registries.

Run: `flock "$LOCK" timeout 300 build/clang-debug/tests/morph_tests "[effects],[records],[push],[ui-service],[ui]"`
Expected: `All tests passed`. If a test asserts an exact list of registered records or actions, it now sees
`morph.customScreens`. Change that test to look for its own entries, not an exact list, and say so in the task report.

- [ ] **Step 9: Commit**

```bash
git add include/morph/ui/custom_screens.hpp src/ui/custom_screens.cpp CMakeLists.txt \
        tests/ui_custom_screens_support.hpp tests/test_ui_custom_screens.cpp
git commit -m "ui: the custom screens model, served under the verified person"
```

---

### Task 7: Spec pages

**Files:**
- Create: `docs/spec/ui/custom_screens.md`
- Modify: `docs/spec/ui/service.md`, `docs/spec/README.md`

- [ ] **Step 1: Write `docs/spec/ui/custom_screens.md`**

````markdown
# Custom screens — design

A person's own version of an application screen, stored on the server through the application's store, checked
whenever it is saved or loaded, and served by the framework's `morph.customScreens` model. The design is in
`docs/superpowers/specs/2026-11-18-custom-screens-design.md`.

| Header | What it holds |
|---|---|
| `include/morph/ui/custom_screens.hpp` | The store seam and memory store, `checkBaseRule`, `checkCustomScreen`, `withScreenId`, `VerdictCache`, the wire types, `CustomScreensModel`, `registerCustomScreens` |
| `include/morph/ui/service.hpp` | `permittedActions`, `screenAdmitted`, `kCustomScreenIdPrefix`, `ServedBundle::catalog` |

## Contents

- [The store](#the-store)
- [The model](#the-model)
- [The checks](#the-checks)
- [The base rule](#the-base-rule)
- [Records and effects](#records-and-effects)
- [Design decisions](#design-decisions)

## The store

`ICustomScreenStore` has `list(person, app)`, `get(key)`, `put(key, screen)` and `remove(key)`, all asynchronous.
`get`, `put` and `remove` answer a `CustomScreenRefusal` the person is shown as it is. The application's store is where
every policy over custom screens lives: what a person may open (which may include screens others saved), what they may
overwrite or delete, and how many they may keep. The model calls it only with a person the server derived and a
document that passed every check.

`MemoryCustomScreenStore(base)` is the framework's store: a map on an `OwnerStrand`, each person seeing only their own
screens. A missing screen is `no such custom screen`.

## The model

`registerCustomScreens(store, bundle, authorizer, dispatcher, registry, options)` registers `morph.customScreens`.
`bundle` is called at each action, so a development reload is seen. `authorizer` is the server's.

| Action id | Body | `ok` reply |
|---|---|---|
| `morph.customScreens.list` | `{app}` | `{screens: [CustomScreenSummary]}` |
| `morph.customScreens.get` | `{app, id}` | `{summary, document}` |
| `morph.customScreens.check` | `{app, basedOn, document}` | `{refusal?: {pointer, reason}}` |
| `morph.customScreens.save` | `{app, id?, title, basedOn, document}` | `{id}` |
| `morph.customScreens.remove` | `{app, id}` | `{removed}` |

- **The person** is the verified principal, copied at the start of each action, before anything is awaited. A body
  naming a person has the key ignored. With no verified principal, `list` answers nothing and the rest are refused
  with `custom screens are stored for a signed-in person`.
- **`save` with an `id`** overwrites only a screen `get` answers for the caller; otherwise `no such custom screen`.
  With no `id`, the id is `custom:` and 32 hexadecimal digits from `newFileRef`.
- **Before it is stored**, a document passes `checkCustomScreen`, and `withScreenId` writes the custom screen's id into
  it. A refusal is the `err` reply's message, with its pointer; `check` answers it structured.
- **A summary** carries `mine` (the caller saved it), `opensWithoutParams`, and `broken`: the refusal the checks give
  the stored document today. A broken screen is listed, never mounted.
- The actions are `Loggable::No`: a screen is not application data.

## The checks

`checkCustomScreen(bundle, session, authorizer, basedOn, document, options, cache)` answers the first refusal:

1. **Bounds.** At most `maxDocumentBytes` (256 KiB by default, at most `kMaxFileBytes`, which
   `registerCustomScreens` enforces so every stored screen can be served), nested within `kMaxJsonDepth`.
2. **The base.** A screen document of the bundle that `screenAdmitted` admits the person to.
3. **The application screen checks.** `loadDocumentText` with `ServedBundle::catalog`, and a screen, not the app
   document.
4. **The accessibility rules.** `author::checkAccessibility`.
5. **The base rule.** `checkBaseRule`.
6. **Permission.** Every action the document names is in `permittedActions`.

Checks 2 to 6 are kept in a `VerdictCache` (bounded by `verdictCacheSize`, least recently used dropped), keyed by
`verdictKey`: the document's hash, the base's id, the bundle's digest and a hash of the permitted set. A reload, a
permission change or an edit is a new key.

The checks bound what a document contains. What it does is still bounded by the server's checks on each call it makes.

## The base rule

`checkBaseRule(base, baseText, custom, customText)` refuses the first use in the custom document that the base does
not have:

| Compared | How |
|---|---|
| Params | Equal to the base's, names and types |
| Actions | Each `(model type, action)` a query, mutation or model-bound node names |
| Commands run without a press | `<trigger>: <command>`, where the trigger is a lifecycle key or `watch`, and a mutation's `onSuccess`/`onError` reached from such a run inherits its trigger |
| Client-side commands | `upload`, `download`, `switchBackend <name>`, `signIn`, `signOut`, `resetPrefs`, wherever they are |
| Host names | Every string member named `host`: host commands, host values and host components share one set |
| Delivered components | Name and content hash |

Not compared: text, layout, styles, states, lets, expressions other than host values, `navigate` targets (opening a
screen goes through the catalog's admission), and ids.

## Records and effects

`CustomScreenSummary` is the record `morph.customScreens`, keyed by `id`. `save` and `remove` declare
`changes<CustomScreenSummary>`. `list` and `get` hold it, so a client's list refetches after any save. These are
registered at static initialisation in `src/ui/custom_screens.cpp`, beside `registerCustomScreens`, because the
process-level registries resolve once, at their first read. A static build links them only into a server that calls
`registerCustomScreens`; a shared build carries them for every application, where they are inert.

## Design decisions

| Decision | Chosen | Why |
|---|---|---|
| Where policy lives | The application's store | Sharing, roles and limits differ per application; what a document may contain does not. |
| What a shared screen may do | No more than its base | A viewer opening someone else's screen can then do only what opening the base would do. |
| Action ids | `morph.customScreens.<action>` | Action ids are process-global; a bare `list` would clash with an application's. |
| Effects | `changes`, record-wide | A list refetches after any save; a save is a person's deliberate act, and the verdict cache keeps the refetch cheap. |
| The session | Copied before the first await | The session is the dispatching thread's; an action resumes elsewhere. |
| Servability | A bound on `maxDocumentBytes` | Any document within it fits a file and an envelope, so no per-document check is needed. |
````

- [ ] **Step 2: Update `docs/spec/ui/service.md`.** Under "## The catalog", append a paragraph:

```markdown
**The permitted set and admission as functions.** `permittedActions(bundle, session, authorizer)` is the `permitted`
list, from `allActions`, and `screenAdmitted(screen, session, authorizer)` whether a screen admits the principal.
`ui-catalog` and the custom screens model ([`custom_screens.md`](custom_screens.md)) both use them, so they answer the
same set.
```

Under "## The registry", append:

```markdown
- **`custom:` is reserved.** An entry id that starts with `custom:` (`kCustomScreenIdPrefix`) is refused, by
  `addScreen` and `addEntry` alike, because that prefix names a person's custom screen.
- **The bundle keeps its catalog.** `ServedBundle::catalog` is the catalog every document was loaded with, so a
  document checked after the build is loaded against the same facts.
```

- [ ] **Step 3: Index it.** In `docs/spec/README.md`, after the `ui/preferences.md` line, add:

```markdown
[`ui/custom_screens.md`](ui/custom_screens.md) — a person's own screens: the store seam the application implements,
the checks and the base rule every custom document passes, and the model that serves them ·
```

- [ ] **Step 4: Build the documentation the way CI does**

Run:
```bash
flock "$LOCK" cmake -S . -B build/docs -G Ninja -DMORPH_BUILD_DOCUMENTATION=ON -DMORPH_BUILD_TESTS=OFF -DMORPH_BUILD_EXAMPLES=OFF
flock "$LOCK" cmake --build build/docs --target doc
```
Expected: the `doc` target finishes with no warnings. A warning fails CI (`FAIL_ON_WARNINGS`). Fix every
`@param`/`@return` it names in `custom_screens.hpp` or `service.hpp`.

- [ ] **Step 5: Commit**

```bash
git add docs/spec/ui/custom_screens.md docs/spec/ui/service.md docs/spec/README.md
git commit -m "docs: the custom screens spec page"
```

---

### Task 8: Verify against the CI configurations

**Files:** none, unless a check finds something.

- [ ] **Step 1: The whole suite**

Run: `flock "$LOCK" cmake --build build/clang-debug -j5 && flock "$LOCK" timeout 600 build/clang-debug/tests/morph_tests`
Expected: `All tests passed`. A test that fails during another lane's build is rerun once before it is reported.

- [ ] **Step 2: The shared build**, where the static registrations reach every test

Run: `flock "$LOCK" cmake --preset clang-shared-debug && flock "$LOCK" cmake --build build/clang-shared-debug --target morph_tests -j5 && flock "$LOCK" timeout 600 build/clang-shared-debug/tests/morph_tests "[effects],[records],[push],[ui]"`
Expected: `All tests passed`.

- [ ] **Step 3: GCC.** CI builds with GCC 15, which warns where clang does not (`-Wrange-loop-construct`, and
  `-Wstrict-overflow` at `-O3`).

Run: `flock "$LOCK" cmake --preset gcc-debug && flock "$LOCK" cmake --build build/gcc-debug --target morph_tests -j5`
Expected: no warnings from `custom_screen_checks.cpp`, `custom_screens.cpp`, `service.cpp` or the three test files.
Never pass `-w`. To confirm the warnings are live, add a temporary `for (std::string s : std::vector<std::string>{})`
loop in a test file, check that GCC warns, then remove it.

- [ ] **Step 4: clang-tidy** on the changed sources, three at a time, without the lock

Run: `clang-tidy -p build/clang-debug src/ui/custom_screen_checks.cpp src/ui/custom_screens.cpp src/ui/service.cpp`
then `clang-tidy -p build/clang-debug tests/test_ui_custom_screen_rules.cpp tests/test_ui_custom_screen_checks.cpp tests/test_ui_custom_screens.cpp`
Expected: no warnings. Do not accept an `auto*` or `std::expected` fix-it without writing the type out. Those pass on
libstdc++ and break MSVC.

- [ ] **Step 5: Qt keywords and MSVC traps in the added lines**

Run: `git diff master -- include/morph | grep -nE '^\+.*\b(slots|signals|emit)\b' ; git diff master -- include/morph src | grep -nE '^\+.*template\s*<.*=\s*[A-Za-z_:]+<'`
Expected: no output from the first command. The second should only list lines that already have `typename`. This plan
adds none.

- [ ] **Step 6: Report.** State, as measured: the suite result on clang static and shared, the GCC build, the
  clang-tidy result, the doc target, and which mutation checks ran and failed as expected. Name any test whose
  expectation you changed, and why.

---

## Self-Review

**Spec coverage.**

| Spec | Task |
|---|---|
| `permittedActions` as a function | 1 |
| `ServedBundle` keeps its catalog | 2 |
| Reserved `custom:` | 2 |
| The store, policy in the store, the memory store | 5 |
| The model's five actions, the person, `save` with an id, `mine`, `broken`, `Loggable::No` | 6 |
| Effects and a record | 6 |
| Checks 1–7 | 4 (1–6), 6 (7, via `withScreenId` in `save`) |
| The verdict cache | 4 |
| The base rule and params | 3 |
| The bundle getter for reloads | 6 |
| Spec page and service.md | 7 |

`remount`, drafts, the client cache, **Open as** and the assistant belong to plans 2 and 3.

**Placeholder scan.** Each place where the plan leaves a choice to the implementer (a fixture's JSON spelling, the
ready-made `Completion` constructor's name, `registerActionResultOnce`'s namespace, `parseJson`'s error type) says
what to read, what to do, and what must not change.

**Type consistency.** These names are used consistently across tasks: `CustomScreensOptions` (4, 6), `VerdictCache`
(4, 6), `checkCustomScreen` (4, 6), `withScreenId` (4, 6), `CustomScreenSummary` (6, effects),
`kCustomScreenIdPrefix` (2, 6), `screens_test::bundle()`/`Server` (4, 6).

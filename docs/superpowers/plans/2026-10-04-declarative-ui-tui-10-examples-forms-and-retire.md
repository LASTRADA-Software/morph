# Declarative UI, Part 10 — the forms demo, then retiring the QML renderer and the Qt client stack

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or
> superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for
> tracking.

**Goal:** Move `examples/forms` onto the injected frontend (`morph_forms_app`: `LabApp` through
`forms::appShellView`, runtime by default, typed with `--typed`), then delete everything the program replaced —
the shipped QML forms renderer and its Qt components, `examples/common`'s Qt client stack, and `morph_add_rung`'s
old client conventions — leaving no reference behind but history.

**Architecture:** Two groups, two commits. `forms-demo` adds a toolkit-free app library (`examples/forms/app/`,
target `forms_app`): one `forms::AppShellSession` over `LabApp`'s `app-*` document, which makes each screen's
session from the served JSON documents, all of them submitting through `forms::handlerSubmitter` over one
in-process `BridgeHandler<LabModel>`; `--typed` swaps the two form screens for `forms::Form<A>`s in a shell view
of the app's own. One binary (`examples/forms/ui/main.cpp`, target `morph_forms_app`) picks the terminal UI or Qt
Quick at run time; the QML demo goes. `retire` then removes `src/qt/forms`, `include/morph/qt/{forms,bridge}`, the
`qt_forms`/`forms_qml`/`forms_qmlplugin` components and `MORPH_BUILD_FORMS_QML`, the `examples/common` presenter
stack, and the rung conventions nothing uses any more, and rewrites the documents that described them.

**Tech Stack:** C++23; `morph::reactive`, `morph::ui`, the forms engine (`include/morph/forms/engine/`),
`morph::tui`, `morph::qt_quick`, `examples/common/app` (Part 6); CMake; Catch2 v3; GitHub Actions YAML; Markdown.

**Spec:** `docs/superpowers/specs/2026-10-04-examples-migration-design.md` (spec 4) §3, §4 (removals), §5 (forms
demo), §6, §7, §8 item 3, §9; `docs/superpowers/specs/2026-10-04-forms-engine-design.md` (spec 2) §5, §8, §11, §12.
Interface contract: `docs/superpowers/plans/2026-10-04-declarative-ui-tui-interfaces.md`, including "Conventions for
every example application".

This is **Part 10 of 11** of the declarative-UI program, and its last. Read the master plan
`docs/superpowers/plans/2026-10-04-declarative-ui-tui.md` first: it fixes the branch, the 18-commit history, the
`wip(<key>)` convention and the squash every group ends with. Parts 0–9 have landed: this part produces commits
17 (`examples/forms: …`) and 18 (`forms, examples: retire the QML renderer and the Qt client stack`), then hands
over to the master plan's "Finishing the branch".

## Global Constraints

- Everything in Part 1's "Global Constraints" applies unchanged (SPDX first line, `#pragma once`, naming,
  identifier length, no history or issue numbers in comments and docs, full Doxygen under `include/morph`,
  `-Weverything -Werror`, clang-tidy clean, the sign-off trailer).
- clang-tidy's `readability-identifier-length` holds in every code block, tests included: no parameter, lambda
  parameter or local shorter than three characters except `i j k x y n N fn cb op` (parameters) and
  `i j k x y lk cb op fn` (variables); data members are exempt. Headers under `include/**` and `examples/forms/app/**`
  use `.at()` or iteration, never an unchecked `operator[]` on a container.
- `examples/forms/app/` is toolkit-free: target `forms_app` links `morph::morph` and `morph_ladder_app_common`
  (Part 6, Qt-free by construction) and nothing else, so an include of `morph/tui/…`, `morph/qt_quick/…` or a Qt
  header there fails to build (spec 4 §7 rule 1).
- Views are bindings only; every conditional and format lives in a session, a `Computed` or a plain function the
  view binds (spec 4 §7 rule 2).
- Client code lives in `lab::client` and exposes the contract's
  `[[nodiscard]] std::unique_ptr<ui::Application> lab::client::makeApplication(ui::AppContext& ctx,
  examples::AppEnvironment const& env)`. The demo's one knob, `--typed`, is not an `AppEnvironment` field, so a
  second overload takes it as a third argument and `ui/main.cpp` passes `entranceFromArgs(argc, argv)` — the one
  line by which its `main` differs from spec 4 §3's shape (stated in the `forms-demo` squash body).
- `examples/forms` is not a ladder rung: it is configured from the root `CMakeLists.txt`'s "Demo executable"
  section, before the Tests section and before the "Example applications on an injected frontend" block that adds
  `examples/common/app` (Part 6). It therefore names `morph_test_main`, `morph_ladder_app_common` and
  `morph_example_testkit` by their plain target names, which CMake resolves at generate time, as `examples/bank`
  does; Part 6's root block adds `examples/common/app` whenever `examples/forms` exists (every native configure
  with examples on). The app's own tests wait the way the framework's suite does —
  `morph::testing::waitUntil` over a `StepExecutor` owner (`tests/test_support.hpp`), never a sleep — and the
  application test uses Part 6's `FakeAppContext` and `pumpUntil`.
- Smoke tests run in a binary of their own whose `main` is `morph_test_main` (`forms_app_smoke_tests`), because
  the Qt Quick frontend constructs its own application object (Part 6, "The frontend smoke harness").
- `ui::testing::RecordingBackend` is read exactly as Part 2 writes it: kinds `Text`, `Button`, `TextInput`,
  `Select`, `Menu`, `Table`, `Dialog`, `Busy`, a stack as `Column`/`Row` by its axis (so a `ui::forEach` mounts as a
  `Column`), a `Switch` as `Slot`; a prop is its setter's name without `set`, first letter lower-cased (`text`,
  `label`, `items`, `enabled`, `open`, `title`), a bool `"true"`/`"false"`, a `Menu`'s `items` `"[A,B]"`; the mount
  skips a `Common` setter whose value is the constant default and always calls it for a binding; a `Dialog`'s content
  exists only while it is open; `chooseIndex` on a `Menu` runs that item's action; `find` returns the lowest live id.
- The Part 5 names this part consumes beyond the interface contract are listed once, in Task 2's Interfaces block,
  exactly as Part 5's plan declares them; Task 2 Step 0 confirms them against the headers.
- `examples/forms` gets no `CHANGELOG.md` line (CONTRIBUTING: example-only). The `retire` commit gets one
  "Removed" entry naming every replacement.
- A defect found in `include/morph` or `src` outside this part's scope is filed per `AGENTS.md`; one found in the
  forms engine *by this part's tests* is fixed in the forms-engine headers in the same group, with an engine test
  beside it, and named in that group's squash body.
- Commits end with `Signed-off-by: Christian Parpart <christian@parpart.family>`.

## Review Focus

1. **The wizard's Next is enabled before step one's submission succeeded** — the step gate is the reply, not the
   edit (Task 3 test "Intake wizard: Next waits for RegisterSample's reply").
2. **A Bind prefill into a `Choice` whose options were fetched before the referenced row existed** — the
   `WizardSession` builds every step's `FormSession` up front, so step two's `sampleId` options (`ListSamples`) are
   answered before `RegisterSample` creates sample 100. Part 5's `clearStaleSelection` clears a selection that is
   not among options *answered for the current parents*; the independent query's key never changes, so without a
   re-fetch on step entry the prefill is cleared at once (Part 5 Task 14b engine test "entering a step refetches its Choice
   options…", Task 3 test "Intake wizard: the prefilled sample id survives step two's options loading").
3. **The typed and runtime entrances disagree on the wire** — same input, different body bytes (Task 5 test
   "both entrances build the same ComputeDryDensity body").
4. **A confirm-guarded collection action that runs without confirmation** — dismissing the dialog must issue
   nothing (Task 4 test "Samples: Delete asks first, and dismissing deletes nothing").
5. **Leaving a screen and coming back resets it** — the app shell remounts the screen's view, but its session
   (the wizard's step, a form's drafts) must outlive the view (Task 3 test "Intake wizard: the step survives
   leaving the screen and coming back").

---

## File Structure

| File | Responsibility |
|---|---|
| `examples/forms/lab_views.hpp` (new) | `SamplesView` and its `BRIDGE_REGISTER_VIEW`, split out of `lab_schemas.hpp` so `LabApp` can name it |
| `examples/forms/lab_wizard.hpp` | `IntakeWizard`; `LabApp` gains the `Samples` menu entry and `ViewScreen<"samples", SamplesView>` |
| `examples/forms/lab_schemas.hpp` | The served documents (`schemasJson`, `wizardSchemasJson`, `appSchemaJson`, `viewsJson`) |
| `examples/forms/app/lab_documents.{hpp,cpp}` | `LabDocuments` (the four JSON documents as text), `memberJson`, `lookupIn`, `sourcesOf` (the shell's `AppShellSources`) |
| `examples/forms/app/lab_link.hpp` | `LabLink`: the one `BridgeHandler<LabModel>`, and `forms::handlerSubmitter`/`handlerChoiceFetcher` over it |
| `examples/forms/app/typed_lab_forms.{hpp,cpp}` | `TypedLabForms`: the two `forms::Form<A>` screens and their typed reply lines |
| `examples/forms/app/lab_shell.{hpp,cpp}` | `Entrance`, `entranceFromArgs`, `kLabAppId`, `parseShell`, `LabShell` (the `AppShellSession`; the typed entrance's shell view) |
| `examples/forms/app/lab_application.{hpp,cpp}` | `LabApplication` (`ui::Application`) and both `makeApplication` overloads |
| `examples/forms/ui/main.cpp` (new) | The composition root: environment, frontend selection, the factory |
| `examples/forms/tests/*` (new) | `lab_test_support.hpp`, `.clang-tidy`, schema, shell, wizard, collection, typed and application tests; `smoke/test_lab_frontend_smoke.cpp` |
| `examples/forms/CMakeLists.txt` | `forms_app`, `morph_forms_app`, `forms_app_tests`, `forms_app_smoke_tests`; the CLI targets unchanged |
| `examples/forms/gui_qml/` | Deleted |
| `CMakeLists.txt` (root) | the QML-renderer comments |
| `examples/forms/README.md`, `README.md`, `docs/ARCHITECTURE.md` | The demo's description |
| `src/qt/forms/`, `include/morph/qt/forms/`, `include/morph/qt/bridge/` | Deleted |
| `CMakeLists.txt`, `cmake/morphConfig.cmake.in`, `CMakePresets.json` | Option, targets, install components, header guard |
| `scripts/check_forms_qml_install.sh` | Deleted |
| `scripts/test_check_install_export.sh`, `scripts/check_coverage_objects.sh`, `scripts/coverage.sh` | References to removed targets |
| `.github/workflows/ci.yml`, `nightly-slow-checks.yml`, `wasm-ladder.yml` | `MORPH_BUILD_FORMS_QML`, the forms-QML install job, the AUTOMOC step, the `gui_wasm` fallback |
| `examples/common/gui/`, `examples/common/testkit/qml_surface.hpp`, `testkit_src/qml_surface.cpp`, five self-tests | Deleted |
| `examples/common/CMakeLists.txt`, `testkit/testkit_main.cpp`, `testkit/{pump,client_pool,backend_rig,journey}.hpp` | The remaining testkit, Qt-free wording |
| `cmake/morph_add_rung.cmake` | Old conventions and the `src/headless` block removed; a refusal for a leftover old client directory |
| `examples/TESTING.md`, `examples/IMPLEMENTATION.md`, `examples/LADDER.md` | The toolkit-free app on an injected frontend |
| `docs/spec/forms/*.md`, `docs/GETTING-STARTED.md`, `docs/todo.md`, `docs/spec/offline/offline.md`, `CONTRIBUTING.md`, comments in `include/`, `tests/`, `cmake/`, `examples/*/.clang-tidy`, `codecov.yml` | Every remaining reference |
| `CHANGELOG.md` | `[Unreleased]` → Removed |

## Build and test commands

```bash
# forms-demo group: both frontends (spec 4 §6), examples on (the default)
cmake -S . -B build/forms -G Ninja -DCMAKE_BUILD_TYPE=Debug \
      -DMORPH_BUILD_TUI=ON -DMORPH_BUILD_QT=ON -DMORPH_BUILD_QT_QUICK=ON
cmake --build build/forms --target morph_tests forms_app_tests forms_app_smoke_tests morph_forms_app morph_forms_demo
./build/forms/tests/morph_tests "[forms-engine][wizard]"
ctest --test-dir build/forms -L forms-demo --output-on-failure
ctest --test-dir build/forms -R 'forms_html_math|forms_repl_roundtrip' --output-on-failure

# the same tree with no frontend: the library and its tests must still build (spec 4 §6)
cmake -S . -B build/forms-nofe -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build/forms-nofe --target forms_app_tests && ctest --test-dir build/forms-nofe -L forms-demo

# retire group: the master plan's build/all, now without MORPH_BUILD_FORMS_QML
cmake -S . -B build/all -G Ninja -DCMAKE_BUILD_TYPE=Debug -DMORPH_BUILD_TUI=ON -DMORPH_BUILD_QT=ON \
      -DMORPH_BUILD_QT_QUICK=ON -DMORPH_BUILD_LADDER=ON -DMORPH_LADDER_RUNGS=all \
      -DMORPH_BUILD_BANK_EXAMPLE=ON -DMORPH_BUILD_NET=ON
cmake --build build/all && QT_QPA_PLATFORM=offscreen ctest --test-dir build/all --output-on-failure
```

Every configure prints `morph: warnings: ... strict=ON`; if it does not, stop and say so (CONTRIBUTING).

---

## Group `forms-demo` — `examples/forms: one app, any frontend, runtime and typed forms`

### Task 1: `LabApp` routes to the samples view

**Files:**
- Create: `examples/forms/lab_views.hpp`
- Modify: `examples/forms/lab_schemas.hpp` — remove `SamplesView`, its `using` and its `BRIDGE_REGISTER_VIEW`;
  include `lab_views.hpp`; the `@file` comment
- Modify: `examples/forms/lab_wizard.hpp` — include `lab_views.hpp`; `LabApp`; the `@file` comment
- Modify: `examples/forms/CMakeLists.txt` — add the `forms_app_tests` block after the CLI's demo-level tests
- Create: `examples/forms/tests/.clang-tidy` (a copy of `examples/bank/tests/.clang-tidy`)
- Test: `examples/forms/tests/test_lab_app_schema.cpp`

**Interfaces:**
- Consumes: `morph::app::ViewScreen<Id, View>` (Part 5, `include/morph/forms/app.hpp`, Task 15: `id()`, `kind()` = `"view"`, `ref()` =
  `views::ViewTraits<View>::typeId()`); `morph::app::appSchemaJson<AppT>()` (`include/morph/forms/app.hpp:123`);
  `morph::views::describeAction`, `BindEntry`, `ActionScope`, `BRIDGE_REGISTER_VIEW` (`include/morph/forms/views.hpp`).
- Produces: `lab::SamplesView` in `lab_views.hpp`; `LabApp` with menu `Density, Measure, Intake, Samples` and
  screens `density` (form), `measure` (form), `intake` (wizard), `samples` (view → `SamplesView`).

- [ ] **Step 1: Write the failing test**

Create `examples/forms/tests/test_lab_app_schema.cpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0

#include <catch2/catch_test_macros.hpp>
#include <string>

#include "lab_schemas.hpp"

TEST_CASE("LabApp: Samples is a menu entry routed to a view screen", "[forms-demo]") {
    std::string const app = lab::appSchemaJson();
    CHECK(app.contains(R"({"label":"Samples","screen":"samples"})"));
    CHECK(app.contains(R"("samples":{"kind":"view","ref":"SamplesView"})"));
}

TEST_CASE("LabApp: the menu keeps its order", "[forms-demo]") {
    std::string const app = lab::appSchemaJson();
    auto const density = app.find(R"("screen":"density")");
    auto const measure = app.find(R"("screen":"measure")");
    auto const intake = app.find(R"("screen":"intake")");
    auto const samples = app.find(R"("screen":"samples")");
    REQUIRE(samples != std::string::npos);
    CHECK(density < measure);
    CHECK(measure < intake);
    CHECK(intake < samples);
}

TEST_CASE("lab::viewsJson: SamplesView keeps its query and its actions", "[forms-demo]") {
    std::string const views = lab::viewsJson();
    CHECK(views.contains(R"("v-query":"ListSamples")"));
    CHECK(views.contains(R"("action":"EditSample")"));
    CHECK(views.contains(R"("action":"DeleteSample")"));
    CHECK(views.contains(R"("action":"CreateSample")"));
}
```

Copy the clang-tidy configuration the bank suite uses (it subtracts only `bugprone-chained-comparison`, which
Catch2's `REQUIRE(a == b)` expansion trips):

```bash
cp examples/bank/tests/.clang-tidy examples/forms/tests/.clang-tidy
```

In `examples/forms/CMakeLists.txt`, after the closing `endif()` of the existing `if(MORPH_BUILD_TESTS)` block that
registers `forms_html_math` and `forms_repl_roundtrip`, add:

```cmake
# The app's own tests: headless, on ui::testing::RecordingBackend, over an in-process LabModel. Registered like
# examples/bank's suite: this directory is configured before the root CMakeLists.txt's "Tests" section, so
# morph_test_main is named by its plain target name, which CMake resolves at generate time.
if(MORPH_BUILD_TESTS)
    find_package(Catch2 3 CONFIG QUIET)
    if(NOT Catch2_FOUND)
        message(WARNING "Catch2 not found; forms_app_tests will not be built.")
    else()
        add_executable(forms_app_tests
            tests/test_lab_app_schema.cpp
        )
        target_include_directories(forms_app_tests PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}
            ${CMAKE_CURRENT_SOURCE_DIR}/tests
            ${PROJECT_SOURCE_DIR}/tests)
        target_link_libraries(forms_app_tests PRIVATE morph::morph morph_test_main)
        target_compile_features(forms_app_tests PRIVATE cxx_std_23)
        apply_warnings(forms_app_tests)
        apply_bigobj(forms_app_tests)
        if(DEFINED AF_SANITIZER)
            apply_sanitizers(forms_app_tests ${AF_SANITIZER})
        endif()
        if(AF_COVERAGE)
            apply_coverage(forms_app_tests TEST)
        endif()
        list(APPEND CMAKE_MODULE_PATH ${Catch2_DIR})
        include(Catch)
        catch_discover_tests(forms_app_tests
            DISCOVERY_MODE PRE_TEST
            PROPERTIES LABELS forms-demo)
    endif()
endif()
```

- [ ] **Step 2: Run it to verify it fails**

```bash
cmake -S . -B build/forms -G Ninja -DCMAKE_BUILD_TYPE=Debug \
      -DMORPH_BUILD_TUI=ON -DMORPH_BUILD_QT=ON -DMORPH_BUILD_QT_QUICK=ON
cmake --build build/forms --target forms_app_tests
./build/forms/examples/forms/forms_app_tests "[forms-demo]"
```

Expected: FAIL — "LabApp: Samples is a menu entry routed to a view screen" (both `CHECK`s) and "LabApp: the menu
keeps its order" (`REQUIRE( samples != std::string::npos )`); the `viewsJson` case passes.

- [ ] **Step 3: Implement**

Create `examples/forms/lab_views.hpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// The demo's view descriptors (docs/spec/forms/views.md). `SamplesView` lists `ListSamples`' rows, opens
/// `EditSample` for a row, deletes a row behind a confirmation, and creates a new sample. Split from
/// lab_schemas.hpp so `LabApp` (lab_wizard.hpp) can route a menu entry to it.

#include <array>
#include <morph/forms/views.hpp>
#include <string_view>

#include "lab_model.hpp"

namespace lab {

/// @brief The samples list/master-detail screen.
///
/// `kEditBind` binds only the row key, as views.md's `v-rowAction` example does: the editor opens with `name`
/// blank, so `EditSample` — which submits automatically once ready — runs only after the user types a name.
struct SamplesView {
    using kind = morph::views::CollectionView;
    using query = ListSamples;

    static constexpr std::string_view title = "Samples";

    static constexpr std::array<morph::views::BindEntry, 1> kEditBind{
        morph::views::BindEntry{.actionField = "id", .rowField = "id"},
    };
    static constexpr auto rowAction =
        morph::views::describeAction<EditSample>({}, morph::views::ActionScope::Row, kEditBind);

    static constexpr std::array<morph::views::BindEntry, 1> kDeleteBind{
        morph::views::BindEntry{.actionField = "id", .rowField = "id"},
    };
    static constexpr std::array<morph::views::ActionDescriptor, 2> actions{
        morph::views::describeAction<DeleteSample>("Delete", morph::views::ActionScope::Row, kDeleteBind, true),
        morph::views::describeAction<CreateSample>("New", morph::views::ActionScope::Collection),
    };
};

}  // namespace lab

using lab::SamplesView;

BRIDGE_REGISTER_VIEW(SamplesView, "SamplesView")
```

In `examples/forms/lab_schemas.hpp`: delete the `SamplesView` struct with its doc comment, the
`using lab::SamplesView;` line and the `BRIDGE_REGISTER_VIEW(SamplesView, "SamplesView")` line; add
`#include "lab_views.hpp"` after `#include "lab_model.hpp"`; drop `#include <array>` (no longer used there); and
replace the `@file` comment with:

```cpp
/// @file
/// The documents every forms client is served, from one source: `{actionType: schema}` for the standalone
/// action forms, `{wizardId: schema}`, the `app-*` document and `{viewId: schema}`. The console demo prints
/// and embeds the first; `morph_forms_app` reads all four as JSON text, the way a remote client would read them
/// from a describe endpoint.
```

In `examples/forms/lab_wizard.hpp`: add `#include "lab_views.hpp"` after `#include "lab_model.hpp"`; replace
the `@file` comment's last sentence ("`LabApp` is the app-shell descriptor the QML reference renderer's
AppShell.qml loads as its navigation root.") with "`LabApp` is the app-shell descriptor `morph_forms_app`
renders through `forms::appShellView`."; and replace `LabApp` and its doc comment with:

```cpp
/// @brief The demo's app shell: a density calculator, a standalone measurement form, the intake wizard and the
///        samples collection.
using LabApp = morph::app::App<
    "Lab console",
    std::tuple<morph::app::MenuEntry<"Density", "density">, morph::app::MenuEntry<"Measure", "measure">,
               morph::app::MenuEntry<"Intake", "intake">, morph::app::MenuEntry<"Samples", "samples">>,
    std::tuple<morph::app::FormScreen<"density", ComputeDryDensity>,
               morph::app::FormScreen<"measure", RecordMeasurement>,
               morph::app::WizardScreen<"intake", IntakeWizard>, morph::app::ViewScreen<"samples", SamplesView>>>;
```

- [ ] **Step 4: Run the tests to verify they pass**

```bash
cmake --build build/forms --target forms_app_tests morph_forms_demo
./build/forms/examples/forms/forms_app_tests "[forms-demo]"
ctest --test-dir build/forms -R 'forms_html_math|forms_repl_roundtrip' --output-on-failure
```

Expected: 3 test cases pass; the two CLI tests pass unchanged (they read `schemasJson()`, which did not change).

Mutation check: in `LabApp`, delete `morph::app::ViewScreen<"samples", SamplesView>` from the screens tuple
(keep the menu entry). Rebuild: "LabApp: Samples is a menu entry routed to a view screen" fails on its second
`CHECK`. Restore.

- [ ] **Step 5: Commit**

```bash
git add examples/forms/lab_views.hpp examples/forms/lab_schemas.hpp examples/forms/lab_wizard.hpp \
        examples/forms/CMakeLists.txt examples/forms/tests/.clang-tidy examples/forms/tests/test_lab_app_schema.cpp
git commit -m "wip(forms-demo): route LabApp's Samples entry to SamplesView

Signed-off-by: Christian Parpart <christian@parpart.family>"
```

---
### Task 2: The runtime shell — form screens from the served documents

**Files:**
- Create: `examples/forms/app/lab_documents.hpp`, `examples/forms/app/lab_documents.cpp`
- Create: `examples/forms/app/lab_link.hpp`
- Create: `examples/forms/app/lab_shell.hpp`, `examples/forms/app/lab_shell.cpp`
- Create: `examples/forms/tests/lab_test_support.hpp`
- Modify: `examples/forms/CMakeLists.txt` — the `forms_app` library before the tests block; `forms_app_tests`
  sources and link
- Test: `examples/forms/tests/test_lab_shell_view.cpp`

**Interfaces:**
- Consumes (contract): `reactive::Runtime` (Part 1); `ui::Node`, `ui::Mounted`, `ui::testing::RecordingBackend`
  (Part 2); `forms::FormSession` (`field`, `body`, `lastReply`, `lastError`), `forms::Submitter`,
  `forms::ChoiceFetcher`, `forms::SchemaError`, `forms::appShellView`, `forms::handlerSubmitter`,
  `forms::handlerChoiceFetcher` (Part 5); `bridge::Bridge`, `bridge::BridgeHandler<M>(Bridge&, IExecutor*)`
  (`include/morph/core/bridge.hpp`); `morph::testing::StepExecutor`, `waitUntil`, `WaitBudget`
  (`tests/test_support.hpp`).
- Consumes (Part 5, beyond the contract, as Part 5's plan declares them — Step 0 confirms each):
  ```cpp
  namespace morph::forms {
  // form_session.hpp (Task 6)
  class FieldState { public: [[nodiscard]] reactive::Signal<std::string>& text() noexcept; /* … */ };
  // collection.hpp (Task 13)
  using SchemaLookup = std::function<std::optional<std::string>(std::string_view actionType)>;
  // app_shell.hpp (Task 15)
  enum class ScreenKind : std::uint8_t { Form, Wizard, View };
  struct AppMenuEntry { std::string label{}; std::string screen{}; };
  struct AppScreen { std::string id{}; ScreenKind kind = ScreenKind::Form; std::string ref{}; };
  class AppShellModel { public:
      [[nodiscard]] static std::expected<AppShellModel, SchemaError> fromSchema(std::string_view appId,
                                                                                std::string_view appJson);
      id(), title(), menu() -> std::span<AppMenuEntry const>, screens() -> std::span<AppScreen const>,
      find(std::string_view) -> AppScreen const*; };
  struct AppShellSources { SchemaLookup actions{}; SchemaLookup wizards{}; SchemaLookup views{}; };
  class AppShellSession { public:                 // starts on the first menu entry's screen
      AppShellSession(reactive::Runtime&, AppShellModel, AppShellSources, Submitter, ChoiceFetcher,
                      FormSessionOptions = {});
      model(), current() -> std::string const&, select(std::string_view), title(), menuLabel(std::size_t),
      form(std::string_view screenId) -> FormSession*, wizard(…) -> WizardSession*,
      collection(…) -> CollectionSession*; };     // each made the first time it is asked for, then kept
  }
  ```
  `AppShellSession` builds every screen's session itself, from `AppShellSources`, and `appShellView` renders the
  session it made (`formView`, `wizardView` or `collectionView`), or the error text
  `this screen's document is not available: <id>` when the source has no document for it. The runtime entrance is
  therefore nothing but the right sources and submitter; the typed entrance (Task 5) is a shell view of the app's
  own over the same session, so Part 5 needs no renderer hook.
- Produces (namespace `lab::client`): `LabDocuments{app, schemas, wizards, views}`, `labDocuments()`,
  `memberJson(objectJson, key)`, `lookupIn(objectJson) -> forms::SchemaLookup`,
  `sourcesOf(LabDocuments const&) -> forms::AppShellSources` (actions only here; Tasks 3–4 add the rest);
  `LabLink(bridge, callbacks)` with `handler()`, `submitter()`, `choices()`; `kLabAppId`, `parseShell(appJson)`,
  `LabShell(runtime, bridge, callbacks)` with `session()`, `view()`; test support `lab::testing::LabRig`, `menuOf`,
  `shows`, `withProp`, `the`, `kDensity…kSamples`.

- [ ] **Step 0: Confirm the Part 5 names against the headers**

```bash
grep -n 'enum class ScreenKind\|struct AppScreen\|struct AppShellSources\|class AppShellModel\|class AppShellSession\|AppShellSession(reactive\|fromSchema(std::string_view appId\|FormSession\* form(\|WizardSession\* wizard(\|CollectionSession\* collection(' \
     include/morph/forms/engine/app_shell.hpp
grep -n 'using SchemaLookup' include/morph/forms/engine/collection.hpp
grep -n 'Signal<std::string>& text()' include/morph/forms/engine/form_session.hpp
grep -n 'Submitter handlerSubmitter\|ChoiceFetcher handlerChoiceFetcher' include/morph/forms/engine/handler_submitter.hpp
```

Expected: every name above, with the shape the Interfaces block gives. This is a confirmation: Part 5's plan is
the source the block was written from. A difference means Part 5 deviated in its implementation — follow the
header, and list the difference in Task 8's squash body.

- [ ] **Step 1: Write the failing test**

Create `examples/forms/tests/lab_test_support.hpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// The forms demo's test rig: a reactive runtime on a hand-stepped owner, and an in-process LabModel behind a
/// real worker pool. Replies reach the owner only when a test drives it, so "before the reply" is a state a test
/// can stand in rather than race.

#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstddef>
#include <memory>
#include <morph/core/backend.hpp>
#include <morph/core/bridge.hpp>
#include <morph/core/executor.hpp>
#include <morph/reactive/runtime.hpp>
#include <morph/ui/testing/recording_backend.hpp>
#include <string_view>
#include <vector>

#include "test_support.hpp"

namespace lab::testing {

using morph::ui::testing::RecordingBackend;

/// @brief Menu positions of `LabApp`'s screens.
inline constexpr std::size_t kDensity = 0;
/// @brief See `kDensity`.
inline constexpr std::size_t kMeasure = 1;
/// @brief See `kDensity`.
inline constexpr std::size_t kIntake = 2;
/// @brief See `kDensity`.
inline constexpr std::size_t kSamples = 3;

/// @brief Owner executor, runtime, worker pool and bridge, destroyed bridge-first so no model runs on a dead pool.
struct LabRig {
    /// @brief The runtime's and the bridge's owner; runs only when a test drives it.
    morph::testing::StepExecutor owner;
    /// @brief The reactive runtime every session in the test binds to.
    morph::reactive::Runtime runtime{owner};
    /// @brief Where LabModel's actions run.
    morph::exec::ThreadPoolExecutor pool{2};
    /// @brief The in-process bridge; replies are delivered on `owner`.
    morph::bridge::Bridge bridge{std::make_unique<morph::backend::LocalBackend>(pool), owner};

    /// @brief Runs the owner's queue until @p done holds, delivering replies as the pool produces them.
    /// @tparam Pred A callable returning `bool`.
    /// @param done The condition to wait for.
    /// @return `true` if @p done held before the default wait budget ran out.
    template <class Pred>
    [[nodiscard]] bool driveUntil(Pred done) {
        return morph::testing::waitUntil([&] {
            owner.runAll();
            return done();
        });
    }

    /// @brief Delivers replies until the owner has had nothing to run for @p quiet, so a claim made after it is
    ///        about the settled state rather than the moment before a late reply.
    /// @param quiet How long the owner's queue must stay empty.
    void settle(std::chrono::milliseconds quiet = std::chrono::milliseconds{250}) {
        while (morph::testing::waitUntil([&] { return owner.pending() > 0; }, morph::testing::WaitBudget{quiet})) {
            owner.runAll();
        }
    }
};

/// @brief Whether a `Text` widget shows exactly @p text.
/// @param backend The backend to search.
/// @param text The text to look for.
/// @return `true` if one exists.
[[nodiscard]] inline bool shows(RecordingBackend const& backend, std::string_view text) {
    return backend.find("Text", "text", text).has_value();
}

/// @brief Every widget of @p kind whose @p prop equals @p value, in creation order.
/// @param backend The backend to search.
/// @param kind The widget kind.
/// @param prop The prop name.
/// @param value The prop value.
/// @return The matching widget ids.
[[nodiscard]] inline std::vector<int> withProp(RecordingBackend const& backend, std::string_view kind,
                                               std::string_view prop, std::string_view value) {
    std::vector<int> found;
    for (int const widgetId : backend.all(kind)) {
        if (backend.prop(widgetId, prop) == value) {
            found.push_back(widgetId);
        }
    }
    return found;
}

/// @brief The first widget of @p kind whose @p prop equals @p value; fails the test when there is none.
/// @param backend The backend to search.
/// @param kind The widget kind.
/// @param prop The prop name.
/// @param value The prop value.
/// @return The widget id.
[[nodiscard]] inline int the(RecordingBackend const& backend, std::string_view kind, std::string_view prop,
                             std::string_view value) {
    auto const found = backend.find(kind, prop, value);
    REQUIRE(found.has_value());
    return *found;
}

/// @brief The app shell's one menu; fails the test when there is not exactly one.
/// @param backend The backend to search.
/// @return The menu's widget id.
[[nodiscard]] inline int menuOf(RecordingBackend const& backend) {
    auto const menus = backend.all("Menu");
    REQUIRE(menus.size() == 1);
    return menus.front();
}

}  // namespace lab::testing
```

Create `examples/forms/tests/test_lab_shell_view.cpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0

#include <catch2/catch_test_macros.hpp>
#include <morph/forms/engine/form_session.hpp>
#include <morph/ui/mount.hpp>
#include <optional>
#include <stdexcept>
#include <string>

#include "app/lab_documents.hpp"
#include "app/lab_shell.hpp"
#include "lab_schemas.hpp"
#include "lab_test_support.hpp"

using lab::testing::LabRig;
using lab::testing::menuOf;
using lab::testing::shows;
using morph::ui::Mounted;
using morph::ui::testing::RecordingBackend;

TEST_CASE("Lab shell: the menu lists the four screens and opens on the first", "[forms-demo]") {
    LabRig rig;
    lab::client::LabShell shell{rig.runtime, rig.bridge, rig.owner};
    RecordingBackend backend;
    Mounted const mounted{rig.runtime, backend, shell.view()};
    rig.owner.runAll();

    CHECK(backend.prop(menuOf(backend), "items") == "[Density,Measure,Intake,Samples]");
    CHECK(shows(backend, "Lab console"));
    CHECK(shell.session().current() == "density");
    CHECK(shows(backend, "Oven-dry mass of the specimen"));  // ComputeDryDensity.massDry's help text
}

TEST_CASE("Lab shell: choosing an entry swaps the screen and tears the old one down", "[forms-demo]") {
    LabRig rig;
    lab::client::LabShell shell{rig.runtime, rig.bridge, rig.owner};
    RecordingBackend backend;
    Mounted const mounted{rig.runtime, backend, shell.view()};
    rig.owner.runAll();

    backend.chooseIndex(menuOf(backend), lab::testing::kMeasure);
    rig.owner.runAll();
    CHECK(shell.session().current() == "measure");
    CHECK(shows(backend, "Sample under test"));  // RecordMeasurement.sampleId's help text
    CHECK_FALSE(shows(backend, "Oven-dry mass of the specimen"));
}

TEST_CASE("lab::client::parseShell: LabApp's document reads; one the engine cannot read is refused", "[forms-demo]") {
    auto const model = lab::client::parseShell(lab::appSchemaJson());
    CHECK(model.id() == "LabApp");
    CHECK(model.menu().size() == 4);
    CHECK_THROWS_AS(lab::client::parseShell("[]"), std::runtime_error);
    CHECK_THROWS_AS(lab::client::parseShell(R"({"app-screens":{"chart":{"kind":"chart","ref":"DensityChart"}}})"),
                    std::runtime_error);
}

TEST_CASE("Lab shell: the density form submits through the in-process LabModel", "[forms-demo]") {
    LabRig rig;
    lab::client::LabShell shell{rig.runtime, rig.bridge, rig.owner};
    morph::forms::FormSession* const density = shell.session().form("density");
    REQUIRE(density != nullptr);

    density->field("massDry").text().set("2650.5");
    density->field("volume").text().set("1");
    REQUIRE(rig.driveUntil([&] { return density->lastReply().has_value(); }));
    CHECK(density->lastError() == nullptr);
    CHECK(density->lastReply()->contains(R"("num":5301)"));  // 2650.5 kg/m³ is 5301/2, exactly
    CHECK(density->lastReply()->contains(R"("den":2)"));
}

TEST_CASE("lab::client::memberJson and sourcesOf: each screen's document, read from the served text", "[forms-demo]") {
    CHECK(lab::client::memberJson(R"({"A":{"x":1},"B":[2]})", "A") == std::optional<std::string>{R"({"x":1})"});
    CHECK(lab::client::memberJson(R"({"A":{"x":1}})", "C") == std::nullopt);
    CHECK(lab::client::memberJson("[1,2]", "A") == std::nullopt);
    CHECK(lab::client::memberJson("not json", "A") == std::nullopt);

    auto const sources = lab::client::sourcesOf(lab::client::labDocuments());
    REQUIRE(sources.actions);
    CHECK(sources.actions("ComputeDryDensity").has_value());
    CHECK_FALSE(sources.actions("ListSamples").has_value());  // a query, never a form of its own
}
```

In `examples/forms/CMakeLists.txt`, change the `add_executable(forms_app_tests …)` source list to:

```cmake
        add_executable(forms_app_tests
            tests/test_lab_app_schema.cpp
            tests/test_lab_shell_view.cpp
        )
```

and its link line to `target_link_libraries(forms_app_tests PRIVATE forms_app morph_test_main)`.

- [ ] **Step 2: Run it to verify it fails**

```bash
cmake --build build/forms --target forms_app_tests
```

Expected: FAIL to configure or build — `forms_app` is not a target and the app headers do not exist
(`fatal error: 'app/lab_documents.hpp' file not found`).

- [ ] **Step 3: Implement**

Create `examples/forms/app/lab_documents.hpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// The four JSON documents a forms client is served, held as text, and the lookups the app shell reads each
/// screen's document through. The runtime entrance builds every screen from these strings and nothing else.

#include <morph/forms/engine/app_shell.hpp>
#include <optional>
#include <string>
#include <string_view>

namespace lab::client {

/// @brief The documents a forms client needs to build the demo's screens.
struct LabDocuments {
    std::string app;      ///< The `app-*` document: title, menu, screens.
    std::string schemas;  ///< `{actionType: schema}` for every standalone action form.
    std::string wizards;  ///< `{wizardId: schema}` for every wizard.
    std::string views;    ///< `{viewId: schema}` for every view.
};

/// @brief The documents `lab_schemas.hpp` emits for the lab model.
/// @return All four documents.
[[nodiscard]] LabDocuments labDocuments();

/// @brief One member of a JSON object, re-serialised as JSON text.
/// @param objectJson A JSON object.
/// @param key The member's name.
/// @return The member's JSON text; `nullopt` when @p objectJson is not a JSON object or has no such member.
[[nodiscard]] std::optional<std::string> memberJson(std::string_view objectJson, std::string_view key);

/// @brief A lookup into one `{id: document}` object, as an app-shell source reads it.
/// @param objectJson The object; the lookup keeps its own copy.
/// @return A lookup returning `memberJson(objectJson, id)`.
[[nodiscard]] morph::forms::SchemaLookup lookupIn(std::string objectJson);

/// @brief Where the app shell finds each screen's document: the served text, through `lookupIn`.
/// @param documents The served documents.
/// @return The sources.
[[nodiscard]] morph::forms::AppShellSources sourcesOf(LabDocuments const& documents);

}  // namespace lab::client
```

Create `examples/forms/app/lab_documents.cpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0

#include "app/lab_documents.hpp"

#include <glaze/glaze.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "lab_schemas.hpp"

namespace lab::client {

LabDocuments labDocuments() {
    return LabDocuments{
        .app = lab::appSchemaJson(),
        .schemas = lab::schemasJson(),
        .wizards = lab::wizardSchemasJson(),
        .views = lab::viewsJson(),
    };
}

std::optional<std::string> memberJson(std::string_view objectJson, std::string_view key) {
    // generic_u64 keeps integers beyond 2^53 exact, which a schema's exact bounds can carry.
    glz::generic_u64 dom{};
    if (glz::read_json(dom, objectJson) || !dom.is_object()) {
        return std::nullopt;
    }
    auto const& members = dom.get_object();
    auto const found = members.find(std::string{key});
    if (found == members.end()) {
        return std::nullopt;
    }
    std::string text;
    if (glz::write_json(found->second, text)) {
        return std::nullopt;
    }
    return text;
}

morph::forms::SchemaLookup lookupIn(std::string objectJson) {
    return [document = std::move(objectJson)](std::string_view key) { return memberJson(document, key); };
}

morph::forms::AppShellSources sourcesOf(LabDocuments const& documents) {
    return morph::forms::AppShellSources{
        .actions = lookupIn(documents.schemas),
        .wizards = {},
        .views = {},
    };
}

}  // namespace lab::client
```

Create `examples/forms/app/lab_link.hpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// `LabLink`: the one handler every screen talks through. Under `LocalBackend` one handler is one LabModel
/// instance, so a sample the wizard registers is the one the measurement form's sample picker lists and the
/// Samples table shows — which is why the forms submit through `forms::handlerSubmitter` over this handler rather
/// than through `forms::bridgeSubmitter`, which would keep handlers (and so model instances) of its own.

#include <morph/core/bridge.hpp>
#include <morph/core/executor.hpp>
#include <morph/forms/engine/form_session.hpp>
#include <morph/forms/engine/handler_submitter.hpp>

#include "lab_model.hpp"

namespace lab::client {

/// @brief A `BridgeHandler<LabModel>` with the forms engine's submit and options entry points over it.
class LabLink {
public:
    /// @brief Registers one LabModel instance on @p bridge.
    /// @param bridge The bridge the model lives behind.
    /// @param callbacks Where replies are delivered: the runtime's owner.
    LabLink(morph::bridge::Bridge& bridge, morph::exec::IExecutor& callbacks)
        : _callbacks{&callbacks}, _handler{bridge, &callbacks} {}

    LabLink(LabLink const&) = delete;
    LabLink& operator=(LabLink const&) = delete;
    LabLink(LabLink&&) = delete;
    LabLink& operator=(LabLink&&) = delete;
    ~LabLink() = default;

    /// @brief The handler, for typed access (`forms::Form<A, LabModel>`).
    /// @return The handler.
    [[nodiscard]] morph::bridge::BridgeHandler<LabModel>& handler() noexcept { return _handler; }

    /// @brief Every submission, through this link's handler.
    /// @return `forms::handlerSubmitter` over the handler; it must not outlive the link.
    [[nodiscard]] morph::forms::Submitter submitter() { return morph::forms::handlerSubmitter(*_callbacks, _handler); }

    /// @brief Every `Choice`'s options, through this link's handler.
    /// @return `forms::handlerChoiceFetcher` over the handler; it must not outlive the link.
    [[nodiscard]] morph::forms::ChoiceFetcher choices() {
        return morph::forms::handlerChoiceFetcher(*_callbacks, _handler);
    }

private:
    morph::exec::IExecutor* _callbacks;
    morph::bridge::BridgeHandler<LabModel> _handler;
};

}  // namespace lab::client
```

Create `examples/forms/app/lab_shell.hpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// `LabShell`: the demo's controller — the link to LabModel and the app-shell session that navigates between the
/// screens and keeps each screen's session. Toolkit-free: it is mounted on a frontend by `LabApplication` and on
/// `ui::testing::RecordingBackend` by the tests.

#include <morph/core/bridge.hpp>
#include <morph/core/executor.hpp>
#include <morph/forms/engine/app_shell.hpp>
#include <morph/reactive/runtime.hpp>
#include <morph/ui/view.hpp>
#include <string_view>

#include "app/lab_documents.hpp"
#include "app/lab_link.hpp"

namespace lab::client {

/// @brief `LabApp`'s registered id (`BRIDGE_REGISTER_APP` in lab_wizard.hpp): the stem of its translation keys.
inline constexpr std::string_view kLabAppId = "LabApp";

/// @brief Parses the `app-*` document.
/// @param appJson The document.
/// @return The model, with id `kLabAppId`.
/// @throws std::runtime_error when the document does not parse.
[[nodiscard]] morph::forms::AppShellModel parseShell(std::string_view appJson);

/// @brief The demo's controller: one LabModel link, and the shell session over the served documents.
class LabShell {
public:
    /// @brief Opens the shell on the first menu entry; each screen's session is made when it is first shown.
    /// @param runtime The runtime every session binds to.
    /// @param bridge The bridge LabModel lives behind.
    /// @param callbacks Where replies are delivered: @p runtime's owner.
    LabShell(morph::reactive::Runtime& runtime, morph::bridge::Bridge& bridge, morph::exec::IExecutor& callbacks);

    LabShell(LabShell const&) = delete;
    LabShell& operator=(LabShell const&) = delete;
    LabShell(LabShell&&) = delete;
    LabShell& operator=(LabShell&&) = delete;
    ~LabShell() = default;

    /// @brief The app-shell session: the current screen, and every screen's session.
    /// @return The session.
    [[nodiscard]] morph::forms::AppShellSession& session() noexcept { return _session; }

    /// @brief The shell's view: the title, and the menu beside the current screen.
    /// @return `forms::appShellView(session())`.
    [[nodiscard]] morph::ui::Node view();

private:
    LabShell(morph::reactive::Runtime& runtime, morph::bridge::Bridge& bridge, morph::exec::IExecutor& callbacks,
             LabDocuments const& documents);

    LabLink _link;
    morph::forms::AppShellSession _session;  // after _link: its submitter routes through _link's handler
};

}  // namespace lab::client
```

Create `examples/forms/app/lab_shell.cpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0

#include "app/lab_shell.hpp"

#include <format>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace lab::client {

morph::forms::AppShellModel parseShell(std::string_view appJson) {
    auto model = morph::forms::AppShellModel::fromSchema(kLabAppId, appJson);
    if (!model) {
        throw std::runtime_error{
            std::format("app document: {} (at '{}')", model.error().message, model.error().path)};
    }
    return *std::move(model);
}

LabShell::LabShell(morph::reactive::Runtime& runtime, morph::bridge::Bridge& bridge, morph::exec::IExecutor& callbacks)
    : LabShell{runtime, bridge, callbacks, labDocuments()} {}

LabShell::LabShell(morph::reactive::Runtime& runtime, morph::bridge::Bridge& bridge, morph::exec::IExecutor& callbacks,
                   LabDocuments const& documents)
    : _link{bridge, callbacks},
      _session{runtime, parseShell(documents.app), sourcesOf(documents), _link.submitter(), _link.choices()} {}

morph::ui::Node LabShell::view() { return morph::forms::appShellView(_session); }

}  // namespace lab::client
```

In `examples/forms/CMakeLists.txt`, before the first `if(MORPH_BUILD_TESTS)` block, add:

```cmake
# ── forms_app: the demo's app library ────────────────────────────────────────
# Toolkit-free by construction: it links morph and nothing else, so a frontend or Qt include in app/ does not
# build. The binary (ui/main.cpp) and the tests link it.
add_library(forms_app STATIC
    app/lab_documents.cpp
    app/lab_shell.cpp
)
target_include_directories(forms_app PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(forms_app PUBLIC morph::morph)
target_compile_features(forms_app PUBLIC cxx_std_23)
apply_warnings(forms_app)
apply_bigobj(forms_app)
if(DEFINED AF_SANITIZER)
    apply_sanitizers(forms_app ${AF_SANITIZER})
endif()
if(AF_COVERAGE)
    apply_coverage(forms_app)
endif()
```

- [ ] **Step 4: Run the tests to verify they pass**

```bash
cmake --build build/forms --target forms_app_tests
./build/forms/examples/forms/forms_app_tests "[forms-demo]"
```

Expected: 8 test cases pass.

Mutation check: in `sourcesOf`, set `.actions = {}`. Rebuild: "Lab shell: the menu lists the four screens and opens
on the first" fails on `CHECK( shows(backend, "Oven-dry mass of the specimen") )` (the shell shows
`this screen's document is not available: density`), and "the density form submits…" fails at
`REQUIRE( density != nullptr )`. Restore.

- [ ] **Step 5: Commit**

```bash
git add examples/forms/app examples/forms/tests/lab_test_support.hpp examples/forms/tests/test_lab_shell_view.cpp \
        examples/forms/CMakeLists.txt
git commit -m "wip(forms-demo): the runtime shell, form screens from the served documents

Signed-off-by: Christian Parpart <christian@parpart.family>"
```

---

### Task 3: The intake wizard screen

**Files:**
- Modify: `examples/forms/app/lab_documents.cpp` — `sourcesOf` serves the wizard documents
- Modify: `examples/forms/CMakeLists.txt` — add `tests/test_lab_wizard.cpp` to `forms_app_tests`
- Test: `examples/forms/tests/test_lab_wizard.cpp`

**Interfaces:**
- Consumes: `forms::AppShellSession::select`, `wizard(screenId)`; `forms::WizardSession::current()`,
  `step(std::size_t) -> FormSession&`; `forms::wizardView` (rendered by `appShellView`: a heading
  `"<w-title>  (n / m)"`, the step title as muted text, the step's form in a `Switch`, `Back`/`Next` buttons whose
  `enabled` is bound to `canBack()`/`canNext()`) (Part 5, Task 14); Part 5 Task 14b's re-fetch on step entry.
- Produces: `sourcesOf(documents).wizards`.

- [ ] **Step 1: Write the failing test**

Create `examples/forms/tests/test_lab_wizard.cpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0

#include <catch2/catch_test_macros.hpp>
#include <morph/forms/engine/form_session.hpp>
#include <morph/forms/engine/wizard.hpp>
#include <morph/ui/mount.hpp>

#include "app/lab_shell.hpp"
#include "lab_test_support.hpp"

using lab::testing::LabRig;
using lab::testing::menuOf;
using lab::testing::shows;
using lab::testing::the;
using morph::ui::Mounted;
using morph::ui::testing::RecordingBackend;

namespace {

/// @brief The shell over a fresh LabModel, mounted, on the intake screen.
struct Intake {
    explicit Intake(LabRig& rig)
        : shell{rig.runtime, rig.bridge, rig.owner}, mounted{rig.runtime, backend, shell.view()} {
        shell.session().select("intake");
        rig.owner.runAll();
    }

    lab::client::LabShell shell;
    RecordingBackend backend;
    Mounted mounted;  // last: destroyed first, while the shell's sessions still live
};

}  // namespace

TEST_CASE("Intake wizard: Next waits for RegisterSample's reply", "[forms-demo]") {
    LabRig rig;
    Intake intake{rig};
    RecordingBackend& backend = intake.backend;

    CHECK(shows(backend, "New sample"));  // step one's title
    int const next = the(backend, "Button", "label", "Next");
    CHECK(backend.prop(next, "enabled") == "false");

    auto const inputs = backend.all("TextInput");
    REQUIRE(inputs.size() == 1);  // RegisterSample has one field, name
    backend.edit(inputs.front(), "Core 9");
    CHECK(backend.prop(next, "enabled") == "false");  // edited, but no reply has been delivered

    REQUIRE(rig.driveUntil([&] { return backend.prop(next, "enabled") == "true"; }));
    backend.click(next);
    rig.owner.runAll();
    CHECK(shows(backend, "First measurement"));
    CHECK_FALSE(shows(backend, "New sample"));
}

TEST_CASE("Intake wizard: the prefilled sample id survives step two's options loading", "[forms-demo]") {
    LabRig rig;
    Intake intake{rig};
    RecordingBackend& backend = intake.backend;

    backend.edit(backend.all("TextInput").front(), "Core 9");
    int const next = the(backend, "Button", "label", "Next");
    REQUIRE(rig.driveUntil([&] { return backend.prop(next, "enabled") == "true"; }));
    backend.click(next);
    rig.settle();

    // A fresh LabModel hands out 100 first; step two's options were answered before sample 100 existed.
    morph::forms::WizardSession* const wizard = intake.shell.session().wizard("intake");
    REQUIRE(wizard != nullptr);
    CHECK(wizard->current() == 1);
    CHECK(wizard->step(1).field("sampleId").text().peek() == "100");
}

TEST_CASE("Intake wizard: Back keeps step one's draft", "[forms-demo]") {
    LabRig rig;
    Intake intake{rig};
    RecordingBackend& backend = intake.backend;

    backend.edit(backend.all("TextInput").front(), "Core 9");
    int const next = the(backend, "Button", "label", "Next");
    REQUIRE(rig.driveUntil([&] { return backend.prop(next, "enabled") == "true"; }));
    backend.click(next);
    rig.owner.runAll();
    backend.click(the(backend, "Button", "label", "Back"));
    rig.owner.runAll();

    CHECK(shows(backend, "New sample"));
    CHECK(backend.find("TextInput", "text", "Core 9").has_value());
}

TEST_CASE("Intake wizard: the step survives leaving the screen and coming back", "[forms-demo]") {
    LabRig rig;
    lab::client::LabShell shell{rig.runtime, rig.bridge, rig.owner};
    RecordingBackend backend;
    Mounted const mounted{rig.runtime, backend, shell.view()};
    rig.owner.runAll();

    backend.chooseIndex(menuOf(backend), lab::testing::kIntake);
    rig.owner.runAll();
    backend.edit(backend.all("TextInput").front(), "Core 9");
    REQUIRE(rig.driveUntil([&] { return backend.prop(the(backend, "Button", "label", "Next"), "enabled") == "true"; }));
    backend.click(the(backend, "Button", "label", "Next"));
    rig.owner.runAll();
    REQUIRE(shows(backend, "First measurement"));

    backend.chooseIndex(menuOf(backend), lab::testing::kDensity);
    rig.owner.runAll();
    backend.chooseIndex(menuOf(backend), lab::testing::kIntake);
    rig.owner.runAll();
    CHECK(shows(backend, "First measurement"));
    CHECK_FALSE(shows(backend, "New sample"));
}
```

Add `tests/test_lab_wizard.cpp` to the `forms_app_tests` source list.

- [ ] **Step 2: Run it to verify it fails**

```bash
cmake --build build/forms --target forms_app_tests
./build/forms/examples/forms/forms_app_tests "Intake wizard:*"
```

Expected: FAIL — every case fails at `the(backend, "Button", "label", "Next")` (`REQUIRE( found.has_value() )`): with
no wizard source the shell shows `this screen's document is not available: intake`.

- [ ] **Step 3: Implement**

In `examples/forms/app/lab_documents.cpp`, `sourcesOf`: replace `.wizards = {},` with
`.wizards = lookupIn(documents.wizards),`.

- [ ] **Step 4: Run the tests to verify they pass**

```bash
cmake --build build/forms --target forms_app_tests
./build/forms/examples/forms/forms_app_tests "[forms-demo]"
```

Expected: 12 test cases pass.

Mutation check: delete the `session.refreshOptions();` line Part 5 Task 14b added to `WizardSession::next()`. Rebuild:
"Intake wizard: the prefilled sample id survives step two's options loading" fails with `"" == "100"` — the demo's
own test detects Review Focus 2, not only the engine's. Restore.

- [ ] **Step 5: Commit**

```bash
git add examples/forms/app/lab_documents.cpp examples/forms/tests/test_lab_wizard.cpp examples/forms/CMakeLists.txt
git commit -m "wip(forms-demo): the intake wizard screen

Signed-off-by: Christian Parpart <christian@parpart.family>"
```

---

### Task 4: The samples collection screen

**Files:**
- Modify: `examples/forms/app/lab_documents.cpp` — `sourcesOf` serves the view documents
- Modify: `examples/forms/CMakeLists.txt` — add `tests/test_lab_collection.cpp`
- Test: `examples/forms/tests/test_lab_collection.cpp`

**Interfaces:**
- Consumes: `forms::collectionView` as Part 5 Task 13 renders it, through `appShellView`: a heading with the
  view's title and a `Button` per collection action (`New`); a `Table` whose row key is the row's `v-rowKey` value
  as a string `ui::Key` and whose last cell holds `Open` and each row action's button (`Delete`); the editor as a
  `Dialog` titled with the row action's type (`EditSample`); the confirm `Dialog` titled `Confirm` (`Are you
  sure?`, `Yes`, `No`), whose `onDismiss` cancels; `RecordingBackend::activateRow`, `dismiss` (Part 2).
- Produces: `sourcesOf(documents).views`.

- [ ] **Step 1: Write the failing test**

Create `examples/forms/tests/test_lab_collection.cpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0

#include <catch2/catch_test_macros.hpp>
#include <morph/ui/mount.hpp>
#include <morph/ui/view.hpp>
#include <string>

#include "app/lab_shell.hpp"
#include "lab_test_support.hpp"

using lab::testing::LabRig;
using lab::testing::shows;
using lab::testing::the;
using lab::testing::withProp;
using morph::ui::Mounted;
using morph::ui::testing::RecordingBackend;

namespace {

/// @brief The shell over a fresh LabModel (samples 1, 2 and 7), mounted, on the samples screen.
struct Samples {
    explicit Samples(LabRig& rig)
        : shell{rig.runtime, rig.bridge, rig.owner}, mounted{rig.runtime, backend, shell.view()} {
        shell.session().select("samples");
    }

    lab::client::LabShell shell;
    RecordingBackend backend;
    Mounted mounted;  // last: destroyed first, while the shell's sessions still live
};

}  // namespace

TEST_CASE("Samples: the table lists the seeded samples", "[forms-demo]") {
    LabRig rig;
    Samples samples{rig};
    RecordingBackend& backend = samples.backend;

    REQUIRE(rig.driveUntil([&] { return shows(backend, "Crushed aggregate 0/32"); }));
    CHECK(backend.all("Table").size() == 1);
    CHECK(shows(backend, "Proctor A"));
    CHECK(shows(backend, "Proctor B"));
}

TEST_CASE("Samples: New creates a sample and the list refetches", "[forms-demo]") {
    LabRig rig;
    Samples samples{rig};
    RecordingBackend& backend = samples.backend;
    REQUIRE(rig.driveUntil([&] { return shows(backend, "Proctor A"); }));

    backend.click(the(backend, "Button", "label", "New"));
    REQUIRE(rig.driveUntil([&] { return shows(backend, "New sample"); }));  // CreateSample's default name
}

TEST_CASE("Samples: Delete asks first, and dismissing deletes nothing", "[forms-demo]") {
    LabRig rig;
    Samples samples{rig};
    RecordingBackend& backend = samples.backend;
    REQUIRE(rig.driveUntil([&] { return shows(backend, "Proctor A"); }));

    auto const deletes = withProp(backend, "Button", "label", "Delete");
    REQUIRE(deletes.size() == 3);  // one per row; rows are in ListSamples' order, Proctor A first
    backend.click(deletes.front());
    rig.owner.runAll();
    auto const open = withProp(backend, "Dialog", "open", "true");
    REQUIRE(open.size() == 1);
    CHECK(backend.prop(open.front(), "title") == "Confirm");
    CHECK(shows(backend, "Are you sure?"));

    backend.dismiss(open.front());
    rig.owner.runAll();
    CHECK(withProp(backend, "Dialog", "open", "true").empty());

    // A create refetches the list; had the dismissed delete run, Proctor A would be gone from it.
    backend.click(the(backend, "Button", "label", "New"));
    REQUIRE(rig.driveUntil([&] { return shows(backend, "New sample"); }));
    CHECK(shows(backend, "Proctor A"));
}

TEST_CASE("Samples: activating a row opens EditSample prefilled with the row's id", "[forms-demo]") {
    LabRig rig;
    Samples samples{rig};
    RecordingBackend& backend = samples.backend;
    REQUIRE(rig.driveUntil([&] { return shows(backend, "Proctor B"); }));

    backend.activateRow(backend.all("Table").front(), morph::ui::Key{std::string{"2"}});
    rig.owner.runAll();
    CHECK(backend.prop(the(backend, "Dialog", "title", "EditSample"), "open") == "true");
    REQUIRE(rig.driveUntil([&] { return backend.find("TextInput", "text", "2").has_value(); }));
}
```

Add `tests/test_lab_collection.cpp` to the `forms_app_tests` source list.

- [ ] **Step 2: Run it to verify it fails**

```bash
cmake --build build/forms --target forms_app_tests
./build/forms/examples/forms/forms_app_tests "Samples:*"
```

Expected: FAIL — every case's first `REQUIRE(rig.driveUntil(…))` times out: with no view source the shell shows
`this screen's document is not available: samples`, so no row text appears.

- [ ] **Step 3: Implement**

In `examples/forms/app/lab_documents.cpp`, `sourcesOf`: replace `.views = {},` with
`.views = lookupIn(documents.views),`.

- [ ] **Step 4: Run the tests to verify they pass**

```bash
cmake --build build/forms --target forms_app_tests
./build/forms/examples/forms/forms_app_tests "[forms-demo]"
```

Expected: 16 test cases pass.

Mutation check: in `include/morph/forms/engine/collection.hpp`'s `collectionView`, change the confirm dialog's
`.onDismiss = [list] { list->cancelConfirm(); }` to `.onDismiss = [list] { list->confirm(); }`. Rebuild: "Samples:
Delete asks first, and dismissing deletes nothing" fails on `CHECK( shows(backend, "Proctor A") )`. Restore.

- [ ] **Step 5: Commit**

```bash
git add examples/forms/app/lab_documents.cpp examples/forms/tests/test_lab_collection.cpp examples/forms/CMakeLists.txt
git commit -m "wip(forms-demo): the samples collection as an app-shell view screen

Signed-off-by: Christian Parpart <christian@parpart.family>"
```

---

### Task 5: The typed entrance — `--typed`

**Files:**
- Create: `examples/forms/app/typed_lab_forms.hpp`, `examples/forms/app/typed_lab_forms.cpp`
- Modify: `examples/forms/app/lab_shell.hpp` — `Entrance`, `entranceFromArgs`; `LabShell` takes an `Entrance`,
  gains `typed()`, `typedScreen()` and the private `typedView()`
- Modify: `examples/forms/app/lab_shell.cpp` — `entranceFromArgs`, the constructors, `view`, `typedScreen`,
  `typedView`
- Modify: `examples/forms/CMakeLists.txt` — `app/typed_lab_forms.cpp` in `forms_app`; `tests/test_lab_typed.cpp`
- Test: `examples/forms/tests/test_lab_typed.cpp`

**Interfaces:**
- Consumes: `forms::Form<A, M, S>(runtime, handler, choices, options)`, `Form::session()`, `Form::value()`,
  `Form::lastResult()`, `forms::FormModel::forAction<A>()`, `forms::formView`, `wizardView`, `collectionView`
  (contract, Part 5); `forms::AppShellSession::{model, current, select, title, menuLabel, form, wizard,
  collection}`, `AppShellModel::find`, `ScreenKind`, `AppMenuEntry` (Task 2's block); `ui::forEach<RowT>`,
  `ui::menu`, `ui::MenuItem`, `ui::column`, `ui::row`, `ui::text` (Part 2); `morph::model::ActionTraits<A>::typeId()`
  (`include/morph/core/registry.hpp`); `morph::units::toString(Quantity)` (`include/morph/util/quantity.hpp`).
- Produces: `TypedLabForms(runtime, link)` with `density()`, `measurement()`, `screen(actionType) ->
  std::optional<ui::Node>`; `densityResultText(std::optional<Density> const&)`,
  `measurementResultText(std::optional<MeasurementAck> const&)`; `Entrance{Runtime, Typed}`,
  `entranceFromArgs(argc, argv)`; `LabShell(runtime, bridge, callbacks, Entrance = Runtime)` with `typed()` and
  `typedScreen(screenId)`.

The typed entrance keeps the one `AppShellSession` — its navigation, its wizard and its collection — and draws it
with a shell view of its own, the same `Column{title, Row{Menu, ForEach over current()}}` shape `appShellView`
draws, whose per-screen choice is `LabShell::typedScreen`: a typed form screen for an action `TypedLabForms` types,
else the session's own screen. The session makes a screen's session only when asked, so the typed entrance never
builds a runtime `FormSession` for a typed screen.

- [ ] **Step 1: Write the failing test**

Create `examples/forms/tests/test_lab_typed.cpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <morph/forms/engine/field_model.hpp>
#include <morph/forms/engine/form_session.hpp>
#include <morph/forms/engine/typed_form.hpp>
#include <morph/ui/mount.hpp>
#include <string>

#include "app/lab_documents.hpp"
#include "app/lab_shell.hpp"
#include "app/typed_lab_forms.hpp"
#include "lab_model.hpp"
#include "lab_test_support.hpp"

using lab::client::Entrance;
using lab::testing::LabRig;
using lab::testing::menuOf;
using lab::testing::shows;
using morph::ui::Mounted;
using morph::ui::testing::RecordingBackend;

TEST_CASE("entranceFromArgs: --typed anywhere selects the typed entrance", "[forms-demo]") {
    std::array<char const*, 1> const plain{"morph_forms_app"};
    std::array<char const*, 3> const typed{"morph_forms_app", "--ui=tui", "--typed"};
    CHECK(lab::client::entranceFromArgs(1, plain.data()) == Entrance::Runtime);
    CHECK(lab::client::entranceFromArgs(3, typed.data()) == Entrance::Typed);
}

TEST_CASE("Typed entrance: the served ComputeDryDensity schema reads as the type's own", "[forms-demo]") {
    auto const served = lab::client::memberJson(lab::client::labDocuments().schemas, "ComputeDryDensity");
    REQUIRE(served.has_value());
    auto const runtimeModel = morph::forms::FormModel::fromSchema("ComputeDryDensity", *served);
    REQUIRE(runtimeModel.has_value());
    auto const typedModel = morph::forms::FormModel::forAction<ComputeDryDensity>();
    CHECK(typedModel.actionType() == runtimeModel->actionType());
    REQUIRE(typedModel.fields().size() == runtimeModel->fields().size());
    for (std::size_t i = 0; i < typedModel.fields().size(); ++i) {
        CHECK(typedModel.fields()[i].name == runtimeModel->fields()[i].name);
        CHECK(typedModel.fields()[i].kind == runtimeModel->fields()[i].kind);
        CHECK(typedModel.fields()[i].required == runtimeModel->fields()[i].required);
    }
}

TEST_CASE("Typed entrance: both entrances build the same ComputeDryDensity body", "[forms-demo]") {
    LabRig rig;
    lab::client::LabShell runtimeShell{rig.runtime, rig.bridge, rig.owner};
    lab::client::LabShell typedShell{rig.runtime, rig.bridge, rig.owner, Entrance::Typed};
    REQUIRE(typedShell.typed() != nullptr);
    morph::forms::FormSession* const runtimeForm = runtimeShell.session().form("density");
    REQUIRE(runtimeForm != nullptr);
    morph::forms::FormSession& typedForm = typedShell.typed()->density().session();

    for (morph::forms::FormSession* const session : {runtimeForm, &typedForm}) {
        session->field("massDry").text().set("2650.5");
        session->field("volume").text().set("1");
    }
    rig.owner.runAll();

    auto const runtimeBody = runtimeForm->body();
    REQUIRE(runtimeBody.has_value());
    CHECK(typedForm.body() == runtimeBody);
    REQUIRE(typedShell.typed()->density().value().has_value());
}

TEST_CASE("Typed entrance: the density screen shows the reply as a Density", "[forms-demo]") {
    LabRig rig;
    lab::client::LabShell shell{rig.runtime, rig.bridge, rig.owner, Entrance::Typed};
    RecordingBackend backend;
    Mounted const mounted{rig.runtime, backend, shell.view()};
    rig.owner.runAll();
    CHECK(backend.prop(menuOf(backend), "items") == "[Density,Measure,Intake,Samples]");

    auto& density = shell.typed()->density();
    density.session().field("massDry").text().set("2650.5");
    density.session().field("volume").text().set("1");
    REQUIRE(rig.driveUntil([&] { return density.lastResult().has_value(); }));
    rig.owner.runAll();

    auto const line = lab::client::densityResultText(density.lastResult());
    CHECK(line.starts_with("Dry density: 2650.5"));
    CHECK(shows(backend, line));
}

TEST_CASE("Typed entrance: the wizard and the samples come from the documents", "[forms-demo]") {
    LabRig rig;
    lab::client::LabShell shell{rig.runtime, rig.bridge, rig.owner, Entrance::Typed};
    RecordingBackend backend;
    Mounted const mounted{rig.runtime, backend, shell.view()};
    rig.owner.runAll();

    backend.chooseIndex(menuOf(backend), lab::testing::kIntake);
    rig.owner.runAll();
    CHECK(shows(backend, "New sample"));
    backend.chooseIndex(menuOf(backend), lab::testing::kSamples);
    REQUIRE(rig.driveUntil([&] { return shows(backend, "Proctor A"); }));
}
```

Add `tests/test_lab_typed.cpp` to the `forms_app_tests` source list.

- [ ] **Step 2: Run it to verify it fails**

```bash
cmake --build build/forms --target forms_app_tests
```

Expected: FAIL to compile — `'app/typed_lab_forms.hpp' file not found`.

- [ ] **Step 3: Implement**

Create `examples/forms/app/typed_lab_forms.hpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// The typed entrance's form screens: `forms::Form<A>`s over the lab's two form actions, so a screen can show the
/// reply decoded into its C++ type. The engine reads the same schema either way (`FormModel::forAction<A>()` is the
/// runtime reader over `schemaJson<A>()`); the wizard and the collection have no typed facade, so both entrances
/// take those from the served documents.

#include <morph/forms/engine/typed_form.hpp>
#include <morph/reactive/runtime.hpp>
#include <morph/ui/view.hpp>
#include <optional>
#include <string>
#include <string_view>

#include "app/lab_link.hpp"
#include "lab_model.hpp"

namespace lab::client {

/// @brief The line the typed density screen shows for a reply.
/// @param result The typed reply, if one arrived.
/// @return `"Dry density: <value><unit>"`, or empty before the first reply.
[[nodiscard]] std::string densityResultText(std::optional<Density> const& result);

/// @brief The line the typed measurement screen shows for a reply.
/// @param result The typed reply, if one arrived.
/// @return `"Recorded: <summary>"`, or empty before the first reply.
[[nodiscard]] std::string measurementResultText(std::optional<MeasurementAck> const& result);

/// @brief The two typed form screens.
class TypedLabForms {
public:
    /// @brief Builds both forms on @p link's handler.
    /// @param runtime The runtime the forms bind to; its owner is @p link's callback executor.
    /// @param link The handler both forms submit through; it outlives this object.
    TypedLabForms(morph::reactive::Runtime& runtime, LabLink& link);

    TypedLabForms(TypedLabForms const&) = delete;
    TypedLabForms& operator=(TypedLabForms const&) = delete;
    TypedLabForms(TypedLabForms&&) = delete;
    TypedLabForms& operator=(TypedLabForms&&) = delete;
    ~TypedLabForms() = default;

    /// @brief The typed screen of a form screen whose action this class types.
    /// @param actionType The screen's `ref`: an action's registered type id.
    /// @return The form and its typed reply line, or `std::nullopt` for an action it does not type.
    [[nodiscard]] std::optional<morph::ui::Node> screen(std::string_view actionType);

    /// @brief The typed density form.
    /// @return The form.
    [[nodiscard]] morph::forms::Form<ComputeDryDensity, LabModel>& density() noexcept { return _density; }

    /// @brief The typed measurement form.
    /// @return The form.
    [[nodiscard]] morph::forms::Form<RecordMeasurement, LabModel>& measurement() noexcept { return _measurement; }

private:
    morph::forms::Form<ComputeDryDensity, LabModel> _density;
    morph::forms::Form<RecordMeasurement, LabModel> _measurement;
};

}  // namespace lab::client
```

Create `examples/forms/app/typed_lab_forms.cpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0

#include "app/typed_lab_forms.hpp"

#include <functional>
#include <morph/core/registry.hpp>
#include <morph/forms/engine/form_view.hpp>
#include <morph/util/quantity.hpp>
#include <string>
#include <utility>

namespace lab::client {

namespace {

/// @brief A typed form screen: the form, then the typed reply line.
morph::ui::Node typedScreen(morph::forms::FormSession& session, std::function<std::string()> resultLine) {
    return morph::ui::column({
        .children = {morph::forms::formView(session),
                     morph::ui::text({.text = std::move(resultLine), .role = morph::ui::TextRole::Success})},
        .gap = 1,
    });
}

}  // namespace

std::string densityResultText(std::optional<Density> const& result) {
    return result ? "Dry density: " + morph::units::toString(*result) : std::string{};
}

std::string measurementResultText(std::optional<MeasurementAck> const& result) {
    return result ? "Recorded: " + result->summary : std::string{};
}

TypedLabForms::TypedLabForms(morph::reactive::Runtime& runtime, LabLink& link)
    : _density{runtime, link.handler(), link.choices()}, _measurement{runtime, link.handler(), link.choices()} {}

std::optional<morph::ui::Node> TypedLabForms::screen(std::string_view actionType) {
    if (actionType == morph::model::ActionTraits<ComputeDryDensity>::typeId()) {
        return typedScreen(_density.session(), [this] { return densityResultText(_density.lastResult()); });
    }
    if (actionType == morph::model::ActionTraits<RecordMeasurement>::typeId()) {
        return typedScreen(_measurement.session(),
                           [this] { return measurementResultText(_measurement.lastResult()); });
    }
    return std::nullopt;
}

}  // namespace lab::client
```

In `examples/forms/app/lab_shell.hpp`: add `#include <cstdint>`, `#include <memory>` and
`#include "app/typed_lab_forms.hpp"`; before `parseShell` add

```cpp
/// @brief How the form screens are built.
enum class Entrance : std::uint8_t {
    Runtime,  ///< From the served schema documents, by the app-shell session.
    Typed,    ///< From `forms::Form<A>` over the lab's action types.
};

/// @brief The entrance the command line asks for: `--typed` anywhere selects `Entrance::Typed`.
/// @param argc Argument count.
/// @param argv Arguments; `argv[0]` is the program.
/// @return The entrance.
[[nodiscard]] Entrance entranceFromArgs(int argc, char const* const* argv);
```

and replace the `LabShell` class with

```cpp
/// @brief The demo's controller: one LabModel link, the shell session over the served documents, and in the typed
///        entrance the typed form screens.
class LabShell {
public:
    /// @brief Opens the shell on the first menu entry; each screen's session is made when it is first shown.
    /// @param runtime The runtime every session binds to.
    /// @param bridge The bridge LabModel lives behind.
    /// @param callbacks Where replies are delivered: @p runtime's owner.
    /// @param entrance How the form screens are built.
    LabShell(morph::reactive::Runtime& runtime, morph::bridge::Bridge& bridge, morph::exec::IExecutor& callbacks,
             Entrance entrance = Entrance::Runtime);

    LabShell(LabShell const&) = delete;
    LabShell& operator=(LabShell const&) = delete;
    LabShell(LabShell&&) = delete;
    LabShell& operator=(LabShell&&) = delete;
    ~LabShell() = default;

    /// @brief The app-shell session: the current screen, and every screen's session.
    /// @return The session.
    [[nodiscard]] morph::forms::AppShellSession& session() noexcept { return _session; }

    /// @brief The typed form screens.
    /// @return The forms in the typed entrance; null in the runtime entrance.
    [[nodiscard]] TypedLabForms* typed() noexcept { return _typed.get(); }

    /// @brief One screen as the typed entrance shows it: a typed form screen when its action is typed, else the
    ///        session's own screen, else a line saying its document is not available.
    /// @param screenId An `app-screens` id.
    /// @return The screen's view.
    [[nodiscard]] morph::ui::Node typedScreen(std::string_view screenId);

    /// @brief The shell's view: the title, and the menu beside the current screen.
    /// @return `forms::appShellView(session())` in the runtime entrance; the same shape over `typedScreen` in the
    ///         typed one.
    [[nodiscard]] morph::ui::Node view();

private:
    LabShell(morph::reactive::Runtime& runtime, morph::bridge::Bridge& bridge, morph::exec::IExecutor& callbacks,
             Entrance entrance, LabDocuments const& documents);

    [[nodiscard]] morph::ui::Node typedView();

    LabLink _link;
    std::unique_ptr<TypedLabForms> _typed;  // after _link: both forms submit through its handler
    morph::forms::AppShellSession _session;
};
```

In `examples/forms/app/lab_shell.cpp`: add `#include <cstddef>`, `#include <memory>`, `#include <morph/forms/engine/form_view.hpp>`,
`#include <span>`, `#include <string>` and `#include <vector>`; add

```cpp
Entrance entranceFromArgs(int argc, char const* const* argv) {
    if (argc < 2) {
        return Entrance::Runtime;
    }
    for (char const* arg : std::span{argv, static_cast<std::size_t>(argc)}.subspan(1)) {
        if (std::string_view{arg} == "--typed") {
            return Entrance::Typed;
        }
    }
    return Entrance::Runtime;
}
```

and replace both constructor definitions and `view()` with:

```cpp
LabShell::LabShell(morph::reactive::Runtime& runtime, morph::bridge::Bridge& bridge, morph::exec::IExecutor& callbacks,
                   Entrance entrance)
    : LabShell{runtime, bridge, callbacks, entrance, labDocuments()} {}

LabShell::LabShell(morph::reactive::Runtime& runtime, morph::bridge::Bridge& bridge, morph::exec::IExecutor& callbacks,
                   Entrance entrance, LabDocuments const& documents)
    : _link{bridge, callbacks},
      _typed{entrance == Entrance::Typed ? std::make_unique<TypedLabForms>(runtime, _link) : nullptr},
      _session{runtime, parseShell(documents.app), sourcesOf(documents), _link.submitter(), _link.choices()} {}

morph::ui::Node LabShell::view() {
    if (_typed == nullptr) {
        return morph::forms::appShellView(_session);
    }
    return typedView();
}

morph::ui::Node LabShell::typedScreen(std::string_view screenId) {
    morph::forms::AppScreen const* const screen = _session.model().find(screenId);
    if (_typed != nullptr && screen != nullptr && screen->kind == morph::forms::ScreenKind::Form) {
        if (auto typedNode = _typed->screen(screen->ref)) {
            return *std::move(typedNode);
        }
    }
    if (morph::forms::FormSession* const form = _session.form(screenId)) {
        return morph::forms::formView(*form);
    }
    if (morph::forms::WizardSession* const wizard = _session.wizard(screenId)) {
        return morph::forms::wizardView(*wizard);
    }
    if (morph::forms::CollectionSession* const collection = _session.collection(screenId)) {
        return morph::forms::collectionView(*collection);
    }
    return morph::ui::text({.text = std::format("this screen's document is not available: {}", screenId),
                            .role = morph::ui::TextRole::Error});
}

morph::ui::Node LabShell::typedView() {
    morph::forms::AppShellSession* const shell = &_session;
    std::vector<morph::ui::MenuItem> items;
    std::size_t index = 0;
    for (morph::forms::AppMenuEntry const& entry : shell->model().menu()) {
        items.push_back(morph::ui::MenuItem{.label = shell->menuLabel(index),
                                            .onSelect = [shell, screen = entry.screen] { shell->select(screen); }});
        ++index;
    }
    auto content = morph::ui::forEach<std::string>(
        [shell] { return std::vector<std::string>{shell->current()}; },
        [](std::string const& screenId) { return morph::ui::Key{screenId}; },
        [this](morph::reactive::Signal<std::string> const& slot) { return typedScreen(slot.peek()); });
    return morph::ui::column({
        .children = {morph::ui::text({.text = shell->title(), .role = morph::ui::TextRole::Heading}),
                     morph::ui::row({.children = {morph::ui::menu({.items = std::move(items)}), std::move(content)},
                                     .gap = 2})},
        .gap = 1,
    });
}
```

In `examples/forms/CMakeLists.txt`, add `app/typed_lab_forms.cpp` to `forms_app`'s sources after
`app/lab_shell.cpp`.

- [ ] **Step 4: Run the tests to verify they pass**

```bash
cmake --build build/forms --target forms_app_tests
./build/forms/examples/forms/forms_app_tests "[forms-demo]"
```

Expected: 21 test cases pass.

Mutation check: in `labDocuments()` (`app/lab_documents.cpp`), make the runtime documents disagree with the types
by one declared precision — replace the `return` with:

```cpp
    LabDocuments documents{.app = lab::appSchemaJson(), .schemas = lab::schemasJson(),
                           .wizards = lab::wizardSchemasJson(), .views = lab::viewsJson()};
    constexpr std::string_view kFour = R"("x-decimalPlaces":4)";
    if (auto const found = documents.schemas.find(kFour); found != std::string::npos) {
        documents.schemas.replace(found, kFour.size(), R"("x-decimalPlaces":3)");
    }
    return documents;
```

Rebuild: "Typed entrance: both entrances build the same ComputeDryDensity body" fails on its `CHECK` (the runtime
body carries `"dp":3` for `volume`, the typed one `"dp":4`). Restore.

- [ ] **Step 5: Commit**

```bash
git add examples/forms/app examples/forms/tests/test_lab_typed.cpp examples/forms/CMakeLists.txt
git commit -m "wip(forms-demo): the typed entrance, --typed

Signed-off-by: Christian Parpart <christian@parpart.family>"
```

---

### Task 6: `morph_forms_app` on the injected frontend; the QML demo goes

**Files:**
- Create: `examples/forms/app/lab_application.hpp`, `examples/forms/app/lab_application.cpp`
- Create: `examples/forms/ui/main.cpp`
- Modify: `examples/forms/CMakeLists.txt` — `app/lab_application.cpp` and the `morph_ladder_app_common` link in
  `forms_app`; the `morph_forms_app` binary; `tests/test_lab_application.cpp`; the `forms_app_smoke_tests` binary;
  the header comment; remove the `if(MORPH_BUILD_FORMS_QML) add_subdirectory(gui_qml) endif()` block
- Delete: `examples/forms/gui_qml/` (CMakeLists.txt, FormsController.{hpp,cpp}, main.cpp, qml/AppShell.qml,
  qml/Main.qml)
- Modify: `CMakeLists.txt` — the two comments that name
  `examples/forms/gui_qml` (the "Qt/QML forms renderer setup" block before `if(MORPH_BUILD_FORMS_QML)` and the
  "Qt/QML forms renderer (optional)" block before `add_subdirectory(src/qt/forms)`)
- Test: `examples/forms/tests/test_lab_application.cpp`, `examples/forms/tests/smoke/test_lab_frontend_smoke.cpp`

**Interfaces:**
- Consumes: `ui::Application`, `ui::AppContext`, `ui::ApplicationFactory`, `ui::FrontendOption`,
  `ui::selectFrontend` (Part 2, `morph/ui/frontend.hpp`); `tui::frontendOption()` (Part 3, `morph/tui/frontend.hpp`);
  `qt_quick::frontendOption(int&, char**)` (Part 4, `morph/qt_quick/frontend.hpp`); from Part 6:
  `examples::AppEnvironment` (`app/app_environment.hpp`), `examples::connect`, `examples::LocalSetup{setupDatabase,
  workers}`, `examples::Connection::{bridge, callbacks}` (`app/transport.hpp`), target `morph_ladder_app_common`;
  `examples::testing::FakeAppContext` (`testkit/fake_app_context.hpp`, `owner()` a `MainThreadExecutor`),
  `examples::testing::pumpUntil` (`testkit/wait.hpp`), `examples::testing::runFrontendSmoke`, `SmokeFrontend{Tui,
  QtQuick}` (`testkit/frontend_smoke.hpp`), target `morph_example_testkit` (links every built frontend and defines
  `MORPH_EXAMPLE_HAS_TUI`/`MORPH_EXAMPLE_HAS_QT_QUICK`, both PUBLIC); CMake `morph_add_example_ui(TARGET … SOURCES …
  LIBRARIES …)` (`cmake/morph_example_app.cmake`).
- Produces: `lab::client::LabApplication(ui::AppContext&, examples::AppEnvironment const&, Entrance)`;
  `lab::client::makeApplication(ui::AppContext&, examples::AppEnvironment const&)` (the contract's convention, the
  runtime entrance) and `makeApplication(ui::AppContext&, examples::AppEnvironment const&, Entrance)`; the binary
  `morph_forms_app`; the test binary `forms_app_smoke_tests`.

- [ ] **Step 1: Write the failing test**

Create `examples/forms/tests/test_lab_application.cpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0

#include <app/app_environment.hpp>
#include <catch2/catch_test_macros.hpp>
#include <memory>
#include <morph/ui/frontend.hpp>
#include <morph/ui/mount.hpp>
#include <stdexcept>
#include <string>
#include <testkit/fake_app_context.hpp>
#include <testkit/wait.hpp>

#include "app/lab_application.hpp"
#include "lab_test_support.hpp"

using lab::testing::menuOf;
using lab::testing::shows;
using morph::examples::AppEnvironment;
using morph::examples::testing::FakeAppContext;
using morph::examples::testing::pumpUntil;
using morph::ui::Mounted;
using morph::ui::testing::RecordingBackend;

TEST_CASE("makeApplication: the runtime entrance mounts the shell on any AppContext", "[forms-demo]") {
    FakeAppContext ctx;
    auto const app = lab::client::makeApplication(ctx, AppEnvironment{});
    REQUIRE(app != nullptr);
    RecordingBackend backend;
    Mounted const mounted{ctx.runtime(), backend, app->view()};

    REQUIRE(pumpUntil(ctx.owner(), [&] { return shows(backend, "Oven-dry mass of the specimen"); }));
    CHECK(backend.prop(menuOf(backend), "items") == "[Density,Measure,Intake,Samples]");
}

TEST_CASE("makeApplication: the typed entrance shows the typed reply line", "[forms-demo]") {
    FakeAppContext ctx;
    auto const app = lab::client::makeApplication(ctx, AppEnvironment{}, lab::client::Entrance::Typed);
    RecordingBackend backend;
    Mounted const mounted{ctx.runtime(), backend, app->view()};
    REQUIRE(pumpUntil(ctx.owner(), [&] { return backend.all("TextInput").size() == 2; }));  // massDry, volume

    auto const inputs = backend.all("TextInput");
    backend.edit(inputs.at(0), "2650.5");
    backend.edit(inputs.at(1), "1");

    auto const densityLine = [&] {
        for (int const widgetId : backend.all("Text")) {
            if (std::string const text = backend.prop(widgetId, "text"); text.starts_with("Dry density: ")) {
                return text;
            }
        }
        return std::string{};
    };
    REQUIRE(pumpUntil(ctx.owner(), [&] { return !densityLine().empty(); }));
    CHECK(densityLine().starts_with("Dry density: 2650.5"));
}

TEST_CASE("makeApplication: --server is refused, the model runs in this process", "[forms-demo]") {
    FakeAppContext ctx;
    AppEnvironment env;
    env.server = "ws://localhost:8080";
    CHECK_THROWS_AS(lab::client::makeApplication(ctx, env), std::invalid_argument);
}
```

Create `examples/forms/tests/smoke/test_lab_frontend_smoke.cpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0
//
// The application mounts and quits on every frontend this configure built: on the terminal through scripted
// input, and on Qt Quick offscreen.

#include <app/app_environment.hpp>
#include <catch2/catch_test_macros.hpp>
#include <morph/ui/frontend.hpp>
#include <testkit/frontend_smoke.hpp>

#include "app/lab_application.hpp"

namespace {

[[maybe_unused]] morph::ui::ApplicationFactory factoryFor(lab::client::Entrance entrance) {
    return [entrance](morph::ui::AppContext& ctx) {
        return lab::client::makeApplication(ctx, morph::examples::AppEnvironment{}, entrance);
    };
}

}  // namespace

#if MORPH_EXAMPLE_HAS_TUI
TEST_CASE("morph_forms_app: mounts and quits on the terminal UI", "[forms-demo][smoke][tui]") {
    morph::examples::testing::runFrontendSmoke(factoryFor(lab::client::Entrance::Runtime),
                                               morph::examples::testing::SmokeFrontend::Tui);
}

TEST_CASE("morph_forms_app --typed: mounts and quits on the terminal UI", "[forms-demo][smoke][tui]") {
    morph::examples::testing::runFrontendSmoke(factoryFor(lab::client::Entrance::Typed),
                                               morph::examples::testing::SmokeFrontend::Tui);
}
#endif

#if MORPH_EXAMPLE_HAS_QT_QUICK
TEST_CASE("morph_forms_app: mounts and quits on Qt Quick, offscreen", "[forms-demo][smoke][qt]") {
    morph::examples::testing::runFrontendSmoke(factoryFor(lab::client::Entrance::Runtime),
                                               morph::examples::testing::SmokeFrontend::QtQuick);
}
#endif
```

In `examples/forms/CMakeLists.txt`, add `tests/test_lab_application.cpp` to the `forms_app_tests` source list
(`FakeAppContext` and `pumpUntil` are header-only, on `morph_ladder_app_common`'s PUBLIC include path
`examples/common`, which `forms_app` passes on), and after `catch_discover_tests(forms_app_tests …)` add:

```cmake
        # ── forms_app_smoke_tests: the application on each built frontend ────
        # A binary of its own whose main (morph_test_main) owns no Qt application object, because the Qt Quick
        # frontend constructs its own. morph_example_testkit (examples/common/app, named by its plain target name)
        # brings the harness, every built frontend and MORPH_EXAMPLE_HAS_TUI / MORPH_EXAMPLE_HAS_QT_QUICK. It
        # exists only where the binary does.
        if(TARGET morph_forms_app)
            add_executable(forms_app_smoke_tests tests/smoke/test_lab_frontend_smoke.cpp)
            target_link_libraries(forms_app_smoke_tests PRIVATE forms_app morph_example_testkit morph_test_main)
            target_compile_features(forms_app_smoke_tests PRIVATE cxx_std_23)
            apply_warnings(forms_app_smoke_tests)
            apply_bigobj(forms_app_smoke_tests)
            if(DEFINED AF_SANITIZER)
                apply_sanitizers(forms_app_smoke_tests ${AF_SANITIZER})
            endif()
            catch_discover_tests(forms_app_smoke_tests
                DISCOVERY_MODE PRE_TEST
                PROPERTIES LABELS forms-demo)
        endif()
```

- [ ] **Step 2: Run it to verify it fails**

```bash
cmake --build build/forms --target forms_app_tests
```

Expected: FAIL to compile — `'app/lab_application.hpp' file not found`.

- [ ] **Step 3: Implement**

Create `examples/forms/app/lab_application.hpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// The forms demo as a `ui::Application`: an in-process LabModel and the shell over it, on whichever frontend
/// runs it.

#include <app/app_environment.hpp>
#include <app/transport.hpp>
#include <memory>
#include <morph/ui/frontend.hpp>
#include <morph/ui/view.hpp>

#include "app/lab_shell.hpp"

namespace lab::client {

/// @brief The demo application: a local connection (a worker pool and a bridge on the frontend's executor) and the
///        shell over it.
class LabApplication final : public morph::ui::Application {
public:
    /// @brief Connects locally and builds the shell on @p ctx's runtime and executor.
    /// @param ctx The frontend's context.
    /// @param env The command line's environment; the demo runs LabModel in this process.
    /// @param entrance How the form screens are built.
    /// @throws std::invalid_argument when @p env names a server: the demo has none.
    LabApplication(morph::ui::AppContext& ctx, morph::examples::AppEnvironment const& env, Entrance entrance);

    /// @brief The shell's view.
    /// @return The root node.
    [[nodiscard]] morph::ui::Node view() override;

private:
    std::unique_ptr<morph::examples::Connection> _connection;  // first: the shell's handler lives on its bridge
    LabShell _shell;
};

/// @brief The demo's `ui::ApplicationFactory` body: the runtime entrance.
/// @param ctx The frontend's context.
/// @param env The command line's environment.
/// @return The application.
/// @throws std::invalid_argument when @p env names a server.
[[nodiscard]] std::unique_ptr<morph::ui::Application> makeApplication(morph::ui::AppContext& ctx,
                                                                      morph::examples::AppEnvironment const& env);

/// @brief The demo's `ui::ApplicationFactory` body, with the entrance `--typed` selects.
/// @param ctx The frontend's context.
/// @param env The command line's environment.
/// @param entrance How the form screens are built.
/// @return The application.
/// @throws std::invalid_argument when @p env names a server.
[[nodiscard]] std::unique_ptr<morph::ui::Application> makeApplication(morph::ui::AppContext& ctx,
                                                                      morph::examples::AppEnvironment const& env,
                                                                      Entrance entrance);

}  // namespace lab::client
```

Create `examples/forms/app/lab_application.cpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0

#include "app/lab_application.hpp"

#include <memory>
#include <stdexcept>

namespace lab::client {

namespace {

/// @brief The local connection LabModel runs behind; there is no LabModel server to reach.
std::unique_ptr<morph::examples::Connection> connectLocally(morph::ui::AppContext& ctx,
                                                            morph::examples::AppEnvironment const& env) {
    if (env.server.has_value()) {
        throw std::invalid_argument{"morph_forms_app runs LabModel in this process; --server is not supported"};
    }
    return morph::examples::connect(ctx, env, morph::examples::LocalSetup{.setupDatabase = {}, .workers = 2});
}

}  // namespace

LabApplication::LabApplication(morph::ui::AppContext& ctx, morph::examples::AppEnvironment const& env,
                               Entrance entrance)
    : _connection{connectLocally(ctx, env)},
      _shell{ctx.runtime(), _connection->bridge(), _connection->callbacks(), entrance} {}

morph::ui::Node LabApplication::view() { return _shell.view(); }

std::unique_ptr<morph::ui::Application> makeApplication(morph::ui::AppContext& ctx,
                                                        morph::examples::AppEnvironment const& env) {
    return makeApplication(ctx, env, Entrance::Runtime);
}

std::unique_ptr<morph::ui::Application> makeApplication(morph::ui::AppContext& ctx,
                                                        morph::examples::AppEnvironment const& env,
                                                        Entrance entrance) {
    return std::make_unique<LabApplication>(ctx, env, entrance);
}

}  // namespace lab::client
```

Create `examples/forms/ui/main.cpp` (spec 4 §3's composition root, as `examples/bank/ui/main.cpp` has it, plus the
entrance):

```cpp
// SPDX-License-Identifier: Apache-2.0

/// @file
/// morph_forms_app — the forms demo on any frontend.
///
///   morph_forms_app [--ui=tui|qt] [--typed]
///
/// The frontend is `--ui=<name>`, else `MORPH_UI`, else the first built frontend that can run here (Qt Quick,
/// then the terminal UI). `--typed` builds the form screens from `forms::Form<A>` instead of the served schemas.

#include <app/app_environment.hpp>
#include <exception>
#include <iostream>
#include <morph/ui/frontend.hpp>
#include <vector>

#include "app/lab_application.hpp"

#if MORPH_EXAMPLE_HAS_QT_QUICK
#include <morph/qt_quick/frontend.hpp>
#endif
#if MORPH_EXAMPLE_HAS_TUI
#include <morph/tui/frontend.hpp>
#endif

int main(int argc, char** argv) {
    try {
        auto const env = morph::examples::AppEnvironment::fromArgs(argc, argv);
        auto const entrance = lab::client::entranceFromArgs(argc, argv);
        std::vector<morph::ui::FrontendOption> built;
#if MORPH_EXAMPLE_HAS_QT_QUICK
        built.push_back(morph::qt_quick::frontendOption(argc, argv));
#endif
#if MORPH_EXAMPLE_HAS_TUI
        built.push_back(morph::tui::frontendOption());
#endif
        auto const frontend = morph::ui::selectFrontend(built, argc, argv);
        return frontend->run([&env, entrance](morph::ui::AppContext& ctx) {
            return lab::client::makeApplication(ctx, env, entrance);
        });
    } catch (std::exception const& error) {
        std::cerr << "morph_forms_app: " << error.what() << '\n';
        return 1;
    }
}
```

In `examples/forms/CMakeLists.txt`:

1. Replace the header comment (the four lines after the SPDX line) with:

   ```cmake
   # Schema-driven forms demo. morph_forms_demo is the console client: it prints the generated action schemas,
   # emits a self-contained HTML/JS form renderer, and executes pasted action lines through the type-erased
   # dispatcher. morph_forms_app is the same lab model's forms on the terminal UI or Qt Quick: forms_app (app/)
   # is its toolkit-free app library, ui/main.cpp its composition root.
   ```

2. Delete the block `if(MORPH_BUILD_FORMS_QML)` / `add_subdirectory(gui_qml)` / `endif()`.
3. In `forms_app`: add `app/lab_application.cpp` after `app/typed_lab_forms.cpp`; change its link line to
   `target_link_libraries(forms_app PUBLIC morph::morph morph_ladder_app_common)` and its comment's "it links morph
   and nothing else" to "it links morph and morph_ladder_app_common (examples/common/app, Qt-free, added by the root
   after this directory and named by its plain target name) and nothing else".
4. After the `forms_app` library block, add:

   ```cmake
   # ── morph_forms_app: the binary ──────────────────────────────────────────────
   # Built when at least one frontend is (spec 4 §6); it links every frontend that was built and says which
   # through MORPH_EXAMPLE_HAS_TUI / MORPH_EXAMPLE_HAS_QT_QUICK (cmake/morph_example_app.cmake).
   morph_add_example_ui(TARGET morph_forms_app SOURCES ui/main.cpp LIBRARIES forms_app)
   ```

The root `CMakeLists.txt`'s "Example applications on an injected frontend" block (Part 6) already adds
`examples/common/app` in every native configure with examples on, which is what `forms_app` needs; confirm with
`grep -n "NOT EMSCRIPTEN OR MORPH_BUILD_BANK_EXAMPLE" CMakeLists.txt`.

Delete the QML demo:

```bash
git rm -r examples/forms/gui_qml
```

In the root `CMakeLists.txt`, replace the whole comment block between the line
`# ── Qt/QML forms renderer setup (optional) ───…` and `if(MORPH_BUILD_FORMS_QML)` with:

```cmake
# ── Qt/QML forms renderer setup (optional) ───────────────────────────────────
# Ships the Qt/QML forms renderer (MorphForms, src/qt/forms) as a reusable component, independent of
# MORPH_BUILD_EXAMPLES. The find_package/qt_standard_project_setup() call and the morph::qt_forms interface target
# are created here, before the rest of the tree is configured, because a consumer links morph::qt_forms by its
# namespaced name. add_subdirectory(src/qt/forms) itself (the module, its plugin and a Catch2-linked test
# executable) is deferred to just after the "Tests" section, since Catch2 is only found or fetched there.
# Emscripten builds this too; the module's one host-only piece, the QuickTest suite, is guarded inside
# src/qt/forms/CMakeLists.txt.
```

and the comment block between `# ── Qt/QML forms renderer (optional) ───…` and the second
`if(MORPH_BUILD_FORMS_QML)` with:

```cmake
# ── Qt/QML forms renderer (optional) ─────────────────────────────────────────
# The MorphForms module and plugin (src/qt/forms), deferred to here because its CMakeLists.txt links a Catch2
# test executable against Catch2::Catch2 when MORPH_BUILD_TESTS is ON. Qt6 Quick/Qml was found and
# morph::qt_forms created earlier, in "Qt/QML forms renderer setup".
```

Both blocks are deleted outright in Task 9; this keeps commit 17's comments true.

- [ ] **Step 4: Run the tests to verify they pass**

```bash
cmake -S . -B build/forms   # re-configure: new sources
cmake --build build/forms --target forms_app_tests forms_app_smoke_tests morph_forms_app morph_forms_demo
ctest --test-dir build/forms -L forms-demo --output-on-failure
ctest --test-dir build/forms -R 'forms_html_math|forms_repl_roundtrip' --output-on-failure
cmake -S . -B build/forms-nofe -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build/forms-nofe --target forms_app_tests && ctest --test-dir build/forms-nofe -L forms-demo
cmake --build build/forms-nofe --target morph_forms_app 2>&1 | grep -q "unknown target 'morph_forms_app'" \
    && echo "no frontend, no binary"
git ls-files examples/forms | grep -c gui_qml   # expect 0
```

Expected: on `build/forms`, 24 `forms_app_tests` cases and 3 `forms_app_smoke_tests` cases, all passing; on
`build/forms-nofe`, the 24 `forms_app_tests` cases pass and neither `morph_forms_app` nor `forms_app_smoke_tests`
exists ("no frontend, no binary"); the CLI's two tests pass; the last line prints `0`.

Mutation check: in the three-argument `makeApplication`, pass `Entrance::Runtime` instead of `entrance`. Rebuild:
"makeApplication: the typed entrance shows the typed reply line" fails at its second `REQUIRE(pumpUntil(…))`.
Restore.

- [ ] **Step 5: Commit**

```bash
git add examples/forms CMakeLists.txt
git commit -m "wip(forms-demo): morph_forms_app on the injected frontend; remove the QML demo

Signed-off-by: Christian Parpart <christian@parpart.family>"
```

---

### Task 7: The demo's documentation

**Files:**
- Modify: `examples/forms/README.md` — the file map, "Build", section 3, section 4, "Tests", "Notes"
- Modify: `README.md` — the `examples/forms` paragraph and code block in the forms section; the "Examples" list entry
- Modify: `docs/ARCHITECTURE.md` — the `examples/forms` sentence after the forms headers paragraph; the `Choice`
  paragraph's last sentence

**Interfaces:** none (documentation of Tasks 1–6).

- [ ] **Step 1: Write the failing test**

The check every doc task in this part uses — every backticked repository path in the file exists, and no retired
name is left — saved once as a scratch script:

```bash
cat > /tmp/morph-doc-check.sh <<'SH'
#!/usr/bin/env bash
# Usage: morph-doc-check.sh PATTERN FILE...  -- fails on a missing path or a PATTERN hit
set -u
pattern=$1; shift
status=0
for file in "$@"; do
    while IFS= read -r path; do
        [ -e "$path" ] || { echo "$file: names a missing path: $path"; status=1; }
    done < <(grep -oE '`(examples|include|src|scripts|cmake|tests|docs|\.github)/[^` ]+`' "$file" \
             | tr -d '`' | grep -v '[<*{]' | sed -E 's/[:#].*$//' | sort -u)
    if grep -nE "$pattern" "$file"; then status=1; fi
done
exit $status
SH
chmod +x /tmp/morph-doc-check.sh
/tmp/morph-doc-check.sh 'gui_qml|morph_forms_qml|AppShell\.qml|Main\.qml|forms_qml_logic|QML renderer' \
    examples/forms/README.md
git grep -nE 'gui_qml|morph_forms_qml|QML client|Qt Quick GUI|QML renderers' -- README.md docs/ARCHITECTURE.md
```

(The path half runs on the demo's README only; `README.md` and `docs/ARCHITECTURE.md` get the name check, so a
path that was already missing there before this part cannot fail this task.)

- [ ] **Step 2: Run it to verify it fails**

Expected: the first command exits 1, printing `examples/forms/README.md` lines naming `gui_qml/`,
`morph_forms_qml`, `AppShell.qml`, `forms_qml_logic`; the second prints `README.md`'s
`./build/examples/forms/gui_qml/morph_forms_qml` line and `docs/ARCHITECTURE.md`'s "the QML client fetches them
live" line.

- [ ] **Step 3: Implement**

`examples/forms/README.md`:

- Opening paragraph: replace "This example runs the whole loop locally with **two renderers driven by the same
  generated schemas** — nothing about the forms is hardcoded in either client." with "This example runs the whole
  loop locally with **two clients driven by the same generated schemas** — a browser page and an app on the
  terminal UI or Qt Quick — and nothing about the forms is hardcoded in either."
- File map: add `lab_views.hpp     SamplesView (a list/master-detail view over ListSamples)` after `lab_wizard.hpp`'s
  two lines; change `lab_schemas.hpp`'s lines to `lab_schemas.hpp   the served documents: {actionType: schema},
  {wizardId: schema}, the app-* document, {viewId: schema}`; replace the `gui_qml/` line with
  `app/              forms_app: the app library (screens over the forms engine)` and
  `ui/main.cpp       morph_forms_app: frontend selection and the app factory`; add
  `tests/            the app's tests (RecordingBackend, both entrances, frontend smoke)`.
- Reading order: `lab_units.hpp` → `lab_model.hpp` → `lab_views.hpp` → `lab_wizard.hpp` → `lab_schemas.hpp` →
  `app/`.
- "## Build": replace the code block with

  ```sh
  cmake -B build -G Ninja -DMORPH_BUILD_TUI=ON -DMORPH_BUILD_QT=ON -DMORPH_BUILD_QT_QUICK=ON
  ninja -C build morph_forms_demo morph_forms_app
  ```

  and add below it: "`morph_forms_app` is built when at least one frontend is (`MORPH_BUILD_TUI` or
  `MORPH_BUILD_QT_QUICK`); the console demo needs neither."
- Section 2's sentence "(its options were resolved at emit time by executing `ListSamples`; the QML client fetches
  them live instead)" → "(its options were resolved at emit time by executing `ListSamples`; `morph_forms_app`
  fetches them live instead)".
- Replace section "## 3. QML renderer" and section "## 4. Views: the SamplesView list/master-detail screen" (up to
  "## Tests") with:

  ````markdown
  ## 3. The app: one binary, any frontend

  ```sh
  ./build/examples/forms/morph_forms_app              # Qt Quick if it can open a window, else the terminal UI
  ./build/examples/forms/morph_forms_app --ui=tui     # the terminal UI
  ./build/examples/forms/morph_forms_app --ui=qt      # Qt Quick
  ./build/examples/forms/morph_forms_app --typed      # the same screens, form screens built from Form<A>
  ```

  The frontend is `--ui=<name>`, else the `MORPH_UI` environment variable, else the first built frontend that can
  run here. Nothing below depends on which one it is: the app library (`app/`, target `forms_app`) links morph
  and nothing else.

  The app renders `LabApp` (`lab_wizard.hpp`) through `morph::forms::appShellView`: a menu beside the selected
  screen.

  | Menu | Screen kind | What it is |
  |---|---|---|
  | Density | form | `ComputeDryDensity` — two `Quantity` fields with unit alternatives |
  | Measure | form | `RecordMeasurement` — a `Choice` served by `ListSamples`, a date-time, sections and an accordion |
  | Intake | wizard | `IntakeWizard` — `RegisterSample`, then `RecordMeasurement` with `sampleId` prefilled from step one's reply (`Bind<"sampleId", "RegisterSample.id">`); Next is enabled once step one's submission succeeded, and entering step two re-fetches its sample options, so the new sample is among them |
  | Samples | view | `SamplesView` — the `ListSamples` table, New, a row's Delete behind a confirmation, and the `EditSample` editor a row opens prefilled with its id |

  **Two entrances, one engine.** By default the app is a runtime client: it takes the four documents a server
  would serve — the `app-*` document, the action schemas, the wizard and the view, all from `lab_schemas.hpp` —
  as JSON text and builds every screen from them: one `forms::AppShellSession` reads each screen's document
  through the lookups in `app/lab_documents.cpp`, which name no lab action type, and makes the screen's session
  the first time it is shown. `--typed` builds the two form screens from `forms::Form<ComputeDryDensity, LabModel>` and
  `forms::Form<RecordMeasurement, LabModel>` instead: the engine reads the same schema, so the form behaves the
  same and submits the same bytes, and the screen adds the reply decoded into its C++ type (`Density`,
  `MeasurementAck`), which a runtime client can only show as JSON. The engine has no typed facade for wizards or
  collections, so both entrances build those from the documents; the typed entrance draws the same menu and
  screens with a shell view of its own (`LabShell::typedScreen`).

  Every screen submits through `forms::handlerSubmitter` over one `BridgeHandler<LabModel>` (`app/lab_link.hpp`)
  — one model instance — so a sample the wizard registers is the one Measure's sample picker lists and the Samples table shows. Forms submit
  automatically when they become ready and on every edit that leaves them ready with a changed body; an action
  that wants a Submit button declares `explicitSubmit` (`docs/spec/forms/forms.md`, "Explicit submit mode").
  Leaving a screen and coming back finds it as it was: each screen's session lives as long as the app, so the
  wizard keeps its step and a half-filled form keeps its drafts.

  See `docs/spec/forms/workflows_navigation.md` for the `w-*`/`app-*` documents, `docs/spec/forms/views.md` for
  `v-*`, and `docs/spec/forms/engine.md` for how the engine reads them.
  ````

- "## Tests": replace the `forms_qml_logic` bullet with "- `forms_app_tests` (label `forms-demo`) — the app on
  `ui::testing::RecordingBackend` over an in-process `LabModel`: the shell's menu and screen switching, the wizard's
  gate and prefill, the collection's list, create, confirm and row editor, byte-identical bodies from both
  entrances, and the application on Part 6's `FakeAppContext`; and `forms_app_smoke_tests` (label `forms-demo`,
  built with the binary) — the application mounting and quitting on each built frontend."
- "## Notes", the `measuredAt` bullet: replace "Both clients provide a picker: the browser's native datetime-local
  control (with a *now* button) and a QML calendar/time popup." with "Both clients provide a picker: the browser's
  native datetime-local control (with a *now* button) and the app's date-time input, drawn by each frontend."

`README.md`, forms section: replace "[`examples/forms`](examples/forms) shows the whole loop with two renderers
driven purely by the generated schemas — a self-contained HTML page and a runtime-built Qt Quick GUI:" with
"[`examples/forms`](examples/forms) shows the whole loop with two clients driven purely by the generated schemas —
a self-contained HTML page and an app on the terminal UI or Qt Quick:"; in the code block below it replace the
first line with `cmake -B build -G Ninja -DMORPH_BUILD_TUI=ON   # add -DMORPH_BUILD_QT=ON -DMORPH_BUILD_QT_QUICK=ON for Qt Quick`
and the last line with `./build/examples/forms/morph_forms_app --typed     # same forms, on the terminal UI or Qt Quick`.
"## Examples": the `examples/forms` entry becomes "the schema-driven forms loop (an HTML page, and an app on the
terminal UI or Qt Quick) over an exact-value lab model."

`docs/ARCHITECTURE.md`: replace "`examples/forms` demonstrates the whole loop with two renderers: a self-contained
HTML page and a Qt Quick client (`MORPH_BUILD_FORMS_QML=ON`), both driven purely by the generated schemas." with
"`examples/forms` demonstrates the whole loop with two clients: a self-contained HTML page and `morph_forms_app` on
the terminal UI or Qt Quick, both driven purely by the generated schemas."; and "the QML client fetches them live
over the same in-process wire it submits on." with "`morph_forms_app` fetches them live over the same in-process
wire it submits on."

- [ ] **Step 4: Run the check to verify it passes**

```bash
/tmp/morph-doc-check.sh 'gui_qml|morph_forms_qml|AppShell\.qml|Main\.qml|forms_qml_logic|QML renderer' \
    examples/forms/README.md && echo clean
git grep -nE 'gui_qml|morph_forms_qml|QML client|Qt Quick GUI|QML renderers' -- README.md docs/ARCHITECTURE.md \
    || echo "no stale name"
pre-commit run --files examples/forms/README.md README.md docs/ARCHITECTURE.md
```

Expected: `clean`, `no stale name`; pre-commit's hooks pass (codespell, whitespace).

Mutation check: add the line ``See `examples/forms/gui_qml/qml/Main.qml`.`` to `examples/forms/README.md`; the check
exits 1 naming the missing path and the `gui_qml` hit. Remove it.

- [ ] **Step 5: Commit**

```bash
git add examples/forms/README.md README.md docs/ARCHITECTURE.md
git commit -m "wip(forms-demo): document morph_forms_app

Signed-off-by: Christian Parpart <christian@parpart.family>"
```

---

### Task 8: Group verification, then squash `forms-demo`

**Files:** none new; fixes land in the files they concern.

- [ ] **Step 1: Strict build and the suites**

```bash
cmake --build build/forms && ctest --test-dir build/forms --output-on-failure
cmake --build build/forms-nofe && ctest --test-dir build/forms-nofe --output-on-failure
```

Expected: every test passes on both trees, not only `forms-demo`.

- [ ] **Step 2: Sanitizers** (Linux; on macOS an ASan configure of `build/forms`)

```bash
cmake --preset clang-asan -DMORPH_BUILD_TUI=ON
cmake --build --preset clang-asan --target forms_app_tests forms_app_smoke_tests morph_tests
bash scripts/check_sanitizer_instrumentation.sh --binary build/clang-asan/examples/forms/forms_app_tests asan
./build/clang-asan/examples/forms/forms_app_tests "[forms-demo]"
./build/clang-asan/examples/forms/forms_app_smoke_tests
./build/clang-asan/tests/morph_tests "[forms-engine][wizard]"
```

Expected: clean. The shell and wizard tests destroy screens' views while sessions live on, and the wizard's
re-fetch on step entry replaces a query's in-flight request; ASan is their observer.

- [ ] **Step 3: clang-tidy over the changed lines** — CONTRIBUTING's "Running the `clang-tidy-diff` gate locally",
  configured with `-DMORPH_BUILD_TUI=ON -DMORPH_BUILD_QT=ON -DMORPH_BUILD_QT_QUICK=ON` added, diff base
  `origin/master...HEAD`, file count asserted non-zero.

Expected: no findings in `examples/forms/`.

- [ ] **Step 4: Commit any fixes**

```bash
git add -A examples/forms include/morph/forms tests
git commit -m "wip(forms-demo): fixes from the sanitizer and tidy gates

Signed-off-by: Christian Parpart <christian@parpart.family>"
```

Skip the commit if there was nothing to fix, and say so in the hand-off.

- [ ] **Step 5: Squash the group into its one commit**

Follow the master plan's "Squashing a part" procedure with `key=forms-demo` and this message (add a paragraph
before the trailer if Task 2 Step 0 found a Part 5 header that differs from Part 5's plan — what the plan said, what
the header has, what was done):

```text
examples/forms: one app, any frontend, runtime and typed forms

morph_forms_app replaces the QML demo: one binary that renders LabApp
through forms::appShellView on the terminal UI or Qt Quick, chosen at
run time. By default every screen is built from the served JSON
documents; --typed builds the form screens from forms::Form<A> and shows
the typed reply. SamplesView is now an app-shell view screen, and every
screen submits through forms::handlerSubmitter over one BridgeHandler,
so they share one LabModel. The console demo (--schemas, the REPL,
--emit-html) is unchanged.

Deviations: ui/main.cpp passes the --typed entrance to a second
makeApplication overload, the one line by which it differs from the
composition root every other example has.

Signed-off-by: Christian Parpart <christian@parpart.family>
```

Expected last line of the procedure: `git log --oneline master..HEAD` lists commits 1–17 of the master plan's
history, ending with `examples/forms: one app, any frontend, runtime and typed forms`.

---
## Group `retire` — `forms, examples: retire the QML renderer and the Qt client stack`

Every task in this group deletes or rewrites; none adds behaviour. Its "failing test" is a check that names what
is still there, written before the deletion and passing after it, and each check is mutated to prove it detects
what it claims to.

### Task 9: Remove the QML forms renderer and its Qt components

**Files:**
- Delete: `src/qt/forms/` (the `MorphForms` module: `qml/` with `DynamicForm.qml`, `CollectionView.qml`,
  `WizardView.qml`, `SlotRegistry.qml`, `DateTimePicker.qml`, `JsonExact.js`; `I18nCatalog.{hpp,cpp}`; `tests/`
  with the QuickTest suite, `test_forms_controller_core.cpp`, `test_multi_model_bridge_core.cpp`; `CMakeLists.txt`)
- Delete: `include/morph/qt/forms/` (`forms_controller_core.hpp`, `multi_model_forms_controller_core.hpp`),
  `include/morph/qt/bridge/` (`generic_model_bridge_core.hpp`, `multi_model_bridge_core.hpp`,
  `detail/owned_local_bridge.hpp`)
- Delete: `scripts/check_forms_qml_install.sh`
- Modify: `CMakeLists.txt` — the option; the Windows Qt detection; the public-header guard; the offline-sqlite
  comment; the "Qt/QML forms renderer setup" block; the "Application ladder" comment; the "Qt/QML forms renderer"
  block; the install components; the MorphForms install block; `configure_package_config_file`
- Modify: `cmake/morphConfig.cmake.in` — the `qt_forms` and `forms_qml` blocks
- Modify: `CMakePresets.json` — `windows-everything`, `linux-everything`
- Modify: `scripts/test_check_install_export.sh` — the `foreach` pattern of the vacuity-guard case
- Modify: `scripts/check_coverage_objects.sh` — the `morph_forms_qml_tests|morph_forms_controller_core_tests` arm
- Modify: `.github/workflows/ci.yml`, `.github/workflows/nightly-slow-checks.yml`, `.github/workflows/wasm-ladder.yml`
- Modify: `README.md` — prerequisites, build options, install components; `CONTRIBUTING.md` — the options list
- Modify: `CHANGELOG.md` — `[Unreleased]` → `### Removed`

**Interfaces:**
- Consumes: nothing new.
- Produces: an install whose components are `offline_sqlite`, `qt`, `net`, `tui`, `qt_quick` (as configured);
  no `MORPH_BUILD_FORMS_QML`.

- [ ] **Step 1: Write the failing check**

Every removed path is gone, no build or CI file names the renderer, and a fresh configure that passes the old
option reports it unused (CMake's own proof that nothing reads it):

```bash
cat > /tmp/morph-retire-renderer.sh <<'SH'
#!/usr/bin/env bash
set -u
status=0
for path in src/qt/forms include/morph/qt/forms include/morph/qt/bridge scripts/check_forms_qml_install.sh; do
    [ -e "$path" ] && { echo "still present: $path"; status=1; }
done
if git grep -nE 'FORMS_QML|qt_forms|forms_qml|MorphForms|morph_forms_module|check_forms_qml_install|morph_QML_IMPORT_PATH|qt/bridge/|qt/forms/' \
       -- CMakeLists.txt cmake CMakePresets.json .github scripts README.md CONTRIBUTING.md \
          ':!cmake/compiler_options.cmake'; then
    status=1
fi
rm -rf build/retire-check
cmake -S . -B build/retire-check -G Ninja -DMORPH_BUILD_EXAMPLES=OFF -DMORPH_BUILD_TESTS=OFF \
      -DMORPH_BUILD_FORMS_QML=ON > build/retire-check.log 2>&1
grep -A3 'Manually-specified variables were not used by the project' build/retire-check.log \
    | grep -q MORPH_BUILD_FORMS_QML || { echo "MORPH_BUILD_FORMS_QML is still read by the configure"; status=1; }
exit $status
SH
chmod +x /tmp/morph-retire-renderer.sh
```

(`cmake/compiler_options.cmake` is excluded: its one `FORMS_QML` is the recorded configuration of a past
measurement, which Task 14 lists as an allowed hit.)

Pre-condition — nothing outside the files this task deletes or edits still uses the renderer's C++ side:

```bash
git grep -lE 'morph/qt/forms/|morph/qt/bridge/|FormsControllerCore|GenericModelBridgeCore|MultiModelBridgeCore' \
    -- examples include src tests ':!include/morph/qt/forms' ':!include/morph/qt/bridge' ':!src/qt/forms'
```

Expected: no output. A hit under `examples/<app>/` is code Parts 7–9 should have removed with that app's QML:
stop and report it — it belongs in that app's commit (master plan, "Every commit builds").

- [ ] **Step 2: Run it to verify it fails**

```bash
/tmp/morph-retire-renderer.sh; echo "exit $?"
```

Expected: `exit 1`, listing the four paths as still present, the `MORPH_BUILD_FORMS_QML`/`qt_forms`/`forms_qml`
lines of `CMakeLists.txt`, `cmake/morphConfig.cmake.in`, `CMakePresets.json`, the workflows, the scripts and
`README.md`, and "MORPH_BUILD_FORMS_QML is still read by the configure".

- [ ] **Step 3: Implement**

Delete the sources:

```bash
git rm -r src/qt/forms include/morph/qt/forms include/morph/qt/bridge scripts/check_forms_qml_install.sh
```

`CMakeLists.txt`:

1. Delete the line `option(MORPH_BUILD_FORMS_QML     "Build the shipped Qt/QML forms renderer module (MorphForms) and its demo" OFF)`.
2. Replace the comment `# Both Qt consumers (the WebSocket backend and the forms QML renderer) need` /
   `# Qt6 auto-detected on Windows before their find_package(Qt6 ...) calls run.` and the condition below it with:

   ```cmake
   # Every Qt consumer (the WebSocket backend, the Qt Quick frontend, which requires it) needs Qt6
   # auto-detected on Windows before its find_package(Qt6 ...) call runs.
   if(WIN32 AND MORPH_BUILD_QT)
   ```

3. Public-header guard: delete the list entry `"include/morph/qt/forms/:morph_qt_forms"`; delete `morph_qt_forms`
   from the `foreach(_morph_target IN ITEMS …)` list; in the comment above, delete `morph::qt_forms, ` from
   "(morph::net, morph::qt, morph::qt_forms, …)".
4. The offline-sqlite block's comment: delete " -- see the identical note on morph::qt_forms vs the plain
   morph_forms_moduleplugin just below" (the sentence ends at "the point it is named).").
5. Delete the whole "Qt/QML forms renderer setup (optional)" block: its comment and the `if(MORPH_BUILD_FORMS_QML)`
   … `endif()` that creates `morph_qt_forms`.
6. "Application ladder (optional)" comment: replace "Deferred to here (after the Tests section above), the same way
   MORPH_BUILD_FORMS_QML's src/qt/forms subdirectory is deferred further below:" with "Deferred to here (after the
   Tests section above):".
7. Delete the whole "Qt/QML forms renderer (optional)" block (`add_subdirectory(src/qt/forms)`).
8. Install: delete the word `qt_forms` from `foreach(_morph_component IN ITEMS offline_sqlite qt_forms qt net)`.
   Parts 3 and 4 left that line byte-identical and gave `tui` and `qt_quick` install blocks of their own, so the
   result is `foreach(_morph_component IN ITEMS offline_sqlite qt net)` — the line the
   `scripts/test_check_install_export.sh` edit below matches byte for byte.
9. Delete the MorphForms install block: from its comment `# The MorphForms QML module (MORPH_BUILD_FORMS_QML): …`
   through the `endif()` closing `if(TARGET morph_forms_module)`, the `set(MORPH_INSTALL_QMLDIR …)` cache variable
   included. Then `git grep -n MORPH_INSTALL_QMLDIR` prints only `PATH_VARS MORPH_INSTALL_QMLDIR` in
   `configure_package_config_file(…)` — Part 4 installs its private `MorphUi` module inside its own `qt_quick` block
   and names no such variable — so delete that line too. Any other hit is a consumer no plan declared: stop and
   report it.

`cmake/morphConfig.cmake.in`: delete

```cmake
if("qt_forms" IN_LIST morph_KNOWN_COMPONENTS)
    find_dependency(Qt6 6.5 COMPONENTS Core)
endif()
```

and the comment block beginning `# The MorphForms QML module: morph::forms_qml (backing library) and` through the
`endif()` of `if("forms_qml" IN_LIST morph_KNOWN_COMPONENTS)`.

`CMakePresets.json`: in `windows-everything` and `linux-everything`, delete `"MORPH_BUILD_FORMS_QML": "ON",` (Parts 3
and 4 added `"MORPH_BUILD_TUI": "ON"` and `"MORPH_BUILD_QT_QUICK": "ON"` to both). Validate:
`python3 -m json.tool CMakePresets.json > /dev/null`.

`scripts/test_check_install_export.sh`, the case "no optional component installed, leaving the export-name check
with nothing to read": change the sed expression's left-hand side to the `foreach(_morph_component IN ITEMS …)`
line from item 8, exactly:

```bash
    "edit CMakeLists.txt -e 's@foreach(_morph_component IN ITEMS offline_sqlite qt net)@foreach(_morph_component IN ITEMS \"\")@'" \
```

`scripts/check_coverage_objects.sh`: delete the three-line arm

```bash
        morph_forms_qml_tests|morph_forms_controller_core_tests)
            echo "GAP: built by -DMORPH_BUILD_FORMS_QML=ON, …"
            ;;
```

`.github/workflows/ci.yml`:

- `env:` — replace `# MORPH_BUILD_FORMS_QML needs Qt 6.5+; ubuntu-24.04 apt still ships 6.4.2.` with
  `# examples/common and the Qt Quick frontend need Qt 6.5+; ubuntu-24.04 apt still ships 6.4.2.`
- The two "Install Qt" comments in the ladder jobs (the one beginning "Not the distro's Qt: examples/common/
  CMakeLists.txt requires 6.5+ unconditionally (not gated on MORPH_BUILD_FORMS_QML)" and the one beginning
  "(QQmlApplicationEngine::loadFromModule, used by MORPH_BUILD_FORMS_QML rungs)"): replace each with

  ```yaml
      # Not the distro's Qt: examples/common/CMakeLists.txt requires Qt 6.5+
      # (qt_standard_project_setup(REQUIRES 6.5)) and Ubuntu 24.04 still ships
      # 6.4.2 -- the gap the "all optional features" job's identical step
      # documents.
  ```

  keeping each step's own remaining sentences that do not mention `MORPH_BUILD_FORMS_QML`.
- `linux-all-features`: the "Install Qt" comment becomes `# Not the distro's Qt: the ladder and the Qt Quick
  frontend need 6.5+ and Ubuntu 24.04 still ships 6.4.2.`; in "Configure (every optional feature ON)" delete
  `-DMORPH_BUILD_FORMS_QML=ON \` and replace the comment paragraph beginning "MORPH_BUILD_LADDER belongs in this job
  by its own charter" through "…and the smoke test runs, on every push." with

  ```yaml
          # MORPH_BUILD_LADDER belongs in this job by its own charter ("every
          # MORPH_BUILD_* option … enabling them together also proves they
          # compose"): every rung's binary and its frontend smoke tests are
          # built here against both frontends.
  ```

- `clang-tidy`: delete `-DMORPH_BUILD_FORMS_QML=ON \` from "Configure (generates compile_commands.json …)".
- `clang-tidy`, the step "Generate the AUTOMOC headers two sources include by name": both sources it names
  (`examples/common/testkit/test_qml_surface.cpp`, removed in Task 11, and `src/qt/forms/tests/tst_main.cpp`) are
  gone after this group. Run

  ```bash
  git grep -lE '^[[:space:]]*#[[:space:]]*include[[:space:]]+"[^"]+\.moc"' -- '*.cpp' '*.cc' '*.cxx' \
      ':!examples/common/testkit/test_qml_surface.cpp' ':!src/qt/forms'
  ```

  If it prints nothing, delete the step together with its preceding comment block (from "# Two sources end with
  `#include \"<own-basename>.moc\"`" to the end of its `run:`), and in "Run clang-tidy-diff on changed lines"'s
  comment delete the paragraph beginning "It is not sufficient for a generated header that a source includes
  *by name*." through "…why this job builds two targets.". If it prints files, keep the step: replace its target
  list with each printed source's `<target>_autogen` target, and its comment's two file names with the printed
  ones.
- Delete the whole `install-export-forms-qml:` job.

`.github/workflows/nightly-slow-checks.yml`: in the header comment replace "including MORPH_BUILD_LADDER +
MORPH_BUILD_FORMS_QML" with "including MORPH_BUILD_LADDER with both frontends"; delete `-DMORPH_BUILD_FORMS_QML=ON \`.

`.github/workflows/wasm-ladder.yml`: delete `-DMORPH_BUILD_FORMS_QML=ON \` from "Configure"; in the comment above
the build loop, delete the clause listing reasons a target is skipped if it still names
`MORPH_BUILD_FORMS_QML off` (the remaining reasons stand).

`README.md`: the prerequisites line "Optional: Qt 6 for the WebSocket transport and QML example." → "Optional:
Qt 6 for the WebSocket transport and the Qt Quick frontend."; the build-options list ("Relevant CMake options: …",
where Part 3 added `` `MORPH_BUILD_TUI`, `` after `` `MORPH_BUILD_QT`, ``): replace `` `MORPH_BUILD_FORMS_QML`, ``
with `` `MORPH_BUILD_QT_QUICK`, ``; the components sentence (Part 3 added `tui` after `net`, Part 4 `qt_quick` after
`qt`): replace "`qt_quick` (`MORPH_BUILD_QT_QUICK`, which needs `qt`), and `qt_forms` and `forms_qml` (both
`MORPH_BUILD_FORMS_QML`)" with "and `qt_quick` (`MORPH_BUILD_QT_QUICK`, which needs `qt`)" — and delete the paragraph
beginning "`forms_qml` is the `MorphForms` QML module", its `cmake` block, and the paragraph after it ending "builds
and runs such an application against an install."

`CONTRIBUTING.md`: in the options list "(`MORPH_BUILD_TESTS`, `MORPH_BUILD_EXAMPLES`, `MORPH_BUILD_QT`,
`MORPH_BUILD_FORMS_QML`, …)" replace `` `MORPH_BUILD_FORMS_QML` `` with `` `MORPH_BUILD_TUI`, `MORPH_BUILD_QT_QUICK` ``.

`CHANGELOG.md`, under `## [Unreleased]` → `### Removed`, as the first entry:

```markdown
- **The Qt/QML forms renderer and the Qt components behind it.** `src/qt/forms` — the `MorphForms` QML
  module (`DynamicForm`, `CollectionView`, `WizardView`, `SlotRegistry`, `DateTimePicker`, `JsonExact.js`)
  and `I18nCatalog` — with its QuickTest suite; `include/morph/qt/forms/` (`FormsControllerCore`,
  `MultiModelFormsControllerCore`) and `include/morph/qt/bridge/` (`GenericModelBridgeCore`,
  `MultiModelBridgeCore`); the `qt_forms`, `forms_qml` and `forms_qmlplugin` install components, their
  package-config blocks and `morph_QML_IMPORT_PATH`; and the `MORPH_BUILD_FORMS_QML` option. Forms render
  through the C++ forms engine instead — `forms::formView`, `collectionView`, `wizardView` and `appShellView`
  over `FormSession` and its siblings (`include/morph/forms/engine/`, in the base `morph` target) — on any
  `morph::ui` frontend, `morph::tui` or `morph::qt_quick`. `forms::Overrides` replaces `SlotRegistry`; a
  `render::TranslationProvider` in `FormSessionOptions` replaces `I18nCatalog`; `forms::bridgeSubmitter` and
  `forms::bridgeChoiceFetcher` replace the controller cores. `morph::qt` (the WebSocket transport and
  `QtExecutor`) is unchanged. Pre-1.0, per `docs/spec/VERSIONING.md`.
```

- [ ] **Step 4: Run the checks to verify they pass**

```bash
/tmp/morph-retire-renderer.sh && echo "renderer retired"
cmake -S . -B build/all && cmake --build build/all && QT_QPA_PLATFORM=offscreen ctest --test-dir build/all --output-on-failure
cmake -S . -B build/qt -G Ninja -DCMAKE_BUILD_TYPE=Debug -DMORPH_BUILD_EXAMPLES=OFF -DMORPH_BUILD_QT=ON \
      -DMORPH_BUILD_QT_QUICK=ON
cmake --build build/qt && QT_QPA_PLATFORM=offscreen ctest --test-dir build/qt --output-on-failure
bash scripts/test_check_install_export.sh
bash scripts/check_install_export.sh
```

Expected: `renderer retired`; both trees build and every test passes (`build/qt` proves `morph::qt` keeps
`qt_executor.hpp` in its own header set, verified standalone by `VERIFY_INTERFACE_HEADER_SETS`); the install
checker's self-test ends "detects every install/export defect it claims to", and the checker passes.

Mutation checks:
- Re-add `-DMORPH_BUILD_FORMS_QML=ON \` to `nightly-slow-checks.yml`'s configure: `/tmp/morph-retire-renderer.sh`
  exits 1 naming that line. Restore.
- Restore the old `foreach(_morph_component IN ITEMS offline_sqlite qt_forms qt net)` text in
  `scripts/test_check_install_export.sh`'s sed expression: `bash scripts/test_check_install_export.sh` fails on
  "no optional component installed, leaving the export-name check with nothing to read" (the edit matches nothing,
  so the defect it injects is never injected). Restore.

- [ ] **Step 5: Commit**

```bash
git add -A src/qt include/morph/qt scripts CMakeLists.txt cmake/morphConfig.cmake.in CMakePresets.json \
        .github/workflows README.md CONTRIBUTING.md CHANGELOG.md
git commit -m "wip(retire): remove the QML forms renderer and its Qt components

Signed-off-by: Christian Parpart <christian@parpart.family>"
```

---

### Task 10: `morph_add_rung` loses the old client conventions

**Files:**
- Modify: `cmake/morph_add_rung.cmake` — the header's directory table; the `EMSCRIPTEN` guard; delete the
  `gui_lib/`, `gui/qml/`, `gui/` and `gui_wasm/` blocks; the tests block's `gui_lib` link and `_qml_plugin` block;
  delete the `src/headless/` block; add the refusal of a leftover old client directory
- Modify: `.github/workflows/wasm-ladder.yml` — the build loop's `gui_wasm` fallback and the comment naming it

**Interfaces:**
- Consumes: Part 6's `app/` → `ladder_<rung>_app`, `ui/*.cpp` → `<rung>` and `tests/smoke/*.cpp` →
  `ladder_<rung>_smoke_tests` blocks (unchanged), its Qt-free target `morph_ladder_app_common`; Part 8's
  `src/server/app/` → `ladder_<rung>_server_app` block (unchanged); Part 9's kanban, which builds
  `ladder_kanban_headless` from `examples/kanban/headless/main.cpp` in its own `CMakeLists.txt`.
- Produces: `morph_add_rung()` creates `ladder_<rung>_{lib,app,server_app,server,tests,smoke_tests}` and `<rung>`;
  a rung with a top-level `gui/`, `gui_lib/` or `gui_wasm/` directory fails the configure.

- [ ] **Step 1: Write the failing check**

A stray old-convention directory must fail the configure loudly, and no old target may be generated:

```bash
cat > /tmp/morph-retire-rung.sh <<'SH'
#!/usr/bin/env bash
set -u
status=0
mkdir -p examples/pastebin/gui_lib
printf '// SPDX-License-Identifier: Apache-2.0\n' > examples/pastebin/gui_lib/stray.cpp
cmake -S . -B build/all > build/retire-rung.log 2>&1
grep -q "examples/pastebin/gui_lib/ is not a ladder directory" build/retire-rung.log \
    || { echo "a stray gui_lib/ was not refused"; status=1; }
rm -rf examples/pastebin/gui_lib
cmake -S . -B build/all > build/retire-rung.log 2>&1 || { echo "configure failed without the stray"; status=1; }
if cmake --build build/all --target help | grep -E '_gui_lib|_qml\b|_qmlplugin|_gui_wasm|ladder_[a-z]+_gui:'; then
    status=1
fi
if git grep -nE 'gui_lib|_qml_plugin|_qml_uri|MORPH_LADDER_QML_URI|MORPH_LADDER_TESTKIT_GUI_APP|morph::ladder_app([^_]|$)|morph_ladder_app([^_]|$)|morph::ladder_gui|morph_qt_forms|FORMS_QML|src/headless' \
       -- cmake/morph_add_rung.cmake; then
    status=1
fi
if git grep -nE 'gui_wasm|gui_lib' -- .github/workflows/wasm-ladder.yml; then
    status=1
fi
exit $status
SH
chmod +x /tmp/morph-retire-rung.sh
```

- [ ] **Step 2: Run it to verify it fails**

```bash
/tmp/morph-retire-rung.sh; echo "exit $?"
```

Expected: `exit 1` — "a stray gui_lib/ was not refused" (the configure creates `ladder_pastebin_gui_lib` instead),
the first `git grep` lists the `gui_lib`, `_qml_plugin`, `MORPH_LADDER_QML_URI`, `MORPH_LADDER_TESTKIT_GUI_APP`,
`morph::ladder_app` and `src/headless` lines of `cmake/morph_add_rung.cmake`, and the second the `gui_wasm` branch
Part 8 left in `wasm-ladder.yml`'s build loop.

- [ ] **Step 3: Implement**

In `cmake/morph_add_rung.cmake`:

1. Header comment, "Directory -> target convention:" table: delete the rows for `gui_lib/*.cpp`, `gui/qml/*.qml`,
   `gui/*.cpp`, `gui_wasm/*.cpp` and `src/headless/*.cpp`. The rows Part 6 added (`app/*.cpp`, `ui/*.cpp`,
   `tests/smoke/*.cpp`) and Part 8 added (`src/server/app/*.cpp + src/server/include/`) stay as they are; in Part 8's
   note below its row, delete "(the declarative client layout)" — every rung's client is that layout now.
2. The `if(EMSCRIPTEN)` guard at the top of the function: replace `morph_ladder_app` with `morph_ladder_app_common`
   in both the `if(NOT TARGET …)` condition and its message.
3. Delete the `gui_lib/*.cpp -> ladder_<rung>_gui_lib` block (from its leading comment through the `endif()` closing
   `if(_gui_lib_sources)`), the `gui/qml/*.qml` block (`set(_qml_plugin "")` through the `endif()` closing
   `if(_qml_files AND TARGET morph_qt_forms)`, with the comment above it), the desktop `gui/*.cpp` block (the
   `if(NOT EMSCRIPTEN)` holding `_gui_sources`) and the `gui_wasm/*.cpp` block (the `if(EMSCRIPTEN)` holding
   `_gui_wasm_sources`), each with its leading comment.
4. Tests block: delete

   ```cmake
               if(TARGET ladder_${_rung}_gui_lib)
                   target_link_libraries(ladder_${_rung}_tests PRIVATE morph::ladder_${_rung}_gui_lib)
               endif()
   ```

   and the `if(_qml_plugin)` … `endif()` block (with its comment) that links the QML plugin and defines
   `MORPH_LADDER_QML_URI` and `MORPH_LADDER_TESTKIT_GUI_APP`.
5. Delete the `# ── ladder_<rung>_headless: QProcess test-client binary (rung 4+) ────` block (its `file(GLOB_RECURSE
   _headless_sources … "${_dir}/src/headless/*.cpp")` through the `endif()` closing
   `if(NOT EMSCRIPTEN AND _headless_sources AND TARGET ladder_${_rung}_gui_lib)`). Its condition can no longer hold
   (no `gui_lib` target exists), and kanban, the one rung with a headless client, builds `ladder_kanban_headless`
   from `examples/kanban/headless/main.cpp` in its own `CMakeLists.txt` (Part 9). Confirm first:
   `ls -d examples/*/src/headless` prints nothing.
6. After the `set(_rung "${RUNG_NAME}")` line, add:

   ```cmake
       # A rung's client is app/ (the toolkit-free app library, ladder_<rung>_app) and ui/main.cpp (the binary,
       # <rung>). A top-level directory under another client name holds code no target builds, so it is refused
       # here rather than skipped in silence.
       foreach(_old_client_dir IN ITEMS gui gui_lib gui_wasm)
           if(IS_DIRECTORY "${_dir}/${_old_client_dir}")
               message(FATAL_ERROR
                   "morph_add_rung(${_rung}): examples/${_rung}/${_old_client_dir}/ is not a ladder directory, so "
                   "nothing would build it. Client code lives in examples/${_rung}/app/ (the toolkit-free app "
                   "library, ladder_${_rung}_app) and examples/${_rung}/ui/main.cpp (the binary, ${_rung}); see "
                   "examples/TESTING.md, \"App architecture\".")
           endif()
       endforeach()
       unset(_old_client_dir)
   ```

`.github/workflows/wasm-ladder.yml`, the step "Build the WASM-remote spike and every rung's WASM client": every rung
has `ui/main.cpp` now, so replace the loop body Part 8 wrote with

```bash
            if [ -f "examples/$rung/ui/main.cpp" ]; then
              cmake --build build-wasm-ladder --target "$rung"
              built=$((built + 1))
            else
              echo "::notice::examples/$rung has no ui/main.cpp -- no browser client to build (see wasm-ladder.yml's comment)"
            fi
```

and in the comment above the loop replace each mention of a rung's `gui_wasm` target or directory with its
`ui/main.cpp` binary, keeping every other reason a target is skipped.

- [ ] **Step 4: Run the check to verify it passes**

```bash
/tmp/morph-retire-rung.sh && echo "conventions retired"
cmake --build build/all && QT_QPA_PLATFORM=offscreen ctest --test-dir build/all -L ladder --output-on-failure
```

Expected: `conventions retired`; every ladder test passes, including every `<rung>.smoke.` case and kanban's
process-separation tests, which spawn `ladder_kanban_headless` (built by kanban's own `CMakeLists.txt`).

Mutation check: remove `gui_lib` from the `foreach(_old_client_dir IN ITEMS …)` list; `/tmp/morph-retire-rung.sh`
exits 1 with "a stray gui_lib/ was not refused". Restore.

- [ ] **Step 5: Commit**

```bash
git add cmake/morph_add_rung.cmake .github/workflows/wasm-ladder.yml
git commit -m "wip(retire): morph_add_rung loses the gui/, gui_lib/, gui/qml/ and gui_wasm/ conventions

Signed-off-by: Christian Parpart <christian@parpart.family>"
```

---

### Task 11: Remove `examples/common`'s Qt client stack

**Files:**
- Delete: `examples/common/gui/` (`presenter.{hpp,cpp}`, `event_poller.{hpp,cpp}`, `error_text.{hpp,cpp}`,
  `id_qml.hpp`, `app_context.{hpp,cpp}`)
- Delete: `examples/common/testkit/qml_surface.hpp`, `examples/common/testkit_src/qml_surface.cpp`
- Delete: `examples/common/testkit/test_presenter.cpp`, `test_error_text.cpp`, `test_id_qml.cpp`,
  `test_event_poller.cpp`, `test_qml_surface.cpp`
- Modify: `examples/common/CMakeLists.txt` — header comment; the `MORPH_BUILD_QT` refusal's message; delete
  `morph_ladder_gui` and `morph_ladder_app`; the WebAssembly comment; `morph_ladder_testkit`'s sources, comment and
  links; `ladder_common_tests`' sources
- Modify: `examples/common/testkit/testkit_main.cpp` — the `QGuiApplication` branch
- Modify: `examples/common/testkit/pump.hpp` — delete `settle`
- Modify: `examples/common/testkit/client_pool.hpp`, `test_client_pool.cpp`, `backend_rig.hpp`,
  `test_backend_rig.cpp`, `journey.hpp` — "presenter" becomes "client"/"controller"
- Modify: `examples/common/testkit/.clang-tidy` — the comment naming `qml_surface.cpp`

**Interfaces:**
- Consumes: Part 6's `examples/common/app/` (`morph_ladder_app_common`, `examples_common_app_tests`) and
  `morph_example_testkit` (`testkit/wait.hpp`, `testkit/fake_app_context.hpp`, `testkit/frontend_smoke.hpp`,
  `testkit_src/frontend_smoke.cpp`), unchanged.
- Produces: `examples/common` targets `morph_ladder_app_common`, `morph_example_testkit`, `morph_ladder_testkit`,
  `examples_common_app_tests`, `ladder_common_tests` (plus the WASM spike under Emscripten);
  `morph::ladder::testkit::ClientPool<Client>`.

- [ ] **Step 1: Write the failing check**

```bash
cat > /tmp/morph-retire-common.sh <<'SH'
#!/usr/bin/env bash
set -u
status=0
for path in examples/common/gui examples/common/testkit/qml_surface.hpp examples/common/testkit_src/qml_surface.cpp \
            examples/common/testkit/test_presenter.cpp examples/common/testkit/test_error_text.cpp \
            examples/common/testkit/test_id_qml.cpp examples/common/testkit/test_event_poller.cpp \
            examples/common/testkit/test_qml_surface.cpp; do
    [ -e "$path" ] && { echo "still present: $path"; status=1; }
done
if git grep -niE 'presenter|qml_surface|QmlSurface|EventPoller|event_poller|id_qml|error_text|(^|[^_])app_context|AppContext::Mode|morph_ladder_gui|ladder_gui|morph_ladder_app([^_]|$)|ladder_app([^_]|$)|MORPH_LADDER_TESTKIT_GUI_APP' \
       -- examples/common; then
    status=1
fi
exit $status
SH
chmod +x /tmp/morph-retire-common.sh
```

Pre-condition — no rung or app still uses the stack:

```bash
git grep -lE 'gui/(presenter|event_poller|error_text|id_qml|app_context)\.hpp|testkit/qml_surface\.hpp|morph::ladder_gui|morph::ladder_app([^_]|$)|ClientPool<.*Presenter' \
    -- examples ':!examples/common'
```

Expected: no output; a hit is a Part 7–9 leftover — stop and report it.

- [ ] **Step 2: Run it to verify it fails**

```bash
/tmp/morph-retire-common.sh; echo "exit $?"
```

Expected: `exit 1`, listing the eight paths and the matching lines of `examples/common/CMakeLists.txt`,
`testkit_main.cpp`, `pump.hpp`, `client_pool.hpp`, `backend_rig.hpp`, `journey.hpp`, `test_client_pool.cpp`,
`test_backend_rig.cpp` and `testkit/.clang-tidy`.

- [ ] **Step 3: Implement**

```bash
git rm -r examples/common/gui examples/common/testkit/qml_surface.hpp examples/common/testkit_src/qml_surface.cpp \
          examples/common/testkit/test_presenter.cpp examples/common/testkit/test_error_text.cpp \
          examples/common/testkit/test_id_qml.cpp examples/common/testkit/test_event_poller.cpp \
          examples/common/testkit/test_qml_surface.cpp
```

`examples/common/CMakeLists.txt`:

1. Header: `# Shared ladder infrastructure: the presenter architecture (gui/) and the` / `# testkit (testkit/). See
   examples/TESTING.md.` → `# Shared ladder infrastructure: what every rung's client shares (app/, target` /
   `# morph_ladder_app_common) and the testkit (testkit/). See examples/TESTING.md.`
2. The `if(NOT MORPH_BUILD_QT)` refusal: replace its comment with

   ```cmake
   # MORPH_BUILD_QT is required in every configure: the rungs' servers are Qt (QCoreApplication +
   # QtWebSocketServer), the testkit's BackendRig Socket mode and the fault-injection proxy are Qt, and a
   # rung's binary uses QtWebSocketBackend for --server on Qt Quick (and in the browser, where it is the only
   # transport).
   ```

   and its message with `"MORPH_BUILD_LADDER requires MORPH_BUILD_QT=ON: the ladder's servers, the testkit's
   BackendRig Socket mode and the fault-injection proxy all need morph::qt (Qt6::WebSockets)."`
3. Delete the `# ── morph_ladder_gui: …` block and the `# ── morph_ladder_app: AppContext …` block, comments
   included.
4. The `# ── WebAssembly build ──` comment: replace its first paragraph with

   ```cmake
   # Everything above this line builds under Emscripten and is what a rung's browser binary needs: the shared
   # app layer (morph_ladder_app_common), whose remote transport there is QtWebSocketBackend. Everything below
   # does not: morph_ladder_testkit and ladder_common_tests need Catch2 (MORPH_BUILD_TESTS is never part of a
   # WASM configure) and the Lightweight ORM speaks ODBC, which does not exist in a browser
   # (examples/IMPLEMENTATION.md rule 4's WASM clause).
   ```

   and delete its second paragraph ("Rung 0 returned *before* the two targets above as well, …").
5. `morph_ladder_testkit`: delete `testkit_src/qml_surface.cpp` and `testkit/qml_surface.hpp` from its sources;
   in `target_link_libraries(morph_ladder_testkit PUBLIC …)` delete `morph::ladder_gui morph::ladder_app` and, if
   not already there, add `morph::ladder_app_common`. In the comment above it: "the library already links non-empty
   TUs (fault_proxy.cpp, qml_surface.cpp)" → "the library already links a non-empty TU (fault_proxy.cpp)"; delete
   the sentence beginning "qml_surface.cpp is a .cpp for the opposite reason"; "Those two TUs live in
   testkit_src/" → "That TU lives in testkit_src/"; "Placed in testkit/ these two would inherit" → "Placed in
   testkit/ it would inherit"; "Both headers are listed rather than only the one carrying a Q_OBJECT today, so the
   move is moc-neutral by construction instead of by which header happens to need moc." → "The header is listed so
   the move stays moc-neutral by construction."; "Which is why the two headers are listed as sources below" →
   "Which is why the header is listed as a source below".
6. `ladder_common_tests`: delete the source lines `testkit/test_presenter.cpp`, `testkit/test_error_text.cpp`,
   `testkit/test_id_qml.cpp`, `testkit/test_event_poller.cpp` and `testkit/test_qml_surface.cpp`.

`examples/common/testkit/testkit_main.cpp`, whole file:

```cpp
// SPDX-License-Identifier: Apache-2.0
//
// Qt-owning Catch2 main for the ladder's test binaries. The rig's Socket mode, the fault proxy and the rungs'
// servers are QObjects, so a QCoreApplication must outlive every QObject Catch2 constructs during the run and be
// destroyed before static teardown, or Qt's cleanup runs against a torn-down application. Frontend smoke tests
// live in binaries of their own whose main owns no Qt application object (ladder_<rung>_smoke_tests, over
// morph_test_main), because the Qt Quick frontend constructs its own; so this main never needs a GUI application.

#include <QCoreApplication>
#include <QEvent>
#include <catch2/catch_session.hpp>
#include <testkit/log_level.hpp>

int main(int argc, char* argv[]) {
    QCoreApplication app{argc, argv};
    Catch::Session session;
    int const result = morph::testkit::runSession(session, argc, argv);
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    QCoreApplication::processEvents(QEventLoop::AllEvents);
    return result;
}
```

Before writing it, confirm the claim in its last sentence: `git grep -ln 'runFrontendSmoke' -- examples` lists only
files under a `tests/smoke/` directory (compiled into `ladder_<rung>_smoke_tests`, `bank_smoke_tests` or
`forms_app_smoke_tests`) and `examples/common/app/tests/test_frontend_smoke.cpp` (`examples_common_app_tests`, also
over `morph_test_main`). A hit compiled into a `ladder_<rung>_tests` binary is a smoke case running under this Qt
`main`, which Part 6's helper refuses: stop and report it — it belongs in that rung's commit.

`examples/common/testkit/pump.hpp`: run `git grep -n 'testkit::settle' examples`; with no caller left, delete the
`settle` template and its doc comment (the block beginning `/// @brief \`pumpUntil(!presenter.busy())\``).

`examples/common/testkit/client_pool.hpp`: rename the template parameter `Presenter` → `Client` and the member
`_presenters` → `_clients`; the class comment becomes "`ClientPool<Client>` -- N client objects (a rung's
controller set) over one `BackendRig`'s N clients, each constructed from that client's `(Bridge&, IExecutor*)`
pair."; the constructor's `@brief` "Constructs one `Client` per client in @p rig, forwarding each client's
`(Bridge&, IExecutor*)` pair to its constructor."; `@param rig` "…to build the clients over"; `@param nClients`
"How many clients to construct -- …"; `at`'s `@return` "The client object for client @p index."; `size`'s
"How many clients this pool holds.". `examples/common/testkit/test_client_pool.cpp`: `FakePresenter` →
`FakeClient`, its comment "Minimal stand-in for a rung's controller set: takes the `(Bridge&, IExecutor*)` pair
`ClientPool` forwards, and builds its own `BridgeHandler` over it. Exercises `ClientPool<Client>` without depending
on any rung's concrete type, which the shared testkit must not"; the test name "ClientPool constructs one
presenter per client, each over its own bridge" → "ClientPool constructs one client object per client, each over
its own bridge"; the comments' "presenter(s)" → "client object(s)".

`examples/common/testkit/backend_rig.hpp`: "the test's own presenters/handlers go first" → "the test's own
controllers/handlers go first"; "any Presenter built over this rig" → "any controller built over this rig";
"a `Presenter` subclass" → "a controller"; "presenter tests need the raw bridge" → "controller tests need the raw
bridge"; "The second half of a presenter's `(Bridge&, IExecutor*)` pair" → "The second half of a controller's
`(Bridge&, IExecutor*)` pair". `test_backend_rig.cpp`: the test name "BackendRig exposes bridge/executor/url so
presenters compose over it" → "…so controllers compose over it"; its comment "The pair a Presenter subclass is
constructed from" → "The pair a controller is constructed from", "which a presenter that builds its own" → "which a
controller that builds its own". `journey.hpp`: "a presenter test proves one call wires through" → "a controller
test proves one call wires through".

`examples/common/testkit/.clang-tidy`: replace the sentence "The two translation units of `morph_ladder_testkit` --
`fault_proxy.cpp` and `qml_surface.cpp`, neither containing a single REQUIRE -- are therefore kept in
`examples/common/testkit_src/`, one directory up and outside this file's reach." with "The library translation units
-- `morph_ladder_testkit`'s `fault_proxy.cpp` and `morph_example_testkit`'s `frontend_smoke.cpp` -- are therefore
kept in `examples/common/testkit_src/`, one directory up and outside this file's reach." (if Part 6 already named
`frontend_smoke.cpp` there, only the `qml_surface.cpp` clause goes).

- [ ] **Step 4: Run the checks to verify they pass**

```bash
/tmp/morph-retire-common.sh && echo "client stack retired"
cmake -S . -B build/all && cmake --build build/all
QT_QPA_PLATFORM=offscreen ctest --test-dir build/all -L 'ladder|examples-common' --output-on-failure
```

Expected: `client stack retired`; `ladder_common_tests`, `examples_common_app_tests`, every rung's tests and every
`ladder_<rung>_smoke_tests` binary pass.

Mutation check: put back, in `testkit_main.cpp`, the `#ifdef MORPH_LADDER_TESTKIT_GUI_APP` / `#include
<QGuiApplication>` branch; `/tmp/morph-retire-common.sh` exits 1 naming the `#ifdef` line. Restore.

- [ ] **Step 5: Commit**

```bash
git add -A examples/common
git commit -m "wip(retire): remove examples/common's presenter, EventPoller, AppContext and QML-surface audit

Signed-off-by: Christian Parpart <christian@parpart.family>"
```

---

### Task 12: `TESTING.md`, `IMPLEMENTATION.md`, `LADDER.md` — the app on an injected frontend

**Files:**
- Modify: `examples/TESTING.md` — title and introduction; delete "Current state (verified, 2026-08)"; "Presenter
  architecture" → "App architecture"; delete "The QML-surface drift guard"; "The dual-mode fixture"; "Pumping
  discipline"; "Multi-client stress harness" (`client_pool`, convergence, test naming); "WASM reality"; "Build system
  and CI"; "Framework gaps" item 2
- Modify: `examples/IMPLEMENTATION.md` — rule 1's two presenter clauses; rule 2 → "One app, any frontend"; rule 5's
  last bullet; rule 6's checklist item 2
- Modify: `examples/LADDER.md` — the opening, the companion-documents paragraph, the extension-bag spike's result,
  the rung-0 scope paragraph, the explicit-submit paragraph

**Interfaces:** documentation of spec 4 §7 and of Tasks 9–11.

- [ ] **Step 1: Write the failing check**

```bash
/tmp/morph-doc-check.sh '[Pp]resenter|QML-surface|qml_surface\.hpp|testkit_src/qml_surface|QmlSurface|gui_lib|gui_wasm|morph_ladder_gui|ladder_gui|morph_ladder_app([^_]|$)|AppContext::Mode|EventPoller|MorphForms|DynamicForm|FormsControllerCore|FORMS_QML|Qt Quick Test|engine-load smoke' \
    examples/TESTING.md examples/IMPLEMENTATION.md examples/LADDER.md
```

(`/tmp/morph-doc-check.sh` is Task 7's.) The bare `qml_surface.cpp` is not in the pattern on purpose: the section
"What `examples/common`'s coverage number measures (audit, 2026-09)" is a dated measurement of the directory as it
stood that day, its rows name the file, and `examples/crm/README.md` cites the section — it stays as written.

- [ ] **Step 2: Run it to verify it fails**

Expected: exit 1 — `examples/TESTING.md` names missing paths (`examples/common/testkit/qml_surface.hpp`,
`examples/common/testkit/test_qml_surface.cpp`, `examples/common/gui/…`) and dozens of pattern hits across the
three files.

- [ ] **Step 3: Implement**

**`examples/TESTING.md`.**

Title and introduction, up to the first `##`, become:

```markdown
# Ladder testing strategy — controllers, views, dual deployment modes, multi-client stress

Every rung of the [application ladder](LADDER.md) ships a client whose controllers are unit tested in **both
deployment modes** — in-process (`LocalBackend`) and client/server (`QtWebSocketBackend` against a
`RemoteServer`), including **N clients against one server** for stress tests — and whose views are tested on
`ui::testing::RecordingBackend`. This document is the binding convention; rung READMEs reference it instead of
restating it.

**What this machinery is:** [`IMPLEMENTATION.md`](IMPLEMENTATION.md) rule 2 keeps views to bindings and
controllers to declarative state over morph's `Query`/`Mutation`, so the BackendRig / client-pool / convergence
stack is **a conformance harness for morph's client-side stack** — `Bridge`, backends, `morph::reactive`,
completions, attach/reconnect under a real event loop. It is owned by the testkit as framework coverage: the full
matrix runs once per framework surface it conforms, and each rung runs a *thin instantiation* (its controllers
through the rig, one suite per model — not a per-screen × 3-mode combinatorial matrix), which keeps the CI cost
curve flat.
```

Delete the section "## Current state (verified, 2026-08)".

Replace the section "## Presenter architecture (every rung)" (through the end of its rule 7) with:

```markdown
## App architecture (every rung)

A rung's client is a toolkit-free **app library** — `examples/<rung>/app/`, target `ladder_<rung>_app`:
`controllers/`, `views/` and the rung's `ui::Application` — plus one binary, `examples/<rung>/ui/main.cpp`,
target `<rung>`, which picks a frontend at run time (`ui::selectFrontend`: `--ui=<name>`, else `MORPH_UI`, else the
first built frontend that can run) and hands it the application's factory. The same library and binary build
natively (terminal UI and Qt Quick) and for the browser (Qt Quick).

1. **The app library is toolkit-free**, and the build enforces it: `ladder_<rung>_app` links `morph`,
   `morph_ladder_app_common` and the rung's domain library — nothing else — so a Qt, terminal or frontend include
   in `app/` fails to compile.
2. **Views are bindings only.** `app/views/` builds `ui::Node` trees whose props bind to controller state; every
   conditional, format and validation lives in a controller or a `Computed`. Schema-driven parts are
   `forms::formView`, `collectionView` and `wizardView` over the controller's sessions.
3. **Controllers are tested headless**: a `reactive::Runtime` on a `MainThreadExecutor` or `StepExecutor`, the
   multi-mode rig below (`Local`, `LocalSingleThread`, `Socket`), and waits through `testkit/wait.hpp` — never a
   sleep. A controller owns, in order, its handlers, a `Store` if it has user-intent state, its `Query`s,
   `Mutation`s and `Subscription`s, and its `Computed` projections, so the `CallbackScope`s inside them are
   destroyed first.
4. **One view test per screen** on `ui::testing::RecordingBackend`: the screen's tree, and that its controls drive
   the controller — a click, an edit or a choice reaches the controller, and the bound state comes back.
5. **One smoke test per frontend per app** (`testkit/frontend_smoke.hpp`): the application mounts and quits on the
   terminal UI (scripted input) and on Qt Quick (offscreen).
6. **Fingerprints, journeys, convergence and stress harnesses** drive controllers. Polling is a `Query` with
   `refreshEvery` on the frontend's `Scheduler` (`reactive::testing::ManualScheduler` in tests), so a test advances
   time instead of waiting for it.
```

Delete the section "## The QML-surface drift guard" entirely.

"## The dual-mode fixture": "N "clients" are N presenter sets over the shared bridge" → "N "clients" are N
controller sets over the shared bridge"; "each rung shipping a small headless-client binary that drives its
*presenters*, not raw handlers" → "…that drives its *controllers*, not raw handlers"; "Teardown order (encoded in
`~BackendRig`): presenters → client bridges →" → "Teardown order (encoded in `~BackendRig`): controllers → client
bridges →".

"## Pumping discipline — no sleeps": retitle "## Waiting discipline — no sleeps" and replace its first two
paragraphs (through "…copied from `tests/qt/test_qt_websocket.cpp`.") with:

```markdown
Controller tests wait through `examples/common/testkit/wait.hpp`: `pumpUntil(owner, done, budget)` runs the
owner executor (`MainThreadExecutor` or `StepExecutor`) until `done()` holds or the budget — scaled by
`MORPH_LADDER_DEADLINE_MS` through `deadline.hpp` — runs out. `testkit/pump.hpp` is the Qt event-loop pump
(`pumpUntil(pred, deadline)`, `awaitQt<T>(Completion<T>)`) and serves only what is Qt: the rig's `Socket` mode,
whose server and client transport are QObjects, and the fault proxy. A `sleep_for` outside these two headers is a
review-rejectable defect. Test binaries use the Qt-owning `main()` in `testkit/testkit_main.cpp`
(`QCoreApplication`, `Catch::Session`, a DeferredDelete drain).
```

In the next paragraph "routing its callbacks through a `QtExecutor` produces 165 TSan warnings" stays; in the one
after, "`pump.hpp` covers waiting on the *Qt loop*." → "`wait.hpp` and `pump.hpp` cover waiting on an owner
loop."

"## Multi-client stress harness": the component table's first row becomes `` `testkit_main.cpp`, `wait.hpp`,
`pump.hpp`, `deadline.hpp`, `backend_rig.hpp`, `db_fixture.hpp`, `db_fault_fixture.hpp`, **fault proxy + strand
interleaver** `` | rung 0/1, and add a row `` `frontend_smoke.hpp` `` | every app. The `client_pool.hpp` bullet:
"typed pool constructing each client's controller set against `rig.client(i)`; test bodies are mode-blind." The
`convergence.hpp` bullet: "…rungs 0–2 use `settle()` + fingerprint equality…" → "…rungs 0–2 wait for every client's
controllers to go idle and compare fingerprints, without event cursors." The per-rung test naming line becomes:
"Per-rung test naming: `test_model_<entity>.cpp` (full mode matrix), the controller tests (full matrix), one view
test per screen, one frontend smoke test, `test_multiclient.cpp` `[stress]`, `test_offline.cpp` (rungs 4/6/7)." —
and if every rung of Parts 7–9 names its controller, view and smoke files one way (`ls examples/*/tests`), write
that pattern in place of the three descriptive phrases.

Replace the section "## WASM reality" up to its "Open framework facts every rung must respect (verified):" line
with:

```markdown
## WASM reality

A rung's browser client is its ordinary binary: `ui/main.cpp` built under Emscripten with Qt Quick as the only
frontend and `-DMORPH_CLIENT_ONLY=ON`, so the browser runs the same app library as the desktop. It cannot be
unit-tested in a browser in CI today; three layers stand in:

1. **`LocalSingleThread` mode natively** — the same controllers, WASM-shaped wiring, every test run.
2. **Compile gate** — `.github/workflows/wasm-ladder.yml` (and `wasm-demo.yml` for bank) builds each rung's binary
   for wasm32-emscripten, so app code cannot drift away from the browser build.
3. **One scripted browser smoke** (emrun + Playwright against the built client) as an optional stage in the same
   CI run.
```

and its first "Open framework facts" bullet with: "A WASM client over `QtWebSocketBackend` is compiled on every
qualifying PR — `wasm-ladder.yml` builds the rung-0 spike and every rung's binary under
`-DMORPH_LADDER_RUNGS=all` — but has never been *run* in CI: "does it build" is answered, "does it work in a
browser" is not." The remaining bullets stand.

"## Build system and CI":

- The `examples/common/` bullet ("declares exactly three consumable targets") becomes:

  ```markdown
  - `examples/common/` declares three consumable targets: `morph_ladder_app_common` (STATIC, Qt-free:
    what every example application's library and binary share — `AppEnvironment`, `connect`, `Poller`,
    `Wiring`, `mapCompletion`, `newUuid`, the id helpers; the Qt and `morph::net` transports are compiled
    into it only when their options are on), `morph_example_testkit` (morph + Catch2 + every built frontend:
    `wait.hpp`, `fake_app_context.hpp` and the frontend smoke harness) and `morph_ladder_testkit` (morph +
    Catch2 + Qt + Lightweight: the rig, the fixtures, the fault proxy, `pump.hpp`). A rung's
    `ladder_<rung>_app` links `morph_ladder_app_common`; its tests link `morph_ladder_testkit` and
    `morph_example_testkit`; its `ladder_<rung>_smoke_tests` link `morph_example_testkit` over
    `morph_test_main`. Rungs link targets, never paths; the testkits never grow per-rung options.
  ```

  Check each clause against `examples/common/CMakeLists.txt` as Task 11 left it and
  `examples/common/app/CMakeLists.txt` as Part 6 wrote it (what each target compiles and links) and correct the
  clause, not the CMake, where they differ.
- The `morph_add_rung()` bullet: "creates `ladder_<rung>_{lib,gui_lib,gui, gui_wasm,tests,headless}`" → "creates
  `ladder_<rung>_{lib,app,server_app,server,tests,smoke_tests}` and the binary `<rung>` (built when at least one
  frontend is), refuses a rung that still has a top-level `gui/`, `gui_lib/` or `gui_wasm/` directory". Its sentence "every rung's
  `src/`, `include/` and `gui_lib/`" (the `.clang-tidy` bullet) → "every rung's `src/`, `include/` and `app/`".
- The bullet beginning "Do **not** copy bank's `wasm/` shadow-header pattern" (Part 7's wording) becomes:

  ```markdown
  - Do **not** copy bank's `wasm/` shadow-header pattern — a rung's `app/` library is toolkit-free, and copying
    the pattern makes the WASM and native builds different programs, silently falsifying the "same client code"
    DoD. One WASM configure builds every rung's binary (`.github/workflows/wasm-ladder.yml`, which also builds
    rung 0's spike; it caches emsdk but has no compiler cache yet): the same `app/` library and the same
    `ui/main.cpp`, with Qt Quick the only frontend.

    **What rung 1 learned doing this for real**: a client's controllers hold `BridgeHandler<Model>`s, so a WASM
    client still *names* its rung's model type and therefore still includes its model header. …
  ```

  keeping the rest of the existing "What rung 1 learned" paragraph from "Every rung's models acquire their
  `Lightweight::DataMapper` connection…" to its end, unchanged.
- The coverage-wiring bullet: "Every ladder CMake target (`morph_ladder_gui`, `morph_ladder_app`,
  `morph_ladder_testkit`, and each rung's own targets)" → "Every ladder CMake target (`morph_ladder_app_common`,
  `morph_ladder_testkit`, and each rung's own targets)".
- CI tiers, item 1's paragraph "Two pieces of this live outside that job as shipped…": replace its "**The GUI
  half**" sentences (through "…so the omission is never silent.") with "**The frontends** — each rung's binary and
  its smoke tests — need `MORPH_BUILD_TUI=ON` and `MORPH_BUILD_QT_QUICK=ON`, which the ladder jobs configure; with
  neither, `morph_add_rung()` builds the app library and every test but the smoke tests, and no binary." Keep the
  "**The WASM compile gate**" sentences.

"## Framework gaps this strategy exposed": item 2 "→ `include/morph/core/bridge.hpp`, so `settle()` can be exact
rather than substituting presenter-level counters." → "→ `include/morph/core/bridge.hpp`, so a test's wait can be
exact rather than counting calls by hand."

**`examples/IMPLEMENTATION.md`.**

- Rule 1: "nothing domain-shaped may live in presenters, QML, `main()`, or free functions" → "nothing
  domain-shaped may live in controllers, views, `main()`, or free functions"; in the offline carve-out, whose
  kanban sentence Part 9 already rewrote to name `examples/kanban/app/controllers/offline_moves.cpp`, replace
  "**not** a presenter:" with "**not** a screen's controller:".
- Replace "## 2. GUI minimalism" with:

  ```markdown
  ## 2. One app, any frontend

  The UI is deliberately the *least* interesting part of every rung. We are not building UIs; we are proving morph
  can drive them — on every frontend, from one description.

  - **One app library, one binary.** A rung's client is `app/` — controllers and views, toolkit-free — and
    `ui/main.cpp`, which picks the terminal UI or Qt Quick at run time and runs the app's factory on it
    ([`TESTING.md`](TESTING.md), "App architecture"). Nothing in `app/` names a toolkit; the build refuses it.
  - **Schema-driven first, always.** Every form is a `forms::FormSession` built from the schema the rung emits, or a
    typed `forms::Form<A>`, rendered by `forms::formView`; every list over an action is a `forms::collectionView` or
    a `ui::table` bound to a `Query`; multi-step flows are `forms::wizardView`. Hand-built input rows, hand-built
    tables and hand-rolled layouts are **forbidden by default**.
  - **A custom view requires a written justification** in the rung README, and the only two acceptable
    justifications are: (a) the generated UI *cannot* express the interaction — a forms-subsystem finding, so file
    it on the gap ledger (this is how the ladder found the missing explicit-submit mode, the child-table renderer
    gap and the sum-type gap — see [`LADDER.md`](LADDER.md)); or (b) pure glue with no domain logic (an app shell
    frame, a connection-status line).
  - **Controllers decide nothing the model owns.** They hold view state, issue `Query`s and `Mutation`s and project
    results through `Computed`s; views bind. Validation, authorization and invariants stay in the model (rule 1).
  - **Zero styling effort.** Each frontend's default look; no theming, no animations, no custom drawing. A rung that
    looks pretty has spent effort in the wrong place.
  ```

- Rule 5's last bullet: "GUI/presenter testing follows `TESTING.md`; there is no separate GUI logic to test if rule
  2 was followed — presenter tests verify routing, error surfacing, and quiescence, not business behavior." →
  "Controller and view testing follows `TESTING.md`; there is no separate UI logic to test if rule 2 was followed —
  controller tests verify routing, error surfacing and quiescence, view tests verify bindings, and neither tests
  business behaviour."
- Rule 6, item 2: "Custom GUI elements present, each with its written justification…" → "Custom views present, each
  with its written justification…".

**`examples/LADDER.md`.**

- Opening paragraph: "clients are Qt (desktop + WASM), as in [`bank`](bank)." → "clients are toolkit-free apps on
  an injected frontend — the terminal UI or Qt Quick, chosen at run time, and Qt Quick in the browser — as in
  [`bank`](bank)."
- The extension-bag spike's "**Complete:**" sentence: "the journal and the QML renderer needed nothing new" → "the
  journal and the schema-driven renderer needed nothing new".
- The companion-documents paragraph: "(models are the application; minimal schema-driven GUIs; …)" → "(models are
  the application; one app, any frontend, schema-driven first; …)"; and "how they are tested: every rung's GUI is
  presenter-shaped and unit tested in **both deployment modes** (in-process `LocalBackend`, and
  `QtWebSocketBackend` against an in-test `RemoteServer` with N clients) plus a WASM-shaped single-thread mode, via
  the shared `examples/common/testkit`." → "how they are tested: every rung's controllers are unit tested in **both
  deployment modes** (in-process `LocalBackend`, and `QtWebSocketBackend` against an in-test `RemoteServer` with N
  clients) plus a WASM-shaped single-thread mode, its views on the `RecordingBackend`, and its binary by one smoke
  test per frontend, via the shared `examples/common/testkit`."
- Bank's paragraph: Part 7 already rewrote it (no presenter, no QML surface audit); leave it.
- "## Rung 0, scope, and sequencing": "the shared presenter architecture (`examples/common/gui`)" → "the shared
  client layer (`examples/common/app`)".
- The explicit-submit paragraph: "…and the shipped `DynamicForm.qml` renders its own Submit button — gated on the
  form's `ready` state — instead of auto-firing on validity (`docs/spec/forms/forms.md`, "Explicit submit mode").
  Adoption is per rung and only `polls` has migrated so far, on all three of its schema-driven actions;
  `bookmarks`, `lims` and `pastebin` still pair `controller: null` with a hand-written Button at thirteen call
  sites between them." → "…and `forms::formView` renders a Submit button bound to the session's readiness instead
  of submitting on every ready change (`docs/spec/forms/forms.md`, "Explicit submit mode"). Adoption is per
  action."

- [ ] **Step 4: Run the check to verify it passes**

```bash
/tmp/morph-doc-check.sh '[Pp]resenter|QML-surface|qml_surface\.hpp|testkit_src/qml_surface|QmlSurface|gui_lib|gui_wasm|morph_ladder_gui|ladder_gui|morph_ladder_app([^_]|$)|AppContext::Mode|EventPoller|MorphForms|DynamicForm|FormsControllerCore|FORMS_QML|Qt Quick Test|engine-load smoke' \
    examples/TESTING.md examples/IMPLEMENTATION.md examples/LADDER.md && echo clean
pre-commit run --files examples/TESTING.md examples/IMPLEMENTATION.md examples/LADDER.md
grep -n '](#' examples/TESTING.md examples/IMPLEMENTATION.md examples/LADDER.md   # every in-page anchor still names a heading
```

Expected: `clean`; the hooks pass; each `](#…)` anchor printed matches a heading in its file (read them — the
renamed "App architecture" and "Waiting discipline" sections are the ones to look for).

Mutation check: add "See `examples/common/testkit/qml_surface.hpp`." to `examples/TESTING.md`; the check exits 1
naming the missing path and the `qml_surface` hit. Remove it.

- [ ] **Step 5: Commit**

```bash
git add examples/TESTING.md examples/IMPLEMENTATION.md examples/LADDER.md
git commit -m "wip(retire): TESTING, IMPLEMENTATION and LADDER describe the app on an injected frontend

Signed-off-by: Christian Parpart <christian@parpart.family>"
```

---

### Task 13: Every other reference — specs, guides, library comments, tests, tooling

**Files:**
- Modify: `docs/spec/forms/forms.md`, `views.md`, `workflows_navigation.md`, `choice.md`, `instance_constraints.md`,
  `sections.md` — the QML-renderer sections and sentences that remain after Part 5
- Modify: `docs/GETTING-STARTED.md` — only what the check below still prints (Part 8 rewrote §3–§12 for
  pastebin's new client)
- Modify: `docs/todo.md`, `docs/spec/core/callback_scope.md`, `docs/spec/offline/offline.md`
- Modify: comments in `include/morph/forms/forms.hpp`, `include/morph/forms/flows.hpp`,
  `include/morph/forms/views.hpp`, `include/morph/render/i18n.hpp`, `include/morph/render/locale_format.hpp`,
  `include/morph/core/callback_scope.hpp`
- Modify: comments in `tests/` (`CMakeLists.txt`, `forms_rule_corpus.hpp`, `test_forms_*.cpp`,
  `test_render_locale_format.cpp`, `test_quantity_forms.cpp`, `test_extension_bag_spike.cpp`, `test_completion.cpp`,
  `testkit/catch_main.cpp`) — each line `git grep` names
- Modify: `examples/*/tests/.clang-tidy`, `examples/*/include/.clang-tidy`, `examples/crm/tests/.clang-tidy`,
  `examples/crm/include/crm/gui/crm_schemas.hpp`, `examples/crm/EXTENSION-BAG-SPIKE.md`
- Modify: `scripts/coverage.sh`, `codecov.yml`, `scripts/check_automoc_includes.sh`, `cmake/compiler_options.cmake`,
  `cmake/morph_demote_interface_includes.cmake`

**Interfaces:** none.

- [ ] **Step 1: Write the failing check**

```bash
cat > /tmp/morph-retire-refs.sh <<'SH'
#!/usr/bin/env bash
set -u
pattern='DynamicForm|CollectionView\.qml|WizardView|AppShell\.qml|SlotRegistry|MorphForms|I18nCatalog|JsonExact|FormsControllerCore|GenericModelBridgeCore|MultiModelBridgeCore|src/qt/forms|qt/bridge/|tst_[A-Za-z_]+\.qml|QuickTest|Qt/QML|QML renderer|forms_qml|qt_forms|FORMS_QML|[Pp]resenter|gui_lib|gui_wasm|qml_surface|EventPoller|AppContext::Mode|examples/common/gui'
git grep -nE "$pattern" -- docs/spec docs/GETTING-STARTED.md docs/todo.md include tests scripts cmake codecov.yml \
    'examples/*/tests/.clang-tidy' 'examples/*/include/.clang-tidy' examples/common examples/crm \
    ':!tests/lint/automoc_includes' \
  | grep -vE '^(cmake/compiler_options\.cmake|scripts/check_automoc_includes\.sh):[0-9]+:.*budget_presenter\.hpp' \
  | grep -vE '^cmake/compiler_options\.cmake:[0-9]+:.*NET/QT/FORMS_QML/OFFLINE_SQLITE/' \
  && exit 1
for file in docs/spec/forms/*.md docs/GETTING-STARTED.md; do
    /tmp/morph-doc-check.sh 'x^' "$file" || exit 1   # 'x^' matches nothing: paths only
done
exit 0
SH
chmod +x /tmp/morph-retire-refs.sh
```

- [ ] **Step 2: Run it to verify it fails**

```bash
/tmp/morph-retire-refs.sh; echo "exit $?"
```

Run the path half once on the tree as it was before this part (`git stash -u; for f in docs/spec/forms/*.md
docs/GETTING-STARTED.md; do /tmp/morph-doc-check.sh 'x^' "$f"; done; git stash pop`): a path it already reports is
pre-existing — fix it if it names a file that moved, and say so in the hand-off either way.

Expected: `exit 1`, printing the hits this task's Files list names (the counts per file before Parts 5–9 ran were
forms.md 98, workflows_navigation.md 25, views.md 17, GETTING-STARTED.md 8, the `tests/` comments about 25; Part 5
added engine sections beside the spec ones without removing them, and Part 8 took GETTING-STARTED's).

- [ ] **Step 3: Implement**

Rewrite each printed line by this table — a sentence that only described the QML mirror is deleted, not
translated:

| Retired name | Write instead |
|---|---|
| `DynamicForm`, `DynamicForm.qml` | `forms::formView` over a `FormSession` (the forms engine) |
| `CollectionView.qml` | `forms::collectionView` over a `CollectionSession` |
| `WizardView.qml` | `forms::wizardView` over a `WizardSession` |
| the demo's `AppShell.qml` | `forms::appShellView` over an `AppShellSession` |
| `SlotRegistry` (any tier, chrome slots) | `forms::Overrides` (`byField`, `byWidget`, `byUnit`, `byKind`) |
| `I18nCatalog` | a `render::TranslationProvider` in `FormSessionOptions::translations` |
| `JsonExact.js`, `JsonExact.parse` | the engine's JSON reading (glaze's `generic_u64`, which keeps integer digits exact) |
| `FormsControllerCore`, `MultiModelFormsControllerCore`, `submitIfValid`, `fetchOptions` | `forms::bridgeSubmitter` / `forms::bridgeChoiceFetcher` |
| `src/qt/forms/tests/tst_<X>.qml` | the forms-engine test that re-expresses it (Part 5's plan maps every QuickTest file to its replacement, spec 2 §10) |
| `MorphForms`, `forms_qml`, `qt_forms`, `MORPH_BUILD_FORMS_QML` | delete the clause: the engine is in the base `morph` target |
| "the shipped (Qt/QML) renderer", "the QML mirror" | "the forms engine" |
| presenter (examples) | controller |
| `gui_lib/` (a rung directory) | `app/` |

and these sections whole:

`docs/spec/forms/forms.md`:

- Replace "## Shipped Qt/QML reference renderer" — with its subsections "### Prefill — loading a stored payload for
  editing" and "### What `ready` claims", up to "## Renderer conformance kit" — by:

  ```markdown
  ## Rendering — the forms engine

  The schema contract above is renderer-agnostic. morph renders it with the forms engine
  (`include/morph/forms/engine/`, specified in [engine.md](engine.md)): `FormModel::fromSchema` reads a schema into
  fields, `FormSession` holds the drafts, encodes them exactly, evaluates the rules and submits, and
  `forms::formView(session)` returns a `ui::Node` that every `morph::ui` frontend — the terminal UI and Qt Quick —
  draws. A typed `forms::Form<A>` builds the same model from `schemaJson<A>()`, so a typed and a runtime form for
  the same action behave identically.

  - **Prefill** is `FormSession::prefill(bodyJson)`: the reverse of every encoder, applied as one programmatic
    change that never submits by itself. `reset()` clears every draft the same way.
  - **`ready`** is `FormSession::ready()`: every required field encodes, no field has an error, and every gating
    rule is `True`. It is a client-side convenience over the schema; the model's own `validate()` and the
    dispatcher's rule check stay the authority (see "Field metadata is not a security control").
  ```

- "## Renderer conformance kit": replace the "**QML functional assertions**" bullet (and any further bullet naming a
  `.qml` file) with: "- **Engine assertions** run the same five fixtures through `FormModel::fromSchema` and
  `FormSession` ([engine.md](engine.md), "Parity and tests"): fields come out in `x-order`; readiness waits for every
  `required` field; a `Quantity` body is `{num,den,dp}` exact and a unit switch converts it exactly; a `Choice`
  carries its declared options action; a date-time encodes as ISO-8601 UTC."
- Replace "## Theming / component-override registry" with its "### Chrome slots" by:

  ```markdown
  ## Overrides — replacing a field's control

  A field's control is the engine's default for its kind ([engine.md](engine.md), "Rendering"). An app that wants a
  different control for one field, one `x-widget` hint, one unit or one kind registers it in `forms::Overrides`,
  handed to `formView` through `FormViewOptions`:

  - `byField(path, render)`, `byWidget(hint, render)`, `byUnit(unit, render)` and `byKind(kind, render)`, resolved
    in that order, then the default. `render` is `std::function<ui::Node(FieldView&)>`; `FieldView` exposes the
    field's spec and its session state, so an override binds to the same draft and error the default control does.
  - **`x-widget`** (optional, property-level) names a control variant when the type alone is ambiguous —
    `"textarea"`, `"slider"`, `"radio"`, which the engine's defaults dispatch on (see "Widget hints"), or an
    app-defined id an override recognises. Absent, it matches no `byWidget` override.
  ```

- "### The catalog seam": replace the paragraph beginning "The `examples/forms/gui_qml` reference renderer hosts a
  concrete, minimal realization" through the end of that subsection with: "The forms engine is the seam's
  production caller: `FormSessionOptions::translations` is the provider and `FormSessionOptions::bcp47` the locale,
  and every label, help text and placeholder a session shows goes through `resolveText`."
- "### Collections of objects — a host slot draws them" and "### Nested objects — a host slot draws them": retitle
  "### Collections of objects — rows with Add and Remove" and "### Nested objects — a sub-panel"; keep every
  sentence about the schema's shape; replace the sentences about a slot drawing them with "The engine draws an
  object array as a `ForEach` of child panels, each with Remove, and an Add button, and a nested object as a
  `Panel` holding its child fields; both encode and prefill like top-level fields. Nothing is unrepresentable."
  (fix the two in-page links that name the old anchors).
- "### Two evaluators, one corpus": the evaluators are the typed rule nodes (`allRulesSatisfied<A>`, the
  dispatcher's check) and the engine's runtime evaluator (`include/morph/forms/engine/rules.hpp`), both C++,
  compared row by row over `tests/data/rule_corpus.json` in one test binary — rewrite the section to say exactly
  that.
- "#### What `DynamicForm` does with a nested aggregate" → "#### What the engine does with a nested aggregate", body:
  "It draws it — a nested object as a sub-panel, an object array as rows with Add and Remove — and encodes and
  prefills it like any top-level field."

`docs/spec/forms/views.md`: replace "## The Qt/QML reference renderer" and "### Chrome slots and the embedded
editors" by:

```markdown
## Rendering — `collectionView`

`forms::collectionView(CollectionSession&)` renders a view document ([engine.md](engine.md), "Collections"): the
list is a `Query` on `v-query`, shown as a `Table` whose columns come from `v-columns`, its cells formatted
exactly; row and collection actions are `Mutation`s whose bodies are built from `bind` with exact ids; an action
with `confirm` opens a `Dialog` first and runs only on confirmation; a success refetches the list. Activating a
row opens the `v-rowAction` editor — a `FormSession` cleared and then prefilled from the row as one programmatic
change, so opening a row never submits by itself — as a dialog for `v-kind: "collection"` and a side form for
`"master-detail"`.

`examples/forms`' `SamplesView` (`examples/forms/lab_views.hpp`, over `ListSamples`, `EditSample`, `DeleteSample`
and `CreateSample`) is the worked example; `morph_forms_app` shows it as an app-shell `view` screen.
```

and in "## Limitations" delete each bullet that describes the QML renderer's behaviour (lossy cell formatting
through a double, prefill reaching only plain text fields, the auto-fire on row open); keep the bullets about the
`v-*` vocabulary.

`docs/spec/forms/workflows_navigation.md`: replace "## The Qt/QML reference renderer" and "### Chrome slots" by:

```markdown
## Rendering — `wizardView` and `appShellView`

`forms::wizardView(WizardSession&)` renders a wizard document ([engine.md](engine.md), "Wizards"): one
`FormSession` per step, kept alive for the session's lifetime, so a step's drafts survive Back and Next; a step is
done when its submission succeeded, and Next is enabled only then; entering a step applies its `prefill` from the
resolved values — the draft first, then the reply, the reply winning on a name collision, the rule
`FlowSession::captureResult` follows — as JSON values, after re-fetching the step's `Choice` options so a value
naming a row an earlier step created is among them.

`forms::appShellView(AppShellSession&)` renders an `app-*` document: a title, and a `Menu` from `app-menu` beside the
current screen of `app-screens`, for the kinds `form`, `wizard` and `view` (`ViewScreen<Id, View>`). `AppShellSession` makes each
screen's session from `AppShellSources` the first time the screen is shown and keeps it, so a session outlives its
view: leaving a screen and coming back finds it as it was.
`examples/forms`' `LabApp` is the worked example.
```

In "## Limitations" delete the bullets "**`kind: "view"` is not implemented.**", "**No cross-screen state store in
the reference renderer.**" and "**Prefill does not resync a widget's displayed value.**" (if Part 5 has not); in
"## Testing" replace the bullet about `WizardView.qml`/`tst_wizardview.qml` with one naming the forms engine's
wizard tests (from Part 5's plan) and `examples/forms/tests/test_lab_wizard.cpp`; in "## Cross-references" drop the
clauses about `src/qt/forms` vs `examples/forms/gui_qml` and `CollectionView.qml`'s placement.

`docs/spec/forms/instance_constraints.md`: "### What the shipped renderer does with them" → "### What the forms
engine does with them"; "`DynamicForm.qml` honours all three:" → "The engine honours all three:"; the table's
column "Effect in the renderer" → "Effect in the engine"; replace the boundary bullet "**The client compares a
`double`** …" with "**The engine compares exactly**: a bound's `{num,den}` is a `Rational`, compared with the
draft's `Rational`; no double is involved. The model's `checkValue` stays the floor."

`docs/spec/forms/choice.md`: "and `DynamicForm.qml` resolves the `$ref`, reads `type`, and draws a checkbox for
`"boolean"`" → "and a renderer that resolves the `$ref` and reads `type` draws a checkbox for `"boolean"`"; "The
renderer parses an `OptionsAction` reply with `JsonExact.parse` (`src/qt/forms/qml/JsonExact.js`), which keeps an
integer literal a double cannot represent as its exact digits, and emits those digits verbatim into the submitted
body." → "The forms engine reads an `OptionsAction` reply with glaze's `generic_u64`, which keeps an integer
literal a double cannot represent as its exact digits, and writes those digits verbatim into the submitted body."

`docs/spec/forms/sections.md`, "## Limitations": "No renderer ships for `s-*` yet. `WizardView.qml` has no
section-group counterpart in `src/qt/forms`; a host consuming the document builds its own layout for now." → "No
renderer ships for `s-*` yet: the forms engine has no section-group counterpart to `wizardView`, so a host
consuming the document builds its own layout."

`docs/GETTING-STARTED.md`: Part 8 (its Task 7) rewrote §3, §4 and §9–§12 for pastebin's app library and binary.
Fix only the lines the check still prints, by the table above; a passage that only described the QML client is
deleted, not translated.

`docs/todo.md`: in the "Removed the reactive-draft mechanism" entry drop ", and `WizardView.qml` are unchanged";
E-G7: "and the `src/qt/forms` `CollectionView.qml` reference renderer" → "and `forms::collectionView`"; E-G8: "and
the `src/qt/forms` `WizardView.qml` reference renderer plus the demo's `AppShell.qml`" → "and `forms::wizardView` /
`forms::appShellView` (`ViewScreen` included)"; delete the "F3 reworks this" note under E-G8 if `FlowSession` no
longer uses the removed mechanism (`git grep -n 'set<&' include/morph/forms/flows.hpp` shows its own `set`).

`docs/spec/core/callback_scope.md` and `include/morph/core/callback_scope.hpp`: the example class `BoardPresenter` →
`BoardController`. `docs/spec/offline/offline.md` (Part 9 already rewrote the kanban bullet to name
`examples/kanban/app/controllers/offline_moves.cpp`): the quotation of rule 1, "nothing domain-shaped may live in
presenters, QML, `main()`, or free functions", → "nothing domain-shaped may live in controllers, views, `main()`, or
free functions" (Task 12's rule 1, verbatim); "never in a presenter, a QML bridge, or `main()`" → "never in a
screen's controller, a view, or `main()`".

Library comments:

- `include/morph/forms/forms.hpp`, `annotateBasicMemberProperty`'s comment: "The shipped Qt/QML `DynamicForm` then
  fires two mutually exclusive kind flags on it (`isBoolean` and `isArray` both true) and draws a checkbox whose
  payload is a JSON array of the string "false", reporting the form `ready` for a value nobody chose." → "A
  renderer reading that type set sees both a boolean and an array, and has no closed set to offer — it can only
  guess a control, and any value it then submits is one the user did not choose."; the `static_assert` message's
  "…and the shipped DynamicForm renders that wildcard as a checkbox reporting the form ready for a value the user
  never chose." → "…which no renderer can turn into a choice of the declared values."
- `include/morph/forms/flows.hpp`: "(the shipped renderer's `DynamicForm` calls `submitIfValid` on every change that
  leaves the form ready, with no in-flight suppression either)" → "(the forms engine's automatic submission sends
  on every change that leaves a form ready with a changed body, with no in-flight suppression either)".
- `include/morph/forms/views.hpp`: "the same two shapes Choice's optionRows reads (DynamicForm.qml)" → "the same
  two shapes the forms engine reads a `Choice`'s options from".
- `include/morph/render/i18n.hpp`: the `@file` paragraph's "The per-field widget-override registry that pairs with
  it is *not* C++ at all: it is `SlotRegistry`, a QML type in module `MorphForms` (`src/qt/forms/qml/
  SlotRegistry.qml`, documented in docs/spec/forms/forms.md), so do not look for it under this namespace." → "The
  per-field control override that pairs with it is `forms::Overrides` (`morph/forms/engine/overrides.hpp`)."; the
  comment above `resolveText`: delete "The QML renderer carries a mirror of this function with the same parameters
  in the same order (src/qt/forms/qml/DynamicForm.qml, `resolveText(explicitKey, derivedKey, literal)`), and the two
  are meant to be read against each other. Reordering to break the adjacency here would desynchronise that pair
  and" and continue the sentence "…leave the signature the only place…" as "Reordering it would leave the signature
  the only place…".
- `include/morph/render/locale_format.hpp`: "-- matching the QML mirror in `src/qt/forms/qml/DynamicForm.qml`, which
  rejects it too." → "." (end the sentence after "rejected").

Tests, scripts, tooling (comments only — no behaviour changes):

- `tests/CMakeLists.txt`: the comment naming `src/qt/forms/tests/tst_DynamicFormRuleCorpus.qml` names the engine's
  rule-corpus test instead.
- `tests/forms_rule_corpus.hpp`, `tests/test_forms_rule_corpus.cpp`, `tests/test_forms_rule_agreement.cpp`: "a
  JavaScript reimplementation in `src/qt/forms/qml/DynamicForm.qml`" → "the forms engine's runtime evaluator
  (`morph/forms/engine/rules.hpp`)"; the pin's "one file, two readers" names this suite and the engine's corpus
  test.
- `tests/test_forms_{blank_as,boolean_anyof_wire,display_unit,exact_bounds,field_bounds}.cpp`,
  `test_forms_instance_constraints.cpp`, `test_forms_conformance_corpus.cpp`, `test_forms_layout.cpp`,
  `test_quantity_forms.cpp`, `test_render_locale_format.cpp`, `test_extension_bag_spike.cpp`: each "…`tst_X.qml`
  pins the renderer half" → "…the forms engine's `<test file>` pins the renderer half" (the file Part 5 maps `tst_X`
  to); each "the QML mirror …" sentence → "the forms engine …" or deleted where it only described the mirror.
- `tests/test_completion.cpp`: "This is the mechanism behind Presenter::track()'s onErr parameter
  (examples/common/gui/presenter.hpp), documented here at its source." → "A `Mutation` relies on it: its error
  handler is attached alongside the caller's."
- `tests/testkit/catch_main.cpp`: drop `src/qt/forms/tests` from the list of suites that own their `main`.
- Every `examples/*/tests/.clang-tidy`, `examples/*/include/.clang-tidy` and `examples/common/testkit/.clang-tidy`:
  `perl -pi -e 's@src/, include/ and gui_lib/@src/, include/ and app/@' <files>`; `examples/crm/include/.clang-tidy`
  line 78's "(core/, dto/, gui/, …)" names `crm`'s own `include/crm/gui/` directory and stays;
  `examples/crm/tests/.clang-tidy`'s measurement "(… examples/common/testkit_src/qml_surface.cpp, 132 -> 128)" is a
  dated count: delete that parenthesis's file.
- `examples/crm/include/crm/gui/crm_schemas.hpp` (Part 9 rewrote the `@file` comment's first sentences): "Lives
  under `include/crm/gui` rather than `gui_lib/`: it has no Qt dependency … a later step's real Qt presenters move it
  there (or link against it) once `gui_lib/*.cpp` files exist to host them." → "Lives under `include/crm/gui`: it has
  no Qt dependency, and crm has no client yet; one would include it from its `app/`." (keep the rest of the
  comment).
- `examples/crm/EXTENSION-BAG-SPIKE.md`: the table row "**Forms client renderer** (`DynamicForm.qml`)" → "**Forms
  client renderer** (the forms engine)"; its evidence cell's `DynamicForm.qml:227-330` citation is the record of the
  measurement — reword to "verified at the time against the then-shipped QML renderer, which iterated
  `schema.properties` generically; the forms engine does the same (`FormModel::fromSchema`)".
- `scripts/coverage.sh`: Part 8 made the rung loop `for _sub in include src gui_lib app; do`; make it
  `for _sub in include src app; do`, and replace the comment Part 8 wrote above it (from "# include/ + src/ are each
  rung's DTOs and models" through "# the frontend smoke tests and by hand.") with

  ```bash
  # include/ + src/ are each rung's DTOs and models (rule 5's own 100% bar);
  # app/ is its controllers and views, held to the same bar: it is real
  # coverage of morph's client stack (morph::reactive, morph::ui, the forms
  # engine), not app-specific domain logic. ui/ is deliberately absent: it is
  # the main() shell (argv, frontend selection), with no unit-testable seam,
  # exercised by the frontend smoke tests and by hand.
  ```

  and in the file's other comments (the ones `git grep -n -i 'presenter\|QML adapter\|test_event_poller'
  scripts/coverage.sh` prints) write "controllers and views" for "presenters and QML adapters" and drop
  `test_event_poller.cpp`, `test_presenter.cpp` from the list of Catch2 files named as examples.
- `codecov.yml`: Parts 8 and 9 rewrote each migrated rung's component comment and replaced its `gui/**` and
  `gui_wasm/**` ignore entries with `ui/**`. What remains is every line `git grep -nE
  'gui_lib|gui_wasm|/gui/|[Pp]resenter|QML adapter' codecov.yml` prints — the header's "models/app/presenter code"
  (→ "models and app code"), the older components' "presenters and QML adapters" (→ "controllers and views"), any
  rung's leftover `gui/**`/`gui_wasm/**` ignore pair (→ one `examples/<rung>/ui/**`), and crm's
  "examples/crm/{include,src,gui_lib}" (→ "examples/crm/{include,src,app}"). A component's `target:` was set against the old directories: leave every
  target as it is, and if a rung's component fails on the pull request, re-measure it per
  `IMPLEMENTATION.md` rule 5's coverage-artifact guidance rather than lowering it blind.
- `scripts/check_automoc_includes.sh` and `cmake/compiler_options.cmake`: keep the quoted moc output (the
  `budget_presenter.hpp` lines are what moc wrote, and `tests/lint/automoc_includes/` pins them); replace "every
  ladder_<rung>_gui_lib -- and every ladder_<rung>_tests that links one -- stopped building at all." with "every
  AUTOMOC target -- the ladder testkit, and every rung's server and test binary -- stopped building at all." and
  "into a build failure -- ladder_<rung>_gui_lib, and with it every ladder_<rung>_tests binary that links one." with
  "into a build failure -- morph_ladder_testkit, and with it every ladder_<rung>_tests binary that links it."
- `cmake/morph_demote_interface_includes.cmake`: "ladder_<rung>_gui_lib and ladder_<rung>_tests" →
  "ladder_<rung>_app and ladder_<rung>_tests".

- [ ] **Step 4: Run the checks to verify they pass**

```bash
/tmp/morph-retire-refs.sh && echo "references retired"
cmake -S . -B build/docs -G Ninja -DMORPH_BUILD_DOCUMENTATION=ON -DMORPH_BUILD_TESTS=OFF -DMORPH_BUILD_EXAMPLES=OFF
cmake --build build/docs --target doc
cmake --build build/all && QT_QPA_PLATFORM=offscreen ctest --test-dir build/all --output-on-failure
pre-commit run --all-files
```

Expected: `references retired`; the Docs build exits 0 (the edited library comments and the deleted
`include/morph/qt/{forms,bridge}` headers leave no dangling Doxygen reference); `build/all` passes (comment-only
changes, and `tests/` still compiles); the hooks pass.

Mutation check: append `// DynamicForm` to `include/morph/forms/views.hpp`; `/tmp/morph-retire-refs.sh` exits 1
printing that line. `git checkout -- include/morph/forms/views.hpp`.

- [ ] **Step 5: Commit**

```bash
git add -A docs include tests scripts cmake codecov.yml examples
git commit -m "wip(retire): every spec, guide and comment names the forms engine and the app layer

Signed-off-by: Christian Parpart <christian@parpart.family>"
```

---

### Task 14: The whole-repository gate — only intended hits remain

**Files:** none new; a hit is fixed in the file it names.

The check spans every tracked file. These are the **allowed** hits, and the only ones:

| Allowed | Why |
|---|---|
| `CHANGELOG.md` | Release history: its entries describe the renderer and the stack as they were when they shipped, and Task 9's "Removed" entry names them. |
| `docs/superpowers/**` | The program's design record (specs and plans) — it names what it retires. |
| `docs/analysis/qml-bridge-boilerplate.md` | A measurement taken at a stated revision; it records that revision's code. |
| `tests/lint/automoc_includes/**` | Synthetic fixtures for the AUTOMOC include lint; their file names are arbitrary. |
| `cmake/compiler_options.cmake`, `scripts/check_automoc_includes.sh` — the lines quoting `…/gui_lib/budget_presenter.hpp` and `"budget_presenter.hpp"` | moc's recorded output, pinned by the lint fixture above. |
| `cmake/compiler_options.cmake` — the line `configured (clang 22.1.8, clang-debug + NET/QT/FORMS_QML/OFFLINE_SQLITE/` | The configuration a recorded measurement was taken on. |
| `examples/crm/include/.clang-tidy` — `(core/, dto/, gui/, …)` | Names crm's own `include/crm/gui/` directory, which is not a rung client directory. |
| `examples/TESTING.md` — the lines naming `` `qml_surface.cpp` `` | Rows of the dated coverage audit (2026-09), which `examples/crm/README.md` cites; a measurement of the directory as it stood. |

- [ ] **Step 1: Write the failing check**

```bash
cat > /tmp/morph-retire-gate.sh <<'SH'
#!/usr/bin/env bash
set -u
pattern='DynamicForm|CollectionView\.qml|WizardView|AppShell\.qml|SlotRegistry|MorphForms|FormsControllerCore|MultiModelFormsControllerCore|GenericModelBridgeCore|MultiModelBridgeCore|OwnedLocalBridge|owned_local_bridge|I18nCatalog|JsonExact|qt_forms|forms_qml|MORPH_BUILD_FORMS_QML|MORPH_INSTALL_QMLDIR|morph_QML_IMPORT_PATH|check_forms_qml_install|src/qt/forms|qt/bridge/|tst_[A-Za-z_]+\.qml|[Pp]resenter|EventPoller|event_poller|qml_surface|QmlSurface|AppContext::Mode|examples/common/gui|id_qml|morph_ladder_gui|ladder_gui|morph_ladder_app([^_]|$)|ladder_app([^_]|$)|gui_lib|gui_wasm|MORPH_LADDER_TESTKIT_GUI_APP|MORPH_LADDER_QML_URI|morph_forms_qml|gui_qml'
git grep -nE "$pattern" -- . ':!CHANGELOG.md' ':!docs/superpowers' ':!docs/analysis/qml-bridge-boilerplate.md' \
        ':!tests/lint/automoc_includes' \
  | grep -vE '^(cmake/compiler_options\.cmake|scripts/check_automoc_includes\.sh):[0-9]+:.*budget_presenter\.hpp' \
  | grep -vE '^cmake/compiler_options\.cmake:[0-9]+:.*NET/QT/FORMS_QML/OFFLINE_SQLITE/' \
  | grep -vE '^examples/crm/include/\.clang-tidy:[0-9]+:.*\(core/, dto/, gui/' \
  | grep -vE '^examples/TESTING\.md:[0-9]+:.*`qml_surface\.cpp`' \
  && exit 1
exit 0
SH
chmod +x /tmp/morph-retire-gate.sh
```

The pattern covers every name the task lists — `DynamicForm`, `CollectionView.qml`, `WizardView`, `SlotRegistry`,
`MorphForms`, `FormsControllerCore`, `MultiModelFormsControllerCore`, `GenericModelBridgeCore`, `qt_forms`,
`forms_qml`, `MORPH_BUILD_FORMS_QML`, `Presenter`, `EventPoller`, `qml_surface`, `AppContext` (the old
`AppContext::Mode` and `examples/common/gui` forms; `ui::AppContext` is the new frontend seam and is not matched),
`gui_lib`, `morph_ladder_gui` — and the related names a reader would also grep for.

- [ ] **Step 2: Run it**

```bash
/tmp/morph-retire-gate.sh; echo "exit $?"
```

Expected after Tasks 9–13: `exit 0` with no output. Any printed line is a reference those tasks did not reach —
most likely in an app's README or code left by Parts 7–9 (`examples/<app>/README.md` describing its old QML
screens, a `.clang-tidy` comment, a CI comment). Fix each in place by Task 13's table (a README's "visible
differences from the old QML screens" paragraph, spec 4 §9, is written in the present tense: what the screen shows
now and what it does not draw — no "was"), then re-run until `exit 0`.

- [ ] **Step 3: (no implementation beyond Step 2's fixes)**

- [ ] **Step 4: Prove the gate detects what it claims**

Mutation checks — each must make the gate exit 1 printing exactly the injected line, then be undone:

```bash
echo '// DynamicForm' >> include/morph/forms/forms.hpp;              /tmp/morph-retire-gate.sh; git checkout -- include/morph/forms/forms.hpp
echo '# gui_lib/' >> examples/pastebin/CMakeLists.txt;               /tmp/morph-retire-gate.sh; git checkout -- examples/pastebin/CMakeLists.txt
echo 'morph::ladder_app' >> examples/common/CMakeLists.txt;          /tmp/morph-retire-gate.sh; git checkout -- examples/common/CMakeLists.txt
echo 'ui::AppContext and morph_ladder_app_common' >> README.md;      /tmp/morph-retire-gate.sh; git checkout -- README.md
```

Expected: the first three print their line and exit 1; the fourth prints nothing and exits 0 (the new names are not
matched). Then `/tmp/morph-retire-gate.sh && echo "only intended hits"`.

- [ ] **Step 5: Commit**

```bash
git add -A
git commit -m "wip(retire): the last references the repository-wide gate found

Signed-off-by: Christian Parpart <christian@parpart.family>"
```

Skip the commit if Step 2 found nothing, and say so in the hand-off.

---

### Task 15: Group verification, squash `retire`, and hand over to "Finishing the branch"

**Files:** none new; fixes land in the files they concern.

- [ ] **Step 1: Strict build and the full suite on `build/all`**

```bash
cmake -S . -B build/all && cmake --build build/all
QT_QPA_PLATFORM=offscreen ctest --test-dir build/all --output-on-failure
```

Expected: every test passes; the configure printed `strict=ON`.

- [ ] **Step 2: No frontend at all** (spec 4 §6: both frontends are optional)

```bash
cmake -S . -B build/nofe -G Ninja -DCMAKE_BUILD_TYPE=Debug -DMORPH_BUILD_QT=ON -DMORPH_BUILD_LADDER=ON \
      -DMORPH_LADDER_RUNGS=all -DMORPH_BUILD_BANK_EXAMPLE=ON -DMORPH_BUILD_TUI=OFF -DMORPH_BUILD_QT_QUICK=OFF
cmake --build build/nofe && QT_QPA_PLATFORM=offscreen ctest --test-dir build/nofe --output-on-failure
```

Expected: builds and passes; no app binary exists (`morph_forms_app`, `bank`, the rung binaries), every app library
and its tests do.

- [ ] **Step 3: Sanitizers, tidy, docs, install**

```bash
cmake --preset clang-asan -DMORPH_BUILD_QT=ON -DMORPH_BUILD_LADDER=ON -DMORPH_LADDER_RUNGS=all
cmake --build --preset clang-asan
bash scripts/check_sanitizer_instrumentation.sh --binary build/clang-asan/examples/common/ladder_common_tests asan
QT_QPA_PLATFORM=offscreen ASAN_OPTIONS=detect_leaks=0 ctest --test-dir build/clang-asan -L ladder -LE stress
```

then CONTRIBUTING's `clang-tidy-diff` recipe over `origin/master...HEAD` (file count asserted non-zero; the
configure as `ci.yml`'s `clang-tidy` job now has it, without `MORPH_BUILD_FORMS_QML`), the Docs build (Task 13
Step 4's two commands), and `bash scripts/test_check_install_export.sh && bash scripts/check_install_export.sh`.

Expected: all clean. `ladder_common_tests` lost five self-test files; the instrumentation check is what makes its
clean run mean something.

- [ ] **Step 4: Commit any fixes**

```bash
git add -A
git commit -m "wip(retire): fixes from the whole-group gates

Signed-off-by: Christian Parpart <christian@parpart.family>"
```

Skip the commit if there was nothing to fix, and say so in the hand-off.

- [ ] **Step 5: Squash the group into its one commit**

Follow the master plan's "Squashing a part" procedure with `key=retire` and this message (add a paragraph before the
trailer only for a deviation from this plan — what it said, what was done, why — e.g. a hit one of the
pre-condition greps of Tasks 9, 11 or 14 found and how it was resolved):

```text
forms, examples: retire the QML renderer and the Qt client stack

Removes the MorphForms QML module (src/qt/forms, its QuickTest suite),
the qt/forms and qt/bridge controller cores, the qt_forms, forms_qml and
forms_qmlplugin components and MORPH_BUILD_FORMS_QML: forms render
through the C++ forms engine on morph::tui or morph::qt_quick. Removes
examples/common's presenter, EventPoller, AppContext and QML-surface
audit, and morph_add_rung's gui/, gui_lib/, gui/qml/ and gui_wasm/
conventions, which now fail the configure when a rung still has one.

Signed-off-by: Christian Parpart <christian@parpart.family>
```

Expected last line of the procedure: `git log --oneline master..HEAD` shows the master plan's 18 commits, in order,
ending with `forms, examples: retire the QML renderer and the Qt client stack`.

- [ ] **Step 6: Hand over**

This was the program's last part. Continue with the master plan's "Finishing the branch"
(`docs/superpowers/plans/2026-10-04-declarative-ui-tui.md`): the whole-branch gates on `build/all` and Linux CI's
configurations, the no-frontend build, the WebAssembly configurations of `wasm-ladder.yml` and `wasm-demo.yml`,
`git rebase --exec "cmake --build build/all && ctest --test-dir build/all" master` so every commit stands alone, the
history check, and — each confirmed with the user first — pushing the branch, opening the pull request, and
deleting core-cpp's abandoned `feature/reactive-ui`.

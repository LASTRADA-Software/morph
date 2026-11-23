# Guided Tours — Framework Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** An application's server declares guided tours in its app document — general ones and several per screen —
and every client shows them with no per-application code: a **Tours** button lists the active screen's tours before
the general ones, a document can start one with the host command `tour.start`, and a running tour opens each step's
screen, dims the window around the step's target (Qt Quick) or marks it (terminal), and shows a card with Back, Next
and End. A tour that names something the bundle does not have is refused when the bundle is built.

**Architecture:** Data first, then the seam, then the drivers. The app document gains a `tours` member, lowered by
`AppRoot::tours()` into one computed value whose texts follow the locale; the author API writes it; `UiRegistry::build`
checks every name a tour or a literal `tour.start` uses. The backend contract gains `SpotlightWidget`, which resolves
a target by test id in a per-backend index and tracks it; `RecordingBackend`, Qt Quick and the terminal implement it,
proved by shared conformance cases. `ui::TourController` runs one tour at a time over a small `TourShell` interface,
testable with a fake. The card is a client-generated ui/1 document (`kTourCardDocument`, id `morph.tour`) bound to the
controller through `tour.*` host values and commands. The shell frame draws the Tours button (and a Tours submenu in
the compact form) and owns the spotlight; the generic client wires all of it per frame.

**Tech Stack:** C++23, glaze (JSON), Catch2, CMake + Ninja, clang (local), GCC 15 and MSVC in CI; Qt 6 Quick (QML) and
core::tui for the two real backends; `RecordingBackend` for headless tests.

Design: `docs/superpowers/specs/2026-11-23-guided-tours-design.md` (authoritative; the deviations the code forced are
listed in "Deviations from the design" at the end of this plan). Scope: the framework pull request only —
`include/morph`, `src`, `tests`, `docs/spec`. The lims proof is a separate, later plan.

## Global Constraints

- Public symbols carry complete Doxygen `@brief`/`@param`/`@tparam`/`@return`. The Docs workflow runs with
  `WARN_AS_ERROR = FAIL_ON_WARNINGS`, and clang's `-Wdocumentation` catches a misplaced `@param` that GCC misses: check
  each header through a translation unit that includes it (the tests below do).
- Comments and docs state what the code does now and why. No history, no issue numbers, no commit hashes (AGENTS.md).
  Every `docs/spec` page a task changes is updated in that task, in the present tense.
- Never use `slots`, `signals` or `emit` as identifiers under `include/morph`: they break only Qt translation units.
  Task 10 greps the added lines; every target, Qt Quick included, is built.
- A dependent default template argument needs `typename` (MSVC C4346). This plan adds none; keep it that way.
- No `auto*` or `std::expected` fix-its from clang-tidy accepted as offered: write the types out.
- Format every changed C++ file with the repository's `.clang-format` (120 columns) before each commit:
  `clang-format -i <files>`. Some code in this plan is wider than 120 columns; the formatter, not the plan, decides the
  line breaks.
- Every compile (cmake configure, cmake build, a g++ sweep) runs under the build lock with `-j5`:
  `flock "$LOCK" cmake --build build/clang-debug -j5 --target <t>`. `$LOCK` is `/tmp/morph-build.lock`; a second
  parallel lane is given `/tmp/morph-build-2.lock`. Hold the lock for one command at a time, never across edits.
  `ctest` runs without the lock.
- Builds and test runs are foreground commands; give a heavy compile a tool timeout near the 600 s ceiling rather than a
  short cap that restarts it.
- Let `CompileCache.cmake` pick the launcher. Never pass `-DCMAKE_CXX_COMPILER_LAUNCHER`.
- Tests run through ctest, never by calling a binary with a test name: `ctest --test-dir build/clang-debug -R '<regex>'
  --output-on-failure`. Test names are ASCII only (Windows ctest mangles anything else) and never start with `--`.
- Verify rather than assert: every test names, in a comment above it, the mutation that makes it fail. Each task's
  mutation step applies that mutation, sees the test fail, and restores the file. After restoring a file from a backup,
  `touch` it, or ninja keeps the mutant object.
- Parallel lanes (marked on each task) work in a worktree of their own, cut by hand from `feature/guided-tours`
  (`git worktree add ../morph-tours-<lane> -b tours-<lane> feature/guided-tours`), with their own
  `build/clang-debug` configured as in Task 0. The shared `CMakeLists.txt` and `tests/CMakeLists.txt` lists are edited at
  the anchors each task names, so a rebase conflict there is one line.
- Names fixed across tasks: the card's id is `morph.tour` (`ui::kTourCardId`); the button is `shell_tours`; the
  spotlight the frame makes is `shell_spotlight`; host values `tour.title`, `tour.stepTitle`, `tour.body`,
  `tour.position`, `tour.count`, `tour.waiting`, `tour.waitingFor`, `tour.canBack`, `tour.last`; host commands
  `tour.next`, `tour.back`, `tour.end` (card only) and `tour.start` (every document).

## Task order and lanes

| Wave | Tasks (may run in parallel within a wave) | Needs |
|---|---|---|
| 1 | Task 1 (tours in the app document) · Task 3 (spotlight in the backend contract) | Task 0 |
| 2 | Task 2 (registry checks) · Task 4 (Qt Quick spotlight) · Task 5 (terminal spotlight) | 2 needs 1; 4 and 5 need 3 |
| 3 | Task 6 (controller) · Task 8 (shell frame: Tours button, spotlight slot) | 6 needs 1 and 3; 8 needs 3 |
| 4 | Task 7 (card and `tour.*` vocabulary) | 6 |
| 5 | Task 9 (generic client wiring) | 1, 2, 6, 7, 8 |
| 6 | Task 10 (spec index, changelog, sweeps) | all |

Run at most three lanes at once.

## File Structure

| File | Responsibility |
|---|---|
| `include/morph/ui/document.hpp`, `src/ui/document.cpp` (modify) | `TourDecl`, `TourStepDecl`, `kMaxTourSteps`, `kMaxTourIdBytes`, `Document::tours`; the parser reads and shape-checks `tours`, refuses it on a screen, and type-checks its texts in the app scope |
| `include/morph/ui/interpreter.hpp`, `src/ui/interpreter.cpp` (modify) | `Tour`, `TourStep`, `AppRoot::tours()` |
| `include/morph/ui/author.hpp` (modify) | `author::Tour`, `author::TourStep`, `author::withTours` |
| `include/morph/ui/client_ids.hpp` (create) | `kTourCardId`, `ReservedClientId`, `kReservedClientIds`, `reservedClientId` |
| `include/morph/ui/served_document.hpp` (modify) | `servedScreenRefusal` over the reserved ids |
| `src/ui/tour_checks.hpp`, `src/ui/tour_checks.cpp` (create) | `detail::checkTours`: what a tour names against the registered screens, and literal `tour.start` ids |
| `include/morph/ui/service.hpp`, `src/ui/service.cpp` (modify) | `checkNewEntry` over the reserved ids; `build` runs the tour checks |
| `include/morph/ui/backend.hpp`, `src/ui/backend.cpp` (modify) | `SpotlightWidget`, `IViewBackend::createSpotlight` (default throws) |
| `include/morph/ui/testing/recording_backend.hpp` (modify) | store listeners, `detail::shownInStore`, `detail::FakeSpotlight`, the spotlight's key order in `press` |
| `include/morph/ui/testing/backend_conformance.hpp` (modify) | `ConformanceProbe::targetOf`, five cases, `spotlightConformanceCases()` |
| `tests/ui_echoing_backend.hpp` (modify) | `createSpotlight` forwarded |
| `src/qt_quick/context.hpp`, `src/qt_quick/context.cpp` (modify) | `WidgetIndex`: a context's live widgets by test id, with listeners; the process's contexts |
| `src/qt_quick/widget.hpp`, `src/qt_quick/widget.cpp` (modify) | widgets join their context's index; `findWidget`/`liveWidgets` read the indexes |
| `src/qt_quick/kinds.hpp`, `src/qt_quick/spotlight.cpp`, `src/qt_quick/qml/MorphSpotlight.qml` (create/modify) | `SpotlightImpl` and its QML: scrim, ring, card placement, scroll into view, pointer and key handling |
| `src/qt_quick/qml/MorphWidget.qml` (modify) | `takeChord(key, modifiers)`, which the spotlight calls |
| `include/morph/qt_quick/backend.hpp`, `src/qt_quick/backend.cpp`, `src/qt_quick/CMakeLists.txt` (modify) | `createSpotlight` |
| `src/tui/context.hpp`, `src/tui/context.cpp`, `src/tui/widget.hpp`, `src/tui/widget.cpp` (modify) | widget sequence numbers, `carrying`, spotlight hooks: key order, pass-through, Tab target, reverse video |
| `src/tui/container_widgets.hpp`, `src/tui/container_widgets.cpp` (modify) | `OverlayCore::setAnchor`/`setDismissible`, `SpotlightImpl` |
| `include/morph/tui/backend.hpp`, `src/tui/backend.cpp` (modify) | `createSpotlight` |
| `include/morph/ui/navigator.hpp`, `src/ui/navigator.cpp` (modify) | `Navigator::reveal` |
| `include/morph/ui/tours.hpp`, `src/ui/tours.cpp` (create) | `TourShell`, `TourView`, `TourControllerOptions`, `OfferedTours`, `offeredTours`, `tourTargetFor`, `TourController` |
| `include/morph/ui/host_vocabulary.hpp` (modify) | `HostCommand::refusal` |
| `include/morph/ui/tour_card.hpp` (create) | `kTourCardDocument`, `tourCardDocument()`, `TourCardBinding` |
| `include/morph/ui/shell_parts_view.hpp` (modify) | `ShellTourView`, `ShellTourGroup`, `ShellToursView` |
| `include/morph/ui/shell_frame.hpp`, `src/ui/shell_frame.cpp` (modify) | the Tours button and compact submenu, `setTours`, `setOnTour`, `spotlight()` |
| `src/ui/shell_parts_model.cpp` (modify) | built-in texts `morph.shell.tours`, `morph.shell.toursName`, `morph.shell.toursGeneral` |
| `include/morph/ui/frontend.hpp` (modify) | `MountedShell::startTour` |
| `src/client/shell.cpp` (modify) | the generic client's wiring |
| `CMakeLists.txt` (modify) | `src/ui/tour_checks.cpp`, `src/ui/tours.cpp`; headers `client_ids.hpp`, `tours.hpp`, `tour_card.hpp` |
| `tests/test_ui_tours_document.cpp`, `tests/test_ui_tours_registry.cpp`, `tests/test_ui_spotlight_recording.cpp`, `tests/test_ui_tours_controller.cpp`, `tests/test_ui_tour_card.cpp`, `tests/test_ui_shell_tours.cpp`, `tests/ui_tours_support.hpp` (create) | headless tests |
| `tests/test_ui_conformance.cpp` (modify) | the recording probe's `targetOf`; the spotlight cases |
| `tests/qt_quick/test_qt_quick_spotlight.cpp` (create), `tests/qt_quick/test_qt_quick_conformance.cpp`, `tests/qt_quick/CMakeLists.txt` (modify) | Qt Quick |
| `tests/tui/test_tui_spotlight.cpp` (create), `tests/tui/test_tui_conformance.cpp`, `tests/tui/CMakeLists.txt` (modify) | terminal |
| `tests/client/tours_app.hpp`, `tests/client/tours_app.cpp`, `tests/client/test_tours.cpp` (create); `tests/client/test_shell.cpp`, `tests/client/test_custom_screens.cpp`, `tests/client/CMakeLists.txt` (modify) | the generic client |
| `docs/spec/ui/tours.md` (create) | the reference: declaration, controller, card, client |
| `docs/spec/ui/document.md`, `interpreter.md`, `author.md`, `service.md`, `host_vocabulary.md`, `backend_contract.md`, `shell_frame.md`, `shell_parts.md`, `docs/spec/qt_quick/backend.md`, `docs/spec/tui/frontend.md`, `docs/spec/client/client.md`, `docs/spec/README.md`, `CHANGELOG.md` (modify) | the specs this changes |

---

### Task 0: Configure the worktree build

**Files:** none.

- [ ] **Step 1: Configure with Qt Quick and the terminal frontend**

Run:
```bash
cd /home/yaraslau/repo/morph-tours
flock /tmp/morph-build.lock cmake --preset clang-debug -DMORPH_BUILD_QT=ON -DMORPH_BUILD_QT_QUICK=ON -DMORPH_BUILD_TUI=ON
```
Expected: `-- Build files have been written to: /home/yaraslau/repo/morph-tours/build/clang-debug`, and
`grep -E 'MORPH_BUILD_(QT_QUICK|TUI):BOOL=ON' build/clang-debug/CMakeCache.txt` prints both lines.

- [ ] **Step 2: Build the test binaries once, so later steps measure only this plan's changes**

Run:
```bash
flock /tmp/morph-build.lock cmake --build build/clang-debug -j5 --target morph_tests morph_client_tests morph_qt_quick_tests morph_tui_tests
```
Expected: ends without errors.

- [ ] **Step 3: Record the baseline**

Run: `ctest --test-dir build/clang-debug -R 'conformance|shell frame|Client shell' --output-on-failure | tail -3`
Expected: `100% tests passed`. A failure here predates this plan: stop and report it.

---
### Task 1: Tours in the app document, `AppRoot::tours()`, and the author API

**Lane:** wave 1, parallel with Task 3 (disjoint files).

**Files:**
- Modify: `include/morph/ui/document.hpp` (new structs and constants before `struct Document`, ~line 555; member
  `tours` after `navigation`, ~line 609)
- Modify: `src/ui/document.cpp` (`DocumentParser::parse`, after `doc.navigation = navigation(*object, doc.id);`
  ~line 684; new static helpers after `navigation(...)` ~line 936; the checker, after the `menuLabel` check ~line 2188)
- Modify: `include/morph/ui/interpreter.hpp` (`TourStep`, `Tour` after `NavigationBadge` ~line 2335; `AppRoot::tours()`
  after `navigationBadges()` ~line 2693; member `_tours` after `_navigationBadges` ~line 2756)
- Modify: `src/ui/interpreter.cpp` (`AppRoot::~AppRoot` ~line 1370, accessor after `navigationBadges()` ~line 1388,
  `Interpreter::mountApp` after the `_navigationBadges` computed ~line 8930)
- Modify: `include/morph/ui/author.hpp` (before `// ── Screens and registration`, ~line 6011)
- Test: `tests/test_ui_tours_document.cpp` (create); `tests/CMakeLists.txt` (add after `test_ui_host_vocabulary.cpp`)
- Docs: `docs/spec/ui/document.md`, `docs/spec/ui/interpreter.md`, `docs/spec/ui/author.md`

**Interfaces:**
- Consumes: `ui::loadDocumentText(std::string_view, LoadOptions const& = {}) -> std::expected<Document, Refusal>`;
  `ui::valueFromJson(Json const&) -> std::expected<Value, std::string>`; `detail::evalIn(RtScope const&, Bindings
  const*, ExprPtr const&) -> Value`; `detail::textOf(Value const&, FormatContext const&) -> std::string`;
  `author::Handle<std::string>` (`present()`, `json()`), `author::detail::put(Json&, std::string_view, Json)`,
  `ui::parseJson(std::string_view) -> std::expected<Json, std::string>`, `ui::valueToJson(Value const&) -> Json`.
- Produces:
  - `inline constexpr std::size_t ui::kMaxTourSteps = 50;` and `inline constexpr std::size_t ui::kMaxTourIdBytes = 64;`
  - `struct ui::TourStepDecl { std::string path; ExprPtr title; ExprPtr body; std::string screen; ValueRecord params;
    std::string target; std::string advanceOn; };`
  - `struct ui::TourDecl { std::string path; std::string id; std::string screen; ExprPtr title; ExprPtr summary;
    std::vector<TourStepDecl> steps; };`
  - `std::vector<TourDecl> ui::Document::tours;`
  - `struct ui::TourStep { std::string title; std::string body; std::string screen; ValueRecord params; std::string
    target; std::string advanceOn; bool operator==(TourStep const&) const = default; };`
  - `struct ui::Tour { std::string id; std::string screen; std::string title; std::string summary;
    std::vector<TourStep> steps; bool operator==(Tour const&) const = default; };`
  - `reactive::Computed<std::vector<Tour>> const& ui::AppRoot::tours() const;`
  - `struct ui::author::TourStep { Handle<std::string> title; Handle<std::string> body; std::string screen; ValueRecord
    params; std::string target; std::string advanceOn; };`
  - `struct ui::author::Tour { std::string id; std::string screen; Handle<std::string> title; Handle<std::string>
    summary; std::vector<TourStep> steps; };`
  - `[[nodiscard]] inline Json ui::author::tourJson(Tour const& tour);` (throws `AuthoringError` for an absent title)
  - `[[nodiscard]] inline std::expected<std::string, Refusal> ui::author::withTours(std::string_view appDocument,
    std::vector<Tour> const& tours);`

- [ ] **Step 1: Write the failing tests** (`tests/test_ui_tours_document.cpp`)

```cpp
// SPDX-License-Identifier: Apache-2.0
//
// Guided tours in the app document (docs/spec/ui/tours.md, "The declaration"): the loader reads `tours` and refuses a
// malformed one where it is malformed, a screen declares none, `AppRoot::tours` evaluates each text in the app scope
// and again after a change of locale, and `author::withTours` writes a member the loader reads.

#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <morph/ui/author.hpp>
#include <morph/ui/document.hpp>
#include <morph/ui/interpreter.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "ui_interpreter_support.hpp"

namespace ui = morph::ui;

namespace {

constexpr std::string_view kApp = R"({
  "vocab": "ui/1", "id": "app",
  "tours": [
    { "id": "getting-started",
      "title": { "t": "tours.start.title", "fallback": "Getting started" },
      "summary": "The shell in four steps",
      "steps": [
        { "target": "shell_search", "title": { "t": "tours.start.search", "fallback": "Search" },
          "body": "Find anything." },
        { "target": "nav_samples", "advanceOn": { "screen": "samples" }, "title": "Open Samples" },
        { "screen": "samples", "params": { "page": 2 }, "target": "samples_table", "title": "The table" },
        { "title": "Done", "later": true } ] },
    { "id": "register-sample", "screen": "samples", "title": "Register a sample",
      "steps": [ { "target": "action_new", "title": "New" } ] } ] })";

/// The app document with @p tours as its `tours` member.
std::string appWith(std::string_view tours) { return R"({"vocab":"ui/1","id":"app","tours":)" + std::string{tours} + "}"; }

/// @p count steps, each with only a title.
std::string manySteps(std::size_t count) {
    std::string out;
    for (std::size_t index = 0; index < count; ++index) {
        out += index == 0 ? "" : ",";
        out += R"({"title":"S"})";
    }
    return out;
}

}  // namespace

// Mutation: skip `tours` in `DocumentParser::parse` (the document loads with no tours).
TEST_CASE("tours document: the app document's tours load with their steps", "[ui][tours]") {
    auto loaded = ui::loadDocumentText(kApp);
    REQUIRE(loaded.has_value());
    REQUIRE(loaded->tours.size() == 2);
    ui::TourDecl const& start = loaded->tours.front();
    CHECK(start.id == "getting-started");
    CHECK(start.path == "/tours/0");
    CHECK(start.screen.empty());
    REQUIRE(start.summary != nullptr);
    REQUIRE(start.steps.size() == 4);
    CHECK(start.steps.at(0).target == "shell_search");
    CHECK(start.steps.at(0).body != nullptr);
    CHECK(start.steps.at(1).advanceOn == "samples");
    CHECK(start.steps.at(2).screen == "samples");
    CHECK(start.steps.at(2).params.at("page") == ui::Value{2});
    // An unknown step member is ignored.
    CHECK(start.steps.at(3).target.empty());
    CHECK(start.steps.at(3).body == nullptr);
    CHECK(loaded->tours.at(1).screen == "samples");
}

// Mutation: drop any one check from `tours`, `tourOf` or `tourStepOf` (its row then loads, or is refused elsewhere).
TEST_CASE("tours document: a malformed tour is refused where it is malformed", "[ui][tours]") {
    struct Row {
        std::string name;
        std::string text;
        std::string path;
        std::string reason;
    };
    std::vector<Row> const rows{
        {"a screen's tours", R"({"vocab":"ui/1","id":"lims.samples","tours":[],"view":{"kind":"text","text":"x"}})",
         "/tours", "the app document's"},
        {"not an array", appWith("{}"), "/tours", "an array"},
        {"an empty id", appWith(R"([{"id":"","title":"T","steps":[{"title":"S"}]}])"), "/tours/0/id", "[a-z0-9-]"},
        {"an upper-case id", appWith(R"([{"id":"Start","title":"T","steps":[{"title":"S"}]}])"), "/tours/0/id",
         "[a-z0-9-]"},
        {"a long id", appWith(R"([{"id":")" + std::string(65, 'a') + R"(","title":"T","steps":[{"title":"S"}]}])"),
         "/tours/0/id", "at most 64"},
        {"a repeated id",
         appWith(R"([{"id":"a","title":"T","steps":[{"title":"S"}]},{"id":"a","title":"U","steps":[{"title":"S"}]}])"),
         "/tours/1/id", "twice"},
        {"no title", appWith(R"([{"id":"a","steps":[{"title":"S"}]}])"), "/tours/0", "`title` is required"},
        {"no steps", appWith(R"([{"id":"a","title":"T","steps":[]}])"), "/tours/0/steps", "one step or more"},
        {"too many steps", appWith(R"([{"id":"a","title":"T","steps":[)" + manySteps(51) + "]}]"), "/tours/0/steps",
         "at most 50"},
        {"a step without a title", appWith(R"([{"id":"a","title":"T","steps":[{"body":"B"}]}])"), "/tours/0/steps/0",
         "`title` is required"},
        {"a computed param",
         appWith(R"([{"id":"a","title":"T","steps":[{"title":"S","screen":"x","params":{"p":{"ref":"y"}}}]}])"),
         "/tours/0/steps/0/params/p", "a literal"},
        {"an advanceOn without a screen", appWith(R"([{"id":"a","title":"T","steps":[{"title":"S","advanceOn":{}}]}])"),
         "/tours/0/steps/0/advanceOn", "`screen` is required"},
        {"a must-understand member", appWith(R"([{"id":"a","title":"T","steps":[{"title":"S","!pause":1}]}])"),
         "/tours/0/steps/0/!pause", "must-understand"},
        {"a text that names nothing", appWith(R"([{"id":"a","title":{"ref":"nothing"},"steps":[{"title":"S"}]}])"),
         "/tours/0/title", "nothing"},
    };
    for (Row const& row : rows) {
        INFO(row.name);
        auto const loaded = ui::loadDocumentText(row.text);
        REQUIRE_FALSE(loaded.has_value());
        CHECK(loaded.error().path == row.path);
        CHECK(loaded.error().reason.contains(row.reason));
    }
}

// Mutation: evaluate each text once in `mountApp`, outside the computed value (the German title never shows).
TEST_CASE("tours document: AppRoot::tours evaluates the texts in the app scope and again after a change of locale",
          "[ui][tours]") {
    ui::testing::Harness h;
    h.session.setTranslations([](std::string_view key, std::string_view locale) -> std::optional<std::string> {
        if (!locale.starts_with("de")) {
            return std::nullopt;
        }
        if (key == "tours.start.title") {
            return std::string{"Erste Schritte"};
        }
        if (key == "tours.start.search") {
            return std::string{"Suche"};
        }
        return std::nullopt;
    });
    auto root = h.interpreter.mountApp(ui::testing::loadOrFail(kApp));
    REQUIRE(root.has_value());
    std::vector<ui::Tour> tours = (*root)->tours().get();
    REQUIRE(tours.size() == 2);
    CHECK(tours.front().id == "getting-started");
    CHECK(tours.front().title == "Getting started");
    CHECK(tours.front().summary == "The shell in four steps");
    CHECK(tours.front().steps.front().title == "Search");
    CHECK(tours.front().steps.front().body == "Find anything.");
    CHECK(tours.front().steps.at(1).advanceOn == "samples");
    CHECK(tours.front().steps.at(2).params.at("page") == ui::Value{2});
    CHECK(tours.at(1).screen == "samples");
    h.session.setLocale("de");
    h.settle();
    tours = (*root)->tours().get();
    CHECK(tours.front().title == "Erste Schritte");
    CHECK(tours.front().steps.front().title == "Suche");
}

// Mutation: leave `target` out of `author::tourJson` (the loaded steps have none).
TEST_CASE("tours document: author::withTours writes a tours member the loader reads", "[ui][tours][author]") {
    namespace author = ui::author;
    std::vector<author::Tour> const tours{author::Tour{
        .id = "getting-started",
        .screen = {},
        .title = author::t("tours.start.title", "Getting started"),
        .summary = "The shell",
        .steps = {author::TourStep{.title = "Search", .body = "Find anything.", .target = "shell_search"},
                  author::TourStep{.title = "Open Samples", .target = "nav_samples", .advanceOn = "samples"},
                  author::TourStep{.title = "The table",
                                   .screen = "samples",
                                   .params = {{"page", ui::Value{2}}},
                                   .target = "samples_table"}}}};
    auto const written = author::withTours(R"({"vocab":"ui/1","id":"app"})", tours);
    REQUIRE(written.has_value());
    auto const loaded = ui::loadDocumentText(*written);
    REQUIRE(loaded.has_value());
    REQUIRE(loaded->tours.size() == 1);
    REQUIRE(loaded->tours.front().steps.size() == 3);
    CHECK(loaded->tours.front().steps.at(0).target == "shell_search");
    CHECK(loaded->tours.front().steps.at(1).advanceOn == "samples");
    CHECK(loaded->tours.front().steps.at(2).params.at("page") == ui::Value{2});
    // A document that declares tours already, and one that is not an object, are refused.
    CHECK_FALSE(author::withTours(*written, tours).has_value());
    CHECK_FALSE(author::withTours("[]", tours).has_value());
}
```

Add `test_ui_tours_document.cpp` to `tests/CMakeLists.txt` on the line after `test_ui_host_vocabulary.cpp`.

- [ ] **Step 2: Run the tests to verify they fail**

Run: `flock /tmp/morph-build.lock cmake --build build/clang-debug -j5 --target morph_tests`
Expected: compile errors — `TourDecl` is not a member of `morph::ui`, `AppRoot` has no member `tours`,
`author::withTours` is not declared.

- [ ] **Step 3: Declare the parsed tours** (`include/morph/ui/document.hpp`, before `struct Document`)

```cpp
/// @brief The most steps a guided tour has.
inline constexpr std::size_t kMaxTourSteps = 50;

/// @brief The longest a guided tour's id is, in characters (`[a-z0-9-]`, so bytes too).
inline constexpr std::size_t kMaxTourIdBytes = 64;

/// @brief One step of a guided tour, as the app document declares it (`docs/spec/ui/tours.md`).
struct TourStepDecl {
    /// @brief The step's JSON Pointer, where a refusal about it points.
    std::string path;
    /// @brief The step's title: a text expression in the app scope.
    ExprPtr title;
    /// @brief The step's body: a text expression in the app scope; `nullptr` when none is declared.
    ExprPtr body;
    /// @brief The screen the step opens before it shows; empty for none.
    std::string screen;
    /// @brief The params it opens that screen with: literal values.
    ValueRecord params;
    /// @brief The test id of the part the step rings; empty for a card with nothing ringed.
    std::string target;
    /// @brief The screen whose opening moves the tour on; empty for a step that waits for Next.
    std::string advanceOn;
};

/// @brief A guided tour, as the app document declares it.
struct TourDecl {
    /// @brief The tour's JSON Pointer, where a refusal about it points.
    std::string path;
    /// @brief Its id: `[a-z0-9-]+`, at most `kMaxTourIdBytes`, unique among the app's tours.
    std::string id;
    /// @brief The screen it belongs to; empty for a general tour, offered on every screen.
    std::string screen;
    /// @brief Its title: a text expression in the app scope.
    ExprPtr title;
    /// @brief Its summary: a text expression in the app scope; `nullptr` when none is declared.
    ExprPtr summary;
    /// @brief Its steps, one or more and at most `kMaxTourSteps`.
    std::vector<TourStepDecl> steps;
};
```

and in `struct Document`, after `navigation`:

```cpp
    /// @brief The app document's guided tours, in declaration order; empty for a screen, which declares none.
    std::vector<TourDecl> tours;
```

- [ ] **Step 4: Parse and check them** (`src/ui/document.cpp`)

In `DocumentParser::parse`, after `doc.navigation = navigation(*object, doc.id);`:

```cpp
        doc.tours = tours(*object, doc.id);
```

After `navigation(...)`, add:

```cpp
    // The app document's guided tours, each checked for shape here and for the names it uses when the bundle is
    // built; a screen declares none.
    [[nodiscard]] static std::vector<TourDecl> tours(Json::object_t const& object, std::string_view documentId) {
        std::vector<TourDecl> out;
        Json const* const declared = member(object, "tours");
        if (declared == nullptr) {
            return out;
        }
        if (documentId != "app") {
            refuse("/tours", "`tours` is the app document's; a screen has none");
        }
        auto const* const array = std::get_if<Json::array_t>(&declared->data);
        if (array == nullptr) {
            refuse("/tours", "`tours` is an array of tours");
        }
        std::set<std::string, std::less<>> ids;
        for (std::size_t index = 0; index < array->size(); ++index) {
            std::string const path = pointerAppend("/tours", index);
            TourDecl tour = tourOf(array->at(index), path);
            if (!ids.insert(tour.id).second) {
                refuse(pointerAppend(path, "id"), "the tour id `" + tour.id + "` is declared twice");
            }
            out.push_back(std::move(tour));
        }
        return out;
    }

    // A tour id: lower-case letters, digits and dashes, at most `kMaxTourIdBytes`.
    [[nodiscard]] static bool isTourId(std::string_view id) {
        return !id.empty() && id.size() <= kMaxTourIdBytes && std::ranges::all_of(id, [](char chr) {
                   return (chr >= 'a' && chr <= 'z') || (chr >= '0' && chr <= '9') || chr == '-';
               });
    }

    [[nodiscard]] static TourDecl tourOf(Json const& json, std::string const& path) {
        auto const& fields = objectOf(json, path, "a tour is a record of `id`, `title` and `steps`");
        refuseMustUnderstand(fields, path);
        TourDecl tour;
        tour.path = path;
        tour.id = text(fields, "id", path, true);
        if (!isTourId(tour.id)) {
            refuse(pointerAppend(path, "id"), "a tour id is [a-z0-9-]+, at most " + std::to_string(kMaxTourIdBytes) +
                                                  " characters; `" + tour.id + "` is not");
        }
        tour.screen = text(fields, "screen", path, false);
        tour.title = optionalExpr(fields, "title", path);
        if (tour.title == nullptr) {
            refuse(path, "`title` is required");
        }
        tour.summary = optionalExpr(fields, "summary", path);
        Json const* const steps = member(fields, "steps");
        if (steps == nullptr) {
            refuse(path, "`steps` is required");
        }
        std::string const where = pointerAppend(path, "steps");
        auto const* const array = std::get_if<Json::array_t>(&steps->data);
        if (array == nullptr) {
            refuse(where, "`steps` is an array of steps");
        }
        if (array->empty()) {
            refuse(where, "a tour has one step or more");
        }
        if (array->size() > kMaxTourSteps) {
            refuse(where, "a tour has at most " + std::to_string(kMaxTourSteps) + " steps");
        }
        for (std::size_t index = 0; index < array->size(); ++index) {
            tour.steps.push_back(tourStepOf(array->at(index), pointerAppend(where, index)));
        }
        return tour;
    }

    [[nodiscard]] static TourStepDecl tourStepOf(Json const& json, std::string const& path) {
        auto const& fields = objectOf(json, path, "a tour step is a record with a `title`");
        refuseMustUnderstand(fields, path);
        TourStepDecl step;
        step.path = path;
        step.title = optionalExpr(fields, "title", path);
        if (step.title == nullptr) {
            refuse(path, "`title` is required");
        }
        step.body = optionalExpr(fields, "body", path);
        step.screen = text(fields, "screen", path, false);
        step.target = text(fields, "target", path, false);
        if (Json const* const params = member(fields, "params")) {
            std::string const where = pointerAppend(path, "params");
            for (auto const& [name, value] : objectOf(*params, where, "`params` is a record of literal values")) {
                std::string const at = pointerAppend(where, name);
                if (std::holds_alternative<Json::object_t>(value.data) ||
                    std::holds_alternative<Json::array_t>(value.data)) {
                    refuse(at, "a tour step's param is a literal: a string, a whole number, a flag or null");
                }
                auto converted = valueFromJson(value);
                if (!converted) {
                    refuse(at, "a tour step's param is a literal: " + converted.error());
                }
                step.params.insert_or_assign(name, std::move(*converted));
            }
        }
        if (Json const* const advance = member(fields, "advanceOn")) {
            std::string const where = pointerAppend(path, "advanceOn");
            auto const& on = objectOf(*advance, where, R"(`advanceOn` is a record: {"screen": id})");
            refuseMustUnderstand(on, where);
            step.advanceOn = text(on, "screen", where, true);
        }
        return step;
    }
```

In the checker, after `if (_doc->menuLabel) { checkExpr(*_doc->menuLabel, root); }`:

```cpp
        // A tour's texts are read in the app scope, as the navigation badges are.
        for (TourDecl const& tour : _doc->tours) {
            checkExpr(*tour.title, root);
            if (tour.summary) {
                checkExpr(*tour.summary, root);
            }
            for (TourStepDecl const& step : tour.steps) {
                checkExpr(*step.title, root);
                if (step.body) {
                    checkExpr(*step.body, root);
                }
            }
        }
```

(`<set>` and `<variant>` are already included by `document.cpp`; `<variant>` comes through `value.hpp`. Add
`#include <variant>` if the compiler asks.)

- [ ] **Step 5: Lower them in the app root**

`include/morph/ui/interpreter.hpp`, after `struct NavigationBadge`:

```cpp
/// @brief One step of a guided tour, its texts evaluated now, as `AppRoot::tours` answers it.
struct TourStep {
    /// @brief The step's title.
    std::string title;
    /// @brief Its body; empty for none.
    std::string body;
    /// @brief The screen it opens before it shows; empty for none.
    std::string screen;
    /// @brief The params it opens that screen with.
    ValueRecord params;
    /// @brief The test id it rings; empty for a card with nothing ringed.
    std::string target;
    /// @brief The screen whose opening moves the tour on; empty for a step that waits for Next.
    std::string advanceOn;

    /// @brief Memberwise equality.
    /// @return Whether every member matches.
    bool operator==(TourStep const&) const = default;
};

/// @brief A guided tour, its texts evaluated now, as `AppRoot::tours` answers it.
struct Tour {
    /// @brief Its id.
    std::string id;
    /// @brief The screen it belongs to; empty for a general tour.
    std::string screen;
    /// @brief Its title.
    std::string title;
    /// @brief Its summary; empty for none.
    std::string summary;
    /// @brief Its steps, in order.
    std::vector<TourStep> steps;

    /// @brief Memberwise equality.
    /// @return Whether every member matches.
    bool operator==(Tour const&) const = default;
};
```

In `class AppRoot`, after `navigationBadges()`:

```cpp
    /// @brief The app document's guided tours, in declaration order, each text evaluated in the app scope and again
    ///        when what it reads changes, a change of locale included.
    /// @return They, valid as long as the root; empty when the document declares none.
    [[nodiscard]] reactive::Computed<std::vector<Tour>> const& tours() const;
```

and the member, after `_navigationBadges`:

```cpp
    std::unique_ptr<reactive::Computed<std::vector<Tour>>> _tours;
```

`src/ui/interpreter.cpp`, in `AppRoot::~AppRoot`, before `_navigationBadges.reset();`:

```cpp
    _tours.reset();
```

After `AppRoot::navigationBadges()`:

```cpp
reactive::Computed<std::vector<Tour>> const& AppRoot::tours() const { return *_tours; }
```

In `Interpreter::mountApp`, after the statement that makes `root->_navigationBadges`:

```cpp
    // One computed value for every tour: the texts are few, and a change of locale changes all of them at once.
    root->_tours = std::make_unique<reactive::Computed<std::vector<Tour>>>(
        _session->runtime(), [doc, scope = root->_scope.get()] {
            auto const textOf = [scope](ExprPtr const& expr) {
                return expr == nullptr ? std::string{}
                                       : detail::textOf(detail::evalIn(*scope, nullptr, expr), scope->formatting());
            };
            std::vector<Tour> out;
            out.reserve(doc->tours.size());
            for (TourDecl const& declared : doc->tours) {
                Tour tour{.id = declared.id,
                          .screen = declared.screen,
                          .title = textOf(declared.title),
                          .summary = textOf(declared.summary),
                          .steps = {}};
                for (TourStepDecl const& step : declared.steps) {
                    tour.steps.push_back(TourStep{.title = textOf(step.title),
                                                  .body = textOf(step.body),
                                                  .screen = step.screen,
                                                  .params = step.params,
                                                  .target = step.target,
                                                  .advanceOn = step.advanceOn});
                }
                out.push_back(std::move(tour));
            }
            return out;
        });
```

- [ ] **Step 6: Add the author API** (`include/morph/ui/author.hpp`, before `// ── Screens and registration`)

```cpp
// ── Guided tours ─────────────────────────────────────────────────────────────────────────────────────────────────

/// @brief One step of a guided tour (`docs/spec/ui/tours.md`).
struct TourStep {
    /// @brief The step's title.
    Handle<std::string> title;
    /// @brief Its body; absent for none.
    Handle<std::string> body;
    /// @brief The screen it opens first; empty for none.
    std::string screen;
    /// @brief The params it opens that screen with: literal values.
    ValueRecord params;
    /// @brief The test id it rings; empty for a card with nothing ringed.
    std::string target;
    /// @brief The screen whose opening moves the tour on; empty for a step that waits for Next.
    std::string advanceOn;
};

/// @brief A guided tour: data the server builds once, with no lifecycle.
struct Tour {
    /// @brief Its id: `[a-z0-9-]+`, at most 64 characters.
    std::string id;
    /// @brief The screen it belongs to; empty for a general tour.
    std::string screen;
    /// @brief Its title.
    Handle<std::string> title;
    /// @brief Its summary; absent for none.
    Handle<std::string> summary;
    /// @brief Its steps.
    std::vector<TourStep> steps;
};

/// @brief One tour as the app document's `tours` holds it.
/// @param tour The tour.
/// @return Its JSON.
/// @throws AuthoringError when the tour's or a step's title is absent.
[[nodiscard]] inline Json tourJson(Tour const& tour) {
    Json json = Json::object_t{};
    detail::put(json, "id", tour.id);
    if (!tour.screen.empty()) {
        detail::put(json, "screen", tour.screen);
    }
    detail::put(json, "title", tour.title.json());
    if (tour.summary.present()) {
        detail::put(json, "summary", tour.summary.json());
    }
    Json steps = Json::array_t{};
    for (TourStep const& step : tour.steps) {
        Json out = Json::object_t{};
        detail::put(out, "title", step.title.json());
        if (step.body.present()) {
            detail::put(out, "body", step.body.json());
        }
        if (!step.screen.empty()) {
            detail::put(out, "screen", step.screen);
        }
        if (!step.params.empty()) {
            Json params = Json::object_t{};
            for (auto const& [name, value] : step.params) {
                detail::put(params, name, valueToJson(value));
            }
            detail::put(out, "params", std::move(params));
        }
        if (!step.target.empty()) {
            detail::put(out, "target", step.target);
        }
        if (!step.advanceOn.empty()) {
            Json advance = Json::object_t{};
            detail::put(advance, "screen", step.advanceOn);
            detail::put(out, "advanceOn", std::move(advance));
        }
        std::get<Json::array_t>(steps.data).push_back(std::move(out));
    }
    detail::put(json, "steps", std::move(steps));
    return json;
}

/// @brief The app document's text with @p tours as its `tours` member, for `UiRegistry::addFile`.
/// @param appDocument The app document's JSON text.
/// @param tours       The tours, in the order the Tours menu lists them.
/// @return The text, or why it cannot be written: the document is not a JSON object, it declares `tours` already, or a
///         title is absent.
[[nodiscard]] inline std::expected<std::string, Refusal> withTours(std::string_view appDocument,
                                                                   std::vector<Tour> const& tours) {
    auto document = parseJson(appDocument);
    auto* const object = document ? std::get_if<Json::object_t>(&document->data) : nullptr;
    if (object == nullptr) {
        return std::unexpected(Refusal{.document = "app", .path = {}, .reason = "the app document is a JSON object"});
    }
    if (object->contains("tours")) {
        return std::unexpected(
            Refusal{.document = "app", .path = "/tours", .reason = "the app document declares its own `tours`"});
    }
    Json list = Json::array_t{};
    try {
        for (Tour const& tour : tours) {
            std::get<Json::array_t>(list.data).push_back(tourJson(tour));
        }
    } catch (AuthoringError const& error) {
        return std::unexpected(Refusal{.document = "app", .path = "/tours", .reason = error.what()});
    }
    object->insert_or_assign("tours", std::move(list));
    return glz::write_json(*document).value_or(std::string{});
}
```

- [ ] **Step 7: Run the tests to verify they pass**

Run:
```bash
flock /tmp/morph-build.lock cmake --build build/clang-debug -j5 --target morph_tests
ctest --test-dir build/clang-debug -R '^tours document:|^ui document:|^app root:' --output-on-failure
```
Expected: `100% tests passed`, the four new cases among them.

- [ ] **Step 8: Mutation check**

For each test, apply the mutation named in its comment (keep a `.bak` of the file), rebuild `morph_tests`, run its
ctest regex and see it fail; restore the file, `touch` it, rebuild, and see it pass. For the refusal table, drop the
`array->size() > kMaxTourSteps` check: only the `too many steps` row fails.

- [ ] **Step 9: Update the specs**

`docs/spec/ui/document.md`: after the section "The app document's navigation", add:

```markdown
### The app document's tours

The app document's `tours` is a list of guided tours ([tours.md](tours.md)). A tour is `{id, screen?, title,
summary?, steps}`: `id` is `[a-z0-9-]+`, at most 64 characters, unique among the tours; `screen` names the screen the
tour belongs to (without it the tour is general); `title` and `summary` are text expressions in the app scope. `steps`
holds one step or more and at most 50. A step is `{title, body?, screen?, params?, target?, advanceOn?}`: `title` and
`body` are text expressions in the app scope; `screen` is a screen id the step opens before it shows, with `params` a
record of literal values; `target` is a test id; `advanceOn` is `{"screen": id}`. Unknown members are ignored and a
member starting with `!` is refused, as everywhere in ui/1. A screen that declares `tours` is refused
(`` `tours` is the app document's; a screen has none ``). Whether the screens, targets and ids a tour names exist is
checked when the bundle is built ([service.md](service.md#the-registry)). `AppRoot::tours()` answers them
([interpreter.md](interpreter.md#the-app-root-scope)).
```

`docs/spec/ui/interpreter.md`, at the end of "The app root scope":

```markdown
`AppRoot::tours()` is a `reactive::Computed` of the document's tours, in declaration order, each a `Tour{id, screen,
title, summary, steps}` whose steps are `TourStep{title, body, screen, params, target, advanceOn}`. The texts are
evaluated in the app scope inside the computed value, so a change of locale, or of the app state a text reads,
re-evaluates them; it is one value for every tour, since the texts are few and a locale change changes all of them.
It goes with the root, before its scope.
```

`docs/spec/ui/author.md`, in "Out of scope", replace the "**The app document**" bullet's first sentence with:
"**The app document** (spec 6 §5): `Scope` writes a screen with a view, and the app document, which has none, is
written as JSON; `withTours(text, tours)` writes `author::Tour`s (`id`, `screen`, `title`, `summary`, `steps` of
`author::TourStep`) into it as its `tours` member." Keep the sentence about `principalPlacement`.

- [ ] **Step 10: Commit**

```bash
git add include/morph/ui/document.hpp src/ui/document.cpp include/morph/ui/interpreter.hpp src/ui/interpreter.cpp \
  include/morph/ui/author.hpp tests/test_ui_tours_document.cpp tests/CMakeLists.txt \
  docs/spec/ui/document.md docs/spec/ui/interpreter.md docs/spec/ui/author.md
git commit -m "Read guided tours from the app document"
```

---
### Task 2: Check every name a tour uses when the bundle is built; reserve the card's id

**Lane:** wave 2, after Task 1; parallel with Tasks 4 and 5.

**Files:**
- Create: `include/morph/ui/client_ids.hpp`
- Modify: `include/morph/ui/served_document.hpp` (`servedScreenRefusal`, lines 18–32)
- Create: `src/ui/tour_checks.hpp`, `src/ui/tour_checks.cpp`
- Modify: `include/morph/ui/service.hpp` (`build()` doc ~line 274; private `checkTourReferences()` after
  `checkShellNames()` ~line 371)
- Modify: `src/ui/service.cpp` (drop `kChatPanelScreen` ~line 41; `checkNewEntry` ~line 490; `checkBuildable` ~line
  535)
- Modify: `CMakeLists.txt` (`src/ui/tour_checks.cpp` after `src/ui/service.cpp`; `include/morph/ui/client_ids.hpp`
  after `include/morph/ui/served_document.hpp` in the header file set)
- Test: `tests/test_ui_tours_registry.cpp` (create); `tests/CMakeLists.txt` (after `test_ui_tours_document.cpp`)
- Docs: `docs/spec/ui/service.md` ("The registry"), `docs/spec/ui/host_vocabulary.md` ("Document scope")

**Interfaces:**
- Consumes: Task 1's `Document::tours`, `TourDecl`, `TourStepDecl`; `Document::shell` (`std::optional<ShellParts>`);
  `ui::parseJson`, `ui::pointerAppend(std::string_view, std::string_view|std::size_t)`, `ui::kAppFile`.
- Produces:
  - `inline constexpr std::string_view ui::kTourCardId = "morph.tour";`
  - `struct ui::ReservedClientId { std::string_view id; std::string_view owner; };`
  - `inline constexpr std::array<ReservedClientId, 2> ui::kReservedClientIds;`
  - `[[nodiscard]] constexpr ReservedClientId const* ui::reservedClientId(std::string_view id) noexcept;`
  - `[[nodiscard]] std::optional<Refusal> ui::detail::checkTours(Document const* app, std::string_view appText,
    std::span<TourScreen const> screens);` (private to `src/ui`)
  - `UiRegistry::build()` now also refuses a bundle whose tours name what it does not hold.

- [ ] **Step 1: Write the failing tests** (`tests/test_ui_tours_registry.cpp`)

```cpp
// SPDX-License-Identifier: Apache-2.0
//
// The bundle checks of guided tours (docs/spec/ui/tours.md, "Checks"): `UiRegistry::build` refuses a tour that names a
// screen, a target or a tour the bundle does not have, naming the tour and the step, and the tour card's id is the
// client's own, as the chat panel's is.

#include <catch2/catch_test_macros.hpp>
#include <expected>
#include <memory>
#include <morph/ui/client_ids.hpp>
#include <morph/ui/served_document.hpp>
#include <morph/ui/service.hpp>
#include <morph/ui/shell_parts.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace ui = morph::ui;

namespace {

// A screen with a literal test id in its view, one inside a component's fallback, and a head action.
constexpr std::string_view kSamples = R"({"vocab":"ui/1","id":"lims.samples","title":"Samples",
  "state":{"n":{"init":0}},
  "actions":[{"id":"new","label":"New","onClick":[{"set":"n","value":1}]}],
  "components":{"Chip":{"host":"Chip","version":1,
    "fallback":{"kind":"text","testId":"chip_fallback","text":"chip"}}},
  "view":{"kind":"column","children":[{"kind":"text","testId":"samples_table","text":"rows"}]}})";

/// A screen whose button runs `tour.start` with @p args.
std::string home(std::string_view args) {
    return R"({"vocab":"ui/1","id":"lims.home","title":"Home","state":{"which":{"init":"getting-started"}},
      "view":{"kind":"button","testId":"take_tour","label":"Take the tour",
              "onClick":[{"host":"tour.start","args":)" +
           std::string{args} + "}]}}";
}

constexpr std::string_view kValidTours = R"([
  {"id":"getting-started","title":"Getting started","steps":[
    {"target":"shell_tours","title":"Tours"},
    {"target":"nav_lims.samples","advanceOn":{"screen":"lims.samples"},"title":"Open Samples"},
    {"target":"samples_table","title":"The table"},
    {"target":"action_new","title":"New"},
    {"target":"chip_fallback","title":"A chip"},
    {"target":"chat_input","title":"Ask"},
    {"screen":"lims.home","target":"take_tour","title":"Again"}]},
  {"id":"register","screen":"lims.samples","title":"Register","steps":[{"target":"samples_table","title":"Here"}]}])";

struct Bundle {
    std::string tours{kValidTours};
    std::string homeArgs = R"({"obj":{"id":"getting-started"}})";
    std::optional<ui::ShellParts> parts;
};

/// Registers the two screens, a host screen `explorer`, the app document with @p bundle's tours and parts, and
/// builds.
std::expected<std::shared_ptr<ui::ServedBundle const>, ui::Refusal> build(Bundle const& bundle) {
    ui::UiRegistry registry{ui::ApplicationInfo{.id = "lims", .version = "1"}};
    auto const add = [&registry](std::string id, std::string text) {
        ui::CatalogEntry entry;
        entry.id = std::move(id);
        entry.title = "Screen";
        auto const added = registry.addScreen(std::move(entry), std::move(text));
        REQUIRE(added.has_value());
    };
    add("lims.samples", std::string{kSamples});
    add("lims.home", home(bundle.homeArgs));
    ui::CatalogEntry explorer;
    explorer.id = "explorer";
    explorer.title = "Explorer";
    explorer.host = std::string{"explorer"};
    REQUIRE(registry.addEntry(std::move(explorer)).has_value());
    auto const app = registry.addFile(ui::BundleFile{
        .kind = "app", .id = "app", .bytes = R"({"vocab":"ui/1","id":"app","tours":)" + bundle.tours + "}"});
    REQUIRE(app.has_value());
    if (bundle.parts) {
        REQUIRE(registry.setShellParts(*bundle.parts).has_value());
    }
    return registry.build();
}

/// The refusal of building @p bundle.
ui::Refusal refusalOf(Bundle const& bundle) {
    auto const built = build(bundle);
    REQUIRE_FALSE(built.has_value());
    return built.error();
}

/// A one-tour list whose only tour is general, with @p steps.
std::string oneTour(std::string_view steps) {
    return R"([{"id":"t","title":"T","steps":)" + std::string{steps} + "}]";
}

}  // namespace

// The control: every kind of accepted target, a step's screen, an advanceOn and a literal tour.start build.
TEST_CASE("tours registry: a valid tour builds", "[ui][tours][service]") {
    auto const built = build(Bundle{});
    INFO((built ? std::string{"built"} : built.error().message()));
    CHECK(built.has_value());
}

// Mutation: accept every target in `checkTour` (the misspelt id builds).
TEST_CASE("tours registry: a target nothing carries is refused, naming the tour and the step", "[ui][tours][service]") {
    ui::Refusal const refused =
        refusalOf(Bundle{.tours = oneTour(R"([{"screen":"lims.samples","target":"samples_tabel","title":"S"}])")});
    CHECK(refused.document == "app");
    CHECK(refused.path == "/tours/0/steps/0/target");
    CHECK(refused.reason.contains("the tour `t`, step 1"));
    CHECK(refused.reason.contains("samples_tabel"));
}

// Mutation: skip the registered-screen check of the tour's `screen`, the step's `screen` or `advanceOn.screen`.
TEST_CASE("tours registry: a screen a tour names that is not registered is refused", "[ui][tours][service]") {
    CHECK(refusalOf(Bundle{.tours = R"([{"id":"t","screen":"lims.nope","title":"T","steps":[{"title":"S"}]}])"})
              .path == "/tours/0/screen");
    CHECK(refusalOf(Bundle{.tours = oneTour(R"([{"screen":"lims.nope","title":"S"}])")}).path ==
          "/tours/0/steps/0/screen");
    CHECK(refusalOf(Bundle{.tours = oneTour(R"([{"advanceOn":{"screen":"lims.nope"},"title":"S"}])")}).path ==
          "/tours/0/steps/0/advanceOn/screen");
}

// Mutation: read a host screen's targets as if it had a document (its step with a page target builds).
TEST_CASE("tours registry: a step on a host screen may ring only shell ids", "[ui][tours][service]") {
    ui::Refusal const refused =
        refusalOf(Bundle{.tours = oneTour(R"([{"screen":"explorer","target":"samples_table","title":"S"}])")});
    CHECK(refused.path == "/tours/0/steps/0/target");
    CHECK(refused.reason.contains("host screen"));
    CHECK(build(Bundle{.tours = oneTour(R"([{"screen":"explorer","target":"shell_tours","title":"S"}])")})
              .has_value());
}

// Mutation: accept every `shell_*` id in `shellTarget` (the undeclared help button builds).
TEST_CASE("tours registry: a shell id whose part is not declared is refused", "[ui][tours][service]") {
    std::string const tours = oneTour(R"([{"target":"shell_help","title":"S"}])");
    CHECK(refusalOf(Bundle{.tours = tours}).path == "/tours/0/steps/0/target");
    ui::ShellParts parts;
    parts.help = ui::HelpEntry{.screen = "lims.home"};
    CHECK(build(Bundle{.tours = tours, .parts = parts}).has_value());
}

// Mutation: ignore `advanceOn` when following the effective screen (the target after it is refused).
TEST_CASE("tours registry: the effective screen follows a step's screen and the advanceOn before it",
          "[ui][tours][service]") {
    CHECK(build(Bundle{.tours = oneTour(R"([
      {"target":"nav_lims.samples","advanceOn":{"screen":"lims.samples"},"title":"Open"},
      {"target":"samples_table","title":"Here"}])")})
              .has_value());
    CHECK(build(Bundle{.tours = oneTour(R"([
      {"screen":"lims.home","target":"take_tour","title":"Home"},
      {"target":"nav_lims.samples","advanceOn":{"screen":"lims.samples"},"title":"Open"},
      {"target":"samples_table","title":"Here"}])")})
              .has_value());
    CHECK(refusalOf(Bundle{.tours = oneTour(R"([
      {"target":"samples_table","title":"Too early"},
      {"target":"nav_lims.samples","advanceOn":{"screen":"lims.samples"},"title":"Open"}])")})
              .path == "/tours/0/steps/0/target");
}

// Mutation: skip `collectTourStarts` for the screens (the undeclared id builds).
TEST_CASE("tours registry: a literal tour.start naming an undeclared tour is refused, a computed one is not",
          "[ui][tours][service]") {
    ui::Refusal const refused = refusalOf(Bundle{.homeArgs = R"({"obj":{"id":"nope"}})"});
    CHECK(refused.document == "lims.home");
    CHECK(refused.path == "/view/onClick/0");
    CHECK(refused.reason.contains("`nope`"));
    CHECK(build(Bundle{.homeArgs = R"({"obj":{"id":{"ref":"which"}}})"}).has_value());
}

// Mutation: drop `morph.tour` from `kReservedClientIds` (the screen registers, and no refusal names it).
TEST_CASE("tours registry: the tour card's id is the client's own", "[ui][tours][service]") {
    ui::UiRegistry registry{ui::ApplicationInfo{.id = "lims", .version = "1"}};
    ui::CatalogEntry entry;
    entry.id = "morph.tour";
    entry.title = "Forged";
    auto const added = registry.addScreen(
        std::move(entry), R"({"vocab":"ui/1","id":"morph.tour","view":{"kind":"text","text":"x"}})");
    REQUIRE_FALSE(added.has_value());
    CHECK(added.error().reason == "the id `morph.tour` is the tour card's");
    std::optional<ui::Refusal> const served = ui::servedScreenRefusal("morph.tour");
    REQUIRE(served.has_value());
    CHECK(served->reason.contains("which is the tour card's"));
    CHECK(ui::servedScreenRefusal("morph.chat")->reason.contains("which is the chat panel's"));
    CHECK_FALSE(ui::servedScreenRefusal("lims.home").has_value());
}
```

Add `test_ui_tours_registry.cpp` to `tests/CMakeLists.txt` after `test_ui_tours_document.cpp`.

- [ ] **Step 2: Run the tests to verify they fail**

Run: `flock /tmp/morph-build.lock cmake --build build/clang-debug -j5 --target morph_tests`
Expected: a compile error — `morph/ui/client_ids.hpp: No such file or directory`.

- [ ] **Step 3: The reserved client ids** (`include/morph/ui/client_ids.hpp`)

```cpp
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <algorithm>
#include <array>
#include <string_view>

#include "chat/panel_id.hpp"

/// @file
/// @brief The ids of the documents a client generates for itself, which no server document may take: host values and
///        commands are scoped by the id a mounted document carries. Specified in `docs/spec/ui/host_vocabulary.md`.

namespace morph::ui {

/// @brief The tour card's document id.
inline constexpr std::string_view kTourCardId = "morph.tour";

/// @brief A document id the client reserves, and whose it is.
struct ReservedClientId {
    /// @brief The id.
    std::string_view id;
    /// @brief What the client shows under it, as a refusal names it: "the chat panel".
    std::string_view owner;
};

/// @brief Every id the client reserves.
inline constexpr std::array<ReservedClientId, 2> kReservedClientIds = {{
    {.id = chat::kChatPanelId, .owner = "the chat panel"},
    {.id = kTourCardId, .owner = "the tour card"},
}};

/// @brief The reservation of an id, when it is one the client keeps for itself.
/// @param id The document or catalog entry id.
/// @return The reservation, or null for an id a server may use.
[[nodiscard]] constexpr ReservedClientId const* reservedClientId(std::string_view id) noexcept {
    // NOLINTNEXTLINE(readability-qualified-auto): an array iterator is a pointer on some standard libraries only.
    auto const found = std::ranges::find(kReservedClientIds, id, &ReservedClientId::id);
    return found == kReservedClientIds.end() ? nullptr : &*found;
}

}  // namespace morph::ui
```

- [ ] **Step 4: Use them in both checks**

`include/morph/ui/served_document.hpp`: replace `#include "chat/panel_id.hpp"` with `#include "client_ids.hpp"`, and
`servedScreenRefusal` with:

```cpp
/// @brief Why a server's catalog entry may not be mounted as a screen, when it may not.
///
/// The client's own documents (the chat panel, the tour card) carry host values and commands no other document may
/// reach, so their ids are the client's and never a server's.
/// @param screenId The catalog entry's id.
/// @return The refusal, naming @p screenId, when it is a reserved id; otherwise nothing.
[[nodiscard]] inline std::optional<Refusal> servedScreenRefusal(std::string_view screenId) {
    if (ReservedClientId const* const reserved = reservedClientId(screenId)) {
        return Refusal{.document = std::string{screenId},
                       .path = {},
                       .reason = "the catalog lists the id `" + std::string{screenId} + "`, which is " +
                                 std::string{reserved->owner} + "'s; the entry is refused"};
    }
    return std::nullopt;
}
```

`src/ui/service.cpp`: delete `kChatPanelScreen` and its comment, add `#include <morph/ui/client_ids.hpp>`, and in
`UiRegistry::checkNewEntry` replace the `kChatPanelScreen` branch with:

```cpp
    if (ReservedClientId const* const reserved = reservedClientId(entry.id)) {
        return Refusal{.document = entry.id,
                       .path = {},
                       .reason = "the id `" + entry.id + "` is " + std::string{reserved->owner} + "'s"};
    }
```

- [ ] **Step 5: The tour checks** (`src/ui/tour_checks.hpp`)

```cpp
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <morph/ui/document.hpp>
#include <morph/ui/value.hpp>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace morph::ui::detail {

/// What the tour checks read of one registered screen.
struct TourScreen {
    /// Its id.
    std::string id;
    /// Its document's JSON text; empty for an entry without one: a host screen, or one the application has not built.
    std::string text;
    /// Whether a signed-in navigator lists it: it is not public.
    bool listed = true;
};

/// Checks what the app document's tours name against the registered screens, and every literal `tour.start` against
/// the tours (docs/spec/ui/tours.md, "Checks").
/// @param app     The app document, loaded; null when the bundle has none, and then it declares no tour.
/// @param appText The app document's JSON text; empty with none.
/// @param screens The registered screens.
/// @return The first refusal, naming the tour and the step or the document and the command, or nothing.
[[nodiscard]] std::optional<Refusal> checkTours(Document const* app, std::string_view appText,
                                                std::span<TourScreen const> screens);

}  // namespace morph::ui::detail
```

`src/ui/tour_checks.cpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0

#include "tour_checks.hpp"

#include <cstddef>
#include <map>
#include <morph/ui/bundle.hpp>
#include <morph/ui/document.hpp>
#include <morph/ui/shell_parts.hpp>
#include <morph/ui/value.hpp>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace morph::ui::detail {
namespace {

using Names = std::set<std::string, std::less<>>;

// What a registered screen offers a step: whether it has a document, whether the navigator lists it, and the test ids
// its document carries.
struct ScreenInfo {
    bool listed = true;
    bool hasDocument = false;
    Names ids;
};

using Screens = std::map<std::string, ScreenInfo, std::less<>>;

// Every literal `testId` in a document's JSON, the components' fallbacks included.
// NOLINTNEXTLINE(misc-no-recursion): a document is a tree, walked down.
void collectTestIds(Json const& json, Names& out) {
    if (auto const* const object = std::get_if<Json::object_t>(&json.data)) {
        if (auto const found = object->find("testId"); found != object->end()) {
            if (auto const* const id = std::get_if<std::string>(&found->second.data)) {
                out.insert(*id);
            }
        }
        for (auto const& [key, value] : *object) {
            static_cast<void>(key);
            collectTestIds(value, out);
        }
    } else if (auto const* const array = std::get_if<Json::array_t>(&json.data)) {
        for (Json const& element : *array) {
            collectTestIds(element, out);
        }
    }
}

// The ids the interpreter gives the buttons of a screen head's `actions`: `action_<id>`.
void collectHeadActions(Json const& json, Names& out) {
    auto const* const object = std::get_if<Json::object_t>(&json.data);
    if (object == nullptr) {
        return;
    }
    auto const actions = object->find("actions");
    auto const* const list = actions == object->end() ? nullptr : std::get_if<Json::array_t>(&actions->second.data);
    if (list == nullptr) {
        return;
    }
    for (Json const& action : *list) {
        auto const* const fields = std::get_if<Json::object_t>(&action.data);
        if (fields == nullptr) {
            continue;
        }
        if (auto const id = fields->find("id"); id != fields->end()) {
            if (auto const* const text = std::get_if<std::string>(&id->second.data)) {
                out.insert("action_" + *text);
            }
        }
    }
}

// The tour a `tour.start` command's `args` names as a literal: `{"obj": {"id": "x"}}` or `{"lit": {"id": "x"}}`. A
// computed id is checked at run time instead.
std::optional<std::string> literalTourId(Json const& args) {
    auto const* const object = std::get_if<Json::object_t>(&args.data);
    if (object == nullptr || object->size() != 1) {
        return std::nullopt;
    }
    auto const& [form, record] = *object->begin();
    if (form != "obj" && form != "lit") {
        return std::nullopt;
    }
    auto const* const fields = std::get_if<Json::object_t>(&record.data);
    if (fields == nullptr) {
        return std::nullopt;
    }
    auto const id = fields->find("id");
    auto const* const text = id == fields->end() ? nullptr : std::get_if<std::string>(&id->second.data);
    return text == nullptr ? std::nullopt : std::optional<std::string>{*text};
}

struct TourStart {
    std::string path;
    std::string id;
};

// Every `tour.start` whose `args` names a tour as a literal, with its JSON Pointer.
// NOLINTNEXTLINE(misc-no-recursion): a document is a tree, walked down.
void collectTourStarts(Json const& json, std::string const& path, std::vector<TourStart>& out) {
    if (auto const* const object = std::get_if<Json::object_t>(&json.data)) {
        auto const host = object->find("host");
        auto const args = object->find("args");
        if (host != object->end() && args != object->end()) {
            auto const* const name = std::get_if<std::string>(&host->second.data);
            if (name != nullptr && *name == "tour.start") {
                if (std::optional<std::string> id = literalTourId(args->second)) {
                    out.push_back(TourStart{.path = path, .id = std::move(*id)});
                }
            }
        }
        for (auto const& [key, value] : *object) {
            collectTourStarts(value, pointerAppend(path, key), out);
        }
    } else if (auto const* const array = std::get_if<Json::array_t>(&json.data)) {
        for (std::size_t index = 0; index < array->size(); ++index) {
            collectTourStarts(array->at(index), pointerAppend(path, index), out);
        }
    }
}

// Whether the shell draws @p target for this bundle: the Tours button (a tour is declared), the chat's input, a
// navigator entry of a listed screen with a document, or a part the app document declares.
bool shellTarget(std::string_view target, std::optional<ShellParts> const& parts, Screens const& screens) {
    if (target == "shell_tours" || target == "chat_input") {
        return true;
    }
    if (target.starts_with("nav_")) {
        auto const found = screens.find(target.substr(4));
        return found != screens.end() && found->second.listed && found->second.hasDocument;
    }
    if (!parts) {
        return false;
    }
    if (target == "shell_search") {
        return !parts->search.empty();
    }
    if (target == "shell_inbox") {
        return parts->inbox.has_value();
    }
    if (target == "shell_help") {
        return parts->help.has_value();
    }
    if (target == "shell_principal_menu") {
        return parts->principal.has_value();
    }
    if (target == "shell_brand_title") {
        return parts->brand.has_value();
    }
    if (target == "shell_context") {
        return parts->context.has_value();
    }
    return false;
}

Refusal refusal(std::string path, std::string reason) {
    return Refusal{.document = std::string{kAppFile}, .path = std::move(path), .reason = std::move(reason)};
}

std::string stepName(TourDecl const& tour, std::size_t index) {
    return "the tour `" + tour.id + "`, step " + std::to_string(index + 1);
}

// One tour. The effective screen of a step is the screen the active tab shows when the step shows: its own `screen`,
// else the latest of an earlier step's `screen` and the `advanceOn` of an earlier step, else the tour's `screen`.
std::optional<Refusal> checkTour(TourDecl const& tour, std::optional<ShellParts> const& parts,
                                 Screens const& screens) {
    if (!tour.screen.empty() && !screens.contains(tour.screen)) {
        return refusal(pointerAppend(tour.path, "screen"),
                       "the tour `" + tour.id + "` belongs to `" + tour.screen + "`, which is not a registered screen");
    }
    std::string current = tour.screen;
    for (std::size_t index = 0; index < tour.steps.size(); ++index) {
        TourStepDecl const& step = tour.steps.at(index);
        if (!step.screen.empty()) {
            if (!screens.contains(step.screen)) {
                return refusal(pointerAppend(step.path, "screen"), stepName(tour, index) + " opens `" + step.screen +
                                                                       "`, which is not a registered screen");
            }
            current = step.screen;
        }
        if (!step.advanceOn.empty() && !screens.contains(step.advanceOn)) {
            return refusal(pointerAppend(pointerAppend(step.path, "advanceOn"), "screen"),
                           stepName(tour, index) + " waits for `" + step.advanceOn +
                               "`, which is not a registered screen");
        }
        if (!step.target.empty() && !shellTarget(step.target, parts, screens)) {
            auto const found = current.empty() ? screens.end() : screens.find(current);
            bool const onScreen = found != screens.end() && found->second.ids.contains(step.target);
            if (!onScreen) {
                std::string where = "the shell";
                if (found != screens.end() && !found->second.hasDocument) {
                    where = "the shell (`" + current + "` is a host screen, whose parts the server cannot see)";
                } else if (found != screens.end()) {
                    where = "`" + current + "` or the shell";
                }
                return refusal(pointerAppend(step.path, "target"),
                               stepName(tour, index) + " rings `" + step.target + "`, which nothing on " + where +
                                   " carries");
            }
        }
        if (!step.advanceOn.empty()) {
            current = step.advanceOn;
        }
    }
    return std::nullopt;
}

// The first literal `tour.start` in @p text that names a tour not in @p declared.
std::optional<Refusal> checkStarts(std::string const& document, std::string_view text, Names const& declared) {
    auto const json = parseJson(text);
    if (!json) {
        return std::nullopt;
    }
    std::vector<TourStart> starts;
    collectTourStarts(*json, "", starts);
    for (TourStart const& start : starts) {
        if (!declared.contains(start.id)) {
            return Refusal{.document = document,
                           .path = start.path,
                           .reason = "`tour.start` names the tour `" + start.id +
                                     "`, which the app document does not declare"};
        }
    }
    return std::nullopt;
}

}  // namespace

std::optional<Refusal> checkTours(Document const* app, std::string_view appText,
                                  std::span<TourScreen const> screens) {
    Screens known;
    for (TourScreen const& screen : screens) {
        ScreenInfo info{.listed = screen.listed, .hasDocument = !screen.text.empty(), .ids = {}};
        if (info.hasDocument) {
            if (auto const json = parseJson(screen.text)) {
                collectTestIds(*json, info.ids);
                collectHeadActions(*json, info.ids);
            }
        }
        known.insert_or_assign(screen.id, std::move(info));
    }
    Names declared;
    if (app != nullptr) {
        for (TourDecl const& tour : app->tours) {
            declared.insert(tour.id);
            if (auto refused = checkTour(tour, app->shell, known)) {
                return refused;
            }
        }
    }
    for (TourScreen const& screen : screens) {
        if (auto refused = checkStarts(screen.id, screen.text, declared)) {
            return refused;
        }
    }
    if (!appText.empty()) {
        if (auto refused = checkStarts(std::string{kAppFile}, appText, declared)) {
            return refused;
        }
    }
    return std::nullopt;
}

}  // namespace morph::ui::detail
```

- [ ] **Step 6: Run them from `build`**

`include/morph/ui/service.hpp`: extend `build()`'s doc comment with: "a tour that names a screen that is not registered,
or a target nothing the window can show carries, and a literal `tour.start` naming a tour the app document does not
declare (`docs/spec/ui/tours.md`, "Checks");". After `checkShellNames()` add:

```cpp
    /// @brief Checks every name the app document's tours use, and every literal `tour.start`, against the registered
    ///        screens and the declared tours.
    /// @return The first refusal, or nothing.
    [[nodiscard]] std::optional<Refusal> checkTourReferences() const;
```

`src/ui/service.cpp`: `#include "tour_checks.hpp"`; at the start of `UiRegistry::checkBuildable`, after the themes:

```cpp
    if (auto refusal = checkTourReferences()) {
        return refusal;
    }
```

and after `checkShellNames`:

```cpp
std::optional<Refusal> UiRegistry::checkTourReferences() const {
    std::optional<Document> app;
    std::string appText;
    for (BundleFile const& file : _files) {
        if (file.kind != kAppFile) {
            continue;
        }
        // It loaded when it was added; a load that fails now names nothing to check.
        if (auto loaded = loadDocumentText(file.bytes, LoadOptions{.catalog = _catalog})) {
            app = std::move(*loaded);
            appText = file.bytes;
        }
    }
    std::vector<detail::TourScreen> screens;
    screens.reserve(_screens.size());
    for (Screen const& screen : _screens) {
        screens.push_back(detail::TourScreen{
            .id = screen.served.entry.id, .text = screen.text, .listed = !screen.served.entry.isPublic});
    }
    return detail::checkTours(app ? &*app : nullptr, appText, screens);
}
```

`CMakeLists.txt`: add `src/ui/tour_checks.cpp` after `src/ui/service.cpp`, and `include/morph/ui/client_ids.hpp` after
`include/morph/ui/served_document.hpp`.

- [ ] **Step 7: Run the tests to verify they pass**

Run:
```bash
flock /tmp/morph-build.lock cmake --build build/clang-debug -j5 --target morph_tests morph_client_tests
ctest --test-dir build/clang-debug -R '^tours registry:|^ui service:|chat panel' --output-on-failure
```
Expected: `100% tests passed`. `tests/test_ui_service.cpp`'s "the chat panel's id" section and the client's
chat-panel cases keep their texts.

- [ ] **Step 8: Mutation check**

Apply each test's mutation, rebuild, see that test fail, restore, `touch`, rebuild. The "target nothing carries" case
is the design's required mutation check: in `checkTour`, replace `if (!onScreen)` with `if (false)` and confirm the
misspelt `samples_tabel` builds (the case fails).

- [ ] **Step 9: Update the specs**

`docs/spec/ui/service.md`, "The registry", add a bullet after the `build()` bullet:

```markdown
- **Tours are checked against the bundle.** `build()` refuses, with the tour and the step in the reason
  (`the tour `getting-started`, step 3 rings `samples_tabel`, …`), a tour's `screen`, a step's `screen` or
  `advanceOn.screen` that is not a registered screen, and a `target` that nothing the window can show carries. A step's
  *effective screen* is the screen the active tab shows when the step shows: its own `screen`, else the latest of an
  earlier step's `screen` and an earlier step's `advanceOn.screen`, else the tour's `screen`. A target is accepted when
  it is a shell id the declared parts produce (`shell_search`, `shell_inbox`, `shell_help`, `shell_principal_menu`,
  `shell_brand_title`, `shell_context`), `shell_tours` (a tour is declared), `chat_input` (whether chat is offered is the
  chat service's, which the registry does not see; a client without chat shows the card centred), `nav_<id>` for a
  registered screen that has a document and is not public, a literal `testId` anywhere in the effective screen's
  document (its components' fallbacks included), or `action_<id>` for one of its head `actions`. A host screen, which
  has no document, offers only shell ids. A `tour.start` host command whose `args` names a tour as a literal
  (`{"obj": {"id": "…"}}` or `{"lit": {…}}`) in any registered document or the app document must name a declared tour;
  a computed id is reported at run time. The shape of `tours` is checked when the app document is added
  ([document.md](document.md#the-app-documents-tours)).
- **The client's own ids are reserved.** An entry id that is one of `kReservedClientIds` (`client_ids.hpp`: the chat
  panel's `morph.chat`, the tour card's `morph.tour`) is refused: `the id `morph.tour` is the tour card's`.
```

`docs/spec/ui/host_vocabulary.md`, "Document scope": replace "and a catalog entry whose id is the chat panel's,
`morph.chat`; the panel's document is resolved from the client's own generated documents only" with "and a catalog
entry whose id is one the client reserves for its own documents (`kReservedClientIds` in `client_ids.hpp`: the chat
panel's `morph.chat` and the tour card's `morph.tour`); those documents are resolved from the client's own generated
documents only", and "A name limited to the panel therefore reaches the client's own panel document" with "A name
limited to one of them therefore reaches the client's own document".

- [ ] **Step 10: Commit**

```bash
git add include/morph/ui/client_ids.hpp include/morph/ui/served_document.hpp include/morph/ui/service.hpp \
  src/ui/service.cpp src/ui/tour_checks.hpp src/ui/tour_checks.cpp CMakeLists.txt tests/test_ui_tours_registry.cpp \
  tests/CMakeLists.txt docs/spec/ui/service.md docs/spec/ui/host_vocabulary.md
git commit -m "Check the names guided tours use when the bundle is built"
```

---
### Task 3: `SpotlightWidget` in the backend contract, on `RecordingBackend`, with conformance cases

**Lane:** wave 1, parallel with Task 1 (disjoint files).

**Files:**
- Modify: `include/morph/ui/backend.hpp` (`SpotlightWidget` after `PopoverWidget` ~line 1210; `createSpotlight` after
  `createPopover` in `IViewBackend` ~line 1404)
- Modify: `src/ui/backend.cpp` (default `createSpotlight`, after `createCustom`)
- Modify: `include/morph/ui/testing/recording_backend.hpp` (`RecordStore`: listeners ~line 636–800; free
  `detail::shownInStore` and `detail::FakeSpotlight` after `FakePopover` ~line 1719; `RecordingBackend::createSpotlight`
  after `createPopover` ~line 2405; `press` ~line 2675; `isClosedDialog` ~line 3262)
- Modify: `tests/ui_echoing_backend.hpp` (forward `createSpotlight` after `createPopover` ~line 138)
- Modify: `include/morph/ui/testing/backend_conformance.hpp` (`ConformanceProbe::targetOf` after `textExtentOf`
  ~line 222; five cases and `spotlightConformanceCases()` before `conformanceCases()` ~line 4374)
- Modify: `tests/test_ui_conformance.cpp` (`RecordingProbe::targetOf`; two test cases at the end)
- Test: `tests/test_ui_spotlight_recording.cpp` (create); `tests/CMakeLists.txt` (after `test_ui_conformance.cpp`)
- Docs: `docs/spec/ui/backend_contract.md` ("Widgets", a new section "The spotlight" after "The popover",
  "RecordingBackend", "The conformance suite")

**Interfaces:**
- Consumes: `ContainerWidget`, `Widget::setTestId/setVisible/focus`, `Mounted(reactive::Runtime&, IViewBackend&, Node,
  ContainerWidget* parent)`, the conformance helpers `detail::Checks`, `detail::childOf`.
- Produces:
  - ```cpp
    class SpotlightWidget : public ContainerWidget {
    public:
        virtual void setOpen(bool open) = 0;
        virtual void setTarget(std::optional<std::string> testId) = 0;
        [[nodiscard]] virtual bool targetShown() const = 0;
        virtual void setOnTargetShownChanged(std::function<void(bool)> handler) = 0;
        virtual void setTargetFocusable(bool focusable) = 0;
    };
    ```
  - `[[nodiscard]] virtual std::unique_ptr<SpotlightWidget> IViewBackend::createSpotlight(ContainerWidget* parent);`
    (default throws `std::logic_error`)
  - `int testing::detail::RecordStore::listen(std::function<void()> listener);`, `void unlisten(int key);`
  - `[[nodiscard]] bool testing::detail::shownInStore(RecordStore const& store, Record const& record);`
  - `class testing::detail::FakeSpotlight` — kind `Spotlight`, properties `open`, `target`, `targetFocusable`,
    `targetShown`, `ringed`; `[[nodiscard]] int ringed() const noexcept;`
  - `[[nodiscard]] virtual Widget const* ConformanceProbe::targetOf(SpotlightWidget const& spotlight);` (default null)
  - `[[nodiscard]] inline std::span<ConformanceCase const> testing::spotlightConformanceCases();` (five cases)

- [ ] **Step 1: Write the failing tests**

`tests/test_ui_spotlight_recording.cpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0
//
// The recording backend's spotlight (docs/spec/ui/backend_contract.md, "The spotlight"): what it records, how it moves
// the recorded focus, and that the target it rings follows the live widgets without a test setting it by hand.

#include <catch2/catch_test_macros.hpp>
#include <memory>
#include <morph/ui/backend.hpp>
#include <morph/ui/testing/recording_backend.hpp>
#include <optional>
#include <string>
#include <vector>

namespace ui = morph::ui;

// Mutation: leave the recorded focus where it is in `FakeSpotlight::setOpen` (the field keeps it on open, and nothing
// hands it back on close).
TEST_CASE("spotlight: opening moves focus to the card's first shown button, and closing hands it back", "[ui][tours]") {
    ui::testing::RecordingBackend backend;
    auto const page = backend.createStack(nullptr, ui::Axis::Vertical);
    auto const field = backend.createTextInput(page.get(), ui::TextInputMode::SingleLine);
    auto const spotlight = backend.createSpotlight(nullptr);
    auto const back = backend.createButton(spotlight.get());
    back->setVisible(false);
    auto const next = backend.createButton(spotlight.get());
    field->focus();
    spotlight->setOpen(true);
    CHECK(backend.focused() == backend.idOf(*next));
    spotlight->setOpen(false);
    CHECK(backend.focused() == backend.idOf(*field));
}

// Mutation: resolve the target only in `setTarget` (a later testId, hide or destroy changes nothing recorded).
TEST_CASE("spotlight: the recorded target follows the live widgets", "[ui][tours]") {
    ui::testing::RecordingBackend backend;
    std::vector<bool> heard;
    auto const spotlight = backend.createSpotlight(nullptr);
    int const spotlightId = backend.idOf(*spotlight);
    spotlight->setOnTargetShownChanged([&heard](bool shown) { heard.push_back(shown); });
    spotlight->setTarget(std::string{"later"});
    CHECK(backend.prop(spotlightId, "target") == "later");
    CHECK(backend.prop(spotlightId, "targetShown") == "false");
    auto const page = backend.createStack(nullptr, ui::Axis::Vertical);
    auto target = backend.createButton(page.get());
    target->setTestId("later");
    CHECK(backend.prop(spotlightId, "targetShown") == "true");
    CHECK(backend.prop(spotlightId, "ringed") == std::to_string(backend.idOf(*target)));
    page->setVisible(false);
    CHECK(backend.prop(spotlightId, "targetShown") == "false");
    page->setVisible(true);
    target.reset();
    CHECK(backend.prop(spotlightId, "ringed") == "0");
    CHECK(heard == std::vector<bool>{true, false, true, false});
    spotlight->setTarget(std::nullopt);
    CHECK(backend.prop(spotlightId, "target") == "none");
}
```

`tests/test_ui_conformance.cpp`: in `RecordingProbe`, after `dropFiles`, add

```cpp
    [[nodiscard]] ui::Widget const* targetOf(ui::SpotlightWidget const& spotlight) override {
        std::string const ringed = _backend.prop(_backend.idOf(spotlight), "ringed");
        int widgetId = 0;
        char const* const first = ringed.data();
        static_cast<void>(std::from_chars(first, std::next(first, std::ssize(ringed)), widgetId));
        return widgetId == 0 || !_backend.exists(widgetId) ? nullptr : _backend.widget(widgetId);
    }
```

and at the end of the file:

```cpp
// Mutation: in `FakeSpotlight::setTarget`, keep `_target` empty (every case that rings a target fails).
TEST_CASE("ui conformance: RecordingBackend passes every spotlight case", "[ui][conformance][tours]") {
    auto const cases = ui::testing::spotlightConformanceCases();
    REQUIRE(cases.size() == 5);
    for (auto const& testCase : cases) {
        RecordingProbe probe;
        std::optional<std::string> const failure = testCase.run(probe);
        INFO(std::string{testCase.name} + ": " + failure.value_or("passed"));
        CHECK_FALSE(failure.has_value());
    }
}

// A spotlight case that a probe driving and reading nothing passes measures nothing.
TEST_CASE("ui conformance: a probe that drives and reads nothing fails every spotlight case",
          "[ui][conformance][tours]") {
    for (auto const& testCase : ui::testing::spotlightConformanceCases()) {
        InertProbe probe;
        INFO(testCase.name);
        CHECK(testCase.run(probe).has_value());
    }
}
```

Add `test_ui_spotlight_recording.cpp` to `tests/CMakeLists.txt` after `test_ui_conformance.cpp`.

- [ ] **Step 2: Run the tests to verify they fail**

Run: `flock /tmp/morph-build.lock cmake --build build/clang-debug -j5 --target morph_tests`
Expected: compile errors — `IViewBackend` has no member `createSpotlight`, `SpotlightWidget` is not declared.

- [ ] **Step 3: The widget and the factory** (`include/morph/ui/backend.hpp`)

After `class PopoverWidget`:

```cpp
/// @brief A guided tour's spotlight over the whole window: it dims everything but one widget, rings that widget, and
///        shows its children (the tour's card) beside it. A new spotlight is closed and names no target.
///
/// The target is named by test id. A backend keeps an index of its own live widgets by test id, and the spotlight
/// rings the first of them, in creation order, that is effectively shown: visible itself, every container around it
/// visible, every overlay around it open, and no closed section around it. A tab page that is not selected is hidden,
/// so a widget on it is not shown. The spotlight picks again whenever a widget's test id is set, a widget is
/// destroyed, or a candidate or a container around it is shown or hidden. With no candidate shown, `targetShown()` is
/// false and the card is centred with nothing ringed.
///
/// Opening it moves keyboard focus into its children (the first shown button, or the first focusable widget, or the
/// spotlight itself), and closing it hands focus back to the widget that had it when it opened. Tab stays among its
/// children, unless `setTargetFocusable(true)` adds the ringed widget after them. A chord a widget inside it declares
/// is delivered before the focused widget's own keys, except Enter and Space, which the focused widget takes first as
/// it always does. A pointer press outside the ringed widget's box and the card reaches nothing; one inside the ringed
/// widget's box reaches the ringed widget.
class SpotlightWidget : public ContainerWidget {
public:
    /// @brief Shows or hides the spotlight.
    /// @param open Whether it is shown.
    virtual void setOpen(bool open) = 0;

    /// @brief Names the widget to ring.
    /// @param testId The target's test id; `nullopt` rings nothing and centres the card.
    virtual void setTarget(std::optional<std::string> testId) = 0;

    /// @brief Whether a widget carrying the target's test id is shown and ringed now.
    /// @return It; false with no target named.
    [[nodiscard]] virtual bool targetShown() const = 0;

    /// @brief Sets what is called each time `targetShown()` changes, with its new value. No setter calls it except
    ///        `setTarget`, whose resolution is a change like any other.
    /// @param handler The handler; empty calls nothing.
    virtual void setOnTargetShownChanged(std::function<void(bool)> handler) = 0;

    /// @brief Adds the ringed widget to Tab's order after the spotlight's children, or takes it out: what a step that
    ///        waits for the person to use the target needs, so it can be reached from the keyboard.
    /// @param focusable Whether Tab reaches the ringed widget. A new spotlight's Tab stays among its children.
    virtual void setTargetFocusable(bool focusable) = 0;
};
```

In `class IViewBackend`, after `createPopover`:

```cpp
    /// @brief Makes a spotlight. A backend that draws none keeps this default, which throws: a tour on it cannot run,
    ///        and that is said rather than hidden.
    /// @param parent The container to append it to; null makes a root.
    /// @return The widget.
    /// @throws std::logic_error from the default.
    [[nodiscard]] virtual std::unique_ptr<SpotlightWidget> createSpotlight(ContainerWidget* parent);
```

`src/ui/backend.cpp`, after `createCustom` (add `#include <stdexcept>`):

```cpp
std::unique_ptr<SpotlightWidget> IViewBackend::createSpotlight(ContainerWidget* parent) {
    static_cast<void>(parent);
    throw std::logic_error{"morph::ui: this backend draws no spotlight, so it runs no guided tour"};
}
```

- [ ] **Step 4: The recording backend**

In `RecordStore` (`recording_backend.hpp`), public section, after `clearLog()`:

```cpp
    /// @brief Registers a listener told after each change that can move a spotlight's target: a widget made, moved or
    ///        destroyed, and a `testId`, `visible`, `open` or `collapsed` property set by a setter or the user.
    /// @param listener The listener; it may change properties, and a change it makes is told after it returns.
    /// @return Its key, for `unlisten`.
    int listen(std::function<void()> listener) {
        int const key = ++_nextListener;
        _listeners.insert_or_assign(key, std::move(listener));
        return key;
    }

    /// @brief Stops telling a listener.
    /// @param key What `listen` returned.
    void unlisten(int key) { _listeners.erase(key); }
```

In its private section:

```cpp
    [[nodiscard]] static bool movesTargets(std::string_view name) noexcept {
        return name == "testId" || name == "visible" || name == "open" || name == "collapsed";
    }

    // Tells every listener. A change a listener makes while it is told is told once more after the round, not inside
    // it, so a listener never runs inside itself.
    void notify() {
        if (_notifying) {
            _again = true;
            return;
        }
        _notifying = true;
        do {
            _again = false;
            std::vector<int> keys;
            keys.reserve(_listeners.size());
            for (auto const& [key, listener] : _listeners) {
                static_cast<void>(listener);
                keys.push_back(key);
            }
            for (int const key : keys) {
                if (auto const found = _listeners.find(key); found != _listeners.end()) {
                    auto const listener = found->second;
                    listener();
                }
            }
        } while (_again);
        _notifying = false;
    }

    std::map<int, std::function<void()>> _listeners;
    int _nextListener = 0;
    bool _notifying = false;
    bool _again = false;
```

Call `notify();` at the end of `create`, `move` and `destroy`, and at the end of `set` and `setByUser` when
`movesTargets(name)`.

After `class FakePopover`:

```cpp
/// @brief Whether a widget is effectively shown: it and every container around it visible, every dialog, drawer,
///        popover and spotlight around it open, no panel around it collapsed, and no closed collapsible around it
///        unless it is that section's header.
/// @param store  The records.
/// @param record The widget.
/// @return Whether a user could see it.
[[nodiscard]] inline bool shownInStore(RecordStore const& store, Record const& record) {
    auto const flag = [](Record const& of, std::string_view name, bool value) {
        auto const found = of.props.find(name);
        return found != of.props.end() && found->second == formatBool(value);
    };
    auto const closedOverlay = [&flag](Record const& of) {
        return (of.kind == "Dialog" || of.kind == "Drawer" || of.kind == "Popover" || of.kind == "Spotlight") &&
               !flag(of, "open", true);
    };
    if (flag(record, "visible", false) || closedOverlay(record)) {
        return false;
    }
    for (int inner = record.id, outer = record.parent; outer != 0;) {
        Record const& around = store.at(outer);
        if (flag(around, "visible", false) || flag(around, "collapsed", true) || closedOverlay(around)) {
            return false;
        }
        if (around.kind == "Collapsible" && flag(around, "open", false)) {
            auto const header = around.props.find("header");
            if (header == around.props.end() || header->second != std::to_string(inner)) {
                return false;
            }
        }
        inner = outer;
        outer = around.parent;
    }
    return true;
}

/// @brief A fake `SpotlightWidget`: properties `open`, `target` (`none` for none) and `targetFocusable`, and, as it
///        resolves its target against the backend's live widgets on every change the store tells, `targetShown` and
///        `ringed` (the id of the widget it rings, `0` for none). Opening moves the recorded focus to its first shown
///        `Button`, or to itself; closing moves it back to the widget that had it.
class FakeSpotlight final : public FakeContainer<SpotlightWidget> {
public:
    /// @brief Records the spotlight and starts following the store.
    /// @param store  Where the widget records itself.
    /// @param parent The container it is appended to, or null for a root.
    FakeSpotlight(std::shared_ptr<RecordStore> store, Widget const* parent)
        : FakeContainer<SpotlightWidget>{std::move(store), "Spotlight", parent},
          _listener{this->store().listen([this] { resolve(); })} {
        set("targetShown", formatBool(false));
        set("ringed", "0");
    }

    /// @brief Stops following the store.
    ~FakeSpotlight() override { this->store().unlisten(_listener); }

    FakeSpotlight(FakeSpotlight const&) = delete;
    FakeSpotlight& operator=(FakeSpotlight const&) = delete;
    FakeSpotlight(FakeSpotlight&&) = delete;
    FakeSpotlight& operator=(FakeSpotlight&&) = delete;

    void setOpen(bool open) override {
        if (open == _open) {
            return;
        }
        _open = open;
        if (open) {
            _restore = store().focused();
        }
        set("open", formatBool(open));
        if (open) {
            focusInside();
            return;
        }
        int const restore = std::exchange(_restore, 0);
        if (restore != 0 && store().records().contains(restore)) {
            store().focus(restore);
        }
    }
    void setTarget(std::optional<std::string> testId) override {
        _target = std::move(testId);
        set("target", _target ? formatText(*_target) : std::string{"none"});
        resolve();
    }
    [[nodiscard]] bool targetShown() const override { return _ringed != 0; }
    void setOnTargetShownChanged(std::function<void(bool)> handler) override { _onChanged = std::move(handler); }
    void setTargetFocusable(bool focusable) override { set("targetFocusable", formatBool(focusable)); }

    /// @brief The widget it rings.
    /// @return Its id, or 0 for none.
    [[nodiscard]] int ringed() const noexcept { return _ringed; }

private:
    // Whether @p record is inside this spotlight.
    [[nodiscard]] bool within(Record const& record) const {
        for (int outer = record.parent; outer != 0; outer = store().at(outer).parent) {
            if (outer == id()) {
                return true;
            }
        }
        return false;
    }

    void focusInside() {
        for (auto const& [candidateId, record] : store().records()) {
            if (record.kind == "Button" && within(record) && shownInStore(store(), record)) {
                store().focus(candidateId);
                return;
            }
        }
        store().focus(id());
    }

    void resolve() {
        if (_resolving) {
            return;
        }
        _resolving = true;
        int found = 0;
        if (_target) {
            std::string const wanted = formatText(*_target);
            for (auto const& [candidateId, record] : store().records()) {
                auto const testId = record.props.find("testId");
                if (testId != record.props.end() && testId->second == wanted && candidateId != id() &&
                    !within(record) && shownInStore(store(), record)) {
                    found = candidateId;
                    break;
                }
            }
        }
        bool const was = _ringed != 0;
        if (found != _ringed) {
            _ringed = found;
            set("ringed", std::to_string(found));
        }
        bool const shown = found != 0;
        if (shown != was) {
            set("targetShown", formatBool(shown));
        }
        _resolving = false;
        if (shown != was && _onChanged) {
            auto const handler = _onChanged;
            handler(shown);
        }
    }

    int _listener;
    std::optional<std::string> _target;
    int _ringed = 0;
    int _restore = 0;
    bool _open = false;
    bool _resolving = false;
    std::function<void(bool)> _onChanged;
};
```

In `RecordingBackend`, after `createPopover`:

```cpp
    /// @brief A fake spotlight (`detail::FakeSpotlight`), resolving its target against this backend's live widgets.
    /// @param parent The container to append to, or null for a root.
    /// @return The widget.
    [[nodiscard]] std::unique_ptr<SpotlightWidget> createSpotlight(ContainerWidget* parent) override {
        return std::make_unique<detail::FakeSpotlight>(_store, parent);
    }
```

In `RecordingBackend::press`, between the focused widget's own chord and `ownKey`:

```cpp
        // Inside an open spotlight the card's chords come before the focused widget's own keys, but Enter and Space
        // stay the focused widget's.
        if (chord != "Enter" && chord != "Space") {
            if (int const spotlight = openSpotlightAround(widgetId); spotlight != 0) {
                for (int candidate = parent; candidate != 0;) {
                    detail::Record const& record = _store->at(candidate);
                    if (std::ranges::find(record.chords, chord) != record.chords.end()) {
                        invoke(record.callbacks.chord, chord);
                        return true;
                    }
                    if (candidate == spotlight) {
                        break;
                    }
                    candidate = record.parent;
                }
            }
        }
```

and a private helper beside `reachable`:

```cpp
    // The open spotlight @p widgetId is inside, or 0.
    [[nodiscard]] int openSpotlightAround(int widgetId) const {
        for (int outer = _store->at(widgetId).parent; outer != 0; outer = _store->at(outer).parent) {
            detail::Record const& record = _store->at(outer);
            if (record.kind == "Spotlight" && flagIs(record, "open", true)) {
                return outer;
            }
        }
        return 0;
    }
```

Extend `isClosedDialog` with `|| record.kind == "Spotlight"` inside the kind test, so a closed spotlight's children take
no input. Update `press`'s doc comment: after "Then the built-in keys of its kind" insert "— inside an open spotlight,
first the chords of its ancestors up to the spotlight, except for Enter and Space —".

`tests/ui_echoing_backend.hpp`, after `createPopover`:

```cpp
    std::unique_ptr<::morph::ui::SpotlightWidget> createSpotlight(::morph::ui::ContainerWidget* parent) override {
        created();
        return _recording.createSpotlight(parent);
    }
```

- [ ] **Step 5: The conformance cases** (`include/morph/ui/testing/backend_conformance.hpp`)

In `ConformanceProbe`, after `textExtentOf`:

```cpp
    /// @brief The widget a spotlight rings now, for the spotlight cases (`spotlightConformanceCases`).
    /// @param spotlight The spotlight.
    /// @return The widget, or null when it rings none or the probe reads none.
    [[nodiscard]] virtual Widget const* targetOf(SpotlightWidget const& spotlight) {
        static_cast<void>(spotlight);
        return nullptr;
    }
```

In `namespace detail`, before `conformanceCases()`'s namespace closes:

```cpp
/// @brief Case: a new spotlight is closed and its card takes no input; open, a click on a button in it runs the
///        button's action; closed again, it does not.
/// @param probe The backend under test.
/// @return `nullopt`, or what went wrong.
[[nodiscard]] inline std::optional<std::string> spotlightCardOnlyWhileOpen(ConformanceProbe& probe) {
    int clicks = 0;
    std::unique_ptr<SpotlightWidget> const spotlight = probe.backend().createSpotlight(nullptr);
    std::unique_ptr<ButtonWidget> const next = probe.backend().createButton(spotlight.get());
    next->setLabel("Next");
    next->setOnClick([&clicks] { ++clicks; });
    probe.settle();
    probe.click(*next);
    probe.settle();
    Checks checks;
    checks.that(clicks == 0, "a click reached a button inside a new spotlight; a new spotlight is closed");
    spotlight->setOpen(true);
    probe.settle();
    probe.click(*next);
    probe.settle();
    checks.that(clicks == 1, "a click on a button inside an open spotlight ran its action " + std::to_string(clicks) +
                                 " times");
    spotlight->setOpen(false);
    probe.settle();
    probe.click(*next);
    probe.settle();
    checks.that(clicks == 1, "a click reached a button inside a closed spotlight");
    return checks.result();
}

/// @brief Case: a spotlight named a test id nothing carries rings nothing; a widget made later with that id is found,
///        and the handler hears it once.
/// @param probe The backend under test.
/// @return `nullopt`, or what went wrong.
[[nodiscard]] inline std::optional<std::string> spotlightFindsLateTarget(ConformanceProbe& probe) {
    std::vector<bool> heard;
    std::unique_ptr<SpotlightWidget> const spotlight = probe.backend().createSpotlight(nullptr);
    spotlight->setOnTargetShownChanged([&heard](bool shown) { heard.push_back(shown); });
    spotlight->setOpen(true);
    spotlight->setTarget(std::string{"late"});
    probe.settle();
    Checks checks;
    checks.that(!spotlight->targetShown(), "a spotlight rings a test id nothing carries");
    std::unique_ptr<StackWidget> const root = probe.backend().createStack(nullptr, Axis::Vertical);
    std::unique_ptr<ButtonWidget> const late = probe.backend().createButton(root.get());
    late->setLabel("Late");
    late->setTestId("late");
    probe.settle();
    checks.that(spotlight->targetShown(), "a target made after the spotlight named it is not shown");
    checks.that(probe.targetOf(*spotlight) == late.get(), "a target made after the spotlight named it is not ringed");
    checks.that(heard == std::vector<bool>{true},
                "the handler heard " + std::to_string(heard.size()) + " changes, not one");
    return checks.result();
}

/// @brief Case: hiding the target, hiding a container around it, and destroying it each take it out of the
///        spotlight, and showing it again brings it back; the handler hears every change.
/// @param probe The backend under test.
/// @return `nullopt`, or what went wrong.
[[nodiscard]] inline std::optional<std::string> spotlightFollowsItsTarget(ConformanceProbe& probe) {
    std::vector<bool> heard;
    std::unique_ptr<StackWidget> const root = probe.backend().createStack(nullptr, Axis::Vertical);
    std::unique_ptr<ButtonWidget> target = probe.backend().createButton(root.get());
    target->setLabel("Target");
    target->setTestId("target");
    std::unique_ptr<SpotlightWidget> const spotlight = probe.backend().createSpotlight(nullptr);
    spotlight->setOnTargetShownChanged([&heard](bool shown) { heard.push_back(shown); });
    spotlight->setOpen(true);
    spotlight->setTarget(std::string{"target"});
    probe.settle();
    Checks checks;
    checks.that(spotlight->targetShown() && probe.targetOf(*spotlight) == target.get(),
                "a shown target is not ringed");
    target->setVisible(false);
    probe.settle();
    checks.that(!spotlight->targetShown(), "a hidden target is still shown");
    target->setVisible(true);
    probe.settle();
    checks.that(spotlight->targetShown(), "a target shown again is not ringed");
    root->setVisible(false);
    probe.settle();
    checks.that(!spotlight->targetShown(), "a target inside a hidden container is still shown");
    root->setVisible(true);
    probe.settle();
    target.reset();
    probe.settle();
    checks.that(!spotlight->targetShown() && probe.targetOf(*spotlight) == nullptr, "a destroyed target is still ringed");
    checks.that(heard == std::vector<bool>{true, false, true, false, true, false},
                "the handler heard " + std::to_string(heard.size()) + " changes, not six");
    return checks.result();
}

/// @brief Case: of two widgets with one test id the spotlight rings the first made that is shown.
/// @param probe The backend under test.
/// @return `nullopt`, or what went wrong.
[[nodiscard]] inline std::optional<std::string> spotlightRingsFirstShown(ConformanceProbe& probe) {
    std::unique_ptr<StackWidget> const root = probe.backend().createStack(nullptr, Axis::Vertical);
    std::unique_ptr<ButtonWidget> const first = probe.backend().createButton(root.get());
    first->setLabel("First");
    first->setTestId("twin");
    first->setVisible(false);
    std::unique_ptr<ButtonWidget> const second = probe.backend().createButton(root.get());
    second->setLabel("Second");
    second->setTestId("twin");
    std::unique_ptr<SpotlightWidget> const spotlight = probe.backend().createSpotlight(nullptr);
    spotlight->setOpen(true);
    spotlight->setTarget(std::string{"twin"});
    probe.settle();
    Checks checks;
    checks.that(probe.targetOf(*spotlight) == second.get(), "with the first twin hidden, the second is not ringed");
    first->setVisible(true);
    probe.settle();
    checks.that(probe.targetOf(*spotlight) == first.get(), "with both twins shown, the first made is not ringed");
    return checks.result();
}

/// @brief Case: inside a spotlight a chord its content declares comes before the focused widget's own keys (Left
///        reaches the content, not a segmented select), while Enter stays the focused button's.
/// @param probe The backend under test.
/// @return `nullopt`, or what went wrong.
[[nodiscard]] inline std::optional<std::string> spotlightContentKeysFirst(ConformanceProbe& probe) {
    std::vector<std::string> chords;
    std::vector<Key> chosen;
    int clicks = 0;
    std::unique_ptr<SpotlightWidget> const spotlight = probe.backend().createSpotlight(nullptr);
    spotlight->setOpen(true);
    Mounted const view{
        probe.runtime(), probe.backend(),
        ui::column({.children = {ui::select({.options = std::vector<SelectOption>{{.key = Key{std::int64_t{1}},
                                                                                   .label = "A"},
                                                                                  {.key = Key{std::int64_t{2}},
                                                                                   .label = "B"}},
                                             .selected = std::optional<Key>{Key{std::int64_t{2}}},
                                             .onSelect = [&chosen](Key const& key) { chosen.push_back(key); },
                                             .style = SelectStyle::Segmented}),
                                 ui::button({.label = "Next", .onClick = [&clicks] { ++clicks; }})},
                    .common = {.keys = {{.chord = "Left", .onPress = [&chords] { chords.emplace_back("Left"); }},
                                        {.chord = "Enter", .onPress = [&chords] { chords.emplace_back("Enter"); }}}}}),
        spotlight.get()};
    probe.settle();
    Widget* const select = childOf(probe, &view.root(), 0);
    Widget* const next = childOf(probe, &view.root(), 1);
    if (select == nullptr || next == nullptr) {
        return "the content mounted into a spotlight is not shown";
    }
    Checks checks;
    probe.press(*select, "Left");
    probe.settle();
    checks.that(chords == std::vector<std::string>{"Left"}, "Left inside a spotlight did not reach the content's chord");
    checks.that(chosen.empty(), "Left inside a spotlight chose an option: the select took it first");
    probe.press(*next, "Enter");
    probe.settle();
    checks.that(clicks == 1, "Enter on a button inside a spotlight did not click it");
    checks.that(chords.size() == 1, "Enter on a button inside a spotlight reached the content's chord");
    return checks.result();
}
```

After `namespace detail` closes, before `conformanceCases()`:

```cpp
/// @brief The spotlight's cases, which every backend that runs guided tours passes. They are their own list, so a
///        backend runs them once it draws a spotlight.
/// @return The cases.
[[nodiscard]] inline std::span<ConformanceCase const> spotlightConformanceCases() {
    static std::array<ConformanceCase, 5> const cases{{
        {.name = "a spotlight's card takes input only while it is open", .run = detail::spotlightCardOnlyWhileOpen},
        {.name = "a spotlight finds a target made after it was named", .run = detail::spotlightFindsLateTarget},
        {.name = "hiding or destroying a spotlight's target flips targetShown and tells the handler",
         .run = detail::spotlightFollowsItsTarget},
        {.name = "of two widgets with one test id a spotlight rings the first one shown",
         .run = detail::spotlightRingsFirstShown},
        {.name = "inside a spotlight a chord its content declares comes before the focused widget's own keys",
         .run = detail::spotlightContentKeysFirst},
    }};
    return cases;
}
```

- [ ] **Step 6: Run the tests to verify they pass**

Run:
```bash
flock /tmp/morph-build.lock cmake --build build/clang-debug -j5 --target morph_tests
ctest --test-dir build/clang-debug -R '^spotlight:|^ui conformance:|recording backend' --output-on-failure
```
Expected: `100% tests passed`, the existing conformance cases among them (57 still).

- [ ] **Step 7: Mutation check**

1. `FakeSpotlight::setTarget`: replace `_target = std::move(testId);` with `_target.reset();` → the first conformance
   test fails on cases 2–4; restore.
2. `RecordStore::set`: drop the `notify()` → "the recorded target follows the live widgets" fails; restore.
3. `RecordingBackend::press`: delete the spotlight block → case 5 fails ("Left inside a spotlight chose an option");
   restore. `touch` each file after restoring.

- [ ] **Step 8: Check that the Qt Quick and terminal backends still build**

Run: `flock /tmp/morph-build.lock cmake --build build/clang-debug -j5 --target morph_qt_quick_tests morph_tui_tests`
Expected: builds; they keep the throwing default until Tasks 4 and 5.

- [ ] **Step 9: Update the spec** (`docs/spec/ui/backend_contract.md`)

In "Widgets", after the `createCustom` bullet:

```markdown
- `createSpotlight(parent)` makes a `SpotlightWidget` ([The spotlight](#the-spotlight)). Its default throws
  `std::logic_error`: a backend that draws no spotlight cannot run a guided tour, and says so.
```

After "The popover", a new section:

```markdown
## The spotlight

`SpotlightWidget` (`createSpotlight(parent)`) shows a guided tour's card: a container whose children are the card's
content, drawn over the whole window whatever its parent, which takes no room in its parent's layout. A new spotlight
is closed and names no target.

- **The target.** `setTarget(testId)` names the widget to ring; `nullopt` rings nothing. Each backend keeps an index of
  its own live widgets by test id, updated when a widget's test id is set and when it is destroyed, and the spotlight
  rings the first, in creation order, that is effectively shown: visible itself, every container around it visible,
  every overlay around it open and no closed section around it. Several widgets can carry one id (a hidden tab showing
  the same screen, a repeated row). It picks again on every change of the index and every change of a candidate's or
  a container's visibility. `targetShown()` says whether it rings one; `setOnTargetShownChanged(handler)` hears each
  change. With nothing shown the card is centred with no cut-out. A widget of another backend, such as the
  inspector's window, is never a candidate.
- **Focus.** Opening moves keyboard focus into the card (its first shown button, or its first focusable widget, or the
  spotlight); closing hands it back to the widget that had it when it opened. Tab stays in the card;
  `setTargetFocusable(true)` adds the ringed widget after the card's widgets, so a step that waits for the person can
  be finished from the keyboard.
- **Keys.** A chord a widget inside the spotlight declares is delivered before the focused widget's own keys (so a
  focused segmented select or row cannot take Left and Right from the card), except Enter and Space, which the
  focused widget takes first as it always does.
- **Pointer.** A press on the dimmed area reaches nothing; a press inside the ringed widget's box reaches it.

Qt Quick dims the window with four scrim rectangles and rings the target in the focus colour; the terminal places the
card's box under or over the target and draws the target in reverse video, dimming nothing
([qt_quick/backend.md](../qt_quick/backend.md), [tui/frontend.md](../tui/frontend.md)). The recording backend records
`open`, `target` (`none` for none), `targetFocusable`, and, resolving its target against its live widgets on every
change, `targetShown` and `ringed` (the ringed widget's id, `0` for none).
```

In "RecordingBackend", add to its list of kinds and properties: "`Spotlight` (`open`, `target`, `targetFocusable`,
`targetShown`, `ringed`); its store tells listeners of every change that can move a spotlight's target, and `press`
delivers the chords of an open spotlight's content before the focused widget's own keys except Enter and Space."

In "The conformance suite", after the list of cases: "`spotlightConformanceCases()` holds five more, for the
spotlight: its card takes input only while open; it finds a target made after it was named; hiding the target, hiding
a container around it and destroying it flip `targetShown` and reach the handler; of two widgets with one id it rings
the first shown; inside it the content's chords come before the focused widget's own keys, Enter excepted. They read
the ringed widget through `ConformanceProbe::targetOf`, whose default reads none, so a probe that does not implement it
fails them. A backend runs them once it draws a spotlight."

- [ ] **Step 10: Commit**

```bash
git add include/morph/ui/backend.hpp src/ui/backend.cpp include/morph/ui/testing/recording_backend.hpp \
  include/morph/ui/testing/backend_conformance.hpp tests/ui_echoing_backend.hpp tests/test_ui_conformance.cpp \
  tests/test_ui_spotlight_recording.cpp tests/CMakeLists.txt docs/spec/ui/backend_contract.md
git commit -m "Add the spotlight widget to the backend contract"
```

---
### Task 4: The Qt Quick spotlight

**Lane:** wave 2, after Task 3; parallel with Tasks 2 and 5.

**Files:**
- Modify: `src/qt_quick/context.hpp`, `src/qt_quick/context.cpp` (`WidgetIndex`; `Context::widgets()`;
  `Context::all()`)
- Modify: `src/qt_quick/widget.hpp` (`liveWidgets` returns by value, ~line 195), `src/qt_quick/widget.cpp`
  (`registry()` removed ~lines 61–80; the two `ItemBase` constructors and the destructor ~lines 82–110; `showTestId`
  ~line 267)
- Modify: `src/qt_quick/kinds.hpp` (`SpotlightImpl` after `PopoverImpl` ~line 520)
- Create: `src/qt_quick/spotlight.cpp`, `src/qt_quick/qml/MorphSpotlight.qml`
- Modify: `src/qt_quick/qml/MorphWidget.qml` (`takeChord`, after `takeOwnChord` ~line 272)
- Modify: `include/morph/qt_quick/backend.hpp` (`createSpotlight` after `createPopover` ~line 118),
  `src/qt_quick/backend.cpp` (after `createPopover` ~line 129), `src/qt_quick/CMakeLists.txt` (`spotlight.cpp`; `qml/MorphSpotlight.qml`
  after `qml/MorphPopover.qml`)
- Test: `tests/qt_quick/test_qt_quick_spotlight.cpp` (create); `tests/qt_quick/CMakeLists.txt` (after
  `test_qt_quick_overlays.cpp`); `tests/qt_quick/test_qt_quick_conformance.cpp` (`QtProbe::targetOf`, one test case)
- Docs: `docs/spec/qt_quick/backend.md`

**Interfaces:**
- Consumes: Task 3's `ui::SpotlightWidget`, `ui::testing::spotlightConformanceCases()`,
  `ConformanceProbe::targetOf`; `ItemBase::item()`, `focusTarget()`, `part()`, `write()`; `ContainerItemWidget`.
- Produces:
  - `class qt_quick::detail::WidgetIndex { void add(ItemBase&); void remove(ItemBase const&); void retag(ItemBase
    const&, QString const&); std::vector<ItemBase*> carrying(QString const&) const; std::vector<ItemBase*> const&
    all() const noexcept; int listen(std::function<void()>); void unlisten(int) noexcept; };`
  - `WidgetIndex& Context::widgets() noexcept;`, `static std::vector<Context*> const& Context::all() noexcept;`
  - `std::vector<ItemBase*> liveWidgets();` (by value; every context's widgets, contexts and widgets in creation order)
  - `class qt_quick::detail::SpotlightImpl final : public ContainerItemWidget<ui::SpotlightWidget>` with
    `ItemBase const* ringed() const noexcept;`, `void track();`, `void resolve();`, `bool filterKey(QKeyEvent const&);`
  - `std::unique_ptr<ui::SpotlightWidget> qt_quick::Backend::createSpotlight(ui::ContainerWidget* parent) override;`

- [ ] **Step 1: Write the failing tests** (`tests/qt_quick/test_qt_quick_spotlight.cpp`)

```cpp
// SPDX-License-Identifier: Apache-2.0
//
// The spotlight on the real Qt Quick backend (docs/spec/qt_quick/backend.md, "The spotlight"): the cut-out and the
// ring, where the card goes, what the pointer and the keyboard reach, scrolling a clipped target into view, and that
// a widget of another window is never the target. Set MORPH_SPOTLIGHT_GRAB to a file to keep a picture.

#include <QImage>
#include <QPointF>
#include <QQuickItem>
#include <QQuickWindow>
#include <QRectF>
#include <QString>
#include <QTest>
#include <catch2/catch_test_macros.hpp>
#include <cstdlib>
#include <memory>
#include <morph/qt_quick/backend.hpp>
#include <morph/ui/backend.hpp>
#include <string>

#include "qt_quick/kinds.hpp"
#include "qt_quick_harness.hpp"

namespace {

using morph::qt_quick::testing::Scene;
namespace detail = morph::qt_quick::detail;
namespace ui = morph::ui;

QRectF sceneBox(QQuickItem const& item) { return item.mapRectToScene(QRectF{0, 0, item.width(), item.height()}); }

/// A row of three buttons near the top of the window; the middle one carries `target`.
struct Page {
    explicit Page(Scene& scene) {
        root = scene.backend().createStack(nullptr, ui::Axis::Vertical);
        row = scene.backend().createStack(root.get(), ui::Axis::Horizontal);
        before = scene.backend().createButton(row.get());
        before->setLabel("Before");
        target = scene.backend().createButton(row.get());
        target->setLabel("Target");
        target->setTestId("target");
        after = scene.backend().createButton(row.get());
        after->setLabel("After");
    }
    std::unique_ptr<ui::StackWidget> root;
    std::unique_ptr<ui::StackWidget> row;
    std::unique_ptr<ui::ButtonWidget> before;
    std::unique_ptr<ui::ButtonWidget> target;
    std::unique_ptr<ui::ButtonWidget> after;
};

/// A spotlight with a card of two buttons, open on `target`.
struct Card {
    explicit Card(Scene& scene) {
        spotlight = scene.backend().createSpotlight(nullptr);
        back = scene.backend().createButton(spotlight.get());
        back->setLabel("Back");
        next = scene.backend().createButton(spotlight.get());
        next->setLabel("Next");
        spotlight->setTarget(std::string{"target"});
        spotlight->setOpen(true);
        scene.settle();
        impl().track();
    }
    [[nodiscard]] detail::SpotlightImpl& impl() const { return dynamic_cast<detail::SpotlightImpl&>(*spotlight); }
    [[nodiscard]] QQuickItem& item() const { return Scene::itemOf(*spotlight); }
    std::unique_ptr<ui::SpotlightWidget> spotlight;
    std::unique_ptr<ui::ButtonWidget> back;
    std::unique_ptr<ui::ButtonWidget> next;
};

}  // namespace

// Mutation: write the cut-out without the target's scene position (it sits at the window's corner).
TEST_CASE("qt_quick spotlight: the cut-out is the target's scene box, ringed one spacing unit out", "[qt_quick][tours]") {
    Scene scene;
    scene.window().resize(1280, 800);
    Page const page{scene};
    Card const card{scene};
    QQuickItem const& target = Scene::itemOf(*page.target);
    CHECK(card.spotlight->targetShown());
    CHECK(card.item().property("ringed").toBool());
    QRectF const cutout = card.item().property("cutout").toRectF();
    CHECK(cutout == sceneBox(target));
    qreal const pad = card.item().property("ringPad").toReal();
    CHECK(pad > 0);
    auto const* const ring = card.item().property("ring").value<QQuickItem*>();
    REQUIRE(ring != nullptr);
    CHECK(sceneBox(*ring) == cutout.adjusted(-pad, -pad, pad, pad));
    if (char const* const file = std::getenv("MORPH_SPOTLIGHT_GRAB")) {  // NOLINT(concurrency-mt-unsafe): one thread.
        CHECK(scene.window().grabWindow().save(QString::fromUtf8(file)));
    }
}

// Mutation: place the card at the cut-out's own position (it covers the target).
TEST_CASE("qt_quick spotlight: the card stays inside the window and off the target, wide and compact",
          "[qt_quick][tours]") {
    for (int const width : {1280, 420}) {
        INFO("window width " << width);
        Scene scene;
        scene.window().resize(width, 800);
        Page const page{scene};
        Card const card{scene};
        auto const* const box = card.item().property("card").value<QQuickItem*>();
        REQUIRE(box != nullptr);
        QRectF const placed = sceneBox(*box);
        QRectF const window{0, 0, static_cast<qreal>(scene.window().width()), static_cast<qreal>(scene.window().height())};
        CHECK(window.contains(placed));
        CHECK_FALSE(placed.intersects(sceneBox(Scene::itemOf(*page.target))));
    }
}

// Mutation: give the cut-out a scrim rectangle of its own (the click on the target is swallowed).
TEST_CASE("qt_quick spotlight: a click inside the cut-out reaches the target, one on the scrim reaches nothing",
          "[qt_quick][tours]") {
    Scene scene;
    scene.window().resize(1280, 800);
    Page const page{scene};
    int targetClicks = 0;
    int beforeClicks = 0;
    page.target->setOnClick([&targetClicks] { ++targetClicks; });
    page.before->setOnClick([&beforeClicks] { ++beforeClicks; });
    Card const card{scene};
    scene.click(Scene::itemOf(*page.target));
    scene.click(Scene::itemOf(*page.before));
    CHECK(targetClicks == 1);
    CHECK(beforeClicks == 0);
}

// Mutation: skip `scrollIntoView` in `track` (the target stays below the scroll area's box).
TEST_CASE("qt_quick spotlight: a target in a scrolled-away ScrollWidget is scrolled into view", "[qt_quick][tours]") {
    Scene scene;
    scene.window().resize(800, 600);
    auto const root = scene.backend().createStack(nullptr, ui::Axis::Vertical);
    auto const scroll = scene.backend().createScroll(root.get(), ui::Axis::Vertical);
    scroll->setLayout({.width = ui::Sizing::stretch(), .height = ui::Sizing::fixed(6)});
    std::vector<std::unique_ptr<ui::TextWidget>> lines;
    for (int index = 0; index < 60; ++index) {
        lines.push_back(scene.backend().createText(scroll.get()));
        lines.back()->setText("Line " + std::to_string(index));
    }
    lines.back()->setTestId("target");
    scene.settle();
    QRectF const viewport = sceneBox(Scene::itemOf(*scroll));
    REQUIRE_FALSE(viewport.contains(sceneBox(Scene::itemOf(*lines.back()))));
    auto const spotlight = scene.backend().createSpotlight(nullptr);
    spotlight->setTarget(std::string{"target"});
    spotlight->setOpen(true);
    scene.settle();
    dynamic_cast<detail::SpotlightImpl&>(*spotlight).track();
    scene.settle();
    CHECK(viewport.contains(sceneBox(Scene::itemOf(*lines.back()))));
}

// Mutation: let Tab through in `filterKey` (focus leaves the card for the page's buttons).
TEST_CASE("qt_quick spotlight: Tab stays in the card, and on a waiting step reaches the target", "[qt_quick][tours]") {
    Scene scene;
    scene.window().resize(1280, 800);
    Page const page{scene};
    Card const card{scene};
    QQuickItem const& back = Scene::controlOf(*card.back);
    QQuickItem const& next = Scene::controlOf(*card.next);
    QQuickItem const& target = Scene::controlOf(*page.target);
    CHECK(back.hasActiveFocus());
    scene.key(Qt::Key_Tab);
    CHECK(next.hasActiveFocus());
    scene.key(Qt::Key_Tab);
    CHECK(back.hasActiveFocus());
    card.spotlight->setTargetFocusable(true);
    scene.key(Qt::Key_Tab);
    scene.key(Qt::Key_Tab);
    CHECK(target.hasActiveFocus());
    scene.key(Qt::Key_Tab);
    CHECK(back.hasActiveFocus());
}

// Mutation: leave the focus where it is in `setOpen(false)` (the field never gets it back).
TEST_CASE("qt_quick spotlight: closing hands focus back to where it was", "[qt_quick][tours]") {
    Scene scene;
    auto const root = scene.backend().createStack(nullptr, ui::Axis::Vertical);
    auto const field = scene.backend().createTextInput(root.get(), ui::TextInputMode::SingleLine);
    scene.settle();
    field->focus();
    scene.settle();
    Card const card{scene};
    CHECK_FALSE(Scene::controlOf(*field).hasActiveFocus());
    card.spotlight->setOpen(false);
    scene.settle();
    CHECK(Scene::controlOf(*field).hasActiveFocus());
}

// Mutation: make `findWidget` and the index one process-wide list again (the other window's twin is ringed).
TEST_CASE("qt_quick spotlight: a widget of another window is never the target", "[qt_quick][tours]") {
    Scene scene;
    QQuickWindow other;
    other.resize(400, 300);
    morph::qt_quick::Backend second{scene.engine(), *other.contentItem()};
    auto const elsewhere = second.createButton(nullptr);
    elsewhere->setLabel("Elsewhere");
    elsewhere->setTestId("target");
    auto const spotlight = scene.backend().createSpotlight(nullptr);
    spotlight->setTarget(std::string{"target"});
    spotlight->setOpen(true);
    scene.settle();
    CHECK_FALSE(spotlight->targetShown());
}
```

`tests/qt_quick/test_qt_quick_conformance.cpp`: in `QtProbe`, after `boundsOf`:

```cpp
    ui::Widget const* targetOf(ui::SpotlightWidget const& spotlight) override {
        auto const* const impl = dynamic_cast<detail::SpotlightImpl const*>(&spotlight);
        detail::ItemBase const* const ringed = impl == nullptr ? nullptr : impl->ringed();
        return ringed == nullptr ? nullptr : dynamic_cast<ui::Widget const*>(ringed);
    }
```

and after the layout test case:

```cpp
TEST_CASE("qt_quick conformance: the Qt Quick backend passes every spotlight case", "[qt_quick][conformance][tours]") {
    for (auto const& testCase : ui::testing::spotlightConformanceCases()) {
        QtProbe probe;
        std::optional<std::string> const failure = testCase.run(probe);
        INFO(std::string{testCase.name} + ": " + failure.value_or("passed"));
        CHECK_FALSE(failure.has_value());
    }
}
```

Add `test_qt_quick_spotlight.cpp` to `tests/qt_quick/CMakeLists.txt` after `test_qt_quick_overlays.cpp`.

- [ ] **Step 2: Run the tests to verify they fail**

Run: `flock /tmp/morph-build.lock cmake --build build/clang-debug -j5 --target morph_qt_quick_tests`
Expected: compile error — `SpotlightImpl` is not a member of `morph::qt_quick::detail`.

- [ ] **Step 3: The per-context index** (`src/qt_quick/context.hpp`, before `class Context`)

```cpp
class ItemBase;

/// The live widgets of one context, in creation order, by test id, and what wants to hear when that changes. A
/// spotlight finds its target here, so a widget of another window (the inspector's) is never one.
class WidgetIndex {
public:
    /// Adds a widget just made.
    void add(ItemBase& widget);
    /// Forgets a widget being destroyed, and tells the listeners.
    void remove(ItemBase const& widget);
    /// Files a widget under its new test id, and tells the listeners.
    void retag(ItemBase const& widget, QString const& testId);
    /// The live widgets that carry @p testId, in creation order.
    [[nodiscard]] std::vector<ItemBase*> carrying(QString const& testId) const;
    /// Every live widget, in creation order.
    [[nodiscard]] std::vector<ItemBase*> const& all() const noexcept { return _widgets; }
    /// Registers a listener told after each retag and removal; returns its key.
    int listen(std::function<void()> listener);
    /// Stops telling a listener.
    void unlisten(int key) noexcept { _listeners.erase(key); }

private:
    // Tells every listener once; a change a listener makes is told after the round, never inside it.
    void changed();

    std::vector<ItemBase*> _widgets;
    std::map<ItemBase const*, QString> _tags;
    std::map<int, std::function<void()>> _listeners;
    int _nextListener = 0;
    bool _telling = false;
    bool _again = false;
};
```

In `class Context`, public:

```cpp
    /// The live widgets of this context, by test id.
    [[nodiscard]] WidgetIndex& widgets() noexcept { return _widgets; }

    /// Every live context, in creation order: what a test's `findWidget` reads.
    [[nodiscard]] static std::vector<Context*> const& all() noexcept;
```

private: `WidgetIndex _widgets;` (declared before `_roots`, so the roots, whose widgets leave the index as they go, are
destroyed first).

`src/qt_quick/context.cpp`:

```cpp
namespace {

std::vector<Context*>& contexts() {
    static std::vector<Context*> live;
    return live;
}

}  // namespace

std::vector<Context*> const& Context::all() noexcept { return contexts(); }

void WidgetIndex::add(ItemBase& widget) { _widgets.push_back(&widget); }

void WidgetIndex::remove(ItemBase const& widget) {
    std::erase(_widgets, &widget);
    if (_tags.erase(&widget) != 0) {
        changed();
    }
}

void WidgetIndex::retag(ItemBase const& widget, QString const& testId) {
    _tags.insert_or_assign(&widget, testId);
    changed();
}

std::vector<ItemBase*> WidgetIndex::carrying(QString const& testId) const {
    std::vector<ItemBase*> out;
    for (ItemBase* const widget : _widgets) {
        if (auto const found = _tags.find(widget); found != _tags.end() && found->second == testId) {
            out.push_back(widget);
        }
    }
    return out;
}

int WidgetIndex::listen(std::function<void()> listener) {
    int const key = ++_nextListener;
    _listeners.insert_or_assign(key, std::move(listener));
    return key;
}

void WidgetIndex::changed() {
    if (_telling) {
        _again = true;
        return;
    }
    _telling = true;
    do {
        _again = false;
        std::vector<int> keys;
        for (auto const& [key, listener] : _listeners) {
            static_cast<void>(listener);
            keys.push_back(key);
        }
        for (int const key : keys) {
            if (auto const found = _listeners.find(key); found != _listeners.end()) {
                auto const listener = found->second;
                listener();
            }
        }
    } while (_again);
    _telling = false;
}
```

In `Context::Context`, last statement: `contexts().push_back(this);`. In `Context::~Context`, first statement:
`std::erase(contexts(), this);`.

- [ ] **Step 4: Widgets join their context's index** (`src/qt_quick/widget.cpp`, `widget.hpp`)

Delete `registry()`. In both `ItemBase` constructors replace `registry().push_back(this);` with
`_context->widgets().add(*this);` (the second constructor: `context.widgets().add(*this);`). In `~ItemBase`, replace
`std::erase(registry(), this);` with `_context->widgets().remove(*this);`. At the end of `showTestId`:

```cpp
    _context->widgets().retag(*this, identifier);
```

Replace `liveWidgets` and `findWidget`:

```cpp
std::vector<ItemBase*> liveWidgets() {
    std::vector<ItemBase*> out;
    for (Context* const context : Context::all()) {
        auto const& widgets = context->widgets().all();
        out.insert(out.end(), widgets.begin(), widgets.end());
    }
    return out;
}

ItemBase* findWidget(QString const& testId) noexcept {
    for (Context* const context : Context::all()) {
        for (ItemBase* const widget : context->widgets().all()) {
            if (widget->item() != nullptr && widget->item()->objectName() == testId) {
                return widget;
            }
        }
    }
    return nullptr;
}
```

In `widget.hpp`, change the declarations' comments and `liveWidgets`'s return type:

```cpp
/// Every live widget of every context, contexts and widgets in creation order: for tests and tools.
[[nodiscard]] std::vector<ItemBase*> liveWidgets();

/// The first live widget, contexts and widgets in creation order, whose test id is @p testId; null when there is
/// none. A test helper: a spotlight reads its own context's index, never this.
[[nodiscard]] ItemBase* findWidget(QString const& testId) noexcept;
```

- [ ] **Step 5: `takeChord` for the spotlight** (`src/qt_quick/qml/MorphWidget.qml`, after `takeOwnChord`)

```qml
    // Runs the chord a key is when this widget declares it, called from C++: a spotlight hands its card's keys to the
    // card's widgets before the focused control's own handling.
    function takeChord(key: int, modifiers: int): bool {
        return root.chords.length > 0 && root.question("chord", [key, modifiers])
    }
```

- [ ] **Step 6: The QML** (`src/qt_quick/qml/MorphSpotlight.qml`)

```qml
// SPDX-License-Identifier: Apache-2.0
pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Layouts

// A guided tour's spotlight: four scrim rectangles dim the window around the target's box, a ring in the focus colour
// surrounds it, and the card (`content`) sits on the side of the target with the most room, inside the window. The
// layer is an item of the window's content item above everything else, not a popup: a popup's item takes every press
// inside it, and a press inside the cut-out must reach the target. The scrim and the card take the presses on them.
MorphWidget {
    id: root

    property bool open: false
    // The target's box in window coordinates, as C++ tracks it; `ringed` false centres the card with no cut-out.
    property rect cutout: Qt.rect(0, 0, 0, 0)
    property bool ringed: false
    readonly property real ringPad: Theme.spaceOf(1)
    readonly property Item content: column
    readonly property Item card: card
    readonly property Item ring: ring

    focusTarget: column
    implicitWidth: 0
    implicitHeight: 0
    // Only a zero-sized placeholder: the layer below takes no room in the container's layout.
    Layout.preferredWidth: -1
    Layout.fillWidth: false
    Layout.preferredHeight: -1
    Layout.fillHeight: false

    Item {
        id: layer
        parent: root.Window.window ? root.Window.window.contentItem : root
        anchors.fill: parent
        z: 10000
        visible: root.open

        readonly property rect hole: root.ringed
            ? Qt.rect(root.cutout.x - root.ringPad, root.cutout.y - root.ringPad,
                      root.cutout.width + 2 * root.ringPad, root.cutout.height + 2 * root.ringPad)
            : Qt.rect(width / 2, height / 2, 0, 0)

        component Scrim: Rectangle {
            color: Theme.scrim
            MouseArea {
                anchors.fill: parent
                acceptedButtons: Qt.AllButtons
                hoverEnabled: true
                onWheel: wheel => wheel.accepted = true
            }
        }

        Scrim {
            objectName: "spotlight_scrim_top"
            x: 0
            y: 0
            width: layer.width
            height: Math.max(0, layer.hole.y)
        }
        Scrim {
            objectName: "spotlight_scrim_bottom"
            x: 0
            y: layer.hole.y + layer.hole.height
            width: layer.width
            height: Math.max(0, layer.height - y)
        }
        Scrim {
            objectName: "spotlight_scrim_left"
            x: 0
            y: layer.hole.y
            width: Math.max(0, layer.hole.x)
            height: layer.hole.height
        }
        Scrim {
            objectName: "spotlight_scrim_right"
            x: layer.hole.x + layer.hole.width
            y: layer.hole.y
            width: Math.max(0, layer.width - x)
            height: layer.hole.height
        }

        Rectangle {
            id: ring
            objectName: "spotlight_ring"
            visible: root.ringed
            x: layer.hole.x
            y: layer.hole.y
            width: layer.hole.width
            height: layer.hole.height
            color: "transparent"
            border.color: Theme.focus
            border.width: 2
            radius: Theme.radiusControl
        }

        Rectangle {
            id: card
            objectName: "spotlight_card"
            readonly property real gap: Theme.spaceOf(2)
            readonly property real pad: Theme.spaceOf(4)
            // The room on each side of the hole, less the card: the side with the most is where the card goes.
            readonly property var rooms: [
                layer.height - (layer.hole.y + layer.hole.height) - gap - height,
                layer.hole.y - gap - height,
                layer.width - (layer.hole.x + layer.hole.width) - gap - width,
                layer.hole.x - gap - width
            ]
            readonly property int side: {
                if (!root.ringed)
                    return -1
                let best = 0
                for (let index = 1; index < 4; ++index) {
                    if (rooms[index] > rooms[best])
                        best = index
                }
                return best
            }

            function clamp(value: real, low: real, high: real): real {
                return Math.max(low, Math.min(value, high))
            }

            width: Math.min(column.implicitWidth + 2 * pad, Theme.unitWidth * 40, layer.width - 2 * gap)
            height: column.implicitHeight + 2 * pad
            x: clamp(side === -1 ? (layer.width - width) / 2
                     : side === 2 ? layer.hole.x + layer.hole.width + gap
                     : side === 3 ? layer.hole.x - gap - width
                     : layer.hole.x, gap, layer.width - width - gap)
            y: clamp(side === -1 ? (layer.height - height) / 2
                     : side === 0 ? layer.hole.y + layer.hole.height + gap
                     : side === 1 ? layer.hole.y - gap - height
                     : layer.hole.y, gap, layer.height - height - gap)
            color: Theme.cardScheme.surface
            border.color: Theme.border
            radius: Theme.radiusCard

            // Under the content, so a press on the card's padding reaches nothing behind it.
            MouseArea {
                anchors.fill: parent
                acceptedButtons: Qt.AllButtons
            }

            ColumnLayout {
                id: column
                x: card.pad
                y: card.pad
                width: card.width - 2 * card.pad
                spacing: Theme.spaceOf(2)
            }
        }
    }
}
```

Add `qml/MorphSpotlight.qml` to `src/qt_quick/CMakeLists.txt` after `qml/MorphPopover.qml`, and `spotlight.cpp` to the
library's sources.

- [ ] **Step 7: The widget** (`src/qt_quick/kinds.hpp`, after `PopoverImpl`)

```cpp
/// A guided tour's spotlight (`MorphSpotlight`): it finds its target in its own context's index, follows the
/// target's box each frame, scrolls a clipped target into view, and while open filters the window's keys so the card's
/// chords come first and Tab stays in the card.
class SpotlightImpl final : public ContainerItemWidget<ui::SpotlightWidget> {
public:
    SpotlightImpl(Context& context, ui::ContainerWidget* parent);
    ~SpotlightImpl() override;
    SpotlightImpl(SpotlightImpl const&) = delete;
    SpotlightImpl& operator=(SpotlightImpl const&) = delete;
    SpotlightImpl(SpotlightImpl&&) = delete;
    SpotlightImpl& operator=(SpotlightImpl&&) = delete;

    void setOpen(bool open) override;
    void setTarget(std::optional<std::string> testId) override;
    [[nodiscard]] bool targetShown() const override { return _ringed != nullptr; }
    void setOnTargetShownChanged(std::function<void(bool)> handler) override { _onChanged = std::move(handler); }
    void setTargetFocusable(bool focusable) override { _targetFocusable = focusable; }

    /// The widget it rings now, or null.
    [[nodiscard]] ItemBase const* ringed() const noexcept { return _ringed; }
    /// Reads the target's box again, scrolls a clipped target into view and writes the cut-out: what each frame runs.
    void track();
    /// Picks the target again from the context's index, and hears its candidates' visibility.
    void resolve();
    /// A key pressed while it is open, before the focused item has it: true when it took the key.
    bool filterKey(QKeyEvent const& event);

protected:
    /// The card draws as the theme's card, whatever surface encloses the spotlight.
    [[nodiscard]] QObject* scheme() const override { return context().themeObject("cardScheme"); }

private:
    class Filter;
    // The card's items that Tab stops at, then the target while it is focusable.
    [[nodiscard]] std::vector<QQuickItem*> tabChain() const;
    void cycleFocus(bool forward);
    void focusCard();

    std::unique_ptr<Filter> _filter;
    std::optional<QString> _target;
    ItemBase* _ringed = nullptr;
    std::vector<QMetaObject::Connection> _watching;
    QMetaObject::Connection _frames;
    QPointer<QQuickItem> _restore;
    QPointer<QQuickWindow> _window;
    int _listener = 0;
    bool _open = false;
    bool _targetFocusable = false;
    bool _resolving = false;
    std::function<void(bool)> _onChanged;
};
```

(Add `#include <QKeyEvent>`, `#include <QQuickWindow>` and `#include <optional>` to `kinds.hpp` if absent.)

`src/qt_quick/spotlight.cpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0

#include <QEvent>
#include <QKeyEvent>
#include <QMetaObject>
#include <QObject>
#include <QQuickItem>
#include <QQuickWindow>
#include <QRectF>
#include <QVariant>
#include <algorithm>
#include <cstddef>
#include <functional>
#include <iterator>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "qt_quick/keys.hpp"
#include "qt_quick/kinds.hpp"

namespace morph::qt_quick::detail {

namespace {

// Depth first: the items Tab stops at, shown and enabled.
// NOLINTNEXTLINE(misc-no-recursion): an item tree, walked down.
void collectTabStops(QQuickItem& item, std::vector<QQuickItem*>& out) {
    for (QQuickItem* const child : item.childItems()) {
        if (!child->isVisible() || !child->isEnabled()) {
            continue;
        }
        if (child->activeFocusOnTab()) {
            out.push_back(child);
        }
        collectTabStops(*child, out);
    }
}

// Scrolls the nearest flickable around @p target until the target's box is inside its viewport.
void scrollIntoView(QQuickItem& target) {
    for (QQuickItem* outer = target.parentItem(); outer != nullptr; outer = outer->parentItem()) {
        if (!outer->inherits("QQuickFlickable")) {
            continue;
        }
        auto* const content = outer->property("contentItem").value<QQuickItem*>();
        if (content == nullptr) {
            return;
        }
        QRectF const box = target.mapRectToItem(content, QRectF{0, 0, target.width(), target.height()});
        qreal const top = outer->property("contentY").toReal();
        qreal const left = outer->property("contentX").toReal();
        if (box.top() < top) {
            outer->setProperty("contentY", box.top());
        } else if (box.bottom() > top + outer->height()) {
            outer->setProperty("contentY", box.bottom() - outer->height());
        }
        if (box.left() < left) {
            outer->setProperty("contentX", box.left());
        } else if (box.right() > left + outer->width()) {
            outer->setProperty("contentX", box.right() - outer->width());
        }
        return;
    }
}

}  // namespace

// The window's key filter while the spotlight is open: keys reach the spotlight before the focused item.
class SpotlightImpl::Filter final : public QObject {
public:
    explicit Filter(SpotlightImpl& owner) : _owner{&owner} {}

    bool eventFilter(QObject* watched, QEvent* event) override {
        if (event->type() == QEvent::KeyPress) {
            return _owner->filterKey(*static_cast<QKeyEvent*>(event));  // NOLINT(cppcoreguidelines-pro-type-static-cast-downcast)
        }
        return QObject::eventFilter(watched, event);
    }

private:
    SpotlightImpl* _owner;
};

SpotlightImpl::SpotlightImpl(Context& context, ui::ContainerWidget* parent)
    : ContainerItemWidget{context, QStringLiteral("MorphSpotlight"), parent},
      _filter{std::make_unique<Filter>(*this)},
      _listener{context.widgets().listen([this] {
          resolve();
          track();
      })} {}

SpotlightImpl::~SpotlightImpl() {
    context().widgets().unlisten(_listener);
    for (QMetaObject::Connection const& connection : _watching) {
        QObject::disconnect(connection);
    }
    QObject::disconnect(_frames);
    if (_window != nullptr) {
        _window->removeEventFilter(_filter.get());
    }
}

void SpotlightImpl::setOpen(bool open) {
    if (open == _open) {
        return;
    }
    _open = open;
    write("open", open);
    if (open) {
        _window = item() != nullptr ? item()->window() : nullptr;
        if (_window != nullptr) {
            _restore = _window->activeFocusItem();
            _window->installEventFilter(_filter.get());
            _frames = QObject::connect(_window, &QQuickWindow::afterAnimating, _filter.get(), [this] { track(); });
        }
        resolve();
        track();
        focusCard();
        return;
    }
    if (_window != nullptr) {
        _window->removeEventFilter(_filter.get());
    }
    QObject::disconnect(_frames);
    _window.clear();
    if (QQuickItem* const restore = _restore) {
        restore->forceActiveFocus(Qt::OtherFocusReason);
    }
    _restore.clear();
}

void SpotlightImpl::setTarget(std::optional<std::string> testId) {
    _target = testId ? std::optional<QString>{toQString(*testId)} : std::nullopt;
    resolve();
    track();
}

void SpotlightImpl::resolve() {
    if (_resolving) {
        return;
    }
    _resolving = true;
    for (QMetaObject::Connection const& connection : _watching) {
        QObject::disconnect(connection);
    }
    _watching.clear();
    ItemBase* found = nullptr;
    auto const* const card = qobject_cast<QQuickItem*>(part("card"));
    if (_target) {
        for (ItemBase* const candidate : context().widgets().carrying(*_target)) {
            QQuickItem* const at = candidate->item();
            if (at == nullptr || candidate == this || (card != nullptr && card->isAncestorOf(at))) {
                continue;
            }
            // Its effective visibility: a hidden container around it, or a tab page not selected, hides it too.
            _watching.push_back(QObject::connect(at, &QQuickItem::visibleChanged, _filter.get(), [this] {
                resolve();
                track();
            }));
            if (found == nullptr && at->isVisible()) {
                found = candidate;
            }
        }
    }
    bool const was = _ringed != nullptr;
    _ringed = found;
    write("ringed", found != nullptr);
    _resolving = false;
    if ((found != nullptr) != was && _onChanged) {
        auto const handler = _onChanged;
        handler(found != nullptr);
    }
}

void SpotlightImpl::track() {
    if (_ringed == nullptr || _ringed->item() == nullptr) {
        return;
    }
    QQuickItem& target = *_ringed->item();
    scrollIntoView(target);
    write("cutout", target.mapRectToScene(QRectF{0, 0, target.width(), target.height()}));
}

std::vector<QQuickItem*> SpotlightImpl::tabChain() const {
    std::vector<QQuickItem*> chain;
    if (auto* const content = qobject_cast<QQuickItem*>(part("content"))) {
        collectTabStops(*content, chain);
    }
    if (_targetFocusable && _ringed != nullptr && _ringed->focusTarget() != nullptr) {
        chain.push_back(_ringed->focusTarget());
    }
    return chain;
}

void SpotlightImpl::focusCard() {
    std::vector<QQuickItem*> const chain = tabChain();
    if (!chain.empty()) {
        chain.front()->forceActiveFocus(Qt::TabFocusReason);
    } else if (auto* const content = qobject_cast<QQuickItem*>(part("content"))) {
        content->forceActiveFocus(Qt::OtherFocusReason);
    }
}

void SpotlightImpl::cycleFocus(bool forward) {
    std::vector<QQuickItem*> const chain = tabChain();
    if (chain.empty() || _window == nullptr) {
        return;
    }
    QQuickItem const* const focused = _window->activeFocusItem();
    auto const at = std::ranges::find_if(chain, [focused](QQuickItem const* item) {
        return focused != nullptr && (item == focused || item->isAncestorOf(focused));
    });
    std::size_t const count = chain.size();
    std::size_t next = forward ? 0 : count - 1;
    if (at != chain.end()) {
        auto const index = static_cast<std::size_t>(std::distance(chain.begin(), at));
        next = forward ? (index + 1) % count : (index + count - 1) % count;
    }
    chain.at(next)->forceActiveFocus(forward ? Qt::TabFocusReason : Qt::BacktabFocusReason);
}

bool SpotlightImpl::filterKey(QKeyEvent const& event) {
    auto* const content = qobject_cast<QQuickItem*>(part("content"));
    QQuickItem* const focused = _window != nullptr ? _window->activeFocusItem() : nullptr;
    if (content == nullptr || focused == nullptr) {
        return false;
    }
    bool const inside = content->isAncestorOf(focused);
    int const key = event.key();
    if (key == Qt::Key_Tab || key == Qt::Key_Backtab) {
        QQuickItem const* const target = _ringed != nullptr ? _ringed->item() : nullptr;
        bool const onTarget = target != nullptr && (target == focused || target->isAncestorOf(focused));
        if (!inside && !onTarget) {
            return false;
        }
        cycleFocus(key == Qt::Key_Tab && !event.modifiers().testFlag(Qt::ShiftModifier));
        return true;
    }
    if (!inside || key == Qt::Key_Return || key == Qt::Key_Enter || key == Qt::Key_Space) {
        return false;
    }
    for (QQuickItem* at = focused; at != nullptr && at != content->parentItem(); at = at->parentItem()) {
        if (at->metaObject()->indexOfMethod("takeChord(int,int)") < 0) {
            continue;
        }
        bool taken = false;
        QMetaObject::invokeMethod(at, "takeChord", Qt::DirectConnection, qReturnArg(taken), key,
                                  static_cast<int>(event.modifiers().toInt()));
        if (taken) {
            return true;
        }
    }
    return false;
}

}  // namespace morph::qt_quick::detail
```

`include/morph/qt_quick/backend.hpp`, after `createPopover`:

```cpp
    /// @copydoc ui::IViewBackend::createSpotlight
    [[nodiscard]] std::unique_ptr<ui::SpotlightWidget> createSpotlight(ui::ContainerWidget* parent) override;
```

`src/qt_quick/backend.cpp`, after `createPopover`:

```cpp
std::unique_ptr<ui::SpotlightWidget> Backend::createSpotlight(ui::ContainerWidget* parent) {
    return std::make_unique<detail::SpotlightImpl>(*_context, parent);
}
```

- [ ] **Step 8: Run the tests to verify they pass**

Run:
```bash
flock /tmp/morph-build.lock cmake --build build/clang-debug -j5 --target morph_qt_quick_tests
ctest --test-dir build/clang-debug -R '^qt_quick spotlight:|^qt_quick conformance:|^qt_quick interpreter|^qt_quick_qmllint$' --output-on-failure
```
Expected: `100% tests passed`; `qt_quick_qmllint` (which builds `morph_qt_quick_qml_qmllint`) reports no warning for
`MorphSpotlight.qml`.

- [ ] **Step 9: Mutation check**

Apply each test's mutation, rebuild, see that test fail, restore, `touch`, rebuild. For "never the target", make
`resolve` read `liveWidgets()` instead of `context().widgets().carrying(...)` and confirm the other window's twin is
ringed.

- [ ] **Step 10: Update the spec** (`docs/spec/qt_quick/backend.md`)

Replace the sentence that says `findWidget` matches the item's `objectName` with: "the item's `objectName`, which each
context's `WidgetIndex` files the widget under (a test's `findWidget` reads every context's index; a spotlight reads
only its own, so the inspector window's items are never its target)". Add a section:

```markdown
## The spotlight

`SpotlightImpl` (`MorphSpotlight.qml`) draws a tour's spotlight over the window's content item at `z` 10000, not as a
popup: a popup's item takes every press inside it, and a press inside the cut-out must reach the target. It takes no
room in its parent's layout.

- **Target.** It reads its own context's `WidgetIndex` and rings the first widget carrying the test id whose item is
  visible (`QQuickItem::isVisible`, which is false inside a hidden container or an unselected tab page). It hears the
  index and each candidate's `visibleChanged`.
- **Drawing.** Four `Theme.scrim` rectangles around the cut-out dim everything else; a ring in `Theme.focus`,
  `Theme.spaceOf(1)` outside the target's box, surrounds it. The card is a `Theme.cardScheme` rectangle, at most 40
  units wide, on the side of the target with the most room, clamped a step 2 gap inside the window; with no target it
  is centred and nothing is cut out.
- **Geometry.** Each frame (`QQuickWindow::afterAnimating`) it reads the target's scene box, so it follows resizes,
  layout passes and scrolling; a target clipped by the nearest flickable (a `ScrollWidget`) is scrolled into view.
- **Pointer.** The scrim and the card's own area take presses; the cut-out has no item, so a press there reaches the
  target.
- **Keys.** While open it filters the window's key presses. Tab and Shift+Tab cycle through the card's tab stops, then
  the target's focus item while `setTargetFocusable(true)`. Any other key but Enter and Space goes first to the card's
  widgets from the focused item up (`MorphWidget.takeChord`), so the card's chords come before a control's own keys.
  Opening focuses the card's first tab stop; closing hands focus back to the item that had it.
```

- [ ] **Step 11: Commit**

```bash
git add src/qt_quick/context.hpp src/qt_quick/context.cpp src/qt_quick/widget.hpp src/qt_quick/widget.cpp \
  src/qt_quick/kinds.hpp src/qt_quick/spotlight.cpp src/qt_quick/qml/MorphSpotlight.qml src/qt_quick/qml/MorphWidget.qml \
  include/morph/qt_quick/backend.hpp src/qt_quick/backend.cpp src/qt_quick/CMakeLists.txt \
  tests/qt_quick/test_qt_quick_spotlight.cpp tests/qt_quick/test_qt_quick_conformance.cpp tests/qt_quick/CMakeLists.txt \
  docs/spec/qt_quick/backend.md
git commit -m "Draw the guided tour spotlight on Qt Quick"
```

---
### Task 5: The terminal spotlight

**Lane:** wave 2, after Task 3; parallel with Tasks 2 and 4.

**Files:**
- Modify: `src/tui/context.hpp`, `src/tui/context.cpp` (`nextSequence`, `spotlights`, `spotlit`, `targetsChanged()`;
  `forget`)
- Modify: `src/tui/widget.hpp` (`WidgetBase`: `sequence()`, `applyTestId` out of line, hooks `targetsChanged`,
  `isOpenSpotlight`, `tabTarget`, `passesThrough`, `spotlightChord`; free `carrying`), `src/tui/widget.cpp`
  (constructor ~line 57, `applyTestId`, `syncVisible` ~line 112, `render` ~line 263, `dispatch` ~line 356,
  `blockedByDialog` ~line 541, `fit` ~line 1065, `moveFocus` ~line 1123)
- Modify: `src/tui/container_widgets.hpp` (`OverlayCore::setAnchor`, `setDismissible`, members; `SpotlightImpl` after
  `DrawerImpl`), `src/tui/container_widgets.cpp` (`framePosition` ~line 1107, `frameEvent` Escape branch ~line 1190,
  `SpotlightImpl` definitions)
- Modify: `include/morph/tui/backend.hpp` (after `createPopover` ~line 64), `src/tui/backend.cpp` (after
  `createPopover` ~line 116)
- Test: `tests/tui/test_tui_spotlight.cpp` (create); `tests/tui/CMakeLists.txt` (after `test_tui_dialog.cpp`);
  `tests/tui/test_tui_conformance.cpp` (`TuiProbe::targetOf`, one test case)
- Docs: `docs/spec/tui/frontend.md`

**Interfaces:**
- Consumes: Task 3's `ui::SpotlightWidget`, `spotlightConformanceCases()`, `ConformanceProbe::targetOf`;
  `OverlayCore`, `TuiContainer`, `WidgetBase::displayed()`, `isWithin`, `collectFocusable`, `isActivation`.
- Produces:
  - `std::uint64_t WidgetBase::sequence() const noexcept;`
  - `virtual void WidgetBase::targetsChanged();`, `virtual bool isOpenSpotlight() const;`,
    `virtual WidgetBase* tabTarget() const;`, `virtual WidgetBase const* passesThrough() const;`
  - `std::vector<WidgetBase*> tui::detail::carrying(Context const& context, std::string_view testId);`
  - `void Context::targetsChanged();`, `std::vector<WidgetBase*> Context::spotlights;`,
    `WidgetBase const* Context::spotlit;`
  - `void OverlayCore::setAnchor(std::function<std::optional<::core::tui::Rect>()> anchor);`,
    `void OverlayCore::setDismissible(bool dismissible);`
  - `class tui::detail::SpotlightImpl final : public TuiContainer<ui::SpotlightWidget>` with
    `WidgetBase const* ringed() const noexcept;`
  - `std::unique_ptr<ui::SpotlightWidget> tui::Backend::createSpotlight(ui::ContainerWidget* parent) override;`

- [ ] **Step 1: Write the failing tests** (`tests/tui/test_tui_spotlight.cpp`)

```cpp
// SPDX-License-Identifier: Apache-2.0
//
// The spotlight on the terminal (docs/spec/tui/frontend.md, "The spotlight"): the card is a box under the target, or
// over it when there is no room below; the target is drawn in reverse video and nothing else is dimmed; Tab stays in
// the card, and on a waiting step reaches the target.

#include <catch2/catch_test_macros.hpp>
#include <core/tui/InputEvent.hpp>
#include <cstddef>
#include <memory>
#include <morph/ui/backend.hpp>
#include <string>
#include <vector>

#include "tui/container_widgets.hpp"
#include "tui/leaf_widgets.hpp"
#include "tui/widget.hpp"
#include "tui_harness.hpp"

namespace {

namespace detail = morph::tui::detail;
namespace ui = morph::ui;
using morph::tui::testing::Harness;

/// The row a text first shows on, or npos.
std::size_t rowOf(std::vector<std::string> const& rows, std::string const& text) {
    for (std::size_t row = 0; row < rows.size(); ++row) {
        if (rows.at(row).find(text) != std::string::npos) {
            return row;
        }
    }
    return std::string::npos;
}

/// A column of a spacer @p above rows tall and a button `TARGET`, and an open spotlight on it with a card of two
/// buttons, `BACK` and `NEXT`.
struct Rig {
    explicit Rig(int above) {
        column = harness.make<detail::StackImpl>(nullptr, ui::Axis::Vertical);
        spacer = harness.make<detail::SpacerImpl>(column.get());
        spacer->setLayout({.width = {}, .height = ui::Sizing::fixed(above)});
        target = harness.make<detail::ButtonImpl>(column.get());
        target->setLabel("TARGET");
        target->setTestId("target");
        target->setOnClick([this] { ++clicks; });
        spotlight = harness.make<detail::SpotlightImpl>(nullptr);
        back = harness.make<detail::ButtonImpl>(spotlight.get());
        back->setLabel("BACK");
        next = harness.make<detail::ButtonImpl>(spotlight.get());
        next->setLabel("NEXT");
        spotlight->setTarget(std::string{"target"});
        spotlight->setOpen(true);
        rows = harness.draw();
    }
    Harness harness{40, 12};
    std::unique_ptr<detail::StackImpl> column;
    std::unique_ptr<detail::SpacerImpl> spacer;
    std::unique_ptr<detail::ButtonImpl> target;
    std::unique_ptr<detail::SpotlightImpl> spotlight;
    std::unique_ptr<detail::ButtonImpl> back;
    std::unique_ptr<detail::ButtonImpl> next;
    std::vector<std::string> rows;
    int clicks = 0;
};

}  // namespace

// Mutation: leave `framePosition` centring the box (its row is the screen's middle, not under the target).
TEST_CASE("tui spotlight: the card is a box under the target, or over it when there is no room below", "[tui][tours]") {
    Rig const top{0};
    std::size_t const targetRow = rowOf(top.rows, "TARGET");
    REQUIRE(targetRow != std::string::npos);
    CHECK(rowOf(top.rows, "BACK") > targetRow);
    CHECK(top.spotlight->targetShown());
    Rig const bottom{10};
    std::size_t const lowRow = rowOf(bottom.rows, "TARGET");
    REQUIRE(lowRow != std::string::npos);
    CHECK(rowOf(bottom.rows, "BACK") < lowRow);
}

// Mutation: drop the `spotlit` branch of `WidgetBase::render` (the target is drawn as before).
TEST_CASE("tui spotlight: the target is drawn in reverse video and nothing else is dimmed", "[tui][tours]") {
    Rig rig{0};
    std::size_t const row = rowOf(rig.rows, "TARGET");
    REQUIRE(row != std::string::npos);
    int const column = static_cast<int>(rig.rows.at(row).find("TARGET"));
    auto const& buffer = rig.harness.screen().renderedBuffer();
    CHECK(buffer.cellAt({.x = column, .y = static_cast<int>(row)})->style.inverse);
    CHECK_FALSE(buffer.cellAt({.x = 39, .y = 11})->style.inverse);
    rig.spotlight->setOpen(false);
    rig.harness.draw();
    CHECK_FALSE(rig.harness.screen().renderedBuffer().cellAt({.x = column, .y = static_cast<int>(row)})->style.inverse);
}

// Mutation: make `SpotlightImpl::tabTarget` answer null (Tab never reaches the target).
TEST_CASE("tui spotlight: Tab stays in the card, and on a waiting step reaches the target", "[tui][tours]") {
    Rig rig{0};
    CHECK(rig.harness.focused(*rig.back));
    detail::moveFocus(rig.harness.context(), detail::Direction::Forward);
    CHECK(rig.harness.focused(*rig.next));
    detail::moveFocus(rig.harness.context(), detail::Direction::Forward);
    CHECK(rig.harness.focused(*rig.back));
    rig.spotlight->setTargetFocusable(true);
    detail::moveFocus(rig.harness.context(), detail::Direction::Forward);
    detail::moveFocus(rig.harness.context(), detail::Direction::Forward);
    CHECK(rig.harness.focused(*rig.target));
    static_cast<void>(rig.harness.key(core::tui::KeyCode::Enter));
    CHECK(rig.clicks == 1);
}
```

`tests/tui/test_tui_conformance.cpp`: in `TuiProbe`, after `boundsOf`:

```cpp
    ui::Widget const* targetOf(ui::SpotlightWidget const& spotlight) override {
        auto const* const impl = dynamic_cast<morph::tui::detail::SpotlightImpl const*>(&spotlight);
        WidgetBase const* const ringed = impl == nullptr ? nullptr : impl->ringed();
        return ringed == nullptr ? nullptr : &ringed->asWidget();
    }
```

and after the layout test case:

```cpp
TEST_CASE("tui conformance: the terminal backend passes every spotlight case", "[tui][conformance][tours]") {
    for (auto const& testCase : ui::testing::spotlightConformanceCases()) {
        TuiProbe probe;
        std::optional<std::string> const failure = testCase.run(probe);
        INFO(std::string{testCase.name} + ": " + failure.value_or("passed"));
        CHECK_FALSE(failure.has_value());
    }
}
```

(Add `#include "tui/container_widgets.hpp"` to the conformance test if it does not include it.) Add
`test_tui_spotlight.cpp` to `tests/tui/CMakeLists.txt` after `test_tui_dialog.cpp`.

- [ ] **Step 2: Run the tests to verify they fail**

Run: `flock /tmp/morph-build.lock cmake --build build/clang-debug -j5 --target morph_tui_tests`
Expected: compile error — `SpotlightImpl` is not a member of `morph::tui::detail`.

- [ ] **Step 3: The context's spotlight state** (`src/tui/context.hpp`)

In `struct Context`, after `spanTables`:

```cpp
    /// Numbers each widget as it is made, so the widgets carrying one test id are found in creation order.
    std::uint64_t nextSequence = 0;
    /// The live spotlights, which pick their target again whenever `targetsChanged` runs.
    std::vector<WidgetBase*> spotlights;
    /// The widget an open spotlight rings, drawn in reverse video; null for none.
    WidgetBase const* spotlit = nullptr;
```

and after `endPress()`:

```cpp
    /// Tells every live spotlight that a test id, a widget's visibility or the set of widgets changed: each picks its
    /// target again. A widget made or destroyed, a test id set, a widget shown or hidden, and every frame call it.
    void targetsChanged();
```

`src/tui/context.cpp`: in `forget`, add `std::erase(spotlights, &widget);` and
`if (spotlit == &widget) { spotlit = nullptr; }`, and at its end `targetsChanged();`. Add:

```cpp
void Context::targetsChanged() {
    for (WidgetBase* const spotlight : std::vector<WidgetBase*>{spotlights}) {
        spotlight->targetsChanged();
    }
}
```

(`context.cpp` includes `tui/widget.hpp` already for `WidgetBase`; add it if it does not.)

- [ ] **Step 4: The widget's hooks** (`src/tui/widget.hpp`, `src/tui/widget.cpp`)

In `WidgetBase` (public), replace `void applyTestId(std::string_view testId) { _testId = testId; }` with the
declaration `void applyTestId(std::string_view testId);`, and add:

```cpp
    /// The order the widget was made in, among this context's widgets.
    [[nodiscard]] std::uint64_t sequence() const noexcept { return _sequence; }
    /// Picks a spotlight's target again; every other widget ignores it.
    virtual void targetsChanged() {}
    /// Whether this is a spotlight that is open: the card's chords then come before the focused widget's own keys.
    [[nodiscard]] virtual bool isOpenSpotlight() const { return false; }
    /// The widget Tab reaches after this open overlay's own, or null: a spotlight's target on a waiting step.
    [[nodiscard]] virtual WidgetBase* tabTarget() const { return nullptr; }
    /// The widget outside this open overlay that still takes presses and keys, or null: a spotlight's target.
    [[nodiscard]] virtual WidgetBase const* passesThrough() const { return nullptr; }
    /// Runs the chord @p key is when a container between this widget and the open spotlight around it declares it:
    /// inside a spotlight the card's chords come before the focused widget's own keys.
    /// @return Handled when one did; Ignored outside a spotlight.
    [[nodiscard]] ::core::tui::EventResult spotlightChord(::core::tui::KeyEvent const& key);
```

private: `std::uint64_t _sequence;` (first data member after `_context`). Near `collectFocusable`, declare:

```cpp
/// The live widgets of @p context that carry @p testId, in creation order.
[[nodiscard]] std::vector<WidgetBase*> carrying(Context const& context, std::string_view testId);
```

`src/tui/widget.cpp`:

```cpp
WidgetBase::WidgetBase(Context& context) noexcept : _context{&context}, _sequence{++context.nextSequence} {}

void WidgetBase::applyTestId(std::string_view testId) {
    _testId = testId;
    _context->targetsChanged();
}

EventResult WidgetBase::spotlightChord(::core::tui::KeyEvent const& key) {
    ContainerBase const* spotlight = nullptr;
    for (ContainerBase const* outer = _container; outer != nullptr; outer = outer->container()) {
        if (outer->isOpenSpotlight()) {
            spotlight = outer;
            break;
        }
    }
    if (spotlight == nullptr) {
        return EventResult::Ignored;
    }
    for (ContainerBase* outer = _container; outer != nullptr; outer = outer->container()) {
        bool const last = outer == spotlight;
        if (EventResult const result = outer->chord(key); result != EventResult::Ignored) {
            return result;
        }
        if (last) {
            break;
        }
    }
    return EventResult::Ignored;
}

std::vector<WidgetBase*> carrying(Context const& context, std::string_view testId) {
    std::vector<WidgetBase*> out;
    for (auto const& [view, widget] : context.owners) {
        static_cast<void>(view);
        if (widget->testId() == testId) {
            out.push_back(widget);
        }
    }
    std::ranges::sort(out, {}, &WidgetBase::sequence);
    return out;
}
```

In `syncVisible`, after `countSpinners(*_context);`: `_context->targetsChanged();`.

In `render`, after the `invertsSurface` branch:

```cpp
    // The widget an open spotlight rings: the terminal dims nothing, so the target is marked instead.
    if (_context->spotlit == this) {
        invert(canvas.buffer(), drawn);
    }
```

In `dispatch`, between the widget's own `chord` and `key(*pressed)`:

```cpp
        // Inside a spotlight the card's chords come before this widget's own keys; Enter and Space stay its own.
        if (!isActivation(*pressed)) {
            if (EventResult const result = spotlightChord(*pressed); result != EventResult::Ignored) {
                return result;
            }
        }
```

`blockedByDialog`:

```cpp
bool WidgetBase::blockedByDialog() const {
    auto* const dialog = activeDialog(*_context);
    if (dialog == nullptr || isWithin(_view.get(), dialog->host())) {
        return false;
    }
    WidgetBase const* const through = dialog->passesThrough();
    return through == nullptr || !isWithin(_view.get(), through->view());
}
```

`moveFocus`, after `collectFocusable(*dialog, order);`:

```cpp
        if (WidgetBase* const target = dialog->tabTarget()) {
            collectFocusable(*target, order);
        }
```

At the end of `fit`: `context.targetsChanged();`.

- [ ] **Step 5: The overlay's anchor and the spotlight** (`src/tui/container_widgets.hpp`, `.cpp`)

In `OverlayCore` (public), after `setFooter`:

```cpp
    /// Places the box beside a rectangle rather than centred: under it, or over it when there is no room below,
    /// clamped inside the screen. The anchor is read at every frame; a null one, or one that answers nothing, centres.
    void setAnchor(std::function<std::optional<::core::tui::Rect>()> anchor) { _anchor = std::move(anchor); }
    /// Whether Esc inside the box closes it, as a dialog's does. A spotlight's card handles Esc itself.
    void setDismissible(bool dismissible) noexcept { _dismissible = dismissible; }
```

private: `std::function<std::optional<::core::tui::Rect>()> _anchor;` and `bool _dismissible = true;`.

`framePosition`, after the sheet branch:

```cpp
    if (_anchor) {
        if (std::optional<::core::tui::Rect> const target = _anchor()) {
            auto const area = screen.viewportArea();
            auto const size = frameSize();
            int const below = target->y + target->height;
            int const y = below + size.height <= area.y + area.height ? below
                                                                      : std::max(area.y, target->y - size.height);
            int const x = std::clamp(target->x, area.x, std::max(area.x, area.x + area.width - size.width));
            return {.x = x, .y = y};
        }
    }
```

`frameEvent`'s Escape branch: change the condition to
`if (key == nullptr || key->key != ::core::tui::KeyCode::Escape || !_open || !_dismissible || !_self->actionable())`.

After `DrawerImpl` in the header:

```cpp
/// A guided tour's spotlight: an `OverlayCore` box holding the card, placed under the target's drawn rectangle, or over
/// it when there is no room below; the target is drawn in reverse video and nothing is dimmed. Like a dialog the box
/// is modal and takes the focus when it opens, except that the target still takes presses and keys, and on a waiting
/// step Tab reaches the target after the card. Esc is the card's to handle.
class SpotlightImpl final : public TuiContainer<ui::SpotlightWidget> {
public:
    explicit SpotlightImpl(Context& context);
    ~SpotlightImpl() override;
    SpotlightImpl(SpotlightImpl const&) = delete;
    SpotlightImpl& operator=(SpotlightImpl const&) = delete;
    SpotlightImpl(SpotlightImpl&&) = delete;
    SpotlightImpl& operator=(SpotlightImpl&&) = delete;

    void setOpen(bool open) override;
    void setTarget(std::optional<std::string> testId) override;
    [[nodiscard]] bool targetShown() const override { return _ringed != nullptr; }
    void setOnTargetShownChanged(std::function<void(bool)> handler) override { _onChanged = std::move(handler); }
    void setTargetFocusable(bool focusable) override { _targetFocusable = focusable; }
    void targetsChanged() override { resolve(); }
    [[nodiscard]] bool isOpenSpotlight() const override { return _overlay.isOpen(); }
    [[nodiscard]] WidgetBase* tabTarget() const override { return _targetFocusable ? _ringed : nullptr; }
    [[nodiscard]] WidgetBase const* passesThrough() const override { return _ringed; }
    /// The widget it rings now, or null.
    [[nodiscard]] WidgetBase const* ringed() const noexcept { return _ringed; }
    [[nodiscard]] ::core::tui::Size naturalSize() const override { return {.width = 0, .height = 0}; }
    [[nodiscard]] ::core::tui::Component& host() override { return _overlay.frame(); }
    [[nodiscard]] bool letsChildrenAct() const override { return _overlay.isOpen(); }
    [[nodiscard]] std::vector<::core::tui::Rect> childAreas(::core::tui::Size size) const override {
        return _overlay.childAreas(size);
    }
    [[nodiscard]] OverlayCore* overlay() override { return &_overlay; }

private:
    // Picks the target again: the first widget carrying the test id, in creation order, that is displayed and is not
    // inside the card.
    void resolve();

    OverlayCore _overlay;
    std::optional<std::string> _target;
    WidgetBase* _ringed = nullptr;
    bool _targetFocusable = false;
    bool _resolving = false;
    std::function<void(bool)> _onChanged;
};
```

In `container_widgets.cpp`:

```cpp
SpotlightImpl::SpotlightImpl(Context& context) : TuiContainer{context}, _overlay{*this, std::nullopt} {
    adopt(makeView(*this));
    _overlay.setDismissible(false);
    _overlay.setAnchor([this]() -> std::optional<::core::tui::Rect> {
        if (_ringed == nullptr) {
            return std::nullopt;
        }
        auto const bounds = _ringed->view().screenBounds();
        return bounds.width > 0 && bounds.height > 0 ? std::optional<::core::tui::Rect>{bounds} : std::nullopt;
    });
    context.spotlights.push_back(this);
}

SpotlightImpl::~SpotlightImpl() {
    std::erase(context().spotlights, this);
    if (context().spotlit == _ringed) {
        context().spotlit = nullptr;
    }
    _overlay.setOpen(false);
}

void SpotlightImpl::setOpen(bool open) {
    _overlay.setOpen(open);
    resolve();
    context().spotlit = _overlay.isOpen() ? _ringed : nullptr;
    refresh();
}

void SpotlightImpl::setTarget(std::optional<std::string> testId) {
    _target = std::move(testId);
    resolve();
}

void SpotlightImpl::resolve() {
    if (_resolving) {
        return;
    }
    _resolving = true;
    WidgetBase* found = nullptr;
    if (_target) {
        for (WidgetBase* const candidate : carrying(context(), *_target)) {
            if (candidate != this && !isWithin(&candidate->view(), _overlay.frame()) && candidate->displayed()) {
                found = candidate;
                break;
            }
        }
    }
    bool const was = _ringed != nullptr;
    bool const moved = found != _ringed;
    _ringed = found;
    context().spotlit = _overlay.isOpen() ? found : nullptr;
    _resolving = false;
    // Only a change redraws: every frame asks, and a redraw each time would draw for ever.
    if (moved) {
        refresh();
    }
    if ((found != nullptr) != was && _onChanged) {
        auto const handler = _onChanged;
        handler(found != nullptr);
    }
}
```

`include/morph/tui/backend.hpp`, after `createPopover`:

```cpp
    /// @copydoc ui::IViewBackend::createSpotlight
    [[nodiscard]] std::unique_ptr<ui::SpotlightWidget> createSpotlight(ui::ContainerWidget* parent) override;
```

`src/tui/backend.cpp`, after `createPopover`:

```cpp
std::unique_ptr<ui::SpotlightWidget> Backend::createSpotlight(ui::ContainerWidget* parent) {
    return make<detail::SpotlightImpl>(_impl->context, parent);
}
```

- [ ] **Step 6: Run the tests to verify they pass**

Run:
```bash
flock /tmp/morph-build.lock cmake --build build/clang-debug -j5 --target morph_tui_tests
ctest --test-dir build/clang-debug -R '^tui spotlight:|^tui conformance:|^tui dialog' --output-on-failure
```
Expected: `100% tests passed`.

- [ ] **Step 7: Mutation check**

Apply each test's mutation, rebuild, see that test fail, restore, `touch`, rebuild. For the conformance run, make
`SpotlightImpl::resolve` skip the `displayed()` test and confirm "hiding or destroying a spotlight's target" fails.

- [ ] **Step 8: Update the spec** (`docs/spec/tui/frontend.md`)

Add a section:

```markdown
## The spotlight

`SpotlightImpl` shows a tour's card as a bordered box, an `OverlayCore` like a dialog's, placed under the target's
drawn rectangle, or over it when there is no room below, clamped inside the screen; with no target shown it is
centred. Nothing is dimmed: the ringed widget is drawn in reverse video (`Context::spotlit`). The target is the first
widget, in creation order, whose stored test id matches and that is displayed; the spotlight picks again whenever a
test id is set, a widget is made, destroyed, shown or hidden, and at every frame (`Context::targetsChanged`). The box
is modal and takes the focus when it opens, as a dialog's does, with three differences: the ringed widget still takes
presses and keys (`passesThrough`); on a waiting step (`setTargetFocusable(true)`) Tab reaches the ringed widget after
the card's widgets (`tabTarget`); and Esc is not the box's — the card declares it. Inside the box, a chord a container
between the focused widget and the box declares is delivered before the focused widget's own key handling, Enter and
Space excepted (`WidgetBase::spotlightChord`).
```

- [ ] **Step 9: Commit**

```bash
git add src/tui/context.hpp src/tui/context.cpp src/tui/widget.hpp src/tui/widget.cpp src/tui/container_widgets.hpp \
  src/tui/container_widgets.cpp include/morph/tui/backend.hpp src/tui/backend.cpp tests/tui/test_tui_spotlight.cpp \
  tests/tui/test_tui_conformance.cpp tests/tui/CMakeLists.txt docs/spec/tui/frontend.md
git commit -m "Show the guided tour spotlight on the terminal"
```

---
### Task 6: `TourController`, headless

**Lane:** wave 3, after Tasks 1 and 3; parallel with Task 8.

**Files:**
- Create: `include/morph/ui/tours.hpp`, `src/ui/tours.cpp`
- Modify: `include/morph/ui/navigator.hpp` (`reveal` after `setOpenModule` ~line 191), `src/ui/navigator.cpp` (after
  `setOpenModule` ~line 171)
- Modify: `CMakeLists.txt` (`src/ui/tours.cpp` after `src/ui/shell_parts_model.cpp`; `include/morph/ui/tours.hpp`
  after `include/morph/ui/shell_parts_view.hpp`)
- Test: `tests/ui_tours_support.hpp` (create), `tests/test_ui_tours_controller.cpp` (create); `tests/CMakeLists.txt`
  (after `test_ui_tours_registry.cpp`)
- Docs: `docs/spec/ui/tours.md` (create), `docs/spec/ui/shell_frame.md` ("Module groups": `reveal`)

**Interfaces:**
- Consumes: Task 1's `ui::Tour`, `ui::TourStep`, `AppRoot::tours()`; Task 3's `ui::SpotlightWidget`,
  `RecordingBackend::createSpotlight`; `ui::MountError{screenId, reason, pointer}`, `ui::ParamMap`, `ui::Tone`,
  `reactive::Effect(Runtime&, F)`, `reactive::Computed<T>::get()/peek()`, `reactive::Runtime::untracked(F)`.
- Produces:
  - ```cpp
    class TourShell {
    public:
        virtual std::expected<void, MountError> openScreen(std::string const& screenId, ParamMap const& params) = 0;
        [[nodiscard]] virtual bool shows(std::string const& screenId, ParamMap const& params) const = 0;
        [[nodiscard]] virtual std::string activeScreen() const = 0;
        [[nodiscard]] virtual std::string screenTitle(std::string const& screenId) const = 0;
        [[nodiscard]] virtual bool compact() const = 0;
        virtual void revealNavEntry(std::string const& screenId) = 0;
        virtual SpotlightWidget& spotlight() = 0;
        virtual void notify(std::string const& text, Tone tone) = 0;
    };
    ```
  - `struct TourView { std::string tourId; std::string title; std::string stepTitle; std::string body; std::int64_t
    position = 0; std::int64_t count = 0; bool waiting = false; std::string waitingFor; bool canBack = false; bool last
    = false; bool operator==(TourView const&) const = default; };`
  - `struct TourControllerOptions { std::function<void(std::string const&)> warn; std::function<void()> onChanged; };`
  - `struct OfferedTours { std::vector<Tour> screen; std::vector<Tour> general; };`
  - `[[nodiscard]] MORPH_API OfferedTours offeredTours(std::vector<Tour> const& tours, std::string_view screenId);`
  - `[[nodiscard]] MORPH_API std::string tourTargetFor(std::string_view target, bool compact);`
  - `class MORPH_API TourController { TourController(reactive::Runtime&, AppRoot const&, TourShell&,
    TourControllerOptions = {}); std::expected<void, Refusal> start(std::string_view tourId); void next(); void back();
    void end(); void activeChanged(); bool running() const noexcept; bool declares(std::string_view) const;
    std::optional<TourView> const& view() const noexcept; };`
  - `bool Navigator::reveal(std::string_view screenId);`
  - `class ui::testing::FakeTourShell final : public TourShell` (tests only)

- [ ] **Step 1: Write the test support** (`tests/ui_tours_support.hpp`)

```cpp
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <expected>
#include <functional>
#include <memory>
#include <morph/ui/backend.hpp>
#include <morph/ui/host.hpp>
#include <morph/ui/testing/recording_backend.hpp>
#include <morph/ui/tours.hpp>
#include <set>
#include <string>
#include <utility>
#include <vector>

/// @file
/// A shell a tour runs in, over a `RecordingBackend`: it opens screens by recording them and making them active, and
/// records what the tour asked of it.

namespace morph::ui::testing {

class FakeTourShell final : public TourShell {
public:
    explicit FakeTourShell(RecordingBackend& backend) : _backend{&backend} {}

    std::expected<void, MountError> openScreen(std::string const& screenId, ParamMap const& params) override {
        opened.push_back(screenId);
        if (refused.contains(screenId)) {
            return std::unexpected(MountError{.screenId = screenId, .reason = "not for this person", .pointer = {}});
        }
        active = screenId;
        activeParams = params;
        // What the workspace's change callback does in the generic client: synchronously, inside the open.
        if (onActive) {
            onActive();
        }
        return {};
    }
    [[nodiscard]] bool shows(std::string const& screenId, ParamMap const& params) const override {
        return active == screenId && activeParams == params;
    }
    [[nodiscard]] std::string activeScreen() const override { return active; }
    [[nodiscard]] std::string screenTitle(std::string const& screenId) const override { return "Title of " + screenId; }
    [[nodiscard]] bool compact() const override { return isCompact; }
    void revealNavEntry(std::string const& screenId) override { revealed.push_back(screenId); }
    SpotlightWidget& spotlight() override {
        if (_spotlight == nullptr) {
            _spotlight = _backend->createSpotlight(nullptr);
            if (onSpotlight) {
                onSpotlight(*_spotlight);
            }
        }
        return *_spotlight;
    }
    void notify(std::string const& text, Tone tone) override { notices.emplace_back(text, tone); }

    /// The spotlight's widget id; 0 before the first tour.
    [[nodiscard]] int spotlightId() const { return _spotlight == nullptr ? 0 : _backend->idOf(*_spotlight); }

    // NOLINTBEGIN(cppcoreguidelines-non-private-member-variables-in-classes): what a test sets and reads.
    std::string active;
    ParamMap activeParams;
    bool isCompact = false;
    std::set<std::string, std::less<>> refused;
    std::vector<std::string> opened;
    std::vector<std::string> revealed;
    std::vector<std::pair<std::string, Tone>> notices;
    std::function<void()> onActive;
    std::function<void(SpotlightWidget&)> onSpotlight;
    // NOLINTEND(cppcoreguidelines-non-private-member-variables-in-classes)

private:
    RecordingBackend* _backend;
    std::unique_ptr<SpotlightWidget> _spotlight;
};

}  // namespace morph::ui::testing
```

- [ ] **Step 2: Write the failing tests** (`tests/test_ui_tours_controller.cpp`)

```cpp
// SPDX-License-Identifier: Apache-2.0
//
// The tour controller (docs/spec/ui/tours.md, "The controller"), with no shell: a fake `TourShell`, an `AppRoot` over a
// test app document, and the recording backend's spotlight, whose target the backend resolves.

#include <catch2/catch_test_macros.hpp>
#include <memory>
#include <morph/ui/backend.hpp>
#include <morph/ui/interpreter.hpp>
#include <morph/ui/navigator.hpp>
#include <morph/ui/testing/recording_backend.hpp>
#include <morph/ui/tours.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "ui_interpreter_support.hpp"
#include "ui_tours_support.hpp"

namespace ui = morph::ui;

namespace {

constexpr std::string_view kApp = R"({
  "vocab": "ui/1", "id": "app",
  "tours": [
    { "id": "basics", "title": { "t": "tours.basics", "fallback": "Basics" },
      "steps": [
        { "target": "shell_search", "title": { "t": "tours.search", "fallback": "Search" }, "body": "Find anything." },
        { "title": "Second" },
        { "title": "Last" } ] },
    { "id": "getting-started", "title": "Getting started",
      "steps": [
        { "target": "nav_samples", "advanceOn": { "screen": "samples" }, "title": "Open Samples" },
        { "screen": "samples", "target": "samples_table", "title": "The table" },
        { "title": "Done" } ] },
    { "id": "register", "screen": "samples", "title": "Register",
      "steps": [ { "target": "action_new", "title": "New" },
                 { "screen": "home", "params": { "page": 2 }, "title": "Home" } ] },
    { "id": "find", "screen": "samples", "title": "Find", "steps": [ { "title": "Filter" } ] },
    { "id": "lost", "title": "Lost", "steps": [ { "screen": "locked", "title": "Locked" } ] } ] })";

struct Rig {
    Rig() {
        h.session.setTranslations([](std::string_view key, std::string_view locale) -> std::optional<std::string> {
            if (!locale.starts_with("de")) {
                return std::nullopt;
            }
            if (key == "tours.basics") {
                return std::string{"Grundlagen"};
            }
            if (key == "tours.search") {
                return std::string{"Suche"};
            }
            return std::nullopt;
        });
        auto mounted = h.interpreter.mountApp(ui::testing::loadOrFail(kApp));
        REQUIRE(mounted.has_value());
        root = std::move(*mounted);
        page = h.backend.createStack(nullptr, ui::Axis::Vertical);
        controller = std::make_unique<ui::TourController>(
            h.runtime, *root, shell,
            ui::TourControllerOptions{.warn = [this](std::string const& text) { warnings.push_back(text); },
                                      .onChanged = [this] { ++changes; }});
        shell.onActive = [this] { controller->activeChanged(); };
    }

    /// Puts a widget carrying @p testId on the page.
    void carry(std::string const& testId) {
        parts.push_back(h.backend.createButton(page.get()));
        parts.back()->setTestId(testId);
    }

    /// A property of the spotlight.
    [[nodiscard]] std::string spot(std::string_view name) const { return h.backend.prop(shell.spotlightId(), name); }

    ui::testing::Harness h;
    ui::testing::FakeTourShell shell{h.backend};
    std::unique_ptr<ui::AppRoot> root;
    std::unique_ptr<ui::StackWidget> page;
    std::vector<std::unique_ptr<ui::ButtonWidget>> parts;
    std::unique_ptr<ui::TourController> controller;
    std::vector<std::string> warnings;
    int changes = 0;
};

}  // namespace

// Mutation: make `next` show the step it is on (the position stays 1).
TEST_CASE("tours controller: start shows the first step, next and back move, and Done on the last step ends it",
          "[ui][tours]") {
    Rig r;
    r.carry("shell_search");
    REQUIRE(r.controller->start("basics").has_value());
    REQUIRE(r.controller->view().has_value());
    CHECK(r.controller->view()->title == "Basics");
    CHECK(r.controller->view()->stepTitle == "Search");
    CHECK(r.controller->view()->body == "Find anything.");
    CHECK(r.controller->view()->position == 1);
    CHECK(r.controller->view()->count == 3);
    CHECK_FALSE(r.controller->view()->canBack);
    CHECK(r.spot("open") == "true");
    CHECK(r.spot("target") == "shell_search");
    CHECK(r.spot("targetShown") == "true");
    r.controller->next();
    CHECK(r.controller->view()->position == 2);
    CHECK(r.controller->view()->canBack);
    CHECK(r.spot("target") == "none");
    r.controller->back();
    CHECK(r.controller->view()->position == 1);
    r.controller->next();
    r.controller->next();
    CHECK(r.controller->view()->last);
    r.controller->next();
    CHECK_FALSE(r.controller->running());
    CHECK_FALSE(r.controller->view().has_value());
    CHECK(r.spot("open") == "false");
    CHECK(r.warnings.empty());
}

// Mutation: open a step's screen without asking `shows` (home is opened twice).
TEST_CASE("tours controller: a tour that belongs to a screen opens it first, and a shown screen is not reopened",
          "[ui][tours]") {
    Rig r;
    r.shell.active = "home";
    REQUIRE(r.controller->start("register").has_value());
    CHECK(r.shell.opened == std::vector<std::string>{"samples"});
    r.controller->next();
    CHECK(r.shell.opened == std::vector<std::string>{"samples", "home"});
    CHECK(r.shell.activeParams.at("page") == ui::Value{2});
    // Back does not undo the screen; the earlier step names none, so nothing opens.
    r.controller->back();
    CHECK(r.shell.opened.size() == 2);
    r.controller->next();
    CHECK(r.shell.opened.size() == 2);
    Rig shown;
    shown.shell.active = "samples";
    REQUIRE(shown.controller->start("find").has_value());
    CHECK(shown.shell.opened.empty());
}

// Mutation: advance on any change of the active tab in `activeChanged` (home moves the tour on).
TEST_CASE("tours controller: advanceOn moves on when its screen becomes active and ignores another; Next waits",
          "[ui][tours]") {
    Rig r;
    r.carry("nav_samples");
    REQUIRE(r.controller->start("getting-started").has_value());
    CHECK(r.controller->view()->waiting);
    CHECK(r.controller->view()->waitingFor == "Title of samples");
    CHECK(r.spot("targetFocusable") == "true");
    r.controller->next();
    CHECK(r.controller->view()->position == 1);
    r.shell.active = "home";
    r.controller->activeChanged();
    CHECK(r.controller->view()->position == 1);
    r.shell.active = "samples";
    r.controller->activeChanged();
    CHECK(r.controller->view()->position == 2);
    CHECK_FALSE(r.controller->view()->waiting);
    CHECK(r.spot("targetFocusable") == "false");
    // The step's screen is the one shown already.
    CHECK(r.shell.opened.empty());
}

// Mutation: warn at every showing of a step (Next then Back warns twice).
TEST_CASE("tours controller: a target that is not shown leaves the card centred and warns once per step",
          "[ui][tours]") {
    Rig r;
    REQUIRE(r.controller->start("basics").has_value());
    CHECK(r.spot("targetShown") == "false");
    REQUIRE(r.warnings.size() == 1);
    CHECK(r.warnings.front().contains("`basics`, step 1"));
    CHECK(r.warnings.front().contains("shell_search"));
    r.controller->next();
    r.controller->back();
    CHECK(r.warnings.size() == 1);
    // A target shown later is found by the spotlight itself.
    r.carry("shell_search");
    CHECK(r.spot("targetShown") == "true");
}

// Mutation: skip `revealNavEntry` in `show` (the navigator is never asked).
TEST_CASE("tours controller: a navigator target is revealed in the wide form and substituted when compact",
          "[ui][tours]") {
    Rig r;
    REQUIRE(r.controller->start("getting-started").has_value());
    CHECK(r.shell.revealed == std::vector<std::string>{"samples"});
    CHECK(r.spot("target") == "nav_samples");
    r.controller->end();
    r.shell.isCompact = true;
    r.shell.revealed.clear();
    REQUIRE(r.controller->start("getting-started").has_value());
    CHECK(r.shell.revealed.empty());
    CHECK(r.spot("target") == "shell_nav_menu");
    r.controller->end();
    REQUIRE(r.controller->start("basics").has_value());
    CHECK(r.spot("target") == "shell_search_open");
}

// Mutation: return the target unchanged from `tourTargetFor` when compact (the first compact row fails).
TEST_CASE("tours controller: tourTargetFor keeps every target wide and substitutes what a compact frame moves",
          "[ui][tours]") {
    CHECK(ui::tourTargetFor("nav_samples", false) == "nav_samples");
    CHECK(ui::tourTargetFor("shell_search", false) == "shell_search");
    CHECK(ui::tourTargetFor("nav_samples", true) == "shell_nav_menu");
    CHECK(ui::tourTargetFor("shell_search", true) == "shell_search_open");
    for (std::string_view const moved : {"shell_context", "shell_help", "shell_principal_menu", "shell_tours"}) {
        CHECK(ui::tourTargetFor(moved, true) == "shell_nav_menu");
    }
    CHECK(ui::tourTargetFor("shell_inbox", true) == "shell_inbox");
    CHECK(ui::tourTargetFor("samples_table", true) == "samples_table");
}

// Mutation: show the step after a refused open (the tour keeps running and nothing is told).
TEST_CASE("tours controller: a screen that fails to open ends the tour with a notice naming it", "[ui][tours]") {
    Rig r;
    r.shell.refused.insert("locked");
    REQUIRE(r.controller->start("lost").has_value());
    CHECK_FALSE(r.controller->running());
    REQUIRE(r.shell.notices.size() == 1);
    CHECK(r.shell.notices.front().first.contains("Title of locked"));
    CHECK(r.shell.notices.front().second == ui::Tone::Warn);
    CHECK(r.spot("open") == "false");
}

// Mutation: keep the running tour when another starts (the second's title does not show).
TEST_CASE("tours controller: a new tour replaces a running one, and end closes the spotlight and hands focus back",
          "[ui][tours]") {
    Rig r;
    auto const field = r.h.backend.createTextInput(r.page.get(), ui::TextInputMode::SingleLine);
    field->focus();
    REQUIRE(r.controller->start("basics").has_value());
    REQUIRE(r.controller->start("find").has_value());
    CHECK(r.controller->view()->title == "Find");
    CHECK(r.controller->view()->position == 1);
    r.controller->end();
    CHECK(r.spot("open") == "false");
    CHECK(r.h.backend.focused() == r.h.backend.idOf(*field));
    CHECK_FALSE(r.controller->running());
}

// Mutation: accept any id in `start` (it runs the first tour).
TEST_CASE("tours controller: an unknown tour id is refused and starts nothing", "[ui][tours]") {
    Rig r;
    auto const started = r.controller->start("nope");
    REQUIRE_FALSE(started.has_value());
    CHECK(started.error().reason.contains("`nope`"));
    CHECK_FALSE(r.controller->running());
    CHECK_FALSE(r.controller->declares("nope"));
    CHECK(r.controller->declares("basics"));
}

// Mutation: read the tours once in `start` and never follow them (the German title never shows).
TEST_CASE("tours controller: a change of locale re-evaluates the running step's texts", "[ui][tours]") {
    Rig r;
    REQUIRE(r.controller->start("basics").has_value());
    int const before = r.changes;
    r.h.session.setLocale("de");
    r.h.settle();
    CHECK(r.controller->view()->title == "Grundlagen");
    CHECK(r.controller->view()->stepTitle == "Suche");
    CHECK(r.changes > before);
}

// Mutation: list general tours before the screen's in `offeredTours` (the screen's come second).
TEST_CASE("tours controller: offeredTours lists the screen's tours in order, then the general ones", "[ui][tours]") {
    Rig r;
    ui::OfferedTours const offered = ui::offeredTours(r.root->tours().get(), "samples");
    REQUIRE(offered.screen.size() == 2);
    CHECK(offered.screen.at(0).id == "register");
    CHECK(offered.screen.at(1).id == "find");
    REQUIRE(offered.general.size() == 3);
    CHECK(offered.general.at(0).id == "basics");
    CHECK(ui::offeredTours(r.root->tours().get(), "home").screen.empty());
    CHECK(ui::offeredTours({}, "home").general.empty());
}

// Mutation: leave the search text in `Navigator::reveal` (the filtered entry stays hidden).
TEST_CASE("tours controller: Navigator::reveal clears the search and opens the entry's module", "[ui][tours]") {
    ui::testing::RecordingBackend backend;
    ui::NavigatorOptions options;
    ui::CatalogEntry samples;
    samples.id = "lims.samples";
    samples.title = "Samples";
    samples.module = "Lab";
    ui::CatalogEntry invoices;
    invoices.id = "billing.invoices";
    invoices.title = "Invoices";
    invoices.module = "Billing";
    options.entries = {samples, invoices};
    ui::Navigator navigator{backend, nullptr, std::move(options)};
    navigator.setModuleGroups(true);
    REQUIRE(navigator.setOpenModule("Lab"));
    std::optional<int> const search = backend.find("TextInput", "testId", "navigator_search");
    REQUIRE(search.has_value());
    backend.edit(*search, "sam");
    CHECK(navigator.reveal("billing.invoices"));
    CHECK(navigator.openModule() == "Billing");
    CHECK(backend.prop(*search, "text").empty());
    CHECK_FALSE(navigator.reveal("lims.nope"));
}
```

Add `test_ui_tours_controller.cpp` to `tests/CMakeLists.txt` after `test_ui_tours_registry.cpp`.

- [ ] **Step 3: Run the tests to verify they fail**

Run: `flock /tmp/morph-build.lock cmake --build build/clang-debug -j5 --target morph_tests`
Expected: compile error — `morph/ui/tours.hpp: No such file or directory`.

- [ ] **Step 4: The header** (`include/morph/ui/tours.hpp`)

```cpp
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <morph/export.hpp>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "../attributes.hpp"
#include "../reactive/runtime.hpp"
#include "../reactive/signal.hpp"
#include "backend.hpp"
#include "host.hpp"
#include "interpreter.hpp"

/// @file
/// @brief Guided tours on the client: `TourController` runs one of the app document's tours at a time over a
///        `TourShell`, the little it needs of the shell it runs in. Specified in `docs/spec/ui/tours.md`.

namespace morph::ui {

/// @brief What a tour reaches of the shell it runs in. The generic client implements it over its workspace, its frame
///        and its navigator; a test implements it with a fake.
class TourShell {
public:
    TourShell() = default;
    virtual ~TourShell() = default;
    TourShell(TourShell const&) = delete;
    TourShell& operator=(TourShell const&) = delete;
    TourShell(TourShell&&) = delete;
    TourShell& operator=(TourShell&&) = delete;

    /// @brief Opens a screen as a navigator click does, or focuses the tab that already shows it.
    /// @param screenId The screen.
    /// @param params   Its params.
    /// @return Nothing, or why it could not be opened.
    virtual std::expected<void, MountError> openScreen(std::string const& screenId, ParamMap const& params) = 0;

    /// @brief Whether the active tab shows a screen with these params: the same screen, compared by its identity.
    /// @param screenId The screen.
    /// @param params   Its params.
    /// @return It.
    [[nodiscard]] virtual bool shows(std::string const& screenId, ParamMap const& params) const = 0;

    /// @brief The screen the active tab shows, a custom screen named by the screen it is based on.
    /// @return Its id; empty with no tab.
    [[nodiscard]] virtual std::string activeScreen() const = 0;

    /// @brief A screen's title as the navigator shows it, for the hint of a step that waits for it.
    /// @param screenId The screen.
    /// @return The title; the id when there is none.
    [[nodiscard]] virtual std::string screenTitle(std::string const& screenId) const = 0;

    /// @brief Whether the frame has its compact form.
    /// @return It.
    [[nodiscard]] virtual bool compact() const = 0;

    /// @brief Makes a navigator entry drawable in the wide form: empties the navigator's search and opens the module
    ///        that holds the entry.
    /// @param screenId The entry's screen.
    virtual void revealNavEntry(std::string const& screenId) = 0;

    /// @brief The spotlight a tour is shown in, made on first use and kept as long as the frame.
    /// @return It.
    virtual SpotlightWidget& spotlight() = 0;

    /// @brief Tells the person something, as a toast.
    /// @param text The message.
    /// @param tone Its tone.
    virtual void notify(std::string const& text, Tone tone) = 0;
};

/// @brief What the card shows of the running step.
struct TourView {
    /// @brief The tour's id.
    std::string tourId;
    /// @brief The tour's title, the card's eyebrow.
    std::string title;
    /// @brief The step's title.
    std::string stepTitle;
    /// @brief The step's body; empty for none.
    std::string body;
    /// @brief The step's place, from 1.
    std::int64_t position = 0;
    /// @brief How many steps the tour has.
    std::int64_t count = 0;
    /// @brief Whether the step waits for a screen to become active rather than for Next.
    bool waiting = false;
    /// @brief The title of the screen a waiting step waits for; empty otherwise.
    std::string waitingFor;
    /// @brief Whether there is a step before it.
    bool canBack = false;
    /// @brief Whether it is the last step, whose Next is captioned Done.
    bool last = false;

    /// @brief Memberwise equality.
    /// @return Whether every member matches.
    bool operator==(TourView const&) const = default;
};

/// @brief What a `TourController` reports.
struct TourControllerOptions {
    /// @brief Called with a sentence for the developer: a step whose target is not shown (once per step and tour run),
    ///        a tour that stopped. Empty logs at warn.
    std::function<void(std::string const&)> warn;
    /// @brief Called after the view changed, and after a tour ended; empty calls nothing.
    std::function<void()> onChanged;
};

/// @brief The tours the Tours menu offers on one screen.
struct OfferedTours {
    /// @brief The screen's own tours, in declaration order.
    std::vector<Tour> screen;
    /// @brief The general tours, in declaration order.
    std::vector<Tour> general;
};

/// @brief Splits the app document's tours into those a screen offers and the general ones.
/// @param tours    The tours, as `AppRoot::tours` answers them.
/// @param screenId The active screen, a custom screen named by its base; empty for none, which offers only the
///                 general tours.
/// @return The two lists.
[[nodiscard]] MORPH_API OfferedTours offeredTours(std::vector<Tour> const& tours, std::string_view screenId);

/// @brief The test id a step rings in the frame's form: in the compact form a navigator entry and the parts that
///        moved into the navigation menu are rung as that menu, and the search as its button.
/// @param target  The step's target.
/// @param compact Whether the frame is compact.
/// @return The id to ring.
[[nodiscard]] MORPH_API std::string tourTargetFor(std::string_view target, bool compact);

/// @brief Runs at most one of the app document's tours: opens each step's screen, prepares a shell target, points the
///        spotlight at the step's target, and moves on with Next, Back, a waiting step's screen, or End.
///
/// A shell makes one with each frame and destroys it with the frame, which ends a running tour. It reads the tours
/// from `AppRoot::tours()` and follows them while a tour runs, so a change of locale re-evaluates the card's texts.
/// Neither copyable nor movable; on the runtime's owner.
class MORPH_API TourController {
public:
    /// @brief A controller with no tour running.
    /// @param runtime The runtime the root's values belong to. Borrowed.
    /// @param root    The app document's root. Borrowed: it outlives the controller.
    /// @param shell   The shell the tours run in. Borrowed: it outlives the controller.
    /// @param options Where warnings and changes go.
    TourController(reactive::Runtime& runtime MORPH_LIFETIMEBOUND, AppRoot const& root MORPH_LIFETIMEBOUND,
                   TourShell& shell MORPH_LIFETIMEBOUND, TourControllerOptions options = {});

    /// @brief Stops following the tours. The spotlight is left to the frame, which goes with the controller.
    ~TourController();

    TourController(TourController const&) = delete;
    TourController& operator=(TourController const&) = delete;
    TourController(TourController&&) = delete;
    TourController& operator=(TourController&&) = delete;

    /// @brief Ends a running tour, opens the tour's screen when it names one the active tab does not show, opens the
    ///        spotlight and shows the first step. A screen that cannot be opened ends the tour with a toast.
    /// @param tourId The tour.
    /// @return Nothing, or the refusal of an id the app document does not declare, which starts nothing.
    std::expected<void, Refusal> start(std::string_view tourId);

    /// @brief Shows the next step, or ends the tour on the last one. A step that waits for a screen ignores it.
    void next();

    /// @brief Shows the step before. A screen an earlier step opened is not closed; that step's own `screen` opens
    ///        again if it names one.
    void back();

    /// @brief Ends the running tour: the spotlight closes, which hands focus back, and the view empties.
    void end();

    /// @brief Tells the controller the active tab changed: a step waiting for the screen now active moves on.
    void activeChanged();

    /// @brief Whether a tour runs.
    /// @return It.
    [[nodiscard]] bool running() const noexcept { return !_tourId.empty(); }

    /// @brief Whether the app document declares a tour.
    /// @param tourId The tour.
    /// @return It.
    [[nodiscard]] bool declares(std::string_view tourId) const;

    /// @brief What the card shows of the running step.
    /// @return It; nothing while no tour runs.
    [[nodiscard]] std::optional<TourView> const& view() const noexcept { return _view; }

private:
    [[nodiscard]] Tour const* current() const;
    void show(std::size_t step);
    void refresh();
    void fail(std::string const& screenId, MountError const& error);
    void warn(std::string const& text) const;
    void changed() const;

    reactive::Runtime* _runtime;
    AppRoot const* _root;
    TourShell* _shell;
    TourControllerOptions _options;
    std::vector<Tour> _tours;
    std::unique_ptr<reactive::Effect> _follow;
    std::string _tourId;
    std::size_t _step = 0;
    std::set<std::size_t> _warned;
    std::optional<TourView> _view;
    bool _showing = false;
};

}  // namespace morph::ui
```

- [ ] **Step 5: The implementation** (`src/ui/tours.cpp`)

```cpp
// SPDX-License-Identifier: Apache-2.0

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <morph/core/logger.hpp>
#include <morph/reactive/runtime.hpp>
#include <morph/reactive/signal.hpp>
#include <morph/ui/backend.hpp>
#include <morph/ui/host.hpp>
#include <morph/ui/interpreter.hpp>
#include <morph/ui/tours.hpp>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace morph::ui {

OfferedTours offeredTours(std::vector<Tour> const& tours, std::string_view screenId) {
    OfferedTours offered;
    for (Tour const& tour : tours) {
        if (tour.screen.empty()) {
            offered.general.push_back(tour);
        } else if (!screenId.empty() && tour.screen == screenId) {
            offered.screen.push_back(tour);
        }
    }
    return offered;
}

std::string tourTargetFor(std::string_view target, bool compact) {
    if (!compact) {
        return std::string{target};
    }
    if (target.starts_with("nav_")) {
        return "shell_nav_menu";
    }
    if (target == "shell_search") {
        return "shell_search_open";
    }
    if (target == "shell_context" || target == "shell_help" || target == "shell_principal_menu" ||
        target == "shell_tours") {
        return "shell_nav_menu";
    }
    return std::string{target};
}

TourController::TourController(reactive::Runtime& runtime, AppRoot const& root, TourShell& shell,
                               TourControllerOptions options)
    : _runtime{&runtime}, _root{&root}, _shell{&shell}, _options{std::move(options)} {}

TourController::~TourController() = default;

bool TourController::declares(std::string_view tourId) const {
    std::vector<Tour> const& tours = _root->tours().peek();
    return std::ranges::find(tours, tourId, &Tour::id) != tours.end();
}

std::expected<void, Refusal> TourController::start(std::string_view tourId) {
    std::vector<Tour> const& tours = _root->tours().peek();
    auto const found = std::ranges::find(tours, tourId, &Tour::id);
    if (found == tours.end()) {
        return std::unexpected(Refusal{.document = "app",
                                       .path = "/tours",
                                       .reason = "the app document declares no tour `" + std::string{tourId} + "`"});
    }
    std::string const id = found->id;
    std::string const screen = found->screen;
    end();
    _tours = _root->tours().peek();
    _tourId = id;
    _step = 0;
    _warned.clear();
    if (!screen.empty() && _shell->activeScreen() != screen) {
        _showing = true;
        auto const opened = _shell->openScreen(screen, {});
        _showing = false;
        if (!opened) {
            fail(screen, opened.error());
            return {};
        }
    }
    _shell->spotlight().setOpen(true);
    show(0);
    if (!running()) {
        return {};
    }
    // The texts follow the locale and the app state while the tour runs; the steps' structure is fixed for the
    // frame, since a reloaded app document takes effect at the next one.
    _follow = std::make_unique<reactive::Effect>(*_runtime, [this] {
        std::vector<Tour> const& latest = _root->tours().get();
        _runtime->untracked([this, &latest] {
            _tours = latest;
            refresh();
            changed();
        });
    });
    return {};
}

void TourController::next() {
    Tour const* const tour = current();
    if (tour == nullptr || !tour->steps.at(_step).advanceOn.empty()) {
        return;
    }
    if (_step + 1 >= tour->steps.size()) {
        end();
        return;
    }
    show(_step + 1);
}

void TourController::back() {
    if (current() == nullptr || _step == 0) {
        return;
    }
    show(_step - 1);
}

void TourController::end() {
    if (!running()) {
        return;
    }
    _follow.reset();
    _tourId.clear();
    _step = 0;
    _view.reset();
    SpotlightWidget& spotlight = _shell->spotlight();
    spotlight.setTarget(std::nullopt);
    spotlight.setTargetFocusable(false);
    spotlight.setOpen(false);
    changed();
}

void TourController::activeChanged() {
    Tour const* const tour = current();
    // A screen the tour itself is opening is not the person's move.
    if (_showing || tour == nullptr) {
        return;
    }
    std::string const& waitsFor = tour->steps.at(_step).advanceOn;
    if (waitsFor.empty() || _shell->activeScreen() != waitsFor) {
        return;
    }
    if (_step + 1 >= tour->steps.size()) {
        end();
        return;
    }
    show(_step + 1);
}

Tour const* TourController::current() const {
    if (!running()) {
        return nullptr;
    }
    auto const found = std::ranges::find(_tours, _tourId, &Tour::id);
    return found == _tours.end() ? nullptr : &*found;
}

void TourController::show(std::size_t step) {
    Tour const* const tour = current();
    if (tour == nullptr || step >= tour->steps.size()) {
        end();
        return;
    }
    TourStep const shown = tour->steps.at(step);
    if (!shown.screen.empty() && !_shell->shows(shown.screen, shown.params)) {
        _showing = true;
        auto const opened = _shell->openScreen(shown.screen, shown.params);
        _showing = false;
        if (!opened) {
            fail(shown.screen, opened.error());
            return;
        }
    }
    std::string target = shown.target;
    if (!target.empty()) {
        bool const compact = _shell->compact();
        if (!compact && target.starts_with("nav_")) {
            _shell->revealNavEntry(target.substr(4));
        }
        target = tourTargetFor(target, compact);
    }
    _step = step;
    SpotlightWidget& spotlight = _shell->spotlight();
    spotlight.setTargetFocusable(!shown.advanceOn.empty());
    spotlight.setTarget(target.empty() ? std::nullopt : std::optional<std::string>{target});
    if (!target.empty() && !spotlight.targetShown() && _warned.insert(step).second) {
        warn("the tour `" + _tourId + "`, step " + std::to_string(step + 1) + ": nothing shown carries the test id `" +
             target + "`, so its card is centred");
    }
    refresh();
    changed();
}

void TourController::refresh() {
    Tour const* const tour = current();
    if (tour == nullptr || _step >= tour->steps.size()) {
        _view.reset();
        return;
    }
    TourStep const& step = tour->steps.at(_step);
    _view = TourView{
        .tourId = tour->id,
        .title = tour->title,
        .stepTitle = step.title,
        .body = step.body,
        .position = static_cast<std::int64_t>(_step + 1),
        .count = static_cast<std::int64_t>(tour->steps.size()),
        .waiting = !step.advanceOn.empty(),
        .waitingFor = step.advanceOn.empty() ? std::string{} : _shell->screenTitle(step.advanceOn),
        .canBack = _step > 0,
        .last = _step + 1 == tour->steps.size(),
    };
}

void TourController::fail(std::string const& screenId, MountError const& error) {
    std::string const title = _shell->screenTitle(screenId);
    std::string const id = _tourId;
    end();
    _shell->notify("The tour stopped: " + title + " could not be opened", Tone::Warn);
    warn("the tour `" + id + "` stopped: `" + screenId + "` could not be opened: " + error.reason);
}

void TourController::warn(std::string const& text) const {
    if (_options.warn) {
        _options.warn(text);
    } else {
        morph::log::logWarn("[tours] {}", text);
    }
}

void TourController::changed() const {
    if (_options.onChanged) {
        _options.onChanged();
    }
}

}  // namespace morph::ui
```

`CMakeLists.txt`: add `src/ui/tours.cpp` after `src/ui/shell_parts_model.cpp`, and `include/morph/ui/tours.hpp` after
`include/morph/ui/shell_parts_view.hpp`.

- [ ] **Step 6: `Navigator::reveal`**

`include/morph/ui/navigator.hpp`, after `setOpenModule`:

```cpp
    /// @brief Makes a screen's entry drawable: empties the search, which may be hiding it, and, while modules are
    ///        collapsed, opens the module that holds it.
    /// @param screenId The entry's screen.
    /// @return Whether the navigator lists the screen; `false` changes nothing.
    bool reveal(std::string_view screenId);
```

`src/ui/navigator.cpp`, after `setOpenModule`:

```cpp
bool Navigator::reveal(std::string_view screenId) {
    CatalogEntry const* found = nullptr;
    for (Group const& group : _groups) {
        for (Item const& item : group.items) {
            if (item.entry->id == screenId) {
                found = item.entry;
            }
        }
    }
    if (found == nullptr) {
        return false;
    }
    _search->setText({});
    filter(std::string{});
    if (_moduleGroups) {
        static_cast<void>(setOpenModule(found->module));
    }
    return true;
}
```

- [ ] **Step 7: Run the tests to verify they pass**

Run:
```bash
flock /tmp/morph-build.lock cmake --build build/clang-debug -j5 --target morph_tests
ctest --test-dir build/clang-debug -R '^tours controller:|^navigator:' --output-on-failure
```
Expected: `100% tests passed`.

- [ ] **Step 8: Mutation check**

Apply each test's mutation (keep a `.bak`), rebuild `morph_tests`, see that test fail, restore, `touch`, rebuild.

- [ ] **Step 9: Write the reference** (`docs/spec/ui/tours.md`)

```markdown
# Guided tours

A guided tour is a short sequence of steps an application declares in its app document. Each step points at one part
of the GUI: it opens the screen the part is on, dims the rest of the window, rings the part, and shows a card that says
what the part is for. Tours belong to a screen or to the whole application, and a screen may have several, one per
workflow. Specified across `include/morph/ui/tours.hpp` (the controller), `tour_card.hpp` (the card),
`shell_frame.hpp` (the Tours button) and the generic client.

## Contents

- [The declaration](#the-declaration)
- [The tours of a session](#the-tours-of-a-session)
- [The controller](#the-controller)
- [Design decisions](#design-decisions)
- [Not yet](#not-yet)

## The declaration

The app document's `tours` member ([document.md](document.md#the-app-documents-tours)): tours of `{id, screen?, title,
summary?, steps}`, steps of `{title, body?, screen?, params?, target?, advanceOn?}`. A server writes it by hand or with
`author::withTours` ([author.md](author.md)). The bundle refuses a tour that names a screen, a target or a tour it
does not have ([service.md](service.md#the-registry)).

## The tours of a session

`AppRoot::tours()` evaluates every text in the app scope, inside one computed value, so a change of locale re-evaluates
them ([interpreter.md](interpreter.md#the-app-root-scope)). `offeredTours(tours, screenId)` splits them into the tours
of a screen (those whose `screen` is it, in declaration order) and the general ones (no `screen`, in declaration
order): what the Tours menu lists, the screen's first.

## The controller

`TourController` runs at most one tour. A shell builds one with each frame and destroys it with the frame, so a
sign-in or a sign-out, which rebuild the frame, end a running tour; a reconnect leaves the frame and the tour. It
reaches the shell through `TourShell`: open a screen (`openScreen`, as a navigator click does, focusing a tab that
already shows it), whether the active tab shows a screen with given params (`shows`, by identity), the active screen
(`activeScreen`, a custom screen named by the screen it is based on), a screen's title, whether the frame is compact,
`revealNavEntry`, the spotlight, and a toast.

| Event | What happens |
|---|---|
| `start(id)` | An id the app does not declare is refused and starts nothing. Otherwise a running tour ends; the tour's `screen`, when the active screen is not it, is opened; the spotlight opens; step 0 shows |
| Showing step *n* | The step's `screen`, when the active tab does not show it with those params, is opened; a refusal ends the tour and the shell shows a toast naming the screen. A shell target is prepared (below). The spotlight's target is set, and `setTargetFocusable` is true on a waiting step. A target not shown leaves the card centred and is reported once per step and tour run through `TourControllerOptions::warn` |
| `next()` | Shows step *n*+1, or ends the tour on the last step. A waiting step ignores it |
| `back()` | Shows step *n*−1. A screen an earlier step opened is not closed |
| `activeChanged()` | When the step waits for a screen (`advanceOn`) and the active screen is it, the tour moves on. A screen the controller is opening itself does not count |
| `end()` | The spotlight's target is cleared and it closes, which hands focus back to where it was when the tour started; the view empties |

`view()` is what the card shows: the tour's title, the step's title and body, its position and the count, whether it
waits and for which screen's title, whether Back is possible, and whether it is the last step. `onChanged` is called
after each change of it, a change of locale included, which the controller hears by following `AppRoot::tours()` while a
tour runs.

**Preparing a shell target.** In the wide form a `nav_<id>` target is revealed first: the navigator's search is emptied
and the module holding the entry opened (`Navigator::reveal`). In the compact form `tourTargetFor` rings what the frame
draws instead: `nav_<id>`, `shell_context`, `shell_help`, `shell_principal_menu` and `shell_tours` as `shell_nav_menu`,
which holds them; `shell_search` as `shell_search_open`.

## Design decisions

| Decision | Why, and why not the alternative |
|---|---|
| Tours are a member of the app document | It already has a bundle slot, fetch rules, a signed-out gate and a reload path, as `shell` does; a manifest kind of its own would repeat all four. |
| A step names its target by `testId` | Shell ids are stable and documented, and screen authors set `testId` deliberately. A JSON Pointer breaks with every edit of a screen, and an element `id` is scoped, so not unique in a window. |
| The backend resolves and tracks the target | Geometry is the toolkit's. The generic layer has no geometry API, and one would put a frame-by-frame layout question on the seam. |
| A waiting step advances when a screen becomes active | Every frontend observes the active tab the same way. Watching clicks would need click observation in every backend, and could not tell an accidental press from a deliberate one. |
| A target not shown centres the card and warns | The bundle check catches misspelt ids; what remains is a part hidden by state. The card still helps, and ending the tour would strand the person. |
| One tour at a time, owned by the frame | Two spotlights cannot share a window. The frame is rebuilt on sign-in and sign-out, which is what ends a tour then; a reconnect keeps the screens, and the tour. |

## Not yet

- "Seen" state and first-run auto-start: they need app-scope preferences in the generic client.
- A chat tool that starts a tour: a chat tool has no channel to command the client.
- Targets in dynamic rows, and arbitrary click conditions.
- Tours authored by people in the client.
```

`docs/spec/ui/shell_frame.md`, "Module groups", add after the `setOpenModule` sentence: "`reveal(screenId)` empties
the navigator's search and, with module groups, opens the module holding the entry, so a guided tour can ring an entry
that a search or a closed module hides."

- [ ] **Step 10: Commit**

```bash
git add include/morph/ui/tours.hpp src/ui/tours.cpp include/morph/ui/navigator.hpp src/ui/navigator.cpp CMakeLists.txt \
  tests/ui_tours_support.hpp tests/test_ui_tours_controller.cpp tests/CMakeLists.txt docs/spec/ui/tours.md \
  docs/spec/ui/shell_frame.md
git commit -m "Run guided tours with a controller over a small shell interface"
```

---
### Task 7: The card (`kTourCardDocument`) and the `tour.*` host vocabulary

**Lane:** wave 4, after Task 6.

**Files:**
- Modify: `include/morph/ui/host_vocabulary.hpp` (`HostCommand::refusal` after `documents` ~line 50; `run` ~line 200)
- Create: `include/morph/ui/tour_card.hpp`
- Modify: `CMakeLists.txt` (`include/morph/ui/tour_card.hpp` after `include/morph/ui/tours.hpp`)
- Test: `tests/test_ui_tour_card.cpp` (create); `tests/CMakeLists.txt` (after `test_ui_tours_controller.cpp`);
  `tests/test_ui_host_vocabulary.cpp` (one case for `refusal`)
- Docs: `docs/spec/ui/host_vocabulary.md` ("Running a command"), `docs/spec/ui/tours.md` (section "The card")

**Interfaces:**
- Consumes: Task 6's `TourController` (`start`, `next`, `back`, `end`, `declares`, `view`), `TourView`; Task 2's
  `ui::kTourCardId`; `HostVocabulary::add/set`, `HostValue`, `HostCommand`, `HostArg`, `loadDocumentText`.
- Produces:
  - `std::function<std::optional<std::string>(ValueRecord const&)> HostCommand::refusal;` (checked after the argument
    types, before `run`; a reason refuses the command)
  - `inline constexpr std::string_view ui::kTourCardDocument;`
  - `[[nodiscard]] inline std::expected<std::shared_ptr<Document const>, Refusal> ui::tourCardDocument();`
  - `class ui::TourCardBinding { TourCardBinding(HostVocabulary& host, std::function<TourController*()> controller,
    std::function<void(std::function<void()>)> post = {}); void refresh(); };`

- [ ] **Step 1: Write the failing tests**

`tests/test_ui_host_vocabulary.cpp`, at the end:

```cpp
// Mutation: skip `refusal` in `HostVocabulary::run` (the refused id runs).
TEST_CASE("host vocabulary: a command's refusal check refuses it with its reason, and run is not called",
          "[ui][host]") {
    morph::ui::HostVocabulary host;
    std::vector<std::string> ran;
    host.add(morph::ui::HostCommand{
        .name = "pick",
        .version = 1,
        .args = {morph::ui::HostArg{.name = "id", .type = "string"}},
        .safeForUntrusted = true,
        .run = [&ran](morph::ui::ValueRecord const& args) { ran.push_back(*args.at("id").asString()); },
        .documents = {},
        .refusal = [](morph::ui::ValueRecord const& args) -> std::optional<std::string> {
            return *args.at("id").asString() == "bad" ? std::optional<std::string>{"`bad` is not one"} : std::nullopt;
        }});
    morph::ui::HostCommandResult const refused =
        host.run("pick", morph::ui::Value{morph::ui::ValueRecord{{"id", morph::ui::Value{"bad"}}}});
    CHECK(refused.kind == morph::ui::HostCommandResult::Kind::Refused);
    CHECK(refused.reason == "`bad` is not one");
    CHECK(host.run("pick", morph::ui::Value{morph::ui::ValueRecord{{"id", morph::ui::Value{"good"}}}}).kind ==
          morph::ui::HostCommandResult::Kind::Ran);
    CHECK(ran == std::vector<std::string>{"good"});
}
```

`tests/test_ui_tour_card.cpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0
//
// The tour card (docs/spec/ui/tours.md, "The card"): a client document bound to the controller through `tour.*` host
// values and commands, its keys, and `tour.start` from any document.

#include <catch2/catch_test_macros.hpp>
#include <memory>
#include <morph/ui/backend.hpp>
#include <morph/ui/host_vocabulary.hpp>
#include <morph/ui/interpreter.hpp>
#include <morph/ui/tour_card.hpp>
#include <morph/ui/tours.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "ui_interpreter_support.hpp"
#include "ui_tours_support.hpp"

namespace ui = morph::ui;

namespace {

constexpr std::string_view kApp = R"({
  "vocab": "ui/1", "id": "app",
  "tours": [
    { "id": "basics", "title": "Basics",
      "steps": [ { "target": "shell_search", "title": "Search", "body": "Find anything." },
                 { "title": "Second" }, { "title": "Last" } ] },
    { "id": "getting-started", "title": "Getting started",
      "steps": [ { "target": "nav_samples", "advanceOn": { "screen": "samples" }, "title": "Open Samples" },
                 { "title": "Done" } ] } ] })";

/// A screen that starts a tour by a literal id, one by a computed id the app does not declare, and that reads what
/// the card reads and whether it can start a tour.
constexpr std::string_view kStarter = R"({
  "vocab": "ui/1", "id": "lims.home", "state": { "which": { "init": "nope" } },
  "view": { "kind": "column", "children": [
    { "kind": "button", "testId": "take_tour", "label": "Take the tour",
      "onClick": [ { "host": "tour.start", "args": { "obj": { "id": "basics" } } } ] },
    { "kind": "button", "testId": "bad_tour", "label": "Bad",
      "onClick": [ { "host": "tour.start", "args": { "obj": { "id": { "ref": "which" } } } } ] },
    { "kind": "text", "testId": "can_tour", "text": { "fn": "format", "args": [ "{0}", { "can": "host:tour.start" } ] } },
    { "kind": "text", "testId": "peek", "text": { "fn": "coalesce", "args": [ { "host": "tour.title" }, "-" ] } } ] } })";

struct Card {
    Card() {
        auto mounted = h.interpreter.mountApp(ui::testing::loadOrFail(kApp));
        REQUIRE(mounted.has_value());
        root = std::move(*mounted);
        host.connect(h.session);
        binding = std::make_unique<ui::TourCardBinding>(host, [this] { return controller.get(); });
        controller = std::make_unique<ui::TourController>(
            h.runtime, *root, shell, ui::TourControllerOptions{.warn = {}, .onChanged = [this] { binding->refresh(); }});
        shell.onActive = [this] { controller->activeChanged(); };
        shell.onSpotlight = [this](ui::SpotlightWidget& spotlight) {
            auto const document = ui::tourCardDocument();
            REQUIRE(document.has_value());
            auto made = h.interpreter.mount(*document, {}, h.backend, ui::MountOptions{.parent = &spotlight});
            REQUIRE(made.has_value());
            card = std::move(*made);
        };
        page = h.backend.createStack(nullptr, ui::Axis::Vertical);
    }

    void carry(std::string const& testId) {
        parts.push_back(h.backend.createButton(page.get()));
        parts.back()->setTestId(testId);
    }
    [[nodiscard]] int button(std::string_view testId) const { return h.byTestId("Button", testId); }
    [[nodiscard]] bool shown(std::string_view kind, std::string_view testId) const {
        return h.backend.prop(h.byTestId(kind, testId), "visible") != "false";
    }

    ui::HostVocabulary host;
    ui::testing::Harness h;
    ui::testing::FakeTourShell shell{h.backend};
    std::unique_ptr<ui::AppRoot> root;
    std::unique_ptr<ui::StackWidget> page;
    std::vector<std::unique_ptr<ui::ButtonWidget>> parts;
    std::unique_ptr<ui::TourCardBinding> binding;
    std::unique_ptr<ui::TourController> controller;
    std::unique_ptr<ui::Screen> card;
};

}  // namespace

// Mutation: set `tour.stepTitle` from the tour's title in `refresh` (the card's heading reads "Basics").
TEST_CASE("tour card: the card shows the running step's texts, its progress, Back, and Done on the last step",
          "[ui][tours]") {
    Card c;
    c.carry("shell_search");
    REQUIRE(c.controller->start("basics").has_value());
    c.h.settle();
    CHECK(c.h.text("tour_eyebrow") == "Basics");
    CHECK(c.h.text("tour_title") == "Search");
    CHECK(c.h.text("tour_body") == "Find anything.");
    CHECK(c.h.text("tour_progress") == "1 of 3");
    CHECK_FALSE(c.shown("Button", "tour_back"));
    CHECK(c.h.backend.prop(c.button("tour_next"), "label") == "Next");
    c.controller->next();
    c.h.settle();
    CHECK(c.h.text("tour_progress") == "2 of 3");
    CHECK(c.shown("Button", "tour_back"));
    CHECK_FALSE(c.shown("Text", "tour_body"));
    c.controller->next();
    c.h.settle();
    CHECK(c.h.backend.prop(c.button("tour_next"), "label") == "Done");
}

// Mutation: register `tour.back` to run `next` (Back moves on).
TEST_CASE("tour card: Next, Back and End tour run through the card's buttons", "[ui][tours]") {
    Card c;
    REQUIRE(c.controller->start("basics").has_value());
    c.h.settle();
    c.h.backend.click(c.button("tour_next"));
    c.h.settle();
    CHECK(c.controller->view()->position == 2);
    c.h.backend.click(c.button("tour_back"));
    c.h.settle();
    CHECK(c.controller->view()->position == 1);
    c.h.backend.click(c.button("tour_end"));
    c.h.settle();
    CHECK_FALSE(c.controller->running());
    CHECK(c.h.backend.prop(c.shell.spotlightId(), "open") == "false");
    CHECK(c.h.text("tour_title").empty());
}

// Mutation: drop `keys` from `kTourCardDocument` (no chord reaches the controller).
TEST_CASE("tour card: Right moves on, Left moves back and Esc ends the tour, from a focused button", "[ui][tours]") {
    Card c;
    REQUIRE(c.controller->start("basics").has_value());
    c.h.settle();
    int const next = c.button("tour_next");
    CHECK(c.h.backend.press(next, "Right"));
    c.h.settle();
    CHECK(c.controller->view()->position == 2);
    CHECK(c.h.backend.press(next, "Left"));
    c.h.settle();
    CHECK(c.controller->view()->position == 1);
    CHECK(c.h.backend.press(next, "Esc"));
    c.h.settle();
    CHECK_FALSE(c.controller->running());
}

// Mutation: show `tour_next` on a waiting step (both it and the hint show).
TEST_CASE("tour card: on a waiting step the hint replaces Next, and Right does not move on", "[ui][tours]") {
    Card c;
    c.carry("nav_samples");
    REQUIRE(c.controller->start("getting-started").has_value());
    c.h.settle();
    CHECK_FALSE(c.shown("Button", "tour_next"));
    CHECK(c.shown("Text", "tour_waiting"));
    CHECK(c.h.text("tour_waiting") == "Open Title of samples to continue");
    CHECK(c.h.backend.press(c.button("tour_end"), "Right"));
    c.h.settle();
    CHECK(c.controller->view()->position == 1);
    c.shell.active = "samples";
    c.controller->activeChanged();
    c.h.settle();
    CHECK(c.controller->view()->position == 2);
    CHECK(c.shown("Button", "tour_next"));
}

// Mutation: limit `tour.start` to the card's document (the screen's button reports it missing).
TEST_CASE("tour card: tour.start from a document starts the tour, an undeclared id is reported, and can is true",
          "[ui][tours]") {
    Card c;
    auto const starter = c.h.mount(kStarter);
    CHECK(c.h.text("can_tour") == "true");
    c.h.backend.click(c.button("bad_tour"));
    c.h.settle();
    CHECK_FALSE(c.controller->running());
    bool reported = false;
    for (ui::Refusal const& refusal : c.h.diagnostics) {
        reported = reported || refusal.reason.contains("declares no tour `nope`");
    }
    CHECK(reported);
    c.h.backend.click(c.button("take_tour"));
    c.h.settle();
    REQUIRE(c.controller->running());
    CHECK(c.controller->view()->tourId == "basics");
    // The card's values are the card's: another document reads none of them.
    CHECK(c.h.text("peek") == "-");
}
```

Add `test_ui_tour_card.cpp` to `tests/CMakeLists.txt` after `test_ui_tours_controller.cpp`.

- [ ] **Step 2: Run the tests to verify they fail**

Run: `flock /tmp/morph-build.lock cmake --build build/clang-debug -j5 --target morph_tests`
Expected: compile errors — `HostCommand` has no member `refusal`; `morph/ui/tour_card.hpp` not found.

- [ ] **Step 3: `HostCommand::refusal`** (`include/morph/ui/host_vocabulary.hpp`)

After `documents` in `struct HostCommand`:

```cpp
    /// @brief Checks the arguments beyond their types, after they are checked and before `run`: a reason refuses the
    ///        command, which then does not run and answers as one that did not succeed, and the interpreter reports
    ///        the reason. Empty checks nothing more.
    std::function<std::optional<std::string>(ValueRecord const&)> refusal;
```

In `HostVocabulary::run`, between the argument loop and `registered.command.run(checked);`:

```cpp
        if (registered.command.refusal) {
            if (std::optional<std::string> reason = registered.command.refusal(checked)) {
                return refused(std::move(*reason));
            }
        }
```

(Add `#include <optional>` to the header.)

- [ ] **Step 4: The card and its binding** (`include/morph/ui/tour_card.hpp`)

```cpp
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>

#include "client_ids.hpp"
#include "document.hpp"
#include "host_vocabulary.hpp"
#include "tours.hpp"
#include "value.hpp"

/// @file
/// @brief The guided tour's card: a document like any screen, generated by the client, and what connects it and
///        `tour.start` to the frame's `TourController`. Specified in `docs/spec/ui/tours.md`.

namespace morph::ui {

// clang-format off: the raw string is a JSON document laid out by hand, which clang-format would reflow.
/// @brief The card. What it shows and does comes through the `tour.*` host values and commands a `TourCardBinding`
///        registers for this document alone, so it is written in the vocabulary of any screen, every frontend draws it,
///        and its captions resolve through the session's catalogue with English fallbacks.
inline constexpr std::string_view kTourCardDocument = R"json({
  "vocab": "ui/1", "id": "morph.tour",
  "keys": {
    "Esc": [ { "host": "tour.end" } ],
    "Enter": [ { "host": "tour.next" } ],
    "Right": [ { "host": "tour.next" } ],
    "Left": [ { "host": "tour.back" } ] },
  "view": { "kind": "column", "testId": "tour_card", "gap": 2,
    "a11y": { "name": { "fn": "coalesce", "args": [ { "host": "tour.stepTitle" }, "" ] }, "role": "dialog" },
    "children": [
      { "kind": "text", "testId": "tour_eyebrow", "role": "overline",
        "text": { "fn": "coalesce", "args": [ { "host": "tour.title" }, "" ] } },
      { "kind": "text", "testId": "tour_title", "role": "heading",
        "text": { "fn": "coalesce", "args": [ { "host": "tour.stepTitle" }, "" ] } },
      { "kind": "text", "testId": "tour_body",
        "text": { "fn": "coalesce", "args": [ { "host": "tour.body" }, "" ] },
        "visible": { "fn": "not", "args": [ { "fn": "isEmpty",
          "args": [ { "fn": "coalesce", "args": [ { "host": "tour.body" }, "" ] } ] } ] } },
      { "kind": "row", "gap": 2, "align": "center", "sizing": { "lit": { "width": { "stretch": 1 } } },
        "children": [
          { "kind": "text", "testId": "tour_progress", "role": "muted",
            "sizing": { "lit": { "width": { "stretch": 1 } } },
            "text": { "t": "morph.tour.progress", "fallback": "{$position} of {$count}",
                      "args": { "position": { "fn": "coalesce", "args": [ { "host": "tour.position" }, 0 ] },
                                "count": { "fn": "coalesce", "args": [ { "host": "tour.count" }, 0 ] } } } },
          { "kind": "button", "testId": "tour_back", "label": { "t": "morph.tour.back", "fallback": "Back" },
            "visible": { "fn": "coalesce", "args": [ { "host": "tour.canBack" }, false ] },
            "onClick": [ { "host": "tour.back" } ] },
          { "kind": "button", "testId": "tour_next", "emphasis": "primary",
            "label": { "if": [ { "fn": "coalesce", "args": [ { "host": "tour.last" }, false ] },
                               { "t": "morph.tour.done", "fallback": "Done" },
                               { "t": "morph.tour.next", "fallback": "Next" } ] },
            "visible": { "fn": "not", "args": [ { "fn": "coalesce", "args": [ { "host": "tour.waiting" }, false ] } ] },
            "onClick": [ { "host": "tour.next" } ] },
          { "kind": "text", "testId": "tour_waiting", "role": "muted",
            "text": { "t": "morph.tour.waiting", "fallback": "Open {$screen} to continue",
                      "args": { "screen": { "fn": "coalesce", "args": [ { "host": "tour.waitingFor" }, "" ] } } },
            "visible": { "fn": "coalesce", "args": [ { "host": "tour.waiting" }, false ] } },
          { "kind": "button", "testId": "tour_end", "emphasis": "ghost",
            "label": { "t": "morph.tour.end", "fallback": "End tour" },
            "onClick": [ { "host": "tour.end" } ] } ] } ] } })json";
// clang-format on

/// @brief Loads the card document.
/// @return The document, or why it was refused.
[[nodiscard]] inline std::expected<std::shared_ptr<Document const>, Refusal> tourCardDocument() {
    auto loaded = loadDocumentText(kTourCardDocument);
    if (!loaded) {
        return std::unexpected(std::move(loaded.error()));
    }
    return std::make_shared<Document const>(std::move(*loaded));
}

/// @brief Connects the card document and `tour.start` to the frame's controller through a `HostVocabulary`.
///
/// For the card document alone (`kTourCardId`): the values `tour.title`, `tour.stepTitle`, `tour.body`,
/// `tour.waitingFor` (strings), `tour.position`, `tour.count` (ints), `tour.waiting`, `tour.canBack`, `tour.last`
/// (flags), and the commands `tour.next`, `tour.back` and `tour.end`. For every document: `tour.start` with `{id}`,
/// refused, and reported by the interpreter, for an id the app document does not declare. Every command is safe for an
/// untrusted connection: it changes nothing on the server and shows only what the application declared. The commands'
/// work is posted, since a step can open a screen and a screen must not be replaced inside one of its widgets'
/// handlers. A shell makes one binding, which outlives frames, and calls `refresh` after each change of the
/// controller's view. Neither copyable nor movable; on the runtime's owner.
class TourCardBinding {
public:
    /// @brief Registers the values and the commands.
    /// @param host       The vocabulary. Borrowed: it outlives this binding.
    /// @param controller The frame's controller, read at each command and each `refresh`; null between frames.
    /// @param post       Runs a command's work after the handler that asked returns; empty runs it at once.
    TourCardBinding(HostVocabulary& host, std::function<TourController*()> controller,
                    std::function<void(std::function<void()>)> post = {})
        : _host{&host}, _controller{std::move(controller)}, _post{std::move(post)} {
        std::set<std::string, std::less<>> const card{std::string{kTourCardId}};
        for (std::string_view const name : {"tour.title", "tour.stepTitle", "tour.body", "tour.waitingFor"}) {
            host.add(HostValue{.name = std::string{name},
                               .version = 1,
                               .type = "string",
                               .initial = Value{std::string{}},
                               .documents = card});
        }
        for (std::string_view const name : {"tour.position", "tour.count"}) {
            host.add(HostValue{
                .name = std::string{name}, .version = 1, .type = "int", .initial = Value{0}, .documents = card});
        }
        for (std::string_view const name : {"tour.waiting", "tour.canBack", "tour.last"}) {
            host.add(HostValue{
                .name = std::string{name}, .version = 1, .type = "bool", .initial = Value{false}, .documents = card});
        }
        auto const command = [this, &card](std::string name, std::function<void(TourController&)> work) {
            return HostCommand{.name = std::move(name),
                               .version = 1,
                               .args = {},
                               .safeForUntrusted = true,
                               .run = [this, work = std::move(work)](ValueRecord const& /*args*/) { later(work); },
                               .documents = card,
                               .refusal = {}};
        };
        host.add(command("tour.next", [](TourController& tours) { tours.next(); }));
        host.add(command("tour.back", [](TourController& tours) { tours.back(); }));
        host.add(command("tour.end", [](TourController& tours) { tours.end(); }));
        host.add(HostCommand{
            .name = "tour.start",
            .version = 1,
            .args = {HostArg{.name = "id", .type = "string"}},
            .safeForUntrusted = true,
            .run =
                [this](ValueRecord const& args) {
                    std::string const id = *args.at("id").asString();
                    later([id](TourController& tours) { static_cast<void>(tours.start(id)); });
                },
            .documents = {},
            .refusal = [this](ValueRecord const& args) -> std::optional<std::string> {
                TourController const* const tours = _controller ? _controller() : nullptr;
                std::string const& id = *args.at("id").asString();
                if (tours == nullptr) {
                    return std::string{"no tour can start now: no frame is shown"};
                }
                if (!tours->declares(id)) {
                    return "the app document declares no tour `" + id + "`";
                }
                return std::nullopt;
            }});
        refresh();
    }

    TourCardBinding(TourCardBinding const&) = delete;
    TourCardBinding& operator=(TourCardBinding const&) = delete;
    TourCardBinding(TourCardBinding&&) = delete;
    TourCardBinding& operator=(TourCardBinding&&) = delete;
    ~TourCardBinding() = default;

    /// @brief Copies the controller's view into the values; with no tour running, every value is back to its initial
    ///        one.
    void refresh() {
        TourController const* const tours = _controller ? _controller() : nullptr;
        TourView const shown = tours != nullptr && tours->view() ? *tours->view() : TourView{};
        _host->set("tour.title", Value{shown.title});
        _host->set("tour.stepTitle", Value{shown.stepTitle});
        _host->set("tour.body", Value{shown.body});
        _host->set("tour.waitingFor", Value{shown.waitingFor});
        _host->set("tour.position", Value{shown.position});
        _host->set("tour.count", Value{shown.count});
        _host->set("tour.waiting", Value{shown.waiting});
        _host->set("tour.canBack", Value{shown.canBack});
        _host->set("tour.last", Value{shown.last});
    }

private:
    // Runs @p work on the controller of the moment it runs, posted when a post was given.
    void later(std::function<void(TourController&)> work) {
        auto run = [this, work = std::move(work)] {
            if (TourController* const tours = _controller ? _controller() : nullptr) {
                work(*tours);
            }
        };
        if (_post) {
            _post(std::move(run));
        } else {
            run();
        }
    }

    HostVocabulary* _host;
    std::function<TourController*()> _controller;
    std::function<void(std::function<void()>)> _post;
};

}  // namespace morph::ui
```

`CMakeLists.txt`: add `include/morph/ui/tour_card.hpp` after `include/morph/ui/tours.hpp`.

- [ ] **Step 5: Run the tests to verify they pass**

Run:
```bash
flock /tmp/morph-build.lock cmake --build build/clang-debug -j5 --target morph_tests morph_client_tests
ctest --test-dir build/clang-debug -R '^tour card:|^host vocabulary:|chat' --output-on-failure
```
Expected: `100% tests passed`. The chat binding's commands, which give no `refusal`, behave as before.

- [ ] **Step 6: Mutation check**

Apply each test's mutation, rebuild, see that test fail, restore, `touch`, rebuild.

- [ ] **Step 7: Update the specs**

`docs/spec/ui/host_vocabulary.md`, "Running a command", insert as step 4 (renumbering the last step to 5): "4. A command
with a `refusal` check is given the checked record; a reason it answers refuses the command, which does not run and
answers as one that did not succeed, and the interpreter reports the reason. `tour.start` uses it for an id the app
document does not declare."

`docs/spec/ui/tours.md`: add "- [The card](#the-card)" to the contents after "The controller", and the section:

```markdown
## The card

`kTourCardDocument` (`tour_card.hpp`), id `morph.tour` (`kTourCardId`, reserved like the chat panel's: a server may
never serve it). The shell mounts it into the frame's spotlight with `DocumentScreenHost::mountInto`. A
`TourCardBinding` connects it to the frame's controller through the shell's `HostVocabulary`:

| Values (the card only) | Commands |
|---|---|
| `tour.title`, `tour.stepTitle`, `tour.body`, `tour.waitingFor` (string), `tour.position`, `tour.count` (int), `tour.waiting`, `tour.canBack`, `tour.last` (bool) | `tour.next`, `tour.back`, `tour.end` (the card only); `tour.start` with `{id}` (every document) |

| testId | What it is |
|---|---|
| `tour_card` | the card: a column named after the step's title, role `dialog` |
| `tour_eyebrow` | the tour's title |
| `tour_title`, `tour_body` | the step's texts; the body hides while empty |
| `tour_progress` | "2 of 5" (`morph.tour.progress`) |
| `tour_back` | Back; hidden on the first step |
| `tour_next` | Next, or Done on the last step; hidden on a waiting step |
| `tour_waiting` | on a waiting step, "Open Samples to continue" (`morph.tour.waiting`) |
| `tour_end` | End tour |

The captions are `t` messages (`morph.tour.*`) with English fallbacks, so a bundle's catalogue translates them. The
document's `keys` are Esc (`tour.end`), Enter and Right (`tour.next`) and Left (`tour.back`); a waiting step ignores
`tour.next`. The spotlight delivers the card's chords before a focused control's own keys, Enter and Space excepted, so
Enter on a focused button activates that button. Opening the spotlight focuses the card's first shown button.

`tour.start` is registered for every document and is safe for an untrusted connection: it changes nothing on the
server and shows only what the application declared. An id the app does not declare is refused through the command's
`refusal` check, so it answers as a command that did not succeed and the interpreter reports it; the bundle check
already refuses a literal one. `{"can": "host:tour.start"}` is true on a client that runs tours, so a screen can hide
its button on one that does not. The commands' work is posted: a step can open a screen, which must not be replaced
inside one of its widgets' handlers.
```

- [ ] **Step 8: Commit**

```bash
git add include/morph/ui/host_vocabulary.hpp include/morph/ui/tour_card.hpp CMakeLists.txt tests/test_ui_tour_card.cpp \
  tests/test_ui_host_vocabulary.cpp tests/CMakeLists.txt docs/spec/ui/host_vocabulary.md docs/spec/ui/tours.md
git commit -m "Add the guided tour card and its host vocabulary"
```

---
### Task 8: The shell frame — the Tours button, the compact Tours submenu, the spotlight slot

**Lane:** wave 3, after Task 3; parallel with Task 6 (disjoint files except `docs/spec/ui/shell_frame.md`, where
Task 6 edits only "Module groups").

**Files:**
- Modify: `include/morph/ui/shell_parts_view.hpp` (`ShellTourView`, `ShellTourGroup`, `ShellToursView` before
  `ShellLabels` ~line 204)
- Modify: `include/morph/ui/shell_frame.hpp` (`setTours`, `setOnTour`, `spotlight()` after `showToast` ~line 300;
  `barParts()` ~line 348; `NavTarget::Kind::Tours`; members `_tours`, `_spotlight`, `_toursView`, `_tourMap`,
  `_onTour`; private `tourEntries()`, `chooseTour()`)
- Modify: `src/ui/shell_frame.cpp` (`buildParts` after `_help` ~line 588; `showParts` ~line 735; `showNavEntries` ~line
  395; `chooseNav` ~line 437; new definitions after `showToast`)
- Modify: `src/ui/shell_parts_model.cpp` (`builtinShellText` rows, the array's size 27 → 30)
- Test: `tests/test_ui_shell_tours.cpp` (create); `tests/CMakeLists.txt` (after `test_ui_shell_frame.cpp`)
- Docs: `docs/spec/ui/shell_frame.md` ("Declared parts", "The compact form"), `docs/spec/ui/shell_parts.md` ("Where each
  part is drawn", "The compact form")

**Interfaces:**
- Consumes: Task 3's `SpotlightWidget`, `IViewBackend::createSpotlight`; `MenuWidget`, `MenuEntry`, `MenuTrigger`.
- Produces:
  - `struct ShellTourView { std::string id; std::string title; std::string summary; bool operator==(...) const =
    default; };`
  - `struct ShellTourGroup { std::string heading; std::vector<ShellTourView> tours; bool operator==(...) const =
    default; };`
  - `struct ShellToursView { std::vector<ShellTourGroup> groups; std::string caption = "Tours"; std::string name =
    "Guided tours"; [[nodiscard]] bool shown() const noexcept; bool operator==(...) const = default; };`
  - `void ShellFrame::setTours(ShellToursView view);`
  - `void ShellFrame::setOnTour(std::function<void(std::string const&)> onTour);`
  - `[[nodiscard]] SpotlightWidget& ShellFrame::spotlight();`
  - built-in texts `morph.shell.tours` (Tours / Touren), `morph.shell.toursName` (Guided tours / Geführte Touren),
    `morph.shell.toursGeneral` (General / Allgemein)

- [ ] **Step 1: Write the failing tests** (`tests/test_ui_shell_tours.cpp`)

```cpp
// SPDX-License-Identifier: Apache-2.0
//
// The shell frame's tours (docs/spec/ui/shell_frame.md, "Tours"): the Tours button lists the active screen's tours
// under the screen's title and then the general ones, reports the tour chosen, sits after help and before the person's
// menu, and moves into the navigation menu when compact; the frame makes the spotlight once, over the main column.

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <morph/ui/bundle.hpp>
#include <morph/ui/shell_frame.hpp>
#include <morph/ui/shell_parts_model.hpp>
#include <morph/ui/shell_parts_view.hpp>
#include <morph/ui/testing/recording_backend.hpp>
#include <string>
#include <vector>

namespace ui = morph::ui;
using ui::testing::RecordingBackend;

namespace {

ui::ShellFrameOptions options() {
    ui::ShellFrameOptions made;
    made.applicationName = "Lab";
    made.navigator.entries = {ui::CatalogEntry{.id = "lims.samples", .title = "Samples", .group = "Lab"}};
    made.onSignOut = [] {};
    return made;
}

ui::ShellToursView samplesTours() {
    ui::ShellToursView view;
    view.groups = {
        ui::ShellTourGroup{.heading = "Samples",
                           .tours = {ui::ShellTourView{.id = "register", .title = "Register a sample",
                                                       .summary = "From the form to the label"},
                                     ui::ShellTourView{.id = "find", .title = "Find a sample", .summary = {}}}},
        ui::ShellTourGroup{.heading = "General",
                           .tours = {ui::ShellTourView{.id = "getting-started", .title = "Getting started",
                                                       .summary = "The shell in four steps"}}},
    };
    return view;
}

}  // namespace

// Mutation: list the groups' tours without their headings (the items have no heading entry).
TEST_CASE("shell tours: the Tours button lists the screen's tours, then the general ones, and reports the choice",
          "[ui][shell][tours]") {
    RecordingBackend backend;
    ui::ShellFrame frame{backend, nullptr, options()};
    std::vector<std::string> chosen;
    frame.setOnTour([&chosen](std::string const& id) { chosen.push_back(id); });
    frame.setTours(samplesTours());
    int const menu = backend.find("Menu", "testId", "shell_tours").value();
    CHECK(backend.prop(menu, "visible") != "false");
    CHECK(backend.prop(menu, "label").contains("Tours"));
    CHECK(backend.prop(menu, "a11yName") == "Guided tours");
    std::string const items = backend.prop(menu, "items");
    CHECK(items.find("Samples;enabled=false;heading") < items.find("Register a sample"));
    CHECK(items.find("Register a sample") < items.find("Find a sample"));
    CHECK(items.find("Find a sample") < items.find("General;enabled=false;heading"));
    CHECK(items.find("General;enabled=false;heading") < items.find("Getting started"));
    CHECK(items.contains("description=From the form to the label"));
    backend.chooseMenuEntry(menu, {4});
    backend.chooseMenuEntry(menu, {1});
    CHECK(chosen == std::vector<std::string>{"getting-started", "register"});
    // The top bar's tools show for tours alone.
    CHECK(backend.prop(backend.find("Row", "testId", "shell_topbar_tools").value(), "visible") != "false");
}

// Mutation: show the button whatever the view holds (it shows with no tour).
TEST_CASE("shell tours: with no tour the Tours button is hidden", "[ui][shell][tours]") {
    RecordingBackend backend;
    ui::ShellFrame frame{backend, nullptr, options()};
    frame.setTours(samplesTours());
    frame.setTours(ui::ShellToursView{});
    CHECK(backend.prop(backend.find("Menu", "testId", "shell_tours").value(), "visible") == "false");
}

// Mutation: make the Tours menu before the help button (it comes first among the tools).
TEST_CASE("shell tours: the Tours button sits after the help button and before the person's menu",
          "[ui][shell][tours]") {
    RecordingBackend backend;
    ui::ShellFrame frame{backend, nullptr, options()};
    int const tools = backend.find("Row", "testId", "shell_topbar_tools").value();
    std::vector<int> const children = backend.children(tools);
    auto const at = [&](int widget) { return std::ranges::find(children, widget) - children.begin(); };
    int const help = backend.find("Button", "testId", "shell_help").value();
    int const tours = backend.find("Menu", "testId", "shell_tours").value();
    int const person = backend.find("Menu", "testId", "shell_principal_menu").value();
    CHECK(at(help) + 1 == at(tours));
    CHECK(at(tours) + 1 == at(person));
}

// Mutation: leave the Tours submenu out of `showNavEntries` (the compact menu has no "Tours").
// The navigation menu holds the navigator's one group at 0, a separator at 1 and the Tours submenu at 2.
TEST_CASE("shell tours: compact, the Tours entries join the navigation menu and the button hides",
          "[ui][shell][tours]") {
    RecordingBackend backend;
    ui::ShellFrame frame{backend, nullptr, options()};
    std::vector<std::string> chosen;
    frame.setOnTour([&chosen](std::string const& id) { chosen.push_back(id); });
    frame.setTours(samplesTours());
    frame.setCompact(true);
    CHECK(backend.prop(backend.find("Menu", "testId", "shell_tours").value(), "visible") == "false");
    int const nav = backend.find("Menu", "testId", "shell_nav_menu").value();
    std::string const items = backend.prop(nav, "items");
    REQUIRE(items.contains("Tours"));
    CHECK(items.find("Samples") < items.find("Tours"));
    backend.chooseMenuEntry(nav, {2, 4});
    CHECK(chosen == std::vector<std::string>{"getting-started"});
}

// Mutation: make a new spotlight at each call (two Spotlight widgets).
TEST_CASE("shell tours: the frame makes the spotlight once, over its main column", "[ui][shell][tours]") {
    RecordingBackend backend;
    ui::ShellFrame frame{backend, nullptr, options()};
    CHECK(backend.all("Spotlight").empty());
    ui::SpotlightWidget& first = frame.spotlight();
    ui::SpotlightWidget& second = frame.spotlight();
    CHECK(&first == &second);
    REQUIRE(backend.all("Spotlight").size() == 1);
    int const spotlight = backend.all("Spotlight").front();
    CHECK(backend.prop(spotlight, "testId") == "shell_spotlight");
    int const sidebar = backend.find("Row", "testId", "shell_sidebar").value();
    int const shell = backend.find("Row", "testId", "shell").value();
    std::vector<int> const columns = backend.children(shell);
    REQUIRE(columns.size() >= 2);
    CHECK(columns.at(0) == sidebar);
    CHECK(backend.children(columns.at(1)).back() == spotlight);
}

// Mutation: drop the German row of `morph.shell.toursGeneral` (it reads "General" in German).
TEST_CASE("shell tours: the Tours texts are built in, in English and German", "[ui][shell][tours]") {
    ui::FormatContext english;
    english.locale = "en";
    ui::FormatContext german;
    german.locale = "de";
    CHECK(ui::shellText("morph.shell.tours", english) == "Tours");
    CHECK(ui::shellText("morph.shell.toursName", english) == "Guided tours");
    CHECK(ui::shellText("morph.shell.toursGeneral", german) == "Allgemein");
}
```

Add `test_ui_shell_tours.cpp` to `tests/CMakeLists.txt` after `test_ui_shell_frame.cpp`.

- [ ] **Step 2: Run the tests to verify they fail**

Run: `flock /tmp/morph-build.lock cmake --build build/clang-debug -j5 --target morph_tests`
Expected: compile errors — `ShellToursView` is not a member of `morph::ui`; `ShellFrame` has no member `setTours`.

- [ ] **Step 3: The view** (`include/morph/ui/shell_parts_view.hpp`, before `struct ShellLabels`)

```cpp
/// @brief One tour the Tours menu offers.
struct ShellTourView {
    /// @brief The tour's id, what choosing it reports.
    std::string id;
    /// @brief Its title, the entry's caption.
    std::string title;
    /// @brief Its summary, the entry's description; empty for none.
    std::string summary;

    /// @brief Memberwise equality.
    /// @return Whether every member matches.
    bool operator==(ShellTourView const&) const = default;
};

/// @brief A group of the Tours menu, under a heading: the active screen's tours under its title, or the general ones.
struct ShellTourGroup {
    /// @brief The heading.
    std::string heading;
    /// @brief The tours, in order.
    std::vector<ShellTourView> tours;

    /// @brief Memberwise equality.
    /// @return Whether every member matches.
    bool operator==(ShellTourGroup const&) const = default;
};

/// @brief What the Tours button draws: shown while any group holds a tour.
struct ShellToursView {
    /// @brief The groups, in order: the active screen's tours, then the general ones.
    std::vector<ShellTourGroup> groups;
    /// @brief The button's caption, and the compact submenu's.
    std::string caption = "Tours";
    /// @brief The button's accessible name and tooltip.
    std::string name = "Guided tours";

    /// @brief Whether there is a tour to offer.
    /// @return True when a group holds a tour.
    [[nodiscard]] bool shown() const noexcept {
        return std::ranges::any_of(groups, [](ShellTourGroup const& group) { return !group.tours.empty(); });
    }

    /// @brief Memberwise equality.
    /// @return Whether every member matches.
    bool operator==(ShellToursView const&) const = default;
};
```

(Add `#include <algorithm>` if the header lacks it.)

- [ ] **Step 4: The frame** (`include/morph/ui/shell_frame.hpp`)

After `showToast`:

```cpp
    /// @brief Installs the Tours button (`shell_tours`): a button-style menu after the help button and before the
    ///        person's menu, listing each group's heading and its tours (the title, the summary as the entry's
    ///        description). Shown while the view holds a tour; it counts as a part of the top bar, so the top bar's
    ///        tools show for it alone. Compact, it hides and a submenu with the same entries joins the navigation menu
    ///        after the help entry.
    /// @param view What to draw.
    void setTours(ShellToursView view);

    /// @brief Sets what choosing a tour, from the button or the compact submenu, calls.
    /// @param onTour The handler, given the tour's id; empty does nothing.
    void setOnTour(std::function<void(std::string const&)> onTour) { _onTour = std::move(onTour); }

    /// @brief The spotlight a guided tour is shown in: made the first time it is asked for (`shell_spotlight`), as the
    ///        last child of the main column, where it takes no room and covers the whole window, and kept as long as
    ///        the frame.
    /// @return It.
    [[nodiscard]] SpotlightWidget& spotlight();
```

`barParts()` becomes:

```cpp
    [[nodiscard]] bool barParts() const noexcept {
        return _parts.search.shown || _parts.inbox.shown || _parts.principal.shown || _parts.help.shown ||
               _toursView.shown();
    }
```

In `NavTarget::Kind`, add `Tours` after `Help`. Private declarations after `principalMenuEntry()`:

```cpp
    // The groups as heading entries and their tours; `_tourMap` maps an entry to its tour's id, with no value for a
    // heading.
    [[nodiscard]] std::vector<MenuEntry> tourEntries();

    void chooseTour(std::size_t entry);
```

Members: after `_toast`, `std::unique_ptr<SpotlightWidget> _spotlight;` (with the comment "Made the first time
`spotlight` is called; declared after the main column, its parent, so it goes first."); after `_help`,
`std::unique_ptr<MenuWidget> _tours;`; after `_principalMap`, `std::vector<std::optional<std::string>> _tourMap;`;
after `_handlers`, `ShellToursView _toursView;` and `std::function<void(std::string const&)> _onTour;`.

`src/ui/shell_frame.cpp`, in `buildParts`, between the `_help` block and `_principalMenu`:

```cpp
    // Client-made, after the help button and before the person's menu: declaring a tour already says that it is
    // offered.
    _tours = _backend.createMenu(_tools.get(), MenuStyle::Button);
    _tours->setTestId("shell_tours");
    _tours->setVisible(false);
    _tours->setOnActivate([this](std::vector<std::size_t> const& path) {
        if (path.size() == 1) {
            chooseTour(path.front());
        }
    });
```

In `showParts`, after the help button's two lines:

```cpp
    // The Tours button.
    bool const tours = _toursView.shown();
    _tours->setVisible(tours && !_compact);
    if (tours) {
        _tours->setItems(tourEntries());
        _tours->setTrigger(MenuTrigger{.label = _toursView.caption,
                                       .icon = {},
                                       .emphasis = ButtonEmphasis::Ghost,
                                       .detail = {},
                                       .avatar = {},
                                       .caret = MenuCaret::Down});
        _tours->setAccessibleName(_toursView.name);
        _tours->setTooltip(_toursView.name);
    }
```

In `showNavEntries`, after the help block:

```cpp
    if (_toursView.shown()) {
        add(MenuEntry{.separator = true}, NavTarget::Kind::Separator, 0);
        MenuEntry submenu;
        submenu.label = _toursView.caption;
        submenu.items = tourEntries();
        add(std::move(submenu), NavTarget::Kind::Tours, 0);
    }
```

In `chooseNav`, a case before `Principal`:

```cpp
        case NavTarget::Kind::Tours:
            if (path.size() == 2) {
                chooseTour(path.at(1));
            }
            return;
```

After `showToast`:

```cpp
void ShellFrame::setTours(ShellToursView view) {
    _toursView = std::move(view);
    showParts();
}

SpotlightWidget& ShellFrame::spotlight() {
    if (_spotlight == nullptr) {
        // Under the main column, as the frame's dialogs are: a child of the root row would be a third column of it.
        _spotlight = _backend.createSpotlight(_main.get());
        _spotlight->setTestId("shell_spotlight");
    }
    return *_spotlight;
}

std::vector<MenuEntry> ShellFrame::tourEntries() {
    std::vector<MenuEntry> entries;
    _tourMap.clear();
    for (ShellTourGroup const& group : _toursView.groups) {
        if (group.tours.empty()) {
            continue;
        }
        MenuEntry heading;
        heading.label = group.heading;
        heading.heading = true;
        heading.enabled = false;
        entries.push_back(std::move(heading));
        _tourMap.emplace_back(std::nullopt);
        for (ShellTourView const& tour : group.tours) {
            MenuEntry entry;
            entry.label = tour.title;
            entry.description = tour.summary;
            entries.push_back(std::move(entry));
            _tourMap.emplace_back(tour.id);
        }
    }
    return entries;
}

void ShellFrame::chooseTour(std::size_t entry) {
    if (entry < _tourMap.size() && _tourMap.at(entry) && _onTour) {
        // NOLINTNEXTLINE(bugprone-unchecked-optional-access): the condition above checked this slot.
        _onTour(*_tourMap.at(entry));
    }
}
```

The test "Tours button sits after help": `_help` is made, then `_tours`, then `_principalMenu`, all appended to `_tools`,
so the order holds without a `moveChild`.

- [ ] **Step 5: The built-in texts** (`src/ui/shell_parts_model.cpp`)

In `builtinShellText`, change `std::array<Entry, 27>` to `std::array<Entry, 30>` and add after the `morph.shell.help`
row:

```cpp
        {"morph.shell.tours", "Tours", "Touren"},
        {"morph.shell.toursName", "Guided tours", "Geführte Touren"},
        {"morph.shell.toursGeneral", "General", "Allgemein"},
```

- [ ] **Step 6: Run the tests to verify they pass**

Run:
```bash
flock /tmp/morph-build.lock cmake --build build/clang-debug -j5 --target morph_tests morph_tui_tests
ctest --test-dir build/clang-debug -R '^shell tours:|^shell frame:|^shell parts:|^tui shell' --output-on-failure
```
Expected: `100% tests passed` (the frame's existing cases are unchanged by a hidden Tours button; check
`test_ui_shell_parts.cpp` cases that count `shell_topbar_tools`' children or compare the compact menu's whole `items`
string still pass — the button is hidden and adds no entry while no tour is set).

- [ ] **Step 7: Mutation check**

Apply each test's mutation, rebuild, see that test fail, restore, `touch`, rebuild.

- [ ] **Step 8: Update the specs**

`docs/spec/ui/shell_frame.md`: add "- [Tours](#tours)" to the contents after "Toast", and a section after "Toast":

```markdown
## Tours

`setTours(ShellToursView)` installs the Tours button, `shell_tours`: a button-style menu in the top bar's tools after the
help button and before the person's menu, captioned `caption` ("Tours", `morph.shell.tours`) and named `name` ("Guided
tours", `morph.shell.toursName`). Its entries are each group's `heading` as a heading entry and then its tours, the
title as the caption and the summary as the description; choosing one calls `setOnTour`'s handler with the tour's id.
The client gives the active screen's tours under the screen's title, then the general ones under "General"
(`morph.shell.toursGeneral`). It shows while a group holds a tour, and then counts as a part of the top bar, so the
tools show for it alone. Menu entries carry no test id: a test chooses them by path. Compact, the button hides and a
"Tours" submenu with the same entries joins the navigation menu after the help entry.

`spotlight()` makes the frame's `SpotlightWidget` (`shell_spotlight`) the first time it is asked for, as the last child
of the main column, as the frame's dialogs are: a child of the root row would be a third column of it. It takes no
room and covers the whole window, sidebar included, on every backend, and goes with the frame.
```

In "The compact form", in the list of the navigation menu's entries, after "a separator and the help entry (when
shown)" insert ", a separator and the Tours submenu (when there are tours)". In "Declared parts", in the sentence
listing the top bar's line's widgets in order, insert "`shell_tours` (the Tours button, [Tours](#tours))" between
`shell_help` and `shell_principal_menu`.

`docs/spec/ui/shell_parts.md`, "Where each part is drawn": after the Help row add
`| Tours | after the help button: the client's Tours menu (`shell_tours`, [shell_frame.md](shell_frame.md#tours)), not a declared part: declaring a tour in the app document offers it |`;
change "a bundle that declares `search`, `inbox`, `help` or `principal` mounts its workspace" to "a bundle that declares
`search`, `inbox`, `help` or `principal`, or any tour, mounts its workspace"; in "Focus order" insert "the Tours menu"
after "the help button". In "The compact form", after "a separator and the help entry;" insert "a separator and the
Tours submenu when there are tours;".

- [ ] **Step 9: Commit**

```bash
git add include/morph/ui/shell_parts_view.hpp include/morph/ui/shell_frame.hpp src/ui/shell_frame.cpp \
  src/ui/shell_parts_model.cpp tests/test_ui_shell_tours.cpp tests/CMakeLists.txt docs/spec/ui/shell_frame.md \
  docs/spec/ui/shell_parts.md
git commit -m "Draw the Tours button and own the spotlight in the shell frame"
```

---
### Task 9: The generic client — Tours menu, `tour.start`, `startTour`, frame lifetime

**Lane:** wave 5, after Tasks 1, 2, 6, 7 and 8.

**Files:**
- Modify: `include/morph/ui/frontend.hpp` (`MountedShell::startTour` after `inspectFocused` ~line 400)
- Modify: `src/client/shell.cpp` (includes; `~Shell` ~line 160; the host's documents lookup ~line 247; `relabel` ~line
  386; `finishSignOut` ~line 530; `buildFrame` ~lines 560–675; `tryMount` ~line 1315; new members after `_chatPanel`
  ~line 1700; new members functions beside `followChat`)
- Test: `tests/client/tours_app.hpp`, `tests/client/tours_app.cpp`, `tests/client/test_tours.cpp` (create);
  `tests/client/test_shell.cpp` (one case), `tests/client/test_custom_screens.cpp` (fixtures take a bundle; one case);
  `tests/client/CMakeLists.txt` (add `tours_app.cpp` and `test_tours.cpp` after `test_sources.cpp`)
- Docs: `docs/spec/client/client.md` (a section "Tours" after "Custom screens"), `docs/spec/ui/tours.md` (a section
  "The generic client"), `docs/spec/ui/frontend.md` (`MountedShell::startTour`)

**Interfaces:**
- Consumes: Task 1 `Document::tours`, `AppRoot::tours()`; Task 2 `ui::reservedClientId`, `ui::kTourCardId`; Task 6
  `TourShell`, `TourController`, `TourControllerOptions`, `offeredTours`; Task 7 `TourCardBinding`,
  `tourCardDocument()`; Task 8 `ShellFrame::setTours`, `setOnTour`, `spotlight()`, `ShellToursView`, `ShellTourGroup`,
  `ShellTourView`, the built-in texts `morph.shell.tours*`; `Navigator::reveal`; `CustomScreenController::baseOf`;
  `DocumentScreenHost::identityOf`, `ui::identityKey`, `mountInto`.
- Produces:
  - `virtual std::expected<void, std::string> ui::MountedShell::startTour(std::string_view tourId);` (default refuses)
  - `std::expected<std::shared_ptr<ui::ServedBundle const>, ui::Refusal> tours::toursBundle();` (tests only)

- [ ] **Step 1: The test application** (`tests/client/tours_app.hpp`, `tests/client/tours_app.cpp`)

```cpp
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <expected>
#include <memory>
#include <morph/ui/service.hpp>

/// @file
/// A two-screen application with guided tours, which the client's tour tests open: `tours.home` starts a tour from a
/// button, `tours.samples` carries the targets, and `tours.closed` is listed but not built.

namespace tours {

/// @brief The bundle: the two screens, the unbuilt entry, and an app document with four tours.
/// @return It, or the registry's refusal.
[[nodiscard]] std::expected<std::shared_ptr<morph::ui::ServedBundle const>, morph::ui::Refusal> toursBundle();

}  // namespace tours
```

```cpp
// SPDX-License-Identifier: Apache-2.0

#include "tours_app.hpp"

#include <expected>
#include <memory>
#include <morph/ui/service.hpp>
#include <string>
#include <string_view>
#include <utility>

namespace tours {
namespace {

constexpr std::string_view kHome = R"({"vocab":"ui/1","id":"tours.home","title":"Home",
  "state":{"missing":{"init":"nope"}},
  "view":{"kind":"column","children":[
    {"kind":"text","testId":"home_text","text":"Home"},
    {"kind":"button","testId":"take_tour","label":"Take the tour",
     "onClick":[{"host":"tour.start","args":{"obj":{"id":"getting-started"}}}]},
    {"kind":"button","testId":"bad_tour","label":"Bad",
     "onClick":[{"host":"tour.start","args":{"obj":{"id":{"ref":"missing"}}}}]},
    {"kind":"text","testId":"can_tour","text":{"fn":"format","args":["{0}",{"can":"host:tour.start"}]}}]}})";

constexpr std::string_view kSamples = R"({"vocab":"ui/1","id":"tours.samples","title":"Samples",
  "view":{"kind":"column","children":[
    {"kind":"text","testId":"samples_table","text":"rows"},
    {"kind":"text","testId":"samples_filter","text":"filter"}]}})";

constexpr std::string_view kTours = R"([
  {"id":"getting-started","title":"Getting started","summary":"The shell",
   "steps":[{"target":"shell_tours","title":"Tours"},
            {"target":"nav_tours.samples","advanceOn":{"screen":"tours.samples"},"title":"Open Samples"},
            {"target":"samples_table","title":"The table"}]},
  {"id":"register","screen":"tours.samples","title":"Register a sample",
   "steps":[{"target":"samples_table","title":"Here"}]},
  {"id":"find","screen":"tours.samples","title":"Find a sample",
   "steps":[{"target":"samples_filter","title":"Filter"}]},
  {"id":"locked","title":"Locked away",
   "steps":[{"screen":"tours.closed","target":"shell_tours","title":"Closed"}]}])";

}  // namespace

std::expected<std::shared_ptr<morph::ui::ServedBundle const>, morph::ui::Refusal> toursBundle() {
    morph::ui::UiRegistry served{morph::ui::ApplicationInfo{.id = "tours", .version = "1"}};
    auto const add = [&served](std::string id, std::string const& title, std::string_view text) {
        morph::ui::CatalogEntry entry;
        entry.id = std::move(id);
        entry.title = title;
        return served.addScreen(std::move(entry), std::string{text});
    };
    if (auto added = add("tours.home", "Home", kHome); !added) {
        return std::unexpected(std::move(added.error()));
    }
    if (auto added = add("tours.samples", "Samples", kSamples); !added) {
        return std::unexpected(std::move(added.error()));
    }
    morph::ui::CatalogEntry closed;
    closed.id = "tours.closed";
    closed.title = "Closed";
    closed.available = false;
    if (auto added = served.addEntry(std::move(closed)); !added) {
        return std::unexpected(std::move(added.error()));
    }
    if (auto added = served.addFile(morph::ui::BundleFile{
            .kind = "app", .id = "app", .bytes = R"({"vocab":"ui/1","id":"app","tours":)" + std::string{kTours} + "}"});
        !added) {
        return std::unexpected(std::move(added.error()));
    }
    return served.build();
}

}  // namespace tours
```

- [ ] **Step 2: Write the failing tests**

`tests/client/test_tours.cpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0
//
// The generic client's guided tours (docs/spec/client/client.md, "Tours"), in process: the Tours menu follows the
// active tab, a document's `tour.start` and `startTour` start a tour, a waiting step moves on with the navigator, a
// screen that cannot open ends the tour with a toast, compact rings the navigation menu, and sign-out ends a tour.

// First, ahead of every header that brings view.hpp: GCC's -Wshadow reports value.hpp's enumerator `Type::Key` when
// view.hpp's `ui::Key` was declared before it.
// clang-format off
#include <morph/ui/value.hpp>
// clang-format on

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <expected>
#include <memory>
#include <morph/client/shell.hpp>
#include <morph/client/source.hpp>
#include <morph/ui/testing/recording_backend.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <testkit/fake_app_context.hpp>
#include <testkit/wait.hpp>
#include <utility>
#include <vector>

#include "tours_app.hpp"

namespace client = morph::client;
namespace ui = morph::ui;
using morph::examples::testing::FakeAppContext;
using morph::examples::testing::pumpUntil;

namespace {

/// The generic client over the tours application, signed in locally as `ann`, until the Tours menu is made.
struct Window {
    Window() {
        std::optional<std::expected<ui::Bundle, ui::ConnectError>> opened;
        source.open(context,
                    [&opened](std::expected<ui::Bundle, ui::ConnectError> result) { opened = std::move(result); });
        REQUIRE(pumpUntil(context.owner(), [&opened] { return opened.has_value(); }));
        REQUIRE(opened->has_value());
        shell = client::mountShell(
            context, backend, std::move(*opened), source,
            client::ShellOptions{.startScreen = "tours.home",
                                 .onProblem = [this](std::string const& text) { problems.push_back(text); }});
        REQUIRE(wait([this] { return find("Menu", "shell_tours").has_value(); }));
    }
    ~Window() {
        shell.reset();
        source.close();
    }
    Window(Window const&) = delete;
    Window& operator=(Window const&) = delete;
    Window(Window&&) = delete;
    Window& operator=(Window&&) = delete;

    [[nodiscard]] std::optional<int> find(std::string_view kind, std::string const& testId) const {
        return backend.find(kind, "testId", testId);
    }
    [[nodiscard]] std::string prop(std::string_view kind, std::string const& testId, std::string_view name) const {
        std::optional<int> const widget = find(kind, testId);
        return widget ? backend.prop(*widget, name) : std::string{};
    }
    [[nodiscard]] bool shown(std::string_view kind, std::string const& testId) const {
        std::optional<int> const widget = find(kind, testId);
        return widget && backend.prop(*widget, "visible") != "false";
    }
    [[nodiscard]] std::string text(std::string const& testId) const { return prop("Text", testId, "text"); }
    [[nodiscard]] std::string spotlight(std::string_view name) const { return prop("Spotlight", "shell_spotlight", name); }
    [[nodiscard]] bool reported(std::string_view part) const {
        return std::ranges::any_of(problems, [part](std::string const& line) { return line.contains(part); });
    }
    template <typename Pred>
    bool wait(Pred pred) {
        return pumpUntil(context.owner(), pred);
    }
    void click(std::string const& testId) {
        REQUIRE(wait([this, &testId] { return shown("Button", testId); }));
        backend.click(*find("Button", testId));
    }

    FakeAppContext context;
    ui::testing::RecordingBackend backend;
    client::LocalSource source{client::LocalApp{.name = "tours", .bundle = &tours::toursBundle},
                               client::LocalSetup{.db = {}, .user = "ann"}};
    std::vector<std::string> problems;
    std::unique_ptr<ui::MountedShell> shell;
};

}  // namespace

// Mutation: list the general tours first in `showToursMenu` (Getting started comes before the screen's tours).
TEST_CASE("Client shell tours: the Tours menu lists the active screen's tours before the general ones and follows the "
          "tab",
          "[client][tours]") {
    Window window;
    std::string items = window.prop("Menu", "shell_tours", "items");
    CHECK_FALSE(items.contains("Register a sample"));
    CHECK(items.contains("General;enabled=false;heading"));
    CHECK(items.contains("Getting started"));
    window.click("nav_tours.samples");
    REQUIRE(window.wait(
        [&window] { return window.prop("Menu", "shell_tours", "items").contains("Register a sample"); }));
    items = window.prop("Menu", "shell_tours", "items");
    CHECK(items.find("Samples;enabled=false;heading") < items.find("Register a sample"));
    CHECK(items.find("Register a sample") < items.find("Find a sample"));
    CHECK(items.find("Find a sample") < items.find("General;enabled=false;heading"));
}

// Mutation: skip opening the tour's screen in `TourController::start` (the target is not shown).
TEST_CASE("Client shell tours: a screen's tour started from another screen opens that screen and rings its target",
          "[client][tours]") {
    Window window;
    REQUIRE(window.shell->startTour("register").has_value());
    REQUIRE(window.wait([&window] { return window.spotlight("targetShown") == "true"; }));
    CHECK(window.spotlight("target") == "samples_table");
    REQUIRE(window.wait([&window] { return window.text("tour_title") == "Here"; }));
    CHECK(window.text("tour_eyebrow") == "Register a sample");
}

// Mutation: drop `followTours` from the workspace's change callback (the waiting step never moves on).
TEST_CASE("Client shell tours: a tour from the Tours menu waits for the navigator and moves on when Samples opens",
          "[client][tours]") {
    Window window;
    // On the home screen the menu holds the "General" heading, then Getting started, then Locked away.
    window.backend.chooseMenuEntry(*window.find("Menu", "shell_tours"), {1});
    REQUIRE(window.wait([&window] { return window.spotlight("open") == "true"; }));
    CHECK(window.spotlight("target") == "shell_tours");
    window.click("tour_next");
    REQUIRE(window.wait([&window] { return window.spotlight("target") == "nav_tours.samples"; }));
    REQUIRE(window.wait([&window] { return !window.shown("Button", "tour_next"); }));
    window.click("nav_tours.samples");
    REQUIRE(window.wait([&window] { return window.spotlight("target") == "samples_table"; }));
}

// Mutation: limit `tour.start` to the card's document (the home screen's button is reported missing).
TEST_CASE("Client shell tours: tour.start from a document starts the tour, and can host:tour.start is true",
          "[client][tours]") {
    Window window;
    REQUIRE(window.wait([&window] { return window.text("can_tour") == "true"; }));
    window.click("take_tour");
    REQUIRE(window.wait([&window] { return window.spotlight("open") == "true"; }));
    REQUIRE(window.wait([&window] { return window.text("tour_eyebrow") == "Getting started"; }));
}

// Mutation: drop the `refusal` of `tour.start` (the unknown id is posted, refused by `start`, and never reported).
TEST_CASE("Client shell tours: tour.start naming a tour the app does not declare is reported", "[client][tours]") {
    Window window;
    window.click("bad_tour");
    REQUIRE(window.wait([&window] { return window.reported("declares no tour `nope`"); }));
    CHECK(window.spotlight("open") != "true");
}

// Mutation: skip the toast in `TourController::fail` (no toast shows).
TEST_CASE("Client shell tours: a step whose screen cannot be opened ends the tour with a toast", "[client][tours]") {
    Window window;
    window.backend.chooseMenuEntry(*window.find("Menu", "shell_tours"), {2});
    REQUIRE(window.wait([&window] {
        std::vector<int> const toasts = window.backend.all("Toast");
        return !toasts.empty() && window.backend.prop(toasts.front(), "text").contains("Closed");
    }));
    CHECK(window.spotlight("open") == "false");
}

// Mutation: ignore the frame's form in the shell's `TourShell::compact` (the navigator entry is rung as itself).
TEST_CASE("Client shell tours: compact, the Tours entries join the navigation menu and a navigator target is the menu",
          "[client][tours]") {
    Window window;
    window.backend.setWidthClass("medium");
    REQUIRE(window.wait([&window] { return window.prop("Menu", "shell_nav_menu", "items").contains("Tours"); }));
    CHECK(window.prop("Menu", "shell_tours", "visible") == "false");
    REQUIRE(window.shell->startTour("getting-started").has_value());
    window.click("tour_next");
    REQUIRE(window.wait([&window] { return window.spotlight("target") == "shell_nav_menu"; }));
}

// Mutation: make the frame's spotlight a backend root (`createSpotlight(nullptr)`), so it outlives the frame.
TEST_CASE("Client shell tours: signing out ends a running tour", "[client][tours]") {
    Window window;
    REQUIRE(window.shell->startTour("getting-started").has_value());
    REQUIRE(window.wait([&window] { return window.spotlight("open") == "true"; }));
    window.click("shell_signOut");
    REQUIRE(window.wait([&window] {
        std::vector<int> const spotlights = window.backend.all("Spotlight");
        return std::ranges::none_of(spotlights,
                                    [&window](int id) { return window.backend.prop(id, "open") == "true"; });
    }));
}

// Mutation: answer every id from `startTour` with success (the unknown id is not refused).
TEST_CASE("Client shell tours: startTour refuses a tour the app does not declare", "[client][tours]") {
    Window window;
    auto const refused = window.shell->startTour("nope");
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error().contains("`nope`"));
}
```

`tests/client/test_shell.cpp`: add `#include "tours_app.hpp"` and, after the "env online follows the transport's drop
and reconnect" case:

```cpp
// Mutation: call `buildFrame` from the source's connect listener (the running tour's spotlight goes with the frame).
TEST_CASE("Client shell: a reconnect leaves a running tour running", "[client][shell][tours]") {
    FakeAppContext context;
    ui::testing::RecordingBackend backend;
    DroppingSource source{&tours::toursBundle};
    std::optional<std::expected<ui::Bundle, ui::ConnectError>> opened;
    source.open(context,
                [&opened](std::expected<ui::Bundle, ui::ConnectError> result) { opened = std::move(result); });
    REQUIRE(pumpUntil(context.owner(), [&opened] { return opened.has_value(); }));
    REQUIRE(opened->has_value());
    {
        auto const shell = client::mountShell(context, backend, std::move(*opened), source,
                                              client::ShellOptions{.startScreen = "tours.home"});
        REQUIRE(pumpUntil(context.owner(),
                          [&backend] { return backend.find("Menu", "testId", "shell_tours").has_value(); }));
        REQUIRE(shell->startTour("getting-started").has_value());
        auto const spotlight = [&backend] { return backend.find("Spotlight", "testId", "shell_spotlight"); };
        REQUIRE(pumpUntil(context.owner(),
                          [&] { return spotlight().has_value() && backend.prop(*spotlight(), "open") == "true"; }));
        int const before = *spotlight();
        (*source.disconnectHook)();
        REQUIRE(pumpUntil(context.owner(), [&source] { return !source.online(); }));
        (*source.connectHook)();
        REQUIRE(pumpUntil(context.owner(), [&source] { return source.online(); }));
        REQUIRE(spotlight().has_value());
        CHECK(*spotlight() == before);
        CHECK(backend.prop(before, "open") == "true");
    }
    source.close();
}
```

`tests/client/test_custom_screens.cpp`:

1. After `customsBundle`, add:

```cpp
using BundleFn = std::expected<std::shared_ptr<ui::ServedBundle const>, ui::Refusal> (*)();

constexpr std::string_view kOther =
    R"({"vocab":"ui/1","id":"customs.other","title":"Other","view":{"kind":"text","testId":"other","text":"Other"}})";

/// The customs application with a second screen and a tour whose first step waits for the home screen.
std::expected<std::shared_ptr<ui::ServedBundle const>, ui::Refusal> customsToursBundle() {
    ui::UiRegistry served{ui::ApplicationInfo{.id = "customs", .version = "1"}};
    for (auto const& [id, text] : {std::pair{std::string{"customs.home"}, kHome}, std::pair{std::string{"customs.other"}, kOther}}) {
        ui::CatalogEntry entry;
        entry.id = id;
        entry.title = id;
        if (auto added = served.addScreen(std::move(entry), std::string{text}); !added) {
            return std::unexpected(std::move(added.error()));
        }
    }
    if (auto added = served.addFile(ui::BundleFile{
            .kind = "app",
            .id = "app",
            .bytes = R"({"vocab":"ui/1","id":"app","tours":[{"id":"to-home","title":"To home","steps":[)"
                     R"({"advanceOn":{"screen":"customs.home"},"title":"Open home"},{"title":"Arrived"}]}]})"});
        !added) {
        return std::unexpected(std::move(added.error()));
    }
    return served.build();
}
```

2. `struct Offered`: replace `Offered() {` and `auto built = customsBundle();` with
   `explicit Offered(BundleFn make = &customsBundle) {` and `auto built = make();`.

3. `struct Window`: add a fourth constructor parameter `BundleFn bundle = &customsBundle`, initialize `source` in the
   member-initializer list — `: mineId{offered.mineId}, source{client::LocalApp{.name = "customs", .bundle = bundle},
   client::LocalSetup{.db = {}, .user = "ann"}}` — and remove `source`'s in-class initializer (keep the member's
   declaration where it is, after `backend`).

4. At the end of the file:

```cpp
// Mutation: answer the active tab's own id from the shell's `TourShell::activeScreen`, not its base (the waiting step
// never moves on for a custom screen).
TEST_CASE("Client shell: a tour's waiting step moves on when a custom screen based on its screen opens",
          "[client][custom-screens][tours]") {
    Offered const offered{&customsToursBundle};
    Window window{offered, "customs.other", nullptr, &customsToursBundle};
    REQUIRE(window.shell->startTour("to-home").has_value());
    window.click("nav_" + window.mineId);
    REQUIRE(window.wait([&window] {
        std::optional<int> const title = window.find("Text", "tour_title");
        return title && window.backend.prop(*title, "text") == "Arrived";
    }));
}
```

`tests/client/CMakeLists.txt`: add `tours_app.cpp` and `test_tours.cpp` after `test_sources.cpp`.

- [ ] **Step 3: Run the tests to verify they fail**

Run: `flock /tmp/morph-build.lock cmake --build build/clang-debug -j5 --target morph_client_tests`
Expected: compile error — `MountedShell` has no member `startTour`.

- [ ] **Step 4: `MountedShell::startTour`** (`include/morph/ui/frontend.hpp`, after `inspectFocused`)

```cpp
    /// @brief Starts one of the application's guided tours, as its Tours menu or a document's `tour.start` does. The
    ///        reason comes back as text, so this header stays free of the document model. The default runs no tours.
    /// @param tourId The tour's id, as the app document declares it.
    /// @return Nothing, or why it did not start: an id the app document does not declare, or a shell that runs none.
    virtual std::expected<void, std::string> startTour(std::string_view tourId) {
        return std::unexpected("this shell runs no guided tours, so it cannot start `" + std::string{tourId} + "`");
    }
```

- [ ] **Step 5: Wire the shell** (`src/client/shell.cpp`)

Includes: `<morph/ui/client_ids.hpp>`, `<morph/ui/tour_card.hpp>`, `<morph/ui/tours.hpp>`,
`<morph/ui/custom_screen_controller.hpp>`.

In the host's documents lookup (`hostOptions.documents`), replace `if (screenId == ui::chat::kChatPanelId) {` with
`if (ui::reservedClientId(screenId) != nullptr) {` and its comment with "Only the client's own documents (the chat
panel, the tour card), whatever the server listed."

In `tryMount`, replace the chat panel check with:

```cpp
        // The client's own documents (the chat panel, the tour card) belong to the frame: a route or a navigation to
        // one must not open it as a tab.
        if (ui::ReservedClientId const* const reserved = ui::reservedClientId(screenId)) {
            return std::unexpected(std::string{reserved->owner} + " is not a screen to open");
        }
```

In `~Shell`, after `_chatPanel.reset();`: `resetTours();`. In `buildFrame`, after `_chatPanel.reset();`:
`resetTours();`. In `finishSignOut`'s posted lambda, before `unmount();`: `resetTours();`.

In `buildFrame`, replace the `_headInPage` statement and the comment before `tabs.chrome` with:

```cpp
        // The shell mounts the head row itself, in its top bar: an in-page head would show it a second time. An
        // application that declares the search, the inbox, the help button or the person's menu, or any tour, has the
        // top bar for them, and its screens keep their heads in the page.
        tabs.chrome = [this](ui::WorkspaceTabId tab, std::string_view screenId) -> ui::Node {
            return _customChrome ? _customChrome->chromeFor(tab, screenId) : nullptr;
        };
        bool const declaredBar =
            _appDocument && _appDocument->shell &&
            (!_appDocument->shell->search.empty() || _appDocument->shell->inbox || _appDocument->shell->principal ||
             _appDocument->shell->help);
        _headInPage = declaredBar || (_appDocument && !_appDocument->tours.empty());
```

`tabs.onChanged` becomes:

```cpp
        tabs.onChanged = [this] {
            showActiveTitle();
            followChat();
            followTours();
        };
```

and after `buildChat();` at the end of `buildFrame`: `buildTours();`.

In `relabel()`, after `showActiveTitle();`: `showToursMenu();`.

`startTour`, public, after `inspectFocused`:

```cpp
    std::expected<void, std::string> startTour(std::string_view tourId) override {
        if (_tours == nullptr) {
            return std::unexpected("no tour can start: the shell shows no frame with an app document, so `" +
                                   std::string{tourId} + "` does not run");
        }
        auto const started = _tours->start(tourId);
        if (!started) {
            return std::unexpected(started.error().reason);
        }
        return {};
    }
```

Private, after `followChat`:

```cpp
    // What a tour reaches of this shell: its workspace, its frame and its navigator.
    class Tours final : public ui::TourShell {
    public:
        explicit Tours(Shell& shell) : _shell{&shell} {}

        std::expected<void, ui::MountError> openScreen(std::string const& screenId,
                                                       ui::ParamMap const& params) override {
            auto const opened = _shell->tryMount(screenId, params);
            if (!opened) {
                return std::unexpected(ui::MountError{.screenId = screenId, .reason = opened.error(), .pointer = {}});
            }
            return {};
        }
        [[nodiscard]] bool shows(std::string const& screenId, ui::ParamMap const& params) const override {
            ui::ScreenHandle* const handle = _shell->_workspace ? _shell->_workspace->activeHandle() : nullptr;
            if (handle == nullptr || _shell->activeScreen() != screenId) {
                return false;
            }
            std::optional<std::string> const wanted = _shell->_host->identityOf(screenId, params);
            return !wanted || wanted == ui::identityKey(handle->identity().peek());
        }
        [[nodiscard]] std::string activeScreen() const override { return _shell->activeBaseScreen(); }
        [[nodiscard]] std::string screenTitle(std::string const& screenId) const override {
            return _shell->screenTitleOf(screenId);
        }
        [[nodiscard]] bool compact() const override { return _shell->_frame != nullptr && _shell->_frame->compact(); }
        void revealNavEntry(std::string const& screenId) override {
            if (_shell->_frame != nullptr) {
                static_cast<void>(_shell->_frame->navigator().reveal(screenId));
            }
        }
        ui::SpotlightWidget& spotlight() override { return _shell->tourSpotlight(); }
        void notify(std::string const& text, ui::Tone tone) override {
            if (_shell->_frame != nullptr) {
                _shell->_frame->showToast(text, tone);
            }
        }

    private:
        Shell* _shell;
    };

    // The screen the active tab shows, a custom screen named by the screen it is based on, as a tour compares it.
    [[nodiscard]] std::string activeBaseScreen() const {
        std::string const screenId = activeScreen();
        if (ui::isCustomScreenId(screenId) && _customChrome != nullptr) {
            return _customChrome->controller().baseOf(screenId);
        }
        return screenId;
    }

    // The frame's spotlight, with the tour card mounted in it the first time a tour asks in this frame.
    ui::SpotlightWidget& tourSpotlight() {
        ui::SpotlightWidget& spotlight = _frame->spotlight();
        if (_tourCard == nullptr) {
            auto document = ui::tourCardDocument();
            if (!document) {
                problem("the tour card: " + document.error().message());
                return spotlight;
            }
            _generated.insert_or_assign(std::string{ui::kTourCardId}, std::move(*document));
            auto mounted = _host->mountInto(ui::kTourCardId, {}, spotlight);
            if (!mounted) {
                problem("the tour card: " + mounted.error().reason);
                return spotlight;
            }
            _tourCard = std::move(*mounted);
        }
        return spotlight;
    }

    // The frame's tours: the controller over this shell, the Tours menu following the app document's tours and the
    // active tab, and `tour.start` for every document. With no app root there is nothing to offer.
    void buildTours() {
        if (!_tourBinding) {
            _tourBinding = std::make_unique<ui::TourCardBinding>(
                _chatHost, [this] { return _tours.get(); },
                [this](std::function<void()> work) {
                    // Posted: a step can open a screen, and the command runs inside one of a widget's handlers.
                    _context->executor().post([alive = std::weak_ptr<bool>{_alive}, work = std::move(work)] {
                        if (auto const live = alive.lock(); live != nullptr && *live) {
                            work();
                        }
                    });
                });
        }
        _frame->setOnTour([this](std::string const& tourId) {
            // Posted: the menu is a widget of the frame, and the tour may open a screen.
            _context->executor().post([this, alive = std::weak_ptr<bool>{_alive}, tourId] {
                if (auto const live = alive.lock(); live != nullptr && *live && _tours) {
                    if (auto const started = _tours->start(tourId); !started) {
                        problem(started.error().message());
                    }
                }
            });
        });
        if (_appRoot == nullptr) {
            _frame->setTours(ui::ShellToursView{});
            return;
        }
        ui::TourControllerOptions options;
        options.warn = [this](std::string const& text) { problem(text); };
        options.onChanged = [this] {
            if (_tourBinding) {
                _tourBinding->refresh();
            }
        };
        _tours = std::make_unique<ui::TourController>(_context->runtime(), *_appRoot, _tourShell, std::move(options));
        _toursMenu = std::make_unique<reactive::Effect>(_context->runtime(), [this] {
            static_cast<void>(_appRoot->tours().get());
            _context->runtime().untracked([this] { showToursMenu(); });
        });
    }

    // The Tours menu follows the active tab, and a step waiting for the screen now active moves on.
    void followTours() {
        showToursMenu();
        if (_tours) {
            _tours->activeChanged();
        }
    }

    // The Tours menu for the active tab: its screen's tours under its title, then the general ones.
    void showToursMenu() {
        if (_frame == nullptr || _appRoot == nullptr || _session == nullptr) {
            return;
        }
        std::string const screen = activeBaseScreen();
        ui::OfferedTours const offered = ui::offeredTours(_appRoot->tours().peek(), screen);
        ui::FormatContext const format = _session->formattingNow();
        ui::ShellToursView view;
        view.caption = ui::shellText("morph.shell.tours", format);
        view.name = ui::shellText("morph.shell.toursName", format);
        auto const group = [](std::string heading, std::vector<ui::Tour> const& tours) {
            ui::ShellTourGroup out{.heading = std::move(heading), .tours = {}};
            for (ui::Tour const& tour : tours) {
                out.tours.push_back(ui::ShellTourView{.id = tour.id, .title = tour.title, .summary = tour.summary});
            }
            return out;
        };
        if (!offered.screen.empty()) {
            view.groups.push_back(group(screenTitleOf(screen), offered.screen));
        }
        if (!offered.general.empty()) {
            view.groups.push_back(group(ui::shellText("morph.shell.toursGeneral", format), offered.general));
        }
        _frame->setTours(std::move(view));
    }

    // Drops the frame's tour, its card and its menu: before the frame, the workspace and the app root they read. The
    // card's values go back to their initial ones.
    void resetTours() {
        _toursMenu.reset();
        _tours.reset();
        _tourCard.reset();
        if (_tourBinding) {
            _tourBinding->refresh();
        }
    }
```

Members, after `std::unique_ptr<ui::ScreenHandle> _chatPanel;`:

```cpp
    // What a tour reaches of this shell; it holds nothing of its own.
    Tours _tourShell{*this};
    // Connects the tour card and `tour.start` to the frame's controller. Made once, like the chat's binding, since the
    // vocabulary it registers in outlives frames.
    std::unique_ptr<ui::TourCardBinding> _tourBinding;
    // The frame's tour controller, the card mounted in its spotlight, and what follows the Tours menu: rebuilt with each
    // frame (`resetTours`), before the frame and the app root they read go.
    std::unique_ptr<ui::TourController> _tours;
    std::unique_ptr<ui::ScreenHandle> _tourCard;
    std::unique_ptr<reactive::Effect> _toursMenu;
```

(Update the comment of `_chatHost`: "What the client's own documents (the chat panel, the tour card) and `tour.start`
read and run; it outlives the session it is connected to.")

- [ ] **Step 6: Run the tests to verify they pass**

Run:
```bash
flock /tmp/morph-build.lock cmake --build build/clang-debug -j5 --target morph_client_tests morph_tests
ctest --test-dir build/clang-debug -R '^Client shell|^the_shell|chat|custom' --output-on-failure
```
Expected: `100% tests passed`, the nine new tour cases and the existing client cases among them (an application with no
tours keeps its head placement and shows no Tours button).

- [ ] **Step 7: Mutation check**

Apply each test's mutation (keep a `.bak`), rebuild `morph_client_tests` (`morph_tests` for the frame's spotlight
parent), see that test fail, restore, `touch`, rebuild.

- [ ] **Step 8: Update the specs**

`docs/spec/client/client.md`: add "- [Tours](#tours)" to the contents after "Custom screens", and at the end:

```markdown
## Tours

The shell runs the app document's guided tours ([`../ui/tours.md`](../ui/tours.md)).

- **Per frame.** Each frame builds a `TourController` over the shell (its `TourShell`: `openScreen` is the shell's own
  tab opening, `shows` compares the active tab's identity, `activeScreen` maps a custom screen to the screen it is
  based on through the custom-screens list, `revealNavEntry` is `Navigator::reveal`, `spotlight` is the frame's with the
  card mounted the first time, `notify` the frame's toast). A sign-in or a sign-out builds a new frame, which ends a
  running tour; a reconnect keeps the frame, and the tour. A changed app document takes effect at the next frame, as
  every app-document member does.
- **The Tours menu.** An effect over `AppRoot::tours()` and the workspace's change callback keep `ShellFrame::setTours`
  current: the active screen's tours (a custom screen offers its base's) under the screen's navigator title, then the
  general ones under "General". With any tour declared, the screens' heads are in the page, as they are with a declared
  top-bar part. Choosing a tour starts it, posted.
- **The card.** `kTourCardDocument` is resolved from the client's own generated documents only (its id `morph.tour` is
  reserved, and a route or a navigation to it opens nothing) and mounted into the frame's spotlight. One
  `TourCardBinding`, made once on the shell's host vocabulary, connects it and `tour.start` to the frame's controller;
  its commands' work is posted.
- **From client code.** `MountedShell::startTour(id)` starts a tour, or answers why not: an id the app document does
  not declare, or no frame with an app document.
- **Reports.** A step whose target is not shown is reported once per step and run through the shell's problem
  callback; a screen a step cannot open ends the tour with a toast naming it.
```

`docs/spec/ui/tours.md`: add "- [The generic client](#the-generic-client)" to the contents after "The card", and the
section: "## The generic client\n\nThe generic client builds a controller and the Tours menu with each frame and mounts
the card into the frame's spotlight; see [client.md](../client/client.md#tours). `MountedShell::startTour(id)` starts a
tour from client code; its default refuses."

`docs/spec/ui/frontend.md`: where `MountedShell` is described, add "`startTour(id)` starts one of the application's
guided tours and answers why not as text, so the frontend seam stays free of the document model; the default refuses."

- [ ] **Step 9: Commit**

```bash
git add include/morph/ui/frontend.hpp src/client/shell.cpp tests/client/tours_app.hpp tests/client/tours_app.cpp \
  tests/client/test_tours.cpp tests/client/test_shell.cpp tests/client/test_custom_screens.cpp \
  tests/client/CMakeLists.txt docs/spec/client/client.md docs/spec/ui/tours.md docs/spec/ui/frontend.md
git commit -m "Run guided tours in the generic client"
```

---
### Task 10: Spec index, changelog, and the whole-branch sweeps

**Lane:** last, after every other task.

**Files:**
- Modify: `docs/spec/README.md` (the `ui/` index, after the `ui/shell_parts.md` entry ~line 180)
- Modify: `CHANGELOG.md` (`## [Unreleased]`, a `### Added` entry)
- Modify: any file a sweep below finds wrong

- [ ] **Step 1: Index the new spec** (`docs/spec/README.md`)

After the `ui/shell_parts.md` entry:

```markdown
[`ui/tours.md`](ui/tours.md) — guided tours: the app document's `tours`, the controller that runs one at a time, the
card and its `tour.*` host vocabulary, the spotlight each backend draws, and the Tours menu ·
```

- [ ] **Step 2: The changelog** (`CHANGELOG.md`, under `## [Unreleased]`, in `### Added`, creating the heading if absent)

```markdown
- **Guided tours.** An application declares tours in its app document's `tours` member — general ones, and any number
  per screen, one per workflow — with steps that open a screen, ring a part by its `testId`, and can wait for the
  person to open a screen. The shell's **Tours** button lists the active screen's tours before the general ones; a
  document starts one with the host command `tour.start`; client code with `MountedShell::startTour`. Qt Quick dims the
  window around the target and rings it; the terminal places the card beside the target and draws it in reverse video.
  `UiRegistry::build` refuses a tour that names a screen, a target or a tour the bundle does not have. The backend
  contract gains `SpotlightWidget` (`createSpotlight`), and the id `morph.tour` is reserved for the client's card.
```

- [ ] **Step 3: Build every target**

Run:
```bash
flock /tmp/morph-build.lock cmake --build build/clang-debug -j5
```
Expected: ends without errors or warnings.

- [ ] **Step 4: Run the whole suite**

Run: `ctest --test-dir build/clang-debug -j6 --output-on-failure | tail -5`
Expected: `100% tests passed`.

- [ ] **Step 5: The sweeps**

```bash
# Qt keywords as identifiers in added library lines (they break only Qt translation units).
git diff master...HEAD -U0 -- include/morph src | grep -E '^\+' | grep -nwE 'slots|signals|emit' || echo "no Qt keywords"
# Non-ASCII test names (Windows ctest mangles them).
git diff master...HEAD -U0 -- tests | grep -E '^\+TEST_CASE' | grep -nP '[^\x00-\x7F]' || echo "ASCII test names"
# History and tickets in comments and specs.
git diff master...HEAD -U0 -- include src tests docs/spec | grep -E '^\+' | grep -nE '#[0-9]{2,}|morph#|[0-9a-f]{9,}' \
  || echo "no issue numbers or hashes"
```
Expected: the three `echo` lines. Fix anything else they print.

- [ ] **Step 6: The GCC sweep** (local builds are clang, and miss `-Wrange-loop-construct` and GCC's other warnings)

Run, for each changed translation unit (`src/ui/document.cpp src/ui/interpreter.cpp src/ui/service.cpp
src/ui/tour_checks.cpp src/ui/tours.cpp src/ui/shell_frame.cpp src/ui/navigator.cpp src/ui/shell_parts_model.cpp
src/client/shell.cpp`), the command `compile_commands.json` gives it with `clang++` replaced by `g++` and
`-fsyntax-only` added, under the lock, never with `-w`:

```bash
for tu in src/ui/tour_checks.cpp src/ui/tours.cpp src/ui/shell_frame.cpp; do
  cmd=$(jq -r --arg f "$PWD/$tu" '.[] | select(.file == $f) | .command' build/clang-debug/compile_commands.json \
        | sed -e 's/^[^ ]*clang++/g++/' -e 's/ -o [^ ]*//' -e 's/ -c / -fsyntax-only -c /')
  flock /tmp/morph-build.lock sh -c "$cmd" || echo "FAILED: $tu"
done
```
(extend the list to every file named above). Control: add `for (std::string const s : std::vector<std::string>{}) {}`
to `src/ui/tours.cpp`, confirm g++ warns, and remove it.
Expected: no warning on any file.

- [ ] **Step 7: The documentation build**

Run:
```bash
flock /tmp/morph-build.lock cmake -S . -B build/docs -G Ninja -DMORPH_BUILD_DOCUMENTATION=ON -DMORPH_BUILD_TESTS=OFF \
  -DMORPH_BUILD_EXAMPLES=OFF
flock /tmp/morph-build.lock cmake --build build/docs --target doc
```
Expected: ends without a warning (`WARN_AS_ERROR = FAIL_ON_WARNINGS`). A missing `@param`/`@return` on a new public
symbol (`SpotlightWidget`, `TourController`, `TourCardBinding`, `ShellToursView`, `author::withTours`, …) fails here.

- [ ] **Step 8: clang-tidy on the new files** (at most three at a time, no lock)

Run: `clang-tidy -p build/clang-debug src/ui/tours.cpp src/ui/tour_checks.cpp src/ui/shell_frame.cpp`, then the same
for `src/client/shell.cpp src/qt_quick/spotlight.cpp src/tui/container_widgets.cpp`.
Expected: no diagnostics. Do not accept an `auto*` or `std::expected` fix-it as offered; write the types out.

- [ ] **Step 9: Commit**

```bash
git add docs/spec/README.md CHANGELOG.md
git commit -m "Index the guided tours spec and note it in the changelog"
```

---

## Follow-up (not in this plan)

The lims proof is a separate plan and change, after this framework pull request merges: a general "Getting started"
tour with a waiting step that asks the person to open Samples; "Register a sample", "Find a sample" and "Release
results" on the samples screen and "Issue an invoice" on one billing screen; English and German texts; `tour.start` on
the samples list's empty state; a lims GUI test that checks the Tours list and runs "Register a sample" end to end in
the headless shell; and a render-smoke capture of the spotlight over the samples table.

---

## Deviations from the design

Where the code forced a different choice than `2026-11-23-guided-tours-design.md` states. Update the design to match.

1. **No `AppDocument` author type exists.** The author API has no app-document builder (`author.md`, "Out of
   scope": the app document is written as JSON). Tours are written with `author::Tour`/`author::TourStep` and
   `author::withTours(appDocumentText, tours)`, which returns the app document's text with its `tours` member, ready for
   `UiRegistry::addFile`. `UiRegistry` stays free of the author headers, as `service.hpp` requires.
2. **`chat_input` is always an accepted target.** The registry cannot know whether chat is offered: chat is the
   `ChatService`'s, wired outside `UiRegistry`. A server without chat shows that step's card centred, with the warning.
3. **Shell id names.** The design's `shell_principal` and `shell_scope` are, in the code, `shell_principal_menu` (the
   declared person's menu; `shell_principal` is the sidebar's plain text, hidden while the menu shows) and
   `shell_context` (the switcher). `nav_<id>` is accepted only for a registered screen that has a document and is not
   public, since those are the entries the navigator lists.
4. **Effective screen order.** The design lists "the last `screen` named by an earlier step" before "the `advanceOn`
   of the step just before it". Taken literally, a step after `{screen: home} → {advanceOn: samples}` would be checked
   against `home` though the active tab shows `samples`. The check follows the screen the active tab shows when the step
   shows: its own `screen`, else the latest of an earlier step's `screen` and an earlier step's `advanceOn`, else the
   tour's `screen`.
5. **Shape refusals happen at `addFile`.** An empty, malformed or repeated tour id, no steps, more than 50 and a
   non-literal `params` are refused by the loader when the app document is added (the client's interpreter checks the
   same shape when it loads it); `UiRegistry::build` checks the names. Both are before the bundle is served.
6. **`SpotlightWidget` has a fifth member, `setTargetFocusable(bool)`.** The design wants the target in the focus
   chain on a waiting step only; the backend needs to be told which steps those are.
7. **The card is the spotlight's children; there is no `SpotlightWidget::card()`.** The design names both; the code
   follows the class shown in "The spotlight widget": the spotlight is the container the card mounts into.
8. **Key priority keeps Enter and Space with the focused widget.** "The backend delivers a card's keys before its
   buttons see them" would make Enter on a focused Back or End run `tour.next`. Every chord but Enter and Space comes to
   the card first (so Left and Right cannot be taken by a focused control), and Enter or Space activates the focused
   button, as everywhere else. Opening the spotlight focuses the card's first shown button, which on a first step is
   Next; on a waiting first step it is End tour.
9. **`HostCommand::refusal`.** A host command's `run` returns nothing, so it cannot answer "did not succeed". A new
   optional `refusal(args)` check on `HostCommand` refuses before `run`; `tour.start` uses it for an undeclared id,
   which the interpreter then reports as any refused command.
10. **`tour.waitingFor` and `tour.last` host values.** The waiting hint names a screen ("Open Samples to continue")
    and Next reads Done on the last step; the card needs the screen's title and the last-step flag, which the design's
    value list does not carry.
11. **The card has no accessible description.** ui/1 gives a column an accessible name and role (`a11y`), not a
    description; the card is a `dialog` named after the step's title, and its body is the text after the title.
12. **`MountedShell::startTour` returns `std::expected<void, std::string>`**, not `Refusal`: `frontend.hpp` is kept
    free of the document model (and of glaze, which `value.hpp` brings).
13. **The frame's spotlight is a child of the main column, not of the root row.** A child of the root row would be a
    third column of it, as `shell_frame.md` already says of the toast; the spotlight draws in a window-level layer on
    every backend, so it still covers the sidebar.
14. **Qt Quick's process-wide list.** `findWidget` and `liveWidgets` stay as test helpers, reading every context's
    `WidgetIndex`; the spotlight reads only its own context's index.
15. **The echoing test backend forwards `createSpotlight` to its recording backend** rather than making a plain
    container: the recording probe's spotlight conformance cases run through it.
16. **The conformance probe's lookup is by spotlight.** `ConformanceProbe::targetOf(spotlight)` answers the widget the
    spotlight rings, which is what the cases must observe; a lookup by test id would not show which of two carriers was
    picked. The spotlight cases are their own list, `spotlightConformanceCases()`, so each backend runs them in the task
    that implements it.
17. **`IViewBackend::createSpotlight` has a default that throws `std::logic_error`.** A third-party backend keeps
    compiling, and a tour on it fails loudly rather than silently.
18. **Card captions.** They are `t` messages (`morph.tour.*`) with English fallbacks inside the card document, as
    the chat panel's literals are client-owned; the Tours button's texts are built-in shell texts with German
    (`morph.shell.tours`, `morph.shell.toursName`, `morph.shell.toursGeneral`).

---

## Self-review

### Spec coverage

| Design section | Task(s) |
|---|---|
| Goals — server writes tours; translated texts | 1 (`tours`, `AppRoot::tours()` re-evaluating on locale), 6 (view follows), 9 (menu relabels) |
| Goals — several tours per screen, found on the screen | 6 (`offeredTours`), 8 (Tours button), 9 (menu follows the tab) |
| Goals — shell or screen targets; every frontend; refused at build | 2, 3, 4, 5 |
| Decisions — `tours` in the app document; `testId` targets | 1, 2 |
| Decisions — backend resolves and tracks the target | 3, 4, 5 |
| Decisions — card is a ui/1 document with `tour.*` vocabulary | 7 |
| Decisions — tours belong to a screen; Tours button; `tour.start` | 1, 7, 8, 9 |
| Decisions — `advanceOn` on active tab; missing target centred with a warning | 6, 9 (custom-screen base mapping) |
| Decisions — one tour at a time; owned by the frame; no seen state | 6, 9 |
| Authoring a tour (members, `{"t": …}`, author API) | 1 |
| Starting a tour — the Tours button, compact submenu, menu paths | 8, 9 |
| Starting a tour — from a document (`tour.start`, `can`) | 2 (literal check), 7, 9 |
| Starting a tour — from client code (`startTour`) | 9 |
| Running a tour — events table | 6 |
| Running a tour — preparing shell targets | 6 (`tourTargetFor`, `Navigator::reveal`), 9 (wiring) |
| Running a tour — toast on `MountError`; frame rebuild; reconnect | 6, 9 |
| The spotlight widget — interface, index, picking, notifications | 3 |
| The spotlight widget — Qt Quick drawing, geometry, pointer, missing target, per-context index | 4 |
| The spotlight widget — terminal placement and reverse video | 5 |
| The spotlight widget — recording backend, echoing backend, conformance | 3 |
| The card — document, reserved id, host vocabulary, testIds, keys, focus, accessibility, captions | 2 (reserved id), 7, 3–5 (key order and focus per backend) |
| Checks — every refusal, effective screen, accepted targets, host screens, `tour.start` | 1 (shape), 2 (names) |
| Testing — tours list headless | 6 (`offeredTours`), 8, 9 |
| Testing — controller headless (each listed case) | 6; sign-out and reconnect in 9 (`test_tours.cpp`, `test_shell.cpp`); custom base in 9 (`test_custom_screens.cpp`); focus return in 3 and 6; locale in 6 |
| Testing — load and build, each refusal, mutation check of the target check | 1, 2 |
| Testing — backend conformance on every backend | 3 (recording), 4 (Qt Quick), 5 (terminal) |
| Testing — Qt Quick offscreen (cut-out, placement, pointer, scroll) | 4 |
| Testing — terminal (card box, reverse video, Tab to target) | 5 |
| Testing — host command (`tour.start`, unknown id, `can`) | 7, 9 |
| Proof in lims | follow-up note only |
| Out of scope | `tours.md` "Not yet" (6) |

### Placeholder scan

No "TBD", "TODO", "similar to Task N" or "add error handling" in a step. Every code step carries the code; every run
step names the command and the expected result. One step names a conditional include (Task 1 Step 4, `#include
<variant>` if the compiler asks) — a header, not behaviour.

### Type consistency

- `ui::Tour`/`ui::TourStep` (Task 1) are what `offeredTours`, `TourController` (Task 6) and the shell's menu (Task 9)
  read; `author::Tour`/`author::TourStep` are distinct types in `ui::author`, used only by `withTours`.
- `SpotlightWidget`'s five members (Task 3) are what `FakeSpotlight`, `qt_quick::detail::SpotlightImpl`,
  `tui::detail::SpotlightImpl`, `TourController::show/end` and the conformance cases call.
- `TourShell` (Task 6) is implemented by `testing::FakeTourShell` (Tasks 6, 7) and `client::Shell::Tours` (Task 9) with
  the same eight signatures; `openScreen` returns `std::expected<void, MountError>` in all three.
- `TourView`'s members (Task 6) are exactly what `TourCardBinding::refresh` (Task 7) copies into the nine `tour.*`
  values the card document reads.
- `ShellToursView`/`ShellTourGroup`/`ShellTourView` (Task 8) are built by `Shell::showToursMenu` (Task 9) with the same
  member names (`groups`, `caption`, `name`; `heading`, `tours`; `id`, `title`, `summary`).
- `kTourCardId` (Task 2) is the card document's `id` (Task 7) and the id the shell mounts and reserves (Task 9).
- `HostCommand::refusal` is declared after `documents` (Task 7), so the existing designated initializers, which end at
  `documents`, still compile.

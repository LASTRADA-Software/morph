# Custom Screens — Client Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** The generic client offers custom screens: **Source** on every document screen, drafts mounted in place with a
draft bar (Save, Save as…, Source, Discard), the navigator's Custom section, **Open as** on screens with params, custom
routes in the record and the history, and discovery by probing `list`.

**Architecture:** Framework pieces first, each with its own test: JSON source helpers, a text cursor any backend can
move, drafts on `DocumentScreenHost` (built on `remount`), `WorkspaceModel::openInPlace` and a per-page chrome slot,
the navigator's Custom section, then `CustomScreenSync` (the client of `morph.customScreens`, through the interpreter's
own `Dispatcher`, as `PreferenceSync` is) with its `CustomDocuments` cache, and `CustomScreenChrome`, which builds the
window chrome, the source overlay and the save dialog over all of them. The generic client (`src/client/shell.cpp`)
wires them; the LIMS sample screen proves the whole flow in a GUI test, with no LIMS code for it.

**Tech Stack:** C++23, glaze (JSON), Catch2, CMake + Ninja, clang (local), GCC 15 and MSVC in CI; RecordingBackend,
Qt Quick and the terminal frontend for the backend contract.

Spec: `docs/superpowers/specs/2026-11-18-custom-screens-design.md`. This is plan 2 of 3. Plan 1 (the server) is merged
on this branch: `include/morph/ui/custom_screens.hpp`, `docs/spec/ui/custom_screens.md`. Plan 3 (the assistant,
`propose_screen`) builds on the names this plan produces: `CustomScreenChrome`, `DocumentScreenHost::mountDraft`.

## Global Constraints

- Public symbols carry complete Doxygen `@brief`/`@param`/`@tparam`/`@return`. The Docs workflow runs with
  `WARN_AS_ERROR = FAIL_ON_WARNINGS`, and clang's `-Wdocumentation` catches a misplaced `@param` that GCC misses.
  Check a header through a translation unit that includes it.
- Comments and docs state what the code does now and why. No history, no issue numbers, no commit hashes (AGENTS.md).
- Never use `slots`, `signals` or `emit` as identifiers under `include/morph`. They break only Qt translation units.
  Grep the added lines (Task 13) and build every target, Qt Quick included.
- A dependent default template argument needs `typename` (MSVC C4346). This plan adds no such template. Keep it that
  way.
- No `auto*` or `std::expected` fix-its from clang-tidy accepted as offered: write the types out (they pass on
  libstdc++ and break MSVC).
- Every compile (cmake configure, cmake build, ninja, a g++ sweep) runs under the lane's build lock with `-j5`:
  `flock "$LOCK" <command>`. `$LOCK` is the slot the dispatcher assigned:
  `/tmp/claude-1000/-home-yaraslau-repo-morph/843fe608-d0d7-4f4a-ad5a-4e74db79266e/scratchpad/morph-build.lock` or
  `…/morph-build-2.lock`. Test binaries run as `flock "$LOCK" timeout 300 <binary> <tags>`. clang-tidy (at most 3
  files at a time) and `ctest -j6` run without the lock. Hold the lock for one command at a time, never across edits.
- Builds and test runs are foreground commands. A heavy translation unit can take minutes: give the tool call a
  timeout near its 600 s ceiling rather than a short cap that restarts the compile forever.
- Let `CompileCache.cmake` pick fastcache-cc. Never set `CMAKE_CXX_COMPILER_LAUNCHER=ccache`.
- Build directory for this worktree: `build/clang-debug`, configured in Task 0 with Qt Quick, the terminal frontend
  and the `lims` rung, so the backend contract and the LIMS proof build here. Binaries:
  `build/clang-debug/tests/morph_tests`, `build/clang-debug/tests/client/morph_client_tests`,
  `build/clang-debug/tests/qt_quick/morph_qt_quick_tests`, `build/clang-debug/tests/tui/morph_tui_tests`, and the
  LIMS screen tests at `$(find build/clang-debug -name ladder_lims_screen_tests -type f)`. Qt Quick binaries run with
  `QT_QPA_PLATFORM=offscreen`.
- After restoring a hand-mutated file from a backup, `touch` it, or ninja keeps the mutant object.
- Verify rather than assert: every test names, in a comment above it, the mutation that makes it fail. Each task's
  mutation step applies that mutation, sees the test fail, and restores the file. A test that still passes with its
  feature removed is not evidence: fix the test.
- `examples/` is a detector, not a product (AGENTS.md): keep it building and its tests green, fix an example defect in
  place, and do not file tickets against it. The LIMS proof adds no LIMS code for custom screens.
- Names copied from the spec and plan 1: ids start `custom:` (`kCustomScreenIdPrefix`); the model and the record are
  `morph.customScreens` (`kCustomScreensModel`, `kCustomScreenRecord`); the action ids are `kCustomScreensList`,
  `kCustomScreensGet`, `kCustomScreensCheck`, `kCustomScreensSave`.
- Discovery: any refusal of the `morph.customScreens` registration or of `list` means custom screens are off. Every
  refusal whose text does not contain `unknown model type` is logged at warn, so a misconfiguration is reported, not
  hidden.
- The chrome's words ("Draft", "Save as…", "Source", "Open as", …) are English literals in this plan; resolving them
  through a message catalogue is recorded under "Not yet" in `docs/spec/ui/custom_screens.md` (Task 12).

## File Structure

| File | Responsibility |
|---|---|
| `include/morph/ui/json_source.hpp`, `src/ui/json_source.cpp` (create) | `prettyJson`, `pointerOffset`, `lineAt` |
| `include/morph/ui/backend.hpp` (modify) | `TextInputWidget::setCursor` |
| `include/morph/ui/mount.hpp` (modify) | `detail::Mounter::placeCursor`, `Mounted::placeCursor` |
| `include/morph/ui/testing/recording_backend.hpp`, `src/qt_quick/kinds.hpp`, `src/qt_quick/fields.cpp`, `src/tui/field_widgets.hpp`, `src/tui/field_widgets.cpp` (modify) | `setCursor` on every backend |
| `include/morph/ui/testing/backend_conformance.hpp` (modify) | The conformance case for `setCursor` |
| `include/morph/ui/host.hpp`, `src/ui/host.cpp` (modify) | `mountDraft`, `discardDraft`, `endDraft`, `drafted`, `screenIdOf`, `paramsOf`; `dirty` while drafted; `remount` documented for drafts |
| `include/morph/ui/dev_reload.hpp` (modify) | A reload leaves a drafted screen mounted |
| `include/morph/ui/workspace_model.hpp` (modify) | `WorkspaceOptions::chrome`, `WorkspaceModel::openInPlace` |
| `include/morph/ui/workspace.hpp`, `src/ui/workspace.cpp` (modify) | The chrome mounted at the top of each page |
| `include/morph/ui/navigator.hpp`, `src/ui/navigator.cpp` (modify) | `NavigatorCustomEntry`, `Navigator::setCustom`, the Custom section in the sidebar and the menu |
| `include/morph/ui/custom_screen_sync.hpp`, `src/ui/custom_screen_sync.cpp` (create) | `isCustomScreenId`, `routeScreen`, `CustomDocuments`, `CustomScreensOffer`, `CustomScreenSync` |
| `include/morph/ui/custom_screen_chrome.hpp`, `src/ui/custom_screen_chrome.cpp` (create) | `CustomScreenChromeOptions`, `CustomScreenChrome` |
| `CMakeLists.txt` (modify) | The three new sources and headers |
| `src/client/shell.cpp` (modify) | The generic client's wiring |
| `examples/common/testkit/screen_harness.hpp` (modify) | A simulated server that offers custom screens over a given store |
| `tests/test_ui_json_source.cpp`, `tests/test_ui_host_drafts.cpp`, `tests/test_ui_workspace_in_place.cpp`, `tests/test_ui_navigator_custom.cpp`, `tests/test_ui_custom_screen_sync.cpp`, `tests/test_ui_custom_screen_chrome.cpp` (create) | The framework tests |
| `tests/ui_custom_client_support.hpp` (create) | A bare server, a refusing store, a sync client, the notes bundle and the chrome rig |
| `tests/test_ui_common.cpp` (modify) | `Mounted::placeCursor` |
| `tests/CMakeLists.txt`, `tests/client/CMakeLists.txt` (modify) | The new test files |
| `tests/client/test_custom_screens.cpp` (create) | The generic client's custom screens, in process |
| `examples/lims/screens/tests/test_lims_custom_screens.cpp` (create) | The proof on the LIMS sample screen |
| `docs/spec/ui/host.md`, `docs/spec/ui/interpreter.md`, `docs/spec/ui/service.md`, `docs/spec/ui/workspace.md`, `docs/spec/ui/backend_contract.md`, `docs/spec/ui/custom_screens.md`, `docs/spec/client/client.md`, `docs/spec/README.md`, `CHANGELOG.md` (modify) | The specs this changes |

---

### Task 0: Configure the worktree build

**Files:** none.

- [ ] **Step 1: Configure with Qt Quick, the terminal frontend and the `lims` rung**

Run:
```bash
cd /home/yaraslau/repo/morph-master
flock "$LOCK" cmake --preset clang-debug -DMORPH_BUILD_QT=ON -DMORPH_BUILD_QT_QUICK=ON -DMORPH_BUILD_TUI=ON \
  -DMORPH_BUILD_LADDER=ON -DMORPH_LADDER_RUNGS=lims
```
Expected: `-- Build files have been written to: …/build/clang-debug`, and `grep -E 'MORPH_BUILD_(QT_QUICK|TUI):BOOL=ON'
build/clang-debug/CMakeCache.txt` prints both lines.

- [ ] **Step 2: Build the test binaries once, so later steps measure only this plan's changes**

Run: `flock "$LOCK" cmake --build build/clang-debug --target morph_tests morph_client_tests morph_qt_quick_tests morph_tui_tests ladder_lims_screen_tests -j5`
Expected: ends without errors. Then `find build/clang-debug -name ladder_lims_screen_tests -type f` prints one path.

---
### Task 1: The source text — pretty JSON, a pointer's place, its line

**Files:**
- Create: `include/morph/ui/json_source.hpp`, `src/ui/json_source.cpp`
- Modify: `CMakeLists.txt` (`add_library(morph …)` after `src/ui/custom_screens.cpp`; `FILE_SET HEADERS` after
  `include/morph/ui/custom_screens.hpp`)
- Test: `tests/test_ui_json_source.cpp`; `tests/CMakeLists.txt` (after `test_ui_custom_screens.cpp`)

**Interfaces:**
- Produces:
  - `std::string morph::ui::prettyJson(std::string_view text)`
  - `std::optional<std::size_t> morph::ui::pointerOffset(std::string_view text, std::string_view pointer)`
  - `std::size_t morph::ui::lineAt(std::string_view text, std::size_t offset)`

- [ ] **Step 1: Write the failing tests** (`tests/test_ui_json_source.cpp`)

```cpp
// SPDX-License-Identifier: Apache-2.0
//
// The source view's text (docs/spec/ui/custom_screens.md, "The client"): a document pretty-printed, and the place a
// refusal's JSON Pointer names in text a person may have edited, so the cursor can move there.

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <morph/ui/json_source.hpp>
#include <morph/ui/value.hpp>
#include <optional>
#include <string>
#include <string_view>

namespace {

/// A document as a person might have left it: members on lines of their own, two names that need escaping.
constexpr std::string_view kEdited = "{\n"
                                     "  \"id\": \"a\",\n"
                                     "  \"view\": {\n"
                                     "    \"children\": [\n"
                                     "      {\"kind\": \"text\"},\n"
                                     "      {\"kind\": \"button\", \"a/b\": 1, \"t~n\": [2, {\"x\": \"\\\"q\\\"\"}]}\n"
                                     "    ]\n"
                                     "  }\n"
                                     "}";

/// The text from where @p pointer is found; fails the test when it is not found.
std::string_view at(std::string_view pointer) {
    std::optional<std::size_t> const found = morph::ui::pointerOffset(kEdited, pointer);
    REQUIRE(found.has_value());
    return kEdited.substr(*found);
}

}  // namespace

// Mutation: answer the text unchanged (no line breaks are added).
TEST_CASE("prettyJson writes one member per line in their order, and keeps what is not JSON", "[ui][json-source]") {
    std::string const pretty =
        morph::ui::prettyJson(R"({"vocab":"ui/1","id":"a","view":{"kind":"text","text":"x"}})");
    CHECK(std::ranges::count(pretty, '\n') >= 5);
    CHECK(pretty.find("\"vocab\"") < pretty.find("\"id\""));
    CHECK(pretty.find("\"id\"") < pretty.find("\"view\""));
    CHECK(morph::ui::parseJson(pretty).has_value());
    CHECK(morph::ui::prettyJson("{not json") == "{not json");
}

// Mutations: answer a member's value instead of its name (`/view` lands on `{`); drop the `~1` unescape (`a~1b` is
// not found); count array places from 1 (`/view/children/1` lands on the text element).
TEST_CASE("pointerOffset finds a member's name, an element, and escaped tokens in edited text", "[ui][json-source]") {
    CHECK(at("/view").starts_with("\"view\""));
    CHECK(at("/view/children/1").starts_with("{\"kind\": \"button\""));
    CHECK(at("/view/children/1/kind").starts_with("\"kind\": \"button\""));
    CHECK(at("/view/children/1/a~1b").starts_with("\"a/b\""));
    CHECK(at("/view/children/1/t~0n/1/x").starts_with("\"x\""));
    CHECK(morph::ui::pointerOffset(kEdited, "") == std::optional<std::size_t>{0});
    CHECK_FALSE(morph::ui::pointerOffset(kEdited, "/view/children/2").has_value());
    CHECK_FALSE(morph::ui::pointerOffset(kEdited, "/nope").has_value());
    CHECK_FALSE(morph::ui::pointerOffset(kEdited, "view").has_value());
    CHECK_FALSE(morph::ui::pointerOffset(kEdited, "/view/children/01").has_value());
    CHECK_FALSE(morph::ui::pointerOffset("{\"a\": [1, 2", "/a/5").has_value());
}

// Mutation: drop the depth bound (the 70-deep pointer is found).
TEST_CASE("pointerOffset stops at kMaxJsonDepth", "[ui][json-source]") {
    std::string const deep = std::string(70, '[') + "1" + std::string(70, ']');
    std::string shallow;
    std::string tooDeep;
    for (int level = 0; level < 70; ++level) {
        tooDeep += "/0";
        if (level < 10) {
            shallow += "/0";
        }
    }
    CHECK(morph::ui::pointerOffset(deep, shallow) == std::optional<std::size_t>{10});
    CHECK_FALSE(morph::ui::pointerOffset(deep, tooDeep).has_value());
}

// Mutation: count lines from 0 (the button's `kind` is on line 5).
TEST_CASE("lineAt counts lines from 1", "[ui][json-source]") {
    std::optional<std::size_t> const kind = morph::ui::pointerOffset(kEdited, "/view/children/1/kind");
    REQUIRE(kind.has_value());
    CHECK(morph::ui::lineAt(kEdited, *kind) == 6);
    CHECK(morph::ui::lineAt(kEdited, 0) == 1);
    CHECK(morph::ui::lineAt(kEdited, 100000) == 9);
}
```

Add `test_ui_json_source.cpp` to `tests/CMakeLists.txt`, after `test_ui_custom_screens.cpp`.

- [ ] **Step 2: Run them to verify they fail**

Run: `flock "$LOCK" cmake --build build/clang-debug --target morph_tests -j5`
Expected: compile error, `'morph/ui/json_source.hpp' file not found`.

- [ ] **Step 3: Write the header** (`include/morph/ui/json_source.hpp`)

```cpp
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstddef>
#include <morph/export.hpp>
#include <optional>
#include <string>
#include <string_view>

/// @file
/// @brief A document's JSON text as the source view shows it: pretty-printed, and the place a JSON Pointer names in
///        it, so a refusal moves the cursor to the part it is about.
///
/// Specified in `docs/spec/ui/custom_screens.md`, "The client".

namespace morph::ui {

/// @brief @p text pretty-printed: one member or element per line, members in their order.
/// @param text JSON text.
/// @return The pretty text; @p text unchanged when it is not JSON, so a hand-edited draft that does not parse is shown
///         as the person left it.
[[nodiscard]] MORPH_API std::string prettyJson(std::string_view text);

/// @brief Where the part a JSON Pointer names starts in @p text.
///
/// The text is scanned, not parsed into a value, so the offset is into the text as it is, with its own spacing and
/// member order. For a member, the offset is that of its name's opening quote, where a person looks for it; for an
/// array element, that of the element's first byte; for the empty pointer, that of the document's first byte.
/// @param text    JSON text.
/// @param pointer An RFC 6901 pointer; `~0` and `~1` are unescaped.
/// @return The byte offset, or `std::nullopt` when the pointer names nothing in @p text, @p text is not JSON up to that
///         part, the pointer is not one, or it reaches deeper than `kMaxJsonDepth`.
[[nodiscard]] MORPH_API std::optional<std::size_t> pointerOffset(std::string_view text, std::string_view pointer);

/// @brief The line @p offset is on.
/// @param text   The text.
/// @param offset A byte offset into it; past its end counts as its end.
/// @return The line, counting from 1.
[[nodiscard]] MORPH_API std::size_t lineAt(std::string_view text, std::size_t offset);

}  // namespace morph::ui
```

- [ ] **Step 4: Write the implementation** (`src/ui/json_source.cpp`)

```cpp
// SPDX-License-Identifier: Apache-2.0

#include <algorithm>
#include <charconv>
#include <cstddef>
#include <glaze/glaze.hpp>
#include <morph/ui/json_source.hpp>
#include <morph/ui/value.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace morph::ui {

namespace {

constexpr bool isSpace(char byte) noexcept { return byte == ' ' || byte == '\n' || byte == '\r' || byte == '\t'; }

std::size_t skipSpace(std::string_view text, std::size_t at) {
    while (at < text.size() && isSpace(text[at])) {
        ++at;
    }
    return at;
}

char unescaped(char escape) noexcept {
    switch (escape) {
        case 'n':
            return '\n';
        case 't':
            return '\t';
        case 'r':
            return '\r';
        case 'b':
            return '\b';
        case 'f':
            return '\f';
        default:
            return escape;  // `"`, `\` and `/` stand for themselves
    }
}

// A string at its opening quote: the offset past its closing quote. Its content is appended to @p out when one is
// given, escapes decoded except `\u`, which is kept as written: the vocabulary's member names are ASCII.
std::optional<std::size_t> scanString(std::string_view text, std::size_t at, std::string* out) {
    if (at >= text.size() || text[at] != '"') {
        return std::nullopt;
    }
    for (++at; at < text.size();) {
        char const byte = text[at];
        if (byte == '"') {
            return at + 1;
        }
        if (byte != '\\') {
            if (out != nullptr) {
                out->push_back(byte);
            }
            ++at;
            continue;
        }
        if (at + 1 >= text.size()) {
            return std::nullopt;
        }
        std::size_t const length = text[at + 1] == 'u' ? 6 : 2;
        if (at + length > text.size()) {
            return std::nullopt;
        }
        if (out != nullptr) {
            if (length == 6) {
                out->append(text.substr(at, length));
            } else {
                out->push_back(unescaped(text[at + 1]));
            }
        }
        at += length;
    }
    return std::nullopt;
}

std::optional<std::size_t> skipValue(std::string_view text, std::size_t at, std::size_t depth);

// The container at @p at, skipped: the offset past its closing bracket.
// NOLINTNEXTLINE(misc-no-recursion): a container's values, bounded by kMaxJsonDepth in skipValue.
std::optional<std::size_t> skipContainer(std::string_view text, std::size_t at, std::size_t depth) {
    char const close = text[at] == '{' ? '}' : ']';
    at = skipSpace(text, at + 1);
    if (at < text.size() && text[at] == close) {
        return at + 1;
    }
    while (at < text.size()) {
        if (close == '}') {
            std::optional<std::size_t> const name = scanString(text, at, nullptr);
            if (!name) {
                return std::nullopt;
            }
            at = skipSpace(text, *name);
            if (at >= text.size() || text[at] != ':') {
                return std::nullopt;
            }
            at = skipSpace(text, at + 1);
        }
        std::optional<std::size_t> const value = skipValue(text, at, depth + 1);
        if (!value) {
            return std::nullopt;
        }
        at = skipSpace(text, *value);
        if (at < text.size() && text[at] == ',') {
            at = skipSpace(text, at + 1);
            continue;
        }
        if (at < text.size() && text[at] == close) {
            return at + 1;
        }
        return std::nullopt;
    }
    return std::nullopt;
}

// Any value at @p at, skipped: the offset past it.
// NOLINTNEXTLINE(misc-no-recursion): a container's values, bounded by kMaxJsonDepth.
std::optional<std::size_t> skipValue(std::string_view text, std::size_t at, std::size_t depth) {
    if (depth > kMaxJsonDepth || at >= text.size()) {
        return std::nullopt;
    }
    char const first = text[at];
    if (first == '"') {
        return scanString(text, at, nullptr);
    }
    if (first == '{' || first == '[') {
        return skipContainer(text, at, depth);
    }
    // A number or a literal runs to the next delimiter.
    std::size_t end = at;
    while (end < text.size() && !isSpace(text[end]) && text[end] != ',' && text[end] != '}' && text[end] != ']') {
        ++end;
    }
    return end == at ? std::nullopt : std::optional<std::size_t>{end};
}

// The pointer's reference tokens, unescaped; nothing for text that is not a pointer.
std::optional<std::vector<std::string>> tokensOf(std::string_view pointer) {
    std::vector<std::string> tokens;
    if (pointer.empty()) {
        return tokens;
    }
    if (pointer.front() != '/') {
        return std::nullopt;
    }
    std::string token;
    for (std::size_t at = 1; at <= pointer.size(); ++at) {
        if (at == pointer.size() || pointer[at] == '/') {
            tokens.push_back(std::move(token));
            token.clear();
            continue;
        }
        if (pointer[at] == '~') {
            if (at + 1 >= pointer.size() || (pointer[at + 1] != '0' && pointer[at + 1] != '1')) {
                return std::nullopt;
            }
            token.push_back(pointer[at + 1] == '0' ? '~' : '/');
            ++at;
            continue;
        }
        token.push_back(pointer[at]);
    }
    return tokens;
}

// An array index token: digits, with no leading zero but for `0` itself.
std::optional<std::size_t> indexOf(std::string const& token) {
    if (token.empty() || (token.size() > 1 && token.front() == '0')) {
        return std::nullopt;
    }
    std::size_t index = 0;
    auto const [end, error] = std::from_chars(token.data(), token.data() + token.size(), index);
    if (error != std::errc{} || end != token.data() + token.size()) {
        return std::nullopt;
    }
    return index;
}

}  // namespace

std::string prettyJson(std::string_view text) {
    if (!parseJson(text)) {
        return std::string{text};
    }
    std::string out;
    glz::prettify_json(text, out);
    return out;
}

std::optional<std::size_t> pointerOffset(std::string_view text, std::string_view pointer) {
    std::optional<std::vector<std::string>> const tokens = tokensOf(pointer);
    if (!tokens) {
        return std::nullopt;
    }
    std::size_t value = skipSpace(text, 0);  // where the value reached so far starts
    std::size_t shown = value;               // where a person sees it: its member's name, or the value itself
    for (std::size_t depth = 0; depth < tokens->size(); ++depth) {
        if (depth >= kMaxJsonDepth || value >= text.size() || (text[value] != '{' && text[value] != '[')) {
            return std::nullopt;
        }
        std::string const& token = tokens->at(depth);
        bool const object = text[value] == '{';
        std::optional<std::size_t> const wanted = object ? std::nullopt : indexOf(token);
        if (!object && !wanted) {
            return std::nullopt;
        }
        std::size_t at = skipSpace(text, value + 1);
        std::size_t place = 0;
        bool found = false;
        while (at < text.size() && text[at] != (object ? '}' : ']')) {
            std::size_t const entry = at;
            bool matches = false;
            if (object) {
                std::string name;
                std::optional<std::size_t> const after = scanString(text, at, &name);
                if (!after) {
                    return std::nullopt;
                }
                at = skipSpace(text, *after);
                if (at >= text.size() || text[at] != ':') {
                    return std::nullopt;
                }
                at = skipSpace(text, at + 1);
                matches = name == token;
            } else {
                matches = place == *wanted;
            }
            if (matches) {
                value = at;
                shown = entry;
                found = true;
                break;
            }
            std::optional<std::size_t> const skipped = skipValue(text, at, depth + 1);
            if (!skipped) {
                return std::nullopt;
            }
            at = skipSpace(text, *skipped);
            if (at < text.size() && text[at] == ',') {
                at = skipSpace(text, at + 1);
            }
            ++place;
        }
        if (!found) {
            return std::nullopt;
        }
    }
    return shown;
}

std::size_t lineAt(std::string_view text, std::size_t offset) {
    std::string_view const before = text.substr(0, std::min(offset, text.size()));
    return 1 + static_cast<std::size_t>(std::ranges::count(before, '\n'));
}

}  // namespace morph::ui
```

Add `src/ui/json_source.cpp` to `add_library(morph …)` after `src/ui/custom_screens.cpp`, and
`include/morph/ui/json_source.hpp` to `FILE_SET HEADERS` after `include/morph/ui/custom_screens.hpp`.

- [ ] **Step 5: Run the tests**

Run: `flock "$LOCK" cmake --build build/clang-debug --target morph_tests -j5 && flock "$LOCK" timeout 300 build/clang-debug/tests/morph_tests "[json-source]"`
Expected: `All tests passed`.

- [ ] **Step 6: Mutation check.** Apply each mutation named above the four tests in turn (back up the file, edit, build,
  run `[json-source]`, see the named test fail, restore, `touch src/ui/json_source.cpp`).

- [ ] **Step 7: Commit**

```bash
git add include/morph/ui/json_source.hpp src/ui/json_source.cpp tests/test_ui_json_source.cpp CMakeLists.txt tests/CMakeLists.txt
git commit -m "ui: the source view's text — pretty JSON, a pointer's place and its line"
```

---

### Task 2: A text cursor any backend can move

**Files:**
- Modify: `include/morph/ui/backend.hpp` (`class TextInputWidget`, after `setText`)
- Modify: `include/morph/ui/testing/recording_backend.hpp` (`class FakeTextInput`)
- Modify: `src/qt_quick/kinds.hpp` (`class TextInputImpl`), `src/qt_quick/fields.cpp`
- Modify: `src/tui/field_widgets.hpp` (`class TextInputImpl`), `src/tui/field_widgets.cpp`
- Modify: `include/morph/ui/mount.hpp` (`detail::Mounter`, after `focus`; `class Mounted`, after `focus`)
- Modify: `include/morph/ui/testing/backend_conformance.hpp` (a new case and its entry in `conformanceCases()`)
- Modify: `docs/spec/ui/backend_contract.md` (the `TextInputWidget` row and the RecordingBackend's "Text cursor")
- Test: `tests/test_ui_common.cpp`; the conformance suites `tests/test_ui_conformance.cpp`,
  `tests/qt_quick/test_qt_quick_conformance.cpp`, `tests/tui/test_tui_conformance.cpp` run the new case

**Interfaces:**
- Produces:
  - `virtual void morph::ui::TextInputWidget::setCursor(std::size_t offset) = 0`
  - `bool morph::ui::Mounted::placeCursor(std::string_view focusId, std::size_t offset)`

- [ ] **Step 1: Write the failing tests.** Append to `tests/test_ui_common.cpp`:

```cpp
// Mutations: skip setCursor in Mounter::placeCursor (the cursor stays at the end of the text); answer true for a
// widget that is not a text input.
TEST_CASE("ui::Mounted::placeCursor moves a named text input's cursor and focuses it", "[ui]") {
    Owner owner;
    Runtime runtime{owner};
    RecordingBackend backend;
    ui::Mounted view{runtime, backend,
                     ui::column({.children = {ui::textInput({.value = std::string{"ab\ncd"},
                                                             .mode = ui::TextInputMode::Multiline,
                                                             .common = {.testId = "source", .focusId = "source"}}),
                                              ui::button({.label = "b", .common = {.testId = "b", .focusId = "b"}})}})};
    int const field = *backend.find("TextInput", "testId", "source");
    REQUIRE(backend.cursor(field) == 5);
    CHECK(view.placeCursor("source", 3));
    CHECK(backend.cursor(field) == 3);
    CHECK(backend.focused() == field);
    CHECK_FALSE(view.placeCursor("b", 1));
    CHECK_FALSE(view.placeCursor("nowhere", 1));
}
```

In `include/morph/ui/testing/backend_conformance.hpp`, after `unchangedTextKeepsCursor`, add:

```cpp
/// @brief Case: `setCursor` moves a field's cursor to a byte offset, clamps it to the text's end, and calls no
///        handler.
/// @param probe The backend under test.
/// @return `nullopt`, or what went wrong.
[[nodiscard]] inline std::optional<std::string> setCursorMovesTheCursor(ConformanceProbe& probe) {
    std::unique_ptr<TextInputWidget> const field = probe.backend().createTextInput(nullptr, TextInputMode::Multiline);
    int changes = 0;
    field->setOnChange([&changes](std::string const& /*text*/) { ++changes; });
    field->setText("ab\ncd");
    probe.settle();
    Checks checks;
    field->setCursor(3);
    probe.settle();
    checks.that(probe.cursorOf(*field) == 3,
                "setCursor(3) left the cursor at " + std::to_string(probe.cursorOf(*field)));
    field->setCursor(99);
    probe.settle();
    checks.that(probe.cursorOf(*field) == 5,
                "setCursor past the end left the cursor at " + std::to_string(probe.cursorOf(*field)));
    checks.that(changes == 0, "setCursor called onChange");
    return checks.result();
}
```

and in `conformanceCases()`, change `std::array<ConformanceCase, 56>` to `std::array<ConformanceCase, 57>` and add,
after the entry for `detail::unchangedTextKeepsCursor`:

```cpp
        {.name = "setCursor moves a field's cursor and calls no handler", .run = detail::setCursorMovesTheCursor},
```

- [ ] **Step 2: Run them to verify they fail**

Run: `flock "$LOCK" cmake --build build/clang-debug --target morph_tests -j5`
Expected: compile errors, `no member named 'placeCursor' in 'morph::ui::Mounted'` and `no member named 'setCursor'`.

- [ ] **Step 3: Declare it on the widget.** In `include/morph/ui/backend.hpp`, `class TextInputWidget`, after `setText`:

```cpp
    /// @brief Moves the text cursor to @p offset, a byte offset into the text the field shows at the start of a
    ///        character, clamped to the text's end. Calls no handler and does not move keyboard focus.
    /// @param offset The byte offset.
    virtual void setCursor(std::size_t offset) = 0;
```

- [ ] **Step 4: Implement it on every backend.**

`include/morph/ui/testing/recording_backend.hpp`, `class FakeTextInput`, after `moveCursorByUser`:

```cpp
    /// @brief Moves the cursor to @p offset, clamped to the text's size; nothing is logged, as for a user's move.
    /// @param offset A byte offset into the text shown.
    void setCursor(std::size_t offset) override { _cursor = std::min(offset, _shown.size()); }
```

`src/qt_quick/kinds.hpp`, `class TextInputImpl`, after `setText`:

```cpp
    void setCursor(std::size_t offset) override { moveCursor(offset); }
```

(`moveCursor` already converts the byte offset to the control's `cursorPosition` in UTF-16 units and clamps it.)

`src/tui/field_widgets.hpp`, `class TextInputImpl`, after `void setText(std::string_view text) override;`:

```cpp
    void setCursor(std::size_t offset) override;
```

`src/tui/field_widgets.cpp`, after `TextInputImpl::setText`:

```cpp
void TextInputImpl::setCursor(std::size_t offset) {
    _field->setCursor(std::min(offset, _field->text().size()));
    refresh();
}
```

Add `#include <algorithm>` to `src/tui/field_widgets.cpp` if it is not there.

- [ ] **Step 5: Reach it through a mount.** In `include/morph/ui/mount.hpp`, `class Mounter`, after `focus`:

```cpp
    /// @brief Moves the text cursor of the mounted text input whose `focusId` is @p focusId to @p offset, then
    ///        focuses it.
    ///
    /// Of several such widgets, the one mounted first is taken.
    /// @param focusId The name.
    /// @param offset  A byte offset into the text it shows, at the start of a character.
    /// @return Whether a mounted text input has that name.
    bool placeCursor(std::string_view focusId, std::size_t offset) {
        auto const found = _focusable.lower_bound(focusId);
        if (found == _focusable.end() || found->first != focusId) {
            return false;
        }
        auto* const field = dynamic_cast<TextInputWidget*>(found->second);
        if (field == nullptr) {
            return false;
        }
        field->setCursor(offset);
        try {
            field->focus();
        } catch (...) {  // NOLINT(bugprone-empty-catch)
            // Focus is a request: a backend that cannot move it leaves it where it was.
        }
        return true;
    }
```

and in `class Mounted`, after `focus`:

```cpp
    /// @brief Moves the text cursor of the mounted text input whose `Common::focusId` is @p focusId, at any depth, to
    ///        @p offset, and focuses it: how a source view puts the cursor where a refusal points.
    /// @param focusId The name.
    /// @param offset  A byte offset into the text it shows, at the start of a character.
    /// @return Whether a mounted text input has that name.
    bool placeCursor(std::string_view focusId, std::size_t offset) { return _mounter.placeCursor(focusId, offset); }
```

- [ ] **Step 6: Run the tests and every conformance suite**

Run:
```bash
flock "$LOCK" cmake --build build/clang-debug --target morph_tests morph_qt_quick_tests morph_tui_tests -j5
flock "$LOCK" timeout 300 build/clang-debug/tests/morph_tests "[ui]"
flock "$LOCK" timeout 300 env QT_QPA_PLATFORM=offscreen build/clang-debug/tests/qt_quick/morph_qt_quick_tests
flock "$LOCK" timeout 300 build/clang-debug/tests/tui/morph_tui_tests
```
Expected: `All tests passed` from each.

- [ ] **Step 7: Mutation check.** Apply the two mutations above the `placeCursor` test, and make each backend's
  `setCursor` do nothing in turn (RecordingBackend, Qt Quick, terminal): the conformance case fails on that backend.
  Restore and `touch` each file.

- [ ] **Step 8: Document it.** In `docs/spec/ui/backend_contract.md`, in the `TextInputWidget` row of the
  Interfaces table, after `` `setText` (with the text shown, it keeps the cursor), `` insert
  `` `setCursor(offset)` (moves the cursor to a byte offset at the start of a character, clamped to the text's end; no handler runs and focus stays), ``.
  In the RecordingBackend's "Text cursor" item, after `moveCursor(id, position)` moves it (clamped to the text),`
  insert `` `setCursor` moves it the same way without a log entry, ``.

- [ ] **Step 9: Commit**

```bash
git add include/morph/ui/backend.hpp include/morph/ui/mount.hpp include/morph/ui/testing/recording_backend.hpp \
  include/morph/ui/testing/backend_conformance.hpp src/qt_quick/kinds.hpp src/tui/field_widgets.hpp \
  src/tui/field_widgets.cpp tests/test_ui_common.cpp docs/spec/ui/backend_contract.md
git commit -m "ui: a text input's cursor moves on request, on every backend"
```

---
### Task 3: Drafts on the document host

**Files:**
- Modify: `include/morph/ui/host.hpp` (`DocumentScreenHost`: the `remount` doc comment, new members after `remount`;
  `Handle`: `drafted`, `setDrafted`; `Handle::State`: the `drafted` signal and `dirty`)
- Modify: `src/ui/host.cpp` (after `DocumentScreenHost::remount`)
- Modify: `include/morph/ui/dev_reload.hpp` (`reloadDocuments`)
- Modify: `docs/spec/ui/host.md`, `docs/spec/ui/interpreter.md`, `docs/spec/ui/service.md`
- Test: `tests/test_ui_host_drafts.cpp`; `tests/CMakeLists.txt`

**Interfaces:**
- Produces:
  - `std::expected<void, MountError> DocumentScreenHost::mountDraft(ScreenHandle& handle, std::shared_ptr<Document const> draft)`
  - `std::expected<void, MountError> DocumentScreenHost::discardDraft(ScreenHandle& handle)`
  - `void DocumentScreenHost::endDraft(ScreenHandle& handle)`
  - `static bool DocumentScreenHost::drafted(ScreenHandle const& handle)` (a tracked read)
  - `static std::optional<std::string> DocumentScreenHost::screenIdOf(ScreenHandle const& handle)`
  - `static std::optional<ParamMap> DocumentScreenHost::paramsOf(ScreenHandle const& handle)`

- [ ] **Step 1: Write the failing tests** (`tests/test_ui_host_drafts.cpp`)

```cpp
// SPDX-License-Identifier: Apache-2.0
//
// Drafts (docs/spec/ui/host.md, "Drafts"): a document mounted in place of a screen through remount, keeping the
// screen's id, params and state by name; dirty while it is up, so a close asks the guard; discarded back to the
// screen's own document; left alone by a development reload.

#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <functional>
#include <memory>
#include <morph/ui/bundle.hpp>
#include <morph/ui/dev_reload.hpp>
#include <morph/ui/host.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "ui_interpreter_support.hpp"

using morph::ui::CloseChoice;
using morph::ui::Document;
using morph::ui::DocumentMap;
using morph::ui::DocumentScreenHost;
using morph::ui::DocumentScreenHostOptions;
using morph::ui::ParamMap;
using morph::ui::ScreenHandle;
using morph::ui::Value;
using morph::ui::testing::Harness;
using morph::ui::testing::loadOrFail;

namespace {

/// A note: its body is state a button changes.
constexpr std::string_view kNote = R"({
  "vocab": "ui/1", "id": "note", "params": { "noteId": "int" }, "identity": { "ref": "noteId" }, "title": "Note",
  "state": { "body": { "type": "string", "init": "first" } },
  "view": { "kind": "column", "children": [
    { "kind": "text", "testId": "heading", "text": "Note" },
    { "kind": "text", "testId": "body", "text": { "ref": "body" } },
    { "kind": "button", "testId": "change", "label": "change",
      "onClick": [ { "set": "body", "value": "changed" } ] } ] } })";

/// A draft of the note: another heading, the same state.
constexpr std::string_view kNoteDraft = R"({
  "vocab": "ui/1", "id": "note", "params": { "noteId": "int" }, "identity": { "ref": "noteId" }, "title": "Note",
  "state": { "body": { "type": "string", "init": "draft" } },
  "view": { "kind": "column", "children": [
    { "kind": "text", "testId": "heading", "text": "Drafted" },
    { "kind": "text", "testId": "body", "text": { "ref": "body" } } ] } })";

/// A draft that needs a param the screen was not given: it loads, and the interpreter refuses the mount.
constexpr std::string_view kNoteNeedsOwner = R"({
  "vocab": "ui/1", "id": "note", "params": { "noteId": "int", "owner": "string" },
  "view": { "kind": "text", "testId": "heading", "text": "needs an owner" } })";

/// A document of another screen.
constexpr std::string_view kOther = R"({
  "vocab": "ui/1", "id": "other", "params": { "noteId": "int" },
  "view": { "kind": "text", "testId": "heading", "text": "Other" } })";

/// The note as a development reload changes it.
constexpr std::string_view kNoteReloaded = R"({
  "vocab": "ui/1", "id": "note", "params": { "noteId": "int" }, "identity": { "ref": "noteId" }, "title": "Note",
  "state": { "body": { "type": "string", "init": "first" } },
  "view": { "kind": "column", "children": [
    { "kind": "text", "testId": "heading", "text": "Note v2" },
    { "kind": "text", "testId": "body", "text": { "ref": "body" } } ] } })";

std::shared_ptr<Document const> load(std::string_view text) {
    return loadOrFail(text, morph::ui::testing::corpusCatalog());
}

/// A host over a document map, whose close guard records each screen it is asked about and discards it.
class Shell final : public DocumentScreenHost {
public:
    Shell(Harness& harness, DocumentMap& documents, std::vector<std::string>& asked)
        : DocumentScreenHost{harness.session, harness.dispatcher, options(harness, documents, asked)} {}

    void onNavigate(std::string_view /*screenId*/, ParamMap const& /*params*/) override {}

    using DocumentScreenHost::onNavigate;

private:
    static DocumentScreenHostOptions options(Harness& harness, DocumentMap& documents,
                                             std::vector<std::string>& asked) {
        DocumentScreenHostOptions made;
        made.documents = [&documents](std::string_view screenId) -> std::shared_ptr<Document const> {
            auto const found = documents.find(screenId);
            return found == documents.end() ? nullptr : found->second;
        };
        made.backend = &harness.backend;
        made.application = &harness.host;
        made.guard = [&asked](ScreenHandle& screen, std::function<void(CloseChoice)> const& choose) {
            asked.push_back(screen.title().peek());
            choose(CloseChoice::Discard);
        };
        return made;
    }
};

/// The note mounted with id 7, its body changed by the user.
struct Opened {
    explicit Opened(Harness& harnessIn) : harness{&harnessIn}, shell{harnessIn, documents, asked} {
        documents.emplace("note", load(kNote));
        auto mounted = shell.mount("note", ParamMap{{"noteId", Value{std::int64_t{7}}}});
        REQUIRE(mounted.has_value());
        handle = std::move(*mounted);
        harness->backend.click(harness->byTestId("Button", "change"));
        harness->settle();
        REQUIRE(harness->text("body") == "changed");
    }

    Harness* harness;
    DocumentMap documents;
    std::vector<std::string> asked;
    Shell shell;
    std::unique_ptr<ScreenHandle> handle;
};

}  // namespace

// Mutations: leave the handle undrafted after a draft mounts (`dirty` stays false); remount the screen's own document
// on discard without clearing the draft (`dirty` stays true).
TEST_CASE("A draft keeps the screen's state, params and identity, is dirty while up, and Discard restores the screen",
          "[ui][host][drafts]") {
    Harness harness;
    Opened opened{harness};
    ScreenHandle& handle = *opened.handle;
    CHECK_FALSE(handle.dirty().peek());

    REQUIRE(opened.shell.mountDraft(handle, load(kNoteDraft)).has_value());
    harness.settle();
    CHECK(harness.text("heading") == "Drafted");
    CHECK(harness.text("body") == "changed");
    CHECK(DocumentScreenHost::drafted(handle));
    CHECK(handle.dirty().peek());
    CHECK(handle.identity().peek() == Value{std::int64_t{7}});
    CHECK(DocumentScreenHost::screenIdOf(handle) == std::optional<std::string>{"note"});
    CHECK(DocumentScreenHost::paramsOf(handle) ==
          std::optional<ParamMap>{ParamMap{{"noteId", Value{std::int64_t{7}}}}});

    REQUIRE(opened.shell.discardDraft(handle).has_value());
    harness.settle();
    CHECK(harness.text("heading") == "Note");
    CHECK(harness.text("body") == "changed");
    CHECK_FALSE(DocumentScreenHost::drafted(handle));
    CHECK_FALSE(handle.dirty().peek());
    CHECK_FALSE(opened.shell.discardDraft(handle).has_value());
}

// Mutations: mark the handle drafted before the remount (a refused draft leaves it dirty); drop the id check (the other
// screen's document mounts as the draft).
TEST_CASE("A draft the interpreter refuses, or one that names another screen, leaves the screen mounted",
          "[ui][host][drafts]") {
    Harness harness;
    Opened opened{harness};
    REQUIRE_FALSE(opened.shell.mountDraft(*opened.handle, load(kNoteNeedsOwner)).has_value());
    harness.settle();
    CHECK(harness.text("heading") == "Note");
    CHECK_FALSE(opened.handle->dirty().peek());

    auto const other = opened.shell.mountDraft(*opened.handle, load(kOther));
    REQUIRE_FALSE(other.has_value());
    CHECK(other.error().reason.contains("keeps the screen's id"));
    harness.settle();
    CHECK(harness.text("heading") == "Note");
    CHECK_FALSE(DocumentScreenHost::drafted(*opened.handle));
}

// Mutation: read only the document's `dirty` in the handle's `dirty` (the close goes through without the guard).
TEST_CASE("Closing a screen with a draft up asks the close guard first", "[ui][host][drafts]") {
    Harness harness;
    Opened opened{harness};
    REQUIRE(opened.shell.mountDraft(*opened.handle, load(kNoteDraft)).has_value());
    std::optional<bool> closed;
    opened.handle->requestClose([&closed](bool outcome) { closed = outcome; });
    harness.settle();
    CHECK(opened.asked == std::vector<std::string>{"Note"});
    CHECK(closed == std::optional<bool>{true});
}

// Mutation: make endDraft remount the screen's own document (the heading goes back to "Note").
TEST_CASE("Ending a draft keeps what is mounted and is no longer dirty", "[ui][host][drafts]") {
    Harness harness;
    Opened opened{harness};
    REQUIRE(opened.shell.mountDraft(*opened.handle, load(kNoteDraft)).has_value());
    opened.shell.endDraft(*opened.handle);
    harness.settle();
    CHECK(harness.text("heading") == "Drafted");
    CHECK_FALSE(DocumentScreenHost::drafted(*opened.handle));
    CHECK_FALSE(opened.handle->dirty().peek());
}

// Mutation: drop the skip of a drafted screen in reloadDocuments (the draft is replaced by "Note v2").
TEST_CASE("A development reload leaves a draft mounted, and Discard then mounts the reloaded document",
          "[ui][host][drafts][dev-reload]") {
    Harness harness;
    Opened opened{harness};
    REQUIRE(opened.shell.mountDraft(*opened.handle, load(kNoteDraft)).has_value());
    morph::ui::Bundle bundle;
    bundle.applicationId = "dev";
    bundle.documents.emplace("note", std::string{kNoteReloaded});
    std::vector<std::string> const changed{"note"};
    std::vector<morph::ui::MountedScreen> const mounted{
        morph::ui::MountedScreen{.screenId = "note", .handle = opened.handle.get()}};
    auto const outcome = morph::ui::reloadDocuments(
        changed, bundle, morph::ui::LoadOptions{.catalog = morph::ui::testing::corpusCatalog()}, opened.documents,
        opened.shell, mounted);
    harness.settle();
    CHECK(outcome.remounted.empty());
    CHECK(outcome.refused.empty());
    CHECK(harness.text("heading") == "Drafted");

    REQUIRE(opened.shell.discardDraft(*opened.handle).has_value());
    harness.settle();
    CHECK(harness.text("heading") == "Note v2");
}
```

Add `test_ui_host_drafts.cpp` to `tests/CMakeLists.txt`, after `test_ui_json_source.cpp`.

- [ ] **Step 2: Run them to verify they fail**

Run: `flock "$LOCK" cmake --build build/clang-debug --target morph_tests -j5`
Expected: compile error, `no member named 'mountDraft' in 'Shell'`.

- [ ] **Step 3: Declare the draft API.** In `include/morph/ui/host.hpp`, replace the doc comment of `remount` with:

```cpp
    /// @brief Mounts a screen this host mounted again, on another document of the same screen: a development reload
    ///        (spec 5 §12) or a draft (`mountDraft`, `discardDraft`).
    ///
    /// The new screen is mounted with the same params in the same place, and each state of its own scope starts from
    /// the old screen's value of the state of the same name and type; any other state starts from its init. The old
    /// screen is then destroyed, which runs its `onUnmount`, and the handle shows the new one: its `title`, `dirty`
    /// and `identity` follow it, and a pending call stays tied to it. A refused mount keeps the old screen.
    /// @param handle   A handle this host mounted.
    /// @param document The other document of the same screen, loaded.
    /// @return Nothing, or why the old screen was kept: a foreign or closed handle, a close or save in progress, or
    ///         the interpreter's refusal of the new document.
```

After the `remount` declaration, add:

```cpp
    /// @brief Mounts @p draft in place of a screen this host mounted, as `remount` does, and marks the handle as
    ///        showing a draft.
    ///
    /// A draft keeps the screen's id, so the handle's identity, its route and a shell's record of the tab name the
    /// screen, never the draft. While a draft is mounted the handle is `dirty`, so a close asks the close guard first.
    /// A second draft replaces the first.
    /// @param handle A handle this host mounted.
    /// @param draft  The draft, loaded; its `id` is the screen's.
    /// @return Nothing, or why the screen was kept as it was: a draft whose id is not the screen's, or what `remount`
    ///         refuses.
    [[nodiscard]] std::expected<void, MountError> mountDraft(ScreenHandle& handle,
                                                              std::shared_ptr<Document const> draft);

    /// @brief Mounts the screen's own document in place of its draft, as `remount` does: the document `documents`
    ///        answers for the screen's id now, so a development reload since the draft began is seen.
    /// @param handle A handle this host mounted, showing a draft.
    /// @return Nothing, or why the draft was kept: no draft is mounted, `documents` answers nothing for the screen's
    ///         id, or what `remount` refuses.
    [[nodiscard]] std::expected<void, MountError> discardDraft(ScreenHandle& handle);

    /// @brief Ends a draft without remounting: what is mounted stays, and the handle no longer shows a draft. What a
    ///        shell does once the draft was saved.
    /// @param handle A handle this host mounted; a foreign one, or one with no draft, is left as it is.
    void endDraft(ScreenHandle& handle);

    /// @brief Whether a handle this host mounted shows a draft. A tracked read.
    /// @param handle A handle.
    /// @return Whether it does; `false` for a foreign or closed handle.
    [[nodiscard]] static bool drafted(ScreenHandle const& handle);

    /// @brief The id of the screen a handle this host mounted shows, a draft's included (a draft keeps the screen's).
    /// @param handle A handle.
    /// @return The id; nothing for a foreign or closed handle.
    [[nodiscard]] static std::optional<std::string> screenIdOf(ScreenHandle const& handle);

    /// @brief The params a handle this host mounted was mounted with.
    /// @param handle A handle.
    /// @return The params; nothing for a foreign handle.
    [[nodiscard]] static std::optional<ParamMap> paramsOf(ScreenHandle const& handle);
```

In `class Handle`, after `std::expected<void, MountError> remount(…);`:

```cpp
        /// Whether a draft is mounted; a tracked read.
        [[nodiscard]] bool drafted() const { return _state->drafted.get(); }

        /// Marks the screen as showing a draft, or not.
        void setDrafted(bool drafted) {
            _state->runtime->batch([this, drafted] { _state->drafted.set(drafted); });
        }
```

In `struct State`'s constructor's initializer list, after `generation{*runtime, 0},`, add `drafted{*runtime, false},`,
and change the `dirty` initializer to:

```cpp
                  dirty{*runtime, [this] { return live() && (drafted.get() || screen->dirty()); }},
```

Among `State`'s members, directly after `reactive::Signal<int> generation;`, add:

```cpp
            // Whether a draft is mounted in place of the screen's own document; the handle is dirty while it is.
            reactive::Signal<bool> drafted;
```

- [ ] **Step 4: Implement it** in `src/ui/host.cpp`, after `DocumentScreenHost::remount`:

```cpp
std::expected<void, MountError> DocumentScreenHost::mountDraft(ScreenHandle& handle,
                                                               std::shared_ptr<Document const> draft) {
    auto* const own = dynamic_cast<Handle*>(&handle);
    Screen const* const screen = own == nullptr ? nullptr : own->screen();
    if (screen == nullptr || !draft) {
        return std::unexpected(MountError{
            .screenId = draft ? draft->id : std::string{},
            .reason = "not an open document screen of this host",
        });
    }
    if (draft->id != screen->id()) {
        return std::unexpected(MountError{
            .screenId = draft->id,
            .reason = "a draft keeps the screen's id, `" + std::string{screen->id()} + "`",
        });
    }
    if (auto mounted = own->remount(_interpreter, std::move(draft), *_options.backend); !mounted) {
        return mounted;
    }
    own->setDrafted(true);
    return {};
}

std::expected<void, MountError> DocumentScreenHost::discardDraft(ScreenHandle& handle) {
    auto* const own = dynamic_cast<Handle*>(&handle);
    Screen const* const screen = own == nullptr ? nullptr : own->screen();
    if (screen == nullptr || !own->drafted()) {
        return std::unexpected(MountError{
            .screenId = screen == nullptr ? std::string{} : std::string{screen->id()},
            .reason = "no draft is mounted",
        });
    }
    std::string const screenId{screen->id()};
    std::shared_ptr<Document const> document = _options.documents(screenId);
    if (!document) {
        return std::unexpected(MountError{.screenId = screenId, .reason = "unknown screen"});
    }
    if (auto mounted = own->remount(_interpreter, std::move(document), *_options.backend); !mounted) {
        return mounted;
    }
    own->setDrafted(false);
    return {};
}

void DocumentScreenHost::endDraft(ScreenHandle& handle) {
    if (auto* const own = dynamic_cast<Handle*>(&handle); own != nullptr) {
        own->setDrafted(false);
    }
}

bool DocumentScreenHost::drafted(ScreenHandle const& handle) {
    auto const* const own = dynamic_cast<Handle const*>(&handle);
    return own != nullptr && own->screen() != nullptr && own->drafted();
}

std::optional<std::string> DocumentScreenHost::screenIdOf(ScreenHandle const& handle) {
    auto const* const own = dynamic_cast<Handle const*>(&handle);
    Screen const* const screen = own == nullptr ? nullptr : own->screen();
    return screen == nullptr ? std::nullopt : std::optional<std::string>{std::string{screen->id()}};
}

std::optional<ParamMap> DocumentScreenHost::paramsOf(ScreenHandle const& handle) {
    auto const* const own = dynamic_cast<Handle const*>(&handle);
    return own == nullptr ? std::nullopt : std::optional<ParamMap>{own->params()};
}
```

- [ ] **Step 5: A reload leaves a draft alone.** In `include/morph/ui/dev_reload.hpp`, in `reloadDocuments`'s inner
  loop, after `if (screen.screenId != screenId || screen.handle == nullptr) { continue; }`, add:

```cpp
            // A draft stays mounted: it is checked against the new bundle when the person previews or saves it, and
            // discarding it mounts the document the map holds then.
            if (DocumentScreenHost::drafted(*screen.handle)) {
                continue;
            }
```

In the function's doc comment, after the sentence ending `…the same name and type.`, add: `A screen showing a draft
(DocumentScreenHost::mountDraft) is left as it is.`

- [ ] **Step 6: Run the tests**

Run: `flock "$LOCK" cmake --build build/clang-debug --target morph_tests -j5 && flock "$LOCK" timeout 300 build/clang-debug/tests/morph_tests "[host],[dev-reload],[workspace]"`
Expected: `All tests passed`.

- [ ] **Step 7: Mutation check.** Apply the five mutations named above the tests in turn; each named test fails. Restore
  and `touch` the files.

- [ ] **Step 8: Document it.** In `docs/spec/ui/host.md`, replace the `remount` item of "## DocumentScreenHost" with:

```markdown
- **`remount(handle, document)`** mounts another document of a screen it mounted, with the same params in the same
  place, and replaces the screen behind the same handle. Two things use it: a development reload (spec 5 §12;
  [`service.md`](service.md#development-reload)) and a draft (below).
  - Each state of the new screen's own scope declared under the same name and type starts from the old screen's
    value; every other state starts from its init.
  - The old screen is destroyed after the new one mounted, so its `onUnmount` runs and the root widget is never
    missing. `title`, `dirty` and `identity` then follow the new screen, and a call the screen answers stays tied
    to it.
  - A foreign or closed handle, a close or save in progress, and the interpreter's refusal of the new document are
    `MountError`, and the old screen stays.
- **Drafts.** `mountDraft(handle, draft)` remounts a draft in place of the screen and marks the handle as showing
  one; `drafted(handle)` is a tracked read of the mark.
  - A draft keeps the screen's id: one whose `id` differs is refused, so the handle's identity, its route and a
    shell's record of the tab name the screen, never the draft. `screenIdOf(handle)` and `paramsOf(handle)` answer
    the screen's id and params.
  - While a draft is up the handle is `dirty`, so a close asks the close guard, as a screen with unsaved edits does.
  - `discardDraft(handle)` remounts the document `documents` answers for the screen's id now, so a reload since the
    draft began is seen, and clears the mark. `endDraft(handle)` clears the mark and keeps what is mounted: what a
    shell does once the draft is saved.
  - A development reload (`reloadDocuments`) leaves a drafted screen as it is.
  - The custom screens client is what makes drafts ([`custom_screens.md`](custom_screens.md#the-client)).
```

In `docs/spec/ui/interpreter.md`, change `A mount that replaces a screen, such as a development reload's remount, names
it in` to `A mount that replaces a screen, a development reload's remount or a draft's (host.md, "Drafts"), names it
in`.

In `docs/spec/ui/service.md`, "## Development reload", after the item that describes `reloadDocuments` remounting the
screens that show a changed document, add the sentence: `A screen showing a draft is left as it is; the draft is
checked against the new bundle at its next preview or save.`

- [ ] **Step 9: Commit**

```bash
git add include/morph/ui/host.hpp src/ui/host.cpp include/morph/ui/dev_reload.hpp tests/test_ui_host_drafts.cpp \
  tests/CMakeLists.txt docs/spec/ui/host.md docs/spec/ui/interpreter.md docs/spec/ui/service.md
git commit -m "ui: drafts on the document host, mounted by remount"
```

---
### Task 4: Opening in place, and a chrome slot on every page

**Files:**
- Modify: `include/morph/ui/workspace_model.hpp` (move `struct WorkspaceTabId` above `struct WorkspaceOptions`; add
  `WorkspaceOptions::chrome`; add `WorkspaceModel::openInPlace` after `openTab`; include `view.hpp`)
- Modify: `include/morph/ui/workspace.hpp` (`Page::chrome`, `_chrome`), `src/ui/workspace.cpp` (the constructor,
  `Page::~Page`, `mountPage`)
- Modify: `docs/spec/ui/workspace.md`
- Test: `tests/test_ui_workspace_in_place.cpp`; `tests/CMakeLists.txt`

**Interfaces:**
- Produces:
  - `std::function<Node(WorkspaceTabId tab, std::string_view screenId)> WorkspaceOptions::chrome`
  - `void WorkspaceModel::openInPlace(WorkspaceTabId tab, std::string_view screenId, ParamMap const& params, std::function<void(std::expected<WorkspaceTabId, MountError>)> done)`

- [ ] **Step 1: Write the failing tests** (`tests/test_ui_workspace_in_place.cpp`)

```cpp
// SPDX-License-Identifier: Apache-2.0
//
// Opening another screen in a tab's place (docs/spec/ui/workspace.md, "Opening in place"), what Open as does, and the
// chrome a shell mounts at the top of each tab's page.

#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <map>
#include <memory>
#include <morph/ui/host.hpp>
#include <morph/ui/view.hpp>
#include <morph/ui/workspace.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "ui_interpreter_support.hpp"

using morph::ui::CloseChoice;
using morph::ui::Document;
using morph::ui::DocumentScreenHost;
using morph::ui::DocumentScreenHostOptions;
using morph::ui::MountError;
using morph::ui::ParamMap;
using morph::ui::ScreenHandle;
using morph::ui::Value;
using morph::ui::Workspace;
using morph::ui::WorkspaceOptions;
using morph::ui::WorkspaceTabId;
using morph::ui::testing::Harness;
using morph::ui::testing::loadOrFail;

namespace {

constexpr std::string_view kNote = R"({
  "vocab": "ui/1", "id": "note", "params": { "noteId": "int" }, "identity": { "ref": "noteId" }, "title": "Note",
  "dirty": { "ref": "changed" }, "state": { "changed": { "init": false } },
  "view": { "kind": "column", "children": [
    { "kind": "text", "testId": "which", "text": "note" },
    { "kind": "button", "testId": "edit", "label": "edit", "onClick": [ { "set": "changed", "value": true } ] } ] } })";

constexpr std::string_view kVariant = R"({
  "vocab": "ui/1", "id": "variant", "params": { "noteId": "int" }, "identity": { "ref": "noteId" },
  "title": "Variant", "view": { "kind": "text", "testId": "variant", "text": "variant" } })";

constexpr std::string_view kHome = R"({
  "vocab": "ui/1", "id": "home", "title": "Home", "view": { "kind": "text", "testId": "home", "text": "home" } })";

constexpr std::string_view kNeedsOwner = R"({
  "vocab": "ui/1", "id": "needsOwner", "params": { "noteId": "int", "owner": "string" },
  "view": { "kind": "text", "text": "x" } })";

/// A host whose close guard keeps a dirty screen open, or discards it.
class Host final : public DocumentScreenHost {
public:
    Host(Harness& harness, CloseChoice guardChoice)
        : DocumentScreenHost{harness.session, harness.dispatcher, options(harness, guardChoice)} {}

    void onNavigate(std::string_view /*screenId*/, ParamMap const& /*params*/) override {}

    using DocumentScreenHost::onNavigate;

private:
    static DocumentScreenHostOptions options(Harness& harness, CloseChoice guardChoice) {
        static std::map<std::string, std::shared_ptr<Document const>, std::less<>> const documents{
            {"note", loadOrFail(kNote, morph::ui::testing::corpusCatalog())},
            {"variant", loadOrFail(kVariant, morph::ui::testing::corpusCatalog())},
            {"home", loadOrFail(kHome, morph::ui::testing::corpusCatalog())},
            {"needsOwner", loadOrFail(kNeedsOwner, morph::ui::testing::corpusCatalog())}};
        DocumentScreenHostOptions made;
        made.documents = [](std::string_view id) -> std::shared_ptr<Document const> {
            auto const found = documents.find(id);
            return found == documents.end() ? nullptr : found->second;
        };
        made.backend = &harness.backend;
        made.application = &harness.host;
        made.guard = [guardChoice](ScreenHandle& /*screen*/, std::function<void(CloseChoice)> const& choose) {
            choose(guardChoice);
        };
        return made;
    }
};

ParamMap note(std::int64_t id) { return ParamMap{{"noteId", Value{id}}}; }

/// Opens @p screenId in place of the tab at @p index and answers what `openInPlace` answered, once it has.
std::expected<WorkspaceTabId, MountError> inPlace(Harness& harness, Workspace& shell, std::size_t index,
                                                  std::string_view screenId, ParamMap const& params) {
    std::optional<std::expected<WorkspaceTabId, MountError>> answer;
    shell.model().openInPlace(shell.model().order().at(index), screenId, params,
                              [&answer](std::expected<WorkspaceTabId, MountError> opened) {
                                  answer = std::move(opened);
                              });
    harness.settle();
    REQUIRE(answer.has_value());
    return std::move(*answer);
}

}  // namespace

// Mutations: leave the new tab where it was appended (it is the last tab, not the second); keep the old tab beside the
// new one (four tabs remain).
TEST_CASE("workspace: openInPlace puts another screen in a tab's place, with the params given",
          "[ui][workspace][in-place]") {
    Harness harness;
    Host host{harness, CloseChoice::Discard};
    std::vector<std::vector<std::string>> records;
    WorkspaceOptions options;
    options.onRecord = [&records](std::vector<std::string> const& routes) { records.push_back(routes); };
    Workspace shell{harness.runtime, harness.backend, host, std::move(options)};
    REQUIRE(shell.open("home", {}).has_value());
    REQUIRE(shell.open("note", note(4)).has_value());
    REQUIRE(shell.open("note", note(5)).has_value());
    harness.settle();

    auto const opened = inPlace(harness, shell, 1, "variant", note(4));
    REQUIRE(opened.has_value());
    auto const tabs = shell.tabs();
    REQUIRE(tabs.size() == 3);
    CHECK(tabs[1].screenId == "variant");
    CHECK(tabs[1].identity == Value{std::int64_t{4}});
    CHECK(shell.active() == 1U);
    REQUIRE_FALSE(records.empty());
    CHECK(records.back() == std::vector<std::string>{"home", "variant?noteId=4", "note?noteId=5"});
}

// Mutation: keep the new tab when the old screen stays open (two tabs remain).
TEST_CASE("workspace: openInPlace keeps the tab when its screen stays open, and opens nothing for a refused mount",
          "[ui][workspace][in-place]") {
    Harness harness;
    Host host{harness, CloseChoice::Cancel};
    Workspace shell{harness.runtime, harness.backend, host, WorkspaceOptions{}};
    REQUIRE(shell.open("note", note(4)).has_value());
    harness.settle();
    harness.backend.click(harness.byTestId("Button", "edit"));
    harness.settle();

    CHECK_FALSE(inPlace(harness, shell, 0, "variant", note(4)).has_value());
    REQUIRE(shell.tabs().size() == 1);
    CHECK(shell.tabs()[0].screenId == "note");

    CHECK_FALSE(inPlace(harness, shell, 0, "needsOwner", note(4)).has_value());
    REQUIRE(shell.tabs().size() == 1);
    CHECK(shell.tabs()[0].screenId == "note");
}

// Mutation: append the chrome after the screen without moving it (the page's first child is not the chrome).
TEST_CASE("workspace: a page's chrome sits at its top, and a null chrome mounts nothing",
          "[ui][workspace][in-place]") {
    Harness harness;
    Host host{harness, CloseChoice::Discard};
    WorkspaceOptions options;
    options.chrome = [](WorkspaceTabId /*tab*/, std::string_view screenId) -> morph::ui::Node {
        if (screenId == "home") {
            return nullptr;
        }
        morph::ui::Text text;
        text.text = morph::ui::Prop<std::string>{std::string{"chrome"}};
        text.common.testId = "chrome_" + std::string{screenId};
        return morph::ui::text(std::move(text));
    };
    Workspace shell{harness.runtime, harness.backend, host, std::move(options)};
    REQUIRE(shell.open("note", note(1)).has_value());
    REQUIRE(shell.open("home", {}).has_value());
    harness.settle();
    std::vector<int> const pages = harness.backend.children(harness.backend.idOf(shell.root()));
    REQUIRE(pages.size() == 2);
    REQUIRE_FALSE(harness.backend.children(pages[0]).empty());
    CHECK(harness.backend.children(pages[0]).front() == harness.byTestId("Text", "chrome_note"));
    CHECK_FALSE(harness.backend.find("Text", "testId", "chrome_home").has_value());
}
```

Add `test_ui_workspace_in_place.cpp` to `tests/CMakeLists.txt`, after `test_ui_host_drafts.cpp`.

- [ ] **Step 2: Run them to verify they fail**

Run: `flock "$LOCK" cmake --build build/clang-debug --target morph_tests -j5`
Expected: compile errors, `no member named 'openInPlace'` and `no member named 'chrome' in 'morph::ui::WorkspaceOptions'`.

- [ ] **Step 3: The option and the model's member.** In `include/morph/ui/workspace_model.hpp`, add
  `#include "view.hpp"` after `#include "value.hpp"`, move `struct WorkspaceTabId { … };` with its doc comment to just
  before `struct WorkspaceOptions`, and add as the last member of `WorkspaceOptions`:

```cpp
    /// @brief What a `Workspace` mounts at the top of each tab's page, above the head row: a shell's window chrome for
    ///        that tab, such as its source view's button. Called once per tab, when its page is made; a null node
    ///        mounts nothing. Empty mounts none. `WorkspaceModel` does not read it.
    std::function<Node(WorkspaceTabId tab, std::string_view screenId)> chrome;
```

In `class WorkspaceModel`, after `openTab`:

```cpp
    /// @brief Opens @p screenId with @p params in the place of @p tab: what a shell's **Open as** does.
    ///
    /// The new screen is mounted first, beside the tab; a refused mount opens nothing and leaves the tab. The tab's
    /// screen is then asked to close (spec 6 §3). When it closes, the new screen takes its place in the order and in
    /// the record, and is the active tab; when it stays open, the new screen is closed without asking. No open tab is
    /// focused instead, whatever its identity: the person asked for this tab.
    /// @param tab      The tab.
    /// @param screenId The screen to open in its place.
    /// @param params   Its params.
    /// @param done     Called once with the new tab, or why nothing changed: a refused mount, or a screen that stayed
    ///                 open. May be empty.
    /// @throws std::out_of_range for a tab the model does not hold.
    void openInPlace(WorkspaceTabId tab, std::string_view screenId, ParamMap const& params,
                     std::function<void(std::expected<WorkspaceTabId, MountError>)> done) {
        WorkspaceTabId const old = held(tab);
        auto opened = mountTab(screenId, params);
        if (!opened) {
            if (done) {
                done(std::unexpected(std::move(opened.error())));
            }
            return;
        }
        WorkspaceTabId const fresh = *opened;
        moveTab(fresh, indexOf(old) + 1);
        std::weak_ptr<bool> const alive = _alive;
        _tabs.at(indexOf(old))
            ->handle->requestClose([this, alive, old, fresh, id = std::string{screenId},
                                    done = std::move(done)](bool closed) mutable {
                if (alive.expired()) {
                    return;
                }
                if (!closed) {
                    removeTab(fresh, true);
                    if (done) {
                        done(std::unexpected(MountError{.screenId = id, .reason = "the open screen stayed open"}));
                    }
                    return;
                }
                std::size_t const place = indexOf(old);
                removeTab(old, false);
                if (indexOf(fresh) != kNone) {
                    moveTab(fresh, std::min(place, _tabs.size() - 1));
                    activateTab(fresh, false);
                }
                recordChanged();
                if (done) {
                    done(fresh);
                }
            });
    }
```

- [ ] **Step 4: Mount the chrome.** In `include/morph/ui/workspace.hpp`, in `struct Page`, after
  `std::optional<Mounted> head;`:

```cpp
        // The shell's chrome for the tab, at the top of the page; it reads the handle through the model too.
        std::optional<Mounted> chrome;
```

and among `Workspace`'s members, after `std::function<void(Refusal const&)> _diagnostics;`:

```cpp
    std::function<Node(WorkspaceTabId, std::string_view)> _chrome;
```

In `src/ui/workspace.cpp`: in the constructor's initializer list, after `_diagnostics{options.diagnostics},`, add
`_chrome{options.chrome},`. Change `Workspace::Page::~Page` to:

```cpp
Workspace::Page::~Page() {
    chrome.reset();
    head.reset();
    slot.reset();
}
```

In `mountPage`, after the block that mounts the head row and moves it to index 0, and before
`_pages.insert_or_assign(…)`, add:

```cpp
    if (_chrome) {
        if (Node chrome = _chrome(tab, screenId)) {
            // Mounted last and moved to the top, so it sits above the head row and the screen.
            page->chrome.emplace(*_runtime, *_backend, std::move(chrome), page->slot.get());
            page->slot->moveChild(page->chrome->root(), 0);
        }
    }
```

- [ ] **Step 5: Run the tests**

Run: `flock "$LOCK" cmake --build build/clang-debug --target morph_tests -j5 && flock "$LOCK" timeout 300 build/clang-debug/tests/morph_tests "[workspace]"`
Expected: `All tests passed`.

- [ ] **Step 6: Mutation check.** Apply the three mutations named above the tests; each named test fails. Restore and
  `touch` the files.

- [ ] **Step 7: Document it.** In `docs/spec/ui/workspace.md`: add `- [Opening in place](#opening-in-place)` to
  Contents after `- [Opening](#opening)`, and after the "## Opening" section add:

```markdown
## Opening in place

`WorkspaceModel::openInPlace(tab, screenId, params, done)` opens another screen in a tab's place, which is what a
shell's **Open as** does. The new screen is mounted first, beside the tab, so a refused mount opens nothing and the tab
stays. The tab's screen is then asked to close (spec 6 §3): when it closes, the new screen takes its place in the order
and in the record and is the active tab; when it stays open (a dirty screen whose guard keeps it), the new screen is
closed without asking and `done` answers why. No open tab is focused instead, whatever its identity. Until the close is
answered, the new screen is a tab beside the old one.
```

Under "## Shape", after the item about `WorkspaceOptions::screens`, add:

```markdown
- `WorkspaceOptions::chrome(tab, screenId)` is a shell's window chrome for one tab, mounted at the top of the tab's
  page, above the head row, in either head placement; a null node mounts nothing. The generic client's custom screens
  chrome (the draft bar, **Source**, **Open as**) is one ([`custom_screens.md`](custom_screens.md#the-client)).
```

- [ ] **Step 8: Commit**

```bash
git add include/morph/ui/workspace_model.hpp include/morph/ui/workspace.hpp src/ui/workspace.cpp \
  tests/test_ui_workspace_in_place.cpp tests/CMakeLists.txt docs/spec/ui/workspace.md
git commit -m "ui: open a screen in a tab's place, and a chrome slot on every page"
```

---
### Task 5: The navigator's Custom section

**Files:**
- Modify: `include/morph/ui/navigator.hpp` (`NavigatorCustomEntry`; `NavigatorOptions::customHeading`,
  `customMark`; `Navigator::setCustom`, `custom`; the private `CustomItem` and members)
- Modify: `src/ui/navigator.cpp` (the constructor, `setCurrent`, `choose`, `menu`, `refreshVisibility`, `setCustom`)
- Modify: `docs/spec/ui/workspace.md` ("## Navigator and pins")
- Test: `tests/test_ui_navigator_custom.cpp`; `tests/CMakeLists.txt`

**Interfaces:**
- Produces:
  - `struct morph::ui::NavigatorCustomEntry { std::string id, title, savedBy; bool mine = true; std::string broken; }`
  - `void Navigator::setCustom(std::vector<NavigatorCustomEntry> entries)`
  - `std::vector<NavigatorCustomEntry> const& Navigator::custom() const noexcept`

- [ ] **Step 1: Write the failing tests** (`tests/test_ui_navigator_custom.cpp`)

```cpp
// SPDX-License-Identifier: Apache-2.0
//
// The navigator's Custom section (docs/spec/ui/workspace.md, "Navigator and pins"): a person's custom screens after the
// application's groups, each marked, naming who saved it when that was someone else, and badged when broken.

#include <catch2/catch_test_macros.hpp>
#include <morph/ui/bundle.hpp>
#include <morph/ui/navigator.hpp>
#include <morph/ui/testing/recording_backend.hpp>
#include <string>
#include <vector>

using morph::ui::CatalogEntry;
using morph::ui::Navigator;
using morph::ui::NavigatorCustomEntry;
using morph::ui::NavigatorOptions;
using morph::ui::testing::RecordingBackend;

namespace {

NavigatorOptions options(std::vector<std::string>& opened) {
    NavigatorOptions made;
    made.entries = {CatalogEntry{.id = "notes.home", .title = "Home", .group = "Notes"}};
    made.onOpen = [&opened](std::string const& id) { opened.push_back(id); };
    return made;
}

std::vector<NavigatorCustomEntry> customs() {
    return {
        NavigatorCustomEntry{.id = "custom:aa", .title = "Mine", .savedBy = "ann", .mine = true, .broken = {}},
        NavigatorCustomEntry{.id = "custom:bb", .title = "Shared", .savedBy = "bob", .mine = false, .broken = {}},
        NavigatorCustomEntry{.id = "custom:cc",
                             .title = "Old",
                             .savedBy = "ann",
                             .mine = true,
                             .broken = "the action Lims_Verify is not permitted"},
    };
}

int widget(RecordingBackend const& backend, std::string const& kind, std::string const& testId) {
    auto const found = backend.find(kind, "testId", testId);
    REQUIRE(found.has_value());
    return *found;
}

}  // namespace

// Mutations: show the author of every entry, mine included (`nav_author_custom:aa` shows); leave the section shown
// when the list is emptied.
TEST_CASE("navigator: custom screens are a section after the groups, marked, with their author and breakage",
          "[ui][navigator][custom-screens]") {
    RecordingBackend backend;
    std::vector<std::string> opened;
    Navigator navigator{backend, nullptr, options(opened)};
    int const section = widget(backend, "Column", "navigator_custom_section");
    CHECK(backend.prop(section, "visible") == "false");

    navigator.setCustom(customs());
    CHECK(backend.prop(section, "visible") == "true");
    CHECK(backend.children(widget(backend, "Column", "navigator")).back() == section);
    CHECK(backend.prop(widget(backend, "Text", "navigator_custom"), "text") == "Custom");
    for (std::string const id : {"custom:aa", "custom:bb", "custom:cc"}) {
        CHECK(backend.prop(widget(backend, "Badge", "nav_mark_" + id), "text") == "Custom");
    }
    CHECK(backend.prop(widget(backend, "Text", "nav_author_custom:aa"), "visible") == "false");
    int const author = widget(backend, "Text", "nav_author_custom:bb");
    CHECK(backend.prop(author, "visible") == "true");
    CHECK(backend.prop(author, "text") == "by bob");
    CHECK(backend.prop(widget(backend, "Badge", "nav_broken_custom:cc"), "visible") == "true");
    CHECK(backend.prop(widget(backend, "Button", "nav_custom:cc"), "tooltip") ==
          "the action Lims_Verify is not permitted");
    CHECK(backend.prop(widget(backend, "Badge", "nav_broken_custom:aa"), "visible") == "false");

    navigator.setCustom({});
    CHECK(backend.prop(section, "visible") == "false");
}

// Mutation: drop the custom branch of `choose` (the menu's custom entry opens nothing).
TEST_CASE("navigator: a custom entry opens by its id from the sidebar and from the menu",
          "[ui][navigator][custom-screens]") {
    RecordingBackend backend;
    std::vector<std::string> opened;
    Navigator navigator{backend, nullptr, options(opened)};
    navigator.setCustom(customs());
    backend.click(widget(backend, "Button", "nav_custom:bb"));
    CHECK(opened == std::vector<std::string>{"custom:bb"});

    auto const menu = navigator.menuEntries();
    REQUIRE_FALSE(menu.empty());
    CHECK(menu.back().label == "Custom");
    REQUIRE(menu.back().items.size() == 3);
    CHECK(menu.back().items[1].label == "Shared · bob");
    CHECK(navigator.choose({menu.size() - 1, 2}));
    CHECK(opened.back() == "custom:cc");
}

// Mutation: leave custom rows out of the search (`Shared` still shows while searching "min").
TEST_CASE("navigator: the search filters custom entries, and the open custom screen is marked",
          "[ui][navigator][custom-screens]") {
    RecordingBackend backend;
    std::vector<std::string> opened;
    Navigator navigator{backend, nullptr, options(opened)};
    navigator.setCustom(customs());
    int const search = widget(backend, "TextInput", "navigator_search");
    backend.edit(search, "min");
    CHECK(backend.prop(widget(backend, "Row", "nav_row_custom:aa"), "visible") == "true");
    CHECK(backend.prop(widget(backend, "Row", "nav_row_custom:bb"), "visible") == "false");
    backend.edit(search, "");

    navigator.setCurrent("custom:bb");
    CHECK(navigator.current() == "custom:bb");
    CHECK(backend.prop(widget(backend, "Button", "nav_custom:bb"), "selected") == "true");
}
```

Add `test_ui_navigator_custom.cpp` to `tests/CMakeLists.txt`, after `test_ui_workspace_in_place.cpp`.

- [ ] **Step 2: Run them to verify they fail**

Run: `flock "$LOCK" cmake --build build/clang-debug --target morph_tests -j5`
Expected: compile error, `no type named 'NavigatorCustomEntry' in namespace 'morph::ui'`.

- [ ] **Step 3: Declare it.** In `include/morph/ui/navigator.hpp`, after `struct NavigatorBadge { … };`:

```cpp
/// @brief A person's custom screen as the navigator lists it, in its Custom section.
struct NavigatorCustomEntry {
    /// @brief The screen's id, `custom:` and its digits.
    std::string id;
    /// @brief The name its store gave it.
    std::string title;
    /// @brief Who saved it.
    std::string savedBy;
    /// @brief Whether the person saved it; an entry they did not save names who did.
    bool mine = true;
    /// @brief Why it may not be mounted today; empty when it may.
    std::string broken;
    /// @brief Memberwise equality.
    /// @return Whether every member matches.
    bool operator==(NavigatorCustomEntry const&) const = default;
};
```

In `struct NavigatorOptions`, after `onPins`:

```cpp
    /// @brief The heading of the Custom section, and the label of its submenu in `menuEntries`.
    std::string customHeading = "Custom";
    /// @brief The mark every custom entry carries, so none passes for an application screen's entry.
    std::string customMark = "Custom";
```

In `class Navigator`'s public part, after `setOnChanged`:

```cpp
    /// @brief Lists the person's custom screens in the Custom section, after the application's groups, replacing what
    ///        it listed before.
    ///
    /// Each entry is a row as the catalog's are (`nav_row_<id>`, `nav_<id>`), whose content holds the custom mark
    /// (`nav_mark_<id>`), the title (`nav_title_<id>`), who saved it when it is not the person's own
    /// (`nav_author_<id>`, "by <savedBy>") and, for a broken screen, a badge (`nav_broken_<id>`) with the reason as the
    /// entry's tooltip. Choosing an entry calls `onOpen` with its id, a broken one included: what opening it does is the
    /// application's. The search filters the section as it filters the groups, `setCurrent` marks its entries, and
    /// `menuEntries` ends with it as a submenu. An empty list hides the section.
    /// @param entries The screens, in order.
    void setCustom(std::vector<NavigatorCustomEntry> entries);

    /// @brief The custom screens the Custom section lists.
    /// @return They, in order.
    [[nodiscard]] std::vector<NavigatorCustomEntry> const& custom() const noexcept { return _custom; }
```

In the private part, after `struct Module { … };`:

```cpp
    // A custom screen's row: the button holding the mark, the title, the author and the broken badge.
    struct CustomItem {
        NavigatorCustomEntry entry;
        std::unique_ptr<StackWidget> row;
        std::unique_ptr<ButtonWidget> button;
        std::unique_ptr<StackWidget> content;
        std::unique_ptr<BadgeWidget> mark;
        std::unique_ptr<TextWidget> label;
        std::unique_ptr<TextWidget> author;
        std::unique_ptr<BadgeWidget> broken;
    };
    [[nodiscard]] CustomItem const* customItemOf(std::string_view screenId) const;
    // The Custom section, made empty and hidden at construction, after every group.
    void makeCustomSection();
```

and among the members, after `std::vector<Module> _modules;`:

```cpp
    std::unique_ptr<StackWidget> _customSection;
    std::unique_ptr<TextWidget> _customHeading;
    // After the section, so the rows go before the column that holds them.
    std::vector<CustomItem> _customItems;
    std::vector<NavigatorCustomEntry> _custom;
```

- [ ] **Step 4: Implement it** in `src/ui/navigator.cpp`.

In the constructor, after `arrange();`, add `makeCustomSection();`. (`arrange` places only the module rows and the
group sections, from slot 2 on, so the section made last stays the last child.)

Replace `setCurrent` with:

```cpp
void Navigator::setCurrent(std::string_view screenId) {
    if (screenId == _current) {
        return;
    }
    if (Item const* const previous = itemOf(_current)) {
        previous->button->setSelected(false);
    }
    if (CustomItem const* const previous = customItemOf(_current)) {
        previous->button->setSelected(false);
    }
    _current = std::string{screenId};
    if (Item const* const next = itemOf(_current)) {
        next->button->setSelected(true);
        if (CatalogEntry const* const entry = entryOf(_current); entry != nullptr && _openModule != entry->module) {
            _openModule = entry->module;
            refreshVisibility();
        }
    } else if (CustomItem const* const shown = customItemOf(_current)) {
        shown->button->setSelected(true);
    } else {
        _current.clear();
    }
    changed();
}
```

In `choose`, replace the lines from `CatalogEntry const* const entry = entryOf(slot->screenId);` to the end of the
function with:

```cpp
    if (customItemOf(slot->screenId) == nullptr) {
        CatalogEntry const* const entry = entryOf(slot->screenId);
        if (entry == nullptr || !entry->available) {
            return false;
        }
    }
    if (_options.onOpen) {
        _options.onOpen(slot->screenId);
    }
    return true;
```

In `menu()`, replace
```cpp
    if (_moduleGroups) {
        moduleMenu(out);
        return out;
    }
    for (Group const* const listed : catalogOrder()) {
```
with
```cpp
    if (_moduleGroups) {
        moduleMenu(out);
    }
    for (Group const* const listed : _moduleGroups ? std::vector<Group const*>{} : catalogOrder()) {
```
and, before `return out;` at the end of `menu()`, add:

```cpp
    if (!_customItems.empty()) {
        MenuEntry submenu;
        submenu.label = _options.customHeading;
        MenuSlot slot;
        for (CustomItem const& item : _customItems) {
            MenuEntry made;
            made.label = item.entry.title;
            if (!item.entry.mine) {
                made.label += " · " + item.entry.savedBy;
            }
            if (!item.entry.broken.empty()) {
                made.label += " · !";
            }
            if (item.entry.id == _current) {
                made.checked = true;
            }
            submenu.items.push_back(std::move(made));
            slot.items.push_back(MenuSlot{.screenId = item.entry.id, .items = {}});
        }
        out.entries.push_back(std::move(submenu));
        out.targets.push_back(std::move(slot));
    }
```

At the end of `refreshVisibility()`, add:

```cpp
    bool anyCustom = false;
    for (CustomItem const& item : _customItems) {
        bool const matches = wanted.empty() || lower(item.entry.title).contains(wanted);
        item.row->setVisible(matches);
        anyCustom = anyCustom || matches;
    }
    if (_customSection) {
        _customSection->setVisible(anyCustom);
    }
```

Add the new functions before `}  // namespace morph::ui`:

```cpp
Navigator::CustomItem const* Navigator::customItemOf(std::string_view screenId) const {
    auto const found = std::ranges::find_if(
        _customItems, [screenId](CustomItem const& item) { return !screenId.empty() && item.entry.id == screenId; });
    return found == _customItems.end() ? nullptr : &*found;
}

void Navigator::makeCustomSection() {
    _customSection = _backend->createStack(_root.get(), Axis::Vertical);
    _customSection->setGap(1);
    _customSection->setTestId("navigator_custom_section");
    _customSection->setVisible(false);
    _customHeading = _backend->createText(_customSection.get());
    _customHeading->setRole(TextRole::Overline);
    _customHeading->setTestId("navigator_custom");
    _customHeading->setText(_options.customHeading);
}

void Navigator::setCustom(std::vector<NavigatorCustomEntry> entries) {
    if (entries == _custom) {
        return;
    }
    _customItems.clear();
    _custom = std::move(entries);
    for (NavigatorCustomEntry const& entry : _custom) {
        CustomItem item{.entry = entry,
                        .row = _backend->createStack(_customSection.get(), Axis::Horizontal),
                        .button = {},
                        .content = {},
                        .mark = {},
                        .label = {},
                        .author = {},
                        .broken = {}};
        item.row->setTestId("nav_row_" + entry.id);
        item.row->setLayout({.width = Sizing::stretch(), .height = {}});
        item.row->setAlign(Align::Center);
        item.button = _backend->createButton(item.row.get());
        item.button->setTestId("nav_" + entry.id);
        item.button->setStyle(ButtonStyle::Item);
        item.button->setLayout({.width = Sizing::stretch(), .height = {}});
        item.button->setLabel(entry.title);
        item.button->setAccessibleName(_options.customMark + ", " + entry.title +
                                       (entry.mine ? std::string{} : ", by " + entry.savedBy) +
                                       (entry.broken.empty() ? std::string{} : ", broken"));
        if (!entry.broken.empty()) {
            item.button->setTooltip(entry.broken);
        }
        std::string const screenId = entry.id;
        item.button->setOnClick([this, screenId] {
            if (_options.onOpen) {
                _options.onOpen(screenId);
            }
        });
        // The mark, the title, the author and the badge are the button's content, so its selection spans them all.
        ContainerWidget& content = item.button->content();
        item.content = _backend->createStack(&content, Axis::Horizontal);
        item.content->setTestId("nav_content_" + entry.id);
        item.content->setGap(1);
        item.content->setAlign(Align::Center);
        item.content->setLayout({.width = Sizing::stretch(), .height = {}});
        item.mark = _backend->createBadge(item.content.get());
        item.mark->setTestId("nav_mark_" + entry.id);
        item.mark->setText(_options.customMark);
        item.mark->setTone(Tone::Info);
        item.label = _backend->createText(item.content.get());
        item.label->setTestId("nav_title_" + entry.id);
        item.label->setText(entry.title);
        item.label->setLayout({.width = Sizing::stretch(), .height = {}});
        item.author = _backend->createText(item.content.get());
        item.author->setTestId("nav_author_" + entry.id);
        item.author->setRole(TextRole::Muted);
        item.author->setText("by " + entry.savedBy);
        item.author->setVisible(!entry.mine);
        item.broken = _backend->createBadge(item.content.get());
        item.broken->setTestId("nav_broken_" + entry.id);
        item.broken->setText("Broken");
        item.broken->setTone(Tone::Err);
        item.broken->setVisible(!entry.broken.empty());
        if (entry.id == _current) {
            item.button->setSelected(true);
        }
        _customItems.push_back(std::move(item));
    }
    if (!_current.empty() && itemOf(_current) == nullptr && customItemOf(_current) == nullptr) {
        _current.clear();
    }
    refreshVisibility();
    changed();
}
```

- [ ] **Step 5: Run the tests**

Run: `flock "$LOCK" cmake --build build/clang-debug --target morph_tests -j5 && flock "$LOCK" timeout 300 build/clang-debug/tests/morph_tests "[navigator],[shell]"`
Expected: `All tests passed` (the shell frame's navigator tests included).

- [ ] **Step 6: Mutation check.** Apply the three mutations named above the tests; each named test fails. Restore and
  `touch src/ui/navigator.cpp`.

- [ ] **Step 7: Document it.** In `docs/spec/ui/workspace.md`, "## Navigator and pins", after the "Unavailable
  entries" item, add:

```markdown
- **Custom screens.** `setCustom(entries)` lists the person's custom screens in a section of their own,
  `navigator_custom_section`, after the application's groups, under the heading `navigator_custom`
  (`NavigatorOptions::customHeading`, "Custom"). Each `NavigatorCustomEntry` is a row as a catalog entry's is
  (`nav_row_<id>`, `nav_<id>`), whose content holds the custom mark (`nav_mark_<id>`, `customMark`), the title, who
  saved it when it is not the person's own (`nav_author_<id>`, "by <savedBy>"), and for a broken screen a badge
  (`nav_broken_<id>`) with its reason as the tooltip, so a custom entry never passes for an application entry. The
  accessible name says the same. Choosing one calls `onOpen` with its id, a broken one included; the search filters
  the section, `setCurrent` marks its entries, and `menuEntries` ends with a submenu of it. An empty list hides the
  section.
```

- [ ] **Step 8: Commit**

```bash
git add include/morph/ui/navigator.hpp src/ui/navigator.cpp tests/test_ui_navigator_custom.cpp tests/CMakeLists.txt \
  docs/spec/ui/workspace.md
git commit -m "ui: the navigator lists custom screens in a section of their own"
```

---
### Task 6: `CustomScreenSync` and the `CustomDocuments` cache

**Files:**
- Create: `include/morph/ui/custom_screen_sync.hpp`, `src/ui/custom_screen_sync.cpp`
- Modify: `CMakeLists.txt` (the source after `src/ui/json_source.cpp`, the header after
  `include/morph/ui/json_source.hpp`)
- Create: `tests/ui_custom_client_support.hpp` (Task 7 appends the chrome rig to it)
- Test: `tests/test_ui_custom_screen_sync.cpp`; `tests/CMakeLists.txt`

**Interfaces:**
- Consumes: the wire types and action ids of `include/morph/ui/custom_screens.hpp`; `Dispatcher`, `ModelChannel`,
  `CallOptions`, `Watcher`, `detail::failureText` (`interpreter.hpp`); `loadDocumentText` (`document.hpp`).
- Produces:
  - `bool morph::ui::isCustomScreenId(std::string_view)`, `std::string_view morph::ui::routeScreen(std::string_view)`
  - `class CustomDocuments` with `struct Entry { CustomScreenSummary summary; std::string text; std::shared_ptr<Document const> document; }`,
    `put(CustomScreenFound, LoadOptions const&) -> std::expected<std::shared_ptr<Document const>, Refusal>`,
    `find(std::string_view) -> std::shared_ptr<Document const>`, `entry(std::string_view) -> Entry const*`,
    `refresh(CustomScreensListed const&) -> std::vector<std::string>`, `forget(std::string_view)`, `clear()`
  - `enum class CustomScreensOffer : std::uint8_t { Unknown, Offered, Off }`
  - `using CustomDocumentDone = std::function<void(std::expected<std::shared_ptr<Document const>, std::string>)>`
  - `class CustomScreenSync(Dispatcher&, std::string application, std::function<Catalog()> catalog, Watcher watcher = {})`
    with `probe(std::function<void(bool)>)`, `offer()`, `offRefusal()`, `setOnListed(std::function<void(CustomScreensListed const&)>)`,
    `listed()`, `relist()`, `open(std::string const&, CustomDocumentDone)`, `fetch(std::string const&, CustomDocumentDone)`,
    `get(std::string const&, std::function<void(std::expected<CustomScreenFound, std::string>)>)`,
    `check(std::string basedOn, std::string document, std::function<void(std::expected<std::optional<CheckRefusal>, std::string>)>)`,
    `save(CustomScreensSave, std::function<void(std::expected<std::string, std::string>)>)`,
    `prefetchRoutes(std::vector<std::string> const&, std::function<void(std::vector<std::string>)>)`,
    `reconnected()`, `deliverEvent(wire::EventBody const&)`, `reset()`, `documents()`

- [ ] **Step 1: Write the test support** (`tests/ui_custom_client_support.hpp`)

```cpp
// SPDX-License-Identifier: Apache-2.0
//
// Fixtures of the custom screens client's tests: a server without the model, a store that refuses every save, a
// client of a server's custom screens over a simulated connection, and helpers over the model's replies.

#pragma once
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <expected>
#include <glaze/glaze.hpp>
#include <memory>
#include <morph/core/bridge.hpp>
#include <morph/core/completion.hpp>
#include <morph/core/executor.hpp>
#include <morph/core/owner_strand.hpp>
#include <morph/core/registry.hpp>
#include <morph/core/remote.hpp>
#include <morph/core/wire.hpp>
#include <morph/session/session.hpp>
#include <morph/testing/step_executor.hpp>
#include <morph/ui/custom_screen_sync.hpp>
#include <morph/ui/custom_screens.hpp>
#include <morph/ui/interpreter.hpp>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "test_support.hpp"
#include "ui_custom_screens_support.hpp"

namespace custom_client {

/// A RemoteServer that never registered `morph.customScreens`.
struct BareServer {
    morph::exec::ThreadPoolExecutor pool{2};
    morph::model::detail::ActionDispatcher dispatcher;
    morph::model::detail::ModelRegistryFactory registry;
    std::shared_ptr<uiservice::RuleAuthorizer> authorizer = std::make_shared<uiservice::RuleAuthorizer>();
    std::shared_ptr<morph::backend::RemoteServer> server =
        std::make_shared<morph::backend::RemoteServer>(pool, authorizer, dispatcher, registry);
};

/// A store that keeps what a memory store keeps but refuses every save, with a reason for the person.
struct RefusingStore final : morph::ui::ICustomScreenStore {
    explicit RefusingStore(morph::exec::IExecutor& base) : inner{base}, strand{base} {}
    morph::async::Completion<std::vector<morph::ui::CustomScreen>> list(std::string person, std::string app) override {
        return inner.list(std::move(person), std::move(app));
    }
    morph::async::Completion<std::expected<morph::ui::CustomScreen, morph::ui::CustomScreenRefusal>> get(
        morph::ui::CustomScreenKey key) override {
        return inner.get(std::move(key));
    }
    morph::async::Completion<std::expected<void, morph::ui::CustomScreenRefusal>> put(
        morph::ui::CustomScreenKey /*key*/, morph::ui::CustomScreen /*screen*/) override {
        auto [completion, promise] =
            morph::async::Completion<std::expected<void, morph::ui::CustomScreenRefusal>>::makeSettleable(&strand);
        promise.resolve(std::unexpected(morph::ui::CustomScreenRefusal{.reason = "over your limit of custom screens"}));
        return std::move(completion);
    }
    morph::async::Completion<std::expected<void, morph::ui::CustomScreenRefusal>> remove(
        morph::ui::CustomScreenKey key) override {
        return inner.remove(std::move(key));
    }
    morph::ui::MemoryCustomScreenStore inner;
    /// The serial owner the refusal is delivered on, declared last so it is destroyed first.
    morph::exec::OwnerStrand strand;
};

/// The id a `save` reply carries.
inline std::string savedIdOf(morph::wire::Envelope const& reply) {
    REQUIRE(reply.kind == "ok");
    morph::ui::CustomScreenSaved out;
    REQUIRE_FALSE(glz::read_json(out, reply.body));
    return out.id;
}

/// A `save` body: a screen of @p app based on @p basedOn, overwriting @p id when it is given.
inline std::string saveBody(std::string const& app, std::string const& title, std::string const& basedOn,
                            std::string const& document, std::string const& id = {}) {
    return R"({"app":)" + screens_test::quoted(app) + (id.empty() ? std::string{} : R"(,"id":)" + screens_test::quoted(id)) +
           R"(,"title":)" + screens_test::quoted(title) + R"(,"basedOn":)" + screens_test::quoted(basedOn) +
           R"(,"document":)" + screens_test::quoted(document) + "}";
}

/// One client of @p server, signed in as @p person, for @p app: a bridge over a simulated connection, the dispatcher
/// over it, and a sync. With @p recordWatches, the sync's watches are recorded and answered with nothing refused.
struct SyncClient {
    SyncClient(morph::backend::RemoteServer& server, std::string person, std::string app, bool recordWatches = false)
        : bridge{std::make_unique<morph::backend::SimulatedRemoteBackend>(server), owner},
          sync{dispatcher, std::move(app), [] { return uiservice::facts(); },
               recordWatches ? watcher() : morph::ui::Watcher{}} {
        morph::session::Context signedIn;
        signedIn.token = std::move(person);
        bridge.setDefaultSession(std::move(signedIn));
    }

    /// Runs the owner until @p done holds, for at most @p budget.
    template <typename Pred>
    bool pump(Pred done, std::chrono::milliseconds budget = morph::testing::kDefaultWaitBudget) {
        return morph::testing::waitUntil(
            [&] {
                owner.runAll();
                return done();
            },
            morph::testing::WaitBudget{budget});
    }

    /// Probes, and answers whether the server offers custom screens.
    bool probe() {
        std::optional<bool> offered;
        sync.probe([&offered](bool answer) { offered = answer; });
        REQUIRE(pump([&offered] { return offered.has_value(); }));
        return *offered;
    }

    morph::ui::Watcher watcher() {
        return [this](std::string const& kind, std::string const& body) {
            watches.push_back(kind + " " + body);
            auto [completion, promise] = morph::async::Completion<std::string>::makeSettleable(&owner);
            promise.resolve(std::string{R"({"refusedRecords":[],"refusedInstances":[]})"});
            return std::move(completion);
        };
    }

    std::vector<std::string> watches;
    morph::testing::StepExecutor owner;
    morph::bridge::Bridge bridge;
    morph::ui::BridgeDispatcher dispatcher{bridge, &owner};
    morph::ui::CustomScreenSync sync;
};

}  // namespace custom_client
```

- [ ] **Step 2: Write the failing tests** (`tests/test_ui_custom_screen_sync.cpp`)

```cpp
// SPDX-License-Identifier: Apache-2.0
//
// The client of the custom screens model (docs/spec/ui/custom_screens.md, "The client"): discovery by `list`, the
// cache `DocumentScreenHostOptions::documents` answers custom ids from, its refresh by hash, the server's check and the
// store's refusal passed through, routes fetched before a restore, and the list refreshed by events.

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <glaze/glaze.hpp>
#include <memory>
#include <morph/core/executor.hpp>
#include <morph/core/logger.hpp>
#include <morph/core/wire.hpp>
#include <morph/ui/custom_screen_sync.hpp>
#include <morph/ui/custom_screens.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "ui_custom_client_support.hpp"

using custom_client::SyncClient;
using morph::ui::CustomScreensOffer;

namespace {

/// Captures every warning logged while it lives.
struct Warnings {
    std::vector<std::string> seen;
    morph::log::ScopedLoggerOverride guard{[this](morph::log::LogLevel level, std::string_view message) {
        if (level == morph::log::LogLevel::warn) {
            seen.emplace_back(message);
        }
    }};
};

morph::wire::EventBody changesOf(std::string_view record) {
    return morph::wire::EventBody{
        .topic = std::string{morph::wire::kChangesTopic},
        .payload = glz::raw_json{R"({"records":[{"record":")" + std::string{record} + R"("}],"instances":[]})"}};
}

}  // namespace

// Mutation: log every refusal at warn, `unknown model type` included (one warning is captured).
TEST_CASE("custom screen sync: a server without the model is off, and nothing is logged",
          "[ui][custom-screens][sync]") {
    Warnings warnings;
    custom_client::BareServer server;
    SyncClient client{*server.server, "ann", "lims"};
    CHECK_FALSE(client.probe());
    CHECK(client.sync.offer() == CustomScreensOffer::Off);
    INFO("refusal: " << client.sync.offRefusal());
    // The spec says the dispatcher's refusal reads `unknown model type: morph.customScreens`. If this fails, the
    // wording differs on this transport: stop and report it, since the spec's discovery rests on it.
    CHECK(client.sync.offRefusal().contains("unknown model type: morph.customScreens"));
    // Other parts of the process may warn too (the server about a refused registration); only the sync's own matters.
    CHECK(std::ranges::none_of(warnings.seen, [](std::string const& seen) { return seen.contains("custom screens"); }));
}

// Mutation: treat a refused `list` as offered (`offer()` reads Offered and nothing is logged).
TEST_CASE("custom screen sync: any other refusal of list is off too, and is logged at warn",
          "[ui][custom-screens][sync]") {
    Warnings warnings;
    screens_test::Server server;
    SyncClient client{*server.server, "ann", "elsewhere"};
    CHECK_FALSE(client.probe());
    CHECK(client.sync.offer() == CustomScreensOffer::Off);
    CHECK(std::ranges::count_if(warnings.seen, [](std::string const& seen) {
              return seen.contains("custom screens are off") && seen.contains("morph.customScreens.list");
          }) == 1);
}

// Mutation: keep a cached document whose listed hash changed (`find` still answers it after the relist).
TEST_CASE("custom screen sync: list, open into the cache, and a screen changed on the server is dropped from it",
          "[ui][custom-screens][sync]") {
    screens_test::Server server;
    std::string const id =
        custom_client::savedIdOf(server.save(screens_test::baseScreen("lims.base"), "ann", "Mine"));
    SyncClient client{*server.server, "ann", "lims"};
    REQUIRE(client.probe());
    REQUIRE(client.sync.listed().screens.size() == 1);
    morph::ui::CustomScreenSummary const listed = client.sync.listed().screens.front();
    CHECK(listed.id == id);
    CHECK(listed.title == "Mine");
    CHECK(listed.mine);

    std::optional<std::expected<std::shared_ptr<morph::ui::Document const>, std::string>> opened;
    client.sync.open(id, [&opened](auto loaded) { opened = std::move(loaded); });
    REQUIRE(client.pump([&opened] { return opened.has_value(); }));
    REQUIRE(opened->has_value());
    CHECK((**opened)->id == id);
    REQUIRE(client.sync.documents().find(id) != nullptr);
    REQUIRE(client.sync.documents().entry(id) != nullptr);
    CHECK(client.sync.documents().entry(id)->summary.hash == listed.hash);

    std::string const changed =
        screens_test::replaced(screens_test::baseScreen("lims.base"), R"("Results")", R"("Changed")");
    REQUIRE(server.call(morph::ui::kCustomScreensSave, custom_client::saveBody("lims", "Mine", "lims.base", changed, id),
                        "ann")
                .kind == "ok");
    client.sync.relist();
    REQUIRE(client.pump([&] {
        return !client.sync.listed().screens.empty() && client.sync.listed().screens.front().hash != listed.hash;
    }));
    CHECK(client.sync.documents().find(id) == nullptr);
}

// Mutation: answer every check as passing (the refusal is absent).
TEST_CASE("custom screen sync: check answers the server's refusal with its pointer", "[ui][custom-screens][sync]") {
    screens_test::Server server;
    SyncClient client{*server.server, "ann", "lims"};
    REQUIRE(client.probe());
    std::string const erasing =
        screens_test::replaced(screens_test::baseScreen("lims.base"), R"({"host":"print"})", R"({"host":"erase"})");
    std::optional<std::expected<std::optional<morph::ui::CheckRefusal>, std::string>> refused;
    client.sync.check("lims.base", erasing, [&refused](auto checked) { refused = std::move(checked); });
    REQUIRE(client.pump([&refused] { return refused.has_value(); }));
    REQUIRE(refused->has_value());
    REQUIRE((*refused)->has_value());
    CHECK_FALSE((**refused)->pointer.empty());
    CHECK_FALSE((**refused)->reason.empty());

    std::optional<std::expected<std::optional<morph::ui::CheckRefusal>, std::string>> passed;
    client.sync.check("lims.base", screens_test::baseScreen("lims.base"),
                      [&passed](auto checked) { passed = std::move(checked); });
    REQUIRE(client.pump([&passed] { return passed.has_value(); }));
    REQUIRE(passed->has_value());
    CHECK_FALSE((*passed)->has_value());
}

// Mutation: report a refused save as saved (`done` gets an id).
TEST_CASE("custom screen sync: a store's refusal of save reaches the caller as the store gave it",
          "[ui][custom-screens][sync]") {
    morph::exec::ThreadPoolExecutor storePool{1};
    screens_test::Server server{std::make_shared<custom_client::RefusingStore>(storePool)};
    SyncClient client{*server.server, "ann", "lims"};
    REQUIRE(client.probe());
    std::optional<std::expected<std::string, std::string>> saved;
    client.sync.save(morph::ui::CustomScreensSave{.app = {},
                                                  .id = std::nullopt,
                                                  .title = "Mine",
                                                  .basedOn = "lims.base",
                                                  .document = screens_test::baseScreen("lims.base")},
                     [&saved](std::expected<std::string, std::string> answer) { saved = std::move(answer); });
    REQUIRE(client.pump([&saved] { return saved.has_value(); }));
    REQUIRE_FALSE(saved->has_value());
    CHECK(saved->error().contains("over your limit of custom screens"));
}

// Mutation: skip the fetch in prefetchRoutes (nothing is cached and nothing is missing).
TEST_CASE("custom screen sync: prefetchRoutes loads the custom screens a record names, and reports those get refuses",
          "[ui][custom-screens][sync]") {
    screens_test::Server server;
    std::string const id = custom_client::savedIdOf(server.save(screens_test::baseScreen("lims.base"), "ann"));
    SyncClient client{*server.server, "ann", "lims"};
    REQUIRE(client.probe());
    std::string const gone = "custom:" + std::string(32, '0');
    std::optional<std::vector<std::string>> missing;
    client.sync.prefetchRoutes({std::string{"lims.base"}, id, gone + "?x=1"},
                               [&missing](std::vector<std::string> ids) { missing = std::move(ids); });
    REQUIRE(client.pump([&missing] { return missing.has_value(); }));
    CHECK(*missing == std::vector<std::string>{gone});
    CHECK(client.sync.documents().find(id) != nullptr);
}

// Mutations: ignore a `changes` event (the list stays empty); relist on any record's change (the event naming another
// record already shows the screen).
TEST_CASE("custom screen sync: a changes event naming the record relists, and one naming another does not",
          "[ui][custom-screens][sync]") {
    screens_test::Server server;
    SyncClient client{*server.server, "ann", "lims"};
    REQUIRE(client.probe());
    REQUIRE(client.sync.listed().screens.empty());
    static_cast<void>(custom_client::savedIdOf(server.save(screens_test::baseScreen("lims.base"), "ann")));

    client.sync.deliverEvent(changesOf("SomethingElse"));
    static_cast<void>(client.pump([] { return false; }, std::chrono::milliseconds{200}));
    CHECK(client.sync.listed().screens.empty());

    client.sync.deliverEvent(changesOf(morph::ui::kCustomScreenRecord));
    REQUIRE(client.pump([&client] { return client.sync.listed().screens.size() == 1; }));
}

// Mutation: skip the watch in reconnected (two watches are sent, not three).
TEST_CASE("custom screen sync: the record is watched once offered, again after a reconnect and after a resync",
          "[ui][custom-screens][sync]") {
    screens_test::Server server;
    SyncClient client{*server.server, "ann", "lims", true};
    REQUIRE(client.probe());
    REQUIRE(client.watches.size() == 1);
    CHECK(client.watches.front().starts_with("watch "));
    CHECK(client.watches.front().contains(R"("records":["morph.customScreens"])"));
    CHECK(client.watches.front().contains(R"("replace":false)"));
    client.sync.reconnected();
    client.sync.deliverEvent(
        morph::wire::EventBody{.topic = std::string{morph::wire::kResyncTopic}, .payload = glz::raw_json{"null"}});
    REQUIRE(client.pump([&client] { return client.watches.size() == 3; }));
}
```

Add `test_ui_custom_screen_sync.cpp` to `tests/CMakeLists.txt`, after `test_ui_navigator_custom.cpp`.

- [ ] **Step 3: Run them to verify they fail**

Run: `flock "$LOCK" cmake --build build/clang-debug --target morph_tests -j5`
Expected: compile error, `'morph/ui/custom_screen_sync.hpp' file not found`.

- [ ] **Step 4: Write the header** (`include/morph/ui/custom_screen_sync.hpp`)

```cpp
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <expected>
#include <functional>
#include <map>
#include <memory>
#include <morph/export.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "../attributes.hpp"
#include "../core/callback_scope.hpp"
#include "../core/wire.hpp"
#include "custom_screens.hpp"
#include "document.hpp"
#include "interpreter.hpp"
#include "value.hpp"

/// @file
/// @brief The client of the custom screens model: discovery, the documents a client has fetched, and the calls the
///        source view and the draft bar make.
///
/// Specified in `docs/spec/ui/custom_screens.md`, "The client".

namespace morph::ui {

/// @brief Whether @p screenId names a custom screen.
/// @param screenId The screen id.
/// @return Whether it starts with `kCustomScreenIdPrefix`.
[[nodiscard]] inline bool isCustomScreenId(std::string_view screenId) noexcept {
    return screenId.starts_with(kCustomScreenIdPrefix);
}

/// @brief The screen a route names.
/// @param route The route.
/// @return The route up to its `?`, or the whole route without one.
[[nodiscard]] inline std::string_view routeScreen(std::string_view route) noexcept {
    return route.substr(0, route.find('?'));
}

/// @brief The custom documents a client has fetched, by id: what `DocumentScreenHostOptions::documents` answers for a
///        `custom:` id.
///
/// The documents lookup is synchronous, so whatever opens a custom screen (a navigator entry, **Open as**, a restored
/// tab) has its document fetched into the cache first. A list that shows a kept screen with another hash, or a whole
/// list that no longer shows it, drops it, so the next open fetches the stored document again.
class MORPH_API CustomDocuments {
public:
    /// @brief One fetched screen.
    struct Entry {
        /// @brief Its summary, as `get` answered it.
        CustomScreenSummary summary;
        /// @brief Its document's JSON text.
        std::string text;
        /// @brief Its document, loaded.
        std::shared_ptr<Document const> document;
    };

    /// @brief Loads what `get` answered and keeps it under its id, replacing what was kept.
    /// @param found   The reply.
    /// @param options How documents load: the catalog facts.
    /// @return The loaded document, or why it does not load in this client, or names another id; nothing is kept then.
    std::expected<std::shared_ptr<Document const>, Refusal> put(CustomScreenFound found, LoadOptions const& options);

    /// @brief The loaded document of a kept screen.
    /// @param id The screen's id.
    /// @return It, or null when the screen is not kept.
    [[nodiscard]] std::shared_ptr<Document const> find(std::string_view id) const;

    /// @brief A kept screen.
    /// @param id The screen's id.
    /// @return It, valid until the cache next changes; null when the screen is not kept.
    [[nodiscard]] Entry const* entry(std::string_view id) const;

    /// @brief Drops each kept screen @p listed shows with another hash and, unless the list was truncated, each it does
    ///        not show.
    /// @param listed What `list` answered.
    /// @return The ids dropped.
    std::vector<std::string> refresh(CustomScreensListed const& listed);

    /// @brief Drops a kept screen.
    /// @param id The screen's id; one not kept changes nothing.
    void forget(std::string_view id);

    /// @brief Drops every kept screen: what a change of session does.
    void clear() noexcept { _entries.clear(); }

private:
    std::map<std::string, Entry, std::less<>> _entries;
};

/// @brief Whether a server offers custom screens, as far as a client knows.
enum class CustomScreensOffer : std::uint8_t {
    Unknown,  ///< Not asked yet, or asked under a session that has since changed.
    Offered,  ///< `list` answered.
    Off,      ///< The registration of `morph.customScreens`, or `list`, was refused.
};

/// @brief What an open or a fetch answers: the loaded document, or why there is none.
using CustomDocumentDone = std::function<void(std::expected<std::shared_ptr<Document const>, std::string>)>;

/// @brief The client of a server's custom screens model (`morph.customScreens`) for one application, through the
///        interpreter's own `Dispatcher`, so it travels the same bridge, session and backend as the screens' models.
///
/// No call names a person: the server keys every screen by the verified session's principal. Every reply arrives on
/// the runtime's owner. Replies still in flight when the sync is destroyed or `reset` are dropped.
class MORPH_API CustomScreenSync {
public:
    /// @brief A sync for @p application.
    /// @param dispatcher  Where model calls go: the interpreter's own. Borrowed: it must outlive the sync.
    /// @param application The application's id, the `app` of every call.
    /// @param catalog     The catalog facts a fetched document loads with, read at each load.
    /// @param watcher     What sends a watch on the record `morph.customScreens`, so a save from anywhere relists;
    ///                    empty for a source whose server sends no events, such as an in-process one.
    CustomScreenSync(Dispatcher& dispatcher MORPH_LIFETIMEBOUND, std::string application,
                     std::function<Catalog()> catalog, Watcher watcher = {});

    /// @brief Asks whether the server offers custom screens, by calling `list`.
    ///
    /// The list answering makes the offer `Offered`, keeps the list, tells `setOnListed`'s handler and watches the
    /// record. Any refusal makes it `Off`: a server that never registered the model refuses its registration with
    /// `unknown model type: morph.customScreens`, which is how a client learns custom screens are not offered; any other
    /// refusal is logged at warn too, since it is a misconfiguration rather than an absence.
    /// @param done Called once with whether they are offered; may be empty.
    void probe(std::function<void(bool offered)> done = {});

    /// @brief What the last probe found.
    /// @return The offer.
    [[nodiscard]] CustomScreensOffer offer() const noexcept { return _offer; }

    /// @brief Why the offer is `Off`.
    /// @return The refusal's text; empty unless the offer is `Off`.
    [[nodiscard]] std::string const& offRefusal() const noexcept { return _offRefusal; }

    /// @brief Sets what is told each list the sync receives, from a probe or a relist.
    /// @param onListed The handler; empty tells nothing.
    void setOnListed(std::function<void(CustomScreensListed const&)> onListed) { _onListed = std::move(onListed); }

    /// @brief The last list received.
    /// @return It; empty before the first.
    [[nodiscard]] CustomScreensListed const& listed() const noexcept { return _listed; }

    /// @brief Calls `list` again, while the offer is `Offered`: the cache drops what the list shows changed, and
    ///        `setOnListed`'s handler is told. A refusal is logged at warn and keeps the last list.
    void relist();

    /// @brief The document of a custom screen: the kept one, or `fetch`ed.
    /// @param id   The screen's id.
    /// @param done Called once with the document, or why there is none.
    void open(std::string const& id, CustomDocumentDone done);

    /// @brief Calls `get` and keeps the document it answers, replacing a kept one.
    /// @param id   The screen's id.
    /// @param done Called once with the document, or why there is none: the store's refusal as it gave it, or why the
    ///             document does not load in this client.
    void fetch(std::string const& id, CustomDocumentDone done);

    /// @brief Calls `get` and keeps nothing: what a source view of a broken screen reads.
    /// @param id   The screen's id.
    /// @param done Called once with the summary and the document, or the refusal.
    void get(std::string const& id, std::function<void(std::expected<CustomScreenFound, std::string>)> done);

    /// @brief Asks the server whether @p document would be saved as a custom screen based on @p basedOn.
    /// @param basedOn  The base screen's id.
    /// @param document The document's JSON text.
    /// @param done     Called once with the first refusal, or none, or why the call failed.
    // NOLINTNEXTLINE(bugprone-easily-swappable-parameters): the `check` action's order.
    void check(std::string basedOn, std::string document,
               std::function<void(std::expected<std::optional<CheckRefusal>, std::string>)> done);

    /// @brief Saves a custom screen; its `app` is set to the sync's. A save that is answered relists.
    /// @param request The screen to overwrite, if any, its title, base and document.
    /// @param done    Called once with the screen's id, or the refusal as the store or the checks gave it.
    void save(CustomScreensSave request, std::function<void(std::expected<std::string, std::string>)> done);

    /// @brief Fetches the document of every custom screen @p routes name that is not kept, so a start that reopens
    ///        them can read the routes back.
    /// @param routes The routes, a workspace's record or a history's.
    /// @param done   Called once every fetch has answered, with the ids no document came for, in the routes' order.
    void prefetchRoutes(std::vector<std::string> const& routes, std::function<void(std::vector<std::string>)> done);

    /// @brief The connection was made again: while offered, watches the record again (a reconnect replaces the
    ///        connection's watch set) and relists.
    void reconnected();

    /// @brief A server event: a `changes` naming the record `morph.customScreens` relists, and a `resync` watches
    ///        again and relists. Anything else, or anything before the offer is `Offered`, is ignored.
    /// @param event The event.
    void deliverEvent(wire::EventBody const& event);

    /// @brief Forgets everything learnt under the session that is ending: the offer, the list, the cache and the
    ///        binding. Replies still in flight are dropped.
    void reset();

    /// @brief The fetched documents.
    /// @return The cache.
    [[nodiscard]] CustomDocuments& documents() noexcept { return _documents; }

    /// @brief The fetched documents.
    /// @return The cache.
    [[nodiscard]] CustomDocuments const& documents() const noexcept { return _documents; }

private:
    // Sends one action through the binding, made at the first call; @p ok gets the reply's JSON, @p failed the reason.
    void call(std::string_view action, std::string const& body, std::function<void(std::string const&)> ok,
              std::function<void(std::string const&)> const& failed);
    void watch();
    void off(std::string reason);
    void listedNow(CustomScreensListed listed);

    Dispatcher* _dispatcher;
    std::string _application;
    std::function<Catalog()> _catalog;
    Watcher _watcher;
    std::shared_ptr<ModelChannel> _channel;
    CustomScreensOffer _offer = CustomScreensOffer::Unknown;
    std::string _offRefusal;
    CustomScreensListed _listed;
    std::function<void(CustomScreensListed const&)> _onListed;
    CustomDocuments _documents;
    // Last, so it stops every delivery before anything a reply would touch goes; `reset` replaces it.
    std::unique_ptr<async::CallbackScope> _scope = std::make_unique<async::CallbackScope>();
};

}  // namespace morph::ui
```

- [ ] **Step 5: Write the implementation** (`src/ui/custom_screen_sync.cpp`)

```cpp
// SPDX-License-Identifier: Apache-2.0

#include <cstddef>
#include <exception>
#include <expected>
#include <functional>
#include <glaze/glaze.hpp>
#include <memory>
#include <morph/core/logger.hpp>
#include <morph/core/wire.hpp>
#include <morph/ui/custom_screen_sync.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace morph::ui {

std::expected<std::shared_ptr<Document const>, Refusal> CustomDocuments::put(CustomScreenFound found,
                                                                             LoadOptions const& options) {
    auto loaded = loadDocumentText(found.document, options);
    if (!loaded) {
        return std::unexpected(std::move(loaded.error()));
    }
    if (loaded->id != found.summary.id) {
        return std::unexpected(Refusal{.document = found.summary.id,
                                       .path = "/id",
                                       .reason = "the stored document names another screen, " + loaded->id});
    }
    auto document = std::make_shared<Document const>(std::move(*loaded));
    std::string const id = found.summary.id;
    _entries.insert_or_assign(id, Entry{.summary = std::move(found.summary),
                                        .text = std::move(found.document),
                                        .document = document});
    return document;
}

std::shared_ptr<Document const> CustomDocuments::find(std::string_view id) const {
    auto const found = _entries.find(id);
    return found == _entries.end() ? nullptr : found->second.document;
}

CustomDocuments::Entry const* CustomDocuments::entry(std::string_view id) const {
    auto const found = _entries.find(id);
    return found == _entries.end() ? nullptr : &found->second;
}

std::vector<std::string> CustomDocuments::refresh(CustomScreensListed const& listed) {
    std::vector<std::string> dropped;
    for (auto kept = _entries.begin(); kept != _entries.end();) {
        auto const shown = std::ranges::find(listed.screens, kept->first, &CustomScreenSummary::id);
        bool const gone = shown == listed.screens.end() ? !listed.truncated : shown->hash != kept->second.summary.hash;
        if (gone) {
            dropped.push_back(kept->first);
            kept = _entries.erase(kept);
        } else {
            ++kept;
        }
    }
    return dropped;
}

void CustomDocuments::forget(std::string_view id) {
    if (auto const found = _entries.find(id); found != _entries.end()) {
        _entries.erase(found);
    }
}

CustomScreenSync::CustomScreenSync(Dispatcher& dispatcher MORPH_LIFETIMEBOUND, std::string application,
                                   std::function<Catalog()> catalog, Watcher watcher)
    : _dispatcher{&dispatcher},
      _application{std::move(application)},
      _catalog{std::move(catalog)},
      _watcher{std::move(watcher)} {}

void CustomScreenSync::call(std::string_view action, std::string const& body,
                            std::function<void(std::string const&)> ok,
                            std::function<void(std::string const&)> const& failed) {
    try {
        if (_channel == nullptr) {
            _channel = _dispatcher->bind(std::string{kCustomScreensModel}, bridge::BindSharing::Private, {});
        }
        _channel->execute(std::string{action}, body, CallOptions{})
            .then(*_scope, [ok = std::move(ok)](std::string const& reply) { ok(reply); })
            .onError(*_scope, [failed](std::exception_ptr const& error) { failed(detail::failureText(error)); });
    } catch (std::exception const& error) {
        failed(error.what());
    }
}

void CustomScreenSync::probe(std::function<void(bool offered)> done) {
    auto const shared = std::make_shared<std::function<void(bool)>>(std::move(done));
    call(
        kCustomScreensList, model::ActionTraits<CustomScreensList>::toJson(CustomScreensList{.app = _application}),
        [this, shared](std::string const& reply) {
            CustomScreensListed listed;
            try {
                listed = model::ActionTraits<CustomScreensList>::resultFromJson(reply);
            } catch (std::exception const& error) {
                off(std::string{"not a custom screens list: "} + error.what());
                if (*shared) {
                    (*shared)(false);
                }
                return;
            }
            _offer = CustomScreensOffer::Offered;
            _offRefusal.clear();
            watch();
            listedNow(std::move(listed));
            if (*shared) {
                (*shared)(true);
            }
        },
        [this, shared](std::string const& reason) {
            off(reason);
            if (*shared) {
                (*shared)(false);
            }
        });
}

void CustomScreenSync::off(std::string reason) {
    _offer = CustomScreensOffer::Off;
    // A server without the model is the ordinary way to say custom screens are not offered; anything else is a
    // server that has the model and refuses it, which someone has to hear about.
    if (!reason.contains("unknown model type")) {
        ::morph::log::logWarn("custom screens are off: the server refused `{}`: {}", kCustomScreensList, reason);
    }
    _offRefusal = std::move(reason);
}

void CustomScreenSync::relist() {
    if (_offer != CustomScreensOffer::Offered) {
        return;
    }
    call(
        kCustomScreensList, model::ActionTraits<CustomScreensList>::toJson(CustomScreensList{.app = _application}),
        [this](std::string const& reply) {
            try {
                listedNow(model::ActionTraits<CustomScreensList>::resultFromJson(reply));
            } catch (std::exception const& error) {
                ::morph::log::logWarn("custom screens: not a list from `{}`: {}", kCustomScreensList, error.what());
            }
        },
        [](std::string const& reason) {
            ::morph::log::logWarn("custom screens: `{}` was refused, the last list is kept: {}", kCustomScreensList,
                                  reason);
        });
}

void CustomScreenSync::listedNow(CustomScreensListed listed) {
    static_cast<void>(_documents.refresh(listed));
    _listed = std::move(listed);
    if (_onListed) {
        _onListed(_listed);
    }
}

void CustomScreenSync::open(std::string const& id, CustomDocumentDone done) {
    if (std::shared_ptr<Document const> kept = _documents.find(id)) {
        done(std::move(kept));
        return;
    }
    fetch(id, std::move(done));
}

void CustomScreenSync::fetch(std::string const& id, CustomDocumentDone done) {
    auto const shared = std::make_shared<CustomDocumentDone>(std::move(done));
    get(id, [this, shared](std::expected<CustomScreenFound, std::string> found) {
        if (!found) {
            (*shared)(std::unexpected(std::move(found.error())));
            return;
        }
        auto loaded = _documents.put(std::move(*found), LoadOptions{.catalog = _catalog ? _catalog() : Catalog{}});
        if (!loaded) {
            (*shared)(std::unexpected(loaded.error().message()));
            return;
        }
        (*shared)(std::move(*loaded));
    });
}

void CustomScreenSync::get(std::string const& id,
                           std::function<void(std::expected<CustomScreenFound, std::string>)> done) {
    auto const shared =
        std::make_shared<std::function<void(std::expected<CustomScreenFound, std::string>)>>(std::move(done));
    call(
        kCustomScreensGet,
        model::ActionTraits<CustomScreensGet>::toJson(CustomScreensGet{.app = _application, .id = id}),
        [shared](std::string const& reply) {
            try {
                (*shared)(model::ActionTraits<CustomScreensGet>::resultFromJson(reply));
            } catch (std::exception const& error) {
                (*shared)(std::unexpected(std::string{"not a custom screen: "} + error.what()));
            }
        },
        [shared](std::string const& reason) { (*shared)(std::unexpected(reason)); });
}

void CustomScreenSync::check(std::string basedOn, std::string document,
                             std::function<void(std::expected<std::optional<CheckRefusal>, std::string>)> done) {
    auto const shared =
        std::make_shared<std::function<void(std::expected<std::optional<CheckRefusal>, std::string>)>>(
            std::move(done));
    call(
        kCustomScreensCheck,
        model::ActionTraits<CustomScreensCheck>::toJson(
            CustomScreensCheck{.app = _application, .basedOn = std::move(basedOn), .document = std::move(document)}),
        [shared](std::string const& reply) {
            try {
                (*shared)(model::ActionTraits<CustomScreensCheck>::resultFromJson(reply).refusal);
            } catch (std::exception const& error) {
                (*shared)(std::unexpected(std::string{"not a check: "} + error.what()));
            }
        },
        [shared](std::string const& reason) { (*shared)(std::unexpected(reason)); });
}

void CustomScreenSync::save(CustomScreensSave request,
                            std::function<void(std::expected<std::string, std::string>)> done) {
    auto const shared = std::make_shared<std::function<void(std::expected<std::string, std::string>)>>(std::move(done));
    request.app = _application;
    call(
        kCustomScreensSave, model::ActionTraits<CustomScreensSave>::toJson(request),
        [this, shared](std::string const& reply) {
            CustomScreenSaved saved;
            try {
                saved = model::ActionTraits<CustomScreensSave>::resultFromJson(reply);
            } catch (std::exception const& error) {
                (*shared)(std::unexpected(std::string{"not a save: "} + error.what()));
                return;
            }
            relist();
            (*shared)(std::move(saved.id));
        },
        [shared](std::string const& reason) { (*shared)(std::unexpected(reason)); });
}

void CustomScreenSync::prefetchRoutes(std::vector<std::string> const& routes,
                                      std::function<void(std::vector<std::string>)> done) {
    std::vector<std::string> wanted;
    for (std::string const& route : routes) {
        std::string const id{routeScreen(route)};
        if (isCustomScreenId(id) && _documents.find(id) == nullptr && std::ranges::find(wanted, id) == wanted.end()) {
            wanted.push_back(id);
        }
    }
    if (wanted.empty()) {
        done({});
        return;
    }
    struct Pending {
        std::vector<std::string> ids;
        std::vector<bool> missing;
        std::size_t left = 0;
        std::function<void(std::vector<std::string>)> done;
    };
    auto const pending = std::make_shared<Pending>(Pending{
        .ids = wanted, .missing = std::vector<bool>(wanted.size(), false), .left = wanted.size(), .done = std::move(done)});
    for (std::size_t place = 0; place < wanted.size(); ++place) {
        fetch(wanted.at(place),
              [pending, place](std::expected<std::shared_ptr<Document const>, std::string> const& loaded) {
                  pending->missing.at(place) = !loaded.has_value();
                  if (--pending->left > 0) {
                      return;
                  }
                  std::vector<std::string> missing;
                  for (std::size_t each = 0; each < pending->ids.size(); ++each) {
                      if (pending->missing.at(each)) {
                          missing.push_back(pending->ids.at(each));
                      }
                  }
                  pending->done(std::move(missing));
              });
    }
}

void CustomScreenSync::watch() {
    if (!_watcher) {
        return;
    }
    wire::WatchBody const body{.records = {std::string{kCustomScreenRecord}}, .instances = {}, .replace = false};
    _watcher(std::string{wire::kWatchKind}, glz::write_json(body).value_or(std::string{}))
        .then(*_scope,
              [](std::string const& reply) {
                  auto const read = wire::readWatchReply(reply);
                  if (read && std::ranges::find(read->refusedRecords, std::string{kCustomScreenRecord}) !=
                                  read->refusedRecords.end()) {
                      ::morph::log::logWarn(
                          "custom screens: the server refused a watch on `{}`; lists refresh only on sign-in, "
                          "reconnect and this client's own saves",
                          kCustomScreenRecord);
                  }
              })
        .onError(*_scope, [](std::exception_ptr const& error) {
            ::morph::log::logWarn("custom screens: the watch on `{}` failed: {}", kCustomScreenRecord,
                                  detail::failureText(error));
        });
}

void CustomScreenSync::reconnected() {
    if (_offer != CustomScreensOffer::Offered) {
        return;
    }
    watch();
    relist();
}

void CustomScreenSync::deliverEvent(wire::EventBody const& event) {
    if (_offer != CustomScreensOffer::Offered) {
        return;
    }
    if (event.topic == wire::kResyncTopic) {
        watch();
        relist();
        return;
    }
    if (event.topic != wire::kChangesTopic) {
        return;
    }
    auto const changes = wire::readChangesBody(event.payload.str);
    if (changes && std::ranges::any_of(changes->records, [](auto const& change) {
            return change.record == kCustomScreenRecord;
        })) {
        relist();
    }
}

void CustomScreenSync::reset() {
    _scope = std::make_unique<async::CallbackScope>();
    _channel.reset();
    _offer = CustomScreensOffer::Unknown;
    _offRefusal.clear();
    _listed = {};
    _documents.clear();
}

}  // namespace morph::ui
```

Add `#include <algorithm>` at the top of the file (for `std::ranges::find` and `any_of`). Add the source to
`add_library(morph …)` after `src/ui/json_source.cpp`, and the header to `FILE_SET HEADERS` after
`include/morph/ui/json_source.hpp`.

- [ ] **Step 6: Run the tests**

Run: `flock "$LOCK" cmake --build build/clang-debug --target morph_tests -j5 && flock "$LOCK" timeout 300 build/clang-debug/tests/morph_tests "[sync]"`
Expected: `All tests passed`. If the first test's wording check fails, do not change the check: print the refusal
(the `INFO` line shows it) and report that the spec's discovery wording does not hold on this transport.

- [ ] **Step 7: Mutation check.** Apply the seven mutations named above the tests in turn; each named test fails.
  Restore and `touch src/ui/custom_screen_sync.cpp`.

- [ ] **Step 8: Commit**

```bash
git add include/morph/ui/custom_screen_sync.hpp src/ui/custom_screen_sync.cpp tests/ui_custom_client_support.hpp \
  tests/test_ui_custom_screen_sync.cpp CMakeLists.txt tests/CMakeLists.txt
git commit -m "ui: the client of the custom screens model, and its documents cache"
```

---
### Task 7: `CustomScreenChrome` — Source, drafts and saving

**Files:**
- Create: `include/morph/ui/custom_screen_chrome.hpp`, `src/ui/custom_screen_chrome.cpp`
- Modify: `CMakeLists.txt` (the source after `src/ui/custom_screen_sync.cpp`, the header after
  `include/morph/ui/custom_screen_sync.hpp`)
- Modify: `tests/ui_custom_client_support.hpp` (the notes bundle and the chrome rig)
- Test: `tests/test_ui_custom_screen_chrome.cpp`; `tests/CMakeLists.txt`

**Interfaces:**
- Consumes: `prettyJson`, `pointerOffset`, `lineAt` (Task 1); `Mounted::placeCursor` (Task 2);
  `DocumentScreenHost::mountDraft`, `discardDraft`, `endDraft`, `drafted`, `screenIdOf`, `paramsOf` (Task 3);
  `WorkspaceModel::openInPlace`, `WorkspaceOptions::chrome` (Task 4); `CustomScreenSync`, `CustomDocuments`,
  `isCustomScreenId` (Task 6).
- Produces:
  - `struct CustomScreenChromeOptions { reactive::Runtime* runtime; IViewBackend* backend; DocumentScreenHost* host; CustomScreenSync* sync; std::function<std::shared_ptr<Document const>(std::string_view)> documents; std::function<std::string(std::string_view)> sourceOf; std::function<Catalog()> catalog; std::function<std::string(std::string_view)> titleOf; std::function<void(std::string const&, ParamMap const&)> openTab; std::function<void(std::string const&, Tone)> notify; std::function<void()> onListChanged; ContainerWidget* overlayParent; }`
  - `class CustomScreenChrome` with `attach(WorkspaceModel*)`, `setOffered(bool)`, `listed(CustomScreensListed const&)`,
    `chromeFor(WorkspaceTabId, std::string_view) -> Node`, `showSource(WorkspaceTabId)`,
    `preview(std::function<void(bool)> = {})`, `askSaveAs(WorkspaceTabId, bool fromSource)`, `confirmSaveAs()`,
    `save(WorkspaceTabId)`, `discard(WorkspaceTabId)`
  - Test ids: per tab `screen_chrome_<id>`, `draft_bar_<id>`, `draft_note_<id>`, `draft_message_<id>`,
    `draft_save_<id>`, `draft_save_as_<id>`, `draft_source_<id>`, `draft_discard_<id>`, `screen_tools_<id>`,
    `source_button_<id>`; overlays `source_dialog`, `source_text`, `source_refusal`, `source_status`,
    `source_preview`, `source_save_as`, `source_close`, `save_as_dialog`, `save_as_title`, `save_as_message`,
    `save_as_confirm`, `save_as_cancel`.

- [ ] **Step 1: Add the rig to the test support.** At the top of `tests/ui_custom_client_support.hpp`, add to the
  includes:

```cpp
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <functional>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <morph/ui/custom_screen_chrome.hpp>
#include <morph/ui/document.hpp>
#include <morph/ui/host.hpp>
#include <morph/ui/json_source.hpp>
#include <morph/ui/service.hpp>
#include <morph/ui/workspace.hpp>

#include "ui_interpreter_support.hpp"
```

(keep the include block sorted as clang-format orders it), and append before the closing
`}  // namespace custom_client`:

```cpp
/// A screen with a param, so Open as applies to it; its body is state a button changes.
inline constexpr std::string_view kEdit =
    R"({"vocab":"ui/1","id":"notes.edit","params":{"noteId":"int"},"identity":{"ref":"noteId"},"title":"Note",)"
    R"("state":{"body":{"type":"string","init":"first"}},)"
    R"("view":{"kind":"column","children":[{"kind":"text","testId":"heading","text":"Edit note"},)"
    R"({"kind":"text","testId":"body","text":{"ref":"body"}},)"
    R"({"kind":"button","testId":"change","label":"Change","onClick":[{"set":"body","value":"changed"}]}]}})";

/// A screen without params, so its custom screens are navigator entries.
inline constexpr std::string_view kHome =
    R"({"vocab":"ui/1","id":"notes.home","title":"Home","view":{"kind":"text","testId":"home","text":"Home"}})";

/// The notes application's bundle: the two screens, against the service fixture's facts.
inline std::shared_ptr<morph::ui::ServedBundle const> notesBundle() {
    morph::ui::UiRegistry registry{morph::ui::ApplicationInfo{.id = "notes", .version = "1.0.0"}, uiservice::facts()};
    for (auto const& [id, text] : {std::pair{std::string_view{"notes.edit"}, kEdit},
                                   std::pair{std::string_view{"notes.home"}, kHome}}) {
        auto const added = registry.addScreen(uiservice::entry(std::string{id}), std::string{text});
        if (!added) {
            throw std::runtime_error{added.error().message()};
        }
    }
    auto built = registry.build();
    if (!built) {
        throw std::runtime_error{built.error().message()};
    }
    return *built;
}

/// @p text loaded as the client loads the notes documents.
inline std::shared_ptr<morph::ui::Document const> loadedNotes(std::string_view text) {
    auto loaded = morph::ui::loadDocumentText(text, morph::ui::LoadOptions{.catalog = uiservice::facts()});
    if (!loaded) {
        FAIL("fixture does not load: " << loaded.error().message());
    }
    return std::make_shared<morph::ui::Document const>(std::move(*loaded));
}

/// A text input's text as RecordingBackend records it, with its escapes undone.
inline std::string unformatted(std::string_view recorded) {
    std::string out;
    for (std::size_t at = 0; at < recorded.size(); ++at) {
        if (recorded[at] == '\\' && at + 1 < recorded.size()) {
            char const next = recorded[++at];
            out += next == 'n' ? '\n' : next == 'r' ? '\r' : next;
            continue;
        }
        out += recorded[at];
    }
    return out;
}

inline morph::ui::ParamMap note(std::int64_t id) { return morph::ui::ParamMap{{"noteId", morph::ui::Value{id}}}; }

/// A host whose navigation goes nowhere: the chrome opens tabs through its own `openTab`.
class NotesHost final : public morph::ui::DocumentScreenHost {
public:
    NotesHost(morph::ui::testing::Harness& harness, morph::ui::DocumentScreenHostOptions options)
        : DocumentScreenHost{harness.session, harness.dispatcher, std::move(options)} {}

    void onNavigate(std::string_view /*screenId*/, morph::ui::ParamMap const& /*params*/) override {}

    using DocumentScreenHost::onNavigate;
};

/// Whether the rig's server offers custom screens.
enum class Offer : std::uint8_t { Store, NoStore };

/// One window of the notes application signed in as `ann`: a workspace over the notes screens, the chrome over it,
/// and a sync to a notes server that offers custom screens (over @p store when one is given) or does not.
struct ChromeRig {
    explicit ChromeRig(Offer offer = Offer::Store, std::shared_ptr<morph::ui::ICustomScreenStore> store = nullptr) {
        if (offer == Offer::Store) {
            server.emplace(std::move(store), morph::ui::CustomScreensOptions{}, notesBundle());
            remote = server->server.get();
        } else {
            bare.emplace();
            remote = bare->server.get();
        }
        bridge = std::make_unique<morph::bridge::Bridge>(
            std::make_unique<morph::backend::SimulatedRemoteBackend>(*remote), harness.owner);
        morph::session::Context signedIn;
        signedIn.token = "ann";
        bridge->setDefaultSession(std::move(signedIn));
        models = std::make_unique<morph::ui::BridgeDispatcher>(*bridge, &harness.owner);
        sync = std::make_unique<morph::ui::CustomScreenSync>(*models, "notes", [] { return uiservice::facts(); });
        host = std::make_unique<NotesHost>(harness, hostOptions());
        workspace =
            std::make_unique<morph::ui::Workspace>(harness.runtime, harness.backend, *host, workspaceOptions());
        chrome = std::make_unique<morph::ui::CustomScreenChrome>(chromeOptions());
        chrome->attach(&workspace->model());
        sync->setOnListed([this](morph::ui::CustomScreensListed const& listed) { chrome->listed(listed); });
        std::optional<bool> offered;
        sync->probe([this, &offered](bool answer) {
            chrome->setOffered(answer);
            offered = answer;
        });
        REQUIRE(pump([&offered] { return offered.has_value(); }));
    }

    /// Runs the owner until @p done holds, for at most @p budget.
    template <typename Pred>
    bool pump(Pred done, std::chrono::milliseconds budget = morph::testing::kDefaultWaitBudget) {
        return morph::testing::waitUntil(
            [&] {
                harness.owner.runAll();
                return done();
            },
            morph::testing::WaitBudget{budget});
    }

    [[nodiscard]] std::optional<int> find(std::string_view kind, std::string const& testId) const {
        return harness.backend.find(kind, "testId", testId);
    }

    /// Whether the widget is mounted and not hidden itself.
    [[nodiscard]] bool shown(std::string_view kind, std::string const& testId) const {
        std::optional<int> const widget = find(kind, testId);
        return widget && harness.backend.prop(*widget, "visible") != "false";
    }

    void click(std::string const& testId) {
        std::optional<int> const widget = find("Button", testId);
        REQUIRE(widget.has_value());
        harness.backend.click(*widget);
        harness.settle();
    }

    void type(std::string const& testId, std::string text) {
        std::optional<int> const widget = find("TextInput", testId);
        REQUIRE(widget.has_value());
        harness.backend.edit(*widget, std::move(text));
        harness.settle();
    }

    [[nodiscard]] std::string text(std::string const& testId) const { return harness.text(testId); }

    /// A text input's text.
    [[nodiscard]] std::string field(std::string const& testId) const {
        std::optional<int> const widget = find("TextInput", testId);
        REQUIRE(widget.has_value());
        return unformatted(harness.backend.prop(*widget, "text"));
    }

    void open(std::string_view screenId, morph::ui::ParamMap const& params) {
        REQUIRE(workspace->open(screenId, params).has_value());
        harness.settle();
    }

    [[nodiscard]] std::string screenOf(std::size_t index) const { return workspace->tabs().at(index).screenId; }

    /// Saves a custom screen of `ann`'s on the server, and waits for the sync's list to show it.
    std::string saveOnServer(std::string const& title, std::string const& basedOn, std::string const& document) {
        REQUIRE(server.has_value());
        std::string const id = savedIdOf(
            server->call(morph::ui::kCustomScreensSave, saveBody("notes", title, basedOn, document), "ann"));
        sync->relist();
        REQUIRE(pump([this, &id] {
            return std::ranges::any_of(sync->listed().screens,
                                       [&id](morph::ui::CustomScreenSummary const& screen) { return screen.id == id; });
        }));
        return id;
    }

    morph::ui::DocumentScreenHostOptions hostOptions() {
        morph::ui::DocumentScreenHostOptions made;
        made.documents = [this](std::string_view id) -> std::shared_ptr<morph::ui::Document const> {
            if (morph::ui::isCustomScreenId(id)) {
                return sync->documents().find(id);
            }
            return id == "notes.edit" ? edit : id == "notes.home" ? home : nullptr;
        };
        made.backend = &harness.backend;
        made.application = &harness.host;
        made.guard = [this](morph::ui::ScreenHandle& /*screen*/,
                            std::function<void(morph::ui::CloseChoice)> const& choose) {
            ++guarded;
            choose(guardChoice);
        };
        return made;
    }

    morph::ui::WorkspaceOptions workspaceOptions() {
        morph::ui::WorkspaceOptions made;
        made.chrome = [this](morph::ui::WorkspaceTabId tab, std::string_view screenId) {
            return chrome->chromeFor(tab, screenId);
        };
        return made;
    }

    morph::ui::CustomScreenChromeOptions chromeOptions() {
        morph::ui::CustomScreenChromeOptions made;
        made.runtime = &harness.runtime;
        made.backend = &harness.backend;
        made.host = host.get();
        made.sync = sync.get();
        made.documents = [this](std::string_view id) -> std::shared_ptr<morph::ui::Document const> {
            return id == "notes.edit" ? edit : id == "notes.home" ? home : nullptr;
        };
        made.sourceOf = [](std::string_view id) {
            return id == "notes.edit" ? std::string{kEdit} : id == "notes.home" ? std::string{kHome} : std::string{};
        };
        made.catalog = [] { return uiservice::facts(); };
        made.titleOf = [](std::string_view id) { return id == "notes.edit" ? std::string{"Note"} : std::string{id}; };
        made.openTab = [this](std::string const& id, morph::ui::ParamMap const& params) {
            static_cast<void>(workspace->openTab(id, params));
        };
        made.notify = [this](std::string const& text, morph::ui::Tone /*tone*/) { notices.push_back(text); };
        return made;
    }

    morph::ui::testing::Harness harness;
    std::optional<screens_test::Server> server;
    std::optional<BareServer> bare;
    morph::backend::RemoteServer* remote = nullptr;
    std::unique_ptr<morph::bridge::Bridge> bridge;
    std::unique_ptr<morph::ui::BridgeDispatcher> models;
    std::unique_ptr<morph::ui::CustomScreenSync> sync;
    std::shared_ptr<morph::ui::Document const> edit = loadedNotes(kEdit);
    std::shared_ptr<morph::ui::Document const> home = loadedNotes(kHome);
    std::unique_ptr<NotesHost> host;
    std::unique_ptr<morph::ui::Workspace> workspace;
    std::unique_ptr<morph::ui::CustomScreenChrome> chrome;
    std::vector<std::string> notices;
    int guarded = 0;
    morph::ui::CloseChoice guardChoice = morph::ui::CloseChoice::Cancel;
};

/// Opens @p screenId's source, renames the heading "Edit note" to @p heading and previews it, until the draft shows and
/// the source view has closed.
inline void draftHeading(ChromeRig& rig, std::string const& screenId, std::string const& heading) {
    rig.click("source_button_" + screenId);
    rig.type("source_text", screens_test::replaced(rig.field("source_text"), "\"Edit note\"", "\"" + heading + "\""));
    rig.click("source_preview");
    REQUIRE(rig.pump([&rig, &heading] { return rig.text("heading") == heading; }));
    REQUIRE(rig.pump([&rig] { return !rig.find("Dialog", "source_dialog").has_value(); }));
}
```

(The members are declared in the order they must be destroyed in reverse: the chrome first, the servers and the
harness last.)

- [ ] **Step 2: Write the failing tests** (`tests/test_ui_custom_screen_chrome.cpp`)

```cpp
// SPDX-License-Identifier: Apache-2.0
//
// The custom screens chrome (docs/spec/ui/custom_screens.md, "The client"): Source on every document screen, Preview
// through the client's load and the server's check into a draft, the refusal and its line, the draft bar's Save,
// Save as… and Discard, and the store's refusal shown to the person.

#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstddef>
#include <memory>
#include <morph/core/executor.hpp>
#include <morph/ui/custom_screen_chrome.hpp>
#include <morph/ui/host.hpp>
#include <morph/ui/json_source.hpp>
#include <optional>
#include <string>

#include "ui_custom_client_support.hpp"

using custom_client::ChromeRig;
using custom_client::draftHeading;
using custom_client::kEdit;
using custom_client::note;
using custom_client::Offer;
using morph::ui::DocumentScreenHost;
using morph::ui::ParamMap;

// Mutation: skip mountDraft after the server's check (the heading stays "Edit note").
TEST_CASE("custom screen chrome: Source shows the screen's document, and Preview mounts the edit as the window's draft",
          "[ui][custom-screens][chrome]") {
    ChromeRig rig;
    rig.open("notes.edit", note(1));
    rig.click("change");
    REQUIRE(rig.text("body") == "changed");
    CHECK_FALSE(rig.shown("Row", "draft_bar_notes.edit"));

    rig.click("source_button_notes.edit");
    std::string const shown = rig.field("source_text");
    CHECK(shown == morph::ui::prettyJson(kEdit));
    rig.type("source_text", screens_test::replaced(shown, "\"Edit note\"", "\"Edited by hand\""));
    rig.click("source_preview");
    REQUIRE(rig.pump([&rig] { return rig.text("heading") == "Edited by hand"; }));
    CHECK(rig.text("body") == "changed");
    CHECK(rig.shown("Row", "draft_bar_notes.edit"));
    CHECK_FALSE(rig.shown("Row", "screen_tools_notes.edit"));
    CHECK(rig.screenOf(0) == "notes.edit");
    REQUIRE(rig.pump([&rig] { return !rig.find("Dialog", "source_dialog").has_value(); }));
}

// Mutations: skip placeCursor when the refusal is chosen (the cursor stays at the end of the text); skip the client's id
// check (the host refuses the mount instead, with no pointer, so the label names no line of `"id"`).
TEST_CASE("custom screen chrome: a refusal names its line, and choosing it moves the cursor there",
          "[ui][custom-screens][chrome]") {
    ChromeRig rig;
    rig.open("notes.edit", note(1));
    rig.click("source_button_notes.edit");
    std::string const edited = screens_test::replaced(rig.field("source_text"), "\"notes.edit\"", "\"notes.other\"");
    rig.type("source_text", edited);
    rig.click("source_preview");
    REQUIRE(rig.pump([&rig] { return rig.shown("Button", "source_refusal"); }));
    std::size_t const idAt = edited.find("\"id\"");
    int const refusal = *rig.find("Button", "source_refusal");
    CHECK(rig.harness.backend.prop(refusal, "label")
              .starts_with("Line " + std::to_string(morph::ui::lineAt(edited, idAt)) + ":"));
    rig.click("source_refusal");
    CHECK(rig.harness.backend.cursor(*rig.find("TextInput", "source_text")) == idAt);
    CHECK(rig.text("heading") == "Edit note");
    CHECK_FALSE(rig.shown("Row", "draft_bar_notes.edit"));
}

// Mutation: skip the server's check (the printing document mounts as the draft).
TEST_CASE("custom screen chrome: what the client cannot load, and what the server refuses, stay out of the window",
          "[ui][custom-screens][chrome]") {
    ChromeRig rig;
    rig.open("notes.edit", note(1));
    rig.click("source_button_notes.edit");
    rig.type("source_text", screens_test::replaced(rig.field("source_text"), "\"column\"", "\"hologram\""));
    rig.click("source_preview");
    REQUIRE(rig.pump([&rig] { return rig.shown("Button", "source_refusal"); }));
    CHECK(rig.text("heading") == "Edit note");

    std::string const printing = screens_test::replaced(std::string{kEdit}, R"({"set":"body","value":"changed"})",
                                                        R"({"host":"print"})");
    rig.type("source_text", morph::ui::prettyJson(printing));
    CHECK_FALSE(rig.shown("Button", "source_refusal"));
    rig.click("source_preview");
    REQUIRE(rig.pump([&rig] { return rig.shown("Button", "source_refusal"); }));
    CHECK(rig.text("heading") == "Edit note");
    CHECK_FALSE(rig.shown("Row", "draft_bar_notes.edit"));
}

// Mutation: open the source editable whatever the offer (`readonly` is not set, and Preview is there).
TEST_CASE("custom screen chrome: without a store, Source is read only, with no Preview and no Save as",
          "[ui][custom-screens][chrome]") {
    ChromeRig rig{Offer::NoStore};
    CHECK(rig.sync->offer() == morph::ui::CustomScreensOffer::Off);
    rig.open("notes.edit", note(1));
    rig.click("source_button_notes.edit");
    REQUIRE(rig.find("TextInput", "source_text").has_value());
    CHECK(rig.harness.backend.prop(*rig.find("TextInput", "source_text"), "readonly") == "true");
    CHECK_FALSE(rig.find("Button", "source_preview").has_value());
    CHECK_FALSE(rig.find("Button", "source_save_as").has_value());
}

// Mutation: skip endDraft before opening the saved screen (the old tab's close goes to the guard, which keeps it, and the
// tab stays `notes.edit`).
TEST_CASE("custom screen chrome: Save as… names the draft, saves it, and the tab becomes the saved screen",
          "[ui][custom-screens][chrome]") {
    ChromeRig rig;
    rig.open("notes.edit", note(3));
    draftHeading(rig, "notes.edit", "Edited by hand");
    rig.click("draft_save_as_notes.edit");
    rig.type("save_as_title", "My note");
    rig.click("save_as_confirm");
    REQUIRE(rig.pump([&rig] { return morph::ui::isCustomScreenId(rig.screenOf(0)); }));
    CHECK(rig.workspace->tabs().size() == 1);
    CHECK(rig.text("heading") == "Edited by hand");
    std::string const id = rig.screenOf(0);
    CHECK_FALSE(rig.shown("Row", "draft_bar_" + id));
    CHECK(DocumentScreenHost::paramsOf(*rig.workspace->handleOf(0)) == std::optional<ParamMap>{note(3)});
    REQUIRE(rig.sync->listed().screens.size() == 1);
    CHECK(rig.sync->listed().screens.front().title == "My note");
    CHECK(rig.guarded == 0);
}

// Mutation: save from the source view without previewing (the refused document is saved: the list has a screen).
TEST_CASE("custom screen chrome: Save as… from the source view previews first, and saves nothing it refuses",
          "[ui][custom-screens][chrome]") {
    ChromeRig rig;
    rig.open("notes.edit", note(1));
    rig.click("source_button_notes.edit");
    rig.type("source_text", screens_test::replaced(rig.field("source_text"), "\"notes.edit\"", "\"notes.other\""));
    rig.click("source_save_as");
    rig.type("save_as_title", "Never");
    rig.click("save_as_confirm");
    REQUIRE(rig.pump([&rig] { return rig.shown("Button", "source_refusal"); }));
    static_cast<void>(rig.pump([] { return false; }, std::chrono::milliseconds{200}));
    CHECK(rig.sync->listed().screens.empty());

    rig.type("source_text", screens_test::replaced(rig.field("source_text"), "\"notes.other\"", "\"notes.edit\""));
    rig.type("save_as_title", "Mine");
    rig.click("save_as_confirm");
    REQUIRE(rig.pump([&rig] { return morph::ui::isCustomScreenId(rig.screenOf(0)); }));
    CHECK(rig.sync->listed().screens.size() == 1);
}

// Mutation: hide Save on every draft (the draft of my own custom screen has no Save button).
TEST_CASE("custom screen chrome: Save overwrites a draft of my own custom screen", "[ui][custom-screens][chrome]") {
    ChromeRig rig;
    std::string const id = rig.saveOnServer("Mine", "notes.edit", std::string{kEdit});
    std::optional<bool> fetched;
    rig.sync->open(id, [&fetched](auto loaded) { fetched = loaded.has_value(); });
    REQUIRE(rig.pump([&fetched] { return fetched.has_value(); }));
    REQUIRE(*fetched);
    rig.open(id, note(1));
    draftHeading(rig, id, "Saved by hand");
    REQUIRE(rig.shown("Button", "draft_save_" + id));
    std::string const before = rig.sync->listed().screens.front().hash;
    rig.click("draft_save_" + id);
    REQUIRE(rig.pump([&rig, &id, &before] {
        return !rig.shown("Row", "draft_bar_" + id) && rig.sync->listed().screens.front().hash != before;
    }));
    CHECK(rig.text("heading") == "Saved by hand");
    REQUIRE(rig.sync->documents().entry(id) != nullptr);
    CHECK(rig.sync->documents().entry(id)->text.contains("Saved by hand"));
    CHECK(rig.workspace->tabs().size() == 1);
}

// Mutation: drop the bar's message on a refused save (`draft_message_notes.edit` stays hidden).
TEST_CASE("custom screen chrome: a store's refusal shows in the draft's bar, and the draft stays",
          "[ui][custom-screens][chrome]") {
    morph::exec::ThreadPoolExecutor storePool{1};
    ChromeRig rig{Offer::Store, std::make_shared<custom_client::RefusingStore>(storePool)};
    rig.open("notes.edit", note(1));
    draftHeading(rig, "notes.edit", "Edited by hand");
    rig.click("draft_save_as_notes.edit");
    rig.type("save_as_title", "Too many");
    rig.click("save_as_confirm");
    REQUIRE(rig.pump([&rig] { return rig.shown("Text", "draft_message_notes.edit"); }));
    CHECK(rig.text("draft_message_notes.edit").contains("over your limit of custom screens"));
    CHECK(rig.shown("Row", "draft_bar_notes.edit"));
    CHECK(rig.text("heading") == "Edited by hand");
    CHECK(rig.screenOf(0) == "notes.edit");
}

// Mutation: make Discard end the draft without remounting (the heading stays "Edited by hand").
TEST_CASE("custom screen chrome: Discard restores the screen, and closing a drafted tab asks first",
          "[ui][custom-screens][chrome]") {
    ChromeRig rig;
    rig.open("notes.edit", note(1));
    rig.click("change");
    draftHeading(rig, "notes.edit", "Edited by hand");
    rig.click("draft_discard_notes.edit");
    REQUIRE(rig.pump([&rig] { return rig.text("heading") == "Edit note"; }));
    CHECK(rig.text("body") == "changed");
    CHECK_FALSE(rig.shown("Row", "draft_bar_notes.edit"));

    draftHeading(rig, "notes.edit", "Again");
    std::optional<bool> closed;
    rig.workspace->close(0, [&closed](bool outcome) { closed = outcome; });
    REQUIRE(rig.pump([&closed] { return closed.has_value(); }));
    CHECK(rig.guarded == 1);
    CHECK_FALSE(*closed);
    CHECK(rig.workspace->tabs().size() == 1);
}
```

Add `test_ui_custom_screen_chrome.cpp` to `tests/CMakeLists.txt`, after `test_ui_custom_screen_sync.cpp`.

- [ ] **Step 3: Run them to verify they fail**

Run: `flock "$LOCK" cmake --build build/clang-debug --target morph_tests -j5`
Expected: compile error, `'morph/ui/custom_screen_chrome.hpp' file not found`.

- [ ] **Step 4: Write the header** (`include/morph/ui/custom_screen_chrome.hpp`)

```cpp
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <morph/export.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "../reactive/runtime.hpp"
#include "../reactive/signal.hpp"
#include "backend.hpp"
#include "custom_screen_sync.hpp"
#include "custom_screens.hpp"
#include "document.hpp"
#include "host.hpp"
#include "mount.hpp"
#include "view.hpp"
#include "workspace_model.hpp"

/// @file
/// @brief The window chrome of custom screens: the draft bar and the **Source** button a shell mounts on each tab's
///        page, the source view, and the save dialog.
///
/// Specified in `docs/spec/ui/custom_screens.md`, "The client".

namespace morph::ui {

/// @brief What a `CustomScreenChrome` works with.
struct CustomScreenChromeOptions {
    /// @brief The runtime the chrome's bindings belong to. Borrowed.
    reactive::Runtime* runtime = nullptr;
    /// @brief The backend its overlays are made with. Borrowed.
    IViewBackend* backend = nullptr;
    /// @brief The host that mounts the screens it is for, which mounts and discards drafts. Borrowed.
    DocumentScreenHost* host = nullptr;
    /// @brief The client of the server's custom screens. Borrowed; it outlives the chrome.
    CustomScreenSync* sync = nullptr;
    /// @brief The loaded document of an application screen id, or null: the bundle's. A screen with none (a host
    ///        screen) gets no chrome. Required.
    std::function<std::shared_ptr<Document const>(std::string_view)> documents;
    /// @brief The JSON text of an application screen's document as the bundle has it; empty for an unknown id.
    ///        Required.
    std::function<std::string(std::string_view)> sourceOf;
    /// @brief The catalog facts a draft loads with. Required.
    std::function<Catalog()> catalog;
    /// @brief The title of an application screen, as its navigator entry shows it; empty shows its id.
    std::function<std::string(std::string_view)> titleOf;
    /// @brief Opens a screen as a tab, as the shell's own navigation does. Required.
    std::function<void(std::string const& screenId, ParamMap const& params)> openTab;
    /// @brief Tells the person something the window does not show, with its tone; may be empty.
    std::function<void(std::string const& text, Tone tone)> notify;
    /// @brief Called after the custom screens listed, or whether they are offered, changed, so the shell lists its
    ///        navigator's Custom section again; may be empty.
    std::function<void()> onListChanged;
    /// @brief Where the overlays are made; null makes each a backend root.
    ContainerWidget* overlayParent = nullptr;
};

/// @brief The window chrome of custom screens over one workspace, built from backend nodes so every frontend shows it.
///
/// For each document screen's tab, `chromeFor` gives a draft bar, shown while the tab holds a draft (Save for a draft
/// of the person's own custom screen, Save as…, Source and Discard, and the last refusal of a save), and a row with the
/// **Source** button, shown while it does not. **Source** opens the source view, an overlay with the document's JSON
/// pretty-printed: read only while custom screens are not offered. **Preview** loads the text in the client, asks the
/// server's `check`, and mounts what passes as the tab's draft (`DocumentScreenHost::mountDraft`); a refusal is shown
/// with the line its pointer names, and choosing it moves the cursor there. **Save as…** asks a name, saves, and opens
/// the saved screen in the tab's place. Everything runs on the runtime's owner. Neither copyable nor movable.
class MORPH_API CustomScreenChrome {
public:
    /// @brief The chrome.
    /// @param options What it works with.
    /// @throws std::invalid_argument for a null runtime, backend, host or sync, or an empty `documents`, `sourceOf`,
    ///         `catalog` or `openTab`.
    explicit CustomScreenChrome(CustomScreenChromeOptions options);

    /// @brief Closes the overlays; replies still in flight do nothing once it is gone.
    ~CustomScreenChrome();

    CustomScreenChrome(CustomScreenChrome const&) = delete;
    CustomScreenChrome& operator=(CustomScreenChrome const&) = delete;
    CustomScreenChrome(CustomScreenChrome&&) = delete;
    CustomScreenChrome& operator=(CustomScreenChrome&&) = delete;

    /// @brief The workspace whose tabs the chrome is for. Call it with null before that workspace goes, and with the
    ///        new one once it is built: the overlays close, and the tabs' state is forgotten with a new workspace.
    /// @param model The workspace's model, or null. Borrowed while attached.
    void attach(WorkspaceModel* model);

    /// @brief Whether the server offers custom screens, as the sync's probe found: without them the source view is
    ///        read only and nothing can be saved.
    /// @param offered Whether they are offered.
    void setOffered(bool offered);

    /// @brief A list the sync received: what Save, **Open as** and the navigator read. Custom screens are then offered.
    /// @param listed The list.
    void listed(CustomScreensListed const& listed);

    /// @brief The chrome of one tab, for `WorkspaceOptions::chrome`.
    /// @param tab      The tab.
    /// @param screenId Its screen's id.
    /// @return The draft bar and the tools row; null for a screen with no document, such as a host screen.
    [[nodiscard]] Node chromeFor(WorkspaceTabId tab, std::string_view screenId);

    /// @brief Opens the source view of a tab's screen: its draft's text while it holds one, a custom screen's stored
    ///        text, or an application screen's text as the bundle has it.
    /// @param tab The tab; a closed one does nothing.
    void showSource(WorkspaceTabId tab);

    /// @brief The source view's **Preview**: loads its text in the client, checks it on the server, and mounts it as
    ///        the tab's draft, closing the view; or shows the first refusal.
    /// @param done Called once with whether a draft was mounted; may be empty.
    void preview(std::function<void(bool drafted)> done = {});

    /// @brief Opens the save dialog for a tab's draft, or for the source view's text when @p fromSource holds, which
    ///        is previewed first so nothing is saved the person has not seen mounted.
    /// @param tab        The tab.
    /// @param fromSource Whether the text is the source view's.
    void askSaveAs(WorkspaceTabId tab, bool fromSource);

    /// @brief The save dialog's **Save**: saves under the name typed, then opens the saved screen in the tab's place. A
    ///        refusal stays in the dialog and in the draft's bar.
    void confirmSaveAs();

    /// @brief The draft bar's **Save**: overwrites the person's own custom screen with the tab's draft, which then is
    ///        the screen as stored. A refusal shows in the bar and the draft stays.
    /// @param tab The tab.
    void save(WorkspaceTabId tab);

    /// @brief The draft bar's **Discard**: mounts the screen's own document again, fetching a custom screen's first
    ///        when the cache no longer holds it.
    /// @param tab The tab.
    void discard(WorkspaceTabId tab);

private:
    enum class SourceMode : std::uint8_t { Draft, Stored };

    struct ShownRefusal {
        std::string pointer;
        std::string reason;
        bool operator==(ShownRefusal const&) const = default;
    };

    // What the chrome keeps per tab: the bar's last refusal, and the draft's text while one is mounted.
    struct TabState {
        std::unique_ptr<reactive::Signal<std::string>> message;
        std::string draftText;
    };

    // The source view: its text, its refusal, its status line, and the mount of its dialog, destroyed first.
    struct Source {
        SourceMode mode = SourceMode::Draft;
        std::optional<WorkspaceTabId> tab;
        std::string screenId;
        bool readOnly = false;
        std::unique_ptr<reactive::Signal<std::string>> text;
        std::unique_ptr<reactive::Signal<std::optional<ShownRefusal>>> refusal;
        std::unique_ptr<reactive::Signal<std::string>> status;
        std::unique_ptr<reactive::Signal<bool>> busy;
        std::optional<Mounted> view;
    };

    // The save dialog.
    struct SaveAs {
        WorkspaceTabId tab;
        bool fromSource = false;
        std::unique_ptr<reactive::Signal<std::string>> title;
        std::unique_ptr<reactive::Signal<std::string>> message;
        std::unique_ptr<reactive::Signal<bool>> busy;
        std::optional<Mounted> view;
    };

    // Wraps @p fn so it does nothing once the chrome is gone: replies arrive through the sync, which may outlive it.
    template <typename F>
    [[nodiscard]] auto guarded(F fn) const {
        return [alive = std::weak_ptr<bool>{_alive}, fn = std::move(fn)](auto&&... args) mutable {
            if (auto const live = alive.lock(); live != nullptr && *live) {
                fn(std::forward<decltype(args)>(args)...);
            }
        };
    }

    [[nodiscard]] Node draftBar(WorkspaceTabId tab, std::string const& screenId, TabState& state);
    [[nodiscard]] Node toolsRow(WorkspaceTabId tab, std::string const& screenId);
    TabState& stateOf(WorkspaceTabId tab);
    [[nodiscard]] ScreenHandle* handleOf(WorkspaceTabId tab) const;
    // A tracked read of the model's tabs first, so a binding made while the tab is opening reads the handle once the
    // model holds it.
    [[nodiscard]] ScreenHandle* trackedHandleOf(WorkspaceTabId tab) const;
    [[nodiscard]] bool draftedTracked(WorkspaceTabId tab) const;
    [[nodiscard]] bool canSave(std::string const& screenId) const;
    [[nodiscard]] std::optional<CustomScreenSummary> summaryOf(std::string_view screenId) const;
    [[nodiscard]] std::string baseOf(std::string const& screenId) const;
    [[nodiscard]] std::string currentText(std::string const& screenId) const;
    [[nodiscard]] std::string titleOf(std::string const& screenId) const;
    void openSource(SourceMode mode, std::optional<WorkspaceTabId> tab, std::string screenId, std::string text,
                    bool readOnly);
    void refuse(std::string pointer, std::string reason);
    void moveToRefusal();
    void closeSourceLater();
    void closeSaveAsLater();
    void saveAs(WorkspaceTabId tab, std::string const& title);
    void failSaveAs(WorkspaceTabId tab, std::string const& reason);
    void openSaved(WorkspaceTabId tab, std::string const& id, std::string const& title);
    void notify(std::string const& text, Tone tone) const;

    CustomScreenChromeOptions _options;
    WorkspaceModel* _model = nullptr;
    std::shared_ptr<bool> _alive = std::make_shared<bool>(true);
    std::unique_ptr<reactive::Signal<bool>> _offered;
    // Bumped on each list, so a binding that reads `_listed` follows it.
    std::unique_ptr<reactive::Signal<std::uint64_t>> _listVersion;
    CustomScreensListed _listed;
    std::map<std::uint64_t, TabState> _tabs;
    // The overlays, last: their bindings read everything above.
    std::unique_ptr<Source> _source;
    std::unique_ptr<SaveAs> _saveAs;
};

}  // namespace morph::ui
```

- [ ] **Step 5: Write the implementation** (`src/ui/custom_screen_chrome.cpp`)

```cpp
// SPDX-License-Identifier: Apache-2.0

#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <morph/ui/custom_screen_chrome.hpp>
#include <morph/ui/json_source.hpp>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace morph::ui {

namespace {

// A button of the chrome: its caption, emphasis and press, shown and enabled as bound.
Node chromeButton(std::string testId, std::string label, ButtonEmphasis emphasis, Action onClick,
                  Prop<bool> visible = Prop<bool>{true}, Prop<bool> enabled = Prop<bool>{true}) {
    Button button;
    button.label = Prop<std::string>{std::move(label)};
    button.emphasis = emphasis;
    button.onClick = std::move(onClick);
    button.common.testId = std::move(testId);
    button.common.visible = std::move(visible);
    button.common.enabled = std::move(enabled);
    return ui::button(std::move(button));
}

// What the refusal says: the line its pointer names in @p text, then the reason.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters): a refusal's pointer, then its reason.
std::string refusalLabel(std::string_view text, std::string const& pointer, std::string const& reason) {
    if (std::optional<std::size_t> const offset = pointerOffset(text, pointer)) {
        return "Line " + std::to_string(lineAt(text, *offset)) + ": " + reason;
    }
    return pointer.empty() ? reason : "At " + pointer + ": " + reason;
}

void finish(std::function<void(bool)> const& done, bool outcome) {
    if (done) {
        done(outcome);
    }
}

}  // namespace

CustomScreenChrome::CustomScreenChrome(CustomScreenChromeOptions options) : _options{std::move(options)} {
    if (_options.runtime == nullptr || _options.backend == nullptr || _options.host == nullptr ||
        _options.sync == nullptr) {
        throw std::invalid_argument{"morph::ui::CustomScreenChrome: a null runtime, backend, host or sync"};
    }
    if (!_options.documents || !_options.sourceOf || !_options.catalog || !_options.openTab) {
        throw std::invalid_argument{
            "morph::ui::CustomScreenChrome: documents, sourceOf, catalog and openTab are required"};
    }
    _offered = std::make_unique<reactive::Signal<bool>>(*_options.runtime, false);
    _listVersion = std::make_unique<reactive::Signal<std::uint64_t>>(*_options.runtime, 0);
}

CustomScreenChrome::~CustomScreenChrome() {
    *_alive = false;
    _saveAs.reset();
    _source.reset();
}

void CustomScreenChrome::attach(WorkspaceModel* model) {
    _saveAs.reset();
    _source.reset();
    _model = model;
    // The rows of the old workspace's tabs read this state; they are gone once a new workspace is attached.
    if (model != nullptr) {
        _tabs.clear();
    }
}

void CustomScreenChrome::setOffered(bool offered) {
    _options.runtime->batch([this, offered] { _offered->set(offered); });
    if (_options.onListChanged) {
        _options.onListChanged();
    }
}

void CustomScreenChrome::listed(CustomScreensListed const& listed) {
    _listed = listed;
    _options.runtime->batch([this] {
        _offered->set(true);
        _listVersion->set(_listVersion->peek() + 1);
    });
    if (_options.onListChanged) {
        _options.onListChanged();
    }
}

Node CustomScreenChrome::chromeFor(WorkspaceTabId tab, std::string_view screenId) {
    std::string const id{screenId};
    if (!isCustomScreenId(id) && !_options.documents(id)) {
        return nullptr;
    }
    TabState& state = stateOf(tab);
    Column column;
    column.gap = 1;
    column.common.testId = "screen_chrome_" + id;
    column.children.push_back(draftBar(tab, id, state));
    column.children.push_back(toolsRow(tab, id));
    return ui::column(std::move(column));
}

Node CustomScreenChrome::draftBar(WorkspaceTabId tab, std::string const& screenId, TabState& state) {
    Row bar;
    bar.gap = 2;
    bar.wrap = true;
    bar.align = Align::Center;
    bar.common.testId = "draft_bar_" + screenId;
    bar.common.visible = Prop<bool>{[this, tab] { return draftedTracked(tab); }};
    Badge badge;
    badge.text = Prop<std::string>{std::string{"Draft"}};
    badge.tone = Prop<Tone>{Tone::Warn};
    bar.children.push_back(ui::badge(std::move(badge)));
    Text note;
    note.text = Prop<std::string>{std::string{"Not saved. Forms on a draft change real data."}};
    note.role = Prop<TextRole>{TextRole::Muted};
    note.common.testId = "draft_note_" + screenId;
    bar.children.push_back(ui::text(std::move(note)));
    reactive::Signal<std::string> const* const message = state.message.get();
    Text refused;
    refused.text = Prop<std::string>{[message] { return message->get(); }};
    refused.role = Prop<TextRole>{TextRole::Error};
    refused.common.testId = "draft_message_" + screenId;
    refused.common.visible = Prop<bool>{[message] { return !message->get().empty(); }};
    bar.children.push_back(ui::text(std::move(refused)));
    ActionBar actions;
    actions.children.push_back(chromeButton("draft_save_" + screenId, "Save", ButtonEmphasis::Primary,
                                            guarded([this, tab] { save(tab); }),
                                            Prop<bool>{[this, screenId] { return canSave(screenId); }}));
    actions.children.push_back(chromeButton("draft_save_as_" + screenId, "Save as…", ButtonEmphasis::Normal,
                                            guarded([this, tab] { askSaveAs(tab, false); }),
                                            Prop<bool>{[this] { return _offered->get(); }}));
    actions.children.push_back(chromeButton("draft_source_" + screenId, "Source", ButtonEmphasis::Normal,
                                            guarded([this, tab] { showSource(tab); })));
    actions.children.push_back(chromeButton("draft_discard_" + screenId, "Discard", ButtonEmphasis::Normal,
                                            guarded([this, tab] { discard(tab); })));
    bar.children.push_back(ui::actionBar(std::move(actions)));
    return ui::row(std::move(bar));
}

Node CustomScreenChrome::toolsRow(WorkspaceTabId tab, std::string const& screenId) {
    Row row;
    row.gap = 2;
    row.align = Align::Center;
    row.justify = Justify::End;
    row.common.testId = "screen_tools_" + screenId;
    row.common.visible = Prop<bool>{[this, tab] { return !draftedTracked(tab); }};
    row.children.push_back(chromeButton("source_button_" + screenId, "Source", ButtonEmphasis::Ghost,
                                        guarded([this, tab] { showSource(tab); })));
    return ui::row(std::move(row));
}

CustomScreenChrome::TabState& CustomScreenChrome::stateOf(WorkspaceTabId tab) {
    auto [found, inserted] = _tabs.try_emplace(tab.value);
    if (inserted) {
        found->second.message = std::make_unique<reactive::Signal<std::string>>(*_options.runtime, std::string{});
    }
    return found->second;
}

ScreenHandle* CustomScreenChrome::handleOf(WorkspaceTabId tab) const {
    return _model == nullptr ? nullptr : _model->handleOf(tab);
}

ScreenHandle* CustomScreenChrome::trackedHandleOf(WorkspaceTabId tab) const {
    if (_model == nullptr) {
        return nullptr;
    }
    static_cast<void>(_model->tabs().get());
    return _model->handleOf(tab);
}

bool CustomScreenChrome::draftedTracked(WorkspaceTabId tab) const {
    ScreenHandle const* const handle = trackedHandleOf(tab);
    return handle != nullptr && DocumentScreenHost::drafted(*handle);
}

bool CustomScreenChrome::canSave(std::string const& screenId) const {
    static_cast<void>(_listVersion->get());
    if (!_offered->get() || !isCustomScreenId(screenId)) {
        return false;
    }
    std::optional<CustomScreenSummary> const summary = summaryOf(screenId);
    return summary.has_value() && summary->mine;
}

std::optional<CustomScreenSummary> CustomScreenChrome::summaryOf(std::string_view screenId) const {
    for (CustomScreenSummary const& screen : _listed.screens) {
        if (screen.id == screenId) {
            return screen;
        }
    }
    if (CustomDocuments::Entry const* const entry = _options.sync->documents().entry(screenId)) {
        return entry->summary;
    }
    return std::nullopt;
}

std::string CustomScreenChrome::baseOf(std::string const& screenId) const {
    if (!isCustomScreenId(screenId)) {
        return screenId;
    }
    std::optional<CustomScreenSummary> const summary = summaryOf(screenId);
    return summary ? summary->basedOn : std::string{};
}

std::string CustomScreenChrome::currentText(std::string const& screenId) const {
    if (!isCustomScreenId(screenId)) {
        return _options.sourceOf(screenId);
    }
    CustomDocuments::Entry const* const entry = _options.sync->documents().entry(screenId);
    return entry == nullptr ? std::string{} : entry->text;
}

std::string CustomScreenChrome::titleOf(std::string const& screenId) const {
    if (isCustomScreenId(screenId)) {
        std::optional<CustomScreenSummary> const summary = summaryOf(screenId);
        return summary ? summary->title : screenId;
    }
    return _options.titleOf ? _options.titleOf(screenId) : screenId;
}

void CustomScreenChrome::showSource(WorkspaceTabId tab) {
    ScreenHandle* const handle = handleOf(tab);
    std::optional<std::string> const screenId =
        handle == nullptr ? std::nullopt : DocumentScreenHost::screenIdOf(*handle);
    if (!screenId) {
        return;
    }
    std::string const text = DocumentScreenHost::drafted(*handle) ? stateOf(tab).draftText : currentText(*screenId);
    openSource(SourceMode::Draft, tab, *screenId, prettyJson(text), !_offered->peek());
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity): one dialog, built in reading order.
void CustomScreenChrome::openSource(SourceMode mode, std::optional<WorkspaceTabId> tab, std::string screenId,
                                    std::string text, bool readOnly) {
    _source.reset();
    auto made = std::make_unique<Source>();
    Source* const source = made.get();
    source->mode = mode;
    source->tab = tab;
    source->screenId = std::move(screenId);
    source->readOnly = readOnly;
    source->text = std::make_unique<reactive::Signal<std::string>>(*_options.runtime, std::move(text));
    source->refusal =
        std::make_unique<reactive::Signal<std::optional<ShownRefusal>>>(*_options.runtime, std::nullopt);
    source->status = std::make_unique<reactive::Signal<std::string>>(*_options.runtime, std::string{});
    source->busy = std::make_unique<reactive::Signal<bool>>(*_options.runtime, false);
    Prop<bool> const notBusy{[source] { return !source->busy->get(); }};

    Dialog dialog;
    dialog.open = Prop<bool>{true};
    dialog.title = Prop<std::string>{"Source of " + titleOf(source->screenId)};
    if (readOnly) {
        dialog.description = Prop<std::string>{std::string{"Read only: this server does not store custom screens."}};
    }
    dialog.onDismiss = guarded([this] { closeSourceLater(); });
    dialog.common.testId = "source_dialog";

    Column body;
    body.gap = 2;
    TextInput input;
    input.value = Prop<std::string>{[source] { return source->text->get(); }};
    input.onChange = [source](std::string changed) {
        source->text->set(std::move(changed));
        source->refusal->set(std::nullopt);
    };
    input.mode = TextInputMode::Multiline;
    input.mono = true;
    input.label = Prop<std::string>{std::string{"Document source"}};
    input.field.readonly = Prop<bool>{readOnly};
    input.common.testId = "source_text";
    input.common.focusId = "source_text";
    input.common.layout.width = Sizing::stretch();
    body.children.push_back(ui::textInput(std::move(input)));

    Button refusal;
    refusal.label = Prop<std::string>{[source] {
        std::optional<ShownRefusal> const& shown = source->refusal->get();
        return shown ? refusalLabel(source->text->peek(), shown->pointer, shown->reason) : std::string{};
    }};
    refusal.emphasis = ButtonEmphasis::DangerOutline;
    refusal.onClick = guarded([this] { moveToRefusal(); });
    refusal.common.testId = "source_refusal";
    refusal.common.visible = Prop<bool>{[source] { return source->refusal->get().has_value(); }};
    body.children.push_back(ui::button(std::move(refusal)));

    Text status;
    status.text = Prop<std::string>{[source] { return source->status->get(); }};
    status.role = Prop<TextRole>{TextRole::Muted};
    status.common.testId = "source_status";
    status.common.visible = Prop<bool>{[source] { return !source->status->get().empty(); }};
    body.children.push_back(ui::text(std::move(status)));

    ActionBar actions;
    if (!readOnly && mode == SourceMode::Draft) {
        actions.children.push_back(chromeButton("source_preview", "Preview", ButtonEmphasis::Primary,
                                                guarded([this] { preview(); }), Prop<bool>{true}, notBusy));
        actions.children.push_back(chromeButton("source_save_as", "Save as…", ButtonEmphasis::Normal,
                                                guarded([this, tab] {
                                                    if (tab) {
                                                        askSaveAs(*tab, true);
                                                    }
                                                }),
                                                Prop<bool>{true}, notBusy));
    }
    actions.children.push_back(
        chromeButton("source_close", "Close", ButtonEmphasis::Normal, guarded([this] { closeSourceLater(); })));
    body.children.push_back(ui::actionBar(std::move(actions)));
    dialog.child = ui::column(std::move(body));

    _source = std::move(made);
    source->view.emplace(*_options.runtime, *_options.backend, ui::dialog(std::move(dialog)), _options.overlayParent);
}

void CustomScreenChrome::refuse(std::string pointer, std::string reason) {
    if (Source* const source = _source.get()) {
        _options.runtime->batch([source, &pointer, &reason] {
            source->busy->set(false);
            source->status->set(std::string{});
            source->refusal->set(ShownRefusal{.pointer = std::move(pointer), .reason = std::move(reason)});
        });
    }
}

void CustomScreenChrome::moveToRefusal() {
    Source* const source = _source.get();
    if (source == nullptr || !source->view) {
        return;
    }
    std::optional<ShownRefusal> const& shown = source->refusal->peek();
    if (!shown) {
        return;
    }
    std::size_t const offset = pointerOffset(source->text->peek(), shown->pointer).value_or(0);
    static_cast<void>(source->view->placeCursor("source_text", offset));
}

void CustomScreenChrome::closeSourceLater() {
    Source const* const shown = _source.get();
    // Posted: this runs inside a handler of the dialog it destroys.
    _options.runtime->owner().post(guarded([this, shown] {
        if (_source.get() == shown) {
            _source.reset();
        }
    }));
}

void CustomScreenChrome::closeSaveAsLater() {
    SaveAs const* const shown = _saveAs.get();
    _options.runtime->owner().post(guarded([this, shown] {
        if (_saveAs.get() == shown) {
            _saveAs.reset();
        }
    }));
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity): the client's load, the server's check, then the mount.
void CustomScreenChrome::preview(std::function<void(bool drafted)> done) {
    Source* const source = _source.get();
    if (source == nullptr || source->mode != SourceMode::Draft || source->readOnly || !source->tab) {
        finish(done, false);
        return;
    }
    WorkspaceTabId const tab = *source->tab;
    std::string const text = source->text->peek();
    auto loaded = loadDocumentText(text, LoadOptions{.catalog = _options.catalog()});
    if (!loaded) {
        refuse(loaded.error().path, loaded.error().reason);
        finish(done, false);
        return;
    }
    if (loaded->id != source->screenId) {
        refuse("/id", "a draft keeps the screen's id, " + source->screenId);
        finish(done, false);
        return;
    }
    std::string const basedOn = baseOf(source->screenId);
    if (basedOn.empty()) {
        refuse("", "the base of " + source->screenId + " is not known");
        finish(done, false);
        return;
    }
    auto const document = std::make_shared<Document const>(std::move(*loaded));
    _options.runtime->batch([source] {
        source->busy->set(true);
        source->status->set(std::string{"Checking…"});
    });
    _options.sync->check(
        basedOn, text,
        guarded([this, tab, text, document, done](std::expected<std::optional<CheckRefusal>, std::string> checked) {
            Source* const shown = _source.get();
            if (shown == nullptr || !shown->tab || shown->tab->value != tab.value) {
                finish(done, false);
                return;
            }
            if (!checked) {
                refuse("", checked.error());
                finish(done, false);
                return;
            }
            if (*checked) {
                refuse((*checked)->pointer, (*checked)->reason);
                finish(done, false);
                return;
            }
            ScreenHandle* const handle = handleOf(tab);
            if (handle == nullptr) {
                refuse("", "the screen is closed");
                finish(done, false);
                return;
            }
            if (auto mounted = _options.host->mountDraft(*handle, document); !mounted) {
                refuse("", mounted.error().reason);
                finish(done, false);
                return;
            }
            TabState& state = stateOf(tab);
            state.draftText = text;
            state.message->set(std::string{});
            closeSourceLater();
            finish(done, true);
        }));
}

void CustomScreenChrome::askSaveAs(WorkspaceTabId tab, bool fromSource) {
    if (!_offered->peek()) {
        return;
    }
    _saveAs.reset();
    auto made = std::make_unique<SaveAs>();
    SaveAs* const dialog = made.get();
    dialog->tab = tab;
    dialog->fromSource = fromSource;
    dialog->title = std::make_unique<reactive::Signal<std::string>>(*_options.runtime, std::string{});
    dialog->message = std::make_unique<reactive::Signal<std::string>>(*_options.runtime, std::string{});
    dialog->busy = std::make_unique<reactive::Signal<bool>>(*_options.runtime, false);

    Dialog node;
    node.open = Prop<bool>{true};
    node.title = Prop<std::string>{std::string{"Save as"}};
    node.onDismiss = guarded([this] { closeSaveAsLater(); });
    node.common.testId = "save_as_dialog";
    Column body;
    body.gap = 2;
    TextInput input;
    input.value = Prop<std::string>{[dialog] { return dialog->title->get(); }};
    input.onChange = [dialog](std::string typed) { dialog->title->set(std::move(typed)); };
    input.onSubmit = guarded([this](std::string const& /*typed*/) { confirmSaveAs(); });
    input.label = Prop<std::string>{std::string{"Name"}};
    input.showLabel = true;
    input.common.testId = "save_as_title";
    input.common.autofocus = true;
    body.children.push_back(ui::textInput(std::move(input)));
    Text message;
    message.text = Prop<std::string>{[dialog] { return dialog->message->get(); }};
    message.role = Prop<TextRole>{TextRole::Error};
    message.common.testId = "save_as_message";
    message.common.visible = Prop<bool>{[dialog] { return !dialog->message->get().empty(); }};
    body.children.push_back(ui::text(std::move(message)));
    ActionBar actions;
    actions.children.push_back(chromeButton("save_as_confirm", "Save", ButtonEmphasis::Primary,
                                            guarded([this] { confirmSaveAs(); }), Prop<bool>{true},
                                            Prop<bool>{[dialog] { return !dialog->busy->get(); }}));
    actions.children.push_back(
        chromeButton("save_as_cancel", "Cancel", ButtonEmphasis::Normal, guarded([this] { closeSaveAsLater(); })));
    body.children.push_back(ui::actionBar(std::move(actions)));
    node.child = ui::column(std::move(body));

    _saveAs = std::move(made);
    dialog->view.emplace(*_options.runtime, *_options.backend, ui::dialog(std::move(node)), _options.overlayParent);
}

void CustomScreenChrome::confirmSaveAs() {
    SaveAs* const dialog = _saveAs.get();
    if (dialog == nullptr || dialog->busy->peek()) {
        return;
    }
    std::string const title = dialog->title->peek();
    if (title.empty()) {
        dialog->message->set(std::string{"Give the screen a name."});
        return;
    }
    _options.runtime->batch([dialog] {
        dialog->busy->set(true);
        dialog->message->set(std::string{});
    });
    WorkspaceTabId const tab = dialog->tab;
    if (!dialog->fromSource) {
        saveAs(tab, title);
        return;
    }
    preview(guarded([this, tab, title](bool drafted) {
        if (drafted) {
            saveAs(tab, title);
            return;
        }
        if (SaveAs* const shown = _saveAs.get()) {
            _options.runtime->batch([shown] {
                shown->busy->set(false);
                shown->message->set(std::string{"Not saved: the source view shows why."});
            });
        }
    }));
}

void CustomScreenChrome::saveAs(WorkspaceTabId tab, std::string const& title) {
    ScreenHandle* const handle = handleOf(tab);
    std::optional<std::string> const screenId =
        handle == nullptr ? std::nullopt : DocumentScreenHost::screenIdOf(*handle);
    if (!screenId) {
        failSaveAs(tab, "the screen is closed");
        return;
    }
    std::string const base = baseOf(*screenId);
    std::string const text = DocumentScreenHost::drafted(*handle) ? stateOf(tab).draftText : currentText(*screenId);
    if (base.empty() || text.empty()) {
        failSaveAs(tab, "the screen's base or document is not known");
        return;
    }
    _options.sync->save(
        CustomScreensSave{.app = {}, .id = std::nullopt, .title = title, .basedOn = base, .document = text},
        guarded([this, tab, title](std::expected<std::string, std::string> saved) {
            if (!saved) {
                failSaveAs(tab, saved.error());
                return;
            }
            closeSaveAsLater();
            openSaved(tab, *saved, title);
        }));
}

void CustomScreenChrome::failSaveAs(WorkspaceTabId tab, std::string const& reason) {
    stateOf(tab).message->set(reason);
    if (SaveAs* const dialog = _saveAs.get()) {
        _options.runtime->batch([dialog, &reason] {
            dialog->busy->set(false);
            dialog->message->set(reason);
        });
    }
}

void CustomScreenChrome::openSaved(WorkspaceTabId tab, std::string const& id, std::string const& title) {
    _options.sync->open(
        id, guarded([this, tab, id, title](std::expected<std::shared_ptr<Document const>, std::string> loaded) {
            ScreenHandle* const handle = handleOf(tab);
            if (!loaded || handle == nullptr || _model == nullptr) {
                notify("Saved as " + title + ", but it could not be opened" +
                           (loaded ? std::string{} : ": " + loaded.error()),
                       Tone::Warn);
                return;
            }
            _options.host->endDraft(*handle);
            stateOf(tab).draftText.clear();
            ParamMap const params = DocumentScreenHost::paramsOf(*handle).value_or(ParamMap{});
            _model->openInPlace(tab, id, params,
                                guarded([this, title](std::expected<WorkspaceTabId, MountError> opened) {
                                    if (!opened) {
                                        notify("Saved as " + title + ", but it could not be opened: " +
                                                   opened.error().reason,
                                               Tone::Warn);
                                        return;
                                    }
                                    notify("Saved as " + title, Tone::Ok);
                                }));
        }));
}

void CustomScreenChrome::save(WorkspaceTabId tab) {
    ScreenHandle* const handle = handleOf(tab);
    std::optional<std::string> const screenId =
        handle == nullptr ? std::nullopt : DocumentScreenHost::screenIdOf(*handle);
    if (!screenId || !DocumentScreenHost::drafted(*handle) || !canSave(*screenId)) {
        return;
    }
    std::optional<CustomScreenSummary> const summary = summaryOf(*screenId);
    std::string const id = *screenId;
    _options.sync->save(
        CustomScreensSave{.app = {},
                          .id = id,
                          .title = summary->title,
                          .basedOn = summary->basedOn,
                          .document = stateOf(tab).draftText},
        guarded([this, tab, id](std::expected<std::string, std::string> saved) {
            if (!saved) {
                stateOf(tab).message->set(saved.error());
                return;
            }
            _options.sync->fetch(
                id, guarded([this, tab](std::expected<std::shared_ptr<Document const>, std::string> const& loaded) {
                    if (ScreenHandle* const shown = handleOf(tab)) {
                        _options.host->endDraft(*shown);
                    }
                    TabState& state = stateOf(tab);
                    state.draftText.clear();
                    state.message->set(std::string{});
                    if (!loaded) {
                        notify("Saved, but the stored screen could not be read again: " + loaded.error(), Tone::Warn);
                    }
                }));
        }));
}

void CustomScreenChrome::discard(WorkspaceTabId tab) {
    ScreenHandle* const handle = handleOf(tab);
    std::optional<std::string> const screenId =
        handle == nullptr ? std::nullopt : DocumentScreenHost::screenIdOf(*handle);
    if (!screenId || !DocumentScreenHost::drafted(*handle)) {
        return;
    }
    auto const remount = [this, tab] {
        ScreenHandle* const shown = handleOf(tab);
        if (shown == nullptr) {
            return;
        }
        TabState& state = stateOf(tab);
        if (auto discarded = _options.host->discardDraft(*shown); !discarded) {
            state.message->set(discarded.error().reason);
            return;
        }
        state.draftText.clear();
        state.message->set(std::string{});
    };
    if (isCustomScreenId(*screenId) && _options.sync->documents().find(*screenId) == nullptr) {
        _options.sync->open(*screenId, guarded([this, tab, remount](
                                                   std::expected<std::shared_ptr<Document const>, std::string> loaded) {
            if (!loaded) {
                stateOf(tab).message->set(loaded.error());
                return;
            }
            remount();
        }));
        return;
    }
    // Posted: the press that asked runs in a handler, and a remount must not run inside one.
    _options.runtime->owner().post(guarded(remount));
}

void CustomScreenChrome::notify(std::string const& text, Tone tone) const {
    if (_options.notify) {
        _options.notify(text, tone);
    }
}

}  // namespace morph::ui
```

Add the source to `add_library(morph …)` after `src/ui/custom_screen_sync.cpp`, and the header to `FILE_SET HEADERS`
after `include/morph/ui/custom_screen_sync.hpp`.

- [ ] **Step 6: Run the tests**

Run: `flock "$LOCK" cmake --build build/clang-debug --target morph_tests -j5 && flock "$LOCK" timeout 300 build/clang-debug/tests/morph_tests "[chrome]"`
Expected: `All tests passed`.

- [ ] **Step 7: Mutation check.** Apply the nine mutations named above the tests in turn; each named test fails.
  Restore and `touch src/ui/custom_screen_chrome.cpp`.

- [ ] **Step 8: Commit**

```bash
git add include/morph/ui/custom_screen_chrome.hpp src/ui/custom_screen_chrome.cpp tests/ui_custom_client_support.hpp \
  tests/test_ui_custom_screen_chrome.cpp CMakeLists.txt tests/CMakeLists.txt
git commit -m "ui: the custom screens chrome — Source, drafts and saving"
```

---
### Task 8: `CustomScreenChrome` — Open as, the navigator's entries, broken screens and restore

**Files:**
- Modify: `include/morph/ui/custom_screen_chrome.hpp`, `src/ui/custom_screen_chrome.cpp`
- Modify: `tests/ui_custom_client_support.hpp` (`putDirectly`)
- Test: `tests/test_ui_custom_screen_chrome.cpp` (append)

**Interfaces:**
- Consumes: everything Task 7 produced; `Navigator`'s `NavigatorCustomEntry` (Task 5); `CustomScreenSync::get`,
  `prefetchRoutes` (Task 6).
- Produces:
  - `std::vector<NavigatorCustomEntry> CustomScreenChrome::navigatorEntries() const`
  - `void CustomScreenChrome::openCustom(std::string const& screenId, ParamMap params, std::function<void(std::expected<void, std::string>)> done = {})`
  - `void CustomScreenChrome::showStoredSource(std::string const& screenId)`
  - `void CustomScreenChrome::openAs(WorkspaceTabId tab, std::string const& screenId)`
  - Test ids: `open_as_<id>` (a button-style menu), `source_save` (the source view of a stored screen).

- [ ] **Step 1: Add a helper to the test support.** Append before `}  // namespace custom_client` in
  `tests/ui_custom_client_support.hpp`:

```cpp
/// Stores @p screen for `ann` in the rig's store directly, past every check: how a test gets a broken screen listed.
inline void putDirectly(ChromeRig& rig, morph::ui::CustomScreen screen) {
    REQUIRE(rig.server.has_value());
    std::atomic<bool> stored{false};
    morph::ui::CustomScreenKey key{.person = "ann", .app = "notes", .id = screen.id};
    rig.server->store->put(std::move(key), std::move(screen))
        .thenDetached([&stored](std::expected<void, morph::ui::CustomScreenRefusal> const& /*result*/) {
            stored = true;
        });
    REQUIRE(rig.pump([&stored] { return stored.load(); }));
}

/// A custom screen's id: `custom:` and 32 hexadecimal digits ending in @p last.
inline std::string customId(char last) { return "custom:" + std::string(31, '0') + last; }
```

- [ ] **Step 2: Write the failing tests** (append to `tests/test_ui_custom_screen_chrome.cpp`; add
  `#include <expected>`, `#include <vector>` and `using custom_client::kHome;` at the top)

```cpp
// Mutation: open the chosen screen in a tab of its own (two tabs remain).
TEST_CASE("custom screen chrome: Open as lists the screens based on this one and opens one in the same tab and params",
          "[ui][custom-screens][chrome]") {
    ChromeRig rig;
    std::string const alpha = rig.saveOnServer("Alpha", "notes.edit", std::string{kEdit});
    static_cast<void>(rig.saveOnServer("Beta", "notes.edit", std::string{kEdit}));
    static_cast<void>(rig.saveOnServer("Homely", "notes.home", std::string{kHome}));
    rig.open("notes.edit", note(7));
    REQUIRE(rig.pump([&rig] { return rig.find("Menu", "open_as_notes.edit").has_value(); }));
    int const menu = *rig.find("Menu", "open_as_notes.edit");
    std::string const items = rig.harness.backend.prop(menu, "items");
    CHECK(items.contains("Note"));
    CHECK(items.contains("Alpha"));
    CHECK(items.contains("Beta"));
    CHECK_FALSE(items.contains("Homely"));

    // The base first, then the custom screens of that base in the list's order.
    std::size_t place = 1;
    for (morph::ui::CustomScreenSummary const& screen : rig.sync->listed().screens) {
        if (screen.basedOn != "notes.edit") {
            continue;
        }
        if (screen.id == alpha) {
            break;
        }
        ++place;
    }
    rig.harness.backend.chooseMenuEntry(menu, {place});
    REQUIRE(rig.pump([&rig, &alpha] { return rig.workspace->tabs().size() == 1 && rig.screenOf(0) == alpha; }));
    CHECK(DocumentScreenHost::paramsOf(*rig.workspace->handleOf(0)) == std::optional<ParamMap>{note(7)});
    CHECK(rig.text("heading") == "Edit note");
}

// Mutation: show Open as whatever the offer (the menu stays after custom screens are no longer offered).
TEST_CASE("custom screen chrome: Open as and the navigator's entries go when custom screens are not offered",
          "[ui][custom-screens][chrome]") {
    ChromeRig rig;
    static_cast<void>(rig.saveOnServer("Alpha", "notes.edit", std::string{kEdit}));
    static_cast<void>(rig.saveOnServer("Homely", "notes.home", std::string{kHome}));
    rig.open("notes.edit", note(1));
    REQUIRE(rig.pump([&rig] { return rig.find("Menu", "open_as_notes.edit").has_value(); }));
    CHECK(rig.chrome->navigatorEntries().size() == 1);
    rig.chrome->setOffered(false);
    rig.harness.settle();
    CHECK_FALSE(rig.find("Menu", "open_as_notes.edit").has_value());
    CHECK(rig.chrome->navigatorEntries().empty());
}

// Mutations: leave broken screens out of the entries (`Old` is not listed); mount a broken screen instead of opening its
// source (no source view shows, and a tab is opened).
TEST_CASE("custom screen chrome: navigator entries are the screens of a base without params, a broken one opens its "
          "source",
          "[ui][custom-screens][chrome]") {
    ChromeRig rig;
    std::string const homely = rig.saveOnServer("Homely", "notes.home", std::string{kHome});
    static_cast<void>(rig.saveOnServer("Alpha", "notes.edit", std::string{kEdit}));
    std::string const old = custom_client::customId('1');
    custom_client::putDirectly(rig, morph::ui::CustomScreen{
                                        .id = old,
                                        .title = "Old",
                                        .basedOn = "notes.home",
                                        .document = R"({"vocab":"ui/1","id":")" + old + R"(","view":{"kind":"hologram"}})",
                                        .savedAt = {},
                                        .savedBy = "ann"});
    rig.sync->relist();
    REQUIRE(rig.pump([&rig] { return rig.sync->listed().screens.size() == 3; }));

    std::vector<morph::ui::NavigatorCustomEntry> const entries = rig.chrome->navigatorEntries();
    REQUIRE(entries.size() == 2);
    auto const entryOf = [&entries](std::string const& id) {
        auto const found = std::ranges::find(entries, id, &morph::ui::NavigatorCustomEntry::id);
        REQUIRE(found != entries.end());
        return *found;
    };
    CHECK(entryOf(homely).title == "Homely");
    CHECK(entryOf(homely).mine);
    CHECK(entryOf(homely).broken.empty());
    CHECK_FALSE(entryOf(old).broken.empty());

    rig.chrome->openCustom(old, {});
    REQUIRE(rig.pump([&rig] { return rig.find("Dialog", "source_dialog").has_value(); }));
    CHECK(rig.shown("Button", "source_refusal"));
    CHECK(rig.find("Button", "source_save").has_value());
    CHECK(rig.workspace->tabs().empty());

    // A screen whose base takes no params opens from the navigator, and has no Open as.
    rig.chrome->openCustom(homely, {});
    REQUIRE(rig.pump([&rig, &homely] { return !rig.workspace->tabs().empty() && rig.screenOf(0) == homely; }));
    CHECK_FALSE(rig.find("Menu", "open_as_" + homely).has_value());
}

// Mutation: close the source view after a passing check without saving (the screen stays broken).
TEST_CASE("custom screen chrome: the source of a broken screen of mine is fixed and saved from the source view",
          "[ui][custom-screens][chrome]") {
    ChromeRig rig;
    std::string const old = custom_client::customId('2');
    custom_client::putDirectly(rig, morph::ui::CustomScreen{
                                        .id = old,
                                        .title = "Old",
                                        .basedOn = "notes.home",
                                        .document = R"({"vocab":"ui/1","id":")" + old + R"(","view":{"kind":"hologram"}})",
                                        .savedAt = {},
                                        .savedBy = "ann"});
    rig.sync->relist();
    REQUIRE(rig.pump([&rig] { return rig.sync->listed().screens.size() == 1; }));
    rig.chrome->showStoredSource(old);
    REQUIRE(rig.pump([&rig] { return rig.find("Dialog", "source_dialog").has_value(); }));
    rig.type("source_text",
             morph::ui::prettyJson(screens_test::replaced(std::string{kHome}, "\"notes.home\"", "\"" + old + "\"")));
    rig.click("source_save");
    REQUIRE(rig.pump([&rig, &old] {
        auto const& screens = rig.sync->listed().screens;
        return screens.size() == 1 && screens.front().id == old && !screens.front().broken.has_value();
    }));
    REQUIRE(rig.pump([&rig] { return !rig.find("Dialog", "source_dialog").has_value(); }));
}

// Mutation: open a custom screen without fetching it first (the mount finds no document, and openCustom answers why).
TEST_CASE("custom screen chrome: a custom screen opens once fetched, is recorded by its route, and reads back once fetched "
          "again",
          "[ui][custom-screens][chrome]") {
    ChromeRig rig;
    std::string const alpha = rig.saveOnServer("Alpha", "notes.edit", std::string{kEdit});
    REQUIRE(rig.sync->documents().find(alpha) == nullptr);
    std::optional<std::expected<void, std::string>> opened;
    rig.chrome->openCustom(alpha, note(3),
                           [&opened](std::expected<void, std::string> answer) { opened = std::move(answer); });
    REQUIRE(rig.pump([&opened] { return opened.has_value(); }));
    REQUIRE(opened->has_value());
    REQUIRE(rig.workspace->tabs().size() == 1);
    std::string const route = alpha + "?noteId=3";
    CHECK(rig.workspace->record() == std::vector<std::string>{route});

    rig.sync->documents().forget(alpha);
    CHECK_FALSE(rig.host->parseRoute(route).has_value());
    std::optional<std::vector<std::string>> missing;
    rig.sync->prefetchRoutes({route}, [&missing](std::vector<std::string> ids) { missing = std::move(ids); });
    REQUIRE(rig.pump([&missing] { return missing.has_value(); }));
    CHECK(missing->empty());
    CHECK(rig.host->parseRoute(route).has_value());
}
```

Add `#include <algorithm>` to the test file too.

- [ ] **Step 3: Run them to verify they fail**

Run: `flock "$LOCK" cmake --build build/clang-debug --target morph_tests -j5`
Expected: compile errors, `no member named 'navigatorEntries'`, `'openCustom'`, `'showStoredSource'`.

- [ ] **Step 4: Declare the additions.** In `include/morph/ui/custom_screen_chrome.hpp`, add `#include <expected>`,
  `#include <vector>` and `#include "navigator.hpp"`, and in the public part, after `discard`:

```cpp
    /// @brief The navigator's Custom section: the listed screens whose base takes no params, so they open with none,
    ///        broken ones included with their reason. Empty while custom screens are not offered.
    /// @return The entries, in the list's order.
    [[nodiscard]] std::vector<NavigatorCustomEntry> navigatorEntries() const;

    /// @brief Opens a custom screen: fetches its document into the cache when it is not there, then opens it as a tab
    ///        through `openTab`. A broken one opens its source view instead, since it is never mounted.
    /// @param screenId The custom screen's id.
    /// @param params   Its params.
    /// @param done     Called once with nothing, or why it did not open; may be empty, and then a failure is told
    ///                 through `notify`.
    void openCustom(std::string const& screenId, ParamMap params,
                    std::function<void(std::expected<void, std::string>)> done = {});

    /// @brief Opens the source view of a stored custom screen, as `get` answers it, with its breakage shown as the
    ///        refusal: editable while custom screens are offered and the screen is the person's own, when
    ///        **Check and save** checks the text on the server and saves it over the screen.
    /// @param screenId The custom screen's id.
    void showStoredSource(std::string const& screenId);

    /// @brief **Open as**: opens @p screenId in @p tab's place with the tab's params, fetching a custom screen's
    ///        document first; a broken one opens its source view instead.
    /// @param tab      The tab.
    /// @param screenId The base screen, or a custom screen based on it.
    void openAs(WorkspaceTabId tab, std::string const& screenId);
```

In the private part, after `struct SaveAs { … };`:

```cpp
    // One entry of Open as, and the menu as a whole, keyed so it is drawn again whenever an entry changes.
    struct OpenAsChoice {
        std::string id;
        std::string label;
        bool current = false;
        bool operator==(OpenAsChoice const&) const = default;
    };
    struct OpenAsShape {
        std::string key;
        std::vector<OpenAsChoice> choices;
        bool operator==(OpenAsShape const&) const = default;
    };
    [[nodiscard]] Node openAsMenu(WorkspaceTabId tab, std::string const& screenId);
    // A tracked read of the list and the offer; no choices while Open as has nothing to offer.
    [[nodiscard]] OpenAsShape openAsShape(std::string const& screenId) const;
    void saveFromSource();
```

- [ ] **Step 5: Implement them** in `src/ui/custom_screen_chrome.cpp`. Add `#include <algorithm>`. In `toolsRow`,
  before `row.children.push_back(chromeButton("source_button_" + screenId, …` add:

```cpp
    row.children.push_back(openAsMenu(tab, screenId));
```

In `openSource`, before the line that pushes the `source_close` button, add:

```cpp
    if (!readOnly && mode == SourceMode::Stored) {
        actions.children.push_back(chromeButton("source_save", "Check and save", ButtonEmphasis::Primary,
                                                guarded([this] { saveFromSource(); }), Prop<bool>{true}, notBusy));
    }
```

and before `void CustomScreenChrome::notify(…)` add:

```cpp
Node CustomScreenChrome::openAsMenu(WorkspaceTabId tab, std::string const& screenId) {
    return ui::forEach<OpenAsShape>(
        [this, screenId] {
            OpenAsShape shape = openAsShape(screenId);
            return shape.choices.empty() ? std::vector<OpenAsShape>{} : std::vector<OpenAsShape>{std::move(shape)};
        },
        [](OpenAsShape const& shape) { return Key{shape.key}; },
        [this, tab, screenId](reactive::Signal<OpenAsShape> const& shape) {
            Menu menu;
            menu.style = MenuStyle::Button;
            menu.label = Prop<std::string>{std::string{"Open as"}};
            menu.common.testId = "open_as_" + screenId;
            for (OpenAsChoice const& choice : shape.peek().choices) {
                MenuItem item;
                item.label = Prop<std::string>{choice.label};
                item.checked = Prop<std::optional<bool>>{std::optional<bool>{choice.current}};
                item.onSelect = guarded([this, tab, target = choice.id] { openAs(tab, target); });
                menu.items.push_back(std::move(item));
            }
            return ui::menu(std::move(menu));
        },
        Axis::Horizontal, 0);
}

CustomScreenChrome::OpenAsShape CustomScreenChrome::openAsShape(std::string const& screenId) const {
    static_cast<void>(_listVersion->get());
    OpenAsShape shape;
    if (!_offered->get()) {
        return shape;
    }
    std::string const base = baseOf(screenId);
    std::shared_ptr<Document const> const baseDocument = base.empty() ? nullptr : _options.documents(base);
    // A base without params opens from the navigator; Open as is for screens opened with params.
    if (!baseDocument || baseDocument->params.empty()) {
        return shape;
    }
    std::vector<OpenAsChoice> customs;
    for (CustomScreenSummary const& screen : _listed.screens) {
        if (screen.basedOn != base) {
            continue;
        }
        std::string label = screen.title;
        if (!screen.mine) {
            label += " · by " + screen.savedBy;
        }
        if (screen.broken) {
            label += " · broken";
        }
        customs.push_back(OpenAsChoice{.id = screen.id, .label = std::move(label), .current = screen.id == screenId});
    }
    if (customs.empty()) {
        return shape;
    }
    shape.choices.push_back(OpenAsChoice{.id = base, .label = titleOf(base), .current = base == screenId});
    shape.choices.insert(shape.choices.end(), customs.begin(), customs.end());
    for (OpenAsChoice const& choice : shape.choices) {
        shape.key += choice.id + '\x1f' + choice.label + (choice.current ? "\x1f*" : "") + '\x1e';
    }
    return shape;
}

void CustomScreenChrome::openAs(WorkspaceTabId tab, std::string const& screenId) {
    ScreenHandle* const handle = handleOf(tab);
    if (_model == nullptr || handle == nullptr || DocumentScreenHost::screenIdOf(*handle) == screenId) {
        return;
    }
    if (std::optional<CustomScreenSummary> const summary = summaryOf(screenId); summary && summary->broken) {
        showStoredSource(screenId);
        return;
    }
    ParamMap const params = DocumentScreenHost::paramsOf(*handle).value_or(ParamMap{});
    auto const replace = [this, tab, screenId, params] {
        if (_model == nullptr || _model->handleOf(tab) == nullptr) {
            return;
        }
        _model->openInPlace(tab, screenId, params,
                            guarded([this, screenId](std::expected<WorkspaceTabId, MountError> opened) {
                                if (!opened) {
                                    notify("Cannot open " + titleOf(screenId) + ": " + opened.error().reason,
                                           Tone::Err);
                                }
                            }));
    };
    if (!isCustomScreenId(screenId)) {
        // Posted: the menu that asked runs in a handler, and the tab's screen is about to be replaced.
        _options.runtime->owner().post(guarded(replace));
        return;
    }
    _options.sync->open(screenId, guarded([this, screenId, replace](
                                              std::expected<std::shared_ptr<Document const>, std::string> loaded) {
        if (!loaded) {
            notify("Cannot open " + titleOf(screenId) + ": " + loaded.error(), Tone::Err);
            return;
        }
        replace();
    }));
}

std::vector<NavigatorCustomEntry> CustomScreenChrome::navigatorEntries() const {
    std::vector<NavigatorCustomEntry> entries;
    if (!_offered->peek()) {
        return entries;
    }
    for (CustomScreenSummary const& screen : _listed.screens) {
        // Read from the base, not `opensWithoutParams`, which is false for a broken screen: a broken screen whose base
        // takes no params is still listed, so its source can be opened and fixed.
        std::shared_ptr<Document const> const base = _options.documents(screen.basedOn);
        if (!base || !base->params.empty()) {
            continue;
        }
        entries.push_back(NavigatorCustomEntry{.id = screen.id,
                                               .title = screen.title,
                                               .savedBy = screen.savedBy,
                                               .mine = screen.mine,
                                               .broken = screen.broken ? screen.broken->reason : std::string{}});
    }
    return entries;
}

void CustomScreenChrome::openCustom(std::string const& screenId, ParamMap params,
                                    std::function<void(std::expected<void, std::string>)> done) {
    if (std::optional<CustomScreenSummary> const summary = summaryOf(screenId); summary && summary->broken) {
        showStoredSource(screenId);
        if (done) {
            done({});
        }
        return;
    }
    _options.sync->open(
        screenId, guarded([this, screenId, params = std::move(params),
                           done](std::expected<std::shared_ptr<Document const>, std::string> loaded) {
            if (!loaded) {
                if (done) {
                    done(std::unexpected(loaded.error()));
                } else {
                    notify("Cannot open " + titleOf(screenId) + ": " + loaded.error(), Tone::Err);
                }
                return;
            }
            _options.openTab(screenId, params);
            if (done) {
                done({});
            }
        }));
}

void CustomScreenChrome::showStoredSource(std::string const& screenId) {
    _options.sync->get(screenId, guarded([this, screenId](std::expected<CustomScreenFound, std::string> found) {
        if (!found) {
            notify("Cannot open the source of " + titleOf(screenId) + ": " + found.error(), Tone::Err);
            return;
        }
        bool const editable = _offered->peek() && found->summary.mine;
        openSource(SourceMode::Stored, std::nullopt, screenId, prettyJson(found->document), !editable);
        if (found->summary.broken) {
            refuse(found->summary.broken->pointer, found->summary.broken->reason);
        }
    }));
}

void CustomScreenChrome::saveFromSource() {
    Source* const source = _source.get();
    if (source == nullptr || source->mode != SourceMode::Stored || source->readOnly) {
        return;
    }
    std::optional<CustomScreenSummary> const summary = summaryOf(source->screenId);
    if (!summary) {
        refuse("", "the custom screen is not listed");
        return;
    }
    std::string const text = source->text->peek();
    std::string const id = source->screenId;
    _options.runtime->batch([source] {
        source->busy->set(true);
        source->status->set(std::string{"Checking…"});
    });
    _options.sync->check(
        summary->basedOn, text,
        guarded([this, id, text, listed = *summary](std::expected<std::optional<CheckRefusal>, std::string> checked) {
            Source* const shown = _source.get();
            if (shown == nullptr || shown->screenId != id) {
                return;
            }
            if (!checked) {
                refuse("", checked.error());
                return;
            }
            if (*checked) {
                refuse((*checked)->pointer, (*checked)->reason);
                return;
            }
            _options.sync->save(
                CustomScreensSave{
                    .app = {}, .id = id, .title = listed.title, .basedOn = listed.basedOn, .document = text},
                guarded([this, title = listed.title](std::expected<std::string, std::string> saved) {
                    if (!saved) {
                        refuse("", saved.error());
                        return;
                    }
                    closeSourceLater();
                    notify("Saved " + title, Tone::Ok);
                }));
        }));
}
```

- [ ] **Step 6: Run the tests**

Run: `flock "$LOCK" cmake --build build/clang-debug --target morph_tests -j5 && flock "$LOCK" timeout 300 build/clang-debug/tests/morph_tests "[chrome]"`
Expected: `All tests passed` (Task 7's cases included).

- [ ] **Step 7: Mutation check.** Apply the six mutations named above the new tests in turn; each named test fails.
  Restore and `touch src/ui/custom_screen_chrome.cpp`.

- [ ] **Step 8: Commit**

```bash
git add include/morph/ui/custom_screen_chrome.hpp src/ui/custom_screen_chrome.cpp tests/ui_custom_client_support.hpp \
  tests/test_ui_custom_screen_chrome.cpp
git commit -m "ui: Open as, the navigator's custom entries, broken screens' source and restored routes"
```

---
### Task 9: The generic client offers custom screens

**Files:**
- Modify: `src/client/shell.cpp` (includes; `Shell::~Shell`; `listen`; `start`; `buildFrame`; `openWanted`;
  `openRoute`; new members `probeCustomScreens`, `customChromeOptions`, `openCustomRoute`; new fields)
- Modify: `docs/spec/client/client.md`
- Test: `tests/client/test_custom_screens.cpp`; `tests/client/CMakeLists.txt`

**Interfaces:**
- Consumes: `CustomScreenSync` (Task 6), `CustomScreenChrome` (Tasks 7–8), `WorkspaceOptions::chrome` (Task 4),
  `Navigator::setCustom` (Task 5), `DocumentScreenHost::drafted` (Task 3), `ShellFrame::confirm` and `showToast`.
- Produces: nothing new for later tasks; the generic client's behaviour, which Task 10's LIMS proof drives.

- [ ] **Step 1: Write the failing tests** (`tests/client/test_custom_screens.cpp`)

```cpp
// SPDX-License-Identifier: Apache-2.0
//
// The generic client's custom screens (docs/spec/client/client.md, "Custom screens"), in process: the shell probes
// the model, lists the person's custom screens in the navigator, opens one after fetching it, previews a hand edit as
// a draft and saves it over the screen, and asks before a tab with a draft closes.

// First, ahead of every header that brings view.hpp: GCC's -Wshadow reports value.hpp's enumerator `Type::Key` when
// view.hpp's `ui::Key` was declared before it.
// clang-format off
#include <morph/ui/value.hpp>
// clang-format on

#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <expected>
#include <future>
#include <memory>
#include <morph/client/shell.hpp>
#include <morph/client/source.hpp>
#include <morph/core/executor.hpp>
#include <morph/core/registry.hpp>
#include <morph/session/session.hpp>
#include <morph/ui/custom_screens.hpp>
#include <morph/ui/service.hpp>
#include <morph/ui/testing/recording_backend.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <testkit/fake_app_context.hpp>
#include <testkit/wait.hpp>
#include <utility>

namespace client = morph::client;
namespace ui = morph::ui;
using morph::examples::testing::FakeAppContext;
using morph::examples::testing::pumpUntil;

namespace {

constexpr std::string_view kHome =
    R"({"vocab":"ui/1","id":"customs.home","title":"Home","view":{"kind":"text","testId":"home","text":"Home"}})";

std::expected<std::shared_ptr<ui::ServedBundle const>, ui::Refusal> customsBundle() {
    ui::UiRegistry served{ui::ApplicationInfo{.id = "customs", .version = "1"}};
    ui::CatalogEntry entry;
    entry.id = "customs.home";
    entry.title = "Home";
    if (auto added = served.addScreen(std::move(entry), std::string{kHome}); !added) {
        return std::unexpected(std::move(added.error()));
    }
    return served.build();
}

/// `text` with its first `from` replaced by `to`.
std::string replaced(std::string text, std::string_view from, std::string_view to) {
    auto const at = text.find(from);
    REQUIRE(at != std::string::npos);
    text.replace(at, from.size(), to);
    return text;
}

/// The custom screens model on the process's dispatcher and registry, where an in-process source's models run, over
/// a memory store holding `ann`'s screen "Mine", based on the home screen. ctest runs each case in a process of its
/// own (the cases are discovered PRE_TEST), so the registration reaches no other case.
struct Offered {
    Offered() {
        auto built = customsBundle();
        REQUIRE(built.has_value());
        bundle = *built;
        screens = ui::registerCustomScreens(store, [held = bundle] { return held; }, morph::session::allowAllAuthorizer(),
                                            morph::model::detail::defaultDispatcher(),
                                            morph::model::detail::defaultRegistry());
        std::promise<void> stored;
        store
            ->put(ui::CustomScreenKey{.person = "ann", .app = "customs", .id = mineId},
                  ui::CustomScreen{.id = mineId,
                                   .title = "Mine",
                                   .basedOn = "customs.home",
                                   .document = replaced(std::string{kHome}, "\"customs.home\"", "\"" + mineId + "\""),
                                   .savedAt = {},
                                   .savedBy = "ann"})
            .thenDetached([&stored](std::expected<void, ui::CustomScreenRefusal> const& /*result*/) {
                stored.set_value();
            });
        REQUIRE(stored.get_future().wait_for(std::chrono::seconds{5}) == std::future_status::ready);
    }

    std::string const mineId = "custom:" + std::string(31, '0') + "a";
    morph::exec::ThreadPoolExecutor pool{1};
    std::shared_ptr<ui::MemoryCustomScreenStore> store = std::make_shared<ui::MemoryCustomScreenStore>(pool);
    std::shared_ptr<ui::ServedBundle const> bundle;
    std::shared_ptr<ui::CustomScreens> screens;
};

/// The generic client over the customs application, signed in locally as `ann`, until the navigator lists "Mine".
struct Window {
    explicit Window(Offered const& offered) : mineId{offered.mineId} {
        std::optional<std::expected<ui::Bundle, ui::ConnectError>> opened;
        source.open(context,
                    [&opened](std::expected<ui::Bundle, ui::ConnectError> result) { opened = std::move(result); });
        REQUIRE(pumpUntil(context.owner(), [&opened] { return opened.has_value(); }));
        REQUIRE(opened->has_value());
        shell = client::mountShell(context, backend, std::move(*opened), source,
                                   client::ShellOptions{.startScreen = "customs.home"});
        REQUIRE(wait([this] { return find("Button", "nav_" + mineId).has_value(); }));
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
    [[nodiscard]] bool shown(std::string_view kind, std::string const& testId) const {
        std::optional<int> const widget = find(kind, testId);
        return widget && backend.prop(*widget, "visible") != "false";
    }
    template <typename Pred>
    bool wait(Pred pred) {
        return pumpUntil(context.owner(), pred);
    }
    void click(std::string const& testId) {
        REQUIRE(wait([this, &testId] { return find("Button", testId).has_value(); }));
        backend.click(*find("Button", testId));
    }

    /// Opens "Mine" from the navigator, edits its text in the source view, and previews it, until the draft shows.
    void draftMine() {
        click("nav_" + mineId);
        click("source_button_" + mineId);
        REQUIRE(wait([this] { return find("Button", "source_preview").has_value(); }));
        std::optional<int> const field = find("TextInput", "source_text");
        REQUIRE(field.has_value());
        std::string const pretty = ui::prettyJson(replaced(std::string{kHome}, "\"customs.home\"", "\"" + mineId + "\""));
        backend.edit(*field, replaced(pretty, "\"Home\"", "\"My home\""));
        click("source_preview");
        REQUIRE(wait([this] { return shown("Row", "draft_bar_" + mineId); }));
    }

    std::string mineId;
    FakeAppContext context;
    ui::testing::RecordingBackend backend;
    client::LocalSource source{client::LocalApp{.name = "customs", .bundle = &customsBundle},
                               client::LocalSetup{.db = {}, .user = "ann"}};
    std::unique_ptr<ui::MountedShell> shell;
};

}  // namespace

// Mutations: mount a navigator's custom entry without fetching it (the mount finds no document: no source button for it
// shows); drop the custom branch of the host's documents lookup (the same).
TEST_CASE("Client shell: the navigator opens a custom screen, Source previews a draft, and Save overwrites it",
          "[client][custom-screens]") {
    Offered offered;
    Window window{offered};
    window.draftMine();
    REQUIRE(window.wait([&window] {
        std::optional<int> const text = window.backend.find("Text", "text", "My home");
        return text.has_value();
    }));
    window.click("draft_save_" + window.mineId);
    REQUIRE(window.wait([&window] { return !window.shown("Row", "draft_bar_" + window.mineId); }));
}

// Mutation: answer every close of a drafted screen with Cancel in the client's guard (no confirmation is asked).
TEST_CASE("Client shell: closing a tab with a draft asks first, and Discard closes it", "[client][custom-screens]") {
    Offered offered;
    Window window{offered};
    window.draftMine();
    auto const tabs = window.backend.all("Tabs");
    REQUIRE_FALSE(tabs.empty());
    CHECK(window.backend.press(tabs.front(), "Ctrl+W"));
    REQUIRE(window.wait([&window] { return window.find("Dialog", "shell_confirm").has_value(); }));
    window.click("shell_confirm_yes");
    REQUIRE(window.wait([&window] { return !window.find("Button", "draft_save_" + window.mineId).has_value(); }));
}
```

Add `#include <morph/ui/json_source.hpp>` to the includes. Add `test_custom_screens.cpp` to `morph_client_tests` in
`tests/client/CMakeLists.txt`, after `test_client_args.cpp`.

- [ ] **Step 2: Run them to verify they fail**

Run: `flock "$LOCK" cmake --build build/clang-debug --target morph_client_tests -j5 && flock "$LOCK" timeout 300 build/clang-debug/tests/client/morph_client_tests "[custom-screens]"`
Expected: both fail at `REQUIRE( wait(... "nav_" + mineId ...) )`: the shell lists no custom screens yet.

- [ ] **Step 3: Wire the sync and the chrome** in `src/client/shell.cpp`.

Add the includes `#include <morph/ui/custom_screen_chrome.hpp>` and `#include <morph/ui/custom_screen_sync.hpp>`.

Among `Shell`'s fields, after `std::unique_ptr<Host> _host;`:

```cpp
    // The client of the server's custom screens, and their chrome over each frame's workspace. After the host, which
    // the chrome mounts drafts through, so they go before it; the frame and the workspace go before them.
    std::unique_ptr<ui::CustomScreenSync> _customSync;
    std::unique_ptr<ui::CustomScreenChrome> _customChrome;
```

In `~Shell`, before `_workspace.reset();`:

```cpp
        if (_customChrome) {
            _customChrome->attach(nullptr);
        }
```

In `listen`, in the `onConnected` listener after the chat's `reconnected`:

```cpp
            if (auto const live = alive.lock(); live != nullptr && *live && _customSync) {
                _customSync->reconnected();
            }
```

and in the `onEvent` listener after the chat's `deliverEvent`:

```cpp
            if (auto const live = alive.lock(); live != nullptr && *live && _customSync) {
                _customSync->deliverEvent(event);
            }
```

In `start`, after `showPermitted();`:

```cpp
        // Through the screens' own dispatcher, so its calls carry their session and go where their models go.
        _customSync = std::make_unique<ui::CustomScreenSync>(
            source.dispatcher(), source.bundle() != nullptr ? source.bundle()->applicationId : std::string{},
            [this] { return _source->catalog(); }, source.watcher());
```

At the top of the `hostOptions.documents` lambda, before `auto const found = _documents.find(screenId);`:

```cpp
            if (ui::isCustomScreenId(screenId)) {
                return _customSync->documents().find(screenId);
            }
```

After `hostOptions.backend = _backend;`:

```cpp
        // A draft asks before its tab closes, as unsaved edits do; every other dirty screen keeps this client's rule
        // of staying open.
        hostOptions.guard = [this](ui::ScreenHandle& screen, std::function<void(ui::CloseChoice)> const& choose) {
            if (!ui::DocumentScreenHost::drafted(screen) || _frame == nullptr) {
                choose(ui::CloseChoice::Cancel);
                return;
            }
            _frame->confirm("Discard the draft of " + screen.title().peek() + "?", "Discard", "Keep",
                            [choose](bool discard) {
                                choose(discard ? ui::CloseChoice::Discard : ui::CloseChoice::Cancel);
                            });
        };
```

After `_host = std::make_unique<Host>(…);`:

```cpp
        _customChrome = std::make_unique<ui::CustomScreenChrome>(customChromeOptions());
        _customSync->setOnListed([this](ui::CustomScreensListed const& listed) { _customChrome->listed(listed); });
```

- [ ] **Step 4: Probe per frame, and open custom entries through the chrome.** In `buildFrame`, before
  `_workspace.reset();`:

```cpp
        // A new frame is a new session's: what the old one learnt about custom screens is forgotten.
        if (_customChrome) {
            _customChrome->attach(nullptr);
            _customSync->reset();
            _customChrome->setOffered(false);
        }
```

Replace the body of the posted lambda in `frame.navigator.onOpen` with:

```cpp
                if (auto const live = alive.lock(); live != nullptr && *live) {
                    if (ui::isCustomScreenId(screenId) && _customChrome) {
                        _customChrome->openCustom(screenId, {});
                    } else {
                        mount(screenId, {});
                    }
                }
```

Before `_headInPage = …`, add:

```cpp
        tabs.chrome = [this](ui::WorkspaceTabId tab, std::string_view screenId) -> ui::Node {
            return _customChrome ? _customChrome->chromeFor(tab, screenId) : nullptr;
        };
```

After `_workspace = std::make_unique<ui::Workspace>(…);`:

```cpp
        if (_customChrome) {
            _customChrome->attach(&_workspace->model());
            probeCustomScreens();
        }
```

Add these members to `Shell`, after `buildChat`:

```cpp
    // Asks the server whether it offers custom screens, for the session the frame was built for.
    void probeCustomScreens() {
        _customSync->probe([this, alive = std::weak_ptr<bool>{_alive}](bool offered) {
            if (auto const live = alive.lock(); live != nullptr && *live && _customChrome) {
                _customChrome->setOffered(offered);
            }
        });
    }

    // What the custom screens chrome works with: the host, the bundle's documents and catalog, the navigator's
    // titles, this shell's way of opening a tab, and its toast.
    ui::CustomScreenChromeOptions customChromeOptions() {
        ui::CustomScreenChromeOptions options;
        options.runtime = &_context->runtime();
        options.backend = _backend;
        options.host = _host.get();
        options.sync = _customSync.get();
        options.documents = [this](std::string_view screenId) -> std::shared_ptr<ui::Document const> {
            auto const found = _documents.find(screenId);
            return found == _documents.end() ? nullptr : found->second;
        };
        options.sourceOf = [this](std::string_view screenId) -> std::string {
            ui::Bundle const* const bundle = _source->bundle();
            if (bundle == nullptr) {
                return {};
            }
            auto const found = bundle->documents.find(screenId);
            return found == bundle->documents.end() ? std::string{} : found->second;
        };
        options.catalog = [this] { return _source->catalog(); };
        options.titleOf = [this](std::string_view screenId) -> std::string {
            if (ui::CatalogReply const* const catalog = _source->catalogReply()) {
                auto const entry = std::ranges::find(catalog->entries, screenId, &ui::CatalogEntry::id);
                if (entry != catalog->entries.end()) {
                    std::vector<std::string> const titles = ui::catalogTitlesIn(*entry, &_catalogues, currentLocale());
                    if (!titles.empty()) {
                        return titles.front();
                    }
                }
            }
            return std::string{screenId};
        };
        options.openTab = [this](std::string const& screenId, ui::ParamMap const& params) {
            static_cast<void>(mount(screenId, params));
        };
        options.notify = [this](std::string const& text, ui::Tone tone) {
            if (_frame != nullptr) {
                _frame->showToast(text, tone);
            }
        };
        options.onListChanged = [this] {
            if (_frame != nullptr && _customChrome != nullptr) {
                _frame->navigator().setCustom(_customChrome->navigatorEntries());
            }
        };
        return options;
    }
```

- [ ] **Step 5: Custom routes.** Add, after `openWanted`:

```cpp
    // A route to a custom screen whose document is not fetched yet: fetched, then opened; or the person is told it was
    // not restored and the start screen opens. Answers whether the route was one.
    bool openCustomRoute(std::string const& route) {
        std::string const screenId{ui::routeScreen(route)};
        if (!ui::isCustomScreenId(screenId) || _customSync == nullptr ||
            _customSync->documents().find(screenId) != nullptr) {
            return false;
        }
        _customSync->open(screenId, [this, alive = std::weak_ptr<bool>{_alive}, route, screenId](
                                        std::expected<std::shared_ptr<ui::Document const>, std::string> const& loaded) {
            auto const live = alive.lock();
            if (live == nullptr || !*live || _host == nullptr) {
                return;
            }
            std::expected<ui::Route, std::string> const parsed =
                loaded ? _host->parseRoute(route) : std::expected<ui::Route, std::string>{std::unexpected(loaded.error())};
            if (!parsed) {
                if (_frame != nullptr) {
                    _frame->showToast("The custom screen " + screenId + " was not restored: " + parsed.error(),
                                      ui::Tone::Warn);
                }
                mount(firstScreen(_signedIn), {});
                return;
            }
            mount(parsed->screen, parsed->params);
        });
        return true;
    }
```

In `openWanted`, after `if (_wanted.empty()) { return false; }`, add:

```cpp
        if (openCustomRoute(_wanted)) {
            _wanted.clear();
            return true;
        }
```

and change `if (!route || !_documents.contains(route->screen)) {` to
`if (!route || (!_documents.contains(route->screen) && !ui::isCustomScreenId(route->screen))) {`.

In `openRoute`, after the `_reloading` block, add:

```cpp
        if (openCustomRoute(route)) {
            return;
        }
```

and change `if (parsed && _documents.contains(parsed->screen)) {` to
`if (parsed && (_documents.contains(parsed->screen) || ui::isCustomScreenId(parsed->screen))) {`.

- [ ] **Step 6: Run the tests**

Run:
```bash
flock "$LOCK" cmake --build build/clang-debug --target morph_client_tests -j5
flock "$LOCK" timeout 300 build/clang-debug/tests/client/morph_client_tests
```
Expected: `All tests passed` (the existing shell tests included: the chrome row and the probe must change nothing they
check). Then run the two new cases one process each, as ctest does:
`ctest --test-dir build/clang-debug -R "custom-screens|custom screen" -j6 --output-on-failure` — expected: they pass.
Last, check that no other shell test now logs a refused probe: `flock "$LOCK" timeout 300
build/clang-debug/tests/client/morph_client_tests 2>&1 | grep -c "custom screens are off"` — expected `0`. A count above
0 means an in-process source refuses the model's registration with other words than `unknown model type`: report the
line, since discovery then warns on every in-process client.

- [ ] **Step 7: Mutation check.** Apply the three mutations named above the tests in turn; each named test fails.
  Restore and `touch src/client/shell.cpp`.

- [ ] **Step 8: Document it.** In `docs/spec/client/client.md`, add `- [Custom screens](#custom-screens)` to Contents
  after `- [Chat](#chat)`, and append:

```markdown
## Custom screens

The shell offers a person's custom screens when the server does ([`../ui/custom_screens.md`](../ui/custom_screens.md#the-client)).

- **Discovery.** Each frame (the first, and each one a sign-in or sign-out builds) resets a `CustomScreenSync` over
  the source's dispatcher and asks `list`. A server without the model refuses its registration with `unknown model
  type: morph.customScreens`, and the shell offers no custom screens; any other refusal is the same for the person and
  is logged at warn. The sync watches the record `morph.customScreens` through the source's watcher, again after each
  reconnect and resync, and relists on a `changes` event naming it; an in-process source has no events, so its lists
  refresh at a frame, a reconnect and this client's own saves.
- **The chrome.** Every document screen's tab carries the `CustomScreenChrome` at the top of its page
  (`WorkspaceOptions::chrome`): **Source**, **Open as** for a screen with params, and the draft bar. A host screen,
  such as the explorer, has none.
- **Opening.** The host's documents lookup answers a `custom:` id from the sync's cache. A navigator entry, **Open as**
  and a route to a custom screen fetch its document first; a route whose screen `get` does not answer is not restored,
  a toast says so, and the start screen opens.
- **Closing a draft.** The shell's close guard asks before a tab with a draft closes (`ShellFrame::confirm`:
  "Discard the draft of …?", Discard or Keep). Every other dirty screen without `onClose` stays open, as before.
- **The navigator.** The chrome's `navigatorEntries` fill the navigator's Custom section after each list.
```

- [ ] **Step 9: Commit**

```bash
git add src/client/shell.cpp tests/client/test_custom_screens.cpp tests/client/CMakeLists.txt docs/spec/client/client.md
git commit -m "client: offer custom screens — Source, drafts, Open as and the navigator's Custom section"
```

---
### Task 10: The proof on the LIMS sample screen

**Files:**
- Modify: `examples/common/testkit/screen_harness.hpp` (`SimulatedAppSource`, `ScreenHarness`'s constructor)
- Test: `examples/lims/screens/tests/test_lims_custom_screens.cpp` (picked up by `ladder_lims_screen_tests`'s glob)

**Interfaces:**
- Consumes: the generic client's custom screens (Task 9); `registerCustomScreens` and `MemoryCustomScreenStore`
  (plan 1).
- Produces: `SimulatedAppSource(LocalApp, std::shared_ptr<ui::ICustomScreenStore> customScreens = nullptr)` and a
  trailing `std::shared_ptr<ui::ICustomScreenStore> customScreens = nullptr` parameter of `ScreenHarness`'s
  constructor. No LIMS code changes.

- [ ] **Step 1: Write the failing test** (`examples/lims/screens/tests/test_lims_custom_screens.cpp`)

```cpp
// SPDX-License-Identifier: Apache-2.0
//
// Custom screens proved on lims' sample screen, with no lims code for them: the source of a sample's window is edited
// by hand and previewed as a draft, saved as "My sample", and after a new connection a sample's window offers
// Open as → My sample, which opens it in the same tab.

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <memory>
#include <morph/client/source.hpp>
#include <morph/core/executor.hpp>
#include <morph/session/session_auth.hpp>
#include <morph/ui/custom_screens.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <testkit/screen_harness.hpp>

#include "lims/app/lims_authorizer.hpp"
#include "lims/org/models/org_model.hpp"
#include "lims_screens.hpp"
#include "lims_test_support.hpp"

namespace {

using morph::client::LocalApp;
using morph::client::LocalSetup;
using morph::examples::testing::RigMode;
using morph::examples::testing::ScreenHarness;

std::string const kSample{lims::screens::kSample};

/// lims behind a simulated server that checks tokens with lims' own role lookup, as its server does.
LocalApp limsServer() {
    LocalApp app{.name = "lims", .bundle = &lims::screens::bundle, .setUp = {}};
    app.setUp = [](LocalSetup const& setup) { lims::screens::prepareLocal(setup.user); };
    app.serverAuthorizer = [] {
        return std::make_shared<lims::auth::LimsAuthorizer>(
            "lims-local-development", morph::session::hmacSha256,
            [](std::string_view principal) { return lims::OrgModel::rolesAnywhere(principal); });
    };
    return app;
}

/// A text input's text as RecordingBackend records it, with its escapes undone.
std::string unformatted(std::string_view recorded) {
    std::string out;
    for (std::size_t at = 0; at < recorded.size(); ++at) {
        if (recorded[at] == '\\' && at + 1 < recorded.size()) {
            char const next = recorded[++at];
            out += next == 'n' ? '\n' : next == 'r' ? '\r' : next;
            continue;
        }
        out += recorded[at];
    }
    return out;
}

bool startsWith(ScreenHarness const& harness, std::string_view testId, std::string_view prefix) {
    std::optional<std::string> const shown = harness.text(testId);
    return shown.has_value() && shown->starts_with(prefix);
}

bool shown(ScreenHarness& harness, std::string_view kind, std::string const& testId) {
    std::optional<int> const widget = harness.find(kind, testId);
    return widget.has_value() && harness.backend().prop(*widget, "visible") != "false";
}

void reportProblems(ScreenHarness const& harness) {
    for (std::string const& problem : harness.problems()) {
        UNSCOPED_INFO(problem);
    }
}

/// Signs in as `tech` and opens a new sample from the navigator, until its window's Source shows.
void signInAndOpenSample(ScreenHarness& harness) {
    harness.edit("username", "tech");
    harness.click("signIn");
    REQUIRE(harness.waitFor(
        [&harness] { return !harness.mounted().empty() && harness.mounted().back() == lims::screens::kRegister; }));
    REQUIRE(harness.waitFor([&harness] { return harness.find("Button", "nav_" + kSample).has_value(); }));
    std::optional<int> const navigator = harness.find("Column", "navigator");
    REQUIRE(navigator.has_value());
    int const entry = *harness.find("Button", "nav_" + kSample);
    // With module groups on, the sample's entry is listed only while its module (the legacy screens', "") is open.
    if (!harness.shownUnder(*navigator, entry)) {
        harness.click("nav_module_");
    }
    REQUIRE(harness.waitFor([&] { return harness.shownUnder(*navigator, entry); }));
    harness.click("nav_" + kSample);
    REQUIRE(harness.waitFor([&harness] { return harness.find("Button", "source_button_" + kSample).has_value(); }));
}

}  // namespace

// Mutations: skip mountDraft in CustomScreenChrome::preview (`projectName` never reads "Chosen project:"); drop the
// shell's `tabs.chrome` (no `source_button_lims.sample` shows); keep Open as off a screen with params (the second window
// has no `open_as_lims.sample`).
TEST_CASE("lims custom screens: a hand-edited sample screen is saved as My sample and opened as it after a reconnect",
          "[lims-screens][custom-screens]") {
    lims::test::LabFixture const fixture;
    morph::exec::ThreadPoolExecutor storePool{1};
    auto const store = std::make_shared<morph::ui::MemoryCustomScreenStore>(storePool);
    LocalApp const app = limsServer();
    {
        ScreenHarness harness{app,           std::string{lims::screens::kLogin}, RigMode::Simulated, {},
                              LocalSetup{}, "expanded",                         store};
        signInAndOpenSample(harness);
        REQUIRE(harness.waitFor([&harness] { return startsWith(harness, "projectName", "Project:"); }));

        harness.click("source_button_" + kSample);
        REQUIRE(harness.waitFor([&harness] { return harness.find("Button", "source_preview").has_value(); }));
        std::optional<int> const field = harness.find("TextInput", "source_text");
        REQUIRE(field.has_value());
        std::string edited = unformatted(harness.backend().prop(*field, "text"));
        std::string_view const heading = "\"Project: {0}\"";
        auto const at = edited.find(heading);
        REQUIRE(at != std::string::npos);
        edited.replace(at, heading.size(), "\"Chosen project: {0}\"");
        harness.edit("source_text", edited);
        harness.click("source_preview");
        REQUIRE(harness.waitFor([&harness] { return startsWith(harness, "projectName", "Chosen project:"); }));
        CHECK(shown(harness, "Row", "draft_bar_" + kSample));

        harness.click("draft_save_as_" + kSample);
        harness.edit("save_as_title", "My sample");
        harness.click("save_as_confirm");
        REQUIRE(harness.waitFor([&harness] { return !harness.find("Row", "draft_bar_" + kSample).has_value(); }));
        CHECK(startsWith(harness, "projectName", "Chosen project:"));
        reportProblems(harness);
        CHECK(harness.problems().empty());
    }

    // Another window: a new connection to a server over the same store, signed in again.
    ScreenHarness harness{app,           std::string{lims::screens::kLogin}, RigMode::Simulated, {},
                          LocalSetup{}, "expanded",                         store};
    signInAndOpenSample(harness);
    REQUIRE(harness.waitFor([&harness] { return harness.find("Menu", "open_as_" + kSample).has_value(); }));
    int const menu = *harness.find("Menu", "open_as_" + kSample);
    CHECK(harness.backend().prop(menu, "items").contains("My sample"));
    harness.backend().chooseMenuEntry(menu, {1});
    REQUIRE(harness.waitFor([&harness] { return startsWith(harness, "projectName", "Chosen project:"); }));
    reportProblems(harness);
    CHECK(harness.problems().empty());
}
```

- [ ] **Step 2: Run it to verify it fails**

Run: `flock "$LOCK" cmake --build build/clang-debug --target ladder_lims_screen_tests -j5`
Expected: compile error, no `ScreenHarness` constructor takes eight arguments.

- [ ] **Step 3: Let the simulated server offer custom screens.** In `examples/common/testkit/screen_harness.hpp`, add
  `#include <morph/core/registry.hpp>` and `#include <morph/ui/custom_screens.hpp>`. Replace `SimulatedAppSource`'s
  constructor with:

```cpp
    /// @brief A source for @p app.
    /// @param app           The application.
    /// @param customScreens The store the server offers custom screens over; null offers none.
    explicit SimulatedAppSource(::morph::client::LocalApp app,
                                std::shared_ptr<ui::ICustomScreenStore> customScreens = nullptr)
        : _app{std::move(app)}, _customScreens{std::move(customScreens)} {}
```

In `connect`, replace from `config.services.push_back(std::make_shared<ui::UiService>(std::move(*bundle)));` through
the statement that assigns `_server` with:

```cpp
        std::shared_ptr<ui::ServedBundle const> const served = std::move(*bundle);
        config.services.push_back(std::make_shared<ui::UiService>(served));
        // The server waits for a model's reported instant on the ladder's clock, which a test moves by hand, so it
        // reads the clock often.
        config.clock = [] { return *::morph::ladder::now().value; };
        config.clockRecheck = std::chrono::milliseconds{20};
        std::shared_ptr<session::IAuthorizer> const authorizer =
            _app.serverAuthorizer ? _app.serverAuthorizer() : session::allowAllAuthorizer();
        if (_customScreens) {
            // On the process's dispatcher and registry, where the application's models are and which the server below
            // serves from; a later harness registers again over its own store.
            static_cast<void>(ui::registerCustomScreens(_customScreens, [served] { return served; }, authorizer,
                                                        model::detail::defaultDispatcher(),
                                                        model::detail::defaultRegistry()));
        }
        _server = std::make_shared<backend::RemoteServer>(*_workers, authorizer, config);
```

Add the field after `::morph::client::LocalApp _app;`:

```cpp
    std::shared_ptr<ui::ICustomScreenStore> _customScreens;
```

In `ScreenHarness`, add to the constructor's doc comment
`/// @param customScreens The store a simulated server offers custom screens over; null offers none. Refused in local mode.`
and a trailing parameter `std::shared_ptr<ui::ICustomScreenStore> customScreens = nullptr`, and replace the source
choice with:

```cpp
        if (mode == RigMode::Local) {
            if (customScreens) {
                throw std::invalid_argument{"ScreenHarness: custom screens are offered by a simulated server only"};
            }
            _source = std::make_unique<::morph::client::LocalSource>(app, std::move(setup));
        } else {
            _source = std::make_unique<SimulatedAppSource>(app, std::move(customScreens));
        }
```

- [ ] **Step 4: Run it**

Run:
```bash
flock "$LOCK" cmake --build build/clang-debug --target ladder_lims_screen_tests -j5
flock "$LOCK" timeout 300 "$(find build/clang-debug -name ladder_lims_screen_tests -type f)" "[custom-screens]"
```
Expected: `All tests passed`. If the server's `check` refuses the sample document, print the refusal
(`harness.text("draft_message_lims.sample")` or the source view's `source_refusal` label) and stop: the base rule or
a permission refuses an unchanged LIMS screen, which is a defect in plan 1's checks, not in this test.

- [ ] **Step 5: The rest of the rung stays green**

Run: `ctest --test-dir build/clang-debug -R "lims.screens." -j6 --output-on-failure`
Expected: every LIMS screen test passes: the chrome row and the probe change nothing they check.

- [ ] **Step 6: Mutation check.** Apply the three mutations named above the test in turn; the test fails at the step
  each names. Restore and `touch` the files.

- [ ] **Step 7: Commit**

```bash
git add examples/common/testkit/screen_harness.hpp examples/lims/screens/tests/test_lims_custom_screens.cpp
git commit -m "lims: prove custom screens on the sample screen, with no lims code for them"
```

---
### Task 11: The client's spec page, the index and the changelog

**Files:**
- Modify: `docs/spec/ui/custom_screens.md`, `docs/spec/README.md`, `CHANGELOG.md`

- [ ] **Step 1: The client's section.** In `docs/spec/ui/custom_screens.md`: change the first paragraph's last
  sentence `this page is the server side of it: the store seam, the model, the checks and the base rule.` to `this
  page is the store seam, the model, the checks and the base rule on the server, and the client that offers them.`;
  add rows to the header table:

```markdown
| `include/morph/ui/custom_screen_sync.hpp` | `isCustomScreenId`, `routeScreen`, `CustomDocuments`, `CustomScreensOffer`, `CustomScreenSync` |
| `include/morph/ui/custom_screen_chrome.hpp` | `CustomScreenChromeOptions`, `CustomScreenChrome` |
| `include/morph/ui/json_source.hpp` | `prettyJson`, `pointerOffset`, `lineAt` |
```

add `- [The client](#the-client)` and `- [Not yet](#not-yet)` to Contents after `- [Records and effects](#records-and-effects)`,
and insert before `## Design decisions`:

```markdown
## The client

The client half is three framework pieces and the generic client's wiring
([`../client/client.md`](../client/client.md#custom-screens)). Drafts themselves are the document host's
([`host.md`](host.md), "Drafts").

**`CustomScreenSync`** talks to `morph.customScreens` for one application through the interpreter's own `Dispatcher`,
as `PreferenceSync` talks to the preferences model, so its calls carry the screens' session and go where their models
go. No call names a person.

- **Discovery.** `probe` calls `list`. An answer makes the offer `Offered`; any refusal makes it `Off`. A server that
  never registered the model refuses its registration with `unknown model type: morph.customScreens`, the ordinary way
  to say custom screens are not offered, and is not logged. Any other refusal (a server that has the model and refuses
  this client, a wrong `app`) is logged at warn, because it is a misconfiguration someone has to hear about, and custom
  screens are off for the person all the same. `offRefusal()` keeps the text.
- **The cache.** `CustomDocuments` keeps what `get` answered, loaded against the client's catalog, by id. It is what
  `DocumentScreenHostOptions::documents` answers for a `custom:` id, and that lookup is synchronous, so whatever opens a
  custom screen (a navigator entry, **Open as**, a restored route) fetches it first (`open` answers a kept document
  or `fetch`es one). A list that shows a kept screen with another `hash`, or a whole list that no longer shows it,
  drops it; the next open fetches the stored document again. A document that does not load in the client, or names
  another id, is not kept.
- **Lists stay current.** Once offered, the sync watches the record `morph.customScreens` through the source's
  watcher, with `replace: false`, and watches again after each reconnect and each `resync`, since both replace the
  connection's watch set. A `changes` event naming the record relists. An in-process source sends no events, so its
  lists refresh at a frame, a reconnect and this client's own saves. A refused watch is logged at warn.
- **Calls.** `check` answers the server's first refusal with its pointer; `save` passes a store's refusal through as
  the store worded it, and relists after an answer; `get` answers without keeping, for a broken screen's source.
- **Restore.** `prefetchRoutes(routes, done)` fetches every custom screen a record or a history names that is not
  kept, and answers the ids no document came for, so a start can tell the person which were not restored.
- **Sessions.** `reset` forgets the offer, the list, the cache and the binding, and drops replies in flight: what a
  sign-in or sign-out does before probing again.

**`CustomScreenChrome`** is the window chrome a shell mounts at the top of each document screen's page
(`WorkspaceOptions::chrome`), built from view nodes so every frontend shows it. A host screen gets none.

| Part | Test id | What it does |
|---|---|---|
| The draft bar | `draft_bar_<id>` | Shown while the tab holds a draft: "Draft", that forms on a draft change real data, the last refusal of a save (`draft_message_<id>`), **Save** (`draft_save_<id>`, for a draft of the person's own custom screen), **Save as…**, **Source**, **Discard** |
| The tools row | `screen_tools_<id>` | Shown while the tab holds none: **Open as** (`open_as_<id>`) and **Source** (`source_button_<id>`) |
| The source view | `source_dialog` | The document's JSON pretty-printed in a monospace editor (`source_text`), the refusal (`source_refusal`), **Preview**, **Save as…**, **Close** |
| The save dialog | `save_as_dialog` | The name (`save_as_title`), the refusal, **Save**, **Cancel** |

- **Source** shows the open draft's text, a custom screen's stored text, or an application screen's as the bundle
  has it, through `prettyJson`. While custom screens are not offered it is read only, with no **Preview** and no
  **Save as…**.
- **Preview** loads the text in the client (`loadDocumentText`), refuses a document whose `id` is not the screen's at
  `/id` (a draft keeps the screen's id), then asks the server's `check` against the screen's base, and mounts what
  passes with `DocumentScreenHost::mountDraft`, closing the view. A refusal shows "Line N: reason", the line its
  pointer names in the text as the person left it (`pointerOffset`, `lineAt`); choosing it moves the cursor there
  (`Mounted::placeCursor`). The client's checks are fast feedback; the server's decide.
- **Save as…** asks a name, saves the draft (from the source view, it previews first, so nothing is saved the person
  has not seen mounted), ends the draft and opens the saved screen in the tab's place, with the same params
  (`WorkspaceModel::openInPlace`). **Save** overwrites the person's own custom screen with the draft, fetches the
  stored document again and ends the draft. A refusal shows in the bar and in the dialog, and the draft stays.
- **Discard** mounts the screen's own document again (`discardDraft`), fetching a custom screen's first when the cache
  no longer holds it.
- **One draft per window**: a new preview replaces the tab's draft. Closing a tab with a draft goes to the shell's
  close guard, since the handle is dirty while a draft is up.
- **Open as** is offered on a screen whose base takes params, when the list holds custom screens of that base: the
  base first, then each of them by its store title, "· by <savedBy>" when not the person's, "· broken" when broken. A
  choice opens in the tab's place with the tab's params.
- **The navigator's entries** (`navigatorEntries`) are the listed screens whose base takes no params, read from the
  base rather than from `opensWithoutParams`, which a broken screen does not have, so a broken screen is still listed
  with its reason.
- **A broken screen** is never mounted: opening it from the navigator or **Open as** opens its source view as `get`
  answers it, the breakage shown as the refusal. For the person's own screen while offered, **Check and save**
  (`source_save`) checks the text on the server and saves it over the screen.

## Not yet

- **The chrome's words** ("Draft", "Save as…", "Source", "Open as", the refusal's "Line N") are English; resolving
  them through the bundle's message catalogues, as the shell frame's `morph.shell.*` labels are, is still to come.
- **Removing a custom screen.** The model's `remove` has no control in the client yet.
- **The terminal's source view** is the same dialog; it is best effort, as the spec says.
```

- [ ] **Step 2: Index it.** In `docs/spec/README.md`, change the `ui/custom_screens.md` entry to:

```markdown
[`ui/custom_screens.md`](ui/custom_screens.md) — a person's own screens: the store seam the application implements,
the checks and the base rule every custom document passes, the model that serves them, and the client's drafts,
source view, Open as and navigator section ·
```

- [ ] **Step 3: The changelog.** In `CHANGELOG.md`, under `## [Unreleased]`, `### Added` (the first one), add:

```markdown
- **Custom screens in the generic client.** A server that registers `morph.customScreens` gets, in every client:
  **Source** on each document screen (its `ui/1` JSON, pretty-printed), **Preview** through the client's load and the
  server's check into a draft mounted in place, a draft bar with Save, Save as…, Source and Discard, the navigator's
  Custom section, **Open as** on screens with params, and custom screens in routes. A server without the model shows
  the source read only. New framework pieces: `CustomScreenSync`, `CustomScreenChrome`,
  `DocumentScreenHost::mountDraft`, `WorkspaceModel::openInPlace`, `WorkspaceOptions::chrome`,
  `Navigator::setCustom`, and `TextInputWidget::setCursor` on every backend.
```

- [ ] **Step 4: Build the documentation the way CI does**

Run:
```bash
flock "$LOCK" cmake -S . -B build/docs -G Ninja -DMORPH_BUILD_DOCUMENTATION=ON -DMORPH_BUILD_TESTS=OFF -DMORPH_BUILD_EXAMPLES=OFF
flock "$LOCK" cmake --build build/docs --target doc
```
Expected: the `doc` target finishes with no warnings. A warning fails CI (`FAIL_ON_WARNINGS`): fix every
`@param`/`@return` it names in the headers this plan touched.

- [ ] **Step 5: Commit**

```bash
git add docs/spec/ui/custom_screens.md docs/spec/README.md CHANGELOG.md
git commit -m "docs: the custom screens client"
```

---

### Task 12: Verify against the CI configurations

**Files:** none, unless a check finds something.

- [ ] **Step 1: Every suite this plan touches**

Run:
```bash
flock "$LOCK" cmake --build build/clang-debug -j5
flock "$LOCK" timeout 600 build/clang-debug/tests/morph_tests
flock "$LOCK" timeout 300 build/clang-debug/tests/client/morph_client_tests
flock "$LOCK" timeout 300 env QT_QPA_PLATFORM=offscreen build/clang-debug/tests/qt_quick/morph_qt_quick_tests
flock "$LOCK" timeout 300 build/clang-debug/tests/tui/morph_tui_tests
flock "$LOCK" timeout 300 env QT_QPA_PLATFORM=offscreen "$(find build/clang-debug -name ladder_lims_render_smoke -type f)"
ctest --test-dir build/clang-debug -j6 --output-on-failure
```
Expected: `All tests passed` from each binary (the LIMS render smoke mounts every screen, chrome included, on Qt Quick
with no Qt warning), and ctest reports no failure across every example and suite the build has. A test that fails while another lane is
building is rerun once before it is reported.

- [ ] **Step 2: The shared build**, where every static registration reaches every test

Run: `flock "$LOCK" cmake --preset clang-shared-debug && flock "$LOCK" cmake --build build/clang-shared-debug --target morph_tests -j5 && flock "$LOCK" timeout 600 build/clang-shared-debug/tests/morph_tests "[ui]"`
Expected: `All tests passed`. A new public function without `MORPH_API` is a link error here.

- [ ] **Step 3: GCC.** CI builds with GCC 15, which warns where clang does not (`-Wrange-loop-construct`, `-Wshadow`
  on locals named like members, and `-Wstrict-overflow` at `-O3`).

Run: `flock "$LOCK" cmake --preset gcc-debug && flock "$LOCK" cmake --build build/gcc-debug --target morph_tests morph_client_tests -j5`
Expected: no warnings from the files this plan created or changed. Never pass `-w`. Confirm the warnings are live
first: add a temporary `for (std::string s : std::vector<std::string>{})` loop to `tests/test_ui_json_source.cpp`, see
GCC warn, and remove it.

- [ ] **Step 4: clang-tidy** on the changed sources, three at a time, without the lock

Run:
```bash
clang-tidy -p build/clang-debug src/ui/json_source.cpp src/ui/custom_screen_sync.cpp src/ui/custom_screen_chrome.cpp
clang-tidy -p build/clang-debug src/ui/host.cpp src/ui/workspace.cpp src/ui/navigator.cpp
clang-tidy -p build/clang-debug src/client/shell.cpp tests/test_ui_custom_screen_chrome.cpp tests/test_ui_custom_screen_sync.cpp
clang-tidy -p build/clang-debug tests/test_ui_host_drafts.cpp tests/test_ui_workspace_in_place.cpp tests/test_ui_navigator_custom.cpp
clang-tidy -p build/clang-debug tests/test_ui_json_source.cpp tests/client/test_custom_screens.cpp src/tui/field_widgets.cpp
```
Expected: no warnings. Do not accept an `auto*` or `std::expected` fix-it without writing the type out.

- [ ] **Step 5: clang-format 22** on every file this plan touched

Run: `git diff --name-only master -- '*.cpp' '*.hpp' | xargs clang-format --dry-run --Werror`
Expected: no output. Reformat with `clang-format -i` and amend nothing: commit the formatting as its own commit.

- [ ] **Step 6: Qt keywords and MSVC traps in the added lines**

Run: `git diff master -- include/morph | grep -nE '^\+.*\b(slots|signals|emit)\b' ; git diff master -- include/morph src | grep -nE '^\+.*template\s*<.*=\s*[A-Za-z_:]+<'`
Expected: no output from the first command; the second lists nothing this plan added.

- [ ] **Step 7: Report.** State, as measured: each suite's result (clang static and shared, GCC build, Qt Quick, the
  terminal frontend, the LIMS rung), the clang-tidy and clang-format results, the doc target, and which mutation
  checks ran and failed as expected. Name any test whose expectation you changed, and why. State what was not
  verified (MSVC and the WASM client are built only by CI).

---

## Self-Review

**Spec coverage.**

| Spec | Task |
|---|---|
| Ids: a `custom:` id resolves through the client's cache | 6 (`CustomDocuments`), 9 (the host's lookup) |
| The client's cache: filled by `get`, dropped when the list shows it changed or gone; `get` before every open | 6, 8 (`openCustom`, `openAs`), 9 (routes) |
| No params: the navigator's Custom section after the groups, marked, author when not mine, broken badge | 5, 8 (`navigatorEntries`), 9 |
| With params: **Open as** in the window chrome, the base and its custom screens, same params, same tab | 4 (`openInPlace`, `chrome`), 8 |
| Titles: the store's title names entries; the tab caption is the document's | 5, 8 |
| Routes and tabs: recorded, restored, a gone one reported | 6 (`prefetchRoutes`), 8, 9 |
| Drafts: `remount`, state by name, refused mount keeps the screen, identity and route of the screen | 3 |
| The bar: Save (mine), Save as…, Source, Discard; forms change real data | 7 |
| Discard; closing asks; one draft per window; the bundle changes under a draft | 3, 7, 9 |
| The source view: pretty JSON, Preview (client load, then server check), refusal with pointer → cursor, Save as… previews first, read only without a store | 1, 2, 7 |
| Errors table rows: store refusal in the bar, broken screen badge and source, draft that does not load, server refusal, a gone recorded screen, no store | 6, 7, 8, 9 |
| Testing: refusals reach the person; drafts; the proof on the LIMS sample screen (the assistant's step is plan 3's) | 7, 3, 10 |
| Specs this changes: `host.md`/`interpreter.md` (remount for drafts), `workspace.md` (custom section, Open as, routes) | 3, 4, 5, 11 |
| Discovery by probing `list`, any refusal off, warn unless `unknown model type` | 6, 9 |
| Through the interpreter's Dispatcher, as `PreferenceSync` | 6, 9 |

The assistant's `propose_screen`, `TurnRequest`'s draft and the chat's `draft` event belong to plan 3, which mounts its
drafts through `CustomScreenChrome` and `DocumentScreenHost::mountDraft`.

**Placeholder scan.** Two steps leave a decision to evidence rather than to the implementer: Task 6 Step 6 (the
dispatcher's refusal wording) and Task 10 Step 4 (a refusal of the unchanged LIMS document). Each says what to print and
that the finding is reported, not worked around.

**Type consistency.** Used with one spelling across tasks: `CustomScreenSync` (6–10), `CustomDocuments::Entry` (6, 7),
`CustomScreensOffer` (6, 7), `CustomDocumentDone` (6, 7, 8), `CustomScreenChrome::chromeFor` (7, 9),
`WorkspaceOptions::chrome` (4, 7, 9), `WorkspaceModel::openInPlace` (4, 7, 8), `DocumentScreenHost::mountDraft`,
`discardDraft`, `endDraft`, `drafted`, `screenIdOf`, `paramsOf` (3, 7, 8, 9), `NavigatorCustomEntry` (5, 8, 9),
`Mounted::placeCursor` (2, 7), `prettyJson`/`pointerOffset`/`lineAt` (1, 7, 9), `custom_client::ChromeRig`,
`SyncClient`, `BareServer`, `RefusingStore`, `savedIdOf`, `saveBody` (6, 7, 8), and the test ids listed in Tasks 7 and
8.

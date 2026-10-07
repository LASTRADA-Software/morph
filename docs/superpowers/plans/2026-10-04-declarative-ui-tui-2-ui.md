# Declarative UI, Part 2 — `morph::ui` Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or
> superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for
> tracking.

**Goal:** Ship `morph::ui` — a toolkit-agnostic, immutable view tree whose props are constants or bindings, a
`Mounted` that builds retained widgets through an `IViewBackend` once and keeps them current through reactive
bindings, the frontend seam (`AppContext`, `Application`, `Frontend`, `selectFrontend`), and
`ui::testing::RecordingBackend` plus the backend-conformance cases that fix the contract the TUI and Qt Quick
backends implement.

**Architecture:** Nodes are aggregates (`ui::Text`, `ui::Column`, …) wrapped by builder functions into
`ui::Node = std::shared_ptr<NodeData const>`. Mounting walks the tree once: each node gets a widget from a typed
factory, each bound `Prop` becomes an equality-gated `Computed` plus an `Effect` calling one widget setter, and each
constant is set once; there is no diffing. Switch, Dialog, every Tabs page and every ForEach/Table row mount into a
`reactive::Scope` of their own, adopted widget-first, so a teardown destroys bindings before widgets and children
before parents. Every widget callback runs inside `Runtime::widgetEvent`.

**Tech Stack:** C++23, header-only in the base `morph` target; Part 1's `morph::reactive`; `morph/util/datetime.hpp`
(`time::Timestamp`); Catch2 v3.

**Spec:** `docs/superpowers/specs/2026-10-04-declarative-ui-tui-design.md` §1 (goals), §5 (view IR and mount), §5b
(frontend seam), §7 (the `ui`, frontend-seam and backend-conformance tests), §9 (docs).

This is **Part 2 of 11** of the declarative-UI program. Read the master plan
`docs/superpowers/plans/2026-10-04-declarative-ui-tui.md` first: it fixes the branch, the commit layout and the
`wip(...)` convention every task below follows, and this part ends by squashing its `wip(ui)` commits into one.
The public names are fixed by `docs/superpowers/plans/2026-10-04-declarative-ui-tui-interfaces.md`, "Part 2"; Part 1
(`morph::reactive`) has landed.

## Global Constraints

- Everything in Part 1's "Global Constraints" applies unchanged: SPDX line first, `#pragma once`, clang-tidy
  naming, no history or issue numbers in comments, full Doxygen including `detail`, `-Weverything -Werror`,
  clang-tidy clean, the sign-off trailer.
- `morph::ui` depends on `morph::reactive` and `morph/util/datetime.hpp` only. No header under `include/morph/ui/`
  includes a toolkit, a terminal library, `morph/core/bridge.hpp` or anything under `morph/net`.
- Strings crossing the backend contract are UTF-8 (`std::string_view` into a setter, `std::string` out of a
  callback).
- `Key = std::variant<std::int64_t, std::string>` — never a floating-point key.
- Parameter names are at least three characters: the contract's `int id` is spelled `widgetId` in code.
- Headers index containers with `.at()`, never `operator[]`
  (`cppcoreguidelines-pro-bounds-avoid-unchecked-container-access` is on for `include/morph/**`).
- Misuse in this layer is reported through `reactive::detail::RuntimeCore::report` under the site names in
  `ui::detail::site`, then refused. A test that triggers a report installs `morph::testing::OwnerProbeRecorder`
  (a debug build asserts on a report nobody observes).
- Tests are tagged `[ui]`.

## Review Focus

1. **A row view that reads a signal directly while it builds its nodes** (instead of in a binding) must not
   subscribe the ForEach to that signal — row construction is untracked (Task 5 test "building a row reads
   nothing on the ForEach's behalf").
2. **A Switch whose selector yields a key with no case and no fallback** mounts nothing and stays usable for the
   next key (Task 4 test "a key with no case and no fallback mounts nothing").
3. **A handler that pumps the owner while its widget would be unmounted** — a Dialog's button whose click runs a
   nested event loop while a flush that closes the Dialog is pending — must not destroy the button under its own
   handler (Task 4 test "a Dialog's button survives a nested loop in its own handler").
4. **A ForEach emptied and then refilled** removes every row and leaves the container usable for the next insert
   (Task 5 test "emptied then refilled").
5. **Unmounting destroys children before parents and leaves no binding behind**: a write after the `Mounted` is
   gone posts no flush and touches no widget (Task 3 test "unmounting destroys children first and leaves no
   binding").

---

## File Structure

| File | Responsibility |
|---|---|
| `include/morph/ui/view.hpp` | `Action`, `Key`, `Prop<T>`, the enums, `Sizing`, `LayoutHints`, `Common`, every node aggregate, `NodeData`, `Node`, the builders, `switchOn`, `forEach`, `table`, `detail::ForEachModel`/`ForEachSession`/`RowSlot`/`TypedForEach` |
| `include/morph/ui/backend.hpp` | `Widget`, `ContainerWidget`, one widget interface per node kind, `IViewBackend` |
| `include/morph/ui/mount.hpp` | `detail::site::kDuplicateKey`, `detail::Mounter` and its row helpers, `Mounted` |
| `include/morph/ui/frontend.hpp` | `Scheduler`/`TimerHandle` aliases, `AppContext`, `Application`, `ApplicationFactory`, `Frontend`, `FrontendOption`, `FrontendSelectionError`, `EnvironmentReader`, `processEnvironment`, `selectFrontend` |
| `include/morph/ui/testing/recording_backend.hpp` | `testing::RecordingBackend`, its record store, fake widgets, operation log, golden dump and interaction helpers |
| `include/morph/ui/testing/backend_conformance.hpp` | `testing::ConformanceProbe`, `ConformanceCase`, `conformanceCases()` |
| `tests/test_ui_view.cpp` | `Prop`, `Sizing`, `Common`, builders |
| `tests/test_ui_recording_backend.cpp` | The recording backend on its own: log, dump, lookups, helpers, drag |
| `tests/test_ui_mount.cpp` | Mounting leaf and container kinds, `Common`, bindings, callbacks, teardown |
| `tests/test_ui_structure.cpp` | Switch, `switchOn`, Tabs, Dialog |
| `tests/test_ui_foreach.cpp` | ForEach: identity, minimal operations, keys, duplicates |
| `tests/test_ui_table.cpp` | Table: columns, row keys, selection, activation |
| `tests/test_ui_frontend.cpp` | `selectFrontend` precedence and errors, `processEnvironment` |
| `tests/test_ui_conformance.cpp` | Every conformance case against a `RecordingProbe` |
| `docs/spec/ui/view_tree.md`, `backend_contract.md`, `frontend.md` | Authoritative specs |
| `CMakeLists.txt`, `tests/CMakeLists.txt` | Header and test registration |
| `docs/spec/README.md`, `docs/ARCHITECTURE.md`, `CHANGELOG.md` | Maps and changelog |

## Build and test commands (used by every task)

```bash
cmake -S . -B build/reactive -G Ninja -DCMAKE_BUILD_TYPE=Debug -DMORPH_BUILD_EXAMPLES=OFF   # once (Part 1 made it)
cmake --build build/reactive --target morph_tests
./build/reactive/tests/morph_tests "[ui]"
```

Configuring prints `morph: warnings: ... strict=ON`; if it does not, stop and say so (CONTRIBUTING, "Warnings
are errors").

---

### Task 1: The view tree — `Prop`, `Common`, the leaf and container nodes, builders

Switch, Tabs and Dialog (Task 4), ForEach (Task 5) and Table (Task 6) join `NodeData` in the tasks that mount
them, so every task builds and every node kind lands with its mount.

**Files:**
- Create: `include/morph/ui/view.hpp`
- Modify: `CMakeLists.txt` — the `FILE_SET HEADERS` block of `target_sources(morph …)`: add
  `include/morph/ui/view.hpp` after `include/morph/reactive/testing/manual_scheduler.hpp`
- Modify: `tests/CMakeLists.txt` — the `add_executable(morph_tests …)` list: add `test_ui_view.cpp` after
  `test_reactive_scheduler.cpp`
- Test: `tests/test_ui_view.cpp`

**Interfaces:**
- Consumes: `morph::time::Timestamp` (`include/morph/util/datetime.hpp:376`).
- Produces (later tasks and parts use these exact names):
  - `ui::Action = std::function<void()>`; `ui::Key = std::variant<std::int64_t, std::string>`.
  - `ui::Prop<T>`: implicit from a non-callable value convertible to `T` (constant) or a callable returning one
    (binding); `isBound()`, `constant()`, `binding()`, `evaluate()`.
  - Enums `TextRole{Normal, Muted, Heading, Error, Success}`, `TextInputMode{SingleLine, Multiline, Password}`,
    `SelectStyle{Dropdown, Radio}`, `Axis{Vertical, Horizontal}`, `DateMode{Date, DateTime}`,
    `FilePickerMode{Open, Save}`.
  - `Sizing{kind, amount}` with `content()`, `fixed(int)`, `stretch(int = 1)`; `LayoutHints{width, height}`;
    `Common{visible, enabled, layout, dragKey, accepts, onDrop}`.
  - Aggregates `Text`, `Button`, `TextInput`, `Checkbox`, `SelectOption`, `Select`, `MenuItem`, `Menu`, `Column`,
    `Row`, `GridCell`, `Grid`, `Spacer`, `Panel`, `Scroll`, `Busy`, `DateTimeInput`, `Slider`, `FilePicker`, fields
    exactly as the contract lists them.
  - `NodeData{kind}`, `Node`, `detail::makeNode<Kind>(Kind)`, and the builders `text`, `button`, `textInput`,
    `checkbox`, `select`, `menu`, `column`, `row`, `grid`, `spacer`, `panel`, `scroll`, `busy`, `dateTimeInput`,
    `slider`, `filePicker`.

- [ ] **Step 1: Write the failing test**

Create `tests/test_ui_view.cpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0

#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstdint>
#include <memory>
#include <morph/ui/view.hpp>
#include <morph/util/datetime.hpp>
#include <optional>
#include <string>
#include <type_traits>
#include <variant>
#include <vector>

namespace ui = morph::ui;

static_assert(std::is_same_v<ui::Node, std::shared_ptr<ui::NodeData const>>);
static_assert(std::is_same_v<ui::Key, std::variant<std::int64_t, std::string>>);

TEST_CASE("ui::Prop: a constant is not bound, a callable is", "[ui]") {
    ui::Prop<std::string> const fixed = "hello";
    CHECK_FALSE(fixed.isBound());
    CHECK(fixed.constant() == "hello");
    CHECK(fixed.evaluate() == "hello");

    int calls = 0;
    ui::Prop<std::string> const bound = [&calls] {
        ++calls;
        return std::string{"bound"};
    };
    CHECK(bound.isBound());
    CHECK(bound.evaluate() == "bound");
    CHECK(bound.binding()() == "bound");
    CHECK(calls == 2);
}

TEST_CASE("ui::Prop<bool>: a captureless lambda is a binding, not a truthy constant", "[ui]") {
    ui::Prop<bool> const bound = [] { return false; };
    CHECK(bound.isBound());
    CHECK_FALSE(bound.evaluate());
}

TEST_CASE("ui::Prop: a default-constructed prop is the value-initialised constant", "[ui]") {
    ui::Prop<std::string> const text;
    ui::Prop<std::optional<ui::Key>> const key;
    CHECK_FALSE(text.isBound());
    CHECK(text.constant().empty());
    CHECK_FALSE(key.constant().has_value());
}

TEST_CASE("ui::Sizing and ui::LayoutHints: the three rules and their equality", "[ui]") {
    CHECK(ui::Sizing::content() == ui::Sizing{});
    CHECK(ui::Sizing::fixed(3) == ui::Sizing{.kind = ui::Sizing::Kind::Fixed, .amount = 3});
    CHECK(ui::Sizing::stretch() == ui::Sizing{.kind = ui::Sizing::Kind::Stretch, .amount = 1});
    CHECK(ui::LayoutHints{} == ui::LayoutHints{.width = ui::Sizing::content(), .height = ui::Sizing::content()});
    CHECK_FALSE(ui::LayoutHints{.width = ui::Sizing::fixed(2)} == ui::LayoutHints{});
}

TEST_CASE("ui::Common: shown, enabled, content-sized, not draggable, not a drop target", "[ui]") {
    ui::Common const common;
    CHECK(common.visible.constant());
    CHECK(common.enabled.constant());
    CHECK(common.layout == ui::LayoutHints{});
    CHECK_FALSE(common.dragKey.constant().has_value());
    CHECK_FALSE(common.accepts);
    CHECK_FALSE(common.onDrop);
}

TEST_CASE("ui builders: nodes are shared, immutable data", "[ui]") {
    ui::Node const tree = ui::column({
        .children =
            {
                ui::text({.text = "Title", .role = ui::TextRole::Heading}),
                ui::button({.label = "Go", .onClick = [] {}}),
                ui::spacer(),
            },
        .gap = 1,
    });
    REQUIRE(tree != nullptr);
    auto const* column = std::get_if<ui::Column>(&tree->kind);
    REQUIRE(column != nullptr);
    REQUIRE(column->children.size() == 3);
    CHECK(column->gap == 1);
    auto const* title = std::get_if<ui::Text>(&column->children.at(0)->kind);
    REQUIRE(title != nullptr);
    CHECK(title->text.constant() == "Title");
    CHECK(title->role.constant() == ui::TextRole::Heading);
    CHECK(std::holds_alternative<ui::Spacer>(column->children.at(2)->kind));
}

TEST_CASE("ui builders: the input kinds carry their modes and ranges", "[ui]") {
    using morph::time::DateTime;
    using morph::time::Timestamp;
    Timestamp const when{DateTime{std::chrono::year{2026}, std::chrono::month{10}, std::chrono::day{4},
                                  std::chrono::hours{9}, std::chrono::minutes{30}, std::chrono::seconds{0}}};
    ui::Node const date = ui::dateTimeInput(
        {.value = std::optional<Timestamp>{when}, .mode = ui::DateMode::Date, .offsetMinutes = 120});
    ui::Node const slider = ui::slider({.value = 5, .minimum = 0, .maximum = 10, .step = 5});
    ui::Node const picker = ui::filePicker({.path = "/tmp/out.csv", .mode = ui::FilePickerMode::Save});
    ui::Node const panel = ui::panel({.title = "Advanced", .collapsible = true, .collapsed = true});

    auto const& dateSpec = std::get<ui::DateTimeInput>(date->kind);
    CHECK(dateSpec.value.constant() == std::optional<Timestamp>{when});
    CHECK(dateSpec.offsetMinutes == 120);
    CHECK(std::get<ui::Slider>(slider->kind).step == 5);
    CHECK(std::get<ui::FilePicker>(picker->kind).mode == ui::FilePickerMode::Save);
    CHECK(std::get<ui::Panel>(panel->kind).collapsed.constant());
}
```

Register it: in `tests/CMakeLists.txt`, add `test_ui_view.cpp` to the `add_executable(morph_tests …)` list directly
after `test_reactive_scheduler.cpp`.

- [ ] **Step 2: Run the test to verify it fails**

Run: `cmake --build build/reactive --target morph_tests`
Expected: FAIL — `fatal error: 'morph/ui/view.hpp' file not found`.

- [ ] **Step 3: Implement the view tree**

Create `include/morph/ui/view.hpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0

#pragma once
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include "../util/datetime.hpp"

/// @file
/// @brief The toolkit-agnostic view tree: props, node kinds and their builders.
///
/// Specified in `docs/spec/ui/view_tree.md`.

namespace morph::ui {

/// @brief A widget event handler taking no arguments.
using Action = std::function<void()>;

/// @brief The identity of a Switch case, a Select option, a ForEach or Table row and a drag payload.
///
/// Never a floating-point number: morph ids can exceed 2^53.
using Key = std::variant<std::int64_t, std::string>;

/// @brief A node property: a constant, or a binding the mount re-evaluates reactively.
///
/// A constant is set on the widget once and creates no reactive node. A binding becomes an equality-gated
/// `reactive::Computed` plus an `reactive::Effect` calling one widget setter.
/// @tparam T The property's value type.
template <typename T>
class Prop {
public:
    /// @brief The value-initialised constant.
    Prop()
        requires std::default_initializable<T>
        : _value{std::in_place_index<0>} {}

    /// @brief A constant.
    ///
    /// A callable is never a constant, even when it converts to `T`: a captureless lambda converts to a function
    /// pointer and that to `bool`, so `Prop<bool>{[] { return false; }}` would otherwise be the constant `true`.
    /// @tparam U A non-callable type convertible to `T`.
    /// @param value The constant.
    template <typename U>
        requires(!std::invocable<U&>) && std::convertible_to<U, T>
    Prop(U&& value)  // NOLINT(bugprone-forwarding-reference-overload)
        : _value{std::in_place_index<0>, static_cast<T>(std::forward<U>(value))} {}

    /// @brief A binding.
    /// @tparam F A callable taking no arguments and returning something convertible to `T`.
    /// @param binding Evaluated by the mount inside a `Computed`; every signal it reads is a dependency.
    template <typename F>
        requires std::invocable<F&> && std::convertible_to<std::invoke_result_t<F&>, T>
    Prop(F binding) : _value{std::in_place_index<1>, std::function<T()>{std::move(binding)}} {}

    /// @brief Whether this is a binding.
    /// @return True for a binding, false for a constant.
    [[nodiscard]] bool isBound() const noexcept { return _value.index() == 1; }

    /// @brief The constant. Only valid when `!isBound()`.
    /// @return The constant.
    /// @throws std::bad_variant_access when this is a binding.
    [[nodiscard]] T const& constant() const { return std::get<0>(_value); }

    /// @brief The binding. Only valid when `isBound()`.
    /// @return The binding.
    /// @throws std::bad_variant_access when this is a constant.
    [[nodiscard]] std::function<T()> const& binding() const { return std::get<1>(_value); }

    /// @brief The current value: the constant, or the binding called once.
    /// @return The value.
    [[nodiscard]] T evaluate() const { return isBound() ? std::get<1>(_value)() : std::get<0>(_value); }

private:
    std::variant<T, std::function<T()>> _value;
};

/// @brief How a `Text` is styled; each backend maps a role to its theme.
enum class TextRole : std::uint8_t {
    Normal,   ///< Body text.
    Muted,    ///< Secondary text.
    Heading,  ///< A section heading.
    Error,    ///< An error message.
    Success,  ///< A confirmation.
};

/// @brief How a `TextInput` edits.
enum class TextInputMode : std::uint8_t {
    SingleLine,  ///< One line; Enter submits.
    Multiline,   ///< Several lines.
    Password,    ///< One line, masked.
};

/// @brief How a `Select` presents its options.
enum class SelectStyle : std::uint8_t {
    Dropdown,  ///< A closed field that opens a list.
    Radio,     ///< Every option visible, one marked.
};

/// @brief The direction a stack arranges, or a scroll area scrolls, its content.
enum class Axis : std::uint8_t {
    Vertical,    ///< Top to bottom.
    Horizontal,  ///< Left to right.
};

/// @brief What a `DateTimeInput` edits.
enum class DateMode : std::uint8_t {
    Date,      ///< A calendar date; the time of day is midnight in the display zone.
    DateTime,  ///< A date and a time of day.
};

/// @brief Whether a `FilePicker` chooses an existing file or a file to write.
enum class FilePickerMode : std::uint8_t {
    Open,  ///< An existing file.
    Save,  ///< A file to create or overwrite.
};

/// @brief How much space a widget asks for along one dimension, in backend units (a character cell on the TUI).
struct Sizing {
    /// @brief The sizing rule.
    enum class Kind : std::uint8_t {
        Content,  ///< As much as the content needs.
        Fixed,    ///< Exactly `amount` units.
        Stretch,  ///< A share of the leftover space, weighted by `amount`.
    };
    /// @brief The rule.
    Kind kind = Kind::Content;
    /// @brief Units for `Fixed`, weight for `Stretch`, unused for `Content`.
    int amount = 0;

    /// @brief Size to the content.
    /// @return The sizing.
    [[nodiscard]] static constexpr Sizing content() noexcept { return {}; }

    /// @brief A fixed size.
    /// @param units The size in backend units.
    /// @return The sizing.
    [[nodiscard]] static constexpr Sizing fixed(int units) noexcept { return {.kind = Kind::Fixed, .amount = units}; }

    /// @brief A share of the leftover space.
    /// @param weight The share's weight.
    /// @return The sizing.
    [[nodiscard]] static constexpr Sizing stretch(int weight = 1) noexcept {
        return {.kind = Kind::Stretch, .amount = weight};
    }

    /// @brief Memberwise equality.
    /// @return Whether both rules and amounts match.
    bool operator==(Sizing const&) const = default;
};

/// @brief A widget's size request in both dimensions.
struct LayoutHints {
    /// @brief Along the horizontal axis.
    Sizing width{};
    /// @brief Along the vertical axis.
    Sizing height{};

    /// @brief Memberwise equality.
    /// @return Whether both dimensions match.
    bool operator==(LayoutHints const&) const = default;
};

/// @brief The part every node carries: visibility, enablement, layout, and drag-and-drop.
struct Common {
    /// @brief Whether the widget is shown.
    Prop<bool> visible = true;
    /// @brief Whether the widget accepts input.
    Prop<bool> enabled = true;
    /// @brief The size request; set once.
    LayoutHints layout{};
    /// @brief Engaged: the widget can be dragged, and a drop delivers this key.
    Prop<std::optional<Key>> dragKey;
    /// @brief Whether a drop of a key is accepted; empty accepts every key. Used only when `onDrop` is set.
    std::function<bool(Key const&)> accepts;
    /// @brief Set: the widget is a drop target, called with the dropped key.
    std::function<void(Key)> onDrop;
};

struct NodeData;

/// @brief A view-tree node: immutable, shared data.
using Node = std::shared_ptr<NodeData const>;

/// @brief Static or bound text.
struct Text {
    /// @brief The text, UTF-8.
    Prop<std::string> text;
    /// @brief The style.
    Prop<TextRole> role = TextRole::Normal;
    /// @brief Visibility, enablement, layout, drag-and-drop.
    Common common{};
};

/// @brief A push button.
struct Button {
    /// @brief The caption.
    Prop<std::string> label;
    /// @brief Called on activation.
    Action onClick;
    /// @brief Visibility, enablement, layout, drag-and-drop.
    Common common{};
};

/// @brief An editable text field. The backend never reports its own `setText` back through `onChange`.
struct TextInput {
    /// @brief The text the field shows.
    Prop<std::string> value;
    /// @brief Called with the new text after every user edit.
    std::function<void(std::string)> onChange;
    /// @brief Called with the text when the user submits (Enter in single-line mode).
    std::function<void(std::string)> onSubmit;
    /// @brief Shown while the field is empty.
    Prop<std::string> placeholder;
    /// @brief Single-line, multiline or password; set once.
    TextInputMode mode = TextInputMode::SingleLine;
    /// @brief Visibility, enablement, layout, drag-and-drop.
    Common common{};
};

/// @brief A labelled check box.
struct Checkbox {
    /// @brief The caption.
    Prop<std::string> label;
    /// @brief Whether it is checked.
    Prop<bool> checked;
    /// @brief Called with the new state when the user toggles it.
    std::function<void(bool)> onToggle;
    /// @brief Visibility, enablement, layout, drag-and-drop.
    Common common{};
};

/// @brief One option of a `Select`.
struct SelectOption {
    /// @brief The option's identity.
    Key key;
    /// @brief The caption.
    std::string label;

    /// @brief Memberwise equality.
    /// @return Whether key and label match.
    bool operator==(SelectOption const&) const = default;
};

/// @brief A choice of one option.
struct Select {
    /// @brief The options, in order.
    Prop<std::vector<SelectOption>> options;
    /// @brief The selected key, or none. A key outside `options` shows no selection.
    Prop<std::optional<Key>> selected;
    /// @brief Called with the key the user chose.
    std::function<void(Key)> onSelect;
    /// @brief Dropdown or radio; set once.
    SelectStyle style = SelectStyle::Dropdown;
    /// @brief Visibility, enablement, layout, drag-and-drop.
    Common common{};
};

/// @brief One entry of a `Menu`: its label and its handler in one place, so there are no parallel lists.
struct MenuItem {
    /// @brief The caption.
    Prop<std::string> label;
    /// @brief Called when the entry is chosen.
    Action onSelect;
};

/// @brief A vertical list of commands.
struct Menu {
    /// @brief The entries, in order; the list itself is set once, each label may be bound.
    std::vector<MenuItem> items;
    /// @brief Visibility, enablement, layout, drag-and-drop.
    Common common{};
};

/// @brief Children stacked top to bottom.
struct Column {
    /// @brief The children, in order; a null child is skipped.
    std::vector<Node> children;
    /// @brief Space between children, in backend units.
    int gap = 0;
    /// @brief Visibility, enablement, layout, drag-and-drop.
    Common common{};
};

/// @brief Children laid out left to right.
struct Row {
    /// @brief The children, in order; a null child is skipped.
    std::vector<Node> children;
    /// @brief Space between children, in backend units.
    int gap = 0;
    /// @brief Visibility, enablement, layout, drag-and-drop.
    Common common{};
};

/// @brief One cell of a `Grid`.
struct GridCell {
    /// @brief The cell's content; a null node leaves no cell.
    Node node;
    /// @brief How many columns it spans.
    int span = 1;
};

/// @brief Cells laid out row-major in a fixed number of columns.
struct Grid {
    /// @brief The column count.
    int columns = 1;
    /// @brief The cells, row-major.
    std::vector<GridCell> cells;
    /// @brief Space between cells, in backend units.
    int gap = 0;
    /// @brief Visibility, enablement, layout, drag-and-drop.
    Common common{};
};

/// @brief Empty space that takes what its layout hints ask for.
struct Spacer {
    /// @brief Visibility, enablement, layout, drag-and-drop.
    Common common{};
};

/// @brief A titled frame around one child; a collapsible one is an accordion section.
struct Panel {
    /// @brief The title.
    Prop<std::string> title;
    /// @brief Space between frame and child, in backend units.
    int padding = 0;
    /// @brief The content; may be null.
    Node child;
    /// @brief Whether the user can collapse it; set once.
    bool collapsible = false;
    /// @brief Whether it is collapsed; used only when `collapsible`.
    Prop<bool> collapsed = false;
    /// @brief Called with the collapsed state the user asked for; used only when `collapsible`.
    std::function<void(bool)> onToggle;
    /// @brief Visibility, enablement, layout, drag-and-drop.
    Common common{};
};

/// @brief A scrollable viewport around one child.
struct Scroll {
    /// @brief The content; may be null.
    Node child;
    /// @brief The scroll direction; set once.
    Axis axis = Axis::Vertical;
    /// @brief Visibility, enablement, layout, drag-and-drop.
    Common common{};
};

/// @brief A progress indicator.
struct Busy {
    /// @brief Whether it animates.
    Prop<bool> active;
    /// @brief A caption beside it.
    Prop<std::string> label;
    /// @brief Visibility, enablement, layout, drag-and-drop.
    Common common{};
};

/// @brief A date, or a date and time, edited in a display zone and reported as an instant.
struct DateTimeInput {
    /// @brief The instant shown; `nullopt` (or an empty `Timestamp`) shows an empty field.
    Prop<std::optional<morph::time::Timestamp>> value;
    /// @brief Called with the instant the user entered, or `nullopt` when they cleared the field.
    std::function<void(std::optional<morph::time::Timestamp>)> onChange;
    /// @brief Date or date and time; set once.
    DateMode mode = DateMode::DateTime;
    /// @brief The display zone's offset from UTC, in minutes; set once.
    int offsetMinutes = 0;
    /// @brief Visibility, enablement, layout, drag-and-drop.
    Common common{};
};

/// @brief An integer chosen on a range.
struct Slider {
    /// @brief The value shown.
    Prop<std::int64_t> value;
    /// @brief The smallest value; set once.
    std::int64_t minimum = 0;
    /// @brief The largest value; set once.
    std::int64_t maximum = 100;
    /// @brief The increment between neighbouring values; set once.
    std::int64_t step = 1;
    /// @brief Called with the value the user moved to.
    std::function<void(std::int64_t)> onChange;
    /// @brief Visibility, enablement, layout, drag-and-drop.
    Common common{};
};

/// @brief A file path, typed or chosen in the platform's file dialog.
struct FilePicker {
    /// @brief The path shown.
    Prop<std::string> path;
    /// @brief Open an existing file or name one to save; set once.
    FilePickerMode mode = FilePickerMode::Open;
    /// @brief Called with the path the user picked.
    std::function<void(std::string)> onPicked;
    /// @brief Visibility, enablement, layout, drag-and-drop.
    Common common{};
};

/// @brief What a `Node` points at: exactly one node kind.
struct NodeData {
    /// @brief The node.
    std::variant<Text, Button, TextInput, Checkbox, Select, Menu, Column, Row, Grid, Spacer, Panel, Scroll, Busy,
                 DateTimeInput, Slider, FilePicker>
        kind;
};

namespace detail {

/// @brief Wraps one node aggregate into a shared node.
/// @tparam Kind The node aggregate type.
/// @param spec The node.
/// @return The shared node.
template <typename Kind>
[[nodiscard]] Node makeNode(Kind spec) {
    return std::make_shared<NodeData const>(NodeData{.kind = std::move(spec)});
}

}  // namespace detail

/// @brief Static or bound text.
/// @param spec The node.
/// @return The node.
[[nodiscard]] inline Node text(Text spec) { return detail::makeNode(std::move(spec)); }

/// @brief A push button.
/// @param spec The node.
/// @return The node.
[[nodiscard]] inline Node button(Button spec) { return detail::makeNode(std::move(spec)); }

/// @brief An editable text field.
/// @param spec The node.
/// @return The node.
[[nodiscard]] inline Node textInput(TextInput spec) { return detail::makeNode(std::move(spec)); }

/// @brief A labelled check box.
/// @param spec The node.
/// @return The node.
[[nodiscard]] inline Node checkbox(Checkbox spec) { return detail::makeNode(std::move(spec)); }

/// @brief A choice of one option.
/// @param spec The node.
/// @return The node.
[[nodiscard]] inline Node select(Select spec) { return detail::makeNode(std::move(spec)); }

/// @brief A list of commands.
/// @param spec The node.
/// @return The node.
[[nodiscard]] inline Node menu(Menu spec) { return detail::makeNode(std::move(spec)); }

/// @brief Children stacked top to bottom.
/// @param spec The node.
/// @return The node.
[[nodiscard]] inline Node column(Column spec) { return detail::makeNode(std::move(spec)); }

/// @brief Children laid out left to right.
/// @param spec The node.
/// @return The node.
[[nodiscard]] inline Node row(Row spec) { return detail::makeNode(std::move(spec)); }

/// @brief Cells in a fixed number of columns.
/// @param spec The node.
/// @return The node.
[[nodiscard]] inline Node grid(Grid spec) { return detail::makeNode(std::move(spec)); }

/// @brief Empty space.
/// @param spec The node.
/// @return The node.
[[nodiscard]] inline Node spacer(Spacer spec = {}) { return detail::makeNode(std::move(spec)); }

/// @brief A titled frame, optionally collapsible.
/// @param spec The node.
/// @return The node.
[[nodiscard]] inline Node panel(Panel spec) { return detail::makeNode(std::move(spec)); }

/// @brief A scrollable viewport.
/// @param spec The node.
/// @return The node.
[[nodiscard]] inline Node scroll(Scroll spec) { return detail::makeNode(std::move(spec)); }

/// @brief A progress indicator.
/// @param spec The node.
/// @return The node.
[[nodiscard]] inline Node busy(Busy spec) { return detail::makeNode(std::move(spec)); }

/// @brief A date or date-and-time field.
/// @param spec The node.
/// @return The node.
[[nodiscard]] inline Node dateTimeInput(DateTimeInput spec) { return detail::makeNode(std::move(spec)); }

/// @brief An integer on a range.
/// @param spec The node.
/// @return The node.
[[nodiscard]] inline Node slider(Slider spec) { return detail::makeNode(std::move(spec)); }

/// @brief A file path field.
/// @param spec The node.
/// @return The node.
[[nodiscard]] inline Node filePicker(FilePicker spec) { return detail::makeNode(std::move(spec)); }

}  // namespace morph::ui
```

Register the header: in `CMakeLists.txt`, in the `FILE_SET HEADERS` block of `target_sources(morph …)`, add after
`include/morph/reactive/testing/manual_scheduler.hpp`:

```cmake
        include/morph/ui/view.hpp
```

- [ ] **Step 4: Run the tests to verify they pass**

Run: `cmake --build build/reactive --target morph_tests && ./build/reactive/tests/morph_tests "[ui]"`
Expected: PASS, `All tests passed (… assertions in 7 test cases)`.

Mutation check: in `Prop`'s constant constructor, delete `(!std::invocable<U&>) &&` from the constraint and rebuild.
Expected: FAIL — "a captureless lambda is a binding" no longer compiles (`Prop<bool>` from a captureless lambda is
ambiguous between the constant and the binding constructor). Restore.

- [ ] **Step 5: Commit**

```bash
git add include/morph/ui/view.hpp tests/test_ui_view.cpp tests/CMakeLists.txt CMakeLists.txt
git commit -m "wip(ui): the view tree -- Prop, Common, leaf and container nodes, builders

Signed-off-by: Christian Parpart <christian@parpart.family>"
```

---

### Task 2: The backend contract and `RecordingBackend`

The widget interfaces and the headless backend land together: the interfaces are only testable through an
implementation, and `RecordingBackend` is the reference implementation every later task asserts through.

**Files:**
- Create: `include/morph/ui/backend.hpp`
- Create: `include/morph/ui/testing/recording_backend.hpp`
- Modify: `CMakeLists.txt` — `FILE_SET HEADERS`, after `include/morph/ui/view.hpp`: `include/morph/ui/backend.hpp`
  and `include/morph/ui/testing/recording_backend.hpp`
- Modify: `tests/CMakeLists.txt` — add `test_ui_recording_backend.cpp` after `test_ui_view.cpp`
- Test: `tests/test_ui_recording_backend.cpp`

**Interfaces:**
- Consumes: Task 1's `Key`, `Action`, `LayoutHints`, `Sizing`, `SelectOption`, the enums; `time::Timestamp`,
  `time::DateTime::toIso8601()` (`include/morph/util/datetime.hpp`).
- Produces:
  - `ui::Widget` (`setVisible`, `setEnabled`, `setLayout`, `setDragKey`, `setDropHandler`),
    `ui::ContainerWidget::moveChild`, and `TextWidget`, `ButtonWidget`, `TextInputWidget`, `CheckboxWidget`,
    `SelectWidget`, `MenuWidget`, `StackWidget`, `GridWidget`, `SpacerWidget`, `PanelWidget`, `ScrollWidget`,
    `BusyWidget`, `DateTimeInputWidget`, `SliderWidget`, `FilePickerWidget` exactly as the contract declares them.
  - `ui::IViewBackend` with the factories for those kinds (Task 4 adds `createSlot`, `createTabs`, `createDialog`;
    Task 6 adds `createTable`).
  - `ui::testing::RecordingBackend`: the contract's `dump`, `log`, `clearLog`, `find`, `all`, `prop`, `exists`,
    `click`, `edit`, `submit`, `toggle`, `choose`, `chooseIndex`, `collapse`, `setDateTime`, `slide`, `pick`,
    `drag` (Task 4 adds `dismiss`, Task 6 `selectRows` and `activateRow`), **plus four additions** the
    conformance probe and the mount tests need — `idOf(Widget const&) -> int`, `kindOf(int) -> std::string`,
    `children(int) -> std::vector<int>`, `widget(int) -> Widget*`.
  - `ui::testing::detail::{PropList, Callbacks, Record, RecordStore, FakeLeaf<I>, FakeContainer<I>, Fake*}` and the
    formatters `formatBool`, `formatKey`, `formatOptionalKey`, `formatSizing`, `formatLayout`, `formatTimestamp`,
    `formatList`, `enumName(…)`.
  - Log lines, exactly: `create Kind#id in Parent#id` (`in root` for a root), `set Kind#id name=value`,
    `move Kind#id to n`, `destroy Kind#id`. Dump: one line per widget, `Kind#id name=value …` with the
    properties in name order, children indented two spaces per depth, every line ending in `\n`. A stack's kind
    is `Column` or `Row` by its axis.

- [ ] **Step 1: Write the failing test**

Create `tests/test_ui_recording_backend.cpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0

#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <morph/ui/backend.hpp>
#include <morph/ui/testing/recording_backend.hpp>
#include <morph/ui/view.hpp>
#include <morph/util/datetime.hpp>
#include <optional>
#include <stdexcept>
#include <string>
#include <variant>
#include <vector>

namespace ui = morph::ui;

namespace {

using ui::testing::RecordingBackend;

ui::Key intKey(std::int64_t value) { return ui::Key{value}; }

}  // namespace

TEST_CASE("RecordingBackend: logs creation, setters, moves and destruction", "[ui]") {
    RecordingBackend backend;
    auto column = backend.createStack(nullptr, ui::Axis::Vertical);
    auto first = backend.createText(column.get());
    auto second = backend.createButton(column.get());
    first->setText("hello");
    second->setLabel("Go");
    column->moveChild(*second, 0);
    CHECK(backend.log() == std::vector<std::string>{"create Column#1 in root", "create Text#2 in Column#1",
                                                    "create Button#3 in Column#1", "set Text#2 text=hello",
                                                    "set Button#3 label=Go", "move Button#3 to 0"});
    CHECK(backend.dump() == "Column#1\n  Button#3 label=Go\n  Text#2 text=hello\n");

    backend.clearLog();
    first.reset();
    CHECK(backend.log() == std::vector<std::string>{"destroy Text#2"});
    CHECK_FALSE(backend.exists(2));
    CHECK(backend.children(1) == std::vector<int>{3});
}

TEST_CASE("RecordingBackend: creation parameters are properties, a horizontal stack is a Row", "[ui]") {
    RecordingBackend backend;
    auto row = backend.createStack(nullptr, ui::Axis::Horizontal);
    auto input = backend.createTextInput(row.get(), ui::TextInputMode::Password);
    auto date = backend.createDateTimeInput(row.get(), ui::DateMode::Date, -90);
    auto scroll = backend.createScroll(row.get(), ui::Axis::Horizontal);
    auto picker = backend.createFilePicker(row.get(), ui::FilePickerMode::Save);
    auto choice = backend.createSelect(row.get(), ui::SelectStyle::Radio);
    choice->setOptions({{.key = intKey(1), .label = "One"}, {.key = ui::Key{std::string{"b"}}, .label = "Bee"}});
    choice->setSelected(ui::Key{std::string{"b"}});
    CHECK(backend.dump() == "Row#1\n"
                            "  TextInput#2 mode=Password\n"
                            "  DateTimeInput#3 mode=Date offset=-90\n"
                            "  Scroll#4 axis=Horizontal\n"
                            "  FilePicker#5 mode=Save\n"
                            "  Select#6 options=[1:One,\"b\":Bee] selected=\"b\" style=Radio\n");
}

TEST_CASE("RecordingBackend: Common setters are properties", "[ui]") {
    RecordingBackend backend;
    auto label = backend.createText(nullptr);
    label->setVisible(false);
    label->setEnabled(false);
    label->setLayout({.width = ui::Sizing::stretch(2), .height = ui::Sizing::fixed(1)});
    label->setDragKey(intKey(7));
    label->setDropHandler([](ui::Key const&) { return true; }, [](ui::Key) {});
    CHECK(backend.dump() == "Text#1 dragKey=7 drop=handler enabled=false layout=stretch(2)/fixed(1) visible=false\n");
    label->setDragKey(std::nullopt);
    CHECK(backend.prop(1, "dragKey") == "none");
}

TEST_CASE("RecordingBackend: lookups by kind, property, id and widget", "[ui]") {
    RecordingBackend backend;
    auto column = backend.createStack(nullptr, ui::Axis::Vertical);
    auto save = backend.createButton(column.get());
    auto quit = backend.createButton(column.get());
    save->setLabel("Save");
    quit->setLabel("Quit");
    CHECK(backend.find("Button", "label", "Quit") == std::optional<int>{3});
    CHECK_FALSE(backend.find("Button", "label", "Open").has_value());
    CHECK(backend.all("Button") == std::vector<int>{2, 3});
    CHECK(backend.prop(2, "label") == "Save");
    CHECK(backend.prop(2, "unset").empty());
    CHECK(backend.idOf(*quit) == 3);
    CHECK(backend.kindOf(1) == "Column");
    CHECK(backend.widget(3) == quit.get());
    CHECK_THROWS_AS(backend.prop(99, "label"), std::out_of_range);
    RecordingBackend other;
    auto stranger = other.createText(nullptr);
    CHECK_THROWS_AS(backend.idOf(*stranger), std::out_of_range);
}

TEST_CASE("RecordingBackend: a child outlives a destroyed parent as a root", "[ui]") {
    RecordingBackend backend;
    auto column = backend.createStack(nullptr, ui::Axis::Vertical);
    auto label = backend.createText(column.get());
    column.reset();
    CHECK(backend.dump() == "Text#2\n");
}

TEST_CASE("RecordingBackend: the interaction helpers call what the widget was given", "[ui]") {
    RecordingBackend backend;
    int clicks = 0;
    std::vector<std::string> edits;
    std::vector<std::string> submits;
    std::vector<bool> toggles;
    std::vector<ui::Key> chosen;
    std::vector<std::size_t> activated;
    std::vector<bool> collapses;
    std::vector<std::optional<morph::time::Timestamp>> dates;
    std::vector<std::int64_t> slides;
    std::vector<std::string> picks;

    auto button = backend.createButton(nullptr);
    button->setOnClick([&] { ++clicks; });
    auto input = backend.createTextInput(nullptr, ui::TextInputMode::SingleLine);
    input->setOnChange([&](std::string text) { edits.push_back(std::move(text)); });
    input->setOnSubmit([&](std::string text) { submits.push_back(std::move(text)); });
    auto box = backend.createCheckbox(nullptr);
    box->setOnToggle([&](bool checked) { toggles.push_back(checked); });
    auto choice = backend.createSelect(nullptr, ui::SelectStyle::Dropdown);
    choice->setOnSelect([&](ui::Key key) { chosen.push_back(std::move(key)); });
    auto menu = backend.createMenu(nullptr);
    menu->setOnActivate([&](std::size_t index) { activated.push_back(index); });
    auto panel = backend.createPanel(nullptr);
    panel->setOnToggle([&](bool collapsed) { collapses.push_back(collapsed); });
    auto date = backend.createDateTimeInput(nullptr, ui::DateMode::DateTime, 0);
    date->setOnChange([&](std::optional<morph::time::Timestamp> value) { dates.push_back(value); });
    auto slider = backend.createSlider(nullptr);
    slider->setOnChange([&](std::int64_t value) { slides.push_back(value); });
    auto picker = backend.createFilePicker(nullptr, ui::FilePickerMode::Open);
    picker->setOnPicked([&](std::string path) { picks.push_back(std::move(path)); });

    backend.click(1);
    backend.edit(2, "ab");
    backend.submit(2, "abc");
    backend.toggle(3);
    backend.toggle(3);
    backend.choose(4, intKey(5));
    backend.chooseIndex(5, 1);
    backend.collapse(6, true);
    morph::time::Timestamp const when{morph::time::DateTime{std::chrono::year{2026}, std::chrono::month{10},
                                                             std::chrono::day{4}, std::chrono::hours{9},
                                                             std::chrono::minutes{30}, std::chrono::seconds{0}}};
    backend.setDateTime(7, when);
    backend.slide(8, 40);
    backend.pick(9, "/tmp/in.csv");

    CHECK(clicks == 1);
    CHECK(edits == std::vector<std::string>{"ab"});
    CHECK(submits == std::vector<std::string>{"abc"});
    CHECK(toggles == std::vector<bool>{true, false});
    CHECK(chosen == std::vector<ui::Key>{intKey(5)});
    CHECK(activated == std::vector<std::size_t>{1});
    CHECK(collapses == std::vector<bool>{true});
    CHECK(dates == std::vector<std::optional<morph::time::Timestamp>>{when});
    CHECK(slides == std::vector<std::int64_t>{40});
    CHECK(picks == std::vector<std::string>{"/tmp/in.csv"});
    // What the user changed shows on the widget, without a setter call in the log.
    CHECK(backend.prop(2, "text") == "abc");
    CHECK(backend.prop(3, "checked") == "false");
    CHECK(backend.prop(4, "selected") == "5");
    CHECK(backend.prop(7, "value") == "2026-10-04T09:30:00.000Z");
    CHECK(backend.log().size() == 9);
}

TEST_CASE("RecordingBackend: setText never calls onChange", "[ui]") {
    RecordingBackend backend;
    auto input = backend.createTextInput(nullptr, ui::TextInputMode::SingleLine);
    int changes = 0;
    input->setOnChange([&](std::string const&) { ++changes; });
    input->setText("programmatic");
    CHECK(changes == 0);
    CHECK(backend.prop(1, "text") == "programmatic");
}

TEST_CASE("RecordingBackend: a hidden or disabled widget ignores the helpers", "[ui]") {
    RecordingBackend backend;
    int clicks = 0;
    auto hidden = backend.createButton(nullptr);
    hidden->setOnClick([&] { ++clicks; });
    hidden->setVisible(false);
    auto disabled = backend.createButton(nullptr);
    disabled->setOnClick([&] { ++clicks; });
    disabled->setEnabled(false);
    backend.click(1);
    backend.click(2);
    CHECK(clicks == 0);
    disabled->setEnabled(true);
    backend.click(2);
    CHECK(clicks == 1);
}

TEST_CASE("RecordingBackend: a helper survives a handler that destroys its widget", "[ui]") {
    RecordingBackend backend;
    std::unique_ptr<ui::ButtonWidget> button = backend.createButton(nullptr);
    std::string seen;
    // The capture is larger than std::function's small buffer, so it lives in the stored handler's own allocation.
    button->setOnClick([&button, &seen, marker = std::string{"the handler's state outlives its widget"}] {
        button.reset();
        seen = marker;
    });
    backend.click(1);
    CHECK(seen == "the handler's state outlives its widget");
    CHECK_FALSE(backend.exists(1));
}

TEST_CASE("RecordingBackend: drag delivers the source's key to a target that accepts it", "[ui]") {
    RecordingBackend backend;
    std::vector<ui::Key> dropped;
    auto card = backend.createText(nullptr);
    card->setDragKey(intKey(7));
    auto plain = backend.createText(nullptr);
    auto numbers = backend.createPanel(nullptr);
    numbers->setDropHandler([](ui::Key const& key) { return std::holds_alternative<std::int64_t>(key); },
                            [&](ui::Key key) { dropped.push_back(std::move(key)); });
    auto words = backend.createPanel(nullptr);
    words->setDropHandler([](ui::Key const& key) { return std::holds_alternative<std::string>(key); },
                          [&](ui::Key key) { dropped.push_back(std::move(key)); });

    CHECK(backend.drag(1, 3));
    CHECK_FALSE(backend.drag(1, 4));  // refused by accepts
    CHECK_FALSE(backend.drag(1, 2));  // not a drop target
    CHECK_FALSE(backend.drag(2, 3));  // not draggable
    CHECK(dropped == std::vector<ui::Key>{intKey(7)});
}
```

Register it after `test_ui_view.cpp` in `tests/CMakeLists.txt`.

- [ ] **Step 2: Run the test to verify it fails**

Run: `cmake --build build/reactive --target morph_tests`
Expected: FAIL — `fatal error: 'morph/ui/backend.hpp' file not found`.

- [ ] **Step 3: Write the widget interfaces**

Create `include/morph/ui/backend.hpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0

#pragma once
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "../util/datetime.hpp"
#include "view.hpp"

/// @file
/// @brief The backend contract: the retained widgets a mount drives, and the factory that makes them.
///
/// Specified in `docs/spec/ui/backend_contract.md`.

namespace morph::ui {

/// @brief A retained widget. A new widget is visible, enabled, content-sized, not draggable and not a drop target.
///
/// Its destructor detaches it from its parent and frees its native resources. Every setter takes UTF-8 and may be
/// called with an unchanged value.
class Widget {
public:
    Widget() = default;
    virtual ~Widget() = default;
    Widget(Widget const&) = delete;
    Widget& operator=(Widget const&) = delete;
    Widget(Widget&&) = delete;
    Widget& operator=(Widget&&) = delete;

    /// @brief Shows or hides the widget and everything inside it.
    /// @param visible Whether it is shown.
    virtual void setVisible(bool visible) = 0;

    /// @brief Enables or disables input on the widget and everything inside it.
    /// @param enabled Whether it accepts input.
    virtual void setEnabled(bool enabled) = 0;

    /// @brief Sets the size request.
    /// @param hints The request in both dimensions.
    virtual void setLayout(LayoutHints const& hints) = 0;

    /// @brief Makes the widget draggable, carrying @p key, or not draggable.
    /// @param key Engaged: the key a drop delivers. `nullopt`: not draggable.
    virtual void setDragKey(std::optional<Key> const& key) = 0;

    /// @brief Makes the widget a drop target.
    /// @param accepts Whether a dragged key may be dropped here; the backend asks before highlighting.
    /// @param onDrop Called with the key on a drop that `accepts` allowed.
    virtual void setDropHandler(std::function<bool(Key const&)> accepts, std::function<void(Key)> onDrop) = 0;
};

/// @brief A widget with children. A factory given this container appends the new widget as its last child.
class ContainerWidget : public Widget {
public:
    /// @brief Moves @p child to position @p index: it is removed, then inserted before the child now at @p index.
    /// @param child One of this container's children.
    /// @param index The new position; past the end means last.
    virtual void moveChild(Widget& child, std::size_t index) = 0;
};

/// @brief Read-only text.
class TextWidget : public Widget {
public:
    /// @brief Replaces the text.
    /// @param text The text, UTF-8.
    virtual void setText(std::string_view text) = 0;

    /// @brief Sets the style.
    /// @param role The role, mapped to the backend's theme.
    virtual void setRole(TextRole role) = 0;
};

/// @brief A push button.
class ButtonWidget : public Widget {
public:
    /// @brief Replaces the caption.
    /// @param label The caption, UTF-8.
    virtual void setLabel(std::string_view label) = 0;

    /// @brief Sets what activation calls (Enter, Space or a click).
    /// @param onClick The handler; empty does nothing.
    virtual void setOnClick(Action onClick) = 0;
};

/// @brief An editable text field.
class TextInputWidget : public Widget {
public:
    /// @brief Replaces the text. Never calls the `onChange` handler.
    /// @param text The text, UTF-8.
    virtual void setText(std::string_view text) = 0;

    /// @brief Sets the text shown while the field is empty.
    /// @param placeholder The text, UTF-8.
    virtual void setPlaceholder(std::string_view placeholder) = 0;

    /// @brief Sets what a user edit calls, with the whole new text.
    /// @param onChange The handler; empty does nothing.
    virtual void setOnChange(std::function<void(std::string)> onChange) = 0;

    /// @brief Sets what a submit (Enter in single-line mode) calls, with the text.
    /// @param onSubmit The handler; empty does nothing.
    virtual void setOnSubmit(std::function<void(std::string)> onSubmit) = 0;
};

/// @brief A labelled check box.
class CheckboxWidget : public Widget {
public:
    /// @brief Replaces the caption.
    /// @param label The caption, UTF-8.
    virtual void setLabel(std::string_view label) = 0;

    /// @brief Sets the state. Never calls the `onToggle` handler.
    /// @param checked Whether it is checked.
    virtual void setChecked(bool checked) = 0;

    /// @brief Sets what a user toggle calls, with the new state.
    /// @param onToggle The handler; empty does nothing.
    virtual void setOnToggle(std::function<void(bool)> onToggle) = 0;
};

/// @brief A choice of one keyed option.
class SelectWidget : public Widget {
public:
    /// @brief Replaces the options.
    /// @param options The options, in order.
    virtual void setOptions(std::vector<SelectOption> const& options) = 0;

    /// @brief Marks the option with @p key. A key outside the options marks none. Never calls `onSelect`.
    /// @param key The key, or `nullopt` for no selection.
    virtual void setSelected(std::optional<Key> const& key) = 0;

    /// @brief Sets what a user choice calls, with the option's key.
    /// @param onSelect The handler; empty does nothing.
    virtual void setOnSelect(std::function<void(Key)> onSelect) = 0;
};

/// @brief A vertical list of commands.
class MenuWidget : public Widget {
public:
    /// @brief Replaces the entries.
    /// @param labels One caption per entry, in order.
    virtual void setItems(std::vector<std::string> const& labels) = 0;

    /// @brief Sets what choosing an entry calls, with its index.
    /// @param onActivate The handler; empty does nothing.
    virtual void setOnActivate(std::function<void(std::size_t)> onActivate) = 0;
};

/// @brief Children stacked along one axis, fixed at creation.
class StackWidget : public ContainerWidget {
public:
    /// @brief Sets the space between children.
    /// @param gap Backend units.
    virtual void setGap(int gap) = 0;
};

/// @brief Children laid out row-major in columns.
class GridWidget : public ContainerWidget {
public:
    /// @brief Sets the column count.
    /// @param columns At least one.
    virtual void setColumns(int columns) = 0;

    /// @brief Sets the space between cells.
    /// @param gap Backend units.
    virtual void setGap(int gap) = 0;

    /// @brief Sets how many columns a child spans; a child spans one until this is called.
    /// @param child One of this grid's children.
    /// @param span The column count it occupies.
    virtual void setSpan(Widget& child, int span) = 0;
};

/// @brief Empty space.
class SpacerWidget : public Widget {};

/// @brief A titled frame around its children, optionally collapsible.
class PanelWidget : public ContainerWidget {
public:
    /// @brief Replaces the title.
    /// @param title The title, UTF-8.
    virtual void setTitle(std::string_view title) = 0;

    /// @brief Sets the space between frame and content.
    /// @param padding Backend units.
    virtual void setPadding(int padding) = 0;

    /// @brief Sets whether the user can collapse the panel.
    /// @param collapsible Whether a collapse control is shown.
    virtual void setCollapsible(bool collapsible) = 0;

    /// @brief Collapses or expands the panel. Never calls the `onToggle` handler.
    /// @param collapsed Whether the content is hidden.
    virtual void setCollapsed(bool collapsed) = 0;

    /// @brief Sets what a user collapse or expand calls, with the requested state.
    /// @param onToggle The handler; empty does nothing.
    virtual void setOnToggle(std::function<void(bool)> onToggle) = 0;
};

/// @brief A scrollable viewport, on the axis fixed at creation, that keeps the focused child visible.
class ScrollWidget : public ContainerWidget {};

/// @brief A progress indicator.
class BusyWidget : public Widget {
public:
    /// @brief Starts or stops the animation.
    /// @param active Whether it animates.
    virtual void setActive(bool active) = 0;

    /// @brief Replaces the caption.
    /// @param label The caption, UTF-8.
    virtual void setLabel(std::string_view label) = 0;
};

/// @brief A date or date-and-time field, in the mode and display zone fixed at creation.
class DateTimeInputWidget : public Widget {
public:
    /// @brief Shows an instant. Never calls the `onChange` handler.
    /// @param value The instant; `nullopt` or an empty `Timestamp` shows an empty field.
    virtual void setValue(std::optional<morph::time::Timestamp> const& value) = 0;

    /// @brief Sets what a user edit calls, with the instant entered or `nullopt` for a cleared field.
    /// @param onChange The handler; empty does nothing.
    virtual void setOnChange(std::function<void(std::optional<morph::time::Timestamp>)> onChange) = 0;
};

/// @brief An integer on a range.
class SliderWidget : public Widget {
public:
    /// @brief Sets the range and the increment.
    /// @param minimum The smallest value.
    /// @param maximum The largest value.
    /// @param step The increment between neighbouring values.
    // NOLINTNEXTLINE(bugprone-easily-swappable-parameters) -- the contract's signature; a range reads in this order
    virtual void setRange(std::int64_t minimum, std::int64_t maximum, std::int64_t step) = 0;

    /// @brief Shows a value. Never calls the `onChange` handler.
    /// @param value The value.
    virtual void setValue(std::int64_t value) = 0;

    /// @brief Sets what a user move calls, with the new value.
    /// @param onChange The handler; empty does nothing.
    virtual void setOnChange(std::function<void(std::int64_t)> onChange) = 0;
};

/// @brief A file path field, in the mode fixed at creation.
class FilePickerWidget : public Widget {
public:
    /// @brief Shows a path. Never calls the `onPicked` handler.
    /// @param path The path, UTF-8.
    virtual void setPath(std::string_view path) = 0;

    /// @brief Sets what a user pick calls, with the path.
    /// @param onPicked The handler; empty does nothing.
    virtual void setOnPicked(std::function<void(std::string)> onPicked) = 0;
};

/// @brief Makes widgets. Each factory appends the new widget to @p parent, or makes a root when it is null, and never
///        returns null.
class IViewBackend {
public:
    IViewBackend() = default;
    virtual ~IViewBackend() = default;
    IViewBackend(IViewBackend const&) = delete;
    IViewBackend& operator=(IViewBackend const&) = delete;
    IViewBackend(IViewBackend&&) = delete;
    IViewBackend& operator=(IViewBackend&&) = delete;

    /// @brief Makes read-only text.
    /// @param parent The container to append to, or null for a root.
    /// @return The widget.
    [[nodiscard]] virtual std::unique_ptr<TextWidget> createText(ContainerWidget* parent) = 0;

    /// @brief Makes a push button.
    /// @param parent The container to append to, or null for a root.
    /// @return The widget.
    [[nodiscard]] virtual std::unique_ptr<ButtonWidget> createButton(ContainerWidget* parent) = 0;

    /// @brief Makes a text field.
    /// @param parent The container to append to, or null for a root.
    /// @param mode Single-line, multiline or password.
    /// @return The widget.
    [[nodiscard]] virtual std::unique_ptr<TextInputWidget> createTextInput(ContainerWidget* parent,
                                                                           TextInputMode mode) = 0;

    /// @brief Makes a check box.
    /// @param parent The container to append to, or null for a root.
    /// @return The widget.
    [[nodiscard]] virtual std::unique_ptr<CheckboxWidget> createCheckbox(ContainerWidget* parent) = 0;

    /// @brief Makes a choice of one option.
    /// @param parent The container to append to, or null for a root.
    /// @param style Dropdown or radio.
    /// @return The widget.
    [[nodiscard]] virtual std::unique_ptr<SelectWidget> createSelect(ContainerWidget* parent, SelectStyle style) = 0;

    /// @brief Makes a command list.
    /// @param parent The container to append to, or null for a root.
    /// @return The widget.
    [[nodiscard]] virtual std::unique_ptr<MenuWidget> createMenu(ContainerWidget* parent) = 0;

    /// @brief Makes a stack.
    /// @param parent The container to append to, or null for a root.
    /// @param axis Vertical (a column) or horizontal (a row).
    /// @return The widget.
    [[nodiscard]] virtual std::unique_ptr<StackWidget> createStack(ContainerWidget* parent, Axis axis) = 0;

    /// @brief Makes a grid.
    /// @param parent The container to append to, or null for a root.
    /// @return The widget.
    [[nodiscard]] virtual std::unique_ptr<GridWidget> createGrid(ContainerWidget* parent) = 0;

    /// @brief Makes empty space.
    /// @param parent The container to append to, or null for a root.
    /// @return The widget.
    [[nodiscard]] virtual std::unique_ptr<SpacerWidget> createSpacer(ContainerWidget* parent) = 0;

    /// @brief Makes a titled frame.
    /// @param parent The container to append to, or null for a root.
    /// @return The widget.
    [[nodiscard]] virtual std::unique_ptr<PanelWidget> createPanel(ContainerWidget* parent) = 0;

    /// @brief Makes a scroll area.
    /// @param parent The container to append to, or null for a root.
    /// @param axis The scroll direction.
    /// @return The widget.
    [[nodiscard]] virtual std::unique_ptr<ScrollWidget> createScroll(ContainerWidget* parent, Axis axis) = 0;

    /// @brief Makes a progress indicator.
    /// @param parent The container to append to, or null for a root.
    /// @return The widget.
    [[nodiscard]] virtual std::unique_ptr<BusyWidget> createBusy(ContainerWidget* parent) = 0;

    /// @brief Makes a date or date-and-time field.
    /// @param parent The container to append to, or null for a root.
    /// @param mode Date, or date and time.
    /// @param offsetMinutes The display zone's offset from UTC.
    /// @return The widget.
    [[nodiscard]] virtual std::unique_ptr<DateTimeInputWidget> createDateTimeInput(ContainerWidget* parent,
                                                                                   DateMode mode,
                                                                                   int offsetMinutes) = 0;

    /// @brief Makes a slider.
    /// @param parent The container to append to, or null for a root.
    /// @return The widget.
    [[nodiscard]] virtual std::unique_ptr<SliderWidget> createSlider(ContainerWidget* parent) = 0;

    /// @brief Makes a file path field.
    /// @param parent The container to append to, or null for a root.
    /// @param mode Open or save.
    /// @return The widget.
    [[nodiscard]] virtual std::unique_ptr<FilePickerWidget> createFilePicker(ContainerWidget* parent,
                                                                             FilePickerMode mode) = 0;
};

}  // namespace morph::ui
```

- [ ] **Step 4: Write `RecordingBackend`**

Create `include/morph/ui/testing/recording_backend.hpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0

#pragma once
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

#include "../../util/datetime.hpp"
#include "../backend.hpp"
#include "../view.hpp"

/// @file
/// @brief `morph::ui::testing::RecordingBackend`: a headless widget tree with an operation log, a golden dump and
///        user-interaction helpers — the test double and the reference for every real backend.
///
/// Specified in `docs/spec/ui/backend_contract.md`, "RecordingBackend".

// Every interaction helper takes a widget id and then what the user entered; for integer and boolean input the
// two convert into each other, and the signatures are the contract's.
// NOLINTBEGIN(bugprone-easily-swappable-parameters)

namespace morph::ui::testing {

namespace detail {

/// @brief Properties fixed when a widget is created, as name and formatted value.
using PropList = std::vector<std::pair<std::string, std::string>>;

/// @brief Formats a flag.
/// @param value The flag.
/// @return `true` or `false`.
[[nodiscard]] inline std::string formatBool(bool value) { return value ? "true" : "false"; }

/// @brief Formats a key: an integer in decimal, a string in double quotes, so `7` and `"7"` differ.
/// @param key The key.
/// @return Its text.
[[nodiscard]] inline std::string formatKey(Key const& key) {
    if (auto const* const number = std::get_if<std::int64_t>(&key); number != nullptr) {
        return std::to_string(*number);
    }
    return '"' + std::get<std::string>(key) + '"';
}

/// @brief Formats an optional key.
/// @param key The key, or none.
/// @return `formatKey(*key)`, or `none`.
[[nodiscard]] inline std::string formatOptionalKey(std::optional<Key> const& key) {
    return key.has_value() ? formatKey(*key) : std::string{"none"};
}

/// @brief Formats one dimension's sizing.
/// @param sizing The sizing.
/// @return `content`, `fixed(n)` or `stretch(n)`.
[[nodiscard]] inline std::string formatSizing(Sizing sizing) {
    switch (sizing.kind) {
    case Sizing::Kind::Content:
        return "content";
    case Sizing::Kind::Fixed:
        return "fixed(" + std::to_string(sizing.amount) + ")";
    case Sizing::Kind::Stretch:
        return "stretch(" + std::to_string(sizing.amount) + ")";
    default:
        return "unknown";
    }
}

/// @brief Formats a size request.
/// @param hints The request.
/// @return `width/height`, each as `formatSizing` gives it.
[[nodiscard]] inline std::string formatLayout(LayoutHints const& hints) {
    return formatSizing(hints.width) + "/" + formatSizing(hints.height);
}

/// @brief Formats a date-time value.
/// @param value The value.
/// @return `none` for `nullopt`, `empty` for an empty `Timestamp`, else the ISO-8601 UTC instant.
[[nodiscard]] inline std::string formatTimestamp(std::optional<morph::time::Timestamp> const& value) {
    if (!value.has_value()) {
        return "none";
    }
    if (!value->hasValue()) {
        return "empty";
    }
    return (**value).toIso8601();
}

/// @brief Formats a list as `[a,b,c]`.
/// @tparam T The element type.
/// @tparam F A callable turning a `T const&` into a `std::string`.
/// @param items The elements.
/// @param each Formats one element.
/// @return The list's text.
template <typename T, typename F>
[[nodiscard]] std::string formatList(std::vector<T> const& items, F const& each) {
    std::string text = "[";
    bool first = true;
    for (T const& item : items) {
        if (!first) {
            text += ',';
        }
        first = false;
        text += each(item);
    }
    return text + "]";
}

/// @brief A text role's name.
/// @param role The role.
/// @return The enumerator's name.
[[nodiscard]] inline std::string enumName(TextRole role) {
    switch (role) {
    case TextRole::Normal:
        return "Normal";
    case TextRole::Muted:
        return "Muted";
    case TextRole::Heading:
        return "Heading";
    case TextRole::Error:
        return "Error";
    case TextRole::Success:
        return "Success";
    default:
        return "unknown";
    }
}

/// @brief A text input mode's name.
/// @param mode The mode.
/// @return The enumerator's name.
[[nodiscard]] inline std::string enumName(TextInputMode mode) {
    switch (mode) {
    case TextInputMode::SingleLine:
        return "SingleLine";
    case TextInputMode::Multiline:
        return "Multiline";
    case TextInputMode::Password:
        return "Password";
    default:
        return "unknown";
    }
}

/// @brief A select style's name.
/// @param style The style.
/// @return The enumerator's name.
[[nodiscard]] inline std::string enumName(SelectStyle style) {
    switch (style) {
    case SelectStyle::Dropdown:
        return "Dropdown";
    case SelectStyle::Radio:
        return "Radio";
    default:
        return "unknown";
    }
}

/// @brief An axis's name.
/// @param axis The axis.
/// @return The enumerator's name.
[[nodiscard]] inline std::string enumName(Axis axis) {
    switch (axis) {
    case Axis::Vertical:
        return "Vertical";
    case Axis::Horizontal:
        return "Horizontal";
    default:
        return "unknown";
    }
}

/// @brief A date mode's name.
/// @param mode The mode.
/// @return The enumerator's name.
[[nodiscard]] inline std::string enumName(DateMode mode) {
    switch (mode) {
    case DateMode::Date:
        return "Date";
    case DateMode::DateTime:
        return "DateTime";
    default:
        return "unknown";
    }
}

/// @brief A file picker mode's name.
/// @param mode The mode.
/// @return The enumerator's name.
[[nodiscard]] inline std::string enumName(FilePickerMode mode) {
    switch (mode) {
    case FilePickerMode::Open:
        return "Open";
    case FilePickerMode::Save:
        return "Save";
    default:
        return "unknown";
    }
}

/// @brief The handlers a widget was given, which the interaction helpers call.
struct Callbacks {
    /// @brief From `ButtonWidget::setOnClick`.
    Action click;
    /// @brief From `TextInputWidget::setOnChange`.
    std::function<void(std::string)> change;
    /// @brief From `TextInputWidget::setOnSubmit`.
    std::function<void(std::string)> submit;
    /// @brief From `CheckboxWidget::setOnToggle` or `PanelWidget::setOnToggle`.
    std::function<void(bool)> toggle;
    /// @brief From `SelectWidget::setOnSelect`.
    std::function<void(Key)> select;
    /// @brief From `MenuWidget::setOnActivate` or `TabsWidget::setOnSelect`.
    std::function<void(std::size_t)> index;
    /// @brief From `DateTimeInputWidget::setOnChange`.
    std::function<void(std::optional<morph::time::Timestamp>)> dateTime;
    /// @brief From `SliderWidget::setOnChange`.
    std::function<void(std::int64_t)> slide;
    /// @brief From `FilePickerWidget::setOnPicked`.
    std::function<void(std::string)> picked;
    /// @brief The drop handler's predicate, from `Widget::setDropHandler`.
    std::function<bool(Key const&)> accepts;
    /// @brief The drop handler, from `Widget::setDropHandler`.
    std::function<void(Key)> drop;
};

/// @brief One fake widget's state.
struct Record {
    /// @brief The widget's id: its creation number, from 1.
    int id = 0;
    /// @brief The kind shown in the log and the dump.
    std::string kind;
    /// @brief The parent's id; 0 for a root.
    int parent = 0;
    /// @brief The children's ids, in order.
    std::vector<int> children;
    /// @brief Every property set so far, formatted, in name order.
    std::map<std::string, std::string, std::less<>> props;
    /// @brief The handlers set so far.
    Callbacks callbacks;
    /// @brief The drag key, as `setDragKey` last set it.
    std::optional<Key> dragKey;
    /// @brief The fake widget object.
    Widget* widget = nullptr;
};

/// @brief The records, the roots and the operation log that every fake widget of one backend writes to.
///
/// Held by `shared_ptr` from the backend and from every fake, so a fake that outlives its backend still has
/// somewhere to record its destruction.
class RecordStore {
public:
    /// @brief Records a new widget appended to @p parent and logs its creation.
    /// @param kind The kind shown in the log and the dump.
    /// @param parent The container it is appended to, or null for a root.
    /// @param widget The fake widget.
    /// @param initial Properties fixed at creation; recorded without a log line.
    /// @return The new widget's id.
    int create(std::string kind, ContainerWidget const* parent, Widget& widget, PropList initial) {
        int const widgetId = ++_nextId;
        int const parentId = parent == nullptr ? 0 : idOf(*parent);
        Record record{.id = widgetId, .kind = std::move(kind), .parent = parentId, .widget = &widget};
        for (auto& [name, value] : initial) {
            record.props.insert_or_assign(std::move(name), std::move(value));
        }
        _log.push_back("create " + label(record) + " in " +
                       (parentId == 0 ? std::string{"root"} : label(at(parentId))));
        siblingsOf(parentId).push_back(widgetId);
        _ids.insert_or_assign(&widget, widgetId);
        _records.insert_or_assign(widgetId, std::move(record));
        return widgetId;
    }

    /// @brief Sets a property through a widget setter, and logs it.
    /// @param widgetId The widget.
    /// @param name The property.
    /// @param value Its formatted value.
    void set(int widgetId, std::string_view name, std::string value) {
        Record& record = at(widgetId);
        _log.push_back("set " + label(record) + " " + std::string{name} + "=" + value);
        record.props.insert_or_assign(std::string{name}, std::move(value));
    }

    /// @brief Sets a property the user changed through the widget itself; no setter ran, so nothing is logged.
    /// @param widgetId The widget.
    /// @param name The property.
    /// @param value Its formatted value.
    void setByUser(int widgetId, std::string_view name, std::string value) {
        at(widgetId).props.insert_or_assign(std::string{name}, std::move(value));
    }

    /// @brief Moves a widget among its siblings, as `ContainerWidget::moveChild` defines it, and logs it.
    /// @param widgetId The widget.
    /// @param index The new position; past the end means last.
    void move(int widgetId, std::size_t index) {
        Record const& record = at(widgetId);
        std::vector<int>& siblings = siblingsOf(record.parent);
        std::erase(siblings, widgetId);
        auto const position = static_cast<std::ptrdiff_t>(std::min(index, siblings.size()));
        siblings.insert(siblings.begin() + position, widgetId);
        _log.push_back("move " + label(record) + " to " + std::to_string(index));
    }

    /// @brief Forgets a destroyed widget and logs it. Its remaining children become roots.
    /// @param widgetId The widget; an unknown id does nothing.
    void destroy(int widgetId) {
        auto const found = _records.find(widgetId);
        if (found == _records.end()) {
            return;
        }
        Record const& record = found->second;
        _log.push_back("destroy " + label(record));
        std::erase(siblingsOf(record.parent), widgetId);
        for (int const child : record.children) {
            if (auto const orphan = _records.find(child); orphan != _records.end()) {
                orphan->second.parent = 0;
                _roots.push_back(child);
            }
        }
        _ids.erase(record.widget);
        _records.erase(found);
    }

    /// @brief A widget's record.
    /// @param widgetId The widget.
    /// @return The record.
    /// @throws std::out_of_range for an id that names no live widget.
    [[nodiscard]] Record& at(int widgetId) {
        auto const found = _records.find(widgetId);
        if (found == _records.end()) {
            throw std::out_of_range{"RecordingBackend: no widget #" + std::to_string(widgetId)};
        }
        return found->second;
    }

    /// @brief A widget's record.
    /// @param widgetId The widget.
    /// @return The record.
    /// @throws std::out_of_range for an id that names no live widget.
    [[nodiscard]] Record const& at(int widgetId) const {
        auto const found = _records.find(widgetId);
        if (found == _records.end()) {
            throw std::out_of_range{"RecordingBackend: no widget #" + std::to_string(widgetId)};
        }
        return found->second;
    }

    /// @brief The id of a live widget made by this store's backend.
    /// @param widget The widget.
    /// @return Its id.
    /// @throws std::out_of_range when @p widget is not one of them.
    [[nodiscard]] int idOf(Widget const& widget) const {
        auto const found = _ids.find(&widget);
        if (found == _ids.end()) {
            throw std::out_of_range{"RecordingBackend: not a live widget of this backend"};
        }
        return found->second;
    }

    /// @brief Every live widget's record.
    /// @return The records, by id.
    [[nodiscard]] std::map<int, Record> const& records() const noexcept { return _records; }

    /// @brief The operation log.
    /// @return One line per operation, oldest first.
    [[nodiscard]] std::vector<std::string> const& log() const noexcept { return _log; }

    /// @brief Empties the operation log.
    void clearLog() noexcept { _log.clear(); }

    /// @brief The golden tree.
    /// @return One line per live widget, children indented two spaces per depth.
    [[nodiscard]] std::string dump() const {
        std::string text;
        for (int const root : _roots) {
            dumpInto(text, root, 0);
        }
        return text;
    }

private:
    [[nodiscard]] static std::string label(Record const& record) {
        return record.kind + "#" + std::to_string(record.id);
    }

    std::vector<int>& siblingsOf(int parentId) { return parentId == 0 ? _roots : at(parentId).children; }

    // NOLINTNEXTLINE(misc-no-recursion) -- a widget tree is dumped by descending it
    void dumpInto(std::string& text, int widgetId, std::size_t depth) const {
        Record const& record = at(widgetId);
        text.append(depth * 2, ' ');
        text += label(record);
        for (auto const& [name, value] : record.props) {
            text += ' ';
            text += name;
            text += '=';
            text += value;
        }
        text += '\n';
        for (int const child : record.children) {
            dumpInto(text, child, depth + 1);
        }
    }

    std::map<int, Record> _records;
    std::unordered_map<Widget const*, int> _ids;
    std::vector<int> _roots;
    std::vector<std::string> _log;
    int _nextId = 0;
};

/// @brief The `Widget` half of every fake: records itself on construction, records `Common`'s setters, and records
///        its destruction.
/// @tparam Interface The widget interface the fake implements.
template <typename Interface>
class FakeLeaf : public Interface {
public:
    /// @param store Where the widget records itself.
    /// @param kind The kind shown in the log and the dump.
    /// @param parent The container it is appended to, or null for a root.
    /// @param initial Properties fixed at creation.
    FakeLeaf(std::shared_ptr<RecordStore> store, std::string kind, ContainerWidget const* parent,
             PropList initial = {})
        : _store{std::move(store)}, _id{_store->create(std::move(kind), parent, *this, std::move(initial))} {}

    ~FakeLeaf() override {
        try {
            _store->destroy(_id);
        } catch (...) {  // NOLINT(bugprone-empty-catch)
            // Logging the destruction failed to allocate; the record stays, and a test reads it as a leak.
        }
    }

    FakeLeaf(FakeLeaf const&) = delete;
    FakeLeaf& operator=(FakeLeaf const&) = delete;
    FakeLeaf(FakeLeaf&&) = delete;
    FakeLeaf& operator=(FakeLeaf&&) = delete;

    void setVisible(bool visible) override { set("visible", formatBool(visible)); }
    void setEnabled(bool enabled) override { set("enabled", formatBool(enabled)); }
    void setLayout(LayoutHints const& hints) override { set("layout", formatLayout(hints)); }

    void setDragKey(std::optional<Key> const& key) override {
        _store->at(_id).dragKey = key;
        set("dragKey", formatOptionalKey(key));
    }

    void setDropHandler(std::function<bool(Key const&)> accepts, std::function<void(Key)> onDrop) override {
        Callbacks& handlers = callbacks();
        handlers.accepts = std::move(accepts);
        handlers.drop = std::move(onDrop);
        set("drop", "handler");
    }

protected:
    /// @brief Records a property set through a setter.
    /// @param name The property.
    /// @param value Its formatted value.
    void set(std::string_view name, std::string value) { _store->set(_id, name, std::move(value)); }

    /// @brief This widget's handlers.
    /// @return The handlers, for a setter to store into.
    [[nodiscard]] Callbacks& callbacks() { return _store->at(_id).callbacks; }

    /// @brief The store this widget records into.
    /// @return The store.
    [[nodiscard]] RecordStore& store() const noexcept { return *_store; }

    /// @brief This widget's id.
    /// @return The id.
    [[nodiscard]] int id() const noexcept { return _id; }

private:
    std::shared_ptr<RecordStore> _store;
    int _id;
};

/// @brief The `ContainerWidget` half of every fake container.
/// @tparam Interface The container interface the fake implements.
template <typename Interface>
class FakeContainer : public FakeLeaf<Interface> {
public:
    using FakeLeaf<Interface>::FakeLeaf;

    void moveChild(Widget& child, std::size_t index) override {
        RecordStore& records = this->store();
        int const childId = records.idOf(child);
        if (records.at(childId).parent != this->id()) {
            throw std::logic_error{"RecordingBackend: moveChild of a widget that is not this container's child"};
        }
        records.move(childId, index);
    }
};

/// @brief A fake `TextWidget`: properties `text`, `role`.
class FakeText final : public FakeLeaf<TextWidget> {
public:
    using FakeLeaf<TextWidget>::FakeLeaf;
    void setText(std::string_view text) override { set("text", std::string{text}); }
    void setRole(TextRole role) override { set("role", enumName(role)); }
};

/// @brief A fake `ButtonWidget`: property `label`.
class FakeButton final : public FakeLeaf<ButtonWidget> {
public:
    using FakeLeaf<ButtonWidget>::FakeLeaf;
    void setLabel(std::string_view label) override { set("label", std::string{label}); }
    void setOnClick(Action onClick) override { callbacks().click = std::move(onClick); }
};

/// @brief A fake `TextInputWidget`: properties `mode`, `text`, `placeholder`.
class FakeTextInput final : public FakeLeaf<TextInputWidget> {
public:
    using FakeLeaf<TextInputWidget>::FakeLeaf;
    void setText(std::string_view text) override { set("text", std::string{text}); }
    void setPlaceholder(std::string_view placeholder) override { set("placeholder", std::string{placeholder}); }
    void setOnChange(std::function<void(std::string)> onChange) override { callbacks().change = std::move(onChange); }
    void setOnSubmit(std::function<void(std::string)> onSubmit) override { callbacks().submit = std::move(onSubmit); }
};

/// @brief A fake `CheckboxWidget`: properties `label`, `checked`.
class FakeCheckbox final : public FakeLeaf<CheckboxWidget> {
public:
    using FakeLeaf<CheckboxWidget>::FakeLeaf;
    void setLabel(std::string_view label) override { set("label", std::string{label}); }
    void setChecked(bool checked) override { set("checked", formatBool(checked)); }
    void setOnToggle(std::function<void(bool)> onToggle) override { callbacks().toggle = std::move(onToggle); }
};

/// @brief A fake `SelectWidget`: properties `style`, `options`, `selected`.
class FakeSelect final : public FakeLeaf<SelectWidget> {
public:
    using FakeLeaf<SelectWidget>::FakeLeaf;
    void setOptions(std::vector<SelectOption> const& options) override {
        set("options", formatList(options, [](SelectOption const& option) {
                return formatKey(option.key) + ":" + option.label;
            }));
    }
    void setSelected(std::optional<Key> const& key) override { set("selected", formatOptionalKey(key)); }
    void setOnSelect(std::function<void(Key)> onSelect) override { callbacks().select = std::move(onSelect); }
};

/// @brief A fake `MenuWidget`: property `items`.
class FakeMenu final : public FakeLeaf<MenuWidget> {
public:
    using FakeLeaf<MenuWidget>::FakeLeaf;
    void setItems(std::vector<std::string> const& labels) override {
        set("items", formatList(labels, [](std::string const& label) { return label; }));
    }
    void setOnActivate(std::function<void(std::size_t)> onActivate) override {
        callbacks().index = std::move(onActivate);
    }
};

/// @brief A fake `StackWidget`, of kind `Column` or `Row`: property `gap`.
class FakeStack final : public FakeContainer<StackWidget> {
public:
    using FakeContainer<StackWidget>::FakeContainer;
    void setGap(int gap) override { set("gap", std::to_string(gap)); }
};

/// @brief A fake `GridWidget`: properties `columns`, `gap`, and `span` on a child.
class FakeGrid final : public FakeContainer<GridWidget> {
public:
    using FakeContainer<GridWidget>::FakeContainer;
    void setColumns(int columns) override { set("columns", std::to_string(columns)); }
    void setGap(int gap) override { set("gap", std::to_string(gap)); }
    void setSpan(Widget& child, int span) override { store().set(store().idOf(child), "span", std::to_string(span)); }
};

/// @brief A fake `PanelWidget`: properties `title`, `padding`, `collapsible`, `collapsed`.
class FakePanel final : public FakeContainer<PanelWidget> {
public:
    using FakeContainer<PanelWidget>::FakeContainer;
    void setTitle(std::string_view title) override { set("title", std::string{title}); }
    void setPadding(int padding) override { set("padding", std::to_string(padding)); }
    void setCollapsible(bool collapsible) override { set("collapsible", formatBool(collapsible)); }
    void setCollapsed(bool collapsed) override { set("collapsed", formatBool(collapsed)); }
    void setOnToggle(std::function<void(bool)> onToggle) override { callbacks().toggle = std::move(onToggle); }
};

/// @brief A fake `BusyWidget`: properties `active`, `label`.
class FakeBusy final : public FakeLeaf<BusyWidget> {
public:
    using FakeLeaf<BusyWidget>::FakeLeaf;
    void setActive(bool active) override { set("active", formatBool(active)); }
    void setLabel(std::string_view label) override { set("label", std::string{label}); }
};

/// @brief A fake `DateTimeInputWidget`: properties `mode`, `offset`, `value`.
class FakeDateTimeInput final : public FakeLeaf<DateTimeInputWidget> {
public:
    using FakeLeaf<DateTimeInputWidget>::FakeLeaf;
    void setValue(std::optional<morph::time::Timestamp> const& value) override {
        set("value", formatTimestamp(value));
    }
    void setOnChange(std::function<void(std::optional<morph::time::Timestamp>)> onChange) override {
        callbacks().dateTime = std::move(onChange);
    }
};

/// @brief A fake `SliderWidget`: properties `range` (`minimum..maximum/step`), `value`.
class FakeSlider final : public FakeLeaf<SliderWidget> {
public:
    using FakeLeaf<SliderWidget>::FakeLeaf;
    void setRange(std::int64_t minimum, std::int64_t maximum, std::int64_t step) override {
        set("range", std::to_string(minimum) + ".." + std::to_string(maximum) + "/" + std::to_string(step));
    }
    void setValue(std::int64_t value) override { set("value", std::to_string(value)); }
    void setOnChange(std::function<void(std::int64_t)> onChange) override { callbacks().slide = std::move(onChange); }
};

/// @brief A fake `FilePickerWidget`: properties `mode`, `path`.
class FakeFilePicker final : public FakeLeaf<FilePickerWidget> {
public:
    using FakeLeaf<FilePickerWidget>::FakeLeaf;
    void setPath(std::string_view path) override { set("path", std::string{path}); }
    void setOnPicked(std::function<void(std::string)> onPicked) override { callbacks().picked = std::move(onPicked); }
};

}  // namespace detail

/// @brief A headless `IViewBackend`: fake widgets that record every operation, a golden dump of the live tree, and
///        helpers that act as the user would.
///
/// Widget ids are creation numbers from 1. A hidden or disabled widget ignores every interaction helper, as a real
/// one would. A helper copies the handler before calling it, so a handler may destroy its own widget.
class RecordingBackend final : public IViewBackend {
public:
    [[nodiscard]] std::unique_ptr<TextWidget> createText(ContainerWidget* parent) override {
        return std::make_unique<detail::FakeText>(_store, "Text", parent);
    }
    [[nodiscard]] std::unique_ptr<ButtonWidget> createButton(ContainerWidget* parent) override {
        return std::make_unique<detail::FakeButton>(_store, "Button", parent);
    }
    [[nodiscard]] std::unique_ptr<TextInputWidget> createTextInput(ContainerWidget* parent,
                                                                   TextInputMode mode) override {
        return std::make_unique<detail::FakeTextInput>(_store, "TextInput", parent,
                                                       detail::PropList{{"mode", detail::enumName(mode)}});
    }
    [[nodiscard]] std::unique_ptr<CheckboxWidget> createCheckbox(ContainerWidget* parent) override {
        return std::make_unique<detail::FakeCheckbox>(_store, "Checkbox", parent);
    }
    [[nodiscard]] std::unique_ptr<SelectWidget> createSelect(ContainerWidget* parent, SelectStyle style) override {
        return std::make_unique<detail::FakeSelect>(_store, "Select", parent,
                                                    detail::PropList{{"style", detail::enumName(style)}});
    }
    [[nodiscard]] std::unique_ptr<MenuWidget> createMenu(ContainerWidget* parent) override {
        return std::make_unique<detail::FakeMenu>(_store, "Menu", parent);
    }
    [[nodiscard]] std::unique_ptr<StackWidget> createStack(ContainerWidget* parent, Axis axis) override {
        return std::make_unique<detail::FakeStack>(_store, axis == Axis::Vertical ? "Column" : "Row", parent);
    }
    [[nodiscard]] std::unique_ptr<GridWidget> createGrid(ContainerWidget* parent) override {
        return std::make_unique<detail::FakeGrid>(_store, "Grid", parent);
    }
    [[nodiscard]] std::unique_ptr<SpacerWidget> createSpacer(ContainerWidget* parent) override {
        return std::make_unique<detail::FakeLeaf<SpacerWidget>>(_store, "Spacer", parent);
    }
    [[nodiscard]] std::unique_ptr<PanelWidget> createPanel(ContainerWidget* parent) override {
        return std::make_unique<detail::FakePanel>(_store, "Panel", parent);
    }
    [[nodiscard]] std::unique_ptr<ScrollWidget> createScroll(ContainerWidget* parent, Axis axis) override {
        return std::make_unique<detail::FakeContainer<ScrollWidget>>(
            _store, "Scroll", parent, detail::PropList{{"axis", detail::enumName(axis)}});
    }
    [[nodiscard]] std::unique_ptr<BusyWidget> createBusy(ContainerWidget* parent) override {
        return std::make_unique<detail::FakeBusy>(_store, "Busy", parent);
    }
    [[nodiscard]] std::unique_ptr<DateTimeInputWidget> createDateTimeInput(ContainerWidget* parent, DateMode mode,
                                                                           int offsetMinutes) override {
        return std::make_unique<detail::FakeDateTimeInput>(
            _store, "DateTimeInput", parent,
            detail::PropList{{"mode", detail::enumName(mode)}, {"offset", std::to_string(offsetMinutes)}});
    }
    [[nodiscard]] std::unique_ptr<SliderWidget> createSlider(ContainerWidget* parent) override {
        return std::make_unique<detail::FakeSlider>(_store, "Slider", parent);
    }
    [[nodiscard]] std::unique_ptr<FilePickerWidget> createFilePicker(ContainerWidget* parent,
                                                                     FilePickerMode mode) override {
        return std::make_unique<detail::FakeFilePicker>(_store, "FilePicker", parent,
                                                        detail::PropList{{"mode", detail::enumName(mode)}});
    }

    /// @brief The golden tree of the live widgets.
    /// @return One line per widget, `Kind#id name=value …` in property-name order, children indented two spaces
    ///         per depth, each line ending in a newline.
    [[nodiscard]] std::string dump() const { return _store->dump(); }

    /// @brief The operation log since construction or the last `clearLog()`.
    /// @return `create Kind#id in Parent#id` (`in root` for a root), `set Kind#id name=value`,
    ///         `move Kind#id to n`, `destroy Kind#id`, oldest first. Interaction helpers add nothing.
    [[nodiscard]] std::vector<std::string> const& log() const { return _store->log(); }

    /// @brief Empties the operation log.
    void clearLog() { _store->clearLog(); }

    /// @brief The first live widget, by id, of a kind whose property has a value.
    /// @param kind The kind, as the dump shows it.
    /// @param name The property.
    /// @param value Its formatted value.
    /// @return The widget's id, or `nullopt`.
    [[nodiscard]] std::optional<int> find(std::string_view kind, std::string_view name, std::string_view value) const {
        for (auto const& [widgetId, record] : _store->records()) {
            if (record.kind != kind) {
                continue;
            }
            if (auto const found = record.props.find(name); found != record.props.end() && found->second == value) {
                return widgetId;
            }
        }
        return std::nullopt;
    }

    /// @brief Every live widget of a kind.
    /// @param kind The kind, as the dump shows it.
    /// @return Their ids, in creation order.
    [[nodiscard]] std::vector<int> all(std::string_view kind) const {
        std::vector<int> ids;
        for (auto const& [widgetId, record] : _store->records()) {
            if (record.kind == kind) {
                ids.push_back(widgetId);
            }
        }
        return ids;
    }

    /// @brief A property's formatted value.
    /// @param widgetId The widget.
    /// @param name The property.
    /// @return The value, or an empty string when it was never set.
    /// @throws std::out_of_range for an id that names no live widget.
    [[nodiscard]] std::string prop(int widgetId, std::string_view name) const {
        auto const& props = _store->at(widgetId).props;
        auto const found = props.find(name);
        return found == props.end() ? std::string{} : found->second;
    }

    /// @brief Whether a widget is alive.
    /// @param widgetId The widget.
    /// @return False once its destructor ran.
    [[nodiscard]] bool exists(int widgetId) const { return _store->records().contains(widgetId); }

    /// @brief The id of one of this backend's live widgets.
    /// @param widget The widget.
    /// @return Its id.
    /// @throws std::out_of_range when @p widget is not one.
    [[nodiscard]] int idOf(Widget const& widget) const { return _store->idOf(widget); }

    /// @brief A live widget's kind.
    /// @param widgetId The widget.
    /// @return The kind, as the dump shows it.
    /// @throws std::out_of_range for an id that names no live widget.
    [[nodiscard]] std::string kindOf(int widgetId) const { return _store->at(widgetId).kind; }

    /// @brief A live widget's children.
    /// @param widgetId The widget.
    /// @return Their ids, in order.
    /// @throws std::out_of_range for an id that names no live widget.
    [[nodiscard]] std::vector<int> children(int widgetId) const { return _store->at(widgetId).children; }

    /// @brief A live widget object.
    /// @param widgetId The widget.
    /// @return The widget.
    /// @throws std::out_of_range for an id that names no live widget.
    [[nodiscard]] Widget* widget(int widgetId) const { return _store->at(widgetId).widget; }

    /// @brief Activates a button.
    /// @param widgetId The button.
    void click(int widgetId) {
        if (detail::Record const* const record = actionable(widgetId); record != nullptr) {
            invoke(record->callbacks.click);
        }
    }

    /// @brief Types into a text field: the field shows @p text, then `onChange` gets it.
    /// @param widgetId The field.
    /// @param text The whole new text.
    void edit(int widgetId, std::string text) {
        if (detail::Record const* const record = actionable(widgetId); record != nullptr) {
            _store->setByUser(widgetId, "text", text);
            invoke(record->callbacks.change, std::move(text));
        }
    }

    /// @brief Submits a text field with @p text.
    /// @param widgetId The field.
    /// @param text The text submitted.
    void submit(int widgetId, std::string text) {
        if (detail::Record const* const record = actionable(widgetId); record != nullptr) {
            _store->setByUser(widgetId, "text", text);
            invoke(record->callbacks.submit, std::move(text));
        }
    }

    /// @brief Toggles a check box: it flips its own `checked`, then `onToggle` gets the new state.
    /// @param widgetId The check box.
    void toggle(int widgetId) {
        if (detail::Record const* const record = actionable(widgetId); record != nullptr) {
            bool const checked = prop(widgetId, "checked") != "true";
            _store->setByUser(widgetId, "checked", detail::formatBool(checked));
            invoke(record->callbacks.toggle, checked);
        }
    }

    /// @brief Chooses an option of a select.
    /// @param widgetId The select.
    /// @param key The option's key.
    void choose(int widgetId, Key key) {
        if (detail::Record const* const record = actionable(widgetId); record != nullptr) {
            _store->setByUser(widgetId, "selected", detail::formatKey(key));
            invoke(record->callbacks.select, std::move(key));
        }
    }

    /// @brief Chooses an entry of a menu, or a tab of a tab bar (which marks it selected).
    /// @param widgetId The menu or tab bar.
    /// @param index The entry or tab.
    void chooseIndex(int widgetId, std::size_t index) {
        if (detail::Record const* const record = actionable(widgetId); record != nullptr) {
            if (record->kind == "Tabs") {
                _store->setByUser(widgetId, "selected", std::to_string(index));
            }
            invoke(record->callbacks.index, index);
        }
    }

    /// @brief Collapses or expands a collapsible panel.
    /// @param widgetId The panel.
    /// @param collapsed The requested state.
    void collapse(int widgetId, bool collapsed) {
        if (detail::Record const* const record = actionable(widgetId); record != nullptr) {
            _store->setByUser(widgetId, "collapsed", detail::formatBool(collapsed));
            invoke(record->callbacks.toggle, collapsed);
        }
    }

    /// @brief Enters a date-time, or clears the field with `nullopt`.
    /// @param widgetId The field.
    /// @param value The instant entered.
    void setDateTime(int widgetId, std::optional<morph::time::Timestamp> value) {
        if (detail::Record const* const record = actionable(widgetId); record != nullptr) {
            _store->setByUser(widgetId, "value", detail::formatTimestamp(value));
            invoke(record->callbacks.dateTime, value);
        }
    }

    /// @brief Moves a slider.
    /// @param widgetId The slider.
    /// @param value The value moved to.
    void slide(int widgetId, std::int64_t value) {
        if (detail::Record const* const record = actionable(widgetId); record != nullptr) {
            _store->setByUser(widgetId, "value", std::to_string(value));
            invoke(record->callbacks.slide, value);
        }
    }

    /// @brief Picks a path in a file picker.
    /// @param widgetId The picker.
    /// @param path The path picked.
    void pick(int widgetId, std::string path) {
        if (detail::Record const* const record = actionable(widgetId); record != nullptr) {
            _store->setByUser(widgetId, "path", path);
            invoke(record->callbacks.picked, std::move(path));
        }
    }

    /// @brief Drags one widget onto another.
    ///
    /// An empty `accepts` refuses every key: the mount always passes a predicate, so a drop that lands without
    /// one shows the mount's accept-everything default at work rather than this backend's leniency.
    /// @param sourceId The dragged widget; it must have a drag key.
    /// @param targetId The widget dropped on; it must be a drop target whose `accepts` holds for the key.
    /// @return True when the drop was accepted and `onDrop` ran.
    bool drag(int sourceId, int targetId) {
        detail::Record const* const source = actionable(sourceId);
        detail::Record const* const target = actionable(targetId);
        if (source == nullptr || target == nullptr) {
            return false;
        }
        std::optional<Key> const key = source->dragKey;
        auto const accepts = target->callbacks.accepts;
        auto const drop = target->callbacks.drop;
        if (!key.has_value() || !drop || !accepts || !accepts(*key)) {
            return false;
        }
        drop(*key);
        return true;
    }

private:
    [[nodiscard]] detail::Record* actionable(int widgetId) {
        detail::Record& record = _store->at(widgetId);
        auto const isFalse = [&record](std::string_view name) {
            auto const found = record.props.find(name);
            return found != record.props.end() && found->second == "false";
        };
        return isFalse("visible") || isFalse("enabled") ? nullptr : &record;
    }

    // The copy keeps the handler alive while it runs, even when it destroys the widget it was stored on.
    template <typename... Args, typename... Values>
    static void invoke(std::function<void(Args...)> const& handler, Values&&... values) {
        auto const copy = handler;
        if (copy) {
            copy(std::forward<Values>(values)...);
        }
    }

    std::shared_ptr<detail::RecordStore> _store = std::make_shared<detail::RecordStore>();
};

}  // namespace morph::ui::testing

// NOLINTEND(bugprone-easily-swappable-parameters)
```

Register both headers: in `CMakeLists.txt`'s `FILE_SET HEADERS` block, after `include/morph/ui/view.hpp`:

```cmake
        include/morph/ui/backend.hpp
        include/morph/ui/testing/recording_backend.hpp
```

- [ ] **Step 5: Run the tests to verify they pass**

Run: `cmake --build build/reactive --target morph_tests && ./build/reactive/tests/morph_tests "[ui]"`
Expected: PASS — the seven Task 1 cases plus the ten here.

Mutation checks, one at a time, each restored afterwards:

| Change | Test that must fail |
|---|---|
| In `FakeTextInput::setText`, add `if (callbacks().change) { callbacks().change(std::string{text}); }` | "setText never calls onChange" |
| In `RecordingBackend::actionable`, return `&record` unconditionally | "a hidden or disabled widget ignores the helpers" |
| In `RecordingBackend::invoke`, call `handler(...)` instead of `copy(...)` | "a helper survives a handler that destroys its widget" (ASan reports a use after free; without ASan, run the case under the `clang-asan` preset) |

- [ ] **Step 6: Commit**

```bash
git add include/morph/ui/backend.hpp include/morph/ui/testing/recording_backend.hpp \
        tests/test_ui_recording_backend.cpp tests/CMakeLists.txt CMakeLists.txt
git commit -m "wip(ui): the backend contract and RecordingBackend

Signed-off-by: Christian Parpart <christian@parpart.family>"
```

---

### Task 3: `Mounted` — leaves, containers, `Common`, bindings and callbacks

**Files:**
- Create: `include/morph/ui/mount.hpp`
- Modify: `CMakeLists.txt` — `FILE_SET HEADERS`, after `include/morph/ui/backend.hpp`: `include/morph/ui/mount.hpp`
- Modify: `tests/CMakeLists.txt` — add `test_ui_mount.cpp` after `test_ui_recording_backend.cpp`
- Test: `tests/test_ui_mount.cpp`

**Interfaces:**
- Consumes: `reactive::Runtime` (`batch`, `widgetEvent`, `untracked`, `core()`), `reactive::Scope` (`make`,
  `adopt`, `effect`), `reactive::Computed<T>`, `reactive::Signal<T>`, `reactive::detail::site::kFlushInWidgetEvent`
  (Part 1); Tasks 1–2.
- Produces:
  - `ui::Mounted(reactive::Runtime&, IViewBackend&, Node root, ContainerWidget* parent = nullptr)`,
    `root() -> Widget&`; throws `std::invalid_argument` for a null root.
  - `ui::detail::Mounter(reactive::Runtime&, IViewBackend&)`, `mount(reactive::Scope&, Node const&,
    ContainerWidget*) -> Widget*` — Tasks 4–6 add `mountKind` overloads to it.
  - Mount order per node, which the logs below pin: create the widget (adopted into the scope first); `Common`
    (`visible` and `enabled` only when bound or `false`, `layout` only when not content-sized, `dragKey` only when
    bound or engaged, the drop handler only when `onDrop` is set); the kind's props in field order (every
    kind-specific prop is set once even when it holds its default; a Grid child's span only when not 1; a Panel's
    `collapsed` and `onToggle` only when `collapsible`); then the children.

- [ ] **Step 1: Write the failing test**

Create `tests/test_ui_mount.cpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0

#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstdint>
#include <memory>
#include <morph/reactive/runtime.hpp>
#include <morph/reactive/signal.hpp>
#include <morph/ui/mount.hpp>
#include <morph/ui/testing/recording_backend.hpp>
#include <morph/ui/view.hpp>
#include <morph/util/datetime.hpp>
#include <optional>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <variant>
#include <vector>

#include "owner_probe_recorder.hpp"
#include "test_support.hpp"

namespace ui = morph::ui;

namespace {

using morph::reactive::Runtime;
using morph::reactive::Signal;
using ui::testing::RecordingBackend;
using Owner = morph::testing::StepExecutor;
using Probe = morph::testing::OwnerProbeRecorder;
using Lines = std::vector<std::string>;

}  // namespace

static_assert(!std::is_copy_constructible_v<ui::Mounted> && !std::is_move_constructible_v<ui::Mounted>);

TEST_CASE("ui::Mounted: a constant tree is built once and makes no reactive node", "[ui]") {
    Owner owner;
    Runtime runtime{owner};
    RecordingBackend backend;
    ui::Mounted const view{
        runtime, backend,
        ui::column({
            .children =
                {
                    ui::text({.text = "Title", .role = ui::TextRole::Heading}),
                    ui::row({.children = {ui::button({.label = "Go"}),
                                          ui::spacer({.common = {.layout = {.width = ui::Sizing::stretch()}}})},
                             .gap = 2}),
                    ui::textInput({.value = "abc", .placeholder = "name", .mode = ui::TextInputMode::Password}),
                    ui::checkbox({.label = "Agree", .checked = true}),
                    ui::select({.options = std::vector<ui::SelectOption>{{.key = ui::Key{std::int64_t{1}},
                                                                          .label = "One"},
                                                                         {.key = ui::Key{std::string{"b"}},
                                                                          .label = "Bee"}},
                                .selected = std::optional<ui::Key>{ui::Key{std::string{"b"}}},
                                .style = ui::SelectStyle::Radio}),
                    ui::menu({.items = {{.label = "Open"}, {.label = "Quit"}}}),
                    ui::grid({.columns = 2, .cells = {{.node = ui::text({.text = "wide"}), .span = 2}}, .gap = 1}),
                    ui::panel({.title = "Box",
                               .padding = 1,
                               .child = ui::busy({.active = true, .label = "wait"}),
                               .collapsible = true,
                               .collapsed = false}),
                    ui::scroll({.child = ui::slider({.value = 5, .minimum = 0, .maximum = 10, .step = 1}),
                                .axis = ui::Axis::Horizontal}),
                    ui::dateTimeInput({.value = std::nullopt, .mode = ui::DateMode::Date, .offsetMinutes = 60}),
                    ui::filePicker({.path = "/tmp/x", .mode = ui::FilePickerMode::Save}),
                },
            .gap = 1,
        })};
    CHECK(backend.dump() == "Column#1 gap=1\n"
                            "  Text#2 role=Heading text=Title\n"
                            "  Row#3 gap=2\n"
                            "    Button#4 label=Go\n"
                            "    Spacer#5 layout=stretch(1)/content\n"
                            "  TextInput#6 mode=Password placeholder=name text=abc\n"
                            "  Checkbox#7 checked=true label=Agree\n"
                            "  Select#8 options=[1:One,\"b\":Bee] selected=\"b\" style=Radio\n"
                            "  Menu#9 items=[Open,Quit]\n"
                            "  Grid#10 columns=2 gap=1\n"
                            "    Text#11 role=Normal span=2 text=wide\n"
                            "  Panel#12 collapsed=false collapsible=true padding=1 title=Box\n"
                            "    Busy#13 active=true label=wait\n"
                            "  Scroll#14 axis=Horizontal\n"
                            "    Slider#15 range=0..10/1 value=5\n"
                            "  DateTimeInput#16 mode=Date offset=60 value=none\n"
                            "  FilePicker#17 mode=Save path=/tmp/x\n");
    CHECK(runtime.core()->liveNodes() == 0);
    CHECK(owner.pending() == 0);
}

TEST_CASE("ui::Mounted: a binding calls its setter once per real change", "[ui]") {
    Owner owner;
    Runtime runtime{owner};
    RecordingBackend backend;
    Signal<std::string> name{runtime, "ada"};
    Signal<int> count{runtime, 1};
    ui::Mounted const greeting{runtime, backend, ui::text({.text = [&] { return "Hi " + name.get(); }})};
    ui::Mounted const parity{runtime, backend,
                             ui::text({.text = [&] { return count.get() % 2 == 0 ? "even" : "odd"; }})};
    backend.clearLog();

    name.set("bob");
    owner.runAll();
    CHECK(backend.log() == Lines{"set Text#1 text=Hi bob"});

    backend.clearLog();
    name.set("bob");
    CHECK(owner.pending() == 0);

    count.set(3);  // the binding re-runs and yields "odd" again: no setter call
    owner.runAll();
    CHECK(backend.log().empty());
}

TEST_CASE("ui::Mounted: visible and enabled follow their props", "[ui]") {
    Owner owner;
    Runtime runtime{owner};
    RecordingBackend backend;
    Signal<bool> shown{runtime, true};
    ui::Mounted const view{
        runtime, backend,
        ui::column({.children = {
                        ui::button({.label = "Bound",
                                    .common = {.visible = [&] { return shown.get(); },
                                               .enabled = [&] { return shown.get(); }}}),
                        ui::button({.label = "Hidden", .common = {.visible = false}}),
                        ui::button({.label = "Plain"}),
                    }})};
    CHECK(backend.dump() == "Column#1 gap=0\n"
                            "  Button#2 enabled=true label=Bound visible=true\n"
                            "  Button#3 label=Hidden visible=false\n"
                            "  Button#4 label=Plain\n");
    backend.clearLog();
    shown.set(false);
    owner.runAll();
    CHECK(backend.log() == Lines{"set Button#2 visible=false", "set Button#2 enabled=false"});
}

TEST_CASE("ui::Mounted: a pending flush does not run inside a click handler", "[ui]") {
    Owner owner;
    Probe const probe{owner.coreExecutor()};
    Runtime runtime{owner};
    RecordingBackend backend;
    Signal<int> clicks{runtime, 0};
    std::string labelInHandler;
    ui::Mounted const view{runtime, backend,
                           ui::button({.label = [&] { return "clicked " + std::to_string(clicks.get()); },
                                       .onClick = [&] {
                                           CHECK(owner.runOne());  // a nested event loop pumps the owner
                                           labelInHandler = backend.prop(1, "label");
                                       }})};
    clicks.set(1);  // posts a flush that would update the label
    backend.click(1);
    CHECK(labelInHandler == "clicked 0");
    CHECK(probe.count(morph::reactive::detail::site::kFlushInWidgetEvent) == 1);
    owner.runAll();
    CHECK(backend.prop(1, "label") == "clicked 1");
}

TEST_CASE("ui::Mounted: typing reaches onChange, and a programmatic value is not echoed", "[ui]") {
    Owner owner;
    Runtime runtime{owner};
    RecordingBackend backend;
    Signal<std::string> name{runtime, "a"};
    Lines changes;
    Lines submits;
    ui::Mounted const view{runtime, backend,
                           ui::textInput({.value = [&] { return name.get(); },
                                          .onChange =
                                              [&](std::string text) {
                                                  changes.push_back(text);
                                                  name.set(std::move(text));
                                              },
                                          .onSubmit = [&](std::string text) { submits.push_back(std::move(text)); },
                                          .placeholder = "name"})};
    backend.edit(1, "ab");
    owner.runAll();
    CHECK(changes == Lines{"ab"});
    CHECK(backend.prop(1, "text") == "ab");

    name.set("xyz");
    owner.runAll();
    CHECK(backend.prop(1, "text") == "xyz");
    CHECK(changes == Lines{"ab"});

    backend.submit(1, "xyz");
    CHECK(submits == Lines{"xyz"});
}

TEST_CASE("ui::Mounted: a menu entry runs its own action and its label may be bound", "[ui]") {
    Owner owner;
    Runtime runtime{owner};
    RecordingBackend backend;
    Signal<int> count{runtime, 1};
    Lines ran;
    ui::Mounted const view{runtime, backend,
                           ui::menu({.items = {{.label = [&] { return "Count " + std::to_string(count.get()); },
                                                .onSelect = [&] { ran.emplace_back("count"); }},
                                               {.label = "Quit", .onSelect = [&] { ran.emplace_back("quit"); }}}})};
    CHECK(backend.prop(1, "items") == "[Count 1,Quit]");
    backend.chooseIndex(1, 1);
    backend.chooseIndex(1, 7);  // out of range: nothing runs
    CHECK(ran == Lines{"quit"});
    backend.clearLog();
    count.set(2);
    owner.runAll();
    CHECK(backend.log() == Lines{"set Menu#1 items=[Count 2,Quit]"});
}

TEST_CASE("ui::Mounted: a collapsible panel reports a toggle and follows its collapsed prop", "[ui]") {
    Owner owner;
    Runtime runtime{owner};
    RecordingBackend backend;
    Signal<bool> collapsed{runtime, false};
    ui::Mounted const view{
        runtime, backend,
        ui::column({.children = {
                        ui::panel({.title = "Advanced",
                                   .child = ui::text({.text = "inside"}),
                                   .collapsible = true,
                                   .collapsed = [&] { return collapsed.get(); },
                                   .onToggle = [&](bool value) { collapsed.set(value); }}),
                        ui::panel({.title = "Fixed", .collapsed = true}),
                    }})};
    CHECK(backend.prop(4, "collapsed").empty());  // not collapsible: collapsed is never set
    backend.collapse(2, true);
    owner.runAll();
    CHECK(collapsed.peek());
    CHECK(backend.prop(2, "collapsed") == "true");
}

TEST_CASE("ui::Mounted: date-time, slider and file picker report what the user entered", "[ui]") {
    Owner owner;
    Runtime runtime{owner};
    RecordingBackend backend;
    using morph::time::Timestamp;
    Signal<std::optional<Timestamp>> due{runtime, std::nullopt};
    Signal<std::int64_t> volume{runtime, 3};
    Signal<std::string> path{runtime, ""};
    ui::Mounted const view{
        runtime, backend,
        ui::column({.children = {
                        ui::dateTimeInput({.value = [&] { return due.get(); },
                                           .onChange = [&](std::optional<Timestamp> value) { due.set(value); }}),
                        ui::slider({.value = [&] { return volume.get(); },
                                    .minimum = 0,
                                    .maximum = 10,
                                    .step = 1,
                                    .onChange = [&](std::int64_t value) { volume.set(value); }}),
                        ui::filePicker({.path = [&] { return path.get(); },
                                        .onPicked = [&](std::string value) { path.set(std::move(value)); }}),
                    }})};
    Timestamp const when{morph::time::DateTime{std::chrono::year{2026}, std::chrono::month{10}, std::chrono::day{4},
                                               std::chrono::hours{9}, std::chrono::minutes{30},
                                               std::chrono::seconds{0}}};
    backend.setDateTime(2, when);
    backend.slide(3, 7);
    backend.pick(4, "/tmp/report.csv");
    owner.runAll();
    CHECK(due.peek() == std::optional<Timestamp>{when});
    CHECK(volume.peek() == 7);
    CHECK(path.peek() == "/tmp/report.csv");
    CHECK(backend.dump() == "Column#1 gap=0\n"
                            "  DateTimeInput#2 mode=DateTime offset=0 value=2026-10-04T09:30:00.000Z\n"
                            "  Slider#3 range=0..10/1 value=7\n"
                            "  FilePicker#4 mode=Open path=/tmp/report.csv\n");
}

TEST_CASE("ui::Mounted: a drag key reaches a drop target that accepts it", "[ui]") {
    Owner owner;
    Runtime runtime{owner};
    RecordingBackend backend;
    std::vector<ui::Key> dropped;
    auto const keep = [&dropped](ui::Key key) { dropped.push_back(std::move(key)); };
    ui::Mounted const view{
        runtime, backend,
        ui::column({.children = {
                        ui::text({.text = "card",
                                  .common = {.dragKey = std::optional<ui::Key>{ui::Key{std::int64_t{7}}}}}),
                        ui::panel({.title = "Numbers",
                                   .common = {.accepts = [](ui::Key const& key) {
                                                  return std::holds_alternative<std::int64_t>(key);
                                              },
                                              .onDrop = keep}}),
                        ui::panel({.title = "Words",
                                   .common = {.accepts = [](ui::Key const& key) {
                                                  return std::holds_alternative<std::string>(key);
                                              },
                                              .onDrop = keep}}),
                        ui::panel({.title = "Anything", .common = {.onDrop = keep}}),
                        ui::text({.text = "not a target"}),
                    }})};
    CHECK(backend.prop(2, "dragKey") == "7");
    CHECK(backend.drag(2, 3));
    CHECK_FALSE(backend.drag(2, 4));
    CHECK(backend.drag(2, 5));  // no accepts: every key is accepted
    CHECK_FALSE(backend.drag(2, 6));
    CHECK_FALSE(backend.drag(6, 3));
    CHECK(dropped == std::vector<ui::Key>{ui::Key{std::int64_t{7}}, ui::Key{std::int64_t{7}}});
}

// Review Focus 5.
TEST_CASE("ui::Mounted: unmounting destroys children first and leaves no binding", "[ui]") {
    Owner owner;
    Runtime runtime{owner};
    RecordingBackend backend;
    Signal<std::string> label{runtime, "x"};
    auto view = std::make_unique<ui::Mounted>(
        runtime, backend,
        ui::column({.children = {ui::text({.text = [&] { return label.get(); }}),
                                 ui::row({.children = {ui::button({.label = "Go"})}})}}));
    CHECK(runtime.core()->liveNodes() == 2);  // the text binding: one Computed, one Effect
    backend.clearLog();
    view.reset();
    CHECK(backend.log() == Lines{"destroy Button#4", "destroy Row#3", "destroy Text#2", "destroy Column#1"});
    CHECK(runtime.core()->liveNodes() == 0);
    label.set("y");
    CHECK(owner.pending() == 0);
}

TEST_CASE("ui::Mounted: mounts into an existing container, and refuses a null root", "[ui]") {
    Owner owner;
    Runtime runtime{owner};
    RecordingBackend backend;
    auto host = backend.createStack(nullptr, ui::Axis::Vertical);
    {
        ui::Mounted const view{runtime, backend, ui::text({.text = "inside"}), host.get()};
        CHECK(backend.dump() == "Column#1\n  Text#2 role=Normal text=inside\n");
        CHECK(backend.idOf(view.root()) == 2);
    }
    CHECK(backend.dump() == "Column#1\n");
    CHECK_THROWS_AS((ui::Mounted{runtime, backend, ui::Node{}}), std::invalid_argument);
}
```

Register it after `test_ui_recording_backend.cpp`.

- [ ] **Step 2: Run the test to verify it fails**

Run: `cmake --build build/reactive --target morph_tests`
Expected: FAIL — `fatal error: 'morph/ui/mount.hpp' file not found`.

- [ ] **Step 3: Implement the mount**

Create `include/morph/ui/mount.hpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0

#pragma once
#include <algorithm>
#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "../attributes.hpp"
#include "../reactive/runtime.hpp"
#include "../reactive/scope.hpp"
#include "../reactive/signal.hpp"
#include "backend.hpp"
#include "view.hpp"

/// @file
/// @brief `morph::ui::Mounted`: builds a view tree's widgets through a backend once, and keeps them current through
///        reactive bindings.
///
/// Specified in `docs/spec/ui/view_tree.md`, "Mount".

namespace morph::ui {

namespace detail {

// A view tree is mounted by descending it: a container's mount mounts its children.
// NOLINTBEGIN(misc-no-recursion)

/// @brief Builds widgets for nodes and wires their props and callbacks.
///
/// One per `Mounted`. It must outlive every scope it mounted into: a Switch, Tabs, Dialog or ForEach binding calls
/// back into it to mount content later.
class Mounter {
public:
    /// @param runtime The runtime bindings are made in. Borrowed.
    /// @param backend Makes the widgets. Borrowed.
    Mounter(reactive::Runtime& runtime MORPH_LIFETIMEBOUND, IViewBackend& backend MORPH_LIFETIMEBOUND) noexcept
        : _rt{&runtime}, _backend{&backend} {}

    /// @brief Mounts @p node under @p parent; @p scope owns its widgets and bindings.
    ///
    /// A node's widget is adopted first, then its bindings, then its children, so destroying the scope destroys
    /// bindings before their widget and children before their parent.
    /// @param scope Owns everything mounted.
    /// @param node The node; null mounts nothing.
    /// @param parent The container to append to, or null for a root.
    /// @return The node's widget, or null for a null node.
    Widget* mount(reactive::Scope& scope, Node const& node, ContainerWidget* parent) {
        if (!node) {
            return nullptr;
        }
        return std::visit([&](auto const& spec) -> Widget* { return &mountKind(scope, spec, parent); }, node->kind);
    }

private:
    // A constant is applied once; a binding becomes an equality-gated Computed plus an Effect, made in that order
    // after the widget, so the Effect dies first.
    template <typename T, typename Apply>
    void bind(reactive::Scope& scope, Prop<T> const& prop, Apply apply) {
        if (!prop.isBound()) {
            apply(prop.constant());
            return;
        }
        auto& value = scope.make<reactive::Computed<T>>(*_rt, prop.binding());
        scope.effect([&value, apply = std::move(apply)] { apply(value.get()); });
    }

    // Every widget callback runs inside widgetEvent: one batch, and a flush that would start meanwhile is re-posted,
    // so a remount never destroys a widget whose native handler is on the stack.
    template <typename... Args>
    [[nodiscard]] std::function<void(Args...)> event(std::function<void(Args...)> const& handler) const {
        if (!handler) {
            return {};
        }
        return [runtime = _rt, handler](Args... args) {
            runtime->widgetEvent([&] { handler(std::move(args)...); });
        };
    }

    void applyCommon(reactive::Scope& scope, Widget& widget, Common const& common) {
        if (common.visible.isBound() || !common.visible.constant()) {
            bind(scope, common.visible, [&widget](bool shown) { widget.setVisible(shown); });
        }
        if (common.enabled.isBound() || !common.enabled.constant()) {
            bind(scope, common.enabled, [&widget](bool enabled) { widget.setEnabled(enabled); });
        }
        if (common.layout != LayoutHints{}) {
            widget.setLayout(common.layout);
        }
        if (common.dragKey.isBound() || common.dragKey.constant().has_value()) {
            bind(scope, common.dragKey, [&widget](std::optional<Key> const& key) { widget.setDragKey(key); });
        }
        if (common.onDrop) {
            std::function<bool(Key const&)> accepts = common.accepts;
            if (!accepts) {
                accepts = [](Key const&) { return true; };
            }
            widget.setDropHandler(std::move(accepts), event(common.onDrop));
        }
    }

    void mountChildren(reactive::Scope& scope, std::vector<Node> const& children, ContainerWidget& container) {
        for (Node const& child : children) {
            static_cast<void>(mount(scope, child, &container));
        }
    }

    Widget& mountStack(reactive::Scope& scope, Axis axis, std::vector<Node> const& children, int gap,
                       Common const& common, ContainerWidget* parent) {
        StackWidget& widget = scope.adopt(_backend->createStack(parent, axis));
        applyCommon(scope, widget, common);
        widget.setGap(gap);
        mountChildren(scope, children, widget);
        return widget;
    }

    [[nodiscard]] static std::vector<std::string> labelsOf(std::vector<MenuItem> const& items) {
        std::vector<std::string> labels;
        labels.reserve(items.size());
        for (MenuItem const& item : items) {
            labels.push_back(item.label.evaluate());
        }
        return labels;
    }

    Widget& mountKind(reactive::Scope& scope, Text const& spec, ContainerWidget* parent) {
        TextWidget& widget = scope.adopt(_backend->createText(parent));
        applyCommon(scope, widget, spec.common);
        bind(scope, spec.text, [&widget](std::string const& text) { widget.setText(text); });
        bind(scope, spec.role, [&widget](TextRole role) { widget.setRole(role); });
        return widget;
    }

    Widget& mountKind(reactive::Scope& scope, Button const& spec, ContainerWidget* parent) {
        ButtonWidget& widget = scope.adopt(_backend->createButton(parent));
        applyCommon(scope, widget, spec.common);
        bind(scope, spec.label, [&widget](std::string const& label) { widget.setLabel(label); });
        widget.setOnClick(event(spec.onClick));
        return widget;
    }

    Widget& mountKind(reactive::Scope& scope, TextInput const& spec, ContainerWidget* parent) {
        TextInputWidget& widget = scope.adopt(_backend->createTextInput(parent, spec.mode));
        applyCommon(scope, widget, spec.common);
        bind(scope, spec.value, [&widget](std::string const& value) { widget.setText(value); });
        bind(scope, spec.placeholder, [&widget](std::string const& text) { widget.setPlaceholder(text); });
        widget.setOnChange(event(spec.onChange));
        widget.setOnSubmit(event(spec.onSubmit));
        return widget;
    }

    Widget& mountKind(reactive::Scope& scope, Checkbox const& spec, ContainerWidget* parent) {
        CheckboxWidget& widget = scope.adopt(_backend->createCheckbox(parent));
        applyCommon(scope, widget, spec.common);
        bind(scope, spec.label, [&widget](std::string const& label) { widget.setLabel(label); });
        bind(scope, spec.checked, [&widget](bool checked) { widget.setChecked(checked); });
        widget.setOnToggle(event(spec.onToggle));
        return widget;
    }

    Widget& mountKind(reactive::Scope& scope, Select const& spec, ContainerWidget* parent) {
        SelectWidget& widget = scope.adopt(_backend->createSelect(parent, spec.style));
        applyCommon(scope, widget, spec.common);
        bind(scope, spec.options,
             [&widget](std::vector<SelectOption> const& options) { widget.setOptions(options); });
        bind(scope, spec.selected, [&widget](std::optional<Key> const& key) { widget.setSelected(key); });
        widget.setOnSelect(event(spec.onSelect));
        return widget;
    }

    Widget& mountKind(reactive::Scope& scope, Menu const& spec, ContainerWidget* parent) {
        MenuWidget& widget = scope.adopt(_backend->createMenu(parent));
        applyCommon(scope, widget, spec.common);
        std::vector<MenuItem> const& items = spec.items;
        bool const anyBound = std::ranges::any_of(items, [](MenuItem const& item) { return item.label.isBound(); });
        // One prop for the whole list: a bound label re-sends every label, which keeps setItems the one setter.
        Prop<std::vector<std::string>> const labels =
            anyBound ? Prop<std::vector<std::string>>([items] { return labelsOf(items); })
                     : Prop<std::vector<std::string>>(labelsOf(items));
        bind(scope, labels, [&widget](std::vector<std::string> const& texts) { widget.setItems(texts); });
        std::vector<Action> actions;
        actions.reserve(items.size());
        for (MenuItem const& item : items) {
            actions.push_back(item.onSelect);
        }
        widget.setOnActivate(event(std::function<void(std::size_t)>{[actions = std::move(actions)](std::size_t index) {
            if (index < actions.size() && actions.at(index)) {
                actions.at(index)();
            }
        }}));
        return widget;
    }

    Widget& mountKind(reactive::Scope& scope, Column const& spec, ContainerWidget* parent) {
        return mountStack(scope, Axis::Vertical, spec.children, spec.gap, spec.common, parent);
    }

    Widget& mountKind(reactive::Scope& scope, Row const& spec, ContainerWidget* parent) {
        return mountStack(scope, Axis::Horizontal, spec.children, spec.gap, spec.common, parent);
    }

    Widget& mountKind(reactive::Scope& scope, Grid const& spec, ContainerWidget* parent) {
        GridWidget& widget = scope.adopt(_backend->createGrid(parent));
        applyCommon(scope, widget, spec.common);
        widget.setColumns(spec.columns);
        widget.setGap(spec.gap);
        for (GridCell const& cell : spec.cells) {
            Widget* const child = mount(scope, cell.node, &widget);
            if (child != nullptr && cell.span != 1) {
                widget.setSpan(*child, cell.span);
            }
        }
        return widget;
    }

    Widget& mountKind(reactive::Scope& scope, Spacer const& spec, ContainerWidget* parent) {
        SpacerWidget& widget = scope.adopt(_backend->createSpacer(parent));
        applyCommon(scope, widget, spec.common);
        return widget;
    }

    Widget& mountKind(reactive::Scope& scope, Panel const& spec, ContainerWidget* parent) {
        PanelWidget& widget = scope.adopt(_backend->createPanel(parent));
        applyCommon(scope, widget, spec.common);
        bind(scope, spec.title, [&widget](std::string const& title) { widget.setTitle(title); });
        widget.setPadding(spec.padding);
        widget.setCollapsible(spec.collapsible);
        if (spec.collapsible) {
            bind(scope, spec.collapsed, [&widget](bool collapsed) { widget.setCollapsed(collapsed); });
            widget.setOnToggle(event(spec.onToggle));
        }
        static_cast<void>(mount(scope, spec.child, &widget));
        return widget;
    }

    Widget& mountKind(reactive::Scope& scope, Scroll const& spec, ContainerWidget* parent) {
        ScrollWidget& widget = scope.adopt(_backend->createScroll(parent, spec.axis));
        applyCommon(scope, widget, spec.common);
        static_cast<void>(mount(scope, spec.child, &widget));
        return widget;
    }

    Widget& mountKind(reactive::Scope& scope, Busy const& spec, ContainerWidget* parent) {
        BusyWidget& widget = scope.adopt(_backend->createBusy(parent));
        applyCommon(scope, widget, spec.common);
        bind(scope, spec.active, [&widget](bool active) { widget.setActive(active); });
        bind(scope, spec.label, [&widget](std::string const& label) { widget.setLabel(label); });
        return widget;
    }

    Widget& mountKind(reactive::Scope& scope, DateTimeInput const& spec, ContainerWidget* parent) {
        DateTimeInputWidget& widget =
            scope.adopt(_backend->createDateTimeInput(parent, spec.mode, spec.offsetMinutes));
        applyCommon(scope, widget, spec.common);
        bind(scope, spec.value,
             [&widget](std::optional<morph::time::Timestamp> const& value) { widget.setValue(value); });
        widget.setOnChange(event(spec.onChange));
        return widget;
    }

    Widget& mountKind(reactive::Scope& scope, Slider const& spec, ContainerWidget* parent) {
        SliderWidget& widget = scope.adopt(_backend->createSlider(parent));
        applyCommon(scope, widget, spec.common);
        widget.setRange(spec.minimum, spec.maximum, spec.step);
        bind(scope, spec.value, [&widget](std::int64_t value) { widget.setValue(value); });
        widget.setOnChange(event(spec.onChange));
        return widget;
    }

    Widget& mountKind(reactive::Scope& scope, FilePicker const& spec, ContainerWidget* parent) {
        FilePickerWidget& widget = scope.adopt(_backend->createFilePicker(parent, spec.mode));
        applyCommon(scope, widget, spec.common);
        bind(scope, spec.path, [&widget](std::string const& path) { widget.setPath(path); });
        widget.setOnPicked(event(spec.onPicked));
        return widget;
    }

    reactive::Runtime* _rt;
    IViewBackend* _backend;
};

// NOLINTEND(misc-no-recursion)

}  // namespace detail

/// @brief A view tree mounted on a backend: its widgets, built once, and the bindings that keep them current.
///
/// Destroying it destroys every binding before the widget it drives and every child before its parent. The
/// runtime, the backend and every signal a binding reads must outlive it; the owner of the runtime constructs and
/// destroys it. Non-copyable and non-movable: bindings point into it.
class Mounted {
public:
    /// @param runtime The runtime the bindings are made in. Borrowed.
    /// @param backend Makes the widgets. Borrowed.
    /// @param root The tree; kept alive as long as the mount.
    /// @param parent The container the root widget is appended to, or null for a backend root.
    /// @throws std::invalid_argument when @p root is null.
    Mounted(reactive::Runtime& runtime MORPH_LIFETIMEBOUND, IViewBackend& backend MORPH_LIFETIMEBOUND, Node root,
            ContainerWidget* parent = nullptr)
        : _root{nonNull(std::move(root))},
          _mounter{runtime, backend},
          _scope{runtime},
          // Untracked, so mounting from inside an Effect does not subscribe that Effect to anything read here.
          _widget{runtime.untracked([this, parent] { return _mounter.mount(_scope, _root, parent); })} {}

    ~Mounted() = default;
    Mounted(Mounted const&) = delete;
    Mounted& operator=(Mounted const&) = delete;
    Mounted(Mounted&&) = delete;
    Mounted& operator=(Mounted&&) = delete;

    /// @brief The root node's widget.
    /// @return The widget; valid as long as this object.
    [[nodiscard]] Widget& root() const noexcept { return *_widget; }

private:
    [[nodiscard]] static Node nonNull(Node root) {
        if (!root) {
            throw std::invalid_argument{"morph::ui::Mounted: the root node is null"};
        }
        return root;
    }

    Node _root;
    detail::Mounter _mounter;
    reactive::Scope _scope;  // after _mounter: destroyed first, while the mounter its bindings call is alive
    Widget* _widget;
};

}  // namespace morph::ui
```

Register it: in `CMakeLists.txt`'s `FILE_SET HEADERS`, after `include/morph/ui/backend.hpp`:

```cmake
        include/morph/ui/mount.hpp
```

- [ ] **Step 4: Run the tests to verify they pass**

Run: `cmake --build build/reactive --target morph_tests && ./build/reactive/tests/morph_tests "[ui]"`
Expected: PASS.

Mutation checks, one at a time, each restored afterwards:

| Change | Test that must fail |
|---|---|
| In `Mounter::bind`, replace the Computed and the Effect with `scope.effect([binding = prop.binding(), apply = std::move(apply)] { apply(binding()); });` | "a binding calls its setter once per real change" (`set Text#2 text=odd` appears) |
| In `Mounter::event`, return `handler` unwrapped | "a pending flush does not run inside a click handler" |
| In Part 1's `reactive::Scope::clear`, destroy front-first (`_owned.erase(_owned.begin())`) | "unmounting destroys children first and leaves no binding" (`destroy Column#1` comes first) |
| In `Mounter::bind`, make the Computed with `new reactive::Computed<T>(*_rt, prop.binding())` instead of `scope.make` (leaked) | "unmounting destroys children first and leaves no binding" (`liveNodes()` stays 1) |
| In `applyCommon`, delete the `if (!accepts) { … }` default | "a drag key reaches a drop target that accepts it" (`drag(2, 5)`: `RecordingBackend` refuses every key for an empty predicate, so only the mount's default makes that drop land) |

- [ ] **Step 5: Commit**

```bash
git add include/morph/ui/mount.hpp tests/test_ui_mount.cpp tests/CMakeLists.txt CMakeLists.txt
git commit -m "wip(ui): Mounted -- leaves, containers, Common, bindings and callbacks

Signed-off-by: Christian Parpart <christian@parpart.family>"
```

---

### Task 4: Structure — Switch, `switchOn`, Tabs and Dialog

Each of the three mounts its content into a child `reactive::Scope`, so swapping content tears the old content down
child-first before the new content is built.

**Files:**
- Modify: `include/morph/ui/view.hpp` — the node structs, the builders, `switchOn`, and `NodeData`'s variant
- Modify: `include/morph/ui/backend.hpp` — `SlotWidget`, `TabsWidget`, `DialogWidget` and their factories
- Modify: `include/morph/ui/testing/recording_backend.hpp` — `Callbacks::dismiss`, `FakeTabs`, `FakeDialog`, the
  three factories, `dismiss()`
- Modify: `include/morph/ui/mount.hpp` — `detail::Pages`, three `mountKind` overloads, `showPage`
- Modify: `tests/CMakeLists.txt` — add `test_ui_structure.cpp` after `test_ui_mount.cpp`
- Test: `tests/test_ui_structure.cpp`

**Interfaces:**
- Consumes: Tasks 1–3; `reactive::detail::site::kFlushInWidgetEvent`.
- Produces: `ui::SwitchCase`, `ui::Switch`, `ui::Tab`, `ui::Tabs`, `ui::Dialog`; `switchOf(Switch)`,
  `tabs(Tabs)`, `dialog(Dialog)`, `switchOn<E>(std::function<E()>, std::vector<std::pair<E, Node>>, Node = {})`;
  `ui::SlotWidget`, `ui::TabsWidget`, `ui::DialogWidget`; `IViewBackend::createSlot`, `createTabs`, `createDialog`;
  `RecordingBackend::dismiss(int)`. A Switch is a `Slot` holding its current case; a Tabs holds one `Slot` per
  page shown so far, in the order they were first shown; a Dialog holds its content only while open.

- [ ] **Step 1: Write the failing test**

Create `tests/test_ui_structure.cpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0

#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <morph/reactive/runtime.hpp>
#include <morph/reactive/signal.hpp>
#include <morph/ui/mount.hpp>
#include <morph/ui/testing/recording_backend.hpp>
#include <morph/ui/view.hpp>
#include <string>
#include <vector>

#include "owner_probe_recorder.hpp"
#include "test_support.hpp"

namespace ui = morph::ui;

namespace {

using morph::reactive::Runtime;
using morph::reactive::Signal;
using ui::testing::RecordingBackend;
using Owner = morph::testing::StepExecutor;
using Probe = morph::testing::OwnerProbeRecorder;
using Lines = std::vector<std::string>;

enum class Page : std::uint8_t { Home, Settings };

ui::Key intKey(std::int64_t value) { return ui::Key{value}; }

}  // namespace

TEST_CASE("ui::switchOf: mounts the selected case and remounts child-first", "[ui]") {
    Owner owner;
    Runtime runtime{owner};
    RecordingBackend backend;
    Signal<std::int64_t> which{runtime, 0};
    ui::Mounted const view{
        runtime, backend,
        ui::switchOf({
            .selector = [&] { return ui::Key{which.get()}; },
            .cases = {{.key = intKey(0), .node = ui::column({.children = {ui::text({.text = "zero"})}})},
                      {.key = intKey(1), .node = ui::column({.children = {ui::text({.text = "one"})}})}},
        })};
    CHECK(backend.dump() == "Slot#1\n  Column#2 gap=0\n    Text#3 role=Normal text=zero\n");
    backend.clearLog();
    which.set(1);
    owner.runAll();
    CHECK(backend.log() == Lines{"destroy Text#3", "destroy Column#2", "create Column#4 in Slot#1",
                                 "set Column#4 gap=0", "create Text#5 in Column#4", "set Text#5 text=one",
                                 "set Text#5 role=Normal"});
}

TEST_CASE("ui::switchOf: an unchanged key never remounts", "[ui]") {
    Owner owner;
    Runtime runtime{owner};
    RecordingBackend backend;
    Signal<std::int64_t> count{runtime, 1};
    ui::Mounted const view{
        runtime, backend,
        ui::switchOf({
            .selector = [&] { return intKey(count.get() > 0 ? 1 : 0); },
            .cases = {{.key = intKey(0), .node = ui::text({.text = "none"})},
                      {.key = intKey(1), .node = ui::text({.text = "some"})}},
        })};
    backend.clearLog();
    count.set(5);
    owner.runAll();
    CHECK(backend.log().empty());
}

// Review Focus 2.
TEST_CASE("ui::switchOf: a key with no case and no fallback mounts nothing", "[ui]") {
    Owner owner;
    Runtime runtime{owner};
    RecordingBackend backend;
    Signal<std::int64_t> which{runtime, 9};
    ui::Mounted const view{runtime, backend,
                           ui::switchOf({.selector = [&] { return ui::Key{which.get()}; },
                                         .cases = {{.key = intKey(0), .node = ui::text({.text = "zero"})}}})};
    CHECK(backend.dump() == "Slot#1\n");
    which.set(0);
    owner.runAll();
    CHECK(backend.dump() == "Slot#1\n  Text#2 role=Normal text=zero\n");
    which.set(9);
    owner.runAll();
    CHECK(backend.dump() == "Slot#1\n");
}

TEST_CASE("ui::switchOf: the fallback answers a key with no case", "[ui]") {
    Owner owner;
    Runtime runtime{owner};
    RecordingBackend backend;
    ui::Mounted const view{runtime, backend,
                           ui::switchOf({.selector = ui::Key{std::string{"missing"}},
                                         .cases = {{.key = intKey(0), .node = ui::text({.text = "zero"})}},
                                         .fallback = ui::text({.text = "other"})})};
    CHECK(backend.dump() == "Slot#1\n  Text#2 role=Normal text=other\n");
}

TEST_CASE("ui::switchOn: each enumerator selects its case", "[ui]") {
    Owner owner;
    Runtime runtime{owner};
    RecordingBackend backend;
    Signal<Page> page{runtime, Page::Home};
    ui::Mounted const view{runtime, backend,
                           ui::switchOn<Page>([&] { return page.get(); },
                                              {{Page::Home, ui::text({.text = "home"})},
                                               {Page::Settings, ui::text({.text = "settings"})}})};
    CHECK(backend.dump() == "Slot#1\n  Text#2 role=Normal text=home\n");
    page.set(Page::Settings);
    owner.runAll();
    CHECK(backend.dump() == "Slot#1\n  Text#3 role=Normal text=settings\n");
}

TEST_CASE("ui::tabs: pages mount on first selection and are kept, hidden, afterwards", "[ui]") {
    Owner owner;
    Runtime runtime{owner};
    RecordingBackend backend;
    Signal<std::size_t> selected{runtime, 0};
    ui::Mounted const view{runtime, backend,
                           ui::tabs({
                               .tabs = {{.label = "Dashboard", .node = ui::text({.text = "dash"})},
                                        {.label = "Stats", .node = ui::text({.text = "stats"})}},
                               .selected = [&] { return selected.get(); },
                               .onSelect = [&](std::size_t index) { selected.set(index); },
                           })};
    CHECK(backend.dump() == "Tabs#1 selected=0 tabs=[Dashboard,Stats]\n  Slot#2\n    Text#3 role=Normal text=dash\n");

    backend.clearLog();
    backend.chooseIndex(1, 1);
    owner.runAll();
    CHECK(backend.log() == Lines{"set Slot#2 visible=false", "create Slot#4 in Tabs#1", "create Text#5 in Slot#4",
                                 "set Text#5 text=stats", "set Text#5 role=Normal", "set Tabs#1 selected=1"});

    backend.clearLog();
    backend.chooseIndex(1, 0);
    owner.runAll();
    CHECK(backend.log() == Lines{"set Slot#4 visible=false", "set Slot#2 visible=true", "set Tabs#1 selected=0"});
}

TEST_CASE("ui::dialog: the content exists only while open, and dismiss reaches onDismiss", "[ui]") {
    Owner owner;
    Runtime runtime{owner};
    RecordingBackend backend;
    Signal<bool> open{runtime, false};
    ui::Mounted const view{runtime, backend,
                           ui::dialog({.open = [&] { return open.get(); },
                                       .title = "Confirm",
                                       .child = ui::column({.children = {ui::text({.text = "Sure?"})}}),
                                       .onDismiss = [&] { open.set(false); }})};
    CHECK(backend.dump() == "Dialog#1 open=false title=Confirm\n");

    backend.clearLog();
    open.set(true);
    owner.runAll();
    CHECK(backend.log() == Lines{"create Column#2 in Dialog#1", "set Column#2 gap=0", "create Text#3 in Column#2",
                                 "set Text#3 text=Sure?", "set Text#3 role=Normal", "set Dialog#1 open=true"});

    backend.clearLog();
    backend.dismiss(1);
    owner.runAll();
    CHECK(backend.log() == Lines{"destroy Text#3", "destroy Column#2", "set Dialog#1 open=false"});
}

// Review Focus 3.
TEST_CASE("ui::dialog: a Dialog's button survives a nested loop in its own handler", "[ui]") {
    Owner owner;
    Probe const probe{owner.coreExecutor()};
    Runtime runtime{owner};
    RecordingBackend backend;
    Signal<bool> open{runtime, true};
    bool aliveInHandler = false;
    ui::Mounted const view{runtime, backend,
                           ui::dialog({.open = [&] { return open.get(); },
                                       .title = "Confirm",
                                       .child = ui::button({.label = "Close", .onClick = [&] {
                                                                CHECK(owner.runOne());  // a modal loop pumps the owner
                                                                aliveInHandler = backend.exists(2);
                                                            }})})};
    REQUIRE(backend.kindOf(2) == "Button");
    open.set(false);  // a write from elsewhere: its posted flush would unmount the button
    backend.click(2);
    CHECK(aliveInHandler);
    CHECK(probe.count(morph::reactive::detail::site::kFlushInWidgetEvent) == 1);
    owner.runAll();
    CHECK_FALSE(backend.exists(2));
    CHECK(backend.prop(1, "open") == "false");
}
```

Register it after `test_ui_mount.cpp`.

- [ ] **Step 2: Run the test to verify it fails**

Run: `cmake --build build/reactive --target morph_tests`
Expected: FAIL — `no member named 'switchOf' in namespace 'morph::ui'`.

- [ ] **Step 3: Add the nodes**

In `include/morph/ui/view.hpp`, insert after `struct Scroll { … };`:

```cpp
/// @brief One case of a `Switch`.
struct SwitchCase {
    /// @brief The selector value this case answers.
    Key key;
    /// @brief The content; may be null.
    Node node;
};

/// @brief Shows the case whose key the selector yields; remounts only when that key changes.
struct Switch {
    /// @brief Which case to show.
    Prop<Key> selector;
    /// @brief The cases; the first one with the selected key wins.
    std::vector<SwitchCase> cases;
    /// @brief Shown when no case matches; may be null, and then nothing is shown.
    Node fallback;
    /// @brief Visibility, enablement, layout, drag-and-drop.
    Common common{};
};

/// @brief One page of `Tabs`.
struct Tab {
    /// @brief The tab's caption; set once.
    std::string label;
    /// @brief The page content, mounted the first time the tab is selected.
    Node node;
};

/// @brief A tab bar over pages.
struct Tabs {
    /// @brief The pages, in order; set once.
    std::vector<Tab> tabs;
    /// @brief The selected page's index; an index past the last page shows none.
    Prop<std::size_t> selected;
    /// @brief Called with the index the user picked.
    std::function<void(std::size_t)> onSelect;
    /// @brief Visibility, enablement, layout, drag-and-drop.
    Common common{};
};

/// @brief A modal overlay; its content is mounted only while it is open.
struct Dialog {
    /// @brief Whether it is shown.
    Prop<bool> open;
    /// @brief The title.
    Prop<std::string> title;
    /// @brief The content; may be null.
    Node child;
    /// @brief Called when the user dismisses it (Esc on the TUI).
    Action onDismiss;
    /// @brief Visibility, enablement, layout, drag-and-drop.
    Common common{};
};
```

Replace `NodeData`'s member with:

```cpp
    /// @brief The node.
    std::variant<Text, Button, TextInput, Checkbox, Select, Menu, Column, Row, Grid, Spacer, Panel, Scroll, Switch,
                 Tabs, Dialog, Busy, DateTimeInput, Slider, FilePicker>
        kind;
```

Insert after the `scroll` builder:

```cpp
/// @brief Shows the case the selector picks. Spelled `switchOf` because `switch` is a keyword.
/// @param spec The node.
/// @return The node.
[[nodiscard]] inline Node switchOf(Switch spec) { return detail::makeNode(std::move(spec)); }

/// @brief A tab bar over pages.
/// @param spec The node.
/// @return The node.
[[nodiscard]] inline Node tabs(Tabs spec) { return detail::makeNode(std::move(spec)); }

/// @brief A modal overlay.
/// @param spec The node.
/// @return The node.
[[nodiscard]] inline Node dialog(Dialog spec) { return detail::makeNode(std::move(spec)); }
```

and after the `filePicker` builder, before the closing `}  // namespace morph::ui`:

```cpp
/// @brief A Switch over an enumeration: each enumerator becomes the `int64` key of its case.
/// @tparam E The enumeration.
/// @param selector Which enumerator to show; every signal it reads is a dependency.
/// @param cases One node per enumerator shown.
/// @param fallback Shown when no case matches; may be null.
/// @return The node.
template <typename E>
    requires std::is_enum_v<E>
[[nodiscard]] Node switchOn(std::function<E()> selector, std::vector<std::pair<E, Node>> cases, Node fallback = {}) {
    std::vector<SwitchCase> keyed;
    keyed.reserve(cases.size());
    for (auto& [value, node] : cases) {
        keyed.push_back(SwitchCase{.key = Key{static_cast<std::int64_t>(value)}, .node = std::move(node)});
    }
    return switchOf(Switch{
        .selector = [current = std::move(selector)] { return Key{static_cast<std::int64_t>(current())}; },
        .cases = std::move(keyed),
        .fallback = std::move(fallback),
    });
}
```

- [ ] **Step 4: Add the widget interfaces**

In `include/morph/ui/backend.hpp`, insert after `class ScrollWidget : public ContainerWidget {};`:

```cpp
/// @brief A container that shows its children and nothing else: the host of a Switch's case and of a Tabs page.
class SlotWidget : public ContainerWidget {};

/// @brief A tab bar over page slots.
///
/// Its children are page slots in the order they were first shown; the mount keeps exactly the selected page's
/// slot visible. `setSelected` moves the bar's highlight only.
class TabsWidget : public ContainerWidget {
public:
    /// @brief Replaces the tab captions.
    /// @param labels One caption per tab, in order.
    virtual void setTabs(std::vector<std::string> const& labels) = 0;

    /// @brief Highlights a tab. Never calls the `onSelect` handler.
    /// @param index The tab; past the last one highlights none.
    virtual void setSelected(std::size_t index) = 0;

    /// @brief Sets what a user's tab choice calls, with its index.
    /// @param onSelect The handler; empty does nothing.
    virtual void setOnSelect(std::function<void(std::size_t)> onSelect) = 0;
};

/// @brief A modal overlay around its children, with a focus trap while open.
class DialogWidget : public ContainerWidget {
public:
    /// @brief Shows or hides the overlay.
    /// @param open Whether it is shown.
    virtual void setOpen(bool open) = 0;

    /// @brief Replaces the title.
    /// @param title The title, UTF-8.
    virtual void setTitle(std::string_view title) = 0;

    /// @brief Sets what a user dismissal (Esc on the TUI) calls.
    /// @param onDismiss The handler; empty does nothing.
    virtual void setOnDismiss(Action onDismiss) = 0;
};
```

and in `IViewBackend`, after `createScroll`:

```cpp
    /// @brief Makes a slot.
    /// @param parent The container to append to, or null for a root.
    /// @return The widget.
    [[nodiscard]] virtual std::unique_ptr<SlotWidget> createSlot(ContainerWidget* parent) = 0;

    /// @brief Makes a tab bar.
    /// @param parent The container to append to, or null for a root.
    /// @return The widget.
    [[nodiscard]] virtual std::unique_ptr<TabsWidget> createTabs(ContainerWidget* parent) = 0;

    /// @brief Makes a modal overlay.
    /// @param parent The container to append to, or null for a root.
    /// @return The widget.
    [[nodiscard]] virtual std::unique_ptr<DialogWidget> createDialog(ContainerWidget* parent) = 0;
```

- [ ] **Step 5: Teach `RecordingBackend` the three kinds**

In `include/morph/ui/testing/recording_backend.hpp`:

In `struct Callbacks`, after `picked`:

```cpp
    /// @brief From `DialogWidget::setOnDismiss`.
    Action dismiss;
```

After `class FakeFilePicker { … };`:

```cpp
/// @brief A fake `TabsWidget`: properties `tabs`, `selected`.
class FakeTabs final : public FakeContainer<TabsWidget> {
public:
    using FakeContainer<TabsWidget>::FakeContainer;
    void setTabs(std::vector<std::string> const& labels) override {
        set("tabs", formatList(labels, [](std::string const& label) { return label; }));
    }
    void setSelected(std::size_t index) override { set("selected", std::to_string(index)); }
    void setOnSelect(std::function<void(std::size_t)> onSelect) override { callbacks().index = std::move(onSelect); }
};

/// @brief A fake `DialogWidget`: properties `open`, `title`.
class FakeDialog final : public FakeContainer<DialogWidget> {
public:
    using FakeContainer<DialogWidget>::FakeContainer;
    void setOpen(bool open) override { set("open", formatBool(open)); }
    void setTitle(std::string_view title) override { set("title", std::string{title}); }
    void setOnDismiss(Action onDismiss) override { callbacks().dismiss = std::move(onDismiss); }
};
```

In `RecordingBackend`, after `createScroll`:

```cpp
    [[nodiscard]] std::unique_ptr<SlotWidget> createSlot(ContainerWidget* parent) override {
        return std::make_unique<detail::FakeContainer<SlotWidget>>(_store, "Slot", parent);
    }
    [[nodiscard]] std::unique_ptr<TabsWidget> createTabs(ContainerWidget* parent) override {
        return std::make_unique<detail::FakeTabs>(_store, "Tabs", parent);
    }
    [[nodiscard]] std::unique_ptr<DialogWidget> createDialog(ContainerWidget* parent) override {
        return std::make_unique<detail::FakeDialog>(_store, "Dialog", parent);
    }
```

and after `collapse`:

```cpp
    /// @brief Dismisses a dialog, as Esc does on the TUI.
    /// @param widgetId The dialog.
    void dismiss(int widgetId) {
        if (detail::Record const* const record = actionable(widgetId); record != nullptr) {
            invoke(record->callbacks.dismiss);
        }
    }
```

- [ ] **Step 6: Mount the three kinds**

In `include/morph/ui/mount.hpp`, insert inside `namespace detail`, before the `// A view tree is mounted by
descending it` comment:

```cpp
/// @brief The pages of one Tabs: a slot per tab, made the first time the tab is selected and kept, hidden, after.
struct Pages {
    /// @brief Each page's scope, owning its slot and its content; null until the tab is first selected.
    std::vector<std::unique_ptr<reactive::Scope>> scopes;
    /// @brief Each page's slot; null until the tab is first selected.
    std::vector<SlotWidget*> slots;
    /// @brief The page shown now, if any.
    std::optional<std::size_t> shown;
};
```

and inside `class Mounter`, after `mountKind(…, Scroll const& …)`:

```cpp
    // The case lives in a scope of its own: reset() tears the old case down, newest first, before the new one is
    // built. The Computed behind a bound selector is equality-gated, so an unchanged key never gets here.
    Widget& mountKind(reactive::Scope& scope, Switch const& spec, ContainerWidget* parent) {
        SlotWidget& widget = scope.adopt(_backend->createSlot(parent));
        applyCommon(scope, widget, spec.common);
        auto& content = scope.make<std::unique_ptr<reactive::Scope>>();
        bind(scope, spec.selector,
             [this, &widget, &content, cases = spec.cases, fallback = spec.fallback](Key const& key) {
                 content.reset();
                 auto const chosen = std::ranges::find(cases, key, &SwitchCase::key);
                 Node const& node = chosen == cases.end() ? fallback : chosen->node;
                 if (node) {
                     content = std::make_unique<reactive::Scope>(*_rt);
                     _rt->untracked([&] { static_cast<void>(mount(*content, node, &widget)); });
                 }
             });
        return widget;
    }

    Widget& mountKind(reactive::Scope& scope, Tabs const& spec, ContainerWidget* parent) {
        TabsWidget& widget = scope.adopt(_backend->createTabs(parent));
        applyCommon(scope, widget, spec.common);
        std::vector<std::string> labels;
        labels.reserve(spec.tabs.size());
        for (Tab const& tab : spec.tabs) {
            labels.push_back(tab.label);
        }
        widget.setTabs(labels);
        widget.setOnSelect(event(spec.onSelect));
        auto& pages = scope.make<Pages>();
        pages.scopes.resize(spec.tabs.size());
        pages.slots.resize(spec.tabs.size(), nullptr);
        bind(scope, spec.selected, [this, &widget, &pages, pageSpecs = spec.tabs](std::size_t index) {
            _rt->untracked([&] { showPage(widget, pages, pageSpecs, index); });
        });
        return widget;
    }

    // A page is mounted the first time its tab is selected and only hidden afterwards, so its widgets keep their
    // state (focus, scroll position, half-typed text) across tab switches.
    void showPage(TabsWidget& widget, Pages& pages, std::vector<Tab> const& pageSpecs, std::size_t index) {
        if (pages.shown.has_value() && *pages.shown < pages.slots.size()) {
            if (SlotWidget* const previous = pages.slots.at(*pages.shown); previous != nullptr) {
                previous->setVisible(false);
            }
        }
        pages.shown = index;
        if (index < pageSpecs.size()) {
            if (SlotWidget* const existing = pages.slots.at(index); existing != nullptr) {
                existing->setVisible(true);
            } else {
                auto page = std::make_unique<reactive::Scope>(*_rt);
                SlotWidget& slot = page->adopt(_backend->createSlot(&widget));
                static_cast<void>(mount(*page, pageSpecs.at(index).node, &slot));
                pages.slots.at(index) = &slot;
                pages.scopes.at(index) = std::move(page);
            }
        }
        widget.setSelected(index);
    }

    // The content is built before the overlay opens and torn down before it closes.
    Widget& mountKind(reactive::Scope& scope, Dialog const& spec, ContainerWidget* parent) {
        DialogWidget& widget = scope.adopt(_backend->createDialog(parent));
        applyCommon(scope, widget, spec.common);
        bind(scope, spec.title, [&widget](std::string const& title) { widget.setTitle(title); });
        widget.setOnDismiss(event(spec.onDismiss));
        auto& content = scope.make<std::unique_ptr<reactive::Scope>>();
        bind(scope, spec.open, [this, &widget, &content, child = spec.child](bool open) {
            if (!open) {
                content.reset();
            } else if (content == nullptr) {
                content = std::make_unique<reactive::Scope>(*_rt);
                _rt->untracked([&] { static_cast<void>(mount(*content, child, &widget)); });
            }
            widget.setOpen(open);
        });
        return widget;
    }
```

- [ ] **Step 7: Run the tests to verify they pass**

Run: `cmake --build build/reactive --target morph_tests && ./build/reactive/tests/morph_tests "[ui]"`
Expected: PASS.

Mutation checks, one at a time, each restored afterwards:

| Change | Test that must fail |
|---|---|
| In the Switch binding, build the new case into a fresh scope first and assign it to `content` afterwards (so the old case is destroyed after the new one is created) | "mounts the selected case and remounts child-first" |
| In `showPage`, delete the `existing != nullptr` branch (always make a new page) | "pages mount on first selection and are kept" |
| In the Dialog binding, delete `content.reset();` | "the content exists only while open" |
| In `Mounter::event`, return `handler` unwrapped | "a Dialog's button survives a nested loop in its own handler" |

- [ ] **Step 8: Commit**

```bash
git add include/morph/ui tests/test_ui_structure.cpp tests/CMakeLists.txt
git commit -m "wip(ui): Switch, switchOn, Tabs and Dialog mount into child scopes

Signed-off-by: Christian Parpart <christian@parpart.family>"
```

---

### Task 5: Keyed rows — `forEach`

A ForEach keeps one row `Signal<RowT>` and one `reactive::Scope` per key. On a new snapshot, kept keys are updated in
place (widget identity, focus and selection survive), gone keys are unmounted, new keys are mounted, and the
container is brought into order by moving only the rows outside a longest run already in order. The row type is
erased behind `detail::ForEachModel`, which Table (Task 6) reuses.

**Files:**
- Modify: `include/morph/ui/view.hpp` — includes, `detail::RowSlot`, `ForEachSession`, `ForEachModel`,
  `TypedForEach<RowT>`, `struct ForEach`, `NodeData`'s variant, both `forEach` overloads
- Modify: `include/morph/ui/mount.hpp` — includes, `detail::site::kDuplicateKey`, `detail::KeyedRow`,
  `detail::RowHook`, `detail::stableTargets`, `detail::reorderChildren`, and the ForEach mount in `Mounter`
- Modify: `tests/CMakeLists.txt` — add `test_ui_foreach.cpp` after `test_ui_structure.cpp`
- Test: `tests/test_ui_foreach.cpp`

**Interfaces:**
- Consumes: `reactive::Signal<T>` (`set`, `get`, `peek`), `reactive::Runtime::untracked`, `core()->report(...)`
  (Part 1); Tasks 1–4.
- Produces:
  - `ui::detail::RowSlot{assign(std::size_t), view() -> Node}`, `ui::detail::ForEachSession{pull() ->
    std::vector<Key>, makeRow(std::size_t) -> std::unique_ptr<RowSlot>}`, `ui::detail::ForEachModel{open(
    reactive::Runtime&) -> std::unique_ptr<ForEachSession>}`, `ui::detail::TypedForEach<RowT>` (Task 6's `table`
    builds on it).
  - `ui::ForEach{model, axis, gap, common}`; `forEach<RowT>(std::function<std::vector<RowT>()>, …)` and
    `forEach<RowT>(reactive::Signal<std::vector<RowT>> const&, …)`.
  - `ui::detail::site::kDuplicateKey`; `detail::RowHook = std::function<void(Widget&, Key const&)>`, and
    `Mounter::mountRows(scope, model, container, RowHook)` (Task 6 passes `setRowKey` through it).

- [ ] **Step 1: Write the failing test**

Create `tests/test_ui_foreach.cpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0

#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <morph/reactive/runtime.hpp>
#include <morph/reactive/signal.hpp>
#include <morph/ui/mount.hpp>
#include <morph/ui/testing/recording_backend.hpp>
#include <morph/ui/view.hpp>
#include <string>
#include <variant>
#include <vector>

#include "owner_probe_recorder.hpp"
#include "test_support.hpp"

namespace ui = morph::ui;

namespace {

using morph::reactive::Runtime;
using morph::reactive::Signal;
using ui::testing::RecordingBackend;
using Owner = morph::testing::StepExecutor;
using Probe = morph::testing::OwnerProbeRecorder;
using Lines = std::vector<std::string>;

struct Lap {
    std::int64_t id = 0;
    std::string label;
    bool operator==(Lap const&) const = default;
};

struct Tag {
    std::string name;
    int uses = 0;
    bool operator==(Tag const&) const = default;
};

ui::Key keyOf(Lap const& lap) { return ui::Key{lap.id}; }

ui::Node lapView(Signal<Lap> const& lap) {
    return ui::text({.text = [&lap] { return lap.get().label; }});
}

ui::Node lapList(Signal<std::vector<Lap>> const& rows) { return ui::forEach<Lap>(rows, keyOf, lapView); }

std::vector<Lap> abc() { return {{.id = 1, .label = "a"}, {.id = 2, .label = "b"}, {.id = 3, .label = "c"}}; }

}  // namespace

TEST_CASE("ui::forEach: a session snapshots the rows and makes a slot per row", "[ui]") {
    Owner owner;
    Runtime runtime{owner};
    Signal<std::vector<Lap>> rows{runtime, {{.id = 1, .label = "a"}, {.id = 2, .label = "b"}}};
    ui::Node const node = ui::forEach<Lap>(rows, keyOf, lapView, ui::Axis::Horizontal, 2);
    auto const& each = std::get<ui::ForEach>(node->kind);
    CHECK(each.axis == ui::Axis::Horizontal);
    CHECK(each.gap == 2);
    auto session = each.model->open(runtime);
    CHECK(session->pull() == std::vector<ui::Key>{ui::Key{std::int64_t{1}}, ui::Key{std::int64_t{2}}});
    auto slot = session->makeRow(1);
    REQUIRE(slot->view() != nullptr);
    CHECK(std::get<ui::Text>(slot->view()->kind).text.evaluate() == "b");
    rows.set({{.id = 2, .label = "b2"}});
    CHECK(session->pull() == std::vector<ui::Key>{ui::Key{std::int64_t{2}}});
    slot->assign(0);
    CHECK(std::get<ui::Text>(slot->view()->kind).text.evaluate() == "b2");
}

TEST_CASE("ui::forEach: mounts one row per key, in order", "[ui]") {
    Owner owner;
    Runtime runtime{owner};
    RecordingBackend backend;
    Signal<std::vector<Lap>> rows{runtime, abc()};
    ui::Mounted const view{runtime, backend, lapList(rows)};
    CHECK(backend.dump() == "Column#1 gap=0\n"
                            "  Text#2 role=Normal text=a\n"
                            "  Text#3 role=Normal text=b\n"
                            "  Text#4 role=Normal text=c\n");
}

TEST_CASE("ui::forEach: an update of a kept key keeps its widget", "[ui]") {
    Owner owner;
    Runtime runtime{owner};
    RecordingBackend backend;
    Signal<std::vector<Lap>> rows{runtime, abc()};
    ui::Mounted const view{runtime, backend, lapList(rows)};
    backend.clearLog();
    rows.set({{.id = 1, .label = "a"}, {.id = 2, .label = "b2"}, {.id = 3, .label = "c"}});
    owner.runAll();
    CHECK(backend.log() == Lines{"set Text#3 text=b2"});
}

TEST_CASE("ui::forEach: an append only creates and a removal only destroys", "[ui]") {
    Owner owner;
    Runtime runtime{owner};
    RecordingBackend backend;
    Signal<std::vector<Lap>> rows{runtime, abc()};
    ui::Mounted const view{runtime, backend, lapList(rows)};

    backend.clearLog();
    rows.mutate([](std::vector<Lap>& laps) { laps.push_back({.id = 4, .label = "d"}); });
    owner.runAll();
    CHECK(backend.log() == Lines{"create Text#5 in Column#1", "set Text#5 text=d", "set Text#5 role=Normal"});

    backend.clearLog();
    rows.set({{.id = 1, .label = "a"}, {.id = 3, .label = "c"}, {.id = 4, .label = "d"}});
    owner.runAll();
    CHECK(backend.log() == Lines{"destroy Text#3"});
}

TEST_CASE("ui::forEach: an insert at the front moves only the new row", "[ui]") {
    Owner owner;
    Runtime runtime{owner};
    RecordingBackend backend;
    Signal<std::vector<Lap>> rows{runtime, {{.id = 1, .label = "a"}, {.id = 2, .label = "b"}}};
    ui::Mounted const view{runtime, backend, lapList(rows)};
    backend.clearLog();
    rows.set({{.id = 3, .label = "c"}, {.id = 1, .label = "a"}, {.id = 2, .label = "b"}});
    owner.runAll();
    CHECK(backend.log() == Lines{"create Text#4 in Column#1", "set Text#4 text=c", "set Text#4 role=Normal",
                                 "move Text#4 to 0"});
}

TEST_CASE("ui::forEach: a reorder moves only the rows outside the longest run in order", "[ui]") {
    Owner owner;
    Runtime runtime{owner};
    RecordingBackend backend;

    SECTION("the last row to the front is one move") {
        Signal<std::vector<Lap>> rows{runtime, abc()};
        ui::Mounted const view{runtime, backend, lapList(rows)};
        backend.clearLog();
        rows.set({{.id = 3, .label = "c"}, {.id = 1, .label = "a"}, {.id = 2, .label = "b"}});
        owner.runAll();
        CHECK(backend.log() == Lines{"move Text#4 to 0"});
    }
    SECTION("the first row to the end is one move") {
        Signal<std::vector<Lap>> rows{runtime, abc()};
        ui::Mounted const view{runtime, backend, lapList(rows)};
        backend.clearLog();
        rows.set({{.id = 2, .label = "b"}, {.id = 3, .label = "c"}, {.id = 1, .label = "a"}});
        owner.runAll();
        CHECK(backend.log() == Lines{"move Text#2 to 2"});
    }
    SECTION("a reversal of three is two moves") {
        Signal<std::vector<Lap>> rows{runtime, abc()};
        ui::Mounted const view{runtime, backend, lapList(rows)};
        backend.clearLog();
        rows.set({{.id = 3, .label = "c"}, {.id = 2, .label = "b"}, {.id = 1, .label = "a"}});
        owner.runAll();
        CHECK(backend.log() == Lines{"move Text#2 to 2", "move Text#3 to 1"});
        CHECK(backend.dump() == "Column#1 gap=0\n"
                                "  Text#4 role=Normal text=c\n"
                                "  Text#3 role=Normal text=b\n"
                                "  Text#2 role=Normal text=a\n");
    }
}

TEST_CASE("ui::forEach: string keys are keys too", "[ui]") {
    Owner owner;
    Runtime runtime{owner};
    RecordingBackend backend;
    Signal<std::vector<Tag>> tags{runtime, {{.name = "red"}, {.name = "blue"}}};
    ui::Mounted const view{runtime, backend,
                           ui::forEach<Tag>(
                               tags, [](Tag const& tag) { return ui::Key{tag.name}; },
                               [](Signal<Tag> const& tag) {
                                   return ui::text({.text = [&tag] {
                                       return tag.get().name + " " + std::to_string(tag.get().uses);
                                   }});
                               })};
    backend.clearLog();
    tags.set({{.name = "blue", .uses = 1}, {.name = "red"}});
    owner.runAll();
    // The move happens in the ForEach's own run; the row's binding runs after it in the same flush.
    CHECK(backend.log() == Lines{"move Text#2 to 1", "set Text#3 text=blue 1"});
}

TEST_CASE("ui::forEach: a duplicate key is reported and refused; the first one is kept", "[ui]") {
    Owner owner;
    Probe const probe{owner.coreExecutor()};
    Runtime runtime{owner};
    RecordingBackend backend;
    Signal<std::vector<Lap>> rows{runtime,
                                  {{.id = 1, .label = "a"}, {.id = 1, .label = "dup"}, {.id = 2, .label = "b"}}};
    ui::Mounted const view{runtime, backend, lapList(rows)};
    CHECK(probe.count(ui::detail::site::kDuplicateKey) == 1);
    CHECK(backend.dump() == "Column#1 gap=0\n  Text#2 role=Normal text=a\n  Text#3 role=Normal text=b\n");
}

// Review Focus 4.
TEST_CASE("ui::forEach: emptied then refilled", "[ui]") {
    Owner owner;
    Runtime runtime{owner};
    RecordingBackend backend;
    Signal<std::vector<Lap>> rows{runtime, abc()};
    ui::Mounted const view{runtime, backend, lapList(rows)};
    rows.set({});
    owner.runAll();
    CHECK(backend.dump() == "Column#1 gap=0\n");
    CHECK(backend.all("Text").empty());
    rows.set({{.id = 9, .label = "z"}});
    owner.runAll();
    CHECK(backend.dump() == "Column#1 gap=0\n  Text#5 role=Normal text=z\n");
}

// Review Focus 1.
TEST_CASE("ui::forEach: building a row reads nothing on the ForEach's behalf", "[ui]") {
    Owner owner;
    Runtime runtime{owner};
    RecordingBackend backend;
    Signal<std::vector<Lap>> rows{runtime, {{.id = 1, .label = "a"}}};
    Signal<std::string> theme{runtime, "light"};
    int pulls = 0;
    ui::Mounted const view{runtime, backend,
                           ui::forEach<Lap>(
                               [&] {
                                   ++pulls;
                                   return rows.get();
                               },
                               keyOf,
                               [&theme](Signal<Lap> const& lap) {
                                   return ui::text({.text = theme.get() + ":" + lap.peek().label});
                               })};
    CHECK(backend.prop(2, "text") == "light:a");
    theme.set("dark");
    CHECK(owner.pending() == 0);
    rows.set({{.id = 1, .label = "a"}, {.id = 2, .label = "b"}});
    owner.runAll();
    CHECK(pulls == 2);
    CHECK(backend.prop(3, "text") == "dark:b");
}
```

Register it after `test_ui_structure.cpp`.

- [ ] **Step 2: Run the test to verify it fails**

Run: `cmake --build build/reactive --target morph_tests`
Expected: FAIL — `no member named 'forEach' in namespace 'morph::ui'`.

- [ ] **Step 3: Add the type-erased row model and the node**

In `include/morph/ui/view.hpp`, add to the includes, after `#include "../util/datetime.hpp"`:

```cpp
#include "../reactive/runtime.hpp"
#include "../reactive/signal.hpp"
```

Insert after `struct Busy { … };`:

```cpp
namespace detail {

/// @brief One mounted row of a ForEach or a Table: the row's signal, and the view built from it once.
class RowSlot {
public:
    RowSlot() = default;
    virtual ~RowSlot() = default;
    RowSlot(RowSlot const&) = delete;
    RowSlot& operator=(RowSlot const&) = delete;
    RowSlot(RowSlot&&) = delete;
    RowSlot& operator=(RowSlot&&) = delete;

    /// @brief Sets the row's signal to an entry of its session's latest snapshot; equal rows notify nobody.
    /// @param index A position in that snapshot.
    virtual void assign(std::size_t index) = 0;

    /// @brief The view built for this row when the slot was made.
    /// @return The row's node.
    [[nodiscard]] virtual Node view() const = 0;
};

/// @brief One mount's use of a ForEach: the latest snapshot of the rows, and a way to make row slots from it.
class ForEachSession {
public:
    ForEachSession() = default;
    virtual ~ForEachSession() = default;
    ForEachSession(ForEachSession const&) = delete;
    ForEachSession& operator=(ForEachSession const&) = delete;
    ForEachSession(ForEachSession&&) = delete;
    ForEachSession& operator=(ForEachSession&&) = delete;

    /// @brief Reads the rows — tracked, inside the mount's Effect — keeps them as the snapshot, and returns their
    ///        keys.
    /// @return One key per row, in order, duplicates included.
    [[nodiscard]] virtual std::vector<Key> pull() = 0;

    /// @brief Makes the slot for one snapshot entry, building its view. The mount calls it untracked.
    /// @param index A position in the latest snapshot.
    /// @return The slot. It refers to this session, which must outlive it.
    [[nodiscard]] virtual std::unique_ptr<RowSlot> makeRow(std::size_t index) = 0;
};

/// @brief A ForEach's row source, key function and row view, with the row type erased.
class ForEachModel {
public:
    ForEachModel() = default;
    virtual ~ForEachModel() = default;
    ForEachModel(ForEachModel const&) = delete;
    ForEachModel& operator=(ForEachModel const&) = delete;
    ForEachModel(ForEachModel&&) = delete;
    ForEachModel& operator=(ForEachModel&&) = delete;

    /// @brief Starts one mount's session.
    /// @param runtime The runtime the row signals belong to.
    /// @return The session. It refers to this model, which must outlive it.
    [[nodiscard]] virtual std::unique_ptr<ForEachSession> open(reactive::Runtime& runtime) const = 0;
};

/// @brief The typed model behind `forEach<RowT>` and `table<RowT>`.
/// @tparam RowT The row type; copyable. With an `operator==`, an unchanged row notifies nobody.
template <typename RowT>
class TypedForEach final : public ForEachModel {
public:
    /// @param rows Reads the rows; every signal it reads is a dependency of the ForEach.
    /// @param keyOf A row's identity.
    /// @param rowView Builds a row's view once, from a signal the mount keeps equal to the row.
    TypedForEach(std::function<std::vector<RowT>()> rows, std::function<Key(RowT const&)> keyOf,
                 std::function<Node(reactive::Signal<RowT> const&)> rowView)
        : _rows{std::move(rows)}, _keyOf{std::move(keyOf)}, _rowView{std::move(rowView)} {}

    [[nodiscard]] std::unique_ptr<ForEachSession> open(reactive::Runtime& runtime) const override {
        return std::make_unique<Session>(runtime, *this);
    }

private:
    class Slot final : public RowSlot {
    public:
        Slot(reactive::Runtime& runtime, std::vector<RowT> const& snapshot, std::size_t index,
             TypedForEach const& model)
            : _snapshot{&snapshot}, _row{runtime, snapshot.at(index)}, _view{model._rowView(_row)} {}

        void assign(std::size_t index) override { _row.set(_snapshot->at(index)); }
        [[nodiscard]] Node view() const override { return _view; }

    private:
        std::vector<RowT> const* _snapshot;
        reactive::Signal<RowT> _row;
        Node _view;
    };

    class Session final : public ForEachSession {
    public:
        Session(reactive::Runtime& runtime, TypedForEach const& model) : _rt{&runtime}, _model{&model} {}

        [[nodiscard]] std::vector<Key> pull() override {
            _snapshot = _model->_rows();
            std::vector<Key> keys;
            keys.reserve(_snapshot.size());
            for (RowT const& entry : _snapshot) {
                keys.push_back(_model->_keyOf(entry));
            }
            return keys;
        }

        [[nodiscard]] std::unique_ptr<RowSlot> makeRow(std::size_t index) override {
            return std::make_unique<Slot>(*_rt, _snapshot, index, *_model);
        }

    private:
        reactive::Runtime* _rt;
        TypedForEach const* _model;
        std::vector<RowT> _snapshot;
    };

    std::function<std::vector<RowT>()> _rows;
    std::function<Key(RowT const&)> _keyOf;
    std::function<Node(reactive::Signal<RowT> const&)> _rowView;
};

}  // namespace detail

/// @brief One widget per row of a keyed collection, updated in place when a row with the same key changes.
struct ForEach {
    /// @brief The rows, the key function and the row view, type-erased; null shows no rows.
    std::shared_ptr<detail::ForEachModel const> model;
    /// @brief The direction rows are stacked in; set once.
    Axis axis = Axis::Vertical;
    /// @brief Space between rows, in backend units.
    int gap = 0;
    /// @brief Visibility, enablement, layout, drag-and-drop of the row container.
    Common common{};
};
```

Replace `NodeData`'s member with:

```cpp
    /// @brief The node.
    std::variant<Text, Button, TextInput, Checkbox, Select, Menu, Column, Row, Grid, Spacer, Panel, Scroll, Switch,
                 Tabs, Dialog, Busy, ForEach, DateTimeInput, Slider, FilePicker>
        kind;
```

Append after `switchOn`, before the closing `}  // namespace morph::ui`:

```cpp
/// @brief One widget per row of a keyed collection read through a binding (a `Query`'s `value()` included).
/// @tparam RowT The row type; copyable.
/// @param rows Reads the rows; every signal it reads is a dependency.
/// @param keyOf A row's identity; a later duplicate of a key is refused at mount.
/// @param rowView Builds a row's view once, from a signal the mount keeps equal to the row. Read the signal inside
///        bindings: what the view reads directly while it is built is read once and never again.
/// @param axis The stacking direction.
/// @param gap Space between rows.
/// @return The node.
template <typename RowT>
[[nodiscard]] Node forEach(std::function<std::vector<RowT>()> rows, std::function<Key(RowT const&)> keyOf,
                           std::function<Node(reactive::Signal<RowT> const&)> rowView, Axis axis = Axis::Vertical,
                           int gap = 0) {
    return detail::makeNode(ForEach{
        .model = std::make_shared<detail::TypedForEach<RowT> const>(std::move(rows), std::move(keyOf),
                                                                    std::move(rowView)),
        .axis = axis,
        .gap = gap,
    });
}

/// @brief One widget per row of a keyed collection held in a signal.
/// @tparam RowT The row type; copyable.
/// @param rows The signal; it must outlive every mount of the node.
/// @param keyOf A row's identity; a later duplicate of a key is refused at mount.
/// @param rowView Builds a row's view once, from a signal the mount keeps equal to the row.
/// @param axis The stacking direction.
/// @param gap Space between rows.
/// @return The node.
template <typename RowT>
[[nodiscard]] Node forEach(reactive::Signal<std::vector<RowT>> const& rows, std::function<Key(RowT const&)> keyOf,
                           std::function<Node(reactive::Signal<RowT> const&)> rowView, Axis axis = Axis::Vertical,
                           int gap = 0) {
    return forEach<RowT>([&rows] { return rows.get(); }, std::move(keyOf), std::move(rowView), axis, gap);
}
```

- [ ] **Step 4: Mount it**

In `include/morph/ui/mount.hpp`, add `#include <unordered_map>` and `#include <unordered_set>` to the includes.
Insert at the top of `namespace detail`, before `struct Pages`:

```cpp
/// @brief The site names a misuse in this layer is reported under, through the runtime's owner probe.
namespace site {
/// @brief A ForEach or Table snapshot repeated a key; the later row is refused and the first one kept.
inline constexpr char const* kDuplicateKey = "morph::ui: duplicate ForEach key";
}  // namespace site

/// @brief Called for each newly mounted row with its widget and key; a Table tells its widget the row's key.
using RowHook = std::function<void(Widget&, Key const&)>;

/// @brief One mounted row of a ForEach or Table.
struct KeyedRow {
    /// @brief The row's key.
    Key key;
    /// @brief Owns the row's slot (adopted first) and its widgets and bindings.
    std::unique_ptr<reactive::Scope> scope;
    /// @brief The row's slot, owned by `scope`.
    RowSlot* slot = nullptr;
    /// @brief The row's widget, owned by `scope`; null when the row view was a null node.
    Widget* widget = nullptr;
};

/// @brief Which entries of a permutation lie on one longest increasing run.
///
/// @p sequence holds, in current order, each widget's target position; the widgets on the run are already in
/// order relative to each other and stay where they are, and every other one is moved once.
/// @param sequence A permutation of `0 .. n-1`.
/// @return Indexed by target position: whether that widget stays.
[[nodiscard]] inline std::vector<bool> stableTargets(std::vector<std::size_t> const& sequence) {
    constexpr std::size_t kNone = static_cast<std::size_t>(-1);
    std::vector<std::size_t> tails;  // tails.at(n - 1): where the smallest tail of a run of length n sits
    std::vector<std::size_t> previous(sequence.size(), kNone);
    for (std::size_t i = 0; i < sequence.size(); ++i) {
        auto const place = std::ranges::lower_bound(tails, sequence.at(i), {},
                                                    [&sequence](std::size_t entry) { return sequence.at(entry); });
        auto const length = static_cast<std::size_t>(place - tails.begin());
        if (length > 0) {
            previous.at(i) = tails.at(length - 1);
        }
        if (length == tails.size()) {
            tails.push_back(i);
        } else {
            tails.at(length) = i;
        }
    }
    std::vector<bool> stable(sequence.size(), false);
    for (std::size_t cursor = tails.empty() ? kNone : tails.back(); cursor != kNone; cursor = previous.at(cursor)) {
        stable.at(sequence.at(cursor)) = true;
    }
    return stable;
}

/// @brief Brings a container's children from their current order into the target order with `moveChild`,
///        moving only the children outside a longest run already in order.
/// @param container The container.
/// @param current Its children, in their current order.
/// @param target The same children, in the order wanted.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters) -- the before and after orders of one container
inline void reorderChildren(ContainerWidget& container, std::vector<Widget*> current,
                            std::vector<Widget*> const& target) {
    std::unordered_map<Widget const*, std::size_t> targetOf;
    for (std::size_t i = 0; i < target.size(); ++i) {
        targetOf.emplace(target.at(i), i);
    }
    std::vector<std::size_t> sequence;
    sequence.reserve(current.size());
    for (Widget const* const child : current) {
        sequence.push_back(targetOf.at(child));
    }
    std::vector<bool> const stable = stableTargets(sequence);
    auto const indexOf = [&current](Widget const* child) {
        return static_cast<std::size_t>(std::ranges::find(current, child) - current.begin());
    };
    // From the back: each moved child goes directly before its successor in the target order, which is already
    // in place, so one move per unstable child suffices.
    for (std::size_t position = target.size(); position-- > 0;) {
        if (stable.at(position)) {
            continue;
        }
        Widget* const moving = target.at(position);
        std::size_t const from = indexOf(moving);
        std::size_t destination = current.size() - 1;
        if (position + 1 < target.size()) {
            std::size_t const before = indexOf(target.at(position + 1));
            destination = from < before ? before - 1 : before;
        }
        if (destination == from) {
            continue;
        }
        container.moveChild(*moving, destination);
        current.erase(current.begin() + static_cast<std::ptrdiff_t>(from));
        current.insert(current.begin() + static_cast<std::ptrdiff_t>(destination), moving);
    }
}
```

Inside `class Mounter`, after the Dialog `mountKind`, add:

```cpp
    Widget& mountKind(reactive::Scope& scope, ForEach const& spec, ContainerWidget* parent) {
        StackWidget& widget = scope.adopt(_backend->createStack(parent, spec.axis));
        applyCommon(scope, widget, spec.common);
        widget.setGap(spec.gap);
        mountRows(scope, spec.model, widget, {});
        return widget;
    }

    // The model, then the session, then the rows, then the Effect: destroyed in reverse, so the Effect stops
    // first, the rows (whose slots point into the session's snapshot) go next, and the model outlives its session.
    void mountRows(reactive::Scope& scope, std::shared_ptr<ForEachModel const> const& model,
                   ContainerWidget& container, RowHook onRow) {
        if (!model) {
            return;
        }
        scope.make<std::shared_ptr<ForEachModel const>>(model);
        ForEachSession& session = scope.adopt(model->open(*_rt));
        auto& rows = scope.make<std::vector<KeyedRow>>();
        scope.effect([this, &session, &rows, &container, onRow = std::move(onRow)] {
            std::vector<Key> const keys = session.pull();  // tracked: the rows are this Effect's only source
            // Untracked: what a row view reads while it is built, or what a kept row's update touches, must not
            // become a dependency of the whole list.
            _rt->untracked([&] { reconcile(session, rows, container, keys, onRow); });
        });
    }

    // The snapshot index of each key's first occurrence, in order; every later duplicate is reported.
    [[nodiscard]] std::vector<std::size_t> firstOccurrences(std::vector<Key> const& keys) {
        std::unordered_set<Key> seen;
        std::vector<std::size_t> order;
        order.reserve(keys.size());
        for (std::size_t index = 0; index < keys.size(); ++index) {
            if (seen.insert(keys.at(index)).second) {
                order.push_back(index);
            } else {
                _rt->core()->report(site::kDuplicateKey);
            }
        }
        return order;
    }

    [[nodiscard]] KeyedRow mountRow(ForEachSession& session, ContainerWidget& container, Key const& key,
                                    std::size_t index, RowHook const& onRow) {
        KeyedRow row{.key = key, .scope = std::make_unique<reactive::Scope>(*_rt)};
        row.slot = &row.scope->adopt(session.makeRow(index));
        row.widget = mount(*row.scope, row.slot->view(), &container);
        if (row.widget != nullptr && onRow) {
            onRow(*row.widget, key);
        }
        return row;
    }

    // `rows` mirrors the container's order on entry and on exit.
    void reconcile(ForEachSession& session, std::vector<KeyedRow>& rows, ContainerWidget& container,
                   std::vector<Key> const& keys, RowHook const& onRow) {
        std::vector<std::size_t> const order = firstOccurrences(keys);
        std::unordered_map<Key, std::size_t> wanted;  // key -> snapshot index
        for (std::size_t const index : order) {
            wanted.emplace(keys.at(index), index);
        }

        // Unmount the gone keys; update the kept ones in place, which keeps their widgets.
        std::vector<KeyedRow> current;
        current.reserve(order.size());
        for (KeyedRow& row : rows) {
            if (auto const found = wanted.find(row.key); found != wanted.end()) {
                row.slot->assign(found->second);
                current.push_back(std::move(row));
            } else {
                row.scope.reset();
            }
        }

        // Mount the new keys; each widget is appended to the container.
        std::unordered_map<Key, std::size_t> position;  // key -> index in `current`
        for (std::size_t i = 0; i < current.size(); ++i) {
            position.emplace(current.at(i).key, i);
        }
        for (std::size_t const index : order) {
            Key const& key = keys.at(index);
            if (!position.contains(key)) {
                position.emplace(key, current.size());
                current.push_back(mountRow(session, container, key, index, onRow));
            }
        }

        // Move the widgets into snapshot order, and keep `rows` in that order too.
        std::vector<Widget*> present;
        for (KeyedRow const& row : current) {
            if (row.widget != nullptr) {
                present.push_back(row.widget);
            }
        }
        std::vector<Widget*> target;
        std::vector<KeyedRow> ordered;
        ordered.reserve(current.size());
        for (std::size_t const index : order) {
            KeyedRow& row = current.at(position.at(keys.at(index)));
            if (row.widget != nullptr) {
                target.push_back(row.widget);
            }
            ordered.push_back(std::move(row));
        }
        reorderChildren(container, std::move(present), target);
        rows = std::move(ordered);
    }
```

- [ ] **Step 5: Run the tests to verify they pass**

Run: `cmake --build build/reactive --target morph_tests && ./build/reactive/tests/morph_tests "[ui]"`
Expected: PASS.

Mutation checks, one at a time, each restored afterwards:

| Change | Test that must fail |
|---|---|
| In `mountRows`' Effect, call `reconcile(…)` directly instead of inside `_rt->untracked` | "building a row reads nothing on the ForEach's behalf" (`owner.pending()` is 1 after `theme.set`) |
| In `reconcile`, unmount every kept row too (`row.scope.reset()` in both branches, and drop the `current.push_back`) | "an update of a kept key keeps its widget" |
| In `reorderChildren`, treat every child as unstable (`std::vector<bool> const stable(target.size(), false);`) | "an insert at the front moves only the new row" and "the last row to the front is one move" |
| In `firstOccurrences`, drop the `seen` check (push every index) | "a duplicate key is reported and refused" |

- [ ] **Step 6: Commit**

```bash
git add include/morph/ui tests/test_ui_foreach.cpp tests/CMakeLists.txt
git commit -m "wip(ui): keyed forEach rows, updated in place and reordered with the fewest moves

Signed-off-by: Christian Parpart <christian@parpart.family>"
```

---

### Task 6: Tables — `table<RowT>`

A Table is a ForEach whose row view is a `Row` of cells, under column headers, with selection and activation by
key. It reuses Task 5's keyed machinery and tells the widget each new row's key.

**Files:**
- Modify: `include/morph/ui/view.hpp` — `SelectionMode`, `TableColumn`, `Table`, `NodeData`'s variant,
  `TableOptions`, `table<RowT>`
- Modify: `include/morph/ui/backend.hpp` — `TableWidget`, `IViewBackend::createTable`
- Modify: `include/morph/ui/testing/recording_backend.hpp` — `Callbacks::selection`/`activate`,
  `enumName(SelectionMode)`, `FakeTable`, `createTable`, `selectRows`, `activateRow`
- Modify: `include/morph/ui/mount.hpp` — the Table `mountKind`
- Modify: `tests/CMakeLists.txt` — add `test_ui_table.cpp` after `test_ui_foreach.cpp`
- Test: `tests/test_ui_table.cpp`

**Interfaces:**
- Consumes: Task 5's `detail::TypedForEach`, `Mounter::mountRows`, `detail::RowHook`.
- Produces: `ui::SelectionMode{None, Single, Multiple}`, `ui::TableColumn{label, width}`, `ui::Table`,
  `ui::TableOptions`, `ui::table<RowT>(columns, rows, keyOf, cells, options)`; `ui::TableWidget`,
  `IViewBackend::createTable`; `RecordingBackend::selectRows(int, std::vector<Key>)`,
  `activateRow(int, Key)`. Mount order: columns, selection mode, the two callbacks, the rows (each new row's
  `setRowKey` right after it is built), then `selection` — so a selection names rows that already exist.

- [ ] **Step 1: Write the failing test**

Create `tests/test_ui_table.cpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0

#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <morph/reactive/runtime.hpp>
#include <morph/reactive/signal.hpp>
#include <morph/ui/mount.hpp>
#include <morph/ui/testing/recording_backend.hpp>
#include <morph/ui/view.hpp>
#include <string>
#include <vector>

#include "test_support.hpp"

namespace ui = morph::ui;

namespace {

using morph::reactive::Runtime;
using morph::reactive::Signal;
using ui::testing::RecordingBackend;
using Owner = morph::testing::StepExecutor;
using Lines = std::vector<std::string>;

struct Item {
    std::int64_t id = 0;
    std::string name;
    int qty = 0;
    bool operator==(Item const&) const = default;
};

struct Fixture {
    Owner owner;
    Runtime runtime{owner};
    RecordingBackend backend;
    Signal<std::vector<Item>> items{runtime,
                                    {{.id = 1, .name = "apple", .qty = 3}, {.id = 2, .name = "pear", .qty = 5}}};
    Signal<std::vector<ui::Key>> selection{runtime, {}};
    std::vector<ui::Key> activated;

    ui::Node view() {
        return ui::table<Item>(
            {{.label = "Name"}, {.label = "Qty", .width = ui::Sizing::fixed(6)}}, [this] { return items.get(); },
            [](Item const& item) { return ui::Key{item.id}; },
            [](Signal<Item> const& item) {
                return std::vector<ui::Node>{ui::text({.text = [&item] { return item.get().name; }}),
                                             ui::text({.text = [&item] { return std::to_string(item.get().qty); }})};
            },
            {.selectionMode = ui::SelectionMode::Single,
             .selection = [this] { return selection.get(); },
             .onSelectionChange = [this](std::vector<ui::Key> keys) { selection.set(std::move(keys)); },
             .onActivate = [this](ui::Key key) { activated.push_back(std::move(key)); }});
    }
};

ui::Key intKey(std::int64_t value) { return ui::Key{value}; }

}  // namespace

TEST_CASE("ui::table: columns, keyed rows of cells, selection after the rows", "[ui]") {
    Fixture fixture;
    ui::Mounted const mounted{fixture.runtime, fixture.backend, fixture.view()};
    CHECK(fixture.backend.dump() == "Table#1 columns=[Name:content,Qty:fixed(6)] selection=[] selectionMode=Single\n"
                                    "  Row#2 gap=0 rowKey=1\n"
                                    "    Text#3 role=Normal text=apple\n"
                                    "    Text#4 role=Normal text=3\n"
                                    "  Row#5 gap=0 rowKey=2\n"
                                    "    Text#6 role=Normal text=pear\n"
                                    "    Text#7 role=Normal text=5\n");
    CHECK(fixture.backend.log().back() == "set Table#1 selection=[]");
}

TEST_CASE("ui::table: a user's selection comes back through the selection prop", "[ui]") {
    Fixture fixture;
    ui::Mounted const mounted{fixture.runtime, fixture.backend, fixture.view()};
    fixture.backend.clearLog();
    fixture.backend.selectRows(1, {intKey(2)});
    fixture.owner.runAll();
    CHECK(fixture.selection.peek() == std::vector<ui::Key>{intKey(2)});
    CHECK(fixture.backend.log() == Lines{"set Table#1 selection=[2]"});
}

TEST_CASE("ui::table: activating a row reports its key", "[ui]") {
    Fixture fixture;
    ui::Mounted const mounted{fixture.runtime, fixture.backend, fixture.view()};
    fixture.backend.activateRow(1, intKey(1));
    CHECK(fixture.activated == std::vector<ui::Key>{intKey(1)});
}

TEST_CASE("ui::table: a new row is told its key once; a reorder only moves", "[ui]") {
    Fixture fixture;
    ui::Mounted const mounted{fixture.runtime, fixture.backend, fixture.view()};

    fixture.backend.clearLog();
    fixture.items.mutate([](std::vector<Item>& rows) { rows.push_back({.id = 3, .name = "plum", .qty = 1}); });
    fixture.owner.runAll();
    CHECK(fixture.backend.log() == Lines{"create Row#8 in Table#1", "set Row#8 gap=0", "create Text#9 in Row#8",
                                         "set Text#9 text=plum", "set Text#9 role=Normal",
                                         "create Text#10 in Row#8", "set Text#10 text=1",
                                         "set Text#10 role=Normal", "set Row#8 rowKey=3"});

    fixture.backend.clearLog();
    fixture.items.set({{.id = 2, .name = "pear", .qty = 5},
                       {.id = 1, .name = "apple", .qty = 3},
                       {.id = 3, .name = "plum", .qty = 1}});
    fixture.owner.runAll();
    CHECK(fixture.backend.log() == Lines{"move Row#2 to 1"});
}
```

Register it after `test_ui_foreach.cpp`.

- [ ] **Step 2: Run the test to verify it fails**

Run: `cmake --build build/reactive --target morph_tests`
Expected: FAIL — `no member named 'table' in namespace 'morph::ui'`.

- [ ] **Step 3: Add the node and its builder**

In `include/morph/ui/view.hpp`, insert after `enum class FilePickerMode { … };`:

```cpp
/// @brief How many rows of a `Table` the user can select.
enum class SelectionMode : std::uint8_t {
    None,      ///< Rows can be activated, not selected.
    Single,    ///< At most one row.
    Multiple,  ///< Any number of rows.
};
```

Insert after `struct ForEach { … };`:

```cpp
/// @brief One column of a `Table`.
struct TableColumn {
    /// @brief The header caption.
    std::string label;
    /// @brief The column's width request.
    Sizing width = Sizing::content();
};

/// @brief Keyed rows of cells under column headers. Each row's view is a `Row` whose children are the cells, one
///        per column.
struct Table {
    /// @brief The columns, in order; set once.
    std::vector<TableColumn> columns;
    /// @brief The rows, the key function and the row view, type-erased; null shows no rows.
    std::shared_ptr<detail::ForEachModel const> rows;
    /// @brief How many rows the user can select; set once.
    SelectionMode selectionMode = SelectionMode::None;
    /// @brief The selected rows' keys. A key with no row selects nothing until such a row appears.
    Prop<std::vector<Key>> selection;
    /// @brief Called with the keys the user selected.
    std::function<void(std::vector<Key>)> onSelectionChange;
    /// @brief Called with a row's key when the user activates it (Enter or a double click).
    std::function<void(Key)> onActivate;
    /// @brief Visibility, enablement, layout, drag-and-drop.
    Common common{};
};
```

Replace `NodeData`'s member with the final palette:

```cpp
    /// @brief The node.
    std::variant<Text, Button, TextInput, Checkbox, Select, Menu, Column, Row, Grid, Spacer, Panel, Scroll, Switch,
                 Tabs, Dialog, Busy, ForEach, Table, DateTimeInput, Slider, FilePicker>
        kind;
```

Append after the two `forEach` overloads:

```cpp
/// @brief Everything about a `table` beyond its columns and rows.
struct TableOptions {
    /// @brief How many rows the user can select.
    SelectionMode selectionMode = SelectionMode::None;
    /// @brief The selected rows' keys.
    Prop<std::vector<Key>> selection;
    /// @brief Called with the keys the user selected.
    std::function<void(std::vector<Key>)> onSelectionChange;
    /// @brief Called with a row's key when the user activates it.
    std::function<void(Key)> onActivate;
    /// @brief Visibility, enablement, layout, drag-and-drop of the table.
    Common common{};
};

/// @brief A table of keyed rows: one `Row` of cells per row, under the column headers.
/// @tparam RowT The row type; copyable.
/// @param columns The columns, in order.
/// @param rows Reads the rows; every signal it reads is a dependency.
/// @param keyOf A row's identity; a later duplicate of a key is refused at mount.
/// @param cells Builds a row's cells once, one per column, from a signal the mount keeps equal to the row.
/// @param options Selection, activation and `Common`.
/// @return The node.
template <typename RowT>
[[nodiscard]] Node table(std::vector<TableColumn> columns, std::function<std::vector<RowT>()> rows,
                         std::function<Key(RowT const&)> keyOf,
                         std::function<std::vector<Node>(reactive::Signal<RowT> const&)> cells,
                         TableOptions options = {}) {
    std::function<Node(reactive::Signal<RowT> const&)> rowView =
        [cellsOf = std::move(cells)](reactive::Signal<RowT> const& entry) {
            return row(Row{.children = cellsOf(entry)});
        };
    return detail::makeNode(Table{
        .columns = std::move(columns),
        .rows = std::make_shared<detail::TypedForEach<RowT> const>(std::move(rows), std::move(keyOf),
                                                                   std::move(rowView)),
        .selectionMode = options.selectionMode,
        .selection = std::move(options.selection),
        .onSelectionChange = std::move(options.onSelectionChange),
        .onActivate = std::move(options.onActivate),
        .common = std::move(options.common),
    });
}
```

- [ ] **Step 4: Add the widget interface**

In `include/morph/ui/backend.hpp`, insert after `class BusyWidget { … };`:

```cpp
/// @brief Keyed rows under column headers, with selection and activation by key.
///
/// Its children are the rows, each a horizontal stack whose children are the cells in column order. Selection is
/// kept by key: it survives a reorder and applies to a row that appears later.
class TableWidget : public ContainerWidget {
public:
    /// @brief Sets the column headers and widths.
    /// @param columns The columns, in order.
    virtual void setColumns(std::vector<TableColumn> const& columns) = 0;

    /// @brief Sets how many rows the user can select.
    /// @param mode None, one, or any number.
    virtual void setSelectionMode(SelectionMode mode) = 0;

    /// @brief Tells the table which key a row stands for; called once per row, right after it is built.
    /// @param row One of this table's rows.
    /// @param key The row's key.
    virtual void setRowKey(Widget& row, Key const& key) = 0;

    /// @brief Marks the rows with these keys selected. Never calls the `onSelectionChange` handler.
    /// @param keys The selected keys.
    virtual void setSelection(std::vector<Key> const& keys) = 0;

    /// @brief Sets what a user's selection change calls, with the selected keys.
    /// @param onSelectionChange The handler; empty does nothing.
    virtual void setOnSelectionChange(std::function<void(std::vector<Key>)> onSelectionChange) = 0;

    /// @brief Sets what activating a row calls, with its key.
    /// @param onActivate The handler; empty does nothing.
    virtual void setOnActivate(std::function<void(Key)> onActivate) = 0;
};
```

and in `IViewBackend`, after `createBusy`:

```cpp
    /// @brief Makes a table.
    /// @param parent The container to append to, or null for a root.
    /// @return The widget.
    [[nodiscard]] virtual std::unique_ptr<TableWidget> createTable(ContainerWidget* parent) = 0;
```

- [ ] **Step 5: Teach `RecordingBackend` the table**

In `include/morph/ui/testing/recording_backend.hpp`:

After the `enumName(FilePickerMode)` overload:

```cpp
/// @brief A selection mode's name.
/// @param mode The mode.
/// @return The enumerator's name.
[[nodiscard]] inline std::string enumName(SelectionMode mode) {
    switch (mode) {
    case SelectionMode::None:
        return "None";
    case SelectionMode::Single:
        return "Single";
    case SelectionMode::Multiple:
        return "Multiple";
    default:
        return "unknown";
    }
}
```

In `struct Callbacks`, after `dismiss`:

```cpp
    /// @brief From `TableWidget::setOnSelectionChange`.
    std::function<void(std::vector<Key>)> selection;
    /// @brief From `TableWidget::setOnActivate`.
    std::function<void(Key)> activate;
```

After `class FakeDialog { … };`:

```cpp
/// @brief A fake `TableWidget`: properties `columns`, `selectionMode`, `selection`, and `rowKey` on a row.
class FakeTable final : public FakeContainer<TableWidget> {
public:
    using FakeContainer<TableWidget>::FakeContainer;
    void setColumns(std::vector<TableColumn> const& columns) override {
        set("columns", formatList(columns, [](TableColumn const& column) {
                return column.label + ":" + formatSizing(column.width);
            }));
    }
    void setSelectionMode(SelectionMode mode) override { set("selectionMode", enumName(mode)); }
    void setRowKey(Widget& row, Key const& key) override { store().set(store().idOf(row), "rowKey", formatKey(key)); }
    void setSelection(std::vector<Key> const& keys) override {
        set("selection", formatList(keys, [](Key const& key) { return formatKey(key); }));
    }
    void setOnSelectionChange(std::function<void(std::vector<Key>)> onSelectionChange) override {
        callbacks().selection = std::move(onSelectionChange);
    }
    void setOnActivate(std::function<void(Key)> onActivate) override { callbacks().activate = std::move(onActivate); }
};
```

In `RecordingBackend`, after `createBusy`:

```cpp
    [[nodiscard]] std::unique_ptr<TableWidget> createTable(ContainerWidget* parent) override {
        return std::make_unique<detail::FakeTable>(_store, "Table", parent);
    }
```

and after `dismiss`:

```cpp
    /// @brief Selects rows of a table by key.
    /// @param widgetId The table.
    /// @param keys The keys the user selected.
    void selectRows(int widgetId, std::vector<Key> keys) {
        if (detail::Record const* const record = actionable(widgetId); record != nullptr) {
            _store->setByUser(widgetId, "selection",
                              detail::formatList(keys, [](Key const& key) { return detail::formatKey(key); }));
            invoke(record->callbacks.selection, std::move(keys));
        }
    }

    /// @brief Activates a row of a table (Enter or a double click).
    /// @param widgetId The table.
    /// @param key The row's key.
    void activateRow(int widgetId, Key key) {
        if (detail::Record const* const record = actionable(widgetId); record != nullptr) {
            invoke(record->callbacks.activate, std::move(key));
        }
    }
```

- [ ] **Step 6: Mount it**

In `include/morph/ui/mount.hpp`, inside `class Mounter`, after the ForEach `mountKind`:

```cpp
    // The rows are mounted before the selection is applied, so the first setSelection names rows that exist.
    Widget& mountKind(reactive::Scope& scope, Table const& spec, ContainerWidget* parent) {
        TableWidget& widget = scope.adopt(_backend->createTable(parent));
        applyCommon(scope, widget, spec.common);
        widget.setColumns(spec.columns);
        widget.setSelectionMode(spec.selectionMode);
        widget.setOnSelectionChange(event(spec.onSelectionChange));
        widget.setOnActivate(event(spec.onActivate));
        mountRows(scope, spec.rows, widget, [&widget](Widget& rowWidget, Key const& key) {
            widget.setRowKey(rowWidget, key);
        });
        bind(scope, spec.selection, [&widget](std::vector<Key> const& keys) { widget.setSelection(keys); });
        return widget;
    }
```

- [ ] **Step 7: Run the tests to verify they pass**

Run: `cmake --build build/reactive --target morph_tests && ./build/reactive/tests/morph_tests "[ui]"`
Expected: PASS.

Mutation checks, one at a time, each restored afterwards:

| Change | Test that must fail |
|---|---|
| In the Table `mountKind`, pass `{}` instead of the `setRowKey` hook | "columns, keyed rows of cells, selection after the rows" |
| Move the `selection` `bind` above `mountRows` | "columns, keyed rows of cells, selection after the rows" (the last log line is `set Row#5 rowKey=2`) |
| In `FakeTable::setRowKey`, record nothing | "a new row is told its key once; a reorder only moves" |

- [ ] **Step 8: Commit**

```bash
git add include/morph/ui tests/test_ui_table.cpp tests/CMakeLists.txt
git commit -m "wip(ui): table of keyed rows with selection and activation by key

Signed-off-by: Christian Parpart <christian@parpart.family>"
```

---

### Task 7: The frontend seam — `AppContext`, `Application`, `Frontend`, `selectFrontend`

An application is a factory the frontend calls with an `AppContext`; `main` is the composition root that lists the
frontends it was built with and lets `selectFrontend` pick one. The order `Frontend::run` guarantees (runtime
first, application destroyed before it) is a property of each frontend, so Parts 3 and 4 pin it in their own
frontend tests — Part 3's "tui::Frontend: the application is destroyed before the runtime" and Part 4's "qt_quick
Frontend: the mount dies before the application, the application before the runtime, Qt last" — and Task 9's
`frontend.md` names both; this task pins the selection.

**Files:**
- Create: `include/morph/ui/frontend.hpp`
- Modify: `CMakeLists.txt` — `FILE_SET HEADERS`, after `include/morph/ui/mount.hpp`: `include/morph/ui/frontend.hpp`
- Modify: `tests/CMakeLists.txt` — add `test_ui_frontend.cpp` after `test_ui_table.cpp`
- Test: `tests/test_ui_frontend.cpp`

**Interfaces:**
- Consumes: `reactive::Runtime`, `reactive::Scheduler`, `reactive::TimerHandle` (Part 1, `scheduler.hpp`);
  `exec::IExecutor` (`include/morph/core/executor.hpp:68`, a `struct`); `exec::IoLoop`
  (`include/morph/core/io_loop.hpp:54`, a `class`, forward-declared here so `ui` does not pull in core-cpp's
  event loop).
- Produces, exactly as the contract declares them: `ui::Scheduler`, `ui::TimerHandle`, `ui::AppContext`,
  `ui::Application`, `ui::ApplicationFactory`, `ui::Frontend`, `ui::FrontendOption{name, usable, make}`,
  `ui::FrontendSelectionError`, `ui::EnvironmentReader`, `ui::processEnvironment()`, `ui::selectFrontend(…)`.
  Precedence: the last `--ui=<name>` or `--ui <name>` on the command line; else a non-empty `MORPH_UI`; else the
  first option, in the order given, whose `usable()` holds (an empty `usable` counts as usable). A named frontend
  is used even when its `usable()` is false. Errors, exactly:
  `morph::ui: no frontend named '<name>' is built (built: <a, b>)`,
  `morph::ui: no built frontend is usable here (built: <a, b>)`,
  `morph::ui: --ui needs a frontend name (built: <a, b>)`,
  `morph::ui: frontend '<name>' could not be made` — `<a, b>` is `none` when nothing is built.

- [ ] **Step 1: Write the failing test**

Create `tests/test_ui_frontend.cpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0

#include <catch2/catch_test_macros.hpp>
#include <memory>
#include <morph/ui/frontend.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ui = morph::ui;

namespace {

class NamedFrontend final : public ui::Frontend {
public:
    explicit NamedFrontend(std::string name) : _name{std::move(name)} {}
    [[nodiscard]] std::string_view name() const override { return _name; }
    int run(ui::ApplicationFactory const&) override { return 0; }

private:
    std::string _name;
};

ui::FrontendOption option(std::string const& name, bool usable) {
    return ui::FrontendOption{
        .name = name,
        .usable = [usable] { return usable; },
        .make = [name] { return std::unique_ptr<ui::Frontend>{std::make_unique<NamedFrontend>(name)}; },
    };
}

ui::EnvironmentReader morphUi(std::optional<std::string> value) {
    return [value = std::move(value)](std::string_view name) { return name == "MORPH_UI" ? value : std::nullopt; };
}

std::string chosen(std::vector<ui::FrontendOption> const& built, std::vector<char const*> args,
                   ui::EnvironmentReader const& env) {
    return std::string{ui::selectFrontend(built, static_cast<int>(args.size()), args.data(), env)->name()};
}

std::string selectionError(std::vector<ui::FrontendOption> const& built, std::vector<char const*> args,
                           ui::EnvironmentReader const& env) {
    try {
        static_cast<void>(ui::selectFrontend(built, static_cast<int>(args.size()), args.data(), env));
    } catch (ui::FrontendSelectionError const& error) {
        return error.what();
    }
    return "no error";
}

std::vector<ui::FrontendOption> qtThenTui(bool qtUsable = true, bool tuiUsable = true) {
    return {option("qt", qtUsable), option("tui", tuiUsable)};
}

}  // namespace

TEST_CASE("ui::selectFrontend: --ui= wins over MORPH_UI and over the order", "[ui]") {
    CHECK(chosen(qtThenTui(), {"app", "--ui=tui"}, morphUi("qt")) == "tui");
}

TEST_CASE("ui::selectFrontend: --ui takes its name from the next argument", "[ui]") {
    CHECK(chosen(qtThenTui(), {"app", "--verbose", "--ui", "tui"}, morphUi(std::nullopt)) == "tui");
}

TEST_CASE("ui::selectFrontend: the last --ui wins", "[ui]") {
    CHECK(chosen(qtThenTui(), {"app", "--ui=qt", "--ui=tui"}, morphUi(std::nullopt)) == "tui");
}

TEST_CASE("ui::selectFrontend: MORPH_UI wins over the order, and an empty one is ignored", "[ui]") {
    CHECK(chosen(qtThenTui(), {"app"}, morphUi("tui")) == "tui");
    CHECK(chosen(qtThenTui(), {"app"}, morphUi("")) == "qt");
}

TEST_CASE("ui::selectFrontend: otherwise the first usable option, in the order given", "[ui]") {
    CHECK(chosen(qtThenTui(), {"app"}, morphUi(std::nullopt)) == "qt");
    CHECK(chosen(qtThenTui(false, true), {"app"}, morphUi(std::nullopt)) == "tui");
    std::vector<ui::FrontendOption> const unconditional{{.name = "web", .usable = {}, .make = [] {
                                                             return std::unique_ptr<ui::Frontend>{
                                                                 std::make_unique<NamedFrontend>("web")};
                                                         }}};
    CHECK(chosen(unconditional, {"app"}, morphUi(std::nullopt)) == "web");
}

TEST_CASE("ui::selectFrontend: a named frontend is used even when it is not usable", "[ui]") {
    CHECK(chosen(qtThenTui(false, true), {"app", "--ui=qt"}, morphUi(std::nullopt)) == "qt");
}

TEST_CASE("ui::selectFrontend: an unknown or unbuilt name lists the built frontends", "[ui]") {
    CHECK(selectionError(qtThenTui(), {"app", "--ui=web"}, morphUi(std::nullopt)) ==
          "morph::ui: no frontend named 'web' is built (built: qt, tui)");
    CHECK(selectionError(qtThenTui(), {"app"}, morphUi("web")) ==
          "morph::ui: no frontend named 'web' is built (built: qt, tui)");
}

TEST_CASE("ui::selectFrontend: nothing usable is an error", "[ui]") {
    CHECK(selectionError(qtThenTui(false, false), {"app"}, morphUi(std::nullopt)) ==
          "morph::ui: no built frontend is usable here (built: qt, tui)");
    CHECK(selectionError({}, {"app"}, morphUi(std::nullopt)) ==
          "morph::ui: no built frontend is usable here (built: none)");
}

TEST_CASE("ui::selectFrontend: --ui without a name is an error", "[ui]") {
    CHECK(selectionError(qtThenTui(), {"app", "--ui"}, morphUi("tui")) ==
          "morph::ui: --ui needs a frontend name (built: qt, tui)");
    CHECK(selectionError(qtThenTui(), {"app", "--ui="}, morphUi("tui")) ==
          "morph::ui: --ui needs a frontend name (built: qt, tui)");
}

TEST_CASE("ui::selectFrontend: an option that makes nothing is an error", "[ui]") {
    std::vector<ui::FrontendOption> const broken{
        {.name = "tui", .usable = [] { return true; }, .make = [] { return std::unique_ptr<ui::Frontend>{}; }}};
    CHECK(selectionError(broken, {"app"}, morphUi(std::nullopt)) == "morph::ui: frontend 'tui' could not be made");
}

TEST_CASE("ui::processEnvironment: reads the process environment", "[ui]") {
    auto const env = ui::processEnvironment();
    CHECK(env("PATH").has_value());
    CHECK_FALSE(env("MORPH_UI_TEST_VARIABLE_THAT_IS_NEVER_SET").has_value());
}
```

Register it after `test_ui_table.cpp`.

- [ ] **Step 2: Run the test to verify it fails**

Run: `cmake --build build/reactive --target morph_tests`
Expected: FAIL — `fatal error: 'morph/ui/frontend.hpp' file not found`.

- [ ] **Step 3: Implement the seam**

Create `include/morph/ui/frontend.hpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0

#pragma once
#include <algorithm>
#include <cstddef>
#include <cstdlib>
#include <functional>
#include <memory>
#include <optional>
#include <ranges>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include "../core/executor.hpp"
#include "../reactive/runtime.hpp"
#include "../reactive/scheduler.hpp"
#include "view.hpp"

/// @file
/// @brief The frontend seam: an application is a factory the frontend calls, and `main` picks the frontend at
///        runtime.
///
/// Specified in `docs/spec/ui/frontend.md`.

namespace morph::exec {
class IoLoop;
}  // namespace morph::exec

namespace morph::ui {

/// @brief Timers on the frontend's owner executor; see `reactive::Scheduler`.
using Scheduler = reactive::Scheduler;

/// @brief Owns one scheduled timer and cancels it when destroyed; see `reactive::TimerHandle`.
using TimerHandle = reactive::TimerHandle;

/// @brief What a frontend hands the application it runs.
class AppContext {
public:
    AppContext() = default;
    virtual ~AppContext() = default;
    AppContext(AppContext const&) = delete;
    AppContext& operator=(AppContext const&) = delete;
    AppContext(AppContext&&) = delete;
    AppContext& operator=(AppContext&&) = delete;

    /// @brief The runtime the view is mounted in. Owned by the frontend; it outlives the application.
    /// @return The runtime.
    virtual reactive::Runtime& runtime() = 0;

    /// @brief The runtime's owner, and the callback executor of every `BridgeHandler` the application makes.
    /// @return The executor.
    virtual exec::IExecutor& executor() = 0;

    /// @brief Timers that fire on the owner executor.
    /// @return The scheduler.
    virtual Scheduler& scheduler() = 0;

    /// @brief The I/O loop the frontend runs on, for sockets and timers that share its one thread.
    /// @return The loop, or null when the frontend has none (Qt Quick).
    virtual exec::IoLoop* ioLoop() = 0;

    /// @brief Makes `Frontend::run` return once the current event has been handled.
    /// @param exitCode What `run` returns.
    virtual void quit(int exitCode = 0) = 0;

    /// @brief The running frontend's name.
    /// @return The name `selectFrontend` matched, such as `tui` or `qt`.
    [[nodiscard]] virtual std::string_view frontendName() const = 0;
};

/// @brief An application: its controllers, handlers and bridge wiring, and the view over them.
class Application {
public:
    Application() = default;
    virtual ~Application() = default;
    Application(Application const&) = delete;
    Application& operator=(Application const&) = delete;
    Application(Application&&) = delete;
    Application& operator=(Application&&) = delete;

    /// @brief The view tree to mount; called once, after construction.
    /// @return The root node; never null.
    [[nodiscard]] virtual Node view() = 0;
};

/// @brief Makes the application, given the running frontend's context.
using ApplicationFactory = std::function<std::unique_ptr<Application>(AppContext&)>;

/// @brief A frontend: owns an event loop and renders one application's view.
class Frontend {
public:
    Frontend() = default;
    virtual ~Frontend() = default;
    Frontend(Frontend const&) = delete;
    Frontend& operator=(Frontend const&) = delete;
    Frontend(Frontend&&) = delete;
    Frontend& operator=(Frontend&&) = delete;

    /// @brief The name `selectFrontend` matches.
    /// @return The name.
    [[nodiscard]] virtual std::string_view name() const = 0;

    /// @brief Builds the runtime and its executor, calls @p factory, mounts the application's view, and drives the
    ///        loop until `AppContext::quit`. Tears down in reverse: the mounted view, then the application, then
    ///        the runtime.
    /// @param factory Makes the application.
    /// @return The exit code passed to `quit`.
    virtual int run(ApplicationFactory const& factory) = 0;
};

/// @brief One frontend a binary was built with.
struct FrontendOption {
    /// @brief The name `--ui=` and `MORPH_UI` match.
    std::string name;
    /// @brief Whether the frontend can run here (a terminal on stdin, a display); empty counts as usable.
    std::function<bool()> usable;
    /// @brief Makes the frontend; must not return null.
    std::function<std::unique_ptr<Frontend>()> make;
};

/// @brief No frontend could be selected; the message names the built ones.
class FrontendSelectionError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

/// @brief Reads an environment variable: its value, or `nullopt` when it is unset.
using EnvironmentReader = std::function<std::optional<std::string>(std::string_view name)>;

/// @brief The process environment, read through `std::getenv`.
/// @return A reader over the process environment.
[[nodiscard]] inline EnvironmentReader processEnvironment() {
    return [](std::string_view name) -> std::optional<std::string> {
        std::string const key{name};
        // getenv races only with a concurrent setenv; frontend selection runs in main before any thread exists.
        char const* const value = std::getenv(key.c_str());  // NOLINT(concurrency-mt-unsafe)
        if (value == nullptr) {
            return std::nullopt;
        }
        return std::string{value};
    };
}

namespace detail {

/// @brief The built frontends' names, for an error message.
/// @param built The options.
/// @return The names joined by `, `, or `none`.
[[nodiscard]] inline std::string builtNames(std::span<FrontendOption const> built) {
    if (built.empty()) {
        return "none";
    }
    std::string names;
    for (FrontendOption const& option : built) {
        if (!names.empty()) {
            names += ", ";
        }
        names += option.name;
    }
    return names;
}

/// @brief The name the command line asks for: the last `--ui=<name>` or `--ui <name>`.
/// @param built The options, for the error message.
/// @param argc The argument count, as `main` received it.
/// @param argv The arguments, as `main` received them; `argv[0]` is skipped.
/// @return The name, or `nullopt` when no `--ui` is given.
/// @throws FrontendSelectionError for a `--ui` without a name.
[[nodiscard]] inline std::optional<std::string> commandLineFrontend(std::span<FrontendOption const> built, int argc,
                                                                    char const* const* argv) {
    std::size_t const count = argv == nullptr || argc <= 0 ? 0 : static_cast<std::size_t>(argc);
    std::span<char const* const> const args{argv, count};
    std::optional<std::string> name;
    bool nameFollows = false;
    for (char const* const raw : args | std::views::drop(1)) {
        std::string_view const arg = raw == nullptr ? std::string_view{} : std::string_view{raw};
        if (nameFollows) {
            name = std::string{arg};
            nameFollows = false;
        } else if (arg == "--ui") {
            nameFollows = true;
        } else if (arg.starts_with("--ui=")) {
            name = std::string{arg.substr(std::string_view{"--ui="}.size())};
        }
    }
    if (nameFollows || (name.has_value() && name->empty())) {
        throw FrontendSelectionError{"morph::ui: --ui needs a frontend name (built: " + builtNames(built) + ")"};
    }
    return name;
}

}  // namespace detail

/// @brief Picks the frontend to run: the last `--ui=<name>` (or `--ui <name>`), else a non-empty `MORPH_UI`, else
///        the first option in the given order whose `usable()` holds.
///
/// A frontend named on the command line or in `MORPH_UI` is used even when its `usable()` is false: the user asked
/// for it.
/// @param built The frontends this binary was built with, in order of preference.
/// @param argc The argument count, as `main` received it.
/// @param argv The arguments, as `main` received them.
/// @param env Reads `MORPH_UI`; the process environment by default.
/// @return The frontend, made.
/// @throws FrontendSelectionError for an unknown or unbuilt name, a `--ui` without one, no usable option, or an
///         option that makes nothing; the message names the built frontends.
[[nodiscard]] inline std::unique_ptr<Frontend> selectFrontend(std::span<FrontendOption const> built, int argc,
                                                              char const* const* argv,
                                                              EnvironmentReader const& env = processEnvironment()) {
    std::optional<std::string> requested = detail::commandLineFrontend(built, argc, argv);
    if (!requested.has_value() && env) {
        if (std::optional<std::string> fromEnvironment = env("MORPH_UI");
            fromEnvironment.has_value() && !fromEnvironment->empty()) {
            requested = std::move(fromEnvironment);
        }
    }
    FrontendOption const* chosen = nullptr;
    if (requested.has_value()) {
        auto const found = std::ranges::find(built, *requested, &FrontendOption::name);
        if (found == built.end()) {
            throw FrontendSelectionError{"morph::ui: no frontend named '" + *requested + "' is built (built: " +
                                         detail::builtNames(built) + ")"};
        }
        chosen = &*found;
    } else {
        auto const found = std::ranges::find_if(
            built, [](FrontendOption const& candidate) { return !candidate.usable || candidate.usable(); });
        if (found == built.end()) {
            throw FrontendSelectionError{"morph::ui: no built frontend is usable here (built: " +
                                         detail::builtNames(built) + ")"};
        }
        chosen = &*found;
    }
    std::unique_ptr<Frontend> frontend = chosen->make ? chosen->make() : nullptr;
    if (frontend == nullptr) {
        throw FrontendSelectionError{"morph::ui: frontend '" + chosen->name + "' could not be made"};
    }
    return frontend;
}

}  // namespace morph::ui
```

Register it after `include/morph/ui/mount.hpp` in `CMakeLists.txt`'s `FILE_SET HEADERS`.

- [ ] **Step 4: Run the tests to verify they pass**

Run: `cmake --build build/reactive --target morph_tests && ./build/reactive/tests/morph_tests "[ui]"`
Expected: PASS.

Mutation checks, one at a time, each restored afterwards:

| Change | Test that must fail |
|---|---|
| In `selectFrontend`, read `MORPH_UI` first and the command line only when it is unset | "--ui= wins over MORPH_UI and over the order" |
| In the `find_if` predicate, return `true` unconditionally | "otherwise the first usable option, in the order given" |
| In `commandLineFrontend`, `return name;` before the `nameFollows` check | "--ui without a name is an error" |

- [ ] **Step 5: Commit**

```bash
git add include/morph/ui/frontend.hpp tests/test_ui_frontend.cpp tests/CMakeLists.txt CMakeLists.txt
git commit -m "wip(ui): the frontend seam and selectFrontend

Signed-off-by: Christian Parpart <christian@parpart.family>"
```

---

### Task 8: The backend-conformance suite

The same scripted cases run against every backend: `RecordingBackend` here, the TUI backend in Part 3, Qt Quick in
Part 4. A backend author implements a `ConformanceProbe` (how to read a widget's text, visibility and children, how
to click, type and drag, how to run pending work) and loops over `conformanceCases()`.

**Files:**
- Create: `include/morph/ui/testing/backend_conformance.hpp`
- Modify: `CMakeLists.txt` — `FILE_SET HEADERS`, after `include/morph/ui/testing/recording_backend.hpp`:
  `include/morph/ui/testing/backend_conformance.hpp`
- Modify: `tests/CMakeLists.txt` — add `test_ui_conformance.cpp` after `test_ui_frontend.cpp`
- Test: `tests/test_ui_conformance.cpp`

**Interfaces:**
- Consumes: `Mounted`, the builders, `reactive::Signal`, `Runtime::batch` (Tasks 1–6, Part 1).
- Produces: `ui::testing::ConformanceProbe` (`backend`, `runtime`, `settle`, `textOf`, `visibleOf`, `enabledOf`,
  `childCount`, `childAt`, `click`, `type`, `drag`), `ui::testing::ConformanceCase{name, run}`,
  `ui::testing::conformanceCases()` — thirteen cases; `ui::testing::detail::{Checks, Entry, entryList,
  asContainer, childTexts}` and one `detail` function per case. `textOf` is a Text's or TextInput's text and a
  Button's or Checkbox's label. A case reaches a widget only through `Mounted::root()`, `childAt` and a second
  `Mounted` root (drag), so a probe never needs a widget lookup.

- [ ] **Step 1: Write the failing test**

Create `tests/test_ui_conformance.cpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0

#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <morph/reactive/runtime.hpp>
#include <morph/ui/backend.hpp>
#include <morph/ui/testing/backend_conformance.hpp>
#include <morph/ui/testing/recording_backend.hpp>
#include <optional>
#include <set>
#include <string>
#include <string_view>

#include "test_support.hpp"

namespace ui = morph::ui;

namespace {

// The reference probe: what a TUI or Qt Quick probe does with a rendered screen or a QQuickItem tree, this one does
// with the recording backend's records.
class RecordingProbe final : public ui::testing::ConformanceProbe {
public:
    ui::IViewBackend& backend() override { return _backend; }
    morph::reactive::Runtime& runtime() override { return _rt; }
    void settle() override { _owner.runAll(); }

    [[nodiscard]] std::string textOf(ui::Widget const& widget) override {
        int const widgetId = _backend.idOf(widget);
        std::string const kind = _backend.kindOf(widgetId);
        return _backend.prop(widgetId, kind == "Button" || kind == "Checkbox" ? "label" : "text");
    }
    [[nodiscard]] bool visibleOf(ui::Widget const& widget) override {
        return _backend.prop(_backend.idOf(widget), "visible") != "false";
    }
    [[nodiscard]] bool enabledOf(ui::Widget const& widget) override {
        return _backend.prop(_backend.idOf(widget), "enabled") != "false";
    }
    [[nodiscard]] std::size_t childCount(ui::ContainerWidget const& container) override {
        return _backend.children(_backend.idOf(container)).size();
    }
    [[nodiscard]] ui::Widget const* childAt(ui::ContainerWidget const& container, std::size_t index) override {
        auto const children = _backend.children(_backend.idOf(container));
        return index < children.size() ? _backend.widget(children.at(index)) : nullptr;
    }
    void click(ui::Widget& widget) override { _backend.click(_backend.idOf(widget)); }
    void type(ui::Widget& widget, std::string_view text) override {
        _backend.edit(_backend.idOf(widget), std::string{text});
    }
    void drag(ui::Widget& source, ui::Widget& target) override {
        static_cast<void>(_backend.drag(_backend.idOf(source), _backend.idOf(target)));
    }

private:
    morph::testing::StepExecutor _owner;
    ui::testing::RecordingBackend _backend;
    morph::reactive::Runtime _rt{_owner};
};

}  // namespace

TEST_CASE("ui conformance: RecordingBackend passes every case", "[ui][conformance]") {
    auto const cases = ui::testing::conformanceCases();
    REQUIRE(cases.size() == 13);
    std::set<std::string_view> names;
    for (auto const& testCase : cases) {
        CHECK(names.insert(testCase.name).second);
        RecordingProbe probe;
        std::optional<std::string> const failure = testCase.run(probe);
        INFO(std::string{testCase.name} + ": " + failure.value_or("passed"));
        CHECK_FALSE(failure.has_value());
    }
}
```

Register it after `test_ui_frontend.cpp`.

- [ ] **Step 2: Run the test to verify it fails**

Run: `cmake --build build/reactive --target morph_tests`
Expected: FAIL — `fatal error: 'morph/ui/testing/backend_conformance.hpp' file not found`.

- [ ] **Step 3: Write the cases**

Create `include/morph/ui/testing/backend_conformance.hpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0

#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "../../reactive/runtime.hpp"
#include "../../reactive/signal.hpp"
#include "../backend.hpp"
#include "../mount.hpp"
#include "../view.hpp"

/// @file
/// @brief The scripted cases every `IViewBackend` must pass, and the probe a backend author implements to run them.
///
/// Specified in `docs/spec/ui/backend_contract.md`, "The conformance suite".

namespace morph::ui::testing {

/// @brief How the conformance cases read and drive one backend under test.
class ConformanceProbe {
public:
    ConformanceProbe() = default;
    virtual ~ConformanceProbe() = default;
    ConformanceProbe(ConformanceProbe const&) = delete;
    ConformanceProbe& operator=(ConformanceProbe const&) = delete;
    ConformanceProbe(ConformanceProbe&&) = delete;
    ConformanceProbe& operator=(ConformanceProbe&&) = delete;

    /// @brief The backend under test.
    /// @return The backend every case mounts on.
    virtual IViewBackend& backend() = 0;

    /// @brief The runtime the cases' signals and mounts belong to.
    /// @return The runtime, whose owner `settle` drives.
    virtual reactive::Runtime& runtime() = 0;

    /// @brief Runs posted flushes and pending native events until nothing is left.
    virtual void settle() = 0;

    /// @brief The text a widget shows: a Text's or TextInput's text, a Button's or Checkbox's label.
    /// @param widget The widget.
    /// @return The text, UTF-8.
    [[nodiscard]] virtual std::string textOf(Widget const& widget) = 0;

    /// @brief The widget's own visibility flag, as its last `setVisible` left it.
    ///
    /// Only the widget's own flag counts: a hidden ancestor, a collapsed panel or a closed dialog around it does
    /// not make this false. That is what `RecordingBackend` records, and it lets a case tell which Tabs page slot
    /// the mount hid.
    /// @param widget The widget.
    /// @return True for a new widget; false once `setVisible(false)` reached it and until `setVisible(true)` does.
    [[nodiscard]] virtual bool visibleOf(Widget const& widget) = 0;

    /// @brief Whether a widget accepts input, by its own flag: a disabled ancestor does not make this false, which
    ///        is what `RecordingBackend` records.
    /// @param widget The widget.
    /// @return True for a new widget; false once `setEnabled(false)` reached it and until `setEnabled(true)` does.
    [[nodiscard]] virtual bool enabledOf(Widget const& widget) = 0;

    /// @brief How many children a container has.
    /// @param container The container.
    /// @return The count.
    [[nodiscard]] virtual std::size_t childCount(ContainerWidget const& container) = 0;

    /// @brief A container's child.
    /// @param container The container.
    /// @param index The position.
    /// @return The child, or null past the end.
    [[nodiscard]] virtual Widget const* childAt(ContainerWidget const& container, std::size_t index) = 0;

    /// @brief Activates a button as a user would.
    /// @param widget The button.
    virtual void click(Widget& widget) = 0;

    /// @brief Replaces a text field's text as a user typing would; `onChange` may see intermediate texts.
    /// @param widget The field.
    /// @param text The text to end up with.
    virtual void type(Widget& widget, std::string_view text) = 0;

    /// @brief Drags one widget onto another as a user would.
    /// @param source The dragged widget.
    /// @param target The widget it is dropped on.
    virtual void drag(Widget& source, Widget& target) = 0;
};

/// @brief One scripted case.
struct ConformanceCase {
    /// @brief What the case checks; unique among the cases.
    std::string_view name;
    /// @brief Runs the case on a fresh probe.
    std::function<std::optional<std::string>(ConformanceProbe&)> run;
};

namespace detail {

/// @brief Collects a case's checks and keeps the first failure, so a case reads top to bottom.
class Checks {
public:
    /// @brief Checks that a text is what it should be.
    /// @param what What is checked, for the message.
    /// @param expected The text it should be.
    /// @param actual The text it is.
    // NOLINTNEXTLINE(bugprone-easily-swappable-parameters) -- a label, then expected before actual
    void text(std::string_view what, std::string_view expected, std::string_view actual) {
        if (expected != actual) {
            fail(std::string{what} + ": expected '" + std::string{expected} + "', got '" + std::string{actual} + "'");
        }
    }

    /// @brief Checks a condition.
    /// @param holds The condition.
    /// @param what The failure message when it does not hold.
    void that(bool holds, std::string_view what) {
        if (!holds) {
            fail(std::string{what});
        }
    }

    /// @brief The case's verdict.
    /// @return `nullopt` when every check held, else the first failure.
    [[nodiscard]] std::optional<std::string> result() const { return _failure; }

private:
    void fail(std::string message) {
        if (!_failure.has_value()) {
            _failure = std::move(message);
        }
    }

    std::optional<std::string> _failure;
};

/// @brief A row of the ForEach cases.
struct Entry {
    /// @brief The row's key.
    std::int64_t id = 0;
    /// @brief The text the row shows.
    std::string label;

    /// @brief Memberwise equality.
    /// @return Whether both fields match.
    bool operator==(Entry const&) const = default;
};

/// @brief A ForEach of Texts, one per entry, keyed by id.
/// @param rows The entries; must outlive the node's mounts.
/// @return The node.
[[nodiscard]] inline Node entryList(reactive::Signal<std::vector<Entry>> const& rows) {
    return ui::forEach<Entry>(
        rows, [](Entry const& entry) { return Key{entry.id}; },
        [](reactive::Signal<Entry> const& entry) { return ui::text({.text = [&entry] { return entry.get().label; }}); });
}

/// @brief A widget as a container.
/// @param widget The widget.
/// @return It, or null when it is not a `ContainerWidget`.
[[nodiscard]] inline ContainerWidget const* asContainer(Widget const& widget) {
    return dynamic_cast<ContainerWidget const*>(&widget);
}

/// @brief The texts of a container's children, joined by commas.
/// @param probe The backend under test.
/// @param container The container.
/// @return For example `a,b,c`; a missing child reads `<null>`.
[[nodiscard]] inline std::string childTexts(ConformanceProbe& probe, ContainerWidget const& container) {
    std::string texts;
    for (std::size_t i = 0; i < probe.childCount(container); ++i) {
        if (i != 0) {
            texts += ',';
        }
        Widget const* const child = probe.childAt(container, i);
        texts += child == nullptr ? std::string{"<null>"} : probe.textOf(*child);
    }
    return texts;
}

/// @brief Case: a constant Text shows its text.
/// @param probe The backend under test.
/// @return `nullopt`, or what went wrong.
[[nodiscard]] inline std::optional<std::string> textShowsItsText(ConformanceProbe& probe) {
    Mounted const view{probe.runtime(), probe.backend(), ui::text({.text = "hello"})};
    probe.settle();
    Checks checks;
    checks.text("a constant Text", "hello", probe.textOf(view.root()));
    return checks.result();
}

/// @brief Case: a bound Text follows its signal, once per batch, and not for an equal write.
/// @param probe The backend under test.
/// @return `nullopt`, or what went wrong.
[[nodiscard]] inline std::optional<std::string> boundTextUpdatesOncePerChange(ConformanceProbe& probe) {
    reactive::Runtime& runtime = probe.runtime();
    reactive::Signal<std::string> word{runtime, "one"};
    int evaluations = 0;
    Mounted const view{runtime, probe.backend(), ui::text({.text = [&word, &evaluations] {
                                                      ++evaluations;
                                                      return word.get();
                                                  }})};
    probe.settle();
    Checks checks;
    checks.text("a bound Text", "one", probe.textOf(view.root()));
    runtime.batch([&word] {
        word.set("two");
        word.set("three");
    });
    probe.settle();
    checks.text("a bound Text after a batch of two writes", "three", probe.textOf(view.root()));
    word.set("three");
    probe.settle();
    checks.that(evaluations == 2, "the binding ran " + std::to_string(evaluations) +
                                      " times; expected once at mount and once for the batch");
    return checks.result();
}

/// @brief Case: `visible` and `enabled` follow their bindings.
/// @param probe The backend under test.
/// @return `nullopt`, or what went wrong.
[[nodiscard]] inline std::optional<std::string> visibleAndEnabledFollowBindings(ConformanceProbe& probe) {
    reactive::Signal<bool> shown{probe.runtime(), true};
    Mounted const view{probe.runtime(), probe.backend(),
                       ui::button({.label = "Go",
                                   .common = {.visible = [&shown] { return shown.get(); },
                                              .enabled = [&shown] { return shown.get(); }}})};
    probe.settle();
    Checks checks;
    checks.that(probe.visibleOf(view.root()), "a Button bound visible=true is hidden");
    checks.that(probe.enabledOf(view.root()), "a Button bound enabled=true is disabled");
    shown.set(false);
    probe.settle();
    checks.that(!probe.visibleOf(view.root()), "a Button bound visible=false is shown");
    checks.that(!probe.enabledOf(view.root()), "a Button bound enabled=false is enabled");
    return checks.result();
}

/// @brief Case: a click runs the button's action once, and what it wrote reaches the screen.
/// @param probe The backend under test.
/// @return `nullopt`, or what went wrong.
[[nodiscard]] inline std::optional<std::string> clickReachesAction(ConformanceProbe& probe) {
    reactive::Signal<int> clicks{probe.runtime(), 0};
    Mounted const view{probe.runtime(), probe.backend(),
                       ui::button({.label = [&clicks] { return "clicked " + std::to_string(clicks.get()); },
                                   .onClick = [&clicks] { clicks.set(clicks.peek() + 1); }})};
    probe.settle();
    probe.click(view.root());
    probe.settle();
    Checks checks;
    checks.that(clicks.peek() == 1, "one click ran the action " + std::to_string(clicks.peek()) + " times");
    checks.text("the label bound to the click count", "clicked 1", probe.textOf(view.root()));
    return checks.result();
}

/// @brief Case: typing reaches `onChange`; a value set by the application is shown and not reported back.
/// @param probe The backend under test.
/// @return `nullopt`, or what went wrong.
[[nodiscard]] inline std::optional<std::string> typingWithoutEcho(ConformanceProbe& probe) {
    reactive::Signal<std::string> name{probe.runtime(), ""};
    int changes = 0;
    Mounted const view{probe.runtime(), probe.backend(),
                       ui::textInput({.value = [&name] { return name.get(); },
                                      .onChange =
                                          [&name, &changes](std::string text) {
                                              ++changes;
                                              name.set(std::move(text));
                                          }})};
    probe.settle();
    probe.type(view.root(), "abc");
    probe.settle();
    Checks checks;
    checks.that(changes >= 1, "typing did not reach onChange");
    checks.text("the value after typing", "abc", name.peek());
    checks.text("the field after typing", "abc", probe.textOf(view.root()));
    int const typed = changes;
    name.set("xyz");
    probe.settle();
    checks.text("the field after the application set a value", "xyz", probe.textOf(view.root()));
    checks.that(changes == typed, "a value set by the application was reported through onChange");
    return checks.result();
}

/// @brief Case: a Switch shows the selected case, nothing for a key without one, and remounts on a change.
/// @param probe The backend under test.
/// @return `nullopt`, or what went wrong.
[[nodiscard]] inline std::optional<std::string> switchShowsSelectedCase(ConformanceProbe& probe) {
    reactive::Signal<std::int64_t> which{probe.runtime(), 0};
    Mounted const view{probe.runtime(), probe.backend(),
                       ui::switchOf({.selector = [&which] { return Key{which.get()}; },
                                     .cases = {{.key = Key{std::int64_t{0}}, .node = ui::text({.text = "zero"})},
                                               {.key = Key{std::int64_t{1}}, .node = ui::text({.text = "one"})}}})};
    probe.settle();
    ContainerWidget const* const slot = asContainer(view.root());
    if (slot == nullptr) {
        return "a Switch's widget is not a ContainerWidget";
    }
    Checks checks;
    checks.text("the case for key 0", "zero", childTexts(probe, *slot));
    which.set(1);
    probe.settle();
    checks.text("the case for key 1", "one", childTexts(probe, *slot));
    which.set(7);
    probe.settle();
    checks.that(probe.childCount(*slot) == 0, "a key with no case and no fallback still shows a child");
    which.set(0);
    probe.settle();
    checks.text("the case for key 0 again", "zero", childTexts(probe, *slot));
    return checks.result();
}

/// @brief Case: Tabs mount a page on its first selection, keep it, and show only the selected one.
/// @param probe The backend under test.
/// @return `nullopt`, or what went wrong.
[[nodiscard]] inline std::optional<std::string> tabsMountPagesLazily(ConformanceProbe& probe) {
    reactive::Signal<std::size_t> selected{probe.runtime(), 0};
    Mounted const view{probe.runtime(), probe.backend(),
                       ui::tabs({.tabs = {{.label = "A", .node = ui::text({.text = "first"})},
                                          {.label = "B", .node = ui::text({.text = "second"})}},
                                 .selected = [&selected] { return selected.get(); },
                                 .onSelect = [&selected](std::size_t index) { selected.set(index); }})};
    probe.settle();
    ContainerWidget const* const bar = asContainer(view.root());
    if (bar == nullptr) {
        return "a Tabs' widget is not a ContainerWidget";
    }
    Checks checks;
    checks.that(probe.childCount(*bar) == 1, "Tabs mounted a page that was never selected");
    selected.set(1);
    probe.settle();
    checks.that(probe.childCount(*bar) == 2, "selecting a tab did not mount its page");
    Widget const* const first = probe.childAt(*bar, 0);
    Widget const* const second = probe.childAt(*bar, 1);
    checks.that(first != nullptr && !probe.visibleOf(*first), "the unselected tab's page is shown");
    checks.that(second != nullptr && probe.visibleOf(*second), "the selected tab's page is hidden");
    selected.set(0);
    probe.settle();
    checks.that(probe.childCount(*bar) == 2, "re-selecting a tab mounted its page again");
    checks.that(first != nullptr && probe.visibleOf(*first), "the re-selected tab's page is hidden");
    return checks.result();
}

/// @brief Case: a ForEach updates a row whose key stays in place, keeping its widget.
/// @param probe The backend under test.
/// @return `nullopt`, or what went wrong.
[[nodiscard]] inline std::optional<std::string> forEachUpdatesInPlace(ConformanceProbe& probe) {
    reactive::Signal<std::vector<Entry>> rows{probe.runtime(), {{.id = 1, .label = "a"}, {.id = 2, .label = "b"}}};
    Mounted const view{probe.runtime(), probe.backend(), entryList(rows)};
    probe.settle();
    ContainerWidget const* const list = asContainer(view.root());
    if (list == nullptr) {
        return "a ForEach's widget is not a ContainerWidget";
    }
    Widget const* const before = probe.childAt(*list, 0);
    rows.set({{.id = 1, .label = "a2"}, {.id = 2, .label = "b"}});
    probe.settle();
    Checks checks;
    checks.that(probe.childAt(*list, 0) == before, "updating a kept row replaced its widget");
    checks.text("the rows after an update", "a2,b", childTexts(probe, *list));
    return checks.result();
}

/// @brief Case: a ForEach follows inserts, removals and reorders.
/// @param probe The backend under test.
/// @return `nullopt`, or what went wrong.
[[nodiscard]] inline std::optional<std::string> forEachInsertsRemovesReorders(ConformanceProbe& probe) {
    reactive::Signal<std::vector<Entry>> rows{
        probe.runtime(), {{.id = 1, .label = "a"}, {.id = 2, .label = "b"}, {.id = 3, .label = "c"}}};
    Mounted const view{probe.runtime(), probe.backend(), entryList(rows)};
    probe.settle();
    ContainerWidget const* const list = asContainer(view.root());
    if (list == nullptr) {
        return "a ForEach's widget is not a ContainerWidget";
    }
    Checks checks;
    checks.text("the rows as mounted", "a,b,c", childTexts(probe, *list));
    rows.set({{.id = 3, .label = "c"}, {.id = 1, .label = "a"}});
    probe.settle();
    checks.text("the rows after removing b and moving c first", "c,a", childTexts(probe, *list));
    rows.set({{.id = 3, .label = "c"}, {.id = 4, .label = "d"}, {.id = 1, .label = "a"}});
    probe.settle();
    checks.text("the rows after inserting d in the middle", "c,d,a", childTexts(probe, *list));
    return checks.result();
}

/// @brief Case: a Dialog holds its content only while it is open.
/// @param probe The backend under test.
/// @return `nullopt`, or what went wrong.
[[nodiscard]] inline std::optional<std::string> dialogHoldsContentWhileOpen(ConformanceProbe& probe) {
    reactive::Signal<bool> open{probe.runtime(), false};
    Mounted const view{probe.runtime(), probe.backend(),
                       ui::dialog({.open = [&open] { return open.get(); },
                                   .title = "Confirm",
                                   .child = ui::text({.text = "inside"})})};
    probe.settle();
    ContainerWidget const* const dialog = asContainer(view.root());
    if (dialog == nullptr) {
        return "a Dialog's widget is not a ContainerWidget";
    }
    Checks checks;
    checks.that(probe.childCount(*dialog) == 0, "a closed Dialog holds content");
    open.set(true);
    probe.settle();
    checks.text("an open Dialog's content", "inside", childTexts(probe, *dialog));
    open.set(false);
    probe.settle();
    checks.that(probe.childCount(*dialog) == 0, "a Dialog closed again still holds content");
    return checks.result();
}

/// @brief Drags a card carrying key 7 onto a panel built with @p accepts, and returns what the panel received.
/// @param probe The backend under test.
/// @param accepts The panel's predicate; empty leaves `Common::accepts` unset.
/// @return The keys the panel's `onDrop` received.
[[nodiscard]] inline std::vector<Key> dropOntoPanel(ConformanceProbe& probe, std::function<bool(Key const&)> accepts) {
    std::vector<Key> dropped;
    Mounted const card{probe.runtime(), probe.backend(),
                       ui::text({.text = "card", .common = {.dragKey = std::optional<Key>{Key{std::int64_t{7}}}}})};
    Mounted const target{probe.runtime(), probe.backend(),
                         ui::panel({.title = "Done",
                                    .common = {.accepts = std::move(accepts),
                                               .onDrop = [&dropped](Key key) { dropped.push_back(std::move(key)); }}})};
    probe.settle();
    probe.drag(card.root(), target.root());
    probe.settle();
    return dropped;
}

/// @brief Case: a drag onto a target that accepts the key delivers it once.
/// @param probe The backend under test.
/// @return `nullopt`, or what went wrong.
[[nodiscard]] inline std::optional<std::string> dragOntoAcceptingTarget(ConformanceProbe& probe) {
    Checks checks;
    checks.that(dropOntoPanel(probe, [](Key const& key) { return key == Key{std::int64_t{7}}; }) ==
                    std::vector<Key>{Key{std::int64_t{7}}},
                "a drop the target accepts did not deliver the dragged key exactly once");
    return checks.result();
}

/// @brief Case: a drag onto a target that refuses the key delivers nothing.
/// @param probe The backend under test.
/// @return `nullopt`, or what went wrong.
[[nodiscard]] inline std::optional<std::string> dragOntoRefusingTarget(ConformanceProbe& probe) {
    Checks checks;
    checks.that(dropOntoPanel(probe, [](Key const&) { return false; }).empty(),
                "a drop the target refuses reached its onDrop");
    return checks.result();
}

/// @brief Case: a drop target without `accepts` takes every key.
/// @param probe The backend under test.
/// @return `nullopt`, or what went wrong.
[[nodiscard]] inline std::optional<std::string> dropTargetWithoutAcceptsTakesAll(ConformanceProbe& probe) {
    Checks checks;
    checks.that(dropOntoPanel(probe, {}) == std::vector<Key>{Key{std::int64_t{7}}},
                "a drop target without accepts refused a key");
    return checks.result();
}

}  // namespace detail

/// @brief Every conformance case, in a fixed order.
///
/// Run each on a fresh probe; `nullopt` is a pass, anything else is the failure message. A backend's test suite
/// loops over these with its own `ConformanceProbe`.
/// @return The cases.
[[nodiscard]] inline std::span<ConformanceCase const> conformanceCases() {
    static std::array<ConformanceCase, 13> const cases{{
        {.name = "a Text shows its constant text", .run = detail::textShowsItsText},
        {.name = "a bound Text updates once per batch and not for an equal write",
         .run = detail::boundTextUpdatesOncePerChange},
        {.name = "visible and enabled follow their bindings", .run = detail::visibleAndEnabledFollowBindings},
        {.name = "a click runs the button's action once", .run = detail::clickReachesAction},
        {.name = "typing reaches onChange and a value set by the application is not echoed",
         .run = detail::typingWithoutEcho},
        {.name = "a Switch shows the selected case and nothing for a key without one",
         .run = detail::switchShowsSelectedCase},
        {.name = "Tabs mount a page on first selection and show only the selected one",
         .run = detail::tabsMountPagesLazily},
        {.name = "a ForEach updates a kept row in place", .run = detail::forEachUpdatesInPlace},
        {.name = "a ForEach follows inserts, removals and reorders", .run = detail::forEachInsertsRemovesReorders},
        {.name = "a Dialog holds its content only while open", .run = detail::dialogHoldsContentWhileOpen},
        {.name = "a drag onto an accepting target delivers the key", .run = detail::dragOntoAcceptingTarget},
        {.name = "a drag onto a refusing target delivers nothing", .run = detail::dragOntoRefusingTarget},
        {.name = "a drop target without accepts takes every key", .run = detail::dropTargetWithoutAcceptsTakesAll},
    }};
    return cases;
}

}  // namespace morph::ui::testing
```

Register it after `include/morph/ui/testing/recording_backend.hpp` in `CMakeLists.txt`'s `FILE_SET HEADERS`.

- [ ] **Step 4: Run the tests to verify they pass**

Run: `cmake --build build/reactive --target morph_tests && ./build/reactive/tests/morph_tests "[conformance]"`
Expected: PASS, `All tests passed (… assertions in 1 test case)`.

Mutation checks, one at a time, each restored afterwards — each must fail the conformance test with the named case
in its `INFO` line:

| Change | Case that must fail |
|---|---|
| In `FakeTextInput::setText`, add `if (callbacks().change) { callbacks().change(std::string{text}); }` | "typing reaches onChange and a value set by the application is not echoed" |
| In `Mounter::reconcile`, unmount every kept row too (as in Task 5's mutation) | "a ForEach updates a kept row in place" |
| In `Mounter::applyCommon`, delete the `if (!accepts) { … }` default | "a drop target without accepts takes every key" |
| In `showPage`, skip `previous->setVisible(false)` | "Tabs mount a page on first selection and show only the selected one" |

- [ ] **Step 5: Commit**

```bash
git add include/morph/ui/testing/backend_conformance.hpp tests/test_ui_conformance.cpp tests/CMakeLists.txt \
        CMakeLists.txt
git commit -m "wip(ui): the backend-conformance cases, run against RecordingBackend

Signed-off-by: Christian Parpart <christian@parpart.family>"
```

---

### Task 9: Specs, maps and changelog

**Files:**
- Create: `docs/spec/ui/view_tree.md`, `docs/spec/ui/backend_contract.md`, `docs/spec/ui/frontend.md`
- Modify: `docs/spec/README.md` — "Start here" map
- Modify: `docs/ARCHITECTURE.md` — "Namespace map" table and "Header map"
- Modify: `CHANGELOG.md` — `## [Unreleased]` → `### Added`

**Interfaces:**
- Consumes: the API exactly as Tasks 1–8 ship it. Where this text and the code disagree, fix the text.
- Produces: the authoritative specs Parts 3–10 read before touching `morph::ui`. No constant is pinned in
  `pinned_facts.toml`: the layer has no numeric limit, and its one site name, like Part 1's, is asserted by the
  tests that trigger it.

- [ ] **Step 1: Write `docs/spec/ui/view_tree.md`**

```markdown
# View tree and mount — design

Design spec for `morph::ui`'s view tree (`include/morph/ui/view.hpp`) and its mount
(`include/morph/ui/mount.hpp`): an application describes its UI once, as immutable nodes whose
properties are constants or reactive bindings, and `Mounted` turns that description into retained
widgets on any backend and keeps them current. The widgets themselves are
[backend_contract.md](backend_contract.md); the reactive graph underneath is
[../reactive/signals.md](../reactive/signals.md).

## Contents

- [Nodes](#nodes)
- [Props](#props)
- [The palette](#the-palette)
- [Keyed collections](#keyed-collections)
- [Mount](#mount)
- [Misuse](#misuse)
- [Design decisions](#design-decisions)

## Nodes

- A node is an aggregate (`ui::Text`, `ui::Column`, …) wrapped by its builder (`ui::text`,
  `ui::column`, …) into `ui::Node`, a `std::shared_ptr<NodeData const>`. `NodeData::kind` is a
  `std::variant` over every node type. Nodes are immutable and shared: a subtree may appear in
  several places, and mounting never changes it.
- Nodes are not templated on a message type. Events are `ui::Action` (`std::function<void()>`) or
  typed callbacks — `onChange(std::string)`, `onToggle(bool)`, `onSelect(Key)`.
- Strings are UTF-8; a backend converts.
- `ui::Key` is `std::variant<std::int64_t, std::string>`. It identifies Switch cases, Select
  options, ForEach and Table rows and drag payloads. It is never a floating-point number: morph ids
  exceed 2^53.
- Every node carries `Common`:

| Field | Type | Default | Meaning |
|---|---|---|---|
| `visible` | `Prop<bool>` | `true` | Shown |
| `enabled` | `Prop<bool>` | `true` | Accepts input |
| `layout` | `LayoutHints{width, height}` | content × content | `Sizing::content()`, `fixed(n)` or `stretch(weight)` per dimension, in backend units (a cell on the TUI) |
| `dragKey` | `Prop<std::optional<Key>>` | `nullopt` | Engaged: the widget can be dragged and a drop delivers this key |
| `accepts` | `std::function<bool(Key const&)>` | empty | Which keys a drop target takes; empty takes every key |
| `onDrop` | `std::function<void(Key)>` | empty | Set: the widget is a drop target |

## Props

`Prop<T>` holds a constant or a binding:

| Constructed from | Is | At mount |
|---|---|---|
| a non-callable value convertible to `T` | a constant | the setter is called once; no reactive node is made |
| a callable returning something convertible to `T` | a binding | an equality-gated `Computed<T>` plus an `Effect` that calls the setter |

A callable is never a constant, even when it converts to `T`: a captureless lambda converts to a
function pointer and that to `bool`, so without the rule `Prop<bool>{[] { return false; }}` would be
the constant `true`. A braced list does not deduce through the constructor template; write the type
(`std::vector<SelectOption>{…}`).

## The palette

| Node | Builder | Props and callbacks |
|---|---|---|
| `Text` | `text` | `text`, `role` (`TextRole{Normal, Muted, Heading, Error, Success}`) |
| `Button` | `button` | `label`, `onClick` |
| `TextInput` | `textInput` | `value`, `onChange`, `onSubmit`, `placeholder`, `mode` (`TextInputMode{SingleLine, Multiline, Password}`, set once) |
| `Checkbox` | `checkbox` | `label`, `checked`, `onToggle` |
| `Select` | `select` | `options` (`std::vector<SelectOption{key, label}>`), `selected` (`std::optional<Key>`; a key outside the options selects nothing), `onSelect`, `style` (`SelectStyle{Dropdown, Radio}`, set once) |
| `Menu` | `menu` | `items` (`MenuItem{label, onSelect}`: one table, no parallel lists; the list is set once, each label may be bound) |
| `Column`, `Row` | `column`, `row` | `children` (null children skipped), `gap` |
| `Grid` | `grid` | `columns`, `cells` (`GridCell{node, span}`), `gap` |
| `Spacer` | `spacer` | none |
| `Panel` | `panel` | `title`, `padding`, `child`, `collapsible` (set once), `collapsed`, `onToggle` — collapsible panels are accordion sections |
| `Scroll` | `scroll` | `child`, `axis` (set once) |
| `Switch` | `switchOf`, `switchOn<E>` | `selector` (`Prop<Key>`), `cases` (`SwitchCase{key, node}`), `fallback` |
| `Tabs` | `tabs` | `tabs` (`Tab{label, node}`, set once), `selected` (`Prop<std::size_t>`), `onSelect` |
| `Dialog` | `dialog` | `open`, `title`, `child`, `onDismiss` |
| `Busy` | `busy` | `active`, `label` |
| `ForEach` | `forEach<RowT>` | rows, key function, row view, `axis`, `gap` ([below](#keyed-collections)) |
| `Table` | `table<RowT>` | `columns` (`TableColumn{label, width}`), rows, key function, cells, and `TableOptions{selectionMode, selection, onSelectionChange, onActivate, common}` |
| `DateTimeInput` | `dateTimeInput` | `value` (`std::optional<time::Timestamp>`), `onChange`, `mode` (`DateMode{Date, DateTime}`), `offsetMinutes` (the display zone; both set once) |
| `Slider` | `slider` | `value` (`std::int64_t`), `minimum`, `maximum`, `step` (set once), `onChange` |
| `FilePicker` | `filePicker` | `path`, `mode` (`FilePickerMode{Open, Save}`, set once), `onPicked` |

The builder for a Switch is `switchOf` because `switch` is a keyword. `switchOn<E>(selector, cases,
fallback)` is the enumeration form: each enumerator becomes the `int64` key of its case.

Not in the palette: themes beyond `TextRole`, right-to-left layout, images, charts, i18n catalogues
(bindings produce translated text). Schema forms are spec 2's `forms::formView`, built on this
palette.

## Keyed collections

`forEach<RowT>(rows, keyOf, rowView, axis, gap)` takes the rows as a binding
(`std::function<std::vector<RowT>()>`, so a `Query`'s `value()` works) or as a
`Signal<std::vector<RowT>> const&` that must outlive every mount of the node. The row type is erased
behind `detail::ForEachModel`: each mount opens a `ForEachSession`, which keeps the latest snapshot
and makes one `RowSlot` per row. A slot owns a `Signal<RowT>` and the view `rowView` built from it
**once**. The view reads the row inside bindings; what it reads directly while it is built is read
once, and is not a dependency of anything.

`table<RowT>(columns, rows, keyOf, cells, options)` is the same machinery: the row view is a `Row`
whose children are `cells(row)`, one per column.

## Mount

`ui::Mounted(runtime, backend, root, parent = nullptr)` builds the tree once, untracked, into a
`reactive::Scope` it owns. `root()` is the root node's widget. A null root throws
`std::invalid_argument`. The runtime, the backend and every signal a binding reads outlive the
`Mounted`; the runtime's owner constructs and destroys it.

### Per node

1. The widget is made by its factory and adopted into the scope **first**.
2. `Common`: `visible` and `enabled` only when bound or `false`; `layout` only when not
   content × content; `dragKey` only when bound or engaged; the drop handler only when `onDrop` is
   set, with an accept-everything predicate when `accepts` is empty. A new widget is already
   visible, enabled, content-sized and neither draggable nor a drop target.
3. The kind's props in field order. A constant calls its setter once; a binding makes a
   `Computed` (equality-gated, so an unchanged result calls nothing) and an `Effect` calling one
   setter. Every kind-specific prop is set even when it holds its default; a Grid child's span only
   when not 1; a Panel's `collapsed` and `onToggle` only when it is collapsible. A Menu sends all
   its labels through `setItems` whenever one bound label changes.
4. Its children, appended in order.

Destroying the scope destroys in reverse creation order: bindings before the widget they drive,
children before their parent.

### Callbacks

Every callback handed to a widget runs inside `Runtime::widgetEvent`: one batch, during which a
flush that starts — a nested event loop pumping the owner — is refused and re-posted. Flushes are
always posted, so a write inside a handler never destroys the handler's own widget; the refusal
covers the one remaining way, a handler that pumps the loop itself.

### Switch, Tabs, Dialog

- **Switch** is a slot widget. Its case lives in a child `Scope`; a new key resets that scope (the
  old case is torn down, newest first) and then mounts the new case. The selector's `Computed` is
  equality-gated, so an unchanged key never remounts. A key with no case and no fallback shows
  nothing.
- **Tabs** creates a page slot under the tab bar the first time a tab is selected, in its own
  `Scope`, and keeps it: switching tabs hides the old page's slot and shows the new one, so a page's
  widgets keep focus, scroll position and half-typed text. An index past the last tab shows no page.
- **Dialog** mounts its content into a child `Scope` when `open` becomes true, then opens the
  overlay; when `open` becomes false it destroys the content, then closes the overlay.

### ForEach and Table rows

The mount adopts the model, the session, a row list and then one `Effect`, in that order. The
Effect pulls the keys (tracked: the rows binding is its only source) and reconciles untracked:

1. The first occurrence of each key is kept; every later duplicate is reported
   (`detail::site::kDuplicateKey`) and refused.
2. A row whose key is gone is unmounted. A row whose key stays is updated in place: its slot's
   signal is set to the new snapshot entry, and the row's own bindings run later in the same flush.
   Its widget, focus and selection survive.
3. A new key gets a row `Scope` — the slot adopted first, then its widgets — appended to the
   container. A Table then calls `setRowKey(rowWidget, key)`.
4. The container is brought into snapshot order with `ContainerWidget::moveChild`. The widgets on a
   longest run already in order stay; every other widget moves once, directly before its successor
   in the target order. An append moves nothing; moving one row is one move; reversing three rows is
   two.

A Table's `selection` binding is made after its rows, so its first `setSelection` names rows that
exist.

## Misuse

Reported through the runtime's owner probe, then refused, as in
[../reactive/signals.md](../reactive/signals.md):

| Misuse | Site | Refusal |
|---|---|---|
| A ForEach or Table snapshot repeats a key | `ui::detail::site::kDuplicateKey` | The later row is skipped; the first is kept |

## Design decisions

| Decision | Why |
|---|---|
| Fine-grained bindings, no virtual tree and no diff | A change reaches exactly the setter that depends on it; widget identity, focus and selection survive. |
| A constant creates no reactive node | Most props of most screens never change; they cost one setter call and nothing after. |
| Content in child scopes | One mechanism gives "bindings before widgets, children before parents" for a case, a page, a dialog and a row. |
| Lazy, kept Tabs pages | A tab that is never opened costs nothing; a tab left keeps its state. |
| Keyed rows with a longest-run reorder | Keys keep identity across updates; the run keeps a reorder to the fewest moves a backend has to animate or re-lay out. |
| Row construction untracked | A row view that reads a signal directly must not make the whole list a dependent of that signal. |
| `switchOf`, not `switch_` | A keyword cannot be a function name; the spelling reads as a phrase at the call site. |
```

- [ ] **Step 2: Write `docs/spec/ui/backend_contract.md`**

```markdown
# Backend contract — design

Design spec for `include/morph/ui/backend.hpp` — the retained widgets a mount drives and the
`IViewBackend` that makes them — and for the two test instruments that fix the contract:
`ui::testing::RecordingBackend` (`testing/recording_backend.hpp`) and the conformance cases
(`testing/backend_conformance.hpp`). Implemented by `morph::tui::Backend` and
`morph::qt_quick::Backend`. What the mount does with these widgets is
[view_tree.md](view_tree.md).

## Contents

- [Widgets](#widgets)
- [Containers](#containers)
- [Interfaces](#interfaces)
- [Drag and drop](#drag-and-drop)
- [RecordingBackend](#recordingbackend)
- [The conformance suite](#the-conformance-suite)
- [Design decisions](#design-decisions)

## Widgets

- `IViewBackend` has one factory per kind. Each makes the widget, appends it as the last child of
  `parent` — or makes a root when `parent` is null — and never returns null. A kind's fixed
  parameters (a TextInput's mode, a stack's or scroll's axis, a DateTimeInput's mode and zone, a
  Select's style, a FilePicker's mode) are factory arguments.
- A new widget is visible, enabled, content-sized, not draggable and not a drop target. The mount
  calls those setters only to change them.
- Every setter takes UTF-8 and may be called with the value the widget already has.
- **A setter never calls the widget's own callback**: `TextInputWidget::setText` does not call
  `onChange`, `setChecked` does not call `onToggle`, and likewise for every selection, value and
  path setter. A binding that writes back what the user typed therefore cannot loop.
- A widget's destructor detaches it from its parent and frees its native resources. The mount
  destroys children before parents.
- Callbacks are handed over once (`setOnClick`, `setOnChange`, …); an empty one does nothing. The
  mount wraps each in `Runtime::widgetEvent`, so a backend calls them directly from its native
  event handler.

## Containers

`ContainerWidget::moveChild(child, index)` removes `child` and inserts it before the child now at
`index`; an index past the end means last. Only the mount reorders children, and only through this.

| Container | Children |
|---|---|
| `StackWidget` | In order along the axis, `setGap` apart |
| `GridWidget` | Row-major in `setColumns` columns; `setSpan(child, n)` widens one, a child spans 1 until told |
| `PanelWidget` | Inside the frame, `setPadding` in; hidden while collapsed |
| `ScrollWidget` | One scrolled viewport that keeps the focused child visible |
| `SlotWidget` | Shown as they are; hosts a Switch case or a Tabs page |
| `TabsWidget` | Page slots in the order first shown; the mount keeps exactly the selected page's slot visible, and `setSelected` moves the bar's highlight only |
| `DialogWidget` | The overlay's content, present only while open; a focus trap while `setOpen(true)` |
| `TableWidget` | Rows, each a horizontal stack of cells in column order; `setRowKey(row, key)` once per row, right after it is built |

## Interfaces

| Interface | Setters | Callbacks |
|---|---|---|
| `Widget` | `setVisible`, `setEnabled`, `setLayout`, `setDragKey` | `setDropHandler(accepts, onDrop)` |
| `TextWidget` | `setText`, `setRole` | — |
| `ButtonWidget` | `setLabel` | `setOnClick` (Enter, Space, click) |
| `TextInputWidget` | `setText`, `setPlaceholder` | `setOnChange` (whole text after each edit), `setOnSubmit` |
| `CheckboxWidget` | `setLabel`, `setChecked` | `setOnToggle(bool)` |
| `SelectWidget` | `setOptions`, `setSelected(optional<Key>)` | `setOnSelect(Key)` |
| `MenuWidget` | `setItems(labels)` | `setOnActivate(index)` |
| `PanelWidget` | `setTitle`, `setPadding`, `setCollapsible`, `setCollapsed` | `setOnToggle(collapsed)` |
| `TabsWidget` | `setTabs(labels)`, `setSelected(index)` | `setOnSelect(index)` |
| `DialogWidget` | `setOpen`, `setTitle` | `setOnDismiss` (Esc on the TUI) |
| `BusyWidget` | `setActive`, `setLabel` | — |
| `TableWidget` | `setColumns`, `setSelectionMode`, `setRowKey`, `setSelection(keys)` | `setOnSelectionChange(keys)`, `setOnActivate(Key)` |
| `DateTimeInputWidget` | `setValue(optional<Timestamp>)` | `setOnChange(optional<Timestamp>)` |
| `SliderWidget` | `setRange(minimum, maximum, step)`, `setValue` | `setOnChange(value)` |
| `FilePickerWidget` | `setPath` | `setOnPicked(path)` |

A Table keeps its selection by key: it survives a reorder, and a key whose row is not there yet
selects that row when it appears. A Select's key outside its options shows no selection.

## Drag and drop

A widget with a drag key can be dragged; a widget with a drop handler is a target. While dragging,
the backend asks the target's `accepts(key)` before highlighting it and before a drop; a drop it
refuses delivers nothing, and an accepted one calls `onDrop(key)` once. The mount always passes a
predicate (accept-everything when the node's `accepts` is empty), so a backend never sees an empty
one.

## RecordingBackend

The headless reference backend and the test double for everything above it.

- Widget ids are creation numbers from 1. A stack's kind is `Column` or `Row` by its axis; every
  other kind is the interface name without `Widget`.
- **Operation log** (`log()`, `clearLog()`), one line per operation:
  `create Kind#id in Parent#id` (`in root` for a root), `set Kind#id name=value`,
  `move Kind#id to n`, `destroy Kind#id`. Factory parameters are recorded as properties without a
  log line; callbacks are recorded without one.
- **Golden dump** (`dump()`): one line per live widget, `Kind#id name=value …`, properties in name
  order, children indented two spaces per depth, every line ending in a newline. A child whose
  parent was destroyed first becomes a root.
- Values: `true`/`false`; a key as a decimal integer or a double-quoted string (`7` and `"7"`
  differ); `none` for an absent key or date; a date as ISO-8601 UTC (`empty` for an empty
  `Timestamp`); sizing as `content`, `fixed(n)` or `stretch(n)`, layout as `width/height`; lists as
  `[a,b]`; options as `[key:label,…]`; columns as `[label:sizing,…]`; a slider's range as
  `minimum..maximum/step`.
- **Lookups**: `find(kind, name, value)`, `all(kind)`, `prop(id, name)` (empty when never set),
  `exists`, `idOf(widget)`, `kindOf`, `children`, `widget(id)`.
- **Interaction helpers** act as the user would: `click`, `edit`, `submit`, `toggle`, `choose`,
  `chooseIndex`, `collapse`, `dismiss`, `selectRows`, `activateRow`, `setDateTime`, `slide`, `pick`,
  `drag`. A helper first changes what the native widget would change by itself (the field's text, the
  box's check mark, the selection) without a log line, then calls the stored callback. A hidden or
  disabled widget ignores every helper. A helper copies the callback before calling it, so a handler
  may destroy its own widget. `drag(source, target)` returns true when the source has a drag key and
  the target's predicate accepted it; an empty predicate accepts nothing.

## The conformance suite

`conformanceCases()` is a list of `ConformanceCase{name, run}`; each `run` takes a
`ConformanceProbe` and returns `nullopt` for a pass or the failure message. A backend author writes
one probe — `backend()`, `runtime()`, `settle()` (run posted flushes and native events until idle),
`textOf`, `visibleOf`, `enabledOf`, `childCount`, `childAt`, `click`, `type`, `drag` — and runs
every case on a fresh probe. `visibleOf` is the widget's own flag as its last `setVisible` left it,
not whether it is on screen: a hidden ancestor does not change it. A case reaches widgets only through `Mounted::root()`, `childAt` and a
second root, so a probe needs no lookup by name.

The cases: a constant Text; a bound Text updating once per batch and not for an equal write;
`visible` and `enabled` bindings; a click reaching its action; typing reaching `onChange` with no
echo of an application-set value; a Switch's selected case, and nothing for a key without one;
lazily mounted, kept Tabs pages; a ForEach updating a kept row in place, and following inserts,
removals and reorders; a Dialog holding content only while open; a drag onto an accepting target,
onto a refusing one, and onto one without `accepts`.

`RecordingBackend` passes them in `tests/test_ui_conformance.cpp`; the TUI and Qt Quick backends
run the same list in their own suites.

## Design decisions

| Decision | Why |
|---|---|
| Typed factories, one interface per kind | A backend implements exactly what each node needs; a missing setter is a compile error, not a runtime lookup. |
| Setters never echo | Two-way bindings (a field bound to the signal its `onChange` writes) cannot loop, on any toolkit. |
| Defaults stated by the contract | The mount skips the setters that would only restate them, which keeps logs and native call counts to what a node actually says. |
| A recording backend as the reference | Every mount behaviour is asserted as a log or a dump without a toolkit, and the real backends are held to the same cases through the probe. |
```

- [ ] **Step 3: Write `docs/spec/ui/frontend.md`**

```markdown
# Frontend seam — design

Design spec for `include/morph/ui/frontend.hpp`: how an application is written once and run on
whichever frontend `main` picks at runtime. Implemented by `morph::tui::Frontend` and
`morph::qt_quick::Frontend`.

## Contents

- [Shape](#shape)
- [What `run` guarantees](#what-run-guarantees)
- [Choosing a frontend](#choosing-a-frontend)
- [Design decisions](#design-decisions)

## Shape

- `ui::Application` is the application: its controllers, handlers and bridge wiring, and
  `view()`, the tree to mount.
- `ui::ApplicationFactory` (`std::function<std::unique_ptr<Application>(AppContext&)>`) makes it.
  The factory is where an application constructs its `BridgeHandler`s, with `ctx.executor()` as
  their callback executor.
- `ui::AppContext` is what the frontend hands the factory:

| Member | Meaning |
|---|---|
| `runtime()` | The reactive runtime the view is mounted in; owned by the frontend |
| `executor()` | The runtime's owner; every `BridgeHandler`'s callback executor |
| `scheduler()` | Timers on that executor (`ui::Scheduler` is `reactive::Scheduler`, `ui::TimerHandle` is `reactive::TimerHandle`); a `Query` refreshed on a period uses it |
| `ioLoop()` | The caller-driven I/O loop the frontend runs on (the TUI), for sockets that share its thread; null when there is none (Qt Quick) |
| `quit(exitCode)` | Ends `run` once the current event is handled |
| `frontendName()` | The name `selectFrontend` matched |

- `ui::Frontend` is a frontend: `name()` and `run(factory)`.
- `ui::FrontendOption{name, usable, make}` is one frontend a binary was built with.

## What `run` guarantees

1. The runtime and its owner executor exist before the factory is called.
2. The factory is called once, then `view()` once, and the view is mounted.
3. The loop runs until `AppContext::quit`; `run` returns its exit code.
4. Teardown is the reverse: the mounted view, then the application, then the runtime. A binding or
   handler never outlives what it points at.

This header defines the order; each frontend's own suite pins it, and both
pin the step that is easiest to get wrong — **the application is destroyed
before the runtime**:

| Frontend | Test |
|---|---|
| `morph::tui` | `tests/tui/test_tui_frontend.cpp`, "tui::Frontend: the application is destroyed before the runtime" |
| `morph::qt_quick` | `tests/qt_quick/test_frontend.cpp`, "qt_quick Frontend: the mount dies before the application, the application before the runtime, Qt last" |

Each application holds a `Signal` on the runtime, and each test asserts that
the runtime's destruction reports no live node
(`reactive::detail::site::kRuntimeOutlived`).

## Choosing a frontend

`selectFrontend(built, argc, argv, env = processEnvironment())`:

1. the last `--ui=<name>` or `--ui <name>` on the command line;
2. else `MORPH_UI`, when set and non-empty;
3. else the first option, in the order `main` lists them, whose `usable()` holds — an empty
   `usable` counts as usable. `main` lists Qt Quick first (usable on macOS and Windows, and with
   `DISPLAY` or `WAYLAND_DISPLAY` set), then the TUI (usable when stdin is a terminal).

A frontend named on the command line or in `MORPH_UI` is used even when its `usable()` is false:
the user asked for it. Every failure throws `FrontendSelectionError` naming the built frontends:

| Failure | Message |
|---|---|
| A name no option has | `morph::ui: no frontend named '<name>' is built (built: qt, tui)` |
| `--ui` with no name, or `--ui=` | `morph::ui: --ui needs a frontend name (built: qt, tui)` |
| Nothing named and nothing usable | `morph::ui: no built frontend is usable here (built: qt, tui)` |
| An option whose `make` returns null | `morph::ui: frontend '<name>' could not be made` |

With nothing built the list reads `none`. `EnvironmentReader` is the seam a test replaces;
`processEnvironment()` reads `std::getenv`, which selection calls from `main` before any thread
exists.

## Design decisions

| Decision | Why |
|---|---|
| The frontend is injected, the application is a factory | Application code never names a toolkit; one binary carries every frontend it was built with. |
| The frontend owns the runtime and the loop | The runtime's owner must be the loop's thread, and only the frontend knows which thread that is. |
| Command line, then environment, then the first usable option | An explicit request wins; a default that works where the binary runs needs no request at all. |
| Errors name the built frontends | "Not built" and "misspelled" look the same to the user; the list answers both. |
```

- [ ] **Step 4: Update the maps and the changelog**

In `docs/spec/README.md`, under "Start here — specs by the question they answer", add after the
"**Reactive state and control**" group that Part 1 added:

```markdown
**Declarative UI**
[`ui/view_tree.md`](ui/view_tree.md) ·
[`ui/backend_contract.md`](ui/backend_contract.md) ·
[`ui/frontend.md`](ui/frontend.md)
```

In `docs/ARCHITECTURE.md`, "Namespace map" table, add after the `morph::reactive` row:

```markdown
| `morph::ui` | Toolkit-agnostic view tree, its mount, and the frontend seam | `Prop<T>`, `Key`, `Action`, `Common`, the node types and their builders (`text`, `button`, …, `switchOf`, `switchOn<E>`, `forEach<Row>`, `table<Row>`), `Widget` and the widget interfaces, `IViewBackend`, `Mounted`, `AppContext`, `Application`, `Frontend`, `FrontendOption`, `selectFrontend`; `ui::testing::RecordingBackend`, `ConformanceProbe`, `conformanceCases()` |
```

and in "Header map", after the `reactive/` sub-section Part 1 added:

```markdown
#### `ui/` — view tree, mount, frontend seam

| Header | Responsibility |
|---|---|
| `ui/view.hpp` | `Prop<T>`, `Key`, `Common`, every node type and builder, `forEach`, `table`, the type-erased row model |
| `ui/backend.hpp` | `Widget`, `ContainerWidget`, one widget interface per node kind, `IViewBackend` |
| `ui/mount.hpp` | `Mounted` — builds widgets once, keeps them current through bindings; keyed-row reconciliation |
| `ui/frontend.hpp` | `AppContext`, `Application`, `Frontend`, `FrontendOption`, `selectFrontend` |
| `ui/testing/recording_backend.hpp` | `RecordingBackend` — headless widgets, operation log, golden dump, interaction helpers |
| `ui/testing/backend_conformance.hpp` | `ConformanceProbe`, `conformanceCases()` — the cases every backend passes |
```

In `CHANGELOG.md`, under `## [Unreleased]` → `### Added`, directly after the `morph::reactive` entry:

```markdown
- **`morph::ui`: a toolkit-agnostic view tree, its mount and the frontend seam.** Header-only, in
  the base `morph` target.
  - Immutable nodes — text, buttons, inputs, check boxes, selects, menus, stacks, grids, panels
    (collapsible), scroll areas, Switch, Tabs, Dialog, busy indicators, keyed `forEach` lists,
    tables, date-time inputs, sliders and file pickers — whose properties are constants or reactive
    bindings, with drag-and-drop on every node.
  - `Mounted` builds retained widgets through an `IViewBackend` once and keeps them current through
    equality-gated bindings; keyed rows are updated in place and reordered by moving only the rows
    outside a longest run already in order.
  - `AppContext`, `Application`, `Frontend` and `selectFrontend` (`--ui=`, `MORPH_UI`, the first
    usable frontend) let `main` pick a frontend at runtime.
  - `ui::testing::RecordingBackend` and the backend-conformance cases fix the contract every
    frontend implements.
  - Specified in `docs/spec/ui/`.
```

- [ ] **Step 5: Build the docs with warnings as errors**

```bash
cmake -S . -B build/docs -G Ninja -DMORPH_BUILD_DOCUMENTATION=ON -DMORPH_BUILD_TESTS=OFF -DMORPH_BUILD_EXAMPLES=OFF
cmake --build build/docs --target doc
```

Expected: exits 0. A warning naming a `morph::ui` symbol is a missing `@param`/`@tparam`/`@return`: fix the
header, not the Doxygen configuration. Then
`pre-commit run --files docs/spec/ui/*.md docs/spec/README.md docs/ARCHITECTURE.md CHANGELOG.md` (whitespace,
end-of-file, codespell) and `npx --yes markdownlint-cli2 docs/spec/ui/*.md`: expected clean against
`.markdownlint.yaml` (119 columns, code blocks and tables exempt).

- [ ] **Step 6: Commit**

```bash
git add docs/spec/ui docs/spec/README.md docs/ARCHITECTURE.md CHANGELOG.md
git commit -m "wip(ui): specify morph::ui

Signed-off-by: Christian Parpart <christian@parpart.family>"
```

---

### Task 10: Whole-part verification and the squash

**Files:** none new; fixes land in the files they concern.

- [ ] **Step 1: Strict build and the full suite**

```bash
cmake --build build/reactive && ctest --test-dir build/reactive --output-on-failure
```

Expected: every test passes, not only `[ui]` — `VERIFY_INTERFACE_HEADER_SETS` compiles each of the six new public
headers standalone as part of the build, so a header that leans on another's includes fails here. Confirm they were
offered to it rather than assuming so:

```bash
find build/reactive/morph_verify_interface_header_sets/morph/ui -name '*.hpp.cxx' | wc -l
```

Expected: `6` — one generated stub per `ui` header (`view`, `backend`, `mount`, `frontend`,
`testing/recording_backend`, `testing/backend_conformance`). Fewer means a header is missing from the `FILE_SET`
and nothing compiled it standalone.

- [ ] **Step 2: The layering holds**

```bash
grep -rnE '#include .*(bridge\.hpp|/net/|/qt/|/tui/|core/tui|Qt|io_loop\.hpp)' include/morph/ui && exit 1 || echo "ui includes no bridge, toolkit, terminal or loop header"
```

Expected: the echo line. `frontend.hpp` names `exec::IoLoop` through a forward declaration only.

- [ ] **Step 3: Sanitizers** (Linux; on macOS use an ASan configure of the same tree)

```bash
cmake --preset clang-asan && cmake --build --preset clang-asan
bash scripts/check_sanitizer_instrumentation.sh --binary build/clang-asan/tests/morph_tests asan
./build/clang-asan/tests/morph_tests "[ui]"
cmake --preset clang-tsan && cmake --build --preset clang-tsan
bash scripts/check_sanitizer_instrumentation.sh --binary build/clang-tsan/tests/morph_tests tsan
./build/clang-tsan/tests/morph_tests "[ui]"
```

Expected: clean. ASan is the observer for "a helper survives a handler that destroys its widget", for every
remount test (a binding left pointing at a destroyed widget is a use after free, not a wrong value), and for the
Review Focus 3 case; the instrumentation check is what makes a clean run mean something.

- [ ] **Step 4: clang-tidy over the changed lines** — the recipe in CONTRIBUTING, "Running the `clang-tidy-diff`
  gate locally", with `origin/master...HEAD` and the file count asserted non-zero. Because the
  `tests/.clang-tidy` subtractions reach headers analysed from test translation units (its header comment), also
  run clang-tidy directly on the header-set stubs, which inherit the root configuration:

```bash
cmake build/reactive -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
clang-tidy -p build/reactive --quiet \
    build/reactive/morph_verify_interface_header_sets/morph/ui/*.hpp.cxx \
    build/reactive/morph_verify_interface_header_sets/morph/ui/testing/*.hpp.cxx
```

Expected: no findings from either. A finding in `include/morph/ui/` is fixed in the header; a `NOLINT` is added
only with the reason on the same line, as the plan's code already does for its four.

- [ ] **Step 5: Install/export**

```bash
bash scripts/check_install_export.sh
```

Expected: passes — its consumer translation unit includes every installed public header, the six `ui/` ones
included.

- [ ] **Step 6: Commit any fixes**

```bash
git add -A include/morph/ui tests docs CMakeLists.txt
git commit -m "wip(ui): fixes from the sanitizer, tidy and install gates

Signed-off-by: Christian Parpart <christian@parpart.family>"
```

Skip the commit if there was nothing to fix, and say so in the hand-off.

- [ ] **Step 7: Squash this part into its one commit**

Write the message to `/tmp/ui-commit.txt`:

```text
ui: toolkit-agnostic view tree, mount, frontend seam and RecordingBackend

morph::ui, header-only in the base morph target. An immutable view tree
whose props are constants or reactive bindings, with drag-and-drop on
every node; Mounted builds retained widgets through an IViewBackend once
and keeps them current through equality-gated bindings, with Switch, Tabs,
Dialog and keyed ForEach/Table rows mounted into child scopes so teardown
is always bindings-first and child-first. The frontend seam (AppContext,
Application, Frontend, selectFrontend) lets main pick a frontend at
runtime. RecordingBackend and the conformance cases fix the contract the
TUI and Qt Quick backends implement. Specified in docs/spec/ui/.

Signed-off-by: Christian Parpart <christian@parpart.family>
```

If the implementation deviated from this plan anywhere, add a paragraph before the sign-off saying what the plan
said, what was done instead, and why (master plan, "The docs commit").

Then run the master plan's "Squashing a part" procedure with `key=ui` and `git commit -F /tmp/ui-commit.txt`:

```bash
key=ui
pre=$(git rev-parse HEAD)
first=$(git log --reverse --format=%H --grep="^wip($key):" master..HEAD | head -1)
test -n "$first" || { echo "no wip($key) commits -- wrong key?"; exit 1; }
git log --format=%s "$first^..HEAD" | grep -v "^wip($key): " && { echo "foreign commit inside the part"; exit 1; }
git reset --soft "$first^"
git commit -F /tmp/ui-commit.txt
git diff --exit-code "$pre" HEAD && echo "squash preserved the tree"
git log --oneline master..HEAD
```

Expected: `squash preserved the tree`, and the last command lists, newest first,
`ui: toolkit-agnostic view tree, mount, frontend seam and RecordingBackend`,
`reactive: signal graph, view state and declarative control`, `core: build against core-cpp 0.7`,
`docs: declarative UI program — design and implementation plans`.

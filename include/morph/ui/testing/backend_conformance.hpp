// SPDX-License-Identifier: Apache-2.0

#pragma once
#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "../../reactive/runtime.hpp"
#include "../../reactive/signal.hpp"
#include "../../util/datetime.hpp"
#include "../backend.hpp"
#include "../mount.hpp"
#include "../view.hpp"

/// @file
/// @brief The scripted cases every `IViewBackend` must pass, and the probe a backend author implements to run them.
///
/// Specified in `docs/spec/ui/backend_contract.md`, "The conformance suite".

namespace morph::ui::testing {

/// @brief Where a widget is drawn, in its backend's own units (a cell on the terminal; `Theme.unitWidth` and
///        `Theme.unitHeight` on Qt Quick), from the top left of the window or screen.
struct WidgetBounds {
    /// @brief The left edge.
    double x = 0;
    /// @brief The top edge.
    double y = 0;
    /// @brief The width.
    double width = 0;
    /// @brief The height.
    double height = 0;
};

/// @brief How the conformance cases read and drive one backend under test.
///
/// The driving operations act as a user would, through the backend's own input path where it has one; each may run
/// pending work before and after it. Reading operations report what the widget shows now.
///
/// Whether a handler can run after its widget was destroyed is observable only for a backend whose input can be
/// delivered later than the driving call, such as a queued connection; a probe whose `click` delivers before it
/// returns passes that case without exercising it.
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

    /// @brief The text a widget shows: a Text's or TextInput's text, a Button's or Checkbox's label, and the label of
    ///        the option a Select marks.
    /// @param widget The widget.
    /// @return The text, UTF-8; empty for a Select that marks no option.
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
    /// @return The child, or null past the end. A case drives it as it drives a root.
    [[nodiscard]] virtual Widget* childAt(ContainerWidget const& container, std::size_t index) = 0;

    /// @brief Activates a button as a user would.
    /// @param widget The button.
    virtual void click(Widget& widget) = 0;

    /// @brief Replaces a text field's text as a user typing would: whatever the field held is cleared first (select
    ///        all, or delete), so it ends up holding exactly @p text. `onChange` may see intermediate texts.
    /// @param widget The field.
    /// @param text The text to end up with.
    virtual void type(Widget& widget, std::string_view text) = 0;

    /// @brief Drags one widget onto another as a user would.
    /// @param source The dragged widget.
    /// @param target The widget it is dropped on.
    virtual void drag(Widget& source, Widget& target) = 0;

    /// @brief Dismisses a dialog or drawer as a user would (Esc on the TUI), or a dismissible banner.
    /// @param dialog The dialog, drawer or banner.
    virtual void dismiss(Widget& dialog) = 0;

    /// @brief Selects rows of a table as a user would, so that the user's selection is exactly these rows.
    /// @param table The table.
    /// @param rows Positions among the table's current rows, in the order the user picks them: at least one, and
    ///        one only for a `Single` table.
    virtual void selectRows(Widget& table, std::vector<std::size_t> const& rows) = 0;

    /// @brief Clicks a row of a table as a user would, with the modifiers @p click stands for: none, Ctrl, or Shift.
    /// @param table The table.
    /// @param row   A position among the table's current rows.
    /// @param click How the click is modified.
    virtual void clickRow(Widget& table, std::size_t row, RowClick click) = 0;

    /// @brief Clicks a column's header of a table as a user would.
    /// @param table  The table.
    /// @param column The column's index among the table's columns.
    virtual void clickHeader(Widget& table, std::size_t column) = 0;

    /// @brief The rows a table shows as selected.
    /// @param table The table.
    /// @return Their positions among the table's current rows, ascending.
    [[nodiscard]] virtual std::vector<std::size_t> selectedRows(Widget const& table) = 0;

    /// @brief Moves a text field's cursor as a user would (the arrow keys, a click), without changing the text.
    /// @param field The text field.
    /// @param position A byte offset into the text it shows.
    virtual void moveCursor(Widget& field, std::size_t position) = 0;

    /// @brief Where a text field's cursor is.
    /// @param field The text field.
    /// @return A byte offset into the text it shows.
    [[nodiscard]] virtual std::size_t cursorOf(Widget const& field) = 0;

    /// @brief Presses a keyboard chord as a user would with focus on a widget.
    /// @param focused The widget focus is on; the probe moves focus there first if it must.
    /// @param chord The chord, as a document declares it, such as `"Ctrl+S"`.
    virtual void press(Widget& focused, std::string_view chord) = 0;

    /// @brief Whether a widget has keyboard focus.
    /// @param widget The widget.
    /// @return True when it is the widget that has focus.
    [[nodiscard]] virtual bool hasFocus(Widget const& widget) = 0;

    /// @brief Chooses a menu entry as a user would, opening each submenu on the path first.
    /// @param menu The menu.
    /// @param path The entry's index at each level, from the top.
    virtual void chooseMenuEntry(Widget& menu, std::vector<std::size_t> const& path) = 0;

    /// @brief Opens or closes a collapsible section with its own control, as a user would.
    /// @param section The section.
    /// @param open The state asked for.
    virtual void expand(Widget& section, bool open) = 0;

    /// @brief Drops files from the operating system onto a drop zone, as a user would.
    /// @param zone The drop zone.
    /// @param paths The files' paths, in the order dropped; not empty.
    virtual void dropFiles(Widget& zone, std::vector<std::string> const& paths) = 0;

    /// @brief Where a widget is drawn, for the layout cases (`layoutConformanceCases`). A backend that lays nothing
    ///        out, such as `RecordingBackend`, answers none and runs only `conformanceCases`; a layout case run on
    ///        it fails.
    /// @param widget The widget.
    /// @return Its bounds, in the backend's own units; `nullopt` when the probe reads no geometry.
    [[nodiscard]] virtual std::optional<WidgetBounds> boundsOf(Widget const& widget) {
        static_cast<void>(widget);
        return std::nullopt;
    }
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

/// @brief A row of the ForEach and Table cases.
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
/// @param builds Counts the row views built, when not null; must outlive the node's mounts.
/// @return The node.
[[nodiscard]] inline Node entryList(reactive::Signal<std::vector<Entry>> const& rows, int* builds = nullptr) {
    return ui::forEach<Entry>(
        rows, [](Entry const& entry) { return Key{entry.id}; },
        [builds](reactive::Signal<Entry> const& entry) {
            if (builds != nullptr) {
                ++*builds;
            }
            return ui::text({.text = [&entry] { return entry.get().label; }});
        });
}

/// @brief A one-column Table of entries, keyed by id, whose cell is a Text of the label.
/// @param rows The entries; must outlive the node's mounts.
/// @param options Selection, activation and `Common`.
/// @param sortable Whether the column is sortable.
/// @return The node.
[[nodiscard]] inline Node entryTable(reactive::Signal<std::vector<Entry>> const& rows, TableOptions options,
                                     bool sortable = false) {
    return ui::table<Entry>(
        {{.label = "Name", .sortable = sortable}}, [&rows] { return rows.get(); },
        [](Entry const& entry) { return Key{entry.id}; },
        [](reactive::Signal<Entry> const& entry) {
            return std::vector<Node>{ui::text({.text = [&entry] { return entry.get().label; }})};
        },
        std::move(options));
}

/// @brief A widget as a container.
/// @param widget The widget.
/// @return It, or null when it is not a `ContainerWidget`.
[[nodiscard]] inline ContainerWidget const* asContainer(Widget const& widget) {
    return dynamic_cast<ContainerWidget const*>(&widget);
}

/// @brief The root widget of a mounted container node, as the container it is.
///
/// The root of a mounted Switch, Tabs, Column, ForEach or Dialog is the widget the backend's `createSlot`,
/// `createTabs`, `createStack` or `createDialog` returned, and each of those is a `ContainerWidget` by its declared
/// type, so the downcast cannot fail for any backend that compiles.
/// @param view The mount, of a container node.
/// @return The root widget as a container.
[[nodiscard]] inline ContainerWidget const& rootContainer(Mounted const& view) {
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-static-cast-downcast): a container by the factory's type, see above
    return static_cast<ContainerWidget const&>(view.root());
}

/// @brief A child of a widget that should be a container.
/// @param probe The backend under test.
/// @param parent The widget; null, or a widget that is no container, has no children.
/// @param index The position.
/// @return The child, or null.
[[nodiscard]] inline Widget* childOf(ConformanceProbe& probe, Widget const* parent, std::size_t index) {
    ContainerWidget const* const container = parent == nullptr ? nullptr : asContainer(*parent);
    return container == nullptr ? nullptr : probe.childAt(*container, index);
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

/// @brief The texts of the rows an `entryTable` shows as selected, joined by commas.
/// @param probe The backend under test.
/// @param table The table.
/// @return For example `a,c`; a row without a first cell reads `<null>`.
[[nodiscard]] inline std::string selectedTexts(ConformanceProbe& probe, Widget const& table) {
    std::string texts;
    for (std::size_t const row : probe.selectedRows(table)) {
        if (!texts.empty()) {
            texts += ',';
        }
        Widget const* const cell = childOf(probe, childOf(probe, &table, row), 0);
        texts += cell == nullptr ? std::string{"<null>"} : probe.textOf(*cell);
    }
    return texts;
}

/// @brief Keys as text, for a failure message.
/// @param keys The keys.
/// @return For example `1,"b"`.
[[nodiscard]] inline std::string keyTexts(std::vector<Key> const& keys) {
    std::string texts;
    for (Key const& key : keys) {
        if (!texts.empty()) {
            texts += ',';
        }
        if (auto const* const number = std::get_if<std::int64_t>(&key); number != nullptr) {
            texts += std::to_string(*number);
        } else {
            texts += '"' + std::get<std::string>(key) + '"';
        }
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
    ContainerWidget const& slot = rootContainer(view);
    Checks checks;
    checks.text("the case for key 0", "zero", childTexts(probe, slot));
    which.set(1);
    probe.settle();
    checks.text("the case for key 1", "one", childTexts(probe, slot));
    which.set(7);
    probe.settle();
    checks.that(probe.childCount(slot) == 0, "a key with no case and no fallback still shows a child");
    which.set(0);
    probe.settle();
    checks.text("the case for key 0 again", "zero", childTexts(probe, slot));
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
    ContainerWidget const& bar = rootContainer(view);
    Checks checks;
    checks.that(probe.childCount(bar) == 1, "Tabs mounted a page that was never selected");
    selected.set(1);
    probe.settle();
    checks.that(probe.childCount(bar) == 2, "selecting a tab did not mount its page");
    Widget const* const first = probe.childAt(bar, 0);
    Widget const* const second = probe.childAt(bar, 1);
    checks.that(first != nullptr && !probe.visibleOf(*first), "the unselected tab's page is shown");
    checks.that(second != nullptr && probe.visibleOf(*second), "the selected tab's page is hidden");
    selected.set(0);
    probe.settle();
    // Read again rather than through `first`: a backend that destroyed the page would leave that pointer dangling.
    checks.that(probe.childCount(bar) == 2, "re-selecting a tab mounted its page again");
    Widget const* const reselected = probe.childAt(bar, 0);
    checks.that(reselected != nullptr && probe.visibleOf(*reselected), "the re-selected tab's page is hidden");
    return checks.result();
}

/// @brief Case: relabelled tabs keep the highlighted tab and its page. With B highlighted and its page shown, a new
///        caption for B leaves B highlighted under the new caption, and B's page the one visible.
///
/// The tab count stays the same, so a backend must replace the captions in place: one that rebuilds its tab buttons
/// loses the highlight, and one that ignores a second `setTabs` keeps the old caption.
/// @param probe The backend under test.
/// @return `nullopt`, or what went wrong.
[[nodiscard]] inline std::optional<std::string> relabelledTabsKeepHighlight(ConformanceProbe& probe) {
    reactive::Signal<std::size_t> selected{probe.runtime(), 0};
    reactive::Signal<std::string> caption{probe.runtime(), "B"};
    Mounted const view{
        probe.runtime(), probe.backend(),
        ui::tabs({.tabs = {{.label = "A", .node = ui::text({.text = "first"})},
                           {.label = [&caption] { return caption.get(); }, .node = ui::text({.text = "second"})}},
                  .selected = [&selected] { return selected.get(); },
                  .onSelect = [&selected](std::size_t index) { selected.set(index); }})};
    probe.settle();
    selected.set(1);
    probe.settle();
    ContainerWidget const& bar = rootContainer(view);
    Checks checks;
    checks.text("the highlighted tab before the relabel", "B", probe.textOf(bar));
    caption.set("B \u00b7 3");
    probe.settle();
    checks.text("the highlighted tab after the relabel", "B \u00b7 3", probe.textOf(bar));
    checks.that(probe.childCount(bar) == 2, "a relabel mounted or dropped a page");
    Widget const* const first = probe.childAt(bar, 0);
    Widget const* const second = probe.childAt(bar, 1);
    checks.that(first != nullptr && !probe.visibleOf(*first), "a relabel showed the page of a tab not highlighted");
    checks.that(second != nullptr && probe.visibleOf(*second), "a relabel hid the highlighted tab's page");
    return checks.result();
}

/// @brief Case: a ForEach updates a row whose key stays in place, keeping its widget and not building its view
///        again.
///
/// The build count is the deciding check: a widget address alone cannot tell a kept widget from a new one that the
/// allocator placed where the old one was.
/// @param probe The backend under test.
/// @return `nullopt`, or what went wrong.
[[nodiscard]] inline std::optional<std::string> forEachUpdatesInPlace(ConformanceProbe& probe) {
    reactive::Signal<std::vector<Entry>> rows{probe.runtime(), {{.id = 1, .label = "a"}, {.id = 2, .label = "b"}}};
    int builds = 0;
    Mounted const view{probe.runtime(), probe.backend(), entryList(rows, &builds)};
    probe.settle();
    ContainerWidget const& list = rootContainer(view);
    Widget const* const before = probe.childAt(list, 0);
    rows.set({{.id = 1, .label = "a2"}, {.id = 2, .label = "b"}});
    probe.settle();
    Checks checks;
    checks.that(builds == 2, "updating a kept row built its view again: " + std::to_string(builds) +
                                 " row views built for two keys");
    checks.that(probe.childAt(list, 0) == before, "updating a kept row replaced its widget");
    checks.text("the rows after an update", "a2,b", childTexts(probe, list));
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
    ContainerWidget const& list = rootContainer(view);
    Checks checks;
    checks.text("the rows as mounted", "a,b,c", childTexts(probe, list));
    rows.set({{.id = 3, .label = "c"}, {.id = 1, .label = "a"}});
    probe.settle();
    checks.text("the rows after removing b and moving c first", "c,a", childTexts(probe, list));
    rows.set({{.id = 3, .label = "c"}, {.id = 4, .label = "d"}, {.id = 1, .label = "a"}});
    probe.settle();
    checks.text("the rows after inserting d in the middle", "c,d,a", childTexts(probe, list));
    return checks.result();
}

/// @brief Case: a widget moved among its siblings keeps its native state; here, the text a user typed into a field.
/// @param probe The backend under test.
/// @return `nullopt`, or what went wrong.
[[nodiscard]] inline std::optional<std::string> movedRowKeepsNativeState(ConformanceProbe& probe) {
    reactive::Signal<std::vector<Entry>> rows{
        probe.runtime(), {{.id = 1, .label = "a"}, {.id = 2, .label = "b"}, {.id = 3, .label = "c"}}};
    // The field's text is the widget's own: no binding sets it after the mount, so only the user's typing does.
    Mounted const view{probe.runtime(), probe.backend(),
                       ui::forEach<Entry>(
                           rows, [](Entry const& entry) { return Key{entry.id}; },
                           [](reactive::Signal<Entry> const& entry) {
                               return ui::textInput({.placeholder = [&entry] { return entry.get().label; }});
                           })};
    probe.settle();
    ContainerWidget const& list = rootContainer(view);
    Widget* const field = probe.childAt(list, 0);
    if (field == nullptr) {
        return "a ForEach of text fields shows no first field";
    }
    probe.type(*field, "edited");
    probe.settle();
    Checks checks;
    checks.text("the fields after typing into the first", "edited,,", childTexts(probe, list));
    rows.set({{.id = 2, .label = "b"}, {.id = 3, .label = "c"}, {.id = 1, .label = "a"}});
    probe.settle();
    checks.that(probe.childAt(list, 2) == field, "moving a row replaced its widget");
    checks.text("the fields after moving the edited one last; moveChild must keep a widget's native state", ",,edited",
                childTexts(probe, list));
    return checks.result();
}

/// @brief Case: a Dialog holds its content only while it is open.
/// @param probe The backend under test.
/// @return `nullopt`, or what went wrong.
[[nodiscard]] inline std::optional<std::string> dialogHoldsContentWhileOpen(ConformanceProbe& probe) {
    reactive::Signal<bool> open{probe.runtime(), false};
    Mounted const view{
        probe.runtime(), probe.backend(),
        ui::dialog(
            {.open = [&open] { return open.get(); }, .title = "Confirm", .child = ui::text({.text = "inside"})})};
    probe.settle();
    ContainerWidget const& dialog = rootContainer(view);
    Checks checks;
    checks.that(probe.childCount(dialog) == 0, "a closed Dialog holds content");
    open.set(true);
    probe.settle();
    checks.text("an open Dialog's content", "inside", childTexts(probe, dialog));
    open.set(false);
    probe.settle();
    checks.that(probe.childCount(dialog) == 0, "a Dialog closed again still holds content");
    return checks.result();
}

/// @brief Case: a new dialog is closed, and a closed dialog takes no input, neither a dismissal nor a click on a
///        child; an open one takes both.
///
/// Built through the backend directly: a mount opens or closes every dialog it makes, and mounts a dialog's content
/// only while it is open, so neither a new dialog nor a closed one with children can be reached through it.
/// @param probe The backend under test.
/// @return `nullopt`, or what went wrong.
[[nodiscard]] inline std::optional<std::string> closedDialogTakesNoInput(ConformanceProbe& probe) {
    int clicks = 0;
    int dismissals = 0;
    std::unique_ptr<DialogWidget> const dialog = probe.backend().createDialog(nullptr);
    dialog->setTitle("Confirm");
    dialog->setOnDismiss([&dismissals] { ++dismissals; });
    std::unique_ptr<ButtonWidget> const button = probe.backend().createButton(dialog.get());
    button->setLabel("OK");
    button->setOnClick([&clicks] { ++clicks; });
    probe.settle();
    probe.click(*button);
    probe.dismiss(*dialog);
    probe.settle();
    Checks checks;
    checks.that(clicks == 0, "a click reached a button inside a new dialog; a new dialog is closed");
    checks.that(dismissals == 0, "a new dialog was dismissed; a new dialog is closed");
    dialog->setOpen(true);
    probe.settle();
    probe.click(*button);
    probe.settle();
    checks.that(clicks == 1,
                "a click on a button inside an open dialog ran its action " + std::to_string(clicks) + " times");
    probe.dismiss(*dialog);
    probe.settle();
    checks.that(dismissals == 1,
                "dismissing an open dialog called onDismiss " + std::to_string(dismissals) + " times");
    dialog->setOpen(false);
    probe.settle();
    probe.click(*button);
    probe.dismiss(*dialog);
    probe.settle();
    checks.that(clicks == 1, "a click reached a button inside a closed dialog");
    checks.that(dismissals == 1, "a closed dialog was dismissed");
    return checks.result();
}

/// @brief Case: a widget inside a hidden, disabled or collapsed container takes no input — no click, edit, drag or
///        row selection — and takes it again once the container is shown, enabled and expanded.
/// @param probe The backend under test.
/// @return `nullopt`, or what went wrong.
[[nodiscard]] inline std::optional<std::string> unreachableWidgetsTakeNoInput(ConformanceProbe& probe) {
    reactive::Runtime& runtime = probe.runtime();
    reactive::Signal<bool> hidden{runtime, false};
    reactive::Signal<bool> disabled{runtime, false};
    reactive::Signal<bool> collapsed{runtime, false};
    reactive::Signal<std::vector<Entry>> const rows{runtime, {{.id = 1, .label = "r1"}, {.id = 2, .label = "r2"}}};
    int clicks = 0;
    int edits = 0;
    int drops = 0;
    int selections = 0;
    // Every gate sits at least two levels above the widgets it gates, so a backend that consults only a widget's
    // direct parent fails: an outer Column hides or disables, a Panel inside it collapses, and an inner Column
    // holds the widgets.
    Node const widgets = ui::column(
        {.children = {ui::button({.label = "Go", .onClick = [&clicks] { ++clicks; }}),
                      ui::textInput({.onChange = [&edits](std::string const&) { ++edits; }}),
                      ui::text({.text = "card", .common = {.dragKey = std::optional<Key>{Key{std::int64_t{7}}}}}),
                      entryTable(rows, {.selectionMode = SelectionMode::Single,
                                        .onSelectionChange = [&selections](std::vector<Key> const&, SelectionChange) {
                                            ++selections;
                                        }})}});
    Mounted const gated{runtime, probe.backend(),
                        ui::column({.children = {ui::panel({.title = "Outer",
                                                            .child = widgets,
                                                            .collapsible = true,
                                                            .collapsed = [&collapsed] { return collapsed.get(); }})},
                                    .common = {.visible = [&hidden] { return !hidden.get(); },
                                               .enabled = [&disabled] { return !disabled.get(); }}})};
    Mounted const target{runtime, probe.backend(),
                         ui::panel({.title = "Target", .common = {.onDrop = [&drops](Key const&) { ++drops; }}})};
    probe.settle();
    Widget const* const column = childOf(probe, childOf(probe, &gated.root(), 0), 0);
    Widget* const button = childOf(probe, column, 0);
    Widget* const field = childOf(probe, column, 1);
    Widget* const card = childOf(probe, column, 2);
    Widget* const table = childOf(probe, column, 3);
    if (button == nullptr || field == nullptr || card == nullptr || table == nullptr) {
        return "a Column in a Panel in a Column does not show the inner Column's four children";
    }

    struct Gate {
        std::string_view name;
        reactive::Signal<bool>* flag;
    };
    std::array<Gate, 3> const gates{{{.name = "hidden", .flag = &hidden},
                                     {.name = "disabled", .flag = &disabled},
                                     {.name = "collapsed", .flag = &collapsed}}};
    std::size_t row = 1;  // each round selects the row not selected yet, so a reachable table reports a change
    std::string text = "a";
    Checks checks;
    for (Gate const& gate : gates) {
        std::string const where = " inside a " + std::string{gate.name} + " container";
        auto const act = [&] {
            probe.click(*button);
            probe.type(*field, text);
            probe.drag(*card, target.root());
            probe.selectRows(*table, {row});
            probe.settle();
        };
        gate.flag->set(true);
        probe.settle();
        std::array<int, 4> const before{clicks, edits, drops, selections};
        act();
        checks.that(clicks == before.at(0), "a click reached a button" + where);
        checks.that(edits == before.at(1), "typing reached a text field" + where);
        checks.that(drops == before.at(2), "a widget" + where + " was dragged and dropped");
        checks.that(selections == before.at(3), "a row selection reached a table" + where);
        gate.flag->set(false);
        probe.settle();
        act();
        std::string const again = " once its container was no longer " + std::string{gate.name};
        checks.that(clicks == before.at(0) + 1, "a click did not reach a button" + again);
        checks.that(edits > before.at(1), "typing did not reach a text field" + again);
        checks.that(drops == before.at(2) + 1, "a drag did not deliver its key" + again);
        checks.that(selections == before.at(3) + 1, "a row selection did not reach a table" + again);
        row = 1 - row;
        text += "b";
    }
    return checks.result();
}

/// @brief Case: a setter given the text a field already shows changes nothing; in particular the cursor stays.
///
/// Built through the backend directly, so the setter is called with the shown text on purpose: a mount sends no such
/// call for an accepted edit, but a re-assertion of a transformed one, or another renderer, may.
/// @param probe The backend under test.
/// @return `nullopt`, or what went wrong.
[[nodiscard]] inline std::optional<std::string> unchangedTextKeepsCursor(ConformanceProbe& probe) {
    std::unique_ptr<TextInputWidget> const field = probe.backend().createTextInput(nullptr, TextInputMode::SingleLine);
    probe.settle();
    probe.type(*field, "abc");
    probe.moveCursor(*field, 1);
    probe.settle();
    Checks checks;
    checks.that(probe.cursorOf(*field) == 1,
                "moving the cursor to 1 left it at " + std::to_string(probe.cursorOf(*field)));
    field->setText("abc");
    probe.settle();
    checks.text("the field after setText with the text it shows", "abc", probe.textOf(*field));
    checks.that(probe.cursorOf(*field) == 1, "setText with the text the field shows moved the cursor from 1 to " +
                                                 std::to_string(probe.cursorOf(*field)));
    return checks.result();
}

/// @brief Case: an edit the document refuses snaps back, and one it transforms shows the transform.
/// @param probe The backend under test.
/// @return `nullopt`, or what went wrong.
[[nodiscard]] inline std::optional<std::string> textInputIsControlled(ConformanceProbe& probe) {
    reactive::Signal<std::string> name{probe.runtime(), "kept"};
    bool refuse = true;
    Mounted const view{probe.runtime(), probe.backend(),
                       ui::textInput({.value = [&name] { return name.get(); },
                                      .onChange =
                                          [&name, &refuse](std::string text) {
                                              if (!refuse) {
                                                  std::ranges::transform(text, text.begin(), [](char character) {
                                                      return character >= 'a' && character <= 'z'
                                                                 ? static_cast<char>(character - 'a' + 'A')
                                                                 : character;
                                                  });
                                                  name.set(std::move(text));
                                              }
                                          }})};
    probe.settle();
    Checks checks;
    probe.type(view.root(), "typed");
    probe.settle();
    checks.text("the field after an edit the document refused", "kept", probe.textOf(view.root()));
    refuse = false;
    // A character past `z`, and a capital, are left as they are.
    probe.type(view.root(), "ty~Ped");
    probe.settle();
    checks.text("the field after an edit the document upper-cased", "TY~PED", probe.textOf(view.root()));
    checks.text("the value after an edit the document upper-cased", "TY~PED", name.peek());
    return checks.result();
}

/// @brief Case: a dialog the user dismisses but the document keeps open is shown again, with its content.
/// @param probe The backend under test.
/// @return `nullopt`, or what went wrong.
[[nodiscard]] inline std::optional<std::string> refusedDismissalKeepsDialogOpen(ConformanceProbe& probe) {
    reactive::Signal<bool> const open{probe.runtime(), true};
    int dismissals = 0;
    int clicks = 0;
    Mounted const view{probe.runtime(), probe.backend(),
                       ui::dialog({.open = [&open] { return open.get(); },
                                   .title = "Confirm",
                                   .child = ui::button({.label = "OK", .onClick = [&clicks] { ++clicks; }}),
                                   .onDismiss = [&dismissals] { ++dismissals; }})};
    probe.settle();
    ContainerWidget const& dialog = rootContainer(view);
    probe.dismiss(view.root());
    probe.settle();
    Checks checks;
    checks.that(dismissals == 1, "dismissing the dialog called onDismiss " + std::to_string(dismissals) + " times");
    Widget* const button = probe.childAt(dialog, 0);
    checks.that(button != nullptr, "a dialog kept open lost its content");
    if (button != nullptr) {
        probe.click(*button);
        probe.settle();
    }
    checks.that(clicks == 1, "a click inside a dialog the document kept open ran its action " +
                                 std::to_string(clicks) + " times; the dialog did not open again");
    return checks.result();
}

/// @brief Case: no setter calls a handler. Setting a value, replacing options, items or rows, requesting a selection,
///        opening or closing — none of it is a user's doing, so none of it reaches `onChange`, `onToggle`,
///        `onSelect`, `onSelectionChange` and the like.
/// @param probe The backend under test.
/// @return `nullopt`, or what went wrong.
[[nodiscard]] inline std::optional<std::string> settersCallNoHandler(ConformanceProbe& probe) {
    using Options = std::vector<SelectOption>;
    using std::chrono::milliseconds;
    reactive::Runtime& runtime = probe.runtime();
    reactive::Signal<int> phase{runtime, 0};
    reactive::Signal<std::vector<Entry>> rows{runtime, {{.id = 1, .label = "a"}, {.id = 2, .label = "b"}}};
    std::vector<std::string> fired;
    auto const note = [&fired](std::string_view handler) {
        return [&fired, handler](auto const&...) { fired.emplace_back(handler); };
    };
    auto const odd = [&phase] { return phase.get() % 2 == 1; };
    auto const instant = [odd, &phase] {
        using Instant = std::chrono::sys_time<milliseconds>;
        return odd() ? std::optional<morph::time::Timestamp>{morph::time::DateTime{Instant{milliseconds{phase.get()}}}}
                     : std::nullopt;
    };
    Mounted const view{
        runtime, probe.backend(),
        ui::column(
            {.children = {
                 ui::textInput({.value = [&phase] { return "v" + std::to_string(phase.get()); },
                                .onChange = note("TextInput onChange"),
                                .onSubmit = note("TextInput onSubmit")}),
                 ui::checkbox({.label = "c", .checked = odd, .onToggle = note("Checkbox onToggle")}),
                 // The requested key is 2, then 1; the options hold 2 in odd phases only, so setOptions
                 // re-resolves the request, which is not a user's choice either.
                 ui::select(
                     {.options =
                          [odd] {
                              // Built up by name rather than as two braced lists in a conditional, whose backing
                              // arrays GCC's dangling-pointer analysis mistakes for escaping temporaries.
                              Options options;
                              options.push_back({.key = Key{std::int64_t{1}}, .label = "One"});
                              if (odd()) {
                                  options.push_back({.key = Key{std::int64_t{2}}, .label = "Two"});
                              }
                              return options;
                          },
                      .selected = [&phase] { return std::optional<Key>{Key{std::int64_t{phase.get() < 2 ? 2 : 1}}}; },
                      .onSelect = note("Select onSelect")}),
                 ui::menu({.items = {{.label = [&phase] { return "m" + std::to_string(phase.get()); },
                                      .onSelect = note("Menu item onSelect")}}}),
                 ui::slider({.value = [&phase] { return std::int64_t{phase.get()}; },
                             .onChange = note("Slider onChange"),
                             .onCommit = note("Slider onCommit")}),
                 ui::dateTimeInput({.value = instant, .onChange = note("DateTimeInput onChange")}),
                 ui::filePicker({.path = [&phase] { return "/p" + std::to_string(phase.get()); },
                                 .onPicked = note("FilePicker onPicked")}),
                 ui::panel({.title = "p", .collapsible = true, .collapsed = odd, .onToggle = note("Panel onToggle")}),
                 ui::tabs({.tabs = {{.label = "A", .node = ui::text({.text = "first"})},
                                    {.label = "B", .node = ui::text({.text = "second"})}},
                           .selected = [odd] { return odd() ? std::size_t{1} : std::size_t{0}; },
                           .onSelect = note("Tabs onSelect")}),
                 ui::dialog({.open = odd, .title = "d", .onDismiss = note("Dialog onDismiss")}),
                 entryTable(rows, {.selectionMode = SelectionMode::Multiple,
                                   .selection =
                                       [&phase] {
                                           return phase.get() < 2
                                                      ? std::vector<Key>{Key{std::int64_t{3}}}
                                                      : std::vector<Key>{Key{std::int64_t{1}}, Key{std::int64_t{2}}};
                                       },
                                   .onSelectionChange = note("Table onSelectionChange"),
                                   .sort =
                                       [odd] {
                                           return odd() ? std::optional<TableSort>{TableSort{
                                                              .column = 0, .direction = SortDirection::Descending}}
                                                        : std::nullopt;
                                       },
                                   .onSort = note("Table onSort"),
                                   .onActivate = note("Table onActivate")},
                            true),
             }})};
    probe.settle();
    for (int next = 1; next <= 3; ++next) {
        phase.set(next);
        // Rows arriving with a requested key, and rows going away, re-mark the table without a user's doing.
        rows.set(next % 2 == 1 ? std::vector<Entry>{{.id = 2, .label = "b"}, {.id = 3, .label = "c"}}
                               : std::vector<Entry>{{.id = 1, .label = "a"}, {.id = 2, .label = "b"}});
        probe.settle();
    }
    std::string list;
    for (std::string const& handler : fired) {
        list += (list.empty() ? "" : ", ") + handler;
    }
    Checks checks;
    checks.that(fired.empty(), "a setter called a handler; only a user's action may: " + list);
    return checks.result();
}

/// @brief Case: a Select marks the requested key whenever its options contain it, whichever arrives first, and
///        remembers the request across options that lack it.
/// @param probe The backend under test.
/// @return `nullopt`, or what went wrong.
[[nodiscard]] inline std::optional<std::string> selectReResolvesRequestedKey(ConformanceProbe& probe) {
    using Options = std::vector<SelectOption>;
    SelectOption const one{.key = Key{std::int64_t{1}}, .label = "One"};
    SelectOption const two{.key = Key{std::int64_t{2}}, .label = "Two"};
    SelectOption const three{.key = Key{std::int64_t{3}}, .label = "Three"};
    reactive::Signal<Options> options{probe.runtime(), Options{}};
    int choices = 0;
    Mounted const view{probe.runtime(), probe.backend(),
                       ui::select({.options = [&options] { return options.get(); },
                                   .selected = std::optional<Key>{two.key},
                                   .onSelect = [&choices](Key const&) { ++choices; }})};
    probe.settle();
    Checks checks;
    checks.text("a Select whose requested key came before any options", "", probe.textOf(view.root()));
    options.set({one, three});
    probe.settle();
    checks.text("a Select whose options lack the requested key", "", probe.textOf(view.root()));
    options.set({one, two});
    probe.settle();
    checks.text("a Select once options with the requested key arrived", "Two", probe.textOf(view.root()));
    options.set({one});
    probe.settle();
    checks.text("a Select whose new options lack the requested key", "", probe.textOf(view.root()));
    options.set({two, one});
    probe.settle();
    checks.text("a Select whose options bring the requested key back", "Two", probe.textOf(view.root()));
    checks.that(choices == 0, "re-marking the requested key called onSelect " + std::to_string(choices) + " times");
    return checks.result();
}

/// @brief Case: a Table in `Multiple` mode reports a user's change as exactly the existing rows now selected.
///
/// A requested key without a row stays requested through row changes and is marked when its row arrives, and a
/// row that goes is unmarked, all without a handler call; a user's change drops the requested keys without a row.
/// @param probe The backend under test.
/// @return `nullopt`, or what went wrong.
[[nodiscard]] inline std::optional<std::string> tableReportsExistingSelectedRows(ConformanceProbe& probe) {
    reactive::Runtime& runtime = probe.runtime();
    std::vector<Entry> const three{{.id = 1, .label = "a"}, {.id = 2, .label = "b"}, {.id = 3, .label = "c"}};
    std::vector<Entry> withNine = three;
    withNine.push_back({.id = 9, .label = "i"});
    reactive::Signal<std::vector<Entry>> rows{runtime, three};
    reactive::Signal<std::vector<Key>> selection{runtime, {Key{std::int64_t{2}}, Key{std::int64_t{9}}}};
    std::vector<std::vector<Key>> reports;
    // The selection is a slot, so the application writes what the user picked for the table to keep showing it.
    Mounted const view{runtime, probe.backend(),
                       entryTable(rows, {.selectionMode = SelectionMode::Multiple,
                                         .selection = [&selection] { return selection.get(); },
                                         .onSelectionChange =
                                             [&reports, &selection](std::vector<Key> keys, SelectionChange) {
                                                 reports.push_back(keys);
                                                 selection.set(std::move(keys));
                                             }})};
    probe.settle();
    Checks checks;
    checks.text("the rows marked for requested keys 2 and 9, with no row 9", "b", selectedTexts(probe, view.root()));
    rows.set(withNine);
    probe.settle();
    checks.text("the rows marked once row 9 arrived", "b,i", selectedTexts(probe, view.root()));
    rows.set(three);
    probe.settle();
    checks.text("the rows marked once row 9 went", "b", selectedTexts(probe, view.root()));
    rows.set(withNine);
    probe.settle();
    checks.text("the rows marked once row 9 returned", "b,i", selectedTexts(probe, view.root()));
    checks.that(reports.empty(), "rows arriving or going called onSelectionChange");
    rows.set(three);
    probe.settle();
    probe.selectRows(view.root(), {0, 2});
    probe.settle();
    checks.text("the rows marked after the user selected a and c", "a,c", selectedTexts(probe, view.root()));
    // A backend may report each step of the user's picking (a click, then a Ctrl-click), so only the last report
    // must be the whole selection; but no report may name a key without a row.
    bool const rowlessReported = std::ranges::any_of(reports, [](std::vector<Key> const& keys) {
        return std::ranges::any_of(keys, [](Key const& key) {
            return key != Key{std::int64_t{1}} && key != Key{std::int64_t{2}} && key != Key{std::int64_t{3}};
        });
    });
    std::vector<Key> last = reports.empty() ? std::vector<Key>{} : reports.back();
    std::ranges::sort(last);
    checks.that(!reports.empty(), "a user's selection of rows 1 and 3 did not call onSelectionChange");
    checks.that(!rowlessReported,
                "a user's selection, with key 9 requested and no row 9, reported a key without a row");
    checks.text("the last report of a user's selection of rows 1 and 3, with key 9 requested and no row 9", "1,3",
                keyTexts(last));
    rows.set(withNine);
    probe.settle();
    checks.text("the rows marked once row 9 arrived after the user's selection", "a,c",
                selectedTexts(probe, view.root()));
    return checks.result();
}

/// @brief Case: a Table shows the user's selection even when the application binds no selection.
/// @param probe The backend under test.
/// @return `nullopt`, or what went wrong.
[[nodiscard]] inline std::optional<std::string> tableShowsUnboundUserSelection(ConformanceProbe& probe) {
    reactive::Signal<std::vector<Entry>> rows{probe.runtime(), {{.id = 1, .label = "a"}, {.id = 2, .label = "b"}}};
    int reports = 0;
    Mounted const view{
        probe.runtime(), probe.backend(),
        entryTable(rows, {.selectionMode = SelectionMode::Single,
                          .onSelectionChange = [&reports](std::vector<Key> const&, SelectionChange) { ++reports; }})};
    probe.settle();
    probe.selectRows(view.root(), {1});
    probe.settle();
    Checks checks;
    checks.that(reports == 1, "a user's selection called onSelectionChange " + std::to_string(reports) + " times");
    checks.text("the rows marked after the user selected b, with no selection bound", "b",
                selectedTexts(probe, view.root()));
    rows.set({{.id = 1, .label = "a"}, {.id = 2, .label = "b2"}});
    probe.settle();
    checks.text("the rows marked after the selected row was updated", "b2", selectedTexts(probe, view.root()));
    return checks.result();
}

/// @brief Case: a hidden Table cell keeps its column: it stays in its row, so the cells after it keep their places.
///
/// This checks the row's structure only; that the cells after it are drawn under their own headers is the
/// backend's own test to make.
/// @param probe The backend under test.
/// @return `nullopt`, or what went wrong.
[[nodiscard]] inline std::optional<std::string> hiddenCellKeepsItsColumn(ConformanceProbe& probe) {
    reactive::Signal<std::vector<Entry>> const rows{probe.runtime(), {{.id = 1, .label = "a"}}};
    Mounted const view{probe.runtime(), probe.backend(),
                       ui::table<Entry>(
                           {{.label = "Note"}, {.label = "Name"}}, [&rows] { return rows.get(); },
                           [](Entry const& entry) { return Key{entry.id}; },
                           [](reactive::Signal<Entry> const& entry) {
                               return std::vector<Node>{ui::text({.text = "note", .common = {.visible = false}}),
                                                        ui::text({.text = [&entry] { return entry.get().label; }})};
                           })};
    probe.settle();
    Widget const* const row = childOf(probe, &view.root(), 0);
    ContainerWidget const* const cells = row == nullptr ? nullptr : asContainer(*row);
    if (cells == nullptr) {
        return "a Table with one row shows no row container";
    }
    Checks checks;
    checks.text("the cells of a row whose first cell is hidden", "note,a", childTexts(probe, *cells));
    Widget const* const first = probe.childAt(*cells, 0);
    checks.that(first != nullptr && !probe.visibleOf(*first), "the hidden cell is shown");
    return checks.result();
}

/// @brief Case: no handler runs after its widget's destructor returned, even for input that arrived before.
/// @param probe The backend under test.
/// @return `nullopt`, or what went wrong.
[[nodiscard]] inline std::optional<std::string> noHandlerAfterDestruction(ConformanceProbe& probe) {
    int clicks = 0;
    std::optional<Mounted> view;
    view.emplace(probe.runtime(), probe.backend(), ui::button({.label = "Go", .onClick = [&clicks] { ++clicks; }}));
    probe.settle();
    probe.click(view->root());
    probe.settle();
    Checks checks;
    checks.that(clicks == 1, "a click ran the action " + std::to_string(clicks) + " times");
    probe.click(view->root());
    int const before = clicks;
    view.reset();
    probe.settle();
    checks.that(clicks == before, "a click's handler ran after the button was destroyed");
    return checks.result();
}

/// @brief What `LifeSentinel` reports about the handler that holds it.
struct HandlerLife {
    /// @brief The sentinel inside the handler while that handler runs; null otherwise.
    void const* running = nullptr;
    /// @brief Set when the running handler's sentinel was destroyed before the handler returned.
    bool freedWhileRunning = false;
};

/// @brief Captured by value in a handler, tells whether the backend destroyed the handler while it ran.
///
/// Every copy of the handler holds its own sentinel; only the destruction of the copy that is running counts.
class LifeSentinel {
public:
    /// @param life Where to report; must outlive every copy.
    explicit LifeSentinel(HandlerLife& life) noexcept : _life{&life} {}

    /// @brief Reports its own destruction when it belongs to the running handler.
    ~LifeSentinel() {
        if (_life->running == this) {
            _life->freedWhileRunning = true;
        }
    }

    /// @brief A copy, for another copy of the handler.
    LifeSentinel(LifeSentinel const&) = default;
    /// @brief Copies the report target.
    /// @return This sentinel.
    LifeSentinel& operator=(LifeSentinel const&) = default;
    /// @brief A copy, for a moved handler.
    LifeSentinel(LifeSentinel&&) = default;
    /// @brief Copies the report target.
    /// @return This sentinel.
    LifeSentinel& operator=(LifeSentinel&&) = default;

    /// @brief Where it reports.
    /// @return The report.
    [[nodiscard]] HandlerLife* life() const noexcept { return _life; }

private:
    HandlerLife* _life;
};

/// @brief Case: a handler may destroy its own widget; the backend keeps the running handler alive until it returns,
///        and stays usable after.
/// @param probe The backend under test.
/// @return `nullopt`, or what went wrong.
[[nodiscard]] inline std::optional<std::string> handlerMayDestroyItsWidget(ConformanceProbe& probe) {
    HandlerLife life;
    int clicks = 0;
    std::optional<Mounted> view;
    // After the reset the handler touches nothing it captured: its captures are gone if the backend let them go.
    view.emplace(probe.runtime(), probe.backend(),
                 ui::button({.label = "Close", .onClick = [sentinel = LifeSentinel{life}, &view, &clicks] {
                                 HandlerLife* const report = sentinel.life();
                                 std::optional<Mounted>* const own = &view;
                                 ++clicks;
                                 report->running = &sentinel;
                                 own->reset();
                                 report->running = nullptr;
                             }}));
    probe.settle();
    probe.click(view->root());
    probe.settle();
    Checks checks;
    checks.that(clicks == 1,
                "the click that destroys its own button ran the action " + std::to_string(clicks) + " times");
    checks.that(!view.has_value(), "the click did not destroy its own button");
    checks.that(!life.freedWhileRunning,
                "destroying a button from its own handler destroyed the running handler; the backend must keep it "
                "alive until it returns");
    Mounted const after{probe.runtime(), probe.backend(), ui::text({.text = "after"})};
    probe.settle();
    checks.text("a Text mounted after a handler destroyed its own widget", "after", probe.textOf(after.root()));
    checks.that(clicks == 1, "a handler ran again after it destroyed its own widget");
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
    Mounted const target{
        probe.runtime(), probe.backend(),
        ui::panel({.title = "Done", .common = {.accepts = std::move(accepts), .onDrop = [&dropped](Key key) {
                                                   dropped.push_back(std::move(key));
                                               }}})};
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

/// @brief Case: a chord goes to the innermost widget that declares it, and one only an outer widget declares reaches
///        that widget.
/// @param probe The backend under test.
/// @return `nullopt`, or what went wrong.
[[nodiscard]] inline std::optional<std::string> innermostChordWins(ConformanceProbe& probe) {
    std::vector<std::string> pressed;
    Mounted const view{
        probe.runtime(), probe.backend(),
        ui::column({.children = {ui::column(
                        {.children = {ui::button(
                             {.label = "Save",
                              .common = {.keys = {{.chord = "Ctrl+S",
                                                   .onPress = [&pressed] { pressed.emplace_back("inner"); }}}}})}})},
                    .common = {.keys = {{.chord = "Ctrl+S", .onPress = [&pressed] { pressed.emplace_back("outer"); }},
                                        {.chord = "Ctrl+Q",
                                         .onPress = [&pressed] { pressed.emplace_back("outer quit"); }}}}})};
    probe.settle();
    Widget* const button = childOf(probe, childOf(probe, &view.root(), 0), 0);
    if (button == nullptr) {
        return "a Button in a Column in a Column is not shown";
    }
    probe.press(*button, "Ctrl+S");
    probe.settle();
    probe.press(*button, "Ctrl+Q");
    probe.settle();
    Checks checks;
    checks.that(pressed == std::vector<std::string>{"inner", "outer quit"},
                "Ctrl+S then Ctrl+Q with focus on the button ran " + std::to_string(pressed.size()) +
                    " handlers, not the inner Ctrl+S and the outer Ctrl+Q");
    return checks.result();
}

/// @brief Case: a chord pressed inside a container hidden two levels up runs nothing; while shown it runs once.
/// @param probe The backend under test.
/// @return `nullopt`, or what went wrong.
[[nodiscard]] inline std::optional<std::string> hiddenChordRunsNothing(ConformanceProbe& probe) {
    reactive::Signal<bool> shown{probe.runtime(), true};
    int presses = 0;
    Mounted const view{
        probe.runtime(), probe.backend(),
        ui::column({.children = {ui::column(
                        {.children = {ui::button(
                             {.label = "Save",
                              .common = {.keys = {{.chord = "Ctrl+S", .onPress = [&presses] { ++presses; }}}}})}})},
                    .common = {.visible = [&shown] { return shown.get(); }}})};
    probe.settle();
    Widget* const button = childOf(probe, childOf(probe, &view.root(), 0), 0);
    if (button == nullptr) {
        return "a Button in a Column in a Column is not shown";
    }
    probe.press(*button, "Ctrl+S");  // shown: the chord runs, so the probe's press is known to reach handlers
    probe.settle();
    shown.set(false);
    probe.settle();
    probe.press(*button, "Ctrl+S");
    probe.settle();
    Checks checks;
    checks.that(presses == 1, "a chord ran " + std::to_string(presses) +
                                  " times; once while shown and not again once hidden is expected");
    return checks.result();
}

/// @brief Case: Enter on a focused button that declares an Enter chord runs the chord, not the click. A chord the
///        focused widget declares comes before the widget's own handling of the key.
/// @param probe The backend under test.
/// @return `nullopt`, or what went wrong.
[[nodiscard]] inline std::optional<std::string> ownChordBeforeOwnKey(ConformanceProbe& probe) {
    std::vector<std::string> ran;
    Mounted const view{
        probe.runtime(), probe.backend(),
        ui::column({.children = {
                        ui::column({.children = {ui::button({.label = "Go",
                                                             .onClick = [&ran] { ran.emplace_back("click"); },
                                                             .common = {.keys = {{.chord = "Enter", .onPress = [&ran] {
                                                                                      ran.emplace_back("chord");
                                                                                  }}}}})}})}})};
    probe.settle();
    Widget* const button = childOf(probe, childOf(probe, &view.root(), 0), 0);
    if (button == nullptr) {
        return "a Button in a Column in a Column is not shown";
    }
    probe.press(*button, "Enter");
    probe.settle();
    Checks checks;
    checks.that(ran == std::vector<std::string>{"chord"}, "Enter on a button that declares an Enter chord ran " +
                                                              std::to_string(ran.size()) +
                                                              " handlers, not the chord alone");
    return checks.result();
}

/// @brief Case: Enter on a focused button inside a column that declares an Enter chord clicks the button, and the
///        column's chord does not run. A button takes Enter for itself before its ancestors' chords.
/// @param probe The backend under test.
/// @return `nullopt`, or what went wrong.
[[nodiscard]] inline std::optional<std::string> ownKeyBeforeAncestorChord(ConformanceProbe& probe) {
    std::vector<std::string> ran;
    Mounted const view{
        probe.runtime(), probe.backend(),
        ui::column({.children = {ui::button({.label = "Go", .onClick = [&ran] { ran.emplace_back("click"); }})},
                    .common = {.keys = {{.chord = "Enter", .onPress = [&ran] { ran.emplace_back("chord"); }}}}})};
    probe.settle();
    Widget* const button = childOf(probe, &view.root(), 0);
    if (button == nullptr) {
        return "a Button in a Column is not shown";
    }
    probe.press(*button, "Enter");
    probe.settle();
    Checks checks;
    checks.that(ran == std::vector<std::string>{"click"},
                "Enter on a button inside a column that declares an Enter chord ran " + std::to_string(ran.size()) +
                    " handlers, not the click alone");
    return checks.result();
}

/// @brief A dialog, open, whose content column declares an Enter chord standing in for a document's `default`, and
///        holds @p field.
/// @param ran Where the chord records `default`.
/// @param field The node inside the content column.
/// @return The dialog.
[[nodiscard]] inline Node dialogWithDefault(std::vector<std::string>& ran, Node field) {
    return ui::dialog(
        {.open = true,
         .title = "Confirm",
         .child = ui::column(
             {.children = {std::move(field)},
              .common = {.keys = {{.chord = "Enter", .onPress = [&ran] { ran.emplace_back("default"); }}}}})});
}

/// @brief The widget inside the content column of the dialog `dialogWithDefault` makes.
/// @param probe The backend under test.
/// @param view The mounted dialog.
/// @return The widget, or null while it is not shown.
[[nodiscard]] inline Widget* insideDialog(ConformanceProbe& probe, Mounted const& view) {
    return childOf(probe, childOf(probe, &view.root(), 0), 0);
}

/// @brief Case: in a dialog, Enter on a single-line text input with a submit handler commits and submits it, and the
///        dialog's Enter chord does not run.
/// @param probe The backend under test.
/// @return `nullopt`, or what went wrong.
[[nodiscard]] inline std::optional<std::string> submittingFieldTakesEnter(ConformanceProbe& probe) {
    std::vector<std::string> ran;
    Mounted const view{
        probe.runtime(), probe.backend(),
        dialogWithDefault(
            ran, ui::textInput({.onSubmit = [&ran](std::string const& text) { ran.push_back("submit " + text); },
                                .onCommit = [&ran](std::string const& text) { ran.push_back("commit " + text); }}))};
    probe.settle();
    Widget* const field = insideDialog(probe, view);
    if (field == nullptr) {
        return "a TextInput in an open Dialog's Column is not shown";
    }
    probe.type(*field, "ok");
    probe.settle();
    probe.press(*field, "Enter");
    probe.settle();
    Checks checks;
    checks.that(ran == std::vector<std::string>{"commit ok", "submit ok"},
                "Enter on a submitting field in a dialog ran " + std::to_string(ran.size()) +
                    " handlers, not its commit and its submit alone");
    return checks.result();
}

/// @brief Case: in a dialog, Enter on a single-line text input without a submit handler commits it, and the dialog's
///        Enter chord runs once.
/// @param probe The backend under test.
/// @return `nullopt`, or what went wrong.
[[nodiscard]] inline std::optional<std::string> fieldWithoutSubmitPassesEnterOn(ConformanceProbe& probe) {
    std::vector<std::string> ran;
    Mounted const view{
        probe.runtime(), probe.backend(),
        dialogWithDefault(
            ran, ui::textInput({.onCommit = [&ran](std::string const& text) { ran.push_back("commit " + text); }}))};
    probe.settle();
    Widget* const field = insideDialog(probe, view);
    if (field == nullptr) {
        return "a TextInput in an open Dialog's Column is not shown";
    }
    probe.type(*field, "ok");
    probe.settle();
    probe.press(*field, "Enter");
    probe.settle();
    Checks checks;
    checks.that(ran == std::vector<std::string>{"commit ok", "default"},
                "Enter on a field without a submit handler in a dialog ran " + std::to_string(ran.size()) +
                    " handlers, not its commit and then the dialog's chord");
    return checks.result();
}

/// @brief Case: in a dialog, Enter on a focused check box leaves it unchanged and runs the dialog's Enter chord;
///        Space toggles it and runs no chord.
/// @param probe The backend under test.
/// @return `nullopt`, or what went wrong.
[[nodiscard]] inline std::optional<std::string> checkboxTakesSpaceNotEnter(ConformanceProbe& probe) {
    std::vector<std::string> ran;
    Mounted const view{probe.runtime(), probe.backend(),
                       dialogWithDefault(ran, ui::checkbox({.label = "Agree", .onToggle = [&ran](bool checked) {
                                                                ran.emplace_back(checked ? "checked" : "unchecked");
                                                            }}))};
    probe.settle();
    Widget* const box = insideDialog(probe, view);
    if (box == nullptr) {
        return "a Checkbox in an open Dialog's Column is not shown";
    }
    probe.press(*box, "Enter");
    probe.settle();
    probe.press(*box, "Space");
    probe.settle();
    Checks checks;
    checks.that(ran == std::vector<std::string>{"default", "checked"},
                "Enter then Space on a check box in a dialog ran " + std::to_string(ran.size()) +
                    " handlers, not the dialog's chord and then one toggle");
    return checks.result();
}

/// @brief Case: Enter on a single-line text input that declares an Enter chord itself runs the chord, and neither
///        submits nor commits through Enter.
/// @param probe The backend under test.
/// @return `nullopt`, or what went wrong.
[[nodiscard]] inline std::optional<std::string> fieldChordBeforeSubmit(ConformanceProbe& probe) {
    std::vector<std::string> ran;
    Mounted const view{
        probe.runtime(), probe.backend(),
        ui::column(
            {.children = {ui::textInput(
                 {.onSubmit = [&ran](std::string const& text) { ran.push_back("submit " + text); },
                  .onCommit = [&ran](std::string const& text) { ran.push_back("commit " + text); },
                  .common = {.keys = {{.chord = "Enter", .onPress = [&ran] { ran.emplace_back("chord"); }}}}})}})};
    probe.settle();
    Widget* const field = childOf(probe, &view.root(), 0);
    if (field == nullptr) {
        return "a TextInput in a Column is not shown";
    }
    probe.type(*field, "ok");
    probe.settle();
    probe.press(*field, "Enter");
    probe.settle();
    Checks checks;
    checks.that(ran == std::vector<std::string>{"chord"}, "Enter on a field that declares an Enter chord ran " +
                                                              std::to_string(ran.size()) +
                                                              " handlers, not the chord alone");
    return checks.result();
}

/// @brief Case: the first `autofocus` widget in document order has focus once the mount completes.
/// @param probe The backend under test.
/// @return `nullopt`, or what went wrong.
[[nodiscard]] inline std::optional<std::string> firstAutofocusWins(ConformanceProbe& probe) {
    Mounted const view{
        probe.runtime(), probe.backend(),
        ui::column({.children = {ui::text({.text = "Name"}), ui::textInput({.common = {.autofocus = true}}),
                                 ui::textInput({.common = {.autofocus = true}})}})};
    probe.settle();
    Widget const* const first = childOf(probe, &view.root(), 1);
    Widget const* const second = childOf(probe, &view.root(), 2);
    if (first == nullptr || second == nullptr) {
        return "a Column does not show its three children";
    }
    Checks checks;
    checks.that(probe.hasFocus(*first), "the first autofocus input does not have focus");
    checks.that(!probe.hasFocus(*second), "the second autofocus input has focus");
    return checks.result();
}

/// @brief Case: a menu reports only an enabled entry without a submenu, and an entry enabled later can be chosen.
/// @param probe The backend under test.
/// @return `nullopt`, or what went wrong.
[[nodiscard]] inline std::optional<std::string> onlyEnabledMenuLeavesAreChosen(ConformanceProbe& probe) {
    reactive::Signal<bool> deletable{probe.runtime(), false};
    std::vector<std::string> chosen;
    auto const choose = [&chosen](std::string name) {
        return [&chosen, name = std::move(name)] { chosen.push_back(name); };
    };
    Mounted const view{probe.runtime(), probe.backend(),
                       ui::menu({.items = {{.label = "Recent",
                                            .onSelect = choose("Recent"),
                                            .items = {{.label = "a.txt", .onSelect = choose("a.txt")}}},
                                           {.label = "Delete", .onSelect = choose("Delete"), .enabled = [&deletable] {
                                                return deletable.get();
                                            }}}})};
    probe.settle();
    probe.chooseMenuEntry(view.root(), {0});  // opens the submenu
    probe.chooseMenuEntry(view.root(), {0, 0});
    probe.chooseMenuEntry(view.root(), {1});  // disabled
    probe.settle();
    deletable.set(true);
    probe.settle();
    probe.chooseMenuEntry(view.root(), {1});
    probe.settle();
    Checks checks;
    checks.that(chosen == std::vector<std::string>{"a.txt", "Delete"},
                "choosing a submenu, its entry, a disabled entry and then that entry enabled ran " +
                    std::to_string(chosen.size()) + " handlers, not the submenu's entry and the enabled one");
    return checks.result();
}

/// @brief Case: typing into a read-only input reaches no handler; once editable again, it does.
/// @param probe The backend under test.
/// @return `nullopt`, or what went wrong.
[[nodiscard]] inline std::optional<std::string> readOnlyInputTakesNoEdit(ConformanceProbe& probe) {
    reactive::Signal<bool> readonly{probe.runtime(), true};
    std::vector<std::string> changes;
    Mounted const view{probe.runtime(), probe.backend(),
                       ui::textInput({.value = "kept",
                                      .onChange = [&changes](std::string text) { changes.push_back(std::move(text)); },
                                      .field = {.readonly = [&readonly] { return readonly.get(); }}})};
    probe.settle();
    probe.type(view.root(), "typed");
    probe.settle();
    Checks checks;
    checks.that(changes.empty(), "typing into a read-only input reached onChange");
    checks.that(probe.textOf(view.root()) == "kept",
                "a read-only input shows '" + probe.textOf(view.root()) + "' after typing, not its value 'kept'");
    readonly.set(false);
    probe.settle();
    probe.type(view.root(), "typed");
    probe.settle();
    checks.that(changes == std::vector<std::string>{"typed"},
                "typing into the input once editable again did not reach onChange once with the text");
    return checks.result();
}

/// @brief Case: dismissing a dismissible banner reaches `onDismiss` once, and the banner stays shown.
/// @param probe The backend under test.
/// @return `nullopt`, or what went wrong.
[[nodiscard]] inline std::optional<std::string> dismissedBannerStays(ConformanceProbe& probe) {
    int dismissed = 0;
    Mounted const view{probe.runtime(), probe.backend(),
                       ui::banner({.text = "Saved", .dismissible = true, .onDismiss = [&dismissed] { ++dismissed; }})};
    probe.settle();
    probe.dismiss(view.root());
    probe.settle();
    Checks checks;
    checks.that(dismissed == 1,
                "dismissing the banner ran onDismiss " + std::to_string(dismissed) + " times, not once");
    checks.that(probe.visibleOf(view.root()), "the banner hid itself; the application hides it");
    return checks.result();
}

/// @brief Case: a closed collapsible section's content takes no click and its header does; the user's opening
///        reaches `onToggle`, and once the application opens it the content takes clicks.
/// @param probe The backend under test.
/// @return `nullopt`, or what went wrong.
[[nodiscard]] inline std::optional<std::string> closedCollapsibleKeepsItsHeader(ConformanceProbe& probe) {
    reactive::Signal<bool> open{probe.runtime(), false};
    std::vector<std::string> ran;
    Mounted const view{
        probe.runtime(), probe.backend(),
        ui::collapsible(
            {.title = "Advanced",
             .open = [&open] { return open.get(); },
             .onToggle = [&open](bool wanted) { open.set(wanted); },
             .header = ui::button({.label = "Reset", .onClick = [&ran] { ran.emplace_back("header"); }}),
             .child = ui::button({.label = "Apply", .onClick = [&ran] { ran.emplace_back("content"); }})})};
    probe.settle();
    Widget* const header = childOf(probe, &view.root(), 0);
    Widget* const content = childOf(probe, &view.root(), 1);
    if (header == nullptr || content == nullptr) {
        return "a Collapsible does not show its header and its content as its two children";
    }
    probe.click(*header);
    probe.click(*content);
    probe.settle();
    probe.expand(view.root(), true);
    probe.settle();
    probe.click(*content);
    probe.settle();
    Checks checks;
    checks.that(ran == std::vector<std::string>{"header", "content"},
                "clicking the header and the content while closed, then the content once opened, ran " +
                    std::to_string(ran.size()) + " handlers, not the header's and then the content's");
    return checks.result();
}

/// @brief Case: a drop zone takes a drop of one accepted file, compared without case, and refuses a file it does
///        not accept and several files when it takes one.
/// @param probe The backend under test.
/// @return `nullopt`, or what went wrong.
[[nodiscard]] inline std::optional<std::string> dropZoneTakesWholeDrops(ConformanceProbe& probe) {
    std::vector<std::vector<std::string>> drops;
    Mounted const view{probe.runtime(), probe.backend(),
                       ui::dropZone({.accept = {".csv"}, .onDrop = [&drops](std::vector<std::string> paths) {
                                         drops.push_back(std::move(paths));
                                     }})};
    probe.settle();
    probe.dropFiles(view.root(), {"/in/RUN.CSV"});
    probe.dropFiles(view.root(), {"/in/notes.txt"});
    probe.dropFiles(view.root(), {"/in/a.csv", "/in/b.csv"});
    probe.settle();
    Checks checks;
    checks.that(drops == std::vector<std::vector<std::string>>{{"/in/RUN.CSV"}},
                "of an accepted file, a refused one and two at once, the zone took " + std::to_string(drops.size()) +
                    " drops, not the accepted file alone");
    return checks.result();
}

/// @brief Reads where the layout cases' widgets are drawn, and fails a case whose probe reads no geometry.
class Geometry {
public:
    /// @brief Over @p probe.
    /// @param probe  The backend under test.
    /// @param checks The case's checks.
    Geometry(ConformanceProbe& probe, Checks& checks) : _probe{&probe}, _checks{&checks} {}

    /// @brief Where @p widget is drawn; zero, with the case failed, when the probe reads no geometry.
    /// @param widget The widget.
    /// @return Its bounds.
    [[nodiscard]] WidgetBounds of(Widget const& widget) {
        std::optional<WidgetBounds> const bounds = _probe->boundsOf(widget);
        _checks->that(bounds.has_value(), "the probe reads no bounds, so a layout case measures nothing");
        return bounds.value_or(WidgetBounds{});
    }

    /// @brief Checks that a length is what it should be, within one unit.
    /// @param what     What is measured, for the message.
    /// @param expected The length it should be.
    /// @param actual   The length it is.
    void near(std::string_view what, double expected, double actual) {
        _checks->that(
            actual >= expected - 1.0 && actual <= expected + 1.0,
            std::string{what} + ": expected " + std::to_string(expected) + ", got " + std::to_string(actual));
    }

private:
    ConformanceProbe* _probe;
    Checks* _checks;
};

/// @brief A text that sizes as @p layout says.
/// @param text   The text.
/// @param layout Its size request.
/// @return The node.
[[nodiscard]] inline Node sizedText(std::string text, LayoutHints layout) {
    Text made{.text = std::move(text)};
    made.common.layout = layout;
    return ui::text(std::move(made));
}

/// @brief @p inner inside a column, so it is sized as a child is: a root fills the window or screen whatever its
///        own sizing.
/// @param inner The container a case measures.
/// @return The column.
[[nodiscard]] inline Node insideColumn(Node inner) { return ui::column(Column{.children = {std::move(inner)}}); }

/// @brief @p inner inside a row, so its height is sized as a child's is.
/// @param inner The container a case measures.
/// @return The row.
[[nodiscard]] inline Node insideRow(Node inner) { return ui::row(Row{.children = {std::move(inner)}}); }

/// @brief The container a case measures, the one child of the mounted root, and its children in order.
struct Measured {
    /// @brief The measured container; null when the root holds none.
    Widget* container = nullptr;
    /// @brief Its children.
    std::vector<Widget*> children;
};

/// @brief The container a case measures, inside the root `insideColumn` or `insideRow` made.
/// @param probe The backend under test.
/// @param view  The mount.
/// @return It and its children.
[[nodiscard]] inline Measured measured(ConformanceProbe& probe, Mounted const& view) {
    Measured found;
    found.container = probe.childAt(rootContainer(view), 0);
    ContainerWidget const* const inner = found.container == nullptr ? nullptr : asContainer(*found.container);
    for (std::size_t index = 0; inner != nullptr && index < probe.childCount(*inner); ++index) {
        found.children.push_back(probe.childAt(*inner, index));
    }
    return found;
}

/// @brief Case: along a row, a stretch child's share stops at its maximum and its stretch sibling takes the rest.
/// @param probe The backend under test.
/// @return `nullopt`, or what went wrong.
[[nodiscard]] inline std::optional<std::string> maximumCapsStretchShare(ConformanceProbe& probe) {
    Row row{.children = {sizedText("a", {.width = Sizing::stretch(), .maxWidth = 20}),
                         sizedText("b", {.width = Sizing::stretch()})}};
    row.common.layout = {.width = Sizing::fixed(60)};
    Mounted const view{probe.runtime(), probe.backend(), insideColumn(ui::row(std::move(row)))};
    probe.settle();
    Checks checks;
    Geometry geometry{probe, checks};
    std::vector<Widget*> const children = measured(probe, view).children;
    checks.that(children.size() == 2, "the row holds two children");
    if (children.size() == 2) {
        geometry.near("the capped stretch child's width", 20, geometry.of(*children.at(0)).width);
        geometry.near("its stretch sibling's width", 40, geometry.of(*children.at(1)).width);
    }
    return checks.result();
}

/// @brief Case: along a row, a content child is at least its minimum wide.
/// @param probe The backend under test.
/// @return `nullopt`, or what went wrong.
[[nodiscard]] inline std::optional<std::string> minimumWidensContent(ConformanceProbe& probe) {
    Row row{.children = {sizedText("ab", {.minWidth = 30}), sizedText("c", {})}};
    row.common.layout = {.width = Sizing::fixed(60)};
    Mounted const view{probe.runtime(), probe.backend(), insideColumn(ui::row(std::move(row)))};
    probe.settle();
    Checks checks;
    Geometry geometry{probe, checks};
    std::vector<Widget*> const children = measured(probe, view).children;
    checks.that(children.size() == 2, "the row holds two children");
    if (children.size() == 2) {
        geometry.near("a content text with minWidth 30", 30, geometry.of(*children.at(0)).width);
    }
    return checks.result();
}

/// @brief Case: across a column, a stretch child stops at its maximum, at the start.
/// @param probe The backend under test.
/// @return `nullopt`, or what went wrong.
[[nodiscard]] inline std::optional<std::string> maximumCapsAcross(ConformanceProbe& probe) {
    Column column{.children = {sizedText("card", {.width = Sizing::stretch(), .maxWidth = 44})}};
    column.common.layout = {.width = Sizing::fixed(70)};
    Mounted const view{probe.runtime(), probe.backend(), insideColumn(ui::column(std::move(column)))};
    probe.settle();
    Checks checks;
    Geometry geometry{probe, checks};
    Measured const inside = measured(probe, view);
    std::vector<Widget*> const& children = inside.children;
    checks.that(children.size() == 1, "the column holds one child");
    if (children.size() == 1) {
        WidgetBounds const outer = geometry.of(*inside.container);
        geometry.near("the column's width", 70, outer.width);
        WidgetBounds const card = geometry.of(*children.at(0));
        geometry.near("a stretch child with maxWidth 44 across a 70-unit column", 44, card.width);
        geometry.near("its left edge, at the column's start", outer.x, card.x);
    }
    return checks.result();
}

/// @brief Case: down a column, a stretch child's height stops at its maximum and its stretch sibling takes the
///        rest.
/// @param probe The backend under test.
/// @return `nullopt`, or what went wrong.
[[nodiscard]] inline std::optional<std::string> maximumCapsStretchHeight(ConformanceProbe& probe) {
    Column column{.children = {sizedText("top", {.height = Sizing::stretch(), .maxHeight = 5}),
                               sizedText("rest", {.height = Sizing::stretch()})}};
    column.common.layout = {.width = Sizing::fixed(30), .height = Sizing::fixed(20)};
    Mounted const view{probe.runtime(), probe.backend(), insideRow(ui::column(std::move(column)))};
    probe.settle();
    Checks checks;
    Geometry geometry{probe, checks};
    std::vector<Widget*> const children = measured(probe, view).children;
    checks.that(children.size() == 2, "the column holds two children");
    if (children.size() == 2) {
        geometry.near("the capped stretch child's height", 5, geometry.of(*children.at(0)).height);
        geometry.near("its stretch sibling's height", 15, geometry.of(*children.at(1)).height);
    }
    return checks.result();
}

/// @brief Case: a row 3 units tall with `align: center` places a 1-unit text a unit down.
/// @param probe The backend under test.
/// @return `nullopt`, or what went wrong.
[[nodiscard]] inline std::optional<std::string> rowCentresAcross(ConformanceProbe& probe) {
    Row row{.children = {sizedText("x", {})}, .align = Align::Center};
    row.common.layout = {.width = Sizing::fixed(30), .height = Sizing::fixed(3)};
    Mounted const view{probe.runtime(), probe.backend(), insideColumn(ui::row(std::move(row)))};
    probe.settle();
    Checks checks;
    Geometry geometry{probe, checks};
    Measured const inside = measured(probe, view);
    checks.that(inside.children.size() == 1, "the row holds one child");
    if (inside.children.size() == 1) {
        WidgetBounds const outer = geometry.of(*inside.container);
        WidgetBounds const text = geometry.of(*inside.children.at(0));
        geometry.near("the text's height", 1, text.height);
        geometry.near("the text's offset down the row", 1, text.y - outer.y);
    }
    return checks.result();
}

/// @brief Case: a column with `justify: end` places its last child's end at its own end, and `between` its first
///        child at the start and its last at the end.
/// @param probe The backend under test.
/// @return `nullopt`, or what went wrong.
[[nodiscard]] inline std::optional<std::string> columnJustifies(ConformanceProbe& probe) {
    Checks checks;
    Geometry geometry{probe, checks};
    for (Justify const justify : {Justify::End, Justify::Between}) {
        Column column{.children = {sizedText("a", {}), sizedText("b", {}), sizedText("c", {})}, .justify = justify};
        column.common.layout = {.width = Sizing::fixed(20), .height = Sizing::fixed(12)};
        Mounted const view{probe.runtime(), probe.backend(), insideRow(ui::column(std::move(column)))};
        probe.settle();
        Measured const inside = measured(probe, view);
        checks.that(inside.children.size() == 3, "the column holds three children");
        if (inside.children.size() != 3) {
            continue;
        }
        WidgetBounds const outer = geometry.of(*inside.container);
        WidgetBounds const first = geometry.of(*inside.children.at(0));
        WidgetBounds const last = geometry.of(*inside.children.at(2));
        geometry.near(justify == Justify::End ? "end: the last child's end" : "between: the last child's end",
                      outer.y + outer.height, last.y + last.height);
        if (justify == Justify::Between) {
            geometry.near("between: the first child's start", outer.y, first.y);
        } else {
            checks.that(first.y > outer.y + 1, "end: the first child is not at the column's start");
        }
    }
    return checks.result();
}

/// @brief Case: a child capped below its column's width with `alignSelf: center` has equal space either side; a
///        stretch child with `maxWidth: 44` in a 70-unit column with `align: center` is 44 wide and centred.
/// @param probe The backend under test.
/// @return `nullopt`, or what went wrong.
[[nodiscard]] inline std::optional<std::string> cappedChildCentres(ConformanceProbe& probe) {
    Checks checks;
    Geometry geometry{probe, checks};
    for (bool const own : {true, false}) {
        LayoutHints card{.width = Sizing::stretch(), .maxWidth = own ? 20 : 44};
        if (own) {
            card.alignSelf = Align::Center;
        }
        Column column{.children = {sizedText("card", card)}, .align = own ? Align::Stretch : Align::Center};
        column.common.layout = {.width = Sizing::fixed(70)};
        Mounted const view{probe.runtime(), probe.backend(), insideColumn(ui::column(std::move(column)))};
        probe.settle();
        Measured const inside = measured(probe, view);
        checks.that(inside.children.size() == 1, "the column holds one child");
        if (inside.children.size() != 1) {
            continue;
        }
        WidgetBounds const outer = geometry.of(*inside.container);
        WidgetBounds const placed = geometry.of(*inside.children.at(0));
        double const before = placed.x - outer.x;
        double const after = (outer.x + outer.width) - (placed.x + placed.width);
        geometry.near(own ? "alignSelf center: the capped child's width" : "align center: the card's width",
                      own ? 20 : 44, placed.width);
        geometry.near(own ? "alignSelf center: the space after it, against the space before"
                          : "align center: the space after the card, against the space before",
                      before, after);
    }
    return checks.result();
}

/// @brief The cells of a table's row, in column order.
/// @param probe The backend under test.
/// @param table The table.
/// @param row   The row's position among the table's rows.
/// @return Its cells; empty when the table has no such row.
[[nodiscard]] inline std::vector<Widget*> rowCells(ConformanceProbe& probe, Widget const& table, std::size_t row) {
    std::vector<Widget*> cells;
    ContainerWidget const* const rows = asContainer(table);
    Widget const* const found = rows == nullptr ? nullptr : probe.childAt(*rows, row);
    ContainerWidget const* const inside = found == nullptr ? nullptr : asContainer(*found);
    for (std::size_t index = 0; inside != nullptr && index < probe.childCount(*inside); ++index) {
        cells.push_back(probe.childAt(*inside, index));
    }
    return cells;
}

/// @brief Case: in a 30-unit column with `align: End`, a badge cell ends where the column's text ends: at the
///        column's right edge, less the inset a backend gives every cell.
/// @param probe The backend under test.
/// @return `nullopt`, or what went wrong.
[[nodiscard]] inline std::optional<std::string> endColumnPlacesBadge(ConformanceProbe& probe) {
    reactive::Signal<std::vector<Entry>> const rows{probe.runtime(),
                                                    {{.id = 1, .label = "ok"}, {.id = 2, .label = "x"}}};
    Mounted const view{probe.runtime(), probe.backend(),
                       insideColumn(ui::table<Entry>(
                           {{.label = "State", .width = Sizing::fixed(30), .align = Align::End}},
                           [&rows] { return rows.get(); }, [](Entry const& entry) { return Key{entry.id}; },
                           [](reactive::Signal<Entry> const& entry) {
                               // Row 1 holds a badge, row 2 a text; both are drawn at the column's end.
                               Node cell = entry.peek().id == 1
                                               ? ui::badge({.text = [&entry] { return entry.get().label; }})
                                               : ui::text({.text = [&entry] { return entry.get().label; }});
                               return std::vector<Node>{std::move(cell)};
                           }))};
    probe.settle();
    Checks checks;
    Geometry geometry{probe, checks};
    Widget const* const table = probe.childAt(rootContainer(view), 0);
    std::vector<Widget*> const badgeRow = table == nullptr ? std::vector<Widget*>{} : rowCells(probe, *table, 0);
    std::vector<Widget*> const textRow = table == nullptr ? std::vector<Widget*>{} : rowCells(probe, *table, 1);
    checks.that(badgeRow.size() == 1 && textRow.size() == 1, "the table holds two rows of one cell");
    if (badgeRow.size() == 1 && textRow.size() == 1) {
        WidgetBounds const outer = geometry.of(*table);
        WidgetBounds const badge = geometry.of(*badgeRow.front());
        WidgetBounds const text = geometry.of(*textRow.front());
        geometry.near("the badge's right edge, against the column's text's", text.x + text.width,
                      badge.x + badge.width);
        checks.that(badge.x - outer.x > 15, "the badge is not at the end of its 30-unit column: it starts " +
                                                std::to_string(badge.x - outer.x) + " units into the table");
    }
    return checks.result();
}

/// @brief Case: a heading row's one cell spans both columns of a table: it starts where the first column's cell does
///        and ends where the second's does, within a unit. Every cell is a container, which fills its column.
/// @param probe The backend under test.
/// @return `nullopt`, or what went wrong.
[[nodiscard]] inline std::optional<std::string> headingSpansColumns(ConformanceProbe& probe) {
    reactive::Signal<std::vector<Entry>> const rows{probe.runtime(),
                                                    {{.id = 0, .label = "group"}, {.id = 1, .label = "x"}}};
    auto const filled = [](std::string text) { return ui::column({.children = {ui::text({.text = std::move(text)})}}); };
    Mounted const view{probe.runtime(), probe.backend(),
                       insideColumn(ui::table<Entry>(
                           {{.label = "First", .width = Sizing::fixed(10)},
                            {.label = "Second", .width = Sizing::fixed(20)}},
                           [&rows] { return rows.get(); }, [](Entry const& entry) { return Key{entry.id}; },
                           std::function<TableRowView(reactive::Signal<Entry> const&)>{
                               [filled](reactive::Signal<Entry> const& entry) {
                                   if (entry.peek().id == 0) {
                                       return TableRowView{.cells = {filled("group")}, .heading = true};
                                   }
                                   return TableRowView{.cells = {filled("a"), filled("b")}};
                               }}))};
    probe.settle();
    Checks checks;
    Geometry geometry{probe, checks};
    Widget const* const table = probe.childAt(rootContainer(view), 0);
    std::vector<Widget*> const heading = table == nullptr ? std::vector<Widget*>{} : rowCells(probe, *table, 0);
    std::vector<Widget*> const data = table == nullptr ? std::vector<Widget*>{} : rowCells(probe, *table, 1);
    checks.that(heading.size() == 1 && data.size() == 2, "the table holds a heading of one cell and a row of two");
    if (heading.size() == 1 && data.size() == 2) {
        WidgetBounds const across = geometry.of(*heading.front());
        WidgetBounds const first = geometry.of(*data.front());
        WidgetBounds const second = geometry.of(*data.back());
        geometry.near("the heading cell's left edge, against the first column's cell's", first.x, across.x);
        geometry.near("the heading cell's right edge, against the second column's cell's", second.x + second.width,
                      across.x + across.width);
    }
    return checks.result();
}

/// @brief Case: in a row where one cell is five lines tall, a one-line cell of a `valign: Start` column has its
///        top at the row's top, and one of a `valign: End` column its bottom at the row's bottom. Five lines, so a
///        centred cell is two lines from either, more than the unit a measure may be off by.
/// @param probe The backend under test.
/// @return `nullopt`, or what went wrong.
[[nodiscard]] inline std::optional<std::string> columnPlacesCellsDownRow(ConformanceProbe& probe) {
    reactive::Signal<std::vector<Entry>> const rows{probe.runtime(), {{.id = 1, .label = "x"}}};
    Mounted const view{probe.runtime(), probe.backend(),
                       insideColumn(ui::table<Entry>(
                           {{.label = "Tall", .width = Sizing::fixed(10)},
                            {.label = "Top", .width = Sizing::fixed(10), .valign = Align::Start},
                            {.label = "Bottom", .width = Sizing::fixed(10), .valign = Align::End}},
                           [&rows] { return rows.get(); }, [](Entry const& entry) { return Key{entry.id}; },
                           [](reactive::Signal<Entry> const& entry) {
                               return std::vector<Node>{ui::text({.text = "one\ntwo\nthree\nfour\nfive"}),
                                                        ui::text({.text = [&entry] { return entry.get().label; }}),
                                                        ui::text({.text = [&entry] { return entry.get().label; }})};
                           }))};
    probe.settle();
    Checks checks;
    Geometry geometry{probe, checks};
    Widget const* const table = probe.childAt(rootContainer(view), 0);
    std::vector<Widget*> const cells = table == nullptr ? std::vector<Widget*>{} : rowCells(probe, *table, 0);
    checks.that(cells.size() == 3, "the row holds three cells");
    if (cells.size() == 3) {
        WidgetBounds const tall = geometry.of(*cells.at(0));
        WidgetBounds const top = geometry.of(*cells.at(1));
        WidgetBounds const bottom = geometry.of(*cells.at(2));
        checks.that(tall.height >= 4.5, "the five-line cell is " + std::to_string(tall.height) + " units tall");
        geometry.near("a valign Start cell's top, against the row's", tall.y, top.y);
        geometry.near("a valign End cell's bottom, against the row's", tall.y + tall.height, bottom.y + bottom.height);
    }
    return checks.result();
}

/// @brief One report of a table's selection: the keys, sorted, and the kind of change.
struct SelectionReport {
    /// @brief The keys reported, sorted.
    std::vector<Key> keys;
    /// @brief The kind of change.
    SelectionChange change = SelectionChange::Replace;
};

/// @brief A table over @p rows whose user's selection is recorded in @p reports and shown without a binding.
/// @param rows    The rows.
/// @param mode    The selection mode.
/// @param reports Where each report goes.
/// @param common  The table's common properties.
/// @return The node.
[[nodiscard]] inline Node reportingTable(reactive::Signal<std::vector<Entry>> const& rows, SelectionMode mode,
                                         std::vector<SelectionReport>& reports, Common common = {}) {
    return entryTable(rows, {.selectionMode = mode,
                             .onSelectionChange =
                                 [&reports](std::vector<Key> keys, SelectionChange change) {
                                     std::ranges::sort(keys);
                                     reports.push_back({.keys = std::move(keys), .change = change});
                                 },
                             .common = std::move(common)});
}

/// @brief The last report as text: its keys, then `/replace` or `/extend`; `none` when there is none.
/// @param reports The reports.
/// @return The text.
[[nodiscard]] inline std::string lastReport(std::vector<SelectionReport> const& reports) {
    if (reports.empty()) {
        return "none";
    }
    return keyTexts(reports.back().keys) +
           (reports.back().change == SelectionChange::Replace ? "/replace" : "/extend");
}

/// @brief Positions as text, comma-separated.
/// @param rows The positions.
/// @return The text.
[[nodiscard]] inline std::string rowTexts(std::vector<std::size_t> const& rows) {
    std::string text;
    for (std::size_t const row : rows) {
        text += (text.empty() ? "" : ",") + std::to_string(row);
    }
    return text;
}

/// @brief Rows a, b, c and d, keyed 1 to 4.
/// @return The rows.
[[nodiscard]] inline std::vector<Entry> fourEntries() {
    return {{.id = 1, .label = "a"}, {.id = 2, .label = "b"}, {.id = 3, .label = "c"}, {.id = 4, .label = "d"}};
}

/// @brief Case: in a `Multiple` table a plain click selects only its row, and a Ctrl-click adds a row and then
///        removes it, keeping the first. The click reports `Replace`, each Ctrl-click `Extend`.
/// @param probe The backend under test.
/// @return `nullopt`, or what went wrong.
[[nodiscard]] inline std::optional<std::string> clickSelectsCtrlClickToggles(ConformanceProbe& probe) {
    reactive::Signal<std::vector<Entry>> const rows{probe.runtime(), fourEntries()};
    std::vector<SelectionReport> reports;
    Mounted const view{probe.runtime(), probe.backend(), reportingTable(rows, SelectionMode::Multiple, reports)};
    probe.settle();
    Checks checks;
    probe.clickRow(view.root(), 2, RowClick::Toggle);
    probe.settle();
    probe.clickRow(view.root(), 0, RowClick::Only);
    probe.settle();
    checks.text("the rows selected after a Ctrl-click on c and a click on a", "0",
                rowTexts(probe.selectedRows(view.root())));
    checks.text("a click's report", "1/replace", lastReport(reports));
    probe.clickRow(view.root(), 1, RowClick::Toggle);
    probe.settle();
    checks.text("the rows selected after a Ctrl-click on b", "0,1", rowTexts(probe.selectedRows(view.root())));
    checks.text("a Ctrl-click's report", "1,2/extend", lastReport(reports));
    probe.clickRow(view.root(), 1, RowClick::Toggle);
    probe.settle();
    checks.text("the rows selected after a second Ctrl-click on b", "0", rowTexts(probe.selectedRows(view.root())));
    checks.text("the second Ctrl-click's report", "1/extend", lastReport(reports));
    return checks.result();
}

/// @brief Case: a click on row 1, then a Shift-click on row 3, selects rows 1 to 3; a Shift-click on row 0 then
///        selects rows 0 and 1, the range from the same anchor in place of the rest. Each Shift-click reports
///        `Replace`.
/// @param probe The backend under test.
/// @return `nullopt`, or what went wrong.
[[nodiscard]] inline std::optional<std::string> shiftClickSelectsRange(ConformanceProbe& probe) {
    reactive::Signal<std::vector<Entry>> const rows{probe.runtime(), fourEntries()};
    std::vector<SelectionReport> reports;
    Mounted const view{probe.runtime(), probe.backend(), reportingTable(rows, SelectionMode::Multiple, reports)};
    probe.settle();
    Checks checks;
    probe.clickRow(view.root(), 1, RowClick::Only);
    probe.settle();
    probe.clickRow(view.root(), 3, RowClick::Range);
    probe.settle();
    checks.text("the rows selected by a click on b and a Shift-click on d", "1,2,3",
                rowTexts(probe.selectedRows(view.root())));
    checks.text("the Shift-click's report", "2,3,4/replace", lastReport(reports));
    probe.clickRow(view.root(), 0, RowClick::Range);
    probe.settle();
    checks.text("the rows selected by a further Shift-click on a", "0,1", rowTexts(probe.selectedRows(view.root())));
    checks.text("the second Shift-click's report", "1,2/replace", lastReport(reports));
    return checks.result();
}

/// @brief Case: the keyboard on a focused table of text cells. Ctrl+A on a `Multiple` table selects every row, and
///        pressed again clears them, each reported as `Extend`. On a `Single` table Ctrl+A selects nothing and
///        reaches the enclosing column's chord, and after a click on row 1, Enter activates row 1.
/// @param probe The backend under test.
/// @return `nullopt`, or what went wrong.
[[nodiscard]] inline std::optional<std::string> tableTakesItsKeys(ConformanceProbe& probe) {
    Checks checks;
    {
        reactive::Signal<std::vector<Entry>> const rows{probe.runtime(), fourEntries()};
        std::vector<SelectionReport> reports;
        Mounted const view{probe.runtime(), probe.backend(), reportingTable(rows, SelectionMode::Multiple, reports)};
        probe.settle();
        probe.press(view.root(), "Ctrl+A");
        probe.settle();
        checks.text("the rows selected by Ctrl+A on a multiple table", "0,1,2,3",
                    rowTexts(probe.selectedRows(view.root())));
        checks.text("Ctrl+A's report", "1,2,3,4/extend", lastReport(reports));
        probe.press(view.root(), "Ctrl+A");
        probe.settle();
        checks.text("the rows selected by a second Ctrl+A", "", rowTexts(probe.selectedRows(view.root())));
        checks.text("the second Ctrl+A's report", "/extend", lastReport(reports));
    }
    reactive::Signal<std::vector<Entry>> const rows{probe.runtime(), fourEntries()};
    std::vector<SelectionReport> reports;
    std::vector<Key> activated;
    int chords = 0;
    Mounted const view{
        probe.runtime(), probe.backend(),
        ui::column({.children = {entryTable(
                        rows, {.selectionMode = SelectionMode::Single,
                               .onSelectionChange =
                                   [&reports](std::vector<Key> keys, SelectionChange change) {
                                       reports.push_back({.keys = std::move(keys), .change = change});
                                   },
                               .onActivate = [&activated](Key const& key) { activated.push_back(key); }})},
                    .common = {.keys = {{.chord = "Ctrl+A", .onPress = [&chords] { ++chords; }}}}})};
    probe.settle();
    Widget* const table = childOf(probe, &view.root(), 0);
    if (table == nullptr) {
        return "a Table in a Column is not shown";
    }
    probe.press(*table, "Ctrl+A");
    probe.settle();
    checks.text("the rows selected by Ctrl+A on a single table", "", rowTexts(probe.selectedRows(*table)));
    checks.that(chords == 1, "Ctrl+A on a single table ran the column's chord " + std::to_string(chords) + " times");
    probe.clickRow(*table, 1, RowClick::Only);
    probe.settle();
    probe.press(*table, "Enter");
    probe.settle();
    checks.text("the rows activated by Enter after a click on b", "2", keyTexts(activated));
    return checks.result();
}

/// @brief Case: a table of a heading row and three data rows. Ctrl+A selects the three data rows and not the
///        heading, and a click on the heading row changes nothing and reports nothing.
/// @param probe The backend under test.
/// @return `nullopt`, or what went wrong.
[[nodiscard]] inline std::optional<std::string> headingIsNeverSelected(ConformanceProbe& probe) {
    reactive::Signal<std::vector<Entry>> const rows{probe.runtime(), {{.id = 0, .label = "group"}, {.id = 1, .label = "a"},
                                                                      {.id = 2, .label = "b"}, {.id = 3, .label = "c"}}};
    std::vector<SelectionReport> reports;
    Mounted const view{
        probe.runtime(), probe.backend(),
        ui::table<Entry>(
            {{.label = "Name"}}, [&rows] { return rows.get(); }, [](Entry const& entry) { return Key{entry.id}; },
            std::function<TableRowView(reactive::Signal<Entry> const&)>{[](reactive::Signal<Entry> const& entry) {
                return TableRowView{.cells = {ui::text({.text = [&entry] { return entry.get().label; }})},
                                    .heading = entry.peek().id == 0};
            }},
            TableOptions{.selectionMode = SelectionMode::Multiple,
                         .onSelectionChange =
                             [&reports](std::vector<Key> keys, SelectionChange change) {
                                 std::ranges::sort(keys);
                                 reports.push_back({.keys = std::move(keys), .change = change});
                             }})};
    probe.settle();
    Checks checks;
    probe.press(view.root(), "Ctrl+A");
    probe.settle();
    checks.text("the rows Ctrl+A selects beside a heading", "1,2,3", rowTexts(probe.selectedRows(view.root())));
    checks.text("Ctrl+A's report", "1,2,3/extend", lastReport(reports));
    std::size_t const before = reports.size();
    probe.clickRow(view.root(), 0, RowClick::Only);
    probe.settle();
    checks.text("the rows selected after a click on the heading", "1,2,3", rowTexts(probe.selectedRows(view.root())));
    checks.that(reports.size() == before, "a click on the heading row reported a selection");
    return checks.result();
}

/// @brief Case: a click on a sortable column's header reports its index once, a click on another column's header
///        reports nothing, and showing a sort reports nothing.
/// @param probe The backend under test.
/// @return `nullopt`, or what went wrong.
[[nodiscard]] inline std::optional<std::string> headerClickAsksToSort(ConformanceProbe& probe) {
    reactive::Signal<std::vector<Entry>> const rows{probe.runtime(), fourEntries()};
    reactive::Signal<std::optional<TableSort>> shown{probe.runtime(), std::nullopt};
    std::vector<std::size_t> asked;
    Mounted const view{
        probe.runtime(), probe.backend(),
        ui::table<Entry>(
            {{.label = "Name", .sortable = true}, {.label = "Id"}}, [&rows] { return rows.get(); },
            [](Entry const& entry) { return Key{entry.id}; },
            [](reactive::Signal<Entry> const& entry) {
                return std::vector<Node>{ui::text({.text = [&entry] { return entry.get().label; }}),
                                         ui::text({.text = [&entry] { return std::to_string(entry.get().id); }})};
            },
            TableOptions{.sort = [&shown] { return shown.get(); },
                         .onSort = [&asked](std::size_t column) { asked.push_back(column); }})};
    probe.settle();
    Checks checks;
    shown.set(TableSort{.column = 0, .direction = SortDirection::Descending});
    probe.settle();
    checks.that(asked.empty(), "showing a sort asked to sort");
    probe.clickHeader(view.root(), 0);
    probe.settle();
    checks.that(asked == std::vector<std::size_t>{0},
                "a click on the sortable header asked " + std::to_string(asked.size()) + " times");
    probe.clickHeader(view.root(), 1);
    probe.settle();
    checks.that(asked.size() == 1, "a click on a header that is not sortable asked to sort");
    return checks.result();
}

}  // namespace detail

/// @brief The layout cases: where a backend that lays widgets out draws them. Run each on a fresh probe whose
///        `boundsOf` reads geometry; a probe that reads none fails every one.
/// @return The cases.
[[nodiscard]] inline std::span<ConformanceCase const> layoutConformanceCases() {
    static std::array<ConformanceCase, 10> const cases{{
        {.name = "a stretch child's maximum caps its share along a row, and its sibling takes the rest",
         .run = detail::maximumCapsStretchShare},
        {.name = "a content child is at least its minimum wide along a row", .run = detail::minimumWidensContent},
        {.name = "a stretch child's maximum caps it across a column", .run = detail::maximumCapsAcross},
        {.name = "a stretch child's maximum height caps its share down a column",
         .run = detail::maximumCapsStretchHeight},
        {.name = "a row centres a child across it", .run = detail::rowCentresAcross},
        {.name = "a column's justify places its spare height", .run = detail::columnJustifies},
        {.name = "a capped child is centred by its own alignment or its column's", .run = detail::cappedChildCentres},
        {.name = "a table column's end alignment places a badge cell at its end", .run = detail::endColumnPlacesBadge},
        {.name = "a table column's valign places a cell at its row's top or bottom",
         .run = detail::columnPlacesCellsDownRow},
        {.name = "a table's heading row spans every column", .run = detail::headingSpansColumns},
    }};
    return cases;
}

/// @brief Every conformance case, in a fixed order.
///
/// Run each on a fresh probe; `nullopt` is a pass, anything else is the failure message. A backend's test suite
/// loops over these with its own `ConformanceProbe`.
/// @return The cases.
[[nodiscard]] inline std::span<ConformanceCase const> conformanceCases() {
    static std::array<ConformanceCase, 46> const cases{{
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
        {.name = "a moved widget keeps its native state", .run = detail::movedRowKeepsNativeState},
        {.name = "a Dialog holds its content only while open", .run = detail::dialogHoldsContentWhileOpen},
        {.name = "a new Dialog is closed and a closed Dialog takes no input", .run = detail::closedDialogTakesNoInput},
        {.name = "a widget inside a hidden, disabled or collapsed container takes no input",
         .run = detail::unreachableWidgetsTakeNoInput},
        {.name = "a setter given the text a field shows keeps the cursor", .run = detail::unchangedTextKeepsCursor},
        {.name = "a text field shows what the document makes of an edit", .run = detail::textInputIsControlled},
        {.name = "a dismissal the document refuses leaves the dialog open",
         .run = detail::refusedDismissalKeepsDialogOpen},
        {.name = "no setter calls a handler", .run = detail::settersCallNoHandler},
        {.name = "a Select marks the requested key whenever its options contain it",
         .run = detail::selectReResolvesRequestedKey},
        {.name = "a Table reports a user's selection as exactly the existing rows selected",
         .run = detail::tableReportsExistingSelectedRows},
        {.name = "a Table shows a user's selection with no selection bound",
         .run = detail::tableShowsUnboundUserSelection},
        {.name = "a hidden Table cell keeps its column", .run = detail::hiddenCellKeepsItsColumn},
        {.name = "no handler runs after its widget was destroyed", .run = detail::noHandlerAfterDestruction},
        {.name = "a handler may destroy its own widget", .run = detail::handlerMayDestroyItsWidget},
        {.name = "a drag onto an accepting target delivers the key", .run = detail::dragOntoAcceptingTarget},
        {.name = "a drag onto a refusing target delivers nothing", .run = detail::dragOntoRefusingTarget},
        {.name = "a drop target without accepts takes every key", .run = detail::dropTargetWithoutAcceptsTakesAll},
        {.name = "a chord goes to the innermost widget that declares it", .run = detail::innermostChordWins},
        {.name = "a chord pressed inside a hidden container runs nothing", .run = detail::hiddenChordRunsNothing},
        {.name = "a chord the focused widget declares comes before its own key", .run = detail::ownChordBeforeOwnKey},
        {.name = "a focused button takes Enter before its ancestors' chords",
         .run = detail::ownKeyBeforeAncestorChord},
        {.name = "a submitting field in a dialog takes Enter from the dialog",
         .run = detail::submittingFieldTakesEnter},
        {.name = "a field without a submit commits on Enter and lets the dialog have it",
         .run = detail::fieldWithoutSubmitPassesEnterOn},
        {.name = "a check box toggles on Space and lets the dialog have Enter",
         .run = detail::checkboxTakesSpaceNotEnter},
        {.name = "a field's own Enter chord comes before its commit and submit",
         .run = detail::fieldChordBeforeSubmit},
        {.name = "the first autofocus widget in document order has focus", .run = detail::firstAutofocusWins},
        {.name = "a menu reports only an enabled entry without a submenu",
         .run = detail::onlyEnabledMenuLeavesAreChosen},
        {.name = "a read-only input takes no edit", .run = detail::readOnlyInputTakesNoEdit},
        {.name = "a dismissed banner reports it and stays shown", .run = detail::dismissedBannerStays},
        {.name = "a closed collapsible's content takes no input, its header does",
         .run = detail::closedCollapsibleKeepsItsHeader},
        {.name = "a drop zone takes a drop whole or refuses it whole", .run = detail::dropZoneTakesWholeDrops},
        {.name = "a click selects only its row, and a Ctrl-click toggles one and keeps the rest",
         .run = detail::clickSelectsCtrlClickToggles},
        {.name = "a Shift-click selects the rows from the anchor, in place of the rest",
         .run = detail::shiftClickSelectsRange},
        {.name = "a focused table takes Ctrl+A in multiple mode, passes it on in single mode, and Enter activates",
         .run = detail::tableTakesItsKeys},
        {.name = "a click on a sortable header asks to sort by it, and no other header or setter does",
         .run = detail::headerClickAsksToSort},
        {.name = "a heading row is never selected, by Ctrl+A or a click", .run = detail::headingIsNeverSelected},
        {.name = "relabelled tabs keep the highlighted tab and its page", .run = detail::relabelledTabsKeepHighlight},
    }};
    return cases;
}

}  // namespace morph::ui::testing

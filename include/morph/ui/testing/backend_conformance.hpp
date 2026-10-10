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

    /// @brief Dismisses a dialog as a user would (Esc on the TUI).
    /// @param dialog The dialog.
    virtual void dismiss(Widget& dialog) = 0;

    /// @brief Selects rows of a table as a user would, so that the user's selection is exactly these rows.
    /// @param table The table.
    /// @param rows Positions among the table's current rows, in the order the user picks them: at least one, and
    ///        one only for a `Single` table.
    virtual void selectRows(Widget& table, std::vector<std::size_t> const& rows) = 0;

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
/// @return The node.
[[nodiscard]] inline Node entryTable(reactive::Signal<std::vector<Entry>> const& rows, TableOptions options) {
    return ui::table<Entry>(
        {{.label = "Name"}}, [&rows] { return rows.get(); }, [](Entry const& entry) { return Key{entry.id}; },
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
        {.children = {
             ui::button({.label = "Go", .onClick = [&clicks] { ++clicks; }}),
             ui::textInput({.onChange = [&edits](std::string const&) { ++edits; }}),
             ui::text({.text = "card", .common = {.dragKey = std::optional<Key>{Key{std::int64_t{7}}}}}),
             entryTable(rows, {.selectionMode = SelectionMode::Single,
                               .onSelectionChange = [&selections](std::vector<Key> const&) { ++selections; }})}});
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
                 ui::slider(
                     {.value = [&phase] { return std::int64_t{phase.get()}; }, .onChange = note("Slider onChange")}),
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
                                   .onActivate = note("Table onActivate")}),
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
                                             [&reports, &selection](std::vector<Key> keys) {
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
    Mounted const view{probe.runtime(), probe.backend(),
                       entryTable(rows, {.selectionMode = SelectionMode::Single,
                                         .onSelectionChange = [&reports](std::vector<Key> const&) { ++reports; }})};
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

}  // namespace detail

/// @brief Every conformance case, in a fixed order.
///
/// Run each on a fresh probe; `nullopt` is a pass, anything else is the failure message. A backend's test suite
/// loops over these with its own `ConformanceProbe`.
/// @return The cases.
[[nodiscard]] inline std::span<ConformanceCase const> conformanceCases() {
    static std::array<ConformanceCase, 31> const cases{{
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
        {.name = "the first autofocus widget in document order has focus", .run = detail::firstAutofocusWins},
        {.name = "a menu reports only an enabled entry without a submenu",
         .run = detail::onlyEnabledMenuLeavesAreChosen},
        {.name = "a read-only input takes no edit", .run = detail::readOnlyInputTakesNoEdit},
    }};
    return cases;
}

}  // namespace morph::ui::testing

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
/// Its destructor detaches it from its parent and frees its native resources; no handler of the widget runs after
/// the destructor returns. A handler may destroy its own widget: the backend keeps the handler alive until that call
/// returns, for example by calling a copy. A backend may assume a handler, a drop's `accepts` predicate included,
/// never throws: the mount catches and reports what one throws. Every setter takes UTF-8 and may be called with an
/// unchanged value.
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
    ///
    /// The child is moved, not recreated: it keeps its native state, such as its properties, handlers, focus and
    /// selection.
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
///
/// The widget keeps the key last requested by `setSelected` or chosen by the user, and marks the option with that key
/// whenever the current options contain one. A key outside the options marks none, and stays requested: options that
/// bring it back mark it again. Options and selection may therefore arrive in either order.
class SelectWidget : public Widget {
public:
    /// @brief Replaces the options, and marks the requested key's option if they contain it.
    /// @param options The options, in order.
    virtual void setOptions(std::vector<SelectOption> const& options) = 0;

    /// @brief Requests the option with @p key. It is marked while the options contain it; otherwise none is marked.
    ///        Never calls `onSelect`.
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

    /// @brief Collapses or expands the panel. A collapsed panel hides its content, which takes no input; its own
    ///        collapse control still works. Never calls the `onToggle` handler.
    /// @param collapsed Whether the content is hidden.
    virtual void setCollapsed(bool collapsed) = 0;

    /// @brief Sets what a user collapse or expand calls, with the requested state.
    /// @param onToggle The handler; empty does nothing.
    virtual void setOnToggle(std::function<void(bool)> onToggle) = 0;
};

/// @brief A scrollable viewport, on the axis fixed at creation, that keeps the focused child visible.
class ScrollWidget : public ContainerWidget {};

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

/// @brief A modal overlay around its children, with a focus trap while open. A new dialog is closed.
///
/// A closed dialog shows nothing and takes no input, its children included.
class DialogWidget : public ContainerWidget {
public:
    /// @brief Shows or hides the overlay.
    /// @param open Whether it is shown.
    virtual void setOpen(bool open) = 0;

    /// @brief Replaces the title.
    /// @param title The title, UTF-8.
    virtual void setTitle(std::string_view title) = 0;

    /// @brief Sets what a user dismissal (Esc on the TUI) of the open dialog calls.
    /// @param onDismiss The handler; empty does nothing.
    virtual void setOnDismiss(Action onDismiss) = 0;
};

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

/// @brief Keyed rows under column headers, with selection and activation by key.
///
/// Its children are the rows, each a horizontal stack whose children are the cells in column order. A hidden cell
/// keeps its column: the cells after it stay under their own headers.
///
/// Selection is kept by key. The widget keeps the keys last requested by `setSelection` or selected by the user, and
/// marks the current rows that have one of them: a key with no row marks nothing and stays requested, so a row that
/// arrives with it later is marked, a row that goes is unmarked, and a reorder keeps the marks. Rows and selection
/// may therefore arrive in either order. The selection mode limits what the user can select, not what `setSelection`
/// marks.
///
/// A user's selection replaces the requested keys, and the widget shows it whether or not the application binds the
/// selection. `onSelectionChange` then gets exactly the keys of the existing rows now selected, in any mode: a key
/// still waiting for its row is dropped by a user's change, while `setSelection` keeps it waiting. It reports the
/// user's selections only: no setter, and no row arriving or going, calls it.
class TableWidget : public ContainerWidget {
public:
    /// @brief Sets the column headers and widths.
    /// @param columns The columns, in order.
    virtual void setColumns(std::vector<TableColumn> const& columns) = 0;

    /// @brief Sets how many rows the user can select.
    /// @param mode None, one, or any number.
    virtual void setSelectionMode(SelectionMode mode) = 0;

    /// @brief Tells the table which key a row stands for; called once per row, right after it is built. Marks the row
    ///        when its key is requested.
    /// @param row One of this table's rows.
    /// @param key The row's key.
    virtual void setRowKey(Widget& row, Key const& key) = 0;

    /// @brief Requests the rows with these keys, and marks those it has. Never calls the `onSelectionChange` handler.
    /// @param keys The selected keys.
    virtual void setSelection(std::vector<Key> const& keys) = 0;

    /// @brief Sets what a user's selection change calls, with the keys of every row then selected.
    /// @param onSelectionChange The handler; empty does nothing.
    virtual void setOnSelectionChange(std::function<void(std::vector<Key>)> onSelectionChange) = 0;

    /// @brief Sets what activating a row (Enter or a double click) calls, with its key.
    /// @param onActivate The handler; empty does nothing.
    virtual void setOnActivate(std::function<void(Key)> onActivate) = 0;
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

    /// @brief Makes a progress indicator.
    /// @param parent The container to append to, or null for a root.
    /// @return The widget.
    [[nodiscard]] virtual std::unique_ptr<BusyWidget> createBusy(ContainerWidget* parent) = 0;

    /// @brief Makes a table.
    /// @param parent The container to append to, or null for a root.
    /// @return The widget.
    [[nodiscard]] virtual std::unique_ptr<TableWidget> createTable(ContainerWidget* parent) = 0;

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

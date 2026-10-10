// SPDX-License-Identifier: Apache-2.0

#pragma once
#include <algorithm>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include "../reactive/runtime.hpp"
#include "../reactive/signal.hpp"
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

/// @brief The stable identity of a slot: a bound property a renderer can refer to by number.
///
/// Whoever builds the tree assigns slot ids; the interpreter numbers every bound property of a document, and a
/// code-generating renderer names the value it reads after it (`v.s17`). `Mounted` does not use them. A slot made
/// from a bare callable has no id.
using SlotId = std::uint32_t;

/// @brief A slot's read together with its id, as `ui::slot` makes it; a `Prop` made from it is a slot with that id.
/// @tparam F A callable taking no arguments.
template <typename F>
struct SlotBinding {
    /// @brief The slot's id.
    SlotId id = 0;
    /// @brief Reads the slot's value; every signal it reads is a dependency.
    F read;
};

/// @brief A slot with an id.
/// @tparam F A callable taking no arguments.
/// @param slotId The slot's id.
/// @param read Reads the value; every signal it reads is a dependency.
/// @return What a `Prop` takes as a slot with @p slotId.
template <typename F>
[[nodiscard]] SlotBinding<std::decay_t<F>> slot(SlotId slotId, F&& read) {
    return SlotBinding<std::decay_t<F>>{.id = slotId, .read = std::forward<F>(read)};
}

namespace detail {

/// @brief Whether a type is a `SlotBinding`: false here, true for the specialisation below.
/// @tparam T The type.
template <typename T>
struct IsSlotBinding : std::false_type {};

/// @brief A `SlotBinding` is one.
/// @tparam F The read's type.
template <typename F>
struct IsSlotBinding<SlotBinding<F>> : std::true_type {};

/// @brief Whether `From` may become a constant `Prop<To>`: an implicit conversion that does not silently change
/// the kind of value.
///
/// Two implicit conversions are refused because they compile and then show something else. A pointer, a string
/// literal included, converts to `bool` as "not null", so `Prop<bool>{"false"}` would be `true`. A floating-point
/// value converts to an integral type by truncation, so `Prop<std::int64_t>{2.9}` would be `2`. Other narrowing,
/// such as `int` to `std::size_t`, is kept: `Prop<std::size_t> count = 0` is the ordinary way to write a count.
///
/// `From` is the source type as deduced for a forwarding reference; `To` is the property's value type.
template <typename From, typename To>
concept ConstantConvertible = std::convertible_to<From, To> &&
                              !(std::is_same_v<std::remove_cv_t<To>, bool> && std::is_pointer_v<std::decay_t<From>>) &&
                              !(std::is_integral_v<To> && std::is_floating_point_v<std::remove_cvref_t<From>>) &&
                              !IsSlotBinding<std::remove_cvref_t<From>>::value;

}  // namespace detail

/// @brief A node property: a constant, or a slot — a reactive read, with a stable id when it was given one.
///
/// A constant is set on the widget once and creates no reactive node. A slot becomes an equality-gated
/// `reactive::Computed` plus an `reactive::Effect` calling one widget setter. An input whose value is a slot is
/// controlled: after a user's edit, the widget shows what the slot says. A constant is not re-asserted.
///
/// A braced list cannot initialise a `Prop`, because the constant constructor deduces its argument's type: name
/// the type, as in `.options = std::vector<ui::SelectOption>{{...}, {...}}`.
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
    ///
    /// A pointer does not become a `bool` constant and a floating-point value does not become an integral one; see
    /// `detail::ConstantConvertible`.
    /// @tparam U A non-callable type that converts to `T` without changing the kind of value.
    /// @param value The constant.
    template <typename U>
        requires(!std::invocable<U&>) && detail::ConstantConvertible<U, T>
    Prop(U&& value)
        // A string literal converting to `T` decays here; that is the conversion.
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-array-to-pointer-decay)
        : _value{std::in_place_index<0>, static_cast<T>(std::forward<U>(value))} {}

    /// @brief A slot without an id.
    ///
    /// The read must be callable: an empty `std::function` or a null function pointer is refused here, where the
    /// mistake is made, rather than when a mount first calls it.
    /// @tparam F A callable taking no arguments and returning something convertible to `T`.
    /// @param binding Evaluated by the mount inside a `Computed`; every signal it reads is a dependency.
    /// @throws std::invalid_argument when `binding` is an empty `std::function` or a null function pointer.
    template <typename F>
        requires std::invocable<F&> && std::convertible_to<std::invoke_result_t<F&>, T>
    Prop(F binding) : _value{std::in_place_index<1>, Slot{.id = std::nullopt, .read = nonEmpty(std::move(binding))}} {}

    /// @brief A slot with an id, as `ui::slot` makes it.
    /// @tparam F A callable taking no arguments and returning something convertible to `T`.
    /// @param binding The id and the read.
    /// @throws std::invalid_argument when the read is an empty `std::function` or a null function pointer.
    template <typename F>
        requires std::invocable<F&> && std::convertible_to<std::invoke_result_t<F&>, T>
    Prop(SlotBinding<F> binding)
        : _value{std::in_place_index<1>, Slot{.id = binding.id, .read = nonEmpty(std::move(binding.read))}} {}

    /// @brief Whether this is a slot.
    /// @return True for a slot, false for a constant.
    [[nodiscard]] bool isBound() const noexcept { return _value.index() == 1; }

    /// @brief The constant. Only valid when `!isBound()`.
    /// @return The constant.
    /// @throws std::bad_variant_access when this is a slot.
    [[nodiscard]] T const& constant() const { return std::get<0>(_value); }

    /// @brief The slot's read. Only valid when `isBound()`.
    /// @return The read.
    /// @throws std::bad_variant_access when this is a constant.
    [[nodiscard]] std::function<T()> const& binding() const { return std::get<1>(_value).read; }

    /// @brief The slot's id.
    /// @return The id given through `ui::slot`; `nullopt` for a constant and for a slot made from a bare callable.
    [[nodiscard]] std::optional<SlotId> slotId() const noexcept {
        return isBound() ? std::get<1>(_value).id : std::nullopt;
    }

    /// @brief The current value: the constant, or the slot read once.
    /// @return The value.
    [[nodiscard]] T evaluate() const { return isBound() ? std::get<1>(_value).read() : std::get<0>(_value); }

private:
    /// @brief A slot: its id, if it has one, and its read.
    struct Slot {
        /// @brief The id, or none.
        std::optional<SlotId> id;
        /// @brief The read; never empty.
        std::function<T()> read;
    };

    /// @brief Wraps a read, refusing an empty one.
    /// @tparam F The read's type.
    /// @param binding The read.
    /// @return The wrapped read.
    /// @throws std::invalid_argument when @p binding is empty.
    template <typename F>
    [[nodiscard]] static std::function<T()> nonEmpty(F binding) {
        std::function<T()> read{std::move(binding)};
        if (!read) {
            throw std::invalid_argument{"ui::Prop: a binding must not be empty"};
        }
        return read;
    }

    std::variant<T, Slot> _value;
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

/// @brief How many rows of a `Table` the user can select.
enum class SelectionMode : std::uint8_t {
    None,      ///< Rows can be activated, not selected.
    Single,    ///< At most one row.
    Multiple,  ///< Any number of rows.
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
    /// @brief Called with the index the user picked, and with the page still shown when a picked page fails to mount.
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

/// @brief A progress indicator.
struct Busy {
    /// @brief Whether it animates.
    Prop<bool> active;
    /// @brief A caption beside it.
    Prop<std::string> label;
    /// @brief Visibility, enablement, layout, drag-and-drop.
    Common common{};
};

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

    /// @brief Sets the row's signal to an entry of its session's latest snapshot; an equal row notifies nobody.
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

    /// @brief Reads the rows (tracked, inside the mount's Effect), keeps them as the snapshot, and returns their
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
/// @tparam RowT The row type; copyable. With an `operator==`, an unchanged row notifies nobody; without one, every
///         new snapshot notifies every kept row.
template <typename RowT>
class TypedForEach final : public ForEachModel {
public:
    /// @param rows Reads the rows; every signal it reads is a dependency of the ForEach.
    /// @param keyOf A row's identity.
    /// @param rowView Builds a row's view once, from a signal the mount keeps equal to the row.
    /// @throws std::invalid_argument when any of the three is empty, as a `Prop` refuses an empty binding.
    TypedForEach(std::function<std::vector<RowT>()> rows, std::function<Key(RowT const&)> keyOf,
                 std::function<Node(reactive::Signal<RowT> const&)> rowView)
        : _rows{nonEmpty(std::move(rows))},
          _keyOf{nonEmpty(std::move(keyOf))},
          _rowView{nonEmpty(std::move(rowView))} {}

    [[nodiscard]] std::unique_ptr<ForEachSession> open(reactive::Runtime& runtime) const override {
        return std::make_unique<Session>(runtime, *this);
    }

private:
    template <typename F>
    [[nodiscard]] static F nonEmpty(F function) {
        if (!function) {
            throw std::invalid_argument{"ui::forEach: the rows, the key function and the row view must not be empty"};
        }
        return function;
    }

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
    std::variant<Text, Button, TextInput, Checkbox, Select, Menu, Column, Row, Grid, Spacer, Panel, Scroll, Switch,
                 Tabs, Dialog, Busy, ForEach, Table, DateTimeInput, Slider, FilePicker>
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

/// @brief A Switch over an enumeration: each enumerator becomes the `int64` key of its case.
///
/// Enumerators are compared as `int64`: an unsigned value above `INT64_MAX` wraps to a negative key, and distinct
/// enumerators still give distinct keys.
/// @tparam E The enumeration.
/// @param selector Which enumerator to show; every signal it reads is a dependency.
/// @param cases One node per enumerator shown.
/// @param fallback Shown when no case matches; may be null.
/// @return The node.
/// @throws std::invalid_argument when @p selector is empty, as a `Prop` refuses an empty binding.
template <typename E>
    requires std::is_enum_v<E>
[[nodiscard]] Node switchOn(std::function<E()> selector, std::vector<std::pair<E, Node>> cases, Node fallback = {}) {
    if (!selector) {
        throw std::invalid_argument{"ui::switchOn: the selector must not be empty"};
    }
    std::vector<SwitchCase> keyed;
    keyed.reserve(cases.size());
    for (auto& [value, node] : cases) {
        keyed.push_back(SwitchCase{.key = Key{static_cast<std::int64_t>(value)}, .node = std::move(node)});
    }
    Switch spec{
        .selector = [current = std::move(selector)] { return Key{static_cast<std::int64_t>(current())}; },
        .cases = std::move(keyed),
    };
    spec.fallback = std::move(fallback);
    return switchOf(std::move(spec));
}

/// @brief One widget per row of a keyed collection read through a binding (a `Query`'s `value()` included).
///
/// A kept key keeps its widget, which a changed row updates in place; a new key is mounted, a gone key unmounted, and
/// the rows are brought into the snapshot's order by moving as few widgets as possible.
/// @tparam RowT The row type; copyable.
/// @param rows Reads the rows; every signal it reads is a dependency.
/// @param keyOf A row's identity; a later duplicate of a key is reported and refused, and the first one is kept.
/// @param rowView Builds a row's view once, from a signal the mount keeps equal to the row. Read the signal inside
///        bindings: what the view reads directly while it is built is read once and never again.
/// @param axis The stacking direction.
/// @param gap Space between rows.
/// @return The node.
/// @throws std::invalid_argument when @p rows, @p keyOf or @p rowView is empty.
template <typename RowT>
[[nodiscard]] Node forEach(std::function<std::vector<RowT>()> rows, std::function<Key(RowT const&)> keyOf,
                           std::function<Node(reactive::Signal<RowT> const&)> rowView, Axis axis = Axis::Vertical,
                           int gap = 0) {
    return detail::makeNode(ForEach{
        .model =
            std::make_shared<detail::TypedForEach<RowT> const>(std::move(rows), std::move(keyOf), std::move(rowView)),
        .axis = axis,
        .gap = gap,
    });
}

/// @brief One widget per row of a keyed collection held in a signal.
/// @tparam RowT The row type; copyable.
/// @param rows The signal; it must outlive every mount of the node.
/// @param keyOf A row's identity; a later duplicate of a key is reported and refused, and the first one is kept.
/// @param rowView Builds a row's view once, from a signal the mount keeps equal to the row.
/// @param axis The stacking direction.
/// @param gap Space between rows.
/// @return The node.
/// @throws std::invalid_argument when @p keyOf or @p rowView is empty.
template <typename RowT>
[[nodiscard]] Node forEach(reactive::Signal<std::vector<RowT>> const& rows, std::function<Key(RowT const&)> keyOf,
                           std::function<Node(reactive::Signal<RowT> const&)> rowView, Axis axis = Axis::Vertical,
                           int gap = 0) {
    return forEach<RowT>([&rows] { return rows.get(); }, std::move(keyOf), std::move(rowView), axis, gap);
}

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
///
/// Rows are mounted, updated, unmounted and reordered as `forEach` does. A row whose cells are not exactly one
/// non-null node per column would shift the columns after it, so it is refused as a row that fails to mount is:
/// reported, left out, and tried again on the next change of the rows.
/// @tparam RowT The row type; copyable.
/// @param columns The columns, in order.
/// @param rows Reads the rows; every signal it reads is a dependency.
/// @param keyOf A row's identity; a later duplicate of a key is reported and refused, and the first one is kept.
/// @param cells Builds a row's cells once, one per column, from a signal the mount keeps equal to the row. Read the
///        signal inside bindings: what the cells read directly while they are built is read once and never again.
/// @param options Selection, activation and `Common`.
/// @return The node.
/// @throws std::invalid_argument when @p rows, @p keyOf or @p cells is empty.
template <typename RowT>
[[nodiscard]] Node table(std::vector<TableColumn> columns, std::function<std::vector<RowT>()> rows,
                         std::function<Key(RowT const&)> keyOf,
                         std::function<std::vector<Node>(reactive::Signal<RowT> const&)> cells,
                         TableOptions options = {}) {
    if (!rows || !keyOf || !cells) {
        throw std::invalid_argument{"ui::table: the rows, the key function and the cells must not be empty"};
    }
    std::function<Node(reactive::Signal<RowT> const&)> rowView =
        [cellsOf = std::move(cells), count = columns.size()](reactive::Signal<RowT> const& entry) {
            std::vector<Node> rowCells = cellsOf(entry);
            if (rowCells.size() != count ||
                std::ranges::any_of(rowCells, [](Node const& cell) { return cell == nullptr; })) {
                throw std::invalid_argument{"ui::table: a row needs exactly one non-null cell per column"};
            }
            return row(Row{.children = std::move(rowCells)});
        };
    Table spec{
        .rows =
            std::make_shared<detail::TypedForEach<RowT> const>(std::move(rows), std::move(keyOf), std::move(rowView)),
        .selectionMode = options.selectionMode,
    };
    spec.columns = std::move(columns);
    spec.selection = std::move(options.selection);
    spec.onSelectionChange = std::move(options.onSelectionChange);
    spec.onActivate = std::move(options.onActivate);
    spec.common = std::move(options.common);
    return detail::makeNode(std::move(spec));
}

}  // namespace morph::ui

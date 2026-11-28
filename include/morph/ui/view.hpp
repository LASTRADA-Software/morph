// SPDX-License-Identifier: Apache-2.0

#pragma once
#include <algorithm>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
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
#include "../table/bands.hpp"
#include "../util/datetime.hpp"
#include "alignment.hpp"
#include "badge_style.hpp"
#include "banner_action_style.hpp"
#include "button_emphasis.hpp"
#include "button_style.hpp"
#include "control_shape.hpp"
#include "input_emphasis.hpp"
#include "page_layout.hpp"
#include "text_align.hpp"
#include "value.hpp"

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
    Prop(F binding) : _value{std::in_place_index<1>, Slot{.id = std::nullopt, .read = std::move(binding)}} {
        refuseEmpty();
    }

    /// @brief A slot with an id, as `ui::slot` makes it.
    /// @tparam F A callable taking no arguments and returning something convertible to `T`.
    /// @param binding The id and the read.
    /// @throws std::invalid_argument when the read is an empty `std::function` or a null function pointer.
    template <typename F>
        requires std::invocable<F&> && std::convertible_to<std::invoke_result_t<F&>, T>
    Prop(SlotBinding<F> binding)
        : _value{std::in_place_index<1>, Slot{.id = binding.id, .read = std::move(binding.read)}} {
        refuseEmpty();
    }

    /// @brief A copy: the same constant, or a slot with the same id and a copy of its read.
    /// @param other The property to copy.
    /// @throws std::bad_variant_access when @p other was left without a value by an assignment that threw.
    Prop(Prop const& other) : _value{copyOf(other._value)} {}

    /// @brief Moves the constant or the slot out of another property.
    Prop(Prop&&) = default;

    /// @brief Replaces this property with a copy of another.
    /// @return This property.
    Prop& operator=(Prop const&) = default;

    /// @brief Replaces this property with another, moved from.
    /// @return This property.
    Prop& operator=(Prop&&) = default;

    /// @brief Destroys the constant or the slot.
    ~Prop() = default;

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
        Slot const* const bound = std::get_if<1>(&_value);
        return bound == nullptr ? std::nullopt : bound->id;
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

    /// @brief Refuses a slot whose read is empty: an empty `std::function` or a null function pointer.
    /// @throws std::invalid_argument when the read is empty.
    void refuseEmpty() const {
        if (!std::get<1>(_value).read) {
            throw std::invalid_argument{"ui::Prop: a binding must not be empty"};
        }
    }

    /// @brief A copy of @p from, built in place as the alternative it holds.
    ///
    /// The copy constructor uses this rather than `std::variant`'s own. GCC 15 at -O3 warns
    /// (-Wmaybe-uninitialized) about the variant's copy: on its exception path it destroys a `Slot` whose
    /// `std::function` it cannot see was constructed. Constructing the chosen alternative in place leaves no
    /// partly built variant to destroy, so that path does not exist.
    /// @param from The value to copy.
    /// @return The copy.
    /// @throws std::bad_variant_access when @p from holds no alternative.
    static std::variant<T, Slot> copyOf(std::variant<T, Slot> const& from) {
        if (Slot const* const bound = std::get_if<1>(&from); bound != nullptr) {
            return std::variant<T, Slot>{std::in_place_index<1>, *bound};
        }
        return std::variant<T, Slot>{std::in_place_index<0>, std::get<0>(from)};
    }

    std::variant<T, Slot> _value;
};

/// @brief How a `Text` is styled; each backend maps a role to its theme.
enum class TextRole : std::uint8_t {
    Normal,    ///< Body text.
    Muted,     ///< Secondary text.
    Heading,   ///< A section heading.
    Error,     ///< An error message.
    Success,   ///< A confirmation.
    Section,   ///< The heading of a group within a list: body size, bold and muted, below a `Heading`.
    Mono,      ///< An identifier: the theme's fixed-pitch family (`font.mono`) at `font.size.mono`, the text colour.
    Display,   ///< A figure that stands out, such as a KPI's number: `font.size.display`, strong, the text colour.
    Small,     ///< A footnote or a timestamp: `font.size.small`, regular, the text colour.
    Overline,  ///< A short label over a group: as `Section` unless the theme's `font.overline.case` is `uppercase`,
               ///< which draws it at `font.size.small`, strong, muted, in letter-spaced capitals. Written as it
               ///< reads; the terminal draws it as `Section`, whatever the theme.
    Caption,   ///< A note under a control or a figure: `font.size.caption`, regular, muted. The terminal draws it as
               ///< `Muted`.
    Label,  ///< A field's or a group's caption: `font.size.caption`, regular, the text colour. The terminal draws it
            ///< as `Normal`.
    Headline,  ///< A record's identity: `font.size.headline`, strong, the text colour. It sits between `Heading` and
               ///< `Display`; the terminal draws it as `Heading`.
};

/// @brief How a `TextInput` edits.
enum class TextInputMode : std::uint8_t {
    SingleLine,  ///< One line; Enter submits.
    Multiline,   ///< Several lines.
    Password,    ///< One line, masked.
};

/// @brief How a `Select` presents its options.
enum class SelectStyle : std::uint8_t {
    Dropdown,   ///< A closed field that opens a list.
    Radio,      ///< Every option visible, one marked.
    Segmented,  ///< Every option visible as one row of joined segments, one marked.
};

/// @brief How a `Menu` presents its entries.
enum class MenuStyle : std::uint8_t {
    Bar,     ///< One control per top-level entry.
    Button,  ///< One button that opens the entries as a popup.
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

/// @brief What a message or a mark conveys, mapped to the theme's colours.
enum class Tone : std::uint8_t {
    Neutral,  ///< Nothing in particular.
    Info,     ///< Information.
    Ok,       ///< Success.
    Warn,     ///< Something to check.
    Err,      ///< A failure.
};

/// @brief The edge a `Drawer` slides in from, in reading direction.
enum class Side : std::uint8_t {
    Start,  ///< The left edge in a left-to-right layout.
    End,    ///< The right edge in a left-to-right layout.
};

/// @brief Where one step of a `Steps` indicator stands.
enum class StepState : std::uint8_t {
    Pending,  ///< Not started.
    Active,   ///< In progress.
    Done,     ///< Completed.
    Failed,   ///< Stopped by a failure.
};

/// @brief How a chart's reference line is stroked.
enum class ChartLineStyle : std::uint8_t {
    Solid,   ///< An unbroken line.
    Dashed,  ///< Dashes.
    Dotted,  ///< Dots.
};

/// @brief Whether a chart's point lies outside the value axis the document fixed, and on which side.
enum class ChartClip : std::uint8_t {
    None,   ///< Inside the axis.
    Below,  ///< Below its lower end; drawn at that end, pointing down.
    Above,  ///< Above its upper end; drawn at that end, pointing up.
};

/// @brief How a chart's series is drawn.
enum class ChartStyle : std::uint8_t {
    Line,  ///< Points, joined by a line when the series is `joined`.
    Bars,  ///< A bar from the baseline to each value, side by side with the other `Bars` series at each position.
};

/// @brief How many rows of a `Table` the user can select.
enum class SelectionMode : std::uint8_t {
    None,      ///< Rows can be activated, not selected.
    Single,    ///< At most one row.
    Multiple,  ///< Any number of rows.
};

/// @brief What a user's change of a table's selection did, which travels with every report: a backend that holds
///        only some of the rows (one page) cannot report the keys it does not hold, so only the kind tells whether
///        those stay selected.
enum class SelectionChange : std::uint8_t {
    Replace,  ///< The reported keys are the whole selection: a click, a Shift-click or Shift+arrow, any `Single`
              ///< change.
    Extend,   ///< The reported keys replace those the backend holds and keep the rest: a Ctrl-click, Space, Ctrl+A.
};

/// @brief Which way a table is sorted by a column.
enum class SortDirection : std::uint8_t {
    Ascending,   ///< Smallest first.
    Descending,  ///< Largest first.
};

/// @brief The column a table's header marks as sorting the rows, and its direction.
struct TableSort {
    /// @brief The column's index among the columns `setColumns` gave.
    std::size_t column = 0;
    /// @brief The direction.
    SortDirection direction = SortDirection::Ascending;
    /// @brief Equal when both fields are.
    /// @param other The other sort.
    /// @return Whether they are equal.
    [[nodiscard]] bool operator==(TableSort const& other) const = default;
};

/// @brief How a click on a sortable header, or `Enter` or `Space` on a focused one, was modified.
enum class HeaderClick : std::uint8_t {
    Plain,  ///< No modifier: sort by the column alone.
    Shift,  ///< `Shift` held: add the column to the sort chain, or flip its direction where it is in it.
};

/// @brief The mark a header draws after its label for one key of the sort chain.
/// @param direction The key's direction.
/// @param place     The key's place in the chain, from 1.
/// @param count     How many keys the chain holds.
/// @return `" ▲"` or `" ▼"` for a chain of one key, else the arrow followed by @p place, such as `" ▲2"`.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters): (`place`, `count`): a chain position then the chain length, passed by name at the three call sites
[[nodiscard]] inline std::string sortMarkText(SortDirection direction, std::size_t place, std::size_t count) {
    std::string mark = direction == SortDirection::Ascending ? " ▲" : " ▼";
    if (count != 1) {
        mark += std::to_string(place);
    }
    return mark;
}

/// @brief A sorted header's accessible description, for one key of the sort chain; both backends set it from this
///        and nothing else.
/// @param direction The key's direction.
/// @param place     The key's place in the chain, from 1.
/// @param count     How many keys the chain holds.
/// @return `"sorted ascending"` or `"sorted descending"` for a chain of one key, else, for example,
///         `"sorted ascending, key 2 of 3"`.
[[nodiscard]] inline std::string sortDescription(SortDirection direction, std::size_t place, std::size_t count) {
    std::string text = direction == SortDirection::Ascending ? "sorted ascending" : "sorted descending";
    if (count != 1) {
        text += ", key " + std::to_string(place) + " of " + std::to_string(count);
    }
    return text;
}

/// @brief A column group's header, drawn over its columns while they stand side by side in the layout.
struct TableColumnGroup {
    /// @brief The group's caption.
    std::string label;
    /// @brief The visible place of its first column in the table's arrangement.
    std::size_t first = 0;
    /// @brief How many visible columns, from `first`, it stands over.
    std::size_t count = 0;
    /// @brief Whether the group has a filter: its filter button is drawn filled.
    bool filtered = false;

    /// @brief Member-wise equality.
    /// @param other The group to compare with.
    /// @return `true` when every member matches.
    [[nodiscard]] bool operator==(TableColumnGroup const& other) const = default;
};

/// @brief One entry of a menu as a `MenuWidget` shows it.
struct MenuEntry {
    /// @brief The caption.
    std::string label;
    /// @brief The icon's name in the theme's icon map; empty shows none.
    std::string icon;
    /// @brief The chord shown beside the label; empty shows none.
    std::string keys;
    /// @brief Engaged: a checkable entry with this mark; `nullopt`: not checkable.
    std::optional<bool> checked;
    /// @brief Whether the entry can be chosen, or its submenu opened.
    bool enabled = true;
    /// @brief The submenu; empty for an entry that is chosen.
    std::vector<MenuEntry> items;
    /// @brief Whether the entry is a line between entries: never chosen, never opened, and its other fields empty.
    bool separator = false;
    /// @brief A muted line of help shown with the entry; empty shows none.
    std::string description;
    /// @brief Whether the entry is a label that is not a command: drawn as muted text, exposed as static text, never
    ///        chosen or opened. Its other fields are empty.
    bool heading = false;

    /// @brief Memberwise equality, submenus included.
    /// @return Whether every field matches.
    bool operator==(MenuEntry const&) const = default;
};

/// @brief How much space a widget asks for along one dimension, in backend units (a character cell on the TUI).
struct Sizing {
    /// @brief The sizing rule.
    enum class Kind : std::uint8_t {
        Content,  ///< As much as the content needs.
        Fixed,    ///< Exactly `amount` units.
        Stretch,  ///< A share of the leftover space, weighted by `amount`.
        Percent,  ///< `amount` percent of the container's width inside its padding; a width along a row or for a
                  ///< horizontal splitter's pane only. Gaps and a splitter's handles are not taken off first.
    };
    /// @brief The rule.
    Kind kind = Kind::Content;
    /// @brief Units for `Fixed`, weight for `Stretch`, a share from 1 to 100 for `Percent`, unused for `Content`.
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

    /// @brief A share of the container's width, which a node's bounds clamp as they do every sizing: a pane is
    ///        usually a percent with a `maxWidth`. Only a width along a row that does not wrap and a horizontal
    ///        splitter's pane take one; the mount refuses it anywhere else.
    /// @param percent The share, 1 to 100.
    /// @return The sizing.
    [[nodiscard]] static constexpr Sizing percent(int percent) noexcept {
        return {.kind = Kind::Percent, .amount = percent};
    }

    /// @brief Memberwise equality.
    /// @return Whether both rules and amounts match.
    bool operator==(Sizing const&) const = default;
};

/// @brief A widget's size request in both dimensions, and the bounds its extent is clamped into.
///
/// A bound clamps the extent the sizing gives, whatever the sizing: a stretch child's share stops at its maximum
/// and a content or fixed child takes at least its minimum. A negative bound, a minimum above its maximum and a
/// fixed amount outside its bounds are refused at mount (`std::logic_error`).
struct LayoutHints {
    /// @brief Along the horizontal axis.
    Sizing width{};
    /// @brief Along the vertical axis.
    Sizing height{};
    /// @brief The least width, in backend units; `nullopt` for none.
    std::optional<int> minWidth;
    /// @brief The most width, in backend units; `nullopt` for none.
    std::optional<int> maxWidth;
    /// @brief The least height, in backend units; `nullopt` for none.
    std::optional<int> minHeight;
    /// @brief The most height, in backend units; `nullopt` for none.
    std::optional<int> maxHeight;
    /// @brief Where the node sits across its row's, column's or grid row's axis; `nullopt` takes the container's
    ///        `align`.
    std::optional<Align> alignSelf;
    /// @brief Whether the node's sides lie at its parent's edges, past the parent's horizontal padding, while its
    ///        siblings stay inside it. Only a direct child of a column, a panel or a tab page bleeds, and only
    ///        across: the node's place along the parent is its siblings'.
    bool bleed = false;

    /// @brief Memberwise equality.
    /// @return Whether the sizings, the bounds and the alignment match.
    bool operator==(LayoutHints const&) const = default;
};

/// @brief What assistive technology announces for a node.
struct Accessibility {
    /// @brief The accessible name; empty leaves the widget's own text as its name.
    Prop<std::string> name;
    /// @brief The role, such as `"heading"`; empty leaves the kind's own role. Set once.
    std::string role;
};

/// @brief A keyboard chord a node handles while focus is inside it.
struct KeyBinding {
    /// @brief The chord: modifiers (`Ctrl`, `Shift`, `Alt`, `Meta`) joined to a key name, such as `"Ctrl+S"`.
    std::string chord;
    /// @brief Called when the chord is pressed.
    Action onPress;
};

/// @brief The part every node carries: visibility, enablement, layout, drag-and-drop, accessibility, test identity,
///        tooltip, surface, keyboard chords and autofocus.
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
    /// @brief The accessible name and role.
    Accessibility a11y{};
    /// @brief The stable identifier an automated test finds the widget by; empty means none. Set once.
    std::string testId;
    /// @brief The name a focus request after mount finds the widget by (`Mounted::focus`), such as a document
    ///        element's id; empty means none. Bound, the widget is found by the name it has now: an element in a
    ///        table cell a pooled cell shows for another row takes that row's name.
    Prop<std::string> focusId;
    /// @brief Text shown while the pointer rests on the widget; empty shows none.
    Prop<std::string> tooltip;
    /// @brief The named token set of the theme that styles this node and everything inside it; empty is the
    ///        theme's default. May be bound: the widget restyles itself and its content on each change.
    Prop<std::string> surface;
    /// @brief Chords handled while focus is inside the node; the innermost node declaring a chord takes it. For a
    ///        chord listed twice, the first entry is the one called.
    std::vector<KeyBinding> keys;
    /// @brief Whether the widget takes keyboard focus when the content it is part of mounts; the first such widget
    ///        in document order wins.
    bool autofocus = false;
};

/// @brief The state of a value a user edits or reads: whether it may be changed, must be given, is wrong, or is
///        being recomputed.
///
/// On text, which no user edits, `readonly` changes nothing; the other three mark it as they mark an input. A
/// constant default calls no setter.
struct FieldState {
    /// @brief Whether the user may not change the value. A read-only field shows its value and can take focus and
    ///        have its text selected; it takes no edit, submit or commit.
    Prop<bool> readonly = false;
    /// @brief Whether the field is marked as one that must be given. Only a mark: whether a value is missing is the
    ///        application's to decide and show through `errors`.
    Prop<bool> required = false;
    /// @brief The messages shown with the field, in order; empty shows none. Which errors show, and when, is
    ///        decided before they reach this property.
    Prop<std::vector<std::string>> errors;
    /// @brief Whether the value shown is the last known one while a newer one is computed.
    Prop<bool> stale = false;
    /// @brief A line that describes the field, under its control and announced as its accessible description; empty
    ///        shows none. Not on a text.
    Prop<std::string> description;
    /// @brief A hint drawn after the caption and its required mark, such as the range a value must lie in; empty
    ///        shows none. Drawn only while the input shows a caption. Not on a text.
    Prop<std::string> labelHint;
    /// @brief An evaluation of the value that does not make it invalid, drawn as the control's outline and the
    ///        description's colour; `Neutral` is none. On a text it is the text's colour.
    Prop<Tone> tone = Tone::Neutral;
};

/// @brief A run of text inside a sentence, drawn in the node's role with its own weight, face and colour.
struct TextSpan {
    /// @brief The run's text, UTF-8; shown as its characters, never as markup.
    std::string text;
    /// @brief Whether the run is drawn in the strong weight.
    bool strong = false;
    /// @brief Whether the run is drawn in the theme's monospace face (`font.mono`).
    bool mono = false;
    /// @brief The run's colour, the tone's; `Neutral` is the text's own colour.
    Tone tone = Tone::Neutral;
    /// @brief Whether the run is the action of a banner drawn as a link: the run's text is the link, at its place in
    ///        the sentence. Only a banner reads it.
    bool action = false;

    /// @brief Memberwise equality.
    /// @return Whether text, both flags, the tone and the action mark match.
    bool operator==(TextSpan const&) const = default;
};

/// @brief The characters a list of runs shows, joined in order: what assistive technology and a probe read.
/// @param spans The runs.
/// @return Their text, joined.
[[nodiscard]] inline std::string spansText(std::vector<TextSpan> const& spans) {
    std::string joined;
    for (TextSpan const& span : spans) {
        joined += span.text;
    }
    return joined;
}

struct NodeData;

/// @brief A view-tree node: immutable, shared data.
using Node = std::shared_ptr<NodeData const>;

/// @brief Static or bound text.
struct Text {
    /// @brief The text, UTF-8; not applied while the text has spans.
    Prop<std::string> text;
    /// @brief The text as runs, each plain, strong or monospace; while bound or not empty, it is shown in place of
    ///        `text`.
    Prop<std::vector<TextSpan>> spans;
    /// @brief The style.
    Prop<TextRole> role = TextRole::Normal;
    /// @brief Where each line sits within the text's width; set once. Unset leaves a text in a table to its column's
    ///        alignment; set, even to `Start`, it keeps its own. It shows nothing on a text as wide as its content.
    std::optional<TextAlign> align;
    /// @brief Read-only, required, errors and stale.
    FieldState field{};
    /// @brief Visibility, enablement, layout, drag-and-drop.
    Common common{};
};

/// @brief A push button.
struct Button {
    /// @brief The caption.
    Prop<std::string> label;
    /// @brief How much it stands out; set once.
    ButtonEmphasis emphasis = ButtonEmphasis::Normal;
    /// @brief Called on activation.
    Action onClick;
    /// @brief Visibility, enablement, layout, drag-and-drop.
    Common common{};
    /// @brief The name of an icon in the theme's icon set, drawn beside the caption; empty shows none. Set once.
    std::string icon;
    /// @brief A push button or a list entry; set once.
    ButtonStyle style = ButtonStyle::Push;
    /// @brief An entry's content, drawn in place of its label, which then names it; only on an `Item`. May be null.
    Node child;
    /// @brief Whether the button is selected: a list entry is the current one, a push button is pressed in.
    Prop<bool> selected = false;
    /// @brief Its outline: the theme's corners, or round; set once.
    ControlShape shape = ControlShape::Default;
    /// @brief Its density: normal, or small, with the least side padding and the small type step; set once.
    ButtonSize size = ButtonSize::Normal;
    /// @brief Whether the caption is drawn in the `mono` text role's face; set once.
    bool mono = false;
    /// @brief The weight of the caption, 1 to 1000; 0 keeps the theme's (`font.weight.button` for a strong caption,
    ///        normal for the rest). Set once.
    int weight = 0;
};

/// @brief An editable text field. The backend never reports its own `setText` back through `onChange`.
struct TextInput {
    /// @brief The text the field shows.
    Prop<std::string> value;
    /// @brief Called with the new text after every user edit.
    std::function<void(std::string)> onChange;
    /// @brief Called with the text when the user submits (Enter in single-line mode).
    std::function<void(std::string)> onSubmit;
    /// @brief Called with the text when the user commits it: Enter in single-line mode, or focus leaving the field
    ///        after an edit.
    std::function<void(std::string)> onCommit;
    /// @brief Shown while the field is empty.
    Prop<std::string> placeholder;
    /// @brief Single-line, multiline or password; set once.
    TextInputMode mode = TextInputMode::SingleLine;
    /// @brief The unit the input shows beside its text, such as `kg`; empty shows none. Only a single-line or
    ///        password input shows one: the mount refuses a multiline input with a unit.
    Prop<std::string> unit;
    /// @brief The name of an icon in the theme's icon set, drawn at the field's leading edge; empty shows none. Only
    ///        a single-line or password input shows one: the mount refuses a multiline input with an icon. Set once.
    std::string icon;
    /// @brief The fraction digits a number is entered with, for a backend or a kit to read; -1 when the input does
    ///        not hold a number. It follows its binding: the mount tells the widget again on each change. The mount
    ///        refuses places on a multiline input.
    Prop<int> decimals = -1;
    /// @brief The field's label: its visible caption while `showLabel`; a form's cell draws it otherwise, beside
    ///        the input. Empty: none.
    Prop<std::string> label;
    /// @brief Whether the input draws `label` as a visible caption itself; set once. A form leaves it false, since
    ///        its cell draws the caption.
    bool showLabel = false;
    /// @brief Read-only, required, errors and stale.
    FieldState field{};
    /// @brief Visibility, enablement, layout, drag-and-drop.
    Common common{};
    /// @brief How much it stands out; set once, and never on a multiline input. The mount tells the widget only
    ///        `Prominent`.
    InputEmphasis emphasis = InputEmphasis::Normal;
    /// @brief Whether the text and the placeholder are drawn in the `mono` text role's face; set once.
    bool mono = false;
    /// @brief Whether a multiline input wraps a line longer than the field; `false` keeps each line whole and the
    ///        field scrolls sideways. Set once; never `false` on a single-line or password input, which has no lines
    ///        to wrap.
    bool wrap = true;
    /// @brief Where the text sits in the field: `Start`, `Center` or `End`; set once. Never on a multiline input,
    ///        and the mount tells the widget only a value other than `Start`.
    Align align = Align::Start;
    /// @brief Text drawn muted at the field's leading edge, inside it, such as `±`; empty shows none. Only a
    ///        single-line or password input shows one: the mount refuses a multiline input with a prefix.
    Prop<std::string> prefix;
};

/// @brief A labelled check box.
struct Checkbox {
    /// @brief The caption.
    Prop<std::string> label;
    /// @brief Whether it is checked.
    Prop<bool> checked;
    /// @brief Called with the new state when the user toggles it.
    std::function<void(bool)> onToggle;
    /// @brief Read-only, required, errors and stale.
    FieldState field{};
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
    /// @brief Called when the user opens the dropdown, whether or not it has options to show; a radio group is never
    ///        opened. A hint, such as retrying a fetch of the options that failed: a backend may not report it.
    Action onOpen;
    /// @brief Dropdown or radio; set once.
    SelectStyle style = SelectStyle::Dropdown;
    /// @brief The field's label: its visible caption while `showLabel`; a form's cell draws it otherwise, beside
    ///        the input. Empty: none.
    Prop<std::string> label;
    /// @brief Whether the input draws `label` as a visible caption itself; set once. A form leaves it false, since
    ///        its cell draws the caption.
    bool showLabel = false;
    /// @brief Read-only, required, errors and stale.
    FieldState field{};
    /// @brief Visibility, enablement, layout, drag-and-drop.
    Common common{};
};

/// @brief One entry of a `Menu`: its label and its handler in one place, so there are no parallel lists.
///
/// An entry with `items` opens a submenu and is not itself chosen.
struct MenuItem {
    /// @brief The caption.
    Prop<std::string> label;
    /// @brief Called when the entry is chosen.
    Action onSelect;
    /// @brief The name of an icon in the theme's icon map; empty shows none. Set once.
    std::string icon;
    /// @brief The chord shown beside the label, such as `"Ctrl+S"`; empty shows none. Set once. It only shows the
    ///        chord: the binding that handles it is a `KeyBinding` on a node.
    std::string keys;
    /// @brief Engaged: the entry is checkable and shows this check mark; `nullopt`: not checkable. Choosing a
    ///        checkable entry runs `onSelect`, and the mark changes only when this binding does.
    Prop<std::optional<bool>> checked;
    /// @brief Whether the entry can be chosen; a disabled entry's submenu cannot be opened.
    Prop<bool> enabled = true;
    /// @brief The submenu's entries; empty: the entry is chosen rather than opened.
    std::vector<MenuItem> items;
    /// @brief Whether the entry is a line between entries, which is never chosen and shows no other field. Set once.
    bool separator = false;
    /// @brief A muted line of help with the entry; empty shows none.
    Prop<std::string> description;
    /// @brief Whether the entry is shown; a hidden entry is not sent at all, and a separator a hidden entry leaves
    ///        at the start or end of its level, or next to another separator, is not sent either.
    Prop<bool> visible = true;
};

/// @brief A menu of commands: a bar of top-level entries, or one button that opens them.
struct Menu {
    /// @brief The entries, in order; the list itself is set once, each entry's label, description, check mark,
    ///        enablement and visibility may be bound.
    std::vector<MenuItem> items;
    /// @brief Visibility, enablement, layout, drag-and-drop.
    Common common{};
    /// @brief How the entries are presented; set once.
    MenuStyle style = MenuStyle::Bar;
    /// @brief The trigger's label, for `MenuStyle::Button`; empty draws the icon alone.
    Prop<std::string> label;
    /// @brief The trigger's icon, for `MenuStyle::Button`: a name in the theme's icon map; empty shows none. Set
    ///        once.
    std::string icon;
    /// @brief The trigger's emphasis, for `MenuStyle::Button`; set once.
    ButtonEmphasis emphasis = ButtonEmphasis::Normal;
};

/// @brief The largest spacing step: a `gap` or `padding` names a step from 0 (none) to this, which each backend maps
///        to its own distance (the theme's `space.<step>` on Qt Quick, cells on the terminal).
inline constexpr int kMaxSpacingStep = 5;

/// @brief A `gap` or `padding` as the spacing step a backend receives.
/// @param value The value as written.
/// @return @p value clamped to `[0, kMaxSpacingStep]`.
[[nodiscard]] constexpr int spacingStep(int value) noexcept { return std::clamp(value, 0, kMaxSpacingStep); }

/// @brief The inset of a stack's children: one spacing step above and below, one at the sides.
struct Padding {
    /// @brief The step above and below, 0 to `kMaxSpacingStep`.
    int vertical = 0;
    /// @brief The step at the leading and trailing sides, 0 to `kMaxSpacingStep`.
    int horizontal = 0;

    /// @brief The same step on every side.
    /// @param step A spacing step.
    // NOLINTNEXTLINE(google-explicit-constructor,hicpp-explicit-conversions): `.padding = 3` writes every side.
    constexpr Padding(int step = 0) noexcept : vertical(step), horizontal(step) {}

    /// @brief A step for each axis.
    /// @param vertical_   The step above and below.
    /// @param horizontal_ The step at the sides.
    // NOLINTNEXTLINE(bugprone-easily-swappable-parameters): vertical, then horizontal; swapped, only the axes trade places.
    constexpr Padding(int vertical_, int horizontal_) noexcept : vertical(vertical_), horizontal(horizontal_) {}

    /// @brief Whether both steps match.
    /// @param other The other padding.
    /// @return Whether both axes are equal.
    [[nodiscard]] constexpr bool operator==(Padding const& other) const noexcept = default;
};

/// @brief A padding with each step clamped as `spacingStep` clamps one.
/// @param padding The padding as written.
/// @return It, with both steps in `[0, kMaxSpacingStep]`.
[[nodiscard]] constexpr Padding clampedPadding(Padding padding) noexcept {
    return Padding{spacingStep(padding.vertical), spacingStep(padding.horizontal)};
}

/// @brief Children stacked top to bottom.
struct Column {
    /// @brief The children, in order; a null child is skipped.
    std::vector<Node> children;
    /// @brief Space between children: a spacing step, 0 to `kMaxSpacingStep`.
    int gap = 0;
    /// @brief Visibility, enablement, layout, drag-and-drop.
    Common common{};
    /// @brief Where the children sit across the column (horizontally); set once.
    Align align = Align::Stretch;
    /// @brief Where the spare height goes while no shown child stretches; set once.
    Justify justify = Justify::Start;
    /// @brief Whether a 1 px rule in the theme's divider colour is drawn between each two shown children, centred in
    ///        the gap; set once. The terminal draws none.
    bool divided = false;
    /// @brief The inset of the children: a spacing step on every side, or one step above and below and one at the
    ///        sides; set once.
    Padding padding;
    /// @brief Whether the stack is the chosen entry of a list: while true it draws the accent's 3 px bar at its
    ///        leading edge, over its surface and under its children, and takes the `Grouping` accessibility role.
    ///        Disengaged when none was given. The mount refuses it on a table's row view.
    std::optional<Prop<bool>> selected;
    /// @brief Whether the column is the holder the interpreter makes for a list of nodes (a panel's, a tab page's, a
    ///        scroll's children). A holder is transparent to `bleed`: its children reach past the padding of the
    ///        container the document names, not past the holder's own. The mount calls
    ///        `StackWidget::setPassesBleed(true)` for a holder, before its first child; set once.
    bool holder = false;
};

/// @brief A table cell that covers cells below and beside its own: the cell of @ref column in the row that gives it,
///        extended down over @ref rows rows counting that row, and right over @ref columns visible columns counting
///        its own.
struct RowSpan {
    /// @brief The column's index in the table's columns; the select column is not one.
    std::size_t column = 0;
    /// @brief How many rows the cell covers, the row that gives it included; 1 covers nothing.
    std::size_t rows = 1;
    /// @brief How many columns the cell covers by visible position, its own included; 1 covers nothing beside it. The
    ///        giver cuts it where a column the span names was moved away or hidden (`cutColumns`).
    std::size_t columns = 1;

    /// @brief Whether two spans name the same column and counts of rows and columns.
    /// @param other The other span.
    /// @return True when equal.
    [[nodiscard]] bool operator==(RowSpan const& other) const noexcept = default;
};

/// @brief Children laid out left to right.
struct Row {
    /// @brief The children, in order; a null child is skipped.
    std::vector<Node> children;
    /// @brief Space between children: a spacing step, 0 to `kMaxSpacingStep`.
    int gap = 0;
    /// @brief Visibility, enablement, layout, drag-and-drop.
    Common common{};
    /// @brief A table row's tone: disengaged when none was given; an engaged `Prop` whose value is `nullopt` has no
    ///        tone at the moment. Only `table`'s row view sets it; the mount refuses it on a row outside a table.
    std::optional<Prop<std::optional<Tone>>> tone;
    /// @brief A table row's accessible name, and its select box's: disengaged when none was given, which names the
    ///        row by its key. Only `table`'s row view sets it; the mount refuses it on a row outside a table.
    std::optional<Prop<std::string>> label;
    /// @brief Whether a table row is a heading: one cell spanning every column, never selected or activated. Only
    ///        `table`'s row view sets it; the mount refuses it on a row outside a table.
    bool heading = false;
    /// @brief A table row's spans, bound in the row's scope: disengaged when none was given. Only `table`'s row view
    ///        sets it; the mount refuses it on a row outside a table.
    std::optional<Prop<std::vector<RowSpan>>> spans;
    /// @brief Where the children sit across the row (vertically); set once.
    Align align = Align::Stretch;
    /// @brief Where the spare width goes while no shown child stretches; set once.
    Justify justify = Justify::Start;
    /// @brief Whether children that do not fit on one line continue on the next, as a flex row with `wrap` does;
    ///        set once. A table row never wraps.
    bool wrap = false;
    /// @brief Whether a vertical 1 px rule in the theme's divider colour is drawn between each two shown children,
    ///        centred in the gap; set once. A wrapping row is not divided: the mount ignores it there.
    bool divided = false;
    /// @brief The inset of the children: a spacing step on every side, or one step above and below and one at the
    ///        sides; set once.
    Padding padding;
    /// @brief Whether the stack is the chosen entry of a list: while true it draws the accent's 3 px bar at its
    ///        leading edge, over its surface and under its children, and takes the `Grouping` accessibility role.
    ///        Disengaged when none was given. The mount refuses it on a table's row view.
    std::optional<Prop<bool>> selected;
};

/// @brief One cell of a `Grid`.
struct GridCell {
    /// @brief The cell's content; a null node leaves no cell.
    Node node;
    /// @brief How many columns it spans.
    int span = 1;
};

/// @brief One column track of a grid: how wide its column is and where each cell sits across it.
struct GridTrack {
    /// @brief The column's width: fixed tracks first, content tracks to their widest one-column cell, stretch tracks
    ///        share the rest by weight.
    Sizing width = Sizing::stretch();
    /// @brief Where a cell sits across the column: the text of a text cell and a label-width cell (a badge, a check
    ///        box, a push button) at its natural width, at the start, centre or end. A container cell fills the
    ///        column whatever this says; `Stretch` reads as `Start`.
    Align align = Align::Start;
};

/// @brief Cells laid out row-major in a fixed number of columns.
struct Grid {
    /// @brief The column count; ignored when `tracks` is not empty.
    int columns = 1;
    /// @brief The column tracks, set once; empty gives `columns` equal columns, a content-sized grid as wide as its
    ///        widest cell times the columns. Given, the column count is their number.
    std::vector<GridTrack> tracks;
    /// @brief The cells, row-major.
    std::vector<GridCell> cells;
    /// @brief Space between cells: a spacing step, 0 to `kMaxSpacingStep`.
    int gap = 0;
    /// @brief Visibility, enablement, layout, drag-and-drop.
    Common common{};
    /// @brief Where each cell's content sits vertically in its row; set once. A grid's columns are sized by their
    ///        count or tracks, so there is no spare length to justify.
    Align align = Align::Stretch;
    /// @brief The least width of a column, in units, while above 0: the grid shows as many of its `columns` as fit
    ///        that width, and at least one, so its columns follow the room it is given; its height follows the columns
    ///        shown. Ignored with `tracks`; set once.
    int minColumnWidth = 0;
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
    /// @brief Shown beside the title, collapsed or not; may be null.
    Node header;
    /// @brief Space between frame and child: a spacing step, 0 to `kMaxSpacingStep`.
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
    /// @brief The tab's caption; may be bound, and the bar follows it.
    Prop<std::string> label;
    /// @brief The page content, mounted the first time the tab is selected.
    Node node;
};

/// @brief A tab bar over pages.
struct Tabs {
    /// @brief The pages, in order; the list is set once, each caption may be bound.
    std::vector<Tab> tabs;
    /// @brief The selected page's index; an index past the last page shows none.
    Prop<std::size_t> selected;
    /// @brief Called with the index the user picked, and with the page still shown when a picked page fails to mount.
    std::function<void(std::size_t)> onSelect;
    /// @brief The label of the menu that holds the tabs that do not fit in the bar, where a frontend has one.
    Prop<std::string> moreLabel = std::string{"More"};
    /// @brief Visibility, enablement, layout, drag-and-drop.
    Common common{};
    /// @brief The strip's inset, a spacing step, 0 to `kMaxSpacingStep`; set once. The strip and its rule span the
    ///        node's width, the first tab starts this far in from the leading edge, and each page is padded by it on
    ///        every side unless `pageInset` says otherwise.
    int inset = 0;
    /// @brief The padding of each page, a spacing step, 0 to `kMaxSpacingStep`; set once. Disengaged pads the pages
    ///        by `inset`. Each page pads its own content inside its scroll, so a child of a page can bleed to the
    ///        node's edges whether or not the page scrolls.
    std::optional<int> pageInset;
    /// @brief The surface the strip, and nothing else, is drawn on; empty draws it on the surface around it. Set once.
    std::string stripSurface;
};

/// @brief A modal overlay; its content is mounted only while it is open.
struct Dialog {
    /// @brief Whether it is shown.
    Prop<bool> open;
    /// @brief The title.
    Prop<std::string> title;
    /// @brief A line under the title, in the head, that describes the dialog and is its accessible description;
    ///        empty shows none. Applied only when bound or not empty.
    Prop<std::string> description;
    /// @brief The content; may be null.
    Node child;
    /// @brief Called when the user dismisses it (Esc on the TUI).
    Action onDismiss;
    /// @brief Visibility, enablement, layout, drag-and-drop.
    Common common{};
    /// @brief Whether it is drawn as a sheet that fills the window, with a head holding a dismiss control and the
    ///        title. Changing it while open keeps the content and its state.
    Prop<bool> sheet = false;
    /// @brief The label of the dismiss control, a sheet's or `closeButton`'s, which dismisses as Escape does.
    Prop<std::string> dismissLabel = std::string{"Close"};
    /// @brief The description as runs; while bound or not empty, shown in place of `description`, which an older
    ///        client draws.
    Prop<std::vector<TextSpan>> descriptionSpans;
    /// @brief A short muted note on the title's row, against the trailing edge; empty shows none.
    Prop<std::string> headNote;
    /// @brief A short muted note leading in the footer's row, opposite the actions; empty shows none.
    Prop<std::string> footerNote;
    /// @brief Whether a dismiss control, labelled `dismissLabel`, is drawn on the title's row, against the trailing
    ///        edge. A sheet's head holds one already, so this adds nothing while `sheet` holds.
    Prop<bool> closeButton = false;
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

/// @brief When a keyed row list realizes a widget only for the rows in or near the visible range, instead of for
///        every row regardless of scroll position. A row's own reactive state (its signal, rebuilt from the
///        session on every change) is unaffected either way and is never released while its key exists; this
///        setting only decides whether a row's mounted widget is also kept alive off-screen.
struct Virtualization {
    /// @brief Below this many rows, every row's widget is realized regardless of scroll position, matching
    ///        today's behavior exactly; set once.
    std::size_t threshold = 2000;
    /// @brief How many rows beyond the visible range, on each side, keep a realized widget so a small scroll does
    ///        not have to realize a widget on every frame; set once.
    std::size_t overscan = 10;
    /// @brief Above this many visible columns, a table over a `TableSource` realizes only the columns in or near the
    ///        horizontal viewport; set once.
    std::size_t columnThreshold = 40;
    /// @brief Above this many cells (rows times visible columns), such a table windows both its rows and its columns;
    ///        set once.
    std::size_t cellThreshold = 20'000;
    /// @brief How many columns beyond the horizontal viewport, on each side, keep realized cells; set once.
    std::size_t columnOverscan = 4;
    /// @brief The most cells one pass of a table's windows builds or binds to another row or column; the rest draw
    ///        blank at their column's width and are realized over the following turns; set once.
    std::size_t realizePerFrame = 240;
};

/// @brief Which of a table's columns show, in which order, how many are pinned, and the widths the user set.
struct TableArrangement {
    /// @brief Indices into `Table::columns`, in layout order; empty shows every column in its own order.
    std::vector<std::size_t> visible;
    /// @brief How many of the leading visible columns stay in view whatever the horizontal scroll.
    std::size_t pinned = 0;
    /// @brief Widths the user set, in `Sizing` units, by index into `Table::columns`; measurement does not change such
    ///        a width.
    std::map<std::size_t, int> widths;

    /// @brief Member-wise equality.
    /// @param other The arrangement to compare with.
    /// @return `true` when every member matches.
    [[nodiscard]] bool operator==(TableArrangement const& other) const = default;
};

/// @brief One part of a header band as the backend draws it: a run of the band's columns that stand together in the
///        layout, on one side of the pinned edge.
struct BandRun {
    /// @brief The band's level, 0 for the outermost.
    std::size_t level = 0;
    /// @brief The band's text.
    std::string label;
    /// @brief The unit shown after the label; empty for none.
    std::string unit;
    /// @brief The visible position of the run's first column (not an index into `Table::columns`).
    std::size_t first = 0;
    /// @brief How many visible columns the run covers, from `first`.
    std::size_t count = 0;
    /// @brief Whether the run lies in the pinned part.
    bool pinned = false;

    /// @brief Member-wise equality.
    /// @param other The run to compare with.
    /// @return `true` when every member matches.
    [[nodiscard]] bool operator==(BandRun const& other) const = default;
};

/// @brief The row a linked chart highlights: its key, and the view row the server has it at.
struct TableHighlight {
    /// @brief The row's key.
    Key key;
    /// @brief Its view row when the highlight was set.
    std::size_t row = 0;

    /// @brief Member-wise equality.
    /// @param other The highlight to compare with.
    /// @return `true` when key and row match.
    [[nodiscard]] bool operator==(TableHighlight const& other) const = default;
};

/// @brief A cell by its row's key and its column's id.
struct CellRef {
    /// @brief The row's key.
    Key key;
    /// @brief The column's id (`TableColumn::id`).
    std::string column;

    /// @brief Member-wise equality.
    /// @param other The cell to compare with.
    /// @return `true` when key and column match.
    [[nodiscard]] bool operator==(CellRef const& other) const = default;
};

/// @brief A rectangle of cells by its two corners: the rows between the corners' keys' view positions and the columns
///        between their ids' places in the layout, both ends included. A sort or a column move keeps the corners, so
///        the rectangle they span may change, as a spreadsheet's does.
struct CellRange {
    /// @brief The corner the selection started from.
    CellRef anchor;
    /// @brief The corner it was extended to.
    CellRef focus;

    /// @brief Member-wise equality.
    /// @param other The range to compare with.
    /// @return `true` when both corners match.
    [[nodiscard]] bool operator==(CellRange const& other) const = default;
};

/// @brief A grid's focused cell as the mount shows it: a row by key, or by view position while the row there is not
///        fetched yet, and a column by id.
struct GridFocus {
    /// @brief The row's key; empty while the row at `viewRow` is not fetched yet (a placeholder).
    std::optional<Key> key;
    /// @brief The row's view position: read only while `key` is empty.
    std::size_t viewRow = 0;
    /// @brief The column's id (`TableColumn::id`).
    std::string column;

    /// @brief Member-wise equality.
    /// @param other The focus to compare with.
    /// @return `true` when key, view row and column match.
    [[nodiscard]] bool operator==(GridFocus const& other) const = default;
};

/// @brief What a table's key handler did with a chord.
enum class KeyOutcome : std::uint8_t {
    Handled,        ///< The table took the key.
    Ignored,        ///< The table did not take it.
    FocusNext,      ///< Focus leaves the table for the next control of the focus chain.
    FocusPrevious,  ///< Focus leaves the table for the previous control of the focus chain.
};

/// @brief One widget per row of a keyed collection, updated in place when a row with the same key changes.
struct ForEach {
    /// @brief The rows, the key function and the row view, type-erased; null shows no rows.
    std::shared_ptr<detail::ForEachModel const> model;
    /// @brief The direction rows are stacked in; set once.
    Axis axis = Axis::Vertical;
    /// @brief Space between rows: a spacing step, 0 to `kMaxSpacingStep`.
    int gap = 0;
    /// @brief Visibility, enablement, layout, drag-and-drop of the row container.
    Common common{};
    /// @brief Whether rows that do not fit on one line continue on the next; set once. Only a horizontal `axis`
    ///        wraps: the mount ignores it on a vertical one.
    bool wrap = false;
    /// @brief Whether a rule is drawn between each two shown rows, as a Column's `divided`; set once. Only a vertical
    ///        `axis` divides: the mount ignores it on a horizontal one.
    bool divided = false;
};

class TableRows;

/// @brief Whether a cell's value is here, for a table mounted over a `TableSource`: read from the row's
///        `TableRows::unloadedLevels` and the level of the cell's column.
enum class CellLoad : std::uint8_t {
    Loaded,    ///< The value is here, or the column has no level.
    Unloaded,  ///< The column's level has not loaded for the row: a placeholder of a fixed width.
    Failed,    ///< The column's level failed to load for the row: an error mark of the same width.
};

/// @brief What a table cell shows beside its text: its tone, its tooltip, and whether its value could not be read.
struct CellAccent {
    /// @brief The cell's tone: the theme's tone surface under its selection and focus marks; disengaged for none.
    std::optional<Tone> tone;
    /// @brief The cell's tooltip, and its accessible description; empty for none.
    std::string tooltip;
    /// @brief Whether the value could not be read as the column's kind: the backend draws its error mark in place of
    ///        the text, and `tooltip` says so.
    bool invalid = false;

    /// @brief Member-wise equality.
    /// @param other The accent to compare with.
    /// @return `true` when every member matches.
    [[nodiscard]] bool operator==(CellAccent const& other) const = default;
};

/// @brief A table cell with an accent: a node a `TableSource::cellView` or `defaultCellView` returns. Mounted as a
///        cell of a source table, its content is the cell and its accent is bound in the cell's own scope and told
///        to `TableWidget::setCellAccent`; mounted anywhere else, its content is mounted and the accent is not read.
struct AccentedCell {
    /// @brief The cell's content. Must not be null.
    Node content;
    /// @brief The accent.
    Prop<CellAccent> accent;
};

/// @brief What marks one cell of an editable table: read-only, changed since the record was loaded, not encoding,
/// being
///        saved, or being recomputed.
struct CellMarks {
    /// @brief The cell takes no edit: drawn on the theme's read-only cell surface.
    bool readOnly = false;
    /// @brief The cell's value differs from the loaded one: the theme's modified mark.
    bool modified = false;
    /// @brief The cell's draft does not encode, or the server refused it: the theme's error mark.
    bool error = false;
    /// @brief The cell's value is being saved, or is the last one known: drawn stale.
    bool stale = false;
    /// @brief The cell's value is being recomputed: drawn muted.
    bool muted = false;

    /// @brief Member-wise equality.
    /// @param other The marks to compare with.
    /// @return `true` when every member matches.
    [[nodiscard]] bool operator==(CellMarks const& other) const = default;
};

/// @brief A table cell with marks: a node a `TableSource::cellView` returns. Mounted as a cell of a source table, its
///        content is the cell and its marks are bound in the cell's own scope and told to `TableWidget::setCellMarks`
///        on every change and whenever the cell is attached to a row; mounted anywhere else, its content is mounted
///        and the marks are not read. Its content may be an `AccentedCell`, whose accent is bound in the same scope.
struct MarkedCell {
    /// @brief The cell's content. Must not be null.
    Node content;
    /// @brief The marks.
    Prop<CellMarks> marks;
};

/// @brief What marks a whole row: it is being worked on, it is locked by someone else, or it is kept in view although
///        it no longer matches the filter. A row's marks do not vary by cell, so its row look carries them.
struct RowMarks {
    /// @brief An action of this session is working on the row.
    bool busy = false;
    /// @brief Another user holds the row; it cannot be edited here.
    bool locked = false;
    /// @brief The row is retained and the filter rejects it: it draws muted.
    bool unmatched = false;
    /// @brief What the row's tooltip says of its marks; empty for none.
    std::string tooltip;

    /// @brief Member-wise equality.
    /// @param other The marks to compare with.
    /// @return `true` when every member matches.
    [[nodiscard]] bool operator==(RowMarks const& other) const = default;
};

/// @brief Rows whose keys an action changed, for the mount to move what it holds by key: its focus and its
///        highlighted row. A generation the mount has applied is not applied again.
struct KeyMoves {
    /// @brief Increases with each set of moves; 0 holds none.
    std::uint64_t generation = 0;
    /// @brief Each row's old key, then its new one.
    std::vector<std::pair<Key, Key>> moves;

    /// @brief Member-wise equality.
    /// @param other The moves to compare with.
    /// @return `true` when generation and moves match.
    [[nodiscard]] bool operator==(KeyMoves const& other) const = default;
};

/// @brief A row's own parts beyond its cells, for a table mounted over a `TableSource`: built once per realized row
///        key, bound in the row's scope. Nothing per cell is carried here.
struct TableRowLook {
    /// @brief The row's tone, bound in the row's scope; disengaged for none.
    std::optional<Prop<std::optional<Tone>>> tone;
    /// @brief The row's marks, bound in the row's scope; disengaged for a table whose rows carry none.
    std::optional<Prop<RowMarks>> marks;
    /// @brief The row's accessible name, bound in the row's scope; disengaged names the row by its key.
    std::optional<Prop<std::string>> label;
    /// @brief Whether some cell of the row changed since the record was loaded, bound in the row's scope: the row
    ///        header's modified mark (`TableWidget::setRowModified`); disengaged for a table whose rows carry none.
    std::optional<Prop<bool>> modified;
    /// @brief The cells of this row that cover the rows below, bound in the row's scope; disengaged for none.
    std::optional<Prop<std::vector<RowSpan>>> spans;
    /// @brief Non-null: the row is a heading, this node its one cell spanning every column; no cell is built for it.
    Node heading;
    /// @brief Whether the row is a summary row: drawn on the theme's summary surface, never selected, activated,
    ///        focused for an edit or pasted into (`TableWidget::setRowSummary`). Its cells are built as any row's.
    bool summary = false;
    /// @brief What the row's parts need kept alive while the row is realized, such as the scope they are bound in;
    ///        the mount holds it with the row.
    std::shared_ptr<void const> keep;
};

/// @brief Where a table's rows come from when they come by view index: the rows, and cells as the mount builds them.
///
/// The mount realizes the rows in and near view. A realized row holds a signal the mount keeps equal to its record,
/// and one cell per column, each with a signal of its own set to the same record: the cell's view is built once from
/// it, and a change of the row re-evaluates only the cells whose record changed.
struct TableSource {
    /// @brief The rows, by view index. Must not be null.
    std::shared_ptr<TableRows> rows;
    /// @brief One cell's view for the column at an index into `Table::columns` (which holds every column and does
    ///        not change with the layout), built once per cell widget from a signal the mount sets to the record of
    ///        the row the cell shows. Must not be empty.
    std::function<Node(std::size_t column, reactive::Signal<Value> const& row)> cellView;
    /// @brief The row's own parts, built once per realized row key from a signal the mount keeps equal to its
    ///        record; empty for none.
    std::function<TableRowLook(reactive::Signal<Value> const& row)> rowView;
    /// @brief The default cell, shared by every column whose `ownCell` entry is `false`: built once per cell from a
    ///        signal holding the column it shows (an index into `Table::columns`, which the mount sets when it moves
    ///        a pooled cell to another column of the same `TextRole`) and one the mount sets to the row's record.
    ///        Empty: every column's cells come from `cellView`.
    std::function<Node(reactive::Signal<std::size_t> const& column, reactive::Signal<Value> const& row)>
        defaultCellView;
    /// @brief Per `Table::columns` index, whether the column's cells come from `cellView`, pooled per column, rather
    ///        than from `defaultCellView`, pooled per role; a missing entry reads as `true`.
    std::vector<bool> ownCell;
    /// @brief Column ids named to every `TableRows::visible` call, whatever the column window holds: what the row's
    ///        own parts read. The ids of the columns in the window, and what their cells read, are added per call.
    std::vector<std::string> alwaysFetched;
    /// @brief Per `Table::columns` index, the row fields that column's cell reads beside its own id, named to
    ///        `TableRows::visible` while the column is in the window; a missing entry reads as none.
    std::vector<std::vector<std::string>> cellReads;
    /// @brief Per `Table::columns` index, the level of that column; empty, or an empty entry: the column has none.
    std::vector<std::string> levels;
    /// @brief Whether the rows are windowed whatever their count, as a server's rows are. `false` realizes every row
    ///        while the view holds fewer than `Virtualization::threshold`, as a short table over a document's rows
    ///        does, and only then draws `TableRowLook::spans`.
    bool alwaysWindowed = true;
    /// @brief Per `Table::columns` index, whether that column's cells each show one row only: their view shares state
    ///        with the row's own parts, so when a realized row is given to another key they are destroyed and built
    ///        for it rather than rebound by their signal. Set it for a column whose cell reads state its row declares;
    ///        each such cell then costs a build where a rebind would do. A missing entry reads as `false`: the
    ///        column's cells are rebound to any row.
    std::vector<bool> rowScopedCells;
};

/// @brief A choice the status line offers beside its text, as a button the user may choose.
struct StatusEntry {
    /// @brief What the choice reports through `Table::onStatusEntry`.
    std::string id;
    /// @brief The button's text.
    std::string label;

    /// @brief Member-wise equality.
    /// @param other The entry to compare with.
    /// @return `true` when id and label match.
    [[nodiscard]] bool operator==(StatusEntry const& other) const = default;
};

/// @brief A table's status line: its counts, the state of its fetches and levels, and the latest message.
struct TableStatus {
    /// @brief The leading part: the rows counted, or empty while no total is known.
    std::string counts;
    /// @brief A failed level or fetch; empty when none.
    std::string state;
    /// @brief Whether the state offers "Retry", which calls `Table::onRetry`.
    bool retry = false;
    /// @brief The latest refusal or notice; empty when none.
    std::string message;
    /// @brief The choices the message offers, after it.
    std::vector<StatusEntry> entries;
    /// @brief What stays on the line while its cause lasts, as rows being saved; empty when nothing does.
    std::string standing;
    /// @brief The choices `standing` offers, after it.
    std::vector<StatusEntry> standingEntries;
    /// @brief Whether the line takes space.
    bool shown = false;

    /// @brief Member-wise equality.
    /// @param other The status to compare with.
    /// @return `true` when every member matches.
    [[nodiscard]] bool operator==(TableStatus const& other) const = default;
};

/// @brief One column of a `Table`.
struct TableColumn {
    /// @brief The column's id; empty for a table built from C++ with no ids.
    std::string id;
    /// @brief The header caption.
    std::string label;
    /// @brief The column's width request. A `Stretch` column starts from its content (the larger of its header and its
    ///        widest shown cell) and its weight shares only the width the content leaves; a table too narrow for the
    ///        content narrows its wrapping columns, then its `shrink` columns, last first, then the other stretch
    ///        columns. A hidden cell widens no column.
    Sizing width = Sizing::content();
    /// @brief The text style of this column's default cell; `Normal` renders unchanged. Has no effect on the style of
    ///        a column's own `.cell`, or on a `Table` constructed directly without going through the interpreter. A
    ///        `Mono` role also sets the column's width unit: a fixed width counts monospace characters.
    TextRole role = TextRole::Normal;
    /// @brief Where a cell sits across the column: the text of a text cell or a text input cell, and a label-width
    ///        cell (a badge, a check box, a push button) at its natural width, at the start, centre or end; the
    ///        header label follows it. A container cell fills the column whatever this says. `Stretch` reads as
    ///        `Start`.
    Align align = Align::Start;
    /// @brief Where a cell sits down its row when the row is taller than the cell: `Start` at the top, `Center`, or
    ///        `End` at the bottom. `Stretch` fills the row's height.
    Align valign = Align::Center;
    /// @brief Whether a click on the column's header asks to sort by it (`Table::onSort`).
    bool sortable = false;
    /// @brief The unit the column's values are in, which the header shows after the label (`headerText`) and names
    ///        in its accessible name (`headerAccessibleName`); empty for none.
    std::string unit;
    /// @brief Whether the column's header draws a filter button (`Table::onFilter`), filled while `Table::filtered`
    ///        names the column.
    bool filterable = false;
    /// @brief Whether the column's text cells may break across lines. With `false` each is drawn on one line and cut
    ///        at the column's width with an ellipsis, such as a value with its unit in a narrow column.
    bool wrap = true;
    /// @brief Whether a `Stretch` column may be narrower than its header when the table is narrower than its columns
    ///        need: it then gives up width down to a small floor and cuts its header and cells at its edge (an
    ///        ellipsis where the backend draws one), where otherwise no column is narrower than its header and the
    ///        table scrolls sideways instead. A `shrink` column also gives up its width before the other stretch
    ///        columns do. It reads as `false` on a column that does not stretch.
    bool shrink = false;
};

/// @brief Keyed rows of cells under column headers. Each row's view is a `Row` whose children are the cells, one
///        per column.
struct Table {
    /// @brief The columns, in order; set once.
    std::vector<TableColumn> columns;
    /// @brief The rows, the key function and the row view, type-erased; null shows no rows.
    std::shared_ptr<detail::ForEachModel const> rows;
    /// @brief Whether the header row is drawn; set once. `false` suits several tables sharing one header, or a
    ///        header a document draws itself.
    bool showHeader = true;
    /// @brief The row count and margin above which a row's widget is realized only near the visible range; set
    ///        once. A row's own reactive state is unaffected either way. Only a backend that implements
    ///        `TableWidget::setOnVisibleRangeChanged` ever dewindows a row; every other backend realizes every
    ///        row regardless of this setting, exactly as before it existed.
    Virtualization virtualize{};
    /// @brief How many rows the user can select; set once.
    SelectionMode selectionMode = SelectionMode::None;
    /// @brief The selected rows' keys. A key with no row selects nothing until such a row appears.
    Prop<std::vector<Key>> selection;
    /// @brief Called with the keys the user selected and what kind of change it was.
    std::function<void(std::vector<Key>, SelectionChange)> onSelectionChange;
    /// @brief Whether the table draws its selection as a leading column of check boxes, one per row, under a box
    ///        that selects every row shown; set once. Only a `Multiple` table draws it.
    bool selectColumn = false;
    /// @brief The accessible name of the select column's header box.
    Prop<std::string> selectAllLabel = std::string{"Select all"};
    /// @brief The sort chain the headers show, most significant key first; empty marks no header. The table never
    ///        orders its rows itself: they come in the order shown.
    Prop<std::vector<TableSort>> sort;
    /// @brief Called with a sortable column's index when the user asks to sort by it (a click on its header, or
    ///        `Enter` or `Space` on it), and whether `Shift` was held.
    std::function<void(std::size_t, HeaderClick)> onSort;
    /// @brief The entries of a header's menu, given its column (an index into `columns`), or of the table's menu,
    ///        given none. Empty: no header or table menu.
    std::function<std::vector<MenuEntry>(std::optional<std::size_t>)> menu;
    /// @brief Called with the menu's column (none for the table's menu) and the chosen entry's path, an index per
    ///        level from the top.
    std::function<void(std::optional<std::size_t>, std::vector<std::size_t>)> onMenu;
    /// @brief Called when the user moves a column's header (a drag, or `Ctrl+Shift+Left` or `Right`), with the
    ///        column (an index into `columns`) and its new place among the arrangement's visible columns.
    std::function<void(std::size_t column, std::size_t position)> onColumnMove;
    /// @brief Called when the user drags a header's trailing edge, with the column and its new width in `Sizing`
    ///        units, or none for a double click on the edge, which gives the column its declared width again.
    ///        Empty: the edges cannot be dragged.
    std::function<void(std::size_t column, std::optional<int> units)> onColumnResize;
    /// @brief Called with a filterable column's index when the user presses its header's filter button.
    std::function<void(std::size_t column)> onFilter;
    /// @brief The columns that have a filter, by index into `columns`: their filter buttons are drawn filled and
    ///        their headers named "{label}, filtered".
    Prop<std::vector<std::size_t>> filtered;
    /// @brief The column groups whose header row is drawn over their columns; empty draws no group row.
    Prop<std::vector<TableColumnGroup>> columnGroups;
    /// @brief Called with a group's index into `columnGroups` when the user presses its filter button.
    std::function<void(std::size_t group)> onGroupFilter;
    /// @brief The header bands over the columns, outermost first, nested at most `table::kMaxBandLevels` levels; set
    ///        once. The mount draws each over the runs of its columns the column window shows (`bandRuns`); empty
    ///        draws none. A table over a `TableSource` only.
    std::vector<table::BandInfo> bands;
    /// @brief Called when the user asks to copy (`Ctrl+C` on the focused table).
    std::function<void()> onCopy;
    /// @brief Called with a row's key when the user activates it (Enter or a double click).
    std::function<void(Key)> onActivate;
    /// @brief Visibility, enablement, layout, drag-and-drop.
    Common common{};
    /// @brief When set, `rows` is ignored and rows come by view index from the source; set once.
    std::optional<TableSource> source;
    /// @brief The status line; the default takes no space.
    Prop<TableStatus> status;
    /// @brief Called when the user chooses the status line's "Retry".
    std::function<void()> onRetry;
    /// @brief Which columns show, their order, the pinned count and the user's widths; a table over a `TableSource`
    ///        only. The default shows every column, none pinned.
    Prop<TableArrangement> arrangement;
    /// @brief The row a linked chart highlights, set once a keyed reply has given its view row: the mount scrolls to
    ///        it, marks it and follows its key through later changes of the rows. Disengaged: no mark. A table over a
    ///        `TableSource` only; it never changes the selection or the keyboard focus.
    Prop<std::optional<TableHighlight>> highlight;
    /// @brief Called with a range gesture the backend cannot resolve itself, as the view rows of its anchor and its
    ///        end and the kind of change; a table over a `TableSource` only.
    std::function<void(std::size_t anchorRow, std::size_t focusRow, SelectionChange change)> onSelectRange;
    /// @brief The focused cell, which the mount keeps realized whatever the windows; disengaged leaves focus where the
    ///        user put it. A table over a `TableSource` only, focusing a row the mount has realized.
    Prop<std::optional<CellRef>> focus;
    /// @brief Called with the cell the user's keys moved focus to, once its row is fetched.
    std::function<void(std::optional<CellRef>)> onFocusCell;
    /// @brief Called, in place of `onSelectionChange`, when the user asks a `Multiple` table to select every row
    ///        (`Ctrl+A`): the handler selects the whole view, rows the backend does not hold included. Empty: the
    ///        backend selects the rows it holds and reports them through `onSelectionChange`.
    std::function<void()> onSelectAll;
    /// @brief The entries of a cell's menu, given its row's key and its column (an index into `columns`), asked for
    ///        each time the menu opens (a right click, the menu key or a long press on the cell), which focuses the
    ///        cell, and for a key of `cellMenuKeys` the user presses while the table has the focus, for the focused
    ///        cell. The mount reads each entry's bound parts once, untracked, and runs the chosen entry's `onSelect`.
    ///        Empty: no cell menu.
    std::function<std::vector<MenuItem>(Key const& row, std::size_t column)> cellMenu;
    /// @brief The chords `cellMenu`'s entries may declare in `keys`: while the table has the focus, a key that is one
    ///        of them asks for the focused cell's entries and runs the one that declares it; any other key never asks.
    std::vector<std::string> cellMenuKeys;
    /// @brief The caller's entries of a header's menu, given its column, read when the menu opens: shown after
    ///        `menu`'s own entries for that column and a separator, and run by the mount when chosen. An entry hidden
    ///        by its `visible` is left out, and with no entry shown nothing is added. Empty: `menu`'s entries alone.
    std::function<std::vector<MenuItem>(std::size_t column)> headerEntries;
    /// @brief Whether the header's sort, filter and arrangement gestures are offered, read each time a header's menu
    ///        opens: while false, every entry `menu` gives for a column is disabled. The gestures' handlers are still
    ///        called, for the caller to refuse; the table's own menu (`menu` with no column) and `headerEntries` are
    ///        left as given.
    Prop<bool> headerEnabled = true;
    /// @brief Engaged: the selection is every row of the view less these keys, which the mount gives the backend
    ///        through `setSelectionView` on each change; when it disengages the mount sets `selection` again.
    Prop<std::optional<std::vector<Key>>> selectionView;
    /// @brief Rows an action gave new keys: the mount's focus and highlighted row follow a row from its old key to
    ///        its new one, and focus the change that removed the old key already moved to the row at its index is
    ///        taken back to the new key. A table over a `TableSource` only.
    Prop<KeyMoves> keyMoves;
    /// @brief Called with the id of the status line's entry the user chose.
    std::function<void(std::string entryId)> onStatusEntry;
    /// @brief Engaged: the table's focus is a grid's. The mount shows the cell it names, at its key's view position
    ///        now, or at `GridFocus::viewRow` while it names no key, and keeps that row realized, fetched or not; a
    ///        click or a `FocusMove` goes to `onGridFocus` and does not move the mount's own focus. A table over a
    ///        `TableSource` only.
    std::optional<Prop<std::optional<GridFocus>>> gridFocus;
    /// @brief Called, while `gridFocus` is engaged, with the cell a click or a `FocusMove` resolves to; its key is
    ///        empty when that row is not fetched yet.
    std::function<void(GridFocus)> onGridFocus;
    /// @brief Called with the cell the user double-clicked.
    std::function<void(CellRef)> onActivateCell;
    /// @brief Called with the cell the user clicked once, after the click moved focus (a fetched row's only).
    std::function<void(CellRef)> onClickCell;
    /// @brief Called when focus leaves the table's widget and everything inside it, an open editor included.
    std::function<void()> onFocusOut;
    /// @brief Engaged: the cell whose editor is open, or none. While it names a cell, the mount builds `editor` for
    ///        it once, mounts it in that cell's row, draws it over the cell and focuses it; a change unmounts it. A
    ///        table over a `TableSource` only.
    std::optional<Prop<std::optional<CellRef>>> editing;
    /// @brief Builds the editor of the cell `editing` names.
    std::function<Node(CellRef const&)> editor;
    /// @brief Called with each chord of `keyChords` pressed while focus is in the table; the outcome says whether the
    ///        table took it, or whether focus leaves the table along the focus chain.
    std::function<KeyOutcome(std::string const& chord)> onKey;
    /// @brief The chords `onKey` takes, declared to the backend again on each change.
    Prop<std::vector<std::string>> keyChords;
    /// @brief Called with printable text typed while a cell has focus and no editor is open.
    std::function<void(std::string)> onText;
    /// @brief Engaged: the table selects cells, not rows, and this is the selected rectangle, or none. The mount shows
    ///        it at its corners' view positions and visible places now, so a sort or a column move redraws the
    ///        rectangle the same corners span; a corner whose row is not in the view, or whose column is hidden, shows
    ///        none, and the range is kept. The backend draws no row selection and reports no `onSelectionChange`. A
    ///        table over a `TableSource` only.
    std::optional<Prop<std::optional<CellRange>>> cellSelection;
    /// @brief Called, while `cellSelection` is engaged, with the rectangle the user selected: a drag from cell to
    /// cell,
    ///        a `Shift`+click, a row header's click or a column header's selection gesture.
    std::function<void(std::optional<CellRange>)> onCellSelect;
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
    /// @brief The field's label: its visible caption while `showLabel`; a form's cell draws it otherwise, beside
    ///        the input. Empty: none.
    Prop<std::string> label;
    /// @brief Whether the input draws `label` as a visible caption itself; set once. A form leaves it false, since
    ///        its cell draws the caption.
    bool showLabel = false;
    /// @brief Read-only, required, errors and stale.
    FieldState field{};
    /// @brief Visibility, enablement, layout, drag-and-drop.
    Common common{};
    /// @brief The pattern the value is shown and read in (`checkDatePattern`), ISO being always read too; empty
    ///        means ISO.
    Prop<std::string> pattern;
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
    /// @brief Called with the value a finished move ended on: a release after a drag, a key step, or a wheel step.
    std::function<void(std::int64_t)> onCommit;
    /// @brief Read-only, required, errors and stale.
    FieldState field{};
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
    /// @brief Read-only, required, errors and stale.
    FieldState field{};
    /// @brief Visibility, enablement, layout, drag-and-drop.
    Common common{};
};

/// @brief A button inside another kind, such as a banner's action: its caption beside its handler.
struct ActionButton {
    /// @brief The caption; empty shows no button.
    Prop<std::string> label;
    /// @brief Called when the button is activated.
    Action onClick;
};

/// @brief A message across the top of a region, with an optional action and an optional dismissal.
struct Banner {
    /// @brief What the message conveys.
    Prop<Tone> tone = Tone::Info;
    /// @brief The message; not applied while the banner has spans.
    Prop<std::string> text;
    /// @brief The message as runs, each plain, strong or monospace; while bound or not empty, it is shown in place
    ///        of `text`.
    Prop<std::vector<TextSpan>> spans;
    /// @brief The button beside the message; an empty label shows none.
    ActionButton action{};
    /// @brief Whether the user can dismiss it; set once.
    bool dismissible = false;
    /// @brief Called when the user dismisses it. The banner stays shown until the application hides it.
    Action onDismiss;
    /// @brief Visibility, enablement, layout, drag-and-drop.
    Common common{};
    /// @brief How the action is drawn; set once. `Link` draws it inside the message, at the run of `spans` marked
    ///        `action`, else after the message.
    BannerActionStyle actionStyle = BannerActionStyle::Ghost;
    /// @brief Whether the action takes a line of its own under the message, as wide as the banner; set once. Not with
    ///        `Link`.
    bool actionStretch = false;
};

/// @brief A short label marking a state, such as a count or a status.
struct Badge {
    /// @brief What the mark conveys.
    Prop<Tone> tone = Tone::Neutral;
    /// @brief The label.
    Prop<std::string> text;
    /// @brief The name of an icon in the theme's icon map; empty shows none. Set once.
    std::string icon;
    /// @brief Filled or outlined; set once.
    BadgeStyle style = BadgeStyle::Filled;
    /// @brief Visibility, enablement, layout, drag-and-drop.
    Common common{};
    /// @brief Called when the user removes the badge; empty: the badge has no remove control and takes no focus.
    ///        Set once.
    Action onRemove;
    /// @brief The remove control's accessible name; empty: `Remove ` and the badge's text.
    Prop<std::string> removeLabel;
    /// @brief Its outline: the theme's corners, or round; set once.
    ControlShape shape = ControlShape::Default;
    /// @brief Whether the label is drawn in the `mono` text role's face; set once.
    bool mono = false;
};

/// @brief A progress bar.
struct Progress {
    /// @brief How far along, from 0 to 1; a value outside is shown at the nearer end. `nullopt`: indeterminate.
    Prop<std::optional<double>> value;
    /// @brief A caption beside the bar.
    Prop<std::string> label;
    /// @brief Visibility, enablement, layout, drag-and-drop.
    Common common{};
};

/// @brief One step of a `Steps` indicator.
struct Step {
    /// @brief The caption.
    std::string label;
    /// @brief Where it stands.
    StepState state = StepState::Pending;
    /// @brief A muted second line, who and when; empty draws none. Qt Quick draws it under the label, cut at the
    ///        label's width; the terminal draws it after the label in brackets, and drops it from a row too narrow
    ///        for both.
    std::string detail;

    /// @brief Memberwise equality.
    /// @return Whether label, state and detail match.
    bool operator==(Step const&) const = default;
};

/// @brief The steps of a process in order, each with its state.
struct Steps {
    /// @brief The steps.
    Prop<std::vector<Step>> items;
    /// @brief The step to highlight, by position; `nullopt` or a position past the last highlights none.
    Prop<std::optional<std::size_t>> current;
    /// @brief Whether the indicator draws the steps' labels; unlabelled, it draws only the marks and connectors
    ///        and no detail. Labelled or not, a step's accessible name is its label, its detail when nonempty and its
    ///        state when it has one, joined by `, `.
    Prop<bool> labelled = true;
    /// @brief Visibility, enablement, layout, drag-and-drop.
    Common common{};
};

/// @brief One row of a `KeyValue` list.
struct KeyValueItem {
    /// @brief The caption.
    std::string label;
    /// @brief The value, as text; not shown while the item has spans.
    std::string value;
    /// @brief The value as runs, as a `Text`'s spans; while not empty, shown in place of `value`.
    std::vector<TextSpan> spans;
    /// @brief An evaluation of the value, drawn in the tone's colour and the strong weight; `Neutral` is none.
    Tone tone = Tone::Neutral;

    /// @brief Memberwise equality.
    /// @return Whether every member matches.
    bool operator==(KeyValueItem const&) const = default;
};

/// @brief One point of a chart's series.
struct ChartPoint {
    /// @brief The position along the horizontal axis: the point's index, a number, or an instant's milliseconds
    ///        since the epoch.
    double x = 0.0;
    /// @brief The value, or the axis end a clipped point is drawn at; `nullopt` is a gap, which no line joins
    ///        across.
    std::optional<double> y;
    /// @brief Whether the value lies outside the axis the document fixed.
    ChartClip clip = ChartClip::None;
    /// @brief The point's label on the horizontal axis; empty for none.
    std::string xLabel;
    /// @brief Engaged: the point is marked, in this tone.
    std::optional<Tone> tone;
    /// @brief Text drawn beside the point; empty for none.
    std::string note;

    /// @brief Memberwise equality.
    /// @return Whether every member matches.
    bool operator==(ChartPoint const&) const = default;
};

/// @brief One series of a chart: its points in order.
struct ChartSeries {
    /// @brief The series' name in the legend; empty for a chart's one series.
    std::string label;
    /// @brief Whether a line joins the points; a `Bars` series is never joined and a frontend reads this for `Line`
    ///        only.
    bool joined = true;
    /// @brief Whether the series is drawn as a line of points or as bars.
    ChartStyle style = ChartStyle::Line;
    /// @brief The points, in order.
    std::vector<ChartPoint> points;

    /// @brief Memberwise equality.
    /// @return Whether every member matches.
    bool operator==(ChartSeries const&) const = default;
};

/// @brief A horizontal reference line of a chart, such as a control or a specification limit.
struct ChartLine {
    /// @brief Where the line is drawn on the value axis.
    double value = 0.0;
    /// @brief Its label.
    std::string label;
    /// @brief Its colour.
    Tone tone = Tone::Neutral;
    /// @brief Its stroke.
    ChartLineStyle style = ChartLineStyle::Solid;

    /// @brief Memberwise equality.
    /// @return Whether every member matches.
    bool operator==(ChartLine const&) const = default;
};

/// @brief One labelled tick of a chart's value axis.
struct ChartTick {
    /// @brief Where it is on the value axis.
    double value = 0.0;
    /// @brief Its label, formatted in the active locale.
    std::string text;

    /// @brief Memberwise equality.
    /// @return Whether both members match.
    bool operator==(ChartTick const&) const = default;
};

/// @brief Everything a chart draws, computed from exact values by the lowering: positions as doubles, and every
///        text already formatted.
///
/// One value rather than a setter per part: the range depends on every point and line, so the parts change
/// together, and one equality check keeps an unchanged chart from drawing again.
struct ChartData {
    /// @brief The left end of the horizontal axis, padding included.
    double xMin = 0.0;
    /// @brief The right end of the horizontal axis, padding included.
    double xMax = 0.0;
    /// @brief The lower end of the value axis, padding included.
    double yMin = 0.0;
    /// @brief The upper end of the value axis, padding included.
    double yMax = 0.0;
    /// @brief The width, in horizontal-axis units, of the room one position gives its bars; 0 when no series is
    ///        `Bars`.
    double barSlot = 0.0;
    /// @brief The value axis's unit or name, drawn above its labels; empty for none.
    std::string yLabel;
    /// @brief The value axis's ticks; empty when three or more reference lines label the axis.
    std::vector<ChartTick> yTicks;
    /// @brief The reference lines.
    std::vector<ChartLine> lines;
    /// @brief The series, in drawing order.
    std::vector<ChartSeries> series;
    /// @brief The text alternative: what a reader must know from the chart.
    std::string summary;

    /// @brief Memberwise equality.
    /// @return Whether every member matches.
    bool operator==(ChartData const&) const = default;
};

/// @brief Points, joined or as bars, against a numeric axis, with horizontal reference lines. Takes no focus and
///        calls no handler.
struct Chart {
    /// @brief What the chart draws.
    Prop<ChartData> data;
    /// @brief Visibility, enablement, layout, drag-and-drop.
    Common common{};
};

/// @brief Where a key-value list draws each caption relative to its value.
enum class CaptionPlacement : std::uint8_t {
    Beside,  ///< Each caption beside its value, one pair per row.
    Above,   ///< Each caption above its value, the pairs one under another.
};

/// @brief Captions beside, or above, their values.
struct KeyValue {
    /// @brief The pairs, in order.
    Prop<std::vector<KeyValueItem>> items;
    /// @brief Where the captions sit; set once.
    CaptionPlacement captions = CaptionPlacement::Beside;
    /// @brief The width of the caption column in units, 1 to 64, while the captions sit beside their values; absent
    ///        sizes it to the widest caption. A caption longer than the column wraps on Qt Quick and is cut with an
    ///        ellipsis in the terminal. Set once; the mount refuses a value below 1 and a value given with
    ///        `CaptionPlacement::Above`, which has no caption column.
    std::optional<int> captionWidth;
    /// @brief Visibility, enablement, layout, drag-and-drop.
    Common common{};
};

/// @brief What a region shows when it has nothing to show: a title, an explanation and an optional action.
struct EmptyState {
    /// @brief The title.
    Prop<std::string> title;
    /// @brief The explanation.
    Prop<std::string> text;
    /// @brief The name of an icon in the theme's icon map; empty shows none. Set once.
    std::string icon;
    /// @brief The button below the text; an empty label shows none.
    ActionButton action{};
    /// @brief Visibility, enablement, layout, drag-and-drop.
    Common common{};
};

/// @brief A panel that slides in from one edge over the content: a `Dialog` with a side, and the same rules.
struct Drawer {
    /// @brief Whether it is shown.
    Prop<bool> open;
    /// @brief The title.
    Prop<std::string> title;
    /// @brief A line under the title, as a dialog's.
    Prop<std::string> description;
    /// @brief The content; may be null.
    Node child;
    /// @brief Called when the user dismisses it (Esc on the TUI).
    Action onDismiss;
    /// @brief The edge it slides in from; set once.
    Side side = Side::End;
    /// @brief Visibility, enablement, layout, drag-and-drop.
    Common common{};
    /// @brief The description as runs, as a dialog's.
    Prop<std::vector<TextSpan>> descriptionSpans;
    /// @brief A short muted note on the title's row, as a dialog's.
    Prop<std::string> headNote;
    /// @brief A short muted note leading in the footer's row, as a dialog's.
    Prop<std::string> footerNote;
    /// @brief Whether a dismiss control is drawn on the title's row, as a dialog's.
    Prop<bool> closeButton = false;
    /// @brief The label of the dismiss control `closeButton` draws.
    Prop<std::string> dismissLabel = std::string{"Close"};
};

/// @brief A row of buttons: the actions of what holds it. Inside the content of a dialog or drawer, at any depth, it
///        is the overlay's footer, as a document's overlay `actions` lower to; anywhere else it is a row in the place
///        it is written. Either way the buttons are the theme's `actions.gap` apart, and so are the rows of a
///        horizontal `ForEach` that is a direct child: they are part of the row, whatever `ForEach::gap` says.
struct ActionBar {
    /// @brief The buttons, in order; a null child is skipped.
    std::vector<Node> children;
    /// @brief Where the buttons sit across the row; `Stretch` fills the row's height.
    Align align = Align::Stretch;
    /// @brief Where the spare width goes while no shown button stretches.
    Justify justify = Justify::Start;
    /// @brief Whether buttons that do not fit on one line continue on the next, as a Row's `wrap`; set once. A part
    ///        with no width takes no gap. Refused at mount in an overlay's footer: the footer sizes the row to its
    ///        one-line width, so it would never wrap there.
    bool wrap = false;
    /// @brief Visibility, enablement, layout, drag-and-drop of the row.
    Common common{};
};

/// @brief Panes side by side, or stacked, with handles the user drags to share the space between them.
struct Splitter {
    /// @brief The panes, in order; a null child is skipped.
    std::vector<Node> children;
    /// @brief The axis the panes are laid out along; set once.
    Axis orientation = Axis::Horizontal;
    /// @brief Each pane's size, in backend units, in order; empty shares the space equally. A list whose length
    ///        differs from the number of panes is shared out as an empty one.
    Prop<std::vector<int>> sizes;
    /// @brief Called with every pane's size after the user dragged a handle.
    std::function<void(std::vector<int>)> onResize;
    /// @brief Visibility, enablement, layout, drag-and-drop.
    Common common{};
};

/// @brief A titled section the user opens and closes. Its header stays usable while it is closed.
struct Collapsible {
    /// @brief The title.
    Prop<std::string> title;
    /// @brief Whether the content is shown.
    Prop<bool> open = true;
    /// @brief Called with the state the user asked for.
    std::function<void(bool)> onToggle;
    /// @brief Shown beside the title, open or closed; may be null.
    Node header;
    /// @brief The content; may be null. Mounted once, and hidden while closed.
    Node child;
    /// @brief Visibility, enablement, layout, drag-and-drop.
    Common common{};
};

/// @brief A region that takes files dropped from the operating system.
struct DropZone {
    /// @brief The extensions it takes, such as `".csv"`, compared without regard to case; empty takes every file.
    ///        Set once.
    std::vector<std::string> accept;
    /// @brief Whether a drop may carry more than one file; set once.
    bool multiple = false;
    /// @brief Called with the paths of a drop it took. A drop with a file outside `accept`, or with several files
    ///        when not `multiple`, is refused whole.
    std::function<void(std::vector<std::string>)> onDrop;
    /// @brief The content shown inside the region; may be null.
    Node child;
    /// @brief Visibility, enablement, layout, drag-and-drop.
    Common common{};
};

/// @brief A region of a page: a rectangle of the sheet that holds its children as a vertical stack. Only a `Page`
///        holds one. Its `layout` hints are refused: the geometry names its size.
struct PageRegion {
    /// @brief Where on the sheet the region lies, in millimetres; set once.
    RegionGeometry geometry{};
    /// @brief The content, top to bottom; a null child is skipped.
    std::vector<Node> children;
    /// @brief Visibility, enablement, accessibility, test identity and chords.
    Common common{};
};

/// @brief A sheet of fixed physical size, drawn on the `paper` surface and displayed scaled down, never up, to the
///        width its container gives it. Its children are `PageRegion` nodes and nothing else. Its `layout` hints are
///        refused except `alignSelf`: the sheet's size is its own.
struct Page {
    /// @brief The sheet's size and margins, in millimetres; set once.
    PageGeometry geometry{};
    /// @brief The regions, in the order they are drawn and read; each is a `PageRegion` node.
    std::vector<Node> children;
    /// @brief Visibility, enablement, accessibility, test identity and chords.
    Common common{};
};

/// @brief One prop a loaded custom component is given.
struct CustomProp {
    /// @brief The prop's name, as the component declares it.
    std::string name;
    /// @brief Its value: text for `string`, `int`, `exact` and `json`, which cross as text so that no value loses
    ///        precision; a `bool`; or a `number`.
    std::variant<Prop<std::string>, Prop<bool>, Prop<double>> value;
};

/// @brief A component the backend may load (spec 5 §10), with the built-in nodes shown when it does not.
///
/// `Mounted` asks `IViewBackend::createCustom` first and mounts the fallback only when the backend loads nothing.
/// The fallback carries its own `Common`.
struct Custom {
    /// @brief The component's name, as the document's `components` declares it.
    std::string name;
    /// @brief The built-in nodes shown in its place; never null.
    Node fallback;
    /// @brief The props a loaded component is given, bound as any other property is.
    std::vector<CustomProp> props;
    /// @brief The lowest version of a host component that will do.
    int minVersion = 0;
    /// @brief Called with an event a loaded component raises: its name and its payload as the backend reports it.
    std::function<void(std::string event, std::string payload)> onEvent;
};

/// @brief What a `Node` points at: exactly one node kind.
struct NodeData {
    /// @brief The node.
    std::variant<Text, Button, TextInput, Checkbox, Select, Menu, Column, Row, Grid, Spacer, Panel, Scroll, Switch,
                 Tabs, Dialog, Busy, ForEach, Table, DateTimeInput, Slider, FilePicker, Banner, Badge, Progress, Steps,
                 KeyValue, EmptyState, Drawer, Splitter, Collapsible, DropZone, Custom, ActionBar, Chart, Page,
                 PageRegion, AccentedCell, MarkedCell>
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

/// @brief A table cell with an accent.
/// @param content The cell's content; must not be null.
/// @param accent  The accent.
/// @return The node.
/// @throws std::invalid_argument when @p content is null.
[[nodiscard]] inline Node accentedCell(Node content, Prop<CellAccent> accent) {
    if (content == nullptr) {
        throw std::invalid_argument{"ui::accentedCell: the content must not be null"};
    }
    return detail::makeNode(AccentedCell{.content = std::move(content), .accent = std::move(accent)});
}

/// @brief A table cell with marks.
/// @param content The cell's content; must not be null.
/// @param marks   The marks.
/// @return The node.
/// @throws std::invalid_argument when @p content is null.
[[nodiscard]] inline Node markedCell(Node content, Prop<CellMarks> marks) {
    if (content == nullptr) {
        throw std::invalid_argument{"ui::markedCell: the content must not be null"};
    }
    return detail::makeNode(MarkedCell{.content = std::move(content), .marks = std::move(marks)});
}

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

/// @brief The layout of a table inside the column that takes its place, which holds the table's own layout. The table
///        fills the column across, and along it unless its height is fixed or a stretch: a share of a row is the
///        column's to take (a column's child takes none), and a row offers to fill its height to the column, not to
///        the table inside it, so a content height stretches within the column, which is as tall as the table is or as
///        its row makes it. Placement in the parent (`alignSelf`, `bleed`) is the column's alone.
/// @param placed The table's own layout.
/// @return The layout the table takes inside the column.
[[nodiscard]] inline LayoutHints wrappedTableLayout(LayoutHints placed) {
    if (placed.width.kind == Sizing::Kind::Percent) {
        placed.width = Sizing::stretch(1);
    }
    if (placed.height.kind == Sizing::Kind::Content) {
        placed.height = Sizing::stretch(1);
    }
    placed.alignSelf.reset();
    placed.bleed = false;
    return placed;
}

/// @brief A table with other nodes under it, in a column that takes the table's place: the column takes the table's
///        visibility and layout, the table `wrappedTableLayout` within it, and every other common property stays on
///        the table alone, so its test id and the element id a focus request finds name the table. The column has no
///        gap, so a node that draws nothing in place (a dialog) adds no space.
/// @param placed The table's common properties; its layout becomes the one inside the column.
/// @param beside The nodes under the table, in order.
/// @param table  Makes the table, after @p placed is changed.
/// @return The column.
[[nodiscard]] inline Node besideTable(Common& placed, std::vector<Node> beside, std::function<Node()> const& table) {
    Column wrapper;
    wrapper.common.visible = placed.visible;
    wrapper.common.layout = placed.layout;
    placed.layout = wrappedTableLayout(placed.layout);
    wrapper.children.push_back(table());
    for (Node& node : beside) {
        wrapper.children.push_back(std::move(node));
    }
    return column(std::move(wrapper));
}

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

/// @brief A message across the top of a region.
/// @param spec The node.
/// @return The node.
[[nodiscard]] inline Node banner(Banner spec) { return detail::makeNode(std::move(spec)); }

/// @brief A short label marking a state.
/// @param spec The node.
/// @return The node.
[[nodiscard]] inline Node badge(Badge spec) { return detail::makeNode(std::move(spec)); }

/// @brief A progress bar.
/// @param spec The node.
/// @return The node.
[[nodiscard]] inline Node progress(Progress spec) { return detail::makeNode(std::move(spec)); }

/// @brief The steps of a process.
/// @param spec The node.
/// @return The node.
[[nodiscard]] inline Node steps(Steps spec) { return detail::makeNode(std::move(spec)); }

/// @brief Points against a numeric axis, with reference lines.
/// @param spec The node.
/// @return The node.
[[nodiscard]] inline Node chart(Chart spec) { return detail::makeNode(std::move(spec)); }

/// @brief Captions beside their values.
/// @param spec The node.
/// @return The node.
[[nodiscard]] inline Node keyValue(KeyValue spec) { return detail::makeNode(std::move(spec)); }

/// @brief What a region shows when it has nothing to show.
/// @param spec The node.
/// @return The node.
[[nodiscard]] inline Node emptyState(EmptyState spec) { return detail::makeNode(std::move(spec)); }

/// @brief A panel that slides in from one edge.
/// @param spec The node.
/// @return The node.
[[nodiscard]] inline Node drawer(Drawer spec) { return detail::makeNode(std::move(spec)); }

/// @brief An action bar node.
/// @param spec The bar.
/// @return The node.
[[nodiscard]] inline Node actionBar(ActionBar spec) { return detail::makeNode(std::move(spec)); }

/// @brief Panes with handles the user drags.
/// @param spec The node.
/// @return The node.
[[nodiscard]] inline Node splitter(Splitter spec) { return detail::makeNode(std::move(spec)); }

/// @brief A titled section the user opens and closes.
/// @param spec The node.
/// @return The node.
[[nodiscard]] inline Node collapsible(Collapsible spec) { return detail::makeNode(std::move(spec)); }

/// @brief A region that takes dropped files.
/// @param spec The node.
/// @return The node.
[[nodiscard]] inline Node dropZone(DropZone spec) { return detail::makeNode(std::move(spec)); }

/// @brief A page node.
/// @param spec The page.
/// @return The node.
[[nodiscard]] inline Node page(Page spec) { return detail::makeNode(std::move(spec)); }

/// @brief A page region node.
/// @param spec The region.
/// @return The node.
[[nodiscard]] inline Node pageRegion(PageRegion spec) { return detail::makeNode(std::move(spec)); }

/// @brief A component loaded by name, mounted by `Mounted` as its fallback.
/// @param spec The node.
/// @return The node.
/// @throws std::invalid_argument when the fallback is null: a renderer that cannot load the component has nothing
///         else to show.
[[nodiscard]] inline Node custom(Custom spec) {
    if (!spec.fallback) {
        throw std::invalid_argument{"morph::ui::custom: the fallback of a custom node is null"};
    }
    return detail::makeNode(std::move(spec));
}

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
/// @param common Visibility, enablement, layout and the rest of the row container's common properties.
/// @param wrap Whether rows continue on the next line when they do not fit; only a horizontal @p axis wraps.
/// @param divided Whether a rule is drawn between each two shown rows; only a vertical @p axis divides.
/// @return The node.
/// @throws std::invalid_argument when @p rows, @p keyOf or @p rowView is empty.
template <typename RowT>
[[nodiscard]] Node forEach(std::function<std::vector<RowT>()> rows, std::function<Key(RowT const&)> keyOf,
                           std::function<Node(reactive::Signal<RowT> const&)> rowView, Axis axis = Axis::Vertical,
                           int gap = 0, Common const& common = {}, bool wrap = false, bool divided = false) {
    return detail::makeNode(ForEach{
        .model =
            std::make_shared<detail::TypedForEach<RowT> const>(std::move(rows), std::move(keyOf), std::move(rowView)),
        .axis = axis,
        .gap = gap,
        .common = common,
        .wrap = wrap,
        .divided = divided,
    });
}

/// @brief One widget per row of a keyed collection held in a signal.
/// @tparam RowT The row type; copyable.
/// @param rows The signal; it must outlive every mount of the node.
/// @param keyOf A row's identity; a later duplicate of a key is reported and refused, and the first one is kept.
/// @param rowView Builds a row's view once, from a signal the mount keeps equal to the row.
/// @param axis The stacking direction.
/// @param gap Space between rows.
/// @param common Visibility, enablement, layout and the rest of the row container's common properties.
/// @param wrap Whether rows continue on the next line when they do not fit; only a horizontal @p axis wraps.
/// @param divided Whether a rule is drawn between each two shown rows; only a vertical @p axis divides.
/// @return The node.
/// @throws std::invalid_argument when @p keyOf or @p rowView is empty.
template <typename RowT>
[[nodiscard]] Node forEach(reactive::Signal<std::vector<RowT>> const& rows, std::function<Key(RowT const&)> keyOf,
                           std::function<Node(reactive::Signal<RowT> const&)> rowView, Axis axis = Axis::Vertical,
                           int gap = 0, Common const& common = {}, bool wrap = false, bool divided = false) {
    return forEach<RowT>([&rows] { return rows.get(); }, std::move(keyOf), std::move(rowView), axis, gap, common, wrap,
                         divided);
}

/// @brief Everything about a `table` beyond its columns and rows.
struct TableOptions {
    /// @brief Whether the header row is drawn.
    bool showHeader = true;
    /// @brief The row count and margin above which a row's widget is realized only near the visible range.
    Virtualization virtualize{};
    /// @brief How many rows the user can select.
    SelectionMode selectionMode = SelectionMode::None;
    /// @brief The selected rows' keys.
    Prop<std::vector<Key>> selection;
    /// @brief Called with the keys the user selected and what kind of change it was.
    std::function<void(std::vector<Key>, SelectionChange)> onSelectionChange;
    /// @brief Whether the table draws its selection as a leading column of check boxes.
    bool selectColumn = false;
    /// @brief The accessible name of the select column's header box.
    Prop<std::string> selectAllLabel = std::string{"Select all"};
    /// @brief The sort chain the headers show, most significant key first.
    Prop<std::vector<TableSort>> sort;
    /// @brief Called with a sortable column's index when the user asks to sort by it, and whether `Shift` was held.
    std::function<void(std::size_t, HeaderClick)> onSort;
    /// @brief The entries of a header's menu, given its column, or of the table's menu, given none.
    std::function<std::vector<MenuEntry>(std::optional<std::size_t>)> menu;
    /// @brief Called with the menu's column (none for the table's menu) and the chosen entry's path.
    std::function<void(std::optional<std::size_t>, std::vector<std::size_t>)> onMenu;
    /// @brief Called with a column the user moved and its new place among the visible columns.
    std::function<void(std::size_t column, std::size_t position)> onColumnMove;
    /// @brief Called with a column the user resized and its width in units, or none to restore its declared width.
    std::function<void(std::size_t column, std::optional<int> units)> onColumnResize;
    /// @brief Called with a filterable column's index when the user presses its filter button.
    std::function<void(std::size_t column)> onFilter;
    /// @brief The columns that have a filter.
    Prop<std::vector<std::size_t>> filtered;
    /// @brief The column groups whose header row is drawn over their columns.
    Prop<std::vector<TableColumnGroup>> columnGroups;
    /// @brief Called with a group's index when the user presses its filter button.
    std::function<void(std::size_t group)> onGroupFilter;
    /// @brief The header bands over the columns (`Table::bands`).
    std::vector<table::BandInfo> bands;
    /// @brief Called when the user asks to copy (`Ctrl+C` on the focused table).
    std::function<void()> onCopy;
    /// @brief Called with a row's key when the user activates it.
    std::function<void(Key)> onActivate;
    /// @brief Visibility, enablement, layout, drag-and-drop of the table.
    Common common{};
    /// @brief When set, the table's rows come by view index from the source, and the rows a builder is given are
    ///        ignored.
    std::optional<TableSource> source;
    /// @brief The status line; the default takes no space.
    Prop<TableStatus> status;
    /// @brief Called when the user chooses the status line's "Retry".
    std::function<void()> onRetry;
    /// @brief Which columns show, their order, the pinned count and the user's widths.
    Prop<TableArrangement> arrangement;
    /// @brief The row a linked chart highlights; disengaged for none.
    Prop<std::optional<TableHighlight>> highlight;
    /// @brief Called with a range gesture the backend cannot resolve itself, as two view rows and the kind of change.
    std::function<void(std::size_t anchorRow, std::size_t focusRow, SelectionChange change)> onSelectRange;
    /// @brief The focused cell; disengaged leaves focus where the user put it.
    Prop<std::optional<CellRef>> focus;
    /// @brief Called with the cell the user's keys moved focus to, once its row is fetched.
    std::function<void(std::optional<CellRef>)> onFocusCell;
    /// @brief Called, in place of `onSelectionChange`, when the user asks a `Multiple` table to select every row
    ///        (`Ctrl+A`): the handler selects the whole view, rows the backend does not hold included. Empty: the
    ///        backend selects the rows it holds and reports them through `onSelectionChange`.
    std::function<void()> onSelectAll;
    /// @brief The entries of a cell's menu, given its row's key and its column; empty for no cell menu.
    std::function<std::vector<MenuItem>(Key const& row, std::size_t column)> cellMenu;
    /// @brief The chords `cellMenu`'s entries may declare; any other key never asks for a cell's entries.
    std::vector<std::string> cellMenuKeys;
    /// @brief The caller's entries of a header's menu, given its column, shown after `menu`'s own.
    std::function<std::vector<MenuItem>(std::size_t column)> headerEntries;
    /// @brief Whether the header's sort, filter and arrangement entries are enabled.
    Prop<bool> headerEnabled = true;
    /// @brief Engaged: the selection is every row of the view less these keys, which the mount gives the backend
    ///        through `setSelectionView` on each change; when it disengages the mount sets `selection` again.
    Prop<std::optional<std::vector<Key>>> selectionView;
    /// @brief Rows an action gave new keys: the mount's focus and highlighted row follow a row from its old key to
    ///        its new one, and focus the change that removed the old key already moved to the row at its index is
    ///        taken back to the new key. A table over a `TableSource` only.
    Prop<KeyMoves> keyMoves;
    /// @brief Called with the id of the status line's entry the user chose.
    std::function<void(std::string entryId)> onStatusEntry;
    /// @brief Engaged: the table's focus is a grid's (`Table::gridFocus`).
    std::optional<Prop<std::optional<GridFocus>>> gridFocus;
    /// @brief Called with the cell a click or a `FocusMove` resolves to, while `gridFocus` is engaged.
    std::function<void(GridFocus)> onGridFocus;
    /// @brief Called with the cell the user double-clicked.
    std::function<void(CellRef)> onActivateCell;
    /// @brief Called with the cell the user clicked once (`Table::onClickCell`).
    std::function<void(CellRef)> onClickCell;
    /// @brief Called when focus leaves the table and everything inside it.
    std::function<void()> onFocusOut;
    /// @brief Engaged: the cell whose editor is open, or none (`Table::editing`).
    std::optional<Prop<std::optional<CellRef>>> editing;
    /// @brief Builds the editor of the cell `editing` names.
    std::function<Node(CellRef const&)> editor;
    /// @brief Called with each chord of `keyChords` pressed while focus is in the table.
    std::function<KeyOutcome(std::string const& chord)> onKey;
    /// @brief The chords `onKey` takes.
    Prop<std::vector<std::string>> keyChords;
    /// @brief Called with printable text typed while a cell has focus and no editor is open.
    std::function<void(std::string)> onText;
    /// @brief Engaged: the table selects cells, and this is the selected rectangle (`Table::cellSelection`).
    std::optional<Prop<std::optional<CellRange>>> cellSelection;
    /// @brief Called with the rectangle the user selected (`Table::onCellSelect`).
    std::function<void(std::optional<CellRange>)> onCellSelect;
};

/// @brief What a table's row view builds for one row: its cells, one per column, and its tone.
struct TableRowView {
    /// @brief The cells, one non-null node per column.
    std::vector<Node> cells;
    /// @brief The row's tone, bound in the row's own scope; disengaged for none.
    std::optional<Prop<std::optional<Tone>>> tone;
    /// @brief The row's accessible name, and its select box's, bound in the row's own scope; disengaged names the row
    ///        by its key.
    std::optional<Prop<std::string>> label = std::nullopt;
    /// @brief Whether the row is a heading: exactly one cell, spanning every column, never selected or activated.
    bool heading = false;
    /// @brief The cells of this row that cover the rows below, bound in the row's own scope; disengaged for none.
    ///        The mount resolves them against the table's rows, refuses and reports a bad span, and tells the widget
    ///        only the effective ones.
    std::optional<Prop<std::vector<RowSpan>>> spans;
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
    std::function<TableRowView(reactive::Signal<RowT> const&)> rowView =
        [cellsOf = std::move(cells)](reactive::Signal<RowT> const& entry) {
            return TableRowView{.cells = cellsOf(entry), .tone = std::nullopt};
        };
    return table<RowT>(std::move(columns), std::move(rows), std::move(keyOf), std::move(rowView), std::move(options));
}

/// @brief A table of keyed rows whose row view gives each row its tone as well as its cells.
///
/// As the other `table`, with @p rowView answering a `TableRowView`: the tone binds in the row's own scope, and the
/// table is told it after the row's key and on every change.
/// @tparam RowT The row type; copyable.
/// @param columns The columns, in order.
/// @param rows Reads the rows; every signal it reads is a dependency.
/// @param keyOf A row's identity; a later duplicate of a key is reported and refused, and the first one is kept.
/// @param rowView Builds a row's cells, one per column, and its tone, once, from a signal the mount keeps equal to
///        the row.
/// @param options Selection, activation and `Common`.
/// @return The node.
/// @throws std::invalid_argument when @p rows, @p keyOf or @p rowView is empty.
template <typename RowT>
[[nodiscard]] Node table(std::vector<TableColumn> columns, std::function<std::vector<RowT>()> rows,
                         std::function<Key(RowT const&)> keyOf,
                         std::function<TableRowView(reactive::Signal<RowT> const&)> rowView,
                         TableOptions options = {}) {
    if (!rows || !keyOf || !rowView) {
        throw std::invalid_argument{"ui::table: the rows, the key function and the row view must not be empty"};
    }
    std::function<Node(reactive::Signal<RowT> const&)> rowNode =
        [viewOf = std::move(rowView), count = columns.size()](reactive::Signal<RowT> const& entry) {
            TableRowView made = viewOf(entry);
            std::size_t const wanted = made.heading ? 1 : count;
            if (made.cells.size() != wanted ||
                std::ranges::any_of(made.cells, [](Node const& cell) { return cell == nullptr; })) {
                throw std::invalid_argument{
                    "ui::table: a row needs exactly one non-null cell per column, and a heading row one cell"};
            }
            return row(Row{.children = std::move(made.cells),
                           .tone = std::move(made.tone),
                           .label = std::move(made.label),
                           .heading = made.heading,
                           .spans = std::move(made.spans)});
        };
    Table spec{
        .rows =
            std::make_shared<detail::TypedForEach<RowT> const>(std::move(rows), std::move(keyOf), std::move(rowNode)),
        .showHeader = options.showHeader,
        .virtualize = options.virtualize,
        .selectionMode = options.selectionMode,
    };
    spec.columns = std::move(columns);
    spec.selection = std::move(options.selection);
    spec.onSelectionChange = std::move(options.onSelectionChange);
    spec.selectColumn = options.selectColumn;
    spec.selectAllLabel = std::move(options.selectAllLabel);
    spec.sort = std::move(options.sort);
    spec.onSort = std::move(options.onSort);
    spec.menu = std::move(options.menu);
    spec.onMenu = std::move(options.onMenu);
    spec.onColumnMove = std::move(options.onColumnMove);
    spec.onColumnResize = std::move(options.onColumnResize);
    spec.onFilter = std::move(options.onFilter);
    spec.filtered = std::move(options.filtered);
    spec.columnGroups = std::move(options.columnGroups);
    spec.onGroupFilter = std::move(options.onGroupFilter);
    spec.bands = std::move(options.bands);
    spec.onCopy = std::move(options.onCopy);
    spec.onActivate = std::move(options.onActivate);
    spec.common = std::move(options.common);
    spec.source = std::move(options.source);
    spec.status = std::move(options.status);
    spec.onRetry = std::move(options.onRetry);
    spec.arrangement = std::move(options.arrangement);
    spec.highlight = std::move(options.highlight);
    spec.onSelectRange = std::move(options.onSelectRange);
    spec.focus = std::move(options.focus);
    spec.onFocusCell = std::move(options.onFocusCell);
    spec.onSelectAll = std::move(options.onSelectAll);
    spec.cellMenu = std::move(options.cellMenu);
    spec.cellMenuKeys = std::move(options.cellMenuKeys);
    spec.headerEntries = std::move(options.headerEntries);
    spec.headerEnabled = std::move(options.headerEnabled);
    spec.selectionView = std::move(options.selectionView);
    spec.keyMoves = std::move(options.keyMoves);
    spec.onStatusEntry = std::move(options.onStatusEntry);
    spec.gridFocus = std::move(options.gridFocus);
    spec.onGridFocus = std::move(options.onGridFocus);
    spec.onActivateCell = std::move(options.onActivateCell);
    spec.onClickCell = std::move(options.onClickCell);
    spec.onFocusOut = std::move(options.onFocusOut);
    spec.editing = std::move(options.editing);
    spec.editor = std::move(options.editor);
    spec.onKey = std::move(options.onKey);
    spec.keyChords = std::move(options.keyChords);
    spec.onText = std::move(options.onText);
    spec.cellSelection = std::move(options.cellSelection);
    spec.onCellSelect = std::move(options.onCellSelect);
    return detail::makeNode(std::move(spec));
}

/// @brief A table whose rows come by view index from a `TableSource`: the mount realizes the rows in and near view,
///        builds each cell from its column's view, and keeps a row's widget when the row moves.
/// @param columns The columns, in order.
/// @param source  The rows, the cells and the row's own parts.
/// @param options Selection, sorting, the status line and `Common`; its `source` is replaced by @p source.
/// @return The node.
/// @throws std::invalid_argument when @p source has no rows or no cell view.
[[nodiscard]] inline Node table(std::vector<TableColumn> columns, TableSource source, TableOptions options = {}) {
    if (!source.rows || !source.cellView) {
        throw std::invalid_argument{"ui::table: a table source needs its rows and its cell view"};
    }
    Table spec{
        .columns = std::move(columns),
        .rows = nullptr,
        .showHeader = options.showHeader,
        .virtualize = options.virtualize,
        .selectionMode = options.selectionMode,
    };
    spec.selection = std::move(options.selection);
    spec.onSelectionChange = std::move(options.onSelectionChange);
    spec.selectColumn = options.selectColumn;
    spec.selectAllLabel = std::move(options.selectAllLabel);
    spec.sort = std::move(options.sort);
    spec.onSort = std::move(options.onSort);
    spec.menu = std::move(options.menu);
    spec.onMenu = std::move(options.onMenu);
    spec.onColumnMove = std::move(options.onColumnMove);
    spec.onColumnResize = std::move(options.onColumnResize);
    spec.onFilter = std::move(options.onFilter);
    spec.filtered = std::move(options.filtered);
    spec.columnGroups = std::move(options.columnGroups);
    spec.onGroupFilter = std::move(options.onGroupFilter);
    spec.bands = std::move(options.bands);
    spec.onCopy = std::move(options.onCopy);
    spec.onActivate = std::move(options.onActivate);
    spec.common = std::move(options.common);
    spec.source = std::move(source);
    spec.status = std::move(options.status);
    spec.onRetry = std::move(options.onRetry);
    spec.arrangement = std::move(options.arrangement);
    spec.highlight = std::move(options.highlight);
    spec.onSelectRange = std::move(options.onSelectRange);
    spec.focus = std::move(options.focus);
    spec.onFocusCell = std::move(options.onFocusCell);
    spec.onSelectAll = std::move(options.onSelectAll);
    spec.cellMenu = std::move(options.cellMenu);
    spec.cellMenuKeys = std::move(options.cellMenuKeys);
    spec.headerEntries = std::move(options.headerEntries);
    spec.headerEnabled = std::move(options.headerEnabled);
    spec.selectionView = std::move(options.selectionView);
    spec.keyMoves = std::move(options.keyMoves);
    spec.onStatusEntry = std::move(options.onStatusEntry);
    spec.gridFocus = std::move(options.gridFocus);
    spec.onGridFocus = std::move(options.onGridFocus);
    spec.onActivateCell = std::move(options.onActivateCell);
    spec.onClickCell = std::move(options.onClickCell);
    spec.onFocusOut = std::move(options.onFocusOut);
    spec.editing = std::move(options.editing);
    spec.editor = std::move(options.editor);
    spec.onKey = std::move(options.onKey);
    spec.keyChords = std::move(options.keyChords);
    spec.onText = std::move(options.onText);
    spec.cellSelection = std::move(options.cellSelection);
    spec.onCellSelect = std::move(options.onCellSelect);
    return detail::makeNode(std::move(spec));
}

}  // namespace morph::ui

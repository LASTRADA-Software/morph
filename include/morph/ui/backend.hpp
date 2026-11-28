// SPDX-License-Identifier: Apache-2.0

#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <morph/export.hpp>
#include <optional>
#include <span>
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
/// never throws: the mount catches and reports what one throws. Every setter takes UTF-8.
///
/// A setter called with the value the widget already shows changes nothing: a text input keeps its cursor, its
/// selection and an input method's composition. Input widgets are controlled: a user's action changes what the
/// widget shows, as the toolkit does, and reports the request through the handler; the mount then shows what the
/// application's slot says, in a turn posted after the request's flush, so a refused request snaps back.
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

    /// @brief Sets the name assistive technology reads for the widget.
    /// @param name The accessible name; empty leaves the widget's own text as its name.
    virtual void setAccessibleName(std::string_view name) = 0;

    /// @brief Sets the role assistive technology announces for the widget.
    /// @param role The role, such as `"heading"`; empty leaves the kind's own role.
    virtual void setAccessibleRole(std::string_view role) = 0;

    /// @brief Sets the stable identifier an automated test finds the widget by.
    /// @param testId The identifier; empty means none.
    virtual void setTestId(std::string_view testId) = 0;

    /// @brief Sets the text shown while the pointer rests on the widget.
    /// @param text The tooltip; empty shows none.
    virtual void setTooltip(std::string_view text) = 0;

    /// @brief Styles the widget and everything inside it with a named token set of the theme. May be called again:
    ///        each call restyles the widget and everything inside it.
    /// @param surface The surface's name; empty is the theme's default.
    virtual void setSurface(std::string_view surface) = 0;

    /// @brief Declares the keyboard chords the widget handles while focus is inside it.
    ///
    /// A chord pressed while focus is inside several widgets that declare it goes to the innermost of them only. A
    /// widget that declares `Tab` or `Shift+Tab` receives them instead of focus moving.
    /// @param chords The chords, each modifiers joined to a key name, such as `"Ctrl+S"`.
    /// @param onChord Called with the chord that was pressed.
    virtual void setKeys(std::vector<std::string> const& chords, std::function<void(std::string)> onChord) = 0;

    /// @brief Moves keyboard focus to the widget.
    virtual void focus() = 0;

    /// @brief Calls @p onHover each time the pointer comes to rest over the widget. A backend without a pointer, or
    ///        one that does not report hovering, keeps the default, which never calls it: hovering is a hint, such as
    ///        a prefetch, never something a widget relies on.
    /// @param onHover The handler; empty stops calling one.
    // NOLINTNEXTLINE(performance-unnecessary-value-param): a sink every override moves from; the default drops it.
    virtual void setOnHover(std::function<void()> onHover) { static_cast<void>(onHover); }
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

/// @brief A widget that shows a value: text, or an input. A new field is editable, not required, without errors and
///        not stale.
class FieldWidget : public Widget {
public:
    /// @brief Makes the value read-only or editable again. A read-only field still takes focus and lets its text be
    ///        selected, and reports no edit, submit or commit.
    /// @param readonly Whether the user may not change the value.
    virtual void setReadOnly(bool readonly) = 0;

    /// @brief Shows or clears the mark of a field that must be given.
    /// @param required Whether to show the mark.
    virtual void setRequired(bool required) = 0;

    /// @brief Replaces the messages shown with the field.
    /// @param errors The messages, in order, UTF-8; empty shows none.
    virtual void setErrors(std::vector<std::string> const& errors) = 0;

    /// @brief Shows or clears the mark of a value that is being recomputed.
    /// @param stale Whether the value shown is the last known one.
    virtual void setStale(bool stale) = 0;

    /// @brief Shows a line that describes the field under its control, after its messages, and makes it the
    ///        control's accessible description, after the messages. A text takes none: the mount never calls it on
    ///        one, and a backend refuses it there.
    /// @param description The line, UTF-8; empty shows none.
    virtual void setDescription(std::string_view description) = 0;

    /// @brief Shows a hint after the field's caption and its required mark, such as the range a value must lie in
    ///        ("Temperature on arrival * · 2-8 degrees"). It is drawn only while the input shows a caption. A text
    ///        takes none: the mount never calls it on one, and a backend refuses it there.
    /// @param hint The hint, UTF-8; empty shows none.
    virtual void setCaptionHint(std::string_view hint) = 0;

    /// @brief Evaluates the value without making it invalid: outlines the control in the tone and draws the
    ///        description in the tone's colour. A field with errors is drawn as `Err` whatever its tone. A text
    ///        takes it as its colour, whatever its role.
    /// @param tone The tone; `Neutral` is none.
    virtual void setTone(Tone tone) = 0;
};

/// @brief Read-only text.
class TextWidget : public FieldWidget {
public:
    /// @brief Replaces the text.
    /// @param text The text, UTF-8.
    virtual void setText(std::string_view text) = 0;

    /// @brief Sets the style.
    /// @param role The role, mapped to the backend's theme.
    virtual void setRole(TextRole role) = 0;

    /// @brief Replaces what the text shows with runs, in order, in the text's role: a strong run in the backend's
    ///        bold weight, a mono run in its monospace face, a toned run in the tone's colour, and every run's text as
    ///        its characters. The mount calls it in place of `setText` for a text that has spans.
    /// @param spans The runs.
    virtual void setSpans(std::vector<TextSpan> const& spans) = 0;

    /// @brief Places each line within the text's own width. A new text starts at the leading edge; the mount calls
    ///        it once, only for a text whose document gives an alignment. Under a right-to-left layout `Start` and
    ///        `End` swap. A text as wide as its content shows no difference. In a table, a text that has its own
    ///        alignment keeps it over its column's.
    /// @param align Where the lines sit.
    virtual void setAlign(TextAlign align) = 0;
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

    /// @brief Sets how much the button stands out. The mount calls it only for another value than `Normal`.
    /// @param emphasis The emphasis.
    virtual void setEmphasis(ButtonEmphasis emphasis) = 0;

    /// @brief Sets the icon drawn beside the caption; called once, and only for a button that has one.
    /// @param icon The name of an icon in the theme's icon set.
    virtual void setIcon(std::string_view icon) = 0;

    /// @brief Sets how the button is drawn. A new button is a push button; the mount calls it once, only for `Item`.
    /// @param style The style.
    virtual void setStyle(ButtonStyle style) = 0;

    /// @brief Marks the button selected, or clears the mark: an `Item` is the current entry of its list, a push
    ///        button is pressed in (a well picked on a plate). A new button is not selected; the mount calls it for
    ///        either style, only when `selected` is bound or `true`.
    /// @param selected Whether it is selected.
    virtual void setSelected(bool selected) = 0;

    /// @brief Sets the button's outline. A new button has the theme's corners; the mount calls it once, only for
    ///        `Round`.
    /// @param shape The shape.
    virtual void setShape(ControlShape shape) = 0;

    /// @brief Sets the button's density. A new button is normal; the mount calls it once, only for `Small`.
    /// @param size The size.
    virtual void setSize(ButtonSize size) = 0;

    /// @brief Draws the caption in the `mono` text role's face. A new button uses the body face; the mount calls it
    ///        once, only for `true`.
    /// @param mono Whether to.
    virtual void setMono(bool mono) = 0;

    /// @brief Draws the caption at a weight, in place of the theme's button weight. A new button draws the theme's;
    ///        the mount calls it once, only for a nonzero weight. A backend with no weights draws a weight of 600 or
    ///        more as bold and a lighter one plain.
    /// @param weight The weight, 1 to 1000.
    virtual void setWeight(int weight) = 0;

    /// @brief The entry's content: a container the button owns and draws as the entry in place of its label, which
    ///        then names it without being drawn. The mount asks for it only for an `Item` that has content. The entry
    ///        stays one button: one focus stop, and a click anywhere on its content is the button's.
    /// @return It, owned by the button and valid as long as the button.
    virtual ContainerWidget& content() = 0;
};

/// @brief An editable text field.
class TextInputWidget : public FieldWidget {
public:
    /// @brief Replaces the text. Never calls the `onChange` handler. With the text the field already shows, it changes
    ///        nothing: the cursor, the selection and an input method's composition stay as they are.
    /// @param text The text, UTF-8.
    virtual void setText(std::string_view text) = 0;

    /// @brief Moves the text cursor to @p offset, a byte offset into the text the field shows. An offset past the
    ///        text's end moves it to the end; one inside a UTF-8 sequence moves it back to the start of that code
    ///        point (a "character" here is a code point). Clears the selection. Calls no handler, does not move
    ///        keyboard focus and does not scroll: `revealCursor` does that.
    /// @param offset The byte offset.
    virtual void setCursor(std::size_t offset) = 0;

    /// @brief Scrolls the scrolling containers around the field so that the line the cursor is on is in view. Best
    ///        effort: a backend that cannot does nothing, and the default does nothing. Calls no handler and does not
    ///        move keyboard focus or the cursor.
    virtual void revealCursor() {}

    /// @brief Sets the text shown while the field is empty.
    /// @param placeholder The text, UTF-8.
    virtual void setPlaceholder(std::string_view placeholder) = 0;

    /// @brief Sets what a user edit calls, with the whole new text.
    /// @param onChange The handler; empty does nothing.
    virtual void setOnChange(std::function<void(std::string)> onChange) = 0;

    /// @brief Sets what a submit (Enter in single-line mode) calls, with the text.
    /// @param onSubmit The handler; empty does nothing.
    virtual void setOnSubmit(std::function<void(std::string)> onSubmit) = 0;

    /// @brief Sets what a commit calls, with the text: Enter in single-line mode, before the submit, or focus
    ///        leaving the field after an edit.
    /// @param onCommit The handler; empty does nothing.
    virtual void setOnCommit(std::function<void(std::string)> onCommit) = 0;

    /// @brief Sets the unit the input shows, muted, at its trailing edge, inside the field; empty shows none. Called
    ///        only on a single-line or password input.
    /// @param unit The unit's display text, UTF-8.
    virtual void setUnit(std::string_view unit) = 0;

    /// @brief Sets the text drawn muted at the field's leading edge, inside it, and announced before the value; empty
    ///        shows none. Called only on a single-line or password input.
    /// @param prefix The prefix's display text, UTF-8.
    virtual void setPrefix(std::string_view prefix) = 0;

    /// @brief Draws the text and the placeholder in the `mono` text role's face. A new field uses the body face; the
    ///        mount calls it once, only for `true`.
    /// @param mono Whether to.
    virtual void setMono(bool mono) = 0;

    /// @brief Stops a multiline input wrapping a line longer than the field: each line stays whole and the field
    ///        scrolls sideways. A new multiline field wraps; the mount calls this once, only for `false`, and only on
    ///        a multiline input. A terminal field shows each line as written, so it draws nothing new.
    /// @param wrap Always false: an input that wraps is never told.
    virtual void setWrap(bool wrap) = 0;

    /// @brief Places the text in the field. A new field's text starts at the leading edge; the mount calls it once,
    ///        only for `Center` or `End`, and only on a single-line or password input. An `End` field draws its
    ///        digits at one width each, so a column of values lines up.
    /// @param align `Start`, `Center` or `End`; `Stretch` reads as `Start`.
    virtual void setAlign(Align align) = 0;

    /// @brief Sets the fraction digits a number is entered with: what a backend or a kit may use to align or
    ///        format the text. Nothing is required of a backend that draws nothing from it. Called only on a
    ///        single-line or password input.
    /// @param decimals The places, from 0; -1 when the input does not hold a number.
    virtual void setDecimals(int decimals) = 0;

    /// @brief Sets the icon drawn at the field's leading edge; empty shows none. Called once, and only for a
    ///        single-line or password input that has one.
    /// @param icon An icon name of the theme's icon set.
    virtual void setIcon(std::string_view icon) = 0;

    /// @brief Sets the field's label, and whether the input draws it as a visible caption above its control.
    ///        Without `shown`, the caption is drawn elsewhere (a form's cell), or not at all.
    /// @param label The label, UTF-8.
    /// @param shown Whether the input shows it.
    virtual void setCaption(std::string_view label, bool shown) = 0;

    /// @brief Sets how much the input stands out: a prominent one is drawn as the screen's focal control, on Qt
    ///        Quick larger and outlined in the accent, on the terminal in bold. The mount calls it once, only for
    ///        `Prominent`, and never on a multiline input.
    /// @param emphasis The emphasis.
    virtual void setEmphasis(InputEmphasis emphasis) = 0;
};

/// @brief A labelled check box.
class CheckboxWidget : public FieldWidget {
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
class SelectWidget : public FieldWidget {
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

    /// @brief Calls @p onOpen each time the user opens the dropdown, also when it has no options to show. A radio
    ///        group, and a backend that does not report opening, keep the default, which never calls it: opening is a
    ///        hint, such as a retry of a failed fetch of the options, never something a select relies on.
    /// @param onOpen The handler; empty stops calling one.
    // NOLINTNEXTLINE(performance-unnecessary-value-param): a sink every override moves from; the default drops it.
    virtual void setOnOpen(Action onOpen) { static_cast<void>(onOpen); }

    /// @brief Sets the field's label, and whether the input draws it as a visible caption above its control.
    ///        Without `shown`, the caption is drawn elsewhere (a form's cell), or not at all.
    /// @param label The label, UTF-8.
    /// @param shown Whether the input shows it.
    virtual void setCaption(std::string_view label, bool shown) = 0;
};

/// @brief The marker a button-style menu's trigger draws after its text, to say that it opens.
enum class MenuCaret : std::uint8_t {
    None,    ///< No marker.
    Down,    ///< A chevron pointing down: a menu of actions or of a person.
    UpDown,  ///< An up and a down chevron: a menu that chooses one of several values, such as a scope.
};

/// @brief The trigger of a button-style menu.
struct MenuTrigger {
    /// @brief The trigger's text and accessible name; empty for an icon-only trigger.
    std::string label;
    /// @brief The trigger's icon name in the theme's icon map; empty for none.
    std::string icon;
    /// @brief The trigger's emphasis, as a button's.
    ButtonEmphasis emphasis = ButtonEmphasis::Normal;
    /// @brief A second line under the label; empty for none. The accessible name is the label, then the detail.
    std::string detail;
    /// @brief Initials drawn in a filled circle before the label (a person's avatar); empty for none. The terminal
    ///        draws them in brackets.
    std::string avatar;
    /// @brief The marker drawn after the text. The terminal's trigger always ends in a marker, since it has no frame
    ///        to say that it opens; `None` and `Down` both draw a down chevron there.
    MenuCaret caret = MenuCaret::None;

    /// @brief Memberwise equality.
    /// @return Whether every field matches.
    bool operator==(MenuTrigger const&) const = default;
};

/// @brief A menu of commands, with nested submenus.
class MenuWidget : public Widget {
public:
    /// @brief Replaces the entries.
    /// @param entries The entries, in order, each with its submenu.
    virtual void setItems(std::vector<MenuEntry> const& entries) = 0;

    /// @brief Sets a button-style menu's trigger; the mount calls it only for a menu created with
    ///        `MenuStyle::Button`, at mount and whenever the label changes.
    ///
    /// The label, when not empty, is the trigger's text and its accessible name. An icon-only trigger draws its icon
    /// from the theme's set; `more` and `nav.menu`, framework names, draw `⋯` and `☰` when the set does not name
    /// them, and another icon the set does not name draws the widget's accessible name as the label, so the trigger is
    /// never blank.
    /// @param trigger The trigger's label, icon and emphasis.
    virtual void setTrigger(MenuTrigger const& trigger) = 0;

    /// @brief Sets what choosing an entry calls, with its path.
    ///
    /// Only an enabled entry without a submenu, inside enabled entries, can be chosen; a separator never can. A path
    /// counts separators, so it names the same entry whichever backend reports it.
    /// @param onActivate The handler, given the entry's index at each level from the top; empty does nothing.
    virtual void setOnActivate(std::function<void(std::vector<std::size_t>)> onActivate) = 0;
};

/// @brief Children stacked along one axis, fixed at creation.
class StackWidget : public ContainerWidget {
public:
    /// @brief Sets the space between children.
    /// @param gap A spacing step, 0 to `kMaxSpacingStep`; the mount never passes another value.
    virtual void setGap(int gap) = 0;

    /// @brief Places the children across the axis. A new stack stretches them; a child's own `alignSelf` wins.
    /// @param align Where they sit.
    virtual void setAlign(Align align) = 0;

    /// @brief Places the spare length along the axis while no shown child stretches. A new stack packs at the start.
    /// @param justify Where it goes.
    virtual void setJustify(Justify justify) = 0;

    /// @brief Makes a horizontal stack wrap: its children fill lines in order, and a child that does not fit after
    ///        the ones already on a line starts the next. The mount calls it once, before the stack's first child,
    ///        and only for a stack that wraps.
    ///
    /// Each shown child has a basis along the row: a content child its natural width, a fixed child its amount, and
    /// a stretch child its minimum width, else its natural width; each clamped into the child's bounds and at most
    /// the row's width. A line always holds one child; a child whose basis is 0 takes no gap and never starts a
    /// line. On a line, the stretch children share the spare width by weight, each stopping at its maximum; a line
    /// with none places it by the stack's `justify`. A line is as tall as its tallest child, the stack's `align`
    /// places a child down its line, and lines are `gap` apart, the step children are apart along a line. A child
    /// wider than the stack sits alone on its line at the stack's width. The stack's natural width is its children's
    /// on one line, and its natural height the height of its lines at the width it is given.
    /// @param wrap Always true: a stack that does not wrap is never told.
    virtual void setWrap(bool wrap) = 0;

    /// @brief Draws a 1 px rule in the theme's divider colour between each two shown children of a stack that does not
    ///        wrap, centred in the gap: horizontal in a vertical stack, vertical in a row. The rule takes 1 px of its
    ///        own, so a gap of 0 still separates the children. The mount calls it once, before the stack's first
    ///        child, and only for a stack that is divided. A backend that cannot spare a row for a rule draws none:
    ///        the terminal's lines are already apart.
    /// @param divided Always true: a stack that is not divided is never told.
    virtual void setDivided(bool divided) = 0;

    /// @brief Insets the children from the stack's edges: the vertical step above and below, the horizontal step at
    ///        the sides. The mount calls it once, before the stack's first child, and only for a stack that has
    ///        padding. The stack's natural size includes the inset.
    /// @param padding The steps, each 0 to `kMaxSpacingStep`.
    virtual void setPadding(Padding padding) = 0;

    /// @brief Marks a horizontal stack as a row of actions: the buttons of an overlay's footer or of an `actionBar`.
    ///        A backend that reads the theme's `actions.gap` spaces the children by it while it is above 0, in place
    ///        of the stack's `gap`; the terminal, which reads no dimension, keeps `gap`. The mount calls it once,
    ///        before the stack's first child, and only for a row of actions.
    virtual void setActionRow() = 0;

    /// @brief Marks the stack as the chosen entry of a list, or not: while selected it draws the accent's 3 px bar at
    ///        its leading edge, down its whole height, over its surface and under its children, inside its leading
    ///        padding. The mount calls it for a stack that gives the key, at mount (possibly with false) and on every
    ///        change; that first call is what tells a backend the stack can be selected, so one that reserves room
    ///        for the bar reserves it from then on whether or not the stack is selected.
    /// @param selected Whether the stack is the chosen entry.
    virtual void setSelected(bool selected) = 0;

    /// @brief Makes the stack transparent to `bleed`: a child of it that bleeds reaches past the padding of the
    ///        stack's own container (a panel's padding, a tab page's), not past the stack's, as if the child were
    ///        that container's. The mount calls it once, before the stack's first child, and only with true, for the
    ///        column the interpreter inserts around a list of nodes. A table's row view ignores it.
    /// @param passes Always true: a stack that is not a holder is never told.
    virtual void setPassesBleed(bool passes) = 0;

    /// @brief Marks the stack as the shell's expanded head bar: its gap, which pads the line between its two
    ///        zero-height spacers, is then the theme's `head.paddingY` when the theme sets one (above 0), in place of
    ///        the spacing step `setGap` gave. A new stack is not a head bar; the frame calls it with true while the
    ///        shell is expanded and with false while it is compact. A backend that reads no such token, such as the
    ///        terminal, keeps the gap it was given.
    /// @param headBar Whether the stack is the expanded head bar.
    virtual void setHeadBar(bool headBar) = 0;
};

/// @brief Children laid out row-major in columns.
class GridWidget : public ContainerWidget {
public:
    /// @brief Sets the column count; the columns share the width.
    /// @param columns At least one.
    virtual void setColumns(int columns) = 0;

    /// @brief Sets the column tracks in place of a column count; the mount calls it once, before any child, and then
    ///        never calls `setColumns`.
    ///
    /// Fixed tracks take their amount, content tracks their widest cell that spans only them, and stretch tracks
    /// share what is left by weight. A spanning cell covers its tracks and the gaps between them, and sizes none.
    /// Across its track, a text cell's text and a label-width cell sit where the track's `align` says (the first
    /// track's, for a spanning cell); a container cell fills the track.
    /// @param tracks At least one.
    virtual void setTracks(std::vector<GridTrack> const& tracks) = 0;

    /// @brief Makes the column count follow the grid's width: the columns shown are
    ///        `max(1, min(columns, floor((W + g) / (m * u + g) + e)))`, W the width inside the grid, g the gap, m the
    ///        least width given here, u the unit and e 1e-6, so a width that holds exactly n columns, computed in
    ///        floating point, gives n. The natural width is unchanged (`columns` times the widest cell, a span divided
    ///        out); the natural height is that of the columns shown at the width the grid was last laid out at, so a
    ///        grid whose columns drop pushes its siblings down. The mount calls it once, before any child, only for a
    ///        grid without tracks that gives a least width above 0, and the tracks and this are never both given.
    /// @param units The least column width in units, at least 1.
    virtual void setMinColumnWidth(int units) = 0;

    /// @brief Sets the space between cells.
    /// @param gap A spacing step, 0 to `kMaxSpacingStep`; the mount never passes another value.
    virtual void setGap(int gap) = 0;

    /// @brief Sets how many columns a child spans; a child spans one until this is called.
    /// @param child One of this grid's children.
    /// @param span The column count it occupies.
    virtual void setSpan(Widget& child, int span) = 0;

    /// @brief Places each cell's content vertically in its row. A new grid stretches it; a child's own `alignSelf`
    ///        wins.
    /// @param align Where it sits.
    virtual void setAlign(Align align) = 0;
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
    /// @param padding A spacing step, 0 to `kMaxSpacingStep`; the mount never passes another value.
    virtual void setPadding(int padding) = 0;

    /// @brief Sets whether the user can collapse the panel.
    /// @param collapsible Whether a collapse control is shown.
    virtual void setCollapsible(bool collapsible) = 0;

    /// @brief Shows one of its children beside the title; called at most once. The header stays visible and usable
    ///        while the panel is collapsed.
    /// @param header A child of this widget.
    virtual void setHeader(Widget& header) = 0;

    /// @brief Collapses or expands the panel. A collapsed panel hides its content, which takes no input; its own
    ///        collapse control still works. The header is not part of "its content" for this purpose: it stays
    ///        shown and usable while collapsed. Never calls the `onToggle` handler.
    /// @param collapsed Whether the content is hidden.
    virtual void setCollapsed(bool collapsed) = 0;

    /// @brief Sets what a user collapse or expand calls, with the requested state.
    /// @param onToggle The handler; empty does nothing.
    virtual void setOnToggle(std::function<void(bool)> onToggle) = 0;
};

/// @brief A scrollable viewport, on the axis fixed at creation, that keeps the focused child visible.
class ScrollWidget : public ContainerWidget {};

/// @brief A container that shows its children and nothing else: the host of a Switch's case and of a Tabs page.
class SlotWidget : public ContainerWidget {
public:
    /// @brief Whether the slot scrolls a child that does not stretch in height. A scrollable slot shows such a
    ///        child at its natural height inside a vertical viewport of the slot's own height, which keeps the
    ///        focused widget in view, and scrolls only while the child is taller than the slot (a slot whose content
    ///        fits takes no input and no focus); a child that stretches in height is laid out as in a slot that does
    ///        not scroll, filling it, but is never shorter than its `minHeight`, and the slot scrolls it while it is
    ///        shorter than that. Where the frontend can, a slot whose content cannot be narrower than it scrolls
    ///        sideways too. The slot reads its shown child's height sizing itself, so a new child follows without
    ///        another call. A new slot does not scroll.
    /// @param scrollable Whether it scrolls.
    virtual void setScrollable(bool scrollable) = 0;
};

/// @brief A tab bar over page slots.
///
/// Its children are page slots in the order they were first shown; the mount keeps exactly the selected page's
/// slot visible. `setSelected` moves the bar's highlight only.
class TabsWidget : public ContainerWidget {
public:
    /// @brief Replaces the tab captions. A call with as many captions as the last one replaces them in place and keeps
    ///        the highlight, the shown page and the keyboard focus.
    /// @param labels One caption per tab, in order.
    virtual void setTabs(std::vector<std::string> const& labels) = 0;

    /// @brief Highlights a tab. Never calls the `onSelect` handler.
    /// @param index The tab; past the last one highlights none.
    virtual void setSelected(std::size_t index) = 0;

    /// @brief Sets what a user's tab choice calls, with its index.
    /// @param onSelect The handler; empty does nothing.
    virtual void setOnSelect(std::function<void(std::size_t)> onSelect) = 0;

    /// @brief Shows or hides the strip of tab buttons. The selected page shows either way. A new tabs widget shows
    ///        its strip.
    /// @param shown Whether the strip is drawn.
    virtual void setBarShown(bool shown) = 0;

    /// @brief Sets the label of the menu that holds the tabs that do not fit in the bar. A frontend that scrolls or
    ///        marks its tabs instead stores it and draws nothing from it.
    /// @param label The label, UTF-8.
    virtual void setMoreLabel(std::string_view label) = 0;

    /// @brief Insets the strip and, unless `setPageInset` is called, the pages: the strip and its rule span the
    ///        widget's width, the first tab starts this far in from the leading edge, and each page is padded by it on
    ///        every side, with nothing else between the strip's rule and the page. The mount calls it once, only for a
    ///        nonzero step.
    /// @param inset A spacing step, 1 to `kMaxSpacingStep`; the mount never passes another value.
    virtual void setInset(int inset) = 0;

    /// @brief Pads each page by its own step in place of `setInset`'s. Each page pads its own content, inside its
    ///        scroll, so a page that scrolls clips at the widget's edges, shows its bar there, and lets a bleeding
    ///        child reach them. The mount calls it once, only when the view gives a page inset, which may be 0.
    /// @param inset A spacing step, 0 to `kMaxSpacingStep`.
    virtual void setPageInset(int inset) = 0;

    /// @brief Draws the strip, and nothing else, on a surface across the widget's width. The mount calls it once,
    ///        only for a nonempty name.
    /// @param surface The surface's name, as `Widget::setSurface` takes it.
    virtual void setStripSurface(std::string_view surface) = 0;
};

/// @brief A modal overlay around its children, with a focus trap while open. A new dialog is closed.
///
/// A closed dialog shows nothing and takes no input, its children included. A user's dismissal closes it, as the
/// toolkit does, before `onDismiss` runs; an application that keeps it open opens it again.
class DialogWidget : public ContainerWidget {
public:
    /// @brief Shows or hides the overlay.
    /// @param open Whether it is shown.
    virtual void setOpen(bool open) = 0;

    /// @brief Replaces the title.
    /// @param title The title, UTF-8.
    virtual void setTitle(std::string_view title) = 0;

    /// @brief Shows a line under the title, in the head, which does not scroll with the content, and makes it the
    ///        overlay's accessible description. The mount calls it only when bound or not empty.
    /// @param description The line, UTF-8; empty shows none.
    virtual void setDescription(std::string_view description) = 0;

    /// @brief Shows the description as runs, as `TextWidget::setSpans` shows a text, and makes their joined
    ///        characters the overlay's accessible description. The mount calls it in place of `setDescription` for a
    ///        dialog whose document gives runs.
    /// @param spans The runs.
    virtual void setDescriptionSpans(std::vector<TextSpan> const& spans) = 0;

    /// @brief Sets what a user dismissal (Esc on the TUI) of the open dialog calls.
    /// @param onDismiss The handler; empty does nothing.
    virtual void setOnDismiss(Action onDismiss) = 0;

    /// @brief The dialog's footer: a container it lays out after its content, which shows nothing while it is empty.
    ///        The mount puts the dialog's actions here.
    /// @return It, owned by the dialog and valid as long as the dialog.
    virtual ContainerWidget& footer() = 0;

    /// @brief Draws the dialog as a sheet, or as a centred overlay again.
    ///
    /// A sheet fills the window, with no margin and no rounding, and keeps every rule of the dialog: the focus trap,
    /// the footer, Escape as a dismissal. A press outside is not possible, so its head holds a dismiss control
    /// (labelled by `setDismissLabel`) before the title, which dismisses exactly as Escape does. Changing it while
    /// the dialog is open keeps the content and its state. A new dialog is not a sheet; the mount calls this only
    /// when the document binds `sheet` or sets it.
    /// @param sheet Whether it is a sheet.
    virtual void setSheet(bool sheet) = 0;

    /// @brief Sets the label of the dismiss control, a sheet's or the one `setCloseButton` draws; a new overlay's is
    ///        "Close". The mount calls this only when the label is bound or not the default.
    /// @param label The label, UTF-8.
    virtual void setDismissLabel(std::string_view label) = 0;

    /// @brief Shows a short muted note on the title's row, against the trailing edge. The mount calls it only when
    ///        the note is bound or not empty.
    /// @param note The note, UTF-8; empty shows none.
    virtual void setHeadNote(std::string_view note) = 0;

    /// @brief Shows a short muted note leading in the footer's row, opposite the actions. The footer shows while
    ///        the note is not empty, though it holds no action. The mount calls it only when the note is bound or
    ///        not empty.
    /// @param note The note, UTF-8; empty shows none.
    virtual void setFooterNote(std::string_view note) = 0;

    /// @brief Draws a dismiss control (labelled by `setDismissLabel`) on the title's row, against the trailing edge,
    ///        which dismisses exactly as Escape does. A sheet's head already holds one, so this adds nothing there.
    ///        The mount calls it only when the condition is bound or true.
    /// @param shown Whether the control is drawn.
    virtual void setCloseButton(bool shown) = 0;
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

/// @brief How a click on a table row is modified, which decides what it does to the selection.
enum class RowClick : std::uint8_t {
    Only,    ///< A plain click: only the row is selected, and it becomes the anchor.
    Toggle,  ///< Ctrl+click: the row is toggled, the rest kept, and it becomes the anchor.
    Range,   ///< Shift+click: the rows from the anchor to this one are selected, in place of the rest.
};

/// @brief A backend's horizontal measure for a table's columns (`TableWidget::tableMeasure`).
struct TableMeasure {
    /// @brief The backend's measure of one `Sizing` unit: a fixed column of `n` units is `n * unit` wide.
    double unit = 1.0;
    /// @brief The measure of one monospace character, the unit of a fixed column whose role is `TextRole::Mono`.
    double monoUnit = 1.0;
    /// @brief The width a column that declares none starts from, in the backend's measure, before its cells measure.
    int contentEstimate = 100;
    /// @brief What a column takes beyond its content, in the backend's measure: added to a fixed column's width and to
    ///        a width the user set, so a column is as wide windowed as unwindowed (a graphical backend's cell padding
    ///        on both sides, a terminal's gap after the column).
    int padding = 0;
    /// @brief The narrowest a stretch column with `shrink` goes, padding included: a column whose header is narrower
    ///        keeps its header.
    int shrinkFloor = 8;
};

/// @brief What a backend measured of one table column (`TableWidget::setOnColumnMeasured`), in its own measure with
/// the
///        column's padding added. A width of 0 reports nothing of that kind.
struct ColumnMeasure {
    /// @brief The width of the column's header: its label, and the marks and button it draws.
    int header = 0;
    /// @brief The width a shown cell asks for.
    int natural = 0;
    /// @brief The narrowest a shown cell is laid out without losing content: a text's widest word, else `natural`.
    int least = 0;

    /// @brief Member-wise equality.
    /// @param other The measure to compare with.
    /// @return `true` when every member matches.
    [[nodiscard]] bool operator==(ColumnMeasure const& other) const = default;
};

/// @brief A key that moves a table's focused cell, which the mount resolves over rows and columns it may not have
///        realized.
enum class FocusMove : std::uint8_t {
    Up,        ///< One row up.
    Down,      ///< One row down.
    Left,      ///< One visible column left.
    Right,     ///< One visible column right.
    Home,      ///< The row's first visible column.
    End,       ///< The row's last visible column.
    First,     ///< The view's first row (Ctrl+Home).
    Last,      ///< The view's last row, or the last row known under a lower bound (Ctrl+End).
    PageUp,    ///< Up by the rows in view.
    PageDown,  ///< Down by the rows in view.
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
///
/// Every backend selects with the same inputs. In `Multiple` mode a click selects only its row and makes it the
/// anchor, Ctrl+click toggles its row and makes it the anchor, Shift+click selects the rows from the anchor to it in
/// place of the rest; Space toggles the current row, Shift+Up and Shift+Down move the current row and select from the
/// anchor to it, and Ctrl+A selects every row, or clears them all when every row is selected already. In `Single`
/// mode each of them selects the one row, and Ctrl+A goes on outwards. The arrows, Home, End, PageUp and PageDown
/// move the current row without selecting, and Enter activates it. A report says which kind of change it was: a
/// click, a Shift-click, a Shift+arrow and every `Single` change `Replace`; a Ctrl-click, Space and Ctrl+A `Extend`.
/// A range runs over the keys in row order, rows the backend has not realized included.
class MORPH_API TableWidget : public ContainerWidget {
public:
    /// @brief Sets the column headers and widths. A column's `unit` is drawn after its label, as `headerText` writes
    ///        it ("Mass [kg]") or on a second, muted line, and the header's accessible name is
    ///        `headerAccessibleName` ("Mass, kg").
    /// @param columns The columns, in order.
    virtual void setColumns(std::vector<TableColumn> const& columns) = 0;

    /// @brief Sets whether the header row is drawn.
    /// @param shown `false` suits several tables sharing one visual header.
    virtual void setShowHeader(bool shown) = 0;

    /// @brief Sets how many rows the user can select.
    /// @param mode None, one, or any number.
    virtual void setSelectionMode(SelectionMode mode) = 0;

    /// @brief Tells the table which key a row stands for; called once per row, right after it is built, and again
    ///        when a table over a `TableSource` gives a row it holds to another key. Marks the row
    ///        when its key is requested.
    /// @param row One of this table's rows.
    /// @param key The row's key.
    virtual void setRowKey(Widget& row, Key const& key) = 0;

    /// @brief Sets a row's tone; called after `setRowKey` for a row given a tone, and on every change.
    /// @param row  One of this table's rows.
    /// @param tone The tone, or none.
    virtual void setRowTone(Widget& row, std::optional<Tone> tone) = 0;

    /// @brief Draws the selection as a leading column of check boxes, one per row, under a header box; called once,
    ///        before any row, and only to turn it on: a new table has none. A row's box is checked while the row is
    ///        selected, and toggling it is a Ctrl-click on the row. The header box is checked while every row shown
    ///        is selected, partly checked while some are, clear while none are, and disabled while there are no
    ///        rows; toggling it is Ctrl+A. Neither box changes its own state: each shows the selection.
    /// @param shown Whether the column is drawn.
    virtual void setSelectColumn(bool shown) = 0;

    /// @brief Sets the accessible name of the select column's header box.
    /// @param label The name, UTF-8.
    virtual void setSelectAllLabel(std::string_view label) = 0;

    /// @brief Sets a row's accessible name, which its select box carries too; called after `setRowKey` for a row
    ///        given a label, and on every change. A row with none is named by its key.
    /// @param row   One of this table's rows.
    /// @param label The name, UTF-8.
    virtual void setRowLabel(Widget& row, std::string_view label) = 0;

    /// @brief Makes a row a heading; called once, right after `setRowKey`, for a heading row. Its one cell spans every
    ///        column and the select column. It is never selected (`setSelection` with its key marks nothing), never
    ///        reported, and never activated: a click on it does nothing, the current row skips it, and Ctrl+A, a
    ///        range and the select-all box pass over it.
    /// @param row One of this table's rows.
    virtual void setRowHeading(Widget& row) = 0;

    /// @brief Sets which of a row's cells cover the rows below it. Called for a row with spans once the change's rows
    ///        are placed, and again on every change, a call with an empty list clearing them; never for a row that
    ///        has never had one. The spans are resolved against the rows (no unknown column, overlap, heading or span
    ///        past the last row once a change settles), but a backend derives which cells are covered from the rows'
    ///        current order when it lays out or paints, never here, and clips a span that runs past its last row or
    ///        onto a heading row. A spanning cell is drawn once over the rows it covers, in its column, with the
    ///        look of this row; the covered rows' cells in that column are not shown. A span whose `columns` is above
    ///        1 also covers that many visible columns from its own, its own included, in each row it covers; the
    ///        backend clips it at the last visible column.
    /// @param row   One of this table's rows.
    /// @param spans The row's effective spans, by column, each column once.
    virtual void setRowSpans(Widget& row, std::vector<RowSpan> const& spans) = 0;

    /// @brief Makes a row a summary row; called once, right after `setRowKey`, for a summary row. It is drawn on the
    ///        theme's summary surface (`color.table.summary`), never selected, activated or focused for an edit, and
    ///        its cells are set as any row's. Pasting into it and copying it are the grid's. A row made a summary row
    ///        (or a heading) keeps that role for as long as it lives: the mount never gives it another key.
    /// @param row One of this table's rows.
    virtual void setRowSummary(Widget& row) { static_cast<void>(row); }

    /// @brief Requests the rows with these keys, and marks those it has. Never calls the `onSelectionChange` handler.
    /// @param keys The selected keys.
    virtual void setSelection(std::vector<Key> const& keys) = 0;

    /// @brief Sets what a user's selection change calls, with the keys of every row then selected and the kind of
    ///        change the input made.
    /// @param onSelectionChange The handler; empty does nothing.
    virtual void setOnSelectionChange(std::function<void(std::vector<Key>, SelectionChange)> onSelectionChange) = 0;

    /// @brief Sets what activating a row (Enter or a double click) calls, with its key.
    /// @param onActivate The handler; empty does nothing.
    virtual void setOnActivate(std::function<void(Key)> onActivate) = 0;

    /// @brief Marks the headers of the sort chain's columns. Each draws its key's mark after its label
    ///        (`sortMarkText`: the direction, and with two or more keys its place from 1) and carries its accessible
    ///        description (`sortDescription`); a header not in the chain draws no mark and has no description. The
    ///        table never reorders its rows: they arrive sorted. Never calls the `onSort` handler; the mount calls it
    ///        only for a table with a sortable column.
    /// @param chain The keys, most significant first; empty marks no header.
    virtual void setSort(std::vector<TableSort> const& chain) = 0;

    /// @brief Sets what a user's request to sort calls: a click on a sortable column's header (or Enter or Space on
    ///        it, where the header takes focus) calls it with the column's index among the columns `setColumns` gave,
    ///        which the select column is not one of, and `HeaderClick::Plain`; a `Shift`+click, `Shift+Enter` or
    ///        `Shift+Space` with `HeaderClick::Shift`. A header that is not sortable reports nothing. What the request
    ///        means is the caller's to decide; the backend only shows `setSort`.
    /// @param onSort The handler; empty does nothing.
    virtual void setOnSort(std::function<void(std::size_t, HeaderClick)> onSort) = 0;

    /// @brief Gives the headers and the table their menus. A header's menu opens on a right click on it, or on
    ///        `Shift+F10` or the context-menu key with it focused; the table's menu from a `⋯` button at the header
    ///        row's trailing end, whose accessible name is `Table options`. The entries are asked for each time a menu
    ///        opens. Called only for a table that has menus; a table never told this draws no `⋯` and opens none.
    /// @param entries  The entries of a column's menu (an index into the columns `setColumns` gave), or of the
    ///                 table's menu (none).
    /// @param onChoose Called with the menu's column, or none, and the chosen entry's path, an index per level from
    ///                 the top; never for a separator, a heading, a disabled entry or one with a submenu.
    virtual void setMenu(
        // NOLINTNEXTLINE(performance-unnecessary-value-param): overrides take `entries` by value to move it into their widget, and an override must match this signature; the default only discards it
        std::function<std::vector<MenuEntry>(std::optional<std::size_t>)> entries,
        // NOLINTNEXTLINE(performance-unnecessary-value-param): overrides take `onChoose` by value to move it into their widget, and an override must match this signature; the default only discards it
        std::function<void(std::optional<std::size_t>, std::vector<std::size_t>)> onChoose) {
        static_cast<void>(entries);
        static_cast<void>(onChoose);
    }

    /// @brief Gives the cells their menu. A right click, a long press, or `Shift+F10` or the context-menu key on the
    ///        focused cell opens it on that cell: the backend first makes the cell's row the selection alone, reported
    ///        as `Replace`, when the row is not selected already (a row already selected keeps the selection as it
    ///        is), then asks for the entries, which focuses the cell when it is not focused, and shows them at the
    ///        cell. While the table has the focus, a key the table does not take itself that is one of the chords
    ///        `setCellMenuKeys` gave asks for the focused cell's entries and is matched against their `keys` at every
    ///        level: the first enabled entry without a submenu whose chord it is runs, as if chosen from the menu, and
    ///        the key stops there. Any other key never asks for the entries.
    /// @param entries The entries of a cell's menu, given its row's key and its column (an index into the columns
    ///                `setColumns` gave); asked for each time the menu opens or a key is matched.
    /// @param chosen  Called with the cell's row and column and the chosen entry's path, an index per level from the
    ///                top; never for a separator, a heading, a disabled entry or one with a submenu.
    virtual void setCellMenu(
        std::function<std::vector<MenuEntry>(Key const& row, std::size_t column)> entries,
        std::function<void(Key const& row, std::size_t column, std::vector<std::size_t> const& path)> chosen) = 0;

    /// @brief The chords the cell menu's entries may declare, the only keys that ask for the focused cell's entries
    ///        (`setCellMenu`). Called for a table with a cell menu, once it is built; a table never told this runs no
    ///        entry from a key.
    /// @param chords The chords, as an entry's `keys` writes them.
    virtual void setCellMenuKeys(std::vector<std::string> const& chords) { static_cast<void>(chords); }

    /// @brief Tells the backend which columns are visible, in their places, so that a move and a group header count
    ///        places as the arrangement does. Called for a table over a `TableSource` once it is built and whenever
    ///        its visible columns change; a table never told this shows every column in its own order.
    /// @param visible Indices into the columns `setColumns` gave, in layout order.
    virtual void setVisibleColumns(std::vector<std::size_t> const& visible) { static_cast<void>(visible); }

    /// @brief Sets what moving a column calls: dragging a header onto another, or `Ctrl+Shift+Left` and
    ///        `Ctrl+Shift+Right` on a focused header, with the column and its new place among the visible columns
    ///        (one less, or one more, for the keys). A move to the place the column holds, or past either end,
    ///        reports nothing.
    /// @param handler The handler; empty does nothing.
    // NOLINTNEXTLINE(performance-unnecessary-value-param): overrides take `handler` by value to move it into their widget, and an override must match this signature; the default only discards it
    virtual void setOnColumnMove(std::function<void(std::size_t column, std::size_t position)> handler) {
        static_cast<void>(handler);
    }

    /// @brief Sets what dragging a header's trailing edge calls, with the column and its new width in `Sizing` units
    ///        (`tableMeasure`'s unit), or none for a double click on the edge. Called only for a table whose columns
    ///        can be resized; a backend that draws no edge to drag ignores it.
    /// @param handler The handler; empty does nothing.
    // NOLINTNEXTLINE(performance-unnecessary-value-param): overrides take `handler` by value to move it into their widget, and an override must match this signature; the default only discards it
    virtual void setOnColumnResize(std::function<void(std::size_t column, std::optional<int> units)> handler) {
        static_cast<void>(handler);
    }

    /// @brief Sets what a filterable column's filter button calls, with the column.
    /// @param handler The handler; empty does nothing.
    // NOLINTNEXTLINE(performance-unnecessary-value-param): overrides take `handler` by value to move it into their widget, and an override must match this signature; the default only discards it
    virtual void setOnFilter(std::function<void(std::size_t column)> handler) { static_cast<void>(handler); }

    /// @brief Fills the filter buttons of the columns that have a filter, and names their headers "{label},
    ///        filtered" for assistive technology; every other filterable column's button is drawn empty.
    /// @param columns Indices into the columns `setColumns` gave.
    virtual void setFiltered(std::vector<std::size_t> const& columns) { static_cast<void>(columns); }

    /// @brief Draws a group header row over the groups' columns, each group's caption over the visible places it
    ///        names, with its own filter button, filled while it is `filtered`; an empty list draws no group row.
    /// @param groups The groups, in order.
    virtual void setColumnGroups(std::vector<TableColumnGroup> const& groups) { static_cast<void>(groups); }

    /// @brief Draws the header bands above the column headers (and above a group row): each run over the visible
    ///        columns it names, at its level, its label followed by its unit in brackets when it has one; an empty
    ///        list draws none. Called once at mount and again whenever the runs change, as the column window or the
    ///        layout moves.
    /// @param runs The runs, outermost level first, each level left to right.
    virtual void setBands(std::vector<BandRun> const& runs) { static_cast<void>(runs); }

    /// @brief Sets what a group's filter button calls, with the group's index among those `setColumnGroups` gave.
    /// @param handler The handler; empty does nothing.
    // NOLINTNEXTLINE(performance-unnecessary-value-param): overrides take `handler` by value to move it into their widget, and an override must match this signature; the default only discards it
    virtual void setOnGroupFilter(std::function<void(std::size_t group)> handler) { static_cast<void>(handler); }

    /// @brief Sets what `Ctrl+C` on the focused table calls.
    /// @param onCopy The handler; empty lets the key go on.
    // NOLINTNEXTLINE(performance-unnecessary-value-param): overrides take `onCopy` by value to move it into their widget, and an override must match this signature; the default only discards it
    virtual void setOnCopy(std::function<void()> onCopy) { static_cast<void>(onCopy); }

    /// @brief Tells the backend which contiguous range of view rows is realized and how many rows exist in total, so
    ///        it can size a scrollbar and place leading/trailing space for the rows that are not realized. Called
    ///        after each pass for every table over a `TableSource` (one that realizes every row with the whole view,
    ///        `(0, count, count)`), and for a table over `rows` at or above its `Virtualization::threshold`, whether
    ///        or not the backend reports a range. The focused row a source table keeps realized beyond the range is
    ///        not counted: `setRowIndex` places it.
    /// @param first The view index of the first row of the range.
    /// @param count How many rows the range holds, from `first`.
    /// @param total How many rows the table's data has, realized or not.
    virtual void setRealizedWindow(std::size_t first, std::size_t count, std::size_t total);

    /// @brief Sets what the backend calls when the user scrolls, asking the mount to realize roughly this range of
    ///        rows next. A backend that never virtualizes leaves this empty; the mount then never dewindows a row.
    /// @param onVisibleRangeChanged The handler, with the first view index and how many rows are wanted from there;
    ///        empty does nothing.
    virtual void setOnVisibleRangeChanged(std::function<void(std::size_t, std::size_t)> onVisibleRangeChanged) {
        static_cast<void>(onVisibleRangeChanged);
    }

    /// @brief Tells the backend every key currently in the table's data, realized or not -- called whenever that
    ///        set changes (a data change, or a scroll that dewindows or re-realizes a row). A virtualized backend
    ///        needs this to know a selected key still exists in the data when it has no realized row for it right
    ///        now (`setRowKey` alone cannot say that, since it is only ever called for a realized row): without
    ///        it, a backend that reconstructs "does this key still exist" purely from its realized children would
    ///        silently drop a selected key's membership the moment its row scrolls out of view. A backend that
    ///        never virtualizes may ignore this -- every key it cares about already has a realized row, by
    ///        definition, so `setRowKey` alone already tells it everything this call would add.
    /// @param keys Every key currently in the table's data, in row order, so a range or Ctrl+A covers the rows the
    ///        backend has not realized.
    virtual void setExistingKeys(std::vector<Key> const& keys) { static_cast<void>(keys); }

    /// @brief Sets what a `Multiple` table's `Ctrl+A` calls in place of selecting the rows the backend holds and
    ///        reporting them: the caller selects the whole view, and sets the selection back through `setSelection`.
    ///        A backend that does not take it selects as before.
    /// @param onSelectAll The handler; empty to select as before.
    // NOLINTNEXTLINE(performance-unnecessary-value-param): overrides take `onSelectAll` by value to move it into their widget, and an override must match this signature; the default only discards it
    virtual void setOnSelectAll(std::function<void()> onSelectAll) { static_cast<void>(onSelectAll); }

    /// @brief Marks every row of the table selected but those whose key is in @p except, rows the backend does not
    ///        hold included, until the next `setSelection`. In that state a `Ctrl`+click reports `Extend` with the
    ///        keys now selected among the rows the backend holds, so the caller reads each held key absent from the
    ///        report as left out and each present one as selected again; a plain click reports `Replace` and ends the
    ///        state. A backend that takes `setOnSelectAll` must take this too: the caller selects the view through it,
    ///        and a backend that kept drawing its key selection would report every other shown row as left out.
    /// @param except The keys left out of the selection.
    virtual void setSelectionView(std::vector<Key> const& except) { static_cast<void>(except); }

    /// @brief Sets what choosing one of the status line's entries (`TableStatus::entries` and `standingEntries`, each
    ///        drawn as a button with its label) calls, with the entry's id.
    /// @param onStatusEntry The handler; empty does nothing.
    // NOLINTNEXTLINE(performance-unnecessary-value-param): overrides take `onStatusEntry` by value to move it into their widget, and an override must match this signature; the default only discards it
    virtual void setOnStatusEntry(std::function<void(std::string entryId)> onStatusEntry) {
        static_cast<void>(onStatusEntry);
    }

    /// @brief Sets a row's marks; called after `setRowKey` for a row whose look carries marks, and on every change.
    ///        An `unmatched` row draws muted, a `busy` one with the busy mark, a `locked` one with the locked mark,
    ///        and `tooltip` is the row's tooltip; `RowMarks{}` draws an unmarked row. A backend that marks no row
    ///        ignores it.
    /// @param row   One of this table's rows.
    /// @param marks The marks.
    virtual void setRowMarks(Widget& row, RowMarks const& marks) {
        static_cast<void>(row);
        static_cast<void>(marks);
    }

    /// @brief Shows the table's status line: its counts, the state of a failed fetch or level with "Retry" when it
    ///        offers it, and the latest message; a status that is not `shown` takes no space. Called once the table
    ///        is built and on every change. A backend without a status line ignores it.
    /// @param status The status line.
    virtual void setStatus(TableStatus const& status) { static_cast<void>(status); }

    /// @brief Sets what choosing the status line's "Retry" calls.
    /// @param onRetry The handler; empty does nothing.
    // NOLINTNEXTLINE(performance-unnecessary-value-param): overrides take `onRetry` by value to move it into their widget, and an override must match this signature; the default only discards it
    virtual void setOnRetry(std::function<void()> onRetry) { static_cast<void>(onRetry); }

    /// @brief Sets a cell's load state; called after the row's key, and on every change, for a table whose rows come
    ///        from a `TableSource`. `Unloaded` draws a placeholder of a fixed width in the column's alignment and
    ///        `Failed` an error mark of the same width, over the cell's own content; neither widens the column.
    ///        A cell no call names is `Loaded`.
    /// @param row    One of this table's rows.
    /// @param column The cell's column: an index into the columns `setColumns` gave.
    /// @param load   The state.
    virtual void setCellLoad(Widget& row, std::size_t column, CellLoad load) {
        static_cast<void>(row);
        static_cast<void>(column);
        static_cast<void>(load);
    }

    /// @brief Sets what a cell shows beside its text; called after the row's key, on every change, and whenever a
    ///        cell is attached to a row at a column, for a table whose rows come from a `TableSource`. A `tone` tints
    ///        the cell with the theme's tone surface under its selection and focus marks; a `tooltip` shows on hover
    ///        after the theme's delay and on keyboard focus at once, is the cell's accessible description, and draws
    ///        the theme's "more here" mark; `invalid` draws the theme's error mark in place of the cell's text. A
    ///        cell no call names has no accent.
    /// @param row    One of this table's rows.
    /// @param column The cell's column: an index into the columns `setColumns` gave.
    /// @param accent The accent.
    virtual void setCellAccent(Widget& row, std::size_t column, CellAccent const& accent) {
        static_cast<void>(row);
        static_cast<void>(column);
        static_cast<void>(accent);
    }

    /// @brief Tells the backend which columns the mount realizes, for a table whose rows come from a `TableSource`
    ///        and whose columns the mount lays out: once the backend reported its horizontal viewport, or the
    ///        arrangement is not the default. The header shows these columns only, each at its offset and width; the
    ///        first @p pinned stay in view whatever the horizontal scroll. Called again whenever any of it changes.
    /// @param columns    Indices into the columns `setColumns` gave: the pinned ones first, then the scrolled range.
    /// @param offsets    Each column's offset from the table's left edge, as if every visible column were laid out,
    ///                   in the backend's measure (`tableMeasure`).
    /// @param widths     Each column's width, in the backend's measure.
    /// @param totalWidth The width of every visible column together, in the backend's measure, which sizes the
    ///                   horizontal scroll bar.
    /// @param pinned     How many of @p columns are pinned.
    virtual void setColumnWindow(
        // NOLINTNEXTLINE(bugprone-easily-swappable-parameters): `offsets` and `widths` are the window's two parallel int spans, and the mount builds both with `columns` from one window and passes them in this order
        std::span<std::size_t const> columns, std::span<int const> offsets,
        // NOLINTNEXTLINE(bugprone-easily-swappable-parameters): `totalWidth` is a width and `pinned` a column count; the one call site in the mount passes the extents' total first, then the pinned count
        std::span<int const> widths, int totalWidth, std::size_t pinned) {
        static_cast<void>(columns);
        static_cast<void>(offsets);
        static_cast<void>(widths);
        static_cast<void>(totalWidth);
        static_cast<void>(pinned);
    }

    /// @brief Sets what the backend calls when its horizontal scroll or viewport changes, so the mount can window the
    ///        columns. A backend that never calls it gets every visible column realized.
    /// @param handler The handler, with the scroll offset and the viewport's width, both in the backend's measure
    ///        (`tableMeasure`); empty does nothing.
    // NOLINTNEXTLINE(performance-unnecessary-value-param): overrides take `handler` by value to move it into their widget, and an override must match this signature; the default only discards it
    virtual void setOnHorizontalRange(std::function<void(int scrollX, int viewport)> handler) {
        static_cast<void>(handler);
    }

    /// @brief Moves the horizontal scroll offset without reporting it back: the mount keeps the columns in view where
    ///        they were when a column left of them grew.
    /// @param scrollX The offset, in the backend's measure.
    virtual void setHorizontalOffset(int scrollX) { static_cast<void>(scrollX); }

    /// @brief Sets what the backend calls for a range gesture (Shift+click, Shift+arrow) over rows it does not hold
    ///        keys for, instead of resolving the range itself.
    /// @param handler The handler, with the view rows of the range's anchor and end, and the kind of change; empty
    ///        does nothing.
    virtual void setOnSelectRange(
        // NOLINTNEXTLINE(performance-unnecessary-value-param): overrides take `handler` by value to move it into their widget, and an override must match this signature; the default only discards it
        std::function<void(std::size_t anchorRow, std::size_t focusRow, SelectionChange)> handler) {
        static_cast<void>(handler);
    }

    /// @brief Shows keyboard focus on a cell, and announces it; a row not fetched yet is announced as loading.
    /// @param viewRow The cell's view row.
    /// @param column  The cell's column: an index into the columns `setColumns` gave.
    // NOLINTNEXTLINE(bugprone-easily-swappable-parameters): (`viewRow`, `column`): the row position then the column, the pair every table seam in this header passes in this order
    virtual void setFocusCell(std::size_t viewRow, std::size_t column) {
        static_cast<void>(viewRow);
        static_cast<void>(column);
    }

    /// @brief Sets what the backend calls with the widths a column's header and cells measured, so the mount can grow
    /// a
    ///        content column and fit the stretch columns to the table. A report only ever adds to what the mount
    ///        knows: it keeps the largest of each width reported.
    /// @param handler The handler, with the column (an index into the columns `setColumns` gave) and what was
    /// measured,
    ///        in the backend's measure (`tableMeasure`), padding included; empty does nothing.
    // NOLINTNEXTLINE(performance-unnecessary-value-param): overrides take `handler` by value to move it into their widget, and an override must match this signature; the default only discards it
    virtual void setOnColumnMeasured(std::function<void(std::size_t column, ColumnMeasure const& measure)> handler) {
        static_cast<void>(handler);
    }

    /// @brief Sets what the backend calls for a key that moves the focused cell, which the mount resolves over rows
    ///        and columns it may not have realized.
    /// @param handler The handler, with the move; empty does nothing.
    // NOLINTNEXTLINE(performance-unnecessary-value-param): overrides take `handler` by value to move it into their widget, and an override must match this signature; the default only discards it
    virtual void setOnFocusMove(std::function<void(FocusMove)> handler) { static_cast<void>(handler); }

    /// @brief Sets what printable text typed while a cell has focus and no editor is open calls, with the text.
    /// @param onText The handler; empty does nothing and lets the key go on.
    // NOLINTNEXTLINE(performance-unnecessary-value-param): overrides take `onText` by value to move it into their widget, and an override must match this signature; the default only discards it
    virtual void setOnText(std::function<void(std::string)> onText) { static_cast<void>(onText); }

    /// @brief Sets what a click on a cell calls: a single click with `false`, the second click of a double click with
    ///        `true` (its first click calls it with `false`, as a single click does).
    /// @param onClick The handler, with the cell's view row and its column (an index into the columns `setColumns`
    ///        gave); empty does nothing.
    virtual void setOnCellClick(
        // NOLINTNEXTLINE(performance-unnecessary-value-param): overrides take `onClick` by value to move it into their widget, and an override must match this signature; the default only discards it
        std::function<void(std::size_t viewRow, std::size_t column, bool doubleClick)> onClick) {
        static_cast<void>(onClick);
    }

    /// @brief Sets what focus moving to a widget outside the table, its cell editor and its popups calls.
    /// @param onFocusOut The handler; empty does nothing.
    // NOLINTNEXTLINE(performance-unnecessary-value-param): overrides take `onFocusOut` by value to move it into their widget, and an override must match this signature; the default only discards it
    virtual void setOnFocusOut(std::function<void()> onFocusOut) { static_cast<void>(onFocusOut); }

    /// @brief Draws a cell editor over a cell's area in place of the cell's content, or the cell again.
    /// @param row    One of this table's rows; @p editor is one of its children.
    /// @param column The cell's column: an index into the columns `setColumns` gave.
    /// @param editor The editor, or null to draw the cell again.
    virtual void setCellEditor(Widget& row, std::size_t column, Widget* editor) {
        static_cast<void>(row);
        static_cast<void>(column);
        static_cast<void>(editor);
    }

    /// @brief Draws a rectangle of selected cells in place of a row selection, or none. Never calls the handler
    ///        `setOnCellSelect` gave.
    /// @param rect `{firstRow, rowCount, firstPosition, positionCount}`: view rows, and places among the visible
    /// columns
    ///        in layout order (a rectangle is of the layout, so these are positions, not indices into the columns
    ///        `setColumns` gave); empty draws none.
    virtual void setCellSelection(std::optional<std::array<std::size_t, 4>> rect) { static_cast<void>(rect); }

    /// @brief Makes the table select cells, not rows: it draws no row selection and reports no `onSelectionChange`.
    ///        It calls the handler with the rectangle a drag from cell to cell or a `Shift`+click selects, a row
    ///        header's click (anchor `(row, 0)`, focus `(row, last position)`) and a column header's selection gesture
    ///        (anchor
    ///        `(0, position)`, focus `(last row, position)`). `Shift` with an arrow and `Ctrl+A` are the table's own
    ///        chords, declared with `setKeys`, not the backend's.
    /// @param onSelect The handler, with the anchor's and the focus's view rows and visible positions; empty selects
    ///        rows again.
    virtual void setOnCellSelect(
        std::function<void(std::size_t anchorRow, std::size_t anchorPosition, std::size_t focusRow,
                           std::size_t focusPosition)>
            // NOLINTNEXTLINE(performance-unnecessary-value-param): overrides take `onSelect` by value to move it into their widget, and an override must match this signature; the default only discards it
            onSelect) {
        static_cast<void>(onSelect);
    }

    /// @brief Sets a cell's marks; called after the row's key, on every change, and whenever a cell is attached to a
    ///        row at a column, for a table whose rows come from a `TableSource`. `readOnly` draws the theme's
    ///        read-only cell surface, `modified` the modified mark, `error` the error mark, `stale` and `muted` the
    ///        stale and muted looks. A cell no call names has no marks.
    /// @param row    One of this table's rows.
    /// @param column The cell's column: an index into the columns `setColumns` gave.
    /// @param marks  The marks.
    virtual void setCellMarks(Widget& row, std::size_t column, CellMarks const& marks) {
        static_cast<void>(row);
        static_cast<void>(column);
        static_cast<void>(marks);
    }

    /// @brief Sets whether a row draws the row header's modified mark; called after `setRowKey` for a row whose look
    ///        carries it, and on every change.
    /// @param row      One of this table's rows.
    /// @param modified Whether a cell of the row changed since the record was loaded.
    virtual void setRowModified(Widget& row, bool modified) {
        static_cast<void>(row);
        static_cast<void>(modified);
    }

    /// @brief Tells the backend the counts it reports to assistive technology.
    /// @param rows    The view's total when it is exact; nothing when it is a lower bound or not known yet.
    /// @param columns The visible columns.
    virtual void setCounts(std::optional<std::int64_t> rows, std::size_t columns) {
        static_cast<void>(rows);
        static_cast<void>(columns);
    }

    /// @brief Scrolls a view row into view.
    /// @param viewRow The row.
    virtual void scrollToRow(std::size_t viewRow) { static_cast<void>(viewRow); }

    /// @brief Marks one row as the one a linked view highlights, or none; neither selection nor focus.
    /// @param viewRow The row's view index; nothing clears the mark.
    virtual void setHighlightedRow(std::optional<std::size_t> viewRow) { static_cast<void>(viewRow); }

    /// @brief Places a cell of a row in a column, for a table whose rows come from a `TableSource`: called when a cell
    ///        joins its row's cells and when a pooled cell is given another column, and with nothing when the cell
    ///        leaves the column window or waits for its turn to be bound. A cell with no column is pooled: it is not
    ///        drawn and takes no place, and the column it left draws blank at its width until a cell is placed
    ///        there.
    /// @param row    One of this table's rows.
    /// @param cell   One of @p row's children.
    /// @param column An index into the columns `setColumns` gave, or nothing.
    // NOLINTNEXTLINE(bugprone-easily-swappable-parameters): (`row`, `cell`): the table's realized row then its child cell, which both call sites in the mount pass in that order
    virtual void setCellColumn(Widget& row, Widget& cell, std::optional<std::size_t> column) {
        static_cast<void>(row);
        static_cast<void>(cell);
        static_cast<void>(column);
    }

    /// @brief Tells the backend a realized row's view index, for a table whose rows come from a `TableSource`: called
    ///        for a row once it is realized and whenever its index changes. The realized rows are the children in
    ///        view order, but not always contiguous: the focused row stays realized beyond `setRealizedWindow`'s
    ///        range, so a backend places each row by its index, not by its position among the children.
    /// @param row     One of this table's rows.
    /// @param viewRow The row's view index.
    virtual void setRowIndex(Widget& row, std::size_t viewRow) {
        static_cast<void>(row);
        static_cast<void>(viewRow);
    }

    /// @brief How this backend measures a table's widths, for a table whose rows come from a `TableSource`. The
    ///        horizontal scroll and viewport it reports, the widths it measures and the offsets and widths
    ///        `setColumnWindow` gives it are all in its own measure (pixels for a graphical backend, cells for a
    ///        terminal); the mount turns a column's `Sizing::fixed` amount, and a width the user set, into that
    ///        measure with these factors. The default measure is one per unit, with a content column estimated at 100.
    /// @return The measure.
    [[nodiscard]] virtual TableMeasure tableMeasure() const { return {}; }
};

/// @brief A date or date-and-time field, in the mode and display zone fixed at creation.
class DateTimeInputWidget : public FieldWidget {
public:
    /// @brief Shows an instant. Never calls the `onChange` handler.
    /// @param value The instant; `nullopt` or an empty `Timestamp` shows an empty field.
    virtual void setValue(std::optional<morph::time::Timestamp> const& value) = 0;

    /// @brief Sets what a user edit calls, with the instant entered or `nullopt` for a cleared field.
    /// @param onChange The handler; empty does nothing.
    virtual void setOnChange(std::function<void(std::optional<morph::time::Timestamp>)> onChange) = 0;

    /// @brief Sets the pattern the field shows its value in and reads typed text in (`formatDate`, `parseDate`), ISO
    ///        being always read too. The mount calls it before the value when the pattern is not empty, and again
    ///        each time the session's pattern changes, an empty one included; the field then shows the value it holds
    ///        in the new pattern. A new field shows ISO.
    /// @param pattern The pattern, such as `dd-MMM-yyyy HH:mm`.
    virtual void setPattern(std::string_view pattern) = 0;

    /// @brief Sets the field's label, and whether the input draws it as a visible caption above its control.
    ///        Without `shown`, the caption is drawn elsewhere (a form's cell), or not at all.
    /// @param label The label, UTF-8.
    /// @param shown Whether the input shows it.
    virtual void setCaption(std::string_view label, bool shown) = 0;
};

/// @brief An integer on a range.
class SliderWidget : public FieldWidget {
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

    /// @brief Sets what a finished move calls, with the value it ended on: a release after a drag, a key step, or a
    ///        wheel step.
    /// @param onCommit The handler; empty does nothing.
    virtual void setOnCommit(std::function<void(std::int64_t)> onCommit) = 0;
};

/// @brief A file path field, in the mode fixed at creation.
class FilePickerWidget : public FieldWidget {
public:
    /// @brief Shows a path. Never calls the `onPicked` handler.
    /// @param path The path, UTF-8.
    virtual void setPath(std::string_view path) = 0;

    /// @brief Sets what a user pick calls, with the path.
    /// @param onPicked The handler; empty does nothing.
    virtual void setOnPicked(std::function<void(std::string)> onPicked) = 0;
};

/// @brief A message across the top of a region, with an optional action button and an optional dismissal.
class BannerWidget : public Widget {
public:
    /// @brief Sets what the message conveys.
    /// @param tone The tone, mapped to the backend's theme.
    virtual void setTone(Tone tone) = 0;

    /// @brief Replaces the message.
    /// @param text The message, UTF-8.
    virtual void setText(std::string_view text) = 0;

    /// @brief Replaces the message with runs, as `TextWidget::setSpans` does. The mount calls it in place of
    ///        `setText` for a banner that has spans.
    /// @param spans The runs.
    virtual void setSpans(std::vector<TextSpan> const& spans) = 0;

    /// @brief Replaces the action button's caption.
    /// @param label The caption, UTF-8; empty shows no button.
    virtual void setActionLabel(std::string_view label) = 0;

    /// @brief Sets what activating the action button calls.
    /// @param onAction The handler; empty does nothing.
    virtual void setOnAction(Action onAction) = 0;

    /// @brief Sets whether the user can dismiss the banner; called once, before the banner is shown.
    /// @param dismissible Whether to offer the dismissal.
    virtual void setDismissible(bool dismissible) = 0;

    /// @brief Sets what a dismissal calls. A dismissal leaves the banner shown: the application hides it.
    /// @param onDismiss The handler; empty does nothing.
    virtual void setOnDismiss(Action onDismiss) = 0;

    /// @brief Sets how the action is drawn. The mount calls it once, only for a banner whose style is not `Ghost`
    ///        or that stretches its action. A `Link` draws the action inside the message, at the span marked
    ///        `action` or else after the message, and stays a button to assistive technology.
    /// @param style   How the action is drawn.
    /// @param stretch Whether the action takes a line of its own under the message, as wide as the banner; never
    ///                true with `Link`.
    virtual void setActionStyle(BannerActionStyle style, bool stretch) = 0;
};

/// @brief A passive, transient message outside the normal flow: non-modal, dims nothing, and never takes keyboard
/// focus. Auto-dismisses after a backend-chosen interval.
class ToastWidget : public Widget {
public:
    /// @brief Shows the message, replacing any shown or dismissing, and (re)starts the dismissal interval regardless
    ///        of whether the toast is already on-screen — a second call while one is showing replaces its text and
    ///        tone and restarts the interval, rather than being a no-op or stacking a second message.
    /// @param text The message, UTF-8.
    /// @param tone The tone, mapped to the backend's theme.
    virtual void show(std::string_view text, Tone tone) = 0;
};

/// @brief A short label marking a state.
class BadgeWidget : public Widget {
public:
    /// @brief Sets what the mark conveys.
    /// @param tone The tone, mapped to the backend's theme.
    virtual void setTone(Tone tone) = 0;

    /// @brief Replaces the label.
    /// @param text The label, UTF-8.
    virtual void setText(std::string_view text) = 0;

    /// @brief Sets the icon; called once.
    /// @param icon The name of an icon in the theme's icon map; empty shows none.
    virtual void setIcon(std::string_view icon) = 0;

    /// @brief Makes the badge removable: it draws a remove control after its text, drawn as the framework icon
    ///        `close`, and takes the focus. Delete, Backspace, Enter or Space on it, or a click on its control, calls
    ///        @p onRemove. The mount calls it only for a badge with a remove handler, at mount and whenever the
    ///        label changes; a badge it is never called on takes no focus.
    /// @param onRemove The handler.
    /// @param label    The remove control's accessible name.
    // NOLINTNEXTLINE(performance-unnecessary-value-param): a sink every override moves from.
    virtual void setOnRemove(Action onRemove, std::string_view label) = 0;

    /// @brief Sets how the badge is drawn. A new badge is filled; the mount calls it once, only for `Outline`, which
    ///        draws the surface's fill with the tone's colour as border and text, or `Quiet`, which draws the
    ///        surface's fill with the control border and the muted text colour.
    /// @param style The style.
    virtual void setStyle(BadgeStyle style) = 0;

    /// @brief Sets the badge's outline. A new badge has the theme's corners; the mount calls it once, only for
    ///        `Round`.
    /// @param shape The shape.
    virtual void setShape(ControlShape shape) = 0;

    /// @brief Draws the label in the `mono` text role's face. A new badge uses its own face; the mount calls it once,
    ///        only for `true`.
    /// @param mono Whether to.
    virtual void setMono(bool mono) = 0;
};

/// @brief A progress bar.
class ProgressWidget : public Widget {
public:
    /// @brief Sets how far along it is.
    /// @param value From 0 to 1, a value outside shown at the nearer end; `nullopt` shows an indeterminate bar.
    virtual void setValue(std::optional<double> value) = 0;

    /// @brief Replaces the caption beside the bar.
    /// @param label The caption, UTF-8.
    virtual void setLabel(std::string_view label) = 0;
};

/// @brief The steps of a process, each with its state.
class StepsWidget : public Widget {
public:
    /// @brief Replaces the steps.
    /// @param steps The steps, in order.
    virtual void setSteps(std::vector<Step> const& steps) = 0;

    /// @brief Highlights one step.
    /// @param index The step's position; `nullopt` or a position past the last highlights none.
    virtual void setCurrent(std::optional<std::size_t> index) = 0;

    /// @brief Chooses whether the labels are drawn; unlabelled, only the marks are, and the labels still name the
    ///        steps and the indicator.
    /// @param labelled Whether the labels are drawn.
    virtual void setLabelled(bool labelled) = 0;
};

/// @brief Captions beside, or above, their values. A toned value is drawn in the tone's colour and the strong weight;
///        a value with spans draws them as a text does.
class KeyValueWidget : public Widget {
public:
    /// @brief Replaces the pairs.
    /// @param items The pairs, in order.
    virtual void setItems(std::vector<KeyValueItem> const& items) = 0;

    /// @brief Sets where each caption sits. A new list draws them beside their values; the mount calls it once, only
    ///        for `Above`.
    /// @param placement Where the captions sit.
    virtual void setCaptionPlacement(CaptionPlacement placement) = 0;

    /// @brief Sets the width of the caption column, in units, while the captions sit beside their values; the mount
    ///        calls it once, only when the node gives a width. Without it the column is as wide as the widest caption.
    ///        A caption longer than the column wraps where the backend can and is cut with an ellipsis where it
    ///        cannot.
    /// @param units The column's width in units, at least 1.
    virtual void setCaptionWidth(int units) = 0;
};

/// @brief Points against a numeric axis, with reference lines. It takes no focus and calls no handler.
class ChartWidget : public Widget {
public:
    /// @brief Replaces what the chart draws. Called at mount and whenever the data changes; an unchanged chart is
    ///        not set again.
    /// @param data The range, the ticks, the lines, the series and the summary, every text already formatted.
    virtual void setData(ChartData const& data) = 0;
};

/// @brief What a region shows when it has nothing to show.
class EmptyStateWidget : public Widget {
public:
    /// @brief Replaces the title.
    /// @param title The title, UTF-8.
    virtual void setTitle(std::string_view title) = 0;

    /// @brief Replaces the explanation.
    /// @param text The explanation, UTF-8.
    virtual void setText(std::string_view text) = 0;

    /// @brief Sets the icon; called once.
    /// @param icon The name of an icon in the theme's icon map; empty shows none.
    virtual void setIcon(std::string_view icon) = 0;

    /// @brief Replaces the action button's caption.
    /// @param label The caption, UTF-8; empty shows no button.
    virtual void setActionLabel(std::string_view label) = 0;

    /// @brief Sets what activating the action button calls.
    /// @param onAction The handler; empty does nothing.
    virtual void setOnAction(Action onAction) = 0;
};

/// @brief A panel over the content that slides in from the side fixed at creation; it follows a dialog's rules. A new
///        drawer is closed.
class DrawerWidget : public ContainerWidget {
public:
    /// @brief Shows or hides it. Never calls the `onDismiss` handler.
    /// @param open Whether it is shown.
    virtual void setOpen(bool open) = 0;

    /// @brief Replaces the title.
    /// @param title The title, UTF-8.
    virtual void setTitle(std::string_view title) = 0;

    /// @brief Shows a line under the title, as `DialogWidget::setDescription` does.
    /// @param description The line, UTF-8; empty shows none.
    virtual void setDescription(std::string_view description) = 0;

    /// @brief Shows the description as runs, as `DialogWidget::setDescriptionSpans` does.
    /// @param spans The runs.
    virtual void setDescriptionSpans(std::vector<TextSpan> const& spans) = 0;

    /// @brief Sets what a user dismissal (Esc on the TUI) calls.
    /// @param onDismiss The handler; empty does nothing.
    virtual void setOnDismiss(Action onDismiss) = 0;

    /// @brief The drawer's footer: a container it lays out after its content, which shows nothing while it is empty.
    ///        The mount puts the drawer's actions here.
    /// @return It, owned by the drawer and valid as long as the drawer.
    virtual ContainerWidget& footer() = 0;

    /// @brief Sets the label of the dismiss control `setCloseButton` draws; a new drawer's is "Close". The mount
    ///        calls this only when the label is bound or not the default.
    /// @param label The label, UTF-8.
    virtual void setDismissLabel(std::string_view label) = 0;

    /// @brief Shows a short muted note on the title's row, against the trailing edge. The mount calls it only when
    ///        the note is bound or not empty.
    /// @param note The note, UTF-8; empty shows none.
    virtual void setHeadNote(std::string_view note) = 0;

    /// @brief Shows a short muted note leading in the footer's row, opposite the actions. The footer shows while
    ///        the note is not empty, though it holds no action. The mount calls it only when the note is bound or
    ///        not empty.
    /// @param note The note, UTF-8; empty shows none.
    virtual void setFooterNote(std::string_view note) = 0;

    /// @brief Draws a dismiss control (labelled by `setDismissLabel`) on the title's row, against the trailing edge,
    ///        which dismisses exactly as Escape does.
    ///        The mount calls it only when the condition is bound or true.
    /// @param shown Whether the control is drawn.
    virtual void setCloseButton(bool shown) = 0;
};

/// @brief A non-modal panel that belongs to another widget: the results of a search under its field. It takes no
///        keyboard focus, dims nothing and traps nothing, so focus stays where it was.
///
/// It is anchored under its parent container's box and aligned to the box's leading edge. A backend with a floating
/// layer draws it over the content and gives it no room in the parent's layout; a backend without one draws its
/// children in flow, under the parent's other children. A new popover is closed.
class PopoverWidget : public ContainerWidget {
public:
    /// @brief Shows or hides it. Never calls the `onDismiss` handler.
    /// @param open Whether it is shown.
    virtual void setOpen(bool open) = 0;

    /// @brief Sets its width.
    /// @param units The width in units; 0 is the parent's width.
    virtual void setWidth(int units) = 0;

    /// @brief Sets what a user dismissal calls: a press outside both the panel and its parent, where the backend
    ///        reports one. The panel takes no keyboard focus, so Escape reaches the widgets that have it, not this.
    /// @param onDismiss The handler; empty does nothing.
    virtual void setOnDismiss(Action onDismiss) = 0;
};

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
/// it always does; while focus is on the ringed widget or a popup it opened, that control takes the key first and the
/// chord runs only for a key it left alone. A pointer press outside the ringed widget's box and the card reaches
/// nothing; one inside the ringed widget's box reaches the ringed widget.
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

/// @brief Panes along the axis fixed at creation, with handles between them that the user drags.
class SplitterWidget : public ContainerWidget {
public:
    /// @brief Sets each pane's size. Never calls the `onResize` handler.
    /// @param sizes Each pane's size, in backend units, in order; a list that is empty or whose length differs from
    ///              the number of panes shares the space equally.
    virtual void setSizes(std::vector<int> const& sizes) = 0;

    /// @brief Sets what dragging a handle calls, with every pane's size once the user lets go.
    /// @param onResize The handler; empty does nothing.
    virtual void setOnResize(std::function<void(std::vector<int>)> onResize) = 0;
};

/// @brief A titled section the user opens and closes. A new collapsible is open.
///
/// A widget inside it takes no input while it is closed, except the header and everything inside the header.
class CollapsibleWidget : public ContainerWidget {
public:
    /// @brief Replaces the title.
    /// @param title The title, UTF-8.
    virtual void setTitle(std::string_view title) = 0;

    /// @brief Shows one of its children beside the title, open or closed; called at most once.
    /// @param header A child of this widget.
    virtual void setHeader(Widget& header) = 0;

    /// @brief Opens or closes it. Never calls the `onToggle` handler.
    /// @param open Whether the content is shown.
    virtual void setOpen(bool open) = 0;

    /// @brief Sets what the user opening or closing it calls.
    /// @param onToggle The handler, given the state the user asked for; empty does nothing.
    virtual void setOnToggle(std::function<void(bool)> onToggle) = 0;
};

/// @brief A sheet of fixed physical size (docs/spec/ui/document_page.md). Its children are
///        `PageRegionWidget`s. The geometry is a factory argument; the page reads the width its container gives it
///        from the container's layout, asks for the sheet's width as its natural width, no least width, and a height
///        that follows the width it is given, and displays the sheet scaled by `min(1, width / sheet width)`.
class PageWidget : public ContainerWidget {};

/// @brief A rectangle of a page that holds its children as a vertical stack, clipped to the rectangle. The geometry
///        is a factory argument. A region whose content does not fit draws an overflow mark.
class PageRegionWidget : public ContainerWidget {};

/// @brief A region that takes files dropped from the operating system.
class DropZoneWidget : public ContainerWidget {
public:
    /// @brief Sets the extensions it takes; called once.
    /// @param extensions Such as `".csv"`, compared without regard to case; empty takes every file.
    virtual void setAccept(std::vector<std::string> const& extensions) = 0;

    /// @brief Sets whether a drop may carry more than one file; called once.
    /// @param multiple Whether several files are taken at once.
    virtual void setMultiple(bool multiple) = 0;

    /// @brief Sets what a drop it takes calls. A drop with a file outside the accepted extensions, or with several
    ///        files when only one is taken, is refused whole and calls nothing.
    /// @param onDrop The handler, given the paths in the order dropped; empty does nothing.
    virtual void setOnDrop(std::function<void(std::vector<std::string>)> onDrop) = 0;
};

/// @brief A component a renderer loaded in a `custom` node's place (spec 5 §10): a bundle's verified QML or a host
///        component, given the props the document binds and handing back the events it raises.
class CustomWidget : public Widget {
public:
    /// @brief Sets a text prop: `string`, and `int`, `exact` and `json`, which cross as text so that no value loses
    ///        precision.
    /// @param name  The prop.
    /// @param value Its text.
    // NOLINTNEXTLINE(bugprone-easily-swappable-parameters): a prop's name, then its value, as every setter takes them.
    virtual void setTextProp(std::string_view name, std::string_view value) = 0;

    /// @brief Sets a `bool` prop.
    /// @param name  The prop.
    /// @param value Its value.
    virtual void setBoolProp(std::string_view name, bool value) = 0;

    /// @brief Sets a `number` prop: the one kind of value that crosses as a double.
    /// @param name  The prop.
    /// @param value Its value.
    virtual void setNumberProp(std::string_view name, double value) = 0;

    /// @brief Hands over the handler of the component's events.
    /// @param onEvent Called with the event's name and its payload as text (a key as `i:<int64>` or `s:<text>`,
    ///                anything else as the component wrote it); empty does nothing.
    virtual void setOnEvent(std::function<void(std::string event, std::string payload)> onEvent) = 0;
};

/// @brief What `IViewBackend::createCustom` asks for.
struct CustomRequest {
    /// @brief The component's name, as the document's `components` declares it.
    std::string name;
    /// @brief The props the document declares for it; a component that does not declare each one is refused.
    std::vector<std::string> props;
    /// @brief The lowest version of a host component that will do; a bundle component ignores it.
    int minVersion = 0;
};

/// @brief Makes widgets. Each factory appends the new widget to @p parent, or makes a root when it is null, and never
///        returns null, except `createCustom`, whose null shows a custom node's fallback.
class MORPH_API IViewBackend {
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
    /// @param style Dropdown, radio or segmented.
    /// @return The widget.
    [[nodiscard]] virtual std::unique_ptr<SelectWidget> createSelect(ContainerWidget* parent, SelectStyle style) = 0;

    /// @brief Makes a command list.
    /// @param parent The container to append to, or null for a root.
    /// @param style A bar of top-level entries, or one button that opens them.
    /// @return The widget.
    [[nodiscard]] virtual std::unique_ptr<MenuWidget> createMenu(ContainerWidget* parent, MenuStyle style) = 0;

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

    /// @brief Makes a transient message.
    /// @param parent The container to append to, or null for a root.
    /// @return The widget.
    [[nodiscard]] virtual std::unique_ptr<ToastWidget> createToast(ContainerWidget* parent) = 0;

    /// @brief Makes a non-modal panel anchored under @p parent (see `PopoverWidget`), closed.
    /// @param parent The container it belongs to and is anchored under, or null for a root.
    /// @return The widget.
    [[nodiscard]] virtual std::unique_ptr<PopoverWidget> createPopover(ContainerWidget* parent) = 0;

    /// @brief Makes a spotlight. A backend that draws none keeps this default, which throws: a tour on it cannot run,
    ///        and that is said rather than hidden.
    /// @param parent The container to append it to; null makes a root.
    /// @return The widget.
    /// @throws std::logic_error from the default.
    [[nodiscard]] virtual std::unique_ptr<SpotlightWidget> createSpotlight(ContainerWidget* parent);

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

    /// @brief Makes a banner.
    /// @param parent The container to append to, or null for a root.
    /// @return The widget.
    [[nodiscard]] virtual std::unique_ptr<BannerWidget> createBanner(ContainerWidget* parent) = 0;

    /// @brief Makes a badge.
    /// @param parent The container to append to, or null for a root.
    /// @return The widget.
    [[nodiscard]] virtual std::unique_ptr<BadgeWidget> createBadge(ContainerWidget* parent) = 0;

    /// @brief Makes a progress bar.
    /// @param parent The container to append to, or null for a root.
    /// @return The widget.
    [[nodiscard]] virtual std::unique_ptr<ProgressWidget> createProgress(ContainerWidget* parent) = 0;

    /// @brief Makes a steps indicator.
    /// @param parent The container to append to, or null for a root.
    /// @return The widget.
    [[nodiscard]] virtual std::unique_ptr<StepsWidget> createSteps(ContainerWidget* parent) = 0;

    /// @brief Makes a key-value list.
    /// @param parent The container to append to, or null for a root.
    /// @return The widget.
    [[nodiscard]] virtual std::unique_ptr<KeyValueWidget> createKeyValue(ContainerWidget* parent) = 0;

    /// @brief Makes a chart.
    /// @param parent The container to append to, or null for a root.
    /// @return The widget.
    [[nodiscard]] virtual std::unique_ptr<ChartWidget> createChart(ContainerWidget* parent) = 0;

    /// @brief Makes an empty state.
    /// @param parent The container to append to, or null for a root.
    /// @return The widget.
    [[nodiscard]] virtual std::unique_ptr<EmptyStateWidget> createEmptyState(ContainerWidget* parent) = 0;

    /// @brief Makes a drawer.
    /// @param parent The container to append to, or null for a root.
    /// @param side The edge it slides in from.
    /// @return The widget.
    [[nodiscard]] virtual std::unique_ptr<DrawerWidget> createDrawer(ContainerWidget* parent, Side side) = 0;

    /// @brief Makes a splitter.
    /// @param parent The container to append to, or null for a root.
    /// @param axis The axis its panes are laid out along.
    /// @return The widget.
    [[nodiscard]] virtual std::unique_ptr<SplitterWidget> createSplitter(ContainerWidget* parent, Axis axis) = 0;

    /// @brief Makes a collapsible section.
    /// @param parent The container to append to, or null for a root.
    /// @return The widget.
    [[nodiscard]] virtual std::unique_ptr<CollapsibleWidget> createCollapsible(ContainerWidget* parent) = 0;

    /// @brief Makes a drop zone.
    /// @param parent The container to append to, or null for a root.
    /// @return The widget.
    [[nodiscard]] virtual std::unique_ptr<DropZoneWidget> createDropZone(ContainerWidget* parent) = 0;

    /// @brief Makes a page.
    /// @param parent   The container to append to, or null for a root.
    /// @param geometry The sheet's size and margins; valid (`PageGeometry::problem`).
    /// @return The widget.
    [[nodiscard]] virtual std::unique_ptr<PageWidget> createPage(ContainerWidget* parent,
                                                                 PageGeometry const& geometry) = 0;

    /// @brief Makes a page region.
    /// @param parent   The page to append to.
    /// @param geometry The region's place; valid on the page.
    /// @return The widget.
    [[nodiscard]] virtual std::unique_ptr<PageRegionWidget> createPageRegion(ContainerWidget* parent,
                                                                             RegionGeometry const& geometry) = 0;

    /// @brief Loads the component a `custom` node names, if this backend can. The only factory that may return null:
    ///        null means "not loaded", and the mount shows the node's fallback instead. A backend that loads no
    ///        components keeps this default.
    /// @param parent  The container to append it to; null makes a root.
    /// @param request The component, the props the document declares and the host component's lowest version.
    /// @return The component, or null to show the fallback.
    [[nodiscard]] virtual std::unique_ptr<CustomWidget> createCustom(ContainerWidget* parent,
                                                                     CustomRequest const& request);

    /// @brief The width class of the surface this backend draws on, now: `compact`, `medium` or `expanded`
    ///        (`widthClassOf`), what `{"env": "widthClass"}` reads.
    /// @return The class.
    [[nodiscard]] virtual std::string widthClass() const = 0;

    /// @brief Registers the one listener told of each later change of `widthClass()`, on the owner and outside any
    ///        layout pass; a backend reports to the shell mounted on it. It is not called at once: the caller reads
    ///        `widthClass()` for the current class.
    /// @param onWidthClass The listener, given the new class; empty clears it.
    virtual void setOnWidthClass(std::function<void(std::string const&)> onWidthClass) = 0;

    /// @brief Puts text on the system clipboard: Qt Quick's clipboard, the terminal's through an OSC 52 sequence.
    ///        A backend without a clipboard keeps this default.
    /// @param text The text, UTF-8.
    /// @return Whether the text was handed to the clipboard.
    virtual bool copyText(std::string_view text) {
        static_cast<void>(text);
        return false;
    }

    /// @brief Reads the system clipboard's text, what a grid's paste writes into its cells. A backend that cannot
    ///        read a clipboard keeps this default.
    /// @return The text, UTF-8; nothing when there is no clipboard or it holds no text.
    [[nodiscard]] virtual std::optional<std::string> pasteText() { return std::nullopt; }
};

/// @brief The width, in units, from which a surface is `expanded`: the compact bound, 80, plus the shell's sidebar
///        (28), the narrowest window whose page beside that sidebar is not itself compact.
inline constexpr int kExpandedWidthUnits = 108;

/// @brief A width in the vocabulary's units (what `fixed(n)` counts) as a width class: `compact` below 80 units,
///        `medium` below `kExpandedWidthUnits` (108), `expanded` otherwise. Every backend classes its surface with
///        it, so a document's breakpoints sit at the same number of its own units on every frontend.
/// @param units The width, in units: a terminal's columns, or a window's width over its unit width.
/// @return The class: `compact`, `medium` or `expanded`.
[[nodiscard]] MORPH_API std::string widthClassOf(double units);

}  // namespace morph::ui

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
    /// A chord pressed while focus is inside several widgets that declare it goes to the innermost of them only.
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

    /// @brief Memberwise equality, submenus included.
    /// @return Whether every field matches.
    bool operator==(MenuEntry const&) const = default;
};

/// @brief The trigger of a button-style menu.
struct MenuTrigger {
    /// @brief The trigger's text and accessible name; empty for an icon-only trigger.
    std::string label;
    /// @brief The trigger's icon name in the theme's icon map; empty for none.
    std::string icon;
    /// @brief The trigger's emphasis, as a button's.
    ButtonEmphasis emphasis = ButtonEmphasis::Normal;

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

    /// @brief Marks the stack as the chosen entry of a list, or not: while selected it draws the accent's 3 px bar at
    ///        its leading edge, down its whole height, over its surface and under its children, inside its leading
    ///        padding. The mount calls it for a stack that gives the key, at mount (possibly with false) and on every
    ///        change; that first call is what tells a backend the stack can be selected, so one that reserves room
    ///        for the bar reserves it from then on whether or not the stack is selected.
    /// @param selected Whether the stack is the chosen entry.
    virtual void setSelected(bool selected) = 0;
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
    ///        focused widget in view; a child that stretches in height is laid out as in a slot that does not
    ///        scroll, filling it. The slot reads its shown child's height sizing itself, so a new child follows
    ///        without another call. A new slot does not scroll.
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

    /// @brief Pads each page by its own step in place of `setInset`'s, so a page that scrolls can pad its scrolled
    ///        content and let a bleeding child of that content reach the scroll's edges. The mount calls it once,
    ///        only when the view gives a page inset, which may be 0.
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

    /// @brief Sets the label of a sheet's dismiss control; a new dialog's is "Close". The mount calls this only when
    ///        the label is bound or not the default.
    /// @param label The label, UTF-8.
    virtual void setDismissLabel(std::string_view label) = 0;
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
class TableWidget : public ContainerWidget {
public:
    /// @brief Sets the column headers and widths.
    /// @param columns The columns, in order.
    virtual void setColumns(std::vector<TableColumn> const& columns) = 0;

    /// @brief Sets whether the header row is drawn.
    /// @param shown `false` suits several tables sharing one visual header.
    virtual void setShowHeader(bool shown) = 0;

    /// @brief Sets how many rows the user can select.
    /// @param mode None, one, or any number.
    virtual void setSelectionMode(SelectionMode mode) = 0;

    /// @brief Tells the table which key a row stands for; called once per row, right after it is built. Marks the row
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

    /// @brief Marks a column's header with the direction the rows are sorted by it, or marks none. The table never
    ///        reorders its rows: they arrive sorted. Never calls the `onSort` handler; the mount calls it only for a
    ///        table with a sortable column.
    /// @param sort The column and direction, or none.
    virtual void setSort(std::optional<TableSort> sort) = 0;

    /// @brief Sets what a user's request to sort calls: a click on a sortable column's header (or Enter or Space on
    ///        it, where the header takes focus) calls it with the column's index among the columns `setColumns` gave,
    ///        which the select column is not one of. A header that is not sortable reports nothing. What the request
    ///        means is the caller's to decide; the backend only shows `setSort`.
    /// @param onSort The handler; empty does nothing.
    virtual void setOnSort(std::function<void(std::size_t)> onSort) = 0;

    /// @brief Tells the backend how many of the table's rows are realized, where, and how many exist in total, so
    ///        it can size a scrollbar and place leading/trailing space for the rows that are not realized. A
    ///        backend that never virtualizes (the default for every implementer but a windowed Qt Quick table)
    ///        never calls `setOnVisibleRangeChanged`, so this is never called either.
    /// @param first The view index of the first realized row.
    /// @param count How many rows are realized, starting at `first`.
    /// @param total How many rows the table's data has, realized or not.
    virtual void setRealizedWindow(std::size_t first, std::size_t count, std::size_t total) {
        static_cast<void>(first);
        static_cast<void>(count);
        static_cast<void>(total);
    }

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
    ///        being always read too. The mount calls it once, before the value, and only for a pattern that is not
    ///        empty; a new field shows ISO.
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
    ///        draws the surface's fill with the tone's colour as border and text.
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

    /// @brief Loads the component a `custom` node names, if this backend can. The only factory that may return null:
    ///        null means "not loaded", and the mount shows the node's fallback instead. A backend that loads no
    ///        components keeps this default.
    /// @param parent  The container to append it to; null makes a root.
    /// @param request The component, the props the document declares and the host component's lowest version.
    /// @return The component, or null to show the fallback.
    [[nodiscard]] virtual std::unique_ptr<CustomWidget> createCustom(ContainerWidget* parent,
                                                                     CustomRequest const& request) {
        static_cast<void>(parent);
        static_cast<void>(request);
        return nullptr;
    }

    /// @brief The width class of the surface this backend draws on, now: `compact`, `medium` or `expanded`
    ///        (`widthClassOf`), what `{"env": "widthClass"}` reads.
    /// @return The class.
    [[nodiscard]] virtual std::string widthClass() const = 0;

    /// @brief Registers the one listener told of each later change of `widthClass()`, on the owner and outside any
    ///        layout pass; a backend reports to the shell mounted on it. It is not called at once: the caller reads
    ///        `widthClass()` for the current class.
    /// @param onWidthClass The listener, given the new class; empty clears it.
    virtual void setOnWidthClass(std::function<void(std::string const&)> onWidthClass) = 0;
};

/// @brief The width, in units, from which a surface is `expanded`: the compact bound, 80, plus the shell's sidebar
///        (28), the narrowest window whose page beside that sidebar is not itself compact.
inline constexpr int kExpandedWidthUnits = 108;

/// @brief A width in the vocabulary's units (what `fixed(n)` counts) as a width class: `compact` below 80 units,
///        `medium` below `kExpandedWidthUnits` (108), `expanded` otherwise. Every backend classes its surface with
///        it, so a document's breakpoints sit at the same number of its own units on every frontend.
/// @param units The width, in units: a terminal's columns, or a window's width over its unit width.
/// @return The class: `compact`, `medium` or `expanded`.
[[nodiscard]] inline std::string widthClassOf(double units) {
    if (units < 80.0) {
        return "compact";
    }
    if (units < static_cast<double>(kExpandedWidthUnits)) {
        return "medium";
    }
    return "expanded";
}

}  // namespace morph::ui

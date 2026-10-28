// SPDX-License-Identifier: Apache-2.0

#pragma once
#include <core/tui/Canvas.hpp>
#include <core/tui/Component.hpp>
#include <core/tui/InputEvent.hpp>
#include <core/tui/Rect.hpp>
#include <core/tui/Tooltip.hpp>
#include <algorithm>
#include <cstddef>
#include <functional>
#include <memory>
#include <morph/ui/backend.hpp>
#include <morph/ui/view.hpp>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "tui/chord.hpp"
#include "tui/context.hpp"
#include "tui/layout.hpp"

namespace morph::tui::detail {

class ContainerBase;

/// The morph side of one TUI widget: the Common props, the container it sits in, and the core::tui view it owns.
///
/// The view is registered in the Context while the widget lives. The destructor clears the screen's focus from it
/// and unregisters it before the view itself is destroyed, so core::tui never keeps a pointer to a freed view.
///
/// Every handler is called through a local copy, and nothing of the widget is touched after the call returns: a
/// handler may destroy its own widget.
class WidgetBase {
public:
    explicit WidgetBase(Context& context) noexcept;
    /// @brief Clears the screen's focus from the view, unregisters it, and leaves the roots and its container.
    virtual ~WidgetBase();
    WidgetBase(WidgetBase const&) = delete;
    WidgetBase& operator=(WidgetBase const&) = delete;
    WidgetBase(WidgetBase&&) = delete;
    WidgetBase& operator=(WidgetBase&&) = delete;

    /// The TUI widget behind @p widget; throws std::logic_error for a widget another backend made.
    [[nodiscard]] static WidgetBase& of(ui::Widget& widget);
    /// The TUI widget behind @p widget; throws std::logic_error for a widget another backend made.
    [[nodiscard]] static WidgetBase const& of(ui::Widget const& widget);

    [[nodiscard]] ::core::tui::Component& view() const noexcept { return *_view; }
    [[nodiscard]] Context& context() const noexcept { return *_context; }
    [[nodiscard]] ContainerBase* container() const noexcept { return _container; }
    void setContainer(ContainerBase* container) noexcept { _container = container; }
    /// This widget as the ui interface it implements.
    [[nodiscard]] virtual ui::Widget const& asWidget() const noexcept = 0;

    void applyVisible(bool visible);
    /// Shown or hidden by its container (a collapsed panel), independently of `visible`.
    void setStructuralVisible(bool visible);
    [[nodiscard]] bool userVisible() const noexcept { return _userVisible; }
    [[nodiscard]] bool shown() const noexcept { return _userVisible && _structuralVisible; }
    void applyEnabled(bool enabled);
    [[nodiscard]] bool enabled() const noexcept { return _enabled; }
    /// Whether the user can reach this widget: it and every container around it are shown and enabled, and each of
    /// those containers lets its children act (a collapsed panel and a closed dialog do not). Every input path asks
    /// this, because core::tui sends keys to the focused view however its ancestors changed since it took focus.
    [[nodiscard]] bool actionable() const;
    /// Whether the user could see this widget: it and every container around it are shown, and none of those
    /// containers holds its children out of reach (a closed dialog shows nothing of them).
    [[nodiscard]] bool displayed() const;
    void applyLayout(ui::LayoutHints const& hints);
    [[nodiscard]] ui::LayoutHints const& layout() const noexcept { return _layout; }
    void applyDragKey(std::optional<ui::Key> const& key);
    [[nodiscard]] std::optional<ui::Key> const& dragKey() const noexcept { return _dragKey; }
    void applyDropHandler(std::function<bool(ui::Key const&)> accepts, std::function<void(ui::Key)> onDrop);
    [[nodiscard]] bool isDropTarget() const noexcept { return static_cast<bool>(_onDrop); }
    /// Whether a drop of @p key lands here: a drop target whose `accepts` is empty or holds.
    [[nodiscard]] bool accepts(ui::Key const& key) const;
    /// Calls the drop handler with @p key.
    void drop(ui::Key key) const;

    /// Keeps the name, role and test id. A terminal has no assistive technology to tell them to; the test id is how
    /// a probe finds the widget.
    void applyAccessibleName(std::string_view name) { _accessibleName = name; }
    void applyAccessibleRole(std::string_view role) { _accessibleRole = role; }
    void applyTestId(std::string_view testId) { _testId = testId; }
    [[nodiscard]] std::string const& accessibleName() const noexcept { return _accessibleName; }
    [[nodiscard]] std::string const& accessibleRole() const noexcept { return _accessibleRole; }
    [[nodiscard]] std::string const& testId() const noexcept { return _testId; }
    /// Keeps the surface by name: the token set that styles the widget and everything inside it.
    void applySurface(std::string_view surface) { _surface = surface; }
    [[nodiscard]] std::string const& surface() const noexcept { return _surface; }
    /// The surface the widget draws on: its own, or else its nearest container's. The terminal draws the `sidebar`
    /// and `toolbar` surfaces in the inverse of its colours and every other surface as it draws no surface.
    [[nodiscard]] std::string const& effectiveSurface() const noexcept;
    /// Keeps a table row's tone, which its cells draw in.
    void applyRowTone(std::optional<ui::Tone> tone) noexcept { _rowTone = tone; }
    /// The tone the widget draws in: its own row tone, or else its nearest container's; none for no toned row.
    [[nodiscard]] std::optional<ui::Tone> effectiveRowTone() const noexcept;
    /// Sets the text core::tui's hover shows; empty shows none.
    void applyTooltip(std::string_view text) { _tooltip = text; }
    /// What the hover shows: the tooltip, and a field adds its messages.
    [[nodiscard]] virtual std::string hoverText() const { return _tooltip; }
    /// What assistive technology reads as the widget's description; none for a widget that is not described.
    [[nodiscard]] virtual std::string accessibleDescription() const { return {}; }
    /// Declares the chords this widget handles while the focus is inside it.
    void applyKeys(std::vector<std::string> const& chords, std::function<void(std::string)> onChord);
    /// Runs the handler of the chord @p key is, when this widget declares it. A view that handles keys itself calls
    /// it with each key it leaves alone. The handler may destroy this widget.
    /// @return Handled when the widget declares the chord.
    [[nodiscard]] ::core::tui::EventResult chord(::core::tui::KeyEvent const& key);
    /// Moves the keyboard focus here, when the widget can take it.
    void takeFocus();
    /// Whether the user may not change the value: a field's read-only state; never for any other widget.
    [[nodiscard]] virtual bool readOnly() const { return false; }

    /// The size this widget asks for, in cells.
    [[nodiscard]] virtual ::core::tui::Size naturalSize() const = 0;
    /// Whether keyboard focus may land here (and, while actionable, does).
    [[nodiscard]] virtual bool wantsFocus() const { return false; }
    /// Whether a press on this widget moves the focus to it; by default, whenever it wants the focus at all.
    /// Whether a press on the widget is its own (it then takes the moves and the release that follow).
    [[nodiscard]] virtual bool takesPress() const { return wantsFocus(); }
    [[nodiscard]] virtual bool focusesOnPress() const { return wantsFocus(); }
    /// Whether a Content main-axis size means Stretch(1) in a stack (a Spacer).
    [[nodiscard]] virtual bool expandsByDefault() const { return false; }
    /// Whether a table column places this widget at its natural width (a text, a badge, a check box, a push
    /// button) rather than filling the column with it.
    [[nodiscard]] virtual bool placedAcrossColumn() const { return false; }
    /// The text a conformance probe reads.
    [[nodiscard]] virtual std::string probeText() const { return {}; }
    /// Draws the widget into its own area; a container also places its children here. The canvas is as large as
    /// the area the widget was laid out at, even where an ancestor shows only part of it.
    virtual void paint(::core::tui::Canvas& canvas);
    /// Draws the widget for its view, which core::tui hands @p canvas over the drawn part of the view's area.
    ///
    /// core::tui clips a view to its parent's drawn bounds and starts the canvas at the clipped corner, so a view
    /// cut off at its top or left (one scrolled partly out of view) would draw its first row or column where its
    /// later ones belong. When the drawn part is smaller than the laid-out area, `paint` draws into a canvas of the
    /// whole area over a scratch copy of the drawn cells, and only the drawn cells are copied back; the cursor a
    /// widget sets comes back with them, hidden when it lies outside. `drawShift` records the cut, which `place`
    /// and a release's click position account for.
    void render(::core::tui::Canvas& canvas);
    /// How many cells of this widget's laid-out area its last drawing left off at the left and at the top.
    [[nodiscard]] ::core::tui::Point drawShift() const noexcept { return _drawShift; }
    /// Gives this widget @p area, in its container's coordinates: those of the container's whole laid-out area,
    /// as its `paint` sees them. An empty area means it is not drawn.
    void place(::core::tui::Rect area);
    /// Handles a key while actionable and focused (or bubbled to).
    [[nodiscard]] virtual ::core::tui::EventResult key(::core::tui::KeyEvent const& key);
    /// The primary action: what Enter, Space and a click do.
    virtual void activate() {}
    /// A click on @p cell (0-based, relative to this widget); runs `activate()` unless overridden.
    virtual void click(::core::tui::Point cell);
    /// A click with the modifiers the terminal reported with it; `click(cell)` unless overridden, since only a table
    /// row's selection reads them.
    virtual void clickWith(::core::tui::Point cell, ::core::tui::Modifier modifiers) {
        static_cast<void>(modifiers);
        click(cell);
    }
    /// A left press on @p cell (0-based, relative to this widget) that this widget took, after it took the focus
    /// when it takes it on a press. It may destroy the widget.
    virtual void pressAt(::core::tui::Point cell);
    /// The pointer moved to @p cell (relative to this widget, and possibly outside it) while a press this widget took
    /// lasts and the widget is actionable. It may destroy the widget.
    virtual void dragTo(::core::tui::Point cell);
    /// A wheel notch, -1 up and +1 down; true when handled.
    [[nodiscard]] virtual bool wheel(int delta);
    /// Takes text pasted while the widget is actionable and has the focus; Ignored by default.
    [[nodiscard]] virtual ::core::tui::EventResult paste(std::string_view text);
    /// Called when the release of a press this widget took arrives, inside the widget or not, before any click.
    /// @return True when it took the release: nothing else happens with it, and the widget may be gone by then.
    [[nodiscard]] virtual bool pressEnded() { return false; }
    /// Closes the popup this widget has open (a dropdown's list), as `closeUnreachablePopups` asks of a widget in
    /// `Context::popups` the user can no longer reach; it runs no handler.
    virtual void closePopup() {}
    /// The popup this widget has open (a dropdown's list), or null.
    [[nodiscard]] virtual ::core::tui::Component const* openPopup() const { return nullptr; }

    /// Routes one event of this widget's view: mouse to `pointer()`, keys to `key()` while actionable and not behind
    /// an open dialog (see `activeDialog`).
    [[nodiscard]] ::core::tui::EventResult dispatch(::core::tui::InputEvent const& event);
    /// Press, motion, release and wheel on this widget's view. Behind an open dialog they are swallowed, and a press
    /// there still ends a press whose release never came. A widget with a drag key is dragged by the motion that
    /// follows its press (see `DragController`), and a release that ends a drag is no click.
    [[nodiscard]] ::core::tui::EventResult pointer(::core::tui::MouseEvent const& mouse);
    /// Whether an open dialog the user sees lies over this widget: its view is outside that dialog's frame.
    [[nodiscard]] bool blockedByDialog() const;
    [[nodiscard]] bool focusable() const { return wantsFocus() && actionable(); }
    [[nodiscard]] bool hasFocus() const noexcept { return _view != nullptr && _view->focused(); }
    /// Asks the screen for a full repaint on the next draw.
    void refresh() const;

protected:
    /// Takes ownership of the widget's view and registers it; returns it typed.
    template <class ViewType>
    ViewType& adopt(std::unique_ptr<ViewType> view) {
        ViewType& adopted = *view;
        _view = std::move(view);
        _context->owners.insert_or_assign(_view.get(), this);
        return adopted;
    }

private:
    void syncVisible();
    void paintClipped(::core::tui::Canvas& canvas, ::core::tui::Size laidOut);
    [[nodiscard]] ::core::tui::EventResult press(::core::tui::MouseEvent const& mouse);
    [[nodiscard]] ::core::tui::EventResult release(::core::tui::MouseEvent const& mouse);
    [[nodiscard]] ::core::tui::EventResult move(::core::tui::MouseEvent const& mouse);
    /// The cell of this widget's laid-out area a pointer event relative to its view lies on.
    [[nodiscard]] ::core::tui::Point cellOf(::core::tui::MouseEvent const& mouse) const noexcept;
    /// The viewport cell a pointer event relative to this widget's view lies on.
    [[nodiscard]] ::core::tui::Point viewportCellOf(::core::tui::MouseEvent const& mouse) const noexcept;

    Context* _context;
    ContainerBase* _container = nullptr;
    bool _userVisible = true;
    bool _structuralVisible = true;
    bool _enabled = true;
    ui::LayoutHints _layout{};
    std::optional<ui::Key> _dragKey;
    std::function<bool(ui::Key const&)> _accepts;
    std::function<void(ui::Key)> _onDrop;
    std::string _accessibleName;
    std::string _accessibleRole;
    std::string _testId;
    std::string _surface;
    std::optional<ui::Tone> _rowTone;
    std::string _tooltip;
    std::vector<std::string> _chords;
    std::function<void(std::string)> _onChord;
    ::core::tui::Point _drawShift{};
    std::unique_ptr<::core::tui::Component> _view;
};

/// A widget whose children's views sit in a host component it provides (its own view, unless overridden).
///
/// The order of `children()` is the order of layout and of Tab. The child views stay in core::tui's tree in the
/// order they were attached: core::tui can reorder a child only by removing and re-adding it, and a removal ends
/// the screen's pointer capture on it, which would cut a press or a drag short. That order decides only which of
/// two overlapping siblings is on top, and the children of a stack never overlap.
class ContainerBase : public WidgetBase {
public:
    explicit ContainerBase(Context& context) noexcept : WidgetBase{context} {}
    /// @brief Leaves the children that outlive this container with no container.
    ~ContainerBase() override;
    ContainerBase(ContainerBase const&) = delete;
    ContainerBase& operator=(ContainerBase const&) = delete;
    ContainerBase(ContainerBase&&) = delete;
    ContainerBase& operator=(ContainerBase&&) = delete;

    /// The TUI container behind @p container; throws std::logic_error for one another backend made.
    [[nodiscard]] static ContainerBase& of(ui::ContainerWidget& container);
    /// The TUI container behind @p container; throws std::logic_error for one another backend made.
    [[nodiscard]] static ContainerBase const& of(ui::ContainerWidget const& container);

    void attach(WidgetBase& child);
    void forget(WidgetBase& child);
    /// Moves @p child to position @p index of `children()` (past the end: last); its view is not touched.
    void move(WidgetBase& child, std::size_t index);
    [[nodiscard]] std::span<WidgetBase* const> children() const noexcept { return _children; }
    [[nodiscard]] std::vector<WidgetBase*> shownChildren() const;
    [[nodiscard]] virtual ::core::tui::Component& host() { return view(); }
    /// Whether the children may take input while this container is actionable itself; false while it holds them
    /// out of reach (a collapsed panel, a closed dialog).
    [[nodiscard]] virtual bool letsChildrenAct() const { return true; }
    /// Whether @p child, one of this container's children, is drawn and takes input: `letsChildrenAct` for every
    /// child, unless a container lets some through (a closed collapsible's header).
    [[nodiscard]] virtual bool letsChildAct(WidgetBase const& /*child*/) const { return letsChildrenAct(); }
    /// The overlay this container shows as a dialog or a drawer; null for any other container.
    [[nodiscard]] virtual class OverlayCore* overlay() { return nullptr; }
    /// Where each of `children()` goes inside this container when it is laid out at @p size, in its own
    /// coordinates; an empty area for a child that is not drawn. This is what `paint` places, and what a scroll
    /// around the container reads to find a widget it should bring into view. A container that does not say returns
    /// an empty list.
    [[nodiscard]] virtual std::vector<::core::tui::Rect> childAreas(::core::tui::Size size) const;
    /// Gives each of `children()` its area from @p areas, in order; a child past the end gets none.
    void placeChildren(std::span<::core::tui::Rect const> areas) const;

protected:
    virtual void childAttached(WidgetBase& child);
    virtual void childForgotten(WidgetBase& child);

private:
    std::vector<WidgetBase*> _children;
};

/// A core::tui component that renders and reacts for its owner, and is visible only while its ancestors are.
template <class Base>
class Hosted : public Base {
public:
    explicit Hosted(WidgetBase& owner) : _owner{&owner} {}

    [[nodiscard]] bool visible() const noexcept override {
        return Base::visible() && (this->parent() == nullptr || this->parent()->visible());
    }
    [[nodiscard]] bool focusable() const override { return _owner->focusable(); }
    [[nodiscard]] ::core::tui::Size preferredSize() const override { return _owner->naturalSize(); }
    [[nodiscard]] std::optional<::core::tui::HoverResult> onHover(int /*x*/, int /*y*/) override {
        std::string text = _owner->hoverText();
        if (text.empty()) {
            return std::nullopt;
        }
        return ::core::tui::HoverResult{
            .text = std::move(text), .position = {}, .contentType = ::core::tui::TooltipContentType::PlainText};
    }
    [[nodiscard]] WidgetBase& owner() const noexcept { return *_owner; }

private:
    WidgetBase* _owner;
};

/// The plain view most widgets use: painting and events go to @p owner. Its class lives in widget.cpp, where it is
/// the only instantiation of `Hosted` over a plain Component: a header-defined one would instantiate `Hosted` in
/// every translation unit that includes this file, most of which never use its virtual members.
[[nodiscard]] std::unique_ptr<::core::tui::Component> makeView(WidgetBase& owner);

/// Draws @p marks right-aligned on the canvas's first row, over what is there.
void paintFieldMarks(::core::tui::Canvas& canvas, std::string_view marks);
/// @p text split at '\n'.
[[nodiscard]] std::vector<std::string_view> splitLines(std::string_view text);
/// The display width of @p text in cells.
[[nodiscard]] int displayWidth(std::string_view text);
/// A tone's style: the theme's info, success, warning or error style, and the normal text for `Neutral`.
[[nodiscard]] ::core::tui::Style const& toneStyle(::core::tui::Theme const& theme, ui::Tone tone);
/// Draws a field's description under its control: each line muted, or in @p tone's style while toned, clipped at
/// the canvas's width.
void paintFieldDescription(::core::tui::Canvas& canvas, std::string_view description, ui::Tone tone);
/// The required mark drawn after a caption or a text's last line, a space before it.
inline constexpr std::string_view kRequiredMark = " *";
/// Draws @p caption on the canvas's first row, followed by the required mark in the error style while @p required,
/// and answers the canvas below it, where a captioned field draws its control; the whole canvas for an empty caption.
[[nodiscard]] ::core::tui::Canvas paintFieldCaption(::core::tui::Canvas& canvas, std::string_view caption,
                                                    bool required = false);
/// @p control grown by @p caption's row, and as wide as the caption and, while @p required, its mark need;
/// @p control itself for an empty caption.
[[nodiscard]] ::core::tui::Size withFieldCaption(::core::tui::Size control, std::string_view caption,
                                                 bool required = false);

/// Implements the Common setters of ui interface @p Interface over WidgetBase.
///
/// A widget deriving from this (or from TuiContainer) defines its constructor inside its class. A translation unit
/// that sees the class then instantiates this template's virtual members along with its vtable; with the
/// constructor out of line, a unit that only uses the widget instantiates the class but not its virtual members,
/// which compilers are free to do differently (MSVC instantiates them all).
/// @tparam Base `WidgetBase`, or `ContainerBase` for a widget that holds a part of its own (a list entry's content).
template <class Interface, class Base = WidgetBase>
class TuiWidget : public Interface, public Base {
public:
    explicit TuiWidget(Context& context) noexcept : Base{context} {}
    void setVisible(bool visible) override { this->applyVisible(visible); }
    void setEnabled(bool enabled) override { this->applyEnabled(enabled); }
    void setLayout(ui::LayoutHints const& hints) override { this->applyLayout(hints); }
    void setDragKey(std::optional<ui::Key> const& key) override { this->applyDragKey(key); }
    void setDropHandler(std::function<bool(ui::Key const&)> accepts, std::function<void(ui::Key)> onDrop) override {
        this->applyDropHandler(std::move(accepts), std::move(onDrop));
    }
    void setAccessibleName(std::string_view name) override { this->applyAccessibleName(name); }
    void setAccessibleRole(std::string_view role) override { this->applyAccessibleRole(role); }
    void setTestId(std::string_view testId) override { this->applyTestId(testId); }
    void setTooltip(std::string_view text) override { this->applyTooltip(text); }
    void setSurface(std::string_view surface) override { this->applySurface(surface); }
    void setKeys(std::vector<std::string> const& chords, std::function<void(std::string)> onChord) override {
        this->applyKeys(chords, std::move(onChord));
    }
    void focus() override { this->takeFocus(); }
    [[nodiscard]] ui::Widget const& asWidget() const noexcept override { return *this; }
};

/// Implements the field state of field interface @p Interface: read-only, required, errors and stale.
///
/// A terminal shows the state as marks at the right edge of the control's first row, `paintMarks` drawing them: `*`
/// for a required field that draws no caption, `!` for one with errors or toned `err` or `warn` (the terminal has
/// no outline), `~` for a stale value. A captioned field's `*` follows its caption instead (`paintCaption`), and a
/// text's its last line. The description takes the rows under the control, muted or in the tone's style. The
/// messages, then the description, are the field's accessible description, and show on hover after the tooltip.
/// A field that edits checks `readOnly()` before it reports an edit.
///
/// A kind sizes and draws its control (`controlSize`, `paintControl`); the field adds the description's rows.
template <class Interface>
class TuiField : public TuiWidget<Interface> {
public:
    using TuiWidget<Interface>::TuiWidget;
    void setReadOnly(bool readonly) override {
        _readOnly = readonly;
        this->refresh();
    }
    void setRequired(bool required) override {
        _required = required;
        this->refresh();
    }
    void setErrors(std::vector<std::string> const& errors) override {
        _errors = errors;
        this->refresh();
    }
    void setStale(bool stale) override {
        _stale = stale;
        this->refresh();
    }
    void setDescription(std::string_view description) override {
        _description = std::string{description};
        this->refresh();
    }
    void setTone(ui::Tone tone) override {
        _tone = tone;
        this->refresh();
    }
    [[nodiscard]] bool readOnly() const override { return _readOnly; }
    /// What assistive technology reads as the field's description: its messages, then its description.
    [[nodiscard]] std::string accessibleDescription() const override {
        std::string text;
        for (std::string const& error : _errors) {
            text += text.empty() ? error : "\n" + error;
        }
        if (!_description.empty()) {
            text += text.empty() ? _description : "\n" + _description;
        }
        return text;
    }
    [[nodiscard]] std::string hoverText() const override {
        std::string text = WidgetBase::hoverText();
        std::string const described = accessibleDescription();
        if (!described.empty()) {
            text += text.empty() ? described : "\n" + described;
        }
        return text;
    }
    [[nodiscard]] ::core::tui::Size naturalSize() const final {
        ::core::tui::Size size = controlSize();
        if (!_description.empty()) {
            for (auto const line : splitLines(_description)) {
                size.width = std::max(size.width, displayWidth(line));
                ++size.height;
            }
        }
        return size;
    }
    void paint(::core::tui::Canvas& canvas) final {
        int const rows = descriptionRows();
        auto control = canvas.subcanvas(
            {.x = 0, .y = 0, .width = canvas.width(), .height = std::max(0, canvas.height() - rows)});
        paintControl(control);
        if (rows > 0) {
            auto below = canvas.subcanvas({.x = 0,
                                           .y = control.height(),
                                           .width = canvas.width(),
                                           .height = canvas.height() - control.height()});
            detail::paintFieldDescription(below, _description, _tone);
        }
    }

protected:
    /// The size of the control the kind draws, its caption included, without the description.
    [[nodiscard]] virtual ::core::tui::Size controlSize() const = 0;
    /// Draws the control, its caption included, on the canvas left above the description.
    virtual void paintControl(::core::tui::Canvas& canvas) = 0;
    /// The rows the description takes under the control.
    [[nodiscard]] int descriptionRows() const {
        return _description.empty() ? 0 : static_cast<int>(splitLines(_description).size());
    }
    /// Draws the control's marks at the right edge of the canvas's first row; @p required false leaves out the `*`,
    /// for a kind that draws it elsewhere.
    void paintMarks(::core::tui::Canvas& canvas, bool required = true) const {
        detail::paintFieldMarks(canvas, controlMarks(required));
    }
    /// How many cells the control's marks take at the right edge.
    [[nodiscard]] int marksWidth() const { return static_cast<int>(controlMarks(true).size()); }
    /// Whether the field is marked required.
    [[nodiscard]] bool required() const noexcept { return _required; }
    /// Draws the caption, with the required mark after it, and answers the canvas left for the control.
    [[nodiscard]] ::core::tui::Canvas paintCaption(::core::tui::Canvas& canvas) const {
        return detail::paintFieldCaption(canvas, _caption, _required);
    }
    /// @p control grown by the caption and its mark.
    [[nodiscard]] ::core::tui::Size withCaption(::core::tui::Size control) const {
        return detail::withFieldCaption(control, _caption, _required);
    }
    /// Takes the field's label as its caption while @p shown; otherwise the field draws none: what `setCaption`
    /// does for the kinds that have one.
    void showCaption(std::string_view label, bool shown) {
        _caption = shown ? std::string{label} : std::string{};
        this->refresh();
    }
    /// The caption drawn above the control; empty when there is none.
    [[nodiscard]] std::string const& caption() const noexcept { return _caption; }
    /// Rows the caption takes above the control: 1 while there is one.
    [[nodiscard]] int captionRows() const noexcept { return _caption.empty() ? 0 : 1; }

private:
    // The `*` follows a caption while there is one, so the control's marks leave it out then.
    [[nodiscard]] std::string controlMarks(bool required) const {
        std::string out;
        if (_required && required && _caption.empty()) {
            out += '*';
        }
        if (!_errors.empty() || _tone == ui::Tone::Err || _tone == ui::Tone::Warn) {
            out += '!';
        }
        if (_stale) {
            out += '~';
        }
        return out;
    }

    bool _readOnly = false;
    bool _required = false;
    bool _stale = false;
    std::vector<std::string> _errors;
    std::string _caption;
    std::string _description;
    ui::Tone _tone = ui::Tone::Neutral;
};

/// Implements the Common setters and `moveChild` of container interface @p Interface over ContainerBase.
template <class Interface>
class TuiContainer : public Interface, public ContainerBase {
public:
    explicit TuiContainer(Context& context) noexcept : ContainerBase{context} {}
    void setVisible(bool visible) override { applyVisible(visible); }
    void setEnabled(bool enabled) override { applyEnabled(enabled); }
    void setLayout(ui::LayoutHints const& hints) override { applyLayout(hints); }
    void setDragKey(std::optional<ui::Key> const& key) override { applyDragKey(key); }
    void setDropHandler(std::function<bool(ui::Key const&)> accepts, std::function<void(ui::Key)> onDrop) override {
        applyDropHandler(std::move(accepts), std::move(onDrop));
    }
    void setAccessibleName(std::string_view name) override { applyAccessibleName(name); }
    void setAccessibleRole(std::string_view role) override { applyAccessibleRole(role); }
    void setTestId(std::string_view testId) override { applyTestId(testId); }
    void setTooltip(std::string_view text) override { applyTooltip(text); }
    void setSurface(std::string_view surface) override { applySurface(surface); }
    void setKeys(std::vector<std::string> const& chords, std::function<void(std::string)> onChord) override {
        applyKeys(chords, std::move(onChord));
    }
    void focus() override { takeFocus(); }
    void moveChild(ui::Widget& child, std::size_t index) override { move(WidgetBase::of(child), index); }
    [[nodiscard]] ui::Widget const& asWidget() const noexcept override { return *this; }
};

/// Where a table cell sits in its slot: across its column and down its row.
struct CellPlace {
    ui::Align across = ui::Align::Start;  ///< Start, Center or End; Stretch reads as Start.
    ui::Align down = ui::Align::Center;   ///< Start, Center or End; Stretch fills the row's height.
};

/// How `arrangeStack` places children.
struct StackSpec {
    ui::Axis axis = ui::Axis::Vertical;  ///< The main axis.
    int gap = 0;                         ///< Cells between neighbours.
    std::span<int const> extents;        ///< When non-empty, the main-axis extents to use, in order (table columns).
    /// With extents, each child's place in its slot, in order; a child past the last takes the default place.
    std::span<CellPlace const> places;
    ui::Align align = ui::Align::Stretch;      ///< Where a child sits across the axis, unless its `alignSelf` says.
    ui::Justify justify = ui::Justify::Start;  ///< Where the spare length goes while no shown child stretches.
};

/// Where a child of @p extent sits in @p space under @p align: its offset from the space's start, rounded down when
/// centred; none for a child that fills the space.
[[nodiscard]] int alignedOffset(ui::Align align, int extent, int space) noexcept;

/// The extent across its container a child takes under @p align, in a @p space-cell line: a content child under a
/// non-stretch alignment its requested extent, every other child its sizing's (`layout::crossExtent`); both clamped
/// into the child's bounds and the space.
[[nodiscard]] int alignedExtent(ui::Align align, ui::Sizing sizing, int requested, int space,
                                layout::Bounds bounds) noexcept;

/// The size @p widget asks its container for: its natural size, but a Fixed amount where its layout hints say so.
[[nodiscard]] ::core::tui::Size requestedSize(WidgetBase const& widget);
/// The size @p children ask for stacked along @p axis with @p gap between them (`requestedSize` each); hidden
/// children count nothing.
[[nodiscard]] ::core::tui::Size stackNaturalSize(std::span<WidgetBase* const> children, ui::Axis axis, int gap);
/// Where @p children go inside @p area: one area per child, in order, an empty one for a child that is not drawn.
///
/// With no extents the layout solver sizes the shown children, and a hidden one takes no space. With extents every
/// child takes the next one, in order, so a hidden child keeps its slot and the children after it stay in their
/// columns; a child past the last extent takes its natural extent.
[[nodiscard]] std::vector<::core::tui::Rect> stackAreas(std::span<WidgetBase* const> children, ::core::tui::Rect area,
                                                        StackSpec const& spec);
/// Places @p children inside @p area (in the container's coordinates) as `stackAreas` says. Hidden children get an
/// empty area, so they neither render nor take a click (see `forgetDrawnBounds`).
void arrangeStack(std::span<WidgetBase* const> children, ::core::tui::Rect area, StackSpec const& spec);
/// Draws @p runs one after another from row @p row, column @p column, in @p style, a strong run bold; every cell is
/// monospace already, so a mono run is drawn as the others. A run's '\n' starts the next row at column 0 while
/// @p multiline, and is drawn as a space otherwise.
void paintRuns(::core::tui::Canvas& canvas, std::vector<ui::TextSpan> const& runs, ::core::tui::Style const& style,
               bool multiline, int row = 0, int column = 0);
/// Enter, or Space, with no modifier.
[[nodiscard]] bool isActivation(::core::tui::KeyEvent const& key) noexcept;
/// Appends the view of every focusable widget at or below @p root, depth first in `children()` order: Tab order.
void collectFocusable(WidgetBase& root, std::vector<::core::tui::Component*>& out);
/// Empties the screen bounds of every component below @p top, which a drawn tree's top component (a root widget's
/// view) calls as it renders, before its descendants render.
///
/// core::tui leaves a component's screen bounds as they were when it is not drawn: one whose area is empty or lies
/// outside its parent's. Hit-testing reads those bounds, so a widget squeezed or scrolled out of a frame would still
/// take clicks at the place it last had. Emptied first, the bounds after a frame are those of what it drew.
void forgetDrawnBounds(::core::tui::Component& top);
/// Whether @p node is @p root or one of its descendants.
[[nodiscard]] bool isWithin(::core::tui::Component const* node, ::core::tui::Component const& root) noexcept;
/// Puts @p widget under @p parent, or at the screen's root when @p parent is null.
void attach(Context& context, ui::ContainerWidget* parent, WidgetBase& widget);
/// The top-left corner that centres an overlay of @p size on @p screen's viewport.
[[nodiscard]] ::core::tui::Point centredIn(::core::tui::Screen const& screen, ::core::tui::Size size);
/// Sizes every root widget to the viewport and centres every open dialog's frame; runs before every frame.
///
/// It first ends a press whose pointer capture ended without its release (see `Context::endPress`): core::tui ends the
/// capture on a press that reaches no widget, which no widget sees, or when the pressed view leaves the screen.
///
/// It also empties the screen bounds of each root view, and of each open dialog's frame and everything in it, which
/// the frame that follows sets again for what it draws. core::tui draws neither a hidden view nor one with no room
/// on the screen, and leaves its bounds as they were; shown again, it would take clicks at its old place until the
/// next frame.
void fit(Context& context);
/// The open dialog the user sees whose frame is on top, or null: the one that is modal.
[[nodiscard]] ContainerBase* activeDialog(Context const& context);
/// Counts the active Busy widgets the user could see into `Context::activeBusy`. Showing or hiding any widget, and
/// opening or closing a dialog, calls it, as does `fit`.
void countSpinners(Context& context);
/// Lets the focus follow an open dialog's box that showed or vanished because a widget was shown or hidden: a box
/// that vanished with the focus inside hands it back as closing does, and the box on top that showed takes it. It
/// lives with the dialogs (container_widgets.cpp); showing or hiding any widget calls it.
void followDialogs(Context& context);
/// Moves keyboard focus to @p target, or nowhere for null; returns whether @p target has the focus afterwards.
///
/// The focus leaves the old widget on its own first. A field commits what was typed when it loses the focus, and the
/// handler that runs may destroy widgets, @p target or the whole view among them; core::tui would go on to focus
/// a destroyed target. So @p target takes the focus only if it is still alive and focusable once that is done.
bool focusWidget(Context& context, WidgetBase* target);
/// Moves keyboard focus to @p popup, a component @p owner shows outside its own view (a dropdown's list), as
/// `focusWidget` moves it: the old focus leaves first, and @p popup takes the focus only if @p owner is then still
/// alive and actionable and @p popup still shown. Returns whether @p popup has the focus afterwards; when it does
/// not, @p owner may be gone.
bool focusPopup(Context& context, WidgetBase& owner, ::core::tui::Component& popup);
/// Closes the popup of every widget in `Context::popups` the user can no longer reach.
///
/// A popup is an overlay, outside the tree of the containers that gate its widget, so it would stay open and focused
/// when one of them is hidden, disabled or collapsed. Hiding, showing, disabling and enabling a widget calls this; a
/// container that stops letting its children act by other means (`letsChildrenAct`) must call it too.
void closeUnreachablePopups(Context& context);
/// Moves keyboard focus to the next or previous focusable widget, in `collectFocusable` order, wrapping around at
/// either end: of the roots, or inside the `activeDialog` while there is one, whose frame takes the focus when it
/// holds nothing focusable. Through `focusWidget`, so a widget destroyed meanwhile gets nothing.
void moveFocus(Context& context, Direction direction);
/// Moves keyboard focus into the `activeDialog`: to its first focusable widget, or to its frame when it holds none.
///
/// The old focus leaves first, and the dialog and the widget are picked only then: a field committing what was
/// typed runs a handler, which may destroy widgets, a dialog among them. With no dialog open by then the focus
/// stays nowhere.
void enterDialog(Context& context);

}  // namespace morph::tui::detail

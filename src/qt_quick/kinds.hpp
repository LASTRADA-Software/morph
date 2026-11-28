// SPDX-License-Identifier: Apache-2.0

#pragma once
#include <QHash>
#include <QKeyEvent>
#include <QMetaObject>
#include <QPointer>
#include <QQuickItem>
#include <QQuickWindow>
#include <QString>
#include <QStringList>
#include <QVariantMap>
#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <morph/qt_quick/backend.hpp>
#include <morph/ui/backend.hpp>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

#include "qt_quick/keys.hpp"
#include "qt_quick/widget.hpp"

// Each widget kind of the backend contract over its MorphUi component. A setter writes what the component shows
// and does nothing when it already shows the value; a handler is registered with the widget's relay under the
// event name the component fires.

namespace morph::qt_quick::detail {

// ── Fields ────────────────────────────────────────────────────────────────

/// Runs of text as the list a label's markup is built from, in QML: one map per run holding `text` (escaped for
/// rich text), `strong`, `mono` and `tone` (the `Tone` index, 0 none). The markup itself is built in QML
/// (`MorphWidget.runsHtml`), from the theme and the widget's scheme as they are when it is drawn, so a theme applied
/// after the mount restyles the runs.
[[nodiscard]] QVariantList runsOf(std::vector<ui::TextSpan> const& spans);

class TextImpl final : public FieldItemWidget<ui::TextWidget> {
public:
    TextImpl(Context& context, ui::ContainerWidget* parent);
    void setText(std::string_view text) override;
    void setRole(ui::TextRole role) override;
    void setSpans(std::vector<ui::TextSpan> const& spans) override;
    void setAlign(ui::TextAlign align) override;
    /// A text is not described; the mount never asks.
    void setDescription(std::string_view description) override;
    void setCaptionHint(std::string_view hint) override;
    [[nodiscard]] std::string shownText() const override;
};

class TextInputImpl final : public FieldItemWidget<ui::TextInputWidget> {
public:
    TextInputImpl(Context& context, ui::ContainerWidget* parent, ui::TextInputMode mode);
    void setText(std::string_view text) override;
    void setCursor(std::size_t offset) override { moveCursor(offset); }
    void revealCursor() override;
    void setPlaceholder(std::string_view placeholder) override;
    void setOnChange(std::function<void(std::string)> onChange) override;
    void setOnSubmit(std::function<void(std::string)> onSubmit) override;
    void setOnCommit(std::function<void(std::string)> onCommit) override;
    void setUnit(std::string_view unit) override;
    void setPrefix(std::string_view prefix) override;
    void setMono(bool mono) override;
    void setWrap(bool wrap) override;
    void setAlign(ui::Align align) override;
    void setDecimals(int decimals) override;
    void setIcon(std::string_view icon) override;
    void setCaption(std::string_view label, bool shown) override { showCaption(label, shown); }
    void setEmphasis(ui::InputEmphasis emphasis) override;
    [[nodiscard]] std::string shownText() const override;
    /// The cursor, as a byte offset into the UTF-8 text.
    [[nodiscard]] std::size_t cursor() const;
    /// Moves the cursor to a byte offset, clamped to the text.
    void moveCursor(std::size_t position) const;

private:
    bool _multiline;
};

class CheckboxImpl final : public FieldItemWidget<ui::CheckboxWidget> {
public:
    CheckboxImpl(Context& context, ui::ContainerWidget* parent);
    void setLabel(std::string_view label) override;
    void setChecked(bool checked) override;
    void setOnToggle(std::function<void(bool)> onToggle) override;
    [[nodiscard]] std::string shownText() const override;
    [[nodiscard]] bool checked() const;
};

class SelectImpl final : public FieldItemWidget<ui::SelectWidget> {
public:
    SelectImpl(Context& context, ui::ContainerWidget* parent, ui::SelectStyle style);
    void setOptions(std::vector<ui::SelectOption> const& options) override;
    void setSelected(std::optional<ui::Key> const& key) override;
    void setOnSelect(std::function<void(ui::Key)> onSelect) override;
    void setOnOpen(ui::Action onOpen) override;
    void setCaption(std::string_view label, bool shown) override { showCaption(label, shown); }
    [[nodiscard]] std::string shownText() const override;
    /// The index of the option the select marks, or -1.
    [[nodiscard]] int marked() const;
    /// The options.
    [[nodiscard]] std::vector<ui::SelectOption> const& options() const noexcept { return _options; }

private:
    void markRequested() const;

    std::vector<ui::SelectOption> _options;
    std::optional<ui::Key> _requested;
};

class DateTimeImpl final : public FieldItemWidget<ui::DateTimeInputWidget> {
public:
    DateTimeImpl(Context& context, ui::ContainerWidget* parent, ui::DateMode mode, int offsetMinutes);
    void setValue(std::optional<morph::time::Timestamp> const& value) override;
    void setOnChange(std::function<void(std::optional<morph::time::Timestamp>)> onChange) override;
    void setPattern(std::string_view pattern) override;
    void setCaption(std::string_view label, bool shown) override { showCaption(label, shown); }
    [[nodiscard]] std::string shownText() const override;

private:
    /// @brief Shows @p text, unless the control already shows it, and the day it names as the calendar's.
    /// @param text  The text to show.
    /// @param force Whether to write it even when the control already shows it.
    void show(QString const& text, bool force = false) const;
    /// @brief A day the calendar chose: written with the time of day the field holds, shown and reported.
    /// @param day The day's `[year, month (zero-based), day]`.
    void picked(QVariant const& day);

    ui::DateMode _mode;
    int _offsetMinutes;
    std::string _pattern;
    std::function<void(std::optional<morph::time::Timestamp>)> _onChange;
    QString _shown;
};

/// The text a date-time field shows for an instant, in a zone @p offsetMinutes from UTC: in @p pattern, or ISO for
/// an empty one.
[[nodiscard]] QString formatDateTime(morph::time::DateTime const& instant, ui::DateMode mode, int offsetMinutes,
                                     std::string_view pattern = {});

/// The instant a date-time field's text names in a zone @p offsetMinutes from UTC, in @p pattern or ISO; none for
/// text that names none.
[[nodiscard]] std::optional<morph::time::DateTime> parseDateTime(QString const& text, ui::DateMode mode,
                                                                 int offsetMinutes, std::string_view pattern = {});

class SliderImpl final : public FieldItemWidget<ui::SliderWidget> {
public:
    SliderImpl(Context& context, ui::ContainerWidget* parent);
    void setRange(std::int64_t minimum, std::int64_t maximum, std::int64_t step) override;
    void setValue(std::int64_t value) override;
    void setOnChange(std::function<void(std::int64_t)> onChange) override;
    void setOnCommit(std::function<void(std::int64_t)> onCommit) override;
    [[nodiscard]] std::int64_t value() const;
};

class FilePickerImpl final : public FieldItemWidget<ui::FilePickerWidget> {
public:
    FilePickerImpl(Context& context, ui::ContainerWidget* parent, ui::FilePickerMode mode);
    void setPath(std::string_view path) override;
    void setOnPicked(std::function<void(std::string)> onPicked) override;
    [[nodiscard]] std::string shownText() const override;
};

/// A path for a URL a file dialog or a drop gives: the local file of a `file:` URL, else the text itself.
[[nodiscard]] std::string localPath(QString const& url);

// ── Leaves ────────────────────────────────────────────────────────────────

/// A button; a list entry with content holds it in a slot inside the `Button`, so a press on it is the button's.
// The button is an item and the container of its content's slot at once, as a ContainerItemWidget is: a parent passes
// its scheme down by cross-casting a child item to its ContainerCore, so the two cannot be split into members.
// NOLINTNEXTLINE(misc-multiple-inheritance)
class ButtonImpl final : public ItemWidget<ui::ButtonWidget>, public ContainerCore {
public:
    ButtonImpl(Context& context, ui::ContainerWidget* parent);
    ~ButtonImpl() override;
    ButtonImpl(ButtonImpl const&) = delete;
    ButtonImpl& operator=(ButtonImpl const&) = delete;
    ButtonImpl(ButtonImpl&&) = delete;
    ButtonImpl& operator=(ButtonImpl&&) = delete;
    void setLabel(std::string_view label) override;
    void setOnClick(ui::Action onClick) override;
    void setEmphasis(ui::ButtonEmphasis emphasis) override;
    void setIcon(std::string_view icon) override;
    void setStyle(ui::ButtonStyle style) override;
    void setShape(ui::ControlShape shape) override;
    void setSize(ui::ButtonSize size) override;
    void setWeight(int weight) override;
    void setMono(bool mono) override;
    void setSelected(bool selected) override;
    /// The content, a `MorphSlot` in the button's `content` item, made the first time it is asked for.
    ui::ContainerWidget& content() override;
    [[nodiscard]] std::string shownText() const override;

protected:
    [[nodiscard]] QQuickItem* contentFor(ItemBase& child) override;
    /// The scheme the content draws in: the button's, with its text role following the selection.
    [[nodiscard]] QObject* scheme() const override { return part("contentScheme"); }

private:
    std::unique_ptr<ContainerItemWidget<ui::SlotWidget>> _content;
};

class MenuImpl final : public ItemWidget<ui::MenuWidget> {
public:
    MenuImpl(Context& context, ui::ContainerWidget* parent, ui::MenuStyle style);
    void setItems(std::vector<ui::MenuEntry> const& entries) override;
    void setTrigger(ui::MenuTrigger const& trigger) override;
    /// A button-style menu's trigger label; a bar's is empty.
    [[nodiscard]] std::string shownText() const override;
    void setOnActivate(std::function<void(std::vector<std::size_t>)> onActivate) override;
    /// Chooses the entry at @p path as the user would, opening each submenu on the way.
    void choose(std::vector<std::size_t> const& path) const;

private:
    // Reports each entry's icon the bundle's declared icon set does not name.
    void checkIcons(std::vector<ui::MenuEntry> const& entries) const;
    std::vector<ui::MenuEntry> _entries;
};

/// Whether @p path names an entry the user can choose: not a separator, enabled, without a submenu, inside enabled
/// entries.
[[nodiscard]] bool choosable(std::vector<ui::MenuEntry> const& entries, std::vector<std::size_t> const& path);

/// A menu's entries as MorphMenu reads them: `[{label, icon, keys, checked, enabled, items, separator, description,
/// heading}]`, each submenu's entries in its `items`.
[[nodiscard]] QVariantList menuEntries(std::vector<ui::MenuEntry> const& entries);

class SpacerImpl final : public ItemWidget<ui::SpacerWidget> {
public:
    SpacerImpl(Context& context, ui::ContainerWidget* parent);
};

class BusyImpl final : public ItemWidget<ui::BusyWidget> {
public:
    BusyImpl(Context& context, ui::ContainerWidget* parent);
    void setActive(bool active) override;
    void setLabel(std::string_view label) override;
};

class ToastImpl final : public ItemWidget<ui::ToastWidget> {
public:
    ToastImpl(Context& context, ui::ContainerWidget* parent);
    void show(std::string_view text, ui::Tone tone) override;
};

class BannerImpl final : public ItemWidget<ui::BannerWidget> {
public:
    BannerImpl(Context& context, ui::ContainerWidget* parent);
    void setTone(ui::Tone tone) override;
    void setText(std::string_view text) override;
    void setSpans(std::vector<ui::TextSpan> const& spans) override;
    void setActionLabel(std::string_view label) override;
    void setOnAction(ui::Action onAction) override;
    void setDismissible(bool dismissible) override;
    void setOnDismiss(ui::Action onDismiss) override;
    void setActionStyle(ui::BannerActionStyle style, bool stretch) override;
    [[nodiscard]] std::string shownText() const override;
};

class BadgeImpl final : public ItemWidget<ui::BadgeWidget> {
public:
    BadgeImpl(Context& context, ui::ContainerWidget* parent);
    void setTone(ui::Tone tone) override;
    void setText(std::string_view text) override;
    void setIcon(std::string_view icon) override;
    void setOnRemove(ui::Action onRemove, std::string_view label) override;
    void setStyle(ui::BadgeStyle style) override;
    void setShape(ui::ControlShape shape) override;
    void setMono(bool mono) override;
    [[nodiscard]] std::string shownText() const override;
};

class ProgressImpl final : public ItemWidget<ui::ProgressWidget> {
public:
    ProgressImpl(Context& context, ui::ContainerWidget* parent);
    void setValue(std::optional<double> value) override;
    void setLabel(std::string_view label) override;
};

class StepsImpl final : public ItemWidget<ui::StepsWidget> {
public:
    StepsImpl(Context& context, ui::ContainerWidget* parent);
    void setSteps(std::vector<ui::Step> const& steps) override;
    void setCurrent(std::optional<std::size_t> index) override;
    void setLabelled(bool labelled) override;
    [[nodiscard]] std::string shownText() const override;
};

class ChartImpl final : public ItemWidget<ui::ChartWidget> {
public:
    ChartImpl(Context& context, ui::ContainerWidget* parent);
    void setData(ui::ChartData const& data) override;
    [[nodiscard]] std::string shownText() const override;
};

class KeyValueImpl final : public ItemWidget<ui::KeyValueWidget> {
public:
    KeyValueImpl(Context& context, ui::ContainerWidget* parent);
    void setItems(std::vector<ui::KeyValueItem> const& items) override;
    void setCaptionPlacement(ui::CaptionPlacement placement) override;
    void setCaptionWidth(int units) override;
};

class EmptyStateImpl final : public ItemWidget<ui::EmptyStateWidget> {
public:
    EmptyStateImpl(Context& context, ui::ContainerWidget* parent);
    void setTitle(std::string_view title) override;
    void setText(std::string_view text) override;
    void setIcon(std::string_view icon) override;
    void setActionLabel(std::string_view label) override;
    void setOnAction(ui::Action onAction) override;
    [[nodiscard]] std::string shownText() const override;
};

class CustomImpl final : public ItemWidget<CustomWidget> {
public:
    CustomImpl(Context& context, Context::Made made, ui::ContainerWidget* parent);
    void setTextProp(std::string_view name, std::string_view value) override;
    void setBoolProp(std::string_view name, bool value) override;
    void setNumberProp(std::string_view name, double value) override;
    void setOnEvent(std::function<void(std::string event, std::string payload)> onEvent) override;
};

// ── Containers ────────────────────────────────────────────────────────────

class StackImpl final : public ContainerItemWidget<ui::StackWidget> {
public:
    StackImpl(Context& context, ui::ContainerWidget* parent, ui::Axis axis);
    void setGap(int gap) override;
    void setAlign(ui::Align align) override;
    void setJustify(ui::Justify justify) override;
    void setWrap(bool wrap) override;
    void setDivided(bool divided) override;
    void setPadding(ui::Padding padding) override;
    void setSelected(bool selected) override;
    void setPassesBleed(bool passes) override;
    void setActionRow() override;
    void setHeadBar(bool headBar) override;

protected:
    /// A wrapping row places its children itself, again whenever one's size or visibility changes.
    void adopted(ItemBase& child) override;
    /// A column fills its children's width, a row their height.
    [[nodiscard]] CrossFill crossFill() const noexcept override {
        return CrossFill{.width = _axis == ui::Axis::Vertical, .height = _axis == ui::Axis::Horizontal};
    }
    /// A column places its children horizontally, a row vertically.
    [[nodiscard]] int alignsAcross() const noexcept override { return _axis == ui::Axis::Vertical ? 1 : 2; }

private:
    ui::Axis _axis;
    bool _wrap = false;
};

class GridImpl final : public ContainerItemWidget<ui::GridWidget> {
public:
    GridImpl(Context& context, ui::ContainerWidget* parent);
    void setColumns(int columns) override;
    void setTracks(std::vector<ui::GridTrack> const& tracks) override;
    void setMinColumnWidth(int units) override;
    void setGap(int gap) override;
    void setSpan(ui::Widget& child, int span) override;
    void setAlign(ui::Align align) override;

protected:
    /// A cell's content is placed vertically in its row.
    [[nodiscard]] int alignsAcross() const noexcept override { return 2; }
    /// A grid places its cells itself, again whenever one's size, visibility or span changes.
    void adopted(ItemBase& child) override;

private:
    /// Lays the grid out again.
    void place() const;
};

class PanelImpl final : public ContainerItemWidget<ui::PanelWidget> {
public:
    PanelImpl(Context& context, ui::ContainerWidget* parent);
    void setTitle(std::string_view title) override;
    void setHeader(ui::Widget& header) override;
    void setPadding(int padding) override;
    void setCollapsible(bool collapsible) override;
    void setCollapsed(bool collapsed) override;
    void setOnToggle(std::function<void(bool)> onToggle) override;

protected:
    // A panel that names no surface is a card, so its content draws with the card's scheme.
    [[nodiscard]] QObject* scheme() const override;
};

class ScrollImpl final : public ContainerItemWidget<ui::ScrollWidget> {
public:
    ScrollImpl(Context& context, ui::ContainerWidget* parent, ui::Axis axis);
};

class SlotImpl final : public ContainerItemWidget<ui::SlotWidget> {
public:
    SlotImpl(Context& context, ui::ContainerWidget* parent);
    void setScrollable(bool scrollable) override;
};

/// An overlay's footer: a `MorphFooter` row of its actions, which never scrolls.
class FooterItem final : public ContainerItemWidget<ui::SlotWidget> {
public:
    using ContainerItemWidget<ui::SlotWidget>::ContainerItemWidget;
    void setScrollable(bool scrollable) override { static_cast<void>(scrollable); }
};

class TabsImpl final : public ContainerItemWidget<ui::TabsWidget> {
public:
    TabsImpl(Context& context, ui::ContainerWidget* parent);
    void setTabs(std::vector<std::string> const& labels) override;
    void setSelected(std::size_t index) override;
    void setOnSelect(std::function<void(std::size_t)> onSelect) override;
    void setBarShown(bool shown) override;
    void setMoreLabel(std::string_view label) override;
    void setInset(int inset) override;
    void setPageInset(int inset) override;
    void setStripSurface(std::string_view surface) override;
    /// The highlighted tab, or -1.
    [[nodiscard]] int highlighted() const;
    /// The highlighted tab's caption; empty when none is highlighted.
    [[nodiscard]] std::string shownText() const override;
};

/// A dialog or a drawer: a modal overlay whose content is shown only while it is open.
// NOLINTBEGIN(portability-template-virtual-member-function): the template implements the overlay interfaces' virtual
// functions, and is instantiated only as the dialog's base and as the drawer, whose virtual functions are all used
// through the interface; every compiler then instantiates all of them.
template <class Interface>
class OverlayImpl : public ContainerItemWidget<Interface> {
public:
    OverlayImpl(Context& context, QString const& type, ui::ContainerWidget* parent, QVariantMap initial = {})
        : ContainerItemWidget<Interface>{context, type, parent, std::move(initial)} {}
    ~OverlayImpl() override = default;
    OverlayImpl(OverlayImpl const&) = delete;
    OverlayImpl& operator=(OverlayImpl const&) = delete;
    OverlayImpl(OverlayImpl&&) = delete;
    OverlayImpl& operator=(OverlayImpl&&) = delete;

    /// The footer, a `MorphFooter` in the overlay's `footer` item, made the first time it is asked for.
    ui::ContainerWidget& footer() override {
        if (_footer == nullptr) {
            _makingFooter = true;
            try {
                _footer = std::make_unique<FooterItem>(this->context(), QStringLiteral("MorphFooter"), this);
            } catch (...) {
                _makingFooter = false;
                throw;
            }
            _makingFooter = false;
        }
        return *_footer;
    }
    void setOpen(bool open) override {
        if (this->read("shown").toBool() != open) {
            this->call("show", open);
        }
    }
    void setTitle(std::string_view title) override { this->write("title", toQString(title)); }
    void setDescription(std::string_view description) override {
        this->write("descriptionRuns", QVariantList{});
        this->write("description", toQString(description));
    }
    void setDescriptionSpans(std::vector<ui::TextSpan> const& spans) override {
        this->write("description", toQString(ui::spansText(spans)));
        this->write("descriptionRuns", runsOf(spans));
    }
    void setHeadNote(std::string_view note) override { this->write("headNote", toQString(note)); }
    // The note is drawn by the footer item, so a note needs one even when no action asked for it.
    void setFooterNote(std::string_view note) override {
        if (!note.empty()) {
            static_cast<void>(footer());
        }
        this->write("footerNote", toQString(note));
    }
    void setCloseButton(bool shown) override { this->write("closeButton", shown); }
    void setDismissLabel(std::string_view label) override { this->write("dismissLabel", toQString(label)); }
    void setOnDismiss(ui::Action onDismiss) override {
        this->relay().on(QStringLiteral("dismiss"), [onDismiss = std::move(onDismiss)](QVariant const&) {
            if (onDismiss) {
                onDismiss();
            }
        });
    }
    /// Whether it is open.
    [[nodiscard]] bool open() const { return this->read("shown").toBool(); }
    /// Dismisses it as Escape would.
    void dismiss() const { this->call("dismiss"); }

protected:
    // The footer goes in the overlay's `footer` item; every other child in its content.
    [[nodiscard]] QQuickItem* contentFor(ItemBase& child) override {
        static_cast<void>(child);
        return qobject_cast<QQuickItem*>(this->part(_makingFooter ? "footer" : "content"));
    }
    // The content draws on the card the overlay is, whatever surface encloses the node that declares it.
    [[nodiscard]] QObject* scheme() const override { return this->context().themeObject("cardScheme"); }

private:
    std::unique_ptr<FooterItem> _footer;
    bool _makingFooter = false;
};
// NOLINTEND(portability-template-virtual-member-function)

/// A dialog: an overlay that can be drawn as a sheet filling the window.
class DialogImpl final : public OverlayImpl<ui::DialogWidget> {
public:
    using OverlayImpl<ui::DialogWidget>::OverlayImpl;
    void setSheet(bool sheet) override { write("sheet", sheet); }
};

using DrawerImpl = OverlayImpl<ui::DrawerWidget>;

/// A popover: a non-modal panel under its container that floats over the content and takes no room.
class PopoverImpl final : public ContainerItemWidget<ui::PopoverWidget> {
public:
    PopoverImpl(Context& context, ui::ContainerWidget* parent);
    void setOpen(bool open) override;
    void setWidth(int units) override;
    void setOnDismiss(ui::Action onDismiss) override;

protected:
    /// Drawn as the theme's card whatever surface encloses it.
    [[nodiscard]] QObject* scheme() const override { return context().themeObject("cardScheme"); }
};

/// A guided tour's spotlight (`MorphSpotlight`): it finds its target in its own context's index, follows the
/// target's box each frame, scrolls a clipped target into view, and while open filters the window's keys so the card's
/// chords come first and Tab stays in the card.
class SpotlightImpl final : public ContainerItemWidget<ui::SpotlightWidget> {
public:
    SpotlightImpl(Context& context, ui::ContainerWidget* parent);
    ~SpotlightImpl() override;
    SpotlightImpl(SpotlightImpl const&) = delete;
    SpotlightImpl& operator=(SpotlightImpl const&) = delete;
    SpotlightImpl(SpotlightImpl&&) = delete;
    SpotlightImpl& operator=(SpotlightImpl&&) = delete;

    void setOpen(bool open) override;
    void setTarget(std::optional<std::string> testId) override;
    [[nodiscard]] bool targetShown() const override { return _ringed != nullptr; }
    void setOnTargetShownChanged(std::function<void(bool)> handler) override { _onChanged = std::move(handler); }
    void setTargetFocusable(bool focusable) override { _targetFocusable = focusable; }

    /// The widget it rings now, or null.
    [[nodiscard]] ItemBase const* ringed() const noexcept { return _ringed; }
    /// Reads the target's box again, scrolls a clipped target into view and writes the cut-out: what each frame runs.
    void track();
    /// Picks the target again from the context's index, and hears its candidates' visibility.
    void resolve();
    /// A key pressed while it is open, before the focused item has it: true when it took the key.
    bool filterKey(QKeyEvent const& event);

protected:
    /// The card draws as the theme's card, whatever surface encloses the spotlight.
    [[nodiscard]] QObject* scheme() const override { return context().themeObject("cardScheme"); }

private:
    class Filter;
    // The card's items that Tab stops at, then the target while it is focusable.
    [[nodiscard]] std::vector<QQuickItem*> tabChain() const;
    void cycleFocus(bool forward);
    void focusCard();

    std::unique_ptr<Filter> _filter;
    std::optional<QString> _target;
    ItemBase* _ringed = nullptr;
    std::vector<QMetaObject::Connection> _watching;
    QMetaObject::Connection _frames;
    QPointer<QQuickItem> _restore;
    QPointer<QQuickWindow> _window;
    int _listener = 0;
    bool _open = false;
    bool _targetFocusable = false;
    bool _resolving = false;
    bool _scrollPending = false;
    std::function<void(bool)> _onChanged;
};

class SplitterImpl final : public ContainerItemWidget<ui::SplitterWidget> {
public:
    SplitterImpl(Context& context, ui::ContainerWidget* parent, ui::Axis axis);
    void setSizes(std::vector<int> const& sizes) override;
    void setOnResize(std::function<void(std::vector<int>)> onResize) override;

protected:
    /// The split view sizes its panes itself.
    [[nodiscard]] CrossFill crossFill() const noexcept override { return {}; }
    [[nodiscard]] QQuickItem* contentFor(ItemBase& child) override;
    void adopted(ItemBase& child) override;
    // NOLINTNEXTLINE(bugprone-easily-swappable-parameters): a move's two places, in the order moveChild gives them.
    void reorder(ItemBase& child, std::size_t origin, std::size_t destination) override;
    void releasing(ItemBase& child) noexcept override;
};

class CollapsibleImpl final : public ContainerItemWidget<ui::CollapsibleWidget> {
public:
    CollapsibleImpl(Context& context, ui::ContainerWidget* parent);
    void setTitle(std::string_view title) override;
    void setHeader(ui::Widget& header) override;
    void setOpen(bool open) override;
    void setOnToggle(std::function<void(bool)> onToggle) override;
    /// Opens or closes it with its own control, as the user would.
    void toggleByUser() const;
};

class PageRegionImpl;

/// A sheet: the item displays it scaled, and this widget computes the scale and every region's rectangle with
/// `ui::layoutPage`, from the width the item reports and the content height each region reports.
class PageImpl final : public ContainerItemWidget<ui::PageWidget> {
public:
    PageImpl(Context& context, ui::ContainerWidget* parent, ui::PageGeometry const& geometry);
    ~PageImpl() override;
    PageImpl(PageImpl const&) = delete;
    PageImpl& operator=(PageImpl const&) = delete;
    PageImpl(PageImpl&&) = delete;
    PageImpl& operator=(PageImpl&&) = delete;

    /// Adds a region to the layout, in creation order.
    void join(PageRegionImpl& region);
    /// Forgets a region that is going.
    void leave(PageRegionImpl& region) noexcept;
    /// Lays the sheet out and writes the scale and the rectangles into the items.
    void relayout();

private:
    ui::PageGeometry _geometry;
    std::optional<ui::Mm> _available;
    std::vector<PageRegionImpl*> _regions;
};

class PageRegionImpl final : public ContainerItemWidget<ui::PageRegionWidget> {
public:
    PageRegionImpl(Context& context, ui::ContainerWidget* parent, ui::RegionGeometry const& geometry);
    ~PageRegionImpl() override;
    PageRegionImpl(PageRegionImpl const&) = delete;
    PageRegionImpl& operator=(PageRegionImpl const&) = delete;
    PageRegionImpl(PageRegionImpl&&) = delete;
    PageRegionImpl& operator=(PageRegionImpl&&) = delete;

    /// The region's place.
    [[nodiscard]] ui::RegionGeometry const& geometry() const noexcept { return _geometry; }
    /// The height the content asks for.
    [[nodiscard]] ui::Mm contentHeight() const noexcept { return _contentHeight; }
    /// Writes the region's rectangle and overflow into the item.
    void show(ui::RegionLayout const& layout) const;
    /// The page lets go of the region first when it is destroyed first.
    void orphan() noexcept { _page = nullptr; }

private:
    PageImpl* _page;
    ui::RegionGeometry _geometry;
    ui::Mm _contentHeight{};
};

class DropZoneImpl final : public ContainerItemWidget<ui::DropZoneWidget> {
public:
    DropZoneImpl(Context& context, ui::ContainerWidget* parent);
    void setAccept(std::vector<std::string> const& extensions) override;
    void setMultiple(bool multiple) override;
    void setOnDrop(std::function<void(std::vector<std::string>)> onDrop) override;
    /// Drops files as the operating system would; returns whether the zone took them.
    [[nodiscard]] bool offer(std::vector<std::string> const& paths) const;
    /// Whether the zone takes a drop of these paths.
    [[nodiscard]] bool takes(std::vector<std::string> const& paths) const;

private:
    std::vector<std::string> _accept;
    bool _multiple = false;
};

// ── Tables ────────────────────────────────────────────────────────────────

class TableRowImpl;

class TableImpl final : public ContainerItemWidget<ui::TableWidget> {
public:
    TableImpl(Context& context, ui::ContainerWidget* parent);
    void setColumns(std::vector<ui::TableColumn> const& columns) override;
    void setShowHeader(bool shown) override;
    void setSelectionMode(ui::SelectionMode mode) override;
    void setRowKey(ui::Widget& row, ui::Key const& key) override;
    void setRowTone(ui::Widget& row, std::optional<ui::Tone> tone) override;
    void setSelectColumn(bool shown) override;
    void setSelectAllLabel(std::string_view label) override;
    void setRowLabel(ui::Widget& row, std::string_view label) override;
    /// Throws std::logic_error when @p row is not one of this table's rows.
    void setRowMarks(ui::Widget& row, ui::RowMarks const& marks) override;
    void setRowHeading(ui::Widget& row) override;
    void setRowSummary(ui::Widget& row) override;
    /// Throws std::logic_error when @p row is not one of this table's rows.
    void setRowSpans(ui::Widget& row, std::vector<ui::RowSpan> const& spans) override;
    void setSelection(std::vector<ui::Key> const& keys) override;
    void setOnSelectionChange(
        std::function<void(std::vector<ui::Key>, ui::SelectionChange)> onSelectionChange) override;
    void setOnActivate(std::function<void(ui::Key)> onActivate) override;
    /// Each header of the chain carries its mark (`ui::sortMarkText`) and its accessible description
    /// (`ui::sortDescription`), which MorphTable reads as `sortMarks` and `sortDescriptions`, by column.
    void setSort(std::vector<ui::TableSort> const& chain) override;
    void setOnSort(std::function<void(std::size_t, ui::HeaderClick)> onSort) override;
    /// A menu opens through MorphTable's `menu` event: its entries are asked for then, and handed to MorphTable's
    /// `showMenu`, which shows them in a button-style MorphMenu's popup under the header or the `⋯`.
    void setMenu(std::function<std::vector<ui::MenuEntry>(std::optional<std::size_t>)> entries,
                 std::function<void(std::optional<std::size_t>, std::vector<std::size_t>)> onChoose) override;
    /// A cell's menu opens through its row's `cellMenu` event (a right click or a long press on the cell, or the menu
    /// key on the focused cell): its entries are asked for then, and handed to MorphTable's `showCellMenu`, which
    /// shows them in the header menu's popup under the cell; a choice comes back as `menuChoose` with the column
    /// `kCellMenuColumn`. A key the table leaves is offered to the focused cell's entries through `cellKey`.
    void setCellMenu(
        std::function<std::vector<ui::MenuEntry>(ui::Key const&, std::size_t)> entries,
        std::function<void(ui::Key const&, std::size_t, std::vector<std::size_t> const&)> chosen) override;
    void setCellMenuKeys(std::vector<std::string> const& chords) override { _cellMenuKeys = chords; }
    void setVisibleColumns(std::vector<std::size_t> const& visible) override;
    void setOnColumnMove(std::function<void(std::size_t, std::size_t)> handler) override;
    /// MorphTable reports the header's width in pixels; its padding is taken off and the rest rounded to units of
    /// `Theme.unitWidth` here.
    void setOnColumnResize(std::function<void(std::size_t, std::optional<int>)> handler) override;
    void setOnFilter(std::function<void(std::size_t)> handler) override;
    void setFiltered(std::vector<std::size_t> const& columns) override;
    void setColumnGroups(std::vector<ui::TableColumnGroup> const& groups) override;
    void setBands(std::vector<ui::BandRun> const& runs) override;
    void setOnGroupFilter(std::function<void(std::size_t)> handler) override;
    void setOnCopy(std::function<void()> onCopy) override;
    void setExistingKeys(std::vector<ui::Key> const& keys) override;
    void setOnSelectAll(std::function<void()> onSelectAll) override;
    void setSelectionView(std::vector<ui::Key> const& except) override;
    void setOnStatusEntry(std::function<void(std::string entryId)> onStatusEntry) override;
    void setRealizedWindow(std::size_t first, std::size_t count, std::size_t total) override;
    void setOnVisibleRangeChanged(std::function<void(std::size_t, std::size_t)> onVisibleRangeChanged) override;
    /// Throws std::logic_error when @p row is not one of this table's rows.
    void setRowIndex(ui::Widget& row, std::size_t viewRow) override;
    /// The status line: one line of text under the rows, a polite live region, with "Retry" while the state offers
    /// it; it takes no space while not shown.
    void setStatus(ui::TableStatus const& status) override;
    void setOnRetry(std::function<void()> onRetry) override;
    /// Throws std::logic_error when @p row is not one of this table's rows.
    void setCellLoad(ui::Widget& row, std::size_t column, ui::CellLoad load) override;
    /// The row's cell draws the tone's tint under its selection and focus marks, the theme's error mark in place of
    /// an invalid value, and the "more here" mark and a tooltip (its accessible description) for a tooltip. Throws
    /// std::logic_error when @p row is not one of this table's rows.
    void setCellAccent(ui::Widget& row, std::size_t column, ui::CellAccent const& accent) override;
    /// The window's columns, offsets and widths are pixels: the header and the rows draw these columns only.
    void setColumnWindow(std::span<std::size_t const> columns, std::span<int const> offsets,
                         std::span<int const> widths, int totalWidth, std::size_t pinned) override;
    /// Only a table over a `TableSource` is given one, before any row: MorphTable then places each row by its view
    /// index and each cell by its column.
    void setOnHorizontalRange(std::function<void(int scrollX, int viewport)> handler) override;
    void setHorizontalOffset(int scrollX) override;
    void setOnColumnMeasured(
        std::function<void(std::size_t column, ui::ColumnMeasure const& measure)> handler) override;
    /// Pixels: a unit is `Theme.unitWidth`, a monospace character `Theme.monoAdvance`, a content column starts at
    /// twelve units, and a column's padding is `table.cellPaddingX` a side, as the unwindowed table sizes them.
    [[nodiscard]] ui::TableMeasure tableMeasure() const override;
    void setFocusCell(std::size_t viewRow, std::size_t column) override;
    void setOnFocusMove(std::function<void(ui::FocusMove)> handler) override;
    /// MorphTable reports printable text typed on its focused cell, while no editor is open, as its `text` event.
    void setOnText(std::function<void(std::string)> onText) override;
    /// A row reports a tap on a cell as its `cellClick` event, `{column, double}`.
    void setOnCellClick(
        std::function<void(std::size_t viewRow, std::size_t column, bool doubleClick)> onClick) override;
    /// MorphTable reports its `focusOut` event once the active focus is outside the table, its editor and its popups.
    void setOnFocusOut(std::function<void()> onFocusOut) override;
    /// The editor's item takes the cell's column, the cell in that column is covered, and the row places the editor
    /// over the cell's rectangle. Throws std::logic_error when @p row is not one of this table's rows.
    void setCellEditor(ui::Widget& row, std::size_t column, ui::Widget* editor) override;
    /// Throws std::logic_error when @p row is not one of this table's rows.
    void setCellMarks(ui::Widget& row, std::size_t column, ui::CellMarks const& marks) override;
    /// Throws std::logic_error when @p row is not one of this table's rows.
    void setRowModified(ui::Widget& row, bool modified) override;
    /// MorphTable draws the rectangle (`cellRect`, `[firstRow, rowCount, firstPosition, positionCount]`, empty for
    /// none) over the rows it spans and the visible columns at those positions.
    void setCellSelection(std::optional<std::array<std::size_t, 4>> rect) override;
    /// MorphTable selects cells (`selectsCells`): a drag from cell to cell, a Shift-click, a click on a row's leading
    /// edge and a Ctrl-click on a column's header report its `cellSelect` event, and no row is selected.
    void setOnCellSelect(std::function<void(std::size_t anchorRow, std::size_t anchorPosition, std::size_t focusRow,
                                            std::size_t focusPosition)>
                             onSelect) override;
    /// A tap on a row's cell, reported by the row.
    void cellClicked(TableRowImpl const& row, std::size_t column, bool doubleClick) const;
    /// Shift with an arrow, a Shift-click and Ctrl+A over a source go to the handler as view rows.
    void setOnSelectRange(
        std::function<void(std::size_t anchorRow, std::size_t focusRow, ui::SelectionChange)> handler) override;
    void setCounts(std::optional<std::int64_t> rows, std::size_t columns) override;
    void scrollToRow(std::size_t viewRow) override;
    /// Throws std::logic_error when @p row is not one of this table's rows.
    void setCellColumn(ui::Widget& row, ui::Widget& cell, std::optional<std::size_t> column) override;

    /// A click on a row, or Space or Shift with an arrow on the current row, as @p click; the table's selection model
    /// (`ui::TableWidget`).
    void tapped(TableRowImpl const& row, ui::RowClick click);
    /// Makes @p row the anchor of a range when the table has none, or its anchor's row is gone: the row Shift with an
    /// arrow leaves.
    void anchorAt(TableRowImpl const& row);
    /// Ctrl+A in `Multiple` mode: every row, or none when every row is selected already.
    void selectAll();
    /// Opens the menu of @p row's cell in @p column, after making the row the selection alone when it is not selected.
    void openCellMenu(TableRowImpl const& row, std::size_t column);
    /// The column MorphTable reports a cell menu's choice with: no header has it.
    static constexpr qlonglong kCellMenuColumn = -2;
    /// A double click on a row, or Enter on it.
    void activated(TableRowImpl const& row) const;
    /// The Flickable reported a scroll; @p first and @p count are what it wants realized next.
    void scrolled(std::size_t first, std::size_t count) const;
    /// Whether a row is marked selected.
    [[nodiscard]] static bool marked(ItemBase const& row);
    /// Whether a row is a group heading.
    [[nodiscard]] static bool heading(ItemBase const& row);
    /// Whether a row is never current, selected, counted or activated: a heading or a summary row.
    [[nodiscard]] static bool passive(ItemBase const& row);

protected:
    void adopted(ItemBase& child) override;
    void moved() override;
    void releasing(ItemBase& child) noexcept override;

private:
    void mark() const;
    /// Works out which cells span and which are covered from the rows as they stand (without @p leaving), clipping a
    /// span that runs past the last row or onto a heading, hides the covered cells and tells each row its spans and
    /// the columns above cover.
    void planSpans(ItemBase const* leaving);
    /// Writes `alternate` on every row but @p leaving: true at each odd position.
    void stripe(ItemBase const* leaving) const noexcept;
    [[nodiscard]] std::vector<ui::Key> existingSelection() const;
    /// Every key the table has, realized or not, in row order.
    [[nodiscard]] std::vector<ui::Key> rowKeys() const;
    /// Makes @p next the requested keys, marks the rows and reports the existing ones as @p change.
    void select(std::vector<ui::Key> next, ui::SelectionChange change);

    ui::SelectionMode _mode = ui::SelectionMode::None;
    std::vector<ui::Key> _requested;
    std::function<void(std::vector<ui::Key>, ui::SelectionChange)> _onSelectionChange;
    // Ctrl+A's and the header box's handler in place of selecting the rows held; empty for none.
    std::function<void()> _onSelectAll;
    // Where a range starts: the row last clicked or toggled.
    std::optional<ui::Key> _anchor;
    std::function<void(ui::Key)> _onActivate;
    std::function<void(std::size_t, ui::HeaderClick)> _onSort;
    std::function<std::vector<ui::MenuEntry>(std::optional<std::size_t>)> _menu;
    std::function<void(std::optional<std::size_t>, std::vector<std::size_t>)> _onMenu;
    std::function<std::vector<ui::MenuEntry>(ui::Key const&, std::size_t)> _cellMenu;
    std::function<void(ui::Key const&, std::size_t, std::vector<std::size_t> const&)> _cellChosen;
    /// The cell whose menu is open; none before any.
    std::optional<std::pair<ui::Key, std::size_t>> _cellMenuAt;
    /// The chords that ask for the focused cell's entries.
    std::vector<std::string> _cellMenuKeys;
    std::function<void(std::size_t, std::size_t)> _onColumnMove;
    std::function<void(std::size_t, std::optional<int>)> _onColumnResize;
    std::function<void(std::size_t)> _onFilter;
    std::function<void(std::size_t)> _onGroupFilter;
    std::function<void()> _onCopy;
    // Every key the table's data has, realized or dewindowed; set only above a virtualized table's threshold
    // (mount.hpp never calls setExistingKeys below it). Empty below threshold, where existingSelection() falls
    // back to realized children alone, which is already exactly every key that exists at that size.
    std::unordered_set<ui::Key> _existingKeys;
    // The same keys, in row order, for a range or Ctrl+A over rows that are not realized.
    std::vector<ui::Key> _existingOrder;
    // The keys of the summary rows, which no selection, count or Ctrl+A takes in.
    std::vector<ui::Key> _summaryKeys;
    std::function<void(std::size_t, std::size_t)> _onVisibleRangeChanged;
    std::function<void()> _onRetry;
    std::function<void(std::string)> _onStatusEntry;
    // Engaged while the selection is of the view: every row but these keys is marked, until the next setSelection.
    std::optional<std::vector<ui::Key>> _viewExcept;
    std::function<void(int, int)> _onHorizontalRange;
    std::function<void(std::size_t, ui::ColumnMeasure const&)> _onColumnMeasured;
    std::function<void(ui::FocusMove)> _onFocusMove;
    std::function<void(std::size_t, std::size_t, ui::SelectionChange)> _onSelectRange;
    std::function<void(std::string)> _onText;
    std::function<void(std::size_t, std::size_t, bool)> _onCellClick;
    std::function<void(std::size_t, std::size_t, std::size_t, std::size_t)> _onCellSelect;
    std::function<void()> _onFocusOut;
    /// Each row's open cell editor's item, by row.
    QHash<TableRowImpl const*, QPointer<QQuickItem>> _editors;
    /// Each row's effective spans, as last told.
    std::unordered_map<ItemBase const*, std::vector<ui::RowSpan>> _spans;
    /// Whether any row has been told it spans or is covered, so a table that never spans does no work.
    bool _planned = false;
};

/// A row of a table: the horizontal stack the mount makes for it, placing its cells by column.
class TableRowImpl final : public ContainerItemWidget<ui::StackWidget> {
public:
    TableRowImpl(Context& context, ui::ContainerWidget* parent, TableImpl& table);
    void setGap(int gap) override;
    /// The table places a row's cells in its columns; the mount never aligns a table row.
    void setAlign(ui::Align align) override { static_cast<void>(align); }
    /// As `setAlign`.
    void setJustify(ui::Justify justify) override { static_cast<void>(justify); }
    /// As `setAlign`.
    void setWrap(bool wrap) override { static_cast<void>(wrap); }
    /// As `setAlign`.
    void setDivided(bool divided) override { static_cast<void>(divided); }
    /// As `setAlign`.
    void setPadding(ui::Padding padding) override { static_cast<void>(padding); }
    /// As `setAlign`: the table draws the selection.
    void setSelected(bool selected) override { static_cast<void>(selected); }
    /// As `setAlign`: a table row is never a holder.
    void setPassesBleed(bool passes) override { static_cast<void>(passes); }
    /// As `setAlign`: a table row is never a row of actions.
    void setActionRow() override {}
    /// As `setAlign`: a table row is never the head bar.
    void setHeadBar(bool headBar) override { static_cast<void>(headBar); }
    /// Its key, once the table was told it.
    [[nodiscard]] std::optional<ui::Key> const& key() const noexcept { return _key; }
    void setKey(ui::Key key);
    /// Writes the load state of the row's cell in @p column to the row's `cellLoads`, which holds the columns not
    /// loaded.
    void setLoad(std::size_t column, ui::CellLoad load);
    /// Writes the accent of the row's cell in @p column to the row's `cellAccents`, which holds the columns with one:
    /// `{tone, tooltip, invalid}`, the tone -1 for none.
    void setAccent(std::size_t column, ui::CellAccent const& accent);
    /// Writes the marks of the row's cell in @p column to the row's `cellMarks`, which holds the columns with some:
    /// `{readOnly, modified, error, stale, muted}`.
    void setMarks(std::size_t column, ui::CellMarks const& marks);
    /// Its view row, once the table was told it.
    [[nodiscard]] std::optional<std::size_t> viewRow() const noexcept { return _viewRow; }
    /// Remembers the view row the table told it.
    void setViewRow(std::size_t viewRow) noexcept { _viewRow = viewRow; }

protected:
    /// The table places a row's cells in its columns.
    [[nodiscard]] CrossFill crossFill() const noexcept override { return {}; }
    void adopted(ItemBase& child) override;

private:
    std::optional<ui::Key> _key;
    /// The columns not loaded, by column as text: `1` unloaded, `2` failed.
    QVariantMap _loads;
    /// The columns with an accent, by column as text.
    QVariantMap _accents;
    /// The columns with marks, by column as text.
    QVariantMap _marks;
    std::optional<std::size_t> _viewRow;
};

}  // namespace morph::qt_quick::detail

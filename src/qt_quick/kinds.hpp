// SPDX-License-Identifier: Apache-2.0

#pragma once
#include <QString>
#include <QStringList>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <morph/qt_quick/backend.hpp>
#include <morph/ui/backend.hpp>
#include <optional>
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
    /// A text is not described; the mount never asks.
    void setDescription(std::string_view description) override;
    [[nodiscard]] std::string shownText() const override;
};

class TextInputImpl final : public FieldItemWidget<ui::TextInputWidget> {
public:
    TextInputImpl(Context& context, ui::ContainerWidget* parent, ui::TextInputMode mode);
    void setText(std::string_view text) override;
    void setPlaceholder(std::string_view placeholder) override;
    void setOnChange(std::function<void(std::string)> onChange) override;
    void setOnSubmit(std::function<void(std::string)> onSubmit) override;
    void setOnCommit(std::function<void(std::string)> onCommit) override;
    void setUnit(std::string_view unit) override;
    void setPrefix(std::string_view prefix) override;
    void setMono(bool mono) override;
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
    void setGap(int gap) override;
    void setSpan(ui::Widget& child, int span) override;
    void setAlign(ui::Align align) override;

protected:
    /// A cell's content is placed vertically in its row.
    [[nodiscard]] int alignsAcross() const noexcept override { return 2; }
    /// A grid with tracks places its cells itself, again whenever one's size, visibility or span changes.
    void adopted(ItemBase& child) override;

private:
    /// Lays a grid with tracks out again; a grid without them leaves it to its GridLayout.
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
    void setDismissLabel(std::string_view label) override { write("dismissLabel", toQString(label)); }
};

using DrawerImpl = OverlayImpl<ui::DrawerWidget>;

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
    void setRowHeading(ui::Widget& row) override;
    void setSelection(std::vector<ui::Key> const& keys) override;
    void setOnSelectionChange(
        std::function<void(std::vector<ui::Key>, ui::SelectionChange)> onSelectionChange) override;
    void setOnActivate(std::function<void(ui::Key)> onActivate) override;
    void setSort(std::optional<ui::TableSort> sort) override;
    void setOnSort(std::function<void(std::size_t)> onSort) override;
    void setExistingKeys(std::vector<ui::Key> const& keys) override;
    void setRealizedWindow(std::size_t first, std::size_t count, std::size_t total) override;
    void setOnVisibleRangeChanged(std::function<void(std::size_t, std::size_t)> onVisibleRangeChanged) override;

    /// A click on a row, or Space or Shift with an arrow on the current row, as @p click; the table's selection model
    /// (`ui::TableWidget`).
    void tapped(TableRowImpl const& row, ui::RowClick click);
    /// Makes @p row the anchor of a range when the table has none, or its anchor's row is gone: the row Shift with an
    /// arrow leaves.
    void anchorAt(TableRowImpl const& row);
    /// Ctrl+A in `Multiple` mode: every row, or none when every row is selected already.
    void selectAll();
    /// A double click on a row, or Enter on it.
    void activated(TableRowImpl const& row) const;
    /// The Flickable reported a scroll; @p first and @p count are what it wants realized next.
    void scrolled(std::size_t first, std::size_t count) const;
    /// Whether a row is marked selected.
    [[nodiscard]] static bool marked(ItemBase const& row);
    /// Whether a row is a group heading.
    [[nodiscard]] static bool heading(ItemBase const& row);

protected:
    void adopted(ItemBase& child) override;
    void moved() override;
    void releasing(ItemBase& child) noexcept override;

private:
    void mark() const;
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
    // Where a range starts: the row last clicked or toggled.
    std::optional<ui::Key> _anchor;
    std::function<void(ui::Key)> _onActivate;
    std::function<void(std::size_t)> _onSort;
    // Every key the table's data has, realized or dewindowed; set only above a virtualized table's threshold
    // (mount.hpp never calls setExistingKeys below it). Empty below threshold, where existingSelection() falls
    // back to realized children alone, which is already exactly every key that exists at that size.
    std::unordered_set<ui::Key> _existingKeys;
    // The same keys, in row order, for a range or Ctrl+A over rows that are not realized.
    std::vector<ui::Key> _existingOrder;
    std::function<void(std::size_t, std::size_t)> _onVisibleRangeChanged;
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
    /// Its key, once the table was told it.
    [[nodiscard]] std::optional<ui::Key> const& key() const noexcept { return _key; }
    void setKey(ui::Key key);

protected:
    /// The table places a row's cells in its columns.
    [[nodiscard]] CrossFill crossFill() const noexcept override { return {}; }
    void adopted(ItemBase& child) override;

private:
    std::optional<ui::Key> _key;
};

}  // namespace morph::qt_quick::detail

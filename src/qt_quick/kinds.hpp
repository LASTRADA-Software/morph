// SPDX-License-Identifier: Apache-2.0

#pragma once
#include <QString>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <morph/qt_quick/backend.hpp>
#include <morph/ui/backend.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "qt_quick/keys.hpp"
#include "qt_quick/widget.hpp"

// Each widget kind of the backend contract over its MorphUi component. A setter writes what the component shows
// and does nothing when it already shows the value; a handler is registered with the widget's relay under the
// event name the component fires.

namespace morph::qt_quick::detail {

// ── Fields ────────────────────────────────────────────────────────────────

class TextImpl final : public FieldItemWidget<ui::TextWidget> {
public:
    TextImpl(Context& context, ui::ContainerWidget* parent);
    void setText(std::string_view text) override;
    void setRole(ui::TextRole role) override;
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
    [[nodiscard]] std::string shownText() const override;

private:
    void show(QString const& text) const;

    ui::DateMode _mode;
    int _offsetMinutes;
    std::function<void(std::optional<morph::time::Timestamp>)> _onChange;
    QString _shown;
};

/// The text a date-time field shows for an instant, in a zone @p offsetMinutes from UTC.
[[nodiscard]] QString formatDateTime(morph::time::DateTime const& instant, ui::DateMode mode, int offsetMinutes);

/// The instant a date-time field's text names in a zone @p offsetMinutes from UTC; none for text that names none.
[[nodiscard]] std::optional<morph::time::DateTime> parseDateTime(QString const& text, ui::DateMode mode,
                                                                 int offsetMinutes);

class SliderImpl final : public FieldItemWidget<ui::SliderWidget> {
public:
    SliderImpl(Context& context, ui::ContainerWidget* parent);
    void setRange(std::int64_t minimum, std::int64_t maximum, std::int64_t step) override;
    void setValue(std::int64_t value) override;
    void setOnChange(std::function<void(std::int64_t)> onChange) override;
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

class ButtonImpl final : public ItemWidget<ui::ButtonWidget> {
public:
    ButtonImpl(Context& context, ui::ContainerWidget* parent);
    void setLabel(std::string_view label) override;
    void setOnClick(ui::Action onClick) override;
    void setEmphasis(ui::ButtonEmphasis emphasis) override;
    [[nodiscard]] std::string shownText() const override;
};

class MenuImpl final : public ItemWidget<ui::MenuWidget> {
public:
    MenuImpl(Context& context, ui::ContainerWidget* parent);
    void setItems(std::vector<ui::MenuEntry> const& entries) override;
    void setOnActivate(std::function<void(std::vector<std::size_t>)> onActivate) override;
    /// Chooses the entry at @p path as the user would, opening each submenu on the way.
    void choose(std::vector<std::size_t> const& path) const;

private:
    std::vector<ui::MenuEntry> _entries;
};

/// Whether @p path names an entry the user can choose: enabled, without a submenu, inside enabled entries.
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

class BannerImpl final : public ItemWidget<ui::BannerWidget> {
public:
    BannerImpl(Context& context, ui::ContainerWidget* parent);
    void setTone(ui::Tone tone) override;
    void setText(std::string_view text) override;
    void setActionLabel(std::string_view label) override;
    void setOnAction(ui::Action onAction) override;
    void setDismissible(bool dismissible) override;
    void setOnDismiss(ui::Action onDismiss) override;
    [[nodiscard]] std::string shownText() const override;
};

class BadgeImpl final : public ItemWidget<ui::BadgeWidget> {
public:
    BadgeImpl(Context& context, ui::ContainerWidget* parent);
    void setTone(ui::Tone tone) override;
    void setText(std::string_view text) override;
    void setIcon(std::string_view icon) override;
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
};

class KeyValueImpl final : public ItemWidget<ui::KeyValueWidget> {
public:
    KeyValueImpl(Context& context, ui::ContainerWidget* parent);
    void setItems(std::vector<ui::KeyValueItem> const& items) override;
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

protected:
    /// A column fills its children's width, a row their height.
    [[nodiscard]] CrossFill crossFill() const noexcept override {
        return CrossFill{.width = _axis == ui::Axis::Vertical, .height = _axis == ui::Axis::Horizontal};
    }

private:
    ui::Axis _axis;
};

class GridImpl final : public ContainerItemWidget<ui::GridWidget> {
public:
    GridImpl(Context& context, ui::ContainerWidget* parent);
    void setColumns(int columns) override;
    void setGap(int gap) override;
    void setSpan(ui::Widget& child, int span) override;
};

class PanelImpl final : public ContainerItemWidget<ui::PanelWidget> {
public:
    PanelImpl(Context& context, ui::ContainerWidget* parent);
    void setTitle(std::string_view title) override;
    void setPadding(int padding) override;
    void setCollapsible(bool collapsible) override;
    void setCollapsed(bool collapsed) override;
    void setOnToggle(std::function<void(bool)> onToggle) override;
};

class ScrollImpl final : public ContainerItemWidget<ui::ScrollWidget> {
public:
    ScrollImpl(Context& context, ui::ContainerWidget* parent, ui::Axis axis);
};

class SlotImpl final : public ContainerItemWidget<ui::SlotWidget> {
public:
    SlotImpl(Context& context, ui::ContainerWidget* parent);
};

class TabsImpl final : public ContainerItemWidget<ui::TabsWidget> {
public:
    TabsImpl(Context& context, ui::ContainerWidget* parent);
    void setTabs(std::vector<std::string> const& labels) override;
    void setSelected(std::size_t index) override;
    void setOnSelect(std::function<void(std::size_t)> onSelect) override;
    /// The highlighted tab, or -1.
    [[nodiscard]] int highlighted() const;
};

/// A dialog or a drawer: a modal overlay whose content is shown only while it is open.
template <class Interface>
class OverlayImpl final : public ContainerItemWidget<Interface> {
public:
    OverlayImpl(Context& context, QString const& type, ui::ContainerWidget* parent, QVariantMap initial = {})
        : ContainerItemWidget<Interface>{context, type, parent, std::move(initial)} {}
    void setOpen(bool open) override {
        if (this->read("shown").toBool() != open) {
            this->call("show", open);
        }
    }
    void setTitle(std::string_view title) override { this->write("title", toQString(title)); }
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
};

using DialogImpl = OverlayImpl<ui::DialogWidget>;
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
    void setSelectionMode(ui::SelectionMode mode) override;
    void setRowKey(ui::Widget& row, ui::Key const& key) override;
    void setSelection(std::vector<ui::Key> const& keys) override;
    void setOnSelectionChange(std::function<void(std::vector<ui::Key>)> onSelectionChange) override;
    void setOnActivate(std::function<void(ui::Key)> onActivate) override;

    /// A click on a row; @p extend for Ctrl+click.
    void tapped(TableRowImpl const& row, bool extend);
    /// A double click on a row, or Enter on it.
    void activated(TableRowImpl const& row) const;
    /// Whether a row is marked selected.
    [[nodiscard]] static bool marked(ItemBase const& row);

protected:
    void adopted(ItemBase& child) override;

private:
    void mark() const;
    [[nodiscard]] std::vector<ui::Key> existingSelection() const;

    ui::SelectionMode _mode = ui::SelectionMode::None;
    std::vector<ui::Key> _requested;
    std::function<void(std::vector<ui::Key>)> _onSelectionChange;
    std::function<void(ui::Key)> _onActivate;
};

/// A row of a table: the horizontal stack the mount makes for it, placing its cells by column.
class TableRowImpl final : public ContainerItemWidget<ui::StackWidget> {
public:
    TableRowImpl(Context& context, ui::ContainerWidget* parent, TableImpl& table);
    void setGap(int gap) override;
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

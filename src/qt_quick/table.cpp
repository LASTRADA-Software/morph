// SPDX-License-Identifier: Apache-2.0

#include <QMetaObject>
#include <QObject>
#include <QPointer>
#include <QQuickItem>
#include <QQuickWindow>
#include <QStringList>
#include <QTimer>
#include <QVariantList>
#include <QVariantMap>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <morph/ui/cell_format.hpp>
#include <ranges>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

#include "qt_quick/kinds.hpp"

namespace morph::qt_quick::detail {

// NOLINTNEXTLINE(readability-function-cognitive-complexity): the constructor wires every signal of the QML table to its handler in one place; the connections share the locals they capture
TableImpl::TableImpl(Context& context, ui::ContainerWidget* parent)
    : ContainerItemWidget{
          context, QStringLiteral("MorphTable"), parent,
          QVariantMap{
              {QStringLiteral("headerBackground"),
               QVariant::fromValue(&context.component(QStringLiteral("MorphTableHeaderBackground")))},
              {QStringLiteral("bar"), QVariant::fromValue(&context.component(QStringLiteral("MorphScrollBar")))}}} {
    relay().on(QStringLiteral("scroll"), [this](QVariant const& payload) {
        QVariantMap const map = payload.toMap();
        scrolled(static_cast<std::size_t>(std::max<qlonglong>(0, map.value(QStringLiteral("first")).toLongLong())),
                 static_cast<std::size_t>(std::max<qlonglong>(0, map.value(QStringLiteral("count")).toLongLong())));
    });
    relay().on(QStringLiteral("selectAll"), [this](QVariant const&) { selectAll(); });
    // A copy of each handler below: it may destroy this table.
    relay().on(QStringLiteral("hscroll"), [this](QVariant const& payload) {
        QVariantMap const map = payload.toMap();
        if (auto const handler = _onHorizontalRange) {
            handler(map.value(QStringLiteral("scrollX")).toInt(), map.value(QStringLiteral("viewport")).toInt());
        }
    });
    relay().on(QStringLiteral("measured"), [this](QVariant const& payload) {
        QVariantMap const map = payload.toMap();
        qlonglong const column = map.value(QStringLiteral("column")).toLongLong();
        if (auto const handler = _onColumnMeasured; handler && column >= 0) {
            handler(static_cast<std::size_t>(column),
                    ui::ColumnMeasure{.header = map.value(QStringLiteral("header")).toInt(),
                                      .natural = map.value(QStringLiteral("natural")).toInt(),
                                      .least = map.value(QStringLiteral("least")).toInt()});
        }
    });
    relay().on(QStringLiteral("focusMove"), [this](QVariant const& payload) {
        int const move = payload.toInt();
        if (auto const handler = _onFocusMove;
            handler && move >= 0 && move <= static_cast<int>(ui::FocusMove::PageDown)) {
            handler(static_cast<ui::FocusMove>(move));
        }
    });
    relay().on(QStringLiteral("selectRange"), [this](QVariant const& payload) {
        QVariantMap const map = payload.toMap();
        qlonglong const anchor = map.value(QStringLiteral("anchor")).toLongLong();
        qlonglong const focus = map.value(QStringLiteral("focus")).toLongLong();
        if (auto const handler = _onSelectRange; handler && anchor >= 0 && focus >= 0) {
            handler(static_cast<std::size_t>(anchor), static_cast<std::size_t>(focus),
                    map.value(QStringLiteral("extend")).toBool() ? ui::SelectionChange::Extend
                                                                 : ui::SelectionChange::Replace);
        }
    });
    relay().on(QStringLiteral("statusEntry"), [this](QVariant const& columnId) {
        if (auto const handler = _onStatusEntry) {
            handler(columnId.toString().toStdString());
        }
    });
    relay().on(QStringLiteral("retry"), [this](QVariant const&) {
        if (auto const handler = _onRetry) {
            handler();
        }
    });
    relay().on(QStringLiteral("text"), [this](QVariant const& text) {
        if (auto const handler = _onText) {
            handler(text.toString().toStdString());
        }
    });
    relay().on(QStringLiteral("focusOut"), [this](QVariant const&) {
        if (auto const handler = _onFocusOut) {
            handler();
        }
    });
    relay().on(QStringLiteral("sort"), [this](QVariant const& payload) {
        QVariantMap const map = payload.toMap();
        qlonglong const column = map.value(QStringLiteral("column")).toLongLong();
        if (auto const handler = _onSort; handler && column >= 0) {
            handler(static_cast<std::size_t>(column),
                    map.value(QStringLiteral("shift")).toBool() ? ui::HeaderClick::Shift : ui::HeaderClick::Plain);
        }
    });
    // A rectangle of cells the user selected, by its corners' view rows and visible positions.
    relay().on(QStringLiteral("cellSelect"), [this](QVariant const& payload) {
        QVariantMap const map = payload.toMap();
        qlonglong const anchorRow = map.value(QStringLiteral("anchorRow")).toLongLong();
        qlonglong const anchorPosition = map.value(QStringLiteral("anchorPosition")).toLongLong();
        qlonglong const focusRow = map.value(QStringLiteral("focusRow")).toLongLong();
        qlonglong const focusPosition = map.value(QStringLiteral("focusPosition")).toLongLong();
        if (auto const handler = _onCellSelect;
            handler && anchorRow >= 0 && anchorPosition >= 0 && focusRow >= 0 && focusPosition >= 0) {
            handler(static_cast<std::size_t>(anchorRow), static_cast<std::size_t>(anchorPosition),
                    static_cast<std::size_t>(focusRow), static_cast<std::size_t>(focusPosition));
        }
    });
    // A menu asked for by its column, -1 for the table's: its entries are read now and shown under its place.
    relay().on(QStringLiteral("menu"), [this](QVariant const& payload) {
        qlonglong const column = payload.toLongLong();
        if (!_menu || item() == nullptr) {
            return;
        }
        auto const entries = _menu;
        std::optional<std::size_t> const which =
            column >= 0 ? std::optional<std::size_t>{static_cast<std::size_t>(column)} : std::nullopt;
        QVariantList const shown = menuEntries(entries(which));
        if (item() != nullptr && !shown.isEmpty()) {
            QMetaObject::invokeMethod(item(), "showMenu", Qt::DirectConnection, Q_ARG(QVariant, column),
                                      Q_ARG(QVariant, shown));
        }
    });
    relay().on(QStringLiteral("menuChoose"), [this](QVariant const& payload) {
        QVariantMap const map = payload.toMap();
        qlonglong const column = map.value(QStringLiteral("column")).toLongLong();
        std::vector<std::size_t> path;
        for (QVariant const& index : map.value(QStringLiteral("path")).toList()) {
            if (index.toLongLong() < 0) {
                return;
            }
            path.push_back(static_cast<std::size_t>(index.toLongLong()));
        }
        if (column == kCellMenuColumn) {
            // A copy: the handler may destroy this table.
            if (auto const handler = _cellChosen; handler && _cellMenuAt) {
                auto const [key, index] = *_cellMenuAt;
                handler(key, index, path);
            }
            return;
        }
        if (auto const handler = _onMenu) {
            handler(column >= 0 ? std::optional<std::size_t>{static_cast<std::size_t>(column)} : std::nullopt,
                    std::move(path));
        }
    });
    // A key the table leaves, with a cell focused, that is one of the entries' chords: the focused cell's first
    // enabled entry without a submenu whose chord it is runs, as if chosen from its menu.
    relay().answer(QStringLiteral("cellKey"), [this](QVariant const& payload) {
        QVariantList const pressed = payload.toList();
        if (pressed.size() != 2 || !_cellMenu || !_cellChosen || item() == nullptr || !reachable()) {
            return false;
        }
        int const key = pressed.at(0).toInt();
        auto const modifiers = Qt::KeyboardModifiers::fromInt(pressed.at(1).toUInt());
        // A key that is none of the entries' chords asks for nothing.
        if (std::ranges::none_of(_cellMenuKeys,
                                 [&](std::string const& chord) { return chordMatches(chord, key, modifiers); })) {
            return false;
        }
        qlonglong const viewRow = item()->property("focusedRowIndex").toLongLong();
        int const column = item()->property("focusedColumnIndex").toInt();
        std::optional<ui::Key> rowKey;
        for (ItemBase const* const child : children()) {
            auto const* const row = dynamic_cast<TableRowImpl const*>(child);
            if (row != nullptr && row->item() != nullptr && row->key() &&
                row->item()->property("viewRow").toLongLong() == viewRow) {
                rowKey = row->key();
            }
        }
        if (!rowKey || column < 0) {
            return false;
        }
        auto const entries = _cellMenu;
        auto const chosen = _cellChosen;
        std::vector<std::size_t> path;
        std::function<bool(std::vector<ui::MenuEntry> const&)> const find =
            [&](std::vector<ui::MenuEntry> const& level) {
                for (std::size_t at = 0; at < level.size(); ++at) {
                    ui::MenuEntry const& entry = level.at(at);
                    if (entry.separator || entry.heading || !entry.enabled) {
                        continue;
                    }
                    path.push_back(at);
                    if (entry.items.empty() ? !entry.keys.empty() && chordMatches(entry.keys, key, modifiers)
                                            : find(entry.items)) {
                        return true;
                    }
                    path.pop_back();
                }
                return false;
            };
        if (!find(entries(*rowKey, static_cast<std::size_t>(column)))) {
            return false;
        }
        chosen(*rowKey, static_cast<std::size_t>(column), path);
        return true;
    });
    relay().on(QStringLiteral("columnMove"), [this](QVariant const& payload) {
        QVariantMap const map = payload.toMap();
        qlonglong const column = map.value(QStringLiteral("column")).toLongLong();
        qlonglong const position = map.value(QStringLiteral("position")).toLongLong();
        if (auto const handler = _onColumnMove; handler && column >= 0 && position >= 0) {
            handler(static_cast<std::size_t>(column), static_cast<std::size_t>(position));
        }
    });
    // A header's width in pixels, or none for a double click on the edge. The header holds the column's content and
    // its padding on both sides, which a width in units does not count (the mount adds it back), so the padding is
    // taken off before the pixels become units.
    relay().on(QStringLiteral("columnResize"), [this](QVariant const& payload) {
        QVariantMap const map = payload.toMap();
        qlonglong const column = map.value(QStringLiteral("column")).toLongLong();
        auto const handler = _onColumnResize;
        if (!handler || column < 0) {
            return;
        }
        std::optional<int> units;
        if (QVariant const width = map.value(QStringLiteral("width")); width.isValid() && !width.isNull()) {
            double const unit = read("measureUnit").toDouble();
            double const content = width.toDouble() - read("measurePadding").toDouble();
            units = unit > 0 ? std::max(1, static_cast<int>(std::lround(content / unit))) : 1;
        }
        handler(static_cast<std::size_t>(column), units);
    });
    relay().on(QStringLiteral("filter"), [this](QVariant const& payload) {
        if (auto const handler = _onFilter; handler && payload.toLongLong() >= 0) {
            handler(static_cast<std::size_t>(payload.toLongLong()));
        }
    });
    relay().on(QStringLiteral("groupFilter"), [this](QVariant const& payload) {
        if (auto const handler = _onGroupFilter; handler && payload.toLongLong() >= 0) {
            handler(static_cast<std::size_t>(payload.toLongLong()));
        }
    });
    relay().on(QStringLiteral("copy"), [this](QVariant const&) {
        if (auto const handler = _onCopy) {
            handler();
        }
    });
}

void TableImpl::setColumns(std::vector<ui::TableColumn> const& columns) {
    QVariantList list;
    for (ui::TableColumn const& column : columns) {
        list.append(QVariantMap{
            {QStringLiteral("label"), toQString(column.label)},
            {QStringLiteral("unit"), toQString(column.unit)},
            {QStringLiteral("accessibleName"), toQString(ui::headerAccessibleName(column.label, column.unit))},
            {QStringLiteral("kind"), static_cast<int>(column.width.kind)},
            {QStringLiteral("amount"), column.width.amount},
            {QStringLiteral("role"), static_cast<int>(column.role)},
            {QStringLiteral("wrap"), column.wrap},
            {QStringLiteral("shrink"), column.shrink},
            {QStringLiteral("align"), static_cast<int>(column.align)},
            {QStringLiteral("valign"), static_cast<int>(column.valign)},
            {QStringLiteral("sortable"), column.sortable},
            {QStringLiteral("filterable"), column.filterable}});
    }
    write("columns", list);
}

void TableImpl::setShowHeader(bool shown) { write("showHeader", shown); }

void TableImpl::setSelectionMode(ui::SelectionMode mode) {
    _mode = mode;
    write("multiple", mode == ui::SelectionMode::Multiple);
}

void TableImpl::setRowKey(ui::Widget& row, ui::Key const& key) {
    if (auto* const tableRow = dynamic_cast<TableRowImpl*>(&row)) {
        tableRow->setKey(key);
        mark();
    }
}

void TableImpl::setRowTone(ui::Widget& row, std::optional<ui::Tone> tone) {
    auto const* const tableRow = dynamic_cast<TableRowImpl const*>(&row);
    if (tableRow == nullptr || tableRow->container() != this) {
        throw std::logic_error{"morph::qt_quick: setRowTone of a widget that is not this table's row"};
    }
    tableRow->write("tone", tone ? static_cast<int>(*tone) : -1);
}

void TableImpl::setRowMarks(ui::Widget& row, ui::RowMarks const& marks) {
    auto const* const tableRow = dynamic_cast<TableRowImpl const*>(&row);
    if (tableRow == nullptr || tableRow->container() != this) {
        throw std::logic_error{"morph::qt_quick: setRowMarks of a widget that is not this table's row"};
    }
    tableRow->write("busy", marks.busy);
    tableRow->write("locked", marks.locked);
    tableRow->write("unmatched", marks.unmatched);
    tableRow->write("marksTooltip", toQString(marks.tooltip));
}

void TableImpl::setRowHeading(ui::Widget& row) {
    auto const* const tableRow = dynamic_cast<TableRowImpl const*>(&row);
    if (tableRow == nullptr || tableRow->container() != this) {
        throw std::logic_error{"morph::qt_quick: setRowHeading of a widget that is not this table's row"};
    }
    tableRow->write("heading", true);
    mark();
}

void TableImpl::setRowSummary(ui::Widget& row) {
    auto const* const tableRow = dynamic_cast<TableRowImpl const*>(&row);
    if (tableRow == nullptr || tableRow->container() != this) {
        throw std::logic_error{"morph::qt_quick: setRowSummary of a widget that is not this table's row"};
    }
    tableRow->write("summary", true);
    if (tableRow->key() && !std::ranges::contains(_summaryKeys, *tableRow->key())) {
        _summaryKeys.push_back(*tableRow->key());
    }
    mark();
}

void TableImpl::setRowSpans(ui::Widget& row, std::vector<ui::RowSpan> const& spans) {
    auto const* const tableRow = dynamic_cast<TableRowImpl const*>(&row);
    if (tableRow == nullptr || tableRow->container() != this) {
        throw std::logic_error{"morph::qt_quick: setRowSpans of a widget that is not this table's row"};
    }
    if (spans.empty()) {
        _spans.erase(tableRow);
    } else {
        _spans.insert_or_assign(tableRow, spans);
    }
    planSpans(nullptr);
}

void TableImpl::setSelectColumn(bool shown) { write("selectColumn", shown); }

void TableImpl::setSelectAllLabel(std::string_view label) { write("selectAllLabel", toQString(label)); }

void TableImpl::setRowLabel(ui::Widget& row, std::string_view label) {
    auto const* const tableRow = dynamic_cast<TableRowImpl const*>(&row);
    if (tableRow == nullptr || tableRow->container() != this) {
        throw std::logic_error{"morph::qt_quick: setRowLabel of a widget that is not this table's row"};
    }
    tableRow->write("rowLabel", toQString(label));
}

void TableImpl::setSelection(std::vector<ui::Key> const& keys) {
    _viewExcept.reset();
    _requested = keys;
    mark();
}

void TableImpl::setSelectionView(std::vector<ui::Key> const& except) {
    _viewExcept = except;
    mark();
}

void TableImpl::setOnStatusEntry(std::function<void(std::string entryId)> onStatusEntry) {
    _onStatusEntry = std::move(onStatusEntry);
}

void TableImpl::setOnSelectionChange(
    std::function<void(std::vector<ui::Key>, ui::SelectionChange)> onSelectionChange) {
    _onSelectionChange = std::move(onSelectionChange);
}

void TableImpl::setOnActivate(std::function<void(ui::Key)> onActivate) { _onActivate = std::move(onActivate); }

// By column, as text keys: a header not in the chain has neither a mark nor a description.
void TableImpl::setSort(std::vector<ui::TableSort> const& chain) {
    QVariantMap marks;
    QVariantMap descriptions;
    for (std::size_t place = 0; place < chain.size(); ++place) {
        QString const column = QString::number(chain.at(place).column);
        marks.insert(column, toQString(ui::sortMarkText(chain.at(place).direction, place + 1, chain.size())));
        descriptions.insert(column,
                            toQString(ui::sortDescription(chain.at(place).direction, place + 1, chain.size())));
    }
    write("sortMarks", marks);
    write("sortDescriptions", descriptions);
}

void TableImpl::setOnSort(std::function<void(std::size_t, ui::HeaderClick)> onSort) { _onSort = std::move(onSort); }

void TableImpl::setMenu(std::function<std::vector<ui::MenuEntry>(std::optional<std::size_t>)> entries,
                        std::function<void(std::optional<std::size_t>, std::vector<std::size_t>)> onChoose) {
    _menu = std::move(entries);
    _onMenu = std::move(onChoose);
    write("hasMenu", static_cast<bool>(_menu));
}

void TableImpl::setCellMenu(std::function<std::vector<ui::MenuEntry>(ui::Key const&, std::size_t)> entries,
                            std::function<void(ui::Key const&, std::size_t, std::vector<std::size_t> const&)> chosen) {
    _cellMenu = std::move(entries);
    _cellChosen = std::move(chosen);
    write("hasCellMenu", static_cast<bool>(_cellMenu));
}

void TableImpl::openCellMenu(TableRowImpl const& row, std::size_t column) {
    if (!_cellMenu || !row.key() || !row.reachable() || passive(row) || item() == nullptr) {
        return;
    }
    ui::Key const key = *row.key();
    // The menu shows at the cell; a row not drawn as a cell (none in the column) anchors it.
    QPointer<QQuickItem> anchor{row.item()};
    for (ItemBase const* const child : row.children()) {
        QQuickItem* const cell = child->item();
        if (cell != nullptr && cell->isVisible() &&
            cell->property("measuredIndex").toLongLong() == static_cast<qlonglong>(column)) {
            anchor = cell;
        }
    }
    // A row already selected keeps the selection, so the menu acts on every selected row.
    if (_mode != ui::SelectionMode::None && !marked(row)) {
        _anchor = key;
        select({key}, ui::SelectionChange::Replace);
    }
    auto const entries = _cellMenu;
    QVariantList const shown = menuEntries(entries(key, column));
    if (item() != nullptr && anchor != nullptr && !shown.isEmpty()) {
        _cellMenuAt = std::pair{key, column};
        QMetaObject::invokeMethod(item(), "showCellMenu", Qt::DirectConnection,
                                  Q_ARG(QVariant, QVariant::fromValue<QObject*>(anchor.data())),
                                  Q_ARG(QVariant, shown));
    }
}

void TableImpl::setVisibleColumns(std::vector<std::size_t> const& visible) {
    QVariantList list;
    for (std::size_t const column : visible) {
        list.append(static_cast<int>(column));
    }
    write("visibleColumns", list);
}

void TableImpl::setOnColumnMove(std::function<void(std::size_t, std::size_t)> handler) {
    _onColumnMove = std::move(handler);
    write("movable", static_cast<bool>(_onColumnMove));
}

void TableImpl::setOnColumnResize(std::function<void(std::size_t, std::optional<int>)> handler) {
    _onColumnResize = std::move(handler);
    write("resizable", static_cast<bool>(_onColumnResize));
}

void TableImpl::setOnFilter(std::function<void(std::size_t)> handler) { _onFilter = std::move(handler); }

void TableImpl::setFiltered(std::vector<std::size_t> const& columns) {
    QVariantMap filtered;
    for (std::size_t const column : columns) {
        filtered.insert(QString::number(column), true);
    }
    write("filteredColumns", filtered);
}

void TableImpl::setColumnGroups(std::vector<ui::TableColumnGroup> const& groups) {
    QVariantList list;
    for (ui::TableColumnGroup const& group : groups) {
        list.append(QVariantMap{{QStringLiteral("label"), toQString(group.label)},
                                {QStringLiteral("first"), static_cast<int>(group.first)},
                                {QStringLiteral("count"), static_cast<int>(group.count)},
                                {QStringLiteral("filtered"), group.filtered}});
    }
    write("columnGroups", list);
}

void TableImpl::setBands(std::vector<ui::BandRun> const& runs) {
    QVariantList list;
    for (ui::BandRun const& run : runs) {
        list.append(QVariantMap{{QStringLiteral("level"), static_cast<int>(run.level)},
                                {QStringLiteral("text"), toQString(ui::headerText(run.label, run.unit))},
                                {QStringLiteral("first"), static_cast<int>(run.first)},
                                {QStringLiteral("count"), static_cast<int>(run.count)},
                                {QStringLiteral("pinned"), run.pinned}});
    }
    write("bands", list);
}

void TableImpl::setOnGroupFilter(std::function<void(std::size_t)> handler) { _onGroupFilter = std::move(handler); }

void TableImpl::setOnCopy(std::function<void()> onCopy) {
    _onCopy = std::move(onCopy);
    write("copies", static_cast<bool>(_onCopy));
}

void TableImpl::setExistingKeys(std::vector<ui::Key> const& keys) {
    _existingKeys = std::unordered_set<ui::Key>{keys.begin(), keys.end()};
    _existingOrder = keys;
}

void TableImpl::setRealizedWindow(std::size_t first, std::size_t count, std::size_t total) {
    write("realizedFirst", static_cast<qlonglong>(first));
    write("realizedCount", static_cast<qlonglong>(count));
    write("totalRows", static_cast<qlonglong>(total));
}

void TableImpl::setOnVisibleRangeChanged(std::function<void(std::size_t, std::size_t)> onVisibleRangeChanged) {
    _onVisibleRangeChanged = std::move(onVisibleRangeChanged);
}

void TableImpl::setRowIndex(ui::Widget& row, std::size_t viewRow) {
    auto* const tableRow = dynamic_cast<TableRowImpl*>(&row);
    if (tableRow == nullptr || tableRow->container() != this) {
        throw std::logic_error{"morph::qt_quick: setRowIndex of a widget that is not this table's row"};
    }
    tableRow->setViewRow(viewRow);
    tableRow->write("viewRow", static_cast<qlonglong>(viewRow));
    if (item() != nullptr) {
        QMetaObject::invokeMethod(item(), "scheduleRows", Qt::DirectConnection);
    }
}

// The counts, the state, what stands and the message on one line, two spaces apart; the entries are buttons after
// it, the standing ones first, each with its id and label.
void TableImpl::setStatus(ui::TableStatus const& status) {
    QStringList parts;
    for (std::string const* const part : {&status.counts, &status.state, &status.standing, &status.message}) {
        if (!part->empty()) {
            parts.append(toQString(*part));
        }
    }
    write("statusText", parts.join(QStringLiteral("  ")));
    QVariantList entries;
    for (auto const* const list : {&status.standingEntries, &status.entries}) {
        for (ui::StatusEntry const& entry : *list) {
            entries.append(QVariantMap{{QStringLiteral("id"), toQString(entry.id)},
                                       {QStringLiteral("label"), toQString(entry.label)}});
        }
    }
    write("statusEntries", status.shown ? entries : QVariantList{});
    write("retryShown", status.shown && status.retry);
    write("statusShown", status.shown);
}

void TableImpl::setOnRetry(std::function<void()> onRetry) { _onRetry = std::move(onRetry); }

void TableImpl::setCellLoad(ui::Widget& row, std::size_t column, ui::CellLoad load) {
    auto* const tableRow = dynamic_cast<TableRowImpl*>(&row);
    if (tableRow == nullptr || tableRow->container() != this) {
        throw std::logic_error{"morph::qt_quick: setCellLoad of a widget that is not this table's row"};
    }
    tableRow->setLoad(column, load);
}

void TableImpl::setCellAccent(ui::Widget& row, std::size_t column, ui::CellAccent const& accent) {
    auto* const tableRow = dynamic_cast<TableRowImpl*>(&row);
    if (tableRow == nullptr || tableRow->container() != this) {
        throw std::logic_error{"morph::qt_quick: setCellAccent of a widget that is not this table's row"};
    }
    tableRow->setAccent(column, accent);
}

void TableImpl::setColumnWindow(
    std::span<std::size_t const> columns, std::span<int const> offsets,
    // NOLINTNEXTLINE(bugprone-easily-swappable-parameters): overrides `Table::setColumnWindow`; the declaration says how its totals are told apart
    std::span<int const> widths, int totalWidth, std::size_t pinned) {
    QVariantList window;
    for (auto const [column, offset, width] : std::views::zip(columns, offsets, widths)) {
        window.append(QVariantMap{{QStringLiteral("column"), static_cast<int>(column)},
                                  {QStringLiteral("offset"), offset},
                                  {QStringLiteral("width"), width}});
    }
    write("windowTotalWidth", totalWidth);
    write("pinnedShown", static_cast<int>(std::min(pinned, columns.size())));
    write("windowColumns", window);
    write("arranged", true);
}

void TableImpl::setOnHorizontalRange(std::function<void(int scrollX, int viewport)> handler) {
    _onHorizontalRange = std::move(handler);
    write("sourceRows", true);
}

void TableImpl::setHorizontalOffset(int scrollX) {
    if (item() != nullptr) {
        QMetaObject::invokeMethod(item(), "setHorizontalOffset", Qt::DirectConnection, Q_ARG(int, scrollX));
    }
}

// The measure the table's widths are in: pixels, from the theme the table item reads.
ui::TableMeasure TableImpl::tableMeasure() const {
    return ui::TableMeasure{.unit = read("measureUnit").toDouble(),
                            .monoUnit = read("measureMonoUnit").toDouble(),
                            .contentEstimate = static_cast<int>(std::lround(12 * read("measureUnit").toDouble())),
                            .padding = static_cast<int>(std::lround(read("measurePadding").toDouble())),
                            .shrinkFloor = static_cast<int>(std::lround(8 * read("measureUnit").toDouble()))};
}

void TableImpl::setOnColumnMeasured(
    std::function<void(std::size_t column, ui::ColumnMeasure const& measure)> handler) {
    _onColumnMeasured = std::move(handler);
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters): overrides `Table::setFocusCell`; the declaration says how its pair is told apart
void TableImpl::setFocusCell(std::size_t viewRow, std::size_t column) {
    write("focusedRowIndex", static_cast<qlonglong>(viewRow));
    write("focusedColumnIndex", static_cast<int>(column));
    if (item() != nullptr) {
        QMetaObject::invokeMethod(item(), "showFocusCell", Qt::DirectConnection);
    }
}

void TableImpl::setOnFocusMove(std::function<void(ui::FocusMove)> handler) { _onFocusMove = std::move(handler); }

void TableImpl::setOnText(std::function<void(std::string)> onText) {
    _onText = std::move(onText);
    write("takesText", static_cast<bool>(_onText));
}

void TableImpl::setOnCellClick(std::function<void(std::size_t, std::size_t, bool)> onClick) {
    _onCellClick = std::move(onClick);
    write("reportsCellClicks", static_cast<bool>(_onCellClick));
}

void TableImpl::setOnFocusOut(std::function<void()> onFocusOut) {
    _onFocusOut = std::move(onFocusOut);
    write("reportsFocusOut", static_cast<bool>(_onFocusOut));
}

// The editor is placed as a cell of its column is, over the cell, which is covered while it is open; the row's
// `editorColumn` says which column and `editorItem` the editor, whose own popups the table's focus watch counts as
// inside the table. An editor that holds the focus when it closes gives it back to the table, so the next key reaches
// the table.
// NOLINTNEXTLINE(readability-function-cognitive-complexity): places the editor over its cell and hands focus back on close; the open and close paths share the row's editor state
void TableImpl::setCellEditor(ui::Widget& row, std::size_t column, ui::Widget* editor) {
    auto const* const tableRow = dynamic_cast<TableRowImpl*>(&row);
    if (tableRow == nullptr || tableRow->container() != this) {
        throw std::logic_error{"morph::qt_quick: setCellEditor of a widget that is not this table's row"};
    }
    ItemBase* const shown = editor != nullptr ? baseOf(*editor) : nullptr;
    if (shown == nullptr) {
        QPointer<QQuickItem> const closing = _editors.take(tableRow);
        QQuickWindow const* const window = item() != nullptr ? item()->window() : nullptr;
        bool held = false;
        for (QQuickItem const* at = window != nullptr ? window->activeFocusItem() : nullptr;
             at != nullptr && closing != nullptr; at = at->parentItem()) {
            held = held || at == closing.data();
        }
        if (held && item() != nullptr) {
            item()->forceActiveFocus(Qt::OtherFocusReason);
        }
    } else if (shown->item() != nullptr) {
        _editors.insert(tableRow, QPointer<QQuickItem>{shown->item()});
    }
    for (ItemBase* const cell : tableRow->children()) {
        if (cell == shown || cell->item() == nullptr) {
            continue;
        }
        if (cell->item()->property("measuredIndex").toInt() == static_cast<int>(column)) {
            cell->cover(shown != nullptr);
        }
    }
    if (shown != nullptr) {
        shown->offer("measuredIndex", static_cast<int>(column));
        shown->cover(false);
    }
    tableRow->write("editorColumn", shown != nullptr ? static_cast<int>(column) : -1);
    tableRow->write("editorItem", QVariant::fromValue(shown != nullptr ? shown->item() : nullptr));
    if (QQuickItem* const rowItem = tableRow->item(); rowItem != nullptr) {
        QMetaObject::invokeMethod(rowItem, "schedulePlace", Qt::DirectConnection);
    }
}

void TableImpl::setCellMarks(ui::Widget& row, std::size_t column, ui::CellMarks const& marks) {
    auto* const tableRow = dynamic_cast<TableRowImpl*>(&row);
    if (tableRow == nullptr || tableRow->container() != this) {
        throw std::logic_error{"morph::qt_quick: setCellMarks of a widget that is not this table's row"};
    }
    tableRow->setMarks(column, marks);
}

void TableImpl::setRowModified(ui::Widget& row, bool modified) {
    auto const* const tableRow = dynamic_cast<TableRowImpl*>(&row);
    if (tableRow == nullptr || tableRow->container() != this) {
        throw std::logic_error{"morph::qt_quick: setRowModified of a widget that is not this table's row"};
    }
    tableRow->write("modified", modified);
}

void TableImpl::setCellSelection(std::optional<std::array<std::size_t, 4>> rect) {
    QVariantList shown;
    if (rect) {
        for (std::size_t const part : *rect) {
            shown.append(static_cast<qlonglong>(part));
        }
    }
    write("cellRect", shown);
}

void TableImpl::setOnCellSelect(std::function<void(std::size_t, std::size_t, std::size_t, std::size_t)> onSelect) {
    _onCellSelect = std::move(onSelect);
    write("selectsCells", static_cast<bool>(_onCellSelect));
}

void TableImpl::cellClicked(TableRowImpl const& row, std::size_t column, bool doubleClick) const {
    if (!row.viewRow() || !row.reachable() || !_onCellClick) {
        return;
    }
    auto const handler = _onCellClick;
    handler(*row.viewRow(), column, doubleClick);
}

void TableImpl::setOnSelectRange(
    std::function<void(std::size_t anchorRow, std::size_t focusRow, ui::SelectionChange)> handler) {
    _onSelectRange = std::move(handler);
}

// -1 for a row count that is a lower bound or not known yet.
void TableImpl::setCounts(std::optional<std::int64_t> rows, std::size_t columns) {
    write("accessibleRowCount", static_cast<qlonglong>(rows.value_or(-1)));
    write("accessibleColumnCount", static_cast<int>(columns));
}

void TableImpl::scrollToRow(std::size_t viewRow) {
    if (item() != nullptr) {
        QMetaObject::invokeMethod(item(), "scrollToRow", Qt::DirectConnection,
                                  Q_ARG(QVariant, QVariant{static_cast<qlonglong>(viewRow)}));
    }
}

// A cell with no column is pooled: hidden, as a covered cell is, and placed nowhere. `measuredIndex` is the column
// the row places it in and the one its widths are reported for.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters): overrides `Table::setCellColumn`; the declaration says how its pair is told apart
void TableImpl::setCellColumn(ui::Widget& row, ui::Widget& cell, std::optional<std::size_t> column) {
    auto const* const tableRow = dynamic_cast<TableRowImpl const*>(&row);
    if (tableRow == nullptr || tableRow->container() != this) {
        throw std::logic_error{"morph::qt_quick: setCellColumn of a widget that is not this table's row"};
    }
    ItemBase* const base = baseOf(cell);
    if (base == nullptr) {
        return;
    }
    base->offer("measuredIndex", column ? static_cast<int>(*column) : -1);
    base->cover(!column.has_value());
    // A cell reports a change of its width itself, but its width may have changed before it had this column, and a
    // pooled cell's content is rebound after its column is set. Its width in this column is told to the table once the
    // pass is over, when the content is the row's own.
    if (column && base->item() != nullptr && item() != nullptr) {
        QTimer::singleShot(0, item(),
                           [tableRef = QPointer<QQuickItem>{item()}, cellRef = QPointer<QQuickItem>{base->item()}] {
                               if (tableRef != nullptr && cellRef != nullptr && cellRef->isVisible()) {
                                   QMetaObject::invokeMethod(tableRef, "reportCell", Qt::DirectConnection,
                                                             Q_ARG(int, cellRef->property("measuredIndex").toInt()),
                                                             Q_ARG(QVariant, QVariant::fromValue(cellRef.data())));
                               }
                           });
    }
    // Once per row and event loop turn, however many of its cells a pass places.
    if (QQuickItem* const rowItem = tableRow->item(); rowItem != nullptr) {
        QMetaObject::invokeMethod(rowItem, "schedulePlace", Qt::DirectConnection);
    }
}

void TableImpl::scrolled(std::size_t first, std::size_t count) const {
    if (_onVisibleRangeChanged) {
        _onVisibleRangeChanged(first, count);
    }
}

void TableImpl::adopted(ItemBase& child) {
    static_cast<void>(child);
    mark();
    stripe(nullptr);
    planSpans(nullptr);
    // A newly-adopted row may be the table's first with columns already set (columns usually arrive before any
    // row), or `setColumns` may not yet have run for every column at mount time -- either way, make sure the cache
    // covers every column before anything reads `widths`. This is an explicit, outside-any-binding call: seeding
    // lazily from inside the `widths` binding's own evaluation (cachedWidth) previously caused `widths` to be
    // marked dirty and re-entered while still computing itself, as each row's adoption retriggered a seed.
    if (item() != nullptr) {
        QMetaObject::invokeMethod(item(), "seedMissingColumns", Qt::DirectConnection);
    }
}

void TableImpl::moved() {
    stripe(nullptr);
    planSpans(nullptr);
}

void TableImpl::releasing(ItemBase& child) noexcept {
    stripe(&child);
    _spans.erase(&child);
    _editors.remove(dynamic_cast<TableRowImpl const*>(&child));
    planSpans(&child);
    // `child`'s item is still parented into the QML body (ItemBase's own destructor calls release() -- which calls
    // this -- before it hides and reparents the item away), so a QML-side rescan of body.children would still see
    // it unless its own item is hidden first. Doing that here, directly, is simpler and safer than teaching the
    // QML-side scan a second way to exclude a row, and a row headed for destruction needing no visible state is a
    // narrower assumption than inventing a new cross-language exclusion parameter.
    if (QQuickItem* const leavingItem = child.item(); leavingItem != nullptr) {
        leavingItem->setVisible(false);
    }
    if (item() != nullptr) {
        QMetaObject::invokeMethod(item(), "invalidateAllColumns", Qt::DirectConnection);
    }
}

// Coverage follows the rows' order, so it is worked out here from the rows as they stand, whenever a row comes, goes,
// moves or is told its spans, and never from a span alone.
// NOLINTNEXTLINE(readability-function-cognitive-complexity): works out coverage in one pass in the rows' order: the spans carried from earlier rows decide what each next row covers
void TableImpl::planSpans(ItemBase const* leaving) {
    if (_spans.empty() && !_planned) {
        return;
    }
    std::vector<TableRowImpl*> rows;
    for (ItemBase* const child : children()) {
        if (auto* const row = dynamic_cast<TableRowImpl*>(child); row != nullptr && child != leaving) {
            rows.push_back(row);
        }
    }
    std::vector<QVariantList> plans(rows.size());
    std::vector<QVariantList> coveredColumns(rows.size());
    std::vector<std::vector<bool>> covered(rows.size());
    std::vector<std::size_t> coveredUntil;
    for (std::size_t index = 0; index < rows.size(); ++index) {
        covered.at(index).assign(rows.at(index)->children().size(), false);
    }
    for (std::size_t index = 0; index < rows.size(); ++index) {
        auto const found = _spans.find(rows.at(index));
        if (found == _spans.end() || heading(*rows.at(index))) {
            continue;
        }
        std::size_t const cells = rows.at(index)->children().size();
        coveredUntil.resize(std::max(coveredUntil.size(), cells), 0);
        for (ui::RowSpan const& span : found->second) {
            if (span.column >= cells || span.rows == 0 || coveredUntil.at(span.column) > index) {
                continue;
            }
            // The columns it covers, its own and the next ones, clipped at the row's last cell.
            std::size_t const wide = std::max<std::size_t>(1, std::min(span.columns, cells - span.column));
            std::size_t last = std::min(index + span.rows, rows.size()) - 1;
            for (std::size_t between = index + 1; between <= last; ++between) {
                if (heading(*rows.at(between))) {
                    last = between - 1;
                    break;
                }
            }
            if (last == index && wide == 1) {
                continue;
            }
            for (std::size_t column = span.column; column < span.column + wide; ++column) {
                coveredUntil.at(column) = last + 1;
            }
            plans.at(index).append(QVariantMap{{QStringLiteral("column"), static_cast<qulonglong>(span.column)},
                                               {QStringLiteral("rows"), static_cast<qulonglong>(last - index + 1)},
                                               {QStringLiteral("columns"), static_cast<qulonglong>(wide)}});
            for (std::size_t between = index; between <= last; ++between) {
                for (std::size_t column = span.column; column < span.column + wide; ++column) {
                    if ((between != index || column != span.column) && column < covered.at(between).size()) {
                        covered.at(between).at(column) = true;
                        coveredColumns.at(between).append(static_cast<qulonglong>(column));
                    }
                }
            }
        }
    }
    // An origin row is drawn above every row below it, so its spanning cell lies over the rows it covers.
    bool any = false;
    for (std::size_t index = 0; index < rows.size(); ++index) {
        TableRowImpl& row = *rows.at(index);
        bool const origin = !plans.at(index).isEmpty();
        any = any || origin || !coveredColumns.at(index).isEmpty();
        row.write("spanPlan", plans.at(index));
        row.write("coveredColumns", coveredColumns.at(index));
        if (QQuickItem* const rowItem = row.item(); rowItem != nullptr) {
            rowItem->setZ(origin ? static_cast<qreal>(rows.size() - index) : 0.0);
        }
        auto const& cells = row.children();
        for (std::size_t column = 0; column < cells.size(); ++column) {
            cells.at(column)->cover(covered.at(index).at(column));
        }
    }
    _planned = any;
    if (item() != nullptr) {
        QMetaObject::invokeMethod(item(), "scheduleSpans", Qt::DirectConnection);
    }
}

void TableImpl::stripe(ItemBase const* leaving) const noexcept {
    // Over a source a row is striped by its view row (`MorphTableRow.viewRow`), not by its place among the children.
    if (read("sourceRows").toBool()) {
        return;
    }
    // Every child is a row, and one being adopted is still being made, so its type is not asked.
    bool alternate = false;
    for (ItemBase const* const child : children()) {
        if (child == leaving) {
            continue;
        }
        child->write("alternate", alternate);
        alternate = !alternate;
    }
}

bool TableImpl::marked(ItemBase const& row) { return row.read("selected").toBool(); }

bool TableImpl::heading(ItemBase const& row) { return row.read("heading").toBool(); }

bool TableImpl::passive(ItemBase const& row) { return heading(row) || row.read("summary").toBool(); }

void TableImpl::mark() const {
    // Every child of a table is one of its rows; the mount makes nothing else in one.
    for (ItemBase const* const child : children()) {
        auto const* const row = dynamic_cast<TableRowImpl const*>(child);
        if (row == nullptr) {
            continue;
        }
        bool const selected = row->key() && !passive(*row) &&
                              (_viewExcept ? !std::ranges::contains(*_viewExcept, *row->key())
                                           : std::ranges::find(_requested, *row->key()) != _requested.end());
        row->write("selected", selected);
    }
}

std::vector<ui::Key> TableImpl::existingSelection() const {
    std::vector<ui::Key> keys;
    for (ui::Key const& key : _requested) {
        // Below a virtualized table's threshold (_existingKeys empty: mount.hpp never calls setExistingKeys
        // there), every key that exists already has a realized row, so the realized-children check alone is
        // exactly right. Above it, a key can exist in the data with no realized row right now -- it is merely
        // dewindowed, not gone -- and must not be dropped from the selection just because its row is off-screen;
        // _existingKeys (the full key set, realized or not) is what tells the two cases apart.
        bool const exists =
            _existingKeys.contains(key) || std::ranges::any_of(children(), [&key](ItemBase const* child) {
                auto const* const row = dynamic_cast<TableRowImpl const*>(child);
                return row != nullptr && row->key() == key;
            });
        if (exists) {
            keys.push_back(key);
        }
    }
    return keys;
}

std::vector<ui::Key> TableImpl::rowKeys() const {
    // Below a virtualized table's threshold every row is realized, in row order. A summary row is never selected.
    if (!_existingOrder.empty()) {
        std::vector<ui::Key> keys;
        for (ui::Key const& key : _existingOrder) {
            if (!std::ranges::contains(_summaryKeys, key)) {
                keys.push_back(key);
            }
        }
        return keys;
    }
    std::vector<ui::Key> keys;
    for (ItemBase const* const child : children()) {
        auto const* const row = dynamic_cast<TableRowImpl const*>(child);
        if (row != nullptr && row->key() && !passive(*row)) {
            keys.push_back(*row->key());
        }
    }
    return keys;
}

void TableImpl::tapped(TableRowImpl const& row, ui::RowClick click) {
    if (_mode == ui::SelectionMode::None || !row.key() || !row.reachable() || passive(row)) {
        return;
    }
    ui::Key const key = *row.key();
    if (_mode == ui::SelectionMode::Single || click == ui::RowClick::Only) {
        _anchor = key;
        select({key}, ui::SelectionChange::Replace);
        return;
    }
    if (click == ui::RowClick::Range) {
        std::vector<ui::Key> const keys = rowKeys();
        auto const target = std::ranges::find(keys, key);
        auto from = _anchor ? std::ranges::find(keys, *_anchor) : keys.end();
        if (from == keys.end()) {
            from = target;
        }
        auto const [low, high] = std::minmax(from, target);
        select(target == keys.end() ? std::vector<ui::Key>{key} : std::vector<ui::Key>{low, std::next(high)},
               ui::SelectionChange::Replace);
        return;
    }
    // A selection of the view: the keys now marked among the rows held, the clicked one toggled. The view state
    // lasts until the caller sets the selection or the view again.
    if (_viewExcept) {
        std::vector<ui::Key> marked;
        for (ui::Key const& held : rowKeys()) {
            bool const out = std::ranges::contains(*_viewExcept, held);
            if (held == key ? out : !out) {
                marked.push_back(held);
            }
        }
        _anchor = key;
        if (_onSelectionChange) {
            // A copy: the handler may destroy this table.
            auto const handler = _onSelectionChange;
            handler(std::move(marked), ui::SelectionChange::Extend);
        }
        return;
    }
    // A user's change drops a requested key still waiting for its row.
    std::vector<ui::Key> next = existingSelection();
    if (auto const found = std::ranges::find(next, key); found != next.end()) {
        next.erase(found);
    } else {
        next.push_back(key);
    }
    _anchor = key;
    select(std::move(next), ui::SelectionChange::Extend);
}

void TableImpl::anchorAt(TableRowImpl const& row) {
    std::vector<ui::Key> const keys = rowKeys();
    if (row.key() && (!_anchor || std::ranges::find(keys, *_anchor) == keys.end())) {
        _anchor = row.key();
    }
}

void TableImpl::setOnSelectAll(std::function<void()> onSelectAll) {
    _onSelectAll = std::move(onSelectAll);
    write("selectAllHandled", static_cast<bool>(_onSelectAll));
}

void TableImpl::selectAll() {
    if (_mode != ui::SelectionMode::Multiple) {
        return;
    }
    if (_onSelectAll) {
        // A copy: the handler may destroy this table.
        auto const handler = _onSelectAll;
        handler();
        return;
    }
    std::vector<ui::Key> keys = rowKeys();
    std::vector<ui::Key> const selected = existingSelection();
    bool const every = std::ranges::all_of(
        keys, [&selected](ui::Key const& key) { return std::ranges::find(selected, key) != selected.end(); });
    select(every ? std::vector<ui::Key>{} : std::move(keys), ui::SelectionChange::Extend);
}

void TableImpl::select(std::vector<ui::Key> next, ui::SelectionChange change) {
    _requested = std::move(next);
    mark();
    if (_onSelectionChange) {
        // A copy: the handler may destroy this table.
        auto const handler = _onSelectionChange;
        handler(existingSelection(), change);
    }
}

void TableImpl::activated(TableRowImpl const& row) const {
    if (!row.key() || !row.reachable() || !_onActivate || passive(row)) {
        return;
    }
    auto const handler = _onActivate;
    handler(*row.key());
}

TableRowImpl::TableRowImpl(Context& context, ui::ContainerWidget* parent, TableImpl& table)
    : ContainerItemWidget{
          context, QStringLiteral("MorphTableRow"), parent,
          QVariantMap{{QStringLiteral("table"), QVariant::fromValue<QObject*>(table.item())},
                      {QStringLiteral("background"),
                       QVariant::fromValue(&context.component(QStringLiteral("MorphTableRowBackground")))}}} {
    // A click or a key on the row: `ctrl` toggles it, `shift` selects a range, neither selects it alone.
    relay().on(QStringLiteral("tap"), [this](QVariant const& payload) {
        if (auto* const owner = dynamic_cast<TableImpl*>(container())) {
            QVariantMap const held = payload.toMap();
            ui::RowClick click = ui::RowClick::Only;
            if (held.value(QStringLiteral("ctrl")).toBool()) {
                click = ui::RowClick::Toggle;
            } else if (held.value(QStringLiteral("shift")).toBool()) {
                click = ui::RowClick::Range;
            }
            owner->tapped(*this, click);
        }
    });
    relay().on(QStringLiteral("anchor"), [this](QVariant const&) {
        if (auto* const owner = dynamic_cast<TableImpl*>(container())) {
            owner->anchorAt(*this);
        }
    });
    // A right click, a long press or the menu key on one of the row's cells, by its column.
    relay().on(QStringLiteral("cellMenu"), [this](QVariant const& payload) {
        qlonglong const column = payload.toMap().value(QStringLiteral("column")).toLongLong();
        if (auto* const owner = dynamic_cast<TableImpl*>(container()); owner != nullptr && column >= 0) {
            owner->openCellMenu(*this, static_cast<std::size_t>(column));
        }
    });
    relay().on(QStringLiteral("activate"), [this](QVariant const&) {
        if (auto const* const owner = dynamic_cast<TableImpl const*>(container())) {
            owner->activated(*this);
        }
    });
    // A tap on one of the row's cells, by its column: the second tap of a double tap with `double`.
    relay().on(QStringLiteral("cellClick"), [this](QVariant const& payload) {
        QVariantMap const map = payload.toMap();
        qlonglong const column = map.value(QStringLiteral("column")).toLongLong();
        if (auto const* const owner = dynamic_cast<TableImpl const*>(container()); owner != nullptr && column >= 0) {
            owner->cellClicked(*this, static_cast<std::size_t>(column), map.value(QStringLiteral("double")).toBool());
        }
    });
}

void TableRowImpl::adopted(ItemBase& child) {
    QQuickItem* const cell = child.item();
    if (cell == nullptr || item() == nullptr) {
        return;
    }
    // The table places each cell at its column's width, so a cell whose content is wider keeps its column and is cut
    // there, rather than pushing its row out of line with the header.
    child.offer("honoursContentMinimum", false);
    // The row places its cells by column and sizes itself to the tallest; a cell whose content arrives later, or
    // changes, places them again. The row's item is the connection's context, and the call goes through it, not
    // through this widget, which may be gone while Qt still holds the item.
    auto const place = [row = QPointer<QQuickItem>{item()}] {
        if (row != nullptr) {
            QMetaObject::invokeMethod(row, "placeSoon", Qt::DirectConnection);
        }
    };
    QObject::connect(cell, &QQuickItem::implicitWidthChanged, item(), place);
    QObject::connect(cell, &QQuickItem::implicitHeightChanged, item(), place);

    // ContainerCore::adopt appends before calling adopted(), so this cell's index in the row is the column its
    // content is bound to -- cells are always adopted in column order. Reported to the table's own QML item, the
    // same safe idiom `place` uses above -- a QPointer to the item, never a pointer to the C++ TableImpl/TableRowImpl
    // wrapper, which may already be destroyed while Qt still holds the items.
    auto const* const table = dynamic_cast<TableImpl const*>(container());
    QQuickItem* const tableItem = table != nullptr ? table->item() : nullptr;
    if (tableItem == nullptr) {
        return;
    }
    int const columnIndex = static_cast<int>(children().size()) - 1;
    // A cell reports a change of its least or width-free natural width itself, through these. Its column is read when
    // it reports: a table over a source moves a pooled cell to another column (`TableImpl::setCellColumn`).
    // A table over a source places each cell in a column of its own choosing (`setCellColumn`), whatever its place
    // among the row's children, so no cell is reported for a column before it has one.
    child.offer("measuredBy", QVariant::fromValue<QObject*>(tableItem));
    child.offer("measuredIndex", tableItem->property("sourceRows").toBool() ? -1 : columnIndex);
    QObject::connect(cell, &QQuickItem::implicitWidthChanged, item(),
                     [tableRef = QPointer<QQuickItem>{tableItem}, cellRef = QPointer<QQuickItem>{cell}] {
                         if (tableRef != nullptr && cellRef != nullptr && cellRef->isVisible()) {
                             QMetaObject::invokeMethod(tableRef, "reportCell", Qt::DirectConnection,
                                                       Q_ARG(int, cellRef->property("measuredIndex").toInt()),
                                                       Q_ARG(QVariant, QVariant::fromValue(cellRef.data())));
                         }
                     });
    // A cell's visibility flipping can only be resolved by rescanning its own column: becoming hidden may drop
    // today's widest cell, becoming visible may introduce a wider one. Rescans only this one column, not every
    // column via invalidateAllColumns: every cell in the table carries this connection, so a full-table sweep here
    // costs O(rows) per cell whose visibility ever changes during mount, compounding into O(rows^2) overall.
    QObject::connect(cell, &QQuickItem::visibleChanged, item(),
                     [tableRef = QPointer<QQuickItem>{tableItem}, cellRef = QPointer<QQuickItem>{cell}] {
                         if (tableRef != nullptr && cellRef != nullptr) {
                             QMetaObject::invokeMethod(tableRef, "rescanColumn", Qt::DirectConnection,
                                                       Q_ARG(int, cellRef->property("measuredIndex").toInt()));
                         }
                     });
}

// A row's cells are bound one by one, each calling one of the three setters below, and each write hands the row's
// whole map to QML, which converts it to a JavaScript object: a write per cell makes a pass over a wide row cost
// the square of its columns. A cell whose entry does not change writes nothing.
namespace {

/// Sets or clears one column's entry in a row's map.
/// @param map The map.
/// @param key The column's key.
/// @param entry The entry; an empty variant clears the column's.
/// @return Whether the map changed.
bool setEntry(QVariantMap& map, QString const& key, QVariant const& entry) {
    if (!entry.isValid()) {
        return map.remove(key) > 0;
    }
    auto const found = map.constFind(key);
    if (found != map.constEnd() && *found == entry) {
        return false;
    }
    map.insert(key, entry);
    return true;
}

}  // namespace

void TableRowImpl::setLoad(std::size_t column, ui::CellLoad load) {
    if (setEntry(_loads, QString::number(column),
                 load == ui::CellLoad::Loaded ? QVariant{} : QVariant{static_cast<int>(load)})) {
        write("cellLoads", _loads);
    }
}

void TableRowImpl::setAccent(std::size_t column, ui::CellAccent const& accent) {
    QVariant entry;
    if (accent != ui::CellAccent{}) {
        entry = QVariantMap{{QStringLiteral("tone"), accent.tone ? static_cast<int>(*accent.tone) : -1},
                            {QStringLiteral("tooltip"), toQString(accent.tooltip)},
                            {QStringLiteral("invalid"), accent.invalid}};
    }
    if (setEntry(_accents, QString::number(column), entry)) {
        write("cellAccents", _accents);
    }
}

void TableRowImpl::setMarks(std::size_t column, ui::CellMarks const& marks) {
    QVariant entry;
    if (marks != ui::CellMarks{}) {
        entry = QVariantMap{{QStringLiteral("readOnly"), marks.readOnly},
                            {QStringLiteral("modified"), marks.modified},
                            {QStringLiteral("error"), marks.error},
                            {QStringLiteral("stale"), marks.stale},
                            {QStringLiteral("muted"), marks.muted}};
    }
    if (setEntry(_marks, QString::number(column), entry)) {
        write("cellMarks", _marks);
    }
}

void TableRowImpl::setGap(int gap) { static_cast<void>(gap); }

void TableRowImpl::setKey(ui::Key key) {
    write("rowKey", encodeKey(key));
    // The row's name until the table gives it a label: its key as text.
    auto const* const number = std::get_if<std::int64_t>(&key);
    write("keyText", number != nullptr ? QString::number(*number) : toQString(std::get<std::string>(key)));
    _key = std::move(key);
}

}  // namespace morph::qt_quick::detail

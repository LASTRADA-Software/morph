// SPDX-License-Identifier: Apache-2.0

#include <QMetaObject>
#include <QObject>
#include <QPointer>
#include <QQuickItem>
#include <QStringList>
#include <QVariantList>
#include <QVariantMap>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <ranges>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

#include <morph/ui/cell_format.hpp>

#include "qt_quick/kinds.hpp"

namespace morph::qt_quick::detail {

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
            handler(static_cast<std::size_t>(column), map.value(QStringLiteral("width")).toInt());
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
    relay().on(QStringLiteral("retry"), [this](QVariant const&) {
        if (auto const handler = _onRetry) {
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
        if (auto const handler = _onMenu) {
            handler(column >= 0 ? std::optional<std::size_t>{static_cast<std::size_t>(column)} : std::nullopt,
                    std::move(path));
        }
    });
    relay().on(QStringLiteral("columnMove"), [this](QVariant const& payload) {
        QVariantMap const map = payload.toMap();
        qlonglong const column = map.value(QStringLiteral("column")).toLongLong();
        qlonglong const position = map.value(QStringLiteral("position")).toLongLong();
        if (auto const handler = _onColumnMove; handler && column >= 0 && position >= 0) {
            handler(static_cast<std::size_t>(column), static_cast<std::size_t>(position));
        }
    });
    // A width in pixels, or none for a double click on the edge.
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
            units = unit > 0 ? std::max(1, static_cast<int>(std::lround(width.toDouble() / unit))) : 1;
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
    _columns = columns;
    QVariantList list;
    for (ui::TableColumn const& column : columns) {
        list.append(QVariantMap{{QStringLiteral("label"), toQString(column.label)},
                                {QStringLiteral("unit"), toQString(column.unit)},
                                {QStringLiteral("accessibleName"),
                                 toQString(ui::headerAccessibleName(column.label, column.unit))},
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

void TableImpl::setRowHeading(ui::Widget& row) {
    auto const* const tableRow = dynamic_cast<TableRowImpl const*>(&row);
    if (tableRow == nullptr || tableRow->container() != this) {
        throw std::logic_error{"morph::qt_quick: setRowHeading of a widget that is not this table's row"};
    }
    tableRow->write("heading", true);
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
    _requested = keys;
    mark();
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
    auto const* const tableRow = dynamic_cast<TableRowImpl const*>(&row);
    if (tableRow == nullptr || tableRow->container() != this) {
        throw std::logic_error{"morph::qt_quick: setRowIndex of a widget that is not this table's row"};
    }
    tableRow->write("viewRow", static_cast<qlonglong>(viewRow));
    if (item() != nullptr) {
        QMetaObject::invokeMethod(item(), "scheduleRows", Qt::DirectConnection);
    }
}

// The counts, the state and the message on one line, two spaces apart.
void TableImpl::setStatus(ui::TableStatus const& status) {
    QStringList parts;
    for (std::string const* const part : {&status.counts, &status.state, &status.message}) {
        if (!part->empty()) {
            parts.append(toQString(*part));
        }
    }
    write("statusText", parts.join(QStringLiteral("  ")));
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

void TableImpl::setColumnWindow(std::span<std::size_t const> columns, std::span<int const> offsets,
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
                            .padding = static_cast<int>(std::lround(read("measurePadding").toDouble()))};
}

void TableImpl::setOnColumnMeasured(std::function<void(std::size_t column, int width)> handler) {
    _onColumnMeasured = std::move(handler);
}

void TableImpl::setFocusCell(std::size_t viewRow, std::size_t column) {
    write("focusedRowIndex", static_cast<qlonglong>(viewRow));
    write("focusedColumnIndex", static_cast<int>(column));
    if (item() != nullptr) {
        QMetaObject::invokeMethod(item(), "showFocusCell", Qt::DirectConnection);
    }
}

void TableImpl::setOnFocusMove(std::function<void(ui::FocusMove)> handler) { _onFocusMove = std::move(handler); }

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
            if (span.column >= cells || span.rows < 2 || coveredUntil.at(span.column) > index) {
                continue;
            }
            std::size_t last = std::min(index + span.rows, rows.size()) - 1;
            for (std::size_t between = index + 1; between <= last; ++between) {
                if (heading(*rows.at(between))) {
                    last = between - 1;
                    break;
                }
            }
            if (last == index) {
                continue;
            }
            coveredUntil.at(span.column) = last + 1;
            plans.at(index).append(QVariantMap{{QStringLiteral("column"), static_cast<qulonglong>(span.column)},
                                               {QStringLiteral("rows"), static_cast<qulonglong>(last - index + 1)}});
            for (std::size_t between = index + 1; between <= last; ++between) {
                if (span.column < covered.at(between).size()) {
                    covered.at(between).at(span.column) = true;
                    coveredColumns.at(between).append(static_cast<qulonglong>(span.column));
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

void TableImpl::mark() const {
    // Every child of a table is one of its rows; the mount makes nothing else in one.
    for (ItemBase const* const child : children()) {
        auto const* const row = dynamic_cast<TableRowImpl const*>(child);
        if (row == nullptr) {
            continue;
        }
        bool const selected =
            row->key() && !heading(*row) && std::ranges::find(_requested, *row->key()) != _requested.end();
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
    // Below a virtualized table's threshold every row is realized, in row order.
    if (!_existingOrder.empty()) {
        return _existingOrder;
    }
    std::vector<ui::Key> keys;
    for (ItemBase const* const child : children()) {
        auto const* const row = dynamic_cast<TableRowImpl const*>(child);
        if (row != nullptr && row->key() && !heading(*row)) {
            keys.push_back(*row->key());
        }
    }
    return keys;
}

void TableImpl::tapped(TableRowImpl const& row, ui::RowClick click) {
    if (_mode == ui::SelectionMode::None || !row.key() || !row.reachable() || heading(row)) {
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
    if (!row.key() || !row.reachable() || !_onActivate || heading(row)) {
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
    relay().on(QStringLiteral("activate"), [this](QVariant const&) {
        if (auto const* const owner = dynamic_cast<TableImpl const*>(container())) {
            owner->activated(*this);
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
            QMetaObject::invokeMethod(row, "place", Qt::DirectConnection);
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
    child.offer("measuredBy", QVariant::fromValue<QObject*>(tableItem));
    child.offer("measuredIndex", columnIndex);
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

void TableRowImpl::setLoad(std::size_t column, ui::CellLoad load) {
    QString const key = QString::number(column);
    if (load == ui::CellLoad::Loaded) {
        _loads.remove(key);
    } else {
        _loads.insert(key, static_cast<int>(load));
    }
    write("cellLoads", _loads);
}

void TableRowImpl::setAccent(std::size_t column, ui::CellAccent const& accent) {
    QString const key = QString::number(column);
    if (accent == ui::CellAccent{}) {
        _accents.remove(key);
    } else {
        _accents.insert(key, QVariantMap{{QStringLiteral("tone"), accent.tone ? static_cast<int>(*accent.tone) : -1},
                                         {QStringLiteral("tooltip"), toQString(accent.tooltip)},
                                         {QStringLiteral("invalid"), accent.invalid}});
    }
    write("cellAccents", _accents);
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

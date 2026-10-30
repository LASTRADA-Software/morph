// SPDX-License-Identifier: Apache-2.0

#include <QMetaObject>
#include <QObject>
#include <QPointer>
#include <QVariantList>
#include <QVariantMap>
#include <algorithm>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

#include "qt_quick/kinds.hpp"

namespace morph::qt_quick::detail {

TableImpl::TableImpl(Context& context, ui::ContainerWidget* parent)
    : ContainerItemWidget{
          context, QStringLiteral("MorphTable"), parent,
          QVariantMap{{QStringLiteral("headerBackground"),
                       QVariant::fromValue(&context.component(QStringLiteral("MorphTableHeaderBackground")))}}} {
    relay().on(QStringLiteral("scroll"), [this](QVariant const& payload) {
        QVariantMap const map = payload.toMap();
        scrolled(static_cast<std::size_t>(std::max<qlonglong>(0, map.value(QStringLiteral("first")).toLongLong())),
                 static_cast<std::size_t>(std::max<qlonglong>(0, map.value(QStringLiteral("count")).toLongLong())));
    });
    relay().on(QStringLiteral("selectAll"), [this](QVariant const&) { selectAll(); });
    relay().on(QStringLiteral("sort"), [this](QVariant const& payload) {
        if (_onSort && payload.toLongLong() >= 0) {
            // A copy: the handler may destroy this table.
            auto const handler = _onSort;
            handler(static_cast<std::size_t>(payload.toLongLong()));
        }
    });
}

void TableImpl::setColumns(std::vector<ui::TableColumn> const& columns) {
    QVariantList list;
    for (ui::TableColumn const& column : columns) {
        list.append(QVariantMap{{QStringLiteral("label"), toQString(column.label)},
                                {QStringLiteral("kind"), static_cast<int>(column.width.kind)},
                                {QStringLiteral("amount"), column.width.amount},
                                {QStringLiteral("role"), static_cast<int>(column.role)},
                                {QStringLiteral("wrap"), column.wrap},
                                {QStringLiteral("align"), static_cast<int>(column.align)},
                                {QStringLiteral("valign"), static_cast<int>(column.valign)},
                                {QStringLiteral("sortable"), column.sortable}});
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

// MorphTable's `sortColumn` is -1 for none.
void TableImpl::setSort(std::optional<ui::TableSort> sort) {
    write("sortDescending", sort && sort->direction == ui::SortDirection::Descending);
    write("sortColumn", sort ? static_cast<int>(sort->column) : -1);
}

void TableImpl::setOnSort(std::function<void(std::size_t)> onSort) { _onSort = std::move(onSort); }

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

void TableImpl::scrolled(std::size_t first, std::size_t count) const {
    if (_onVisibleRangeChanged) {
        _onVisibleRangeChanged(first, count);
    }
}

void TableImpl::adopted(ItemBase& child) {
    static_cast<void>(child);
    mark();
    stripe(nullptr);
    // A newly-adopted row may be the table's first with columns already set (columns usually arrive before any
    // row), or `setColumns` may not yet have run for every column at mount time -- either way, make sure the cache
    // covers every column before anything reads `widths`. This is an explicit, outside-any-binding call: seeding
    // lazily from inside the `widths` binding's own evaluation (cachedWidth) previously caused `widths` to be
    // marked dirty and re-entered while still computing itself, as each row's adoption retriggered a seed.
    if (item() != nullptr) {
        QMetaObject::invokeMethod(item(), "seedMissingColumns", Qt::DirectConnection);
    }
}

void TableImpl::moved() { stripe(nullptr); }

void TableImpl::releasing(ItemBase& child) noexcept {
    stripe(&child);
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

void TableImpl::stripe(ItemBase const* leaving) const noexcept {
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

void TableImpl::selectAll() {
    if (_mode != ui::SelectionMode::Multiple) {
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
    QObject::connect(cell, &QQuickItem::implicitWidthChanged, item(),
                     [tableRef = QPointer<QQuickItem>{tableItem}, columnIndex, cellRef = QPointer<QQuickItem>{cell}] {
                         if (tableRef != nullptr && cellRef != nullptr && cellRef->isVisible()) {
                             QMetaObject::invokeMethod(tableRef, "reportCellWidth", Qt::DirectConnection,
                                                       Q_ARG(int, columnIndex),
                                                       Q_ARG(qreal, cellRef->implicitWidth()));
                         }
                     });
    // A cell's visibility flipping can only be resolved by rescanning its own column: becoming hidden may drop
    // today's widest cell, becoming visible may introduce a wider one. Rescans only this one column, not every
    // column via invalidateAllColumns: every cell in the table carries this connection, so a full-table sweep here
    // costs O(rows) per cell whose visibility ever changes during mount, compounding into O(rows^2) overall.
    QObject::connect(
        cell, &QQuickItem::visibleChanged, item(), [tableRef = QPointer<QQuickItem>{tableItem}, columnIndex] {
            if (tableRef != nullptr) {
                QMetaObject::invokeMethod(tableRef, "rescanColumn", Qt::DirectConnection, Q_ARG(int, columnIndex));
            }
        });
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

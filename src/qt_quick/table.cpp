// SPDX-License-Identifier: Apache-2.0

#include <QVariantList>
#include <QVariantMap>
#include <algorithm>
#include <utility>

#include "qt_quick/kinds.hpp"

namespace morph::qt_quick::detail {

TableImpl::TableImpl(Context& context, ui::ContainerWidget* parent)
    : ContainerItemWidget{context, QStringLiteral("MorphTable"), parent} {}

void TableImpl::setColumns(std::vector<ui::TableColumn> const& columns) {
    QVariantList list;
    for (ui::TableColumn const& column : columns) {
        list.append(QVariantMap{{QStringLiteral("label"), toQString(column.label)},
                                {QStringLiteral("kind"), static_cast<int>(column.width.kind)},
                                {QStringLiteral("amount"), column.width.amount}});
    }
    write("columns", list);
}

void TableImpl::setSelectionMode(ui::SelectionMode mode) { _mode = mode; }

void TableImpl::setRowKey(ui::Widget& row, ui::Key const& key) {
    if (auto* const tableRow = dynamic_cast<TableRowImpl*>(&row)) {
        tableRow->setKey(key);
        mark();
    }
}

void TableImpl::setSelection(std::vector<ui::Key> const& keys) {
    _requested = keys;
    mark();
}

void TableImpl::setOnSelectionChange(std::function<void(std::vector<ui::Key>)> onSelectionChange) {
    _onSelectionChange = std::move(onSelectionChange);
}

void TableImpl::setOnActivate(std::function<void(ui::Key)> onActivate) { _onActivate = std::move(onActivate); }

void TableImpl::adopted(ItemBase& child) {
    static_cast<void>(child);
    mark();
}

bool TableImpl::marked(ItemBase const& row) { return row.read("selected").toBool(); }

void TableImpl::mark() const {
    for (ItemBase* const child : children()) {
        auto const* const row = dynamic_cast<TableRowImpl const*>(child);
        bool const selected =
            row != nullptr && row->key() && std::ranges::find(_requested, *row->key()) != _requested.end();
        child->write("selected", selected);
    }
}

std::vector<ui::Key> TableImpl::existingSelection() const {
    std::vector<ui::Key> keys;
    for (ui::Key const& key : _requested) {
        bool const exists = std::ranges::any_of(children(), [&key](ItemBase const* child) {
            auto const* const row = dynamic_cast<TableRowImpl const*>(child);
            return row != nullptr && row->key() == key;
        });
        if (exists) {
            keys.push_back(key);
        }
    }
    return keys;
}

void TableImpl::tapped(TableRowImpl const& row, bool extend) {
    if (_mode == ui::SelectionMode::None || !row.key() || !row.reachable()) {
        return;
    }
    ui::Key const& key = *row.key();
    if (_mode == ui::SelectionMode::Multiple && extend) {
        // A user's change drops a requested key still waiting for its row.
        std::vector<ui::Key> next = existingSelection();
        if (auto const found = std::ranges::find(next, key); found != next.end()) {
            next.erase(found);
        } else {
            next.push_back(key);
        }
        _requested = std::move(next);
    } else {
        _requested = {key};
    }
    mark();
    if (_onSelectionChange) {
        // A copy: the handler may destroy this table.
        auto const handler = _onSelectionChange;
        handler(existingSelection());
    }
}

void TableImpl::activated(TableRowImpl const& row) const {
    if (!row.key() || !row.reachable() || !_onActivate) {
        return;
    }
    auto const handler = _onActivate;
    handler(*row.key());
}

TableRowImpl::TableRowImpl(Context& context, ui::ContainerWidget* parent, TableImpl& table)
    : ContainerItemWidget{context, QStringLiteral("MorphTableRow"), parent,
                          QVariantMap{{QStringLiteral("table"), QVariant::fromValue<QObject*>(table.item())}}} {
    relay().on(QStringLiteral("tap"), [this](QVariant const& payload) {
        if (auto* const owner = dynamic_cast<TableImpl*>(container())) {
            owner->tapped(*this, payload.toBool());
        }
    });
    relay().on(QStringLiteral("activate"), [this](QVariant const&) {
        if (auto const* const owner = dynamic_cast<TableImpl const*>(container())) {
            owner->activated(*this);
        }
    });
}

void TableRowImpl::setGap(int gap) { static_cast<void>(gap); }

void TableRowImpl::setKey(ui::Key key) {
    write("rowKey", encodeKey(key));
    _key = std::move(key);
}

}  // namespace morph::qt_quick::detail

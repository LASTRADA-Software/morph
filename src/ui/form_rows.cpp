// SPDX-License-Identifier: Apache-2.0

// The non-template definitions of <morph/ui/form_rows.hpp>.

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <morph/core/callback_scope.hpp>
#include <morph/forms/engine/form_session.hpp>
#include <morph/reactive/runtime.hpp>
#include <morph/reactive/signal.hpp>
#include <morph/table/data_source.hpp>
#include <morph/ui/form_rows.hpp>
#include <morph/ui/table_rows.hpp>
#include <morph/ui/value.hpp>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

namespace morph::ui::detail {

table::ColumnKind columnKindOf(forms::FieldKind kind) noexcept {
    using forms::FieldKind;
    switch (kind) {
        case FieldKind::Integer:
        case FieldKind::Slider:
            return table::ColumnKind::Integer;
        case FieldKind::Decimal:
        case FieldKind::Quantity:
            return table::ColumnKind::Decimal;
        case FieldKind::Number:
            return table::ColumnKind::Number;
        case FieldKind::Date:
        case FieldKind::EpochDate:
            return table::ColumnKind::Date;
        case FieldKind::DateTime:
        case FieldKind::EpochDateTime:
            return table::ColumnKind::DateTime;
        case FieldKind::Boolean:
            return table::ColumnKind::Bool;
        case FieldKind::Text:
        case FieldKind::Multiline:
        case FieldKind::Enum:
        case FieldKind::Choice:
        case FieldKind::Array:
        case FieldKind::Object:
        case FieldKind::ObjectArray:
        default:
            return table::ColumnKind::Text;
    }
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
table::Cell memberCell(forms::FieldState const& field, table::ColumnKind kind) {
    forms::Encoding const& encoding = field.encoded().get();
    if (!encoding.has_value()) {
        return std::monostate{};
    }
    if (!encoding->has_value()) {
        return std::string{field.draft()};
    }
    forms::WireValue const& wire = **encoding;
    Value const& value = wire.value;
    switch (kind) {
        case table::ColumnKind::Integer:
            if (std::int64_t const* const whole = value.asInt()) {
                return *whole;
            }
            break;
        case table::ColumnKind::Decimal:
            if (Decimal const* const exact = value.asDecimal()) {
                return exact->value;
            }
            if (std::int64_t const* const whole = value.asInt()) {
                return math::Rational{*whole, math::DecimalPlaces{0}};
            }
            break;
        case table::ColumnKind::Number: {
            if (Decimal const* const exact = value.asDecimal()) {
                return exact->value.toDouble();
            }
            if (std::int64_t const* const whole = value.asInt()) {
                return static_cast<double>(*whole);
            }
            break;
        }
        case table::ColumnKind::Bool:
            if (bool const* const flag = value.asBool()) {
                return *flag;
            }
            break;
        case table::ColumnKind::Date: {
            if (std::int64_t const* const days = value.asInt()) {
                return *days;
            }
            std::string_view const text = forms::detail::engine::trim(field.draft());
            if (auto const midnight = forms::detail::engine::parseWallClock(std::string{text} + "T00:00")) {
                // Assigned, not cast: the duration's count type is the library's own, and only the
                // assignment is right whether or not it is `std::int64_t`.
                std::int64_t const days = std::chrono::floor<std::chrono::days>(*midnight).time_since_epoch().count();
                return days;
            }
            break;
        }
        case table::ColumnKind::DateTime:
            if (std::int64_t const* const seconds = value.asInt()) {
                return *seconds;
            }
            if (time::DateTime const* const instant = value.asTimestamp()) {
                std::int64_t const seconds =
                    std::chrono::floor<std::chrono::seconds>(instant->value).time_since_epoch().count();
                return seconds;
            }
            break;
        case table::ColumnKind::Quantity:
        case table::ColumnKind::Text:
        case table::ColumnKind::Key:
        case table::ColumnKind::Custom:
        case table::ColumnKind::Measure:
        default:
            if (std::string const* const text = value.asString()) {
                return *text;
            }
            return wire.json;
    }
    return std::string{field.draft()};
}

table::Cell perRowCell(forms::FieldState const& field) {
    forms::Encoding const& encoding = field.encoded().get();
    if (encoding.has_value() && encoding->has_value()) {
        if (std::string const* const text = (*encoding)->value.asString()) {
            return *text;
        }
    }
    std::string const& draft = field.draft();
    return draft.empty() ? table::Cell{} : table::Cell{draft};
}

}  // namespace morph::ui::detail

namespace morph::ui {

FormRowsSource::FormRowsSource(forms::FormSession& form, std::string arrayPath, std::vector<table::ColumnInfo> columns,
                               std::vector<std::string> members,
                               std::vector<std::function<std::string(forms::RowState const&)>> perRow)
    : _form{&form},
      _arrayPath{std::move(arrayPath)},
      _array{&form.field(_arrayPath)},
      _columns{std::move(columns)},
      _members{std::move(members)},
      _perRow{std::move(perRow)} {
    if (_array->spec().kind != forms::FieldKind::ObjectArray || _array->spec().truncated) {
        throw std::invalid_argument{"morph::ui::FormRowsSource: `" + _arrayPath + "` is not a collection"};
    }
    if (_members.empty()) {
        for (table::ColumnInfo const& column : _columns) {
            _members.push_back(column.id);
        }
    }
    if (_members.size() != _columns.size()) {
        throw std::invalid_argument{"morph::ui::FormRowsSource: one member per column"};
    }
    if (_perRow.empty()) {
        _perRow.resize(_columns.size());
    }
    if (_perRow.size() != _columns.size()) {
        throw std::invalid_argument{"morph::ui::FormRowsSource: one per-row member reader per column"};
    }
    for (std::size_t at = 0; at < _members.size(); ++at) {
        std::string const& name = _members.at(at);
        if (_perRow.at(at)) {
            continue;
        }
        if (std::ranges::none_of(_array->spec().children,
                                 [&name](forms::FieldSpec const& member) { return member.name == name; })) {
            throw std::invalid_argument{"morph::ui::FormRowsSource: no member `" + name + "` in `" + _arrayPath + "`"};
        }
    }
    // Reading it is free while nothing it tracks changed, and walks every member once after a change.
    _version = std::make_unique<reactive::Computed<std::uint64_t>>(form.runtime(), [this] {
        for (auto const& row : _array->rows()) {
            for (auto const& member : row->fields()) {
                static_cast<void>(member->encoded().get());
                static_cast<void>(member->draft());
            }
        }
        return ++_versions;
    });
    _synced = currentVersion();
    _held = build();
    _watch = std::make_unique<reactive::Effect>(form.runtime(), [this] { watch(); });
}

forms::RowState* FormRowsSource::row(table::RowId const& key) const {
    std::int64_t const* const number = std::get_if<std::int64_t>(&key);
    if (number == nullptr || *number < 0) {
        return nullptr;
    }
    // The rows by key, taken again only after the field's rows or a draft changed.
    if (std::uint64_t const now = currentVersion(); now != _indexed) {
        _indexed = now;
        _byKey.clear();
        _form->runtime().untracked([&] {
            for (auto const& held : _array->rows()) {
                _byKey.insert_or_assign(held->key(), held.get());
            }
        });
    }
    auto const found = _byKey.find(static_cast<std::uint64_t>(*number));
    return found == _byKey.end() ? nullptr : found->second;
}

void FormRowsSource::setGroup(std::function<Value(forms::RowState const&)> groupBy) {
    bool const listed = _group != nullptr;
    _group = std::move(groupBy);
    if (_group && !listed) {
        _columns.push_back(table::ColumnInfo{.id = std::string{kGroupRunColumn}, .kind = table::ColumnKind::Integer});
        _members.emplace_back();
        _perRow.emplace_back();
    } else if (!_group && listed) {
        _columns.pop_back();
        _members.pop_back();
        _perRow.pop_back();
    }
    _held = build();
    _listeners.notify(table::RowChange{.kind = table::ChangeKind::Reset, .rows = {}});
}

RecordOf FormRowsSource::recordOf() {
    return [](table::RowSnapshot const& snapshot, std::size_t sourceRow) {
        auto const* const rows = dynamic_cast<detail::FormRowsSnapshot const*>(&snapshot);
        return RowRecord{.key = snapshot.rowId(sourceRow), .row = rows == nullptr ? Value{} : rows->record(sourceRow)};
    };
}

bool FormRowsSource::sync() {
    _posted = false;
    if (std::uint64_t const now = currentVersion(); now != _synced) {
        _synced = now;
    } else {
        return false;
    }
    auto next = build();
    auto const& before = *_held;
    auto const& after = *next;
    if (before.keys() == after.keys()) {
        std::vector<std::size_t> changed;
        for (std::size_t row = 0; row < after.rowCount(); ++row) {
            if (before.cells().at(row) != after.cells().at(row) || !(before.record(row) == after.record(row))) {
                changed.push_back(row);
            }
        }
        if (changed.empty()) {
            return false;
        }
        _held = std::move(next);
        _listeners.notify(table::RowChange{.kind = table::ChangeKind::Updated, .rows = std::move(changed)});
        return true;
    }
    std::optional<table::RowChange> const change = structural(before, after);
    _held = std::move(next);
    _listeners.notify(change.value_or(table::RowChange{.kind = table::ChangeKind::Reset, .rows = {}}));
    return true;
}

std::optional<table::RowChange> FormRowsSource::structural(detail::FormRowsSnapshot const& before,
                                                           detail::FormRowsSnapshot const& after) {
    bool const growing = after.rowCount() > before.rowCount();
    detail::FormRowsSnapshot const& longer = growing ? after : before;
    detail::FormRowsSnapshot const& shorter = growing ? before : after;
    std::vector<std::size_t> rows;
    std::size_t kept = 0;
    for (std::size_t at = 0; at < longer.rowCount(); ++at) {
        if (kept < shorter.rowCount() && longer.keys().at(at) == shorter.keys().at(kept)) {
            if (longer.cells().at(at) != shorter.cells().at(kept) || !(longer.record(at) == shorter.record(kept))) {
                return std::nullopt;
            }
            ++kept;
        } else {
            rows.push_back(at);
        }
    }
    if (kept != shorter.rowCount()) {
        return std::nullopt;
    }
    return table::RowChange{.kind = growing ? table::ChangeKind::Inserted : table::ChangeKind::Removed,
                            .rows = std::move(rows)};
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity): one pass building the snapshot's parallel vectors.
std::shared_ptr<detail::FormRowsSnapshot const> FormRowsSource::build() const {
    ++_builds;
    return _form->runtime().untracked([&] {
        std::vector<table::RowId> keys;
        std::vector<std::vector<table::Cell>> cells;
        std::vector<Value> records;
        // The run column, while grouped, is the last; its cell is filled once the runs are known.
        std::size_t const own = _group ? _columns.size() - 1 : _columns.size();
        for (auto const& row : _array->rows()) {
            keys.emplace_back(static_cast<std::int64_t>(row->key()));
            std::vector<table::Cell> rowCells;
            rowCells.reserve(_columns.size());
            for (std::size_t at = 0; at < own; ++at) {
                if (_perRow.at(at)) {
                    // A member per row: its kind varies, so the cell is text (`detail::perRowCell`).
                    forms::FieldState const* const member = row->field(_perRow.at(at)(*row));
                    rowCells.push_back(member == nullptr ? table::Cell{} : detail::perRowCell(*member));
                    continue;
                }
                forms::FieldState const* const member = row->field(_members.at(at));
                rowCells.push_back(member == nullptr ? table::Cell{}
                                                     : detail::memberCell(*member, _columns.at(at).kind));
            }
            cells.push_back(std::move(rowCells));
            ValueRecord record;
            for (auto const& member : row->fields()) {
                forms::Encoding const& encoding = member->encoded().get();
                record.insert_or_assign(member->spec().name,
                                        encoding.has_value() && encoding->has_value() ? (*encoding)->value : Value{});
            }
            record.insert_or_assign(std::string{kGridKeyField}, Value{static_cast<std::int64_t>(row->key())});
            records.emplace_back(std::move(record));
        }
        std::vector<Value> runs;
        if (_group) {
            runs = groupRuns(records, cells);
        }
        return std::make_shared<detail::FormRowsSnapshot const>(_columns.size(), std::move(keys), std::move(cells),
                                                                std::move(records), std::move(runs));
    });
}

std::vector<Value> FormRowsSource::groupRuns(std::vector<Value> const& records,
                                             std::vector<std::vector<table::Cell>>& cells) const {
    std::vector<Value> runs;
    std::map<std::string, std::size_t, std::less<>> ordinals;
    std::optional<Value> key;
    ValueList items;
    auto const close = [&] {
        if (!key) {
            return;
        }
        std::string const json = ui::detail::canonicalJson(*key);
        Value const record = runRecord(*key, std::exchange(items, {}));
        ValueRecord entry = *record.asRecord();
        entry.insert_or_assign(std::string{detail::kGroupHeadingField},
                               Value{"\x1Fgroup:" + json + ":" + std::to_string(ordinals[json]++)});
        runs.emplace_back(std::move(entry));
    };
    std::size_t index = 0;
    for (auto const& row : _array->rows()) {
        Value groupKey = _group(*row);
        if (key && !detail::sameElement(*key, groupKey, UnitTable{})) {
            close();
            key.reset();
        }
        if (!key) {
            key = std::move(groupKey);
        }
        items.push_back(records.at(index));
        cells.at(index).emplace_back(static_cast<std::int64_t>(runs.size()));
        ++index;
    }
    close();
    return runs;
}

void FormRowsSource::watch() {
    static_cast<void>(_version->get());
    if (_watching && !_posted) {
        _posted = true;
        _form->runtime().owner().post(_alive.guard([this] { sync(); }));
    }
    _watching = true;
}

}  // namespace morph::ui

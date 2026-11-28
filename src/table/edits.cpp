// SPDX-License-Identifier: Apache-2.0

// The non-template definitions of <morph/table/edits.hpp>.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <expected>
#include <functional>
#include <initializer_list>
#include <iterator>
#include <map>
#include <morph/core/callback_scope.hpp>
#include <morph/forms/engine/field_model.hpp>
#include <morph/table/data_source.hpp>
#include <morph/table/description.hpp>
#include <morph/table/edits.hpp>
#include <morph/table/engine.hpp>
#include <morph/table/record_name.hpp>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <typeindex>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

namespace morph::table::detail {

bool editFits(ColumnKind column, forms::FieldKind field) {
    using forms::FieldKind;
    auto const among = [field](std::initializer_list<FieldKind> kinds) { return std::ranges::contains(kinds, field); };
    switch (column) {
        case ColumnKind::Integer:
            return among({FieldKind::Integer, FieldKind::Slider, FieldKind::Enum, FieldKind::Choice});
        case ColumnKind::Key:
            return among({FieldKind::Integer, FieldKind::Choice});
        case ColumnKind::Decimal:
            return among({FieldKind::Decimal});
        case ColumnKind::Quantity:
        case ColumnKind::Measure:
            return among({FieldKind::Quantity});
        case ColumnKind::Number:
            return among({FieldKind::Number, FieldKind::Slider});
        case ColumnKind::Text:
            return among({FieldKind::Text, FieldKind::Multiline, FieldKind::Enum, FieldKind::Choice});
        case ColumnKind::Date:
            return among({FieldKind::Date, FieldKind::EpochDate});
        case ColumnKind::DateTime:
            return among({FieldKind::DateTime, FieldKind::EpochDateTime});
        case ColumnKind::Bool:
            return among({FieldKind::Boolean});
        case ColumnKind::Custom:
            return among({FieldKind::Text, FieldKind::Enum, FieldKind::Choice});
        default:
            return false;
    }
}

}  // namespace morph::table::detail

namespace morph::table {

// NOLINTNEXTLINE(readability-function-cognitive-complexity): a run of independent rules, one `if` each, in the order they are reported; splitting scatters that order.
std::expected<void, TableError> checkEdits(
    TableDescription const& description,
    std::function<std::optional<std::string>(std::string_view action)> const& schemaOf) {
    auto const refuse = [](std::string const& column, std::string message) -> std::expected<void, TableError> {
        return std::unexpected(
            TableError{.code = TableErrorCode::SourceContract, .column = column, .message = std::move(message)});
    };
    for (TableColumn const& column : description.columns) {
        if (!column.edit) {
            continue;
        }
        std::string const& columnId = column.info.id;
        EditInfo const& edit = *column.edit;
        std::optional<std::string> const schema = schemaOf ? schemaOf(edit.action) : std::nullopt;
        if (!schema) {
            return refuse(columnId,
                          "column `" + columnId + "` is edited by `" + edit.action + "`, which is not an action here");
        }
        auto model = forms::FormModel::fromSchema(edit.action, *schema);
        if (!model) {
            return refuse(columnId, "column `" + columnId + "`'s edit action `" + edit.action +
                                        "` has an input schema that does not read: " + model.error().message);
        }
        std::span<forms::FieldSpec const> const fields = model->fields();
        forms::FieldSpec const* field = nullptr;
        if (edit.member.empty()) {
            auto const found = std::ranges::find(fields, std::string_view{"value"}, &forms::FieldSpec::name);
            field = found == fields.end() ? nullptr : &*found;
        } else {
            auto const values = std::ranges::find(fields, std::string_view{"values"}, &forms::FieldSpec::name);
            if (values != fields.end()) {
                auto const found = std::ranges::find(values->children, edit.member, &forms::FieldSpec::name);
                field = found == values->children.end() ? nullptr : &*found;
            }
        }
        if (field == nullptr) {
            return refuse(columnId, edit.member.empty() ? "column `" + columnId + "`'s edit action `" + edit.action +
                                                              "` has no member `value`"
                                                        : "column `" + columnId + "`'s edit action `" + edit.action +
                                                              "` has no member `" + edit.member + "` in its `values`");
        }
        // A measure is edited in its field's unit: one the table's units lack could hold none of its cells.
        if (column.info.kind == ColumnKind::Measure &&
            !std::ranges::contains(description.units, field->unitAscii, &UnitInfo::id)) {
            return refuse(columnId, "column `" + columnId + "` is a measure edited in `" + field->unitAscii +
                                        "`, which is not one of the table's units");
        }
        if (!detail::editFits(column.info.kind, field->kind)) {
            return refuse(columnId, "column `" + columnId + "` cannot be edited by `" + edit.action +
                                        "`'s field of kind `" + std::string{forms::fieldKindName(field->kind)} + "`");
        }
    }
    return {};
}

// NOLINTNEXTLINE(bugprone-exception-escape) -- a handler that throws from here terminates, as from any destructor
CellEdits::~CellEdits() {
    if (_engine == nullptr) {
        return;
    }
    std::vector<RowId> rows{_ending.begin(), _ending.end()};
    for (auto const& [row, queue] : _queues) {
        rows.push_back(row);
    }
    _queues.clear();
    _ending.clear();
    for (auto const& row : rows) {
        _engine->endEdit(row);
    }
}

void CellEdits::commit(EditRequest request) {
    if (unloadedInView(request)) {
        _errors[request.row].insert_or_assign(request.column, "not loaded yet");
        return;
    }
    auto& queue = _queues[request.row];
    queue.push_back(std::move(request));
    if (queue.size() == 1) {
        start(queue.front().row);
    }
}

void CellEdits::commitRow(RowEditRequest request) {
    RowId const row = request.row;
    std::vector<std::optional<std::string>> drafts;
    drafts.reserve(request.cells.size());
    for (EditCell const& cell : request.cells) {
        drafts.push_back(draft(row, cell.column));
    }
    auto& queue = _rowQueues[row];
    queue.push_back(RowFlight{.id = ++_nextFlight,
                              .request = std::move(request),
                              .sent = false,
                              .discard = false,
                              .drafts = std::move(drafts)});
    if (queue.size() == 1) {
        startRow(row);
    }
}

void CellEdits::setDraft(RowId const& row, std::string_view column, std::string text,
                         std::optional<std::string> error) {
    CellState& cell = _cells[row][std::string{column}];
    cell.draft = std::move(text);
    cell.settled = false;
    if (error) {
        _errors[row].insert_or_assign(std::string{column}, std::move(*error));
    } else {
        eraseError(row, column);
    }
}

std::optional<std::string> CellEdits::draft(RowId const& row, std::string_view column) const {
    CellState const* const cell = cellOf(row, column);
    return cell == nullptr ? std::nullopt : cell->draft;
}

void CellEdits::clearDraft(RowId const& row, std::string_view column) {
    if (auto const found = _cells.find(row); found != _cells.end()) {
        if (auto const cell = found->second.find(column); cell != found->second.end()) {
            cell->second.draft.reset();
            cell->second.settled = false;
            if (!cell->second.modified) {
                found->second.erase(cell);
            }
        }
        if (found->second.empty()) {
            _cells.erase(found);
        }
    }
    eraseError(row, column);
}

bool CellEdits::modified(RowId const& row, std::string_view column) const {
    CellState const* const cell = cellOf(row, column);
    return cell != nullptr && cell->modified;
}

bool CellEdits::rowEdited(RowId const& row) const {
    auto const found = _cells.find(row);
    return found != _cells.end() && std::ranges::any_of(found->second, [](auto const& entry) {
               return entry.second.modified || entry.second.draft.has_value();
           });
}

void CellEdits::rowFetched(RowId const& row) {
    auto const found = _cells.find(row);
    if (found == _cells.end()) {
        return;
    }
    for (auto cell = found->second.begin(); cell != found->second.end();) {
        if (!cell->second.modified || stale(row, cell->first)) {
            ++cell;
            continue;
        }
        cell->second.modified = false;
        if (cell->second.settled) {
            cell->second.draft.reset();
            cell->second.settled = false;
        }
        cell = cell->second.draft ? std::next(cell) : found->second.erase(cell);
    }
    if (found->second.empty()) {
        _cells.erase(found);
    }
}

void CellEdits::rowAbsent(RowId const& row) {
    _cells.erase(row);
    _errors.erase(row);
    auto const found = _rowQueues.find(row);
    if (found == _rowQueues.end()) {
        return;
    }
    std::erase_if(found->second, [](RowFlight const& flight) { return !flight.sent; });
    for (RowFlight& flight : found->second) {
        flight.discard = true;
    }
    if (found->second.empty()) {
        _rowQueues.erase(found);
    }
}

void CellEdits::moveRow(RowId const& from, RowId const& destination) {
    if (from == destination) {
        return;
    }
    if (auto const found = _cells.find(from); found != _cells.end()) {
        auto moved = std::move(found->second);
        _cells.erase(found);
        auto& target = _cells[destination];
        for (auto& [column, cell] : moved) {
            target.try_emplace(column, std::move(cell));
        }
    }
    if (auto const found = _errors.find(from); found != _errors.end()) {
        auto moved = std::move(found->second);
        _errors.erase(found);
        auto& target = _errors[destination];
        for (auto& [column, message] : moved) {
            target.try_emplace(column, std::move(message));
        }
    }
    if (auto const found = _rowQueues.find(from); found != _rowQueues.end()) {
        auto moved = std::move(found->second);
        _rowQueues.erase(found);
        auto& target = _rowQueues[destination];
        for (RowFlight& flight : moved) {
            flight.request.row = destination;
            target.push_back(std::move(flight));
        }
    }
}

void CellEdits::keepColumns(std::span<std::string const> columns) {
    auto const gone = [columns](std::string const& column) { return !std::ranges::contains(columns, column); };
    for (auto row = _cells.begin(); row != _cells.end();) {
        std::erase_if(row->second, [&gone](auto const& entry) { return gone(entry.first); });
        row = row->second.empty() ? _cells.erase(row) : std::next(row);
    }
    for (auto row = _errors.begin(); row != _errors.end();) {
        std::erase_if(row->second, [&gone](auto const& entry) { return gone(entry.first); });
        row = row->second.empty() ? _errors.erase(row) : std::next(row);
    }
}

std::size_t CellEdits::errorCount() const {
    std::size_t out = 0;
    for (auto const& [row, cells] : _errors) {
        out += cells.size();
    }
    return out;
}

std::size_t CellEdits::modifiedCount() const {
    std::size_t out = 0;
    for (auto const& [row, cells] : _cells) {
        out += static_cast<std::size_t>(
            std::ranges::count_if(cells, [](auto const& entry) { return entry.second.modified; }));
    }
    return out;
}

bool CellEdits::stale(RowId const& row, std::string_view column) const {
    if (auto const found = _queues.find(row);
        found != _queues.end() &&
        std::ranges::any_of(found->second, [&](EditRequest const& request) { return request.column == column; })) {
        return true;
    }
    auto const flights = _rowQueues.find(row);
    return flights != _rowQueues.end() && std::ranges::any_of(flights->second, [&](RowFlight const& flight) {
               return !flight.discard && std::ranges::any_of(flight.request.cells, [&](EditCell const& cell) {
                   return cell.column == column;
               });
           });
}

std::optional<std::string> CellEdits::error(RowId const& row, std::string_view column) const {
    auto const found = _errors.find(row);
    if (found == _errors.end()) {
        return std::nullopt;
    }
    auto const cell = found->second.find(column);
    return cell == found->second.end() ? std::nullopt : std::optional{cell->second};
}

std::size_t CellEdits::unsettled() const {
    std::size_t out = 0;
    for (auto const& [row, queue] : _queues) {
        out += queue.size();
    }
    return out;
}

bool CellEdits::unloadedInView(EditRequest const& request) const {
    if (_engine == nullptr) {
        return false;
    }
    auto const viewRow = _engine->viewRowOfKey(request.row);
    if (!viewRow) {
        return false;
    }
    auto const columns = _engine->columns();
    auto const column = std::ranges::find(columns, request.column, &ColumnInfo::id);
    if (column == columns.end()) {
        return false;
    }
    auto const index = static_cast<std::size_t>(column - columns.begin());
    return std::holds_alternative<Unloaded>(_engine->cellAt(*viewRow, index));
}

void CellEdits::start(RowId const& row) {
    if (_engine != nullptr) {
        _engine->beginEdit(row);
    }
    // A copy: a commit that settles at once pops the queue's front, and the
    // commit may still read its request afterwards.
    auto const request = _queues.at(row).front();
    // The commit runs last: it may settle, commit again or destroy this object.
    _commit(request, _alive.guard([this, row](std::expected<std::vector<Cell>, FieldError> outcome) {
        settle(row, std::move(outcome));
    }));
}

void CellEdits::settle(RowId const& row, std::expected<std::vector<Cell>, FieldError> outcome) {
    auto const found = _queues.find(row);
    if (found == _queues.end() || found->second.empty()) {
        return;
    }
    auto const request = std::move(found->second.front());
    found->second.pop_front();
    bool const more = !found->second.empty();
    if (!more) {
        _queues.erase(found);
        _ending.insert(row);
    }
    if (outcome) {
        if (auto const errors = _errors.find(row); errors != _errors.end()) {
            errors->second.erase(request.column);
        }
        auto const alive = _alive.token();
        _patcher->patchRow(row, std::move(*outcome));
        if (alive.expired()) {
            return;
        }
    } else {
        auto const column = outcome.error().column.empty() ? request.column : outcome.error().column;
        _errors[row].insert_or_assign(column, std::move(outcome.error().message));
    }
    if (more) {
        start(row);
        return;
    }
    _ending.erase(row);
    if (_queues.contains(row)) {
        // A commit made while the row was patched started its next edit.
        return;
    }
    if (_engine != nullptr) {
        _engine->endEdit(row);
    }
}

CellEdits::CellState const* CellEdits::cellOf(RowId const& row, std::string_view column) const {
    auto const found = _cells.find(row);
    if (found == _cells.end()) {
        return nullptr;
    }
    auto const cell = found->second.find(column);
    return cell == found->second.end() ? nullptr : &cell->second;
}

void CellEdits::eraseError(RowId const& row, std::string_view column) {
    if (auto const found = _errors.find(row); found != _errors.end()) {
        if (auto const cell = found->second.find(column); cell != found->second.end()) {
            found->second.erase(cell);
        }
        if (found->second.empty()) {
            _errors.erase(found);
        }
    }
}

void CellEdits::startRow(RowId const& row) {
    auto const found = _rowQueues.find(row);
    if (found == _rowQueues.end() || found->second.empty() || found->second.front().sent) {
        return;
    }
    RowFlight& front = found->second.front();
    front.sent = true;
    // A copy: a commit that settles at once pops the queue's front, and the commit may still read its request.
    RowEditRequest const request = front.request;
    _rowCommit(request,
               _alive.guard([this, flightId = front.id](std::expected<void, std::vector<FieldError>> outcome) {
                   settleRow(flightId, std::move(outcome));
               }));
}

void CellEdits::landErrors(RowEditRequest const& request, std::vector<FieldError> const& errors) {
    auto const nameOf = [](EditCell const& cell) -> std::string const& {
        return cell.member.empty() ? cell.column : cell.member;
    };
    std::map<std::string, std::string, std::less<>> landed;
    std::optional<std::string> general;
    for (FieldError const& error : errors) {
        auto const named = std::ranges::find_if(request.cells, [&](EditCell const& cell) {
            return !error.column.empty() && nameOf(cell) == error.column;
        });
        if (named != request.cells.end()) {
            landed.insert_or_assign(named->column, error.message);
        } else if (!general) {
            general = error.message;
        }
    }
    for (EditCell const& cell : request.cells) {
        if (auto const found = landed.find(cell.column); found != landed.end()) {
            _errors[request.row].insert_or_assign(cell.column, found->second);
        } else if (general) {
            _errors[request.row].insert_or_assign(cell.column, *general);
        } else {
            eraseError(request.row, cell.column);
        }
    }
}

void CellEdits::settleRow(std::uint64_t flightId, std::expected<void, std::vector<FieldError>> outcome) {
    auto queue = _rowQueues.begin();
    auto flight = std::deque<RowFlight>::iterator{};
    for (; queue != _rowQueues.end(); ++queue) {
        flight = std::ranges::find(queue->second, flightId, &RowFlight::id);
        if (flight != queue->second.end()) {
            break;
        }
    }
    if (queue == _rowQueues.end()) {
        return;
    }
    RowId const row = queue->first;
    RowFlight const settled = std::move(*flight);
    queue->second.erase(flight);
    if (queue->second.empty()) {
        _rowQueues.erase(queue);
    }
    if (!settled.discard) {
        if (outcome) {
            for (std::size_t at = 0; at < settled.request.cells.size(); ++at) {
                EditCell const& cell = settled.request.cells.at(at);
                eraseError(row, cell.column);
                CellState& state = _cells[row][cell.column];
                state.modified = true;
                state.settled =
                    state.draft.has_value() && at < settled.drafts.size() && state.draft == settled.drafts.at(at);
            }
        } else {
            RowEditRequest landing = settled.request;
            landing.row = row;
            landErrors(landing, outcome.error());
        }
    }
    auto const alive = _alive.token();
    startRow(row);
    if (alive.expired() || settled.discard || !_rowSettled) {
        return;
    }
    std::function<void(RowId const&, bool)> const heard = _rowSettled;
    heard(row, !outcome.has_value());
}

}  // namespace morph::table

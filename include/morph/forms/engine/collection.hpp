// SPDX-License-Identifier: Apache-2.0

#pragma once

/// @file forms/engine/collection.hpp
/// @brief Collections, headless: a `v-*` view document as a reactive list of rows, the table source the table
///        engine sorts and filters, row and collection actions, the confirm step and the row editor.
///
/// Specified in `docs/spec/forms/engine.md`, "Collections".

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <expected>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "../../attributes.hpp"
#include "../../core/completion.hpp"
#include "../../reactive/control.hpp"
#include "../../reactive/runtime.hpp"
#include "../../reactive/signal.hpp"
#include "../../table/data_source.hpp"
#include "../../util/rational.hpp"
#include "detail/text.hpp"
#include "field_model.hpp"
#include "form_session.hpp"
#include "rules.hpp"

namespace morph::forms {

/// @brief How a collection presents its row editor.
enum class CollectionKind : std::uint8_t {
    Collection,   ///< `v-kind: collection`: the editor is a dialog.
    MasterDetail  ///< `v-kind: master-detail`: the editor sits beside the table.
};

/// @brief Where an action's button sits.
enum class CollectionActionScope : std::uint8_t {
    Row,        ///< On every row; the body is built from the row's `bind` entries.
    Collection  ///< Once, beside the title; the body is `{}`.
};

/// @brief One `v-columns` entry.
struct CollectionColumn {
    /// @brief The row member the column reads.
    std::string field{};
    /// @brief The header: `label`, else `field`.
    std::string label{};
    /// @brief The column's table kind (`kind`, by `table::ColumnKind`'s JSON names); `Text` when absent.
    table::ColumnKind kind = table::ColumnKind::Text;
    /// @brief `v-hidden`: carried in the rows and the table source, not shown.
    bool hidden = false;
    /// @brief `x-decimalPlaces`: the places an exact value without its own `dp` is shown with.
    std::optional<std::uint32_t> decimalPlaces{};
    /// @brief The unit appended to an exact value (`ExtUnits.unitUnicode`, else `unitAscii`).
    std::string unit{};
};

/// @brief One `v-rowAction` or `v-actions` entry.
struct CollectionAction {
    /// @brief The action type executed.
    std::string action{};
    /// @brief The button label: `label`, else the action type.
    std::string label{};
    /// @brief Row or collection.
    CollectionActionScope scope = CollectionActionScope::Row;
    /// @brief `{actionField: rowField}` pairs, in document order.
    std::vector<std::pair<std::string, std::string>> bind{};
    /// @brief Whether the action waits for a confirmation.
    bool confirm = false;
};

/// @brief Finds an action's schema JSON by its type id (a registry, a map, a served catalog).
using SchemaLookup = std::function<std::optional<std::string>(std::string_view actionType)>;

/// @brief The immutable model of a `v-*` view document.
class CollectionModel {
public:
    /// @brief Reads a view document.
    /// @param viewJson The document, in the vocabulary `views::viewSchemaJson<V>()` emits.
    /// @return The model, or the error naming the malformed key.
    [[nodiscard]] static std::expected<CollectionModel, SchemaError> fromSchema(std::string_view viewJson) {
        using detail::engine::Json;
        auto const dom = detail::engine::parseJson(viewJson);
        if (!dom || !dom->is_object()) {
            return std::unexpected(SchemaError{"", "a view document must be a JSON object"});
        }
        CollectionModel model{};
        auto const* const kind = detail::engine::stringAt(*dom, "v-kind");
        if (kind != nullptr && *kind != "collection" && *kind != "master-detail") {
            return std::unexpected(SchemaError{"v-kind", R"("v-kind" must be "collection" or "master-detail")"});
        }
        model._kind =
            (kind != nullptr && *kind == "master-detail") ? CollectionKind::MasterDetail : CollectionKind::Collection;
        auto const* const query = detail::engine::stringAt(*dom, "v-query");
        if (query == nullptr) {
            return std::unexpected(SchemaError{"v-query", R"(a view document needs a "v-query")"});
        }
        model._query = *query;
        auto const* const title = detail::engine::stringAt(*dom, "v-title");
        model._title = title != nullptr ? *title : model._query;
        auto const* const rowKey = detail::engine::stringAt(*dom, "v-rowKey");
        model._rowKey = rowKey != nullptr ? *rowKey : std::string{"id"};
        if (Json const* const columns = detail::engine::member(*dom, "v-columns")) {
            if (!columns->is_array()) {
                return std::unexpected(SchemaError{"v-columns", R"("v-columns" must be an array)"});
            }
            for (Json const& entry : columns->get_array()) {
                auto column = columnOf(entry);
                if (!column) {
                    return std::unexpected(column.error());
                }
                model._columns.push_back(*std::move(column));
            }
        }
        if (Json const* const rowAction = detail::engine::member(*dom, "v-rowAction")) {
            auto action = actionOf(*rowAction, "v-rowAction");
            if (!action) {
                return std::unexpected(action.error());
            }
            model._rowAction = *std::move(action);
        }
        if (Json const* const actions = detail::engine::member(*dom, "v-actions")) {
            if (!actions->is_array()) {
                return std::unexpected(SchemaError{"v-actions", R"("v-actions" must be an array)"});
            }
            for (Json const& entry : actions->get_array()) {
                auto action = actionOf(entry, "v-actions");
                if (!action) {
                    return std::unexpected(action.error());
                }
                model._actions.push_back(*std::move(action));
            }
        }
        return model;
    }

    /// @brief How the row editor is presented.
    /// @return The kind.
    [[nodiscard]] CollectionKind kind() const noexcept { return _kind; }

    /// @brief `v-title`, or the query's type.
    /// @return The title.
    [[nodiscard]] std::string_view title() const noexcept { return _title; }

    /// @brief `v-query`: the action whose result rows are listed.
    /// @return The action type.
    [[nodiscard]] std::string_view query() const noexcept { return _query; }

    /// @brief `v-rowKey`: the row member that identifies a row (`id` by default).
    /// @return The member name.
    [[nodiscard]] std::string_view rowKey() const noexcept { return _rowKey; }

    /// @brief `v-columns`, hidden ones included.
    /// @return The columns.
    [[nodiscard]] std::span<CollectionColumn const> columns() const noexcept { return _columns; }

    /// @brief `v-rowAction`: the editor's action and its bind entries.
    /// @return The row action, or `std::nullopt`.
    [[nodiscard]] std::optional<CollectionAction> const& rowAction() const noexcept { return _rowAction; }

    /// @brief `v-actions`.
    /// @return The actions.
    [[nodiscard]] std::span<CollectionAction const> actions() const noexcept { return _actions; }

private:
    CollectionModel() = default;

    [[nodiscard]] static std::expected<CollectionColumn, SchemaError> columnOf(detail::engine::Json const& entry) {
        using detail::engine::Json;
        auto const* const field = detail::engine::stringAt(entry, "field");
        if (field == nullptr) {
            return std::unexpected(SchemaError{"v-columns", R"(a column needs a "field")"});
        }
        CollectionColumn column{};
        column.field = *field;
        auto const* const label = detail::engine::stringAt(entry, "label");
        column.label = label != nullptr ? *label : column.field;
        if (auto const* const kind = detail::engine::stringAt(entry, "kind")) {
            table::ColumnKind parsed{};
            if (glz::read_json(parsed, detail::engine::quote(*kind))) {
                return std::unexpected(
                    SchemaError{"v-columns." + column.field, "unknown column kind \"" + *kind + "\""});
            }
            column.kind = parsed;
        }
        Json const* const hidden = detail::engine::member(entry, "v-hidden");
        column.hidden = hidden != nullptr && hidden->is_boolean() && hidden->get<bool>();
        if (auto const places =
                detail::engine::SchemaReader::unsignedOf(detail::engine::member(entry, "x-decimalPlaces"))) {
            column.decimalPlaces =
                static_cast<std::uint32_t>(std::min<std::uint64_t>(*places, math::kMaxDecimalPlaces));
        }
        if (Json const* const units = detail::engine::member(entry, "ExtUnits")) {
            auto const* const unicode = detail::engine::stringAt(*units, "unitUnicode");
            auto const* const ascii = detail::engine::stringAt(*units, "unitAscii");
            column.unit =
                unicode != nullptr && !unicode->empty() ? *unicode : (ascii != nullptr ? *ascii : std::string{});
        }
        return column;
    }

    [[nodiscard]] static std::expected<CollectionAction, SchemaError> actionOf(detail::engine::Json const& node,
                                                                               std::string const& path) {
        auto const* const type = detail::engine::stringAt(node, "action");
        if (type == nullptr) {
            return std::unexpected(SchemaError{path, R"(an action needs an "action")"});
        }
        CollectionAction action{};
        action.action = *type;
        auto const* const label = detail::engine::stringAt(node, "label");
        action.label = label != nullptr ? *label : action.action;
        auto const* const scope = detail::engine::stringAt(node, "scope");
        action.scope = (scope != nullptr && *scope == "collection") ? CollectionActionScope::Collection
                                                                    : CollectionActionScope::Row;
        auto const* const confirm = detail::engine::member(node, "confirm");
        action.confirm = confirm != nullptr && confirm->is_boolean() && confirm->get<bool>();
        if (auto const* const bind = detail::engine::member(node, "bind"); bind != nullptr && bind->is_object()) {
            for (auto const& [field, rowField] : bind->get_object()) {
                if (auto const* const text = rowField.get_if<std::string>()) {
                    action.bind.emplace_back(field, *text);
                }
            }
        }
        return action;
    }

    CollectionKind _kind = CollectionKind::Collection;
    std::string _title;
    std::string _query;
    std::string _rowKey;
    std::vector<CollectionColumn> _columns;
    std::optional<CollectionAction> _rowAction;
    std::vector<CollectionAction> _actions;
};

/// @brief One listed row.
struct CollectionRow {
    /// @brief The row's identity for the table engine: an `int64` key, or its text when it is not one.
    table::RowId id{};
    /// @brief The row key as exact text (every digit kept): what `open`, `fire` and the views address a row by.
    std::string key{};
    /// @brief The row as JSON, every integer digit kept.
    std::string json{};
    /// @brief The visible columns' formatted cells, in column order.
    std::vector<std::string> cells{};
    /// @brief Member-wise equality.
    /// @param other The other row.
    /// @return `true` when every member matches.
    bool operator==(CollectionRow const& other) const = default;
};

/// @brief A confirm-guarded action waiting for an answer.
struct PendingConfirm {
    /// @brief Index into `CollectionModel::actions()`.
    std::size_t action = 0;
    /// @brief The row it was fired on (empty for a collection action).
    std::string rowKey{};
    /// @brief Member-wise equality.
    /// @param other The other pending action.
    /// @return `true` when action and row match.
    bool operator==(PendingConfirm const& other) const = default;
};

namespace detail::engine {

/// @brief A cell's display text: an exact `{num, den}` value at its `dp` (else the column's places) with the
///        column's unit, anything else as `displayText`, an absent or null member as empty.
/// @param row    The row.
/// @param column The column.
/// @return The text.
[[nodiscard]] inline std::string cellText(Json const& row, CollectionColumn const& column) {
    Json const* const value = member(row, column.field);
    if (value == nullptr || value->is_null()) {
        return {};
    }
    if (value->is_object()) {
        auto const num = SchemaReader::signedOf(member(*value, "num"));
        auto const den = SchemaReader::signedOf(member(*value, "den"));
        if (num && den && *den != 0) {
            auto const places = SchemaReader::unsignedOf(member(*value, "dp"));
            std::uint32_t const decimals =
                places ? static_cast<std::uint32_t>(*places) : column.decimalPlaces.value_or(0);
            std::string text = decimalText(
                math::Rational{math::Numerator{*num}, math::Denominator{*den}, math::DecimalPlaces{0}}, decimals);
            return column.unit.empty() ? text : text + " " + column.unit;
        }
    }
    return displayText(*value);
}

/// @brief A row member as a table cell: an integer as `int64` (its digits as text past int64), an exact value as a
///        `Rational`, a fraction as a `double` only in a `Number` column (its digits as text otherwise, for the
///        engine to parse by the column's kind), text, a bool, and absent or null as empty.
/// @param row    The row.
/// @param column The column.
/// @return The cell.
[[nodiscard]] inline table::Cell tableCell(Json const& row, CollectionColumn const& column) {
    Json const* const value = member(row, column.field);
    if (value == nullptr || value->is_null()) {
        return std::monostate{};
    }
    if (auto const* const flag = value->get_if<bool>()) {
        return *flag;
    }
    if (auto const* const text = value->get_if<std::string>()) {
        return *text;
    }
    if (auto const* const fraction = value->get_if<double>()) {
        if (column.kind == table::ColumnKind::Number) {
            return *fraction;
        }
        return numberText(*value).value_or(std::string{});
    }
    if (value->get_if<std::int64_t>() != nullptr || value->get_if<std::uint64_t>() != nullptr) {
        if (auto const whole = SchemaReader::signedOf(value)) {
            return *whole;
        }
        return numberText(*value).value_or(std::string{});
    }
    if (value->is_object()) {
        auto const num = SchemaReader::signedOf(member(*value, "num"));
        auto const den = SchemaReader::signedOf(member(*value, "den"));
        if (num && den && *den != 0) {
            auto const places = SchemaReader::unsignedOf(member(*value, "dp")).value_or(0);
            return math::Rational{math::Numerator{*num}, math::Denominator{*den},
                                  math::DecimalPlaces{static_cast<std::uint32_t>(
                                      std::min<std::uint64_t>(places, math::kMaxDecimalPlaces))}};
        }
    }
    return toJson(*value);
}

/// @brief The table columns of a collection: every `v-columns` entry, hidden ones included, by field and kind.
/// @param model The collection model.
/// @return The columns.
[[nodiscard]] inline std::vector<table::ColumnInfo> tableColumns(CollectionModel const& model) {
    std::vector<table::ColumnInfo> columns;
    columns.reserve(model.columns().size());
    for (CollectionColumn const& column : model.columns()) {
        columns.push_back(table::ColumnInfo{.id = column.field, .kind = column.kind, .comparator = {}, .format = {}});
    }
    return columns;
}

}  // namespace detail::engine

/// @brief The reactive state of one collection: the list, the table source, the actions, the confirm step and the
///        row editor. A controller in the sense of `docs/spec/reactive/control.md`; it draws nothing.
///
/// Non-copyable and non-movable: replies and the editor point into it. Destroy a view of it before the session.
class CollectionSession {
public:
    /// @param runtime The runtime. Borrowed: it must outlive the session.
    /// @param model   The view model.
    /// @param schemas Finds the row action's schema; an unknown or malformed one means the collection has no editor.
    /// @param submit  Executes the query and every action. Required.
    /// @param choices Fetches the editor's Choice options; may be empty.
    /// @param options The editor's session options.
    /// @throws std::invalid_argument when @p submit is empty.
    CollectionSession(reactive::Runtime& runtime MORPH_LIFETIMEBOUND, CollectionModel model,
                      SchemaLookup const& schemas, Submitter submit, ChoiceFetcher choices,
                      FormSessionOptions options = {})
        : _rt{&runtime},
          _model{std::move(model)},
          _submit{std::move(submit)},
          _source{std::make_shared<table::VectorSource>(detail::engine::tableColumns(_model))},
          _openRow{runtime, std::nullopt},
          _pending{runtime, std::nullopt},
          _list{runtime, [this](std::string const& body) { return _submit(_model.query(), body); },
                [] { return std::optional<std::string>{"{}"}; }},
          _rows{runtime, [this] { return parseRows(); }},
          _actions{
              runtime, [this](Call call) { return _submit(call.action, std::move(call.body)); },
              reactive::MutationOptions<Call>{
                  .concurrency = reactive::Concurrency::Exclusive, .serialKey = {}, .invalidates = {_list.link()}}} {
        if (!_submit) {
            throw std::invalid_argument{"CollectionSession: a Submitter is required"};
        }
        _syncSource = std::make_unique<reactive::Effect>(runtime, [this] { syncSource(); });
        if (_model.rowAction().has_value() && schemas) {
            if (auto const schema = schemas(_model.rowAction()->action)) {
                if (auto editorModel = FormModel::fromSchema(_model.rowAction()->action, *schema)) {
                    _editor = std::make_unique<FormSession>(runtime, *std::move(editorModel), _submit,
                                                            std::move(choices), std::move(options));
                    _editorWatch = std::make_unique<reactive::Effect>(runtime, [this] { watchEditor(); });
                }
            }
        }
    }

    ~CollectionSession() = default;
    CollectionSession(CollectionSession const&) = delete;
    CollectionSession& operator=(CollectionSession const&) = delete;
    CollectionSession(CollectionSession&&) = delete;
    CollectionSession& operator=(CollectionSession&&) = delete;

    /// @brief The view model.
    /// @return The model.
    [[nodiscard]] CollectionModel const& model() const noexcept { return _model; }

    /// @brief The listed rows, from the last successful query, in the order the reply gave them. Tracked.
    /// @return The rows.
    [[nodiscard]] std::vector<CollectionRow> const& rows() const { return _rows.get(); }

    /// @brief The rows as a table source, one column per `v-columns` entry: what a table engine sorts, filters and
    ///        selects over. Replaced (a `Reset`) on every list reply. Owner-affine, like every `DataSource`.
    /// @return The source.
    [[nodiscard]] std::shared_ptr<table::VectorSource> const& source() const noexcept { return _source; }

    /// @brief Whether the list is loading. Tracked.
    /// @return `true` while the query is in flight.
    [[nodiscard]] bool loading() const { return _list.pending(); }

    /// @brief The list's last failure. Tracked.
    /// @return The exception, or null.
    [[nodiscard]] std::exception_ptr listError() const { return _list.error(); }

    /// @brief Reloads the list.
    void refresh() { _list.refetch(); }

    /// @brief Fires `model().actions()[action]`, on @p rowKey for a row action. A confirm-guarded action waits in
    ///        `pendingConfirm()` instead. While another action is in flight the call does nothing (the actions are
    ///        one `Exclusive` mutation, and a view disables its buttons while `actionPending()`).
    /// @param action Index into `model().actions()`.
    /// @param rowKey The row's exact key text, for a row action.
    /// @throws std::out_of_range for an index past the actions.
    void fire(std::size_t action, std::string_view rowKey = {}) {
        CollectionAction const& entry = detail::engine::elementAt(_model.actions(), action);
        if (entry.confirm) {
            _pending.set(PendingConfirm{.action = action, .rowKey = std::string{rowKey}});
            return;
        }
        run(entry, rowKey);
    }

    /// @brief The action waiting for confirmation. Tracked.
    /// @return It, or `std::nullopt`.
    [[nodiscard]] std::optional<PendingConfirm> const& pendingConfirm() const { return _pending.get(); }

    /// @brief Runs the action waiting for confirmation, if any.
    void confirm() {
        std::optional<PendingConfirm> const pending = _pending.peek();
        _pending.set(std::nullopt);
        if (pending.has_value()) {
            run(detail::engine::elementAt(_model.actions(), pending->action), pending->rowKey);
        }
    }

    /// @brief Drops the action waiting for confirmation: nothing is sent.
    void cancelConfirm() { _pending.set(std::nullopt); }

    /// @brief Opens the editor on a row, prefilled from the row action's `bind` entries with the row's exact values.
    ///        A prefill replaces every field, so nothing typed for another row carries over. Programmatic, so
    ///        nothing is submitted until the user edits.
    /// @param rowKey The row's exact key text.
    void open(std::string_view rowKey) {
        if (!_editor) {
            return;
        }
        std::string const body = bindBody(*_model.rowAction(), rowKey);
        _rt->batch([&] {
            _openRow.set(std::string{rowKey});
            _editor->prefill(body);
        });
    }

    /// @brief Closes the editor without submitting.
    void close() { _openRow.set(std::nullopt); }

    /// @brief The row the editor is open on. Tracked.
    /// @return Its key text, or `std::nullopt`.
    [[nodiscard]] std::optional<std::string> const& openRow() const { return _openRow.get(); }

    /// @brief The row editor.
    /// @return The editor, or null when the view has no row action or its schema is unknown or malformed.
    [[nodiscard]] FormSession* editor() const noexcept { return _editor.get(); }

    /// @brief Whether an action is in flight. Tracked.
    /// @return `true` while one is.
    [[nodiscard]] bool actionPending() const { return _actions.pending(); }

    /// @brief The last action failure. Tracked.
    /// @return The exception, or null; cleared by the next success.
    [[nodiscard]] std::exception_ptr actionError() const { return _actions.error(); }

private:
    struct Call {
        std::string action;
        std::string body;
    };

    [[nodiscard]] std::vector<CollectionRow> parseRows() const {
        using detail::engine::Json;
        std::vector<CollectionRow> rows;
        std::optional<std::string> const& reply = _list.value();
        auto const dom = reply.has_value() ? detail::engine::parseJson(*reply) : std::nullopt;
        if (!dom) {
            return rows;
        }
        Json const* list = dom->is_array() ? &*dom : nullptr;
        if (list == nullptr && dom->is_object()) {
            for (auto const& [ignored, candidate] : dom->get_object()) {
                if (candidate.is_array()) {
                    list = &candidate;
                    break;
                }
            }
        }
        if (list == nullptr) {
            return rows;
        }
        for (Json const& row : list->get_array()) {
            CollectionRow entry{};
            Json const* const key = detail::engine::member(row, _model.rowKey());
            entry.key = key == nullptr ? std::to_string(rows.size()) : detail::engine::displayText(*key);
            auto const whole = key == nullptr ? std::optional<std::int64_t>{std::int64_t(rows.size())}
                                              : detail::engine::SchemaReader::signedOf(key);
            bool const integral = key == nullptr || key->get_if<double>() == nullptr;
            entry.id = whole && integral ? table::RowId{*whole} : table::RowId{entry.key};
            entry.json = detail::engine::toJson(row);
            for (CollectionColumn const& column : _model.columns()) {
                if (!column.hidden) {
                    entry.cells.push_back(detail::engine::cellText(row, column));
                }
            }
            rows.push_back(std::move(entry));
        }
        return rows;
    }

    void syncSource() {
        std::vector<CollectionRow> const& rows = _rows.get();
        std::vector<table::RowId> ids;
        std::vector<std::vector<table::Cell>> cells;
        ids.reserve(rows.size());
        cells.reserve(rows.size());
        for (CollectionRow const& row : rows) {
            ids.push_back(row.id);
            auto const dom = detail::engine::parseJson(row.json);
            std::vector<table::Cell> line;
            line.reserve(_model.columns().size());
            for (CollectionColumn const& column : _model.columns()) {
                line.push_back(dom ? detail::engine::tableCell(*dom, column) : table::Cell{});
            }
            cells.push_back(std::move(line));
        }
        _source->setRows(std::move(ids), std::move(cells));
    }

    [[nodiscard]] std::string bindBody(CollectionAction const& action, std::string_view rowKey) const {
        std::vector<CollectionRow> const& rows = _rows.peek();
        auto const found = std::ranges::find(rows, rowKey, &CollectionRow::key);
        auto const row = found == rows.end() ? std::nullopt : detail::engine::parseJson(found->json);
        std::string body = "{";
        bool first = true;
        for (auto const& [field, rowField] : action.bind) {
            detail::engine::Json const* const value = row ? detail::engine::member(*row, rowField) : nullptr;
            body += (first ? "" : ",") + detail::engine::quote(field) + ":" +
                    (value == nullptr ? std::string{"null"} : detail::engine::toJson(*value));
            first = false;
        }
        return body + "}";
    }

    void run(CollectionAction const& action, std::string_view rowKey) {
        std::string body =
            action.scope == CollectionActionScope::Collection ? std::string{"{}"} : bindBody(action, rowKey);
        _actions.run(Call{.action = action.action, .body = std::move(body)});
    }

    // Each success of the editor's submission ticks its count: close the editor and reload the list.
    void watchEditor() {
        std::uint64_t const successes = _editor->successCount();
        if (successes == _editorSuccesses) {
            return;
        }
        _editorSuccesses = successes;
        _rt->untracked([&] {
            _rt->batch([&] {
                _openRow.set(std::nullopt);
                _list.refetch();
            });
        });
    }

    // Declaration order is destruction order reversed: the Effects go first, then the editor, the computeds and
    // the query that they read.
    reactive::Runtime* _rt;
    CollectionModel _model;
    Submitter _submit;
    std::shared_ptr<table::VectorSource> _source;
    reactive::Signal<std::optional<std::string>> _openRow;
    reactive::Signal<std::optional<PendingConfirm>> _pending;
    reactive::Query<std::string, std::string> _list;
    reactive::Computed<std::vector<CollectionRow>> _rows;
    reactive::Mutation<Call, std::string> _actions;
    std::unique_ptr<FormSession> _editor;
    std::uint64_t _editorSuccesses = 0;
    std::unique_ptr<reactive::Effect> _syncSource;
    std::unique_ptr<reactive::Effect> _editorWatch;
};

}  // namespace morph::forms

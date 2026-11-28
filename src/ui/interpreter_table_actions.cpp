// SPDX-License-Identifier: Apache-2.0

// Tables over an action's rows and over a model.

#include "interpreter_grid_detail.hpp"

namespace morph::ui::detail {

namespace {

/// What a table over a typed list action is built from: its description, the columns whose kind its first reply
/// decides, the request member its query goes in, the records it watches, and what stops it being paged.
struct ActionTable {
    table::TableDescription description;
    std::vector<std::string> inferKinds;
    std::string queryMember;
    std::vector<std::string> records;
    /// Empty when the table can be paged.
    std::string problem;
    /// Empty when the node's `key`, if any, reads one row field.
    std::string keyProblem;
    /// What the view document's units say that is not taken: a factor of zero or less, or a unit two columns
    /// declare differently.
    std::vector<std::string> unitProblems;
};

/// One `v-columns` entry of a view document, as a table over the action reads it.
struct ViewColumn {
    std::string label;
    std::optional<table::ColumnKind> kind;
    bool hidden = false;
    /// The units the column's quantities are in: its `ExtUnits` and each `x-unitAlternatives` entry.
    std::vector<table::UnitInfo> units;
};

/// The units a `v-columns` entry declares: its `ExtUnits` (`unitAscii` the id, `unitUnicode` the symbol) as the
/// canonical unit, at `x-decimalPlaces`, and each `x-unitAlternatives` entry (`id`, `display`, `decimals`, and
/// `num`/`den`, its factor to the canonical unit, 1 when absent), all in one dimension named by the canonical unit's
/// id. An alternative whose factor is zero or less is left out and said in @p problems.
// NOLINTNEXTLINE(readability-function-cognitive-complexity): reads each `v-columns` entry's canonical unit and its alternatives into one dimension, in the order the schema lists them
std::vector<table::UnitInfo> viewUnitsOf(Json::object_t const& fields, std::vector<std::string>& problems) {
    std::vector<table::UnitInfo> out;
    auto const member = [](Json::object_t const& object, std::string_view name) -> Json const* {
        const auto* const found = object.find(name);
        return found == object.end() ? nullptr : &found->second;
    };
    auto const textOf = [&member](Json::object_t const& object, std::string_view name) -> std::string const* {
        Json const* const value = member(object, name);
        return value == nullptr ? nullptr : std::get_if<std::string>(&value->data);
    };
    auto const integerOf = [&member](Json::object_t const& object,
                                     std::string_view name) -> std::optional<std::int64_t> {
        Json const* const value = member(object, name);
        if (value == nullptr) {
            return std::nullopt;
        }
        if (auto const* const signedValue = std::get_if<std::int64_t>(&value->data)) {
            return *signedValue;
        }
        if (auto const* const unsignedValue = std::get_if<std::uint64_t>(&value->data);
            unsignedValue != nullptr && *unsignedValue <= static_cast<std::uint64_t>(INT64_MAX)) {
            return static_cast<std::int64_t>(*unsignedValue);
        }
        return std::nullopt;
    };
    auto const places = [](std::optional<std::int64_t> value) {
        return static_cast<std::uint32_t>(std::clamp<std::int64_t>(value.value_or(0), 0, math::kMaxDecimalPlaces));
    };
    Json const* const ext = member(fields, "ExtUnits");
    auto const* const extFields = ext != nullptr ? std::get_if<Json::object_t>(&ext->data) : nullptr;
    std::string const* const ascii = extFields != nullptr ? textOf(*extFields, "unitAscii") : nullptr;
    if (ascii == nullptr || ascii->empty()) {
        return out;
    }
    std::string const* const unicode = textOf(*extFields, "unitUnicode");
    out.push_back(table::UnitInfo{.id = *ascii,
                                  .symbol = unicode != nullptr && !unicode->empty() ? *unicode : *ascii,
                                  .dimension = *ascii,
                                  .toBase = math::Rational{1, math::DecimalPlaces{0}},
                                  .decimals = places(integerOf(fields, "x-decimalPlaces"))});
    Json const* const alternatives = member(fields, "x-unitAlternatives");
    auto const* const entries = alternatives != nullptr ? std::get_if<Json::array_t>(&alternatives->data) : nullptr;
    for (Json const& entry : entries != nullptr ? std::span<Json const>{*entries} : std::span<Json const>{}) {
        auto const* const alternative = std::get_if<Json::object_t>(&entry.data);
        std::string const* const columnId = alternative != nullptr ? textOf(*alternative, "id") : nullptr;
        if (columnId == nullptr || columnId->empty() || std::ranges::contains(out, *columnId, &table::UnitInfo::id)) {
            continue;
        }
        auto const num = integerOf(*alternative, "num").value_or(1);
        auto const den = integerOf(*alternative, "den").value_or(1);
        if (num <= 0 || den <= 0) {
            problems.push_back("the unit `" + *columnId + "` has a factor of zero or less; it is not listed");
            continue;
        }
        std::string const* const display = textOf(*alternative, "display");
        out.push_back(table::UnitInfo{.id = *columnId,
                                      .symbol = display != nullptr && !display->empty() ? *display : *columnId,
                                      .dimension = *ascii,
                                      .toBase = math::Rational{math::Numerator{num}, math::Denominator{den},
                                                               math::DecimalPlaces{math::kMaxDecimalPlaces}},
                                      .decimals = places(integerOf(*alternative, "decimals"))});
    }
    return out;
}

/// Lists one column's units beside the units listed already, so the order of the columns does not matter: a column
/// one of whose units is listed joins that unit's dimension, its factors scaled through that unit; any other starts
/// its own. A unit listed already with another dimension or factor is said in @p problems and kept as first listed.
void listViewUnits(std::span<table::UnitInfo const> column, std::vector<table::UnitInfo>& listed,
                   std::vector<std::string>& problems) {
    if (column.empty()) {
        return;
    }
    std::string dimension = column.front().dimension;
    math::Rational scale{1, math::DecimalPlaces{0}};
    for (table::UnitInfo const& unit : column) {
        auto const found = std::ranges::find(listed, unit.id, &table::UnitInfo::id);
        if (found != listed.end()) {
            dimension = found->dimension;
            scale = (found->toBase / unit.toBase).value_or(scale);
            break;
        }
    }
    for (table::UnitInfo unit : column) {
        unit.dimension = dimension;
        unit.toBase = unit.toBase * scale;
        auto const found = std::ranges::find(listed, unit.id, &table::UnitInfo::id);
        if (found == listed.end()) {
            listed.push_back(std::move(unit));
        } else if (found->dimension != unit.dimension || found->toBase != unit.toBase) {
            problems.push_back("the unit `" + unit.id + "` is declared differently by two columns; the first is kept");
        }
    }
}

/// The table a typed list action's node describes. The columns are the node's `columns` parts, else the view
/// document's `v-columns`; the key is the node's `key` when it reads one row field, else the view document's
/// `v-rowKey`, else `id`; a column's kind is the part's literal `kind`, else its `v-columns` entry's, else the first
/// reply's. A catalog carries no reply schema, so nothing else can say what the rows hold.
// NOLINTNEXTLINE(readability-function-cognitive-complexity): the schema, the view document, then the columns.
ActionTable actionTableOf(DocNode const& node, Catalog const& catalog) {
    ActionTable out;
    std::string const action{node.name("action")};
    if (catalog.action) {
        if (std::optional<ActionInfo> const info = catalog.action(action)) {
            for (ActionRecordInfo const& record : info->records) {
                out.records.push_back(record.record);
            }
        }
    }
    if (catalog.schema) {
        if (std::optional<std::string> const schema = catalog.schema(action)) {
            std::vector<std::string> const members = tableQueryMembers(*schema);
            if (members.size() == 1) {
                out.queryMember = members.front();
            }
        }
    }
    if (out.queryMember.empty()) {
        out.problem = "a table over an action needs a query member marked x-table";
        out.queryMember = "query";
    }
    std::vector<std::pair<std::string, ViewColumn>> viewColumns;
    std::string viewKey;
    std::optional<std::string> const view = catalog.view ? catalog.view(action) : std::nullopt;
    if (view) {
        auto const json = parseJson(*view);
        auto const* const object = json ? std::get_if<Json::object_t>(&json->data) : nullptr;
        if (object != nullptr) {
            if (const auto* const key = object->find("v-rowKey"); key != object->end()) {
                if (auto const* const text = std::get_if<std::string>(&key->second.data)) {
                    viewKey = *text;
                }
            }
            const auto* const columns = object->find("v-columns");
            auto const* const entries =
                columns != object->end() ? std::get_if<Json::array_t>(&columns->second.data) : nullptr;
            for (Json const& entry : entries != nullptr ? std::span<Json const>{*entries} : std::span<Json const>{}) {
                auto const* const fields = std::get_if<Json::object_t>(&entry.data);
                auto const text = [fields](std::string_view name) -> std::string const* {
                    const auto* const found = fields->find(name);
                    return found == fields->end() ? nullptr : std::get_if<std::string>(&found->second.data);
                };
                std::string const* const field = fields != nullptr ? text("field") : nullptr;
                if (field == nullptr) {
                    continue;
                }
                ViewColumn column;
                std::string const* const label = text("label");
                column.label = label != nullptr ? *label : *field;
                if (std::string const* const kind = text("kind")) {
                    table::ColumnKind parsed{};
                    if (!glz::read_json(parsed, "\"" + *kind + "\"")) {
                        column.kind = parsed;
                    }
                }
                const auto* const hidden = fields->find("v-hidden");
                column.hidden = hidden != fields->end() && std::get_if<bool>(&hidden->second.data) != nullptr &&
                                *std::get_if<bool>(&hidden->second.data);
                // A column of quantities with units is read as measures, each cell naming its unit.
                column.units = viewUnitsOf(*fields, out.unitProblems);
                if (!column.units.empty() && column.kind == table::ColumnKind::Quantity) {
                    column.kind = table::ColumnKind::Measure;
                }
                viewColumns.emplace_back(*field, std::move(column));
            }
        }
    }
    auto const viewColumn = [&viewColumns](std::string_view columnId) -> ViewColumn const* {
        auto const found =
            std::ranges::find(viewColumns, columnId, [](auto const& entry) -> auto const& { return entry.first; });
        return found == viewColumns.end() ? nullptr : &found->second;
    };
    auto const add = [&out](std::string columnId, std::string label, std::optional<table::ColumnKind> kind, bool shown,
                            ViewColumn const* described) {
        listViewUnits(described != nullptr ? std::span<table::UnitInfo const>{described->units}
                                           : std::span<table::UnitInfo const>{},
                      out.description.units, out.unitProblems);
        if (!kind) {
            out.inferKinds.push_back(columnId);
        }
        out.description.columns.push_back(
            table::TableColumn{.info = table::ColumnInfo{.id = std::move(columnId),
                                                         .kind = kind.value_or(table::ColumnKind::Text),
                                                         .comparator = {},
                                                         .format = {},
                                                         .level = {}},
                               .label = std::move(label),
                               .group = {},
                               .sortable = true,
                               .filterable = true,
                               .shown = shown});
    };
    if (auto const parts = node.partList("columns"); !parts.empty()) {
        for (DocNodePtr const& part : parts) {
            ViewColumn const* const described = viewColumn(part->id);
            std::optional<table::ColumnKind> kind = columnKindNamed(part->name("kind"));
            if (!kind && described != nullptr) {
                kind = described->kind;
            }
            add(part->id, described != nullptr ? described->label : part->id, kind, true, described);
        }
    } else if (view) {
        for (auto const& [columnId, column] : viewColumns) {
            add(columnId, column.label, column.kind, !column.hidden, &column);
        }
    } else if (out.problem.empty()) {
        out.problem = "a table over an action needs its columns, or a view document for the action";
    }
    ExprPtr const key = node.prop("key");
    if (key && key->kind == ExprKind::Row && key->fieldPath.size() == 1) {
        out.description.key = key->fieldPath.front();
    } else {
        out.description.key = viewKey.empty() ? std::string{"id"} : viewKey;
        if (key) {
            out.keyProblem = R"(the key of a table over an action is one row field, {"row": "<id>"}; `)" +
                             out.description.key + "` is used";
        }
    }
    out.description.id = action;
    return out;
}

/// @brief Keeps a model table's selection of the view true to its rows after they changed: a selection of another
///        filter than the rows hold ends, the description's new revision having dropped a filter included, and a
///        key of `except` a reply left out of the view is dropped from it.
/// @param core    The screen.
/// @param element The table's element.
/// @param model   The table.
void fitViewSelection(ScreenCore& core, ElementState& element, ModelTable& model) {
    std::optional<TableSelection> const& current = model.viewSelection.peek();
    if (!current || !current->view) {
        return;
    }
    if (model.window->filter() != current->view->filters) {
        core.runtime().batch([&] {
            model.viewSelection.set(std::nullopt);
            if (element.bound == nullptr) {
                core.session.asUser([&] {
                    core.session.noteWrite();
                    element.selection.set(Value{ValueList{}});
                });
            }
        });
        return;
    }
    TableSelection next = *current;
    auto const dropped =
        std::erase_if(next.except, [&model](table::RowId const& key) { return model.rows->leftView(key); });
    if (dropped > 0) {
        model.viewSelection.set(std::move(next));
    }
}

/// @brief A model table's ear on the session's runs: the rows a run works on are busy until it settles, the rows its
///        reply could not change are marked failed, a row it gave a new key keeps what this session held by its old
///        one, and a run refused because the rows of its selection of this table's view changed is offered again, on
///        the view as it is now, through the status line.
///
/// Holds the table and its element by reference: the table owns it and removes it before either goes. The screen's
/// scope is held weakly, so the observer keeps nothing of the screen alive.
// A row's key changed: the rows, the drafts and, while an editable table is mounted, its held cells and its focus
// move what they hold by it.
void moveRowKey(ModelTable& model, Key const& from, Key const& target) {
    if (std::shared_ptr<ModelGridSource> const grid = model.editGrid.lock()) {
        grid->moveKey(from, target);
        return;
    }
    model.rows->moveKey(from, target);
    if (model.edits != nullptr) {
        model.edits->moveRow(from, target);
    }
}

class TableRuns final : public RunObserver {
public:
    TableRuns(ScreenCore& core, ElementState& element, ModelTable& model)
        : _core{&core}, _element{&element}, _model{&model} {}

    // The rows the run's input names become busy, and their failed marks go. A run of an action that declares an
    // effect on the table's record defers the drop of selected keys that leave the view until it settles, and on a
    // table with `retainChanged` holds the refetches its change events cause.
    void started(RunStarted const& run) override {
        if (_model->rows == nullptr) {
            return;
        }
        markBusy(run);
        auto const& catalog = _core->session.options().catalog;
        std::optional<ActionInfo> const info = catalog.action ? catalog.action(run.action) : std::nullopt;
        if (!info || !info->effects) {
            return;
        }
        std::string const record = recordName();
        if (std::ranges::none_of(*info->effects, [&record](ActionEffect const& effect) {
                return effect.record && *effect.record == record;
            })) {
            return;
        }
        _changing.insert(run.run);
        if (_model->retainChanged) {
            _held.insert(run.run);
            _model->rows->holdRefetches(run.run);
        }
    }

    // A selected key that left the view is dropped from the selection and its failed mark goes; while a run that
    // changes the table is in flight the drop waits for it to settle, since its reply may name the key as a row's
    // old key, and its own change event can come before it.
    void rowsChanged() {
        if (_model->rows == nullptr || _model->viewSelection.peek()) {
            return;
        }
        std::vector<Key> left;
        for (Key const& key : selectedKeys()) {
            if (_model->rows->leftView(key)) {
                left.push_back(key);
            }
        }
        if (left.empty()) {
            return;
        }
        if (!_changing.empty()) {
            _deferred.insert(left.begin(), left.end());
            return;
        }
        drop(left);
    }

    // NOLINTNEXTLINE(readability-function-cognitive-complexity): handles a settled run: its refusal, field errors, failed rows and replies in the order the run reports them
    void settled(RunSettled const& run) override {
        bool const held = _held.erase(run.run) > 0;
        if (_model->rows != nullptr) {
            _model->rows->states().settle(run.run);
            std::vector<Key> moved;
            if (run.succeeded) {
                applyReply(run, moved);
            }
            if (held) {
                keepChanged(run);
            }
            if (_changing.erase(run.run) > 0 && _changing.empty()) {
                std::vector<Key> left;
                for (Key const& key : std::exchange(_deferred, {})) {
                    if (_model->rows->leftView(key) && !std::ranges::contains(moved, key)) {
                        left.push_back(key);
                    }
                }
                drop(left);
            }
            _model->marksRevision.set(_model->marksRevision.peek() + 1);
            if (held) {
                _model->rows->releaseRefetches(run.run);
            }
        }
        if (run.refusalCode != "morph.table.viewChanged" || _model->rows == nullptr) {
            return;
        }
        auto const& catalog = _core->session.options().catalog;
        std::optional<std::string> const schema = catalog.schema ? catalog.schema(run.action) : std::nullopt;
        ValueRecord const* const body = run.body.asRecord();
        if (!schema || body == nullptr) {
            return;
        }
        std::string const record = _model->modelType + "." + _model->id;
        for (TableInput const& input : tableInputsOf(*schema)) {
            if (input.kind != TableInputKind::Selection || input.record != record) {
                continue;
            }
            auto const member = body->find(input.member);
            Value const* const view = member == body->end() ? nullptr : member->second.field("view");
            if (view == nullptr || view->isNull()) {
                continue;
            }
            // Offered only by a table whose view is the one refused: another table of the record, or this one
            // under another filter now, would run the action over rows the user did not select for it.
            table::FilterSpec refused;
            Value const* const filters = view->field("filters");
            if (filters == nullptr || glz::read_json(refused, canonicalJson(*filters)) ||
                refused != _model->window->filter()) {
                continue;
            }
            auto taken = _model->rows->selectView();
            if (!taken) {
                continue;
            }
            TableSelection again{.keys = {}, .view = std::move(*taken), .except = {}};
            if (Value const* const except = member->second.field("except"); except != nullptr) {
                if (ValueList const* const keys = except->asList()) {
                    for (Value const& key : *keys) {
                        again.except.push_back(keyOf(key));
                    }
                }
            }
            auto const json = parseJson(selectionJsonOf(again));
            auto selection = json ? valueFromJson(*json) : std::expected<Value, std::string>{Value{}};
            if (!selection) {
                return;
            }
            ValueRecord nextBody = *body;
            nextBody.insert_or_assign(input.member, std::move(*selection));
            std::int64_t const rows = _model->rows->selectedCount(again).rows;
            std::string const runId = "rerun." + std::to_string(run.run);
            auto const rerun = [model = _model, again, rerun = run.rerun, nextBody = Value{std::move(nextBody)}] {
                model->viewSelection.set(again);
                rerun(nextBody);
            };
            _core->tableMessage(
                *_element,
                "The rows changed since you selected them: " +
                    render::formatCanonicalNumber(std::to_string(rows), _core->session.formatting().numeric) +
                    (rows == 1 ? " row now. Run on these?" : " rows now. Run on these?"),
                {{StatusEntry{.id = runId, .label = "Run on these"}, rerun}});
            return;
        }
    }

private:
    // Retains the rows the run's reply says it changed, all or none: past the bound none is, and the status line
    // says so.
    void keepChanged(RunSettled const& run) {
        if (!run.succeeded) {
            return;
        }
        auto const& catalog = _core->session.options().catalog;
        std::optional<ActionInfo> const info = catalog.action ? catalog.action(run.action) : std::nullopt;
        if (!info) {
            return;
        }
        ChangedRowsValue const changed = changedRowsOf(run.reply, *info, _model->modelType + "." + _model->id);
        std::vector<table::RowId> keys;
        keys.reserve(changed.rows.size());
        for (table::ChangedRow const& row : changed.rows) {
            keys.push_back(row.key);
        }
        if (keys.empty() || _model->rows->retain(keys) != RetainResult::OverBound) {
            return;
        }
        std::string const bound = render::formatCanonicalNumber(std::to_string(table::QueryLimits{}.maxRetained),
                                                                _core->session.formatting().numeric);
        _core->tableMessage(*_element, "The changed rows are not kept in view: a table keeps at most " + bound);
    }

    [[nodiscard]] std::string recordName() const { return _model->modelType + "." + _model->id; }

    // The selected keys, while the selection is keys.
    [[nodiscard]] std::vector<Key> selectedKeys() const {
        std::vector<Key> out;
        Value const selected = _core->runtime().untracked([&] { return tableSelection(*_element); });
        if (ValueList const* const items = selected.asList()) {
            for (Value const& item : *items) {
                out.push_back(keyOf(item));
            }
        }
        return out;
    }

    // Selects @p keys as the user's selection does.
    void select(std::vector<Key> const& keys) {
        if (_model->config.selectKeys) {
            _model->config.selectKeys(keys);
        }
    }

    // Drops keys that left the view from the selection and from the failed marks.
    void drop(std::vector<Key> const& left) {
        if (left.empty()) {
            return;
        }
        _model->rows->states().clearFailed(left);
        std::vector<Key> kept = selectedKeys();
        if (std::erase_if(kept, [&left](Key const& key) { return std::ranges::contains(left, key); }) > 0) {
            select(kept);
        }
        _model->marksRevision.set(_model->marksRevision.peek() + 1);
    }

    // The keys a member of a run's input names, as a key list or a row's key.
    [[nodiscard]] static std::vector<Key> keysIn(Value const* list) {
        std::vector<Key> out;
        if (ValueList const* const items = list == nullptr ? nullptr : list->asList()) {
            for (Value const& item : *items) {
                out.push_back(keyOf(item));
            }
        }
        return out;
    }

    // The rows a run's input names for this table become busy for it: a `RowSelection` member's keys, or every row
    // of its view but `except`, and a `RowRef` member's row. Their failed marks go.
    void markBusy(RunStarted const& run) {
        auto const& catalog = _core->session.options().catalog;
        std::optional<std::string> const schema = catalog.schema ? catalog.schema(run.action) : std::nullopt;
        ValueRecord const* const body = run.body.asRecord();
        if (!schema || body == nullptr) {
            return;
        }
        std::string const record = recordName();
        RowStates& states = _model->rows->states();
        bool marked = false;
        for (TableInput const& input : tableInputsOf(*schema)) {
            auto const member = body->find(input.member);
            if (input.record != record || member == body->end() || member->second.isNull()) {
                continue;
            }
            Value const& named = member->second;
            if (input.kind == TableInputKind::Row || input.kind == TableInputKind::CellEdit) {
                std::vector<Key> const one{keyOf(named)};
                states.clearFailed(one);
                states.busyKeys(run.run, one);
            } else if (Value const* const view = named.field("view"); view != nullptr && !view->isNull()) {
                std::vector<Key> except = keysIn(named.field("except"));
                std::int64_t const count =
                    _model->rows->selectedCount(TableSelection{.keys = {}, .view = ViewSelection{}, .except = except})
                        .rows;
                std::vector<Key> marks = states.failedKeys();
                std::erase_if(marks, [&except](Key const& key) { return std::ranges::contains(except, key); });
                states.clearFailed(marks);
                states.busyView(run.run, std::move(except), count);
            } else {
                std::vector<Key> keys = keysIn(named.field("keys"));
                states.clearFailed(keys);
                states.busyKeys(run.run, std::move(keys));
            }
            marked = true;
        }
        if (marked) {
            _model->marksRevision.set(_model->marksRevision.peek() + 1);
        }
    }

    // Whether this session holds anything by a key: the selection, the focus, a retained row, a busy or failed state,
    // or a drop waiting for the runs to settle.
    [[nodiscard]] bool holds(Key const& key) const {
        RowStates const& states = _model->rows->states();
        std::optional<TableSelection> const& view = _model->viewSelection.peek();
        bool const selected =
            view ? std::ranges::contains(view->except, key) : std::ranges::contains(selectedKeys(), key);
        return selected || (_model->config.focused && _model->config.focused->key == key) ||
               std::ranges::contains(_model->window->retained(), key) || states.isBusy(key) ||
               states.failure(key).has_value() || _deferred.contains(key) ||
               (_model->edits != nullptr && _model->edits->rowEdited(key));
    }

    // A reply's failed rows are marked, and each changed row with a `from` this session holds moves what it holds
    // from that key to the new one: the selection, the retained rows and the states. Every `from` goes to the mount,
    // which moves its focus and highlight by its own keys: focus the action's own event already moved off the old row
    // is held only there. @p moved gets each old key moved here.
    // NOLINTNEXTLINE(readability-function-cognitive-complexity): applies a reply: failed rows are marked, then each changed row with a `from` moves what the session holds; the second step reads the first's marks
    void applyReply(RunSettled const& run, std::vector<Key>& moved) {
        auto const& catalog = _core->session.options().catalog;
        std::optional<ActionInfo> const info = catalog.action ? catalog.action(run.action) : std::nullopt;
        if (!info) {
            return;
        }
        ChangedRowsValue const changed = changedRowsOf(run.reply, *info, recordName());
        _model->rows->states().fail(changed.failed);
        std::vector<std::pair<Key, Key>> moves;
        std::vector<std::pair<Key, Key>> told;
        for (table::ChangedRow const& row : changed.rows) {
            if (!row.from || *row.from == row.key) {
                continue;
            }
            told.emplace_back(*row.from, row.key);
            if (holds(*row.from)) {
                moves.emplace_back(*row.from, row.key);
            }
        }
        // The mount is told after the selection is rewritten: a handler reading both in one turn sees them agree.
        auto const tell = [this, &told] {
            if (!told.empty()) {
                _model->keyMoves.set(
                    KeyMoves{.generation = _model->keyMoves.peek().generation + 1, .moves = std::move(told)});
            }
        };
        if (moves.empty()) {
            tell();
            return;
        }
        std::optional<TableSelection> view = _model->viewSelection.peek();
        std::vector<Key> keys = view ? std::vector<Key>{} : selectedKeys();
        bool reselect = false;
        for (auto const& [from, target] : moves) {
            moved.push_back(from);
            _deferred.erase(from);
            moveRowKey(*_model, from, target);
            if (view) {
                // A key left out of the view's selection leaves its new key out; a selected one selects it.
                if (auto const out = std::ranges::find(view->except, from); out != view->except.end()) {
                    if (std::ranges::contains(view->except, target)) {
                        view->except.erase(out);
                    } else {
                        *out = target;
                    }
                } else {
                    std::erase(view->except, target);
                }
            } else if (auto const found = std::ranges::find(keys, from); found != keys.end()) {
                reselect = true;
                if (std::ranges::contains(keys, target)) {
                    keys.erase(found);
                } else {
                    *found = target;
                }
            }
        }
        if (view) {
            _model->viewSelection.set(std::move(view));
        } else if (reselect) {
            select(keys);
        }
        tell();
    }

    ScreenCore* _core;
    ElementState* _element;
    ModelTable* _model;
    // The runs holding the rows' refetches.
    std::set<std::uint64_t> _held;
    // The runs in flight of actions that change the table's record, and the selected keys that left the view while
    // one was: dropped when the last settles, unless a reply named them as a row's old key.
    std::set<std::uint64_t> _changing;
    std::set<Key> _deferred;
};

// The field errors a reply names in its reserved `errors` member: the first message of each entry, by the name the
// action's input gives the field, `value`, or a member of `values` without its prefix.
std::vector<table::FieldError> editErrorsOf(Value const& reply) {
    std::vector<table::FieldError> out;
    ValueRecord const* const errors = reply.field("errors") != nullptr ? reply.field("errors")->asRecord() : nullptr;
    if (errors == nullptr) {
        return out;
    }
    for (auto const& [path, entry] : *errors) {
        std::string message;
        if (std::string const* const text = entry.asString()) {
            message = *text;
        } else if (ValueList const* const list = entry.asList();
                   list != nullptr && !list->empty() && list->front().asString() != nullptr) {
            message = *list->front().asString();
        }
        if (message.empty()) {
            continue;
        }
        constexpr std::string_view kValues = "values.";
        std::string name = path.starts_with(kValues) ? path.substr(kValues.size()) : path;
        out.push_back(table::FieldError{.column = std::move(name), .message = std::move(message)});
    }
    return out;
}

// What a refused or failed call of an edit action says: its sentence.
std::string editFailureOf(std::exception_ptr const& error) {
    try {
        std::rethrow_exception(error);
    } catch (std::exception const& failed) {
        return failed.what();
    } catch (...) {
        return "The edit could not be saved";
    }
}

// Sends a row commit of a model's table through the table's alias, not as a run of the session, so no row is busy for
// it: a call per `CellEdit` cell, and one per `RowEdit` action with every cell of the request it edits. The commit
// settles once every call has: refused with each call's field errors, a failed row or a failure's sentence, else
// succeeded. A changed row with a `from` moves what the table holds by key. A success whose replies name the row as
// changed waits for the change event's refetch; one that names it nowhere changed nothing the window would fetch, so
// the row counts as fetched at once.
// NOLINTNEXTLINE(readability-function-cognitive-complexity): sends one row commit: a call per cell and per action, settled once every call has, merging each call's outcome in order
void sendRowEdit(ScreenCore& core, ModelTable& model, table::RowEditRequest const& request, table::RowEditDone done) {
    table::TableDescription const* const description = model.window->description();
    std::string const key = std::visit(
        [](auto const& value) -> std::string {
            if constexpr (std::is_same_v<std::decay_t<decltype(value)>, std::string>) {
                return glz::write_json(value).value_or("\"\"");
            } else {
                return std::to_string(value);
            }
        },
        request.row);
    struct Call {
        std::string action;
        std::string body;
        std::vector<std::string> names;
    };
    std::vector<Call> calls;
    std::vector<table::FieldError> refused;
    std::map<std::string, std::string, std::less<>> rowEdits;
    std::map<std::string, std::vector<std::string>, std::less<>> rowNames;
    for (table::EditCell const& cell : request.cells) {
        table::EditInfo const* edit = nullptr;
        if (description != nullptr) {
            auto const found = std::ranges::find_if(
                description->columns, [&](table::TableColumn const& column) { return column.info.id == cell.column; });
            edit = found != description->columns.end() && found->edit ? &*found->edit : nullptr;
        }
        if (edit == nullptr) {
            refused.push_back(table::FieldError{.column = cell.member.empty() ? cell.column : cell.member,
                                                .message = "This column is not edited here any more"});
        } else if (edit->member.empty()) {
            calls.push_back(Call{.action = edit->action,
                                 .body = R"({"key":)" + key + R"(,"value":)" + cell.json + "}",
                                 .names = {cell.member.empty() ? cell.column : cell.member}});
        } else {
            rowNames[edit->action].push_back(cell.member.empty() ? cell.column : cell.member);
            std::string& values = rowEdits[edit->action];
            values += (values.empty() ? "" : ",") + glz::write_json(edit->member).value_or("\"\"") + ":" + cell.json;
        }
    }
    for (auto const& [action, values] : rowEdits) {
        calls.push_back(Call{
            .action = action,
            // NOLINTNEXTLINE(performance-inefficient-string-concatenation): builds one request body from four pieces, once per call, on a path that waits on the network
            .body = R"({"key":)" + key + R"(,"values":{)" + values + "}}",
            .names = rowNames[action]});
    }
    if (!calls.empty() && !model.calls) {
        refused.push_back(table::FieldError{.column = {}, .message = "This table cannot save edits"});
    }
    if (calls.empty() || !model.calls) {
        done(refused.empty() ? std::expected<void, std::vector<table::FieldError>>{} : std::unexpected(refused));
        return;
    }
    struct Settling {
        std::size_t remaining = 0;
        std::vector<table::FieldError> errors;
        bool named = false;
        table::RowId row;
        table::RowEditDone done;
    };
    auto const settling = std::make_shared<Settling>(Settling{.remaining = calls.size(),
                                                              .errors = std::move(refused),
                                                              .named = false,
                                                              .row = request.row,
                                                              .done = std::move(done)});
    auto const finish = [&model, settling] {
        if (--settling->remaining > 0) {
            return;
        }
        table::RowEditDone const settle = std::move(settling->done);
        if (!settling->errors.empty()) {
            settle(std::unexpected(std::move(settling->errors)));
            return;
        }
        settle({});
        if (!settling->named && model.edits != nullptr) {
            model.edits->rowFetched(settling->row);
            if (std::shared_ptr<ModelGridSource> const grid = model.editGrid.lock()) {
                grid->touch();
            }
        }
    };
    std::string const record = model.modelType + "." + model.id;
    for (Call const& call : calls) {
        // A refusal that names no cell is this call's cells': a request of several calls settles as one, and another
        // call's cells may have been saved.
        auto const unnamed = [settling, names = calls.size() > 1 ? call.names : std::vector<std::string>{}](
                                 std::string const& message) {
            if (names.empty()) {
                settling->errors.push_back(table::FieldError{.column = {}, .message = message});
            }
            for (std::string const& name : names) {
                settling->errors.push_back(table::FieldError{.column = name, .message = message});
            }
        };
        model.calls(call.action, call.body)
            .then(model.scope,
                  [&core, &model, settling, finish, unnamed, record, action = call.action](std::string const& text) {
                      auto const json = parseJson(text);
                      Value const reply = json ? valueFromJson(*json).value_or(Value{}) : Value{};
                      std::vector<table::FieldError> errors = editErrorsOf(reply);
                      if (!errors.empty()) {
                          std::ranges::move(errors, std::back_inserter(settling->errors));
                          finish();
                          return;
                      }
                      Catalog const& catalog = core.session.options().catalog;
                      std::optional<ActionInfo> const info = catalog.action ? catalog.action(action) : std::nullopt;
                      if (info) {
                          ChangedRowsValue const changed = changedRowsOf(reply, *info, record);
                          for (table::FailedRow const& failed : changed.failed) {
                              if (failed.key == settling->row) {
                                  unnamed(failed.message);
                              }
                          }
                          std::vector<std::pair<Key, Key>> moves;
                          for (table::ChangedRow const& row : changed.rows) {
                              bool const moved = row.from && *row.from == settling->row && row.key != settling->row;
                              settling->named = settling->named || row.key == settling->row || moved;
                              if (moved) {
                                  moves.emplace_back(*row.from, row.key);
                              }
                          }
                          for (auto const& [from, target] : moves) {
                              moveRowKey(model, from, target);
                              settling->row = target;
                          }
                          if (!moves.empty()) {
                              model.keyMoves.set(KeyMoves{.generation = model.keyMoves.peek().generation + 1,
                                                          .moves = std::move(moves)});
                          }
                      }
                      finish();
                  })
            .onError(model.scope, [finish, unnamed](std::exception_ptr const& error) {
                unnamed(editFailureOf(error));
                finish();
            });
    }
}

}  // namespace

// NOLINTNEXTLINE(readability-function-cognitive-complexity): wires a model's table: rows, runs, states, selection and actions, each taking the config and element the previous stage set up
void ScreenCore::wireModelTable(ElementState& element) {
    DocNode const& node = *element.node;
    RtScope& scope = *element.scope;
    auto made = std::make_unique<ModelTable>(runtime());
    ModelTable& model = *made;
    element.modelTable = std::move(made);
    model.core = this;
    model.element = &element;
    modelTables.push_back(&element);
    NameRef const* const named = scope.find(node.name("model"));
    ModelAlias* const alias = named != nullptr && named->category == NameCategory::Model ? named->model : nullptr;
    if (alias == nullptr) {
        report(node.path, "the table's `model` names no model alias of this scope");
        return;
    }
    std::string const action{node.name("action")};
    model.id = action.empty() ? std::string{node.name("table")} : action;
    model.modelType = alias->decl->type;
    model.statusShown = node.flag("status", true);
    model.retainChanged = node.flag("retainChanged");
    TableConfig& config = model.config;
    config.element = &element;
    auto const [allowed, menus] = configurableOf(node);
    config.allowed = allowed;
    config.menus = menus;
    // The bound the server's `limits` set, which the description carries.
    config.maxSortKeys = [&model] {
        table::TableDescription const* const description = model.window->description();
        return description == nullptr ? std::size_t{8} : std::size_t{description->capabilities.maxSortKeys};
    };
    config.message = [this, &element](std::string text) { tableMessage(element, std::move(text)); };
    config.touch = [this, &element] { tableGesture(element); };
    config.save = [&model](table::StateDocument const& document, StateParts parts) {
        if (model.stateKey) {
            model.states->changed(*model.stateKey, document, parts);
        }
    };
    // The filters the rows hold; the user's go to the rows through `ObservedRows`, which marks them the user's.
    config.filters = [&model] {
        static_cast<void>(model.version.get());
        return model.filter;
    };
    config.setFilters = [this, &model, &element](table::FilterSpec const& filter) {
        if (auto const set = model.rows->setFilter(filter); !set) {
            tableMessage(element, "The filter is refused: " + set.error().message);
            return false;
        }
        model.filter = filter;
        model.heldFilter.reset();
        model.bump();
        return true;
    };
    config.rows = [&model]() -> TableRows* { return model.rows.get(); };
    config.declaredGroups = [&model] {
        table::TableDescription const* const description = model.window->description();
        return description == nullptr ? std::vector<table::ColumnGroupInfo>{} : description->groups;
    };
    config.description = [&model]() -> std::optional<table::TableDescription> {
        table::TableDescription const* const description = model.window->description();
        return description == nullptr ? std::nullopt : std::optional<table::TableDescription>{*description};
    };
    // A bound layout is controlled, as a bound sort is: any expression but a literal `null` binds it.
    if (ExprPtr const layout = node.prop("layout");
        layout && (layout->kind != ExprKind::Literal || !layout->literal.isNull())) {
        config.bound = &scope.nodes.computed([&scope, layout] { return evalIn(scope, nullptr, layout); });
    }
    std::string qualifier = _doc->id;
    bool persists = true;
    if (ExprPtr const persist = node.prop("persist"); persist && persist->kind == ExprKind::Literal) {
        if (bool const* const flag = persist->literal.asBool()) {
            persists = *flag;
        } else if (std::string const* const name = persist->literal.asString()) {
            qualifier = *name;
        }
    }
    model.states = session.tableStates();
    if (persists && model.states != nullptr) {
        model.stateKey = table::TableStateKey{.application = session.options().application,
                                              .model = model.modelType,
                                              .table = model.id,
                                              .qualifier = std::move(qualifier),
                                              .part = "state"};
    }
    // A model's table is described by its `describe` action; a typed list action by the client, from the node's
    // columns or the action's view document.
    std::optional<ActionTable> typed;
    if (action.empty()) {
        model.describeDecl = QueryDecl{.name = {},
                                       .path = node.path,
                                       .model = std::string{node.name("model")},
                                       .action = "Table." + model.id + ".describe",
                                       .evaluate = false,
                                       .body = nullptr,
                                       .when = nullptr,
                                       .debounce = 0,
                                       .refreshEvery = 0,
                                       .staleTime = 0};
        model.describe = std::make_unique<QueryRuntime>(model.describeDecl, scope, alias);
    } else {
        typed = actionTableOf(node, session.options().catalog);
        if (!typed->problem.empty()) {
            report(node.path, typed->problem);
        }
        if (!typed->keyProblem.empty()) {
            report(pointerAppend(node.path, "key"), typed->keyProblem);
        }
        for (std::string const& problem : typed->unitProblems) {
            report(node.path, problem);
        }
    }
    Value const body =
        node.prop("body") ? runtime().untracked([&] { return evalIn(scope, nullptr, node.prop("body")); }) : Value{};
    // The table owns these members of the query; a body that gives one has it replaced, which is said, since the
    // body is the query's fixed part and the author may have meant it to restrict the rows.
    if (ValueRecord const* const fixed = body.asRecord()) {
        if (auto const query = fixed->find("query"); query != fixed->end() && query->second.asRecord() != nullptr) {
            for (std::string_view const owned : {"sort", "filters", "page", "columns", "keys", "retry"}) {
                if (query->second.asRecord()->contains(owned)) {
                    report(pointerAppend(pointerAppend(pointerAppend(node.path, "body"), "query"), owned),
                           "`" + std::string{owned} + "` is the table's own; the body's is not used");
                }
            }
        }
    }
    model.waiting = model.stateKey.has_value();
    model.window = std::make_shared<WindowRows>(
        session, WindowRowsOptions{
                     .channel = channelOf(*alias, *this),
                     .table = typed ? std::string{} : model.id,
                     .action = action,
                     .queryMember = typed ? typed->queryMember : std::string{},
                     .inferKinds = typed ? typed->inferKinds : std::vector<std::string>{},
                     .records = typed ? typed->records : std::vector<std::string>{model.modelType + "." + model.id},
                     .body = body,
                     .pageSize = 200,
                     .maxPages = 8,
                     .maxKeys = 1000,
                     .maxColumns = 96,
                     .restoring = model.waiting});
    element.tableWindow = model.window;
    model.rows = std::make_shared<ObservedRows>(
        model.window, model.scope.token().guard([this, &model, &element] {
            model.bump();
            model.marksRevision.set(model.marksRevision.peek() + 1);
            fitViewSelection(*this, element, model);
            if (model.runs != nullptr) {
                // NOLINTNEXTLINE(cppcoreguidelines-pro-type-static-cast-downcast): `model.runs` is created as a `TableRuns` by this table's own wiring, so the downcast cannot fail
                static_cast<TableRuns&>(*model.runs).rowsChanged();
            }
        }),
        model.scope.token().guard([this, &element] { tableGesture(element); }),
        model.scope.token().guard([&model] { model.resets.set(model.resets.peek() + 1); }),
        model.scope.token().guard([this, &model, &element] {
            model.userFiltered = true;
            tableGesture(element);
        }));
    element.tableRows = model.rows;
    model.calls = aliasCalls(alias);
    model.edits = std::make_unique<table::CellEdits>(
        [this, &model](table::RowEditRequest const& request, table::RowEditDone done) {
            sendRowEdit(*this, model, request, std::move(done));
        });
    // A keyed refetch that found no row for a key: the row was deleted or left the filter, and its drafts go.
    model.window->onRowsAbsent(model.scope.token().guard([&model](std::span<table::RowId const> keys) {
        if (std::shared_ptr<ModelGridSource> const grid = model.editGrid.lock()) {
            grid->rowsAbsent(keys);
            return;
        }
        for (table::RowId const& key : keys) {
            model.edits->rowAbsent(key);
        }
    }));
    model.runs = std::make_unique<TableRuns>(*this, element, model);
    session.addRunObserver(model.runs.get());
    if (model.describe != nullptr) {
        model.window->onStaleDescription(model.scope.token().guard([&model] { model.describe->invalidate(); }));
    }
    // Heard while the first reply is read: the filter held meanwhile goes once that reply is taken.
    model.window->onInferred(model.scope.token().guard([this, &model, &element] {
        runtime().owner().post(model.scope.token().guard([this, &element] { inferredTable(element); }));
    }));
    // Called inside a binding, where no signal may be written: the misses of one turn are fetched by one task.
    model.onMissing = [this, &model](std::string_view field) {
        bool const first = model.missing.empty();
        model.missing.emplace(field);
        if (!first) {
            return;
        }
        // A field no column of the description has is dropped by the rows, which never name it to the server.
        runtime().owner().post(model.scope.token().guard([&model] {
            std::set<std::string, std::less<>> const missed = std::exchange(model.missing, {});
            model.rows->fetchAlso(std::vector<std::string>{missed.begin(), missed.end()});
        }));
    };
    // A literal sort is the default a stored one replaces; any other expression binds it.
    // NOLINTNEXTLINE(readability-simplify-boolean-expr): the literal-null test is kept as written so the comment above it reads as the condition
    if (ExprPtr const sort = node.prop("sort"); sort && !(sort->kind == ExprKind::Literal && sort->literal.isNull())) {
        if (sort->kind == ExprKind::Literal) {
            element.sort.set(sort->literal);
        } else {
            element.sortBound = &scope.nodes.computed([&scope, sort] { return evalIn(scope, nullptr, sort); });
        }
    }
    if (ExprPtr const selection = node.prop("selection");
        selection && (selection->kind != ExprKind::Literal || !selection->literal.isNull())) {
        element.bound = &scope.nodes.computed([&scope, selection] { return evalIn(scope, nullptr, selection); });
    }
    // A literal filter is the default a stored one replaces; an expression binds the filters.
    if (std::string_view const literal = node.name("filters"); !literal.empty()) {
        table::FilterSpec filter;
        if (!glz::read_json(filter, literal)) {
            if (model.window->inferring()) {
                model.heldFilter = std::move(filter);
            } else {
                model.filter = filter;
                std::ignore = (model.window->setFilter(std::move(filter)));
            }
        }
    } else if (ExprPtr const filters = node.prop("filters")) {
        model.filtersBound = true;
        config.filtersBound = true;
        scope.nodes.effect([this, &scope, &element, filters] {
            Value const value = evalIn(scope, nullptr, filters);
            runtime().untracked([&] {
                ModelTable& table = *element.modelTable;
                table::FilterSpec filter;
                if (!value.isNull()) {
                    std::string const text = canonicalJson(value);
                    if (auto const failed = glz::read_json(filter, text)) {
                        report(pointerAppend(element.node->path, "filters"),
                               "`filters` is not a filter: " + glz::format_error(failed, text));
                        return;
                    }
                }
                if (table.window->inferring()) {
                    table.heldFilter = std::move(filter);
                    table.heldStored = false;
                    return;
                }
                if (filter == table.filter) {
                    return;
                }
                if (auto const set = table.window->setFilter(filter); !set) {
                    report(pointerAppend(element.node->path, "filters"),
                           "the filter is refused: " + set.error().message);
                    return;
                }
                table.filter = std::move(filter);
            });
        });
    }
    if (model.describe != nullptr) {
        model.describe->start();
    }
    // The sort the table shows: the bound one, or the table's own, which a header click and a stored state set.
    scope.nodes.effect([this, &element] {
        Value const value = tableSort(element);
        runtime().untracked([&] {
            ModelTable& table = *element.modelTable;
            SortChain chain = sortChainOf(value);
            if (chain == table.sort) {
                return;
            }
            if (auto const set = table.window->setSort(chain); !set) {
                report(pointerAppend(element.node->path, "sort"), "the sort is refused: " + set.error().message);
                return;
            }
            table.sort = std::move(chain);
        });
    });
    if (typed) {
        model.window->setDescription(std::move(typed->description));
        if (table::TableDescription const* const now = model.window->description()) {
            model.keyColumn = now->key;
            for (table::TableColumn const& column : now->columns) {
                model.described.insert(column.info.id);
            }
        }
        modelLayoutColumns(element);
        model.bump();
    } else {
        scope.nodes.effect([this, &element] {
            Value const described = element.modelTable->describe->value();
            runtime().untracked([&] { describedTable(element, described); });
        });
    }
    // A highlight: each non-null key, once a description is held, is located in the view by a keyed fetch naming no
    // columns, which stops the one an earlier key still has in flight; a key the view lacks, or null, marks nothing.
    // A typed list action's description is held from here on.
    if (ExprPtr const highlight = node.prop("highlightKey")) {
        scope.nodes.effect([this, &scope, &element, highlight, typedTable = typed.has_value()] {
            Value const key = evalIn(scope, nullptr, highlight);
            bool const described = typedTable || !element.modelTable->revision.get().empty();
            // A new query (sort, filter) moves the row: located again, the mark cleared meanwhile.
            static_cast<void>(element.modelTable->resets.get());
            runtime().untracked([&] {
                ModelTable& table = *element.modelTable;
                if (key.isNull() || !described) {
                    table.window->stopLocating();
                    table.highlight.set(std::nullopt);
                    return;
                }
                Key const shown = keyOf(key);
                table.highlight.set(std::nullopt);
                table.window->locate(
                    std::visit([](auto const& value) { return table::RowId{value}; }, shown),
                    [&table, shown](std::optional<std::int64_t> index) {
                        table.highlight.set(index && *index >= 0
                                                ? std::optional<TableHighlight>{TableHighlight{
                                                      .key = shown, .row = static_cast<std::size_t>(*index)}}
                                                : std::nullopt);
                    });
            });
        });
    }
    if (!model.stateKey) {
        return;
    }
    // The first query waits for the stored state, at most `tableStateTimeout`; a state that comes later is applied
    // unless the user changed the table meanwhile.
    if (reactive::Scheduler* const scheduler = session.options().scheduler) {
        model.loadTimer = scheduler->after(session.options().tableStateTimeout,
                                           model.scope.token().guard([&model] { model.release(); }));
    }
    model.views = std::make_unique<TableViews>(*model.states, *model.stateKey, runtime().owner());
    model.config.views = model.views.get();
    loadStateAndViews(*model.states, *model.stateKey, *model.views, model.scope,
                      [this, &element](std::optional<std::string> const& json, std::string const& failure) {
                          storedTableState(element, json, failure);
                      });
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity): applies a stored table state to a model table: layout, sort, filter and groups each fit to the description and report their own refusal
void ScreenCore::storedTableState(ElementState& element, std::optional<std::string> const& json,
                                  std::string const& failure) {
    ModelTable& model = *element.modelTable;
    DocNode const& node = *element.node;
    bool const late = !model.waiting;
    if (!failure.empty()) {
        report(node.path, "the stored state of table `" + node.id + "` could not be loaded: " + failure);
        model.release();
        return;
    }
    std::optional<table::StateDocument> stored;
    if (json) {
        auto read = table::readState(*json);
        if (!read) {
            report(node.path, "the stored state of table `" + node.id + "` is not used: " + read.error());
            model.release();
            return;
        }
        stored = std::move(*read);
    }
    // The default view stands in for a state never stored, and its sort and filters for the stored ones when the
    // user starts from it.
    std::optional<table::StateDocument> start =
        model.views != nullptr ? stateAtMount(std::move(stored), model.views->own()) : std::move(stored);
    if (!start) {
        model.release();
        return;
    }
    if (late && model.touched) {
        return;
    }
    table::StateDocument document = std::move(*start);
    // Kinds still to be inferred are placeholders: the stored filters are fitted to the kinds the first reply gives.
    if (model.window->inferring()) {
        if (!model.filtersBound && document.filters) {
            model.heldFilter = std::exchange(document.filters, std::nullopt);
            model.heldStored = true;
        }
    }
    if (table::TableDescription const* const description = model.window->description()) {
        table::FitReport fitted;
        document = table::fitState(std::move(document), *description, fitted);
        for (table::FitDrop const& drop : fitted.dropped) {
            if (drop.what == table::FitDrop::What::Filter || drop.what == table::FitDrop::What::GroupFilter) {
                report(node.path,
                       "the stored filter on `" + drop.id + "` is not applied: the table has no such column");
            }
        }
    }
    // A table started from the default view, with nothing stored, holds it as its state: the next change saves on
    // top of it rather than over nothing.
    if (!json && model.states != nullptr && model.stateKey) {
        model.states->start(*model.stateKey, document);
    }
    if (element.sortBound == nullptr && document.sort) {
        if (model.window->setSort(*document.sort)) {
            model.sort = *document.sort;
            element.sort.set(sortValueOf(model.sort));
        }
    }
    if (!model.filtersBound && document.filters) {
        if (model.window->setFilter(*document.filters)) {
            model.filter = *document.filters;
            model.heldFilter.reset();
        }
    }
    // A bound layout is the document's: the stored one is neither applied nor saved over it.
    bool const arranged = document.layout && model.config.bound == nullptr;
    if (arranged) {
        model.config.held = std::move(document.layout);
        model.config.version.set(model.config.version.peek() + 1);
    }
    if (document.view) {
        model.config.savedView = std::move(document.view);
        model.config.viewShared = false;
    }
    tablePoints(element);
    if (late || !arranged) {
        model.release();
        return;
    }
    // A mounted table takes the new arrangement in a pass of its own, which tells the rows the columns it shows: the
    // first query goes after it, so it names those columns.
    runtime().owner().post(model.scope.token().guard([&model] { model.release(); }));
}

void ScreenCore::describedTable(ElementState& element, Value const& described) {
    ModelTable& model = *element.modelTable;
    if (described.isNull()) {
        return;
    }
    table::TableDescription const* const held = model.window->description();
    std::string const before = held != nullptr ? held->revision : std::string{};
    // The filters a new revision drops are said once, at the table; a dropped sort key is not.
    if (held != nullptr) {
        table::TableDescription next;
        std::string const text = canonicalJson(described);
        if (!glz::read<glz::opts{.error_on_unknown_keys = false}>(next, text) && next.revision != before) {
            table::FitReport fitted;
            static_cast<void>(table::fitState(table::StateDocument{.version = table::kStateVersion,
                                                                   .layout = {},
                                                                   .sort = model.sort,
                                                                   .filters = model.filter,
                                                                   .view = {},
                                                                   .carried = {}},
                                              next, fitted));
            for (table::FitDrop const& drop : fitted.dropped) {
                if (drop.what == table::FitDrop::What::Filter || drop.what == table::FitDrop::What::GroupFilter ||
                    drop.what == table::FitDrop::What::Unit) {
                    report(element.node->path, "the filter on `" + drop.id +
                                                   "` is dropped: the table's new description has no such column");
                }
            }
        }
    }
    model.window->setDescription(described);
    table::TableDescription const* const now = model.window->description();
    if (now == nullptr) {
        model.bump();
        return;
    }
    model.keyColumn = now->key;
    model.described.clear();
    for (table::TableColumn const& column : now->columns) {
        model.described.insert(column.info.id);
    }
    modelLayoutColumns(element);
    // The fitted sort and filters are what the rows hold now.
    table::FitReport ignored;
    table::StateDocument const fitted = table::fitState(table::StateDocument{.version = table::kStateVersion,
                                                                             .layout = {},
                                                                             .sort = model.sort,
                                                                             .filters = model.filter,
                                                                             .view = {},
                                                                             .carried = {}},
                                                        *now, ignored);
    model.sort = fitted.sort.value_or(SortChain{});
    model.filter = fitted.filters.value_or(table::FilterSpec{});
    if (element.sortBound == nullptr) {
        element.sort.set(sortValueOf(model.sort));
    }
    model.revision.set(now->revision);
    model.bump();
}

void ScreenCore::inferredTable(ElementState& element) {
    ModelTable& model = *element.modelTable;
    table::TableDescription const* const description = model.window->description();
    if (description == nullptr) {
        return;
    }
    // The held filter, unless the user filtered meanwhile: a bound filter is the document's and always wins; a
    // literal or a stored one gives way only to a filter the user set, since a stored state held here arrived in
    // time and a sort or a scroll since does not undo it. Without one, the filter the rows hold, which may have been
    // compiled against the placeholders, is fitted again.
    std::optional<table::FilterSpec> held = std::exchange(model.heldFilter, std::nullopt);
    bool const stored = std::exchange(model.heldStored, false);
    bool const yields = !model.filtersBound && model.userFiltered;
    if (held && yields) {
        held.reset();
    }
    table::FilterSpec filter = held ? *held : model.window->filter();
    bool const fromStore = held && stored;
    table::FitReport fitted;
    table::StateDocument const document = table::fitState(
        table::StateDocument{
            .version = table::kStateVersion, .layout = {}, .sort = {}, .filters = filter, .view = {}, .carried = {}},
        *description, fitted);
    for (table::FitDrop const& drop : fitted.dropped) {
        if (drop.what == table::FitDrop::What::Filter || drop.what == table::FitDrop::What::GroupFilter) {
            report(element.node->path,
                   std::string{fromStore ? "the stored filter" : "the filter"} + " on `" + drop.id +
                       "` is not applied: " +
                       (drop.carried ? "the table has no such column" : "it does not apply to the column's kind"));
        }
    }
    filter = document.filters.value_or(table::FilterSpec{});
    if (filter == model.window->filter()) {
        model.filter = std::move(filter);
        return;
    }
    if (auto const set = model.window->setFilter(filter); !set) {
        report(pointerAppend(element.node->path, "filters"), "the filter is refused: " + set.error().message);
        return;
    }
    model.filter = std::move(filter);
}

void ScreenCore::tableGesture(ElementState& element) {
    if (element.rowsTable != nullptr) {
        element.rowsTable->touched = true;
        tableMessage(element, {});
        return;
    }
    if (element.modelTable == nullptr) {
        return;
    }
    element.modelTable->touched = true;
    tableMessage(element, {});
}

void ScreenCore::rowsTableMessage(RowsTable& rows, std::string text) const {
    if (text.empty()) {
        rows.messageTimer.cancel();
        rows.message.set({});
        return;
    }
    if (rows.status == false) {
        if (host != nullptr) {
            static_cast<void>(host->notify(text, "warning"));
        }
        return;
    }
    rows.message.set(text);
    rows.messageTimer.cancel();
    if (reactive::Scheduler* const scheduler = session.options().scheduler) {
        rows.messageTimer =
            scheduler->after(kStatusMessageHold, rows.scope.token().guard([&rows, shown = std::move(text)] {
                if (rows.message.peek() == shown) {
                    rows.message.set({});
                }
            }));
    }
}

void ScreenCore::tableMessage(ElementState& table, std::string text) {
    if (RowsTable* const rows = table.rowsTable.get()) {
        rowsTableMessage(*rows, std::move(text));
        return;
    }
    if (GridTable* const grid = table.gridTable.get()) {
        grid->entryHandlers.clear();
        grid->entries.set({});
        grid->messageTimer.cancel();
        grid->message.set(text);
        if (reactive::Scheduler* const scheduler = session.options().scheduler;
            scheduler != nullptr && !text.empty()) {
            grid->messageTimer = scheduler->after(kStatusMessageHold, grid->scope.token().guard([grid, shown = text] {
                if (grid->message.peek() == shown) {
                    grid->message.set({});
                    grid->entries.set({});
                    grid->entryHandlers.clear();
                }
            }));
        }
        return;
    }
    ModelTable* const model = table.modelTable.get();
    if (model == nullptr) {
        return;
    }
    model->entryHandlers.clear();
    model->entries.set({});
    if (text.empty()) {
        model->messageTimer.cancel();
        model->message.set({});
        return;
    }
    if (!model->statusShown) {
        if (host != nullptr) {
            static_cast<void>(host->notify(text, "warning"));
        }
        return;
    }
    model->message.set(text);
    model->messageTimer.cancel();
    if (reactive::Scheduler* const scheduler = session.options().scheduler) {
        model->messageTimer =
            scheduler->after(kStatusMessageHold, model->scope.token().guard([model, shown = std::move(text)] {
                if (model->message.peek() == shown) {
                    model->message.set({});
                    model->entries.set({});
                    model->entryHandlers.clear();
                }
            }));
    }
}

void ScreenCore::tableMessage(ElementState& table, std::string text,
                              std::vector<std::pair<StatusEntry, std::function<void()>>> entries) {
    tableMessage(table, std::move(text));
    if (GridTable* const grid = table.gridTable.get()) {
        if (grid->message.peek().empty()) {
            return;
        }
        std::vector<StatusEntry> shown;
        for (auto& [entry, handler] : entries) {
            grid->entryHandlers.insert_or_assign(entry.id, std::move(handler));
            shown.push_back(std::move(entry));
        }
        grid->entries.set(std::move(shown));
        return;
    }
    ModelTable* const model = table.modelTable.get();
    if (model == nullptr || !model->statusShown || model->message.peek().empty()) {
        return;
    }
    std::vector<StatusEntry> shown;
    for (auto& [entry, handler] : entries) {
        model->entryHandlers.insert_or_assign(entry.id, std::move(handler));
        shown.push_back(std::move(entry));
    }
    model->entries.set(std::move(shown));
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity): derives each point of a table element in document order, each reading the element's config and engine
void ScreenCore::tablePoints(ElementState& table) {
    ModelTable const* const model = table.modelTable.get();
    // A table over `rows` that keeps a state shows its layout at a `columns` point and its saved view's name at a
    // `saved` one, as a model-backed table does.
    RowsTable const* const rows = model == nullptr && table.rowsTable != nullptr && table.rowsTable->stateKey &&
                                          table.node != nullptr && table.node->kind == "table"
                                      ? table.rowsTable.get()
                                      : nullptr;
    if ((model == nullptr && rows == nullptr) || table.node == nullptr) {
        return;
    }
    TableConfig const& config = model != nullptr ? model->config : rows->config;
    ValueRecord next = prefs.peek();
    bool changed = false;
    for (auto const& [name, point] : doc().customize) {
        // A point without `of` names no table, an id-less one included.
        if (point.of.empty() || point.of != table.node->id || (point.kind != "columns" && point.kind != "saved")) {
            continue;
        }
        Value value;
        if (point.kind == "saved") {
            value = config.savedView ? Value{*config.savedView} : Value{};
        } else if (config.held) {
            auto const json = parseJson(glz::write_json(*config.held).value_or("null"));
            auto read = json ? valueFromJson(*json) : std::expected<Value, std::string>{Value{}};
            value = read ? std::move(*read) : Value{};
        } else {
            continue;
        }
        if (auto const held = next.find(name); held == next.end() || held->second != value) {
            next.insert_or_assign(name, std::move(value));
            changed = true;
        }
    }
    if (changed) {
        prefs.set(std::move(next));
    }
}

std::optional<bool> ScreenCore::tablePoint(CustomizePoint const& point, Value const& value) {
    if (point.kind != "columns" && point.kind != "saved") {
        return std::nullopt;
    }
    // A point without `of` names no table. A model-backed table, and a table over `rows` that keeps a state, hold both
    // points' values in that state.
    if (point.of.empty()) {
        return std::nullopt;
    }
    auto const found = std::ranges::find_if(modelTables, [&point](ElementState const* element) {
        if (element->node == nullptr || element->node->id != point.of) {
            return false;
        }
        return element->modelTable != nullptr ||
               (element->rowsTable != nullptr && element->rowsTable->stateKey && element->node->kind == "table");
    });
    if (found == modelTables.end()) {
        return std::nullopt;
    }
    ModelTable* const model = (*found)->modelTable.get();
    RowsTable* const rows = model == nullptr ? (*found)->rowsTable.get() : nullptr;
    TableConfig& config = model != nullptr ? model->config : rows->config;
    table::StateDocument changed;
    StateParts part = StateParts::Layout;
    if (point.kind == "saved") {
        std::string const* const name = value.asString();
        if (name == nullptr && !value.isNull()) {
            return false;
        }
        config.savedView = name == nullptr ? std::nullopt : std::optional<std::string>{*name};
        config.viewShared = false;
        changed.view = config.savedView;
        part = StateParts::View;
    } else {
        table::ColumnLayoutValue layout;
        if (glz::read_json(layout, canonicalJson(value))) {
            return false;
        }
        config.held = std::move(layout);
        config.version.set(config.version.peek() + 1);
        changed.layout = config.held;
    }
    std::optional<table::TableStateKey> const& key = model != nullptr ? model->stateKey : rows->stateKey;
    TableStates* const states = model != nullptr ? model->states : rows->states;
    (model != nullptr ? model->touched : rows->touched) = true;
    if (key) {
        states->changed(*key, changed, part);
    }
    return true;
}

std::size_t ScreenCore::tableRowScopes(ElementState const& table) {
    return table.modelTable == nullptr ? 0 : table.modelTable->rowScopes.size();
}

void ScreenCore::tableSaveFailed(table::TableStateKey const& key, table::SaveResult const& result) {
    std::string const text = result.reason.empty() ? std::string{"The table's arrangement could not be saved"}
                                                   : "The table's arrangement could not be saved: " + result.reason;
    std::vector<ElementState*> tables;
    for (ElementState* const element : modelTables) {
        std::optional<table::TableStateKey> const* const held =
            element->modelTable != nullptr ? &element->modelTable->stateKey
            // NOLINTNEXTLINE(readability-avoid-nested-conditional-operator): a two-way fall-through over the element's table kinds, in the order they are wired
            : element->rowsTable != nullptr ? &element->rowsTable->stateKey
                                            : nullptr;
        if (held != nullptr && *held == key) {
            tables.push_back(element);
        }
    }
    for (ElementState* const table : tables) {
        tableMessage(*table, text);
    }
}

[[nodiscard]] std::vector<table::TableColumn const*> arrangedColumns(DocNode const& node,
                                                                     table::TableDescription const& description) {
    std::vector<std::string> const companions = table::companionColumns(description);
    std::vector<table::TableColumn const*> out;
    if (auto const parts = node.partList("columns"); !parts.empty()) {
        for (DocNodePtr const& part : parts) {
            auto const found =
                std::ranges::find(description.columns, part->id,
                                  [](table::TableColumn const& column) -> auto const& { return column.info.id; });
            if (found != description.columns.end() && !std::ranges::binary_search(companions, part->id)) {
                out.push_back(&*found);
            }
        }
        return out;
    }
    for (table::TableColumn const& column : description.columns) {
        if (!std::ranges::binary_search(companions, column.info.id)) {
            out.push_back(&column);
        }
    }
    return out;
}

}  // namespace morph::ui::detail

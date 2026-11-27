// SPDX-License-Identifier: Apache-2.0

// Collecting a document's elements and their state: errors, table views and input values.

#include "interpreter_grid_detail.hpp"

namespace morph::ui::detail {

[[nodiscard]] Value filtersValueOf(table::FilterSpec const& filters) {
    auto const json = parseJson(glz::write_json(filters).value_or("null"));
    auto read = json ? valueFromJson(*json) : std::expected<Value, std::string>{Value{}};
    ValueRecord const* const whole = read ? read->asRecord() : nullptr;
    if (whole == nullptr) {
        return Value{};
    }
    ValueRecord out = *whole;
    ValueList groups;
    if (auto const found = whole->find("groups"); found != whole->end() && found->second.asList() != nullptr) {
        for (Value const& group : *found->second.asList()) {
            ValueRecord const* const fields = group.asRecord();
            if (fields == nullptr) {
                continue;
            }
            ValueRecord kept = *fields;
            if (auto const columnId = kept.find("group");
                columnId != kept.end() && columnId->second == Value{std::string{}}) {
                kept.erase(columnId);
            }
            if (auto const columns = kept.find("columns");
                columns != kept.end() && columns->second.asList() != nullptr && columns->second.asList()->empty()) {
                kept.erase(columns);
            }
            groups.emplace_back(std::move(kept));
        }
    }
    out.insert_or_assign("groups", Value{std::move(groups)});
    return Value{std::move(out)};
}

namespace {

/// A table's `layout`, `visibleColumns` or `hiddenCount` element value. Tracked.
[[nodiscard]] Value layoutElementValue(TableConfig const& config, std::string_view name) {
    table::ColumnLayout const layout = layoutOver(config.columns, config.declaredHidden, layoutNow(config));
    if (name == "layout") {
        return layoutValueOf(layout.value());
    }
    std::vector<std::string> const visible = layout.visible();
    if (name == "hiddenCount") {
        return static_cast<std::int64_t>(layout.order().size() - visible.size());
    }
    ValueList ids;
    for (std::string const& columnId : visible) {
        ids.emplace_back(columnId);
    }
    return Value{std::move(ids)};
}

}  // namespace

// --- Declaring scopes ------------------------------------------------------------------------------------------

[[nodiscard]] ElementState* elementFor(RtScope const& scope, DocNode const& node) {
    // A scope that declares nothing (a locale scope and the scope of its mounted content) leaves its children's
    // elements to the enclosing scope, where `collect` registered them, so the lookup continues there.
    for (RtScope const* at = &scope; at != nullptr; at = at->transparent ? at->parent.get() : nullptr) {
        for (auto const& element : at->elements) {
            if (element->node == &node) {
                return element.get();
            }
        }
    }
    return nullptr;
}

namespace {

/// @brief The transfers of the `upload` commands in a list, nested lists included.
/// @param commands The list.
/// @param out      Where the transfer ids go.
// NOLINTNEXTLINE(misc-no-recursion): one level per nested command list
void transfersOf(Commands const& commands, std::vector<std::string>& out) {
    for (Command const& command : commands) {
        if (command.kind == CommandKind::Upload && !command.transfer.empty()) {
            out.push_back(command.transfer);
        }
        transfersOf(command.commands, out);
        transfersOf(command.alternative, out);
    }
}

}  // namespace

// NOLINTBEGIN(misc-no-recursion): a document is a tree of nodes and parts, and collecting descends it.
// NOLINTNEXTLINE(readability-function-cognitive-complexity): one case per kind of node and part.
void ScreenCore::collect(DocNode const& node, RtScope& scope, std::vector<Commands const*>& commands) {
    KindSpec const* const spec = findKind(node.kind, false);
    if (!node.id.empty() || statefulKind(node.kind)) {
        auto element = std::make_unique<ElementState>(runtime());
        element->node = &node;
        element->scope = &scope;
        if (!node.id.empty()) {
            NameRef name;
            name.category = NameCategory::Element;
            name.element = element.get();
            scope.names.insert_or_assign(node.id, name);
        }
        wireErrors(*element);
        scope.elements.push_back(std::move(element));
    }
    bool const createsScope = spec != nullptr && (spec->flags & kScope) != 0;
    for (auto const& [key, list] : node.events) {
        KeySpec const* const keyed = spec == nullptr ? nullptr : keySpec(*spec, key);
        if (!createsScope || keyed == nullptr || (keyed->flags & kInner) == 0) {
            commands.push_back(&list);
        }
    }
    for (auto const& [chord, list] : node.keys) {
        static_cast<void>(chord);
        commands.push_back(&list);
    }
    for (auto const& [key, nodes] : node.children) {
        KeySpec const* const keyed = spec == nullptr ? nullptr : keySpec(*spec, key);
        if (createsScope && keyed != nullptr && (keyed->flags & kInner) != 0) {
            continue;
        }
        for (DocNodePtr const& child : nodes) {
            collect(*child, scope, commands);
        }
    }
    for (auto const& [key, parts] : node.parts) {
        static_cast<void>(key);
        for (DocNodePtr const& part : parts) {
            KindSpec const* const partSpec = findKind(part->kind, true);
            if (partSpec != nullptr && (partSpec->flags & kScope) != 0) {
                continue;
            }
            for (auto const& [event, list] : part->events) {
                static_cast<void>(event);
                commands.push_back(&list);
            }
            for (auto const& [childKey, nodes] : part->children) {
                KeySpec const* const keyed = partSpec == nullptr ? nullptr : keySpec(*partSpec, childKey);
                if (keyed != nullptr && (keyed->flags & kInner) != 0) {
                    continue;
                }
                for (DocNodePtr const& child : nodes) {
                    collect(*child, scope, commands);
                }
            }
        }
    }
}

// NOLINTEND(misc-no-recursion)

// NOLINTNEXTLINE(readability-function-cognitive-complexity): one section per kind of declaration.
void ScreenCore::declare(RtScope& scope, ScopeDecl const* decl, std::vector<DocNode const*> const& content,
                         std::vector<Commands const*> const& commands) {
    std::vector<Commands const*> lists = commands;
    for (DocNode const* const node : content) {
        collect(*node, scope, lists);
    }
    std::vector<std::string> transferIds;
    for (Commands const* const list : lists) {
        transfersOf(*list, transferIds);
    }
    for (std::string const& transferId : transferIds) {
        if (scope.names.contains(transferId)) {
            continue;
        }
        scope.transfers.push_back(std::make_unique<TransferState>(runtime()));
        NameRef name;
        name.category = NameCategory::Transfer;
        name.transfer = scope.transfers.back().get();
        scope.names.insert_or_assign(transferId, name);
    }
    if (decl != nullptr) {
        for (ModelDecl const& model : decl->models) {
            auto alias = std::make_unique<ModelAlias>();
            alias->decl = &model;
            alias->mode = model.instance;
            if (alias->mode == InstanceMode::Default) {
                std::optional<ModelInfo> const info =
                    session.options().catalog.model ? session.options().catalog.model(model.type) : std::nullopt;
                alias->mode = info && info->stateless ? InstanceMode::Shared : InstanceMode::Private;
            }
            ModelAlias* const raw = alias.get();
            switch (raw->mode) {
                case InstanceMode::Keyed:
                    raw->token = &scope.nodes.computed([&scope, raw] -> std::string {
                        std::string const key = instanceKeyText(evalIn(scope, nullptr, raw->decl->key));
                        return key.empty() ? std::string{} : "shared:" + key;
                    });
                    break;
                case InstanceMode::Shared:
                    raw->token = &scope.nodes.computed([] { return std::string{"shared"}; });
                    break;
                case InstanceMode::Borrowed: {
                    auto const found = borrowed.find(model.name);
                    std::string token = found == borrowed.end() ? std::string{} : found->second.token;
                    raw->token = &scope.nodes.computed([token = std::move(token)] { return token; });
                    break;
                }
                case InstanceMode::Private:
                case InstanceMode::Default:
                default: {
                    std::string token = session.privateToken();
                    raw->token = &scope.nodes.computed([token = std::move(token)] { return token; });
                    break;
                }
            }
            NameRef name;
            name.category = NameCategory::Model;
            name.model = raw;
            scope.names.insert_or_assign(model.name, name);
            scope.models.push_back(std::move(alias));
        }
        for (LetDecl const& let : decl->lets) {
            NameRef name;
            name.category = NameCategory::Let;
            name.letExpr = let.value;
            name.let = &scope.nodes.computed([&scope, expr = let.value] { return evalIn(scope, nullptr, expr); });
            scope.names.insert_or_assign(let.name, name);
        }
        for (StateDecl const& state : decl->state) {
            auto slot = std::make_unique<StateSlot>();
            slot->decl = &state;
            slot->scope = &scope;
            NameRef name;
            name.category = NameCategory::State;
            name.state = slot.get();
            scope.names.insert_or_assign(state.name, name);
            scope.states.push_back(std::move(slot));
        }
        auto const aliasOf = [&scope](std::string const& model) -> ModelAlias* {
            NameRef const* const found = scope.find(model);
            return found != nullptr && found->category == NameCategory::Model ? found->model : nullptr;
        };
        for (QueryDecl const& query : decl->queries) {
            scope.queries.push_back(std::make_unique<QueryRuntime>(query, scope, aliasOf(query.model)));
            NameRef name;
            name.category = NameCategory::Query;
            name.query = scope.queries.back().get();
            scope.names.insert_or_assign(query.name, name);
        }
        for (MutationDecl const& mutation : decl->mutations) {
            scope.mutations.push_back(std::make_unique<MutationRuntime>(mutation, scope, aliasOf(mutation.model)));
            NameRef name;
            name.category = NameCategory::Mutation;
            name.mutation = scope.mutations.back().get();
            scope.names.insert_or_assign(mutation.name, name);
        }
    }
    for (auto const& slot : scope.states) {
        initState(scope, *slot);
    }
    for (auto const& element : scope.elements) {
        wireElement(*element);
    }
    if (decl == nullptr) {
        return;
    }
    for (CheckDecl const& check : decl->checks) {
        auto made = std::make_unique<CheckRuntime>();
        made->decl = &check;
        made->failing = &scope.nodes.computed([&scope, expr = check.assertion] {
            Value const asserted = evalIn(scope, nullptr, expr);
            bool const* const flag = asserted.asBool();
            return flag != nullptr && !*flag;
        });
        made->message = &scope.nodes.computed(
            [&scope, expr = check.message] { return textOf(evalIn(scope, nullptr, expr), scope.formatting()); });
        for (std::string const& target : check.targets) {
            NameRef const* const found = scope.find(target);
            if (found != nullptr && found->category == NameCategory::Element) {
                found->element->checks.push_back(made.get());
                made->targets.push_back(found->element);
                found->element->checksVersion.set(found->element->checksVersion.peek() + 1);
            }
        }
        scope.checks.push_back(std::move(made));
    }
    for (auto const& query : scope.queries) {
        query->start();
    }
    for (WatchDecl const& watch : decl->watches) {
        auto& source = scope.nodes.computed([&scope, expr = watch.on] { return evalIn(scope, nullptr, expr); });
        scope.nodes.effect([this, &scope, &source, watched = &watch, first = true] mutable {
            static_cast<void>(source.get());
            if (first) {
                first = false;
                return;
            }
            bool const user = session.lastWriteByUser();
            if (watched->source == WatchSource::User && !user) {
                return;
            }
            runtime().untracked([&] {
                CommandCtx const ctx{
                    .scope = scope.shared_from_this(),
                    .bindings = std::make_shared<Bindings>(),
                    .cancelClose = std::make_shared<bool>(false),
                    .boundary = nullptr,
                };
                if (user) {
                    session.asUser([&] { runList(watched->body, ctx, {}); });
                } else {
                    runList(watched->body, ctx, {});
                }
            });
        });
    }
}

[[nodiscard]] DateReading dateInstantOf(Value const& value, std::int32_t offsetMinutes) {
    DateValue const reading = readDateValue(value);
    switch (reading.form) {
        case DateForm::Absent:
            return {};
        case DateForm::Date:
            return {
                .instant = time::DateTime{std::chrono::sys_days{reading.date} - std::chrono::minutes{offsetMinutes}},
            };
        case DateForm::Timestamp:
        case DateForm::InstantText:
            return {.instant = reading.instant};
        case DateForm::Unreadable:
        default:
            return {.instant = std::nullopt, .readable = false};
    }
}

[[nodiscard]] std::string dateTextOf(time::DateTime const& instant, std::int32_t offsetMinutes) {
    auto const local = instant.value + std::chrono::minutes{offsetMinutes};
    std::chrono::year_month_day const date{std::chrono::floor<std::chrono::days>(local)};
    return std::format("{:04}-{:02}-{:02}", static_cast<int>(date.year()), static_cast<unsigned>(date.month()),
                       static_cast<unsigned>(date.day()));
}

[[nodiscard]] int placesOf(Value const& value, FormatContext const& format) {
    static_cast<void>(format);  // Only to fit `bindProp`'s converter signature.
    std::int64_t const* const whole = value.asInt();
    if (whole == nullptr || *whole < 0) {
        return -1;
    }
    return static_cast<int>(std::min<std::int64_t>(*whole, math::kMaxDecimalPlaces));
}

namespace {

/// @brief The fraction digits of a number as typed in the active locale.
/// @param text   The typed text.
/// @param format The formatting facts.
/// @return The digits after the decimal separator; 0 for a text that is not a number, which no places rule judges.
[[nodiscard]] std::size_t fractionDigits(std::string_view text, FormatContext const& format) {
    std::optional<std::string> const canonical = render::normalizeLocaleNumber(text, withoutGrouping(format.numeric));
    if (!canonical.has_value()) {
        return 0;
    }
    auto const dot = canonical->find('.');
    return dot == std::string::npos ? 0 : canonical->size() - dot - 1;
}

/// @brief Whether two values show the same in an input: by text for a text input, by instant for a date input, by
///        value otherwise.
/// @param kind   The input's kind.
/// @param lhs    One value.
/// @param rhs    The other.
/// @param format The formatting facts.
/// @return It.
[[nodiscard]] bool sameShown(std::string_view kind, Value const& lhs, Value const& rhs, FormatContext const& format) {
    if (kind == "textInput" || kind == "filePicker") {
        return textOf(lhs, format) == textOf(rhs, format);
    }
    if (kind == "dateTimeInput") {
        return dateInstantOf(lhs, format.utcOffsetMinutes).instant ==
               dateInstantOf(rhs, format.utcOffsetMinutes).instant;
    }
    return lhs == rhs;
}

}  // namespace

[[nodiscard]] Value optionKey(Value const& option) {
    if (option.asRecord() == nullptr) {
        return option;
    }
    for (std::string_view const field : {"key", "id", "value"}) {
        if (Value const* const found = option.field(field)) {
            return *found;
        }
    }
    return Value{};
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity): one rule per source of an error.
void ScreenCore::wireErrors(ElementState& element) {
    RtScope& scope = *element.scope;
    element.errors = &scope.nodes.computed([this, &element] -> Value {
        static_cast<void>(element.checksVersion.get());
        DocNode const& field = *element.node;
        RtScope const& owner = *element.scope;
        std::vector<std::string> errors;
        // A group's `required` is its legend's mark only: a group has no value, so no `Required` is derived from it.
        if (ExprPtr const required = field.kind != "group" ? field.prop("required") : nullptr) {
            Value const requiredNow = evalIn(owner, nullptr, required);
            bool const* const flag = requiredNow.asBool();
            if (flag != nullptr && *flag && !engaged(inputValue(element))) {
                errors.emplace_back("Required");
            }
        }
        for (std::string& text : stringsOf(evalIn(owner, nullptr, field.prop("errors")))) {
            errors.push_back(std::move(text));
        }
        if (ExprPtr const places = field.kind == "textInput" ? field.prop("decimals") : nullptr) {
            int const allowed = placesOf(evalIn(owner, nullptr, places), session.formatting());
            if (allowed >= 0 && fractionDigits(textOf(inputValue(element), session.formatting()),
                                               session.formatting()) > static_cast<std::size_t>(allowed)) {
                // The key and fallback of a form field's `too-precise`, so both read alike in any language.
                errors.emplace_back(render::resolveText(session.formatting().translations, session.formatting().locale,
                                                        std::nullopt, "morph.forms.error.too-precise",
                                                        "Too many decimal places"));
            }
        }
        if (!field.id.empty()) {
            std::vector<std::string> const path{"errors", field.id};
            for (auto const& query : owner.queries) {
                for (std::string& text : stringsOf(walk(query->value(), path))) {
                    errors.push_back(std::move(text));
                }
            }
            for (auto const& mutation : owner.mutations) {
                for (std::string& text : stringsOf(walk(mutation->result(), path))) {
                    errors.push_back(std::move(text));
                }
            }
        }
        for (CheckRuntime const* const check : element.checks) {
            if (check->failing->get()) {
                errors.push_back(check->message->get());
            }
        }
        return listOfStrings(errors);
    });
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity): one case per kind of element.
void ScreenCore::wireElement(ElementState& element) {
    DocNode const& node = *element.node;
    RtScope& scope = *element.scope;
    std::string_view const kind = node.kind;
    if (kind == "form") {
        wireForm(element);
        return;
    }
    if (kind == "collection" || kind == "wizard") {
        wireComposite(element);
        return;
    }
    if (kind == "table" && !node.name("model").empty()) {
        wireModelTable(element);
        return;
    }
    bool const input = kind == "textInput" || kind == "checkbox" || kind == "select" || kind == "dateTimeInput" ||
                       kind == "slider" || kind == "filePicker";
    if (input) {
        std::string_view key = "value";
        if (kind == "checkbox") {
            key = "checked";
        } else if (kind == "select") {
            key = "selected";
        }
        ExprPtr const expr = kind == "filePicker" ? nullptr : node.prop(key);
        bool const controlled = expr && (expr->kind != ExprKind::Literal || !expr->literal.isNull());
        if (controlled) {
            element.bound = &scope.nodes.computed([&scope, expr] { return evalIn(scope, nullptr, expr); });
            element.draft.set(runtime().untracked([&] { return element.bound->get(); }));
            scope.nodes.effect([this, &element, kind] {
                Value const bound = element.bound->get();
                runtime().untracked([&] {
                    if (!element.edited.peek()) {
                        element.draft.set(bound);
                    } else if (sameShown(kind, element.draft.peek(), bound, session.formatting())) {
                        element.edited.set(false);
                    }
                });
            });
        } else if (kind == "checkbox") {
            element.draft.set(false);
        }
    }
    if (kind == "select") {
        element.rows =
            &scope.nodes.computed([&scope, expr = node.prop("options")] { return evalIn(scope, nullptr, expr); });
        if (element.bound == nullptr && node.name("onStale") == "clear") {
            scope.nodes.effect([this, &element] {
                Value const options = element.rows->get();
                runtime().untracked([&] {
                    Value const selected = element.draft.peek();
                    if (selected.isNull()) {
                        return;
                    }
                    ValueList const* const items = options.asList();
                    bool const kept = items != nullptr && std::ranges::any_of(*items, [&](Value const& option) {
                                          return optionKey(option) == selected;
                                      });
                    if (!kept) {
                        session.noteWrite();
                        element.draft.set(Value{});
                    }
                });
            });
        }
    }
    // A bound sort is controlled as a bound selection is: any expression but a literal `null` binds it. A form grid
    // binds its sort as a rows table does.
    bool const formGrid = kind == "table" && !node.name("form").empty();
    if (formGrid || (kind == "table" && node.prop("rows"))) {
        ExprPtr const sort = node.prop("sort");
        if (sort && (sort->kind != ExprKind::Literal || !sort->literal.isNull())) {
            element.sortBound = &scope.nodes.computed([&scope, sort] { return evalIn(scope, nullptr, sort); });
        }
    }
    if (kind == "table" && node.prop("rows")) {
        element.rows =
            &scope.nodes.computed([&scope, expr = node.prop("rows")] { return evalIn(scope, nullptr, expr); });
        // A bound selection is controlled, as a select's `selected` is: any expression but a literal `null` binds it.
        ExprPtr const selection = node.prop("selection");
        if (selection && (selection->kind != ExprKind::Literal || !selection->literal.isNull())) {
            element.bound = &scope.nodes.computed([&scope, selection] { return evalIn(scope, nullptr, selection); });
        }
        element.rowsTable = makeRowsTable(element, nullptr, true);
        element.view = element.rowsTable->view.get();
    }
}

[[nodiscard]] Value tableSort(ElementState const& element) {
    return element.sortBound != nullptr ? element.sortBound->get() : element.sort.get();
}

namespace {

/// @brief A table's page values: `viewCount` (the rows it orders, every page), `page`, `pageCount`, and
///        `pageFirst` and `pageLast`, the 1-based positions of the page's first and last rows, both 0 when there are
///        none. A table that does not page reads page 0 of 1, over every row.
/// @param element The table's element.
/// @param name    The value's name.
/// @return The value. Tracked.
[[nodiscard]] Value tablePageValue(ElementState const& element, std::string_view name) {
    if (element.view == nullptr) {
        return name == "pageCount" ? std::int64_t{1} : std::int64_t{0};
    }
    TableView const& view = element.view->get();
    if (name == "viewCount") {
        return static_cast<std::int64_t>(view.rows.size());
    }
    if (name == "page") {
        return view.page;
    }
    if (name == "pageCount") {
        return view.pageCount;
    }
    bool const none = view.first == view.last;
    if (name == "pageFirst") {
        return none ? std::int64_t{0} : static_cast<std::int64_t>(view.first + 1);
    }
    return static_cast<std::int64_t>(view.last);
}

/// @brief A model-backed table's own element values, read from its rows: `viewCount` (the reply's total, `null`
///        before the first), `viewCountKind` (`exact` or `atLeast`), `undecided`, `awaiting`, `failed`, `pending`
///        (its description or a fetch in flight) and `error`.
/// @param table The table.
/// @param name  The value's name.
/// @return The value; nothing for a value the table shares with a table over `rows`. Tracked.
[[nodiscard]] std::optional<Value> modelTableValue(ModelTable const& table, std::string_view name) {
    static_cast<void>(table.version.get());
    auto const strings = [](std::vector<std::string> const& list) {
        ValueList out;
        for (std::string const& item : list) {
            out.emplace_back(item);
        }
        return Value{std::move(out)};
    };
    TableCountsView const counts = table.rows->counts();
    if (name == "viewCount") {
        return counts.view.known ? Value{counts.view.rows} : Value{};
    }
    if (name == "viewCountKind") {
        if (!counts.view.known) {
            return Value{};
        }
        return Value{std::string{counts.view.kind == table::TotalKind::AtLeast ? "atLeast" : "exact"}};
    }
    if (name == "undecided") {
        return Value{counts.undecided};
    }
    if (name == "awaiting") {
        return strings(counts.awaiting);
    }
    if (name == "failed") {
        return strings(counts.failed);
    }
    if (name == "pending") {
        return Value{counts.pending || (table.describe != nullptr && table.describe->pending())};
    }
    if (name == "error") {
        if (auto const& error = table.window->error()) {
            return Value{error->message};
        }
        return table.describe != nullptr ? table.describe->error() : Value{};
    }
    return std::nullopt;
}

}  // namespace

[[nodiscard]] ValueList selectedOffPage(TableView const& view, Value const& selection,
                                        std::function<Key(Value const&)> const& rowKey) {
    std::unordered_set<Key> onPage;
    std::unordered_set<Key> offPage;
    for (std::size_t index = 0; index < view.rows.size(); ++index) {
        (index >= view.first && index < view.last ? onPage : offPage).insert(rowKey(view.rows.at(index)));
    }
    ValueList kept;
    if (ValueList const* const selected = selection.asList()) {
        for (Value const& item : *selected) {
            Key const key = keyOf(item);
            if (offPage.contains(key) && !onPage.contains(key)) {
                kept.push_back(item);
            }
        }
    }
    return kept;
}

namespace {

/// @brief The sortable column part of a table with the id @p columnId.
/// @param table    The table.
/// @param columnId The column's id.
/// @return The part, or `nullptr` when no sortable column has that id.
[[nodiscard]] DocNode const* sortableColumn(DocNode const& table, std::string_view columnId) {
    for (DocNodePtr const& column : table.partList("columns")) {
        if (column->id == columnId && column->flag("sortable")) {
            return column.get();
        }
    }
    return nullptr;
}

}  // namespace

[[nodiscard]] std::optional<std::vector<SortKey>> sortKeysOf(DocNode const& table, Value const& sort) {
    std::vector<SortKey> keys;
    if (sort.isNull()) {
        return keys;
    }
    ValueList const* const items = sort.asList();
    if (items == nullptr) {
        return std::nullopt;
    }
    for (Value const& item : *items) {
        Value const* const column = item.field("column");
        Value const* const direction = item.field("direction");
        std::string const* const columnId = column == nullptr ? nullptr : column->asString();
        std::string const* const way = direction == nullptr ? nullptr : direction->asString();
        DocNode const* const part = columnId == nullptr ? nullptr : sortableColumn(table, *columnId);
        if (part == nullptr || way == nullptr || (*way != "asc" && *way != "desc")) {
            return std::nullopt;
        }
        keys.push_back(SortKey{.column = part, .descending = *way == "desc"});
    }
    return keys;
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity): builds the table's view from the page and the groups in one pass, a heading before the first row of each group
TableView ScreenCore::tableView(RowsTable const& table) {
    static_cast<void>(table.version.get());
    DocNode const& node = *table.node;
    RtScope const& scope = *table.where;
    Bindings const* const bindings = table.bindings.get();
    TableView view;
    if (!table.engine->restoring()) {
        table::Engine const& engine = table.engine->engine();
        std::size_t const count = engine.viewRowCount();
        view.rows.reserve(count);
        for (std::size_t index = 0; index < count; ++index) {
            std::optional<TableRowRef> const ref = table.engine->row(static_cast<std::int64_t>(index));
            view.rows.push_back(ref ? ref->row : Value{});
            if (table.grouped) {
                std::optional<std::size_t> const sourceRow = engine.sourceRowOf(index);
                view.runOf.push_back(sourceRow ? table.source->runOf(*sourceRow) : 0);
            }
        }
        if (table.grouped && count > 0) {
            std::span<Value const> const runs = table.source->runRecords();
            view.runs.assign(runs.begin(), runs.end());
        }
    }
    view.last = view.rows.size();
    ExprPtr const sizeExpr = node.prop("pageSize");
    if (!sizeExpr) {
        return view;
    }
    // A `pageSize` of `null` does not page; one that is no positive integer is reported and does not page either.
    Value const size = evalIn(scope, bindings, sizeExpr);
    std::int64_t const* const perPage = size.asInt();
    if (size.isNull() || perPage == nullptr || *perPage < 1) {
        if (!size.isNull()) {
            report(pointerAppend(node.path, "pageSize"),
                   "pageSize is " + canonicalJson(size) + "; a page holds a positive whole number of rows");
        }
        return view;
    }
    // A `page` of `null` is the first; one that is negative or not a whole number is reported and shows the first.
    // One past the last is the last: a filter that shrinks the rows under the user is normal, not an error.
    std::int64_t asked = 0;
    if (ExprPtr const pageExpr = node.prop("page")) {
        Value const page = evalIn(scope, bindings, pageExpr);
        std::int64_t const* const index = page.asInt();
        if (index != nullptr && *index >= 0) {
            asked = *index;
        } else if (!page.isNull()) {
            report(pointerAppend(node.path, "page"),
                   "page is " + canonicalJson(page) + "; a page is a whole number from 0");
        }
    }
    auto const total = static_cast<std::int64_t>(view.rows.size());
    view.pageCount = std::max<std::int64_t>(1, (total + *perPage - 1) / *perPage);
    view.page = std::min(asked, view.pageCount - 1);
    view.first = static_cast<std::size_t>(std::min(total, view.page * *perPage));
    view.last = static_cast<std::size_t>(std::min(total, (view.page + 1) * *perPage));
    return view;
}

[[nodiscard]] TableSelection selectionNow(ElementState const& element) {
    if (element.modelTable != nullptr) {
        if (auto const& view = element.modelTable->viewSelection.get()) {
            return *view;
        }
    }
    TableSelection out;
    if (Value const itemsValue = tableSelection(element); ValueList const* const items = itemsValue.asList()) {
        for (Value const& item : *items) {
            out.keys.push_back(keyOf(item));
        }
    }
    return out;
}

[[nodiscard]] Value tableSelection(ElementState const& element) {
    if (element.bound == nullptr) {
        return element.selection.get();
    }
    Value const bound = element.bound->get();
    return bound.asList() != nullptr ? bound : Value{ValueList{}};
}

[[nodiscard]] Value jsonTextValue(std::string_view text) {
    auto const json = parseJson(text);
    if (!json) {
        return Value{};
    }
    auto value = valueFromJson(*json);
    return value ? *std::move(value) : Value{};
}

namespace {

/// @brief One field of a form as a record: `value` (the encoded value, `null` while blank or invalid), `draft` (the
///        text), `valid`, `required`, `visible`, `readonly` and `errors`. Tracked.
/// @param field The field.
/// @return The record.
[[nodiscard]] Value formFieldValue(forms::FieldState const& field) {
    ValueRecord record;
    forms::Encoding const& encoding = field.encoded().get();
    Value value;
    if (encoding.has_value() && encoding->has_value()) {
        value = (*encoding)->value.isNull() ? jsonTextValue((*encoding)->json) : (*encoding)->value;
    }
    std::optional<forms::FieldError> const& error = field.error().get();
    record.insert_or_assign("value", std::move(value));
    record.insert_or_assign("draft", Value{field.spec().kind == forms::FieldKind::Object ||
                                                   field.spec().kind == forms::FieldKind::ObjectArray
                                               ? std::string{}
                                               : field.draft()});
    record.insert_or_assign("valid", Value{!error.has_value()});
    record.insert_or_assign("required", Value{field.required().get()});
    record.insert_or_assign("visible", Value{field.visible().get()});
    record.insert_or_assign("readonly", Value{field.readonly().get()});
    record.insert_or_assign("errors",
                            error.has_value() ? Value{ValueList{Value{error->message}}} : Value{ValueList{}});
    return Value{std::move(record)};
}

/// @brief A form's element values (spec 5 §6): `ready`, `body` (the record the form would submit, `null` while not
///        ready), `submitted` (whether a submission was sent), `pending`, and each field by its wire name. Tracked.
/// @param element The form's element.
/// @param name    The value's name.
/// @return The value; `null` for a name the form does not have, or a form without a session.
[[nodiscard]] Value formValue(ElementState const& element, std::string_view name) {
    forms::FormSession const* const form = element.form.get();
    if (form == nullptr) {
        return Value{};
    }
    if (name == "ready") {
        return form->ready();
    }
    if (name == "body") {
        std::optional<std::string> const body = form->body();
        return body.has_value() ? jsonTextValue(*body) : Value{};
    }
    if (name == "submitted") {
        return form->submitAttempts() > 0;
    }
    if (name == "pending") {
        return form->pending();
    }
    for (auto const& field : form->fields()) {
        if (field->spec().name == name) {
            return formFieldValue(*field);
        }
    }
    return Value{};
}

}  // namespace

Value ScreenCore::inputValue(ElementState const& element) const {
    std::string_view const kind = element.node->kind;
    if (kind == "checkbox") {
        return elementValue(element, "checked");
    }
    if (kind == "select") {
        return elementValue(element, "selected");
    }
    return elementValue(element, "value");
}

// NOLINTNEXTLINE(misc-no-recursion,readability-function-cognitive-complexity): one case per element value.
Value ScreenCore::elementValue(ElementState const& element, std::string_view name) const {
    DocNode const& node = *element.node;
    RtScope const& scope = *element.scope;
    std::string_view const kind = node.kind;
    auto const flagOf = [&](std::string_view key, bool fallback) -> Value {
        ExprPtr const expr = node.prop(key);
        if (!expr) {
            return fallback;
        }
        Value const value = evalIn(scope, nullptr, expr);
        bool const* const flag = value.asBool();
        return flag != nullptr && *flag;
    };
    auto const errors = [&] -> Value { return element.errors->get(); };
    auto const count = [](Value const& list) -> Value {
        ValueList const* const items = list.asList();
        return static_cast<std::int64_t>(items == nullptr ? 0 : items->size());
    };
    if (name == "visible" || name == "enabled") {
        return flagOf(name, true);
    }
    if (name == "errors") {
        return errors();
    }
    if (name == "valid") {
        Value const current = errors();
        ValueList const* const items = current.asList();
        return items == nullptr || items->empty();
    }
    if (name == "shownErrors") {
        RtScope const& policy = scope.policyScope();
        std::string const mode = policy.showErrors.empty() ? _doc->showErrors : policy.showErrors;
        bool const submittedNow = policy.submitted != nullptr && policy.submitted->get();
        if (mode == "always" || (mode == "submit" && submittedNow) ||
            (mode != "submit" && (submittedNow || element.touched.get() || touchedWithin(element)))) {
            return errors();
        }
        return Value{ValueList{}};
    }
    if (kind == "dialog" || kind == "drawer") {
        if (name == "open") {
            return node.prop("open") ? flagOf("open", false) : Value{element.open.get()};
        }
        if (name == "ready") {
            RtScope const* const inner = element.inner.get();
            return inner != nullptr && inner->ready();
        }
        if (name == "submitted") {
            return element.submitted.get();
        }
        return Value{};
    }
    if (kind == "table" && (name == "layout" || name == "visibleColumns" || name == "hiddenCount")) {
        TableConfig const* const config =
            element.rowsTable != nullptr ? &element.rowsTable->config
            // NOLINTBEGIN(readability-avoid-nested-conditional-operator): a flat pick through the element's table kinds in the order they are wired
            : element.modelTable != nullptr ? &element.modelTable->config
            : element.gridTable != nullptr  ? &element.gridTable->config
                                            : nullptr;
        // NOLINTEND(readability-avoid-nested-conditional-operator)
        return config == nullptr ? Value{} : layoutElementValue(*config, name);
    }
    if (kind == "table" && name == "filters") {
        TableConfig const* const config =
            element.rowsTable != nullptr ? &element.rowsTable->config
            // NOLINTBEGIN(readability-avoid-nested-conditional-operator): a flat pick through the element's table kinds in the order they are wired
            : element.modelTable != nullptr ? &element.modelTable->config
            : element.gridTable != nullptr  ? &element.gridTable->config
                                            : nullptr;
        // NOLINTEND(readability-avoid-nested-conditional-operator)
        return config == nullptr || !config->filters ? Value{} : filtersValueOf(config->filters());
    }
    if (kind == "table" && element.gridTable != nullptr) {
        FormGrid const& grid = element.gridTable->grid;
        FixedGridSource const* const fixed = element.gridTable->fixed.get();
        if (name == "modifiedCount") {
            std::size_t const cells = fixed != nullptr ? fixed->modifiedCount() : grid.source->modifiedCount();
            return Value{static_cast<std::int64_t>(cells)};
        }
        if (name == "modifiedRows") {
            std::size_t const cells = fixed != nullptr ? fixed->modifiedRows() : grid.source->modifiedRows();
            return Value{static_cast<std::int64_t>(cells)};
        }
        if (name == "errorCount") {
            std::size_t const cells = fixed != nullptr ? fixed->errorCount() : grid.source->errorCount();
            return Value{static_cast<std::int64_t>(cells)};
        }
        if (name == "editing") {
            std::optional<CellRef> const cell = grid.grid->editing();
            if (!cell) {
                return Value{};
            }
            return Value{ValueRecord{{"key", valueOfKey(cell->key)}, {"column", Value{cell->column}}}};
        }
        if (name == "selection") {
            return cellRangeValue(grid.grid->selection());
        }
    }
    if (kind == "table" && (name == "modifiedCount" || name == "modifiedRows" || name == "errorCount")) {
        return Value{std::int64_t{0}};
    }
    if (kind == "table" && name == "busyCount") {
        if (element.modelTable == nullptr || element.modelTable->rows == nullptr) {
            return Value{std::int64_t{0}};
        }
        static_cast<void>(element.modelTable->marksRevision.get());
        return Value{element.modelTable->rows->states().busyCount()};
    }
    if (kind == "table" && (name == "selectionSet" || name == "selectedCount" || name == "selectedCountKind")) {
        TableSelection const selection = selectionNow(element);
        if (name == "selectionSet") {
            auto const json = parseJson(selectionJsonOf(selection));
            auto read = json ? valueFromJson(*json) : std::expected<Value, std::string>{Value{}};
            return read ? std::move(*read) : Value{};
        }
        SelectedCount counted{.rows = static_cast<std::int64_t>(selection.keys.size()), .kind = CountKind::Exact};
        if (element.modelTable != nullptr && element.modelTable->rows != nullptr) {
            static_cast<void>(element.modelTable->version.get());
            counted = element.modelTable->rows->selectedCount(selection);
        }
        if (name == "selectedCount") {
            return Value{counted.rows};
        }
        return Value{std::string{counted.kind == CountKind::Exact ? "exact" : "approximate"}};
    }
    if (kind == "table" && element.modelTable != nullptr && element.modelTable->window != nullptr) {
        if (std::optional<Value> value = modelTableValue(*element.modelTable, name)) {
            return std::move(*value);
        }
    }
    if (kind == "table") {
        if (name == "selection") {
            return tableSelection(element);
        }
        if (name == "activeKey") {
            return element.activeKey.get();
        }
        if (name == "selectedCount") {
            return count(tableSelection(element));
        }
        if (name == "sort") {
            return tableSort(element);
        }
        if (name == "viewCount" || name == "page" || name == "pageCount" || name == "pageFirst" ||
            name == "pageLast") {
            return tablePageValue(element, name);
        }
        if (name == "sourceCount") {
            return element.rows == nullptr ? Value{std::int64_t{0}} : count(element.rows->get());
        }
        ExprPtr const rows = node.prop("rows");
        if ((name == "pending" || name == "error") && rows && rows->kind == ExprKind::Query) {
            ScopeEnv const env{scope, nullptr};
            return env.control(name == "pending" ? ExprKind::Pending : ExprKind::Error, rows->name);
        }
        return name == "pending" ? Value{false} : Value{};
    }
    if (kind == "tabs") {
        // The shown tab's id: the bound one, else the user's own pick, else the first tab's.
        ExprPtr const selected = node.prop("selected");
        if (name != "selected") {
            return Value{};
        }
        if (selected && (selected->kind != ExprKind::Literal || !selected->literal.isNull())) {
            return evalIn(scope, nullptr, selected);
        }
        if (Value picked = element.draft.get(); !picked.isNull()) {
            return picked;
        }
        auto const tabs = node.partList("tabs");
        return tabs.empty() || tabs.front()->id.empty() ? Value{} : Value{tabs.front()->id};
    }
    if (kind == "form") {
        return formValue(element, name);
    }
    if (name == "draft") {
        return element.draft.get();
    }
    if (name == "dirty") {
        return element.edited.get();
    }
    if (name == "focused") {
        return element.focused.get();
    }
    if (name == "touched") {
        return element.touched.get();
    }
    if (name == "checked" || name == "selected") {
        return element.bound != nullptr ? element.bound->get() : element.draft.get();
    }
    if (name == "selectedOption") {
        Value const selected = elementValue(element, "selected");
        ValueList const* const options = element.rows == nullptr ? nullptr : element.rows->get().asList();
        if (options != nullptr && !selected.isNull()) {
            for (Value const& option : *options) {
                if (optionKey(option) == selected) {
                    return option;
                }
            }
        }
        return Value{};
    }
    if (name == "value") {
        if (element.bound != nullptr) {
            return element.bound->get();
        }
        return element.committed.get();
    }
    return Value{};
}

bool ScreenCore::touchedWithin(ElementState const& element) {
    if (element.node->kind != "group") {
        return false;
    }
    std::string const inside = element.node->path + "/";
    return std::ranges::any_of(element.scope->elements, [&inside](std::unique_ptr<ElementState> const& other) {
        return other->node->path.starts_with(inside) && other->touched.get();
    });
}

// --- Commands --------------------------------------------------------------------------------------------------

// NOLINTBEGIN(misc-no-recursion): command lists nest
void ScreenCore::runList(Commands const& commands, CommandCtx const& ctx, Done done) {
    if (!done) {
        done = [](bool) {};
    }
    if (commands.empty()) {
        done(true);
        return;
    }
    struct Join {
        std::size_t left = 0;
        bool ok = true;
        Done done;
    };
    if (recording) {
        logList(commands);
    }
    auto const join = std::make_shared<Join>(Join{.left = commands.size() + 1, .ok = true, .done = std::move(done)});
    auto const finish = [join](bool succeeded) {
        join->ok = join->ok && succeeded;
        if (--join->left == 0) {
            join->done(join->ok);
        }
    };
    for (Command const& command : commands) {
        runOne(command, ctx, finish);
    }
    finish(true);
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity): one case per command of ui/1.
void ScreenCore::runOne(Command const& command, CommandCtx const& ctx, Done done) {
    if (!done) {
        done = [](bool) {};
    }
    RtScope const& scope = *ctx.scope;
    auto const eval = [&](ExprPtr const& expr) { return evalIn(scope, ctx.bindings.get(), expr); };
    auto const finish = [&done](bool succeeded) { done(succeeded); };
    switch (command.kind) {
        case CommandKind::Set:
            runSet(command, ctx, done);
            return;
        case CommandKind::Reset:
            runReset(command, ctx, done);
            return;
        case CommandKind::Run:
            runMutation(command, ctx, std::move(done));
            return;
        case CommandKind::Refetch: {
            // A loaded document's `refetch` names a query in scope (`loadDocument` checks it), as `run` names a
            // mutation and `open` a dialog.
            scope.find(command.target)->query->refetch();
            finish(true);
            return;
        }
        case CommandKind::Open:
        case CommandKind::Close: {
            ElementState* target = nullptr;
            if (command.kind == CommandKind::Close && command.target == "self") {
                // `self` is the innermost dialog or drawer whose content holds the command, found lexically. A
                // component's fallback scope has no parent, so the walk stops at its boundary as its names do.
                RtScope const* walk = &scope;
                while (walk->owner == nullptr && walk->parent) {
                    walk = walk->parent.get();
                }
                target = walk->owner;
                if (target == nullptr && walk != rootScope.lock().get()) {
                    report(command.path, "`close: self` inside a component names no dialog or drawer");
                    finish(false);
                    return;
                }
                if (target == nullptr) {
                    finish((host != nullptr && host->closeSelf(_doc->id, std::nullopt)) ||
                           unsupported(command, "`close: self`"));
                    return;
                }
            } else {
                target = scope.find(command.target)->element;
            }
            ElementState& element = *target;
            if (element.node->prop("open")) {
                report(command.path, "the dialog's `open` is bound to an expression");
                finish(false);
                return;
            }
            bool const opening = command.kind == CommandKind::Open;
            if (opening && !element.open.peek()) {
                element.submitted.set(false);
            }
            session.noteWrite();
            element.open.set(opening);
            finish(true);
            return;
        }
        case CommandKind::Navigate: {
            NavigateRequest const request{
                .screen = command.target,
                .route = eval(command.extra),
                .params = eval(command.value),
                .as = command.option,
                .call = false,
            };
            if (host == nullptr) {
                finish(unsupported(command, "`navigate`"));
                return;
            }
            // A navigation without `onResult` and `onCancel` asks for no answer, so it files no call: the screen it
            // opens answers nobody, and its `return` is the application's.
            if (command.commands.empty() && command.alternative.empty()) {
                finish(host->navigate(request, {}, {}) || unsupported(command, "`navigate`"));
                return;
            }
            std::weak_ptr<RtScope> const weak = ctx.scope;
            CommandCtx const kept = detached(ctx);
            auto onResult = [this, weak, kept, list = &command.commands](Value const& result) {
                if (auto const held = weak.lock()) {
                    auto const bindings = std::make_shared<Bindings>(*kept.bindings);
                    bindings->push_back(Binding{.name = "event", .read = [result] { return result; }});
                    CommandCtx const next{
                        .scope = held,
                        .bindings = bindings,
                        .cancelClose = kept.cancelClose,
                        .boundary = kept.boundary,
                    };
                    runtime().batch([&] { runList(*list, next, {}); });
                }
            };
            auto onCancel = [this, weak, kept, list = &command.alternative] {
                if (auto const held = weak.lock()) {
                    CommandCtx next = kept;
                    next.scope = held;
                    runtime().batch([&] { runList(*list, next, {}); });
                }
            };
            finish(host->navigate(request, std::move(onResult), std::move(onCancel)) ||
                   unsupported(command, "`navigate`"));
            return;
        }
        case CommandKind::Return:
            finish(host != nullptr && host->closeSelf(_doc->id, eval(command.value))
                       ? true
                       : unsupported(command, "`return`"));
            return;
        case CommandKind::Focus:
            if (focusTarget(command, scope, ctx.bindings.get())) {
                finish(true);
                return;
            }
            report(command.path, "`" + command.target + "` is not mounted");
            finish(false);
            return;
        case CommandKind::Host:
            runHostCommand(command, ctx, done);
            return;
        case CommandKind::SwitchBackend: {
            std::weak_ptr<RtScope> const weak = ctx.scope;
            auto committed = [this, weak, target = command.target, path = command.path,
                              done](std::expected<void, std::string> const& outcome) {
                if (!weak.lock()) {
                    return;
                }
                if (!outcome) {
                    report(path, "backendSwitchFailed: " + outcome.error());
                    done(false);
                    return;
                }
                session.backendChanged(target);
                done(true);
            };
            if (host != nullptr && host->switchBackend(command.target, std::move(committed))) {
                return;
            }
            report(command.path, "backend `" + command.target + "` is not listed");
            finish(false);
            return;
        }
        case CommandKind::ResetPrefs: {
            ValueRecord prefsNow = prefs.peek();
            if (command.target == "all") {
                prefsNow = prefDefaults;
            } else if (auto const found = prefDefaults.find(command.target); found != prefDefaults.end()) {
                prefsNow.insert_or_assign(command.target, found->second);
            }
            prefs.set(std::move(prefsNow));
            if (host != nullptr) {
                host->resetPrefs(_doc->id, command.target);
            }
            finish(true);
            return;
        }
        case CommandKind::SetLocale:
            session.setLocale(command.target);
            if (host != nullptr) {
                host->setLocale(command.target);
            }
            finish(true);
            return;
        case CommandKind::SetContext: {
            // The shell checks the value against the switcher's options; with no switcher it is unsupported.
            Value const requested = eval(command.value);
            if (session.requestContext(requested)) {
                finish(true);
            } else {
                report(command.path,
                       "`setContext` was refused: " + std::string{session.can("context")
                                                                      ? "the value is not one of the scope's options"
                                                                      : "no scope switcher is declared"});
                finish(false);
            }
            return;
        }
        case CommandKind::Notify: {
            std::string const text = textOf(eval(command.value), ctx.scope->formatting());
            finish(host != nullptr && host->notify(text, command.option) ? true : unsupported(command, "`notify`"));
            return;
        }
        case CommandKind::Seq: {
            // A step that waits for a reply keeps the rest of the sequence in that reply's handler, which the
            // dispatcher may hold past the screen: the sequence holds its scope weakly and stops once it is gone.
            CommandCtx const inner = detached(CommandCtx{
                .scope = nullptr,
                .bindings = std::make_shared<Bindings>(*ctx.bindings),
                .cancelClose = ctx.cancelClose,
                .boundary = ctx.boundary,
            });
            std::weak_ptr<RtScope> const weak = ctx.scope;
            auto const step = std::make_shared<std::function<void(std::size_t)>>();
            *step = [this, inner, weak, list = &command.commands, done = std::move(done),
                     weakStep = std::weak_ptr<std::function<void(std::size_t)>>{step}](std::size_t index) {
                auto const held = weak.lock();
                if (index == list->size() || !held) {
                    done(held != nullptr);
                    return;
                }
                auto const self = weakStep.lock();
                CommandCtx current = inner;
                current.scope = held;
                runOne(list->at(index), current, [self, index, done](bool succeeded) {
                    if (!succeeded) {
                        done(false);
                        return;
                    }
                    (*self)(index + 1);
                });
            };
            (*step)(0);
            return;
        }
        case CommandKind::All: {
            CommandCtx const inner{
                .scope = ctx.scope,
                .bindings = std::make_shared<Bindings>(*ctx.bindings),
                .cancelClose = ctx.cancelClose,
                .boundary = ctx.boundary,
            };
            runList(command.commands, inner, std::move(done));
            return;
        }
        case CommandKind::If: {
            Value const condition = eval(command.value);
            bool const* const flag = condition.asBool();
            if (flag == nullptr) {
                finish(true);
                return;
            }
            runList(*flag ? command.commands : command.alternative, ctx, std::move(done));
            return;
        }
        case CommandKind::CancelClose:
            *ctx.cancelClose = true;
            finish(true);
            return;
        case CommandKind::Retry:
            // `retry` is valid only inside a boundary's `error`, which is lowered with its boundary.
            for (QueryRuntime* const query : ctx.boundary->queries) {
                if (!runtime().untracked([&] { return query->error(); }).isNull()) {
                    query->refetch();
                }
            }
            finish(true);
            return;
        case CommandKind::SignIn: {
            NameRef const* const found = scope.find(command.target);
            Value const reply = found != nullptr && found->category == NameCategory::Mutation
                                    ? runtime().untracked([&] { return found->mutation->result(); })
                                    : Value{};
            session.clear();
            finish(host != nullptr && host->signIn(reply) ? true : unsupported(command, "`signIn`"));
            return;
        }
        case CommandKind::SignOut:
            session.clear();
            if (host != nullptr) {
                host->signOut();
            }
            finish(true);
            return;
        case CommandKind::Upload:
        case CommandKind::Download:
        default:
            runTransfer(command, ctx, std::move(done));
            return;
    }
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity): a single run, or a run per list item.
void ScreenCore::runMutation(Command const& command, CommandCtx const& ctx, Done done) const {
    RtScope const& scope = *ctx.scope;
    MutationRuntime& mutation = *scope.find(command.target)->mutation;
    RtScope const& policy = scope.policyScope();
    if (policy.submitted != nullptr) {
        policy.submitted->set(true);
    }
    if (policy.owner != nullptr && !runtime().untracked([&] { return policy.ready(); })) {
        done(false);
        return;
    }
    RtScope const& declaring = *[&] -> RtScope const* {
        for (RtScope const* candidate = &scope; candidate != nullptr; candidate = candidate->parent.get()) {
            if (candidate->names.contains(command.target)) {
                return candidate;
            }
        }
        return &scope;
    }();
    auto const bodyFor = [&](Bindings const& bindings) {
        if (command.value) {
            return evalIn(scope, &bindings, command.value);
        }
        if (mutation.decl().body) {
            return evalIn(declaring, &bindings, mutation.decl().body);
        }
        return Value{ValueRecord{}};
    };
    if (!command.extra) {
        Value body = bodyFor(*ctx.bindings);
        mutation.run(std::move(body), std::make_shared<Bindings const>(*ctx.bindings),
                     [bindings = ctx.bindings, bindAs = command.binding, done = std::move(done)](bool succeeded,
                                                                                                 Value const& reply) {
                         if (succeeded && !bindAs.empty()) {
                             bindings->push_back(Binding{.name = bindAs, .read = [reply] { return reply; }});
                         }
                         done(succeeded);
                     });
        return;
    }
    Value const each = evalIn(scope, ctx.bindings.get(), command.extra);
    ValueList const items = each.asList() == nullptr ? ValueList{} : *each.asList();
    if (items.empty()) {
        done(true);
        return;
    }
    struct Each {
        std::size_t left = 0;
        bool ok = true;
        Done done;
    };
    auto const state = std::make_shared<Each>(Each{.left = items.size(), .ok = true, .done = std::move(done)});
    std::vector<Value> bodies;
    std::vector<std::shared_ptr<Bindings const>> bound;
    for (Value const& item : items) {
        Bindings bindings = *ctx.bindings;
        if (!command.binding.empty()) {
            bindings.push_back(Binding{.name = command.binding, .read = [item] { return item; }});
        }
        bodies.push_back(bodyFor(bindings));
        bound.push_back(std::make_shared<Bindings const>(std::move(bindings)));
    }
    if (mutation.decl().concurrency != Concurrency::Exclusive) {
        for (std::size_t index = 0; index < bodies.size(); ++index) {
            mutation.run(bodies.at(index), bound.at(index), [state](bool succeeded, Value const&) {
                state->ok = state->ok && succeeded;
                if (--state->left == 0) {
                    state->done(state->ok);
                }
            });
        }
        return;
    }
    // An exclusive mutation refuses a run while another is in flight, so its elements go one after another.
    auto const step = std::make_shared<std::function<void(std::size_t)>>();
    *step = [&mutation, bodies, bound, state,
             weakStep = std::weak_ptr<std::function<void(std::size_t)>>{step}](std::size_t index) {
        if (index == bodies.size()) {
            state->done(state->ok);
            return;
        }
        auto const self = weakStep.lock();
        mutation.run(bodies.at(index), bound.at(index), [self, state, index](bool succeeded, Value const&) {
            if (!succeeded) {
                state->done(false);
                return;
            }
            (*self)(index + 1);
        });
    };
    (*step)(0);
}

// NOLINTEND(misc-no-recursion)
void ScreenCore::writeSlot(StateSlot& slot, std::span<std::string const> within, Value value, Command const& command,
                           Done const& done) {
    slot.scope->core.initState(*slot.scope, slot);
    Type const& type = slot.decl->type;
    if (within.empty() && !type.isAny() && !type.admits(value)) {
        report(command.path, "`set` writes a value that is not a " + type.name());
        done(false);
        return;
    }
    Value next = withPath(slot.value->peek(), within, std::move(value));
    session.noteWrite();
    slot.value->set(std::move(next));
    done(true);
}

void ScreenCore::runSetApp(Command const& command, std::span<std::string const> rest, Value value, Done const& done) {
    if (RtScope const* const app = session.appScope(); app != nullptr && !rest.empty()) {
        NameRef const* const named = app->find(rest.front());
        if (named != nullptr && named->category == NameCategory::State) {
            writeSlot(*named->state, rest.subspan(1), std::move(value), command, done);
            return;
        }
        report(command.path, "`app." + rest.front() + "` is not state of the app document");
        done(false);
        return;
    }
    std::string name;
    for (std::string const& segment : rest) {
        name += name.empty() ? segment : "." + segment;
    }
    done(host != nullptr && host->setAppValue(name, value) ? true : unsupported(command, "`set` of `app`"));
}

void ScreenCore::runSet(Command const& command, CommandCtx const& ctx, Done const& done) {
    RtScope const& scope = *ctx.scope;
    auto path = splitPath(command.target);
    if (!path || path->empty()) {
        done(false);
        return;
    }
    std::string const head = path->front();
    std::span<std::string const> const rest = std::span<std::string const>{*path}.subspan(1);
    Value value = evalIn(scope, ctx.bindings.get(), command.value);
    if (head == "app") {
        runSetApp(command, rest, std::move(value), done);
        return;
    }
    NameRef const* const found = scope.find(head);
    if (found != nullptr && found->category == NameCategory::State) {
        writeSlot(*found->state, rest, std::move(value), command, done);
        return;
    }
    if (found != nullptr && found->category == NameCategory::Element) {
        ElementState& element = *found->element;
        session.noteWrite();
        element.draft.set(value);
        if (element.bound == nullptr) {
            element.committed.set(std::move(value));
        }
        done(true);
        return;
    }
    report(command.path, "`set` names nothing writable here");
    done(false);
}

void ScreenCore::runReset(Command const& command, CommandCtx const& ctx, Done const& done) {
    RtScope const& scope = *ctx.scope;
    NameRef const* const found = scope.find(command.target);
    auto const resetStates = [this](RtScope const& owner) {
        for (auto const& slot : owner.states) {
            if (slot->value) {
                Value init = runtime().untracked([&] { return evalIn(owner, nullptr, slot->decl->init); });
                slot->value->set(std::move(init));
            }
        }
        for (auto const& element : owner.elements) {
            element->draft.set(element->bound != nullptr ? runtime().untracked([&] { return element->bound->get(); })
                                                         : Value{});
            element->committed.set(Value{});
            element->edited.set(false);
            element->touched.set(false);
        }
    };
    bool known = true;
    session.noteWrite();
    if (found != nullptr && found->category == NameCategory::State) {
        StateSlot& slot = *found->state;
        initState(*slot.scope, slot);
        slot.value->set(runtime().untracked([&] { return evalIn(*slot.scope, nullptr, slot.decl->init); }));
    } else if (found != nullptr && found->category == NameCategory::Element) {
        ElementState& element = *found->element;
        element.submitted.set(false);
        if (RtScope const* const inner = element.inner.peek()) {
            resetStates(*inner);
        }
    } else {
        report(command.path, "`reset` names no state or dialog here");
        known = false;
    }
    done(known);
}

void ScreenCore::runHostCommand(Command const& command, CommandCtx const& ctx, Done const& done) {
    // A command limited to other documents is one this document cannot see: missing, and nothing runs.
    if (!session.hostNameIn(command.target, doc().id)) {
        report(command.path, "host command `" + command.target + "` is missing");
        done(false);
        return;
    }
    Value const args = evalIn(*ctx.scope, ctx.bindings.get(), command.value);
    if (auto const& run = session.hostCommandRunner()) {
        HostCommandResult const result = run(command.target, args);
        if (result.kind == HostCommandResult::Kind::Missing) {
            report(command.path, "host command `" + command.target + "` is missing");
        } else if (result.kind == HostCommandResult::Kind::Refused) {
            report(command.path, result.reason);
        }
        done(result.kind == HostCommandResult::Kind::Ran);
        return;
    }
    bool const known = host != nullptr && host->hostCommand(command.target, args);
    if (!known) {
        report(command.path, "host command `" + command.target + "` is missing");
    }
    done(known);
}

}  // namespace morph::ui::detail

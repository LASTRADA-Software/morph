// SPDX-License-Identifier: Apache-2.0

// Lowering forms, wizards and components.

#include "interpreter_grid_detail.hpp"

namespace morph::ui::detail {

namespace {

/// @brief A document field overlay's expressions, evaluated once, as the engine's overlay.
/// @param scope The scope the form is declared in.
/// @param part  The `fields` entry.
/// @param format How values read as text and numbers.
/// @return The overlay.
[[nodiscard]] forms::FieldOverlay overlayOf(RtScope const& scope, DocNode const& part, FormatContext const& format) {
    auto const read = [&](std::string_view key) -> std::optional<Value> {
        ExprPtr const expr = part.prop(key);
        if (!expr) {
            return std::nullopt;
        }
        return scope.core.runtime().untracked([&] { return evalIn(scope, nullptr, expr); });
    };
    auto const text = [&](std::string_view key) -> std::optional<std::string> {
        auto const value = read(key);
        return value.has_value() && !value->isNull() ? std::optional<std::string>{textOf(*value, format)}
                                                     : std::nullopt;
    };
    auto const flag = [&](std::string_view key) {
        auto const value = read(key);
        bool const* const set = value.has_value() ? value->asBool() : nullptr;
        return set != nullptr && *set;
    };
    forms::FieldOverlay overlay{.omit = flag("omit"), .hidden = flag("hidden")};
    overlay.label = text("label");
    overlay.unit = text("unit");
    overlay.widget = text("widget");
    if (auto const decimals = read("decimals");
        decimals.has_value() && decimals->asInt() != nullptr && *decimals->asInt() >= 0) {
        overlay.decimals = static_cast<std::uint32_t>(std::min<std::int64_t>(*decimals->asInt(), 18));
    }
    if (auto const blankAs = text("blankAs")) {
        overlay.blankAs = *blankAs == "empty" ? forms::BlankAs::Empty : forms::BlankAs::Omit;
    }
    return overlay;
}

}  // namespace

[[nodiscard]] DetachedLower detachedLower(LowerCtx const& ctx) {
    return DetachedLower{.scope = ctx.scope, .bindings = ctx.bindings, .boundary = ctx.boundary};
}

[[nodiscard]] LowerCtx attached(DetachedLower const& held) {
    return LowerCtx{.scope = held.scope.lock(), .bindings = held.bindings, .boundary = held.boundary};
}

// Builds the form's session when its element is declared, so its values can be read before the form is drawn.
// NOLINTNEXTLINE(readability-function-cognitive-complexity): one section per kind of form element and its sessions.
void ScreenCore::wireForm(ElementState& element) {
    DocNode const& node = *element.node;
    RtScope& scope = *element.scope;
    std::string const action{node.name("action")};
    Catalog const& catalog = session.options().catalog;
    std::optional<std::string> const schema = catalog.schema ? catalog.schema(action) : std::nullopt;
    if (!schema.has_value()) {
        report(node.path, "no schema for `" + action + "`: the catalog has none for a `form` to build from");
        return;
    }
    NameRef const* const alias = scope.find(node.name("model"));
    if (alias == nullptr || alias->model == nullptr) {
        report(node.path, "`" + std::string{node.name("model")} + "` is not a model alias");
        return;
    }
    forms::FieldOverlays overlays;
    // `fields` is one record part holding a `field` part per overlaid member.
    for (DocNodePtr const& record : node.partList("fields")) {
        for (DocNodePtr const& part : record->partList("fields")) {
            forms::FieldOverlay overlay = overlayOf(scope, *part, session.formatting());
            // A computed `decimals` is carried by the effect below alone: baking the value read now would make it
            // the fallback when the expression later gives no places, where the schema's places belong.
            if (ExprPtr const places = part->prop("decimals"); places && places->kind != ExprKind::Literal) {
                overlay.decimals.reset();
            }
            overlays.insert_or_assign(part->id, std::move(overlay));
        }
    }
    auto model = forms::FormModel::fromSchema(action, *schema, overlays);
    if (!model) {
        report(node.path,
               "the form of `" + action + "` does not build: " + model.error().path + ": " + model.error().message);
        return;
    }
    if (node.name("submit") == "explicit") {
        model = model->withSubmitMode(forms::SubmitMode::Explicit);
    } else if (node.name("submit") == "auto") {
        model = model->withSubmitMode(forms::SubmitMode::Automatic);
    }
    element.form = std::make_unique<forms::FormSession>(runtime(), *std::move(model), aliasSubmits(alias->model),
                                                        aliasCalls(alias->model), engineOptions());
    followFormatting(element, scope);
    deriveForm(element, alias->model);
    // An overlay's `decimals` follows its expression: the field keeps the draft as typed and checks it again against
    // the places the expression gives now.
    for (DocNodePtr const& record : node.partList("fields")) {
        for (DocNodePtr const& part : record->partList("fields")) {
            ExprPtr const places = part->prop("decimals");
            if (!places) {
                continue;
            }
            forms::FieldState* state = nullptr;
            try {
                state = &element.form->field(part->id);
            } catch (std::out_of_range const&) {
                continue;  // The overlay names a field the form does not have (an omitted one).
            }
            scope.nodes.effect([this, &scope, places, state] {
                int const now = placesOf(evalIn(scope, nullptr, places), session.formatting());
                runtime().untracked([&] {
                    state->places().set(now >= 0 ? std::optional<std::uint32_t>{static_cast<std::uint32_t>(now)}
                                                 : std::nullopt);
                });
            });
        }
    }
    if (ExprPtr const prefill = node.prop("prefill")) {
        // A prefill follows its expression. A value of the record being edited (the first value, or one under the
        // same `key`) keeps the fields the user changed; another record, a new `key`, or any later value of a form
        // without one, loads every field.
        forms::FormSession* const form = element.form.get();
        struct Followed {
            bool loaded = false;
            Value key;
        };
        auto const followed = std::make_shared<Followed>();
        scope.nodes.effect([this, &scope, prefill, key = node.prop("key"), form, followed] {
            Value const value = evalIn(scope, nullptr, prefill);
            if (value.isNull()) {
                return;
            }
            runtime().untracked([&] {
                Value const current = key ? evalIn(scope, nullptr, key) : Value{};
                bool const same = !followed->loaded || (key && current == followed->key);
                followed->loaded = true;
                followed->key = current;
                std::string const json = glz::write_json(valueToJson(value)).value_or(std::string{});
                if (same) {
                    form->refill(json);
                } else {
                    form->prefill(json);
                }
            });
        });
    }
}

namespace {

// Whether a field, or a member of it at any depth, is computed: what an evaluation round would bring back.
// NOLINTBEGIN(misc-no-recursion): an object's and a row's members are fields.
[[nodiscard]] bool computesAny(std::span<forms::FieldSpec const> fields) {
    return std::ranges::any_of(
        fields, [](forms::FieldSpec const& field) { return field.computed || computesAny(field.children); });
}
// NOLINTEND(misc-no-recursion)

}  // namespace

// A form's rounds of derived values: its action's evaluation while a member is computed, and its `derive` action when
// it names one, each sent through the form's alias, `deriveDebounce` (150 ms when absent) after the last commit on the
// session's timers. A path a derive reply names that is no derived member is reported once.
void ScreenCore::deriveForm(ElementState& element, ModelAlias* alias) {
    DocNode const& node = *element.node;
    std::string const action{node.name("action")};
    std::string const derive{node.name("derive")};
    bool const computes = computesAny(element.form->model().fields());
    if (derive.empty() && !computes) {
        return;
    }
    auto const call = [this, alias](std::string actionId, bool evaluate) {
        return [this, alias, actionId = std::move(actionId),
                evaluate](std::string const& body) -> async::Completion<std::string> {
            std::shared_ptr<ModelChannel> const channel = channelOf(*alias, *this);
            if (channel == nullptr) {
                auto [failed, promise] = async::Completion<std::string>::makeSettleable(&runtime().owner());
                promise.reject(std::make_exception_ptr(std::runtime_error{"the node's model alias is not bound"}));
                return std::move(failed);
            }
            return channel->execute(actionId, body, CallOptions{.evaluate = evaluate});
        };
    };
    forms::DeriveOptions options;
    if (computes) {
        options.evaluate = call(action, true);
    }
    if (!derive.empty()) {
        options.derive = call(derive, false);
    }
    options.scheduler = session.options().scheduler;
    if (ExprPtr const debounce = node.prop("deriveDebounce"); debounce && debounce->kind == ExprKind::Literal) {
        if (std::int64_t const* const millis = debounce->literal.asInt(); millis != nullptr && *millis >= 0) {
            options.debounce = std::chrono::milliseconds{*millis};
        }
    }
    options.refused = [this, path = node.path, derive](std::vector<std::string> const& paths) {
        for (std::string const& refused : paths) {
            report(
                path + "/derive",
                // NOLINTNEXTLINE(performance-inefficient-string-concatenation): builds one refusal message from five pieces, once, on the error path
                "the reply of `" + derive + "` names `" + refused + "`, which is not a derived member of the form");
        }
    };
    element.form->deriveWith(std::move(options));
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity): lowers a form: its fields, collections, actions and derives in document order, each reading the form state before
Node ScreenCore::lowerForm(DocNode const& node, LowerCtx const& ctx, ElementState* element) {
    forms::FormSession* const form = element == nullptr ? nullptr : element->form.get();
    if (form == nullptr) {
        return ui::text(Text{.text = std::string{node.name("action")}, .role = TextRole::Muted});
    }
    // The form's events, each run once per change of the count it watches.
    auto const watch = [this, &ctx](Commands const* commands, auto count, auto payload) {
        if (commands == nullptr || commands->empty()) {
            return;
        }
        auto const seen = std::make_shared<std::uint64_t>(runtime().untracked(count));
        ctx.scope->nodes.effect([this, held = detachedLower(ctx), commands, count, payload, seen] {
            std::uint64_t const now = count();
            if (now == *seen) {
                return;
            }
            *seen = now;
            runtime().untracked([&] { runEvent(commands, attached(held), payload()); });
        });
    };
    watch(
        node.event("onSubmit"), [form] { return form->submitAttempts(); },
        [form] {
            std::optional<std::string> const body = form->body();
            return body.has_value() ? jsonTextValue(*body) : Value{};
        });
    watch(
        node.event("onSuccess"), [form] { return form->successCount(); },
        [form] {
            std::optional<std::string> const& reply = form->lastReply();
            return reply.has_value() ? jsonTextValue(*reply) : Value{};
        });
    if (Commands const* const onError = node.event("onError"); onError != nullptr && !onError->empty()) {
        // Each failure is a new exception: run once per failure, with its text.
        auto const seen =
            std::make_shared<std::exception_ptr>(runtime().untracked([form] { return form->lastError(); }));
        ctx.scope->nodes.effect([this, held = detachedLower(ctx), onError, form, seen] {
            std::exception_ptr const error = form->lastError();
            if (error == nullptr || error == *seen) {
                return;
            }
            *seen = error;
            runtime().untracked([&] { runEvent(onError, attached(held), Value{failureText(error)}); });
        });
    }
    forms::FormViewOptions options{.showTitle = false};
    presentForm(node, ctx, options);
    options.title = bindProp<std::string>(ctx, node.prop("title"), std::string{}, stringOf);
    // A field whose overlay names a component is drawn by it; only top-level fields take one, as every overlay.
    for (DocNodePtr const& record : node.partList("fields")) {
        for (DocNodePtr const& overlay : record->partList("fields")) {
            if (!overlay->name("component").empty()) {
                options.overrides.byField(overlay->id,
                                          [this, ctx, field = overlay.get()](forms::FieldView const& view) {
                                              return fieldComponent(view, *field, ctx);
                                          });
            }
            // A `grid` part makes the field a grid table, kept on the form's element by the field's name so every
            // mount of the form shows the same grid; the scope is held weakly, as the grid's is.
            if (std::span<DocNodePtr const> const grid = overlay->partList("grid"); !grid.empty()) {
                options.overrides.byField(
                    overlay->id, [this, element, detached = detachedLower(ctx), part = grid.front().get(),
                                  name = overlay->id](forms::FieldView const& view) {
                        LowerCtx const live = attached(detached);
                        if (live.scope == nullptr) {
                            return ui::text(Text{.text = view.state().path(), .role = TextRole::Muted});
                        }
                        std::unique_ptr<ElementState>& slot = element->fieldGrids[name];
                        if (slot == nullptr) {
                            slot = std::make_unique<ElementState>(runtime());
                            slot->node = part;
                            slot->scope = element->scope;
                        }
                        return formGridTable(*part, live, *slot, view.session(), view.state());
                    });
            }
        }
    }
    Column column{.children = {forms::formView(*form, std::move(options))}};
    commonOf(node, ctx, element, column.common);
    return ui::column(std::move(column));
}

// What a grid's part gives beyond its table: its columns, each a member its `field` names or a display column whose
// `value` is read with `row` the row's record, every member of the row type in schema order when it gives none;
// `enterMoves`, `walk`, and `onActivate`, run with the row's key on a read-only cell. What the columns and
// `onActivate` keep holds the scope weakly: the grid lives as long as the element the scope owns.
// NOLINTNEXTLINE(readability-function-cognitive-complexity): one section per key of a grid column.
void ScreenCore::gridParts(DocNode const& part, LowerCtx const& ctx, forms::FieldState const& array,
                           std::vector<GridColumn>& columns, GridOptions& options) {
    DetachedLower const detached = detachedLower(ctx);
    // An expression read in a row's scope, `row` its record of encoded members; null once the scope is gone. Tracked.
    auto const inRow = [detached](ExprPtr const& expr) {
        return [detached, expr](forms::RowState const& row) -> std::optional<std::pair<Value, FormatContext>> {
            LowerCtx const live = attached(detached);
            if (live.scope == nullptr) {
                return std::nullopt;
            }
            ValueRecord record;
            for (auto const& member : row.fields()) {
                forms::Encoding const& encoding = member->encoded().get();
                record.insert_or_assign(member->spec().name,
                                        encoding.has_value() && encoding->has_value() ? (*encoding)->value : Value{});
            }
            Bindings bindings = live.bindings != nullptr ? *live.bindings : Bindings{};
            bindings.push_back(Binding{.name = "row", .read = [record] { return Value{record}; }});
            return std::pair{evalIn(*live.scope, &bindings, expr), live.scope->formatting()};
        };
    };
    for (DocNodePtr const& column : part.partList("columns")) {
        ExprPtr const label = column->prop("label");
        GridColumn made{.id = column->id,
                        .label = label ? textOf(once(ctx, label), ctx.scope->formatting()) : column->id,
                        .field = std::string{column->name("field")},
                        .value = {}};
        if (ExprPtr const field = column->prop("field")) {
            // One member per row: a name that is no member of the row type is reported once, and its cell is
            // read-only.
            std::vector<std::string> members;
            for (forms::FieldSpec const& member : array.spec().children) {
                members.push_back(member.name);
            }
            made.fieldOf = [this, read = inRow(field), members = std::move(members),
                            path = field->path](forms::RowState const& row) {
                auto const readRow = read(row);
                std::string const name = readRow ? textOf(readRow->first, readRow->second) : std::string{};
                if (!name.empty() && std::ranges::find(members, name) == members.end()) {
                    report(path, "`" + name + "` names no member of the row");
                }
                return name;
            };
        }
        ExprPtr const value = column->prop("value");
        if (made.field.empty() && !made.fieldOf && value) {
            made.value = [read = inRow(value)](forms::RowState const& row) {
                auto const readRow = read(row);
                return readRow ? textOf(readRow->first, readRow->second) : std::string{};
            };
        }
        // Overlays per row, as a form field's overlays, each read in the row's scope.
        if (ExprPtr const decimals = column->prop("decimals")) {
            made.decimals = [read = inRow(decimals)](forms::RowState const& row) -> std::optional<std::uint32_t> {
                auto const readRow = read(row);
                int const places = readRow ? placesOf(readRow->first, readRow->second) : -1;
                return places >= 0 ? std::optional{static_cast<std::uint32_t>(places)} : std::nullopt;
            };
        }
        if (ExprPtr const unit = column->prop("unit")) {
            made.unit = [read = inRow(unit)](forms::RowState const& row) -> std::optional<std::string> {
                auto const readRow = read(row);
                std::string const* const columnId = readRow ? readRow->first.asString() : nullptr;
                return columnId != nullptr && !columnId->empty() ? std::optional{*columnId} : std::nullopt;
            };
        } else if (std::string_view const unitName = column->name("unit"); !unitName.empty()) {
            made.unit = [unitId = std::string{unitName}](forms::RowState const&) { return std::optional{unitId}; };
        }
        if (ExprPtr const readonly = column->prop("readonly")) {
            made.readonly = [read = inRow(readonly)](forms::RowState const& row) {
                auto const readRow = read(row);
                bool const* const flag = readRow ? readRow->first.asBool() : nullptr;
                return flag != nullptr && *flag;
            };
        }
        if (ExprPtr const offered = column->prop("options")) {
            made.options =
                [read = inRow(offered)](forms::RowState const& row) -> std::optional<std::vector<std::string>> {
                auto const readRow = read(row);
                ValueList const* const items = readRow ? readRow->first.asList() : nullptr;
                if (items == nullptr) {
                    return std::nullopt;
                }
                std::vector<std::string> kept;
                for (Value const& item : *items) {
                    kept.push_back(glz::write_json(valueToJson(item)).value_or(std::string{}));
                }
                return kept;
            };
        }
        if (ExprPtr const placeholder = column->prop("placeholder")) {
            made.placeholder = [read = inRow(placeholder)](forms::RowState const& row) {
                auto const readRow = read(row);
                return readRow ? textOf(readRow->first, readRow->second) : std::string{};
            };
        }
        columns.push_back(std::move(made));
    }
    if (columns.empty()) {
        for (forms::FieldSpec const& member : array.spec().children) {
            columns.push_back(GridColumn{.id = member.name, .label = member.title, .field = member.name, .value = {}});
        }
    }
    gridOptions(part, ctx, options);
}

// What any grid's part gives of its walk and its activation: `enterMoves`, `walk`, `appendRows`, `rowsFixed`, the
// session's paste and copy bounds and its timers, and `onActivate`, run with the row's key on a read-only cell, which
// holds the scope weakly.
void ScreenCore::gridOptions(DocNode const& part, LowerCtx const& ctx, GridOptions& options) {
    DetachedLower const detached = detachedLower(ctx);
    std::string_view const enter = part.name("enterMoves");
    options.enterMoves =
        // NOLINTNEXTLINE(readability-avoid-nested-conditional-operator): a three-way map of the enterMoves text to the enum
        enter == "right" ? EnterMoves::Right : (enter == "none" ? EnterMoves::None : EnterMoves::Down);
    options.walk = part.name("walk") == "columns" ? Walk::Columns : Walk::Rows;
    options.appendRows = part.flag("appendRows");
    options.rowsFixed = part.flag("rowsFixed");
    options.maxPasteCells = session.options().maxPasteCells;
    options.maxCopyRows = session.options().maxCopyRows;
    options.scheduler = session.options().scheduler;
    options.undoHold = kStatusMessageHold;
    if (Commands const* const commands = part.event("onActivate"); commands != nullptr && !commands->empty()) {
        options.onActivate = [this, detached, commands](Key const& key) {
            LowerCtx const live = attached(detached);
            if (live.scope != nullptr) {
                runEvent(commands, live, valueOfKey(key));
            }
        };
    }
}

// A grid of a form named by a table's `form` and `field`.
Node ScreenCore::lowerFormGrid(DocNode const& node, LowerCtx const& ctx, ElementState* element) {
    NameRef const* const named = ctx.scope->find(node.name("form"));
    forms::FormSession* const form =
        named != nullptr && named->element != nullptr ? named->element->form.get() : nullptr;
    if (form == nullptr || element == nullptr) {
        return ui::text(Text{.text = std::string{node.name("field")}, .role = TextRole::Muted});
    }
    forms::FieldState* array = nullptr;
    try {
        array = &form->field(node.name("field"));
    } catch (std::out_of_range const&) {
        report(node.path, "`" + std::string{node.name("field")} + "` is not a field of the form");
        return ui::text(Text{.text = std::string{node.name("field")}, .role = TextRole::Muted});
    }
    return formGridTable(node, ctx, *element, *form, *array);
}

[[nodiscard]] std::expected<forms::detail::engine::Json, std::string> changeWire(std::string const& payload,
                                                                                 std::string_view type) {
    std::string_view text{payload};
    if (type == "string") {
        if (text.starts_with("s:")) {
            text.remove_prefix(2);
        }
        // Parentheses: braces would pick the initializer-list constructor and make a one-element array.
        return forms::detail::engine::Json(std::string{text});
    }
    if (text.starts_with("i:")) {
        text.remove_prefix(2);
    }
    std::optional<forms::detail::engine::Json> json = forms::detail::engine::parseJson(text);
    if (!json) {
        return std::unexpected(std::string{"it is not JSON"});
    }
    bool const fits = type == "json" || json->is_null() || (type == "bool" && json->is_boolean()) ||
                      ((type == "int" || type == "number") && json->is_number()) ||
                      (type == "exact" && (json->is_number() || json->is_string()));
    if (!fits) {
        bool const vowel = type == "int" || type == "exact";
        return std::unexpected("it is not " + std::string{vowel ? "an " : "a "} + std::string{type});
    }
    return *std::move(json);
}

namespace {

/// @brief What a form fills of a field component's props: the field's value, errors, read-only and required state,
///        and label, each a tracked read of the field.
/// @param prop  The prop's name.
/// @param state The field.
/// @return The read, or empty for a prop the form does not fill.
[[nodiscard]] std::function<Value()> formFilled(std::string const& prop, forms::FieldState const* state) {
    if (prop == "value") {
        return [state] {
            forms::Encoding const& encoding = state->encoded().get();
            if (!encoding.has_value() || !encoding->has_value()) {
                return Value{};
            }
            return (*encoding)->value.isNull() ? jsonTextValue((*encoding)->json) : (*encoding)->value;
        };
    }
    if (prop == "errors") {
        return [state] {
            std::optional<forms::FieldError> const& error = state->error().get();
            return error.has_value() ? Value{ValueList{Value{error->message}}} : Value{ValueList{}};
        };
    }
    if (prop == "readonly") {
        return [state] { return Value{state->readonly().get()}; };
    }
    if (prop == "required") {
        return [state] { return Value{state->required().get()}; };
    }
    if (prop == "label") {
        return [state] { return Value{state->label()}; };
    }
    return {};
}

}  // namespace

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters): a prop's name, then its declared type.
CustomProp ScreenCore::componentProp(std::string const& prop, std::string const& type, std::function<Value()> read) {
    if (type == "bool") {
        return CustomProp{
            .name = prop,
            .value = slotProp<bool>([this, read = std::move(read)] { return boolOf(read(), session.formatting()); }),
        };
    }
    if (type == "number") {
        return CustomProp{
            .name = prop,
            .value = slotProp<double>([read = std::move(read)] { return componentNumber(read()); }),
        };
    }
    return CustomProp{
        .name = prop,
        .value = slotProp<std::string>(
            [this, read = std::move(read), type] { return componentText(read(), type, session.formatting()); }),
    };
}

void ScreenCore::changeField(forms::FieldState& state, forms::FieldSpec const& spec, std::string const& path,
                             std::string_view valueType, std::string const& payload) {
    auto wire = changeWire(payload, valueType);
    // A base64 member holds a file's bytes, which a document moves inline only under the session's cap.
    if (std::size_t const cap = session.options().maxInlineFileBytes;
        wire && spec.bytes && wire->is_string() && base64Size(wire->get_string()) > cap) {
        report(path, "field " + spec.name + ": " + std::to_string(base64Size(wire->get_string())) +
                         " bytes is over the " + std::to_string(cap) + " inline cap");
        return;
    }
    if (wire) {
        if (auto edited = state.edit(*wire); !edited) {
            wire = std::unexpected(edited.error());
        }
    }
    if (!wire) {
        report(path, "field " + spec.name + ": the component's change does not decode: " + wire.error());
    }
}

Node ScreenCore::fieldComponent(forms::FieldView const& view, DocNode const& overlay, LowerCtx const& ctx) {
    std::string const name{overlay.name("component")};
    // A loaded document's field component is declared, with a `value` prop and a `change` event.
    Component const& component = _doc->components.at(name);
    forms::FieldState* const state = &view.state();
    Custom spec{
        .name = name,
        .fallback = view.defaultControl(),
        .props = {},
        .minVersion =
            static_cast<int>(std::clamp<std::int64_t>(component.version, 0, std::numeric_limits<int>::max())),
        .onEvent = {},
    };
    for (auto const& [prop, type] : component.props) {
        if (prop == "value" && type == "json") {
            // The wire JSON itself, so an object's or a collection's every digit reaches the component.
            spec.props.push_back(CustomProp{
                .name = prop,
                .value = slotProp<std::string>([state] {
                    forms::Encoding const& encoding = state->encoded().get();
                    return encoding.has_value() && encoding->has_value() ? (*encoding)->json : std::string{"null"};
                }),
            });
            continue;
        }
        std::function<Value()> read = formFilled(prop, state);
        if (!read) {
            ExprPtr given;
            for (DocNodePtr const& record : overlay.partList("props")) {
                if (ExprPtr const expr = record->prop(prop)) {
                    given = expr;
                }
            }
            read = [scope = ctx.scope, bindings = ctx.bindings, given] {
                return given ? evalIn(*scope, bindings.get(), given) : Value{};
            };
        }
        spec.props.push_back(componentProp(prop, type, std::move(read)));
    }
    spec.onEvent = [this, state, path = overlay.path, fieldSpec = &view.spec(),
                    valueType = component.props.at("value")](std::string const& event, std::string const& payload) {
        if (event == "change") {
            changeField(*state, *fieldSpec, path, valueType, payload);
        }
    };
    return ui::custom(std::move(spec));
}

// Every call goes through the alias's channel, bound for its current instance when the call is made.
forms::Submitter ScreenCore::aliasCalls(ModelAlias* alias) {
    return [this, alias](std::string_view actionId, std::string const& body) -> async::Completion<std::string> {
        std::shared_ptr<ModelChannel> const channel = channelOf(*alias, *this);
        if (channel == nullptr) {
            auto [failed, promise] = async::Completion<std::string>::makeSettleable(&runtime().owner());
            promise.reject(std::make_exception_ptr(std::runtime_error{"the node's model alias is not bound"}));
            return std::move(failed);
        }
        return channel->execute(std::string{actionId}, body, CallOptions{});
    };
}

// The calls of a form, collection or wizard: a success other than of @p read, the collection's own query, tells the
// session, which applies the action's effects, as a mutation's success does.
forms::Submitter ScreenCore::aliasSubmits(ModelAlias* alias, std::string read) {
    return [this, alias, call = aliasCalls(alias), read = std::move(read)](
               std::string_view actionId, std::string body) -> async::Completion<std::string> {
        async::Completion<std::string> completion = call(actionId, std::move(body));
        if (actionId != read) {
            completion.then([this, alive = std::weak_ptr<bool>{_alive}, alias, typeId = alias->decl->type,
                             action = std::string{actionId}](std::string const& reply) {
                if (alive.expired()) {
                    return;
                }
                auto json = parseJson(reply);
                auto value = json ? valueFromJson(*json) : std::unexpected(json.error());
                std::string const token = runtime().untracked([&] { return alias->token->get(); });
                session.actionSucceeded(typeId, action, value ? *value : Value{}, ReplyUse::Applied, token);
            });
        }
        return completion;
    };
}

// The session's locale, translations, display zone and date patterns, as every form the engine builds here reads them.
forms::FormSessionOptions ScreenCore::engineOptions() const {
    FormatContext const& format = session.formatting();
    forms::FormSessionOptions options{};
    options.locale = withoutGrouping(format.numeric);
    options.bcp47 = format.locale;
    options.translations = format.translations;
    options.displayOffsetMinutes = format.utcOffsetMinutes;
    options.datePattern = format.datePattern;
    options.dateTimePattern = format.dateTimePattern;
    return options;
}

// Keeps a form's, collection's or wizard's number locale, date patterns, translations and locale at the session's
// current formatting, so its field texts follow a `setLocale` or `setTranslations` without a remount. The
// engine's number locale holds views into the facts it was given, so the effect also holds the facts it last applied.
void ScreenCore::followFormatting(ElementState& element, RtScope& scope) {
    auto const held = std::make_shared<std::shared_ptr<FormatContext const>>();
    forms::FormSession* const form = element.form.get();
    forms::CollectionSession* const collection = element.collection.get();
    forms::WizardSession* const wizard = element.wizard.get();
    scope.nodes.effect([this, held, form, collection, wizard] {
        std::shared_ptr<FormatContext const> const format = session.formattingHandle();
        runtime().untracked([&] {
            render::NumericLocale const locale = withoutGrouping(format->numeric);
            if (form != nullptr) {
                form->setFormat(locale, format->datePattern, format->dateTimePattern);
                form->setTranslations(format->translations, format->locale);
            }
            if (collection != nullptr) {
                collection->setFormat(locale, format->datePattern, format->dateTimePattern);
                collection->setTranslations(format->translations, format->locale);
            }
            if (wizard != nullptr) {
                wizard->setFormat(locale, format->datePattern, format->dateTimePattern);
                wizard->setTranslations(format->translations, format->locale);
            }
            *held = format;
        });
    });
}

// Builds a collection's or wizard's session when its element is declared. The node's `action` is the collection's
// query or the wizard's first step; the catalog answers the document for it, and every call (the query, the actions,
// every step's submission) goes through the node's model alias.
void ScreenCore::wireComposite(ElementState& element) {
    DocNode const& node = *element.node;
    RtScope& scope = *element.scope;
    bool const collection = node.kind == "collection";
    std::string const action{node.name("action")};
    Catalog const& catalog = session.options().catalog;
    auto const& lookup = collection ? catalog.view : catalog.wizard;
    std::optional<std::string> const document = lookup ? lookup(action) : std::nullopt;
    if (!document.has_value()) {
        report(node.path, std::string{"no "} + (collection ? "view" : "wizard") + " document for `" + action +
                              "`: the catalog has none for a `" + node.kind + "` to build from");
        return;
    }
    NameRef const* const alias = scope.find(node.name("model"));
    if (alias == nullptr || alias->model == nullptr) {
        report(node.path, "`" + std::string{node.name("model")} + "` is not a model alias");
        return;
    }
    forms::SchemaLookup const schemas = catalog.schema ? forms::SchemaLookup{catalog.schema} : forms::SchemaLookup{};
    forms::Submitter const call = aliasCalls(alias->model);
    if (collection) {
        auto model = forms::CollectionModel::fromSchema(*document);
        if (!model) {
            report(node.path, "the collection of `" + action + "` does not build: " + model.error().path + ": " +
                                  model.error().message);
            return;
        }
        if (model->query() != action) {
            report(node.path, "the view document of `" + action + "` lists `" + std::string{model->query()} + "`");
            return;
        }
        std::string query{model->query()};
        element.collection = std::make_unique<forms::CollectionSession>(runtime(), *std::move(model), schemas,
                                                                        aliasSubmits(alias->model, std::move(query)),
                                                                        call, engineOptions());
        followFormatting(element, scope);
        wireCollectionTable(element, alias->model->decl->type);
        return;
    }
    // The wizard's id, the stem of its translation keys, is the action it starts with.
    auto model = forms::WizardModel::fromSchema(action, *document);
    if (!model) {
        report(node.path,
               "the wizard of `" + action + "` does not build: " + model.error().path + ": " + model.error().message);
        return;
    }
    if (model->steps().front().action != action) {
        report(node.path,
               "the wizard document of `" + action + "` starts with `" + model->steps().front().action + "`");
        return;
    }
    try {
        element.wizard = std::make_unique<forms::WizardSession>(runtime(), *std::move(model), schemas,
                                                                aliasSubmits(alias->model), call, engineOptions());
        followFormatting(element, scope);
    } catch (std::invalid_argument const& error) {
        report(node.path, error.what());
    }
}

// The presentation keys a form, a collection and a wizard share, but the title: the label of the explicit-mode button
// and the status line naming it, bound in the node's scope; whether the status line shows, the grid's columns and the
// field layout and the inline caption column's width, as read at load.
void ScreenCore::presentForm(DocNode const& node, LowerCtx const& ctx, forms::FormViewOptions& options) {
    if (ExprPtr const label = node.prop("submitLabel")) {
        options.submitLabel = bindProp<std::string>(ctx, label, std::string{}, stringOf);
    }
    if (auto const shown = node.flags.find("showStatus"); shown != node.flags.end()) {
        options.showStatus = shown->second;
    }
    if (ExprPtr const columns = node.prop("columns"); columns != nullptr && columns->literal.asInt() != nullptr) {
        options.gridColumns = static_cast<int>(*columns->literal.asInt());
        options.flatGridColumns = options.gridColumns;
    }
    if (node.name("fieldLayout") == "inline") {
        options.fieldLayout = forms::FieldLayout::Inline;
    }
    // The loader has checked `captionWidth` is a literal integer.
    if (ExprPtr const width = node.prop("captionWidth")) {
        options.inlineLabelWidth = static_cast<int>(*width->literal.asInt());
    }
}

Node ScreenCore::lowerComposite(DocNode const& node, LowerCtx const& ctx, ElementState* element) {
    // The editor's and the steps' titles are the dialog's and the header's; the forms do not repeat them. A `title`
    // replaces the view's own heading.
    forms::FormViewOptions form{};
    form.showTitle = false;
    presentForm(node, ctx, form);
    Prop<std::string> title = bindProp<std::string>(ctx, node.prop("title"), std::string{}, stringOf);
    Node view;
    if (element != nullptr && element->collection != nullptr) {
        forms::CollectionViewOptions options{};
        options.form = std::move(form);
        options.title = std::move(title);
        // The rows are a table as every other table of the screen is.
        if (element->rowsTable != nullptr) {
            options.chrome.rows = [this, ctx, element](forms::CollectionSession& /*collection*/,
                                                       std::function<Node(std::string const&)> const& buttons) {
                return lowerCollectionRows(*element, ctx, buttons);
            };
        }
        view = forms::collectionView(*element->collection, std::move(options));
    } else if (element != nullptr && element->wizard != nullptr) {
        forms::WizardViewOptions options{};
        options.form = std::move(form);
        options.title = std::move(title);
        view = forms::wizardView(*element->wizard, options);
    } else {
        return ui::text(Text{.text = std::string{node.name("action")}, .role = TextRole::Muted});
    }
    Column column{.children = {std::move(view)}};
    commonOf(node, ctx, element, column.common);
    return ui::column(std::move(column));
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity): an overlay's scope, content, actions and keys are one lowering.
// NOLINTNEXTLINE(readability-function-cognitive-complexity): an overlay's scope, content, actions and keys are one lowering.
Node ScreenCore::lowerDialog(DocNode const& node, LowerCtx const& ctx, ElementState* element) {
    Prop<bool> open = node.prop("open") ? bindProp<bool>(ctx, node.prop("open"), false, boolOf)
                                        : slotProp<bool>([element] { return element->open.get(); });
    std::vector<DocNode const*> bodyContent;
    for (DocNodePtr const& child : node.nodes("children")) {
        bodyContent.push_back(child.get());
    }
    int const bodyGap =
        node.prop("gap") ? static_cast<int>(intOf(once(ctx, node.prop("gap")), kDialogGap)) : kDialogGap;
    LayoutHints const bodyHints = holderHints(bodyContent, node.scope ? &*node.scope : nullptr, ctx);
    Node body = lazy(
        [this, ctx, &node, element, bodyGap] -> Node {
            auto const inner = std::make_shared<RtScope>(*this, ctx.scope, nullptr, ctx.scope->nodes.depth() + 2);
            inner->owner = element;
            addPending(1);
            inner->submitted = &element->submitted;
            inner->showErrors = std::string{node.name("showErrors")};
            std::vector<DocNode const*> content;
            for (DocNodePtr const& child : node.nodes("children")) {
                content.push_back(child.get());
            }
            std::vector<Commands const*> lists;
            for (std::string_view const key : {"default", "cancel"}) {
                if (Commands const* const commands = node.event(key)) {
                    lists.push_back(commands);
                }
            }
            for (DocNodePtr const& action : node.partList("actions")) {
                if (Commands const* const commands = action->event("onClick")) {
                    lists.push_back(commands);
                }
            }
            declare(*inner, node.scope ? &*node.scope : nullptr, content, lists);
            element->inner.set(inner.get());
            LowerCtx const innerCtx{
                .scope = inner,
                .bindings = ctx.bindings,
                .boundary = ctx.boundary,
                .listGap = bodyGap,
            };
            std::vector<Node> children;
            children.reserve(content.size());
            for (DocNode const* const child : content) {
                children.push_back(lower(*child, innerCtx));
            }
            // The overlay's actions end its scoped content, so they bind in its scope; the mount puts them in its
            // footer.
            if (ActionModels actions = lowerActionParts(node.partList("actions"), innerCtx); !actions.empty()) {
                children.push_back(actionBarOf(std::move(actions)));
            }
            std::vector<KeyBinding> keys;
            if (Commands const* const commands = node.event("default")) {
                keys.push_back(KeyBinding{
                    .chord = "Enter",
                    .onPress = [this, innerCtx, commands] { runEvent(commands, innerCtx, Value{}); },
                });
            }
            if (Commands const* const commands = node.event("cancel")) {
                keys.push_back(KeyBinding{
                    .chord = "Esc",
                    .onPress = [this, innerCtx, commands] { runEvent(commands, innerCtx, Value{}); },
                });
            }
            fillHolder(children, node.path);
            return holding(std::move(children), inner, std::move(keys), bodyGap);
        },
        bodyHints);
    Action onDismiss = [this, ctx, element, commands = node.event("onDismiss")] {
        session.asUser([&] {
            session.noteWrite();
            element->open.set(false);
        });
        runEvent(commands, ctx, Value{});
    };
    if (node.kind == "drawer") {
        ExprPtr const side = node.prop("side");
        Drawer spec{
            .open = std::move(open),
            .title = bindProp<std::string>(ctx, node.prop("title"), std::string{}, stringOf),
            .description = bindProp<std::string>(ctx, node.prop("description"), std::string{}, stringOf),
            .child = std::move(body),
            .onDismiss = std::move(onDismiss),
            .side = side && textOf(once(ctx, side), ctx.scope->formatting()) == "start" ? Side::Start : Side::End,
            .headNote = bindProp<std::string>(ctx, node.prop("headNote"), std::string{}, stringOf),
            .footerNote = bindProp<std::string>(ctx, node.prop("footerNote"), std::string{}, stringOf),
            .closeButton = bindProp<bool>(ctx, node.prop("closeButton"), false, boolOf),
            .dismissLabel = bindProp<std::string>(ctx, node.prop("dismissLabel"), std::string{"Close"}, stringOf),
        };
        if (ExprPtr const spans = node.prop("descriptionSpans")) {
            spec.descriptionSpans = spansProp(ctx, spans);
        }
        commonOf(node, ctx, element, spec.common);
        return ui::drawer(std::move(spec));
    }
    Dialog spec{
        .open = std::move(open),
        .title = bindProp<std::string>(ctx, node.prop("title"), std::string{}, stringOf),
        .description = bindProp<std::string>(ctx, node.prop("description"), std::string{}, stringOf),
        .child = std::move(body),
        .onDismiss = std::move(onDismiss),
        .common = {},  // NOLINT(readability-trailing-comma): the comma ends the designator, not the empty braces.
        .sheet = bindProp<bool>(ctx, node.prop("sheet"), false, boolOf),
        .dismissLabel = bindProp<std::string>(ctx, node.prop("dismissLabel"), std::string{"Close"}, stringOf),
        .headNote = bindProp<std::string>(ctx, node.prop("headNote"), std::string{}, stringOf),
        .footerNote = bindProp<std::string>(ctx, node.prop("footerNote"), std::string{}, stringOf),
        .closeButton = bindProp<bool>(ctx, node.prop("closeButton"), false, boolOf),
    };
    if (ExprPtr const spans = node.prop("descriptionSpans")) {
        spec.descriptionSpans = spansProp(ctx, spans);
    }
    commonOf(node, ctx, element, spec.common);
    return ui::dialog(std::move(spec));
}

Node ScreenCore::lowerSwitch(DocNode const& node, LowerCtx const& ctx) {
    std::vector<ExprPtr> whens;
    Switch spec;
    std::int64_t index = 0;
    for (DocNodePtr const& part : node.partList("cases")) {
        whens.push_back(part->prop("when"));
        DocNode const* const casePart = part.get();
        std::vector<DocNode const*> caseContent;
        for (DocNodePtr const& child : casePart->nodes("children")) {
            caseContent.push_back(child.get());
        }
        LayoutHints const caseHints = holderHints(caseContent, casePart->scope ? &*casePart->scope : nullptr, ctx);
        spec.cases.push_back(SwitchCase{
            .key = Key{index++},
            .node = lazy(
                [this, ctx, casePart, caseGap = ctx.listGap] -> Node {
                    auto const inner =
                        std::make_shared<RtScope>(*this, ctx.scope, nullptr, ctx.scope->nodes.depth() + 2);
                    std::vector<DocNode const*> content;
                    for (DocNodePtr const& child : casePart->nodes("children")) {
                        content.push_back(child.get());
                    }
                    declare(*inner, casePart->scope ? &*casePart->scope : nullptr, content, {});
                    LowerCtx const innerCtx{
                        .scope = inner,
                        .bindings = ctx.bindings,
                        .boundary = ctx.boundary,
                        .listGap = caseGap,
                    };
                    std::vector<Node> children;
                    children.reserve(content.size());
                    for (DocNode const* const child : content) {
                        children.push_back(lower(*child, innerCtx));
                    }
                    fillHolder(children, casePart->path);
                    return holding(std::move(children), inner, {}, caseGap);
                },
                caseHints),
        });
    }
    spec.selector = slotProp<Key>(
        [scope = ctx.scope, bindings = ctx.bindings, selected = node.prop("on"), whens = std::move(whens)] {
            Value const value = evalIn(*scope, bindings.get(), selected);
            for (std::size_t at = 0; at < whens.size(); ++at) {
                if (evalIn(*scope, bindings.get(), whens.at(at)) == value) {
                    return Key{static_cast<std::int64_t>(at)};
                }
            }
            return Key{std::int64_t{-1}};
        });
    auto const fallback = node.nodes("default");
    spec.fallback = fallback.empty() ? Node{} : lowerAll(fallback, ctx, ctx.listGap);
    commonOf(node, ctx, nullptr, spec.common);
    return ui::switchOf(std::move(spec));
}

// The tab a bound `selected` names. A value that names no tab shows the first, and is reported once.
Prop<std::size_t> ScreenCore::controlledTabSelection(DocNode const& node, LowerCtx const& ctx, ExprPtr const& selected,
                                                     std::shared_ptr<std::vector<std::string>> const& ids) {
    auto const unknownReported = std::make_shared<bool>(false);
    return Prop<std::size_t>{[this, scope = ctx.scope, bindings = ctx.bindings, selected, ids, unknownReported,
                              path = pointerAppend(node.path, "selected")] -> std::size_t {
        Value const value = evalIn(*scope, bindings.get(), selected);
        std::string const* const named = value.asString();
        auto const found = named == nullptr ? ids->end() : std::ranges::find(*ids, *named);
        if (found != ids->end()) {
            return static_cast<std::size_t>(std::distance(ids->begin(), found));
        }
        if (!*unknownReported) {
            *unknownReported = true;
            report(path, canonicalJson(value) + " names no tab; the first is shown");
        }
        return 0;
    }};
}

// A document names tabs by id; the view tree by index, so the ids map to indices once. A bound `selected` is the
// controlled index: a pick runs `onChoose` and the page follows `selected` alone. Without it the node keeps the
// user's pick itself, as a bound index too, whose page follows the pick: a constant would move only the highlight.
Node ScreenCore::lowerTabs(DocNode const& node, LowerCtx const& ctx, ElementState* element) {
    Tabs spec;
    auto const ids = std::make_shared<std::vector<std::string>>();
    for (DocNodePtr const& tab : node.partList("tabs")) {
        auto const nodes = tab->nodes("children");
        ids->push_back(tab->id);
        spec.tabs.push_back(Tab{
            .label = bindProp<std::string>(ctx, tab->prop("label"), std::string{}, stringOf),
            .node = nodes.empty() ? Node{} : lowerAll(nodes, ctx),
        });
    }
    spec.moreLabel = bindProp<std::string>(ctx, node.prop("moreLabel"), std::string{"More"}, stringOf);
    if (ExprPtr const inset = node.prop("inset")) {
        spec.inset = static_cast<int>(intOf(once(ctx, inset), 0));
    }
    if (ExprPtr const pageInset = node.prop("pageInset")) {
        spec.pageInset = static_cast<int>(intOf(once(ctx, pageInset), 0));
    }
    spec.stripSurface = std::string{node.name("stripSurface")};
    ExprPtr const selected = node.prop("selected");
    Commands const* const onChoose = node.event("onChoose");
    bool const controlled = selected && (selected->kind != ExprKind::Literal || !selected->literal.isNull());
    auto const picked = std::make_shared<reactive::Signal<std::size_t>>(runtime(), std::size_t{0});
    spec.selected = controlled ? controlledTabSelection(node, ctx, selected, ids)
                               : Prop<std::size_t>{[picked] { return picked->get(); }};
    spec.onSelect = [this, ctx, onChoose, ids, picked, controlled, element](std::size_t index) {
        if (!controlled) {
            picked->set(index);
            if (element != nullptr && index < ids->size()) {
                element->draft.set(Value{ids->at(index)});
            }
        }
        if (onChoose != nullptr && index < ids->size()) {
            runEvent(onChoose, ctx, Value{ids->at(index)});
        }
    };
    commonOf(node, ctx, element, spec.common);
    return ui::tabs(std::move(spec));
}

// Focuses the declaration the command's target resolved to: of several candidates (cases of one switch), the one
// that is mounted. Each candidate is keyed by the command's own enclosing row keys, as many as enclose the
// candidate; with `row`, the innermost of them is the row's key instead.
bool ScreenCore::focusTarget(Command const& command, RtScope const& scope, Bindings const* bindings) {
    if (!focusElement) {
        return false;
    }
    auto const found = _doc->focusTargets.find(command.path);
    if (found == _doc->focusTargets.end()) {
        return focusElement(command.target);
    }
    FocusPlace const place = focusPlace(scope);
    std::optional<std::string> row;
    if (found->second.row) {
        Value const value = evalIn(scope, bindings, command.extra);
        if (value.isNull()) {
            return false;
        }
        row = canonicalJson(valueOfKey(keyOf(value)));
    }
    for (FocusCandidate const& candidate : found->second.candidates) {
        std::size_t const kept = row ? candidate.rows - 1 : candidate.rows;
        if (candidate.rows == 0 && row) {
            continue;
        }
        if (kept > place.rows.size()) {
            continue;
        }
        FocusPlace candidatePlace{
            .prefix = place.prefix,
            .rows = {place.rows.begin(), std::next(place.rows.begin(), static_cast<std::ptrdiff_t>(kept))},
        };
        if (row) {
            candidatePlace.rows.push_back(*row);
        }
        if (focusElement(focusKey(candidatePlace, candidate.path))) {
            return true;
        }
    }
    return false;
}

std::shared_ptr<RtScope> ScreenCore::rowScope(DocNode const& node, LowerCtx const& ctx,
                                              reactive::Signal<Value> const& row,
                                              std::vector<DocNode const*> const& content, std::string key) {
    auto scope = std::make_shared<RtScope>(*this, ctx.scope, &row, ctx.scope->nodes.depth() + 2);
    scope->rowKey = std::move(key);
    declare(*scope, node.scope ? &*node.scope : nullptr, content, {});
    return scope;
}

[[nodiscard]] bool givesDisplay(DocNode const& part) {
    return part.prop("decimals") != nullptr || part.prop("labels") != nullptr || part.names.contains("rounding") ||
           part.names.contains("empty") || part.names.contains("unit") || part.names.contains("date") ||
           part.flags.contains("padPlaces") || part.flags.contains("grouping");
}

[[nodiscard]] table::ColumnDisplay displayOf(DocNode const& part, table::ColumnDisplay display,
                                             std::function<Value(ExprPtr const&)> const& read) {
    if (ExprPtr const decimals = part.prop("decimals")) {
        constexpr std::int64_t kMostPlaces = 18;
        Value const places = read(decimals);
        if (std::int64_t const* const count = places.asInt();
            count != nullptr && *count >= 0 && *count <= kMostPlaces) {
            display.decimals = static_cast<std::uint32_t>(*count);
        }
    }
    if (auto const rounding = part.names.find("rounding"); rounding != part.names.end()) {
        display.rounding = rounding->second == "halfUp" ? table::Rounding::HalfUp : table::Rounding::HalfEven;
    }
    if (auto const pad = part.flags.find("padPlaces"); pad != part.flags.end()) {
        display.padPlaces = pad->second;
    }
    if (auto const grouping = part.flags.find("grouping"); grouping != part.flags.end()) {
        display.grouping = grouping->second;
    }
    for (auto [key, member] :
         {std::pair{"empty", &display.empty}, std::pair{"unit", &display.unit}, std::pair{"date", &display.date}}) {
        if (auto const found = part.names.find(std::string_view{key}); found != part.names.end()) {
            *member = found->second;
        }
    }
    if (ExprPtr const labels = part.prop("labels")) {
        display.labels.clear();
        Value const list = read(labels);
        for (Value const& item : list.asList() != nullptr ? *list.asList() : ValueList{}) {
            Value const* const operand = item.field("operand");
            Value const* const text = item.field("text");
            if (operand == nullptr || text == nullptr || text->asString() == nullptr) {
                continue;
            }
            display.labels.push_back(table::ValueLabel{
                .operand = operand->asString() != nullptr ? *operand->asString() : canonicalJson(*operand),
                .text = *text->asString()});
        }
    }
    return display;
}

[[nodiscard]] table::Cell cellOfRecord(Value const* value, table::TableColumn const& column,
                                       table::UnitRegistry const* units) {
    using table::ColumnKind;
    if (value == nullptr || value->isNull()) {
        return std::monostate{};
    }
    bool const measure = column.info.kind == ColumnKind::Measure;
    if (std::int64_t const* const integer = value->asInt()) {
        return *integer;
    }
    if (Decimal const* const decimal = value->asDecimal()) {
        if (measure) {
            return table::Measure{.amount = decimal->value, .unit = table::kUnknownUnit};
        }
        return decimal->value;
    }
    if (bool const* const flag = value->asBool()) {
        return *flag;
    }
    if (Quantity const* const quantity = value->asQuantity()) {
        if (measure) {
            std::optional<table::UnitId> const unit = units != nullptr ? units->find(quantity->unit) : std::nullopt;
            return table::Measure{.amount = quantity->value, .unit = unit.value_or(table::kUnknownUnit)};
        }
        return quantity->value;
    }
    if (time::DateTime const* const instant = value->asTimestamp()) {
        return std::int64_t{
            std::chrono::duration_cast<std::chrono::seconds>(instant->value.time_since_epoch()).count()};
    }
    if (std::string const* const text = value->asString()) {
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access): `text->size() == 10` is the previous operand of this condition, so indices 4 and 7 are in range
        if (column.info.kind == ColumnKind::Date && text->size() == 10 && (*text)[4] == '-' && (*text)[7] == '-') {
            int year = 0;
            unsigned month = 0;
            unsigned day = 0;
            char const* const cursor = text->data();
            // NOLINTBEGIN(cppcoreguidelines-pro-bounds-pointer-arithmetic): from_chars takes a pointer range; the ranges are fixed offsets into the 10-character `YYYY-MM-DD` text the size and separator checks above validated
            bool const read = std::from_chars(cursor, cursor + 4, year).ec == std::errc{} &&
                              std::from_chars(cursor + 5, cursor + 7, month).ec == std::errc{} &&
                              std::from_chars(cursor + 8, cursor + 10, day).ec == std::errc{};
            // NOLINTEND(cppcoreguidelines-pro-bounds-pointer-arithmetic)
            std::chrono::year_month_day const date{std::chrono::year{year}, std::chrono::month{month},
                                                   std::chrono::day{day}};
            if (read && date.ok()) {
                return std::int64_t{std::chrono::sys_days{date}.time_since_epoch().count()};
            }
        }
        return *text;
    }
    return canonicalJson(*value);
}

[[nodiscard]] table::TableColumn columnFor(Value const& value, table::ColumnDisplay const& display) {
    using table::ColumnKind;
    ColumnKind const kind =
        value.asInt() != nullptr ? ColumnKind::Integer
        // NOLINTBEGIN(readability-avoid-nested-conditional-operator): a flat pick over the value's kind, one test per line in the order the kinds are tried
        : value.asDecimal() != nullptr   ? ColumnKind::Decimal
        : value.asQuantity() != nullptr  ? ColumnKind::Decimal
        : value.asBool() != nullptr      ? ColumnKind::Bool
        : value.asTimestamp() != nullptr ? ColumnKind::DateTime
                                         : ColumnKind::Text;
    // NOLINTEND(readability-avoid-nested-conditional-operator)
    return table::TableColumn{.info = {.id = {}, .kind = kind}, .display = display};
}

}  // namespace morph::ui::detail

// SPDX-License-Identifier: Apache-2.0

// Lowering a document's nodes to widgets.

#include "interpreter_detail.hpp"

namespace morph::ui::detail {

void ScreenCore::fillHolder(std::vector<Node>& children, std::string const& path) {
    for (Node& child : children) {
        if (layoutHintsOf(child).width.kind == Sizing::Kind::Percent) {
            report(path, kPercentInHolder);
            child = withWidth(child, Sizing::stretch(1));
        }
    }
}

LayoutHints ScreenCore::nodeHints(DocNode const& node, std::set<std::string, std::less<>> const& own,
                                  LowerCtx const& ctx) {
    LayoutHints hints;
    if (ExprPtr const sizing = node.prop("sizing")) {
        if (sizing->kind != ExprKind::Literal && readsAny(sizing, own)) {
            report(sizing->path,
                   "this sizing reads a name the case declares, so the case is held at its "
                   "content size");
        } else {
            Value const value = once(ctx, sizing);
            Value const* const width = value.field("width");
            Value const* const height = value.field("height");
            if (width != nullptr || height != nullptr) {
                hints.width = width != nullptr && isSizing(*width) ? sizingOf(*width) : Sizing::content();
                hints.height = height != nullptr && isSizing(*height) ? sizingOf(*height) : Sizing::content();
            } else if (isSizing(value)) {
                hints.width = sizingOf(value);
            }
        }
    }
    return hints;
}

LayoutHints ScreenCore::holderHints(std::span<DocNode const* const> content, ScopeDecl const* decl,
                                    LowerCtx const& ctx) {
    std::set<std::string, std::less<>> own;
    for (DocNode const* const node : content) {
        collectIds(*node, own);
    }
    if (decl != nullptr) {
        for (ModelDecl const& model : decl->models) {
            own.insert(model.name);
        }
        for (StateDecl const& state : decl->state) {
            own.insert(state.name);
        }
        for (LetDecl const& let : decl->lets) {
            own.insert(let.name);
        }
        for (QueryDecl const& query : decl->queries) {
            own.insert(query.name);
        }
        for (MutationDecl const& mutation : decl->mutations) {
            own.insert(mutation.name);
        }
    }
    std::vector<Sizing> widths;
    std::vector<Sizing> heights;
    for (DocNode const* const node : content) {
        LayoutHints const hints = nodeHints(*node, own, ctx);
        widths.push_back(hints.width);
        heights.push_back(hints.height);
    }
    LayoutHints out;
    out.width = holderSizing(widths);
    out.height = holderSizing(heights);
    return out;
}

// NOLINTBEGIN(misc-no-recursion): a menu nests, and lowering descends the document.
Node ScreenCore::lower(DocNode const& node, LowerCtx const& ctx) {
    if (recording) {
        if (loweredNodes.size() >= kPruneLoweredAt && loweredNodes.size() % kPruneLoweredAt == 0) {
            std::erase_if(loweredNodes, [](LoweredNode const& entry) { return entry.scope.expired(); });
        }
        loweredNodes.push_back(LoweredNode{.node = &node, .scope = ctx.scope, .bindings = ctx.bindings});
    }
    // Only a switch reads the gap of the list around it; every other kind's children are held by a holder that sets
    // their gap, so none sees its grandparent's.
    if (ctx.listGap != 0 && node.kind != "switch") {
        LowerCtx plain = ctx;
        plain.listGap = 0;
        return lowerKind(node, plain, elementFor(*ctx.scope, node));
    }
    return lowerKind(node, ctx, elementFor(*ctx.scope, node));
}

Prop<TextRole> ScreenCore::roleProp(LowerCtx const& ctx, ExprPtr const& expr) {
    if (!expr || expr->kind == ExprKind::Literal) {
        return bindProp<TextRole>(ctx, expr, TextRole::Normal, roleOf);
    }
    return slotProp<TextRole>([this, scope = ctx.scope, bindings = ctx.bindings, expr] {
        Value const value = evalIn(*scope, bindings.get(), expr);
        std::optional<TextRole> const role = roleNamed(textOf(value, scope->formatting()));
        if (!role) {
            report(expr->path, "role is " + canonicalJson(value) + "; " + std::string{kRoleNames});
        }
        return role.value_or(TextRole::Normal);
    });
}

Prop<std::string> ScreenCore::surfaceOf(DocNode const& node, LowerCtx const& ctx) {
    ExprPtr const expr = node.prop("surface");
    auto nameOf = [this, path = pointerAppend(node.path, "surface")](Value const& value) -> std::string {
        if (std::string const* const name = value.asString()) {
            return *name;
        }
        if (!value.isNull()) {
            report(path, "a surface is the name of one; " + canonicalJson(value) + " is not a string");
        }
        return {};
    };
    if (!expr || expr->kind == ExprKind::Literal) {
        return expr ? Prop<std::string>{nameOf(expr->literal)} : Prop<std::string>{std::string{}};
    }
    return slotProp<std::string>([scope = ctx.scope, bindings = ctx.bindings, expr, nameOf = std::move(nameOf)] {
        return nameOf(evalIn(*scope, bindings.get(), expr));
    });
}

KeyValueItem ScreenCore::keyValueItemOf(Value const& item, std::string const& path, RtScope const& scope) {
    FormatContext const& format = scope.formatting();
    Value const* const label = item.field("label");
    Value const* const value = item.field("value");
    KeyValueItem out{
        .label = label == nullptr ? std::string{} : textOf(*label, format),
        .value = value == nullptr ? std::string{} : textOf(*value, format),
    };
    if (Value const* const spans = item.field("spans")) {
        bool dropped = false;
        bool badTone = false;
        out.spans = spansOf(*spans, format, dropped, &badTone);
        if (dropped) {
            report(path, kSpansReason);
        }
        if (badTone) {
            report(path, kSpanToneReason);
        }
    }
    if (Value const* const tone = item.field("tone")) {
        std::optional<Tone> const named = toneNamed(*tone);
        if (!named) {
            report(path, "an item's tone is " + canonicalJson(*tone) + "; " + std::string{kToneNames});
        }
        out.tone = named.value_or(Tone::Neutral);
    }
    return out;
}

// A group needs no widget of its own: it lowers to kinds every backend has, so the view tree and the contract do not
// change. Its description is a line read in order after its children, since `Common` carries no accessible
// description.
Node ScreenCore::lowerGroup(DocNode const& node, LowerCtx const& ctx, ElementState* element,
                            std::vector<Node> children) {
    Text legend{
        .text = bindProp<std::string>(ctx, node.prop("label"), std::string{}, stringOf),
        .role = TextRole::Label,
    };
    legend.field.required = bindProp<bool>(ctx, node.prop("required"), false, boolOf);
    std::vector<Node> column;
    column.reserve(children.size() + 3);
    column.push_back(ui::text(std::move(legend)));
    std::ranges::move(children, std::back_inserter(column));
    if (ExprPtr const description = node.prop("description")) {
        Text line{.text = bindProp<std::string>(ctx, description, std::string{}, stringOf), .role = TextRole::Muted};
        line.common.visible = slotProp<bool>([scope = ctx.scope, bindings = ctx.bindings, description] {
            return !textOf(evalIn(*scope, bindings.get(), description), scope->formatting()).empty();
        });
        column.push_back(ui::text(std::move(line)));
    }
    // The shown errors, one line each: a check that targets the group shows its message here.
    auto const shown = [this, element, scope = ctx.scope, bindings = ctx.bindings, errors = node.prop("errors")] {
        return stringsOf(element != nullptr ? elementValue(*element, "shownErrors")
                                            : evalIn(*scope, bindings.get(), errors));
    };
    Text messages{
        .text = slotProp<std::string>([shown] {
            std::string joined;
            for (std::string const& message : shown()) {
                joined += (joined.empty() ? "" : "\n") + message;
            }
            return joined;
        }),
        .role = TextRole::Error,
    };
    messages.common.visible = slotProp<bool>([shown] { return !shown().empty(); });
    column.push_back(ui::text(std::move(messages)));
    Column spec{
        .children = std::move(column),
        .gap = static_cast<int>(intOf(node.prop("gap") ? once(ctx, node.prop("gap")) : Value{}, 0)),
    };
    commonOf(node, ctx, element, spec.common);
    if (spec.common.a11y.role.empty()) {
        spec.common.a11y.role = "group";
    }
    bool const named = std::ranges::any_of(node.partList("a11y"),
                                           [](DocNodePtr const& a11y) { return a11y->prop("name") != nullptr; });
    if (!named) {
        spec.common.a11y.name = bindProp<std::string>(ctx, node.prop("label"), std::string{}, stringOf);
    }
    return ui::column(std::move(spec));
}

Prop<std::vector<TextSpan>> ScreenCore::spansProp(LowerCtx const& ctx, ExprPtr const& expr) {
    return slotProp<std::vector<TextSpan>>([this, scope = ctx.scope, bindings = ctx.bindings, expr] {
        bool dropped = false;
        bool badTone = false;
        std::vector<TextSpan> spans =
            spansOf(evalIn(*scope, bindings.get(), expr), scope->formatting(), dropped, &badTone);
        if (dropped) {
            report(expr->path, kSpansReason);
        }
        if (badTone) {
            report(expr->path, kSpanToneReason);
        }
        return spans;
    });
}

// The size request: `sizing` (the width, or both dimensions), `alignSelf`, and the bounds.
void ScreenCore::layoutOf(DocNode const& node, LowerCtx const& ctx, LayoutHints& layout) {
    if (ExprPtr const sizing = node.prop("sizing")) {
        // The width, or both dimensions from a `{width, height}` record, one left out being content. The loader
        // refuses a literal of another form; a computed one that is none is reported and lowers to content.
        Value const value = once(ctx, sizing);
        Value const* const width = value.field("width");
        Value const* const height = value.field("height");
        bool const record = width != nullptr || height != nullptr;
        bool const valid = record ? (width == nullptr || isSizing(*width)) && (height == nullptr || isSizing(*height))
                                  : isSizing(value);
        if (!valid) {
            report(sizing->path, "sizing is " + canonicalJson(value) +
                                     "; a sizing is \"content\", {\"fixed\": n}, {\"stretch\": w}, {\"percent\": n} "
                                     "with n from 1 to 100, or a record of width and height of them");
        } else if (record) {
            layout.width = width != nullptr ? sizingOf(*width) : Sizing::content();
            layout.height = height != nullptr ? sizingOf(*height) : Sizing::content();
            if (layout.height.kind == Sizing::Kind::Percent) {
                report(sizing->path, kPercentPlaces);
                layout.height = Sizing::content();
            }
        } else {
            layout.width = sizingOf(value);
        }
    }
    if (std::string_view const placed = node.name("alignSelf"); !placed.empty()) {
        layout.alignSelf = alignOf(placed);
    }
    layout.bleed = node.flag("bleed");
    // Literal integers, checked at load.
    for (auto const& [key, bound] : {
             std::pair{"minWidth", &LayoutHints::minWidth},
             std::pair{"maxWidth", &LayoutHints::maxWidth},
             std::pair{"minHeight", &LayoutHints::minHeight},
             std::pair{"maxHeight", &LayoutHints::maxHeight},
         }) {
        if (ExprPtr const expr = node.prop(key); expr != nullptr && expr->literal.asInt() != nullptr) {
            layout.*bound = static_cast<int>(*expr->literal.asInt());
        }
    }
}

[[nodiscard]] FocusPlace focusPlace(RtScope const& scope) {
    FocusPlace place;
    for (RtScope const* current = &scope; current != nullptr; current = current->parent.get()) {
        if (current->row != nullptr) {
            place.rows.push_back(current->rowKeyNow ? current->rowKeyNow() : current->rowKey);
        }
        if (current->parent == nullptr) {
            place.prefix = current->usePrefix;
        }
    }
    std::ranges::reverse(place.rows);
    return place;
}

[[nodiscard]] std::string focusKey(FocusPlace const& place, std::string_view path) {
    std::string key = place.prefix;
    key += path;
    for (std::string const& row : place.rows) {
        key += '@';
        key += row;
    }
    return key;
}

[[nodiscard]] std::string focusKeyOf(std::string_view path, RtScope const& scope) {
    return focusKey(focusPlace(scope), path);
}

void ScreenCore::commonOf(DocNode const& node, LowerCtx const& ctx, ElementState* element, Common& common) {
    static_cast<void>(element);
    common.visible = bindProp<bool>(ctx, node.prop("visible"), true, boolOf);
    common.enabled = bindProp<bool>(ctx, node.prop("enabled"), true, boolOf);
    common.tooltip = bindProp<std::string>(ctx, node.prop("tooltip"), std::string{}, stringOf);
    layoutOf(node, ctx, common.layout);
    common.testId = std::string{node.name("testId")};
    if (!node.id.empty()) {
        // Under a scope whose row changes (a pooled table cell), the key follows the row it holds now.
        bool live = false;
        for (RtScope const* current = ctx.scope.get(); current != nullptr; current = current->parent.get()) {
            live = live || static_cast<bool>(current->rowKeyNow);
        }
        if (live) {
            common.focusId =
                slotProp<std::string>([scope = ctx.scope, path = node.path] { return focusKeyOf(path, *scope); });
        } else {
            common.focusId = focusKeyOf(node.path, *ctx.scope);
        }
    }
    common.surface = surfaceOf(node, ctx);
    if (ExprPtr const autofocus = node.prop("autofocus")) {
        common.autofocus = boolOf(once(ctx, autofocus), ctx.scope->formatting());
    }
    for (DocNodePtr const& a11y : node.partList("a11y")) {
        common.a11y.name = bindProp<std::string>(ctx, a11y->prop("name"), std::string{}, stringOf);
        common.a11y.role = std::string{a11y->name("role")};
    }
    for (DocNodePtr const& drag : node.partList("drag")) {
        ExprPtr const payload = drag->prop("payload");
        common.dragKey =
            slotProp<std::optional<Key>>([scope = ctx.scope, bindings = ctx.bindings, payload] -> std::optional<Key> {
                Value const value = evalIn(*scope, bindings.get(), payload);
                return value.isNull() ? std::nullopt : std::optional<Key>{keyOf(value)};
            });
    }
    for (DocNodePtr const& drop : node.partList("drop")) {
        ExprPtr const accepts = drop->prop("accepts");
        common.accepts = [this, scope = ctx.scope, bindings = ctx.bindings, accepts](Key const& key) {
            if (!accepts) {
                return true;
            }
            Value const accepted = runtime().untracked([&] { return evalIn(*scope, bindings.get(), accepts); });
            if (ValueList const* const items = accepted.asList()) {
                return std::ranges::any_of(*items, [&](Value const& item) { return keyOf(item) == key; });
            }
            return accepted.isNull() || keyOf(accepted) == key;
        };
        common.onDrop = [this, ctx, commands = drop->event("onDrop")](Key const& key) {
            runEvent(commands, ctx, valueOfKey(key));
        };
    }
    for (auto const& [chord, commands] : node.keys) {
        common.keys.push_back(
            KeyBinding{.chord = chord, .onPress = [this, ctx, list = &commands] { runEvent(list, ctx, Value{}); }});
    }
}

void ScreenCore::fieldOf(DocNode const& node, LowerCtx const& ctx, ElementState* element, FieldState& field) {
    field.readonly = bindProp<bool>(ctx, node.prop("readonly"), false, boolOf);
    field.required = bindProp<bool>(ctx, node.prop("required"), false, boolOf);
    field.stale = bindProp<bool>(ctx, node.prop("stale"), false, boolOf);
    if (element != nullptr) {
        field.errors = slotProp<std::vector<std::string>>(
            [this, scope = ctx.scope, element] { return stringsOf(elementValue(*element, "shownErrors")); });
    } else if (ExprPtr const errors = node.prop("errors")) {
        field.errors = slotProp<std::vector<std::string>>([scope = ctx.scope, bindings = ctx.bindings, errors] {
            return stringsOf(evalIn(*scope, bindings.get(), errors));
        });
    }
    // A tone evaluates the value and is not an error: it changes the field's look and nothing of `valid`.
    if (node.prop("description")) {
        field.description = bindProp<std::string>(ctx, node.prop("description"), std::string{}, stringOf);
    }
    if (node.prop("labelHint")) {
        field.labelHint = bindProp<std::string>(ctx, node.prop("labelHint"), std::string{}, stringOf);
    }
    if (ExprPtr const tone = node.prop("tone")) {
        field.tone = slotProp<Tone>([this, scope = ctx.scope, bindings = ctx.bindings, tone] {
            Value const value = evalIn(*scope, bindings.get(), tone);
            std::optional<Tone> const named = toneNamed(value);
            if (!named) {
                report(tone->path, "tone is " + canonicalJson(value) + "; " + std::string{kToneNames});
            }
            return named.value_or(Tone::Neutral);
        });
    }
}

MenuItem ScreenCore::menuItemOf(DocNode const& part, LowerCtx const& ctx) {
    MenuItem item;
    if (part.flag("separator")) {
        item.separator = true;
        return item;
    }
    item.label = bindProp<std::string>(ctx, part.prop("label"), std::string{}, stringOf);
    item.description = bindProp<std::string>(ctx, part.prop("description"), std::string{}, stringOf);
    item.visible = bindProp<bool>(ctx, part.prop("visible"), true, boolOf);
    if (Commands const* const commands = part.event("onClick")) {
        item.onSelect = [this, ctx, commands] { runEvent(commands, ctx, Value{}); };
    }
    if (ExprPtr const icon = part.prop("icon")) {
        item.icon = textOf(once(ctx, icon), ctx.scope->formatting());
    }
    item.keys = std::string{part.name("keys")};
    if (ExprPtr const checked = part.prop("checked")) {
        item.checked = slotProp<std::optional<bool>>(
            [scope = ctx.scope, bindings = ctx.bindings, checked] -> std::optional<bool> {
                Value const value = evalIn(*scope, bindings.get(), checked);
                bool const* const flag = value.asBool();
                return flag == nullptr ? std::nullopt : std::optional<bool>{*flag};
            });
    }
    item.enabled = bindProp<bool>(ctx, part.prop("enabled"), true, boolOf);
    return item;
}

// NOLINTNEXTLINE(misc-no-recursion): a submenu is a menu one level down.
std::vector<MenuItem> ScreenCore::menuItems(std::span<DocNodePtr const> parts, LowerCtx const& ctx) {
    std::vector<MenuItem> items;
    for (DocNodePtr const& part : parts) {
        MenuItem item = menuItemOf(*part, ctx);
        if (!item.separator) {
            item.items = menuItems(part->partList("items"), ctx);
        }
        items.push_back(std::move(item));
    }
    return items;
}

// NOLINTNEXTLINE(misc-no-recursion): a submenu is a menu one level down.
std::vector<HeadMenuModel> ScreenCore::lowerHeadMenu(std::span<DocNodePtr const> parts, LowerCtx const& ctx) {
    std::vector<HeadMenuModel> models;
    models.reserve(parts.size());
    for (DocNodePtr const& part : parts) {
        models.push_back(HeadMenuModel{
            .id = part->id,
            .path = part->path,
            .item = menuItemOf(*part, ctx),
            .items = lowerHeadMenu(part->partList("items"), ctx),
        });
    }
    return models;
}

std::vector<HeadToolModel> ScreenCore::lowerHeadTools(std::span<DocNodePtr const> parts, LowerCtx const& ctx) {
    static constexpr int kSearchWidth = 30;
    std::vector<HeadToolModel> models;
    models.reserve(parts.size());
    for (DocNodePtr const& part : parts) {
        bool const search = part->name("kind") == "search";
        HeadToolModel model{
            .id = part->id,
            .path = part->path,
            .kind = search ? HeadToolKind::Search : HeadToolKind::Select,
            .label = bindProp<std::string>(ctx, part->prop("label"), std::string{}, stringOf),
            .value = bindProp<std::string>(ctx, part->prop("value"), std::string{}, stringOf),
            .placeholder = bindProp<std::string>(ctx, part->prop("placeholder"), std::string{}, stringOf),
            .options = {},
            .selected = {},
            .width = search ? kSearchWidth : 0,
            .enabled = bindProp<bool>(ctx, part->prop("enabled"), true, boolOf),
            .visible = bindProp<bool>(ctx, part->prop("visible"), true, boolOf),
            .edit = {},
            .submit = {},
        };
        if (ExprPtr const width = part->prop("width")) {
            if (std::int64_t const* const units = width->literal.asInt()) {
                model.width = static_cast<int>(*units);
            }
        }
        if (!search) {
            model.options = slotProp<std::vector<SelectOption>>(
                [scope = ctx.scope, bindings = ctx.bindings, options = part->prop("options")] {
                    return selectOptions(evalIn(*scope, bindings.get(), options), scope->formatting());
                });
            model.selected = slotProp<std::optional<Key>>(
                [scope = ctx.scope, bindings = ctx.bindings, selected = part->prop("selected")] -> std::optional<Key> {
                    Value const value = selected ? evalIn(*scope, bindings.get(), selected) : Value{};
                    return value.isNull() ? std::nullopt : std::optional<Key>{keyOf(value)};
                });
        }
        Commands const* const edited = part->event(search ? "onChange" : "onChoose");
        model.edit = [this, ctx, edited](Value event) { runEvent(edited, ctx, std::move(event)); };
        if (search) {
            model.submit = [this, ctx, onSubmit = part->event("onSubmit")](Value event) {
                runEvent(onSubmit, ctx, std::move(event));
            };
        }
        models.push_back(std::move(model));
    }
    return models;
}

ActionModels ScreenCore::lowerActionParts(std::span<DocNodePtr const> parts, LowerCtx const& ctx) {
    ActionModels models;
    models.reserve(parts.size());
    for (DocNodePtr const& part : parts) {
        ActionModel model{
            .id = part->id,
            .path = part->path,
            .label = bindProp<std::string>(ctx, part->prop("label"), std::string{}, stringOf),
            .icon = bindProp<std::string>(ctx, part->prop("icon"), std::string{}, stringOf),
            .iconOnly = bindProp<bool>(ctx, part->prop("iconOnly"), false, boolOf),
            .emphasis = ButtonEmphasis::Normal,
            .enabled = bindProp<bool>(ctx, part->prop("enabled"), true, boolOf),
            .visible = bindProp<bool>(ctx, part->prop("visible"), true, boolOf),
            .run = {},
        };
        model.emphasis = emphasisNamed(part->name("emphasis")).value_or(ButtonEmphasis::Normal);
        if (Commands const* const commands = part->event("onClick")) {
            model.run = [this, ctx, commands] { runEvent(commands, ctx, Value{}); };
        }
        models.push_back(std::move(model));
    }
    return models;
}

Node ScreenCore::actionBarOf(ActionModels actions) {
    ActionBar bar;
    for (ActionModel& action : actions) {
        Button button{
            .label = action.label,
            .emphasis = action.emphasis,
            .onClick = std::move(action.run),
            .common = {},  // NOLINT(readability-trailing-comma): the comma ends the designator, not the empty braces.
            .icon = action.icon.evaluate(),
        };
        if (action.iconOnly.isBound() || action.iconOnly.constant()) {
            // The label is the button's name and tooltip; the caption is empty while the icon stands alone.
            button.label = Prop<std::string>{[label = action.label, iconOnly = action.iconOnly] {
                return iconOnly.evaluate() ? std::string{} : label.evaluate();
            }};
            button.common.a11y.name = action.label;
            button.common.tooltip = action.label;
        }
        button.common.visible = std::move(action.visible);
        button.common.enabled = std::move(action.enabled);
        button.common.testId = "action_" + action.id;
        bar.children.push_back(ui::button(std::move(button)));
    }
    return ui::actionBar(std::move(bar));
}

std::vector<BadgeModel> ScreenCore::lowerBadgeParts(std::span<DocNodePtr const> parts, LowerCtx const& ctx) {
    std::vector<BadgeModel> badges;
    badges.reserve(parts.size());
    for (DocNodePtr const& part : parts) {
        badges.push_back(BadgeModel{
            .text = bindProp<std::string>(ctx, part->prop("text"), std::string{}, stringOf),
            .tone = bindProp<Tone>(ctx, part->prop("tone"), Tone::Neutral, toneOf),
            .icon = bindProp<std::string>(ctx, part->prop("icon"), std::string{}, stringOf),
            .visible = bindProp<bool>(ctx, part->prop("visible"), true, boolOf),
        });
    }
    return badges;
}

// The readers every kind's lowering shares, each bound to the node being lowered.
class ScreenCore::KindKit {
public:
    KindKit(ScreenCore& core, DocNode const& node, LowerCtx const& ctx, ElementState* element)
        : _core{core}, _node{node}, _ctx{ctx}, _element{element}, _format{ctx.scope->formatting()} {}

    // The children under @p key, each lowered with @p gap as the gap of the list around it.
    [[nodiscard]] std::vector<Node> children(std::string_view key, int gap = 0) const {
        LowerCtx listCtx = _ctx;
        listCtx.listGap = gap;
        std::vector<Node> out;
        for (DocNodePtr const& child : _node.nodes(key)) {
            out.push_back(_core.lower(*child, listCtx));
        }
        return out;
    }

    // The children under @p key as one node: none is empty, several share a holder that sets @p gap.
    [[nodiscard]] Node child(std::string_view key, int gap = 0) const {
        auto const nodes = _node.nodes(key);
        return nodes.empty() ? Node{} : _core.lowerAll(nodes, _ctx, gap);
    }

    [[nodiscard]] std::int64_t onceInt(std::string_view key, std::int64_t fallback) const {
        ExprPtr const expr = _node.prop(key);
        return expr ? intOf(_core.once(_ctx, expr), fallback) : fallback;
    }

    [[nodiscard]] bool onceBool(std::string_view key, bool fallback) const {
        ExprPtr const expr = _node.prop(key);
        return expr ? boolOf(_core.once(_ctx, expr), _format) : fallback;
    }

    // A stack's padding: one step, or the loader-checked constant list of the vertical and the horizontal step. A
    // computed padding that yields a list is not a step: it is reported once and taken as 0.
    [[nodiscard]] Padding stackPadding() const {
        ExprPtr const expr = _node.prop("padding");
        if (!expr) {
            return {};
        }
        Value const value = _core.once(_ctx, expr);
        if (ValueList const* const steps = value.asList()) {
            if (expr->kind == ExprKind::Literal && steps->size() == 2) {
                return Padding{static_cast<int>(intOf(steps->at(0), 0)), static_cast<int>(intOf(steps->at(1), 0))};
            }
            _core.report(_node.path + "/padding", "a computed `padding` is one spacing step, not a list; taken as 0");
            return {};
        }
        return Padding{static_cast<int>(intOf(value, 0))};
    }

    [[nodiscard]] std::string onceText(std::string_view key) const {
        ExprPtr const expr = _node.prop(key);
        return expr ? textOf(_core.once(_ctx, expr), _format) : std::string{};
    }

    [[nodiscard]] Prop<std::string> textProp(std::string_view key) const {
        return _core.bindProp<std::string>(_ctx, _node.prop(key), std::string{}, stringOf);
    }

    // A field shows no caption of its own, so its label is what names it to assistive technology. An `a11y` part
    // that gives a name was written for that purpose and wins; one that gives only a role leaves the label.
    void nameByLabel(Common& common) const {
        bool const named = std::ranges::any_of(_node.partList("a11y"),
                                               [](DocNodePtr const& a11y) { return a11y->prop("name") != nullptr; });
        if (_node.prop("label") && !named) {
            common.a11y.name = textProp("label");
        }
    }

    // A standalone input draws its label as a caption only when the document asks for it (`showLabel`).
    template <typename Spec>
    void captionOf(Spec& spec) const {
        if (_node.prop("label")) {
            spec.label = textProp("label");
        }
        spec.showLabel = _node.flag("showLabel");
    }

    [[nodiscard]] Action action(std::string_view key) const {
        Commands const* const commands = _node.event(key);
        if (commands == nullptr) {
            return {};
        }
        return [core = &_core, ctx = _ctx, commands] { core->runEvent(commands, ctx, Value{}); };
    }

    [[nodiscard]] ActionButton actionPart() const {
        ActionButton button;
        for (DocNodePtr const& part : _node.partList("action")) {
            button.label = _core.bindProp<std::string>(_ctx, part->prop("label"), std::string{}, stringOf);
            if (Commands const* const commands = part->event("onClick")) {
                button.onClick = [core = &_core, ctx = _ctx, commands] { core->runEvent(commands, ctx, Value{}); };
            }
        }
        return button;
    }

    // A reader of the element's value under @p name.
    [[nodiscard]] auto read(std::string_view name) const {
        return [element = _element, core = &_core, name = std::string{name}] {
            return core->elementValue(*element, name);
        };
    }

private:
    // NOLINTBEGIN(cppcoreguidelines-avoid-const-or-ref-data-members): the kit lives inside one lowering call, in
    // which all four outlive it.
    ScreenCore& _core;
    DocNode const& _node;
    LowerCtx const& _ctx;
    ElementState* _element;
    FormatContext const& _format;
    // NOLINTEND(cppcoreguidelines-avoid-const-or-ref-data-members)
};

// NOLINTNEXTLINE(readability-function-cognitive-complexity): one case per kind of ui/1.
std::optional<Node> ScreenCore::lowerWidget(DocNode const& node, LowerCtx const& ctx, ElementState* element) {
    std::string_view const kind = node.kind;
    FormatContext const& format = ctx.scope->formatting();
    KindKit const kit{*this, node, ctx, element};
    if (kind == "text") {
        Text spec{.role = roleProp(ctx, node.prop("role"))};
        // A text with spans shows them, so its `text`, an older client's fallback, is not lowered.
        if (ExprPtr const spans = node.prop("spans")) {
            spec.spans = spansProp(ctx, spans);
        } else {
            spec.text = kit.textProp("text");
        }
        if (!node.name("align").empty()) {
            spec.align = textAlignNamed(node.name("align"));
        }
        fieldOf(node, ctx, element, spec.field);
        commonOf(node, ctx, element, spec.common);
        return ui::text(std::move(spec));
    }
    if (kind == "button") {
        Button spec{.label = kit.textProp("label")};
        spec.icon = kit.onceText("icon");
        spec.emphasis = emphasisNamed(node.name("emphasis")).value_or(ButtonEmphasis::Normal);
        if (node.name("style") == "item") {
            spec.style = ButtonStyle::Item;
        }
        spec.shape = shapeNamed(node.name("shape")).value_or(ControlShape::Default);
        spec.size = sizeNamed(node.name("size")).value_or(ButtonSize::Normal);
        spec.mono = node.flag("mono");
        spec.weight = weightNamed(node.name("weight")).value_or(0);
        if (node.prop("selected")) {
            spec.selected = bindProp<bool>(ctx, node.prop("selected"), false, boolOf);
            // An entry's content, drawn in place of its label: one node as it is, several in a column.
            if (std::vector<Node> content = kit.children("children"); content.size() == 1) {
                spec.child = std::move(content.front());
            } else if (!content.empty()) {
                spec.child = ui::column(Column{.children = std::move(content)});
            }
        }
        Commands const* const onClick = node.event("onClick");
        Commands const* const onActivate = node.event("onActivate");
        if (onClick != nullptr || onActivate != nullptr) {
            spec.onClick = [this, ctx, onClick, onActivate] {
                runEvent(onClick, ctx, Value{});
                runEvent(onActivate, ctx, Value{});
            };
        }
        commonOf(node, ctx, element, spec.common);
        return ui::button(std::move(spec));
    }
    if (kind == "textInput") {
        std::string const commitOn = node.name("commitOn").empty() ? "change" : std::string{node.name("commitOn")};
        auto const commit = [this, ctx, element, commitOn, onChange = node.event("onChange")](
                                std::string const& when, std::string const& text) {
            if (commitOn != when) {
                return;
            }
            if (element->bound == nullptr) {
                session.noteWrite();
                element->committed.set(Value{text});
            }
            runEvent(onChange, ctx, Value{text});
        };
        TextInput spec;
        spec.value =
            slotProp<std::string>([this, element] { return textOf(element->draft.get(), session.formatting()); });
        spec.onChange = [this, element, commit](std::string text) {
            session.asUser([&] {
                session.noteWrite();
                element->draft.set(Value{text});
                element->edited.set(true);
            });
            commit("change", text);
        };
        spec.onCommit = [this, ctx, element, commit, onCommit = node.event("onCommit")](std::string const& text) {
            element->touched.set(true);
            commit("blur", text);
            runEvent(onCommit, ctx, Value{text});
        };
        // Only a field that does something on Enter submits: one without `onSubmit` that does not commit on Enter
        // lets Enter go on to its ancestors' chords, a dialog's `default` among them.
        if (Commands const* const onSubmit = node.event("onSubmit"); onSubmit != nullptr || commitOn == "enter") {
            spec.onSubmit = [this, ctx, commit, onSubmit](std::string const& text) {
                commit("enter", text);
                runEvent(onSubmit, ctx, Value{text});
            };
        }
        spec.placeholder = kit.textProp("placeholder");
        std::string const mode = kit.onceText("mode");
        spec.mode = TextInputMode::SingleLine;
        if (mode == "multiline") {
            spec.mode = TextInputMode::Multiline;
        } else if (mode == "password") {
            spec.mode = TextInputMode::Password;
        }
        // A multiline input shows no unit and no icon; a computed mode that turns out multiline is reported and
        // its unit and icon left out, so the mount never meets them there. The loader refuses a literal one.
        for (std::string_view const key : {
                 std::string_view{"unit"},
                 std::string_view{"icon"},
                 std::string_view{"prefix"},
                 std::string_view{"decimals"},
             }) {
            if (ExprPtr const given = node.prop(key); given != nullptr && spec.mode == TextInputMode::Multiline) {
                report(given->path, "a multiline input shows no " + std::string{key});
            }
        }
        spec.mono = node.flag("mono");
        spec.wrap = spec.mode != TextInputMode::Multiline || node.flag("wrap", true);
        if (spec.mode != TextInputMode::Multiline) {
            spec.emphasis = node.name("emphasis") == "prominent" ? InputEmphasis::Prominent : InputEmphasis::Normal;
            spec.icon = kit.onceText("icon");
            spec.align = alignOf(node.name("align"));
            if (spec.align == Align::Stretch) {
                spec.align = Align::Start;
            }
            if (node.prop("unit")) {
                spec.unit = kit.textProp("unit");
            }
            if (node.prop("prefix")) {
                spec.prefix = kit.textProp("prefix");
            }
            if (ExprPtr const places = node.prop("decimals")) {
                spec.decimals = bindProp<int>(ctx, places, -1, placesOf);
            }
        }
        fieldOf(node, ctx, element, spec.field);
        commonOf(node, ctx, element, spec.common);
        kit.nameByLabel(spec.common);
        kit.captionOf(spec);
        return ui::textInput(std::move(spec));
    }
    if (kind == "checkbox") {
        Checkbox spec{
            .label = kit.textProp("label"),
            .checked = slotProp<bool>([read = kit.read("checked")] {
                Value const value = read();
                bool const* const flag = value.asBool();
                return flag != nullptr && *flag;
            }),
        };
        spec.onToggle = [this, ctx, element, onToggle = node.event("onToggle"),
                         onChange = node.event("onChange")](bool checked) {
            // A toggle is a check box's commit, so it touches the box.
            session.asUser([&] {
                if (element->bound == nullptr) {
                    session.noteWrite();
                    element->draft.set(checked);
                }
                element->touched.set(true);
            });
            runEvent(onToggle, ctx, Value{checked});
            runEvent(onChange, ctx, Value{checked});
        };
        fieldOf(node, ctx, element, spec.field);
        commonOf(node, ctx, element, spec.common);
        return ui::checkbox(std::move(spec));
    }
    if (kind == "select") {
        Select spec;
        spec.options = slotProp<std::vector<SelectOption>>(
            [scope = ctx.scope, element] { return selectOptions(element->rows->get(), scope->formatting()); });
        spec.selected = slotProp<std::optional<Key>>([read = kit.read("selected")] -> std::optional<Key> {
            Value const selected = read();
            return selected.isNull() ? std::nullopt : std::optional<Key>{keyOf(selected)};
        });
        spec.onSelect = [this, ctx, element, onChoose = node.event("onChoose"),
                         onChange = node.event("onChange")](Key const& key) {
            Value const chosen = valueOfKey(key);
            // A choice is a select's commit, so it touches the select.
            session.asUser([&] {
                if (element->bound == nullptr) {
                    session.noteWrite();
                    element->draft.set(chosen);
                }
                element->touched.set(true);
            });
            runEvent(onChoose, ctx, chosen);
            runEvent(onChange, ctx, chosen);
        };
        // A computed style of no known name is reported once and drawn as a dropdown; the loader refuses a constant
        // one.
        if (ExprPtr const style = node.prop("style")) {
            std::string const name = textOf(once(ctx, style), ctx.scope->formatting());
            if (name == "radio") {
                spec.style = SelectStyle::Radio;
            } else if (name == "segmented") {
                spec.style = SelectStyle::Segmented;
            } else if (name != "dropdown") {
                report(style->path, "style is `" + name + "`; a select's style is dropdown, radio or segmented");
            }
        }
        fieldOf(node, ctx, element, spec.field);
        commonOf(node, ctx, element, spec.common);
        kit.nameByLabel(spec.common);
        kit.captionOf(spec);
        return ui::select(std::move(spec));
    }
    if (kind == "slider") {
        Slider spec;
        spec.value = slotProp<std::int64_t>([read = kit.read("value"), element] {
            return intOf(element->bound != nullptr ? read() : element->draft.get(), 0);
        });
        spec.minimum = kit.onceInt("min", 0);
        spec.maximum = kit.onceInt("max", 100);
        spec.step = kit.onceInt("step", 1);
        spec.onChange = [this, ctx, element, onChange = node.event("onChange")](std::int64_t value) {
            session.asUser([&] {
                session.noteWrite();
                element->draft.set(value);
                element->edited.set(true);
                if (element->bound == nullptr) {
                    element->committed.set(value);
                }
            });
            runEvent(onChange, ctx, Value{value});
        };
        spec.onCommit = [this, ctx, element, onCommit = node.event("onCommit")](std::int64_t value) {
            element->touched.set(true);
            runEvent(onCommit, ctx, Value{value});
        };
        fieldOf(node, ctx, element, spec.field);
        commonOf(node, ctx, element, spec.common);
        kit.nameByLabel(spec.common);
        return ui::slider(std::move(spec));
    }
    if (kind == "dateTimeInput") {
        DateTimeInput spec;
        DateMode const mode = kit.onceText("mode") == "date" ? DateMode::Date : DateMode::DateTime;
        std::optional<TypeKind> heldAtLoad;
        if (auto const found = _doc->dateValueKinds.find(&node); found != _doc->dateValueKinds.end()) {
            heldAtLoad = found->second;
        }
        std::string const valuePath = pointerAppend(node.path, "value");
        std::int32_t const offsetMinutes = format.utcOffsetMinutes;
        spec.value = slotProp<std::optional<time::Timestamp>>([this, element, offsetMinutes,
                                                               valuePath] -> std::optional<time::Timestamp> {
            DateReading const reading = dateInstantOf(element->draft.get(), offsetMinutes);
            if (!reading.readable) {
                report(valuePath, "`dateTimeInput` shows a timestamp or an ISO-8601 date or date-time string");
            }
            return reading.instant ? std::optional<time::Timestamp>{time::Timestamp{*reading.instant}} : std::nullopt;
        });
        spec.onChange = [this, ctx, element, heldAtLoad, mode, offsetMinutes, onChange = node.event("onChange"),
                         onCommit = node.event("onCommit")](std::optional<time::Timestamp> value) {
            bool asString = element->bound != nullptr && element->bound->get().asString() != nullptr;
            if (heldAtLoad) {
                asString = *heldAtLoad == TypeKind::String;
            }
            std::optional<time::DateTime> const instant = value ? value->value : std::nullopt;
            Value entered;
            if (!asString) {
                if (instant) {
                    entered = Value{*instant};
                }
            } else if (!instant) {
                entered = Value{std::string{}};
            } else if (mode == DateMode::Date) {
                entered = Value{dateTextOf(*instant, offsetMinutes)};
            } else {
                entered = Value{instant->toIso8601()};
            }
            element->touched.set(true);
            session.asUser([&] {
                session.noteWrite();
                element->draft.set(entered);
                element->edited.set(true);
                if (element->bound == nullptr) {
                    element->committed.set(entered);
                }
            });
            runEvent(onChange, ctx, entered);
            runEvent(onCommit, ctx, entered);
        };
        spec.mode = mode;
        spec.offsetMinutes = offsetMinutes;
        // Tracked: a locale switch changes the pattern the screen's dates are written in, and the input follows.
        spec.pattern = slotProp<std::string>([this, mode] {
            FormatContext const& now = session.formatting();
            return mode == DateMode::Date ? now.datePattern : now.dateTimePattern;
        });
        fieldOf(node, ctx, element, spec.field);
        commonOf(node, ctx, element, spec.common);
        kit.nameByLabel(spec.common);
        kit.captionOf(spec);
        return ui::dateTimeInput(std::move(spec));
    }
    if (kind == "filePicker") {
        FilePicker spec;
        spec.path =
            slotProp<std::string>([this, element] { return textOf(element->draft.get(), session.formatting()); });
        spec.onPicked = [this, ctx, element, onPick = node.event("onPick")](std::string path) {
            session.asUser([&] {
                session.noteWrite();
                element->draft.set(Value{path});
                element->committed.set(Value{path});
            });
            runEvent(onPick, ctx, Value{path});
        };
        spec.mode = kit.onceText("mode") == "save" ? FilePickerMode::Save : FilePickerMode::Open;
        fieldOf(node, ctx, element, spec.field);
        commonOf(node, ctx, element, spec.common);
        kit.nameByLabel(spec.common);
        return ui::filePicker(std::move(spec));
    }
    if (kind == "menu") {
        Menu spec{.items = menuItems(node.partList("items"), ctx)};
        if (node.name("style") == "button") {
            spec.style = MenuStyle::Button;
            spec.label = kit.textProp("label");
            spec.icon = kit.onceText("icon");
            if (node.name("emphasis") == "primary") {
                spec.emphasis = ButtonEmphasis::Primary;
            } else if (node.name("emphasis") == "danger") {
                spec.emphasis = ButtonEmphasis::Danger;
            } else if (node.name("emphasis") == "ghost") {
                spec.emphasis = ButtonEmphasis::Ghost;
            }
        }
        commonOf(node, ctx, element, spec.common);
        return ui::menu(std::move(spec));
    }
    return std::nullopt;
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity): one case per kind of ui/1.
std::optional<Node> ScreenCore::lowerDisplay(DocNode const& node, LowerCtx const& ctx, ElementState* element) {
    std::string_view const kind = node.kind;
    KindKit const kit{*this, node, ctx, element};
    if (kind == "banner") {
        Banner spec{.tone = bindProp<Tone>(ctx, node.prop("tone"), Tone::Info, toneOf)};
        if (ExprPtr const spans = node.prop("spans")) {
            spec.spans = spansProp(ctx, spans);
        } else {
            spec.text = kit.textProp("text");
        }
        spec.action = kit.actionPart();
        spec.dismissible = kit.onceBool("dismissible", false);
        spec.onDismiss = kit.action("onDismiss");
        spec.actionStyle = bannerActionStyleNamed(node.name("actionStyle")).value_or(BannerActionStyle::Ghost);
        spec.actionStretch = node.flag("actionStretch");
        commonOf(node, ctx, element, spec.common);
        return ui::banner(std::move(spec));
    }
    if (kind == "badge") {
        Badge spec{
            .tone = bindProp<Tone>(ctx, node.prop("tone"), Tone::Neutral, toneOf),
            .text = kit.textProp("text"),
        };
        spec.icon = kit.onceText("icon");
        std::string_view const style = node.name("style");
        spec.style = BadgeStyle::Filled;
        if (style == "outline") {
            spec.style = BadgeStyle::Outline;
        } else if (style == "quiet") {
            spec.style = BadgeStyle::Quiet;
        }
        spec.shape = shapeNamed(node.name("shape")).value_or(ControlShape::Default);
        spec.mono = node.flag("mono");
        if (Commands const* const onRemove = node.event("onRemove")) {
            spec.onRemove = [this, ctx, onRemove] { runEvent(onRemove, ctx, Value{}); };
            spec.removeLabel = kit.textProp("removeLabel");
        }
        commonOf(node, ctx, element, spec.common);
        return ui::badge(std::move(spec));
    }
    if (kind == "progress") {
        Progress spec;
        spec.value = slotProp<std::optional<double>>(
            [scope = ctx.scope, bindings = ctx.bindings, expr = node.prop("value")] -> std::optional<double> {
                Value const value = evalIn(*scope, bindings.get(), expr);
                if (std::int64_t const* const number = value.asInt()) {
                    return static_cast<double>(*number);
                }
                if (Decimal const* const number = value.asDecimal()) {
                    return realOf(number->value);
                }
                return std::nullopt;
            });
        spec.label = kit.textProp("label");
        commonOf(node, ctx, element, spec.common);
        return ui::progress(std::move(spec));
    }
    if (kind == "steps") {
        Steps spec;
        spec.items =
            slotProp<std::vector<Step>>([scope = ctx.scope, bindings = ctx.bindings, expr = node.prop("items")] {
                std::vector<Step> steps;
                Value const items = evalIn(*scope, bindings.get(), expr);
                if (ValueList const* const list = items.asList()) {
                    for (Value const& item : *list) {
                        Value const* const label = item.field("label");
                        Value const* const state = item.field("state");
                        Value const* const detail = item.field("detail");
                        std::string const name = state == nullptr ? std::string{} : textOf(*state, {});
                        steps.push_back(Step{
                            .label = label == nullptr ? std::string{} : textOf(*label, scope->formatting()),
                            .state = stepStateOf(name),
                            .detail = detail == nullptr ? std::string{} : textOf(*detail, scope->formatting()),
                        });
                    }
                }
                return steps;
            });
        spec.current = slotProp<std::optional<std::size_t>>(
            [scope = ctx.scope, bindings = ctx.bindings, expr = node.prop("current")] -> std::optional<std::size_t> {
                std::int64_t const index = intOf(evalIn(*scope, bindings.get(), expr), -1);
                return index < 0 ? std::nullopt : std::optional<std::size_t>{static_cast<std::size_t>(index)};
            });
        spec.labelled = bindProp<bool>(ctx, node.prop("labelled"), true, boolOf);
        commonOf(node, ctx, element, spec.common);
        return ui::steps(std::move(spec));
    }
    if (kind == "chart") {
        return lowerChart(node, ctx, element);
    }
    if (kind == "keyValue") {
        KeyValue spec;
        spec.captions = node.name("fieldLayout") == "stacked" ? CaptionPlacement::Above : CaptionPlacement::Beside;
        // The loader has checked `captionWidth` is a literal integer.
        if (ExprPtr const width = node.prop("captionWidth")) {
            spec.captionWidth = static_cast<int>(*width->literal.asInt());
        }
        spec.items = slotProp<std::vector<KeyValueItem>>(
            [this, scope = ctx.scope, bindings = ctx.bindings, expr = node.prop("items")] {
                std::vector<KeyValueItem> out;
                Value const items = evalIn(*scope, bindings.get(), expr);
                if (ValueList const* const list = items.asList()) {
                    for (Value const& item : *list) {
                        out.push_back(keyValueItemOf(item, expr->path, *scope));
                    }
                }
                return out;
            });
        commonOf(node, ctx, element, spec.common);
        return ui::keyValue(std::move(spec));
    }
    if (kind == "emptyState") {
        EmptyState spec{.title = kit.textProp("title"), .text = kit.textProp("text")};
        spec.icon = kit.onceText("icon");
        spec.action = kit.actionPart();
        commonOf(node, ctx, element, spec.common);
        return ui::emptyState(std::move(spec));
    }
    return std::nullopt;
}

Node ScreenCore::lowerStack(DocNode const& node, LowerCtx const& ctx, ElementState* element) {
    std::string_view const kind = node.kind;
    KindKit const kit{*this, node, ctx, element};
    int const gap = static_cast<int>(kit.onceInt("gap", 0));
    if (std::string const arrangement = arrangedBy(node.id); !arrangement.empty()) {
        return lowerArranged(node, ctx, element, arrangement, kind == "column" ? Axis::Vertical : Axis::Horizontal,
                             gap);
    }
    if (kind == "column") {
        Column spec{
            .children = kit.children("children", gap),
            .gap = gap,
            .align = alignOf(node.name("align")),
            .justify = justifyOf(node.name("justify")),
            .divided = node.flag("divided"),
            .padding = kit.stackPadding(),
        };
        if (node.prop("selected")) {
            spec.selected = bindProp<bool>(ctx, node.prop("selected"), false, boolOf);
        }
        commonOf(node, ctx, element, spec.common);
        return ui::column(std::move(spec));
    }
    Row spec{
        .children = kit.children("children"),
        .gap = gap,
        .align = alignOf(node.name("align")),
        .justify = justifyOf(node.name("justify")),
        .wrap = node.flag("wrap"),
        .divided = node.flag("divided"),
        .padding = kit.stackPadding(),
    };
    if (node.prop("selected")) {
        spec.selected = bindProp<bool>(ctx, node.prop("selected"), false, boolOf);
    }
    commonOf(node, ctx, element, spec.common);
    return ui::row(std::move(spec));
}

Node ScreenCore::lowerGrid(DocNode const& node, LowerCtx const& ctx, ElementState* element) {
    KindKit const kit{*this, node, ctx, element};
    Grid spec{
        .columns = static_cast<int>(kit.onceInt("columns", 1)),
        .gap = static_cast<int>(kit.onceInt("gap", 0)),
        .align = alignOf(node.name("align")),
        .minColumnWidth = static_cast<int>(kit.onceInt("minColumnWidth", 0)),
    };
    for (DocNodePtr const& track : node.partList("tracks")) {
        ExprPtr const width = track->prop("width");
        spec.tracks.push_back(GridTrack{
            .width = width ? sizingOf(once(ctx, width)) : Sizing::stretch(),
            .align = cellPlaceNamed(track->name("align")).value_or(Align::Start),
        });
    }
    for (DocNodePtr const& cell : node.nodes("children")) {
        ExprPtr const span = cell->prop("span");
        spec.cells.push_back(
            GridCell{.node = lower(*cell, ctx), .span = static_cast<int>(span ? intOf(once(ctx, span), 1) : 1)});
    }
    commonOf(node, ctx, element, spec.common);
    return ui::grid(std::move(spec));
}

Node ScreenCore::lowerPanel(DocNode const& node, LowerCtx const& ctx, ElementState* element) {
    KindKit const kit{*this, node, ctx, element};
    Panel spec{.title = kit.textProp("title"), .padding = static_cast<int>(kit.onceInt("padding", 0))};
    spec.header = kit.child("header");
    spec.child = kit.child("children", static_cast<int>(kit.onceInt("gap", kPanelGap)));
    spec.collapsible = kit.onceBool("collapsible", false);
    spec.collapsed = bindProp<bool>(ctx, node.prop("collapsed"), false, boolOf);
    if (Commands const* const commands = node.event("onToggle")) {
        spec.onToggle = [this, ctx, commands](bool collapsed) { runEvent(commands, ctx, Value{collapsed}); };
    }
    commonOf(node, ctx, element, spec.common);
    return ui::panel(std::move(spec));
}

Node ScreenCore::lowerSplitter(DocNode const& node, LowerCtx const& ctx, ElementState* element) {
    KindKit const kit{*this, node, ctx, element};
    ExprPtr const orientation = node.prop("orientation");
    Splitter spec{
        .children = kit.children("children"),
        .orientation =
            orientation && axisOf(once(ctx, orientation)) == Axis::Vertical ? Axis::Vertical : Axis::Horizontal,
    };
    // No `onResize`: a user's drag stays the splitter's own, and the document's sizes are where it starts.
    if (ExprPtr const sizes = node.prop("sizes")) {
        spec.sizes = bindProp<std::vector<int>>(
            ctx, sizes, {}, [this, path = sizes->path](Value const& value, FormatContext const& /*format*/) {
                std::optional<std::vector<int>> parsed = splitterSizesOf(value);
                if (!parsed) {
                    report(path, "sizes is " + canonicalJson(value) + "; sizes is a list of numbers, one per pane");
                }
                return std::move(parsed).value_or(std::vector<int>{});
            });
    }
    commonOf(node, ctx, element, spec.common);
    // A literal `sizes` that matches the panes places them, so a pane's own width is not used.
    if (ExprPtr const sizes = node.prop("sizes"); sizes && sizes->kind == ExprKind::Literal) {
        std::optional<std::vector<int>> const given = splitterSizesOf(sizes->literal);
        if (given && given->size() == spec.children.size()) {
            for (std::size_t at = 0; at < spec.children.size(); ++at) {
                // A stretch pane asks only to fill, which `sizes` leaves it to do; a fixed or percent pane names a
                // size.
                Sizing::Kind const kind = layoutHintsOf(spec.children.at(at)).width.kind;
                if (spec.orientation == Axis::Horizontal &&
                    (kind == Sizing::Kind::Fixed || kind == Sizing::Kind::Percent)) {
                    report(sizes->path, "this splitter's `sizes` places its panes; pane " + std::to_string(at) +
                                            "'s sizing is not used");
                }
            }
        }
    }
    return ui::splitter(std::move(spec));
}

void ScreenCore::reportActionBarGaps(DocNode const& bar, LowerCtx const& ctx) {
    for (DocNodePtr const& child : bar.nodes("children")) {
        if (child->kind != "forEach" || child->prop("gap") == nullptr) {
            continue;
        }
        ExprPtr const axis = child->prop("axis");
        if (axis != nullptr && axis->kind != ExprKind::Literal && axisOf(once(ctx, axis)) == Axis::Horizontal) {
            report(child->path + "/gap", kActionBarGapRefusal);
        }
    }
}

Node ScreenCore::lowerCollapsible(DocNode const& node, LowerCtx const& ctx, ElementState* element) {
    KindKit const kit{*this, node, ctx, element};
    Collapsible spec{.title = kit.textProp("title"), .open = bindProp<bool>(ctx, node.prop("open"), true, boolOf)};
    if (Commands const* const commands = node.event("onToggle")) {
        spec.onToggle = [this, ctx, commands](bool open) { runEvent(commands, ctx, Value{open}); };
    }
    spec.header = kit.child("header");
    spec.child = kit.child("children", static_cast<int>(kit.onceInt("gap", kPanelGap)));
    commonOf(node, ctx, element, spec.common);
    return ui::collapsible(std::move(spec));
}

Node ScreenCore::lowerDropZone(DocNode const& node, LowerCtx const& ctx, ElementState* element) {
    KindKit const kit{*this, node, ctx, element};
    DropZone spec;
    spec.accept = stringsOf(node.prop("accept") ? once(ctx, node.prop("accept")) : Value{});
    spec.multiple = kit.onceBool("multiple", false);
    if (Commands const* const commands = node.event("onDrop")) {
        spec.onDrop = [this, ctx, commands](std::vector<std::string> const& paths) {
            runEvent(commands, ctx, listOfStrings(paths));
        };
    }
    spec.child = kit.child("children");
    commonOf(node, ctx, element, spec.common);
    return ui::dropZone(std::move(spec));
}

Node ScreenCore::lowerPage(DocNode const& node, LowerCtx const& ctx, ElementState* element) {
    KindKit const kit{*this, node, ctx, element};
    Page spec;
    spec.geometry = pageGeometryOf(node);
    std::optional<PageGeometry> const outer = std::exchange(pageBeingLowered, spec.geometry);
    spec.children = kit.children("children");
    pageBeingLowered = outer;
    commonOf(node, ctx, element, spec.common);
    return ui::page(std::move(spec));
}

Node ScreenCore::lowerPageRegion(DocNode const& node, LowerCtx const& ctx, ElementState* element) {
    if (!pageBeingLowered) {
        report(node.path, "a page region belongs in a page");
        return ui::text(Text{.text = std::string{}, .role = TextRole::Muted});
    }
    KindKit const kit{*this, node, ctx, element};
    PageRegion spec;
    spec.geometry = regionGeometryOf(node, *pageBeingLowered);
    std::optional<PageGeometry> const outer = std::exchange(pageBeingLowered, std::nullopt);
    spec.children = kit.children("children");
    pageBeingLowered = outer;
    commonOf(node, ctx, element, spec.common);
    return ui::pageRegion(std::move(spec));
}

Node ScreenCore::lowerLocaleScope(DocNode const& node, LowerCtx const& ctx) {
    auto const holder = std::make_shared<RtScope>(*this, ctx.scope, nullptr, ctx.scope->nodes.depth() + 2);
    holder->transparent = true;
    ExprPtr const locale = node.prop("locale");
    holder->localeFormat = &holder->nodes.computed([this, scope = ctx.scope, bindings = ctx.bindings,
                                                    locale] -> std::shared_ptr<FormatContext const> {
        Value const value = evalIn(*scope, bindings.get(), locale);
        std::string const* const tag = value.asString();
        if (tag == nullptr && !value.isNull()) {
            report(locale->path, "the locale is " + canonicalJson(value) + "; a locale is a BCP 47 tag, a string");
        }
        if (tag == nullptr || tag->empty()) {
            return session.sharedFormatting();
        }
        if (session.firstUnlistedLocale(*tag)) {
            report(locale->path, "no formatting facts for locale `" + *tag + "`; numbers, money and dates use en's");
        }
        return session.scopedFormatting(*tag);
    });
    // The children are mounted again whenever the tag changes: a value a node reads once, as it is lowered (a
    // table's column labels, a `once` property, a literal), is written in the facts at that moment, and only a new
    // lowering writes it in the new ones. The key is the tag as the scope resolves it; a tag that is not known yet
    // is the key `""`, apart from every tag.
    std::vector<DocNode const*> content;
    for (DocNodePtr const& child : node.nodes("children")) {
        content.push_back(child.get());
    }
    LayoutHints const hints = holderHints(content, nullptr, ctx);
    Switch spec;
    spec.selector = slotProp<Key>([scope = ctx.scope, bindings = ctx.bindings, locale] {
        Value const value = evalIn(*scope, bindings.get(), locale);
        std::string const* const tag = value.asString();
        return Key{tag == nullptr ? std::string{} : *tag};
    });
    spec.fallback = lazy(
        [this, ctx, holder, content, path = node.path] -> Node {
            auto const inner = std::make_shared<RtScope>(*this, holder, nullptr, holder->nodes.depth() + 2);
            inner->transparent = true;
            LowerCtx const innerCtx{.scope = inner, .bindings = ctx.bindings, .boundary = ctx.boundary, .listGap = 0};
            std::vector<Node> children;
            children.reserve(content.size());
            for (DocNode const* const child : content) {
                children.push_back(lower(*child, innerCtx));
            }
            fillHolder(children, path);
            return holding(std::move(children), inner, {}, 0);
        },
        hints);
    commonOf(node, ctx, nullptr, spec.common);
    return ui::switchOf(std::move(spec));
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity): one case per kind of ui/1.
Node ScreenCore::lowerKind(DocNode const& node, LowerCtx const& ctx, ElementState* element) {
    std::string_view const kind = node.kind;
    KindKit const kit{*this, node, ctx, element};
    if (std::optional<Node> widget = lowerWidget(node, ctx, element)) {
        return std::move(*widget);
    }
    if (kind == "group") {
        return lowerGroup(node, ctx, element, kit.children("children"));
    }
    if (kind == "column" || kind == "row") {
        return lowerStack(node, ctx, element);
    }
    if (kind == "grid") {
        return lowerGrid(node, ctx, element);
    }
    if (kind == "spacer") {
        Spacer spec;
        commonOf(node, ctx, element, spec.common);
        return ui::spacer(std::move(spec));
    }
    if (kind == "panel") {
        return lowerPanel(node, ctx, element);
    }
    if (kind == "scroll") {
        ExprPtr const axis = node.prop("axis");
        Scroll spec{.child = kit.child("children"), .axis = axis ? axisOf(once(ctx, axis)) : Axis::Vertical};
        commonOf(node, ctx, element, spec.common);
        return ui::scroll(std::move(spec));
    }
    if (kind == "switch") {
        return lowerSwitch(node, ctx);
    }
    if (kind == "tabs") {
        return lowerTabs(node, ctx, element);
    }
    if (kind == "dialog" || kind == "drawer") {
        return lowerDialog(node, ctx, element);
    }
    if (kind == "busy") {
        Busy spec{.active = bindProp<bool>(ctx, node.prop("active"), false, boolOf), .label = kit.textProp("label")};
        commonOf(node, ctx, element, spec.common);
        return ui::busy(std::move(spec));
    }
    if (kind == "table" && !node.partList("layoutRows").empty()) {
        return lowerFixedGrid(node, ctx, element);
    }
    if (kind == "table" && !node.name("form").empty()) {
        return lowerFormGrid(node, ctx, element);
    }
    if (kind == "table" && !node.name("model").empty()) {
        return lowerModelTable(node, ctx, element);
    }
    if (kind == "forEach" || kind == "table") {
        return lowerRows(node, ctx, element);
    }
    if (std::optional<Node> display = lowerDisplay(node, ctx, element)) {
        return std::move(*display);
    }
    if (kind == "splitter") {
        return lowerSplitter(node, ctx, element);
    }
    if (kind == "collapsible") {
        return lowerCollapsible(node, ctx, element);
    }
    if (kind == "dropZone") {
        return lowerDropZone(node, ctx, element);
    }
    if (kind == "actionBar") {
        KindKit const bar{*this, node, ctx, element};
        ActionBar spec{
            .children = bar.children("children"),
            .align = alignOf(node.name("align")),
            .justify = justifyOf(node.name("justify")),
            .wrap = node.flag("wrap"),
        };
        reportActionBarGaps(node, ctx);
        commonOf(node, ctx, element, spec.common);
        return ui::actionBar(std::move(spec));
    }
    if (kind == "page") {
        return lowerPage(node, ctx, element);
    }
    if (kind == "pageRegion") {
        return lowerPageRegion(node, ctx, element);
    }
    if (kind == "localeScope") {
        return lowerLocaleScope(node, ctx);
    }
    if (kind == "boundary") {
        return lowerBoundary(node, ctx);
    }
    if (kind == "custom") {
        return lowerCustom(node, ctx);
    }
    if (kind == "customize") {
        return lowerCustomize(node, ctx);
    }
    if (kind == "form") {
        return lowerForm(node, ctx, element);
    }
    if (kind == "collection" || kind == "wizard") {
        return lowerComposite(node, ctx, element);
    }
    report(node.path, "`" + node.kind + "` is a node kind this interpreter does not lower");
    return ui::text(Text{.text = std::string{node.kind}, .role = TextRole::Muted});
}

// NOLINTEND(misc-no-recursion)

}  // namespace morph::ui::detail

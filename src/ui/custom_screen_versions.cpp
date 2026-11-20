// SPDX-License-Identifier: Apache-2.0

#include <algorithm>
#include <charconv>
#include <cstddef>
#include <expected>
#include <glaze/glaze.hpp>
#include <map>
#include <morph/core/wire.hpp>
#include <morph/forms/version.hpp>
#include <morph/ui/custom_screen_versions.hpp>
#include <morph/ui/document.hpp>
#include <morph/ui/json_patch.hpp>
#include <morph/ui/value.hpp>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <variant>
#include <vector>

namespace morph::ui {

namespace {

using forms::FieldVersion;

/// @brief A refusal of the whole input, or at @p path.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters): where, then why, as a `Refusal` reads.
[[nodiscard]] std::unexpected<Refusal> refusedAt(std::string path, std::string reason) {
    return std::unexpected(Refusal{.document = {}, .path = std::move(path), .reason = std::move(reason)});
}

/// @brief One element of a document: a node or a part with an id, other than a form's field overlay (whose id is the
///        field's name).
struct Element {
    std::string id;
    std::string path;
    std::string since;
    /// The id of the nearest element holding it, and that element's path; both empty for the document itself.
    std::string parent;
    std::string parentPath;
};

/// @brief One `form` node: its id, the action it binds, where it is, and the fields its overlays name.
struct Form {
    std::string id;
    std::string action;
    std::string path;
    std::map<std::string, std::string, std::less<>> overlays;  // field name to the overlay's path

    [[nodiscard]] std::string key() const { return id.empty() ? action : id; }
};

/// @brief What a document's walk finds, in document order.
struct Walked {
    std::vector<Element> elements;
    std::vector<Form> forms;
    /// The first element of each id; an id used in more than one scope is found by its first.
    std::map<std::string, std::size_t, std::less<>> byId;
};

// NOLINTBEGIN(misc-no-recursion): a document is a tree of nodes, and the walk descends it.
void walkNode(DocNode const& node, Element const& holder, Walked& walked) {
    Element inner = holder;
    if (node.kind == "form") {
        Form form{.id = node.id, .action = std::string{node.name("action")}, .path = node.path, .overlays = {}};
        for (DocNodePtr const& record : node.partList("fields")) {
            for (DocNodePtr const& overlay : record->partList("fields")) {
                form.overlays.emplace(overlay->id, overlay->path);
            }
        }
        walked.forms.push_back(std::move(form));
    }
    if (node.kind != "field" && !node.id.empty()) {
        walked.byId.try_emplace(node.id, walked.elements.size());
        walked.elements.push_back(Element{
            .id = node.id, .path = node.path, .since = node.since, .parent = holder.id, .parentPath = holder.path});
        inner = Element{.id = node.id, .path = node.path, .since = {}, .parent = {}, .parentPath = {}};
    }
    for (auto const& [key, nodes] : node.children) {
        for (DocNodePtr const& child : nodes) {
            walkNode(*child, inner, walked);
        }
    }
    for (auto const& [key, parts] : node.parts) {
        for (DocNodePtr const& part : parts) {
            walkNode(*part, inner, walked);
        }
    }
}
// NOLINTEND(misc-no-recursion)

/// @brief The elements and forms of a document: its head's parts and its view. A component's fallback is a scope of
///        its own that nothing outside it names, so it is not walked.
[[nodiscard]] Walked walk(Document const& document) {
    Walked walked;
    Element const top{};
    for (auto const* const list : {&document.status, &document.actions, &document.menu, &document.tools}) {
        for (DocNodePtr const& part : *list) {
            walkNode(*part, top, walked);
        }
    }
    if (document.view != nullptr) {
        walkNode(*document.view, top, walked);
    }
    return walked;
}

/// @brief The value at @p pointer in @p json, or null.
[[nodiscard]] Json* resolve(Json& json, std::string_view pointer) {
    Json* current = &json;
    while (!pointer.empty()) {
        pointer.remove_prefix(1);
        auto const slash = pointer.find('/');
        std::string_view const raw = pointer.substr(0, slash);
        pointer = slash == std::string_view::npos ? std::string_view{} : pointer.substr(slash);
        std::string token;
        for (std::size_t index = 0; index < raw.size(); ++index) {
            if (raw.at(index) == '~' && index + 1 < raw.size()) {
                token += raw.at(index + 1) == '1' ? '/' : '~';
                ++index;
            } else {
                token += raw.at(index);
            }
        }
        if (auto* const object = std::get_if<Json::object_t>(&current->data)) {
            auto* const found = object->find(token);
            if (found == object->end()) {
                return nullptr;
            }
            current = &found->second;
        } else if (auto* const array = std::get_if<Json::array_t>(&current->data)) {
            std::size_t position = 0;
            char const* const last = std::to_address(token.end());
            auto const [end, error] = std::from_chars(std::to_address(token.begin()), last, position);
            if (error != std::errc{} || end != last || position >= array->size()) {
                return nullptr;
            }
            current = &array->at(position);
        } else {
            return nullptr;
        }
    }
    return current;
}

/// @brief A document's text, loaded as a save loads it apart from the catalog, and parsed.
struct Loaded {
    Document document;
    Json json;
};

[[nodiscard]] std::expected<Loaded, Refusal> load(std::string_view text) {
    auto json = parseJson(text);
    if (!json) {
        return refusedAt("", std::move(json.error()));
    }
    auto document = loadDocument(*json, LoadOptions{});
    if (!document) {
        return std::unexpected(std::move(document.error()));
    }
    return Loaded{.document = std::move(*document), .json = std::move(*json)};
}

/// @brief The `knownVersion` of a loaded document; the first version when it has none. The loader checked it.
[[nodiscard]] FieldVersion knownVersionOf(Json const& json) {
    auto const* const object = std::get_if<Json::object_t>(&json.data);
    Json::object_t::const_iterator const found = object->find(kKnownVersionKey);
    if (found == object->end()) {
        return FieldVersion{};
    }
    return forms::parseSince(std::get<std::string>(found->second.data)).value_or(FieldVersion{});
}

/// @brief One field of an action's schema, with its `x-since`.
struct SchemaField {
    std::string name;
    std::string since;
    FieldVersion version;
};

/// @brief The fields of the action a form binds, or nothing when @p schemas does not know it.
[[nodiscard]] std::expected<std::optional<std::vector<SchemaField>>, Refusal> schemaFields(
    ActionSchemaLookup const& schemas, Form const& form) {
    std::optional<std::string> const text = schemas ? schemas(form.action) : std::nullopt;
    if (!text) {
        return std::nullopt;
    }
    auto const json = parseJson(*text);
    auto const* const object = json ? std::get_if<Json::object_t>(&json->data) : nullptr;
    if (object == nullptr) {
        return refusedAt(form.path, "the schema of `" + form.action + "` is not a JSON object");
    }
    std::vector<SchemaField> fields;
    Json::object_t::const_iterator const properties = object->find("properties");
    auto const* const members =
        properties == object->end() ? nullptr : std::get_if<Json::object_t>(&properties->second.data);
    if (members == nullptr) {
        return fields;
    }
    for (auto const& [name, property] : *members) {
        SchemaField field{.name = name, .since = {}, .version = {}};
        auto const* const node = std::get_if<Json::object_t>(&property.data);
        Json::object_t::const_iterator const since =
            node == nullptr ? Json::object_t::const_iterator{} : node->find("x-since");
        if (node != nullptr && since != node->end()) {
            auto const* const written = std::get_if<std::string>(&since->second.data);
            auto version = forms::parseSince(written == nullptr ? std::string_view{"?"} : *written);
            if (!version) {
                return refusedAt(form.path, "the schema of `" + form.action + "` gives `" + name +
                                                "` an `x-since` that is not a version: " + version.error());
            }
            field.since = *written;
            field.version = std::move(*version);
        }
        fields.push_back(std::move(field));
    }
    return fields;
}

/// @brief The highest `since` of @p walked's elements and of its forms' fields.
[[nodiscard]] std::expected<FieldVersion, Refusal> versionOf(Walked const& walked, ActionSchemaLookup const& schemas) {
    FieldVersion highest;
    for (Element const& element : walked.elements) {
        // The loader checked every `since`.
        FieldVersion version = forms::parseSince(element.since).value_or(FieldVersion{});
        if (version > highest) {
            highest = std::move(version);
        }
    }
    for (Form const& form : walked.forms) {
        auto fields = schemaFields(schemas, form);
        if (!fields) {
            return std::unexpected(std::move(fields.error()));
        }
        for (SchemaField const& field : fields->value_or(std::vector<SchemaField>{})) {
            if (field.version > highest) {
                highest = field.version;
            }
        }
    }
    return highest;
}

/// @brief The form of @p layout that stands for @p form of the base: the same id, or for a form without one the first
///        form of the same action.
[[nodiscard]] Form const* counterpart(Walked const& layout, Form const& form) {
    auto const found = std::ranges::find_if(layout.forms, [&form](Form const& candidate) {
        return form.id.empty() ? candidate.action == form.action : candidate.id == form.id;
    });
    return found == layout.forms.end() ? nullptr : &*found;
}

/// @brief Where @p element sits under its holder in @p json: the member's pointer from the holder and, when the
///        member is an array, the index.
[[nodiscard]] LayoutPlace placeOf(Element const& element, Json& json) {
    std::string const relative = element.path.substr(element.parentPath.size());
    LayoutPlace place{.parent = element.parent, .path = relative, .index = std::nullopt};
    auto const slash = relative.rfind('/');
    std::string const container = relative.substr(0, slash);
    Json const* const holder = resolve(json, element.parentPath + container);
    if (holder != nullptr && std::holds_alternative<Json::array_t>(holder->data)) {
        std::string_view const token = std::string_view{relative}.substr(slash + 1);
        std::size_t index = 0;
        std::from_chars(std::to_address(token.begin()), std::to_address(token.end()), index);
        place.path = container;
        place.index = index;
    }
    return place;
}

/// @brief The elements of the base that are added: newer than @p known, and absent from @p layout. An element held by
///        another added element comes with it and is not listed.
void addElements(Walked const& base, Json& baseJson, Walked const& layout, FieldVersion const& known,
                 std::vector<LayoutAddition>& added) {
    std::set<std::string, std::less<>> coming;
    for (Element const& element : base.elements) {
        if (layout.byId.contains(element.id)) {
            continue;
        }
        if (coming.contains(element.parent)) {
            coming.insert(element.id);
            continue;
        }
        if (forms::parseSince(element.since).value_or(FieldVersion{}) <= known) {
            continue;
        }
        coming.insert(element.id);
        added.push_back(LayoutAddition{.kind = LayoutItemKind::Element,
                                       .key = element.id,
                                       .id = element.id,
                                       .since = element.since,
                                       .place = placeOf(element, baseJson),
                                       .form = {},
                                       .action = {}});
    }
}

/// @brief The changes, with the walks they were found by, for `applyLayoutChoices` to act on.
struct Compared {
    LayoutChanges changes;
    Loaded base;
    Loaded layout;
    Walked baseWalk;
    Walked layoutWalk;
};

/// @brief The fields of base forms that are added: newer than the copy's version, on a form the copy has, with no
///        overlay there. `versionOf` read every base form's schema already, so none is refused here.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters): the base, then the copy, as everywhere here.
void addFields(Walked const& base, Walked const& layout, ActionSchemaLookup const& schemas, LayoutChanges& changes) {
    for (Form const& form : base.forms) {
        Form const* const mine = counterpart(layout, form);
        if (mine == nullptr) {
            continue;
        }
        auto const fields = schemaFields(schemas, form);
        for (SchemaField const& field : fields->value_or(std::vector<SchemaField>{})) {
            if (field.version > changes.knownVersion && !mine->overlays.contains(field.name)) {
                changes.added.push_back(LayoutAddition{.kind = LayoutItemKind::Field,
                                                       .key = form.key() + "/" + field.name,
                                                       .id = field.name,
                                                       .since = field.since,
                                                       .place = {},
                                                       .form = form.id,
                                                       .action = form.action});
            }
        }
    }
}

/// @brief What the copy refers to that the base lost: its elements with a `since` the base has no id for, then the
///        overlays of fields its forms' actions no longer have.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters): the base, then the copy, as everywhere here.
[[nodiscard]] std::optional<Refusal> addRemovals(Walked const& base, Walked const& layout,
                                                 ActionSchemaLookup const& schemas, LayoutChanges& changes) {
    for (Element const& element : layout.elements) {
        if (!element.since.empty() && !base.byId.contains(element.id)) {
            changes.removed.push_back(LayoutRemoval{
                .kind = LayoutItemKind::Element, .key = element.id, .id = element.id, .form = {}, .action = {}});
        }
    }
    for (Form const& form : layout.forms) {
        auto fields = schemaFields(schemas, form);
        if (!fields) {
            return std::move(fields.error());
        }
        if (!fields->has_value()) {
            continue;
        }
        std::vector<SchemaField> const& properties = fields->value();
        for (auto const& [field, overlay] : form.overlays) {
            bool const known = std::ranges::any_of(
                properties, [&field](SchemaField const& candidate) { return candidate.name == field; });
            if (!known) {
                changes.removed.push_back(LayoutRemoval{.kind = LayoutItemKind::Field,
                                                        .key = form.key() + "/" + field,
                                                        .id = field,
                                                        .form = form.id,
                                                        .action = form.action});
            }
        }
    }
    return std::nullopt;
}

[[nodiscard]] std::expected<Compared, Refusal> compare(std::string_view baseText, ActionSchemaLookup const& schemas,
                                                       std::string_view layoutText) {
    auto base = load(baseText);
    if (!base) {
        return std::unexpected(std::move(base.error()));
    }
    auto layout = load(layoutText);
    if (!layout) {
        return std::unexpected(std::move(layout.error()));
    }
    Compared compared{
        .changes = {}, .base = std::move(*base), .layout = std::move(*layout), .baseWalk = {}, .layoutWalk = {}};
    compared.baseWalk = walk(compared.base.document);
    compared.layoutWalk = walk(compared.layout.document);
    auto version = versionOf(compared.baseWalk, schemas);
    if (!version) {
        return std::unexpected(std::move(version.error()));
    }
    LayoutChanges& changes = compared.changes;
    changes.baseVersion = std::move(*version);
    changes.knownVersion = knownVersionOf(compared.layout.json);
    addElements(compared.baseWalk, compared.base.json, compared.layoutWalk, changes.knownVersion, changes.added);
    addFields(compared.baseWalk, compared.layoutWalk, schemas, changes);
    if (auto refusal = addRemovals(compared.baseWalk, compared.layoutWalk, schemas, changes)) {
        return std::unexpected(std::move(*refusal));
    }
    return compared;
}

/// @brief Orders JSON Pointers so that removing them in this order never moves one not yet removed: later array
///        indices and deeper members first.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters): an ordering's two sides.
[[nodiscard]] bool removedBefore(std::string_view lhs, std::string_view rhs) {
    auto const tokens = [](std::string_view pointer) {
        std::vector<std::string_view> out;
        while (!pointer.empty()) {
            pointer.remove_prefix(1);
            auto const slash = pointer.find('/');
            out.push_back(pointer.substr(0, slash));
            pointer = slash == std::string_view::npos ? std::string_view{} : pointer.substr(slash);
        }
        return out;
    };
    auto const numeric = [](std::string_view token) {
        return !token.empty() && std::ranges::all_of(token, [](char chr) { return chr >= '0' && chr <= '9'; });
    };
    auto const left = tokens(lhs);
    auto const right = tokens(rhs);
    std::size_t const common = std::min(left.size(), right.size());
    for (std::size_t index = 0; index < common; ++index) {
        std::string_view const one = left.at(index);
        std::string_view const two = right.at(index);
        if (one == two) {
            continue;
        }
        if (numeric(one) && numeric(two) && one.size() != two.size()) {
            return one.size() > two.size();
        }
        return one > two;
    }
    return left.size() > right.size();
}

/// @brief One RFC 6902 operation.
[[nodiscard]] Json operation(std::string_view kind, std::string path, std::optional<Json> value = std::nullopt) {
    Json::object_t made{{"op", Json(std::string{kind})}, {"path", Json(std::move(path))}};
    if (value) {
        made.emplace("value", std::move(*value));
    }
    // Not braces: a braced `Json` is an array of what the braces hold.
    return Json(std::move(made));  // NOLINT(modernize-return-braced-init-list)
}

/// @brief @p json with @p operations applied, or the refusal naming the operation that does not apply.
[[nodiscard]] std::expected<Json, Refusal> patched(Json json, Json::array_t operations) {
    if (operations.empty()) {
        return json;
    }
    auto result = applyJsonPatch(std::move(json), Json(std::move(operations)));
    if (!result) {
        return refusedAt("", "the update does not apply to this custom screen: " + result.error());
    }
    return std::move(*result);
}

/// @brief @p json as text, with control bytes escaped inside the writer as the wire does: glaze's default leaves them
///        raw, which is invalid JSON.
[[nodiscard]] std::expected<std::string, Refusal> written(Json const& json) {
    auto text = glz::write<::morph::wire::detail::EscapingWriteOpts{}>(json);
    if (!text) {
        return refusedAt("", "the updated custom screen could not be written");
    }
    return std::move(*text);
}

/// @brief The removals and the left-out fields, on the copy as loaded: neither moves an element another names.
[[nodiscard]] std::expected<Json, Refusal> trimmed(Compared& compared, LayoutChoices const& choices) {
    Json::array_t operations;
    std::set<std::string, std::less<>> withFields;
    for (LayoutAddition const& addition : compared.changes.added) {
        auto const choice = choices.added.find(addition.key);
        if (addition.kind != LayoutItemKind::Field ||
            (choice != choices.added.end() && choice->second == AddedChoice::TakeOver)) {
            continue;
        }
        Form const* const form = counterpart(
            compared.layoutWalk, Form{.id = addition.form, .action = addition.action, .path = {}, .overlays = {}});
        std::string const fields = pointerAppend(form->path, "fields");
        if (resolve(compared.layout.json, fields) == nullptr && !withFields.contains(fields)) {
            operations.push_back(operation("add", fields, Json(Json::object_t{})));
        }
        withFields.insert(fields);
        operations.push_back(
            operation("add", pointerAppend(fields, addition.id), Json(Json::object_t{{"omit", Json(true)}})));
    }
    std::vector<std::string> removals;
    for (LayoutRemoval const& removal : compared.changes.removed) {
        auto const choice = choices.removed.find(removal.key);
        if (choice == choices.removed.end() || choice->second != RemovedChoice::Drop) {
            continue;
        }
        if (removal.kind == LayoutItemKind::Element) {
            removals.push_back(compared.layoutWalk.elements.at(compared.layoutWalk.byId.at(removal.id)).path);
        } else {
            Form const* const form = counterpart(
                compared.layoutWalk, Form{.id = removal.form, .action = removal.action, .path = {}, .overlays = {}});
            removals.push_back(form->overlays.at(removal.id));
        }
    }
    std::ranges::sort(removals, removedBefore);
    for (std::string& path : removals) {
        operations.push_back(operation("remove", std::move(path)));
    }
    return patched(std::move(compared.layout.json), std::move(operations));
}

/// @brief @p json with the base's element @p addition inserted at its place.
[[nodiscard]] std::expected<Json, Refusal> inserted(Json const& json, Compared& compared,
                                                    LayoutAddition const& addition) {
    auto text = written(json);
    if (!text) {
        return std::unexpected(std::move(text.error()));
    }
    auto current = load(*text);
    if (!current) {
        return std::unexpected(std::move(current.error()));
    }
    Walked const now = walk(current->document);
    std::string holder;
    if (!addition.place.parent.empty()) {
        auto const found = now.byId.find(addition.place.parent);
        if (found == now.byId.end()) {
            return refusedAt("", "`" + addition.id + "` sits inside `" + addition.place.parent +
                                     "` in the base, which this custom screen does not have");
        }
        holder = now.elements.at(found->second).path;
    }
    std::string const member = holder + addition.place.path;
    Element const& source = compared.baseWalk.elements.at(compared.baseWalk.byId.at(addition.id));
    Json value = *resolve(compared.base.json, source.path);
    Json* const target = resolve(current->json, member);
    std::string path = member;
    if (addition.place.index) {
        if (target == nullptr) {
            Json::array_t list;
            list.push_back(std::move(value));
            value = Json(std::move(list));
        } else if (auto const* const array = std::get_if<Json::array_t>(&target->data)) {
            path = pointerAppend(member, std::min(*addition.place.index, array->size()));
        } else {
            return refusedAt(member, "`" + addition.id + "` goes into a list here, and this is not one");
        }
    } else if (target != nullptr) {
        return refusedAt(member, "`" + addition.id + "` goes here, and this custom screen has something else here");
    }
    Json::array_t operations;
    operations.push_back(operation("add", std::move(path), std::move(value)));
    return patched(std::move(current->json), std::move(operations));
}

/// @brief Refuses a choice whose key names no change.
[[nodiscard]] std::optional<Refusal> unknownChoice(LayoutChanges const& changes, LayoutChoices const& choices) {
    for (auto const& [key, choice] : choices.added) {
        if (std::ranges::none_of(changes.added, [&key](LayoutAddition const& item) { return item.key == key; })) {
            return Refusal{.document = {}, .path = {}, .reason = "no addition is named `" + key + "`"};
        }
    }
    for (auto const& [key, choice] : choices.removed) {
        if (std::ranges::none_of(changes.removed, [&key](LayoutRemoval const& item) { return item.key == key; })) {
            return Refusal{.document = {}, .path = {}, .reason = "no removal is named `" + key + "`"};
        }
    }
    return std::nullopt;
}

}  // namespace

std::expected<FieldVersion, Refusal> baseVersion(std::string_view base, ActionSchemaLookup const& schemas) {
    auto loaded = load(base);
    if (!loaded) {
        return std::unexpected(std::move(loaded.error()));
    }
    return versionOf(walk(loaded->document), schemas);
}

std::expected<LayoutChanges, Refusal> layoutChanges(std::string_view base, ActionSchemaLookup const& baseSchemas,
                                                    std::string_view layout) {
    auto compared = compare(base, baseSchemas, layout);
    if (!compared) {
        return std::unexpected(std::move(compared.error()));
    }
    return std::move(compared->changes);
}

std::expected<std::string, Refusal> withKnownVersion(std::string_view document, FieldVersion const& version) {
    auto json = parseJson(document);
    if (!json) {
        return refusedAt("", std::move(json.error()));
    }
    auto* const object = std::get_if<Json::object_t>(&json->data);
    if (object == nullptr) {
        return refusedAt("", "a document is a JSON object");
    }
    object->insert_or_assign(std::string{kKnownVersionKey}, Json(version.text()));
    return written(*json);
}

std::expected<std::string, Refusal> applyLayoutChoices(std::string_view base, ActionSchemaLookup const& baseSchemas,
                                                       std::string_view layout, LayoutChoices const& choices) {
    auto compared = compare(base, baseSchemas, layout);
    if (!compared) {
        return std::unexpected(std::move(compared.error()));
    }
    if (auto refusal = unknownChoice(compared->changes, choices)) {
        return std::unexpected(std::move(*refusal));
    }
    auto json = trimmed(*compared, choices);
    for (LayoutAddition const& addition : compared->changes.added) {
        auto const choice = choices.added.find(addition.key);
        if (!json || addition.kind != LayoutItemKind::Element || choice == choices.added.end() ||
            choice->second != AddedChoice::TakeOver) {
            continue;
        }
        json = inserted(*json, *compared, addition);
    }
    if (!json) {
        return std::unexpected(std::move(json.error()));
    }
    auto* const object = std::get_if<Json::object_t>(&json->data);
    object->insert_or_assign(std::string{kKnownVersionKey}, Json(compared->changes.baseVersion.text()));
    auto text = written(*json);
    if (!text) {
        return text;
    }
    if (auto result = load(*text); !result) {
        return std::unexpected(std::move(result.error()));
    }
    return text;
}

}  // namespace morph::ui

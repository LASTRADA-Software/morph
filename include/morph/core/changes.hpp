// SPDX-License-Identifier: Apache-2.0

#pragma once
#include <algorithm>
#include <glaze/glaze.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <typeindex>
#include <utility>
#include <vector>

#include "change_types.hpp"
#include "effects.hpp"

/// @file
/// Deriving an execute's changes from the action's declared effects and its result.

namespace morph::effects {

namespace detail {

/// The JSON tree a result is parsed into. It keeps integers exact, so a 64-bit key is copied as written.
using ResultJson = glz::generic_u64;

/// @brief Appends the key of one record value to @p out.
/// @param value A JSON value expected to be one record.
/// @param keyField The record's key member.
/// @param out Where to append the key's raw JSON.
inline void collectKey(ResultJson const& value, std::string_view keyField, std::vector<glz::raw_json>& out) {
    if (!value.is_object()) {
        return;
    }
    auto const& members = value.get_object();
    auto const* const key = members.find(keyField);
    if (key == members.end()) {
        return;
    }
    if (auto text = glz::write_json(key->second)) {
        out.emplace_back(std::move(*text));
    }
}

/// @brief Appends the keys of the records held at a value that the path has reached.
///
/// An object is one record when it has the key member, and otherwise a map whose values are records.
/// @param value The value at the end of a path.
/// @param keyField The record's key member.
/// @param out Where to append.
// NOLINTNEXTLINE(misc-no-recursion): an array nests records to any depth, and the walk follows it.
inline void collectKeysAtLeaf(ResultJson const& value, std::string_view keyField, std::vector<glz::raw_json>& out) {
    if (value.is_array()) {
        for (auto const& element : value.get_array()) {
            collectKeysAtLeaf(element, keyField, out);
        }
    } else if (value.is_object()) {
        auto const& members = value.get_object();
        if (members.find(keyField) != members.end()) {
            collectKey(value, keyField, out);
        } else {
            for (auto const& [name, entry] : members) {
                collectKey(entry, keyField, out);
            }
        }
    }
}

/// @brief Walks the remaining path segments from @p value, then collects the keys found there.
///
/// A vector or optional adds no segment, so an array is entered without consuming one. A map adds
/// none either, so an object that lacks the next segment is searched through its values.
/// @param value The current value.
/// @param path The dot-joined member names; `""` is the value itself.
/// @param keyField The record's key member.
/// @param out Where to append.
// NOLINTNEXTLINE(misc-no-recursion): one call per path segment or nested array, bounded by the result's depth.
inline void collectKeysAtPath(ResultJson const& value, std::string_view path, std::string_view keyField,
                              std::vector<glz::raw_json>& out) {
    if (path.empty()) {
        collectKeysAtLeaf(value, keyField, out);
        return;
    }
    if (value.is_array()) {
        for (auto const& element : value.get_array()) {
            collectKeysAtPath(element, path, keyField, out);
        }
        return;
    }
    if (!value.is_object()) {
        return;
    }
    auto const dot = path.find('.');
    auto const segment = path.substr(0, dot);
    auto const rest = dot == std::string_view::npos ? std::string_view{} : path.substr(dot + 1);
    auto const& members = value.get_object();
    if (auto const* const next = members.find(segment); next != members.end()) {
        collectKeysAtPath(next->second, rest, keyField, out);
        return;
    }
    for (auto const& [name, entry] : members) {
        collectKeysAtPath(entry, path, keyField, out);
    }
}

/// @brief Adds a change for a record, merging with one already there.
///
/// A whole-type change absorbs keys, and keys are not repeated.
/// @param changes The changes so far.
/// @param record The record's wire name.
/// @param keys The changed keys, or `nullopt` for the whole type.
inline void addRecordChange(Changes& changes, std::string const& record,
                            std::optional<std::vector<glz::raw_json>> keys) {
    auto entry = std::ranges::find(changes.records, record, &RecordChange::record);
    if (entry == changes.records.end()) {
        changes.records.push_back(RecordChange{.record = record, .keys = std::move(keys)});
        return;
    }
    if (!keys) {
        entry->keys.reset();
        return;
    }
    if (!entry->keys) {
        return;
    }
    for (auto& key : *keys) {
        auto const same = [&](glz::raw_json const& have) { return have.str == key.str; };
        if (std::ranges::none_of(*entry->keys, same)) {
            entry->keys->push_back(std::move(key));
        }
    }
}

/// @brief Adds a whole-type change for every record derived, directly or through others, from @p source.
/// @param resolved The resolved registries.
/// @param source A changed record's wire name.
/// @param changes Where to add.
inline void addDerivedChanges(Resolved const& resolved, std::string const& source, Changes& changes) {
    auto const* const root = resolved.recordNamed(source);
    if (root == nullptr) {
        return;
    }
    std::vector<std::type_index> reached{root->type};
    // A record is visited once, which also ends a walk over records that share a source.
    for (std::size_t next = 0; next < reached.size(); ++next) {
        // A copy, not a reference: the loop below appends to `reached`.
        auto const current = reached.at(next);
        for (auto const& candidate : resolved.records) {
            if (std::ranges::find(candidate.derivedFrom, current) == candidate.derivedFrom.end() ||
                std::ranges::find(reached, candidate.type) != reached.end()) {
                continue;
            }
            reached.push_back(candidate.type);
            addRecordChange(changes, candidate.name, std::nullopt);
        }
    }
}

/// @brief Adds the change an execute's instance makes, unless it is already there.
/// @param changes Where to add.
/// @param ranOn The instance the action ran on.
inline void addInstanceChange(Changes& changes, InstanceChange const& ranOn) {
    auto const same = [&](InstanceChange const& have) {
        return have.modelType == ranOn.modelType && have.instance == ranOn.instance && have.primary == ranOn.primary;
    };
    if (std::ranges::none_of(changes.instances, same)) {
        changes.instances.push_back(ranOn);
    }
}

/// @brief Adds the change an `updates<R>` effect makes: the keys of the `R` rows found in the result.
///
/// A result that does not parse, or a record without a key, cannot name rows: the whole type is reported,
/// which over-notifies but never misses.
/// @param resolved The resolved registries.
/// @param actionId The action's id.
/// @param record The updated record's wire name.
/// @param parsed The parsed result, or `nullptr` when it did not parse.
/// @param changes Where to add.
inline void addUpdatesChange(Resolved const& resolved, std::string_view actionId, std::string const& record,
                             ResultJson const* parsed, Changes& changes) {
    auto const* const info = resolved.recordNamed(record);
    if (parsed == nullptr || info == nullptr || !info->keyField) {
        addRecordChange(changes, record, std::nullopt);
        return;
    }
    std::vector<glz::raw_json> keys;
    for (auto const& held : resolved.recordsOf(actionId)) {
        if (held.record != record) {
            continue;
        }
        for (auto const& path : held.paths) {
            collectKeysAtPath(*parsed, path, *info->keyField, keys);
        }
    }
    addRecordChange(changes, record, std::move(keys));
}

/// @brief See `morph::effects::deriveChanges`, over given registries.
/// @param resolved The resolved registries.
/// @param actionId The action's id.
/// @param resultJson The action's result as JSON.
/// @param ranOn The instance the action ran on.
/// @return The changes.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters): an id, then a JSON document; swapped, no effects are found.
[[nodiscard]] inline Changes deriveChangesFrom(Resolved const& resolved, std::string_view actionId,
                                               std::string_view resultJson, InstanceChange const& ranOn) {
    Changes changes;
    auto const declared = resolved.effectsOf(actionId);
    if (!declared) {
        return changes;
    }
    // The result is parsed once, and only when an `updates` effect needs its rows.
    std::optional<ResultJson> parsed;
    bool parseTried = false;
    auto const parsedResult = [&]() -> ResultJson const* {
        if (!parseTried) {
            parseTried = true;
            ResultJson tree;
            if (!glz::read_json(tree, resultJson)) {
                parsed = std::move(tree);
            }
        }
        return parsed ? &*parsed : nullptr;
    };
    for (auto const& effect : *declared) {
        switch (effect.kind) {
            case Kind::None:
                break;
            case Kind::ChangesInstance:
                addInstanceChange(changes, ranOn);
                break;
            case Kind::Changes:
                addRecordChange(changes, effect.record, std::nullopt);
                addDerivedChanges(resolved, effect.record, changes);
                break;
            case Kind::Updates:
                addUpdatesChange(resolved, actionId, effect.record, parsedResult(), changes);
                addDerivedChanges(resolved, effect.record, changes);
                break;
            default:
                break;
        }
    }
    // A record naming more rows than a reader accepts is reported whole: it over-notifies, never misses.
    for (auto& change : changes.records) {
        if (change.keys && change.keys->size() > kMaxChangeKeys) {
            change.keys.reset();
        }
    }
    return changes;
}

}  // namespace detail

/// @brief The changes a successful execute made, from the action's declared effects and its result.
///
/// `updates<R>` gives the keys of the `R` rows found in the result, `changes<R>` the whole type, and both
/// also every record derived from `R`, directly or through others, as a whole-type change.
/// `changesInstance` gives @p ranOn. An action that declared nothing, or only `none`, gives no changes.
/// @param actionId The action's id.
/// @param resultJson The action's result as JSON.
/// @param ranOn The instance the action ran on.
/// @return The changes; empty for a reader.
/// @throws morph::model::EffectsRegistrationError when the registered configuration is invalid.
[[nodiscard]] inline Changes deriveChanges(std::string_view actionId, std::string_view resultJson,
                                           InstanceChange const& ranOn) {
    return detail::deriveChangesFrom(detail::resolved(), actionId, resultJson, ranOn);
}

}  // namespace morph::effects

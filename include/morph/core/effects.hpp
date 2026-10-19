// SPDX-License-Identifier: Apache-2.0

#pragma once
#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <iterator>
#include <map>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <typeindex>
#include <typeinfo>
#include <utility>
#include <vector>

#include "detail/result_walker.hpp"
#include "records.hpp"
#include "registry.hpp"

/// @file
/// What an action changes, and which records its result holds.
///
/// `BRIDGE_REGISTER_EFFECTS` records an action's effects as data under its id.
/// Nothing is matched or checked at registration, so the order of record,
/// action and effects registrations across translation units does not matter.
/// The first read through `effectsOf`, `recordsOf`, `recordNamed` or
/// `actionsHolding` matches every action's result slots against the record
/// registry and validates the whole configuration, once. A configuration with
/// problems throws `morph::model::EffectsRegistrationError` naming all of them,
/// on that read and on every later one.

namespace morph::model {

/// @brief Thrown on a read of the effects or record registry when the registered configuration is invalid.
///
/// Lists every problem found, one per line, not only the first. Matching runs on
/// the first read, not when the registration phase closes, because
/// `closeRegistrationPhase()` is `noexcept` and a release build closes it only
/// when the application asks.
struct EffectsRegistrationError : std::logic_error {
    /// @brief Constructs the error from the problems found.
    /// @param problems One sentence per problem.
    explicit EffectsRegistrationError(std::vector<std::string> problems)
        : std::logic_error(message(problems)), _problems(std::move(problems)) {}

    /// @brief The problems found, in the order they were found.
    /// @return One sentence per problem.
    [[nodiscard]] std::vector<std::string> const& problems() const noexcept { return _problems; }

private:
    static std::string message(std::vector<std::string> const& problems) {
        std::string text = "morph: the registered records and effects are invalid:";
        for (auto const& problem : problems) {
            text += "\n  - ";
            text += problem;
        }
        return text;
    }

    std::vector<std::string> _problems;
};

}  // namespace morph::model

namespace morph::effects {

/// @brief Effect: the action edits existing rows of record @p R, and its result carries them.
/// @tparam R A keyed record type that the action's result holds.
template <typename R>
struct updates {};

/// @brief Effect: the action may create, remove or change rows of record @p R, which its result need not carry.
/// @tparam R A record type.
template <typename R>
struct changes {};

/// @brief Effect: the state of the instance the action ran on changed.
struct changesInstance {};

/// @brief Effect: the action changes nothing, declared explicitly.
struct none {};

/// @brief The kind of one declared effect.
enum class Kind : std::uint8_t {
    Updates,          ///< `updates<R>`
    Changes,          ///< `changes<R>`
    ChangesInstance,  ///< `changesInstance`
    None,             ///< `none`
};

/// @brief One effect of an action, with its record resolved to a wire name.
struct Effect {
    /// What the effect is.
    Kind kind;
    /// The record's wire name; empty for `ChangesInstance` and `None`.
    std::string record;

    /// @brief Memberwise equality.
    /// @param lhs One effect.
    /// @param rhs The other.
    /// @return True when kind and record match.
    friend bool operator==(Effect const& lhs, Effect const& rhs) = default;
};

/// @brief A record an action's result holds, and where.
struct ActionRecord {
    /// The record's wire name.
    std::string record;
    /// The record's key field, empty for a keyless record.
    std::optional<std::string> keyField;
    /// The JSON paths at which the result holds the record; `""` is the result itself.
    std::vector<std::string> paths;

    /// @brief Memberwise equality.
    /// @param lhs One entry.
    /// @param rhs The other.
    /// @return True when record, key field and paths match.
    friend bool operator==(ActionRecord const& lhs, ActionRecord const& rhs) = default;
};

namespace detail {

/// @brief One effect as a registrar records it: the record still a type, not yet a name.
struct DeclaredEffect {
    /// What the effect is.
    Kind kind;
    /// The record type, for `Updates` and `Changes`.
    std::optional<std::type_index> record;

    /// @brief Memberwise equality.
    /// @param lhs One effect.
    /// @param rhs The other.
    /// @return True when kind and record match.
    friend bool operator==(DeclaredEffect const& lhs, DeclaredEffect const& rhs) = default;
};

/// @brief Every distinct effects declaration, by action id. More than one for an id is a conflict.
using EffectsMap = std::map<std::string, std::vector<std::vector<DeclaredEffect>>, std::less<>>;

/// @brief Maps an effect tag type to its `DeclaredEffect`.
/// @tparam E An effect tag.
template <typename E>
struct EffectOf {
    static_assert(sizeof(E) == 0,
                  "BRIDGE_REGISTER_EFFECTS: an effect is updates<R>, changes<R>, changesInstance or none");
};
template <typename R>
struct EffectOf<updates<R>> {
    /// @brief The declared effect.
    /// @return `Updates` on @p R.
    static DeclaredEffect value() { return {.kind = Kind::Updates, .record = std::type_index{typeid(R)}}; }
};
template <typename R>
struct EffectOf<changes<R>> {
    /// @brief The declared effect.
    /// @return `Changes` on @p R.
    static DeclaredEffect value() { return {.kind = Kind::Changes, .record = std::type_index{typeid(R)}}; }
};
template <>
struct EffectOf<changesInstance> {
    /// @brief The declared effect.
    /// @return `ChangesInstance`.
    static DeclaredEffect value() { return {.kind = Kind::ChangesInstance, .record = std::nullopt}; }
};
template <>
struct EffectOf<none> {
    /// @brief The declared effect.
    /// @return `None`.
    static DeclaredEffect value() { return {.kind = Kind::None, .record = std::nullopt}; }
};

/// @brief Adds one declaration for an action, unless an equal one is already there.
///
/// A declaration is a set: a repeated effect is stored once, and two
/// declarations holding the same effects in any order are equal, so one
/// repeated in several translation units is stored once.
/// @param map The registry to add to.
/// @param actionId The action's id.
/// @param declared The effects.
inline void addEffects(EffectsMap& map, std::string_view actionId, std::vector<DeclaredEffect> declared) {
    auto entry = map.find(actionId);
    if (entry == map.end()) {
        entry = map.emplace(std::string{actionId}, std::vector<std::vector<DeclaredEffect>>{}).first;
    }
    std::vector<DeclaredEffect> distinct;
    for (auto& effect : declared) {
        if (std::ranges::find(distinct, effect) == distinct.end()) {
            distinct.push_back(std::move(effect));
        }
    }
    auto& declarations = entry->second;
    bool const known = std::ranges::any_of(declarations, [&](std::vector<DeclaredEffect> const& existing) {
        return std::ranges::is_permutation(existing, distinct);
    });
    if (!known) {
        declarations.push_back(std::move(distinct));
    }
}

/// @brief The process-level effects registry, appended to by static registrars.
/// @return The mutable map.
inline EffectsMap& mutableEffectsRegistry() {
    static EffectsMap entries;
    return entries;
}

/// @brief Static-init helper for `BRIDGE_REGISTER_EFFECTS`.
/// @tparam Action The action type; `ActionTraits<Action>` must be visible.
/// @tparam E The effect tags.
/// @return Always true, so the result can initialise a namespace-scope variable.
template <typename Action, typename... E>
inline bool registerEffectsOnce() noexcept {
    static_assert(sizeof...(E) > 0, "BRIDGE_REGISTER_EFFECTS: declare at least one effect");
    // NOLINTNEXTLINE(misc-static-assert,cert-dcl03-c)
    assert(!::morph::model::registrationPhaseClosed() &&
           "registerEffectsOnce: registration after the registration phase closed. The process-level "
           "registries are unsynchronised and are read-only once dispatch begins -- registering now "
           "races their internals against concurrent lookups. Load and register plugin modules before "
           "the first dispatch.");
    addEffects(mutableEffectsRegistry(), ::morph::model::ActionTraits<Action>::typeId(),
               std::vector<DeclaredEffect>{EffectOf<E>::value()...});
    return true;
}

/// @brief The matched and validated registries: what every read is answered from.
struct Resolved {
    /// @brief The records, one entry per registered type, in registration order.
    std::vector<record::RecordInfo> records;
    /// @brief Each action's effects, by action id; actions that declared none are absent.
    std::map<std::string, std::vector<Effect>, std::less<>> effects;
    /// @brief Each action's records, by action id; actions whose result holds none are absent.
    std::map<std::string, std::vector<ActionRecord>, std::less<>> actionRecords;
    /// @brief The ids of the actions holding each record, by record name, in id order.
    std::map<std::string, std::vector<std::string>, std::less<>> holding;

    /// @brief See `morph::effects::effectsOf`.
    /// @param actionId The action's id.
    /// @return Its effects, or `nullopt` when it declared none.
    [[nodiscard]] std::optional<std::vector<Effect>> effectsOf(std::string_view actionId) const {
        if (auto const found = effects.find(actionId); found != effects.end()) {
            return found->second;
        }
        return std::nullopt;
    }

    /// @brief See `morph::effects::recordsOf`.
    /// @param actionId The action's id.
    /// @return The records its result holds.
    [[nodiscard]] std::vector<ActionRecord> recordsOf(std::string_view actionId) const {
        if (auto const found = actionRecords.find(actionId); found != actionRecords.end()) {
            return found->second;
        }
        return {};
    }

    /// @brief See `morph::effects::recordNamed`.
    /// @param name A record's wire name.
    /// @return The record, or null.
    [[nodiscard]] record::RecordInfo const* recordNamed(std::string_view name) const {
        auto const found = std::ranges::find(records, name, &record::RecordInfo::name);
        return found == records.end() ? nullptr : &*found;
    }

    /// @brief See `morph::effects::actionsHolding`.
    /// @param record A record's wire name.
    /// @return The ids of the actions whose result holds it.
    [[nodiscard]] std::vector<std::string> actionsHolding(std::string_view record) const {
        if (auto const found = holding.find(record); found != holding.end()) {
            return found->second;
        }
        return {};
    }
};

/// @brief Whether @p prefix is @p path or a JSON path that contains it.
/// @param prefix A shorter path; `""` contains every path.
/// @param path The path.
/// @return True when @p path is at or below @p prefix.
[[nodiscard]] inline bool pathWithin(std::string_view prefix, std::string_view path) {
    if (prefix.empty() || prefix == path) {
        return true;
    }
    return path.size() > prefix.size() && path.starts_with(prefix) && path[prefix.size()] == '.';
}

/// @brief Whether two registrations of one record type declared the same record.
/// @param lhs One registration.
/// @param rhs The other.
/// @return True when name, key, sources and privacy all match.
[[nodiscard]] inline bool sameRegistration(record::RecordInfo const& lhs, record::RecordInfo const& rhs) {
    return lhs.name == rhs.name && lhs.type == rhs.type && lhs.keyField == rhs.keyField &&
           lhs.derivedFrom == rhs.derivedFrom && lhs.privateKeys == rhs.privateKeys &&
           lhs.keyIsString == rhs.keyIsString;
}

/// @brief Appends a problem for every `derivedFrom` cycle among the records.
/// @param records The records, one registration per type.
/// @param indexOf Each record type's first registration.
/// @param problems Where to append.
inline void findDerivationCycles(std::vector<record::RecordInfo> const& records,
                                 std::map<std::type_index, std::size_t> const& indexOf,
                                 std::vector<std::string>& problems) {
    enum class Mark : std::uint8_t { Unvisited, OnPath, Done };
    std::vector<Mark> marks(records.size(), Mark::Unvisited);
    std::vector<std::size_t> path;
    auto const visit = [&](auto const& self, std::size_t index) -> void {
        marks[index] = Mark::OnPath;
        path.push_back(index);
        for (auto const& source : records[index].derivedFrom) {
            auto const found = indexOf.find(source);
            if (found == indexOf.end()) {
                continue;
            }
            if (marks[found->second] == Mark::OnPath) {
                std::string chain;
                auto const start = std::ranges::find(path, found->second);
                for (auto step = start; step != path.end(); ++step) {
                    chain += records[*step].name + " -> ";
                }
                chain += records[found->second].name;
                problems.push_back("record `" + records[found->second].name + "` is derived from itself: " + chain);
            } else if (marks[found->second] == Mark::Unvisited) {
                self(self, found->second);
            }
        }
        path.pop_back();
        marks[index] = Mark::Done;
    };
    // Registration order, so the cycle a message names does not depend on how `type_index` sorts.
    for (std::size_t index = 0; index < records.size(); ++index) {
        if (marks[index] == Mark::Unvisited) {
            visit(visit, index);
        }
    }
}

/// @brief Matches action results against records and validates the configuration.
/// @param records The records, as registered.
/// @param resultSlots Every action's result slots, by action id.
/// @param declared Every action's effects declarations, by action id.
/// @param slotConflicts Action ids registered by more than one action type.
/// @return The resolved registries.
/// @throws morph::model::EffectsRegistrationError listing every problem, when there is one.
[[nodiscard]] inline Resolved resolve(std::vector<record::RecordInfo> records,
                                      model::detail::ActionResultSlotMap const& resultSlots,
                                      EffectsMap const& declared, std::vector<std::string> const& slotConflicts) {
    std::vector<std::string> problems;

    // An identical registration, as a record registered in a header gives once per translation unit, is
    // one registration. Any other second registration of a type or a name is a conflict.
    std::vector<record::RecordInfo> distinct;
    for (auto& info : records) {
        auto const sameType = std::ranges::find(distinct, info.type, &record::RecordInfo::type);
        if (sameType != distinct.end()) {
            if (sameType->name != info.name) {
                problems.push_back(std::string{"type "} + info.type.name() +
                                   " is registered under two record names (`" + sameType->name + "` and `" +
                                   info.name + "`)");
            } else if (!sameRegistration(*sameType, info)) {
                problems.push_back("record `" + info.name + "` is registered twice with different options");
            }
            continue;
        }
        auto const sameName = std::ranges::find(distinct, info.name, &record::RecordInfo::name);
        if (sameName != distinct.end()) {
            problems.push_back("record name `" + info.name + "` is registered for two types (" +
                               sameName->type.name() + " and " + info.type.name() + ")");
            continue;
        }
        distinct.push_back(std::move(info));
    }
    records = std::move(distinct);

    std::map<std::type_index, std::size_t> indexOf;
    for (std::size_t index = 0; index < records.size(); ++index) {
        auto const& info = records[index];
        indexOf.emplace(info.type, index);
        if (info.keyField && !info.derivedFrom.empty()) {
            problems.push_back("record `" + info.name +
                               "` is derived from other records but has a key; a derived record is keyless");
        }
    }
    // A source that is no record never changes, so the derived record would never be refreshed: refused, not ignored.
    for (auto const& info : records) {
        for (auto const& source : info.derivedFrom) {
            if (!indexOf.contains(source)) {
                problems.push_back("record `" + info.name + "` is derived from " + source.name() +
                                   ", which is not a registered record");
            }
        }
    }
    findDerivationCycles(records, indexOf, problems);

    for (auto const& id : slotConflicts) {
        problems.push_back("action `" + id + "` is registered by two action types; give each its own id");
    }

    Resolved resolved;
    for (auto const& [actionId, result] : resultSlots) {
        std::vector<model::detail::ResultSlot const*> sorted;
        sorted.reserve(result.resultSlots.size());
        for (auto const& slot : result.resultSlots) {
            sorted.push_back(&slot);
        }
        // A prefix sorts before every path below it, so a record is always kept before what it contains.
        std::ranges::stable_sort(sorted, std::less<>{}, [](auto const* slot) { return std::string_view{slot->path}; });
        std::vector<std::string_view> kept;
        std::vector<ActionRecord> matched;
        for (auto const* slot : sorted) {
            auto const found = indexOf.find(slot->type);
            if (found == indexOf.end()) {
                continue;
            }
            if (std::ranges::any_of(kept, [&](std::string_view prefix) { return pathWithin(prefix, slot->path); })) {
                continue;
            }
            kept.emplace_back(slot->path);
            auto const& info = records[found->second];
            auto entry = std::ranges::find(matched, info.name, &ActionRecord::record);
            if (entry == matched.end()) {
                matched.push_back(ActionRecord{.record = info.name, .keyField = info.keyField, .paths = {}});
                entry = std::prev(matched.end());
            }
            entry->paths.push_back(slot->path);
        }
        if (!matched.empty()) {
            for (auto const& entry : matched) {
                resolved.holding[entry.record].push_back(actionId);
            }
            resolved.actionRecords.emplace(actionId, std::move(matched));
        }
    }

    for (auto const& [actionId, declarations] : declared) {
        if (declarations.size() > 1) {
            problems.push_back("action `" + actionId + "` declares different effects in two registrations");
            continue;
        }
        auto const& declaration = declarations.front();
        if (declaration.size() > 1 &&
            std::ranges::any_of(declaration, [](DeclaredEffect const& effect) { return effect.kind == Kind::None; })) {
            problems.push_back("action `" + actionId + "` declares none together with other effects");
            continue;
        }
        std::vector<Effect> effects;
        for (auto const& effect : declaration) {
            if (!effect.record) {
                effects.push_back(Effect{.kind = effect.kind, .record = {}});
                continue;
            }
            char const* const kindName = effect.kind == Kind::Updates ? "updates" : "changes";
            auto const found = indexOf.find(*effect.record);
            if (found == indexOf.end()) {
                problems.push_back("action `" + actionId + "` declares " + kindName + "<" + effect.record->name() +
                                   ">, which is not a registered record");
                continue;
            }
            auto const& info = records[found->second];
            if (effect.kind == Kind::Updates) {
                if (!info.keyField) {
                    problems.push_back("action `" + actionId + "` declares updates on record `" + info.name +
                                       "`, which has no key");
                }
                auto const held = resolved.actionRecords.find(actionId);
                if (held == resolved.actionRecords.end() ||
                    std::ranges::find(held->second, info.name, &ActionRecord::record) == held->second.end()) {
                    problems.push_back("action `" + actionId + "` declares updates on record `" + info.name +
                                       "`, but its result holds no `" + info.name + "`");
                }
            }
            effects.push_back(Effect{.kind = effect.kind, .record = info.name});
        }
        resolved.effects.emplace(actionId, std::move(effects));
    }

    if (!problems.empty()) {
        throw model::EffectsRegistrationError(std::move(problems));
    }
    resolved.records = std::move(records);
    return resolved;
}

/// @brief Resolves a registry built by a test, leaving the process registry untouched.
/// @param records The records.
/// @param resultSlots The action result slots, by action id.
/// @param declared The effects declarations, by action id.
/// @param slotConflicts Action ids registered by more than one action type.
/// @return The resolved registries.
/// @throws morph::model::EffectsRegistrationError listing every problem, when there is one.
[[nodiscard]] inline Resolved resolveForTesting(std::vector<record::RecordInfo> records,
                                                model::detail::ActionResultSlotMap const& resultSlots,
                                                EffectsMap const& declared,
                                                std::vector<std::string> const& slotConflicts = {}) {
    return resolve(std::move(records), resultSlots, declared, slotConflicts);
}

/// @brief The process registries, matched and validated on the first call.
///
/// Every call is a read of a process-level registry and closes the
/// registration latch in a debug build. The first call resolves, under
/// `std::call_once`; an invalid configuration's error is kept and thrown again
/// on every call.
/// @return The resolved registries.
/// @throws morph::model::EffectsRegistrationError when the registered configuration is invalid.
[[nodiscard]] inline Resolved const& resolved() {
    model::detail::noteRegistryRead(true);
    struct State {
        std::once_flag once;
        std::optional<Resolved> value;
        std::optional<model::EffectsRegistrationError> error;
    };
    static State state;
    std::call_once(state.once, [] {
        try {
            state.value.emplace(resolve(record::detail::recordRegistry(), model::detail::actionResultSlots(),
                                        mutableEffectsRegistry(), model::detail::actionResultConflicts()));
        } catch (model::EffectsRegistrationError const& error) {
            state.error.emplace(error);
        }
    });
    if (state.error) {
        throw *state.error;
    }
    return *state.value;
}

}  // namespace detail

/// @brief The effects an action declared.
/// @param actionId The action's id.
/// @return Its effects, or `nullopt` when it declared none: the action reads.
/// @throws morph::model::EffectsRegistrationError when the registered configuration is invalid.
[[nodiscard]] inline std::optional<std::vector<Effect>> effectsOf(std::string_view actionId) {
    return detail::resolved().effectsOf(actionId);
}

/// @brief The records an action's result holds, with the paths to each.
///
/// Each path stops at the first registered record on it, so the members of a
/// record are not searched for further records.
/// @param actionId The action's id.
/// @return One entry per record, in the order their first path sorts; empty for an unknown action.
/// @throws morph::model::EffectsRegistrationError when the registered configuration is invalid.
[[nodiscard]] inline std::vector<ActionRecord> recordsOf(std::string_view actionId) {
    return detail::resolved().recordsOf(actionId);
}

/// @brief A registered record, by wire name.
/// @param name The record's wire name.
/// @return The record, valid for the life of the process, or null when no record has that name.
/// @throws morph::model::EffectsRegistrationError when the registered configuration is invalid.
[[nodiscard]] inline record::RecordInfo const* recordNamed(std::string_view name) {
    return detail::resolved().recordNamed(name);
}

/// @brief The actions whose result holds a record: those whose `recordsOf` names it.
/// @param record The record's wire name.
/// @return The action ids, in id order; empty for a record no result holds.
/// @throws morph::model::EffectsRegistrationError when the registered configuration is invalid.
[[nodiscard]] inline std::vector<std::string> actionsHolding(std::string_view record) {
    return detail::resolved().actionsHolding(record);
}

}  // namespace morph::effects

/// @def BRIDGE_REGISTER_EFFECTS
/// @brief Declares what action @p A changes: any of `morph::effects::updates<R>`, `changes<R>`,
///        `changesInstance` and `none`.
///
/// Place it beside the action's registration, where `ActionTraits<A>` is visible. It defines no
/// specialisation, so a translation unit that does not see it is consistent with one that does. Repeating
/// the same effects is a no-op; different effects for one action are refused on the first read. Expands to
/// nothing under `MORPH_CLIENT_ONLY`.
/// @param A The action type.
/// @param ... The effects.
// Each translation unit that includes a registration registers its own copy, which is the point, so the unnamed
// namespace in a header and its static initialiser are suppressed here, once, rather than at every use.
// NOLINTBEGIN(bugprone-throwing-static-initialization,cert-err58-cpp,misc-anonymous-namespace-in-header,cert-dcl59-cpp)
#ifdef MORPH_CLIENT_ONLY
#define BRIDGE_REGISTER_EFFECTS(A, ...)
#else
#define BRIDGE_REGISTER_EFFECTS(A, ...)                                               \
    namespace {                                                                       \
    [[maybe_unused]] const bool BRIDGE_DETAIL_CAT(bridge_effects_reg_, __COUNTER__) = \
        morph::effects::detail::registerEffectsOnce<A, __VA_ARGS__>();                \
    }
#endif
// NOLINTEND(bugprone-throwing-static-initialization,cert-err58-cpp,misc-anonymous-namespace-in-header,cert-dcl59-cpp)

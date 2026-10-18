#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <cstdint>
#include <morph/core/bridge.hpp>
#include <morph/core/effects.hpp>
#include <morph/core/records.hpp>
#include <optional>
#include <string>
#include <typeindex>
#include <vector>

#include "push_records_late.hpp"

// Glaze reflects member names through an external-linkage object, so these types cannot sit in an
// anonymous namespace.
namespace push_effects_test {

struct Item {
    std::int64_t id{};
    std::string name;
};

struct ItemList {
    std::vector<Item> items;
};

struct EditItem {
    std::int64_t id{};
};

struct LoadItems {
    int limit{};
};

struct OpenItems {
    int limit{};
};

struct ListLate {
    int limit{};
};

struct EffectsModel {
    ItemList execute(EditItem const& /*action*/) { return {}; }
    ItemList execute(LoadItems const& /*action*/) { return {}; }
    ItemList execute(OpenItems const& /*action*/) { return {}; }
    push_records_late::LateList execute(ListLate const& /*action*/) { return {}; }
};

}  // namespace push_effects_test

using namespace push_effects_test;
namespace effects = morph::effects;

BRIDGE_REGISTER_RECORD(Item, "T_EItem")

BRIDGE_REGISTER_MODEL(EffectsModel, "T_EffectsModel")
BRIDGE_REGISTER_ACTION(EffectsModel, EditItem, "T_EffectsEdit")
BRIDGE_REGISTER_EFFECTS(EditItem, effects::updates<Item>, effects::changes<Item>)
// The same declaration again, as a header included by two translation units would give.
BRIDGE_REGISTER_EFFECTS(EditItem, effects::updates<Item>, effects::changes<Item>)
BRIDGE_REGISTER_ACTION(EffectsModel, LoadItems, "T_EffectsLoad")
BRIDGE_REGISTER_EFFECTS(LoadItems, effects::changesInstance)
BRIDGE_REGISTER_ACTION(EffectsModel, OpenItems, "T_EffectsOpen")
BRIDGE_REGISTER_EFFECTS(OpenItems, effects::none)

// The record this action holds and its effects name is registered in push_records_late.cpp, whose
// static initialisers run after this file's.
BRIDGE_REGISTER_ACTION(EffectsModel, ListLate, "T_EffectsListLate")
BRIDGE_REGISTER_EFFECTS(ListLate, effects::updates<push_records_late::LateRow>)

namespace {

// Whether the late record was already registered when this file's registrars ran. The ordering test
// proves something only when it was not.
bool const lateRecordSeenEarly =
    std::ranges::any_of(morph::record::detail::recordRegistry(),
                        [](morph::record::RecordInfo const& info) { return info.name == "T_LateRow"; });

// Types used only for their identity in registries built by the tests.
struct VRow {};
struct VKeyless {};
struct VOther {};
struct VList {};
struct VHeader {};
struct VInner {};
struct VCycleA {};
struct VCycleB {};
struct VActionA {};
struct VActionB {};

using morph::effects::Kind;
using morph::effects::detail::DeclaredEffect;
using morph::effects::detail::EffectsMap;
using morph::model::detail::ActionResult;
using morph::model::detail::ActionResultSlotMap;
using morph::model::detail::ResultSlot;
using morph::record::RecordInfo;

RecordInfo record(std::string name, std::type_index type, std::optional<std::string> keyField = "id",
                  std::vector<std::type_index> derivedFrom = {}) {
    return RecordInfo{.name = std::move(name),
                      .type = type,
                      .keyField = std::move(keyField),
                      .derivedFrom = std::move(derivedFrom),
                      .privateKeys = false,
                      .keyIsString = false};
}

ActionResult result(std::vector<ResultSlot> slots) {
    return ActionResult{.action = typeid(VActionA), .resultSlots = std::move(slots)};
}

DeclaredEffect updatesOf(std::type_index type) { return DeclaredEffect{.kind = Kind::Updates, .record = type}; }
DeclaredEffect changesOf(std::type_index type) { return DeclaredEffect{.kind = Kind::Changes, .record = type}; }

// Fails the test when resolving does not throw; returns the message when it does.
std::string refusal(std::vector<RecordInfo> records, ActionResultSlotMap slots, EffectsMap declared,
                    std::vector<std::string> slotConflicts = {}) {
    try {
        static_cast<void>(morph::effects::detail::resolveForTesting(std::move(records), std::move(slots),
                                                                    std::move(declared), std::move(slotConflicts)));
    } catch (morph::model::EffectsRegistrationError const& error) {
        return error.what();
    }
    FAIL("resolving did not throw EffectsRegistrationError");
    return {};
}

}  // namespace

// ── The process registry ─────────────────────────────────────────────────────

TEST_CASE("records_of_list_action", "[push][records]") {
    auto const records = effects::recordsOf("T_WalkerListRows");
    REQUIRE(records == std::vector<effects::ActionRecord>{{"T_Row", "id", {"rows"}}});
}

TEST_CASE("registration_order_irrelevant", "[push][records]") {
    // Static initialisers ran in link order: this file's before push_records_late.cpp's.
    REQUIRE_FALSE(lateRecordSeenEarly);
    REQUIRE(effects::recordsOf("T_EffectsListLate") ==
            std::vector<effects::ActionRecord>{{"T_LateRow", "id", {"rows"}}});
    REQUIRE(effects::effectsOf("T_EffectsListLate") ==
            std::vector<effects::Effect>{{effects::Kind::Updates, "T_LateRow"}});
}

TEST_CASE("effects_declared_are_read_back", "[push][records]") {
    REQUIRE(effects::effectsOf("T_EffectsEdit") ==
            std::vector<effects::Effect>{{effects::Kind::Updates, "T_EItem"}, {effects::Kind::Changes, "T_EItem"}});
    REQUIRE(effects::effectsOf("T_EffectsLoad") == std::vector<effects::Effect>{{effects::Kind::ChangesInstance, ""}});
    REQUIRE(effects::effectsOf("T_EffectsOpen") == std::vector<effects::Effect>{{effects::Kind::None, ""}});
    REQUIRE(effects::recordsOf("T_EffectsEdit") == std::vector<effects::ActionRecord>{{"T_EItem", "id", {"items"}}});
}

TEST_CASE("unregistered_action_reads", "[push][records]") {
    REQUIRE_FALSE(effects::effectsOf("NoSuch").has_value());
    REQUIRE(effects::recordsOf("NoSuch").empty());
    // Registered, but nothing declared: the action reads.
    REQUIRE_FALSE(effects::effectsOf("T_WalkerListRows").has_value());
}

TEST_CASE("record_named_finds_registered_records", "[push][records]") {
    auto const* row = effects::recordNamed("T_Row");
    REQUIRE(row != nullptr);
    REQUIRE(row->keyField == "id");
    REQUIRE(effects::recordNamed("T_NoSuchRecord") == nullptr);
}

TEST_CASE("actions_holding_a_record", "[push][records]") {
    auto const holding = effects::actionsHolding("T_EItem");
    REQUIRE(std::ranges::find(holding, std::string{"T_EffectsEdit"}) != holding.end());
    REQUIRE(std::ranges::find(holding, std::string{"T_EffectsLoad"}) != holding.end());
    REQUIRE(std::ranges::find(holding, std::string{"T_WalkerListRows"}) == holding.end());
    REQUIRE(effects::actionsHolding("T_NoSuchRecord").empty());
}

TEST_CASE("reading_effects_closes_the_registration_phase", "[push][records]") {
#ifndef NDEBUG
    morph::model::detail::reopenRegistrationPhaseForTesting();
    static_cast<void>(effects::effectsOf("T_EffectsEdit"));
    REQUIRE(morph::model::registrationPhaseClosed());
#endif
}

// ── Registries built by the test ─────────────────────────────────────────────

TEST_CASE("matching_stops_at_first_record_on_a_path", "[push][records]") {
    ActionResultSlotMap slots;
    slots.emplace("A_Nested", result({{typeid(VList), ""},
                                      {typeid(VHeader), "header"},
                                      {typeid(VRow), "header.rows"},
                                      {typeid(VInner), "header.rows.inner"},
                                      {typeid(VRow), "loose"}}));
    slots.emplace("A_Root", result({{typeid(VHeader), ""}, {typeid(VRow), "rows"}}));
    auto const resolved = morph::effects::detail::resolveForTesting(
        {record("V_Row", typeid(VRow)), record("V_Inner", typeid(VInner)), record("V_Header", typeid(VHeader), {})},
        std::move(slots), {});

    REQUIRE(resolved.recordsOf("A_Nested") == std::vector<morph::effects::ActionRecord>{
                                                  {"V_Header", std::nullopt, {"header"}}, {"V_Row", "id", {"loose"}}});
    REQUIRE(resolved.recordsOf("A_Root") ==
            std::vector<morph::effects::ActionRecord>{{"V_Header", std::nullopt, {""}}});
    REQUIRE(resolved.actionsHolding("V_Row") == std::vector<std::string>{"A_Nested"});
    REQUIRE(resolved.actionsHolding("V_Header") == std::vector<std::string>{"A_Nested", "A_Root"});
    REQUIRE(resolved.actionsHolding("V_Inner").empty());
}

TEST_CASE("effects_twice_same_is_noop", "[push][records]") {
    EffectsMap declared;
    morph::effects::detail::addEffects(declared, "A_List", {updatesOf(typeid(VRow)), changesOf(typeid(VRow))});
    morph::effects::detail::addEffects(declared, "A_List", {updatesOf(typeid(VRow)), changesOf(typeid(VRow))});
    REQUIRE(declared.at("A_List").size() == 1);

    ActionResultSlotMap slots;
    slots.emplace("A_List", result({{typeid(VList), ""}, {typeid(VRow), "rows"}}));
    auto const resolved = morph::effects::detail::resolveForTesting({record("V_Row", typeid(VRow))}, std::move(slots),
                                                                    std::move(declared));
    REQUIRE(resolved.effectsOf("A_List") ==
            std::vector<morph::effects::Effect>{{Kind::Updates, "V_Row"}, {Kind::Changes, "V_Row"}});
}

TEST_CASE("effects_conflict_throws", "[push][records]") {
    EffectsMap declared;
    morph::effects::detail::addEffects(declared, "A_Conflicted", {changesOf(typeid(VRow))});
    morph::effects::detail::addEffects(declared, "A_Conflicted", {DeclaredEffect{.kind = Kind::None, .record = {}}});
    REQUIRE(declared.at("A_Conflicted").size() == 2);
    auto const message = refusal({record("V_Row", typeid(VRow))}, {}, std::move(declared));
    REQUIRE_THAT(message, Catch::Matchers::ContainsSubstring("`A_Conflicted`"));
    REQUIRE_THAT(message, Catch::Matchers::ContainsSubstring("different effects"));
}

TEST_CASE("action_id_held_by_two_action_types_throws", "[push][records]") {
    ActionResultSlotMap slots;
    slots.emplace("A_Shared", result({{typeid(VRow), ""}}));
    auto const message = refusal({record("V_Row", typeid(VRow))}, std::move(slots), {}, {"A_Shared"});
    REQUIRE_THAT(message, Catch::Matchers::ContainsSubstring("`A_Shared`"));
    REQUIRE_THAT(message, Catch::Matchers::ContainsSubstring("two action types"));
}

TEST_CASE("updates_without_record_in_result_throws", "[push][records]") {
    ActionResultSlotMap slots;
    slots.emplace("A_Counts", result({{typeid(VList), ""}}));
    EffectsMap declared;
    morph::effects::detail::addEffects(declared, "A_Counts", {updatesOf(typeid(VRow))});
    auto const message = refusal({record("V_Row", typeid(VRow))}, std::move(slots), std::move(declared));
    REQUIRE_THAT(message, Catch::Matchers::ContainsSubstring("`A_Counts`"));
    REQUIRE_THAT(message, Catch::Matchers::ContainsSubstring("result holds no `V_Row`"));
}

TEST_CASE("updates_on_keyless_throws", "[push][records]") {
    ActionResultSlotMap slots;
    slots.emplace("A_Report", result({{typeid(VKeyless), ""}}));
    EffectsMap declared;
    morph::effects::detail::addEffects(declared, "A_Report", {updatesOf(typeid(VKeyless))});
    auto const message =
        refusal({record("V_Keyless", typeid(VKeyless), std::nullopt)}, std::move(slots), std::move(declared));
    REQUIRE_THAT(message, Catch::Matchers::ContainsSubstring("`A_Report`"));
    REQUIRE_THAT(message, Catch::Matchers::ContainsSubstring("`V_Keyless`, which has no key"));
}

TEST_CASE("effect_on_unregistered_type_throws", "[push][records]") {
    EffectsMap declared;
    morph::effects::detail::addEffects(declared, "A_Stray", {changesOf(typeid(VOther))});
    auto const message = refusal({record("V_Row", typeid(VRow))}, {}, std::move(declared));
    REQUIRE_THAT(message, Catch::Matchers::ContainsSubstring("`A_Stray`"));
    REQUIRE_THAT(message, Catch::Matchers::ContainsSubstring("not a registered record"));
}

TEST_CASE("derived_cycle_throws", "[push][records]") {
    auto const message =
        refusal({record("V_CycleA", typeid(VCycleA), std::nullopt, {typeid(VCycleB)}),
                 record("V_CycleB", typeid(VCycleB), std::nullopt, {typeid(VCycleA)}), record("V_Row", typeid(VRow))},
                {}, {});
    REQUIRE_THAT(message, Catch::Matchers::ContainsSubstring("V_CycleA -> V_CycleB -> V_CycleA"));
}

TEST_CASE("duplicate_record_name_throws", "[push][records]") {
    auto const byName = refusal({record("V_Row", typeid(VRow)), record("V_Row", typeid(VOther))}, {}, {});
    REQUIRE_THAT(byName, Catch::Matchers::ContainsSubstring("record name `V_Row` is registered for two types"));
    auto const byType = refusal({record("V_Row", typeid(VRow)), record("V_Row2", typeid(VRow))}, {}, {});
    REQUIRE_THAT(byType, Catch::Matchers::ContainsSubstring("registered as a record twice (`V_Row` and `V_Row2`)"));
}

TEST_CASE("every_problem_is_listed", "[push][records]") {
    ActionResultSlotMap slots;
    slots.emplace("A_Counts", result({{typeid(VList), ""}}));
    slots.emplace("A_Report", result({{typeid(VKeyless), ""}}));
    EffectsMap declared;
    morph::effects::detail::addEffects(declared, "A_Counts", {updatesOf(typeid(VRow))});
    morph::effects::detail::addEffects(declared, "A_Report", {updatesOf(typeid(VKeyless))});
    auto const message = refusal(
        {record("V_Row", typeid(VRow)), record("V_Keyless", typeid(VKeyless), std::nullopt),
         record("V_CycleA", typeid(VCycleA), std::nullopt, {typeid(VCycleA)}), record("V_Row", typeid(VOther))},
        std::move(slots), std::move(declared));
    REQUIRE_THAT(message, Catch::Matchers::ContainsSubstring("result holds no `V_Row`"));
    REQUIRE_THAT(message, Catch::Matchers::ContainsSubstring("`V_Keyless`, which has no key"));
    REQUIRE_THAT(message, Catch::Matchers::ContainsSubstring("V_CycleA -> V_CycleA"));
    REQUIRE_THAT(message, Catch::Matchers::ContainsSubstring("record name `V_Row` is registered for two types"));
}

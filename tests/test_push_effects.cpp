#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <cstdint>
#include <morph/core/bridge.hpp>
#include <morph/core/changes.hpp>
#include <morph/core/effects.hpp>
#include <morph/core/records.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <typeindex>
#include <utility>
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
// NOLINTNEXTLINE(bugprone-throwing-static-initialization,cert-err58-cpp): reading the registry during static initialisation is the measurement.
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
std::string refusal(std::vector<RecordInfo> records, ActionResultSlotMap const& resultSlots,
                    EffectsMap const& declared, std::vector<std::string> const& slotConflicts = {}) {
    try {
        static_cast<void>(
            morph::effects::detail::resolveForTesting(std::move(records), resultSlots, declared, slotConflicts));
    } catch (morph::model::EffectsRegistrationError const& error) {
        return error.what();
    }
    FAIL_CHECK("resolving did not throw EffectsRegistrationError");
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
    slots.emplace("A_Nested", result({{.type = typeid(VList), .path = ""},
                                      {.type = typeid(VHeader), .path = "header"},
                                      {.type = typeid(VRow), .path = "header.rows"},
                                      {.type = typeid(VInner), .path = "header.rows.inner"},
                                      {.type = typeid(VRow), .path = "loose"}}));
    slots.emplace("A_Root", result({{.type = typeid(VHeader), .path = ""}, {.type = typeid(VRow), .path = "rows"}}));
    auto const resolved = morph::effects::detail::resolveForTesting(
        {record("V_Row", typeid(VRow)), record("V_Inner", typeid(VInner)), record("V_Header", typeid(VHeader), {})},
        slots, {});

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
    slots.emplace("A_List", result({{.type = typeid(VList), .path = ""}, {.type = typeid(VRow), .path = "rows"}}));
    auto const resolved = morph::effects::detail::resolveForTesting({record("V_Row", typeid(VRow))}, slots, declared);
    REQUIRE(resolved.effectsOf("A_List") ==
            std::vector<morph::effects::Effect>{{Kind::Updates, "V_Row"}, {Kind::Changes, "V_Row"}});
}

TEST_CASE("effects_conflict_throws", "[push][records]") {
    EffectsMap declared;
    morph::effects::detail::addEffects(declared, "A_Conflicted", {changesOf(typeid(VRow))});
    morph::effects::detail::addEffects(declared, "A_Conflicted", {DeclaredEffect{.kind = Kind::None, .record = {}}});
    REQUIRE(declared.at("A_Conflicted").size() == 2);
    auto const message = refusal({record("V_Row", typeid(VRow))}, {}, declared);
    REQUIRE_THAT(message, Catch::Matchers::ContainsSubstring("`A_Conflicted`"));
    REQUIRE_THAT(message, Catch::Matchers::ContainsSubstring("different effects"));
}

TEST_CASE("action_id_held_by_two_action_types_throws", "[push][records]") {
    ActionResultSlotMap slots;
    slots.emplace("A_Shared", result({{.type = typeid(VRow), .path = ""}}));
    auto const message = refusal({record("V_Row", typeid(VRow))}, slots, {}, {"A_Shared"});
    REQUIRE_THAT(message, Catch::Matchers::ContainsSubstring("`A_Shared`"));
    REQUIRE_THAT(message, Catch::Matchers::ContainsSubstring("two action types"));
}

TEST_CASE("updates_without_record_in_result_throws", "[push][records]") {
    ActionResultSlotMap slots;
    slots.emplace("A_Counts", result({{.type = typeid(VList), .path = ""}}));
    EffectsMap declared;
    morph::effects::detail::addEffects(declared, "A_Counts", {updatesOf(typeid(VRow))});
    auto const message = refusal({record("V_Row", typeid(VRow))}, slots, declared);
    REQUIRE_THAT(message, Catch::Matchers::ContainsSubstring("`A_Counts`"));
    REQUIRE_THAT(message, Catch::Matchers::ContainsSubstring("result holds no `V_Row`"));
}

TEST_CASE("updates_on_keyless_throws", "[push][records]") {
    ActionResultSlotMap slots;
    slots.emplace("A_Report", result({{.type = typeid(VKeyless), .path = ""}}));
    EffectsMap declared;
    morph::effects::detail::addEffects(declared, "A_Report", {updatesOf(typeid(VKeyless))});
    auto const message = refusal({record("V_Keyless", typeid(VKeyless), std::nullopt)}, slots, declared);
    REQUIRE_THAT(message, Catch::Matchers::ContainsSubstring("`A_Report`"));
    REQUIRE_THAT(message, Catch::Matchers::ContainsSubstring("`V_Keyless`, which has no key"));
}

TEST_CASE("effect_on_unregistered_type_throws", "[push][records]") {
    EffectsMap declared;
    morph::effects::detail::addEffects(declared, "A_Stray", {changesOf(typeid(VOther))});
    auto const message = refusal({record("V_Row", typeid(VRow))}, {}, declared);
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
    REQUIRE_THAT(byType,
                 Catch::Matchers::ContainsSubstring("registered under two record names (`V_Row` and `V_Row2`)"));
}

TEST_CASE("identical_record_registrations_are_one", "[push][records]") {
    ActionResultSlotMap slots;
    slots.emplace("A_List", result({{.type = typeid(VList), .path = ""}, {.type = typeid(VRow), .path = "rows"}}));
    auto const resolved = morph::effects::detail::resolveForTesting(
        {record("V_Row", typeid(VRow)), record("V_Row", typeid(VRow))}, slots, {});
    REQUIRE(resolved.records.size() == 1);
    REQUIRE(resolved.recordNamed("V_Row") != nullptr);
    REQUIRE(resolved.recordsOf("A_List") == std::vector<morph::effects::ActionRecord>{{"V_Row", "id", {"rows"}}});
}

TEST_CASE("record_same_type_different_options_throws", "[push][records]") {
    auto const message = refusal({record("V_Row", typeid(VRow)), record("V_Row", typeid(VRow), "code")}, {}, {});
    REQUIRE_THAT(message,
                 Catch::Matchers::ContainsSubstring("record `V_Row` is registered twice with different options"));
}

TEST_CASE("keyed_derived_record_throws", "[push][records]") {
    auto const message =
        refusal({record("V_Row", typeid(VRow)), record("V_Header", typeid(VHeader), "id", {typeid(VRow)})}, {}, {});
    REQUIRE_THAT(message,
                 Catch::Matchers::ContainsSubstring("record `V_Header` is derived from other records but has a key"));
}

TEST_CASE("effect_none_with_others_throws", "[push][records]") {
    EffectsMap declared;
    morph::effects::detail::addEffects(declared, "A_Mixed",
                                       {changesOf(typeid(VRow)), DeclaredEffect{.kind = Kind::None, .record = {}}});
    auto const message = refusal({record("V_Row", typeid(VRow))}, {}, declared);
    REQUIRE_THAT(message,
                 Catch::Matchers::ContainsSubstring("action `A_Mixed` declares none together with other effects"));
}

TEST_CASE("repeated_effect_in_declaration_is_one", "[push][records]") {
    EffectsMap declared;
    morph::effects::detail::addEffects(declared, "A_List", {updatesOf(typeid(VRow)), updatesOf(typeid(VRow))});
    morph::effects::detail::addEffects(declared, "A_List", {updatesOf(typeid(VRow))});
    REQUIRE(declared.at("A_List").size() == 1);
    ActionResultSlotMap slots;
    slots.emplace("A_List", result({{.type = typeid(VRow), .path = ""}}));
    auto const resolved = morph::effects::detail::resolveForTesting({record("V_Row", typeid(VRow))}, slots, declared);
    REQUIRE(resolved.effectsOf("A_List") == std::vector<morph::effects::Effect>{{Kind::Updates, "V_Row"}});
}

TEST_CASE("every_problem_is_listed", "[push][records]") {
    ActionResultSlotMap slots;
    slots.emplace("A_Counts", result({{.type = typeid(VList), .path = ""}}));
    slots.emplace("A_Report", result({{.type = typeid(VKeyless), .path = ""}}));
    EffectsMap declared;
    morph::effects::detail::addEffects(declared, "A_Counts", {updatesOf(typeid(VRow))});
    morph::effects::detail::addEffects(declared, "A_Report", {updatesOf(typeid(VKeyless))});
    auto const message = refusal(
        {record("V_Row", typeid(VRow)), record("V_Keyless", typeid(VKeyless), std::nullopt),
         record("V_CycleA", typeid(VCycleA), std::nullopt, {typeid(VCycleA)}), record("V_Row", typeid(VOther))},
        slots, declared);
    REQUIRE_THAT(message, Catch::Matchers::ContainsSubstring("result holds no `V_Row`"));
    REQUIRE_THAT(message, Catch::Matchers::ContainsSubstring("`V_Keyless`, which has no key"));
    REQUIRE_THAT(message, Catch::Matchers::ContainsSubstring("V_CycleA -> V_CycleA"));
    REQUIRE_THAT(message, Catch::Matchers::ContainsSubstring("record name `V_Row` is registered for two types"));
}

// ── Deriving changes ─────────────────────────────────────────────────────────

namespace {

using morph::effects::InstanceChange;
using morph::effects::RecordChange;

struct DTxn {};
struct DReport {};
struct DSummary {};
struct DNamed {};

std::vector<std::string> keyTexts(RecordChange const& change) {
    std::vector<std::string> texts;
    for (auto const& key : *change.keys) {
        texts.push_back(key.str);
    }
    return texts;
}

// Records: D_Row (keyed), D_Txn (keyed), D_Report derived from D_Txn, D_Summary derived from D_Report.
morph::effects::detail::Resolved derivation(EffectsMap const& declared) {
    ActionResultSlotMap slots;
    slots.emplace("A_Rows", result({{.type = typeid(VList), .path = ""}, {.type = typeid(VRow), .path = "rows"}}));
    slots.emplace("A_Root", result({{.type = typeid(VRow), .path = ""}}));
    slots.emplace("A_Deep",
                  result({{.type = typeid(VList), .path = ""}, {.type = typeid(VRow), .path = "header.rows"}}));
    slots.emplace("A_Named",
                  result({{.type = typeid(VList), .path = ""}, {.type = typeid(DNamed), .path = "byName"}}));
    return morph::effects::detail::resolveForTesting(
        {record("D_Row", typeid(VRow)), record("D_Txn", typeid(DTxn)),
         record("D_Report", typeid(DReport), std::nullopt, {typeid(DTxn)}),
         record("D_Summary", typeid(DSummary), std::nullopt, {typeid(DReport)}),
         record("D_Header", typeid(VHeader), {}), record("D_Named", typeid(DNamed), "code")},
        slots, declared);
}

EffectsMap declare(std::string_view actionId, std::vector<DeclaredEffect> effects) {
    EffectsMap declared;
    morph::effects::detail::addEffects(declared, actionId, std::move(effects));
    return declared;
}

InstanceChange ranOnInstance() { return {.modelType = "M", .instance = 7, .primary = "k"}; }

}  // namespace

TEST_CASE("derive_updates_reads_keys_at_paths", "[push][records]") {
    auto const resolved = derivation(declare("A_Rows", {updatesOf(typeid(VRow))}));
    auto const changes = morph::effects::detail::deriveChangesFrom(
        resolved, "A_Rows", R"({"rows":[{"id":1,"n":"a"},{"id":2},{"n":"no key"}]})", ranOnInstance());
    REQUIRE(changes.instances.empty());
    REQUIRE(changes.records.size() == 1);
    REQUIRE(changes.records[0].record == "D_Row");
    REQUIRE(changes.records[0].keys.has_value());
    REQUIRE(keyTexts(changes.records[0]) == std::vector<std::string>{"1", "2"});
}

TEST_CASE("derive_updates_collapses_past_the_key_bound", "[push][records]") {
    auto const resolved = derivation(declare("A_Rows", {updatesOf(typeid(VRow))}));
    auto const rowsJson = [](std::size_t count) {
        std::string json = R"({"rows":[)";
        for (std::size_t i = 0; i < count; ++i) {
            json += (i == 0 ? "" : ",") + std::string{R"({"id":)"} + std::to_string(i + 1) + "}";
        }
        return json + "]}";
    };
    auto const atBound = morph::effects::detail::deriveChangesFrom(
        resolved, "A_Rows", rowsJson(morph::effects::kMaxChangeKeys), ranOnInstance());
    REQUIRE(atBound.records.size() == 1);
    REQUIRE(atBound.records[0].keys.has_value());
    CHECK(atBound.records[0].keys->size() == morph::effects::kMaxChangeKeys);

    auto const pastBound = morph::effects::detail::deriveChangesFrom(
        resolved, "A_Rows", rowsJson(morph::effects::kMaxChangeKeys + 1), ranOnInstance());
    REQUIRE(pastBound.records.size() == 1);
    CHECK(pastBound.records[0].record == "D_Row");
    CHECK_FALSE(pastBound.records[0].keys.has_value());
}

TEST_CASE("derive_updates_reads_every_result_shape", "[push][records]") {
    auto const rootResolved = derivation(declare("A_Root", {updatesOf(typeid(VRow))}));
    auto const one = morph::effects::detail::deriveChangesFrom(rootResolved, "A_Root", R"({"id":9007199254740993})",
                                                               ranOnInstance());
    REQUIRE(keyTexts(one.records.at(0)) == std::vector<std::string>{"9007199254740993"});

    auto const absent = morph::effects::detail::deriveChangesFrom(rootResolved, "A_Root", "null", ranOnInstance());
    REQUIRE(absent.records.at(0).keys.has_value());
    REQUIRE(absent.records.at(0).keys->empty());

    auto const deepResolved = derivation(declare("A_Deep", {updatesOf(typeid(VRow))}));
    auto const deep = morph::effects::detail::deriveChangesFrom(
        deepResolved, "A_Deep", R"({"header":{"rows":[{"id":"x"},{"id":"y"}]}})", ranOnInstance());
    REQUIRE(keyTexts(deep.records.at(0)) == std::vector<std::string>{"\"x\"", "\"y\""});

    auto const namedResolved = derivation(declare("A_Named", {updatesOf(typeid(DNamed))}));
    auto const map = morph::effects::detail::deriveChangesFrom(
        namedResolved, "A_Named", R"({"byName":{"a":{"code":"c1"},"b":{"code":"c2"}}})", ranOnInstance());
    REQUIRE(keyTexts(map.records.at(0)) == std::vector<std::string>{"\"c1\"", "\"c2\""});
}

TEST_CASE("derive_updates_with_unparseable_result_reports_the_type", "[push][records]") {
    auto const resolved = derivation(declare("A_Rows", {updatesOf(typeid(VRow))}));
    auto const changes = morph::effects::detail::deriveChangesFrom(resolved, "A_Rows", "{not json", ranOnInstance());
    REQUIRE(changes.records.size() == 1);
    REQUIRE_FALSE(changes.records[0].keys.has_value());
}

TEST_CASE("derive_changes_has_no_keys", "[push][records]") {
    auto const resolved = derivation(declare("A_Rows", {changesOf(typeid(VRow))}));
    auto const changes =
        morph::effects::detail::deriveChangesFrom(resolved, "A_Rows", R"({"rows":[{"id":1}]})", ranOnInstance());
    REQUIRE(changes.records.size() == 1);
    REQUIRE(changes.records[0].record == "D_Row");
    REQUIRE_FALSE(changes.records[0].keys.has_value());
}

TEST_CASE("derive_changes_absorbs_updates_keys", "[push][records]") {
    auto const resolved = derivation(declare("A_Rows", {updatesOf(typeid(VRow)), changesOf(typeid(VRow))}));
    auto const changes =
        morph::effects::detail::deriveChangesFrom(resolved, "A_Rows", R"({"rows":[{"id":1}]})", ranOnInstance());
    REQUIRE(changes.records.size() == 1);
    REQUIRE_FALSE(changes.records[0].keys.has_value());
}

TEST_CASE("derive_expands_derived_from", "[push][records]") {
    auto const resolved = derivation(declare("A_Rows", {changesOf(typeid(DTxn))}));
    auto const changes = morph::effects::detail::deriveChangesFrom(resolved, "A_Rows", "{}", ranOnInstance());
    REQUIRE(changes.records.size() == 3);
    REQUIRE(changes.records[0].record == "D_Txn");
    // A derived record of a derived record is reached too, and neither carries keys.
    REQUIRE(changes.records[1].record == "D_Report");
    REQUIRE(changes.records[2].record == "D_Summary");
    REQUIRE_FALSE(changes.records[1].keys.has_value());
    REQUIRE_FALSE(changes.records[2].keys.has_value());
}

TEST_CASE("derive_changes_instance_uses_ran_on", "[push][records]") {
    auto const resolved = derivation(declare("A_Rows", {DeclaredEffect{.kind = Kind::ChangesInstance, .record = {}}}));
    auto const changes = morph::effects::detail::deriveChangesFrom(resolved, "A_Rows", "{}", ranOnInstance());
    REQUIRE(changes.records.empty());
    REQUIRE(changes.instances.size() == 1);
    REQUIRE(changes.instances[0].modelType == "M");
    REQUIRE(changes.instances[0].instance == 7);
    REQUIRE(changes.instances[0].primary == "k");
}

TEST_CASE("derive_reads_nothing_for_reader", "[push][records]") {
    auto const resolved = derivation(declare("A_Other", {changesOf(typeid(VRow))}));
    REQUIRE(morph::effects::detail::deriveChangesFrom(resolved, "A_Rows", "{}", ranOnInstance()).empty());
    auto const none = derivation(declare("A_Rows", {DeclaredEffect{.kind = Kind::None, .record = {}}}));
    REQUIRE(morph::effects::detail::deriveChangesFrom(none, "A_Rows", "{}", ranOnInstance()).empty());
    // The process registry: a registered action that declared nothing reads.
    REQUIRE(morph::effects::deriveChanges("T_WalkerListRows", "{}", ranOnInstance()).empty());
}

TEST_CASE("derive_from_process_registry", "[push][records]") {
    auto const changes =
        morph::effects::deriveChanges("T_EffectsEdit", R"({"items":[{"id":5,"name":"a"}]})", ranOnInstance());
    // updates<Item> and changes<Item> together: the whole type.
    REQUIRE(changes.records.size() == 1);
    REQUIRE(changes.records[0].record == "T_EItem");
    REQUIRE_FALSE(changes.records[0].keys.has_value());
    auto const instance = morph::effects::deriveChanges("T_EffectsLoad", "{}", ranOnInstance());
    REQUIRE(instance.instances.size() == 1);
    REQUIRE(instance.records.empty());
}

// ── Deriving changes, branch by branch ───────────────────────────────────────

namespace {

using morph::effects::ActionRecord;
using morph::effects::Changes;
using morph::effects::Effect;
using morph::effects::detail::Resolved;

std::vector<glz::raw_json> rawKeys(std::vector<std::string> const& texts) {
    std::vector<glz::raw_json> keys;
    keys.reserve(texts.size());
    for (auto const& text : texts) {
        keys.emplace_back(text);
    }
    return keys;
}

// One action, `A`, whose result holds `D_Row` (keyed by `id`) at @p path, and which updates it.
Resolved rowsAt(std::string path) {
    Resolved resolved;
    resolved.records = {record("D_Row", typeid(VRow))};
    resolved.effects["A"] = {Effect{.kind = Kind::Updates, .record = "D_Row"}};
    resolved.actionRecords["A"] = {ActionRecord{.record = "D_Row", .keyField = "id", .paths = {std::move(path)}}};
    return resolved;
}

std::vector<std::string> updatedKeys(Resolved const& resolved, std::string_view result) {
    auto const changes = morph::effects::detail::deriveChangesFrom(resolved, "A", result, ranOnInstance());
    REQUIRE(changes.records.size() == 1);
    REQUIRE(changes.records[0].keys.has_value());
    return keyTexts(changes.records[0]);
}

}  // namespace

// Mutation: stop at the first array, or at an object lacking the segment — the nested and wrapped rows are missed.
TEST_CASE("derive_walks_arrays_and_wrappers_before_the_path_ends", "[push][records]") {
    auto const resolved = rowsAt("rows");
    // An array before the path is consumed is entered without consuming a segment; a scalar in it holds nothing.
    CHECK(updatedKeys(resolved, R"([{"rows":[{"id":1}]},5,{"rows":[{"id":2}]}])") ==
          std::vector<std::string>{"1", "2"});
    // An object without the segment is searched through its values, as a map would be.
    CHECK(updatedKeys(resolved, R"({"wrap":{"rows":[{"id":3}]},"other":"text"})") == std::vector<std::string>{"3"});
    // A map whose values lack the key, or are not records at all, names no row.
    CHECK(updatedKeys(resolved, R"({"rows":{"a":{"n":1},"b":5,"c":{"id":4}}})") == std::vector<std::string>{"4"});
}

// Mutation: append keys without the duplicate check, or let keys narrow a whole-type change.
TEST_CASE("record_changes_merge_keys_and_whole_types", "[push][records]") {
    Changes changes;
    morph::effects::detail::addRecordChange(changes, "R", rawKeys({"1", "2"}));
    morph::effects::detail::addRecordChange(changes, "R", rawKeys({"2", "3"}));
    REQUIRE(changes.records.size() == 1);
    CHECK(keyTexts(changes.records[0]) == std::vector<std::string>{"1", "2", "3"});
    morph::effects::detail::addRecordChange(changes, "R", std::nullopt);
    CHECK_FALSE(changes.records[0].keys.has_value());
    morph::effects::detail::addRecordChange(changes, "R", rawKeys({"4"}));
    CHECK_FALSE(changes.records[0].keys.has_value());
    morph::effects::detail::addRecordChange(changes, "S", rawKeys({"1"}));
    CHECK(changes.records.size() == 2);
}

// Mutation: compare instances by model type alone — a second instance of the same model is lost.
TEST_CASE("instance_changes_are_kept_once_each", "[push][records]") {
    using morph::effects::InstanceChange;
    Changes changes;
    InstanceChange const first{.modelType = "M", .instance = 7, .primary = "k"};
    morph::effects::detail::addInstanceChange(changes, first);
    morph::effects::detail::addInstanceChange(changes, first);
    CHECK(changes.instances.size() == 1);
    morph::effects::detail::addInstanceChange(changes, {.modelType = "N", .instance = 7, .primary = "k"});
    morph::effects::detail::addInstanceChange(changes, {.modelType = "M", .instance = 8, .primary = "k"});
    morph::effects::detail::addInstanceChange(changes, {.modelType = "M", .instance = 7, .primary = "j"});
    CHECK(changes.instances.size() == 4);
}

// Mutation: drop the visited check — a record derived along two routes is reported twice.
TEST_CASE("derive_reports_a_record_derived_along_two_routes_once", "[push][records]") {
    Resolved resolved;
    resolved.records = {record("D_Txn", typeid(DTxn)),
                        record("D_Report", typeid(DReport), std::nullopt, {typeid(DTxn)}),
                        record("D_Summary", typeid(DSummary), std::nullopt, {typeid(DTxn), typeid(DReport)})};
    resolved.effects["A"] = {Effect{.kind = Kind::Changes, .record = "D_Txn"}};
    auto const changes = morph::effects::detail::deriveChangesFrom(resolved, "A", "{}", ranOnInstance());
    REQUIRE(changes.records.size() == 3);
    CHECK(changes.records[1].record == "D_Report");
    CHECK(changes.records[2].record == "D_Summary");

    // A record the registries do not know has no derived records to add.
    Resolved unknown;
    unknown.effects["A"] = {Effect{.kind = Kind::Changes, .record = "D_Ghost"}};
    auto const ghost = morph::effects::detail::deriveChangesFrom(unknown, "A", "{}", ranOnInstance());
    REQUIRE(ghost.records.size() == 1);
    CHECK(ghost.records[0].record == "D_Ghost");
}

// Mutation: read every record the action holds as the updated one — D_Header's rows are reported as D_Row's.
TEST_CASE("derive_updates_names_rows_only_of_a_known_keyed_record", "[push][records]") {
    Resolved resolved;
    resolved.records = {record("D_Row", typeid(VRow)), record("D_Header", typeid(VHeader), std::nullopt)};
    resolved.effects["A"] = {
        Effect{.kind = Kind::Updates, .record = "D_Row"}, Effect{.kind = Kind::Updates, .record = "D_Header"},
        Effect{.kind = Kind::Updates, .record = "D_Ghost"}, Effect{.kind = static_cast<Kind>(99), .record = {}}};
    resolved.actionRecords["A"] = {ActionRecord{.record = "D_Header", .keyField = std::nullopt, .paths = {"header"}},
                                   ActionRecord{.record = "D_Row", .keyField = "id", .paths = {"rows"}}};
    auto const changes = morph::effects::detail::deriveChangesFrom(
        resolved, "A", R"({"header":{"id":9},"rows":[{"id":1}]})", ranOnInstance());
    REQUIRE(changes.records.size() == 3);
    CHECK(changes.records[0].record == "D_Row");
    CHECK(keyTexts(changes.records[0]) == std::vector<std::string>{"1"});
    // A keyless record, and one the registries do not know, are reported whole; an unknown kind adds nothing.
    CHECK(changes.records[1].record == "D_Header");
    CHECK_FALSE(changes.records[1].keys.has_value());
    CHECK(changes.records[2].record == "D_Ghost");
    CHECK_FALSE(changes.records[2].keys.has_value());
    CHECK(changes.instances.empty());
}

// Mutation: accept a prefix that only shares leading characters ("rows" within "rowsTotal").
TEST_CASE("path_within_is_a_whole_segment_prefix", "[push][records]") {
    using morph::effects::detail::pathWithin;
    CHECK(pathWithin("", "rows"));
    CHECK(pathWithin("rows", "rows"));
    CHECK(pathWithin("rows", "rows.items"));
    CHECK_FALSE(pathWithin("rows", "rowsTotal"));
    CHECK_FALSE(pathWithin("rows.items", "rows"));
    CHECK_FALSE(pathWithin("rows", "header.rows"));
}

TEST_CASE("same_registration_compares_every_option", "[push][records]") {
    using morph::effects::detail::sameRegistration;
    RecordInfo const base = record("D_Row", typeid(VRow));
    CHECK(sameRegistration(base, base));
    auto const differing = [&](auto change) {
        RecordInfo other = base;
        change(other);
        return !sameRegistration(base, other);
    };
    CHECK(differing([](RecordInfo& info) { info.name = "D_Other"; }));
    CHECK(differing([](RecordInfo& info) { info.type = typeid(VOther); }));
    CHECK(differing([](RecordInfo& info) { info.keyField = "code"; }));
    CHECK(differing([](RecordInfo& info) { info.derivedFrom = {typeid(DTxn)}; }));
    CHECK(differing([](RecordInfo& info) { info.privateKeys = true; }));
    CHECK(differing([](RecordInfo& info) { info.keyIsString = true; }));
}

TEST_CASE("effects_and_action_records_compare_by_value", "[push][records]") {
    CHECK(Effect{.kind = Kind::Updates, .record = "R"} == Effect{.kind = Kind::Updates, .record = "R"});
    CHECK_FALSE(Effect{.kind = Kind::Updates, .record = "R"} == Effect{.kind = Kind::Changes, .record = "R"});
    CHECK_FALSE(Effect{.kind = Kind::Updates, .record = "R"} == Effect{.kind = Kind::Updates, .record = "S"});
    ActionRecord const held{.record = "R", .keyField = "id", .paths = {"rows"}};
    CHECK(held == held);
    CHECK_FALSE(held == ActionRecord{.record = "S", .keyField = "id", .paths = {"rows"}});
    CHECK_FALSE(held == ActionRecord{.record = "R", .keyField = std::nullopt, .paths = {"rows"}});
    CHECK_FALSE(held == ActionRecord{.record = "R", .keyField = "id", .paths = {"items"}});
    CHECK(updatesOf(typeid(VRow)) == updatesOf(typeid(VRow)));
    CHECK_FALSE(updatesOf(typeid(VRow)) == changesOf(typeid(VRow)));
    CHECK_FALSE(updatesOf(typeid(VRow)) == updatesOf(typeid(VOther)));
}

// Mutation: keep one path per record — the second place the result holds D_Row is never read.
TEST_CASE("a_record_held_at_two_paths_keeps_both", "[push][records]") {
    ActionResultSlotMap slots;
    slots.emplace("A_Two", result({{.type = typeid(VList), .path = ""},
                                   {.type = typeid(VRow), .path = "mine"},
                                   {.type = typeid(VRow), .path = "theirs"}}));
    auto const resolved = morph::effects::detail::resolveForTesting({record("D_Row", typeid(VRow))}, slots, {});
    auto const held = resolved.recordsOf("A_Two");
    REQUIRE(held.size() == 1);
    CHECK(held[0].paths == std::vector<std::string>{"mine", "theirs"});
}

TEST_CASE("updates_on_a_record_the_result_does_not_hold_throws", "[push][records]") {
    ActionResultSlotMap slots;
    slots.emplace("A_Header", result({{.type = typeid(VHeader), .path = ""}}));
    EffectsMap declared;
    morph::effects::detail::addEffects(declared, "A_Header", {updatesOf(typeid(VRow))});
    auto const message =
        refusal({record("D_Row", typeid(VRow)), record("D_Header", typeid(VHeader))}, slots, declared);
    CHECK_THAT(message, Catch::Matchers::ContainsSubstring("its result holds no `D_Row`"));
}

// Mutation: skip the check — a record derived from a type that is no record is accepted and never refreshed.
TEST_CASE("derived_from_an_unregistered_type_throws", "[push][records]") {
    auto const message = refusal({record("D_Loose", typeid(VOther), std::nullopt, {typeid(VInner)})}, {}, {});
    CHECK_THAT(message, Catch::Matchers::ContainsSubstring("record `D_Loose` is derived from"));
    CHECK_THAT(message, Catch::Matchers::ContainsSubstring("which is not a registered record"));
}

// SPDX-License-Identifier: Apache-2.0
//
// Runtime custom fields (README build order §9, "the endgame") — the real
// build on top of EXTENSION-BAG-SPIKE.md's mechanism, scoped to Account
// only. Four layers, same order the spike itself checked in:
//   1. Definitions — AddCustomField/ListCustomFields, journaled, persisted.
//   2. Schema      — does the served CreateAccount/UpdateAccount schema grow
//                    a property node for a registered field?
//   3. Decode/persist — does a submitted custom value survive dispatch and a
//                       round trip through the database, or is it dropped?
//   4. Journal     — does a recorded CreateAccount/UpdateAccount entry carry
//                    the custom value verbatim, the same "journal needs no
//                    framework change" finding the spike already proved?
//
// Explicitly not covered here — named in custom_field_dto.hpp's own doc
// comment and crm/README.md's §9 design decisions, not silently assumed:
// per-field authz on custom fields, delete-a-field races, and unit/Choice-
// backed custom values.

#include <Lightweight/DataMapper/DataMapper.hpp>
#include <catch2/catch_test_macros.hpp>
#include <glaze/glaze.hpp>
#include <memory>
#include <morph/forms/forms.hpp>
#include <morph/journal/action_log.hpp>
#include <string>

#include "crm/core/errors.hpp"
#include "crm/db/crm_entity.hpp"
#include "crm/gui/crm_schemas.hpp"
#include "crm/models/account_model.hpp"
#include "crm/models/custom_field_model.hpp"
#include "crm_test_support.hpp"
#include "testkit/db_fixture.hpp"

using crm::test::ScopedPrincipal;
using morph::ladder::testkit::DbFixture;

// ── 1. Definitions ───────────────────────────────────────────────────────

TEST_CASE("AddCustomField declares a new field definition", "[crm][custom_fields]") {
    DbFixture fixture;
    const ScopedPrincipal alice{"alice"};
    crm::CustomFieldModel model;

    const auto result = model.execute(crm::AddCustomField{
        .entity = crm::CustomFieldEntity::Account, .name = "leadSource", .type = crm::CustomFieldType::Text});
    CHECK(result.name == "leadSource");
    CHECK(result.type == crm::CustomFieldType::Text);
    CHECK_FALSE(result.required);

    const auto listed = model.execute(crm::ListCustomFields{.entity = crm::CustomFieldEntity::Account});
    REQUIRE(listed.fields.size() == 1);
    CHECK(listed.fields[0].name == "leadSource");
}

TEST_CASE("A second AddCustomField naming the same field replaces its declaration", "[crm][custom_fields]") {
    DbFixture fixture;
    const ScopedPrincipal alice{"alice"};
    crm::CustomFieldModel model;

    model.execute(crm::AddCustomField{.name = "priority", .type = crm::CustomFieldType::Text, .required = false});
    model.execute(crm::AddCustomField{.name = "priority", .type = crm::CustomFieldType::Number, .required = true});

    const auto listed = model.execute(crm::ListCustomFields{});
    REQUIRE(listed.fields.size() == 1);  // not two rows — replaced, not duplicated
    CHECK(listed.fields[0].type == crm::CustomFieldType::Number);
    CHECK(listed.fields[0].required);
}

TEST_CASE("AddCustomField with no name is rejected", "[crm][custom_fields]") {
    DbFixture fixture;
    const ScopedPrincipal alice{"alice"};
    crm::CustomFieldModel model;
    CHECK_THROWS_AS(model.execute(crm::AddCustomField{.name = ""}), crm::ValidationError);
}

TEST_CASE("AddCustomField with no principal is refused", "[crm][custom_fields][audit]") {
    DbFixture fixture;
    crm::CustomFieldModel model;
    CHECK_THROWS_AS(model.execute(crm::AddCustomField{.name = "leadSource"}), crm::EmptyPrincipalError);
}

TEST_CASE("AddCustomField journals as a normal action", "[crm][custom_fields][audit]") {
    DbFixture fixture;
    const ScopedPrincipal alice{"alice"};
    auto log = std::make_shared<morph::journal::InMemoryActionLog>();
    crm::CustomFieldModel model;
    model.attachActionLog(log, std::string{"custom-fields"});

    model.execute(crm::AddCustomField{.name = "leadSource", .type = crm::CustomFieldType::Text});

    const auto entries = log->entries("custom-fields");
    REQUIRE(entries.size() == 1);
    CHECK(entries[0].actionType == "AddCustomField");
    CHECK(entries[0].outcome == morph::journal::Outcome::Succeeded);
    CHECK(entries[0].principal == "alice");
}

// ── 2. Schema growth ─────────────────────────────────────────────────────

TEST_CASE("An unregistered custom field is absent from CreateAccount's served schema",
          "[crm][custom_fields][schema]") {
    DbFixture fixture;
    const std::string schema = crm::gui::withCustomFields(::morph::forms::schemaJson<crm::CreateAccount>(), {});
    CHECK(schema.find("leadSource") == std::string::npos);
    CHECK(schema.find("\"name\"") != std::string::npos);  // the compiled field is there unconditionally
}

TEST_CASE("A registered custom field grows CreateAccount's served schema", "[crm][custom_fields][schema]") {
    DbFixture fixture;
    const ScopedPrincipal alice{"alice"};
    crm::CustomFieldModel customFields;
    customFields.execute(
        crm::AddCustomField{.name = "leadSource", .type = crm::CustomFieldType::Text, .required = false});
    const auto defs = customFields.execute(crm::ListCustomFields{}).fields;

    const std::string schema = crm::gui::withCustomFields(::morph::forms::schemaJson<crm::CreateAccount>(), defs);

    glz::generic_u64 dom{};
    REQUIRE_FALSE(glz::read_json(dom, schema));
    REQUIRE(dom.contains("properties"));
    auto& properties = dom["properties"];
    REQUIRE(properties.contains("leadSource"));
    CHECK(properties["leadSource"]["type"].get<std::string>() == "string");
    CHECK(properties["leadSource"]["x-custom"].get<bool>() == true);
    // The compiled fields are untouched — additive, not a rewrite.
    REQUIRE(properties.contains("name"));
    REQUIRE(properties.contains("industry"));
}

TEST_CASE("A required custom field is added to the served `required` array", "[crm][custom_fields][schema]") {
    DbFixture fixture;
    const ScopedPrincipal alice{"alice"};
    crm::CustomFieldModel customFields;
    customFields.execute(
        crm::AddCustomField{.name = "leadSource", .type = crm::CustomFieldType::Text, .required = true});
    const auto defs = customFields.execute(crm::ListCustomFields{}).fields;

    const std::string schema = crm::gui::withCustomFields(::morph::forms::schemaJson<crm::CreateAccount>(), defs);
    glz::generic_u64 dom{};
    REQUIRE_FALSE(glz::read_json(dom, schema));
    REQUIRE(dom.contains("required"));
    bool found = false;
    for (const auto& entry : dom["required"].get<glz::generic_u64::array_t>()) {
        if (entry.get<std::string>() == "leadSource") {
            found = true;
        }
    }
    CHECK(found);
}

TEST_CASE("crmSchemasJson includes AddCustomField", "[crm][custom_fields][schema]") {
    const std::string schema = crm::gui::crmSchemasJson();
    glz::generic_u64 dom{};
    REQUIRE_FALSE(glz::read_json(dom, schema));
    CHECK(dom.contains("AddCustomField"));
}

// ── 3. Decode / persist ──────────────────────────────────────────────────

TEST_CASE("A submitted custom value survives decode, not silently dropped", "[crm][custom_fields][decode]") {
    // Exactly the codec BRIDGE_REGISTER_ACTION generates (registry.hpp's
    // fromJson): lenient glz::read against the wire body — the same
    // mechanism EXTENSION-BAG-SPIKE.md's own decode test exercises.
    auto action = morph::model::ActionTraits<crm::CreateAccount>::fromJson(
        R"({"name":"Acme","industry":"","website":"","leadSource":"referral"})");
    CHECK(action.name == "Acme");
    REQUIRE(action.extra.contains("leadSource"));
    CHECK(action.extra.at("leadSource").get<std::string>() == "referral");
}

TEST_CASE("toJson re-emits a custom value flat, not nested under \"extra\"", "[crm][custom_fields][decode]") {
    crm::CreateAccount action{.name = "Acme", .industry = "", .website = ""};
    action.extra["leadSource"] = std::string{"referral"};

    const std::string wire = morph::model::ActionTraits<crm::CreateAccount>::toJson(action);
    CHECK(wire.find(R"("leadSource":"referral")") != std::string::npos);
    CHECK(wire.find("\"extra\"") == std::string::npos);
}

TEST_CASE("CreateAccount persists a custom value, and GetAccount reads it back", "[crm][custom_fields][persist]") {
    DbFixture fixture;
    const ScopedPrincipal alice{"alice"};
    crm::CustomFieldModel customFields;
    customFields.execute(crm::AddCustomField{.name = "leadSource", .type = crm::CustomFieldType::Text});

    crm::AccountModel accounts;
    crm::CreateAccount action{.name = "Acme", .industry = "", .website = ""};
    action.extra["leadSource"] = std::string{"referral"};
    const auto created = accounts.execute(action);

    const auto view = accounts.execute(crm::GetAccount{.accountId = created.accountId});
    REQUIRE(view.extra.contains("leadSource"));
    CHECK(view.extra.at("leadSource").get<std::string>() == "referral");
}

TEST_CASE("UpdateAccount replaces the account's custom values, full-replace not per-key diff",
          "[crm][custom_fields][persist]") {
    DbFixture fixture;
    const ScopedPrincipal alice{"alice"};
    crm::CustomFieldModel customFields;
    customFields.execute(crm::AddCustomField{.name = "leadSource", .type = crm::CustomFieldType::Text});
    customFields.execute(crm::AddCustomField{.name = "priority", .type = crm::CustomFieldType::Text});

    crm::AccountModel accounts;
    crm::CreateAccount create{.name = "Acme", .industry = "", .website = ""};
    create.extra["leadSource"] = std::string{"referral"};
    create.extra["priority"] = std::string{"high"};
    const auto created = accounts.execute(create);
    const auto before = accounts.execute(crm::GetAccount{.accountId = created.accountId});

    crm::UpdateAccount update{.accountId = created.accountId,
                              .name = "Acme",
                              .industry = "",
                              .website = "",
                              .expectedVersion = before.version};
    update.extra["leadSource"] = std::string{"conference"};  // priority omitted — dropped, not kept
    accounts.execute(update);

    const auto after = accounts.execute(crm::GetAccount{.accountId = created.accountId});
    REQUIRE(after.extra.contains("leadSource"));
    CHECK(after.extra.at("leadSource").get<std::string>() == "conference");
    CHECK_FALSE(after.extra.contains("priority"));
}

TEST_CASE("A missing required custom field is rejected on CreateAccount", "[crm][custom_fields][required]") {
    DbFixture fixture;
    const ScopedPrincipal alice{"alice"};
    crm::CustomFieldModel customFields;
    customFields.execute(
        crm::AddCustomField{.name = "leadSource", .type = crm::CustomFieldType::Text, .required = true});

    crm::AccountModel accounts;
    CHECK_THROWS_AS(accounts.execute(crm::CreateAccount{.name = "Acme", .industry = "", .website = ""}),
                    crm::ValidationError);
}

TEST_CASE("Supplying a required custom field allows CreateAccount to proceed", "[crm][custom_fields][required]") {
    DbFixture fixture;
    const ScopedPrincipal alice{"alice"};
    crm::CustomFieldModel customFields;
    customFields.execute(
        crm::AddCustomField{.name = "leadSource", .type = crm::CustomFieldType::Text, .required = true});

    crm::AccountModel accounts;
    crm::CreateAccount action{.name = "Acme", .industry = "", .website = ""};
    action.extra["leadSource"] = std::string{"referral"};
    CHECK_NOTHROW(accounts.execute(action));
}

TEST_CASE("A malformed stored custom value is skipped on read, not thrown on", "[crm][custom_fields][persist]") {
    // Reaching this state (a row whose value_json no longer decodes) is not
    // reachable through this rung's own write path — it stands in for a
    // custom field whose declared type changed after the value was written,
    // or a hand-edited row. GetAccount must not fail the whole read over one
    // bad value; it simply omits that key, matching loadCustomValues()'s own
    // documented lenient-on-read posture.
    DbFixture fixture;
    const ScopedPrincipal alice{"alice"};
    crm::AccountModel accounts;
    const auto created = accounts.execute(crm::CreateAccount{.name = "Acme", .industry = "", .website = ""});

    Lightweight::DataMapper mapper;
    crm::db::AccountCustomValueRecord row;
    row.account = static_cast<std::uint64_t>(*created.accountId);
    row.fieldName = Lightweight::SqlAnsiString<64>{"corrupt"};
    row.valueJson = Lightweight::SqlDynamicAnsiString<1024>{"{ not json"};
    mapper.Create(row);

    const auto view = accounts.execute(crm::GetAccount{.accountId = created.accountId});
    CHECK_FALSE(view.extra.contains("corrupt"));
}

// ── 4. Journal ────────────────────────────────────────────────────────────

TEST_CASE("A recorded CreateAccount entry carries the custom value verbatim", "[crm][custom_fields][journal]") {
    DbFixture fixture;
    const ScopedPrincipal alice{"alice"};
    crm::CustomFieldModel customFields;
    customFields.execute(crm::AddCustomField{.name = "leadSource", .type = crm::CustomFieldType::Text});

    auto log = std::make_shared<morph::journal::InMemoryActionLog>();
    crm::AccountModel accounts;
    accounts.attachActionLog(log, std::string{"accounts"});

    crm::CreateAccount action{.name = "Acme", .industry = "", .website = ""};
    action.extra["leadSource"] = std::string{"referral"};
    accounts.execute(action);

    const auto entries = log->entries("accounts");
    REQUIRE(entries.size() == 1);
    CHECK(entries[0].payload.find(R"("leadSource":"referral")") != std::string::npos);

    // Journal replay itself needed no framework change — the recorded
    // payload decodes through the identical fromJson every dispatch uses.
    const auto replayed = morph::model::ActionTraits<crm::CreateAccount>::fromJson(entries[0].payload);
    REQUIRE(replayed.extra.contains("leadSource"));
    CHECK(replayed.extra.at("leadSource").get<std::string>() == "referral");
}

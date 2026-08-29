// SPDX-License-Identifier: Apache-2.0
//
// Runtime custom fields, 7b: the three items step 9's own scope decision
// named out of writing rather than silently deciding (custom_field_dto.hpp's
// doc comment, crm/README.md's §9 design decisions):
//   1. Per-field authz — AddCustomField::minRoleToEdit, enforced the same
//      "resubmitting an unchanged value is not a change" way
//      UpdateAccount::industry's own write-guard already works.
//   2. Delete-a-field-while-in-use — DeleteCustomField cascade-deletes
//      stored values; an incoming write naming a now-deleted field is
//      rejected, not silently dropped or stored as an orphan.
//   3. Unit/Choice-backed values — CustomFieldType::Money (crm's existing
//      USD Money type) and CustomFieldType::Choice (a fixed, referentially-
//      validated option list).

#include <catch2/catch_test_macros.hpp>
#include <glaze/glaze.hpp>
#include <morph/util/rational.hpp>

#include "crm/core/errors.hpp"
#include "crm/models/account_model.hpp"
#include "crm/models/custom_field_model.hpp"
#include "crm_test_support.hpp"
#include "testkit/db_fixture.hpp"

using crm::test::ScopedPrincipal;
using morph::ladder::testkit::DbFixture;
using morph::math::DecimalPlaces;
using morph::math::Rational;

namespace {
crm::AccountId createAcme(crm::AccountModel& model) {
    return model.execute(crm::CreateAccount{.name = "Acme", .industry = "", .website = ""}).accountId;
}
}  // namespace

// ── 1. Per-field authz ───────────────────────────────────────────────────

TEST_CASE("Changing a Manager-gated custom field requires Manager", "[crm][custom_fields][authz]") {
    DbFixture fixture;
    const ScopedPrincipal alice{"alice"};
    crm::CustomFieldModel customFields;
    customFields.execute(crm::AddCustomField{
        .name = "contractValue", .type = crm::CustomFieldType::Text, .minRoleToEdit = crm::Role::Manager});

    crm::AccountModel accounts;
    crm::CreateAccount create{.name = "Acme", .industry = "", .website = ""};
    create.extra["contractValue"] = std::string{"confidential"};
    const auto created = accounts.execute(create);  // CreateAccount never checks minRoleToEdit — see doc comment
    const auto before = accounts.execute(crm::GetAccount{.accountId = created.accountId});

    // alice has no role row on this account -- authz.hpp's fail-open default
    // makes her Role::Manager, so set an explicit lower role to exercise the
    // guard meaningfully.
    accounts.execute(
        crm::SetAccountRole{.accountId = created.accountId, .principal = "bob", .role = crm::Role::Member});

    const ScopedPrincipal bob{"bob"};
    crm::UpdateAccount update{.accountId = created.accountId,
                              .name = "Acme",
                              .industry = "",
                              .website = "",
                              .expectedVersion = before.version};
    update.extra["contractValue"] = std::string{"changed by bob"};
    CHECK_THROWS_AS(accounts.execute(update), crm::Forbidden);
}

TEST_CASE("Resubmitting a Manager-gated custom field's current value does not require Manager",
          "[crm][custom_fields][authz]") {
    DbFixture fixture;
    const ScopedPrincipal alice{"alice"};
    crm::CustomFieldModel customFields;
    customFields.execute(crm::AddCustomField{
        .name = "contractValue", .type = crm::CustomFieldType::Text, .minRoleToEdit = crm::Role::Manager});

    crm::AccountModel accounts;
    crm::CreateAccount create{.name = "Acme", .industry = "", .website = ""};
    create.extra["contractValue"] = std::string{"confidential"};
    const auto created = accounts.execute(create);
    accounts.execute(
        crm::SetAccountRole{.accountId = created.accountId, .principal = "bob", .role = crm::Role::Member});
    const auto before = accounts.execute(crm::GetAccount{.accountId = created.accountId});

    const ScopedPrincipal bob{"bob"};
    crm::UpdateAccount update{.accountId = created.accountId,
                              .name = "Acme — renamed",  // a compiled-field change bob IS allowed to make
                              .industry = "",
                              .website = "",
                              .expectedVersion = before.version};
    update.extra["contractValue"] = std::string{"confidential"};  // unchanged
    CHECK_NOTHROW(accounts.execute(update));
}

TEST_CASE("A Member-gated (default) custom field's value can be changed by any account member",
          "[crm][custom_fields][authz]") {
    DbFixture fixture;
    const ScopedPrincipal alice{"alice"};
    crm::CustomFieldModel customFields;
    customFields.execute(crm::AddCustomField{.name = "leadSource", .type = crm::CustomFieldType::Text});

    crm::AccountModel accounts;
    const auto created = createAcme(accounts);
    accounts.execute(crm::SetAccountRole{.accountId = created, .principal = "bob", .role = crm::Role::Member});
    const auto before = accounts.execute(crm::GetAccount{.accountId = created});

    const ScopedPrincipal bob{"bob"};
    crm::UpdateAccount update{
        .accountId = created, .name = "Acme", .industry = "", .website = "", .expectedVersion = before.version};
    update.extra["leadSource"] = std::string{"conference"};
    CHECK_NOTHROW(accounts.execute(update));
}

// ── 2. Delete-a-field-while-in-use ───────────────────────────────────────

TEST_CASE("DeleteCustomField cascade-deletes every account's stored value for it", "[crm][custom_fields][delete]") {
    DbFixture fixture;
    const ScopedPrincipal alice{"alice"};
    crm::CustomFieldModel customFields;
    customFields.execute(crm::AddCustomField{.name = "leadSource", .type = crm::CustomFieldType::Text});

    crm::AccountModel accounts;
    crm::CreateAccount create{.name = "Acme", .industry = "", .website = ""};
    create.extra["leadSource"] = std::string{"referral"};
    const auto created = accounts.execute(create);
    REQUIRE(accounts.execute(crm::GetAccount{.accountId = created.accountId}).extra.contains("leadSource"));

    customFields.execute(crm::DeleteCustomField{.name = "leadSource"});

    const auto view = accounts.execute(crm::GetAccount{.accountId = created.accountId});
    CHECK_FALSE(view.extra.contains("leadSource"));
}

TEST_CASE("Deleting one field leaves other accounts' other custom values untouched", "[crm][custom_fields][delete]") {
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

    customFields.execute(crm::DeleteCustomField{.name = "leadSource"});

    const auto view = accounts.execute(crm::GetAccount{.accountId = created.accountId});
    CHECK_FALSE(view.extra.contains("leadSource"));
    REQUIRE(view.extra.contains("priority"));
    CHECK(view.extra.at("priority").get<std::string>() == "high");
}

TEST_CASE("Submitting a deleted field's key is rejected, not silently dropped or stored",
          "[crm][custom_fields][delete]") {
    DbFixture fixture;
    const ScopedPrincipal alice{"alice"};
    crm::CustomFieldModel customFields;
    customFields.execute(crm::AddCustomField{.name = "leadSource", .type = crm::CustomFieldType::Text});
    customFields.execute(crm::DeleteCustomField{.name = "leadSource"});

    crm::AccountModel accounts;
    crm::CreateAccount create{.name = "Acme", .industry = "", .website = ""};
    create.extra["leadSource"] = std::string{"referral"};  // names a field that no longer exists
    CHECK_THROWS_AS(accounts.execute(create), crm::ValidationError);
}

TEST_CASE("ListCustomFields no longer lists a deleted field", "[crm][custom_fields][delete]") {
    DbFixture fixture;
    const ScopedPrincipal alice{"alice"};
    crm::CustomFieldModel model;
    model.execute(crm::AddCustomField{.name = "leadSource", .type = crm::CustomFieldType::Text});
    model.execute(crm::DeleteCustomField{.name = "leadSource"});
    CHECK(model.execute(crm::ListCustomFields{}).fields.empty());
}

TEST_CASE("A field re-added after delete starts genuinely fresh, not restoring old values",
          "[crm][custom_fields][delete]") {
    DbFixture fixture;
    const ScopedPrincipal alice{"alice"};
    crm::CustomFieldModel customFields;
    customFields.execute(crm::AddCustomField{.name = "leadSource", .type = crm::CustomFieldType::Text});

    crm::AccountModel accounts;
    crm::CreateAccount create{.name = "Acme", .industry = "", .website = ""};
    create.extra["leadSource"] = std::string{"referral"};
    const auto created = accounts.execute(create);

    customFields.execute(crm::DeleteCustomField{.name = "leadSource"});
    customFields.execute(crm::AddCustomField{.name = "leadSource", .type = crm::CustomFieldType::Text});

    // The re-added field has no value for this account -- the old value was
    // cascade-deleted, not preserved as a restorable orphan.
    const auto view = accounts.execute(crm::GetAccount{.accountId = created.accountId});
    CHECK_FALSE(view.extra.contains("leadSource"));
}

TEST_CASE("DeleteCustomField naming an undefined field is NotFound", "[crm][custom_fields][delete]") {
    DbFixture fixture;
    const ScopedPrincipal alice{"alice"};
    crm::CustomFieldModel model;
    CHECK_THROWS_AS(model.execute(crm::DeleteCustomField{.name = "neverDefined"}), crm::NotFound);
}

TEST_CASE("DeleteCustomField with no principal is refused", "[crm][custom_fields][delete][audit]") {
    DbFixture fixture;
    crm::CustomFieldModel model;
    CHECK_THROWS_AS(model.execute(crm::DeleteCustomField{.name = "x"}), crm::EmptyPrincipalError);
}

// ── 3. Money- and Choice-backed custom values ────────────────────────────

TEST_CASE("A Money-typed custom value round-trips through create/get", "[crm][custom_fields][money]") {
    DbFixture fixture;
    const ScopedPrincipal alice{"alice"};
    crm::CustomFieldModel customFields;
    customFields.execute(crm::AddCustomField{.name = "renewalValue", .type = crm::CustomFieldType::Money});

    crm::AccountModel accounts;
    crm::CreateAccount create{.name = "Acme", .industry = "", .website = ""};
    // Money's own wire shape (morph::units::Quantity), decoded directly into
    // the generic_u64 DOM CrmCustomValue actually is -- the same "no bespoke
    // Value type needed" claim custom_field_dto.hpp's own doc comment makes.
    const auto money = crm::Money{Rational{12345, DecimalPlaces{2}}};
    glz::generic_u64 moneyDom{};
    REQUIRE_FALSE(glz::read_json(moneyDom, glz::write_json(money).value_or("")));
    create.extra["renewalValue"] = moneyDom;
    const auto created = accounts.execute(create);

    const auto view = accounts.execute(crm::GetAccount{.accountId = created.accountId});
    REQUIRE(view.extra.contains("renewalValue"));
    crm::Money decodedMoney{};
    REQUIRE_FALSE(glz::read_json(decodedMoney, glz::write_json(view.extra.at("renewalValue")).value_or("")));
    REQUIRE(decodedMoney.hasValue());
    CHECK(*decodedMoney == Rational{12345, DecimalPlaces{2}});
}

TEST_CASE("A Choice-typed custom field accepts a declared option", "[crm][custom_fields][choice]") {
    DbFixture fixture;
    const ScopedPrincipal alice{"alice"};
    crm::CustomFieldModel customFields;
    customFields.execute(crm::AddCustomField{
        .name = "priority", .type = crm::CustomFieldType::Choice, .choiceOptions = {"Low", "Medium", "High"}});

    crm::AccountModel accounts;
    crm::CreateAccount create{.name = "Acme", .industry = "", .website = ""};
    create.extra["priority"] = std::string{"High"};
    const auto created = accounts.execute(create);

    const auto view = accounts.execute(crm::GetAccount{.accountId = created.accountId});
    REQUIRE(view.extra.contains("priority"));
    CHECK(view.extra.at("priority").get<std::string>() == "High");
}

TEST_CASE("A Choice-typed custom field rejects a value outside its declared options", "[crm][custom_fields][choice]") {
    DbFixture fixture;
    const ScopedPrincipal alice{"alice"};
    crm::CustomFieldModel customFields;
    customFields.execute(crm::AddCustomField{
        .name = "priority", .type = crm::CustomFieldType::Choice, .choiceOptions = {"Low", "Medium", "High"}});

    crm::AccountModel accounts;
    crm::CreateAccount create{.name = "Acme", .industry = "", .website = ""};
    create.extra["priority"] = std::string{"Critical"};  // not a declared option
    CHECK_THROWS_AS(accounts.execute(create), crm::ValidationError);
}

TEST_CASE("A Choice-typed custom field rejects a non-string value even if it happens to be listed",
          "[crm][custom_fields][choice]") {
    DbFixture fixture;
    const ScopedPrincipal alice{"alice"};
    crm::CustomFieldModel customFields;
    customFields.execute(
        crm::AddCustomField{.name = "priority", .type = crm::CustomFieldType::Choice, .choiceOptions = {"1", "2"}});

    crm::AccountModel accounts;
    crm::CreateAccount create{.name = "Acme", .industry = "", .website = ""};
    create.extra["priority"] = std::int64_t{1};  // a number, not the string "1"
    CHECK_THROWS_AS(accounts.execute(create), crm::ValidationError);
}

TEST_CASE("AddCustomField declaring a Choice type with no options is rejected", "[crm][custom_fields][choice]") {
    DbFixture fixture;
    const ScopedPrincipal alice{"alice"};
    crm::CustomFieldModel model;
    CHECK_THROWS_AS(model.execute(crm::AddCustomField{.name = "priority", .type = crm::CustomFieldType::Choice}),
                    crm::ValidationError);
}

TEST_CASE("ListCustomFields serves a Choice field's declared options and Money field's type",
          "[crm][custom_fields][choice][money]") {
    DbFixture fixture;
    const ScopedPrincipal alice{"alice"};
    crm::CustomFieldModel model;
    model.execute(crm::AddCustomField{
        .name = "priority", .type = crm::CustomFieldType::Choice, .choiceOptions = {"Low", "High"}});
    model.execute(crm::AddCustomField{.name = "renewalValue", .type = crm::CustomFieldType::Money});

    const auto fields = model.execute(crm::ListCustomFields{}).fields;
    REQUIRE(fields.size() == 2);
    for (const auto& field : fields) {
        if (field.name == "priority") {
            CHECK(field.type == crm::CustomFieldType::Choice);
            CHECK(field.choiceOptions == std::vector<std::string>{"Low", "High"});
        } else {
            CHECK(field.name == "renewalValue");
            CHECK(field.type == crm::CustomFieldType::Money);
        }
    }
}

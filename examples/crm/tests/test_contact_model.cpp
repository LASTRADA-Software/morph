// SPDX-License-Identifier: Apache-2.0
//
// ContactModel CRUD, and the Choice-backed account lookup (README build
// order §1-2).

#include <Lightweight/DataMapper/DataMapper.hpp>
#include <catch2/catch_test_macros.hpp>
#include <memory>
#include <morph/journal/action_log.hpp>
#include <morph/session/session.hpp>
#include <string>

#include "crm/core/errors.hpp"
#include "crm/models/account_model.hpp"
#include "crm/models/contact_model.hpp"
#include "crm_test_support.hpp"
#include "testkit/db_fixture.hpp"

using crm::test::ScopedPrincipal;
using morph::ladder::testkit::DbFixture;

namespace {
crm::AccountId createAcme(crm::AccountModel& accounts) {
    return accounts
        .execute(crm::CreateAccount{.name = "Acme Corp", .industry = "Manufacturing", .website = "acme.example"})
        .accountId;
}

crm::CreateContact ada(crm::AccountChoice account) {
    return crm::CreateContact{.account = std::move(account),
                              .firstName = "Ada",
                              .lastName = "Lovelace",
                              .email = "ada@example.test",
                              .phone = "555-0100"};
}
}  // namespace

TEST_CASE("CreateContact creates the contact under its account", "[crm][contact]") {
    DbFixture fixture;
    const ScopedPrincipal alice{"alice"};
    crm::AccountModel accounts;
    crm::ContactModel contacts;

    const auto accountId = createAcme(accounts);
    const auto created = contacts.execute(ada(crm::AccountChoice{std::to_string(*accountId)}));
    REQUIRE(created.contactId.hasValue());

    const auto view = contacts.execute(crm::GetContact{.contactId = created.contactId});
    CHECK(view.firstName == "Ada");
    CHECK(view.lastName == "Lovelace");
    CHECK(view.accountId == accountId);
    CHECK(view.version == 1);
}

TEST_CASE("CreateContact naming a nonexistent account is NotFound (review D6: stale Choice id)", "[crm][contact]") {
    DbFixture fixture;
    const ScopedPrincipal alice{"alice"};
    crm::ContactModel contacts;

    CHECK_THROWS_AS(contacts.execute(ada(crm::AccountChoice{std::string{"999"}})), crm::NotFound);
}

TEST_CASE("CreateContact with no account selected is rejected", "[crm][contact]") {
    DbFixture fixture;
    const ScopedPrincipal alice{"alice"};
    crm::ContactModel contacts;

    auto bad = ada(crm::AccountChoice{});
    CHECK_THROWS_AS(contacts.execute(bad), crm::ValidationError);
}

TEST_CASE("Creating a contact with no principal is refused", "[crm][contact][audit]") {
    DbFixture fixture;
    crm::AccountModel accounts;
    crm::ContactModel contacts;
    crm::AccountId accountId;
    {
        const ScopedPrincipal alice{"alice"};
        accountId = createAcme(accounts);
    }
    // alice's scope has ended: the create below runs with no principal.
    CHECK_THROWS_AS(contacts.execute(ada(crm::AccountChoice{std::to_string(*accountId)})), crm::EmptyPrincipalError);
}

TEST_CASE("UpdateContact replaces fields and bumps the version", "[crm][contact]") {
    DbFixture fixture;
    const ScopedPrincipal alice{"alice"};
    crm::AccountModel accounts;
    crm::ContactModel contacts;

    const auto accountId = createAcme(accounts);
    const auto created = contacts.execute(ada(crm::AccountChoice{std::to_string(*accountId)}));

    const auto updated = contacts.execute(crm::UpdateContact{
        .contactId = created.contactId,
        .account = crm::AccountChoice{std::to_string(*accountId)},
        .firstName = "Ada",
        .lastName = "King",  // married name
        .email = "ada.king@example.test",
        .phone = "555-0100",
        .expectedVersion = 1,
    });
    CHECK(updated.contact.lastName == "King");
    CHECK(updated.contact.version == 2);
}

TEST_CASE("UpdateContact with a stale version is a Conflict", "[crm][contact]") {
    DbFixture fixture;
    const ScopedPrincipal alice{"alice"};
    crm::AccountModel accounts;
    crm::ContactModel contacts;

    const auto accountId = createAcme(accounts);
    const auto created = contacts.execute(ada(crm::AccountChoice{std::to_string(*accountId)}));

    CHECK_THROWS_AS(contacts.execute(crm::UpdateContact{
                        .contactId = created.contactId,
                        .account = crm::AccountChoice{std::to_string(*accountId)},
                        .firstName = "Ada",
                        .lastName = "King",
                        .email = "ada.king@example.test",
                        .phone = "555-0100",
                        .expectedVersion = 0,  // stale
                    }),
                    crm::Conflict);
}

TEST_CASE("ListContacts filters by account when accountId is engaged", "[crm][contact]") {
    DbFixture fixture;
    const ScopedPrincipal alice{"alice"};
    crm::AccountModel accounts;
    crm::ContactModel contacts;

    const auto acmeId = createAcme(accounts);
    const auto globexId =
        accounts.execute(crm::CreateAccount{.name = "Globex", .industry = "Retail", .website = "globex.example"})
            .accountId;

    contacts.execute(ada(crm::AccountChoice{std::to_string(*acmeId)}));
    contacts.execute(crm::CreateContact{
        .account = crm::AccountChoice{std::to_string(*globexId)},
        .firstName = "Hank",
        .lastName = "Scorpio",
        .email = "hank@globex.example",
        .phone = "",
    });

    const auto acmeContacts = contacts.execute(crm::ListContacts{.accountId = acmeId});
    REQUIRE(acmeContacts.contacts.size() == 1);
    CHECK(acmeContacts.contacts.front().firstName == "Ada");

    const auto allContacts = contacts.execute(crm::ListContacts{});
    CHECK(allContacts.contacts.size() == 2);
}

TEST_CASE("ListContactOptions serves {id, name} rows for the primary-contact Choice combo", "[crm][contact]") {
    DbFixture fixture;
    const ScopedPrincipal alice{"alice"};
    crm::AccountModel accounts;
    crm::ContactModel contacts;

    const auto accountId = createAcme(accounts);
    const auto created = contacts.execute(ada(crm::AccountChoice{std::to_string(*accountId)}));

    const auto options = contacts.execute(crm::ListContactOptions{.accountId = accountId});
    REQUIRE(options.contacts.size() == 1);
    CHECK(options.contacts.front().id == std::to_string(*created.contactId));
    CHECK(options.contacts.front().name == "Ada Lovelace");
}

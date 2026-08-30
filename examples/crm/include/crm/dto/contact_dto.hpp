// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <morph/forms/forms.hpp>
#include <optional>
#include <string>
#include <vector>

#include "crm/core/types.hpp"

/// @file
/// Contact CRUD action/result DTOs. The account link is a
/// `forms::Choice`-backed lookup field (README step 2), served by
/// `AccountModel::execute(const ListAccountOptions&)`.

namespace crm {

/// @brief `forms::Choice` over an account id, backed by `ListAccountOptions`
///        (`account_dto.hpp`) — the "account combo on a contact" README
///        step 2 names.
using AccountChoice = ::morph::forms::Choice<std::string, "ListAccountOptions", "id", "name">;

struct ContactView {
    ContactId id;
    AccountId accountId;
    std::string firstName;
    std::string lastName;
    std::string email;
    std::string phone;
    std::int32_t version = 0;
};

struct CreateContact {
    AccountChoice account;
    std::string firstName;
    std::string lastName;
    std::string email;
    std::string phone;

    [[nodiscard]] bool validate() const noexcept {
        return account.hasValue() && !firstName.empty() && !lastName.empty();
    }
};

struct CreateContactResult {
    ContactId contactId;
};

struct UpdateContact {
    ContactId contactId;
    AccountChoice account;
    std::string firstName;
    std::string lastName;
    std::string email;
    std::string phone;
    std::int32_t expectedVersion = 0;

    [[nodiscard]] bool validate() const noexcept {
        return contactId.hasValue() && account.hasValue() && !firstName.empty() && !lastName.empty();
    }
};

struct UpdateContactResult {
    ContactView contact;
};

struct GetContact {
    ContactId contactId;

    [[nodiscard]] bool validate() const noexcept { return contactId.hasValue(); }
};

/// @brief Lists every contact, or every contact belonging to one account when
///        `accountId` is engaged — the "contacts of an account" child
///        collection README step 2 names, before nested-aggregate child
///        tables (step 2's other half) are in scope.
struct ListContacts {
    std::optional<AccountId> accountId;

    [[nodiscard]] bool validate() const noexcept { return true; }
};

struct ListContactsResult {
    std::vector<ContactView> contacts;
};

/// @brief `{id, name}` rows for a `forms::Choice`-backed contact lookup field
///        (an opportunity's primary contact, README step 2).
struct ContactOption {
    std::string id;
    std::string name;
};

struct ListContactOptions {
    std::optional<AccountId> accountId;

    [[nodiscard]] bool validate() const noexcept { return true; }
};

struct ListContactOptionsResult {
    std::vector<ContactOption> contacts;
};

}  // namespace crm

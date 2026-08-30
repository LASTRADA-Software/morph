// SPDX-License-Identifier: Apache-2.0
#include "crm/models/contact_model.hpp"

#include <Lightweight/DataMapper/DataMapper.hpp>
#include <charconv>

#include "crm/core/errors.hpp"
#include "crm/core/model_support.hpp"
#include "crm/db/crm_entity.hpp"

namespace crm {

namespace {

/// @brief Parses the `AccountChoice`'s wire string id back into `AccountId`.
///
/// `AccountChoice = forms::Choice<std::string, "ListAccountOptions", "id",
/// "name">` (contact_dto.hpp): options rows carry `id` as a string
/// (`AccountOption::id`, minted with `std::to_string` by
/// `AccountModel::execute(const ListAccountOptions&)`) so it composes with
/// `Choice`'s string-keyed convention the way `lims::QualifierChoice` does —
/// the model is what turns it back into the strong id it names.
AccountId parseAccountChoice(const AccountChoice& choice) {
    if (!choice.hasValue()) {
        return AccountId{};
    }
    const std::string& text = *choice;
    std::int64_t parsed = 0;
    auto result = std::from_chars(text.data(), text.data() + text.size(), parsed);
    if (result.ec != std::errc{}) {
        throw ValidationError{"account: not a valid account id"};
    }
    return AccountId{parsed};
}

ContactView toView(const db::ContactRecord& row) {
    return ContactView{
        .id = ContactId{static_cast<std::int64_t>(row.id.Value())},
        .accountId = AccountId{static_cast<std::int64_t>(row.account.Value())},
        .firstName = std::string{row.firstName.Value().ToStringView()},
        .lastName = std::string{row.lastName.Value().ToStringView()},
        .email = std::string{row.email.Value().ToStringView()},
        .phone = std::string{row.phone.Value().ToStringView()},
        .version = row.version.Value(),
    };
}

}  // namespace

CreateContactResult ContactModel::execute(const CreateContact& action) {
    requirePrincipal();
    if (!action.validate()) {
        throw ValidationError{"CreateContact: account, firstName and lastName are required"};
    }
    const AccountId accountId = parseAccountChoice(action.account);

    Lightweight::DataMapper mapper;
    auto accountRows = mapper.Query<db::AccountRecord>()
                           .Where(::Lightweight::FieldNameOf<&db::AccountRecord::id>, "=", *accountId)
                           .All();
    if (accountRows.empty()) {
        // review D6 (crm/README.md): stale Choice id (row deleted between
        // fetch and submit) — the referential re-check the forms layer never
        // does, done here at the model.
        throw NotFound{"CreateContact: no such account"};
    }

    db::ContactRecord row;
    row.account = accountRows.front();
    row.firstName = Lightweight::SqlAnsiString<64>{action.firstName};
    row.lastName = Lightweight::SqlAnsiString<64>{action.lastName};
    row.email = Lightweight::SqlAnsiString<255>{action.email};
    row.phone = Lightweight::SqlAnsiString<32>{action.phone};
    row.createdAt = nowMillis();
    row.version = 1;
    mapper.Create(row);

    CreateContactResult result{.contactId = ContactId{static_cast<std::int64_t>(row.id.Value())}};
    _journal.recordSuccess<ContactModel>(action, result, nowMillis());
    return result;
}

UpdateContactResult ContactModel::execute(const UpdateContact& action) {
    requirePrincipal();
    if (!action.validate()) {
        throw ValidationError{"UpdateContact: contactId, account, firstName and lastName are required"};
    }
    const AccountId accountId = parseAccountChoice(action.account);

    Lightweight::DataMapper mapper;
    auto rows = mapper.Query<db::ContactRecord>()
                    .Where(::Lightweight::FieldNameOf<&db::ContactRecord::id>, "=", *action.contactId)
                    .All();
    if (rows.empty()) {
        throw NotFound{"UpdateContact: no such contact"};
    }
    auto& row = rows.front();
    if (row.version.Value() != action.expectedVersion) {
        throw Conflict{"UpdateContact: version mismatch — record was edited concurrently"};
    }

    auto accountRows = mapper.Query<db::AccountRecord>()
                           .Where(::Lightweight::FieldNameOf<&db::AccountRecord::id>, "=", *accountId)
                           .All();
    if (accountRows.empty()) {
        throw NotFound{"UpdateContact: no such account"};
    }

    row.account = accountRows.front();
    row.firstName = Lightweight::SqlAnsiString<64>{action.firstName};
    row.lastName = Lightweight::SqlAnsiString<64>{action.lastName};
    row.email = Lightweight::SqlAnsiString<255>{action.email};
    row.phone = Lightweight::SqlAnsiString<32>{action.phone};
    row.version = row.version.Value() + 1;
    mapper.Update(row);

    UpdateContactResult result{.contact = toView(row)};
    _journal.recordSuccess<ContactModel>(action, result, nowMillis());
    return result;
}

ContactView ContactModel::execute(const GetContact& action) {
    if (!action.validate()) {
        throw ValidationError{"GetContact: contactId is required"};
    }
    Lightweight::DataMapper mapper;
    auto rows = mapper.Query<db::ContactRecord>()
                    .Where(::Lightweight::FieldNameOf<&db::ContactRecord::id>, "=", *action.contactId)
                    .All();
    if (rows.empty()) {
        throw NotFound{"GetContact: no such contact"};
    }
    return toView(rows.front());
}

ListContactsResult ContactModel::execute(const ListContacts& action) {
    Lightweight::DataMapper mapper;
    auto query = mapper.Query<db::ContactRecord>();
    ListContactsResult result;
    if (action.accountId.has_value() && action.accountId->hasValue()) {
        auto rows =
            query.Where(::Lightweight::FieldNameOf<&db::ContactRecord::account>, "=", **action.accountId).All();
        result.contacts.reserve(rows.size());
        for (const auto& row : rows) {
            result.contacts.push_back(toView(row));
        }
    } else {
        auto rows = query.All();
        result.contacts.reserve(rows.size());
        for (const auto& row : rows) {
            result.contacts.push_back(toView(row));
        }
    }
    return result;
}

ListContactOptionsResult ContactModel::execute(const ListContactOptions& action) {
    Lightweight::DataMapper mapper;
    auto query = mapper.Query<db::ContactRecord>();
    ListContactOptionsResult result;
    const auto pushRow = [&result](const db::ContactRecord& row) {
        result.contacts.push_back(ContactOption{
            .id = std::to_string(row.id.Value()),
            .name = std::string{row.firstName.Value().ToStringView()} + " " +
                    std::string{row.lastName.Value().ToStringView()},
        });
    };
    if (action.accountId.has_value() && action.accountId->hasValue()) {
        auto rows =
            query.Where(::Lightweight::FieldNameOf<&db::ContactRecord::account>, "=", **action.accountId).All();
        result.contacts.reserve(rows.size());
        for (const auto& row : rows) {
            pushRow(row);
        }
    } else {
        auto rows = query.All();
        result.contacts.reserve(rows.size());
        for (const auto& row : rows) {
            pushRow(row);
        }
    }
    return result;
}

}  // namespace crm

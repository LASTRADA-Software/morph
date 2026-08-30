// SPDX-License-Identifier: Apache-2.0
#include "crm/models/custom_field_model.hpp"

#include <Lightweight/DataMapper/DataMapper.hpp>
#include <Lightweight/SqlTransaction.hpp>
#include <glaze/glaze.hpp>

#include "crm/core/errors.hpp"
#include "crm/core/model_support.hpp"
#include "crm/db/crm_entity.hpp"

namespace crm {

namespace {

/// @brief Serialises @p options as a JSON array — the wire/storage shape
///        `db::CustomFieldDefRecord::choiceOptionsJson` persists.
std::string encodeChoiceOptions(const std::vector<std::string>& options) {
    return glz::write_json(options).value_or("[]");
}

/// @brief Decodes a stored `choiceOptionsJson` value, or `{}` if @p row
///        never had one set (a pre-7b field created before this column
///        existed) or it does not decode.
std::vector<std::string> decodeChoiceOptions(const db::CustomFieldDefRecord& row) {
    if (!row.choiceOptionsJson.Value().has_value()) {
        return {};
    }
    std::vector<std::string> options;
    if (glz::read_json(options, std::string{row.choiceOptionsJson.Value()->ToStringView()})) {
        return {};  // malformed stored content: treated as "no options", same lenient-on-read posture
                    // AccountModel::loadCustomValues already takes for a value that no longer decodes.
    }
    return options;
}

/// @brief `row.minRoleToEdit`, or `Role::Member` if it was never set (a
///        pre-7b field created before this column existed).
Role decodeMinRoleToEdit(const db::CustomFieldDefRecord& row) {
    if (!row.minRoleToEdit.Value().has_value()) {
        return Role::Member;
    }
    return static_cast<Role>(*row.minRoleToEdit.Value());
}

}  // namespace

AddCustomFieldResult CustomFieldModel::execute(const AddCustomField& action) {
    requirePrincipal();
    if (!action.validate()) {
        throw ValidationError{"AddCustomField: name is required, and a Choice-typed field needs at least one option"};
    }

    Lightweight::DataMapper mapper;
    auto rows =
        mapper.Query<db::CustomFieldDefRecord>()
            .Where(::Lightweight::FieldNameOf<&db::CustomFieldDefRecord::entity>, "=", static_cast<int>(action.entity))
            .Where(::Lightweight::FieldNameOf<&db::CustomFieldDefRecord::name>, "=", action.name)
            .All();
    if (!rows.empty()) {
        // Replace-by-name: a second AddCustomField naming an existing field
        // updates its declared type/required-ness/authz/options rather than
        // erroring, the same "upsert by natural key" shape SetAccountRole
        // already uses for a repeated (account, principal) pair.
        auto& row = rows.front();
        row.type = static_cast<int>(action.type);
        row.required = action.required;
        row.minRoleToEdit = static_cast<int>(action.minRoleToEdit);
        row.choiceOptionsJson = Lightweight::SqlDynamicAnsiString<1024>{encodeChoiceOptions(action.choiceOptions)};
        mapper.Update(row);
    } else {
        db::CustomFieldDefRecord row;
        row.entity = static_cast<int>(action.entity);
        row.name = Lightweight::SqlAnsiString<64>{action.name};
        row.type = static_cast<int>(action.type);
        row.required = action.required;
        row.minRoleToEdit = static_cast<int>(action.minRoleToEdit);
        row.choiceOptionsJson = Lightweight::SqlDynamicAnsiString<1024>{encodeChoiceOptions(action.choiceOptions)};
        mapper.Create(row);
    }

    AddCustomFieldResult result{
        .name = action.name,
        .type = action.type,
        .required = action.required,
        .minRoleToEdit = action.minRoleToEdit,
    };
    _journal.recordSuccess<CustomFieldModel>(action, result, nowMillis());
    return result;
}

ListCustomFieldsResult CustomFieldModel::execute(const ListCustomFields& action) {
    Lightweight::DataMapper mapper;
    auto rows =
        mapper.Query<db::CustomFieldDefRecord>()
            .Where(::Lightweight::FieldNameOf<&db::CustomFieldDefRecord::entity>, "=", static_cast<int>(action.entity))
            .All();
    ListCustomFieldsResult result;
    result.fields.reserve(rows.size());
    for (const auto& row : rows) {
        result.fields.push_back(CustomFieldDefView{
            .name = std::string{row.name.Value().ToStringView()},
            .type = static_cast<CustomFieldType>(row.type.Value()),
            .required = row.required.Value(),
            .minRoleToEdit = decodeMinRoleToEdit(row),
            .choiceOptions = decodeChoiceOptions(row),
        });
    }
    return result;
}

DeleteCustomFieldResult CustomFieldModel::execute(const DeleteCustomField& action) {
    requirePrincipal();
    if (!action.validate()) {
        throw ValidationError{"DeleteCustomField: name is required"};
    }

    Lightweight::DataMapper mapper;
    auto rows =
        mapper.Query<db::CustomFieldDefRecord>()
            .Where(::Lightweight::FieldNameOf<&db::CustomFieldDefRecord::entity>, "=", static_cast<int>(action.entity))
            .Where(::Lightweight::FieldNameOf<&db::CustomFieldDefRecord::name>, "=", action.name)
            .All();
    if (rows.empty()) {
        throw NotFound{"DeleteCustomField: no such field"};
    }

    // Cascade-delete every account's stored value for this field, in the
    // same transaction as the definition's own removal — no orphaned rows
    // left behind (this file's own doc comment on the written 7b decision).
    // Scoped to Account only, matching CustomFieldEntity's own current
    // scope (custom_field_dto.hpp) — a later entity added there extends
    // this cascade alongside it.
    Lightweight::SqlTransaction transaction{mapper.Connection(), Lightweight::SqlTransactionMode::ROLLBACK};
    auto valueRows = mapper.Query<db::AccountCustomValueRecord>()
                         .Where(::Lightweight::FieldNameOf<&db::AccountCustomValueRecord::fieldName>, "=", action.name)
                         .All();
    for (auto& valueRow : valueRows) {
        mapper.Delete(valueRow);
    }
    mapper.Delete(rows.front());
    transaction.Commit();

    DeleteCustomFieldResult result{.name = action.name};
    _journal.recordSuccess<CustomFieldModel>(action, result, nowMillis());
    return result;
}

}  // namespace crm

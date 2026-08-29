// SPDX-License-Identifier: Apache-2.0
#include "crm/models/custom_field_model.hpp"

#include <Lightweight/DataMapper/DataMapper.hpp>

#include "crm/core/errors.hpp"
#include "crm/core/model_support.hpp"
#include "crm/db/crm_entity.hpp"

namespace crm {

AddCustomFieldResult CustomFieldModel::execute(const AddCustomField& action) {
    requirePrincipal();
    if (!action.validate()) {
        throw ValidationError{"AddCustomField: name is required"};
    }

    Lightweight::DataMapper mapper;
    auto rows =
        mapper.Query<db::CustomFieldDefRecord>()
            .Where(::Lightweight::FieldNameOf<&db::CustomFieldDefRecord::entity>, "=", static_cast<int>(action.entity))
            .Where(::Lightweight::FieldNameOf<&db::CustomFieldDefRecord::name>, "=", action.name)
            .All();
    if (!rows.empty()) {
        // Replace-by-name: a second AddCustomField naming an existing field
        // updates its declared type/required-ness rather than erroring, the
        // same "upsert by natural key" shape SetAccountRole already uses for
        // a repeated (account, principal) pair.
        auto& row = rows.front();
        row.type = static_cast<int>(action.type);
        row.required = action.required;
        mapper.Update(row);
    } else {
        db::CustomFieldDefRecord row;
        row.entity = static_cast<int>(action.entity);
        row.name = Lightweight::SqlAnsiString<64>{action.name};
        row.type = static_cast<int>(action.type);
        row.required = action.required;
        mapper.Create(row);
    }

    AddCustomFieldResult result{
        .name = action.name,
        .type = action.type,
        .required = action.required,
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
        });
    }
    return result;
}

}  // namespace crm

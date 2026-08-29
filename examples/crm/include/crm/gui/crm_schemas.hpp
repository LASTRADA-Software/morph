// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <glaze/glaze.hpp>
#include <morph/forms/forms.hpp>
#include <string>

#include "crm/core/authz.hpp"
#include "crm/dto/account_dto.hpp"
#include "crm/dto/contact_dto.hpp"
#include "crm/dto/custom_field_dto.hpp"
#include "crm/dto/lead_dto.hpp"
#include "crm/dto/opportunity_dto.hpp"
#include "crm/dto/saved_view_dto.hpp"

/// @file
/// The `{actionType: schemaJson<A>()}` document every client renders forms
/// from — mirrors `lims::gui::limsSchemasJson()`'s exact shape
/// (`examples/lims/gui_lib/lims_schemas.hpp`). Lives under `include/crm/gui`
/// rather than `gui_lib/` (unlike lims's copy): it has no Qt dependency of
/// its own — just `morph::forms::schemaJson` — so it needs no
/// `ladder_crm_gui_lib` target to exist yet; a later step's real Qt
/// presenters move it there (or link against it) once `gui_lib/*.cpp` files
/// exist to host them.
///
/// Selection rule (from lims's file, reused here): a field a person types is
/// rendered from the schema; a value the model already supplied is a typed
/// call. `GetAccount`/`GetContact`/`GetOpportunity`/`GetLead` (id-only
/// lookups) and the four `List*`/`List*Options` actions (no user-fillable
/// fields at all) are excluded for the same reason lims excludes its own
/// empty-body transitions.

namespace crm::gui {

/// @brief The served schema document for every crm form action.
/// @return `{"CreateAccount": {...}, "UpdateAccount": {...}, ...}`.
[[nodiscard]] inline std::string crmSchemasJson() {
    return std::string{"{"} +                                                              //
           R"("CreateAccount":)" + ::morph::forms::schemaJson<CreateAccount>() +           //
           R"(,"UpdateAccount":)" + ::morph::forms::schemaJson<UpdateAccount>() +          //
           R"(,"CreateContact":)" + ::morph::forms::schemaJson<CreateContact>() +          //
           R"(,"UpdateContact":)" + ::morph::forms::schemaJson<UpdateContact>() +          //
           R"(,"CreateLead":)" + ::morph::forms::schemaJson<CreateLead>() +                //
           R"(,"UpdateLead":)" + ::morph::forms::schemaJson<UpdateLead>() +                //
           R"(,"CreateOpportunity":)" + ::morph::forms::schemaJson<CreateOpportunity>() +  //
           R"(,"UpdateOpportunity":)" + ::morph::forms::schemaJson<UpdateOpportunity>() +  //
           R"(,"AddCustomField":)" + ::morph::forms::schemaJson<AddCustomField>() +        //
           R"(,"CreateSavedView":)" + ::morph::forms::schemaJson<CreateSavedView>() +      //
           "}";
}

/// @brief @p base with a `properties` node injected for each of @p fields,
///        plus `required` array growth for any declared `required`.
///
/// Mirrors `EXTENSION-BAG-SPIKE.md`'s `ebSchemaJsonWithCustomFields()`
/// exactly (same DOM-rewrite idiom as `InstanceConstraints::decorate()`,
/// extended from *value* overwrites to whole new property nodes) — shared
/// here between `CreateAccount`/`UpdateAccount` rather than duplicated once
/// per action, since both grow by the same field list.
/// @param base The compiled schema JSON to grow.
/// @param fields The custom field definitions to inject.
/// @return @p base unchanged if @p fields is empty, otherwise the grown schema.
[[nodiscard]] inline std::string withCustomFields(std::string base, const std::vector<CustomFieldDefView>& fields) {
    if (fields.empty()) {
        return base;
    }

    // NOLINTBEGIN(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access) — glaze DOM requires operator[]
    glz::generic_u64 dom{};
    if (glz::read_json(dom, base)) {
        return base;  // malformed input passes through unchanged, same rule decorate() follows
    }

    glz::generic_u64::array_t requiredNames{};
    if (dom.contains("required") && dom["required"].holds<glz::generic_u64::array_t>()) {
        requiredNames = dom["required"].get<glz::generic_u64::array_t>();
    }

    std::uint64_t nextOrder = 0;
    if (dom.contains("properties") && dom["properties"].holds<glz::generic_u64::object_t>()) {
        nextOrder = static_cast<std::uint64_t>(dom["properties"].get<glz::generic_u64::object_t>().size());
    }

    for (const auto& field : fields) {
        auto& property = dom["properties"][field.name];
        property["type"] = (field.type == CustomFieldType::Number)    ? std::string{"number"}
                           : (field.type == CustomFieldType::Boolean) ? std::string{"boolean"}
                                                                      : std::string{"string"};
        property["x-order"] = nextOrder++;
        property["x-custom"] = true;  // marks this property as runtime-added
        if (field.required) {
            requiredNames.emplace_back(field.name);
        }
    }
    dom["required"] = requiredNames;
    // NOLINTEND(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)

    return glz::write_json(dom).value_or(base);
}

/// @brief `schemaJson<UpdateAccount>()` with `properties.industry.x-readonly`
///        overwritten to `true` for a caller below `Role::Manager` (README
///        §5: "served schemas must reflect the caller's rights").
///
/// Mirrors `morph::forms::InstanceConstraints::decorate()`'s exact DOM-rewrite
/// idiom (`instance_constraints.hpp`): read the compiled schema into a
/// `glz::generic_u64`, overwrite the *value* of an already-declared framework
/// key (`x-readonly` — `mergeSchemaExtras` already emits this key for
/// compile-time-readonly/computed fields, so this is a value overwrite on an
/// existing key, the same category `decorate()`'s own
/// `x-minimum`/`x-maximum` overwrites fall into, not the "grow a new
/// property" category the extension-bag spike found needs additive DOM
/// writes), write back out. Not memoised, for the same reason
/// `instanceSchemaJson<A>()` isn't: the result varies with the caller's role,
/// so a caller that wants caching does its own.
///
/// This function shapes *presentation only* — it renders a read-only field
/// in a client, nothing more. `AccountModel::execute(const UpdateAccount&)`'s
/// own `requireRole` call on an actually-changed `industry` value is what
/// enforces the restriction; a caller that never asks for this decorated
/// schema, or a client that ignores `x-readonly` and submits an edit anyway,
/// is still caught there. See `docs/spec/forms/forms.md`: "Field metadata is
/// not a security control."
/// @param callerRole The requesting caller's account-scoped role.
/// @return The decorated `UpdateAccount` schema text.
[[nodiscard]] inline std::string updateAccountSchemaJsonFor(Role callerRole) {
    std::string schema = ::morph::forms::schemaJson<UpdateAccount>();
    if (static_cast<std::uint8_t>(callerRole) >= static_cast<std::uint8_t>(Role::Manager)) {
        return schema;  // Manager already has full write access — undecorated
    }

    // NOLINTBEGIN(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access) — glaze DOM requires operator[]
    glz::generic_u64 dom{};
    if (glz::read_json(dom, schema)) {
        return schema;  // malformed input passes through unchanged, same rule decorate() follows
    }
    if (!dom.contains("properties") || !dom["properties"].contains("industry")) {
        return schema;
    }
    dom["properties"]["industry"]["x-readonly"] = true;
    // NOLINTEND(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    return glz::write_json(dom).value_or(schema);
}

}  // namespace crm::gui

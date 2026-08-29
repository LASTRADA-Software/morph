// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <glaze/glaze.hpp>
#include <string>
#include <vector>

#include "crm/core/types.hpp"

/// @file
/// Runtime custom fields (README build order §9, "the endgame") — the
/// framework question this rung exists to ask, answered by
/// `EXTENSION-BAG-SPIKE.md` (yes, reachable, with the `glz::meta::value`
/// catch), built here for real on one entity (`Account`), scoped to the
/// spike's headline mechanism only. Deliberately excluded from this pass —
/// named, not silently decided — per the written scope decision below and
/// `crm/README.md`'s own §9 design-decisions section:
///
/// - **Per-field authz on custom fields.** `UpdateAccount::industry`'s
///   Manager-only write-guard (§5) has no analogue here — any account member
///   who may edit the account at all may set any custom field's value.
/// - **Delete-a-field-while-in-use races.** `AddCustomField` has no removal
///   counterpart; a definition, once added, is permanent for this rung.
/// - **Unit- and Choice-backed custom values.** `CrmCustomValue` covers
///   string, number and bool only — the same narrowing the spike's own
///   `EB_Value` used, for the same reason (a `Quantity`-in-bag needs
///   decimal-places/unit metadata traveling with the value; a `Choice`-in-bag
///   needs referential re-validation; neither fits this pass's scope).
///
/// @par Why `AddCustomField` has no role check
/// Every other crm mutating action's authorization is account-scoped
/// (`requireRole(AccountId, Role)`, `core/authz.hpp`) — but a custom *field
/// definition* is schema-wide, not tied to one account, and crm has no
/// global-admin role concept (confirmed by inspection: no such concept exists
/// anywhere else in this rung). `AddCustomField` therefore only requires an
/// authenticated principal (`requirePrincipal()`), the same floor every
/// mutating crm action already enforces — a genuine gap for a real product
/// (anyone with a login can add a schema-wide field), named here rather than
/// silently assumed away, and out of scope for the same reason per-field
/// custom-value authz is: no framework primitive for global admin exists to
/// build on, and inventing one is a larger decision than this pass's own.

namespace crm {

/// @brief The wire type of one custom field's value — string, number, or
///        bool only (this pass's scope; see this file's own doc comment).
using CrmCustomValue = ::glz::generic_u64;

/// @brief A custom field's declared value type.
enum class CustomFieldType : std::uint8_t {
    Text,     ///< A string value.
    Number,   ///< A numeric value.
    Boolean,  ///< A bool value.
};

/// @brief The entity a custom field definition applies to.
///
/// One enumerator today (`Account`) — this pass builds the mechanism on one
/// entity, per the written scope decision; a later pass extending it to
/// Contact/Opportunity/Lead adds enumerators here, not a second parallel type.
enum class CustomFieldEntity : std::uint8_t {
    Account,
};

/// @brief Adds (or, by name, replaces the declaration of) a runtime custom
///        field on @p entity.
///
/// Journaled and authorized like any other mutation (the spike's own "what a
/// real 7b would still need" list names this explicitly) — unlike the
/// spike's throwaway `EB_CustomFieldRegistry::add()`, which was test setup,
/// not a dispatched action.
struct AddCustomField {
    CustomFieldEntity entity = CustomFieldEntity::Account;
    std::string name;
    CustomFieldType type = CustomFieldType::Text;
    bool required = false;

    [[nodiscard]] bool validate() const noexcept { return !name.empty(); }
};

struct AddCustomFieldResult {
    std::string name;
    CustomFieldType type = CustomFieldType::Text;
    bool required = false;
};

/// @brief One custom field's definition, as served to a caller building or
///        rendering a schema.
struct CustomFieldDefView {
    std::string name;
    CustomFieldType type = CustomFieldType::Text;
    bool required = false;
};

/// @brief Lists every custom field defined on @p entity.
struct ListCustomFields {
    CustomFieldEntity entity = CustomFieldEntity::Account;

    [[nodiscard]] bool validate() const noexcept { return true; }
};

struct ListCustomFieldsResult {
    std::vector<CustomFieldDefView> fields;
};

}  // namespace crm

template <>
struct glz::meta<crm::CustomFieldType> {
    using enum crm::CustomFieldType;
    static constexpr auto value = glz::enumerate(Text, Number, Boolean);
};

template <>
struct glz::meta<crm::CustomFieldEntity> {
    using enum crm::CustomFieldEntity;
    static constexpr auto value = glz::enumerate(Account);
};

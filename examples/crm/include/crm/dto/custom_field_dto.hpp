// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <glaze/glaze.hpp>
#include <string>
#include <vector>

#include "crm/core/authz.hpp"
#include "crm/core/types.hpp"

/// @file
/// Runtime custom fields (README build order §9, "the endgame") — the
/// framework question this rung exists to ask, answered by
/// `EXTENSION-BAG-SPIKE.md` (yes, reachable, with the `glz::meta::value`
/// catch). Step 9 built the spike's core mechanism for real, on one entity
/// (`Account`), and named three items out of scope in writing rather than
/// silently deciding them; this file (7b) builds all three:
///
/// - **Per-field authz on custom fields** — `AddCustomField::minRoleToEdit`
///   (default `Role::Member`, matching every other editable account field's
///   floor) gates *changing* a custom value the same way
///   `UpdateAccount::industry`'s write-guard already gates `industry`:
///   resubmitting an unchanged value never requires the higher role, only an
///   actual change does.
/// - **Delete-a-field-while-in-use races** — `DeleteCustomField` removes a
///   definition and cascade-deletes every account's stored value for it in
///   the same transaction (no orphaned rows). Once gone, an incoming write
///   naming that key is rejected — see `AccountModel::execute`'s own
///   `requireCustomFieldsPresent`-turned-`validateCustomFields` doc comment
///   for why "reject an unrecognised custom key" is the chosen policy over
///   "silently drop it" or "store it as an orphan."
/// - **Unit- and Choice-backed custom values** — `CustomFieldType::Money`
///   (a `Money = Quantity<CrmUnit::usd, 2>`, the same type
///   `Opportunity::expectedCloseValue` already uses — crm has only the one
///   unit family, so no generic unit system is needed) and
///   `CustomFieldType::Choice` (`AddCustomField::choiceOptions`: a fixed,
///   admin-declared option list; a submitted value is rejected unless it
///   names one of them — the referential re-validation the spike's own
///   finding doc calls for).
///
/// @par Why `AddCustomField`/`DeleteCustomField` still have no role check of
///      their own
/// A custom field *definition* is schema-wide, not tied to one account —
/// crm has no global-admin role concept anywhere else in the rung (still
/// true after this file's own additions: `minRoleToEdit` governs *changing a
/// value on an account*, an account-scoped act, not *declaring the field
/// exists*, which remains schema-wide). `AddCustomField`/`DeleteCustomField`
/// therefore still only require an authenticated principal — a genuine gap
/// for a real product, named here rather than assumed away, and still out of
/// scope: building a global-admin primitive is a larger decision than either
/// 7a's step 9 or this file's own 7b scope.

namespace crm {

/// @brief The wire type of one custom field's value.
///
/// String, number, bool, or — for a `CustomFieldType::Money`-typed field —
/// a `Money` decoded from the same `{"num":...,"den":...}` shape
/// `morph::units::Quantity` already reflects elsewhere on the wire. Still a
/// `glz::generic_u64` (glaze's own JSON-DOM variant): a `Money` value is
/// stored and served as its decoded numerator/denominator object, not a
/// bespoke wire shape, so no second `Value` type is needed to add it.
using CrmCustomValue = ::glz::generic_u64;

/// @brief A custom field's declared value type.
///
/// The `Money` enumerator has the same spelling as `crm::Money` (the
/// `Quantity<CrmUnit::usd, 2>` alias in `core/types.hpp`) on purpose: it names
/// exactly that type. GCC's -Wshadow reports an enumerator that matches a
/// namespace-scope name even for a *scoped* enum, where the collision it warns
/// about cannot occur -- `CustomFieldType::Money` is never found by unqualified
/// lookup, so it can never be mistaken for the alias. Renaming either side to
/// satisfy the diagnostic would cost more than it buys: the enumerator's
/// spelling is the wire representation (`glz::enumerate` below serialises it by
/// name), and the alias is used across the rung's DTOs, entity and tests.
/// Suppressed locally, in the same shape `morph::OpaqueIdGenerator` suppresses
/// -Wuseless-cast. Clang does not warn here.
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wshadow"
#endif
enum class CustomFieldType : std::uint8_t {
    Text,     ///< A string value.
    Number,   ///< A numeric value.
    Boolean,  ///< A bool value.
    /// @brief A `Money` value (`Quantity<CrmUnit::usd, 2>`) — the same type
    ///        `Opportunity::expectedCloseValue` uses.
    Money,
    /// @brief A string value constrained to `AddCustomField::choiceOptions`.
    Choice,
};
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic pop
#endif

/// @brief The entity a custom field definition applies to.
///
/// One enumerator today (`Account`) — this pass builds the mechanism on one
/// entity; a later pass extending it to Contact/Opportunity/Lead adds
/// enumerators here, not a second parallel type.
enum class CustomFieldEntity : std::uint8_t {
    Account,
};

/// @brief Adds (or, by name, replaces the declaration of) a runtime custom
///        field on @p entity.
///
/// Journaled and authorized like any other mutation — unlike the spike's
/// throwaway `EB_CustomFieldRegistry::add()`, which was test setup, not a
/// dispatched action.
struct AddCustomField {
    CustomFieldEntity entity = CustomFieldEntity::Account;
    std::string name;
    CustomFieldType type = CustomFieldType::Text;
    bool required = false;

    /// @brief The role required to *change* this field's value on an
    ///        account (default `Role::Member`, the same floor every other
    ///        editable account field already has). Resubmitting the current
    ///        value unchanged never requires this — same "round-trip is not
    ///        a change" rule `UpdateAccount::industry`'s own write-guard
    ///        uses.
    Role minRoleToEdit = Role::Member;

    /// @brief The fixed option list for a `CustomFieldType::Choice` field.
    ///        Ignored for every other type. Empty is legal for a
    ///        non-`Choice` type; empty on a `Choice`-typed field means "no
    ///        value is ever valid," which `validate()` rejects.
    std::vector<std::string> choiceOptions;

    [[nodiscard]] bool validate() const noexcept {
        return !name.empty() && (type != CustomFieldType::Choice || !choiceOptions.empty());
    }
};

struct AddCustomFieldResult {
    std::string name;
    CustomFieldType type = CustomFieldType::Text;
    bool required = false;
    Role minRoleToEdit = Role::Member;
};

/// @brief One custom field's definition, as served to a caller building or
///        rendering a schema.
struct CustomFieldDefView {
    std::string name;
    CustomFieldType type = CustomFieldType::Text;
    bool required = false;
    Role minRoleToEdit = Role::Member;
    std::vector<std::string> choiceOptions;
};

/// @brief Lists every custom field defined on @p entity.
struct ListCustomFields {
    CustomFieldEntity entity = CustomFieldEntity::Account;

    [[nodiscard]] bool validate() const noexcept { return true; }
};

struct ListCustomFieldsResult {
    std::vector<CustomFieldDefView> fields;
};

/// @brief Removes a custom field's definition, cascade-deleting every
///        account's stored value for it in the same transaction.
///
/// No orphaned rows, and no orphaned-but-restorable state either: a later
/// `AddCustomField` with the same name starts genuinely fresh, the same
/// "replace-by-name" upsert `AddCustomField`'s own doc comment already
/// documents for a re-declaration, now also true after a delete-then-recreate.
struct DeleteCustomField {
    CustomFieldEntity entity = CustomFieldEntity::Account;
    std::string name;

    [[nodiscard]] bool validate() const noexcept { return !name.empty(); }
};

struct DeleteCustomFieldResult {
    std::string name;
};

}  // namespace crm

template <>
struct glz::meta<crm::CustomFieldType> {
    using enum crm::CustomFieldType;
    static constexpr auto value = glz::enumerate(Text, Number, Boolean, Money, Choice);
};

template <>
struct glz::meta<crm::CustomFieldEntity> {
    using enum crm::CustomFieldEntity;
    static constexpr auto value = glz::enumerate(Account);
};

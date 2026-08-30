// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <glaze/glaze.hpp>
#include <map>
#include <string>
#include <vector>

#include "crm/core/authz.hpp"
#include "crm/core/types.hpp"
#include "crm/dto/custom_field_dto.hpp"

/// @file
/// Account CRUD action/result DTOs, and the account view served back to
/// clients — separate from `db::AccountRecord` (the raw ORM row shape,
/// db/crm_entity.hpp) and from `AccountModel` (pure orchestration over the
/// two), matching `examples/lims`'s three-way split.
///
/// README build order §5 ("per-field permissions... served schemas must
/// reflect the caller's rights") lands here: `UpdateAccount::industry` is
/// `Manager`-only to change (a `Member`/`Viewer` may still read it, and may
/// resubmit the same value — round-tripping an unchanged field is not the
/// same as changing it). See `AccountModel::execute(const UpdateAccount&)`
/// for the write-guard this schema-level marker does not, by itself,
/// enforce — `docs/spec/forms/forms.md`: "Field metadata is not a security
/// control."

namespace crm {

/// @brief The account's read-model shape: what a client fetches/lists.
///
/// `extra` (README build order §9) is a runtime extension bag: values for
/// whatever `AddCustomField`-declared fields happen to be set on this
/// account, keyed by field name. Not a normal reflected member — reached
/// only through `glz::meta<AccountView>::unknown_write` below, so it never
/// appears as a `properties.extra` node in the served schema; on the wire its
/// contents are merged flat into the object
/// (`{"id":1,"name":"Acme",...,"leadSource":"referral"}`), the same flat
/// shape `EXTENSION-BAG-SPIKE.md`'s probe already proved for the *write*
/// side (`EB_UpdateContact`) and now reused for the *read* side too, so a
/// client that fetched a custom field's value can render it without knowing
/// it is not one of the compiled members.
struct AccountView {
    AccountId id;
    std::string name;
    std::string industry;
    std::string website;
    std::int32_t version = 0;
    std::map<std::string, CrmCustomValue> extra{};
};

/// @brief Creates a new account.
///
/// `extra` carries this create's custom-field values, if any were submitted —
/// same extension-bag shape as `AccountView::extra`, but here reached through
/// `unknown_read` (an *incoming* wire key with no matching compiled member is
/// routed here instead of silently dropped) as well as `unknown_write` (so a
/// round-tripped `toJson` re-emits it flat). See this file's own opening doc
/// comment and `EXTENSION-BAG-SPIKE.md`'s "The catch" section for why this
/// requires the explicit `glz::meta<T>::value` hand-listing below — a real,
/// if modest, per-action cost of opting into the bag.
struct CreateAccount {
    std::string name;
    std::string industry;
    std::string website;
    std::map<std::string, CrmCustomValue> extra{};

    /// @brief Whether this action is well-formed.
    ///
    /// Custom-field required-ness is checked by hand against the live
    /// registry (`CustomFieldModel::execute(ListCustomFields)`'s own
    /// definitions), not by `schemaJson<A>()`'s compiled `required` array —
    /// the same limit `EXTENSION-BAG-SPIKE.md`'s `EB_UpdateContact::validate()`
    /// names: a bag-carrying action is responsible for its own required
    /// check, since the compiled type has no way to express it. That check is
    /// `AccountModel::execute(const CreateAccount&)`'s job (it alone has the
    /// registry query available), not this method's — `validate()` here
    /// checks only what a bag-free `CreateAccount` already checked.
    [[nodiscard]] bool validate() const noexcept { return !name.empty(); }
};

struct CreateAccountResult {
    AccountId accountId;
};

/// @brief Updates an existing account's editable fields.
///
/// Full replace, not a partial patch — matching `lims::ReviseAnalysis`'s
/// shape: the caller re-sends every editable field, and `expectedVersion`
/// guards a lost-update race (README step 1's "schema-served forms for
/// every edit view" implies a form that round-trips the record it fetched).
/// `extra` is the same extension-bag shape as `CreateAccount::extra`.
struct UpdateAccount {
    AccountId accountId;
    std::string name;
    std::string industry;
    std::string website;
    std::int32_t expectedVersion = 0;
    std::map<std::string, CrmCustomValue> extra{};

    [[nodiscard]] bool validate() const noexcept { return accountId.hasValue() && !name.empty(); }
};

struct UpdateAccountResult {
    AccountView account;
};

/// @brief Fetches one account by id.
struct GetAccount {
    AccountId accountId;

    [[nodiscard]] bool validate() const noexcept { return accountId.hasValue(); }
};

/// @brief Lists every account. Unpaginated — no ladder rung has a pagination
///        precedent yet (lims's `ListAnalyses`/`ListResults` are all
///        unpaginated `.All()` too); crm step 1 stays consistent with that
///        rather than inventing a first cursor convention on its own.
struct ListAccounts {
    [[nodiscard]] bool validate() const noexcept { return true; }
};

struct ListAccountsResult {
    std::vector<AccountView> accounts;
};

/// @brief `{id, name}` rows for a `forms::Choice`-backed account lookup field
///        (README step 2: "lookup fields via forms::Choice backed by list
///        actions — 'account' combo on a contact").
struct AccountOption {
    std::string id;
    std::string name;
};

struct ListAccountOptions {
    [[nodiscard]] bool validate() const noexcept { return true; }
};

struct ListAccountOptionsResult {
    std::vector<AccountOption> accounts;
};

/// @brief Sets (or replaces, by principal) one principal's role on an
///        account. `Manager`-only (`AccountModel::execute` requires
///        `Role::Manager` before allowing this — the same gate that guards
///        `industry`, since role administration is itself a Manager-only
///        capability, matching `kanban::ProjectAdminModel`'s equivalent
///        restriction on `SetMemberRole`).
struct SetAccountRole {
    AccountId accountId;
    std::string principal;
    Role role = Role::Viewer;

    [[nodiscard]] bool validate() const noexcept { return accountId.hasValue() && !principal.empty(); }
};

struct SetAccountRoleResult {
    AccountId accountId;
    std::string principal;
    Role role = Role::Viewer;
};

/// @brief One row of an account's role table, as served back to a caller.
struct AccountRoleView {
    std::string principal;
    Role role = Role::Viewer;
};

/// @brief Lists every role declared on an account. Any role (or no role —
///        the fail-open default, `authz.hpp`) may list; this is read
///        access, not administration.
struct GetAccountRoles {
    AccountId accountId;

    [[nodiscard]] bool validate() const noexcept { return accountId.hasValue(); }
};

struct GetAccountRolesResult {
    std::vector<AccountRoleView> roles;
};

/// @brief One journaled change to an account, redacted for the requesting
///        caller's role (README build order §6, "field-level audit... EspoCRM's
///        Stream / Frappe's Version").
///
/// `industry` is empty (never populated) unless the caller's role is at
/// least `Manager` — the same threshold `UpdateAccount`'s write-guard uses,
/// so a restricted principal never learns a Manager-only field's value
/// through history any more than through a live read. This is the
/// "Expected strain points" redaction requirement made concrete: "journal
/// payloads are stored whole, so field-level history naively shows
/// restricted users values they cannot read... test that a restricted
/// principal leaks nothing through history or undo replay."
struct AccountHistoryEntry {
    std::string actionType;
    std::string principal;
    std::int64_t timestampMs = 0;
    std::string name;
    /// @brief Empty unless the caller's role allows reading it — see this
    ///        struct's own doc comment.
    std::string industry;
    std::string website;
};

struct GetAccountHistory {
    AccountId accountId;

    [[nodiscard]] bool validate() const noexcept { return accountId.hasValue(); }
};

struct GetAccountHistoryResult {
    std::vector<AccountHistoryEntry> entries;
};

/// @brief Reverts an account's editable fields to their value immediately
///        before its most recent recorded change.
///
/// An app-level compensating action (README §6's own design decision, in
/// writing — see `AccountModel::execute(const UndoLastAccountChange&)`'s doc
/// comment for why this is not `journal::undoLast()` itself): it never
/// touches a `Manager`-only field's value on behalf of a caller who could
/// not have changed it themselves (the same write-guard `UpdateAccount` uses
/// applies here too), and it writes the restored state back to the live row
/// as a new, journaled `UndoLastAccountChange` entry — an explicit,
/// auditable reversal, not a silent rewrite of history.
struct UndoLastAccountChange {
    AccountId accountId;

    [[nodiscard]] bool validate() const noexcept { return accountId.hasValue(); }
};

struct UndoLastAccountChangeResult {
    AccountView account;
};

}  // namespace crm

/// @brief `unknown_read`/`unknown_write` route any wire key that is not one
///        of the three compiled fields into `extra` instead of dropping it —
///        exactly `EXTENSION-BAG-SPIKE.md`'s `EB_UpdateContact` mechanism.
///        `value` must hand-list the compiled members: glaze's own hooks are
///        only wired for the `glz::meta::value`-declared object path, not
///        morph's usual pure-reflection path (the spike's "The catch"
///        section) — the modest per-action cost of opting into a bag.
template <>
struct glz::meta<crm::CreateAccount> {
    using T = crm::CreateAccount;
    static constexpr auto value = glz::object("name", &T::name, "industry", &T::industry, "website", &T::website);
    static constexpr auto unknown_read = &T::extra;
    static constexpr auto unknown_write = &T::extra;
};

template <>
struct glz::meta<crm::UpdateAccount> {
    using T = crm::UpdateAccount;
    static constexpr auto value = glz::object("accountId", &T::accountId, "name", &T::name, "industry", &T::industry,
                                              "website", &T::website, "expectedVersion", &T::expectedVersion);
    static constexpr auto unknown_read = &T::extra;
    static constexpr auto unknown_write = &T::extra;
};

/// @brief `AccountView` only ever needs `unknown_write`: it is a *served*
///        shape, never decoded from a caller's wire body, so nothing ever
///        routes an incoming key into `extra` — the model fills it directly
///        (`AccountModel`'s own `toView()`). Still needs the full `value`
///        hand-listing for the same reason `CreateAccount`/`UpdateAccount`
///        do: glaze's unknown-field hooks require the `glz::meta::value`
///        path, not pure reflection, whichever direction is actually used.
template <>
struct glz::meta<crm::AccountView> {
    using T = crm::AccountView;
    static constexpr auto value = glz::object("id", &T::id, "name", &T::name, "industry", &T::industry, "website",
                                              &T::website, "version", &T::version);
    static constexpr auto unknown_write = &T::extra;
};

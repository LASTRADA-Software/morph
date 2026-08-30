// SPDX-License-Identifier: Apache-2.0
#include "crm/models/account_model.hpp"

#include <Lightweight/DataMapper/DataMapper.hpp>
#include <algorithm>
#include <glaze/glaze.hpp>
#include <map>
#include <morph/session/session.hpp>
#include <optional>

#include "crm/core/authz.hpp"
#include "crm/core/errors.hpp"
#include "crm/core/model_support.hpp"
#include "crm/db/crm_entity.hpp"

namespace crm {

namespace {

/// @brief Every custom value stored against @p accountId, keyed by field name.
///
/// A separate query rather than a `BelongsTo` join fetched alongside
/// `AccountRecord`: `AccountCustomValueRecord` is a one-to-many child table
/// (README §9), and this rung has no precedent for joining a child table
/// into a parent's own `toView()` — `QuoteModel`'s line items are the closest
/// analogue and are fetched the same separate-query way.
std::map<std::string, CrmCustomValue> loadCustomValues(Lightweight::DataMapper& mapper, AccountId accountId) {
    std::map<std::string, CrmCustomValue> values;
    auto rows = mapper.Query<db::AccountCustomValueRecord>()
                    .Where(::Lightweight::FieldNameOf<&db::AccountCustomValueRecord::account>, "=", *accountId)
                    .All();
    for (const auto& row : rows) {
        CrmCustomValue value{};
        if (!glz::read_json(value, std::string{row.valueJson.Value().ToStringView()})) {
            values.emplace(std::string{row.fieldName.Value().ToStringView()}, std::move(value));
        }
        // A row whose stored JSON no longer decodes is skipped rather than
        // thrown on — the same lenient-on-read posture the extension-bag
        // spike's own wire decode takes (unrecognised/malformed content does
        // not fail the whole read), and there is no single field's write
        // this read could blame the failure on to report usefully.
    }
    return values;
}

/// @brief Replaces every custom value stored against @p accountId with
///        @p extra — delete-then-insert, not a per-key diff.
///
/// Simpler than diffing, and correct for this rung's scope: a client always
/// submits the *full* current bag (the same full-replace convention
/// `UpdateAccount`'s compiled fields already use — "full replace, not a
/// partial patch", this file's own `UpdateAccount` doc comment), so there is
/// no partial-update case where a diff would avoid dropping a value the
/// caller meant to keep.
void saveCustomValues(Lightweight::DataMapper& mapper, const db::AccountRecord& accountRow,
                      const std::map<std::string, CrmCustomValue>& extra) {
    auto existing =
        mapper.Query<db::AccountCustomValueRecord>()
            .Where(::Lightweight::FieldNameOf<&db::AccountCustomValueRecord::account>, "=", accountRow.id.Value())
            .All();
    for (auto& row : existing) {
        mapper.Delete(row);
    }
    for (const auto& [name, value] : extra) {
        db::AccountCustomValueRecord row;
        row.account = accountRow;
        row.fieldName = Lightweight::SqlAnsiString<64>{name};
        row.valueJson = Lightweight::SqlDynamicAnsiString<1024>{glz::write_json(value).value_or("null")};
        mapper.Create(row);
    }
}

/// @brief Throws `ValidationError` if @p extra is missing a value for any
///        field `AddCustomField` declared `required` on `Account`.
///
/// The check `schemaJson<A>()`'s compiled `required` array cannot express
/// (`CreateAccount::validate()`'s own doc comment, and
/// `EXTENSION-BAG-SPIKE.md`'s identical finding for `EB_UpdateContact`) — a
/// bag-carrying action's model is responsible for re-checking this against
/// the live registry, since the compiled type has no way to.
/// @brief One custom field's live definition, decoded once for the whole
///        validate pass.
struct LiveCustomFieldDef {
    CustomFieldType type = CustomFieldType::Text;
    bool required = false;
    Role minRoleToEdit = Role::Member;
    std::vector<std::string> choiceOptions;
};

/// @brief Every custom field currently defined on `Account`, keyed by name.
std::map<std::string, LiveCustomFieldDef> loadCustomFieldDefs(Lightweight::DataMapper& mapper) {
    std::map<std::string, LiveCustomFieldDef> defs;
    auto rows = mapper.Query<db::CustomFieldDefRecord>()
                    .Where(::Lightweight::FieldNameOf<&db::CustomFieldDefRecord::entity>, "=",
                           static_cast<int>(CustomFieldEntity::Account))
                    .All();
    for (const auto& row : rows) {
        LiveCustomFieldDef def{
            .type = static_cast<CustomFieldType>(row.type.Value()),
            .required = row.required.Value(),
            .minRoleToEdit =
                row.minRoleToEdit.Value().has_value() ? static_cast<Role>(*row.minRoleToEdit.Value()) : Role::Member,
        };
        if (row.choiceOptionsJson.Value().has_value()) {
            static_cast<void>(
                glz::read_json(def.choiceOptions, std::string{row.choiceOptionsJson.Value()->ToStringView()}));
        }
        defs.emplace(std::string{row.name.Value().ToStringView()}, std::move(def));
    }
    return defs;
}

/// @brief Validates @p extra against @p defs (7b): every key must name a
///        live definition (an unrecognised key is rejected, not silently
///        stored -- the written 7b decision for what happens once a field is
///        deleted while a client still submits it, custom_field_dto.hpp's
///        own doc comment), every required definition must have a value,
///        and a Choice-typed value must name one of its declared options.
///
/// Money/Number/Boolean/Text type-matching is intentionally not re-checked
/// here beyond what CrmCustomValue's own JSON shape already constrains --
/// the framework's own lenient decode is the only type enforcement this
/// pass adds; a submitted value of the wrong JSON kind for its declared
/// type is out of this pass's scope, same as the original step-9
/// requireCustomFieldsPresent only ever checked presence.
/// @throws ValidationError if any of the above checks fails.
void validateCustomFields(const std::map<std::string, LiveCustomFieldDef>& defs,
                          const std::map<std::string, CrmCustomValue>& extra) {
    for (const auto& [name, value] : extra) {
        const auto found = defs.find(name);
        if (found == defs.end()) {
            throw ValidationError{"custom field '" + name + "' is not defined"};
        }
        if (found->second.type == CustomFieldType::Choice) {
            if (!value.holds<std::string>() ||
                std::ranges::find(found->second.choiceOptions, value.get<std::string>()) ==
                    found->second.choiceOptions.end()) {
                throw ValidationError{"custom field '" + name + "' must be one of its declared options"};
            }
        }
    }
    for (const auto& [name, def] : defs) {
        if (def.required && !extra.contains(name)) {
            throw ValidationError{"required custom field '" + name + "' is missing"};
        }
    }
}

/// @brief Requires minRoleToEdit for every custom field whose value in
///        @p extra actually differs from @p before -- the same "round-trip
///        is not a change" rule UpdateAccount::industry's own write-guard
///        already uses, extended to custom fields (7b).
///
/// A field with no prior value counts as changed the instant any value is
/// submitted for it -- there is no "unchanged" state to compare against.
void requireCustomFieldRoleForChanges(AccountId accountId, const std::map<std::string, LiveCustomFieldDef>& defs,
                                      const std::map<std::string, CrmCustomValue>& before,
                                      const std::map<std::string, CrmCustomValue>& extra) {
    for (const auto& [name, value] : extra) {
        const auto beforeFound = before.find(name);
        // glz::generic_u64 has no operator== of its own — compared by
        // re-serialising both sides to JSON text instead, the same
        // deliberately simple equality test entryNamesAccount()'s own DOM
        // comparisons already use elsewhere in this file (compare the JSON,
        // not a bespoke structural walk).
        const bool changed = beforeFound == before.end() ||
                             glz::write_json(beforeFound->second).value_or("") != glz::write_json(value).value_or("");
        if (!changed) {
            continue;
        }
        const auto defFound = defs.find(name);
        if (defFound != defs.end() && defFound->second.minRoleToEdit != Role::Viewer) {
            requireRole(accountId, defFound->second.minRoleToEdit);
        }
    }
}

AccountView toView(Lightweight::DataMapper& mapper, const db::AccountRecord& row) {
    return AccountView{
        .id = AccountId{static_cast<std::int64_t>(row.id.Value())},
        .name = std::string{row.name.Value().ToStringView()},
        .industry = std::string{row.industry.Value().ToStringView()},
        .website = std::string{row.website.Value().ToStringView()},
        .version = row.version.Value(),
        .extra = loadCustomValues(mapper, AccountId{static_cast<std::int64_t>(row.id.Value())}),
    };
}

/// @brief Whether a decoded JSON DOM's `accountId`/`account`-shaped field
///        equals @p accountId, checking each candidate key in turn.
bool domNamesAccount(const glz::generic_u64& dom, AccountId accountId) {
    // AccountId's wire form is its nullable underlying integer directly
    // (CRM_DEFINE_STRONG_ID_WIRE, core/types.hpp) — u64 mode decodes it as a
    // plain number, not an object.
    for (const char* key : {"accountId", "account"}) {
        if (dom.contains(key) && dom.at(key).holds<std::uint64_t>() &&
            dom.at(key).get<std::uint64_t>() == static_cast<std::uint64_t>(*accountId)) {
            return true;
        }
    }
    // CreateAccountResult{accountId} (the result of the one action whose
    // *payload* never carries an accountId at all — the id does not exist
    // yet at submission time) is checked via the same key list, so the
    // caller decodes `result` too, not just `payload`.
    return false;
}

/// @brief Whether @p entry's recorded `payload` or `result` names @p accountId.
///
/// `AccountModel` is one shared instance across every account
/// (`account_model.hpp`'s own rationale), so the attached log holds every
/// account's history together; this decodes each entry's stored payload/
/// result (`ActionTraits<A>::toJson`/`resultToJson`, plain JSON objects) as
/// generic DOMs rather than relying on `LogEntry::entityKey` (which — for an
/// unkeyed model — names no single account at all). `CreateAccount`'s own
/// payload has no `accountId` (the id doesn't exist until the row commits),
/// which is why `result` is checked too.
bool entryNamesAccount(const ::morph::journal::LogEntry& entry, AccountId accountId) {
    glz::generic_u64 payloadDom{};
    if (!glz::read_json(payloadDom, entry.payload) && domNamesAccount(payloadDom, accountId)) {
        return true;
    }
    if (entry.result.empty()) {
        return false;
    }
    glz::generic_u64 resultDom{};
    if (glz::read_json(resultDom, entry.result)) {
        return false;
    }
    if (domNamesAccount(resultDom, accountId)) {
        return true;
    }
    // UpdateAccountResult/CreateAccountResult nest the id one level deeper
    // (`{"account": {"id": N, ...}}` for UpdateAccountResult specifically —
    // AccountView's own `id` field).
    return resultDom.contains("account") && resultDom.at("account").contains("id") &&
           resultDom.at("account").at("id").holds<std::uint64_t>() &&
           resultDom.at("account").at("id").get<std::uint64_t>() == static_cast<std::uint64_t>(*accountId);
}

/// @brief `name`/`industry`/`website` extracted from whichever of @p entry's
///        `result` or `payload` actually carries them.
///
/// `UpdateAccountResult`'s view nests under `"account"`. `CreateAccount`'s
/// own *payload* (not its result, which is only `{accountId}`) carries the
/// fields it established directly; `UpdateAccount`'s and
/// `UndoLastAccountChange`'s payloads carry them directly too. Checking
/// result, then payload (only if the result had nothing), is what lets every
/// field-changing action type yield a value through one shared path.
/// `SetAccountRole`/`GetAccountRoles` entries carry none of these three
/// fields anywhere and correctly return `std::nullopt` — they are not
/// "changes to the account's own fields" for either history rendering or
/// undo purposes.
struct AccountFieldSnapshot {
    std::string name;
    std::string industry;
    std::string website;
};

std::optional<AccountFieldSnapshot> extractAccountFields(const ::morph::journal::LogEntry& entry) {
    const auto readFrom = [](const glz::generic_u64& dom) -> std::optional<AccountFieldSnapshot> {
        if (!dom.contains("name") || !dom.at("name").holds<std::string>()) {
            return std::nullopt;
        }
        AccountFieldSnapshot snapshot{.name = dom.at("name").get<std::string>()};
        if (dom.contains("industry") && dom.at("industry").holds<std::string>()) {
            snapshot.industry = dom.at("industry").get<std::string>();
        }
        if (dom.contains("website") && dom.at("website").holds<std::string>()) {
            snapshot.website = dom.at("website").get<std::string>();
        }
        return snapshot;
    };

    glz::generic_u64 resultDom{};
    if (!entry.result.empty() && !glz::read_json(resultDom, entry.result)) {
        const glz::generic_u64& view = resultDom.contains("account") ? resultDom.at("account") : resultDom;
        if (auto snapshot = readFrom(view)) {
            return snapshot;
        }
    }
    glz::generic_u64 payloadDom{};
    if (!glz::read_json(payloadDom, entry.payload)) {
        if (auto snapshot = readFrom(payloadDom)) {
            return snapshot;
        }
    }
    return std::nullopt;
}

}  // namespace

CreateAccountResult AccountModel::execute(const CreateAccount& action) {
    requirePrincipal();
    if (!action.validate()) {
        throw ValidationError{"CreateAccount: name is required"};
    }

    Lightweight::DataMapper mapper;
    // No per-field authz check here (7b): minRoleToEdit gates *changing* a
    // custom value on an *existing* account, and neither an account id nor
    // an account-role row exists yet at CreateAccount time — the creator is
    // implicitly the one setting the account's own initial state. Only
    // UpdateAccount's own call site below re-checks minRoleToEdit, where a
    // prior value and a real account/role both exist to compare against.
    validateCustomFields(loadCustomFieldDefs(mapper), action.extra);

    db::AccountRecord row;
    row.name = Lightweight::SqlAnsiString<128>{action.name};
    row.industry = Lightweight::SqlAnsiString<64>{action.industry};
    row.website = Lightweight::SqlAnsiString<255>{action.website};
    row.createdAt = nowMillis();
    row.version = 1;
    mapper.Create(row);
    saveCustomValues(mapper, row, action.extra);

    CreateAccountResult result{.accountId = AccountId{static_cast<std::int64_t>(row.id.Value())}};
    _journal.recordSuccess<AccountModel>(action, result, nowMillis());
    return result;
}

UpdateAccountResult AccountModel::execute(const UpdateAccount& action) {
    requirePrincipal();
    if (!action.validate()) {
        throw ValidationError{"UpdateAccount: accountId and name are required"};
    }
    requireRole(action.accountId, Role::Member);  // per-entity gate (README §5), before any row is touched

    Lightweight::DataMapper mapper;
    auto rows = mapper.Query<db::AccountRecord>()
                    .Where(::Lightweight::FieldNameOf<&db::AccountRecord::id>, "=", *action.accountId)
                    .All();
    if (rows.empty()) {
        throw NotFound{"UpdateAccount: no such account"};
    }
    auto& row = rows.front();
    if (row.version.Value() != action.expectedVersion) {
        throw Conflict{"UpdateAccount: version mismatch — record was edited concurrently"};
    }

    // Per-field write-guard (README §5's "[framework gap]": served schemas
    // can *say* a field is read-only, but nothing in dispatch enforces it —
    // docs/spec/forms/forms.md: "Field metadata is not a security control").
    // A caller below Manager may resubmit the account's *current* industry
    // unchanged (a normal round-trip through a form that fetched, then
    // re-sent, the same value) but may not change it.
    const std::string currentIndustry{row.industry.Value().ToStringView()};
    if (action.industry != currentIndustry) {
        requireRole(action.accountId, Role::Manager);
    }

    const auto customFieldDefs = loadCustomFieldDefs(mapper);
    validateCustomFields(customFieldDefs, action.extra);
    // Per-field write-guard, extended to custom fields (7b) — same
    // "resubmitting an unchanged value is not a change" rule industry's own
    // guard above uses, checked against each field's own declared
    // minRoleToEdit rather than one fixed threshold.
    requireCustomFieldRoleForChanges(action.accountId, customFieldDefs, loadCustomValues(mapper, action.accountId),
                                     action.extra);

    row.name = Lightweight::SqlAnsiString<128>{action.name};
    row.industry = Lightweight::SqlAnsiString<64>{action.industry};
    row.website = Lightweight::SqlAnsiString<255>{action.website};
    row.version = row.version.Value() + 1;
    mapper.Update(row);
    saveCustomValues(mapper, row, action.extra);

    UpdateAccountResult result{.account = toView(mapper, row)};
    _journal.recordSuccess<AccountModel>(action, result, nowMillis());
    return result;
}

SetAccountRoleResult AccountModel::execute(const SetAccountRole& action) {
    requirePrincipal();
    if (!action.validate()) {
        throw ValidationError{"SetAccountRole: accountId and principal are required"};
    }
    requireRole(action.accountId, Role::Manager);  // role administration is itself Manager-only

    Lightweight::DataMapper mapper;
    auto accountRows = mapper.Query<db::AccountRecord>()
                           .Where(::Lightweight::FieldNameOf<&db::AccountRecord::id>, "=", *action.accountId)
                           .All();
    if (accountRows.empty()) {
        throw NotFound{"SetAccountRole: no such account"};
    }

    auto existingRows =
        mapper.Query<db::AccountRoleRecord>()
            .Where(::Lightweight::FieldNameOf<&db::AccountRoleRecord::account>, "=", *action.accountId)
            .Where(::Lightweight::FieldNameOf<&db::AccountRoleRecord::principal>, "=", action.principal)
            .All();
    if (!existingRows.empty()) {
        auto& roleRow = existingRows.front();
        roleRow.role = Lightweight::SqlAnsiString<16>{std::string{roleToString(action.role)}};
        mapper.Update(roleRow);
    } else {
        db::AccountRoleRecord roleRow;
        roleRow.account = accountRows.front();
        roleRow.principal = Lightweight::SqlAnsiString<64>{action.principal};
        roleRow.role = Lightweight::SqlAnsiString<16>{std::string{roleToString(action.role)}};
        mapper.Create(roleRow);
    }

    SetAccountRoleResult result{
        .accountId = action.accountId,
        .principal = action.principal,
        .role = action.role,
    };
    _journal.recordSuccess<AccountModel>(action, result, nowMillis());
    return result;
}

GetAccountRolesResult AccountModel::execute(const GetAccountRoles& action) {
    if (!action.validate()) {
        throw ValidationError{"GetAccountRoles: accountId is required"};
    }
    Lightweight::DataMapper mapper;
    auto rows = mapper.Query<db::AccountRoleRecord>()
                    .Where(::Lightweight::FieldNameOf<&db::AccountRoleRecord::account>, "=", *action.accountId)
                    .All();
    GetAccountRolesResult result;
    result.roles.reserve(rows.size());
    for (const auto& row : rows) {
        result.roles.push_back(AccountRoleView{
            .principal = std::string{row.principal.Value().ToStringView()},
            .role = roleFromString(row.role.Value().ToStringView()),
        });
    }
    return result;
}

GetAccountHistoryResult AccountModel::execute(const GetAccountHistory& action) {
    if (!action.validate()) {
        throw ValidationError{"GetAccountHistory: accountId is required"};
    }
    const Role callerRole = callerRoleOn(action.accountId);
    const bool canReadIndustry = static_cast<std::uint8_t>(callerRole) >= static_cast<std::uint8_t>(Role::Manager);

    GetAccountHistoryResult result;
    for (const auto& entry : _journal.entries()) {
        if (!entryNamesAccount(entry, action.accountId)) {
            continue;
        }
        // Every matching entry is listed, including non-field-changing ones
        // (e.g. SetAccountRole) — an account's history is every recorded
        // action against it, not only the ones with field values. A
        // SetAccountRole entry simply carries empty name/industry/website.
        const auto snapshot = extractAccountFields(entry).value_or(AccountFieldSnapshot{});
        result.entries.push_back(AccountHistoryEntry{
            .actionType = entry.actionType,
            .principal = entry.principal,
            .timestampMs = entry.timestampMs,
            .name = snapshot.name,
            // Redaction (README §6's "Expected strain points" requirement):
            // a caller below Manager never sees industry's historical value,
            // matching UpdateAccount's own live write-guard threshold — the
            // journal is stored whole, so this is where the "app logic"
            // that strain point calls for actually lives.
            .industry = canReadIndustry ? snapshot.industry : std::string{},
            .website = snapshot.website,
        });
    }
    return result;
}

UndoLastAccountChangeResult AccountModel::execute(const UndoLastAccountChange& action) {
    requirePrincipal();
    if (!action.validate()) {
        throw ValidationError{"UndoLastAccountChange: accountId is required"};
    }
    requireRole(action.accountId, Role::Member);

    // Every *field-changing* entry naming this account, oldest first
    // (SelfJournal::entries() already returns append order) —
    // SetAccountRole/GetAccountRoles entries name the account too but carry
    // no name/industry/website, so they are excluded here: "the change
    // being undone" means the account's own field values, not an unrelated
    // administrative action that happened to touch the same account. The
    // *second-to-last* one's fields are the state to restore to (the last
    // one is the change actually being undone).
    std::vector<AccountFieldSnapshot> fieldChanges;
    for (const auto& entry : _journal.entries()) {
        if (!entryNamesAccount(entry, action.accountId)) {
            continue;
        }
        if (auto snapshot = extractAccountFields(entry)) {
            fieldChanges.push_back(std::move(*snapshot));
        }
    }
    if (fieldChanges.size() < 2) {
        // Either nothing at all (impossible if the account exists — its own
        // CreateAccount always carries fields) or exactly one entry: the
        // account's very first (CreateAccount) state, with no earlier
        // state to undo back to.
        throw NotFound{"UndoLastAccountChange: no prior change recorded to undo to"};
    }
    const auto& previous = fieldChanges[fieldChanges.size() - 2];
    const std::string& previousName = previous.name;
    const std::string& previousIndustry = previous.industry;
    const std::string& previousWebsite = previous.website;

    Lightweight::DataMapper mapper;
    auto rows = mapper.Query<db::AccountRecord>()
                    .Where(::Lightweight::FieldNameOf<&db::AccountRecord::id>, "=", *action.accountId)
                    .All();
    if (rows.empty()) {
        throw NotFound{"UndoLastAccountChange: no such account"};
    }
    auto& row = rows.front();

    // Same per-field write-guard as UpdateAccount: restoring industry to a
    // Manager-set value is itself a change to that field, so it requires
    // the same threshold — an undo cannot be used to route around the
    // restriction it would otherwise enforce going forward.
    const std::string currentIndustry{row.industry.Value().ToStringView()};
    if (previousIndustry != currentIndustry) {
        requireRole(action.accountId, Role::Manager);
    }

    row.name = Lightweight::SqlAnsiString<128>{previousName};
    row.industry = Lightweight::SqlAnsiString<64>{previousIndustry};
    row.website = Lightweight::SqlAnsiString<255>{previousWebsite};
    row.version = row.version.Value() + 1;
    mapper.Update(row);

    UndoLastAccountChangeResult result{.account = toView(mapper, row)};
    // Recorded as its own new, journaled entry — an explicit compensating
    // action (this file's doc comment on UndoLastAccountChange), never a
    // silent rewrite of the entries being undone. journal::undoLast()
    // itself is not used: it returns a *detached* holder (LADDER.md's own
    // documented limit) that never touches the live DB row this rung's
    // models actually persist to.
    _journal.recordSuccess<AccountModel>(action, result, nowMillis());
    return result;
}

AccountView AccountModel::execute(const GetAccount& action) {
    if (!action.validate()) {
        throw ValidationError{"GetAccount: accountId is required"};
    }
    Lightweight::DataMapper mapper;
    auto rows = mapper.Query<db::AccountRecord>()
                    .Where(::Lightweight::FieldNameOf<&db::AccountRecord::id>, "=", *action.accountId)
                    .All();
    if (rows.empty()) {
        throw NotFound{"GetAccount: no such account"};
    }
    return toView(mapper, rows.front());
}

ListAccountsResult AccountModel::execute(const ListAccounts& action) {
    (void)action;
    Lightweight::DataMapper mapper;
    auto rows = mapper.Query<db::AccountRecord>().All();
    ListAccountsResult result;
    result.accounts.reserve(rows.size());
    for (const auto& row : rows) {
        result.accounts.push_back(toView(mapper, row));
    }
    return result;
}

ListAccountOptionsResult AccountModel::execute(const ListAccountOptions& action) {
    (void)action;
    Lightweight::DataMapper mapper;
    auto rows = mapper.Query<db::AccountRecord>().All();
    ListAccountOptionsResult result;
    result.accounts.reserve(rows.size());
    for (const auto& row : rows) {
        result.accounts.push_back(AccountOption{
            .id = std::to_string(row.id.Value()),
            .name = std::string{row.name.Value().ToStringView()},
        });
    }
    return result;
}

}  // namespace crm

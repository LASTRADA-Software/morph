# crm — rung 7 of the [application ladder](../LADDER.md)

**Status: 7a under construction** (green-lit 2026-08-28 by direct decision,
ahead of a formal ladder-wide findings-scoreboard review — see
[`../LADDER.md`](../LADDER.md) for the program's general post-rung-4 gate).
This README remains the design record; the rung's defining framework
question (runtime custom fields) was already answered by the standalone
**extension-bag spike** (complete — see
[`EXTENSION-BAG-SPIKE.md`](EXTENSION-BAG-SPIKE.md): yes, reachable today with
no framework change). A mini-Salesforce: accounts, contacts, leads,
opportunities in a pipeline, quotes with exact pricing, per-field
permissions, field-level audit history — and, as the endgame, runtime custom
fields. This rung tests whether morph can carry *metadata-driven* production
business software, the defining property of the Salesforce/SAP class.

Per review, the rung is split: **7a** = steps 1–8 (a conventional CRM on
compiled types), **7b** = steps 9–10 (runtime custom fields), with an
explicit **go/no-go gate** between them — the extension-bag question has a
different risk profile, and a negative answer must not stall the ladder.

## Reference implementations

The open source CRM/ERP world spans a spectrum of "where does the data model
live", and each anchor marks one point on it:

- **[EspoCRM](https://github.com/espocrm/espocrm)** (PHP, AGPL) — **read
  this first.** The whole system, backend and frontend, is driven by merged
  JSON metadata: `entityDefs/{Entity}.json` (fields, types, links),
  `layouts/*.json` (form layouts), with admin-created custom fields written
  as JSON overlays into `custom/`. The Backbone client fetches merged
  metadata and renders every form from it — exactly morph's
  schema-served-forms model, including enum options and link fields
  (analogous to `forms::Choice` action-backed combos). Its **Dynamic Logic**
  (JSON condition trees driving visible/required/read-only) is the spec to
  copy for conditional forms. Also a precedent: EspoCRM ships with
  polling-only notifications. Docs:
  <https://docs.espocrm.com/development/metadata/>
- **[Tryton](https://github.com/tryton/tryton)** (Python, GPL) — the
  cleanest ERP codebase and **the only serious open source ERP that runs on
  SQLite** (its whole test suite does). Exact `Decimal` everywhere for
  money; generic clients render forms from server-served view definitions
  (`fields_view_get`) — same shape as a morph Qt client. The reference for
  the quotes/pricing and document state machines here.
- **[Frappe / ERPNext](https://github.com/frappe/frappe)** (Python, MIT
  framework) — the most complete customization spec in open source: one
  **DocType** JSON defines schema, DB table, form UI, list view, and REST
  API; custom fields are rows merged into the Meta at load time; child
  tables put order lines inside an order form (maps to morph's
  nested-aggregate schema recursion, #35). Submitted documents are
  immutable + amendable — a natural fit for an append-only journal. Docs:
  <https://frappe.io/framework/doctype>
- Runtime ceiling, for orientation only:
  [Twenty](https://github.com/twentyhq/twenty) (metadata in DB tables,
  GraphQL API regenerated at runtime) and
  [Corteza](https://github.com/cortezaproject/corteza) (Apache-2.0, Go —
  the license-safest design to borrow; modules/fields/pages purely runtime
  data). [Odoo](https://github.com/odoo/odoo) is the scope benchmark —
  study its docs, not its source.

## What to implement

Models: `AccountModel`, `ContactModel`, `LeadModel`, `OpportunityModel`
(shared instances keyed by record id), `QuoteModel`, `MetaModel` (serves
schemas/layouts). Build order (each step is a usable milestone):

1. **Core objects + CRUD** — Account, Contact, Lead, Opportunity; list
   actions with filters/pagination; schema-served forms for every edit view
   (validates the existing forms subsystem at real scale).
2. **Relations in forms** — lookup fields via `forms::Choice` backed by
   list actions ("account" combo on a contact); child collections (contacts
   of an account; quote line items via nested aggregates).

   **Design decision, in writing (LADDER.md's discipline rule):** "contacts
   of an account" is served as a **flat, filtered list action**
   (`ContactModel::execute(const ListContacts&)`, filtering on an optional
   `accountId`), not as a nested-aggregate member embedded in
   `AccountView`'s own schema. Reason: `docs/spec/forms/forms.md`'s nested
   aggregate recursion (`std::vector<Sub>` members get their own
   per-field schema annotations, arbitrarily deep) is a real, tested
   framework feature — but this rung's own "Expected strain points" section
   already names its actual gap: *no shipped renderer exists for it*
   ("the QML renderer has no array/child-table control — quote lines need
   an app-level recursive validator plus a child-table renderer
   [framework gap]"). Embedding contacts inside `AccountView` would produce
   a schema no client here can render as an editable child table anyway, so
   step 2 uses the pattern that actually works end-to-end today — the same
   `Choice`-backed lookup + filtered list-action shape `lims`'s "child
   collections" have no precedent for either (no ladder rung has shipped
   one). **Quote line items (step 4) are the rung's one deliberate
   exception** — a true `std::vector<QuoteLine>` nested-aggregate member,
   because unlike a contact list a quote's lines are genuinely part of the
   quote's own submitted payload (one action, one atomic write), not a
   separately-listed collection — and step 4 is where the child-table
   renderer gap actually has to be confronted, not routed around.
3. **Pipeline + lead conversion** — Opportunity stages as guarded, journaled
   transitions (kanban client reuses [`kanban`](../kanban) pieces).
   `ConvertLead` → creates Account + Contact + Opportunity **atomically
   across three models** — the multi-model transactional action morph's
   per-model strands make interesting. Review sharpened both options:
   an orchestrating model that *waits* on sub-actions **blocks a pool
   thread — N concurrent conversions exhaust the pool and deadlock**
   (`Completion<T>` has no chaining to do it non-blockingly); the saga
   alternative leaks partial state on a mid-saga crash (no cross-model
   transactions, and the three per-model journal entries carry **no causal
   link**, so no replay reconstructs the invariant). Decide the idiom
   (recommended: one orchestrating model owning the whole conversion on
   *its own* strand with compensations) — and **write the pool-starvation
   test that shows why naive orchestration is wrong**, plus the
   crash-between-legs test showing what the journal can and cannot say.

   **Design decisions, in writing (LADDER.md's discipline rule):**

   - **Pool-starvation test flakiness, root-caused**: the test wedges a
     3-thread `ThreadPoolExecutor` with 3 blocking "orchestrator" tasks, then
     posts a 4th "signal" task and checks it can't run (`CHECK_FALSE`) and
     that all 3 orchestrators time out (`CHECK(result == "timed out — pool
     starved")`). Two earlier fixes narrowed but didn't eliminate an
     intermittent failure (~1 in 3 runs, then ~1 in 10, then ~1 in 20) where
     one orchestrator returned `"orchestrated"` instead of timing out. Both
     fixes correctly closed *register-as-a-waiter* races (first: poll an
     atomic `blockedCount` incremented before `wait_for` instead of a fixed
     sleep; second: increment it from *inside* `wait_for`'s predicate, which
     the standard guarantees runs under the lock before the call can return
     or block) — but neither was the actual remaining defect. The real cause:
     the signal task targeted `signals.front()`, the specific signal
     orchestrator 0 was waiting on. Once any orchestrator's own independent
     2-second `wait_for` deadline elapsed first (thread-launch jitter means
     the three don't start at the exact same instant), its freed thread
     immediately dequeued the queued signal task (FIFO) and delivered it —
     and if orchestrator 0's own deadline hadn't yet elapsed, it woke early
     and legitimately returned `"orchestrated"`. Not a lost-wakeup bug at
     all: two independent timeouts racing each other, with the signal task
     an accidental third party. Fix: point the signal task at a dedicated
     `Signal` object no live orchestrator is waiting on, so "did the pool
     have a free thread to run this task" (what the test means to assert)
     can never be conflated with "did it happen to beat a real orchestrator's
     own countdown" (irrelevant to the claim). Verified clean across 100
     consecutive runs after the fix, versus reproducible failures at each
     prior fix's stated rate. The lesson for future ladder rungs: when a
     "prove starvation" test also asserts on each blocked task's own return
     value, keep the probe that unblocks-and-observes fully decoupled from
     any object a still-live task under test might independently reach.
4. **Quotes/pricing** — line items with exact `Rational` unit prices,
   discounts, tax; total recomputation as an action (Tryton semantics).
5. **Authorization depth** — role-based per-entity *and per-field*
   permissions via `session::Principal` + `IAuthorizer`; ownership/team
   record scoping (Odoo record-rules style). Served schemas must reflect the
   caller's rights (read-only fields arrive read-only).

   **Design decisions, in writing (LADDER.md's discipline rule):**

   - **Per-entity RBAC**: fully precedented — reuses
     `kanban::BoardModel::requireRole`/`requireRoleOn`'s exact shape
     (`Context::principal` → role-table lookup, `Role` as a linear
     `uint8_t`-ordered hierarchy, `roleFromString` defaulting unrecognized
     text to the least-privileged value). crm's key is `AccountId` rather
     than a per-model key: contacts, opportunities, and quotes all resolve
     back to an account and share its role table (`crm_account_roles`), so
     `requireRole` is one function shared across four models
     (`core/authz.hpp`) rather than kanban's per-model-duplicated copies —
     crm's shared key makes duplication pure cost with no compensating
     benefit kanban's per-model key has.
   - **Fail-open until roles are declared**: an account with zero
     `crm_account_roles` rows (every account steps 1-4's own tests created,
     and any real account before an admin first assigns roles) allows any
     authenticated principal to act — enforcement engages only once *at
     least one* role row exists for that account. This is not "everyone is
     Viewer by default" (which would break every prior step's test and
     silently lock out a freshly-created account with no admin yet) — it is
     a deliberate two-phase adoption: unmanaged accounts behave exactly as
     they did before this rung existed; the moment any role is assigned, the
     account switches to enforced mode and an *unlisted* principal is then
     implicitly `Viewer`. A real deployment would pair this with a
     roles-backfill step at account-creation time (out of scope here — see
     `docs/findings/` convention for where that would be filed as a
     productionization gap, not a bug in this rung).
   - **Per-field enforcement has no framework hook and needed two new
     pieces**, matching the round-5 ground truth this section's "Expected
     strain points" already named: (1) `crm::gui::updateAccountSchemaJsonFor`
     — a per-caller schema decorator, adapting
     `InstanceConstraints::decorate()`'s exact DOM-rewrite idiom
     (`glz::generic_u64`, overwrite the *value* of the already-declared
     `x-readonly` key) to a *role* input instead of *instance data*, proving
     the idiom transfers cleanly; and (2) a hand-written server-side
     write-guard inside `AccountModel::execute(const UpdateAccount&)` — compare
     the submitted `industry` against the stored row's current value, and
     only demand `Role::Manager` if it actually changed. Per
     `docs/spec/forms/forms.md`'s explicit disclaimer ("Field metadata is not
     a security control"), (1) alone is decorative; (2) is what actually
     stops a client that ignores `x-readonly` and submits an edit anyway.
     Only `AccountModel::industry` is retrofitted as the rung's one worked
     example of both halves — the same pattern applies to any other
     Manager-restricted field a real build would add.
   - **Team/ownership record scoping (Odoo record-rules style) is
     out of scope for this rung's build**: the investigation behind this
     decision found no team-based (multi-principal-per-record) precedent
     anywhere in the ladder — only `bookmarks::BookmarksAuthorizer`'s
     single-owner `authorizeInstance` — and crm's own account-role table
     already gives every model a natural, real ownership boundary (the
     account) without needing a second, broader scoping mechanism on top.
     A genuine multi-team-per-account model (e.g. "Sales" vs. "Support" teams
     with different rights on the same account) is a real extension a later
     pass could add, following `crm_account_roles`' shape with a `team_id`
     column, but this rung's own scope does not require it.
6. **Field-level audit + undo** — EspoCRM's Stream / Frappe's Version
   rendered from the morph journal; undo last change per record.

   **Design decisions, in writing:**

   - **Undo is an app-level compensating action, not `journal::undoLast()`
     itself.** `undoLast()` returns a *detached* `IModelHolder` (LADDER.md's
     own documented journal limit) — it never touches a live, DB-backed
     model's row. `AccountModel::execute(const UndoLastAccountChange&)`
     instead reads the second-to-last recorded change's field values and
     writes them back to the live row as a **new**, separately journaled
     entry — an explicit, auditable reversal, matching the framework's own
     guidance ("applications that need to durably reverse a checkpointed
     action must record a compensating action, not rely on `undoLast()`").
   - **History and undo both filter to *field-changing* entries.**
     `AccountModel` is one shared instance across every account (README §1),
     so its attached log holds every account's full mixed history —
     `SetAccountRole` entries name an account but carry no
     name/industry/website. `GetAccountHistory` lists every entry (a
     `SetAccountRole` row simply renders with empty fields — it *is* part of
     the account's history), but `UndoLastAccountChange`'s "second-to-last
     change" specifically means the second-to-last entry with actual field
     values, so an administrative role change interleaved between two edits
     is not mistaken for the edit being undone.
   - **Field extraction checks `result` before `payload`, falling back to
     `payload` only when `result` has nothing.** `CreateAccount`'s own
     *result* is only `{accountId}` (the fields it established are on its
     *payload* instead — the id doesn't exist yet at submission time, so it
     can't be in the payload, but the fields it's creating already are);
     `UpdateAccount`'s result nests the full post-state under `"account"`.
     One shared extraction path handles both shapes rather than special-
     casing each action type.
   - **Redaction is enforced in `GetAccountHistory` itself, at the same
     `Role::Manager` threshold `UpdateAccount`'s write-guard uses** — a
     restricted principal sees every entry's `name`/`website` but never
     `industry`'s historical value, closing the exact gap
     crm/README.md's own "Expected strain points" section names ("journal
     payloads are stored whole, so field-level history naively shows
     restricted users values they cannot read"). Undo carries the identical
     write-guard: restoring `industry` to a value that differs from its
     current one is itself a change to a Manager-only field, so a `Member`
     cannot use undo to route around the restriction they could not bypass
     directly.
7. **Dynamic logic** — conditional required/visible/read-only encoded in
   the served schema. A round-5 correction here said EspoCRM's condition
   *trees* "cannot be adopted as-is" because `x-rules` had no `and`/`or`/`not`
   and combinators would have to be filed as a framework proposal. **The
   combinators shipped** (morph#78): `And`, `Or` and `Not` with the
   `andOf`/`orOf`/`notOf` builders (`include/morph/forms/forms.hpp`), and the
   emitted vocabulary carries `and`/`or`/`not` alongside `engaged`,
   `notEngaged`, `equals`, `greater` and `less`. Condition trees nest to any
   depth, so EspoCRM's tree shape maps directly and no proposal is needed.

   Two limits from that correction do still hold, and the rung must map around
   them rather than assume they lapsed with the combinators: there is **no
   `in`-list / set-membership kind**, and **`Choice` has no ordering** —
   `greater`/`less` are defined over numeric fields, so a picklist comparison
   has to be expressed as an `or` of `equals`.

   **Design decision, in writing.** The rung's first candidate rule —
   "`primaryContact` required once the opportunity's `stage` passes
   `Prospecting`" — turned out **not implementable as originally framed**,
   confirmed by investigation before any code was written: `greater`/`less`
   require `ComparableField` (`EmptyCapableField` plus `operator<=>` on the
   engaged value), and neither of the rung's existing candidate fields
   qualifies — `OpportunityStage` (a plain `enum class`) has no `hasValue()`
   at all, and `Choice` has `operator==` but no `operator<=>`. `equals`
   fares no better: its literal must satisfy the closed `RuleLiteral`
   concept (`int64_t`/`bool`/`std::string`/`Rational`/a string literal), and
   a scoped enum converts to none of those without an explicit cast the
   framework does not perform for you. So the two limits this section
   already named (no `in`-kind, `Choice` has no ordering) are not the whole
   story — **a plain `enum class` field cannot be a rule's condition or
   comparison operand either**, a third, previously-unstated restriction
   this rung's own investigation surfaced.

   The rung's actual rule instead conditions on a new field added for this
   purpose: `Opportunity::expectedCloseValue` (`Money = Quantity<CrmUnit::usd,
   2>`, a genuine `EmptyCapableField`) with `requiredWhen(primaryContact,
   engaged(expectedCloseValue))` — once a rep enters a deal value, the
   schema itself demands a primary contact. This is the rung's one
   `formRules` declaration; it did not need `andOf`/`orOf`/`notOf` nesting
   (the condition is a single `engaged`), but the combinators remain
   available and the same mechanism (`ruleList`, `requiredWhen`,
   `allRulesSatisfied` called from `validate()`) is exactly what a later,
   compound rule would use without any new plumbing.
8. **Offline** — edit queue in `SqliteOfflineQueue`, replay with conflict
   surfacing; no CRM in this class does offline well — it is morph's chance
   to differentiate.

   **Design decisions, in writing (LADDER.md's discipline rule):**

   - **Reused lims's §7 shape wholesale, not reinvented.** lims (rung 6)
     already answers this exact framework question — offline capture with
     conflict surfacing, via `Model::onBackendChanged()` rather than
     `SyncWorker` (docs/spec/offline/offline.md, "Conflict resolution on
     replay": `SyncWorker`'s `ReplayFunction` returns only a `bool`, with no
     channel for "flagged, not merged or dropped"; the model path is the only
     one that supports richer-than-boolean outcomes). crm's version
     (`crm::offline::FieldOutbox`, `QueuedOpportunityUpdate`,
     `ConflictReason`/`ConflictStatus`/`ConflictResolution`/`ReplayOutcome`,
     `ListConflicts`/`ResolveConflict`, `OpportunityModel::onBackendChanged`)
     mirrors `lims::offline::FieldOutbox`/`lims::SampleModel`'s shape line for
     line — same base-version-stamping outbox, same at-most-once ledger table
     (`crm_opportunity_replayed_ops`, deliberately unindexed — enforcement is
     `alreadyDecided()`'s query, not a DB constraint, matching
     `lims_replayed_ops` exactly), same "reason ordering" rule (lifecycle
     closed is checked before stale-base, since a closed deal has also gone
     stale and the more specific reason is the more actionable one to show a
     human), same undecodable-payload handling (journaled via a new
     `SelfJournal::recordRejectedPayload`, dropped, never left to block the
     queue). Two adaptations, both because crm's own shapes differ from
     lims's: `version` here is a plain `std::int32_t` (matching
     `OpportunityView::version`'s existing type), not a `Tagged<int32_t,
     "Version">` newtype the way lims's `SampleVersion` is; and the queued
     envelope carries `UpdateOpportunity`'s full field set (account, primary
     contact, name, expected close value) rather than one typed capture value,
     since an opportunity edit is multi-field where a lab reading is one
     number.
   - **`QueuedOpportunityUpdate` does not reuse `CRM_OPPORTUNITY_FORM_RULES`.**
     `UpdateOpportunity`'s own `requiredWhen(primaryContact,
     engaged(expectedCloseValue))` rule (step 7) is a *live* form rule,
     re-validated against the record as it stands *now*. A queued envelope's
     own `validate()` only needs to confirm the envelope itself decodes into
     something usable (an opportunity, an author, a dedup token, a name) —
     replaying it does not go through `execute(UpdateOpportunity)` at all
     (direct ORM row mutation, matching `ConvertLead`'s reasoning for why an
     orchestrating model writes directly rather than dispatching), so
     step 7's rule is simply not in the replay path for this queued action.
     A field rep's queued edit could therefore, in principle, land with an
     expected-close-value/no-primary-contact combination step 7's rule would
     reject if submitted live — considered out of scope for §8, which asks
     for conflict surfacing, not cross-step rule composition; flagged here in
     writing rather than silently decided.
   - **Conflict resolution's `ApplyAnyway` path re-runs `parseChoice` against
     the *current* account/contact rows,** not the ones live at enqueue time
     — an account or contact the field rep chose could have been deleted (or,
     for contacts, reassigned) while the device was offline, and the resolver
     (a human deciding to apply a stale edit anyway) sees that as a thrown
     `NotFound` rather than a silently-orphaned foreign key, the same
     guard-on-every-write discipline every other crm model write path already
     follows.
9. **Runtime custom fields — the endgame.** Admin action
   `AddCustomField { entity, name, type, unit?, required? }` extends the
   *served* schema at runtime and persists values. Compiled C++ action
   structs cannot grow members, so this decided the framework question this
   rung exists to ask: can a morph model carry an open extension bag
   (`map<string, Value>` alongside typed members) whose fields appear in
   schemas, forms, validation, and the journal like first-class ones?
   **Answered by the extension-bag spike** (yes, with no framework change —
   see [`EXTENSION-BAG-SPIKE.md`](EXTENSION-BAG-SPIKE.md) for the mechanism
   and its one per-action cost); a real `AddCustomField` action, persistence,
   per-field authz, and lifecycle handling remain 7b build work, listed under
   "What a real 7b would still need" in that finding doc. EspoCRM (file
   overlay), Frappe (merged Meta rows), and Twenty (runtime schema regen) are
   the three prior answers.

   **Design decisions, in writing (LADDER.md's discipline rule):**

   - **Scope, decided before building**: the spike's own "what a real 7b
     would still need" list names five separate concerns. Built here: a real
     `AddCustomField`/`ListCustomFields` action pair (journaled, persisted to
     `crm_custom_field_defs`), custom values persisted per-account
     (`crm_account_custom_values`), values appearing in the served
     `CreateAccount`/`UpdateAccount` schema, and the whole mechanism surviving
     create/update/get and journal replay — on one entity (`Account`), not
     all four. Explicitly **not** built, named rather than silently decided:
     per-field authz on custom fields (any account member who may edit the
     account at all may set any custom value — `UpdateAccount::industry`'s
     Manager-only write-guard has no analogue here), delete-a-field-while-
     in-use races (`AddCustomField` has no removal counterpart; a definition
     is permanent for this rung), and unit-/Choice-backed custom values
     (`CrmCustomValue` stays string/number/bool, the spike's own `EB_Value`
     narrowing, for the same reason: a `Quantity`-in-bag needs unit/decimal
     metadata traveling with the value, a `Choice`-in-bag needs referential
     re-validation, and building either is a larger decision than this pass).
   - **`AddCustomField` has no role check.** Every other crm mutating
     action's authorization is account-scoped (`requireRole(AccountId,
     Role)`), but a custom field *definition* is schema-wide, not tied to one
     account — and crm has no global-admin role concept anywhere else in the
     rung. `AddCustomField` therefore only requires an authenticated
     principal, the same floor every mutating action already enforces. A
     genuine gap for a real product (anyone with a login can add a
     schema-wide field), named in `custom_field_dto.hpp`'s own doc comment
     rather than assumed away — out of scope for the same reason per-field
     custom-value authz is: no framework primitive for global admin exists to
     build on.
   - **`AccountView`/`CreateAccount`/`UpdateAccount` pay the spike's
     documented `glz::meta::value` cost.** Opting a compiled action into an
     extension bag means giving up morph's pure-reflection convenience and
     hand-listing every compiled member in a `glz::meta<T>::value`
     declaration alongside `unknown_read`/`unknown_write` — confirmed to
     still be necessary for a real (not throwaway) action, not just the
     spike's own `EB_UpdateContact` probe. `AccountView` needs only
     `unknown_write` (it is a served shape, never decoded from a caller's
     wire body — the model fills `extra` directly via `toView()`), while
     `CreateAccount`/`UpdateAccount` need both directions.
   - **Custom values persist as one row per `(account, field)`, not a JSON
     blob column on `AccountRecord` itself** — `crm_account_custom_values`
     mirrors `AccountRoleRecord`'s existing one-to-many child-table shape
     (fetched via a separate query in `toView()`, the same way `QuoteModel`
     fetches its line items) rather than adding a wide text column that would
     need its own ad-hoc merge/patch logic. `value_json` per row is still a
     JSON-serialised `CrmCustomValue`, not a native typed column — the
     Lightweight ORM has no JSON column type (`EXTENSION-BAG-SPIKE.md` names
     this gap), so a real per-key-queryable design remains future work,
     unchanged from what the spike already flagged.
   - **`UpdateAccount` full-replaces the custom-value set, matching its own
     compiled-field convention** ("full replace, not a partial patch", this
     rung's step-1 design decision): a caller resubmits every custom value it
     wants to keep, and an omitted key is dropped, not left untouched. A
     per-key diff/patch update was considered and rejected as unnecessary
     complexity for a rung whose compiled fields already work this way.
   - **Required custom-field enforcement lives in the model, not
     `validate()`** — `CreateAccount::validate()`/`UpdateAccount::validate()`
     check only what a bag-free action already checked; `AccountModel`'s own
     `validateCustomFields()` queries the live `CustomFieldDefRecord`
     table and throws before any row is touched. `schemaJson<A>()`'s compiled
     `required` array cannot express a runtime-registered field's
     required-ness (the same limit `EXTENSION-BAG-SPIKE.md`'s
     `EB_UpdateContact::validate()` names), so this check has nowhere else to
     live.

   **7b — the three items above named out of scope, built:**

   - **Per-field authz.** `AddCustomField::minRoleToEdit` (default
     `Role::Member`) gates *changing* a custom value on an *existing*
     account, enforced by `requireCustomFieldRoleForChanges()` with the same
     "resubmitting the current value is not a change" rule
     `UpdateAccount::industry`'s own write-guard already uses — compared by
     re-serialising both sides to JSON text (`glz::generic_u64` has no
     `operator==` of its own), the same "compare the JSON, not a bespoke
     structural walk" idiom `entryNamesAccount()` already uses elsewhere in
     this file. Deliberately **not** checked on `CreateAccount`: neither an
     account id nor an account-role row exists yet at creation time, so
     there is nothing to compare a "change" against — the creator implicitly
     sets the account's own initial custom-field state, the same way they
     set `name`/`industry`/`website` with no role check either.
   - **Delete-a-field-while-in-use.** `DeleteCustomField` cascade-deletes
     every account's stored value for the field in the same transaction —
     no orphaned rows. Once gone, `validateCustomFields()`'s own "every key
     must name a live definition" check means a client that still submits
     the deleted key (a stale open form, a queued offline edit, a journal
     replay of an old payload) is rejected with a clear `ValidationError`,
     not silently ignored (which would let the client believe its edit
     landed) or stored as an unreachable orphan (which would grow the table
     with data nothing can ever query or clean up). This was an explicit
     three-way choice, decided with the user before building rather than
     picked unilaterally: reject was chosen over silently-drop specifically
     because a rejected write tells the field client its edit needs
     attention, where a silent drop would not.
   - **Money- and Choice-backed values.** `CustomFieldType::Money` reuses
     `Money = Quantity<CrmUnit::usd, 2>` verbatim — crm has exactly one unit
     family, so no generic unit-metadata system was needed, only a new
     `CustomFieldType` enumerator; a `Money` custom value is written/read as
     its ordinary `Quantity` JSON shape directly into the `CrmCustomValue`
     DOM, no bespoke wire format. `CustomFieldType::Choice` adds
     `AddCustomField::choiceOptions` (a fixed, admin-declared list, stored as
     JSON text — the same "no native array/JSON column" reason
     `AccountCustomValueRecord::valueJson` and
     `OpportunityConflictRecord::payload` already store opaque JSON as text);
     `validateCustomFields()` rejects a submitted value unless it is a
     string matching one of the declared options — the referential
     re-validation `EXTENSION-BAG-SPIKE.md`'s own finding doc calls for.
     `Number`/`Boolean`/`Text` type-matching beyond what `CrmCustomValue`'s
     own JSON shape already constrains is intentionally still not checked —
     unchanged from step 9's own scope, and not one of the three items 7b
     set out to close.
   - **`AddCustomField`/`DeleteCustomField` still have no role check of
     their own**, even after `minRoleToEdit` exists. `minRoleToEdit` gates
     *changing a value on an account* (account-scoped), not *declaring or
     removing the field* (schema-wide) — crm still has no global-admin role
     concept anywhere in the rung, so this gap named in step 9 is
     unchanged by 7b, not accidentally left open.
10. *(stretch)* Saved views/filters as stored definitions executed by list
    actions. Report builders, dashboards, email sync, and workflow timers
    are **out of scope** — every researched product implements these as
    background/push machinery; note it and stop.

    **Design decisions, in writing (LADDER.md's discipline rule):**

    - **No prior ladder precedent, and a terse one-line spec** — this step
      had to be designed from scratch, not adapted from an existing rung.
      Taken literally: "stored definitions executed by list actions" means a
      `SavedView` row names a `ListOpportunities` filter, and `RunSavedView`
      re-dispatches that same action with the stored filter — never a cached
      result. A saved view therefore always reflects the pipeline's *current*
      state; moving a deal into a saved view's filtered stage *after* the
      view was created still shows up the next time it runs (pinned by its
      own test: "RunSavedView reflects the pipeline's current state, not a
      cached result").
    - **`ListOpportunities` grew a `stage` filter** (previously `accountId`
      only) — the concrete target for the first saved view to exist over.
      Combines with the existing `accountId` filter by AND, matching the
      "each engaged filter narrows further" shape `ListContacts`'s single
      filter already established, extended to two independent dimensions.
    - **No generic filter DSL.** A saved view is exactly
      `ListOpportunities`'s own two optional filters (`accountId`, `stage`),
      not an arbitrary predicate tree — building a query language has no
      precedent anywhere in the ladder and is a materially larger decision
      than this stretch step asks for. A later saved view over a richer
      filter set (e.g. `expectedCloseValue` ranges) extends this same
      `(accountId, stage)` shape with more optional fields, the same way
      `ListOpportunities`'s own filter surface itself grew here — not a
      rewrite into a generic predicate language.
    - **Scoped per-principal (`owner`), personal views only** — "my
      Negotiation-stage deals" is the everyday shape a sales rep wants, not
      an org-wide shared view. `RunSavedView` on another principal's view
      returns `NotFound` (not `Forbidden`): unlike `DeleteSavedView` (a
      mutation, where "the id is real but you may not act on it" is the
      honest answer, matching `QueuedOpportunityUpdate`'s author-check
      convention), a *read* of someone else's saved view must not even
      confirm that view exists — the same "must not leak whether someone
      else's resource exists" reasoning `ConflictId`/account-scoped lookups
      elsewhere in this rung already follow. A shared/team-visible view
      (an `AccountRoleRecord`-style visibility table) is future work, not
      silently assumed by this shape — nothing here prevents adding it later
      without a breaking change to `SavedViewRecord`.
    - **`RunSavedView` re-dispatches through a fresh, in-process
      `OpportunityModel`, not a duplicated query.** Reusing
      `OpportunityModel::execute(ListOpportunities)` directly is what makes
      this a "definition executed by [a] list action" rather than a second,
      parallel implementation of the same filter logic that could drift from
      it. No `ConvertLead`-style pool-starvation hazard here: this is a
      plain, unkeyed, single read with no shared strand, transaction, or
      nested-dispatch wait to deadlock on — the concern that idiom exists to
      warn against does not apply to a stateless read composed in-process.

## morph subsystems exercised

Forms as the product (not a feature); nested aggregates + action-backed
choices; per-field authorization; multi-model atomic actions vs. per-model
strands; journal as field-level history; offline for business records; the
compiled-types vs. runtime-metadata boundary.

## Expected strain points

- `ConvertLead` atomicity across three strands and one SQLite database
  (pool-starvation and crash-between-legs tests above).
- Schemas become per-caller (rights) and per-tenant (custom fields) —
  schema serving turns from static reflection into computed data.
  **Round-5 ground truth**: `schemaJson<A>()` is one cached, unversioned
  string per compiled type, and `x-readonly`/`x-hidden` are compile-time
  presentation only ("not a security control") — per-caller shaping means
  app-side JSON post-processing *plus* independent server-side per-field
  enforcement; neither has a framework hook [framework gap]. That includes
  **`x-optionsAction` under authorization**: a `forms::Choice` combo whose
  backing list action the caller cannot run renders as a dead control —
  and its sibling failure, a *filtered* options action returning zero rows,
  makes a required Choice permanently unsubmittable. Also mandatory
  (review D6): **Choice membership is never validated** — a stale id
  (row deleted between fetch and submit) passes the forms layer; the
  model-level referential re-check is the binding convention for every
  lookup field.
- **The shipped form renderer auto-fires on validity and re-fires per
  edit** — there is no submit button (review B4/D7). A CRM of
  side-effectful mutations needs the **explicit-submit / presenter-gated
  mode** (presenter owns the single `submitIfValid`) built before any form
  ships; the two-phase duplicate-detection flow is impossible without it.
- **Nested line items get schemas but no enforcement** (review D3):
  `allRequiredEngaged` and precision reconciliation stop at top level, and
  the QML renderer has no array/child-table control — quote lines need an
  app-level recursive validator plus a child-table renderer [framework
  gap]. Empty-vs-zero also bites here: a computed total with a
  never-entered discount computes to *empty*, not `qty × price` — decide
  per field.
- **Per-field authz vs. one journal**: journal payloads are stored whole,
  so field-level history naively shows restricted users values they cannot
  read. Redaction-on-serve is app logic; test that a restricted principal
  leaks nothing through history *or undo replay*.
- **Custom-field lifecycle races (7b)**: admin deletes a custom field while
  (a) a client holds an open form containing it, (b) an offline client has
  queued edits carrying it, (c) journal replay carries it. Decide
  reject / drop / preserve-as-orphan and test all three arrival paths.
- **Stable pagination**: keyset-cursor lists as the ladder idiom; test
  cursor stability while another client renames/deletes rows mid-walk.
- The extension-bag design: validation, journaling, and forms for fields
  the C++ type system has never heard of.

Two review-added features that stress *new interaction shapes* (not bulk),
**both deferred to a "7-later" bucket per the delivery review** (each is a
mini-rung; neither gates 7a/7b): **duplicate detection on create** ("this
contact may already exist — create anyway?") as a two-phase action —
execute → warnings + confirmation token → re-execute; and **record merge**
(two contacts, each with journal history and possibly live shared
instances — two attached handler sets, one survivor), the hardest
journal + instance-directory interaction in the ladder.

## Definition of done

- A rep works a lead → conversion → opportunity → quote → won, entirely on
  generated forms, on desktop and WASM, local and remote.
- A second user with a restricted role sees the same records with fields
  hidden/read-only, enforced server-side.
- An admin adds a custom field at runtime; existing clients render it on
  next schema fetch; its values persist, validate, and journal.

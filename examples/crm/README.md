# crm — rung 7 of the [application ladder](../LADDER.md)

**Status: design annex** ([round-7 program decision](../LADDER.md)) — this
README is the deliverable; the rung's defining framework question (runtime
custom fields) runs earlier as the standalone **extension-bag spike**, and
building 7a is a post-rung-4 decision. A mini-Salesforce: accounts,
contacts, leads,
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
4. **Quotes/pricing** — line items with exact `Rational` unit prices,
   discounts, tax; total recomputation as an action (Tryton semantics).
5. **Authorization depth** — role-based per-entity *and per-field*
   permissions via `session::Principal` + `IAuthorizer`; ownership/team
   record scoping (Odoo record-rules style). Served schemas must reflect the
   caller's rights (read-only fields arrive read-only).
6. **Field-level audit + undo** — EspoCRM's Stream / Frappe's Version
   rendered from the morph journal; undo last change per record.
7. **Dynamic logic** — conditional required/visible/read-only encoded in
   the served schema. **Round-5 correction: EspoCRM's condition *trees*
   cannot be adopted as-is** — morph's `x-rules` vocabulary is closed
   single-node conditions (no `and`/`or`/`not`, no `in`-lists, and lookup
   fields support only `engaged`/`equals` — `Choice` has no ordering).
   The rung maps EspoCRM logic onto the closed vocabulary and files
   combinators as a framework proposal where the mapping fails.
8. **Offline** — edit queue in `SqliteOfflineQueue`, replay with conflict
   surfacing; no CRM in this class does offline well — it is morph's chance
   to differentiate.
9. **Runtime custom fields — the endgame.** Admin action
   `AddCustomField { entity, name, type, unit?, required? }` extends the
   *served* schema at runtime and persists values. Compiled C++ action
   structs cannot grow members, so this decides the framework question this
   rung exists to ask: can a morph model carry an open extension bag
   (`map<string, Value>` alongside typed members) whose fields appear in
   schemas, forms, validation, and the journal like first-class ones?
   EspoCRM (file overlay), Frappe (merged Meta rows), and Twenty (runtime
   schema regen) are the three prior answers.
10. *(stretch)* Saved views/filters as stored definitions executed by list
    actions. Report builders, dashboards, email sync, and workflow timers
    are **out of scope** — every researched product implements these as
    background/push machinery; note it and stop.

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

# lims grows into a lab business: ERP modules and a production GUI

lims, the ladder's large and growing rung, becomes the system a contract laboratory runs its business on: it bills
the analyses it publishes, keeps a double-entry ledger, buys and consumes reagents, and separates the companies and
sites it operates. It gets a GUI built to the standard of a commercial product. Both exist to find where morph stops
being enough for an SAP-class application, while keeping morph free of the domain those applications contain.

## Principle: domain in modules, mechanism in morph

morph knows no domain. An ERP is domain knowledge — accounts, prices, stock, tax — composed from separate modules, and
the framework's job is to make that composition possible without the modules knowing each other. A piece of work lands
on one side by two questions, both of which must be answered yes before anything moves into `include/morph`:

1. **Is it mechanism?** Would bookmarks, bank or an unwritten CRM need it unchanged? The chart of accounts, pricing,
   invoice layout, stock valuation, approval thresholds and the tax rate are not; atomic work across models, durable
   reactions between models, leases, scheduled work, approval state and scoped authorization may be. Document
   rendering (PDF, print files) is domain output and is not built in morph at all.
2. **Has it been needed twice?** `AGENTS.md`: the same hand-rolled guarantee in a second place is a missing framework
   seam. Inside lims, two modules count as two places. `IMPLEMENTATION.md`'s promotion rule counts rungs; it is amended
   to say that a lims module counts as a consumer and that the second module's need promotes.

So a guarantee confirmed once is recorded as confirmed and built in lims; its second instance is built against a
framework seam designed for both. The design is built so that every hypothesis below has two instances, except
hypothesis 4, which has one: approval escalation is a deadline the approval rule reads, not scheduled work (§ Expected
strain points).

## Size

The ladder was cut to three rungs because applications of one size find what one would, so lims must become clearly
larger than bank. Counted as registered models and actions, excluding tests and bank's in-memory WebAssembly copy:

| | lims today | lims after this design (estimate) | bank |
|---|---|---|---|
| Models | 4 | ~16 | 11 |
| Actions | 45 | ~115 | 41 |
| Screens | 4 | ~18 | 6 |
| Modules | 1 | 5 | 1 |

The plan fixes the numbers; the scope below is already cut to the smallest set that gives every hypothesis its
instances, because `AGENTS.md` measured scaffolding outgrowing the library six to one, and features that only repeat an
instance already covered add scaffolding and find nothing.

## Reference implementations

Requirements are mined from these; no code is read or copied from the GPL projects.

- **[Tryton](https://www.tryton.org/)** (GPL) — the *module* reference: strictly separated modules (party, product,
  stock, purchase, sale, account) with declared dependencies, and a clean double-entry account module with periods.
- **[InvenTree](https://github.com/inventree/InvenTree)** (MIT, already a lims anchor) — stock items, lots with expiry,
  locations, purchase orders and receipts.
- **[ERPNext](https://docs.erpnext.com/)** (GPL) — documentation only: approval workflows, naming series, period
  closing.

## Module architecture

```
lims/app ── composition: wires modules, owns the server, the bundle and the GUI
  ├── lims/org        companies, sites, principals, role assignments        (shared)
  ├── lims/posting    the posting contract: request and result DTOs only    (shared)
  ├── lims/lab        samples, analyses, results (today's lims)
  ├── lims/billing    price lists, billable lines, invoices, billing runs
  ├── lims/ledger     accounts, periods, journal entries, exchange rates
  └── lims/inventory  items, lots, locations, requisitions, orders, receipts
```

**Dependency rule.** Each module is its own static library with its own include directory, DTOs, entities, migrations
and models. A module's target links `lims/org` and, where it posts or records postings, `lims/posting`; nothing else.
Because a module's include path holds only its own headers and those two, including another module's header is a
compile error, not a lint finding. A compile check (`tests/compile_checks/`) includes billing from inventory and must
fail to build: the control that proves the boundary is real. `morph_add_rung()` grows an optional `MODULES` argument
that builds one library per module with these link rules; this is build configuration and changes nothing for the
other rungs.

**Why the rule is strict.** Real ERP modules are written and released by different teams; the property under test is
whether morph lets them cooperate without knowing each other. A single model owning invoices and journal entries would
make posting trivially atomic and hide exactly the gap this design is looking for, so the rule is what makes
hypothesis 1 binding, and it is kept deliberately.

**Reactions: domain events.** A module that commits a fact other modules may react to — `lab.AnalysisPublished`,
`lab.ResultCaptured` — writes an outbox row in the same transaction as the fact. The event's id is
`<module>.<event>:<aggregate id>:<aggregate version>`, so a redelivery has the same id. `lims/app` wires each event
to a consumer in another module (billing turns `AnalysisPublished` into a billable line; inventory turns
`ResultCaptured` into a consumption). Consumers are idempotent by event id. Which framework parts carry delivery is
what hypothesis 2 tests.

**Atomic steps: posting.** Posting an invoice must assign its number, mark it posted and create its journal entry as
one fact: a posted invoice with no entry, or an entry with no invoice, is a ledger defect, not an eventual-consistency
window. The domain halves stay in their modules: **billing** owns the invoice, its number series and the mapping from
an invoice to accounts (receivable, revenue, tax, as account codes configured per company) and builds a
`posting::PostingRequest`; **ledger** owns validation and recording of a `PostingRequest` and knows nothing of
invoices. The atomic step itself — billing's state change and ledger's entry committed together — is the open
question: hypothesis 1 says morph has no way to express it, and the first implementation shows what happens when the
two calls are made in sequence. Goods receipt (inventory → ledger) is the second instance.

**Shared database.** All modules use one SQLite file through the Lightweight ORM (`IMPLEMENTATION.md` rule 4), with
table names prefixed per module. No module reads another's tables. Sharing the file is a deployment choice: it makes a
cross-model transaction physically possible, so if one cannot be expressed, the gap is morph's, not SQLite's.

## Part A — Org and authorization foundation

Built first, because every later record carries its company and every later action is authorized against it. It lands
in this order:

1. **The split.** Today's lims moves into `lims/lab` and `lims/app` with no behaviour change: the existing suites pass
   unmodified, apart from include paths.
2. **Org.** Companies have a base currency and a fiscal-year start; sites belong to a company. Every lab record gains
   its site.
3. **Scoped roles.** Role assignments become `(principal, role, scope)`, where the scope is a company or a site. Roles
   stay non-hierarchical (lims decision 16). Existing assignments migrate to the company of the single seeded site.
4. **Declared authorization.** Per action: the role it needs, the payload field holding its scope, optional value
   limits (an approver approves up to a stated amount), and separation-of-duties pairs (one principal may not both
   create and approve one invoice). The capture-and-verify rule lims enforces today becomes the first declared pair.
5. **Visibility.** Every list query returns only records in scopes where the principal holds a role.
6. **Administration screens** for companies, sites, users and role assignments.

## Part B — Ledger and billing

**Ledger.**

- Chart of accounts per company: asset, liability, equity, revenue and expense accounts, each with a code and a name.
- A journal entry has a date, a period, a source reference supplied by the poster, and two or more lines, each with an
  account, a side (debit or credit), an amount in transaction currency and its value in company currency.
- **Balancing.** An entry must sum to zero per transaction currency and in company currency, checked before commit.
  The ledger never rounds or auto-balances. Company-currency values are rounded by the **poster**, half-up to the
  company currency's decimals, line by line; where that leaves a company-currency difference, the poster adds an
  explicit exchange-rounding line to the company's configured rounding account. The ledger refuses any entry that
  still does not balance.
- Entries are immutable once posted. A correction is a reversal entry that names the original.
- Periods are monthly and open, closing or closed. Posting into a closed period is refused. Closing verifies that no
  draft entries remain and freezes the period's balances.
- Exchange rates are dated, per currency pair, entered by hand; a posting uses the rate valid on its date.
- Reports: trial balance, account ledger, balance per account as of a date.
- Money is `morph::units::Quantity` over `morph::math::Rational`, never integer minor units. One company currency is
  zero-decimal (JPY), so that case is always exercised.

**Billing.**

- Price lists per company, versioned with effective dates: a price per analysis definition, optionally overridden per
  client.
- `lab.AnalysisPublished` creates a billable line at the price valid on the publishing date. A redelivered event bills
  nothing twice.
- An invoice collects one client's billable lines, in the client's currency. Lifecycle: draft → submitted → approved
  → posted. Approval is four-eyes and limited by the approver's value limit; an approval not acted on within its
  deadline escalates to a company-scoped approver.
- While one clerk edits a draft invoice, it is leased to them: a second clerk sees who holds it, and the lease lapses
  when the holder's connection closes or after a timeout.
- Posting assigns a **gap-free number** per company and fiscal year (`INV-2026-000123`). A posting that fails leaves no
  hole in the series and no partial state.
- Tax: one flat rate per company, computed on the invoice total and rounded half-up to the currency's decimals. No tax
  engine.
- The **billing run** is scheduled: on a configured day each month it drafts invoices for every client with unbilled
  lines. Its progress is visible while it runs, and a run interrupted by a restart resumes rather than duplicating
  invoices.

## Part C — Inventory and procurement

- Items (reagents, consumables) with a stock unit; quantities are `morph::units::Quantity`, so a receipt in mL of a
  reagent stocked in L converts exactly.
- Lots with an expiry date, at a location in a site. Stock on hand is per item, lot and location.
- A **requisition** is approved by a principal whose value limit covers its total, and escalates past its deadline,
  as invoices do. An approved requisition becomes a purchase order to a supplier, in the supplier's currency.
- A **goods receipt** against an order creates lots and posts goods-received against goods-received-not-invoiced,
  atomically with the receipt: the second posting instance.
- Analysis definitions declare reagent consumption per analysis. `lab.ResultCaptured` consumes from the
  first-expiring lot and posts the consumption cost.
- **Reservation.** Assigning analyses to a sample reserves the lots they will consume. Two benches reserving the last
  lot race: one wins, the other is told which principal holds it; a reservation lapses when its holder's connection
  closes or after a timeout. Reservation is per sample, so lims decision 19 (no worksheets) stands; a reservation that
  must span several samples atomically is its stated condition for reopening, and this design does not need one.

Out of this part: supplier invoices and three-way match, stock counts, reorder jobs. Each would repeat an instance
already covered.

## Part D — The production GUI

lims's GUI is built to the standard of a commercial product, to find where the declarative UI stops. Its audience is
still morph's regression suite, not a customer: it is production-quality because the limit being looked for is where
production quality cannot be reached. `IMPLEMENTATION.md` rule 2 and `LADDER.md`'s "no rung is a showcase" are amended
to exempt lims's look and to say why; bookmarks and bank keep minimalism.

**Constraints.**

- Every screen is a ui/1 document written with the C++ authoring API and served by the lims server. There is no
  hand-written QML and no per-screen client code. Where a design asks for something ui/1 cannot say, the fix is a
  vocabulary or frontend change in morph, not a screen workaround.
- **Mockups first**, in `examples/lims/design/mockups/`: one application shell (module navigation, global search, an
  approvals inbox, a company and site switcher) and one HTML mockup per screen. The lab screens adapt the ten
  `examples/lims_gallery` mockups into that shell rather than redrawing them; the gallery itself is unchanged and stays
  a backend-free study. Each screen is compared to its mockup by side-by-side grabs.
- Qt Quick on the desktop is the reference frontend. The terminal frontend must render every screen usably and is not
  judged on look.

**Production criteria**, applied to every screen:

- loading, empty and error states, and a refused action explains itself in words, never an exception text;
- no raw ids, enum names or unformatted numbers on screen; money and quantities in the user's locale, with English and
  German shipped;
- lists of 10,000 rows scroll, sort, filter and page without blocking;
- full keyboard operation, visible focus, and labels for every control;
- compact and expanded layouts by the client's width class;
- every screen stays current when another client changes its data (declared effects only).

**Screens.**

| Module | Screens |
|---|---|
| shell | home dashboard (open work and KPIs per module), approvals inbox across modules, global search |
| lab | sample list, sample detail with lifecycle, result entry, verification, audit trail |
| billing | price lists, invoice list, invoice editor and print view, billing runs |
| ledger | chart of accounts, journal entries with entry detail, trial balance, period close |
| inventory | stock by lot and location, requisitions and orders, goods receipt |
| org | companies and sites, users and role assignments |

The invoice editor (lines and totals from billing, the lease, approval), the goods-receipt dialog (order and new lots)
and the cross-module inbox are the deliberate tests of hypothesis 9; the print view is laid out as a document.

## Expected strain points

Each row is a hypothesis about the framework, checked by reading `include/morph` and `docs/spec` at `b2036a271` (by
the author and an independent review). None is reproduced yet. Each names its two instances, the evidence it rests on,
and what would refute it. The outcome — confirmed, refuted, or mis-framed — is recorded in the lims README's resolved
decisions; a confirmed one gets a framework spec under `docs/spec/` and its implementation before its second instance
is called done.

| # | Hypothesis | Instances | Read in | Refuted by |
|---|---|---|---|---|
| 1 | **No atomic work across models.** Each `execute` takes its own pooled connection and commits alone, models are not ordered against each other, and an awaiting handler holds its own model's gate; there is no way to commit billing's state change and ledger's entry together. Gap-free numbering is part of it: a number assigned in billing's transaction is gap-free only if the entry commits with it. | invoice posting; goods receipt | `IMPLEMENTATION.md` rule 4, `core/backend.md`, `core/coroutines.md`, `LADDER.md` | Both postings expressed with existing seams, and a failure injected between the halves leaves no partial state and no hole in either series |
| 2 | **Delivering events to a model consumer needs hand-rolled code per consumer.** `OutboxRelay` delivers at least once to an `IActionLog` sink, and `IReplayLedger` dedups by op id; neither delivers to another model. | analysis published → billable line; result captured → consumption | `journal/journal.md`, `offline/offline.md` | Both consumers receive each event exactly once in effect, across a restart, with only wiring code in `lims/app` and no per-consumer delivery loop or cursor |
| 3 | **No lease that reaches a model.** `IEnvelopeService::connectionClosed` and shared-instance attach counts exist, but no hook tells a model that the connection holding its lease closed. | draft invoice edit lease; lot reservation | `core/remote.hpp`, `core/shared_instances.md` | Both leases lapse on disconnect through an existing hook, without a timer of the model's own |
| 4 | **No periodic trigger and no resume after restart.** The internal-client and submit → poll patterns in `LADDER.md` cover a job someone starts; `TimeoutScheduler` is a detail type; nothing starts work on a schedule or resumes it after a restart. | billing run. Approval escalation is not an instance: the hypothesis is mis-framed for it, since escalation is a deadline the approval rule reads when an approver acts or lists, so nothing starts at the deadline or resumes after a restart; what it leaves is an effects question — an approvals inbox is not told when a deadline passes | `LADDER.md`, `core/timeout_scheduler.hpp` | The billing run scheduled and resumable with public primitives and no per-module scheduler |
| 5 | **No long-running approval state.** `forms::flows` are wizards inside one session; approvals last days, across principals, with limits, deadlines and escalation. | invoice approval; requisition approval | `forms/workflows_navigation.md` | Both lifecycles expressed as ordinary actions sharing no code but the declared authorization |
| 6 | **Authorization cannot see the payload.** `IAuthorizer::authorize(ctx, modelType, actionType)` decides before the payload is decoded, so a scope, a value limit or a separation-of-duties rule can only be checked inside each model, by hand. | lab's capture and verify; billing's create and approve; procurement's limits | `session/session.hpp` (`authorize`); lims decision 16 | A declared form of rule checked once by the framework for every module, or shown to be domain logic the models must own |
| 7 | **Effects leak at row and company granularity.** `fanOut` re-authorizes each watcher, but at model and action level only, and `record::privateKeys` hides keys from non-writers; a principal of company A can still learn that company B's record changed. | invoices; stock | `core/remote.hpp` (`fanOut`), `core/records.hpp` | A company-B change reaching no company-A connection, in a two-company remote test |
| 8 | **Money at ERP magnitude and with runtime currency.** `Rational` saturates and logs unless `checked*` is used; a `Quantity` unit is a compile-time tag, and `UnitTraits::convert` takes no date, so a currency known only from the record and a dated rate do not fit `Quantity`. | trial balance; invoices and orders in their party's currency | `util/rational.md`, `util/quantity_type.md` | A trial balance over 10⁷ lines of up to 10⁹ minor units each (totals within int64) summing exactly, with an over-range sum refused rather than saturated; and runtime currencies handled with `Quantity` as it stands |
| 9 | **ui/1 has no action spanning several models, and no document layout.** Downloads (`downloadToFile`), charts, display text and `sum`/`count` exist. | invoice editor; goods-receipt dialog; invoice print view | `ui/author.hpp`, `ui/document.md`, `ui/expressions.md` | Both dialogs and the print view built in ui/1 as it stands, meeting the production criteria |
| 10 | **Lists and reports run over rows already in memory.** `table::Query` pages with limits (1,000 rows a page, offset 10⁷, totals counted to 100,000), but `apply` runs over rows the model has loaded, and no total above the cap is shown; a report that totals a large table (a trial balance) likewise loads every row it adds up. | journal entries and the trial balance over them; stock movements | `table/query.hpp`, `table/engine.md` | A list over 10⁶ database rows meeting the production criteria without loading them, with its total shown; and a trial balance over 10⁷ posted lines answering exactly without loading them all |

## Delivery

One initiative branch and one pull request, with commits grouped per part:

1. Part A in the order given; the mockups start alongside it.
2. Part B: ledger, then billing.
3. Part C.
4. Part D's screens are built as each module's models land.

Each confirmed hypothesis is designed (a spec under `docs/spec/`, one independent review) and implemented in
`include/morph` in the same branch, before the part holding its second instance is called done. Library defects found
on the way are filed as `AGENTS.md` requires. The amendments to `LADDER.md` and `IMPLEMENTATION.md` (rule 2, the
promotion rule, and its stale "`DecimalPlaces >= 1` floor", which `util/quantity_type.md` contradicts by making zero
decimals legal) are made in the same branch. Implementation lanes follow the repository's lane rules: two or three at
a time, one compile at a time.

## Definition of done

- Every module follows `IMPLEMENTATION.md` rules 1, 3, 4 and 5, and rule 2 as amended for lims.
- The module boundary is a link boundary, and the compile check that includes one module from another fails to
  build.
- Models are tested through the dual-mode rig, including a remote run with several clients for posting, numbering,
  leases and reservation races.
- Failure-injection tests for both postings and the billing run show no partial state and no hole in any number
  series.
- Every hypothesis has a recorded outcome; each confirmed one has its framework spec and implementation merged, and
  its second instance uses it.
- Every screen meets the production criteria and matches its mockup in side-by-side grabs on Qt Quick, and renders on
  the terminal frontend.
- The lims README, `LADDER.md` and `IMPLEMENTATION.md` describe the rung as it now is.

## Out of scope

PDF and print-file rendering; a tax engine and statutory reports; payments, credit notes and supplier invoices;
stock counts and reorder jobs; payroll and HR; bank and payment integration; electronic invoicing; data migration from
other systems; WebAssembly for lims; high availability and clustering of the server.

## Decisions

| Decision | Choice | Why |
|---|---|---|
| Where the ERP lives | lims grows | The ladder keeps one growing rung; a second large rung would repeat the cut that left three. |
| Module boundaries | One library per module with its own include path; only `org` and `posting` shared | Composition of separated domain knowledge is the property under test, and a link boundary makes a violation a compile error. |
| Who owns posting | Billing and inventory map their documents to a `PostingRequest`; ledger validates and records it; the atomic step between them is hypothesis 1 | Keeps the domain mapping in the module that knows the document and leaves the ledger domain-free. |
| Module cooperation | Outbox events for reactions; an atomic step for posting | They are different guarantees; conflating them would hide hypothesis 1 or serialize everything. |
| Database | One SQLite file, tables prefixed per module | Makes a cross-model transaction physically possible, so a gap found is morph's. |
| Promotion | Mechanism, and needed by two lims modules | `AGENTS.md`'s second-place signal, with modules as the places; `IMPLEMENTATION.md` amended to match. |
| Scope | Smallest set that gives every hypothesis its instances (two each, one for hypothesis 4) | Features that repeat a covered instance add scaffolding and find nothing. |
| Money | `Quantity` over `Rational`, poster rounds half-up per line with an explicit rounding line, one zero-decimal currency | Tests morph's exactness claim; the ledger stays a validator that never adjusts. |
| Reservation | Per sample | Keeps decision 19; cross-sample atomicity, its reopen condition, is not needed. |
| Order | Org first, beginning with a behaviour-free split | Every later record and action carries a scope; the split proves the move changed nothing. |
| GUI | ui/1 only, mockups first, gallery mockups adapted for the lab screens | The point is the declarative UI's limit; a hand-built screen measures the screen, not ui/1. |
| Document output | Not in morph | Rendering a document is domain output. |

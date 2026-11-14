# lims ERP Part D: The Production GUI Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build lims's GUI to the standard of a commercial product — Part D of `docs/superpowers/specs/2026-11-09-lims-erp-design.md` — as ui/1 documents written with the C++ authoring API and served by the lims server, one screen per committed mockup in `examples/lims/design/mockups/`, each meeting the production criteria and matching its mockup in side-by-side grabs; and, on the way, measure hypotheses 9 and 10 and build in morph every vocabulary or frontend change a mockup needs that ui/1 cannot say.

**Architecture:** The screens stay in `examples/lims/screens/`, one source file per module (`org_screens.cpp`, `lab_screens.cpp`, `billing_screens.cpp`, `ledger_screens.cpp`, `inventory_screens.cpp`, `shell_screens.cpp`) over a small kit (`screens/kit/`: the width class, the page head, money, dates, people's names, status words, refusal banners) and message catalogues kept per module (`screens/i18n/<locale>/<module>.json`, merged into the bundle's `i18n/en.json` and `i18n/de.json`), registered by `lims::screens::bundle()` through one `add<Module>Screens` function per module with each catalog entry's module and group. Every screen reads and writes only through the models' actions and stays current through declared effects. Where a mockup asks for something ui/1 cannot say, the change is made in `include/morph` (and the frontends under `src/`), specified in `docs/spec/`, reviewed once, tested in `tests/`, built through the Doxygen docs target, and the screen then uses it; no screen carries a workaround. Every framework change this plan already knows of is a task of its own (Tasks 2's helpers, 4, 4a, 6, 8–12, 17 and 20), separate from the lims tasks that use it; a gap found while building a screen is added to the framework task its subject belongs to (or a new framework task in the same branch) before the screen is called done.

**Tech Stack:** C++23, CMake ≥ 3.25 (Ninja), morph (header-only) with its ui/1 authoring API (`morph/ui/author.hpp`), Qt 6 Quick (the reference frontend), the terminal frontend, Catch2, the examples' screen harness (`examples/common/testkit/screen_harness.hpp`), headless Firefox for the mockups' grabs, Doxygen for the docs build.

**README decision numbers.** The lims README holds decisions up to 37 (Part B Task 13, the lease). Part B Tasks 14, 15, 16 and 17 record decisions **38, 39, 40 and 41** (posting an invoice; cross-module effects; hypothesis 7's first instance; hypothesis 6 resolved) — this numbering overrides the numbers the Part B plan's own task text gives them (37–40). Part C's decisions are 42–49; Part D's start at **50**. Every reference to a Part B decision in this plan uses the 38–41 numbers.

## Where Part D stands, what waits, and what never runs together

Part D's task 1, the mockups, is done (commit `327a3a480`). Part B Task 14 is in flight on the branch (its files — `billing_test_support.hpp`, `test_declared_authorization.cpp`, `declared_requirements.hpp`, the billing and ledger models and DTOs — are modified and uncommitted) and Part B Task 16 is in flight in a worktree. A framework task and the lims task that uses it are separate rows, each with its own "waits for".

| Part D task | Half | Waits for | Never in flight with |
|---|---|---|---|
| 1. Mockups | — | — | **done** (`327a3a480`) |
| 2. Screen kit, catalogues, scenario, grabs | framework (`ui::widthClass`, `ui::byWidth`) and new lims screen files | nothing — **the next lane to open** | the other framework tasks (4, 4a, 6, 8–12, 20) |
| 3. People's names | lims (org) | Part B Task 14 committed (it edits `declared_requirements.hpp`, `test_declared_authorization.cpp`) | Part C Tasks 1 and 2 (`role_dto.hpp`, `org_schema.cpp`, `org_model.cpp`); Part B Task 19 |
| 4. Refusals in words: framework | framework | Task 2 (both edit `author.hpp`) | **Part B Task 18 — Task 4 lands before it starts** (both edit `registry.hpp`'s failure path and `bridge.hpp`'s `localOp`/`localTaskOp`), or after Part B Task 19; Task 20 (`registry.hpp`); the other framework tasks |
| 4a. Catalogues and locale formats reach the client (closes #1098) | framework (`ui/locale_table.hpp`, `expression.hpp`'s `money`, `Session::setLocale`, `catalogue.hpp`, `navigator.hpp`, `src/client/shell.cpp`, `client/source.hpp`) and `test_lims_kit.cpp` | Tasks 2 and 4 (the interpreter, `expression.hpp`, `catalogue.md`, `expressions.md`); **lands before Tasks 6, 7, 12 and 13** | the other framework tasks (2, 4, 6, 8–12, 20); Task 3 (`test_lims_kit.cpp`) |
| 5. Refusals in words: lims | lims | Tasks 2, 3, 4; Part B Task 14 committed (`invoice_edit_model.cpp`, `invoice_approval.cpp`) | Part B Task 19 |
| 6. Application shell: framework | framework | Task 4a (`navigator.hpp`, `src/client/shell.cpp`, `client/client.md`; the principal menu switches the locale 4a makes visible) | the other framework tasks |
| 7. Application shell: lims | lims | Tasks 2, 3, 4a, 5, 6; Part B Task 14 committed (`invoice_model.hpp`, `ledger_model.hpp`); Part C Task 2 (inventory's count and inbox source) | Part C Task 2 (`org_model.cpp`); Part B Task 19 |
| 8. Per-record decimals in an input | framework | Task 4 | the other framework tasks |
| 9. A step's second line | framework | Task 8 | the other framework tasks |
| 10. Bars in a chart | framework | Task 9 | the other framework tasks |
| 11. A table row that spans rows | framework | Task 10 | the other framework tasks |
| 12. A document page, its regions, a locale scope | framework | Task 11; Task 4a (`localeScope` takes its facts from the locale table) | the other framework tasks |
| 13. Org screens (31, 32) | lims | Tasks 2, 3, 4a, 5, 7, 8; Part C Task 2 (a limit per role) | Part C Task 2; Part B Task 19 |
| 14. Lab screens (14–18) | lims | Tasks 2, 5, 7, 9, and 18 (`ListSamples` moves first; Task 17's refuted verdict where Task 18 is skipped); Part C Task 5 (`applyCapture`) | Part C Task 5; Task 18; Part B Task 19 |
| 15. Billing screens (19–21, 23); hypothesis 9, first instance | lims | Tasks 5, 7, 8, 9; Part B Task 14; Part B Task 19, or Part B Task 17's no-seam branch (it edits `billing_authorization.*` and the declared tables) | Part B Task 19 |
| 16. Invoice print view (22); hypothesis 9, document instance | lims | Tasks 12, 15 | Part B Task 19 |
| 17. Hypothesis 10 on the ledger, and database pages | framework (`include/morph/table`) and one new lims test | nothing | Task 18 (it follows) |
| 18. Ledger reads in the database; `ListSamples` on the same plan | lims | Task 17 (only if it built the seam); Part B Task 14 committed (`entry_dto.hpp`, `ledger_model.hpp`, `ledger_entries.cpp`) | Task 14 (`ListSamples`); Part B Task 19 |
| 19. Ledger screens (24–27) | lims | Tasks 5, 7, 18 | Part B Task 19 |
| 20. The deadline effect | framework | Task 4 (both edit `registry.hpp`) | Part C Task 8's scoped fan-out and connection-lease seam implementations (`remote.hpp`'s fan-out); the other framework tasks |
| 21. Shell screens (11–13), and lims telling an inbox of a deadline | lims | Tasks 7, 10, 14, 15, 19, 20; Part C Tasks 1 and 2 | Part B Task 19 |
| 22. Inventory screens (28–30); hypotheses 9 and 10, second instances | lims | Tasks 9, 11, 17 (and 18 if it ran); Part C Tasks 1–6; Part C Task 8's connection-lease and runtime-units seams implemented, or recorded in decision 49 as none | Part B Task 19; Part C Task 8's seam implementations |
| 23. Part D's definition of done | — | everything above | — |

"Part B Task 19" in the last column applies only if Part B Task 17 takes the seam branch (the action guard): Task 19 rewrites every lims model `.cpp` and the declared tables, so while it runs no task that edits a lims model, a lims DTO or a declared table is in flight. If Task 17 takes the no-seam branch, Tasks 18–19 do not exist and that column's entry drops.

**Schedule.** Three lanes at most, one compile at a time. This is the same schedule Part C's plan states; Lane B is the Part B lane.

| Wave | Lane B (Part B) | Lane C | Lane D |
|---|---|---|---|
| 1 — now | B14 (on the branch) and B16 (in a worktree), both in flight | — | **D2** (the next lane to open) |
| 2 — B14 and D2 landed | B15 → B17 (spec only) | D3 → C1 → C2 → C3 | D4 (lands before B18 starts) → D4a (after D3 has landed: both edit `test_lims_kit.cpp`) → D6 → D5 |
| 3 | B18 (after D4) → B19 (a barrier: see above) | C7 (needs B16; a new test and the README) → D17 → D20 | D8 → D9 → D10 → D11 → D12, serially |
| 4 — B19 landed | B20 | C4 → C5 → C6 | D7 → D13 → D15 → D16 |
| 5 | — | C8 (specs and plans), then its seam plans | D18 → D19 → D14 (after C5) |
| 6 | — | C9 | D21 → D22 (after C8's lease and units seams) → D23 |

**Pairs that are never in flight together:** C5 ‖ D14 (`applyCapture`, `CaptureConcentration`); C1 ‖ D3 and C2 ‖ D3 (`role_dto.hpp`, `org_schema.cpp`, `org_model.cpp`); C2 ‖ D7 and C2 ‖ D13 (`org_model.cpp`); D3 ‖ D4a (`test_lims_kit.cpp`); D4 ‖ B18 (`registry.hpp`, `bridge.hpp`: D4 lands first); B19 ‖ any task that edits a lims model, DTO or declared table (C1–C6, D3, D5, D7, D13–D16, D18, D19, D21, D22); D14 ‖ D18 (`ListSamples`); D20 ‖ Part C Task 8's fan-out and lease seam implementations (`remote.hpp`); D4 ‖ D20 (`registry.hpp`); any two of D2, D4, D4a, D6, D8–D12 and D20 (`include/morph/ui`, `include/morph/core`, the interpreter, `src/client/shell.cpp`, both frontends; D4a and D6 both edit `navigator.hpp`, `src/client/shell.cpp` and `client/client.md`, D4a, D4 and D12 the interpreter and `expression.hpp`, so D4a runs strictly between D4 and D6). D17 changes only `include/morph/table` and may run beside the widget tasks.

## Global Constraints

Copied from the design, `examples/IMPLEMENTATION.md`, `AGENTS.md`, the mockups' README and the user's standing rules for ui/1 work. Every task's requirements include this section.

- **Part D (design, verbatim):** "lims's GUI is built to the standard of a commercial product, to find where the declarative UI stops. Its audience is still morph's regression suite, not a customer: it is production-quality because the limit being looked for is where production quality cannot be reached. `IMPLEMENTATION.md` rule 2 and `LADDER.md`'s 'no rung is a showcase' are amended to exempt lims's look and to say why; bookmarks and bank keep minimalism."
- **Constraints (design, verbatim):** "Every screen is a ui/1 document written with the C++ authoring API and served by the lims server. There is no hand-written QML and no per-screen client code. Where a design asks for something ui/1 cannot say, the fix is a vocabulary or frontend change in morph, not a screen workaround." "Mockups first … Each screen is compared to its mockup by side-by-side grabs." "Qt Quick on the desktop is the reference frontend. The terminal frontend must render every screen usably and is not judged on look."
- **Production criteria (design, verbatim), applied to every screen:** "loading, empty and error states, and a refused action explains itself in words, never an exception text; no raw ids, enum names or unformatted numbers on screen; money and quantities in the user's locale, with English and German shipped; lists of 10,000 rows scroll, sort, filter and page without blocking; full keyboard operation, visible focus, and labels for every control; compact and expanded layouts by the client's width class; every screen stays current when another client changes its data (declared effects only)."
- **Screens (design):** shell — home dashboard, approvals inbox across modules, global search; lab — sample list, sample detail with lifecycle, result entry, verification, audit trail; billing — price lists, invoice list, invoice editor and print view, billing runs; ledger — chart of accounts, journal entries with entry detail, trial balance, period close; inventory — stock by lot and location, requisitions and orders, goods receipt; org — companies and sites, users and role assignments. "The invoice editor (lines and totals from billing, the lease, approval), the goods-receipt dialog (order and new lots) and the cross-module inbox are the deliberate tests of hypothesis 10; the print view is laid out as a document." (The design's table numbers these as hypothesis 9's instances; its sentence says 10. Hypothesis 9 is "ui/1 has no action spanning several models, and no document layout", whose instances the table names as exactly these; this plan measures them as hypothesis 9, corrects the design's sentence in Task 2 and records the slip in decision 50.)
- **Hypotheses Part D measures (design table, "Hypothesis" / "Refuted by"):**
  9. "**ui/1 has no action spanning several models, and no document layout.** Downloads (`downloadToFile`), charts, display text and `sum`/`count` exist." / "Both dialogs and the print view built in ui/1 as it stands, meeting the production criteria". Instances: invoice editor (Task 15); goods-receipt dialog (Task 22); invoice print view (Tasks 12 and 16).
  10. "**Lists and reports run over rows already in memory.** `table::Query` pages with limits (1,000 rows a page, offset 10⁷, totals counted to 100,000), but `apply` runs over rows the model has loaded, and no total above the cap is shown; a report that totals a large table (a trial balance) likewise loads every row it adds up." / "A list over 10⁶ database rows meeting the production criteria without loading them, with its total shown; and a trial balance over 10⁷ posted lines answering exactly without loading them all". Instances: journal entries and the trial balance over them (Tasks 17–18); stock movements (Task 22).
- **What Parts B and C hand to Part D:** a JPY price's form advertises two decimals (decision 33), as a yen order's does (decision 44); an approvals inbox is not told when a deadline passes (decisions 35, 43; filed against `include/morph`); a lease's or a reservation's lapse on disconnect is not told to a waiting clerk or bench (decisions 37, 47). Part D meets each: the per-record decimals of an input are Task 8, the frontend half of hypothesis 8, built on `morph::forms::InstanceConstraints` (Part C Task 8's runtime-units spec, if written, names Task 8 as its frontend half and does not specify it again); the deadline effect is Task 20; the told lapse is Part C Task 8's connection-lease seam, which Task 22 waits for when it was specified, and which Task 22 adds as a framework task of its own in this branch when Part C Task 8 recorded none.
- **ui/1 work (user, standing):** "ideally we want to be able to generate these designs using declarative ui" — classify every visual difference by the framework change needed to express the mockup; prefer a vocabulary or frontend change over a screen workaround, and make a screen fix only where the vocabulary already expresses the design. A difference found by a comparison that needs a framework addition is designed and implemented in the same branch, never parked; only a genuinely environmental difference (a font not installed on this machine) stays out. Verify by side-by-side grabs (`scripts/shoot_lims.sh`, which runs `morph_lims_client --grab-size WxH --grab f.png` and refuses a client older than the screens' sources) against the mockups rendered with headless Firefox; Build lims rebuilds `morph_lims_client` first.
- **Decide, don't ask (user, standing):** no design questions to the user; take the recommended answer, record it in the task's README decision, and go on.
- **Rule 2 (IMPLEMENTATION.md):** "Screens never say what to refresh. Every action that changes data declares its effects … The rung's bundle loads with no refusal and no warning." `docs/KEEPING-SCREENS-CURRENT.md`'s checklist for every action a screen adds or changes.
- **Rule 5 (IMPLEMENTATION.md):** screen tests are part of the suite; every screen is mounted by the render smoke at both width classes.
- **AGENTS.md:** comments and specs state what the code does now and why (no history, no issue numbers); a defect in `include/morph`/`src` gets an issue (verification status, real output, what would change the verdict; no `triage:` label by hand); "The *same* example defect in a second place — File it against `include/morph`"; "Ask whether the check would still pass if the feature did nothing … Mutate the feature and confirm the check fails." Doxygen runs with `WARN_AS_ERROR = FAIL_ON_WARNINGS`: every public symbol a framework task adds keeps complete `@param`/`@tparam`/`@return`, and every framework task runs the Docs build below.
- **Design review (user):** every spec under `docs/spec/` that this plan writes or changes gets one independent review subagent on the default model (never Fable) before it is implemented.
- **Lessons Part B paid for (user):** a screen that lists a filtered set relies on its actions declaring `changes`, not `updates`, when a row can newly enter or leave the set; a screen test that waits for a list to change after another client's action is the check. Qt keyword identifiers (`slots`, `signals`, `emit`) never name anything in `include/morph` (Qt TUs break); GCC `-Wshadow`; clang-format at 119 columns; Clang's doc warnings over headers checked through a TU that includes them.
- **Lane rules (user):** every build/configure is wrapped in `flock /tmp/claude-1000/-home-yaraslau-repo-morph/b1b73e49-c41d-4e27-bb68-b8c0cfacf145/scratchpad/morph-build.lock`, builds at `-j10` at most; tests under the same lock with `timeout 300`. Implementation lanes run on Sonnet; two or three at a time, one compile at a time; builds and tests in the foreground. Commit as the current git user with the session's attribution trailer. Another lane codes on the same branch: rebase before each commit and keep the change to the files the task names.

## Build and test commands

- Configure: `flock /tmp/claude-1000/-home-yaraslau-repo-morph/b1b73e49-c41d-4e27-bb68-b8c0cfacf145/scratchpad/morph-build.lock cmake --preset linux-everything`
- Build lims: `flock /tmp/claude-1000/-home-yaraslau-repo-morph/b1b73e49-c41d-4e27-bb68-b8c0cfacf145/scratchpad/morph-build.lock cmake --build build/linux-everything --target ladder_lims_tests ladder_lims_screen_tests ladder_lims_render_smoke ladder_lims_server morph_lims_client -j10` (`morph_lims_client` is the client `scripts/shoot_lims.sh` grabs with; `morph_examples_client` cannot open lims)
- Model suite: `flock /tmp/claude-1000/-home-yaraslau-repo-morph/b1b73e49-c41d-4e27-bb68-b8c0cfacf145/scratchpad/morph-build.lock timeout 300 build/linux-everything/examples/lims/ladder_lims_tests "[<tag>]"`
- Framework suite (every framework task): `flock /tmp/claude-1000/-home-yaraslau-repo-morph/b1b73e49-c41d-4e27-bb68-b8c0cfacf145/scratchpad/morph-build.lock cmake --build build/linux-everything --target morph_tests -j10` and `flock /tmp/claude-1000/-home-yaraslau-repo-morph/b1b73e49-c41d-4e27-bb68-b8c0cfacf145/scratchpad/morph-build.lock timeout 300 build/linux-everything/tests/morph_tests "[<tag>]"` (the target and path are `tests/CMakeLists.txt`'s; use what it names if they differ). A change under `src/` (a frontend) also builds the Qt Quick and terminal frontends' own suites, which `tests/CMakeLists.txt` names.
- Docs build (every framework task, after its last header change): `flock /tmp/claude-1000/-home-yaraslau-repo-morph/b1b73e49-c41d-4e27-bb68-b8c0cfacf145/scratchpad/morph-build.lock cmake -S . -B build/docs -G Ninja -DMORPH_BUILD_DOCUMENTATION=ON -DMORPH_BUILD_TESTS=OFF -DMORPH_BUILD_EXAMPLES=OFF` then `flock /tmp/claude-1000/-home-yaraslau-repo-morph/b1b73e49-c41d-4e27-bb68-b8c0cfacf145/scratchpad/morph-build.lock cmake --build build/docs --target doc`. Expected: the target builds with no warning (CI runs it with `WARN_AS_ERROR = FAIL_ON_WARNINGS`). Beside it, the headers a task changed are compiled through a clang TU that includes them, so Clang's `-Wdocumentation` sees them too (GCC does not).
- Screen suite: `flock /tmp/claude-1000/-home-yaraslau-repo-morph/b1b73e49-c41d-4e27-bb68-b8c0cfacf145/scratchpad/morph-build.lock env QT_QPA_PLATFORM=offscreen timeout 300 build/linux-everything/ladder_lims_screen_tests`
- Render smoke: `flock /tmp/claude-1000/-home-yaraslau-repo-morph/b1b73e49-c41d-4e27-bb68-b8c0cfacf145/scratchpad/morph-build.lock env QT_QPA_PLATFORM=offscreen timeout 300 build/linux-everything/ladder_lims_render_smoke`
- Everything labelled for the rung: `flock /tmp/claude-1000/-home-yaraslau-repo-morph/b1b73e49-c41d-4e27-bb68-b8c0cfacf145/scratchpad/morph-build.lock ctest --test-dir build/linux-everything -j6 -L ladder-lims --output-on-failure`
- Grabs (Task 2's script): `flock /tmp/claude-1000/-home-yaraslau-repo-morph/b1b73e49-c41d-4e27-bb68-b8c0cfacf145/scratchpad/morph-build.lock scripts/shoot_lims.sh /tmp/claude-1000/-home-yaraslau-repo-morph/b1b73e49-c41d-4e27-bb68-b8c0cfacf145/scratchpad/lims-grabs <screen id>…`

## Conventions every task uses

- **Screen ids** are constants in the module's header, `screens/<module>_screens.hpp`, named `lims.<module>.<screen>`: `lims.home`, `lims.approvals`, `lims.search` (shell); `lims.lab.samples`, `lims.lab.sample`, `lims.lab.result-entry`, `lims.lab.verification`, `lims.lab.audit`; `lims.billing.price-lists`, `lims.billing.invoices`, `lims.billing.invoice`, `lims.billing.invoice-print`, `lims.billing.runs`; `lims.ledger.accounts`, `lims.ledger.journal`, `lims.ledger.trial-balance`, `lims.ledger.period-close`; `lims.inventory.stock`, `lims.inventory.requisitions`, `lims.inventory.goods-receipt`; the org screens keep `lims.org.companies` and `lims.org.access` (in `lims_screens.hpp`, where they are). `kSample` (`lims.sample`, the sample-entry working copy), `kIgnition` and `kLogin` stay as they are; `kRegister`, `kLifecycle` and `kResults` are replaced by the lab screens in Task 14, and their tests move onto those screens before they are removed. `lims_screens.hpp` includes every module header.
- **Catalog entries** carry `module` (`shell`, `lab`, `billing`, `ledger`, `inventory`, `org`), `group` (the module's name in words, a catalogue key) and `icon`, so the navigator groups by module; titles are catalogue references (`{"t": "lims.<id>.title", "fallback": "…"}`).
- **One file per module**, `screens/<module>_screens.hpp`/`.cpp`, each declaring a `ui::Screen <name>Screen()` per screen with a doc comment naming its mockup, a `std::vector<std::string_view> <module>ScreenIds()` listing its ids in registration order, and `void add<Module>Screens(ui::ScreenRegistry&, std::vector<ListedScreen>&)` (`addLabScreens`, `addBillingScreens`, …), which `bundle()` calls once per module in a fixed order (legacy screens, org, lab, billing, ledger, inventory, shell). Task 2 creates every module's files with an empty `add<Module>Screens` and an empty id list, so a screen task edits only its own module's files.
- **Shared append points, one section per module.** So that screen tasks in separate worktrees merge without conflict, no screen task edits a list another task also edits:
  - *Scenario:* `screens/scenario/scenario.cpp` holds `seedScenario()`, which calls `seedOrgScenario()`, `seedLabScenario()`, `seedBillingScenario()`, `seedLedgerScenario()`, `seedInventoryScenario()` in that order; each is defined in its own file, `screens/scenario/<module>_scenario.cpp`, and Task 2 creates all five (org's with the people and companies, the others empty). A task adds records only to its module's file and its ids' accessors (`scenarioDraftInvoice()`, …) only to `screens/scenario/<module>_scenario.hpp`.
  - *Catalogues:* one file per module and locale, `screens/i18n/<locale>/<module>.json` (`kit`, `org`, `lab`, `billing`, `ledger`, `inventory`, `shell`), each a flat JSON object with keys sorted; a key lives in the file of the module whose words it is (`lims.status.InvoiceState.*` in `billing.json`; `lims.module.*`, `lims.credit`, `lims.loading` and the refusal class sentences in `kit.json`). `addCatalogues` merges each locale's files into the bundle's one `i18n/<locale>.json` and refuses a key found in two files. The kit's catalogue case checks every file pair.
  - *Grabs:* `scripts/shoot_lims.sh` reads its rows from `examples/lims/screens/grabs/<module>.tsv` (screen id, mockup file, scenario person, expanded height, compact height, mockup height), one file per module; a task edits only its module's file.
  - *Render smoke:* its expected list is `lims::screens::allScreenIds()`, the concatenation of the modules' id lists in `bundle()`'s order (Task 2), so no screen task edits the smoke.
  - *README:* each decision is inserted at its number's place.
- **The kit** (`screens/kit/`, Task 2) is used for the width class, the page head, money, dates, quantities, people's names, status words and refusals; a screen does not format a number, a date or an enum by hand.
- **Words, not ids.** A record is named on screen by what a person knows it by — an invoice by its number or "Draft for <client>", a sample by its reference, a lot by its number, an account by its code and name, a person by their display name (Task 3: `kit::personName` over `ListUsers` on screen; `OrgModel::displayNameOf` where a refusal names someone) — never by a database id or a login; an enum is shown through the kit's status words (catalogue keys), never by its glaze name.
- **Catalogue patterns are MessageFormat 2** (`docs/spec/ui/catalogue.md`): a variable is `{$name}`, and `{name}` is literal text. Every pattern and every `t` fallback in this plan uses `{$…}`.
- **Test ids** are the screen's nouns in camelCase (`invoiceList`, `leaseBanner`, `postButton`); every control has an accessible name (`.a11y(...)`) and every input a label.
- **Screen tests** are `screens/tests/test_lims_<module>_screens.cpp`, tagged `[lims-screens][<module>]`, over `ScreenHarness` in `RigMode::Local` with `LocalSetup{.db = {}, .user = …}`, at both width classes where the layout differs (the harness's last constructor argument), and start from `screens/tests/screen_test_support.hpp` (Task 2: `limsApp()`, `reportProblems`, `prop`). Each case that pins a behaviour names its mutation in a `// Mutation:` comment, and the step that adds it runs that mutation once.
- **Compact layouts** follow the mockups' README: collapsible sections with a summary header where an expanded screen shows several panels side by side, and a pinned footer carrying the primary action (and a total, where the screen has one). Each screen task's Step 1 names which of its screens need them; a missing word is added to a framework task.
- **Grabs.** Every screen task ends with its screens' grabs at 1215 × the mockup's height (expanded) and 390 × the mockup's height (compact), beside the mockup's own grab, and a list of differences in `/tmp/claude-1000/-home-yaraslau-repo-morph/b1b73e49-c41d-4e27-bb68-b8c0cfacf145/scratchpad/lims-visual-review.md` (scratch, not committed), each classified as screen (the vocabulary says it and the screen did not), framework (ui/1 cannot say it: added to a framework task now), domain (the mockup shows something lims's models deliberately do not do: recorded in the task's decision), or environment. A screen task is done when its list holds only domain and environment items.
- **README decisions** for Part D are 50–62, reserved per task: 50 (Task 2), 51 (3), 52 (5), 53 (7), 54 (13), 55 (14), 56 (15), 57 (16), 58 (17, amended by 18), 59 (19), 60 (21), 61 (22), 62 (23). Framework-only tasks record no README decision: their spec under `docs/spec/` is their record, and the lims task that uses them names them in its decision. **Migrations** Part D adds take ids from `20261130000001` in this order: `20261130000001` (Task 3, people's names), `20261130000002` (Task 16, a billing client's address and locale).

## File structure after Part D

```
examples/lims/screens/
  lims_screens.hpp / .cpp              legacy ids, prepareLocal, bundle(), allScreenIds(), addCatalogues()
  kit/kit.hpp, kit/kit.cpp             width class, page head, money, dates, quantities, names, status words, refusals
  i18n/<locale>/<module>.json          en and de; kit, org, lab, billing, ledger, inventory, shell
  scenario/scenario.hpp / .cpp         seedScenario(), calling one function per module
  scenario/<module>_scenario.hpp/.cpp  the mockups' records of one module, and their ids' accessors
  grabs/<module>.tsv                   shoot_lims.sh's rows, one file per module
  shell_parts.cpp                      lims's ShellParts declaration
  org_screens.hpp / .cpp               31-Companies, 32-Users
  lab_screens.hpp / .cpp               14-Samples, 15-SampleDetail, 16-ResultEntry, 17-Verification, 18-Audit
  billing_screens.hpp / .cpp           19-PriceLists, 20-Invoices, 21-InvoiceEditor, 22-InvoicePrint, 23-BillingRuns
  ledger_screens.hpp / .cpp            24-Accounts, 25-Journal, 26-TrialBalance, 27-PeriodClose
  inventory_screens.hpp / .cpp         28-Stock, 29-Requisitions, 30-GoodsReceipt
  shell_screens.hpp / .cpp             11-Home, 12-Approvals, 13-Search
  tests/screen_test_support.hpp        limsApp(), reportProblems(), prop()
  tests/test_lims_<module>_screens.cpp one per module
  render_smoke/test_lims_render.cpp    every screen at both widths
include/morph/ui/                      widthClass/byWidth, refusalText, shell parts, bound decimals, Step::detail,
                                       chart bars, row spans, page/pageRegion/localeScope
include/morph/core/                    action_refusal.hpp, the err envelope's refusal, changesAt
include/morph/table/                   plan() and exact totals (Task 17, if hypothesis 10 is confirmed)
docs/spec/                             each addition specified and reviewed
scripts/shoot_lims.sh                  lims's grabs beside the mockups'
```

---

### Task 1: Mockups (done)

Done in commit `327a3a480` ("examples(lims): mockups for the production GUI"): `examples/lims/design/mockups/Shell.dc.html` and `11-Home` … `32-Users`, with `README.md` holding the scenario, the design language and the list of what ui/1 may not express. Every later task reads its mockups and that list first. Nothing to do.

---
### Task 2: The screen kit, the catalogues, the scenario and the grabs

**Files:**
- Create: `examples/lims/screens/kit/kit.hpp`, `examples/lims/screens/kit/kit.cpp`, `examples/lims/screens/i18n/en/{kit,org,lab,billing,ledger,inventory,shell}.json` and the same seven under `i18n/de/`, `examples/lims/screens/scenario/scenario.hpp`/`.cpp` and `scenario/{org,lab,billing,ledger,inventory}_scenario.hpp`/`.cpp`, `examples/lims/screens/grabs/{org,lab,billing,ledger,inventory,shell}.tsv`, `examples/lims/screens/{lab,billing,ledger,inventory,shell}_screens.hpp`/`.cpp` (each with an empty `add<Module>Screens` and id list), `examples/lims/screens/tests/screen_test_support.hpp`, `examples/lims/screens/tests/test_lims_kit.cpp`, `scripts/shoot_lims.sh`, `tests/test_ui_width_class.cpp`
- Modify: `include/morph/ui/author.hpp` (`widthClass`, `byWidth`), `docs/spec/ui/author.md` (the two helpers), `examples/lims_gallery/src/screens/screen_layout.hpp` (delegates to them), `examples/lims/screens/lims_screens.hpp`/`.cpp` (`ListedScreen`, `bundle()` calling each module's `add<Module>Screens`, `allScreenIds()`, `addCatalogues`; `prepareLocal` seeds the scenario on request), `examples/lims/screens/org_screens.hpp`/`.cpp` (`addOrgScreens`, `orgScreenIds`), `examples/lims/screens/render_smoke/test_lims_render.cpp` (its list is `allScreenIds()`), `examples/lims/CMakeLists.txt` (`LIMS_I18N_DIR`, the catalogues' embedding), `examples/IMPLEMENTATION.md` (rule 2's exemption for lims), `examples/LADDER.md` ("no rung is a showcase"), `docs/superpowers/specs/2026-11-09-lims-erp-design.md` (Part D's sentence says hypothesis 9), `examples/lims/README.md` (decision 50)

**Interfaces:**
- Consumes: `morph::ui::author::Scope`, `env`, `coalesce`, `switchOn`, `Case`, `t`, `call`, `badge`, `banner`, `morph::ui::BundleFile`, `morph::ui::UiRegistry::addFile`, `glz::reflect`, the lims models' actions (through a `Bridge` over `LocalBackend` with each scenario person's session, as `prepareLocal` dispatches its bootstrap `AssignRole`).
- Produces: `morph::ui::author::AnyHandle morph::ui::author::widthClass(Scope&)` (the client's `{"env": "widthClass"}`, `expanded` where none is set, as a `let` named `widthClass`); `Node morph::ui::author::byWidth(AnyHandle const& width, Nodes compact, Nodes const& expanded)`; in `lims::screens::kit`: `ui::AnyHandle width(ui::Scope&)`, `void pageHead(ui::Scope&, std::string_view module, ui::AnyHandle const& title)`, `ui::Handle<std::string> money(ui::AnyHandle const& amount, ui::AnyHandle const& currency)`, `ui::Handle<std::string> balance(ui::AnyHandle const& amount, ui::AnyHandle const& currency)` (a credit as `13,196.40 Cr`), `ui::Handle<std::string> day(ui::AnyHandle const& instant)`, `ui::Handle<std::string> measure(ui::AnyHandle const& quantity, ui::AnyHandle const& unit)`, `template <typename E> ui::Handle<std::string> statusWord(ui::AnyHandle const& value)`, `template <typename E> ui::Node statusBadge(ui::AnyHandle const& value, std::map<std::string_view, std::string_view> const& tones)`, `ui::Node errorBanner(ui::AnyHandle const& message)` (Task 5 replaces it with a refusal banner); in `lims::screens`: `struct ListedScreen { std::string id; std::string module; ui::Handle<std::string> title; std::expected<ui::RegisteredScreen const*, morph::ui::Refusal> registered; bool isPublic = false; }`, `void add<Module>Screens(ui::ScreenRegistry&, std::vector<ListedScreen>&)` per module, `std::vector<std::string_view> <module>ScreenIds()` per module, `std::vector<std::string> allScreenIds()`, `std::expected<void, morph::ui::Refusal> addCatalogues(morph::ui::UiRegistry&)`, `void seedScenario()` (the mockups' scenario, idempotent, one function per module, grown by each screen task in its own module's file); `LIMS_SCENARIO=1` makes `prepareLocal` call it; in `screens/tests/screen_test_support.hpp` (namespace `lims::screens::test`): `morph::client::LocalApp limsApp()`, `void reportProblems(ScreenHarness const&)`, `std::string prop(ScreenHarness&, std::string_view kind, std::string_view testId, std::string_view property)`.

The kit exists because every screen formats the same things the same way, and the production criteria forbid a raw id, an enum name or an unformatted number on any of them. The width-class helpers are the examples' second copy of the same code — `examples/lims_gallery/src/screens/screen_layout.hpp` holds the first — so `AGENTS.md`'s rule promotes them into morph rather than copying them a second time.

- [ ] **Step 1: Write the failing tests**

Create `tests/test_ui_width_class.cpp` (the framework suite's file list is globbed or named in `tests/CMakeLists.txt`; add it there if named):

```cpp
// SPDX-License-Identifier: Apache-2.0
//
// The width-class helpers: a screen reads the client's width class, `expanded` where none is set, and switches its
// layout between the compact one and the expanded one.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <morph/ui/author.hpp>
#include <string>

namespace ui = morph::ui::author;

// Mutation: drop the `expanded` fallback from widthClass (the document's let reads only the environment).
TEST_CASE("ui width class: a screen's width class falls back to expanded and switches its layout", "[ui][author]") {
    auto const screen = ui::screen("probe", [](ui::Scope& scope) {
        auto const width = ui::widthClass(scope);
        return ui::byWidth(width, {ui::text({.text = "compact"}).testId("compact")},
                           {ui::text({.text = "expanded"}).testId("expanded")});
    });
    ui::ScreenRegistry registry;
    auto const added = registry.add(screen);
    REQUIRE(added);
    std::string const& document = (*added)->text;
    CHECK_THAT(document, Catch::Matchers::ContainsSubstring(R"("widthClass")"));
    CHECK_THAT(document, Catch::Matchers::ContainsSubstring(R"("expanded")"));
    CHECK_THAT(document, Catch::Matchers::ContainsSubstring(R"("switch")"));
}
```

`ScreenRegistry::add` answers the registered screen, whose `text` is the document `bundle()` serves, as `lims_screens.cpp` reads it.

Create `examples/lims/screens/tests/test_lims_kit.cpp`, mounting a probe bundle the way `test_lims_approval_inbox.cpp`
mounts its one-screen bundle:

```cpp
// SPDX-License-Identifier: Apache-2.0
//
// The screen kit: money, balances, days and states in words, in English and German, and catalogues that agree.

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <expected>
#include <fstream>
#include <glaze/glaze.hpp>
#include <map>
#include <memory>
#include <morph/ui/author.hpp>
#include <morph/ui/service.hpp>
#include <sstream>
#include <string>
#include <string_view>
#include <testkit/db_fixture.hpp>
#include <testkit/screen_harness.hpp>

#include "kit/kit.hpp"
#include "lims/billing/core/types.hpp"

namespace {

namespace ui = morph::ui::author;
namespace kit = lims::screens::kit;
using morph::examples::testing::RigMode;
using morph::examples::testing::ScreenHarness;

constexpr std::string_view kProbe = "test.kit";

/// @brief One screen showing the kit's five formats and one catalogue message with an argument over literals, and a
///        button that switches to German.
/// @return The bundle, with lims's two catalogues, or the refusal.
std::expected<std::shared_ptr<morph::ui::ServedBundle const>, morph::ui::Refusal> kitBundle() {
    ui::ScreenRegistry screens;
    auto registered = screens.add(ui::screen(std::string{kProbe}, [](ui::Scope& screen) {
        static_cast<void>(screen);
        return ui::column({
            ui::text({.text = kit::money(ui::AnyHandle{morph::ui::Decimal{"13196.40"}}, "EUR")}).testId("eur"),
            ui::text({.text = kit::money(ui::AnyHandle{morph::ui::Decimal{"1234567"}}, "JPY")}).testId("jpy"),
            ui::text({.text = kit::balance(ui::AnyHandle{morph::ui::Decimal{"-13196.40"}}, "EUR")}).testId("credit"),
            ui::text({.text = kit::day(ui::AnyHandle{"2026-11-09T09:00:00Z"})}).testId("day"),
            ui::text({.text = kit::statusWord<lims::InvoiceState>(ui::AnyHandle{"Submitted"})}).testId("state"),
            ui::text({.text = ui::t("lims.billing.draftFor", "Draft for {$client}",
                                    {{"client", ui::AnyHandle{"Stadtwerke Köln"}}})})
                .testId("argument"),
            ui::button({.label = "Deutsch", .onClick = ui::setLocale("de")}).testId("german"),
        });
    }));
    if (!registered) {
        return std::unexpected(registered.error());
    }
    morph::ui::UiRegistry served{morph::ui::ApplicationInfo{.id = "lims-kit-test", .version = "1.0"},
                                 screens.catalog()};
    morph::ui::CatalogEntry entry;
    entry.id = std::string{kProbe};
    entry.title = "Kit";
    if (auto added = served.addScreen(std::move(entry), (*registered)->text); !added) {
        return std::unexpected(std::move(added.error()));
    }
    if (auto added = lims::screens::addCatalogues(served); !added) {
        return std::unexpected(std::move(added.error()));
    }
    return served.build();
}

/// @brief The modules whose catalogue files the screens keep, one file per module and locale.
constexpr std::array<std::string_view, 7> kCatalogueModules{"kit", "org", "lab", "billing", "ledger", "inventory", "shell"};

/// @brief One module's catalogue in one locale, read from the source tree.
/// @param locale `en` or `de`.
/// @param module One of `kCatalogueModules`.
/// @return Its keys and messages.
std::map<std::string, std::string> catalogue(std::string_view locale, std::string_view module) {
    std::ifstream file{std::string{LIMS_I18N_DIR} + "/" + std::string{locale} + "/" + std::string{module} + ".json"};
    std::stringstream bytes;
    bytes << file.rdbuf();
    std::map<std::string, std::string> messages;
    REQUIRE_FALSE(glz::read_json(messages, bytes.str()));
    return messages;
}

}  // namespace

// Mutations: make statusWord show the enumerator's name (the English state reads "Submitted"); write
// `lims.billing.draftFor`'s pattern as `Draft for {client}` (MessageFormat 2 reads it as literal text, and the
// argument case reads "Draft for {client}").
TEST_CASE("lims kit: money, balances, days and states in words, in English and German", "[lims-screens][kit]") {
    morph::ladder::testkit::DbFixture fixture;
    ScreenHarness harness{morph::client::LocalApp{.name = "lims-kit", .bundle = &kitBundle, .setUp = {}},
                          std::string{kProbe},
                          RigMode::Local,
                          {},
                          morph::client::LocalSetup{.db = {}, .user = "tech"}};
    CHECK(harness.waitForText("eur", "€13,196.40"));
    CHECK(harness.waitForText("jpy", "¥1,234,567"));
    CHECK(harness.waitForText("credit", "€13,196.40 Cr"));
    CHECK(harness.waitForText("day", "09-Nov-2026"));
    CHECK(harness.waitForText("state", "Waiting for approval"));
    CHECK(harness.waitForText("argument", "Draft for Stadtwerke Köln"));
    harness.click("german");
    CHECK(harness.waitForText("eur", "13.196,40 €"));
    CHECK(harness.waitForText("jpy", "1.234.567 ¥"));
    CHECK(harness.waitForText("credit", "13.196,40 € H"));
    CHECK(harness.waitForText("day", "09.11.2026"));
    CHECK(harness.waitForText("state", "Wartet auf Freigabe"));
    CHECK(harness.waitForText("argument", "Entwurf für Stadtwerke Köln"));
}

// Mutations: delete one key from de/org.json (the case names it); copy one key of kit.json into org.json (the
// duplicate check names it).
TEST_CASE("lims kit: each module's catalogues hold the same keys in both locales, and no key is in two modules",
          "[lims-screens][kit]") {
    std::map<std::string, std::string> owner;
    for (std::string_view const module : kCatalogueModules) {
        auto const english = catalogue("en", module);
        auto const german = catalogue("de", module);
        for (auto const& [key, message] : english) {
            INFO("missing from de/" << module << ".json: " << key);
            CHECK(german.contains(key));
            INFO(key << " is in both " << owner[key] << ".json and " << module << ".json");
            CHECK((owner[key].empty() || owner[key] == module));
            owner[key] = std::string{module};
        }
        for (auto const& [key, message] : german) {
            INFO("missing from en/" << module << ".json: " << key);
            CHECK(english.contains(key));
        }
    }
}
```

The literal forms (`morph::ui::Decimal{"…"}` for an exact decimal, an ISO instant as text) are the author API's for a constant; read `include/morph/ui/value.hpp` and use the constructors it declares if these differ. The expected strings are the mockups' (README, "Money", "Dates"). Where `money`'s or `date`'s locale formatting answers a different but correct form — a no-break space before `€`, or `date`'s `medium` style in English answering `Nov 9, 2026` — assert what it answers and add the difference to the grabs' review list as framework (a `date` style that gives the mockups' `09-Nov-2026`), never pad the expectation. The argument case's German words come from `de/billing.json`'s `lims.billing.draftFor` ("Entwurf für {$client}"), which Task 2 adds with the English one; Task 15 uses the key. `LIMS_I18N_DIR` is set for the screen-test target in `examples/lims/CMakeLists.txt`, after `morph_add_rung`: `if(TARGET ladder_lims_screen_tests) target_compile_definitions(ladder_lims_screen_tests PRIVATE LIMS_I18N_DIR="${CMAKE_CURRENT_SOURCE_DIR}/screens/i18n") endif()`.

- [ ] **Step 2: Run them and see them fail**

Run: the framework suite build, then Build lims. Expected: `widthClass` and `byWidth` are not members of `morph::ui::author`; `kit/kit.hpp` not found.

- [ ] **Step 3: Promote the width-class helpers**

In `include/morph/ui/author.hpp`, beside `switchOn`:

```cpp
/// @brief The width class a screen lays out for: the client's `{"env": "widthClass"}`, and `expanded` where no
///        client sets it.
///
/// A screen draws a compact layout below 80 units and the expanded one otherwise; `medium` takes the expanded one
/// unless the screen switches on it itself.
/// @param screen The screen's scope.
/// @return The width class, a `let` named `widthClass`.
[[nodiscard]] inline AnyHandle widthClass(Scope& screen) {
    return screen.let("widthClass", coalesce({env("widthClass"), AnyHandle{"expanded"}}));
}

/// @brief The compact layout when @p width is `compact`, the expanded one otherwise.
/// @param width The width class, as `widthClass` answers it.
/// @param compact The compact layout.
/// @param expanded The expanded layout.
/// @return The switch.
[[nodiscard]] inline Node byWidth(AnyHandle const& width, Nodes compact, Nodes const& expanded) {
    return switchOn(width, {Case{"compact", std::move(compact)}}, expanded);
}
```

Place it after `Scope`, `env`, `coalesce`, `switchOn` and `Case` are declared. Add a "Width class" paragraph to `docs/spec/ui/author.md` naming both and the reason (`medium` takes the expanded layout because the designs draw two). Change the gallery's `layout::widthClass` and `layout::byWidth` to call these (one line each) and keep their names, so the gallery's screens are unchanged. This is a framework change; dispatch the one independent review (default model) on the author.md paragraph and the header change before committing. Then run the Docs build (Build and test commands) and see it finish with no warning.

- [ ] **Step 4: The kit**

`screens/kit/kit.hpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <glaze/glaze.hpp>
#include <map>
#include <morph/ui/author.hpp>
#include <string>
#include <string_view>

/// @file
/// What every lims screen formats the same way: the width class, the page head, money, balances, days, quantities,
/// states in words and refusals. No screen formats a number, a date or an enumerator by hand.

namespace lims::screens::kit {

namespace ui = morph::ui::author;

/// @brief The width class the screen lays out for.
/// @param screen The screen.
/// @return The width class.
[[nodiscard]] inline ui::AnyHandle width(ui::Scope& screen) { return ui::widthClass(screen); }

/// @brief The page head every screen owns: the module's name over the title.
/// @param screen The screen.
/// @param module The module's id: `lab`, `billing`, `ledger`, `inventory`, `org` or `shell`.
/// @param title The title.
void pageHead(ui::Scope& screen, std::string_view module, ui::AnyHandle const& title);

/// @brief @p amount in @p currency, in the session's locale, at the currency's minor digits.
/// @param amount The amount.
/// @param currency The currency's code.
/// @return The text: `€13,196.40`, `¥1,234,567`.
[[nodiscard]] inline ui::Handle<std::string> money(ui::AnyHandle const& amount, ui::AnyHandle const& currency) {
    return ui::call<std::string>("money", {amount, currency});
}

/// @brief A balance: a debit as money, a credit (a negative amount) as its magnitude followed by the credit mark.
/// @param amount The balance, debit positive.
/// @param currency The currency's code.
/// @return The text: `€13,196.40` or `€13,196.40 Cr`.
[[nodiscard]] ui::Handle<std::string> balance(ui::AnyHandle const& amount, ui::AnyHandle const& currency);

/// @brief The day of @p instant, as the screens show a date.
/// @param instant The instant.
/// @return The text, in the session's locale.
[[nodiscard]] inline ui::Handle<std::string> day(ui::AnyHandle const& instant) {
    return ui::call<std::string>("date", {instant, ui::AnyHandle{"medium"}});
}

/// @brief @p quantity followed by @p unit's symbol, in the session's locale.
/// @param quantity The quantity.
/// @param unit The unit's symbol.
/// @return The text: `0.250 L`.
[[nodiscard]] inline ui::Handle<std::string> measure(ui::AnyHandle const& quantity, ui::AnyHandle const& unit) {
    return ui::call<std::string>("quantity", {quantity, unit});
}

/// @brief The words for @p value, an enumerator of @p E by its glaze name: the catalogue's
///        `lims.status.<E>.<name>`, with the name split into words as the fallback.
/// @tparam E A reflected enumeration.
/// @param value The enumerator's name, as the record carries it.
/// @return The words.
template <typename E>
[[nodiscard]] ui::Handle<std::string> statusWord(ui::AnyHandle const& value);

/// @brief A badge with @p value's words, in the tone @p tones names for it (neutral otherwise).
/// @tparam E A reflected enumeration.
/// @param value The enumerator's name.
/// @param tones Tone per enumerator name: `ok`, `warn`, `err`, `info`.
/// @return The badge.
template <typename E>
[[nodiscard]] ui::Node statusBadge(ui::AnyHandle const& value, std::map<std::string_view, std::string_view> const& tones);

/// @brief An error banner over @p message, shown only while it is engaged.
/// @param message The text.
/// @return The banner.
[[nodiscard]] ui::Node errorBanner(ui::AnyHandle const& message);

}  // namespace lims::screens::kit
```

`statusWord` builds, from `glz::reflect<E>::keys`, one `ui::cond(ui::eq(value, name), ui::t("lims.status.<E>.<name>", <name in words>), …)` chain ending in the value itself; the type's name for the key is a `static constexpr std::string_view` the caller's enumeration gives through a small trait `kit::StatusName<E>` (`InvoiceState` → `"InvoiceState"`), specialised in `kit.hpp` for each enumeration a screen shows, because `glz::name_v` is compiler-dependent. `statusBadge` is `ui::badge(statusWord<E>(value), <the same chain over tones>)`. `balance` is `ui::cond(ui::lt(amount, 0), ui::concat({money(ui::abs(amount), currency), ui::AnyHandle{" "}, ui::t("lims.credit", "Cr")}), money(amount, currency))`. `pageHead` sets `screen.eyebrow(ui::t("lims.module." + module, …))` and `screen.title(title)`; the company in the eyebrow ("Billing · Rheinlab Analytik GmbH") is added by Task 7 once the shell's context exists.

`i18n/en/<module>.json` and `i18n/de/<module>.json`, one pair per module (`kit`, `org`, `lab`, `billing`, `ledger`, `inventory`, `shell`): flat objects with sorted keys, each holding the keys of its module's words (Conventions). `kit.json` starts with the six modules (`lims.module.lab` "Laboratory" / "Labor", `lims.module.billing` "Billing" / "Fakturierung", `lims.module.ledger` "Ledger" / "Hauptbuch", `lims.module.inventory` "Inventory" / "Lager", `lims.module.org` "Organisation" / "Organisation", `lims.module.shell` "Home" / "Start"), `lims.credit` ("Cr" / "H") and `lims.loading` ("Loading…" / "Wird geladen…"); `billing.json` starts with `lims.status.InvoiceState.*` (`lims.status.InvoiceState.Submitted` "Waiting for approval" / "Wartet auf Freigabe", …) and `lims.billing.draftFor` ("Draft for {$client}" / "Entwurf für {$client}"); each other module's file starts as `{}` with its group's name (`lims.group.lab` …). Each later task adds its keys to its own module's pair, sorted; the kit's catalogue case fails for a key in one locale and not the other, or in two modules.

`lims_screens.hpp` declares `std::expected<void, morph::ui::Refusal> addCatalogues(morph::ui::UiRegistry& served)`, which `bundle()` calls after the screens (and the kit's probe bundle calls too); per locale it reads the seven module files into one `std::map<std::string, std::string>` (refusing, as a `morph::ui::Refusal` naming the key and both files, a key found in two of them), writes the map back with `glz::write_json`, and adds it with `served.addFile(morph::ui::BundleFile{.kind = "messages", .id = "i18n/en.json", .bytes = <the merged bytes>})` and the same for `de`; the bundle's catalogue id stays `i18n/<locale>.json`, which `checkCatalogue` requires. The module files' bytes are embedded at build time — read each file with `#embed` if the toolchain has it, else from a generated header the rung's CMake writes (`file(READ …)` into a `constexpr std::string_view`); look at how `examples/` embeds a file elsewhere (`grep -rn 'embed\|file(READ' examples/*/CMakeLists.txt cmake/`) and follow it.

- [ ] **Step 5: The scenario, and the grabs**

`screens/scenario/scenario.hpp`/`.cpp`: `void seedScenario()` does nothing when a company named "Rheinlab Analytik GmbH" exists, and otherwise calls `seedOrgScenario()`, `seedLabScenario()`, `seedBillingScenario()`, `seedLedgerScenario()` and `seedInventoryScenario()` in that order, each declared in `scenario/<module>_scenario.hpp` and defined in its own `scenario/<module>_scenario.cpp`. Each writes its module's part of the mockups' scenario (README, "Scenario") through the models, each record by the person the scenario names, dispatched through a `Bridge` over a `LocalBackend` with that person's session (as `prepareLocal` dispatches its `AssignRole`). `seedOrgScenario()` writes Rheinlab Analytik GmbH (EUR, fiscal year from January, VAT 19 %, sites Köln and Leverkusen) and Rheinlab Japan K.K. (JPY, April, Osaka), and the people by their logins — `anna` (Dr. Anna Meyer), `manpreet` (Manpreet Kaur), `lena` (Lena Fischer), `markus` (Markus Weber), `julia` (Julia Schneider), `tobias` (Tobias Krüger), `kenji` (Kenji Sato) — with their scoped roles and the invoice approver limits; the display names are Task 3's and the requisition approver limit Part C Task 2's, each added to `org_scenario.cpp` by the task that makes it possible. The other four files are empty functions now; each screen task fills its own module's file (samples for Task 14, invoices for Task 15, and so on), so the grabs show the mockups' figures. `prepareLocal` calls `seedScenario()` when `LIMS_SCENARIO` is `1` (read once, with the same `NOLINT(concurrency-mt-unsafe)` comment as the server's `environment`).

The screens' structure the later tasks append to: `lims_screens.hpp` declares `ListedScreen` (Interfaces) and `allScreenIds()`; `bundle()` calls `addLegacyScreens` (the screens `lims_screens.cpp` holds today: register, lifecycle, results, sample, ignition, sign-in), then `addOrgScreens`, `addLabScreens`, `addBillingScreens`, `addLedgerScreens`, `addInventoryScreens`, `addShellScreens`, then registers every listed screen's catalog entry (module, group, title) and calls `addCatalogues`. Task 2 moves the two org screens' registration into `addOrgScreens` (`org_screens.cpp`) and creates `lab_screens.hpp`/`.cpp`, `billing_screens.hpp`/`.cpp`, `ledger_screens.hpp`/`.cpp`, `inventory_screens.hpp`/`.cpp` and `shell_screens.hpp`/`.cpp`, each with its `add<Module>Screens` adding nothing and its `<module>ScreenIds()` empty. The render smoke's expected list becomes `lims::screens::allScreenIds()` (the ids the modules list, in `bundle()`'s order), so a screen dropped from `bundle()` while its module still lists it fails the smoke, as before, and no screen task edits the smoke.

`screens/tests/screen_test_support.hpp`: `limsApp()` (`LocalApp{.name = "lims", .bundle = &lims::screens::bundle, .setUp = {}}`), `reportProblems` (each of `harness.problems()` as an `UNSCOPED_INFO`) and `prop` (`GalleryScreen::prop`'s two lines over `harness.find` and `harness.backend().prop`, answering the property as text), each with a doc comment. Every screen test includes it instead of defining its own.

`scripts/shoot_lims.sh OUT_DIR [CLIENT] [SCREEN…]` (executable, the shape of `scripts/shoot_screens.sh`): for each lims screen id given (all of them by default), in a fresh scratch directory, with `LIMS_SCENARIO=1`, run `QT_QPA_PLATFORM=offscreen "$client" --ui=qt --app lims --user <the scenario person the mockup shows> --screen <id> --db "$scratch/app.db" --grab-size 1215x<h> --grab "$OUT/<id>-expanded.png"` and the same at `390x<h>` for `-compact.png`; and render the mockup with `firefox --headless --window-size=1215,<H> --screenshot "$OUT/<mockup>.png" "file://$repo/examples/lims/design/mockups/<mockup>.dc.html"`, where `<h>` and `<H>` come from the rows of `examples/lims/screens/grabs/*.tsv` (tab-separated: screen id, mockup file, person, expanded height, compact height, mockup height), one file per module. `grabs/org.tsv` holds the two org rows now; the other files start empty, and each screen task adds its rows to its module's file. It stops at the first shot that fails, naming it.

- [ ] **Step 6: The rule's exemption for lims, and the design's word**

In `examples/IMPLEMENTATION.md` rule 2, after its zero-styling clause: "lims is exempt from the zero-styling clause, and only lims: its GUI is built to a commercial product's standard against committed mockups (`examples/lims/design/mockups/`), because the limit it looks for is where the declarative UI stops reaching production quality, which a minimal screen never meets. Its look is still a ui/1 document with no hand-written QML and no per-screen client code; bookmarks and bank keep minimalism." In `examples/LADDER.md`'s Audience paragraph: "No rung is a showcase: `IMPLEMENTATION.md` rule 2's zero-styling rule applies to bookmarks and bank; lims is held to its mockups instead, for the reason rule 2 gives." In the design (`docs/superpowers/specs/2026-11-09-lims-erp-design.md`, Part D, the sentence ending "are the deliberate tests of hypothesis 10"), change "hypothesis 10" to "hypothesis 9", which its own hypothesis table names (`AGENTS.md`: the spec is updated, not worked around).

- [ ] **Step 7: Build, run, record decision 50 and commit**

Run: the framework suite `"[ui]"`, the Docs build, Build lims, Screen suite `"[kit]"`, Render smoke. Expected: all pass, the Docs build with no warning; run each mutation once and restore it. Then `scripts/shoot_lims.sh /tmp/claude-1000/-home-yaraslau-repo-morph/b1b73e49-c41d-4e27-bb68-b8c0cfacf145/scratchpad/lims-grabs lims.org.companies` and open the two grabs: the script works end to end.

```markdown
### 50. lims's screens are held to committed mockups through a kit, two catalogues and side-by-side grabs

Every screen formats money, balances, days, quantities and states through `screens/kit/`, so no screen shows a raw
id, an enum name or an unformatted number; English and German catalogues, kept per module and merged into the bundle, must hold the same keys. The width-class helpers are morph's (`ui::widthClass`, `ui::byWidth`): the gallery held the first copy and lims
needed the second. `LIMS_SCENARIO=1` seeds the mockups' scenario through the models, and `scripts/shoot_lims.sh`
grabs each screen at both widths beside its mockup; each module's screens, scenario, catalogue and grabs live in
files of their own, so screen tasks merge without touching each other's lists. The design names the invoice editor, the goods-receipt dialog and
the print view as hypothesis 9's instances in its table and calls them hypothesis 10's in its Part D text; they are
hypothesis 9's (an action spanning several models, a document layout), and hypothesis 10 is the large lists; the
design's sentence now says 9.
`IMPLEMENTATION.md` rule 2 exempts lims's look, and only lims's.
```

```bash
cd /home/yaraslau/repo/morph
git add include/morph/ui/author.hpp docs/spec/ui/author.md tests/test_ui_width_class.cpp tests/CMakeLists.txt \
        examples/lims_gallery/src/screens/screen_layout.hpp examples/lims/screens scripts/shoot_lims.sh \
        examples/IMPLEMENTATION.md examples/LADDER.md examples/lims/README.md examples/lims/CMakeLists.txt \
        docs/superpowers/specs/2026-11-09-lims-erp-design.md
git commit -m "Give lims's screens a kit, English and German catalogues, a seeded scenario and side-by-side grabs

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01No1NznqxEBvjhZBaBRLCd6"
```

---

### Task 3: People's names

**Waits for** Part B Task 14 committed (this task edits `declared_requirements.hpp` and `test_declared_authorization.cpp`, which Task 14 holds uncommitted). **Never in flight with** Part C Tasks 1 and 2 (they edit `role_dto.hpp`, `org_schema.cpp` and `org_model.cpp`) or Part B Task 19.

**Files:**
- Modify: `examples/lims/org/include/lims/org/dto/role_dto.hpp` (`UserView::displayName`, `SetDisplayName`, `GetSignedInUser`), `examples/lims/org/include/lims/org/db/org_entity.hpp` (`PersonRecord`), `examples/lims/org/src/db/org_schema.cpp` (migration `20261130000001`), `examples/lims/org/include/lims/org/models/org_model.hpp` and `examples/lims/org/src/models/org_model.cpp` (`SetDisplayName`, `GetSignedInUser`, `ListUsers` carries the name, `displayNameOf`), `examples/lims/app/include/lims/app/declared_requirements.hpp` (`SetDisplayName` in `openMutatingActions()`), `examples/lims/tests/test_declared_authorization.cpp` (the coverage case), `examples/lims/tests/test_org_roles.cpp`, `examples/lims/screens/kit/kit.hpp`/`.cpp` (`personName`), `examples/lims/screens/tests/test_lims_kit.cpp`, `examples/lims/screens/scenario/org_scenario.cpp` (the seven names), `examples/lims/README.md` (decision 51)

**Interfaces:**
- Consumes: `OrgModel` (`ListUsers`, `ListRoleAssignments`, `visibleCompanies`), `requirePrincipal`, `ValidationError`, `Forbidden`, the kit, `screen_test_support.hpp`.
- Produces: `UserView::displayName` (`std::string`, empty when none is set); `SetDisplayName { std::string principal; std::string displayName; }` → `UserView` (mutating, `changes<UserView>`); `GetSignedInUser {}` → `UserView` (a read: the caller's own view); `static std::string OrgModel::displayNameOf(std::string_view principal)` (the display name, or the principal itself when none is set); `ui::Handle<std::string> kit::personName(ui::AnyHandle const& users, ui::AnyHandle const& principal)` (the display name from a `ListUsers` result, the login where the person has none).

No lims record holds a person's name: a principal is a login (`UserView{id, roles}`), and the mockups name every person ("Dr. Anna Meyer", "Markus Weber is editing", "Julia Schneider is the last administrator"). The production criteria's "words, not ids" holds for people as for records, so org — which owns principals — gains a display name per principal. It is org's domain, not morph's: morph's session knows a principal and nothing else.

A screen names a person by mapping a principal through `ListUsers` (`kit::personName`), so no view across the modules grows a name field per person it mentions; a refusal names a person by the name the server resolves when it builds the refusal (`OrgModel::displayNameOf`, Task 5), because a refusal reaches a screen as words for a code with arguments, not as a record a screen can map.

- [ ] **Step 1: Write the failing tests**

Append to `examples/lims/tests/test_org_roles.cpp`:

```cpp
// Mutation: answer UserView without its displayName in ListUsers (the check for "Dr. Anna Meyer" fails).
TEST_CASE("org: a supervisor names a person, and the users list carries the name", "[lims][org]") {
    DbFixture fixture;
    lims::test::grant("sam", lims::LimsRole::Supervisor);
    lims::test::grant("anna", lims::LimsRole::Analyst);
    ScopedPrincipal const sam{"sam"};
    lims::OrgModel org;
    CHECK(org.execute(lims::SetDisplayName{.principal = "anna", .displayName = "Dr. Anna Meyer"}).displayName ==
          "Dr. Anna Meyer");
    auto const users = org.execute(lims::ListUsers{}).users;
    auto const anna = std::ranges::find(users, std::string{"anna"}, &lims::UserView::id);
    REQUIRE(anna != users.end());
    CHECK(anna->displayName == "Dr. Anna Meyer");
    CHECK(lims::OrgModel::displayNameOf("anna") == "Dr. Anna Meyer");
    CHECK(lims::OrgModel::displayNameOf("nobody") == "nobody");
}

// Mutation: drop the self-or-supervisor check from SetDisplayName (rita renames anna).
TEST_CASE("org: a person names themselves; nobody else but a supervisor of their company names them", "[lims][org]") {
    DbFixture fixture;
    lims::test::grant("anna", lims::LimsRole::Analyst);
    lims::test::grant("rita", lims::LimsRole::Analyst);
    {
        ScopedPrincipal const anna{"anna"};
        CHECK(lims::OrgModel{}.execute(lims::SetDisplayName{.principal = "anna", .displayName = "Anna Meyer"})
                  .displayName == "Anna Meyer");
        CHECK(lims::OrgModel{}.execute(lims::GetSignedInUser{}).displayName == "Anna Meyer");
    }
    ScopedPrincipal const rita{"rita"};
    CHECK_THROWS_AS(lims::OrgModel{}.execute(lims::SetDisplayName{.principal = "anna", .displayName = "Someone"}),
                    lims::Forbidden);
}
```

Use the file's own helpers (`grant`, `ScopedPrincipal`, `DbFixture`) as its existing cases do; read the top of the file first. Append to `test_lims_kit.cpp` a case over a probe screen that queries `ListUsers` and shows `kit::personName(users, "anna")` and `kit::personName(users, "kenji")` after `seedOrgScenario()` has named anna and not kenji (`"Dr. Anna Meyer"`, `"kenji"`; mutation: `personName` answers the principal always).

- [ ] **Step 2: Run them and see them fail**

Run: Build lims. Expected: `SetDisplayName`, `GetSignedInUser` and `UserView::displayName` do not exist.

- [ ] **Step 3: The table, the actions and the kit**

`PersonRecord` (`org_people`): `id`, `principal` (`SqlAnsiString<64>`), `display_name` (the string type `CompanyRecord::name` uses, so "Krüger" round-trips). `org_schema.cpp`:

```cpp
LIGHTWEIGHT_SQL_MIGRATION(20261130000001, "Give people a display name") {
    plan.CreateTableIfNotExists("org_people")
        .PrimaryKeyWithAutoIncrement("id", Bigint())
        .RequiredColumn("principal", Varchar(64))
        .RequiredColumn("display_name", Varchar(128));
    // One name per person: the index settles two first namings of one principal.
    plan.CreateUniqueIndex("idx_org_people_principal", "org_people", {"principal"});
}
```

`role_dto.hpp`: `UserView` gains `std::string displayName;` after `id` (doc: "Their display name; empty when none is set, and a screen then shows the login."); `SetDisplayName` (principal 1–64 bytes, display name 1–128 bytes, `explicitSubmit = true`, complete doc comments) and `GetSignedInUser` (always valid). `OrgModel`: `SetDisplayName` (`requirePrincipal`; validate; allowed when the caller is the person, or supervises a company in which the person holds a role — read on a mapper of its own through the assignments, as `ListRoleAssignments` does; `Forbidden` otherwise; an upsert of the person's row by principal, the unique index's refusal on a concurrent first naming turned into a re-read and update; answers the person's `UserView`), `GetSignedInUser` (the caller's view: their roles from `ListUsers`' query, their name), `ListUsers` reading the names in one query beside the assignments, and `displayNameOf`. Register:

```cpp
BRIDGE_REGISTER_ACTION(lims::OrgModel, lims::SetDisplayName, "SetDisplayName")
BRIDGE_REGISTER_EFFECTS(lims::SetDisplayName, ::morph::effects::changes<lims::UserView>)
BRIDGE_REGISTER_ACTION(lims::OrgModel, lims::GetSignedInUser, "GetSignedInUser", ::morph::model::Loggable::No)
```

`SetDisplayName`'s rule is the person or a supervisor of one of their companies — a relation between two principals that the declared table's role-and-scope shape cannot state — so it is checked in the model and listed in `openMutatingActions()` (size raised by one), with that reason added to the function's doc comment beside the existing ones; add it to the coverage case's `Mutating` tuple.

`kit::personName(users, principal)`: the `displayName` of the first row of `users` whose `id` equals `principal`, when it is not empty, and `principal` otherwise — built with the author API's `filter`, `first` and `coalesce`/`cond` (read `author.hpp` for their spelling), with a complete doc comment.

`seedOrgScenario()` names the seven people (`anna` "Dr. Anna Meyer", `manpreet` "Manpreet Kaur", `lena` "Lena Fischer", `markus` "Markus Weber", `julia` "Julia Schneider", `tobias` "Tobias Krüger", `kenji` "Kenji Sato"), each as `julia`, the company's supervisor in the scenario.

- [ ] **Step 4: Build, run, record decision 51, commit**

Run: Build lims, Model suite `"[org]"` and `"[authorization]"`, Screen suite `"[kit]"`, Everything labelled for the rung, the GCC shadow sweep (Part C's command). Expected: all pass; run each mutation once and restore it (touch the restored file).

```markdown
### 51. A person has a display name, kept by org beside the principal

A principal is a login; org keeps a display name per principal (`org_people`), set by the person or by a supervisor of
one of their companies (`SetDisplayName`), and every user view carries it. Screens name a person by mapping a
principal through the users list, and fall back to the login where no name is set; a refusal that names a person
carries the name the server resolved (`OrgModel::displayNameOf`). The rule relates two principals, which the declared
table's role and scope cannot say, so the model checks it and the action is listed as open.
```

```bash
cd /home/yaraslau/repo/morph
git add examples/lims/org examples/lims/app/include/lims/app/declared_requirements.hpp \
        examples/lims/tests/test_declared_authorization.cpp examples/lims/tests/test_org_roles.cpp \
        examples/lims/screens/kit examples/lims/screens/tests/test_lims_kit.cpp \
        examples/lims/screens/scenario/org_scenario.cpp examples/lims/README.md
git commit -m "Give lims's people a display name, kept by org beside the principal

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01No1NznqxEBvjhZBaBRLCd6"
```

---

### Task 4: Refusals in words (framework)

**Waits for** Task 2 (both edit `author.hpp`). **Lands before Part B Task 18 starts** (both edit `registry.hpp`'s failure path and `bridge.hpp`'s `localOp`/`localTaskOp`); if Task 18 has started, this task waits for Part B Task 19. Changes no file under `examples/`.

**Files:**
- Create: `docs/spec/ui/refusals.md`, `include/morph/core/action_refusal.hpp`, `tests/test_action_refusal.cpp`
- Modify: `include/morph/core/wire.hpp` (the `err` envelope's refusal), `include/morph/core/registry.hpp` (the server path carries it), `include/morph/core/bridge.hpp` (the local paths keep it), `include/morph/ui/author.hpp` (`refusal()` on mutation and query handles; `t` with a key known at run time; `refusalText`), the interpreter that evaluates a mutation's state and `t` (`docs/spec/ui/interpreter.md` names it), `docs/spec/core/wire.md`, `docs/spec/ui/document.md`, `docs/spec/ui/catalogue.md`, `docs/spec/ui/expressions.md`, `docs/spec/README.md`, `tests/CMakeLists.txt` (if it names files)

**Interfaces:**
- Consumes: `morph::wire::Envelope`, `makeErr`, `decode` (reads with `error_on_unknown_keys = false`), `ActionDispatcher`'s failure path, `Bridge::localOp`/`localTaskOp`, `ui::author::MutationHandle`, `QueryHandle`, `ui::author::t`, the catalogue's `parseMessage`.
- Produces: `struct morph::model::RefusalArg { std::string name; std::string value; }`; `class morph::model::ActionRefusal : public std::runtime_error { ActionRefusal(std::string code, std::vector<RefusalArg> args, std::string message); std::string const& code() const noexcept; std::vector<RefusalArg> const& args() const noexcept; }`; the `err` envelope's optional `refusal` member (`{code, args}`), omitted when the failure is not an `ActionRefusal`; `refusal()` on `MutationHandle` and `QueryHandle` (`null`, or `{code, args}`); `ui::author::tAt(AnyHandle const& key, std::string const& fallback, AnyHandle const& args)` (a catalogue message whose key is an expression); `ui::author::refusalText(AnyHandle const& refusal)` (the catalogue's words for the refusal's code with its arguments, else its class's sentence, else the interpreter's built-in sentence; never the message).

The production criteria say a refused action explains itself in words, never an exception text, in the user's locale. The wire carries a failure as one `message` string, so a screen has only the server's text — which an application writes for logs and tests, with action names and ids ("SubmitInvoice: invoice 7 is being edited by 'alice'") and in one language. A screen cannot word that, and the server cannot word it in the client's locale. What is missing is mechanism: a failure that carries a code and its arguments to the client, and a catalogue lookup by a key that is known only when the failure arrives. The type is named `ActionRefusal`, not `Refusal`, because `morph::ui::Refusal` already names the bundle's refusal (why a bundle or a screen was not accepted at load), a different thing a reader must not confuse with an action's.

- [ ] **Step 1: The spec, and one review**

`docs/spec/ui/refusals.md` states, present tense, with complete reasoning:

- *What a refusal is:* `morph::model::ActionRefusal`, a code (dotted, the application's: `lims.billing.invoiceLeased`), named string arguments, and a message for logs.
- *How it travels:* the server's dispatcher catches any exception deriving from `ActionRefusal` and puts `{code, args}` in the `err` envelope beside the message; the journal records the message as today; the local paths reject the completion with the exception itself, which the interpreter reads the same way. Any other exception yields an `err` with no `refusal`.
- *How a document reads it:* `refusal()` on a mutation's or a query's state; `refusalText(refusal)`.
- *The catalogue lookup by a run-time key* — the part `t` cannot do today, because `t`'s key is a literal string in the document (`author.hpp`'s `t`, `expression.hpp`'s evaluation): the `t` form accepts, in place of its literal key, an expression that evaluates to a string (`{"t": {"expr": …}, "fallback": …, "args": …}`; `tAt` in the author API). Its `args` may be a record of expressions (as today) or an expression evaluating to a list of `{name, value}` records, which becomes the message's named arguments: an argument named `holder` is the MessageFormat 2 variable `$holder`, its value a string formatted as text (no `:number` or `:date` is applied to it; a pattern that wants one names it, `{$amount :number}`). `refusalText(refusal)` is `tAt(code, <the class sentence>, args)` where the class sentence is a lookup of the code's class keys — each dotted prefix of the code, longest first, followed by `.generic` (`lims.billing.invoiceLeased` → `lims.billing.generic`, then `lims.generic`), tried in turn — and the interpreter's built-in "This was refused." when none has an entry. morph knows no prefix by name; the keys follow from the code alone. The message is never shown.
- *Load-time checks:* a literal key is checked as the bundle checks it today. A run-time key cannot be: the bundle does not know which codes a server sends. The interpreter therefore reports, once per key and session, a missing run-time key as a problem (the diagnostics a client logs and `ScreenHarness::problems()` collects), and the application covers its codes with a test of its own over its catalogues (lims's is Task 5's).
- *Wire compatibility:* the field is omitted when empty; a client that does not know it ignores it, since `decode` reads with `error_on_unknown_keys = false`.
- *Alternatives considered:* wording on the server in the client's locale (the server would need the locale per call, and the words would live in two places); a status-code enum in morph (domain words in morph); parsing the message (fragile, and in one language); a load-time list of codes per bundle (the server's codes are the application's and change without the bundle).

Index it in `docs/spec/README.md`; add a paragraph to `docs/spec/core/wire.md` (the field), `docs/spec/ui/document.md` (`refusal()`, `t` with a key expression, `refusalText`), `docs/spec/ui/catalogue.md` (run-time keys and how load-time checks treat them) and `docs/spec/ui/expressions.md` (list arguments). One independent review (default model) with `wire.hpp`, `registry.hpp`'s failure path, `bridge.hpp`'s local paths, the interpreter's mutation state and its `t` evaluation as its material; address every finding.

- [ ] **Step 2: Write the failing framework test**

`tests/test_action_refusal.cpp`, each case with its mutation:

1. A probe model whose action throws `ActionRefusal{"probe.taken", {{"holder", "Alice"}}, "taken by alice"}`, dispatched over a server rig: the reply is `err` with `refusal.code == "probe.taken"` and the argument (mutation: drop the field from the envelope).
2. The same over the local path: the completion's exception is an `ActionRefusal` with the code (mutation: rethrow as `std::runtime_error`).
3. A thrown `std::runtime_error` yields an `err` with no `refusal` (mutation: wrap every exception as a refusal with an empty code).
4. An authored probe screen whose mutation fails with the refusal shows `refusalText`'s words from a probe catalogue (`"probe.taken": "Held by {$holder}"` → "Held by Alice"), the class sentence (`"probe.generic"`) when the catalogue lacks the code, the built-in sentence when it lacks both, and never the message — on the screen harness, with a probe bundle (mutation: show `message` as the fallback).
5. The same screen with the code missing from the catalogue reports one problem naming the key (mutation: report nothing).

- [ ] **Step 3: Implement**

Implement to the spec in the files listed, every public symbol with complete `@param`/`@return`. Run: the framework suite `"[refusal]"` and `"[ui]"`, the Docs build. Expected: all pass, the Docs build with no warning; each mutation fails its case; restore.

- [ ] **Step 4: Commit**

```bash
cd /home/yaraslau/repo/morph
git add docs/spec include/morph tests src
git commit -m "Carry action refusals to the client as codes with arguments, and word them from the catalogue

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01No1NznqxEBvjhZBaBRLCd6"
```

---

### Task 4a: Catalogues and locale formats reach the client (framework)

**Waits for** Task 4 (both edit the interpreter, `expression.hpp`, `docs/spec/ui/catalogue.md` and `docs/spec/ui/expressions.md`) and Task 2 (it changes Task 2's `test_lims_kit.cpp` back to the brief's expectations, and Task 2's fix declares lims's `formatting.date`). **Lands before** Tasks 6 (both edit `navigator.hpp`, `src/client/shell.cpp` and `docs/spec/client/client.md`; Task 6's principal menu switches the locale this task makes visible), 7 (lims's shell catalogues), 12 (`localeScope` takes its facts from this task's locale table) and 13 (the first screen to assert `€10,000.00`). **Never in flight with** the other framework tasks (2, 4, 6, 8–12, 20), nor with Task 3 (both edit `test_lims_kit.cpp`). Closes #1098.

**Files:**
- Create: `docs/spec/ui/locale_table.md`, `include/morph/ui/locale_table.hpp`, `tests/test_ui_locale_table.cpp`, `tests/client/test_shell_locale.cpp`
- Modify: `include/morph/ui/expression.hpp` (`FormatContext::currency`; `money`'s third argument and its symbol forms; the function table's `money` row), `include/morph/ui/interpreter.hpp` (`Session::setLocale` installs the locale's facts; `Session::setDatePatterns` records that the application declared its patterns), `include/morph/ui/catalogue.hpp` (`catalogTitlesIn`, `catalogLabel`), `include/morph/ui/navigator.hpp` (`NavigatorOptions::label`, the headings through it, `nav_title_<id>`, `relabel()`), `include/morph/client/source.hpp` and `src/client/source.cpp` (`Source::catalogues()`), `include/morph/client/shell.hpp` (`ShellOptions::locale`, `ShellOptions::onLocale`, `ApplicationHost::setLocale`, `ApplicationHost::setLocaleSink`), `src/client/shell.cpp` (installs the catalogues and the initial locale, labels the navigator, follows a locale change and a reload), `tests/client/CMakeLists.txt` (the new file), `tests/CMakeLists.txt` (if it names files), `docs/spec/ui/catalogue.md`, `docs/spec/ui/expressions.md`, `docs/spec/ui/workspace.md`, `docs/spec/client/client.md`, `docs/spec/README.md`, `examples/lims/screens/tests/test_lims_kit.cpp` (back to the brief's expectations), and any other rung's screen test whose expected text changes from the C forms to `en`'s (fixed in place, as examples are)

**Interfaces:**
- Consumes: `ui::MessageCatalogues` (`lookup`, `provider`, `problems`, `locales`), `ui::loadCatalogues(Manifest const&, files)`, `ui::catalogTitle(entry, catalogues, locale)`, `ui::catalogTitles(entry, catalogues)`, `ui::parseMessage`, `ui::LoadedBundle` (`manifest`, `files`; held by `client::Source::_loaded`), `ui::Session` (`setLocale`, `setTranslations`, `setDatePatterns`, `formattingNow`), `ui::FormatContext`, `render::NumericLocale`, `render::formatCanonicalNumber`, `ui::currencyDigits`, `ui::InterpreterHost::setLocale` (`host.hpp` forwards a document's `setLocale` to the application host), `client::mountShell`, `ui::Navigator`, `ui::ShellFrame::navigator()`, `ScreenHarness` (`examples/common/testkit/screen_harness.hpp`, already on `morph_client_tests`' include path).
- Produces: in `include/morph/ui/locale_table.hpp` (namespace `morph::ui`): `enum class SymbolPlacement : std::uint8_t { Before, After }`; `struct CurrencySymbol { std::string_view code; std::string_view symbol; }`; `struct CurrencyFacts { SymbolPlacement placement = SymbolPlacement::After; std::span<CurrencySymbol const> symbols; }` (empty `symbols`: every currency by its code, today's form); `struct LocaleFacts { std::string_view tag; render::NumericLocale numeric; CurrencyFacts currency; std::string_view datePattern; std::string_view dateTimePattern; }`; `[[nodiscard]] std::span<LocaleFacts const> localeTable() noexcept` (`en`, `de`); `[[nodiscard]] LocaleFacts const* findLocaleFacts(std::string_view tag) noexcept` (the exact tag, then each shorter one, `de-AT` → `de`; null when none); `[[nodiscard]] LocaleFacts const& localeFacts(std::string_view tag) noexcept` (`findLocaleFacts`, else `en`'s); `enum class MoneyForm : std::uint8_t { Symbol, Code }`; `[[nodiscard]] std::string placeCurrency(std::string_view number, std::string_view currency, CurrencyFacts const& facts, render::NumericLocale const& numeric, MoneyForm form)`. `ui::FormatContext::currency` (`CurrencyFacts`, default empty). `money(x, currency, form?)` with `form` `"symbol"` (the default) or `"code"`. In `catalogue.hpp`: `[[nodiscard]] std::vector<std::string> catalogTitlesIn(CatalogEntry const& entry, MessageCatalogues const* catalogues, std::string_view locale)` (the title in @p locale first, then the other locales' titles not equal to it); `[[nodiscard]] std::string catalogLabel(MessageCatalogues const& catalogues, std::string_view text, std::string_view locale)` (@p text's message in @p locale when the catalogues hold it as a key, else @p text). `NavigatorOptions::label` (`std::function<std::string(std::string_view text)>`; empty shows the text as written); `void Navigator::relabel()`. `[[nodiscard]] ui::MessageCatalogues client::Source::catalogues() const`. `client::ShellOptions::locale` (`std::string`, the locale the shell starts in; empty is `en`) and `client::ShellOptions::onLocale` (`std::function<void(std::string const& locale)>`, called after each `setLocale`, for the client to store the choice). `void client::ApplicationHost::setLocale(std::string const& locale) override`; `void client::ApplicationHost::setLocaleSink(std::function<void(std::string const&)> sink)`.

A bundle's catalogues are served, hash-verified and listed in the manifest, but no client reads them: `mountShell` creates a `Session` and never calls `setTranslations`, so every `t` shows its fallback. `Session::setLocale` changes only `FormatContext::locale`; `numeric` stays the C spelling and `money` always writes `<number> <CODE>`, so `setLocale("de")` changes nothing a person sees. The navigator resolves titles with no catalogues (`catalogTitles(entry, nullptr)`) and draws `module` and `group` as written. Each is mechanism every application needs unchanged, so each is morph's: the shell installs what the bundle carries, and a locale table morph owns gives the facts a locale writes numbers, money and dates with. The date pattern itself is not this task's: the app document's `formatting.date` already sets it, and lims declares it in Task 2.

- [ ] **Step 1: The spec, and one review**

`docs/spec/ui/locale_table.md` states, present tense, with complete reasoning:

- *What the table holds:* per locale, its `NumericLocale` (`en`: `.` and `,`; `de`: `,` and `.`), its currency facts (where the symbol stands, and the symbols it writes: `EUR` `€`, `USD` `$`, `GBP` `£`, `JPY` `¥`, `CHF` `CHF`, in both), and its default date and date-and-time patterns (`de`: `dd.MM.yyyy` and `dd.MM.yyyy HH:mm`, the DIN 5008 numeric form; `en`: none, so ISO stays, because English has no one convention — `11/10/2026` reads two ways — and ISO is what the client writes today). The table is data in the header, compiled in, with static storage, since `NumericLocale` holds views.
- *Lookup:* `findLocaleFacts` tries the exact tag, then each shorter one (`de-AT`, then `de`), as a catalogue lookup does; `localeFacts` answers `en`'s where none matches. The shell reports a locale with no facts once, as a problem (`no formatting facts for locale `pl`; numbers, money and dates use en's`), rather than switching silently.
- *Who installs them:* `Session::setLocale(tag)` installs `localeFacts(tag)`: its `numeric`, its `currency`, and its date patterns *only while the application declared none* (`Session::setDatePatterns` records that it did; the app document's `formatting.date` is the on-screen pattern for the whole application, in every locale). A `Session` constructed alone keeps `FormatContext`'s defaults (the C spelling, money by code, ISO) until `setLocale` is called — interpreter tests and embedders that call `setFormatting` keep what they set — and the client shell always calls `setLocale` before the first mount. Task 12's `localeScope` reads the same table and states in its own spec whether a scope's locale pattern wins over the application's inside the scope (Task 16's German invoice shows `31.10.2026`).
- *Both frontends:* Qt Quick and the terminal read the session's `FormatContext`; neither consults `QLocale` or the C library's locale, so the two write the same text. `QLocale` may not override the table: two frontends that disagree on `1.234,50` make a screen test's expectation frontend-dependent.
- *`money`'s forms:* in `en` the symbol stands before the amount, `€13,196.40`, `¥1,234,567`, and a symbol ending in a letter is followed by a no-break space, `CHF 2,450.35`; in `de` it follows the amount after a no-break space, `13.196,40 €`, `1.234.567 ¥`, `2.450,35 CHF`. A negative amount's sign stands first in both, `-€13.00`, `-13,00 €`. A currency the locale lists no symbol for is written by its code, `5.00 XTS`. The code form stays available as `money(x, currency, "code")`, `13,196.40 EUR`, because an invoice line or an export names the currency unambiguously where `$` does not; `"symbol"` is the default because the mockups and the production criteria show symbols. A default `FormatContext` (no currency facts) writes today's `13196.40 EUR`, so the expression corpus's expectations hold.
- *`MMM` under `de`:* stays the English abbreviation (`Nov`), as `expressions.md`'s date patterns already state: a localized month name would read back differently per locale, and an application that wants a German month declares a numeric pattern.
- *Alternatives considered:* `QLocale`/ICU as the source (the terminal frontend has neither, and the two frontends would diverge); CLDR in full (a dependency and megabytes for two shipped locales; the table grows a row per locale an application ships); facts in the bundle (each application would restate what a locale is; it stays possible later as an override); the locale's date pattern winning over the app's (an application that regulates its date form — lims's `dd-MMM-yyyy` — would lose it under `de`).

Update `docs/spec/ui/catalogue.md` ("Switching locale": the client shell installs the provider from `Source::catalogues()` at start and after a reload that changed a catalogue, and reports `problems()`; this answers the question the spec left open of who installs it), `docs/spec/ui/expressions.md` ("Formatting": `money`'s third argument and its forms; `number` and `quantity` take the table's separators through `setLocale`), `docs/spec/ui/workspace.md` ("Navigator and pins": headings through `NavigatorOptions::label` — the group's label, or the module's when there is no group, so a module id is never drawn as written once a label is given; titles through `NavigatorOptions::titles`, the active locale's first; the title text's test id `nav_title_<id>`; `relabel()`), `docs/spec/client/client.md` ("The shell": catalogues, the initial locale from `ShellOptions::locale`, `onLocale`, the navigator's labels, relabelling on a locale change), and index `locale_table.md` in `docs/spec/README.md`. One independent review (default model) with `expression.hpp`'s `FormatContext` and `money`, `Session::setLocale`/`setDatePatterns`, `catalogue.hpp`, `navigator.hpp`, `src/client/shell.cpp`'s `start` and `buildFrame`, #1098 and `.superpowers/sdd/d-task-2-review1.md` ("Framework work this exposes") as its material; address every finding.

- [ ] **Step 2: Write the failing framework tests**

`tests/test_ui_locale_table.cpp`, tagged `[ui][locale]`, over the interpreter's `morph::ui::testing::Harness` (`tests/ui_interpreter_support.hpp`, as `tests/test_ui_catalogue.cpp` uses it):

```cpp
// SPDX-License-Identifier: Apache-2.0
//
// The locale table: which facts a locale tag finds, and what `number` and `money` write once `Session::setLocale`
// has installed them.

#include <catch2/catch_test_macros.hpp>
#include <morph/ui/locale_table.hpp>
#include <string>

#include "ui_interpreter_support.hpp"

namespace {

constexpr char const* kAmounts = R"({
  "vocab": "ui/1", "id": "amounts",
  "view": { "kind": "column", "children": [
    { "kind": "text", "testId": "eur",
      "text": { "fn": "money", "args": [{ "lit": { "num": 1319640, "den": 100, "dp": 2 } }, "EUR"] } },
    { "kind": "text", "testId": "jpy", "text": { "fn": "money", "args": [1234567, "JPY"] } },
    { "kind": "text", "testId": "chf",
      "text": { "fn": "money", "args": [{ "lit": { "num": 245035, "den": 100, "dp": 2 } }, "CHF"] } },
    { "kind": "text", "testId": "debt",
      "text": { "fn": "money", "args": [{ "lit": { "num": -1300, "den": 100, "dp": 2 } }, "EUR"] } },
    { "kind": "text", "testId": "unlisted", "text": { "fn": "money", "args": [5, "XTS"] } },
    { "kind": "text", "testId": "code",
      "text": { "fn": "money", "args": [{ "lit": { "num": 1319640, "den": 100, "dp": 2 } }, "EUR", "code"] } },
    { "kind": "text", "testId": "number",
      "text": { "fn": "number", "args": [{ "lit": { "num": 12345675, "den": 10, "dp": 1 } }] } } ] } })";

}  // namespace

// Mutation: drop the shorter-tag step from findLocaleFacts (de-AT finds nothing and falls back to en).
TEST_CASE("locale table: a tag finds its facts, then its language's, else none", "[ui][locale]") {
    REQUIRE(morph::ui::findLocaleFacts("en") != nullptr);
    CHECK(morph::ui::findLocaleFacts("en")->tag == "en");
    REQUIRE(morph::ui::findLocaleFacts("de-AT") != nullptr);
    CHECK(morph::ui::findLocaleFacts("de-AT")->tag == "de");
    CHECK(morph::ui::findLocaleFacts("pl") == nullptr);
    CHECK(morph::ui::localeFacts("pl").tag == "en");
}

// Mutations: skip the table switch in Session::setLocale (en reads 13196.40 EUR); ignore the placement in
// placeCurrency (de reads €13.196,40).
TEST_CASE("locale table: setLocale changes how numbers and money are written", "[ui][locale]") {
    morph::ui::testing::Harness h;
    auto const screen = h.mount(kAmounts);
    // A session nobody set a locale on keeps the evaluator's own forms.
    CHECK(h.text("eur") == "13196.40 EUR");

    h.session.setLocale("en");
    h.settle();
    CHECK(h.text("eur") == "€13,196.40");
    CHECK(h.text("jpy") == "¥1,234,567");
    CHECK(h.text("chf") == "CHF 2,450.35");
    CHECK(h.text("debt") == "-€13.00");
    CHECK(h.text("unlisted") == "5.00 XTS");
    CHECK(h.text("code") == "13,196.40 EUR");
    CHECK(h.text("number") == "1,234,567.5");

    h.session.setLocale("de");
    h.settle();
    CHECK(h.text("eur") == "13.196,40 €");
    CHECK(h.text("jpy") == "1.234.567 ¥");
    CHECK(h.text("chf") == "2.450,35 CHF");
    CHECK(h.text("debt") == "-13,00 €");
    CHECK(h.text("code") == "13.196,40 EUR");
    CHECK(h.text("number") == "1.234.567,5");
}

// Mutation: let setLocale install the locale's date pattern although the application declared one (the declared
// case reads 09.11.2026).
TEST_CASE("locale table: a locale's date pattern applies only where the application declared none", "[ui][locale]") {
    morph::ui::testing::Harness h;
    auto const screen = h.mount(R"({
      "vocab": "ui/1", "id": "day",
      "view": { "kind": "text", "testId": "day", "text": { "fn": "date", "args": ["2026-11-09T09:00:00Z"] } } })");
    h.session.setLocale("de");
    h.settle();
    CHECK(h.text("day") == "09.11.2026");
    h.session.setDatePatterns("dd-MMM-yyyy", "dd-MMM-yyyy HH:mm");
    h.session.setLocale("en");
    h.session.setLocale("de");
    h.settle();
    CHECK(h.text("day") == "09-Nov-2026");
}
```

The harness's `mount`, `text` and `settle` are `ui_interpreter_support.hpp`'s; use what it declares if they differ. Add the file to `tests/CMakeLists.txt` if it names files.

`tests/client/test_shell_locale.cpp`, tagged `[client][shell][locale]`, over `ScreenHarness` in `RigMode::Local` (if linking it pulls a library `morph_client_tests` lacks, mount through `client::mountShell` over `client::LocalSource` exactly as `tests/client/test_shell.cpp`'s first case does — the shell under test is the same):

```cpp
// SPDX-License-Identifier: Apache-2.0
//
// The client shell installs a bundle's message catalogues and its locale's formatting facts: a screen's `t` and
// `money` follow `setLocale`, the navigator's titles and headings are the catalogue's words in the active locale, and
// a locale with no facts is reported.

// First, ahead of every header that brings view.hpp (GCC's -Wshadow; see test_shell.cpp).
// clang-format off
#include <morph/ui/value.hpp>
// clang-format on

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <expected>
#include <memory>
#include <morph/client/source.hpp>
#include <morph/ui/service.hpp>
#include <string>
#include <string_view>
#include <testkit/screen_harness.hpp>
#include <utility>
#include <vector>

namespace ui = morph::ui;
using morph::examples::testing::RigMode;
using morph::examples::testing::ScreenHarness;

namespace {

constexpr std::string_view kScreen = R"({
  "vocab": "ui/1", "id": "probe.words",
  "view": { "kind": "column", "children": [
    { "kind": "text", "testId": "hello", "text": { "t": "probe.hello", "fallback": "Fallback" } },
    { "kind": "text", "testId": "eur",
      "text": { "fn": "money", "args": [{ "lit": { "num": 1319640, "den": 100, "dp": 2 } }, "EUR"] } },
    { "kind": "button", "testId": "german", "label": "Deutsch", "onClick": { "setLocale": "de" } },
    { "kind": "button", "testId": "polish", "label": "Polski", "onClick": { "setLocale": "pl" } } ] } })";

constexpr char const* kEn =
    R"({"probe.group.main": "Main work", "probe.hello": "Hello", "probe.words.title": "Words and amounts"})";
constexpr char const* kDe =
    R"({"probe.group.main": "Hauptarbeit", "probe.hello": "Hallo", "probe.words.title": "Wörter und Beträge"})";

/// The one screen, listed for a signed-in person under a group that is a catalogue key, with both catalogues.
/// @return It, or the refusal.
std::expected<std::shared_ptr<ui::ServedBundle const>, ui::Refusal> wordsBundle() {
    ui::UiRegistry served{ui::ApplicationInfo{.id = "probe", .version = "1"}};
    ui::CatalogEntry entry;
    entry.id = "probe.words";
    entry.title = *ui::parseJson(R"({"t": "probe.words.title", "fallback": "Words"})");
    entry.module = "probe";
    entry.group = "probe.group.main";
    if (auto added = served.addScreen(std::move(entry), std::string{kScreen}); !added) {
        return std::unexpected(std::move(added.error()));
    }
    for (auto const& [id, bytes] : {std::pair{"i18n/en.json", kEn}, std::pair{"i18n/de.json", kDe}}) {
        auto added = served.addFile(ui::BundleFile{.kind = "messages", .id = id, .bytes = bytes, .isPublic = false});
        if (!added) {
            return std::unexpected(std::move(added.error()));
        }
    }
    return served.build();
}

/// The navigator's title text for @p screenId, or empty.
std::string navTitle(ScreenHarness const& harness, std::string const& screenId) {
    return harness.text("nav_title_" + screenId).value_or("");
}

}  // namespace

// Mutations: drop the shell's setTranslations call (hello reads "Fallback"); skip the table switch in
// Session::setLocale (eur reads "13196.40 EUR"); build the navigator's titles with catalogTitles instead of
// catalogTitlesIn (the English title reads "Wörter und Beträge", de sorting first); give the navigator no label
// (the heading reads "probe.group.main"); leave relabel out of the shell's locale sink (the German heading reads
// "Main work").
TEST_CASE("Client shell: a bundle's catalogues and the locale's formats reach the screen and the navigator",
          "[client][shell][locale]") {
    ScreenHarness harness{morph::client::LocalApp{.name = "probe", .bundle = &wordsBundle}, "probe.words",
                          RigMode::Local};
    CHECK(harness.waitForText("hello", "Hello"));
    CHECK(harness.waitForText("eur", "€13,196.40"));
    CHECK(navTitle(harness, "probe.words") == "Words and amounts");
    CHECK(harness.text("navigator_group_probe/probe.group.main") == "Main work");

    harness.click("german");
    CHECK(harness.waitForText("hello", "Hallo"));
    CHECK(harness.waitForText("eur", "13.196,40 €"));
    CHECK(harness.waitFor([&harness] { return navTitle(harness, "probe.words") == "Wörter und Beträge"; }));
    CHECK(harness.text("navigator_group_probe/probe.group.main") == "Hauptarbeit");
    CHECK(harness.problems().empty());
}

// Mutation: drop the shell's report of a locale with no facts (problems stays empty).
TEST_CASE("Client shell: a locale with no formatting facts is reported, and written as en's",
          "[client][shell][locale]") {
    ScreenHarness harness{morph::client::LocalApp{.name = "probe", .bundle = &wordsBundle}, "probe.words",
                          RigMode::Local};
    harness.click("polish");
    CHECK(harness.waitFor([&harness] {
        return std::ranges::any_of(harness.problems(), [](std::string const& line) {
            return line.contains("no formatting facts for locale `pl`");
        });
    }));
    // No `pl` catalogue: the `t` shows the fallback; the amount is written as en writes it.
    CHECK(harness.waitForText("hello", "Fallback"));
    CHECK(harness.waitForText("eur", "€13,196.40"));
}
```

Add `test_shell_locale.cpp` to `morph_client_tests` in `tests/client/CMakeLists.txt`. Two further cases in the same file, written the same way: `ShellOptions::locale = "de"` mounts the screen in German from the first frame (through `client::mountShell`, since `ScreenHarness` does not pass it; mutation: start the session in `en` whatever the option), and `ShellOptions::onLocale` is called with `de` after the button (mutation: never call it). Run them: they fail to compile until Step 3 adds the names.

- [ ] **Step 3: Implement**

To the spec, every public symbol with complete `@param`/`@tparam`/`@return`:

- `include/morph/ui/locale_table.hpp`: the table as `inline constexpr` arrays (`kEnglishSymbols`, `kGermanSymbols`, `kLocaleTable`), `findLocaleFacts` walking the tag as `MessageCatalogues::find` does, `localeFacts`, and `placeCurrency`: the code form when `form == MoneyForm::Code`, `facts.symbols` is empty, or @p currency is not listed (`number + " " + currency`); otherwise the sign (a leading `numeric.negativeSign`, or `-` when that is empty) taken off and put first, then `symbol` and the digits (`Before`, with `" "` between them when the symbol's last byte is an ASCII letter) or the digits, `" "` and `symbol` (`After`).
- `expression.hpp`: `FormatContext::currency`; the `money` row takes 2 to 3 arguments, the third a string; `FunctionId::Money` writes `ui::placeCurrency(render::formatCanonicalNumber(*text, context.numeric), *currency, context.currency, context.numeric, form)`, a third argument other than `"symbol"` or `"code"` a mismatch ("`money`'s form is \"symbol\" or \"code\"").
- `interpreter.hpp`: `Session::setLocale` installs `localeFacts(locale)`'s `numeric` and `currency` in the same `setFormatting` call as the locale, and its date patterns while `_declaredDatePatterns` is false; `setDatePatterns` sets `_declaredDatePatterns` when either pattern is non-empty.
- `catalogue.hpp`: `catalogTitlesIn` and `catalogLabel` (a key the catalogues hold for @p locale is parsed with `parseMessage` and formatted with no arguments; anything else is @p text).
- `navigator.hpp`: `NavigatorOptions::label`; the heading is `label(group)`, or `label(module)` when the group is empty (no `module / group` join), and as written when `label` is empty; the title text gets `setTestId("nav_title_" + entry.id)`; `relabel()` re-reads every entry's titles and every heading and keeps the pins, the current entry, the badges and the search text, then calls the on-changed handler so `menuEntries()` follows.
- `source.hpp`/`source.cpp`: `Source::catalogues()` is `ui::loadCatalogues(_loaded->manifest, _loaded->files)`, an empty `MessageCatalogues` before a bundle; confirm `LocalSource` fills `files` with the catalogues' bytes (the shell test fails on `hello` otherwise).
- `shell.hpp`/`shell.cpp`: `ApplicationHost::setLocale` calls the sink; in `start`, right after the session is made, `installCatalogues()` (`_catalogues = _source->catalogues()`, each of its `problems()` reported, `_session->setTranslations(_catalogues.provider())`) and `applyLocale(_options.locale.empty() ? "en" : _options.locale)` (`_session->setLocale`, and the report when `findLocaleFacts` finds nothing); the sink, set once in `start`, reports a locale with no facts, calls `_frame->navigator().relabel()` and `showActiveTitle()`, then `_options.onLocale`. `buildFrame` sets `frame.navigator.titles` to `catalogTitlesIn(entry, &_catalogues, <the session's locale>)` and `frame.navigator.label` to `catalogLabel(_catalogues, text, <the session's locale>)`, each reading `_session->formattingNow().locale` when called. `reloadChanged` reinstalls the catalogues and relabels when the poll's changed ids include an `i18n/` file (read `src/client/source.cpp`'s poll for what those ids are; if catalogue ids are not among them, reinstall on every changed digest). Every other place the shell draws a catalog title (`grep -n "catalogTitle\|\.title" src/client/shell.cpp`) resolves it the same way.

Run: the framework suite `"[locale]"`, `"[catalogue]"` and `"[ui]"`; `flock /tmp/claude-1000/-home-yaraslau-repo-morph/b1b73e49-c41d-4e27-bb68-b8c0cfacf145/scratchpad/morph-build.lock cmake --build build/linux-everything --target morph_client_tests -j10` and `flock /tmp/claude-1000/-home-yaraslau-repo-morph/b1b73e49-c41d-4e27-bb68-b8c0cfacf145/scratchpad/morph-build.lock timeout 300 build/linux-everything/tests/client/morph_client_tests "[client]"` (the path is the build tree's; use where the target lands); the Qt Quick and terminal frontends' suites; the expression corpus (`tests/ui/corpus/expressions.json`'s `money` rows still answer `1234.50 EUR`: a default `FormatContext` has no currency facts). Expected: all pass. Run each mutation named in the two files once, confirm its case fails, restore it (and `touch` the restored file).

- [ ] **Step 4: lims's kit test back to the brief, and every rung green**

In `examples/lims/screens/tests/test_lims_kit.cpp`, the case "lims kit: money, balances, days and states as the shell shows them" becomes the brief's "lims kit: money, balances, days and states in words, in English and German" (Task 2's Step 1 text): drop the comment saying no client installs the catalogues; assert `€13,196.40`, `¥1,234,567`, `€13,196.40 Cr`, the day as lims's declared pattern writes it (`09-Nov-2026`), `Waiting for approval` and `Draft for Stadtwerke Köln`; click `german`; assert `13.196,40 €`, `1.234.567 ¥`, `13.196,40 € H` (each with the no-break space `money` writes, spelled ` `), the day still `09-Nov-2026` (the application's pattern holds in every locale; `09.11.2026` would need lims to declare no pattern), `Wartet auf Freigabe` and `Entwurf für Stadtwerke Köln`. Its mutations stay the brief's.

Run: Build lims, Screen suite `"[kit]"`, Render smoke, then Everything labelled for every rung (`flock /tmp/claude-1000/-home-yaraslau-repo-morph/b1b73e49-c41d-4e27-bb68-b8c0cfacf145/scratchpad/morph-build.lock ctest --test-dir build/linux-everything -j6 -L ladder --output-on-failure`). A rung's screen test whose expectation was the C form and is now `en`'s (`1234.50` → `1,234.50`, `12.00 EUR` → `€12.00`) is changed to what the shell now writes, in place; a failure of any other kind is a defect of this task and is fixed here. Expected: all pass.

- [ ] **Step 5: The Docs build, the issue, commit**

Run the Docs build. Expected: no warning; `locale_table.hpp`, `catalogue.hpp`, `navigator.hpp`, `shell.hpp` and `source.hpp` also compiled through a clang TU that includes them (`-Wdocumentation`). Then add a comment to #1098 stating what this task measured (the shell case's output, on the revision committed) and that the money symbol and the date's on-screen pattern are settled as `locale_table.md` states.

```bash
cd /home/yaraslau/repo/morph
git add docs/spec include/morph src tests examples
git commit -m "Install a bundle's catalogues and a locale's number, money and date facts in the client shell

The shell reads the verified catalogues and installs them; setLocale installs the
locale's separators, currency symbols and default date pattern from a table morph
owns (en, de); the navigator's titles and headings are the catalogue's words.

Closes #1098

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01No1NznqxEBvjhZBaBRLCd6"
```

`git add examples` stages only the rung tests Step 4 changed; check `git status` first and add those paths by name if another lane has work under `examples/`.

---

### Task 5: Refusals in words (lims)

**Waits for** Tasks 2, 3 and 4, and Part B Task 14 committed (`invoice_edit_model.cpp`, `invoice_approval.cpp`). **Never in flight with** Part B Task 19.

**Files:**
- Create: `examples/lims/billing/include/lims/billing/core/refusal_codes.hpp`
- Modify: `examples/lims/org/include/lims/org/core/errors.hpp` (a code per class, a constructor with a specific code and arguments, `kOrgRefusalCodes`), the lease refusal in `examples/lims/billing/src/models/invoice_edit_model.cpp` and `SubmitInvoice`'s "is being edited by" in `examples/lims/billing/src/models/invoice_approval.cpp`, `examples/lims/tests/test_invoice_lease.cpp`, `examples/lims/screens/kit/kit.hpp`/`.cpp` (`refusalBanner`; `errorBanner` removed), the kit's and billing's catalogue files, `examples/lims/screens/tests/test_lims_kit.cpp`, the existing screens that used `errorBanner`, `examples/lims/README.md` (decision 52)

**Interfaces:**
- Consumes: `morph::model::ActionRefusal`, `RefusalArg`, `ui::author::refusalText`, `MutationHandle::refusal()`, `OrgModel::displayNameOf`.
- Produces: `LimsError` deriving from `morph::model::ActionRefusal`, each class with a default code (`lims.refusal.validation`, `.notFound`, `.forbidden`, `.conflict`, `.outOfRange`, `.illegalTransition`, `.emptyPrincipal`) and a second constructor `(std::string code, std::vector<morph::model::RefusalArg> args, std::string message)`; per module, an `inline constexpr std::array<std::string_view, N> k<Module>RefusalCodes` in that module's `core/refusal_codes.hpp` (org's in `errors.hpp`), listing every specific code the module throws; `lims.billing.invoiceLeased` (argument `holder`, the holder's display name); `kit::refusalBanner(ui::AnyHandle const& refusal)`.

- [ ] **Step 1: Write the failing tests**

In `test_invoice_lease.cpp`, a case "invoice lease: a refused lease carries its code and the holder's name" (alice leases; bob's `LeaseInvoice` throws a `Conflict` whose `code()` is `lims.billing.invoiceLeased` and whose `holder` argument is `alice`'s display name — `alice`, since the test names nobody; then name alice "Alice Adams" through `SetDisplayName` as a supervisor, refuse bob again, and read "Alice Adams"; mutation: pass the principal instead of `displayNameOf`). Every existing `CHECK_THROWS_WITH` keeps its message text unchanged. In `test_lims_kit.cpp`: "lims kit: a refusal banner shows the code's words, never the message" (a probe screen whose mutation fails with the lease refusal; English and German words; mutation: the banner shows the message) and "lims kit: every code lims throws has words in both catalogues" (iterates `kOrgRefusalCodes`, `kBillingRefusalCodes` and the class codes against the merged catalogues; mutation: drop `lims.billing.invoiceLeased` from `de/billing.json`). The second case's list of arrays grows when a later task creates a module's `refusal_codes.hpp`; that task adds its array to the case.

- [ ] **Step 2: Move lims onto `ActionRefusal`**

`LimsError` derives from `morph::model::ActionRefusal`; each subclass's existing constructor keeps its signature and passes its class's default code and no arguments, so every throw site compiles unchanged; each gains the second constructor. The lease refusal in `InvoiceEditModel::execute(const LeaseInvoice&)` and `SubmitInvoice`'s "being edited by" become `Conflict{"lims.billing.invoiceLeased", {{"holder", OrgModel::displayNameOf(holder)}}, <the message as today>}`. `kit.json` gains the words of the seven class codes, each under its code (`lims.refusal.conflict` "Someone changed this meanwhile. Reload it and try again." / "Jemand hat das inzwischen geändert. Bitte neu laden und erneut versuchen.", and one each for validation, not found, forbidden, out of range, illegal transition and empty principal), and `lims.generic`, the sentence for any lims code without words; `billing.json` gains `lims.billing.invoiceLeased` ("{$holder} is editing this invoice." / "{$holder} bearbeitet diese Rechnung.") and `lims.billing.generic`. `kit::refusalBanner(refusal)` is `ui::banner({.tone = "err", .text = ui::refusalText(refusal)})`, visible while the refusal is engaged; `kit::errorBanner` is removed and its callers moved. Each later screen task gives the refusals its mockup shows a specific code in its module's `refusal_codes.hpp`, with both catalogues' words, as part of building that screen.

- [ ] **Step 3: Build, run, record decision 52, commit**

Run: Build lims, Model suite (every `CHECK_THROWS_WITH` holds, the message texts being unchanged), Screen suite, Render smoke. Expected: all pass; the mutations each fail their case; restore.

```markdown
### 52. A refusal reaches a screen as a code with its arguments, and the screen words it

Every lims error is a `morph::model::ActionRefusal`: its class gives a default code (`lims.refusal.conflict`, …) and
a refusal a screen shows gives a specific one with its arguments (`lims.billing.invoiceLeased`, holder). A person
named in a refusal is named by the display name the server resolved. The code travels in the `err` envelope beside
the message; a screen shows `refusalText`, the catalogue's words for the code in the user's locale, or its class's
sentence, and never the message, which stays what lims logs and its tests read. Every code a module throws is listed
beside it, and a test holds both catalogues to the list.
```

```bash
cd /home/yaraslau/repo/morph
git add examples/lims/org/include/lims/org/core/errors.hpp examples/lims/billing examples/lims/tests/test_invoice_lease.cpp \
        examples/lims/screens examples/lims/README.md
git commit -m "Throw lims's refusals as codes with arguments, and show them in words

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01No1NznqxEBvjhZBaBRLCd6"
```

---

### Task 6: The application shell (framework)

**Waits for** Task 4a (both edit `navigator.hpp`, `src/client/shell.cpp` and `docs/spec/client/client.md`, and the principal menu's locale switch needs 4a's facts to show anything). Changes no file under `examples/`. **Never in flight with** the other framework tasks.

**Files:**
- Create: `docs/spec/ui/shell_parts.md`, `include/morph/ui/shell_parts.hpp`, `tests/test_ui_shell_parts.cpp`
- Modify: `include/morph/ui/bundle.hpp` (the manifest's shell parts), `include/morph/ui/service.hpp` (`UiRegistry::setShellParts`), `include/morph/ui/shell_frame.hpp` and its implementation (the switcher, the search field and its results, the inbox button, module groups with counts, the principal's menu), `include/morph/ui/navigator.hpp` (collapsed groups with a count), the generic client's shell wiring (`docs/spec/client/client.md` names it), the terminal frontend's shell, `docs/spec/ui/shell_frame.md`, `docs/spec/ui/workspace.md`, `docs/spec/client/client.md`, `docs/spec/README.md`

**Interfaces:**
- Consumes: `ui::ShellFrame`, `ui::Navigator` (`setBadge`, `menuEntries`), `ui::Workspace` (`HeadPlacement::InPage`), `CatalogEntry::module`/`group`, `Session::setLocale`, `ui::author::env`.
- Produces: `struct morph::ui::ShellParts { std::optional<ContextSwitch> context; std::vector<ModuleCount> modules; std::vector<SearchSource> search; std::optional<InboxEntry> inbox; std::optional<PrincipalMenu> principal; }`, each naming a model and a read action by id with the fields it reads (`ContextSwitch { model, action, optionsField, labelField, valueField }`, `ModuleCount { module, model, action, countField }`, `SearchSource { module, model, action, queryField, resultsField }`, `InboxEntry { screen, sources: std::vector<ModuleCount> }`, `PrincipalMenu { model, action, nameField, rolesField, locales }`); `UiRegistry::setShellParts(ShellParts)`; the chosen context as `{"env": "context"}` to every screen (its value the option's `valueField`), kept per session and sent with no action.

The mockups' shell (`Shell.dc.html`) holds what the ShellFrame does not: a company and site switcher whose choice scopes every list and every new record; module groups that collapse to one row with a count of the module's open work, only the open module listing its screens; one search field over every module with grouped, typed results while typing (Ctrl K; arrows move the highlight while focus stays in the field; Enter opens; Shift Enter opens all results); an approvals button with its count in the top bar; and the signed-in person as a menu with their name, roles and the locale. The frame is the client's, built from widgets, and an application cannot draw any of these in a screen. Each is mechanism — a CRM needs a scope switcher, counts, search and an inbox unchanged — and each is declared by the application as data the shell reads: which model action answers it. A module's count is one action of that module's model (`ModuleCount` names a model and an action per module), because an action type belongs to one model. The domain stays in the application: morph knows no company, no role, no record kind.

- [ ] **Step 1: The spec, and one review**

`docs/spec/ui/shell_parts.md`: each part's declaration, what the client asks and when (counts and the inbox's count on mount and again on any change to the records their actions read, through the same effects screens use; search on each edit after a pause, cancelled by the next), where each is drawn on `ShellFrame` (the switcher at the sidebar's top under the application's name; module groups in the navigator, one open; the search field and the inbox button in the top bar's line before the principal; the principal's menu replacing the principal text), the compact form (the switcher and the module groups inside the navigation menu; search and the inbox stay in the bar; search opens full-screen), the terminal frontend (each part as a menu or a line; usable, not styled), keyboard (Ctrl K focuses search; the search results' highlight moves with arrows while the field keeps focus; Escape closes), accessibility names, how a screen reads the context (`env("context")`) and how the context reaches a query's body (the screen passes it; the shell never edits a body), what happens when the chosen option disappears (the first option), a screen that changes the context (a command a banner's action can run, so a refusal can offer to change the scope), and alternatives considered (a screen per part: each screen would redraw the shell; an application-specific shell: per-application client code, which the design forbids). Update `shell_frame.md`, `workspace.md`, `client/client.md` and the index. One independent review (default model) with the shell frame's header and implementation, the navigator, the client's shell wiring and the mockup as its material; address every finding.

- [ ] **Step 2: Failing framework tests, then the implementation**

`tests/test_ui_shell_parts.cpp`, over a probe application on the recording backend: the switcher lists a probe action's options and the chosen value reaches a screen's `env("context")`; a screen's command changes the context; a module's count shows on its collapsed group and changes after an action that declares a change to the record the count action reads; search results appear grouped after typing and Enter opens the highlighted hit's screen with its params; the inbox button's count is the sum of its sources; the principal's menu shows the name, lists the roles and switches the locale. Each with its mutation. Implement to the spec; the Qt Quick and terminal frontends draw each part.

- [ ] **Step 3: Build, run, commit**

Run: the framework suite `"[shell]"` and `"[ui]"`, the frontends' suites, the Docs build. Expected: all pass, the Docs build with no warning; run each mutation once and restore it.

```bash
cd /home/yaraslau/repo/morph
git add docs/spec include/morph src tests
git commit -m "Let an application declare its shell's scope switcher, counts, search, inbox and principal menu

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01No1NznqxEBvjhZBaBRLCd6"
```

---

### Task 7: The application shell (lims)

**Waits for** Tasks 2, 3, 4a, 5 and 6; Part B Task 14 committed (`invoice_model.hpp`, `ledger_model.hpp`); Part C Task 2 (the requisitions count and inbox source; it also edits `org_model.cpp`). **Never in flight with** Part C Task 2 or Part B Task 19.

**Files:**
- Create: `examples/lims/org/include/lims/org/dto/shell_dto.hpp`, `examples/lims/screens/shell_parts.cpp`
- Modify: `examples/lims/org/include/lims/org/models/org_model.hpp`/`org_model.cpp` (`ListMyScopes`), `examples/lims/lab/include/lims/lab/models/sample_model.hpp` and its source (`CountSamplesToVerify`, `SearchSamples`), `examples/lims/billing/include/lims/billing/models/invoice_model.hpp` and its source (`CountOpenInvoiceWork`, `SearchInvoices`), `examples/lims/ledger/include/lims/ledger/models/ledger_model.hpp` and its source (`CountClosingPeriods`, `SearchAccounts`), `examples/lims/inventory/include/lims/inventory/models/procurement_model.hpp` and its source (`CountPendingRequisitions`), `examples/lims/inventory/include/lims/inventory/models/stock_model.hpp` and its source (`SearchStock`), each module's test suite, `examples/lims/screens/lims_screens.cpp` (`setShellParts`), `examples/lims/screens/kit/kit.cpp` (`pageHead`'s company), the kit's and shell's catalogue files, `examples/lims/screens/tests/test_lims_kit.cpp`, `examples/lims/README.md` (decision 53)

**Interfaces:**
- Consumes: `morph::ui::ShellParts` and its parts, `UiRegistry::setShellParts`, `env("context")`, `OrgModel::visibleCompanies`, `GetSignedInUser` (Task 3), `ListPendingApprovals`, `ListPendingRequisitions` (Part C Task 2), `decimalsOf`.
- Produces: in `org/dto/shell_dto.hpp`, the result types every module's shell reads share: `OpenWork { Count count; }`, `SearchHit { std::string title; std::string subtitle; std::string screen; std::string params; }`, `SearchHits { std::vector<SearchHit> hits; Count total; }`; `ListMyScopes {}` → `ListMyScopesResult { std::vector<ScopeOption> scopes; }` (`ScopeOption { CompanyId company; SiteId site; std::string label; CurrencyCode currency; Count decimals; }`); per module, its own count and search actions, each a read with its own type: `CountSamplesToVerify { CompanyId company; }` (lab), `CountOpenInvoiceWork { CompanyId company; }` (billing), `CountClosingPeriods { CompanyId company; }` (ledger), `CountPendingRequisitions { CompanyId company; }` (inventory) → `OpenWork`; `SearchSamples`, `SearchInvoices`, `SearchAccounts`, `SearchStock { std::string query; CompanyId company; }` → `SearchHits`.

**Why one action type per module, composed by the shell.** morph's registry specialises `ActionTraits<A>` once per action type (`registry.hpp`), so an action type belongs to exactly one model, and Part C's convention forbids one name in two modules. An aggregating model in `lims/app` was the alternative: its one action would read four modules' records through dispatches inside a read, it would own each module's definition of "open work", and no module's declared effects would name its result, so its count would not refresh when a sample, invoice or period changed. One count per module keeps each count beside the records and the effects that change it; the composition is data, the four `ModuleCount` entries in `shell_parts.cpp`. The result types are shared, defined once in org, since a result type is not an action and several actions may answer one.

- [ ] **Step 1: Write the failing tests**

Model tests for each new action in its module's suite (`[org]`, `[lab]`, `[invoice]`, `[ledger]`, `[requisition]`, `[inventory]`), each with its mutation: `ListMyScopes` lists every visible company with each of its sites and only those (mutation: list every company); each count counts only the caller's company's open work (mutation: ignore the company); each search answers at most 8 hits and the exact total, only of the caller's companies (mutation: answer the total as the hit count). A screen test in `test_lims_kit.cpp`: choosing Leverkusen in the switcher changes a probe list to Leverkusen's records (mutation: a query that ignores `env("context")`).

- [ ] **Step 2: Implement, and declare the shell**

`ListMyScopes` on `OrgModel` (every company the caller can see, with each of its sites, labelled "Rheinlab Analytik GmbH · Köln", a company alone as "… · all sites", each with its base currency and that currency's decimals); the four counts (lab: samples to verify at the company; billing: drafts and submitted invoices; ledger: periods closing; inventory: requisitions waiting for approval), each a read; the four searches (`SearchSamples` by reference prefix, `SearchInvoices` by number or client name, `SearchAccounts` by code or name, `SearchStock` by item code or lot number), each a read limited to the caller's companies and answering at most 8 hits and the total. Each read is registered beside its model with `::morph::model::Loggable::No`. `screens/shell_parts.cpp` builds the `ShellParts`: the context from `ListMyScopes`; one `ModuleCount` per module; one `SearchSource` per module; `inbox` from billing's `ListPendingApprovals` count and inventory's `CountPendingRequisitions`; `principal` from `GetSignedInUser` (`displayName`, `roles`) with `en` and `de`; a hit's `screen` and `params` open the record's screen. `lims_screens.cpp`'s `bundle()` calls `served.setShellParts(lims::screens::shellParts())`. `kit::pageHead` adds the context's company label to the eyebrow.

- [ ] **Step 3: Build, run, grab, record decision 53, commit**

Run: Build lims, Model suite, Screen suite, Render smoke; `scripts/shoot_lims.sh` for the org screens beside `Shell.dc.html`. Expected: all pass; the shell's grabs match the mockup's frame, differences classified per the Conventions and the framework ones added to Task 6 (re-opened in this branch) before this task is done.

```markdown
### 53. The shell's switcher, counts, search, inbox and principal menu are declared, and morph draws them

lims declares its shell as data (`ShellParts`): the scopes the caller can see (`ListMyScopes`), each module's open
work through that module's own count (`CountSamplesToVerify`, `CountOpenInvoiceWork`, `CountClosingPeriods`,
`CountPendingRequisitions`), a search action per module, the approvals inbox's sources, and the signed-in person's
name and roles. One action type belongs to one model, so the counts are four actions composed by the declaration, not
one action in four modules; each refreshes through its own module's effects. morph's shell draws each part on both
frontends and hands the chosen scope to every screen as `env("context")`; every lims list and every new record reads
it. Nothing in morph knows a company, a module's work or a record kind.
```

```bash
cd /home/yaraslau/repo/morph
git add examples/lims/org examples/lims/lab examples/lims/billing examples/lims/ledger examples/lims/inventory \
        examples/lims/tests examples/lims/screens examples/lims/README.md
git commit -m "Declare lims's shell: scopes, each module's open work, search, the inbox and the signed-in person

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01No1NznqxEBvjhZBaBRLCd6"
```

---

### Task 8: Per-record decimals in an input (framework; hypothesis 8's frontend half)

**Waits for** Task 4 (the framework lane runs Tasks 8–12 serially). **Never in flight with** the other framework tasks. Changes no file under `examples/`.

**Files:**
- Modify: `include/morph/ui/view.hpp` (`TextInput::decimals` and `unit` bound), `include/morph/ui/author.hpp` (the `textInput` options and a `form` field overlay's `decimals` take expressions), the interpreter (re-evaluates them), the Qt Quick and terminal frontends (re-apply them), `docs/spec/ui/view_tree.md` (the `TextInput` row and the mount note), `docs/spec/ui/document.md` (the `form` overlay), `docs/spec/forms/instance_constraints.md` (a paragraph "In ui/1"), `tests/test_ui_bound_decimals.cpp` (create)

**Interfaces:**
- Consumes: `morph::forms::InstanceConstraints` (`FieldConstraint::decimalPlaces`, `decorate`, `checkValue`), `TextInput::unit` (a `Prop`), `TextInput::decimals` (a plain `int`, set once), the `form` node's field overlays.
- Produces: `TextInput::decimals` as a `Prop<int>` the mount re-applies on change; a `form` field overlay's `decimals` (an expression) that overrides the served schema's `x-decimalPlaces` for that field.

One mechanism serves every per-record precision a screen meets: a value limit's input follows the chosen scope's currency (Task 13), a price list's prices follow the list's currency (Task 15), and an order's prices follow the supplier's (Task 22). The server side of it already exists: `morph::forms::InstanceConstraints` serves per-instance decimals on a decorated schema (`x-decimalPlaces`, stamped in `x-instanceConstraints`) and checks a submitted value against the same declaration. What ui/1 lacks is the client half: a `TextInput`'s `decimals` is set once at mount (`view_tree.md`), and a `form` node builds from the action's static schema (`document.md`'s `schema(actionId)` lookup), which no instance decorates. This is that half and nothing more; Part C Task 8's runtime-units spec, if written, names this task as its frontend half and specifies the wire, combination and dated conversion only.

- [ ] **Step 1: Read, then the spec, and one review**

Read `docs/spec/forms/instance_constraints.md` and `include/morph/forms/instance_constraints.hpp` first, and the `form` node's construction in the interpreter. If a `form` node can already take a decorated schema (a query's result carrying `x-instanceConstraints`), the spec uses that for forms and this task's form half is that wiring only; say which in the spec. The spec changes: `view_tree.md`'s `TextInput` row says `decimals` follows its binding (and `unit`, which already does); a change of decimals with a value already typed keeps the text and re-validates it, showing the field's error when it no longer fits; `document.md`'s `form` overlay gains `decimals`, which overrides the schema's `x-decimalPlaces` for that field and means exactly what `InstanceConstraints`' key means; `instance_constraints.md` gains "In ui/1": a screen binds the instance's decimals to the input or overlay, from the same record the model's `InstanceConstraints` reads, and the model keeps checking the submitted value (`checkValue`, or the application's own check) because a client's decimals are a convenience, never the check. One independent review (default model) with the mount code, the interpreter's `form` construction and `instance_constraints.hpp` as its material; address every finding.

- [ ] **Step 2: Failing tests, then the implementation**

`tests/test_ui_bound_decimals.cpp`, over the recording backend, one case per frontend: a `textInput` whose `decimals` is bound to a state shows 2, then 0 after the state changes, and a typed `12.5` turns invalid at 0 (mutation: apply decimals at mount only); a `form` whose overlay binds a field's `decimals` re-validates the field when the binding changes (mutation: the overlay's decimals ignored). Implement in the files listed.

- [ ] **Step 3: Build, run, commit**

Run: the framework suite `"[ui]"`, the frontends' suites, the Docs build. Expected: all pass, no warning; each mutation fails its case; restore.

```bash
cd /home/yaraslau/repo/morph
git add docs/spec include/morph src tests
git commit -m "Let an input's and a form field's decimals follow a bound value, as an instance constraint serves them

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01No1NznqxEBvjhZBaBRLCd6"
```

---

### Task 9: A step's second line (framework)

**Waits for** Task 8. **Never in flight with** the other framework tasks. Changes no file under `examples/`.

**Files:**
- Modify: `include/morph/ui/view.hpp` (`Step::detail`), `include/morph/ui/author.hpp` (`steps` items accept it), the interpreter, the Qt Quick and terminal frontends' steps widget, `docs/spec/ui/view_tree.md` (the `Steps` row), `tests/test_ui_step_detail.cpp` (create)

**Interfaces:**
- Consumes: `morph::ui::Step` (`{label, state}`), `ui::author::steps`.
- Produces: `std::string morph::ui::Step::detail` (a muted second line: who and when), bound like `label`.

The lab's sample lifecycle (mockup 15), the invoice editor's approval route (21) and a requisition's route (29) each show a second, muted line per step — who and when. `Step` has a label and a state only.

- [ ] **Step 1: Spec, review, failing test, implementation**

`view_tree.md`'s `Steps` row gains `detail`; Qt Quick draws it under the label, muted; the terminal after the label, in brackets. One independent review (default model) with the steps widget's frontend code as its material; address every finding. `tests/test_ui_step_detail.cpp` over the recording backend (mutation: the frontend drops the detail). Implement.

- [ ] **Step 2: Build, run, commit**

Run: the framework suite `"[ui]"`, the frontends' suites, the Docs build. Expected: all pass, no warning; the mutation fails the case; restore.

```bash
cd /home/yaraslau/repo/morph
git add docs/spec include/morph src tests
git commit -m "Give a step a second line for who and when

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01No1NznqxEBvjhZBaBRLCd6"
```

---

### Task 10: Bars in a chart (framework)

**Waits for** Task 9. **Never in flight with** the other framework tasks. Changes no file under `examples/`.

**Files:**
- Modify: `include/morph/ui/view.hpp` (`ChartSeries::style`), `include/morph/ui/author.hpp`, the chart widget of both frontends, `docs/spec/ui/view_tree.md` (the chart rows), `tests/test_ui_chart_bars.cpp` (create)

**Interfaces:**
- Consumes: `morph::ui::ChartSeries` (no style today).
- Produces: `ChartSeries::style` (`ChartStyle{Line, Bars}`, `Line` by default).

Home (mockup 11) draws the monthly revenue as bars.

- [ ] **Step 1: Spec, review, failing test, implementation**

`ChartSeries` gains `style`; bars of several series stand side by side per x; the terminal draws a bar as a run of block characters per x. `view_tree.md`'s chart rows say so; one independent review (default model) with both frontends' chart code as its material; `tests/test_ui_chart_bars.cpp` over the recording backend (mutation: the style is not passed to the widget). Implement in both frontends.

- [ ] **Step 2: Build, run, commit**

Run: the framework suite `"[ui]"`, the frontends' suites, the Docs build. Expected: all pass, no warning; restore the mutation.

```bash
cd /home/yaraslau/repo/morph
git add docs/spec include/morph src tests
git commit -m "Draw a chart series as bars

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01No1NznqxEBvjhZBaBRLCd6"
```

---

### Task 11: A table row that spans rows (framework)

**Waits for** Task 10. **Never in flight with** the other framework tasks. Changes no file under `examples/`.

**Files:**
- Modify: `include/morph/ui/view.hpp` (`TableRowView::spans`), `include/morph/ui/author.hpp`, both frontends' table, `docs/spec/ui/view_tree.md` (the `Table` row), `tests/test_ui_table_row_span.cpp` (create)

**Interfaces:**
- Consumes: `morph::ui::TableRowView` (`tone`, `heading`; no span).
- Produces: `TableRowView::spans`, a list of (column, rows) the row's cells cover downward.

The goods-receipt dialog (mockup 30) shows one order line split into two lots: the order line's cells span the lots' rows.

- [ ] **Step 1: Spec, review, failing test, implementation**

A table cell may span the rows below it; both frontends draw it (Qt Quick merges the cells; the terminal shows the cell's text once and leaves the covered cells blank); `view_tree.md`'s `Table` row says so, including what sorting does to a span (a sorted table drops spans, since the rows they join are no longer adjacent); a framework test over the recording backend (mutation: span ignored); one independent review (default model) with both frontends' table code as its material.

- [ ] **Step 2: Build, run, commit**

Run: the framework suite `"[ui]"`, the frontends' suites, the Docs build. Expected: all pass, no warning; restore the mutation.

```bash
cd /home/yaraslau/repo/morph
git add docs/spec include/morph src tests
git commit -m "Let a table row's cells span the rows below it

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01No1NznqxEBvjhZBaBRLCd6"
```

---

### Task 12: A document page, its regions and a locale scope (framework; hypothesis 9, the document instance)

**Waits for** Task 11, and Task 4a (`localeScope` takes its facts from 4a's locale table, `ui::localeFacts`). **Never in flight with** the other framework tasks. Changes no file under `examples/`.

**Files:**
- Create: `docs/spec/ui/document_page.md`, `tests/test_ui_document_page.cpp`
- Modify: `include/morph/ui/view.hpp`, `include/morph/ui/author.hpp` (`page`, `pageRegion`, `localeScope`), the interpreter, the Qt Quick and terminal frontends, `docs/spec/ui/view_tree.md`, `docs/spec/ui/expressions.md` (formatting under a scope's locale), `docs/spec/README.md`

**Interfaces:**
- Consumes: `LayoutHints`, `spacer`, `money`, `date`, `number`, `t`, the interpreter's formatting context.
- Produces: `ui::author::page(PageOptions{width, height, margins}, Nodes)` — a fixed-size page in millimetres, laid out at that size and scaled to the available width when the window is narrower (compact); `ui::author::pageRegion(RegionOptions{anchor: top|bottom, offset}, Nodes)` — content pinned to a page's top or bottom margin (the letterhead, the footer) or placed at an offset (the address window, DIN 5008's 45 mm × 85 mm at 20 mm from the left and 45 mm from the top); `ui::author::localeScope(AnyHandle locale, Nodes)` — `money`, `date`, `number` and `t` inside it format in that locale.

The print view is the design's document instance of hypothesis 9: "a fixed-size A4 page laid out as a document (letterhead, address window, footer pinned to the page bottom), in a locale other than the user's, scaled to the width when compact. Nothing in ui/1 lays out a page." (the mockups' README). Rendering the page to a file or a printer is out of the design's scope ("PDF and print-file rendering"; "Document rendering … is domain output and is not built in morph at all"). What is built is the page on screen, which needs three words ui/1 does not have — a page of fixed physical size scaled to fit, regions pinned within it, and formatting in a locale other than the session's. Each is mechanism (a CRM's quote or a bank's statement is the same page), and none knows an invoice.

- [ ] **Step 1: Measure first: can ui/1 as it stands lay out the page?**

Build a probe screen in `tests/test_ui_document_page.cpp` from mockup 22's literals with the vocabulary as it stands — a `column` of fixed width (`LayoutHints` `fixed`) for the sheet, a `spacer` with `stretch` to push the footer down, `money` and `date` for the figures — and write down, against mockup 22, what it cannot do: a physical size (units are backend units, not millimetres); a footer pinned to the page's bottom when the page's height is fixed and the content short (a `stretch` spacer needs a container whose height is fixed, and a `column`'s height follows its content); an address window at an offset; the client's locale while the session's is the clerk's; the page scaled to the width when compact (nothing scales a subtree). Each point that ui/1 can say after all is dropped from Step 2; what remains is the measured gap. Write it to `/tmp/claude-1000/-home-yaraslau-repo-morph/b1b73e49-c41d-4e27-bb68-b8c0cfacf145/scratchpad/lims-h9-print.md` for Task 16's decision (expected: confirmed — no document layout).

- [ ] **Step 2: The spec, one review, failing framework tests, the implementation**

`docs/spec/ui/document_page.md`: the three nodes, their units (millimetres in the document; each frontend maps to its own units at a stated density — Qt Quick at the screen's logical DPI, the terminal at a fixed cells-per-millimetre so a page reads as a page of text), scaling (a page wider than its container is scaled down uniformly, never up; the terminal does not scale and scrolls), pinning and offsets (a region's anchor is the page's margin box; overlapping regions draw in order), locale scope (every formatting function and `t` inside evaluates with the scope's locale; the session's locale is unchanged outside), accessibility (the page is a document region; its reading order is the node order), and alternatives considered (a `custom` QML component per document: per-application client code the design forbids; rendering to an image on the server: document rendering is out of scope). Index it; amend `view_tree.md` and `expressions.md`. One independent review (default model) with the layout code of both frontends and the interpreter's formatting context as its material. `tests/test_ui_document_page.cpp` over the recording backend: a page's size and scale at a narrower width, a footer at the bottom of a short page, an address window at its offset, and `money` in `de` inside a scope while the session is `en` (each with its mutation). Implement in the files listed.

- [ ] **Step 3: Build, run, commit**

Run: the framework suite `"[ui]"`, the frontends' suites, the Docs build. Expected: all pass, no warning; each mutation fails its case; restore.

```bash
cd /home/yaraslau/repo/morph
git add docs/spec include/morph src tests
git commit -m "Lay out a fixed-size document page with pinned regions and a locale of its own

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01No1NznqxEBvjhZBaBRLCd6"
```

---
### Task 13: Org — companies and sites, users and roles (mockups 31, 32)

**Waits for** Tasks 2, 3, 4a, 5, 7 and 8, and Part C Task 2 (a value limit per role; it also edits `org_model.cpp`). **Never in flight with** Part C Task 2 or Part B Task 19.

**Files:**
- Modify: `examples/lims/screens/org_screens.hpp`/`.cpp` (both screens rebuilt to their mockups; `addOrgScreens`' catalog entries), `examples/lims/screens/tests/test_lims_org_screens.cpp`, `examples/lims/screens/scenario/org_scenario.cpp` (the requisition approver limits and the org records the mockups show), the org catalogue files (`i18n/en/org.json`, `i18n/de/org.json`), `examples/lims/screens/grabs/org.tsv`, `examples/lims/org/include/lims/org/core/errors.hpp` (`kOrgRefusalCodes`), `examples/lims/org/src/models/org_model.cpp` (the last-supervisor refusal's code), `examples/lims/README.md` (decision 54)

**Interfaces:**
- Consumes: `OrgModel` (`ListCompanies`, `ListSites`, `CreateCompany`, `CreateSite`, `ListUsers` with display names, `ListRoleAssignments`, `AssignRole`, `RevokeRole`, `ListValueLimits` with each limit's role, `SetValueLimit`, `ListMyScopes` with each scope's currency and decimals, `displayNameOf`), `PriceListModel::GetBillingSettings`, `StockModel::GetInventorySettings` (Part C Task 1), billing's number series read (Part B Task 14's `NumberSeriesRecord`), the kit (`personName`, `refusalBanner`), `env("context")`, Task 8's bound `TextInput.decimals`, `screen_test_support.hpp`.
- Produces: `companiesScreen()` and `accessScreen()` rebuilt to mockups 31 and 32 (ids unchanged); refusal code `lims.org.lastSupervisor`.

Mockup 31 shows, for the company the switcher chose, its settings — name, base currency and fiscal year start, both marked fixed (lims has no action that changes them, so they are read-only by construction), the number series (invoices: `INV-<year>-<six digits>`, next number; receipts after Part C: `GR-…`), the posting accounts (billing's receivable, revenue, tax and rounding accounts; inventory's stock, goods-received-not-invoiced, consumption and rounding accounts) and its sites, each with a form to add one. Mockup 32 shows the users with their roles, and one selected user's scoped roles, each with its value limit in the scope's company currency and the duty rules it is subject to, an inline row to add an assignment whose limit's unit and decimals follow the chosen scope's currency, and the alert that refuses removing a company's last administrator.

The number series panel reads billing's series (Part B Task 14) and, for receipts, inventory's (Part C Task 4, `GR-…`); where a series has no read yet when this task runs, the panel shows the format with no next number and the read is added with the task that gives the series. The limit column shows each role's limit (Part C Task 2).

- [ ] **Step 1: Read the mockups and list what each element needs**

Open `31-Companies.dc.html` and `32-Users.dc.html` in a browser and write, in the scratch review file, each element and the ui/1 node it becomes (`panel`, `keyValue`, `table` with a `TableRowView` tone, `badge`, `form`, `dialog`, `textInput` with `unit`, `banner`), or "framework" with the missing word. The README's list names one for this task: "an inline add-assignment row whose limit's unit follows the chosen scope's currency" — `TextInput`'s `unit` is bound, but its `decimals` is set once (`view_tree.md`), so a JPY scope cannot take whole yen while an EUR scope takes cents. Task 8 made `decimals` follow its binding; this screen binds it to the chosen scope's `ScopeOption::decimals`. Name which of the two screens needs the compact layout's collapsible sections and pinned footer (Conventions). A word found missing goes to the framework task its subject belongs to (Tasks 6, 8–12, re-opened in this branch) or to a new framework task, which runs before this one is called done.

- [ ] **Step 2: Write the failing screen tests**

Replace the cases in `test_lims_org_screens.cpp` that the rebuilt screens change (the counts become the mockups' headings) and add:

```cpp
// Mutation: show the assignment's company id in the scope column (the check for "Rheinlab Analytik GmbH" fails, and
// the no-raw-id check finds "1").
TEST_CASE("lims org screens: a user's roles show their scope by name and their limit in the scope's currency",
          "[lims-screens][org]") {
    DbFixture fixture;
    lims::screens::seedScenario();
    ScreenHarness harness{limsApp(), std::string{lims::screens::kAccess}, RigMode::Local, {},
                          LocalSetup{.db = {}, .user = "julia"}};
    REQUIRE(harness.waitForText("userList", "Dr. Anna Meyer"));
    harness.click("user-anna");
    CHECK(harness.waitForText("assignmentScope", "Rheinlab Analytik GmbH"));
    CHECK(harness.waitForText("assignmentLimit", "€10,000.00"));
    CHECK_FALSE(harness.backend().find("Text", "text", "1").has_value());
    reportProblems(harness);
    CHECK(harness.problems().empty());
}

// Mutation: show RevokeRole's message instead of the refusal's words (the check for the sentence fails, and the message
// "RevokeRole:" appears).
TEST_CASE("lims org screens: removing a company's last administrator is refused in words, in a dialog",
          "[lims-screens][org]") {
    DbFixture fixture;
    lims::screens::seedScenario();
    ScreenHarness harness{limsApp(), std::string{lims::screens::kAccess}, RigMode::Local, {},
                          LocalSetup{.db = {}, .user = "julia"}};
    harness.click("user-julia");
    harness.click("revoke-supervisor");
    CHECK(harness.waitForText("refusalDialog", "Julia Schneider is the last administrator of Rheinlab Analytik GmbH"));
    CHECK_FALSE(harness.backend().find("Text", "text", "RevokeRole:").has_value());
}

// Mutation: keep the limit input's decimals at 2 for every scope (typing 12.5 for the yen company is accepted).
TEST_CASE("lims org screens: a limit for the yen company takes whole yen", "[lims-screens][org]") {
    DbFixture fixture;
    lims::screens::seedScenario();
    ScreenHarness harness{limsApp(), std::string{lims::screens::kAccess}, RigMode::Local, {},
                          LocalSetup{.db = {}, .user = "julia"}};
    harness.click("user-kenji");
    harness.chooseField("addAssignment", "scope", R"("Rheinlab Japan K.K.")");
    CHECK(prop(harness, "TextInput", "addLimit", "unit") == "¥");
    CHECK(prop(harness, "TextInput", "addLimit", "decimals") == "0");
}

// Mutation: drop changes<SiteView> from CreateSite (the new site never appears in the sites table).
TEST_CASE("lims org screens: a site created on the companies screen appears in its table", "[lims-screens][org]") {
    DbFixture fixture;
    lims::screens::seedScenario();
    ScreenHarness harness{limsApp(), std::string{lims::screens::kCompanies}, RigMode::Local, {},
                          LocalSetup{.db = {}, .user = "julia"}};
    REQUIRE(harness.waitForText("siteList", "Leverkusen"));
    harness.editField("siteForm", "name", "Bonn");
    REQUIRE(harness.submitForm("siteForm"));
    CHECK(harness.waitForText("siteList", "Bonn"));
    reportProblems(harness);
    CHECK(harness.problems().empty());
}
```

The screen harness runs one client, so "another client" is measured where the harness can: a change made through one part of a screen reaches another part only through the action's declared effect, which is the mechanism a second client relies on; the remote fan-out itself is measured by the model suites over `BackendRig::connect`. `limsApp`, `reportProblems` and `prop` are `screen_test_support.hpp`'s (Task 2); the file includes it and adds `using namespace lims::screens::test;`. "Dr. Anna Meyer" and "Julia Schneider" are the display names `seedOrgScenario()` gives (Task 3). The test ids (`userList`, `user-<name>`, `assignmentScope`, `assignmentLimit`, `revoke-<role>`, `refusalDialog`, `addAssignment`, `addLimit`) are the screen's; give them in Step 3.

- [ ] **Step 3: Build the two screens**

`accessScreen()` in `org_screens.cpp`, to mockup 32: `kit::pageHead(screen, "org", t("lims.org.access.title", "Users and roles"))`; the users table (`userList`, one row per user from `ListUsers`, the cell a column of the display name (`kit::personName`) and the roles as quiet badges, a row's key the principal, `selectionMode = Single`, each row's test id `user-<principal>`); beside it (expanded) or under it (compact, `kit::width` and `ui::byWidth`), the selected user's panel: their assignments (`ListRoleAssignments` filtered by `filter` over the selected principal), each row with the role in words (`kit::statusWord<lims::LimsRole>`), the scope by name (the company's or site's name from `ListCompanies`/`ListSites` through a `map` over the rows, never the id), the limit for that role and company from `ListValueLimits` (matched by principal, company and role; `kit::money` in the company's currency), the duty rules as catalogue sentences per role (`lims.org.duty.InvoiceApprover`: "Cannot approve an invoice they created or submitted."), and a revoke button per row (`revoke-<role>`); the inline add row (`addAssignment`: role select, scope select over `ListMyScopes`, limit input `addLimit` whose `unit` is the chosen scope's currency symbol and whose `decimals` is bound to its `decimals`, Add); a `dialog` (`refusalDialog`) opened by the revoke mutation's refusal, showing `kit::refusalBanner`. `companiesScreen()`, to mockup 31: the context's company from `ListCompanies`; a `keyValue` of its settings with "fixed" badges; the number series panel; the posting accounts panel from `GetBillingSettings` and `GetInventorySettings` with an empty state ("Billing is not set up for this company.") when a settings read is `NotFound`; the sites table and the create-site form. Both screens show a `busy` while their first query loads and the kit's refusal banner for a failed query.

`RevokeRole`'s last-supervisor refusal gets the code `lims.org.lastSupervisor` (added to `kOrgRefusalCodes`) with arguments `principal` (`OrgModel::displayNameOf` of the person) and `company` (the company's name, read where the refusal is thrown); both org catalogue files get its words ("{$principal} is the last administrator of {$company}. Make someone else an administrator first." / "{$principal} ist die letzte Administratorin bzw. der letzte Administrator von {$company}. Ernennen Sie zuerst eine weitere Person.").

`org_scenario.cpp` gains what the two mockups show beyond Task 2's seed: the requisition approver limits (anna up to €5,000.00 as `PurchaseApprover`), and any site, role or limit the mockups list that Task 2 did not. `grabs/org.tsv` keeps the two rows, with the person each mockup shows (`julia`).

- [ ] **Step 4: Build, run and check the production criteria**

Run: Build lims, Screen suite `"[org]"` and `"[kit]"`, Model suite `"[org]"`, Render smoke. Then, for both screens, check and write down: loading (a `busy` while the first query runs), empty (a company with no sites shows the empty state), error (a refused query shows words), no raw id or enum name (the case above, and reading the grab), keyboard (Tab reaches every control in reading order; Enter submits the add row; focus is visible on Qt Quick), labels (every control has an accessible name: `harness.findAll` over controls with an empty `a11y` name finds none), both widths, and currency (a yen limit shows `¥`, no decimals). Run each mutation named in the file once and restore it.

- [ ] **Step 5: Grab beside the mockups**

`scripts/shoot_lims.sh /tmp/claude-1000/-home-yaraslau-repo-morph/b1b73e49-c41d-4e27-bb68-b8c0cfacf145/scratchpad/lims-grabs lims.org.companies lims.org.access`, and compare each pair at both widths. Classify every difference (Conventions). Fix every screen difference now; add every framework difference to the framework task its subject belongs to (re-opened in this branch) or to a new framework task, which runs before this one is called done; record every domain difference in decision 54. Repeat until the list holds only domain and environment items.

- [ ] **Step 6: Record decision 54 and commit**

```markdown
### 54. The organisation screens match their mockups; settings with no action to change them are shown fixed

Companies (mockup 31) and users (mockup 32) are rebuilt to their mockups. A company's base currency and fiscal year
start are shown fixed, since no action changes them; the posting accounts are read from billing's and inventory's
settings, each shown "not set up" when absent. A user's roles show their scope by name, their limit in the scope's
company currency, and the duty rules of the role; people are shown by their display names. Removing a company's last
administrator is refused in words that name the person and the company. A limit input's decimals follow the chosen
scope's currency through ui/1's bound `TextInput.decimals`, the client half of the per-instance decimals
`InstanceConstraints` serves. [Domain differences from the grabs, one sentence each.]
```

```bash
cd /home/yaraslau/repo/morph
git add examples/lims/screens examples/lims/org/src/models/org_model.cpp examples/lims/org/include/lims/org/core/errors.hpp \
        examples/lims/README.md
git commit -m "Build lims's organisation screens to their mockups

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01No1NznqxEBvjhZBaBRLCd6"
```

---
### Task 14: Lab — samples, sample detail, result entry, verification and audit (mockups 14–18)

**Waits for** Tasks 2, 5, 7 and 9; Task 18 (which moves `ListSamples` onto the database plan), or Task 17's refuted verdict where Task 18 is skipped; Part C Task 5 (it writes `lab.ResultCaptured` in `applyCapture`, which this task also edits, and reads the captures' payload). **Never in flight with** Part C Task 5, Task 18 or Part B Task 19.

**Files:**
- Create: `examples/lims/screens/tests/test_lims_lab_screens.cpp`, `examples/lims/lab/include/lims/lab/core/refusal_codes.hpp`
- Delete (once their tests have moved): `registerScreen`, `lifecycleScreen`, `resultsScreen` from `lims_screens.cpp`, and `kRegister`, `kLifecycle`, `kResults` from `lims_screens.hpp`; `addLegacyScreens` and the legacy id list drop them; `screens/tests/test_lims_screens.cpp`'s cases move to `test_lims_lab_screens.cpp`
- Modify: `examples/lims/lab/include/lims/lab/dto/result_dto.hpp` (`CaptureConcentration::reason`), `examples/lims/lab/src/models/sample_model.cpp` (a reason is required to replace a captured value), `examples/lims/tests/test_result_entry.cpp`, `examples/lims/screens/lab_screens.hpp`/`.cpp` (Task 2 created them empty: the five screens, `addLabScreens`, `labScreenIds`), `examples/lims/screens/scenario/lab_scenario.hpp`/`.cpp` (1,284 samples, the mockups' results), the lab catalogue files, `examples/lims/screens/grabs/lab.tsv`, the kit's refusal-code case (adds `kLabRefusalCodes`), `examples/lims/README.md` (decision 55)

**Interfaces:**
- Consumes: `SampleModel` (`ListSamples` with `TableQuery`, `GetSample`, the transitions, `CaptureConcentration`, `ListResults`, `VerifyResult`, `ListVerifications`, `GetAuditTrail`, `OpenSample`), `AnalysisCatalogModel` (`GetAnalysisSchema` for the result form), `InvoiceModel::ListBillableLines` (the billing a sample causes), the gallery's screens as designs (`examples/lims_gallery/src/screens/samples.cpp`, `results.cpp`, `review.cpp`, `esignature.cpp`, `audit.cpp`), the kit (`personName` for who did what), the shell's context, Task 9's `Step::detail`, `screen_test_support.hpp`.
- Produces: `samplesScreen()`, `sampleDetailScreen()`, `resultEntryScreen()`, `verificationScreen()`, `auditScreen()`; `CaptureConcentration::reason` (`std::string`, at most 256 bytes, required when the capture replaces a value already captured for the same analysis version); refusal codes `lims.lab.reasonRequired`, `lims.lab.sameActor`, `lims.lab.illegalTransition` (in `kLabRefusalCodes`).

The lab screens put the gallery's designs 05, 02/10, 08, 09 with the e-signature, and 04 into the shell over lims's real models, keyed by sample rather than worksheet (decision 19). The gallery's screens are backend-free studies and stay unchanged; their layout code is the design to follow, rebuilt over queries and mutations.

Two mockup elements are lims domain, decided here: the result-entry reason-for-change dialog needs somewhere to keep the reason — a regulated lab records why a captured value was replaced — so `CaptureConcentration` gains `reason`, required by the model when the capture replaces a value (`lims.lab.reasonRequired`), and the audit trail shows it from the journal. The e-signature's password and its wrong-password state are not built: lims signs in by name (`LimsLogin`) and has no password to re-enter; the signature dialog confirms the meaning of the act ("I verify these results as correct") and records it as `VerifyResult` does now. Both are recorded in decision 55. The sample detail's reagents section (Part C Task 6's reservations, with the lost race naming its holder) is Task 22's, which adds it to this screen.

- [ ] **Step 1: Read the mockups and the gallery screens; list what each element needs**

For each of `14-Samples`, `15-SampleDetail`, `16-ResultEntry`, `17-Verification`, `18-Audit`, and its gallery screen, write each element and its ui/1 node in the scratch review file. The README's list for these screens: a table cell holding two lines or several badges (a `column` or a `forEach` in the cell: expressible); tinted rows by a condition (`TableRowView` tone: expressible); a selection bar with "select all 1,284" beyond the page (`table` selection plus a text over the query's total: expressible); filter chips that remove a filter (`badge` with `onRemove`: expressible); a facet column with counts (a `column` of checkboxes with counts from the query: expressible); `steps` with a second muted line per step (time and person): `Step::detail`, Task 9; editable numeric cells in a grouped table with Enter moving to the next cell and a value-or-qualifier cell (the gallery's `results.cpp` does it: expressible); a dialog opened by a cell's change that can undo it (the gallery's: expressible); the signature dialog's password with an attempt count (domain: not built); a queue item disabled with its reason as text (expressible). Compact: the sample detail's panels become collapsible sections with a summary header, and result entry's Save and verification's Sign sit in a pinned footer (Conventions); name any other screen that needs them.

- [ ] **Step 2: Write the failing screen tests**

`test_lims_lab_screens.cpp`, every case in a `lims::test::LabFixture` with `lims::screens::seedScenario()` first. The first, in full:

```cpp
// Mutation: page ListSamples in the screen by fetching every row and slicing in an expression (the "1,284 samples"
// count still shows, but the query's page size in the request is 0, which the check on the request reads).
TEST_CASE("lims lab screens: the sample list pages 1,284 samples and selects across pages", "[lims-screens][lab]") {
    lims::test::LabFixture const fixture;
    lims::screens::seedScenario();
    ScreenHarness harness{limsApp(), std::string{lims::screens::kSamples}, RigMode::Local, {},
                          LocalSetup{.db = {}, .user = "manpreet"}};
    REQUIRE(harness.waitForText("sampleCount", "1,284 samples"));
    CHECK(harness.findAll("Row", "sampleRow").size() == 50);
    harness.click("selectPage");
    CHECK(harness.waitForText("selectionBar", "50 selected"));
    harness.click("selectAll");
    CHECK(harness.waitForText("selectionBar", "All 1,284 selected"));
    reportProblems(harness);
    CHECK(harness.problems().empty());
}
```

Then, each with its mutation in a `// Mutation:` line and written the same way:

- "the sample list filters by status and a chip removes the filter" (mutation: the chip's `onRemove` does nothing);
- "a row with a result out of specification is tinted" (mutation: no row tone);
- "the sample detail shows the lifecycle with who and when per step" (`Step::detail`; mutation: detail omitted);
- "a transition another client made reaches an open sample detail" (the detail's `GetSample` refetches on `updates<SampleView>` from a transition run through the screen's own lifecycle buttons in a second mounted detail of the same sample; mutation: `ReceiveSample` declares no effect);
- "the sample detail lists the billing the sample will cause" (`ListBillableLines` for the sample; mutation: filter by client instead);
- "result entry: Enter moves to the next cell, and a value below the detection limit shows as `< LOD`" (mutation: Enter submits without moving);
- "result entry: replacing a value asks for the reason, and Cancel undoes the edit" (mutation: the dialog's Cancel keeps the edit);
- "result entry: a replacement without a reason is refused in words" (the model's `lims.lab.reasonRequired`; mutation: the model accepts it);
- "verification: the verifier's own capture is disabled with its reason as text" (mutation: the item is enabled);
- "verification: signing verifies the selected results" (mutation: the dialog's Sign runs nothing);
- "audit: facets narrow the trail and chips show the active ones" (mutation: the facet's change does not reach the query);
- the old `test_lims_screens.cpp` cases, moved, with their ids changed to the new screens' (registration through the samples screen's "New sample" dialog, the lifecycle through the sample detail, capture and verification through result entry and verification).

Add a model case to `tests/test_result_entry.cpp`: "result entry: replacing a captured value needs a reason, kept in the journal" (mutation: drop the check).

- [ ] **Step 3: The model's reason, then the five screens**

`CaptureConcentration` gains, last, `std::string reason;` with `"reason"` added to its `optionalFields`; `validate()` keeps every existing condition and adds `reason.size() <= 256`. `applyCapture` refuses with `ValidationError{"lims.lab.reasonRequired", {}, "CaptureConcentration: replacing a captured value needs a reason"}` when a result for the same analysis version exists and the reason is empty (the existing loop that deletes the earlier answer is where it knows). The audit trail's step for a capture shows the reason from the journalled payload. Part C Task 5 already writes `lab.ResultCaptured` after `mapper.Update(sampleRow)` in the same function; the reason check runs earlier, in the loop, so a refused replacement writes no event, and the event's payload (read from the result row) does not carry the reason. This changes the action's journal fingerprint; lims's shipped code never replays an older journal (README, "Expected strain points"), so no migration is registered — say so in decision 55.

The screens, each a function in `lab_screens.cpp` with a doc comment naming its mockup and gallery design:

- `samplesScreen()` (14): the page head (eyebrow, title, a "New sample" action opening a dialog with the registration fields, which runs `RegisterSample` at the context's site); filters (search, status chips, priority) feeding the `ListSamples` query's `TableQuery` (sort, filters, page of 50); the table (`sampleRow` per row: the reference and client as two lines, the status badge in words, the tests as quiet badges, the due day, the out-of-specification tint; `selectColumn`); the selection bar (`selectionBar`: count, "Select all 1,284" over the query's total, bulk actions available to the role); the pager; the compact layout as cards with the same selection; loading, empty ("No samples at Köln yet") and refused states.
- `sampleDetailScreen()` (15), param `sampleId`: `OpenSample` on mount; the lifecycle as `steps` with each step's detail (person and day from `GetAuditTrail`); the analyses and results; the transitions the state allows, each a button whose refusal shows in words; the billing the sample causes (`ListBillableLines`, money in the line's currency); the chain of custody (the trail's steps); a place for the reagents section, which Task 22 fills.
- `resultEntryScreen()` (16), param `sampleId`: the queue of the analyst's samples in progress beside the grid of analyses; per analysis the value-or-qualifier cell (a `textInput` with the version's unit and decimals from `GetAnalysisSchema`, and a qualifier select), Enter moving focus to the next cell (`keys` with `focus`); a change to a captured value opens the reason dialog (`watch` on the cell), whose Save runs `CaptureConcentration` with the reason and whose Cancel restores the cell.
- `verificationScreen()` (17): the queue of samples to verify; an item the verifier captured disabled with "You captured this result" as text; the results with their flags; the signature dialog confirming the meaning and running `VerifyResult` for each selected result.
- `auditScreen()` (18), param `sampleId` (optional; all the caller's samples otherwise): facets (who, action, day range) with counts beside the read-only trail table; active facets as removable chips; an export action (`downloadToFile` of the visible rows as CSV).

Register the five in `addLabScreens` with module `lab` and list their ids in `labScreenIds()` (the render smoke reads them); remove the three old screens from `addLegacyScreens` and the legacy id list once their moved tests pass. `lab_scenario.cpp` gains the lab records the mockups show (the 1,284 samples are generated: the eight the gallery design names, then routine ones with references and days that sort as the design's).

- [ ] **Step 4: Build, run, check the production criteria, measure the list**

Run: Build lims, Model suite `"[result]"` and `"[events]"`, Screen suite `"[lab]"`, Render smoke. Check each screen against the production criteria as Task 13 Step 4 lists them. Measure the sample list at 10,000 samples (seed 10,000 instead of 1,284 in a `Stress:`-named case, label `stress`): the time from a sort or filter change to the page shown, three runs, written down as measured. `ListSamples` pages in the database if Task 18 moved it (hypothesis 10 confirmed in Task 17), and in memory otherwise; record which and the numbers in decision 55.

- [ ] **Step 5: Grab beside the mockups**

`scripts/shoot_lims.sh` for the five screens (their rows added to `grabs/lab.tsv`, each with the person the mockup shows), compared and classified until only domain and environment items remain.

- [ ] **Step 6: Record decision 55 and commit**

```markdown
### 55. The lab screens are the gallery's designs over lims's models; a replaced value keeps its reason

The sample list, sample detail, result entry, verification and audit screens follow the gallery's designs 05, 02/10,
08, 09 and 04 in the shell, keyed by sample. A capture that replaces a value needs a reason, which the journal keeps
and the audit trail shows; the action's fingerprint changed with it, and lims's shipped code replays no older journal,
so no migration is registered. The e-signature confirms the meaning of verifying and asks for no password: lims signs
in by name and has none to re-enter. A lifecycle step shows who and when, by name, which ui/1 says (`Step::detail`).
The sample list pages [in the database / in memory over every visible sample]: measured at 10,000 samples, a sort or
filter change shows its page in [n] ms (three runs: [n], [n], [n]). [Domain differences from the grabs.]
```

```bash
cd /home/yaraslau/repo/morph
git add examples/lims/screens examples/lims/lab examples/lims/tests/test_result_entry.cpp examples/lims/README.md
git commit -m "Build lims's lab screens from the gallery's designs over the real models

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01No1NznqxEBvjhZBaBRLCd6"
```

---

### Task 15: Billing — price lists, invoices, the invoice editor and billing runs (mockups 19–21, 23; hypothesis 9, first instance)

**Waits for** Tasks 5, 7, 8 and 9; Part B Task 14; Part B Task 19, or Part B Task 17's no-seam branch (this task edits `billing_authorization.*`, `declared_requirements.hpp` and `test_declared_authorization.cpp`, which Task 19 rewrites). **Never in flight with** Part B Task 19.

**Files:**
- Create: `examples/lims/screens/tests/test_lims_billing_screens.cpp`
- Modify: `examples/lims/billing/include/lims/billing/dto/price_dto.hpp` and `price_list_model.hpp`/`.cpp` (`SetPrices`, one draft saved whole), `examples/lims/billing/include/lims/billing/dto/run_dto.hpp` and `billing_run_model.hpp`/`.cpp` (`ListBillingRunItems`), `examples/lims/billing/include/lims/billing/dto/invoice_dto.hpp` and `invoice_model.hpp`/the invoice sources (`InvoiceView::clientName`, `ListInvoiceLeases`, `ListInvoicesResult::total`), `examples/lims/billing/src/models/*.cpp` (the refusals the mockups show get codes, listed in `billing/core/refusal_codes.hpp`), `examples/lims/tests/test_price_lists.cpp`, `examples/lims/tests/test_billing_run.cpp`, `examples/lims/billing/include/lims/billing/authorization/billing_authorization.hpp`/`.cpp`, `declared_requirements.hpp`, `test_declared_authorization.cpp`, `examples/lims/screens/billing_screens.hpp`/`.cpp` (Task 2 created them empty: the four screens, `addBillingScreens`, `billingScreenIds`), `examples/lims/screens/scenario/billing_scenario.hpp`/`.cpp`, the billing catalogue files, `examples/lims/screens/grabs/billing.tsv`, `examples/lims/README.md` (decision 56)

**Interfaces:**
- Consumes: `PriceListModel` (`ListPriceLists`, `CreatePriceList`, `SetPrice`, `QuotePrice`, `GetBillingSettings`), `InvoiceModel` (`ListInvoices`, `GetInvoice`, `GetInvoiceLease`, `CreateInvoice`, `SubmitInvoice`, `ApproveInvoice`, `ListPendingApprovals`, `PostInvoice`), `InvoiceEditModel` (`LeaseInvoice`, `ReleaseInvoice`, `RemoveInvoiceLine`, `SetInvoiceReference`), `BillingRunModel` (`ListBillingRuns`, `StartBillingRun`), `LedgerModel::ListPeriods` (the closed-period refusal's words), `BillingClientView::name` (`settings_dto.hpp`), `OrgModel::ListUsers` (names), the kit (`personName`, `refusalBanner`), the shell's context, Task 8's bound decimals, Task 9's `Step::detail`, `screen_test_support.hpp`.
- Produces: `priceListsScreen()`, `invoicesScreen()`, `invoiceEditorScreen()`, `billingRunsScreen()`; `SetPrices { PriceListId priceList; std::vector<PriceEntry> prices; }` (`PriceEntry { AnalysisId analysis; ClientId client; Amount amount; }`) → `PriceListView`, every price set in one transaction or none; `ListBillingRunItems { BillingRunId run; }` → `ListBillingRunItemsResult { std::vector<BillingRunItemView> items; }` (`BillingRunItemView { ClientId client; std::string clientName; SiteId site; std::string state; InvoiceId invoice; std::string error; }`, the state through the kit's words); `InvoiceView::clientName` (`std::string`, the billing client's `BillingClientView::name`, read where the view is built); `ListInvoiceLeases { CompanyId company; }` → `ListInvoiceLeasesResult { std::vector<InvoiceLeaseView> leases; }` (the live leases of the company's drafts); `ListInvoicesResult::total` (`Amount`, the exact sum of the filtered set's gross); refusal codes `lims.billing.invoiceLeased` (Task 5), `lims.billing.periodClosed`, `lims.billing.deletedMeanwhile`, `lims.billing.notDraft`, `lims.billing.overLimit`, `lims.billing.sameActor`.

The invoice editor is hypothesis 9's first instance: a screen whose header, lines grouped by analysis and expandable to their samples, totals and approval route come from billing's `InvoiceModel`, whose lease and edits come from the editor's private `InvoiceEditModel`, and whose page turns read-only while another principal holds the lease — two models of one module. What hypothesis 9 says ui/1 lacks is an *action* spanning several models; the measurement is whether the editor needs one. The screen is built with ui/1 as it stands; the decision records whether any of its operations had to commit across both models in one action (expected: none — each edit is the edit model's, each transition the invoice model's).

The price list's "next year's draft edited in place" is an input per row saved as one draft: a domain action, `SetPrices`, sets every price of one list in one transaction, so the draft is saved whole or not at all. The billing runs' per-client outcomes need the run's items, which billing holds and does not list: `ListBillingRunItems` reads them.

- [ ] **Step 1: Read the mockups; list what each element needs**

`19-PriceLists`, `20-Invoices`, `21-InvoiceEditor`, `23-BillingRuns`. The README's list: a table whose column is an input per row with a computed change column (`textInput` cells and an expression column: expressible; saved by `SetPrices`); a row badge naming the lease holder (a `GetInvoiceLease` per row would be one query per row — the invoice list reads the leases in one query instead: add `ListInvoiceLeases { CompanyId company; }` → the live leases of the company's drafts to billing, a read); status counts in the segmented filter (counts over the list: expressible with `filter` and `count`); totals under a paged list (`sum` over the page's gross is not the total of all pages: `ListInvoices` answers the total of the filtered set — add `total` to `ListInvoicesResult`, a domain read); a refusal that offers to change the scope (a banner with an action that runs Task 6's command to choose a context option); the editor's multi-model page, read-only while leased (`enabled` bound to "this editor holds the lease": expressible); a field error and a banner from one refused action (the refusal's code decides which field: expressible with `refusal()`); live progress of a server job with a resume note (`progress` over `done`/`total`, refreshed by the run's declared effect: expressible); a nested per-client table under the selected run (expressible). Compact: the invoice editor's lines become collapsible sections per analysis with a summary header, and its totals with Submit sit in a pinned footer; the price list's Save sits in a pinned footer (Conventions).

- [ ] **Step 2: Write the failing tests**

Model cases first: `test_price_lists.cpp` "price lists: a draft's prices are saved whole or not at all" (two prices, the second for an analysis the list's currency cannot price — refused; neither stored; mutation: one transaction per price); `test_billing_run.cpp` "billing run: its items list each client's outcome" (mutation: `ListBillingRunItems` answers only done items).

Screen cases in `test_lims_billing_screens.cpp`, the first two in full:

```cpp
// SPDX-License-Identifier: Apache-2.0
//
// lims's billing screens: price lists, invoices, the invoice editor and billing runs, against their mockups.

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <string>
#include <testkit/screen_harness.hpp>

#include "billing_test_support.hpp"
#include "lims/billing/models/invoice_edit_model.hpp"
#include "lims_screens.hpp"
#include "scenario/billing_scenario.hpp"
#include "scenario/scenario.hpp"
#include "screen_test_support.hpp"

namespace {

using lims::screens::test::limsApp;
using lims::screens::test::prop;
using lims::screens::test::reportProblems;
using morph::client::LocalSetup;
using morph::examples::testing::RigMode;
using morph::examples::testing::ScreenHarness;

}  // namespace

// Mutation: show the invoice's id for a draft (the check for "Draft for Stadtwerke Köln" fails and "#3" appears).
TEST_CASE("lims billing screens: drafts are named by their client, posted invoices by their number, leases by holder",
          "[lims-screens][billing]") {
    lims::test::LabFixture const fixture;
    lims::screens::seedScenario();
    ScreenHarness harness{limsApp(), std::string{lims::screens::kInvoices}, RigMode::Local, {},
                          LocalSetup{.db = {}, .user = "lena"}};
    REQUIRE(harness.waitForText("invoiceList", "Draft for Stadtwerke Köln"));
    CHECK(harness.waitForText("leaseBadge", "Markus Weber is editing"));
    CHECK(harness.waitForText("statusCounts", "Drafts 3"));
    CHECK(harness.waitForText("listTotal", "€48,211.70"));
    CHECK_FALSE(harness.backend().find("Text", "text", "#3").has_value());
    reportProblems(harness);
    CHECK(harness.problems().empty());
}

// Hypothesis 9, first instance. Mutation: bind the editor's inputs' `enabled` to true (lena types into markus's draft).
TEST_CASE("Hypothesis 9: the invoice editor turns read-only while another clerk holds its lease, and says who",
          "[lims-screens][billing][h9]") {
    lims::test::LabFixture const fixture;
    lims::screens::seedScenario();
    lims::InvoiceEditModel markusEditor;
    {
        lims::test::ScopedPrincipal const markus{"markus"};
        static_cast<void>(markusEditor.execute(lims::LeaseInvoice{.invoiceId = lims::screens::scenarioDraftInvoice()}));
    }
    ScreenHarness harness{limsApp(),
                          std::string{lims::screens::kInvoiceEditor},
                          RigMode::Local,
                          {{"invoiceId", std::to_string(*lims::screens::scenarioDraftInvoice())}},
                          LocalSetup{.db = {}, .user = "lena"}};
    CHECK(harness.waitForText("leaseBanner", "Markus Weber is editing this invoice."));
    CHECK(prop(harness, "TextInput", "reference", "enabled") == "false");
    CHECK(prop(harness, "Button", "removeLine", "enabled") == "false");
    reportProblems(harness);
    CHECK(harness.problems().empty());
}
```

`lims::screens::scenarioDraftInvoice()` is a function `scenario/billing_scenario.hpp` declares: the id of the scenario's draft for Stadtwerke Köln (the scenario's records are named by the mockups; a test asks the scenario for the ids it needs, and never hard-codes one). "Markus Weber" is markus's display name (Task 3), shown through `kit::personName`. The parameter map's literal form is `ui::ParamMap`'s; read `screen_harness.hpp`'s constructor and use its type. Then, each with its mutation, written the same way:

- "the price list's draft is edited in place and saved as one draft, with the change shown per row" (mutation: Save runs `SetPrice` per row);
- "a yen price list takes whole yen" (Task 8's bound decimals, from the list's currency; mutation: decimals 2);
- "the invoice list's out-of-scope refusal offers to change the scope" (mutation: no action on the banner);
- "the invoice list shows loading, then an empty state for a new company" (mutation: no empty state);
- "the editor's lines are grouped by analysis and expand to their samples" (mutation: lines not grouped);
- "the editor leases on open, releases on close, and another editor's list hears the release" (`ReleaseInvoice`'s `changes<InvoiceLeaseView>`; mutation: the editor does not release on close);
- "a submission another client made reaches an open approvals list" (the existing `test_lims_approval_inbox.cpp` measurement, kept);
- "the editor's draft deleted meanwhile shows words and a way back" (`NotFound` with `lims.billing.deletedMeanwhile`; mutation: the raw refusal shows);
- "posting into a closed period is refused in words" (`lims.billing.periodClosed` with the period's month; mutation: the message shows);
- "the billing run in progress shows its progress and resume note, and each client's outcome with the failure in words" (mutation: progress over a fixed total).

- [ ] **Step 3: The domain reads, then the four screens**

`SetPrices` (billing clerk at the list's company; `changes<PriceView>`; every price validated before any is written, then one transaction), `ListBillingRunItems` (a read; billing clerk at the run's company sees it), `ListInvoiceLeases` (a read), `ListInvoicesResult::total` (the exact sum of the filtered set's gross in the company's currency, `ExactSum`; `OutOfRange` when it does not fit) — each with its model test, its declared authorization where it mutates, and the coverage case. The refusals the mockups show get their codes and both catalogues' words.

`invoicesScreen()`, in full, as the pattern the other three follow:

```cpp
ui::Screen invoicesScreen() {
    return ui::screen(std::string{kInvoices}, [](ui::Scope& screen) {
        auto const width = kit::width(screen);
        kit::pageHead(screen, "billing", ui::t("lims.billing.invoices.title", "Invoices"));
        auto const context = screen.let("context", ui::env("context"));
        auto const company = context.at("company");
        auto const invoices = screen.model<InvoiceModel>("invoices");
        auto const org = screen.model<OrgModel>("org");
        auto const users = screen.query<ListUsers>("users", org, {});
        auto const status = screen.state("status", std::string{"all"});
        auto const listed =
            screen.query<ListInvoices>("listed", invoices, {{&ListInvoices::company, company}});
        auto const leases =
            screen.query<ListInvoiceLeases>("leases", invoices, {{&ListInvoiceLeases::company, company}});
        auto const all = listed.value().at(&ListInvoicesResult::invoices);
        auto const shown = screen.let(
            "shown", ui::filter(all, "invoice", ui::or_(ui::eq(status, "all"), ui::eq(ui::var("invoice").at("state"), status))));
        auto const leaseOf = [&](ui::AnyHandle const& invoice) {
            return ui::first(ui::filter(leases.value().at(&ListInvoiceLeasesResult::leases), "lease",
                                        ui::eq(ui::var("lease").at("invoice"), invoice.at("id"))));
        };
        auto const nameOf = [](ui::AnyHandle const& invoice) {
            return ui::cond(ui::isEmpty(invoice.at("number")),
                            ui::t("lims.billing.draftFor", "Draft for {$client}", {{"client", invoice.at("clientName")}}),
                            invoice.at("number"));
        };
        auto const editor = ui::element("open");
        return ui::column(
            {kit::refusalBanner(ui::coalesce({listed.refusal(), leases.refusal()})),
             ui::busy(listed.pending(), ui::t("lims.loading", "Loading…")),
             ui::select({.options = kit::statusFilter<InvoiceState>(all), .selected = status,
                         .onSelect = ui::setTo(status), .style = morph::ui::SelectStyle::Segmented})
                 .testId("statusCounts")
                 .a11y("Invoice status"),
             ui::emptyState({.title = ui::t("lims.billing.noInvoices", "No invoices yet"),
                             .text = ui::t("lims.billing.noInvoicesText",
                                           "Invoices appear here once a billing run or a clerk drafts them.")})
                 .visible(ui::and_(ui::not_(listed.pending()), ui::isEmpty(all))),
             ui::byWidth(width,
                          {kit::invoiceCards(shown, nameOf, leaseOf)},
                          {ui::table(shown,
                                     {.key = ui::row("id"),
                                      .columns = {{.name = "name", .label = ui::t("lims.billing.invoice", "Invoice"),
                                                   .cell = ui::text({.text = nameOf(ui::row())})},
                                                  {.name = "state", .label = ui::t("lims.billing.status", "Status"),
                                                   .cell = kit::invoiceStateBadge(ui::row("state"))},
                                                  {.name = "lease", .label = "",
                                                   .cell = kit::leaseBadge(leaseOf(ui::row()), users.value()).testId("leaseBadge")},
                                                  {.name = "gross", .label = ui::t("lims.billing.gross", "Gross"),
                                                   .cell = ui::text({.text = kit::money(ui::row("gross"),
                                                                                        ui::row("currency"))})}},
                                      .onActivate = ui::navigate(std::string{kInvoiceEditor},
                                                                 {.params = ui::obj({{"invoiceId", ui::row("id")}})})})
                               .testId("invoiceList")
                               .a11y("Invoices")}),
             ui::text({.text = ui::t("lims.billing.total", "Total {$amount}",
                                     {{"amount", kit::money(listed.value().at(&ListInvoicesResult::total),
                                                            context.at("currency"))}})})
                 .testId("listTotal")},
            {.gap = 3});
    });
}
```

This is the shape, not a transcript of a working build: the author API's exact spellings for the pieces it uses — `screen.state`, `ui::filter`, `ui::var`, `ui::or_`/`ui::and_`/`ui::not_` (the header may spell them as operators `||`, `&&`, `!` on `Handle<bool>`, as `org_screens.cpp` and the gallery do), `ui::setTo`, `query.pending()` (the handle's loading state; there is no `loading()`), `query.refusal()` (Task 4), `t` with named arguments — are `author.hpp`'s; read it, use what it declares, and keep the structure: the context's company in every query's body, every text through the kit or a catalogue key, the empty and loading states, the refusal banner, the table's `onActivate` opening the editor, and compact cards (`kit::invoiceCards`, a kit function over `forEach`) in place of the table. `kit::statusFilter<E>` (segmented options with each state's count over the rows), `kit::invoiceStateBadge` and `kit::leaseBadge(lease, users)` (`lims.billing.leaseHolder`, "{$holder} is editing", the holder through `kit::personName`, hidden without a live lease) are added to the kit with complete doc comments.

`priceListsScreen()` (19): the context company's lists by validity, the selected list's prices, next year's draft editable in place (an input per row, the change against the list in force as a computed column, Save running `SetPrices`), a yen list taking whole yen. `invoiceEditorScreen()` (21), param `invoiceId`: a private `InvoiceEditModel` alias (`.instance = ui::Instance::Private`), `LeaseInvoice` on mount and `ReleaseInvoice` on close; the header (client, site, reference input), lines grouped by analysis with a heading row per analysis and the samples beneath (`TableRowView` headings, collapsible by a state per analysis), totals (net, tax, gross in the invoice's currency), the approval route (`steps` with Task 9's detail, each person by name: drafted by, submitted by, approver due by the deadline, escalated after it), Submit / Approve / Post each enabled by the invoice's state and the caller's role, the page's inputs enabled only while this editor holds the lease (a `leaseBanner` naming the holder by `kit::personName` otherwise: `lims.billing.leasedBy`, "{$holder} is editing this invoice."), and the refusals in words. `billingRunsScreen()` (23): the schedule (billing day from the settings), the run in progress with `progress` over done/total and a resume note when its start is before the server's last start (the run's `startedAt` against the newest run tick, a field `BillingRunView` carries — add `lastTickAt` to it if it does not), past runs, and the selected run's items (`ListBillingRunItems`) with each failure's text in words.

- [ ] **Step 4: Build, run, check the production criteria**

Run: Build lims, Model suite `"[billing]"`, `"[authorization]"`, Screen suite `"[billing]"`, Render smoke. Check each screen against the production criteria (Task 13 Step 4's list), the list totals and the lease badge with a second clerk holding a lease. Run every mutation once and restore it. Write down for the h9 decision: did any editor operation need one action across `InvoiceModel` and `InvoiceEditModel` (expected: no — name each operation and its one model), and did the screen meet every production criterion with ui/1 as it stands (after the framework Tasks 4, 6, 8 and 9 added)?

- [ ] **Step 5: Grab beside the mockups**

`scripts/shoot_lims.sh` for the four screens, compared and classified until only domain and environment items remain; a framework difference is added to the framework task its subject belongs to and fixed before this task is done.

- [ ] **Step 6: Record decision 56 and commit**

```markdown
### 56. The billing screens match their mockups, and the invoice editor needed no action across models (hypothesis 9, first instance)

Price lists (19), invoices (20), the invoice editor (21) and billing runs (23) are built to their mockups. A price
list's draft is saved whole by `SetPrices`; the invoice list reads its leases and its total in one query each
(`ListInvoiceLeases`, `ListInvoicesResult::total`); a run's per-client outcomes are `ListBillingRunItems`.

**Hypothesis 9 is [refuted | confirmed | mis-framed] for the invoice editor.** Built with ui/1 as it stands (with
refusals in words, the shell's context, a bound input's decimals and a step's second line added to morph on the way):
its header, grouped lines, totals and approval route are `InvoiceModel`'s, its lease and edits the editor's private
`InvoiceEditModel`'s, and [no operation needed one action across the two: each edit is one model's action, each
transition the other's | the operation that needed both was …]. [If mis-framed: the domain put every commit in one
model's action, so the question whether ui/1 can span models was never exercised by this screen; say what would
exercise it.] The page turns read-only while another clerk holds the lease, and says who, by name.
[Domain differences from the grabs.]
```

```bash
cd /home/yaraslau/repo/morph
git add examples/lims/screens examples/lims/billing examples/lims/app/include/lims/app/declared_requirements.hpp \
        examples/lims/tests examples/lims/README.md
git commit -m "Build lims's billing screens to their mockups and measure the invoice editor against hypothesis 9

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01No1NznqxEBvjhZBaBRLCd6"
```

---

### Task 16: The invoice print view, laid out as a document (mockup 22; hypothesis 9, the document instance)

**Waits for** Tasks 12 and 15. **Never in flight with** Part B Task 19.

**Files:**
- Modify: `examples/lims/billing/include/lims/billing/dto/settings_dto.hpp` (`RegisterBillingClient` and `BillingClientView` gain `address` and `locale`), `examples/lims/billing/include/lims/billing/db/billing_entity.hpp`, `examples/lims/billing/src/db/billing_schema.cpp` (migration `20261130000002`), the model that registers billing clients (`grep -rn 'RegisterBillingClient' examples/lims/billing/src`), the suite that covers it (`grep -rln 'RegisterBillingClient' examples/lims/tests`), `examples/lims/screens/billing_screens.hpp`/`.cpp` (`invoicePrintScreen()`, registered in `addBillingScreens`), `examples/lims/screens/tests/test_lims_billing_screens.cpp`, `examples/lims/screens/scenario/billing_scenario.cpp` (the clients' addresses and locales), the billing catalogue files, `examples/lims/screens/grabs/billing.tsv`, `examples/lims/README.md` (decision 57)

**Interfaces:**
- Consumes: Task 12's `page`, `pageRegion`, `localeScope`; `InvoiceModel::GetInvoice`, `PriceListModel::GetBillingSettings`, `OrgModel` (`ListCompanies`), the billing client's view, the kit; Task 12's measurement in `/tmp/claude-1000/-home-yaraslau-repo-morph/b1b73e49-c41d-4e27-bb68-b8c0cfacf145/scratchpad/lims-h9-print.md`.
- Produces: `RegisterBillingClient::address` and `BillingClientView::address` (`std::string`, at most 512 bytes), `RegisterBillingClient::locale` and `BillingClientView::locale` (`std::string`, a BCP 47 tag, `en` by default), stored in `billing_clients` by migration `20261130000002` "Give billing clients an address and a locale" (`AddNotRequiredColumn` for each; a client registered before reads as no address and `en`); `invoicePrintScreen()`.

The print view is a German client's invoice in German whatever the clerk's locale, as an A4 page. The page, its regions and the locale scope are Task 12's; this task is the domain data the page needs (a client's address and locale, which billing does not hold today) and the screen. The mockup's Print and Save buttons are not built: rendering a document to a printer or a file is out of the design's scope, and decision 57 says so.

- [ ] **Step 1: The client's address and locale**

A model case in the suite that covers `RegisterBillingClient`: "billing clients: a client's address and locale are kept, and a client registered before reads as no address and `en`" (mutation: drop `locale` from the view). Then the fields, the columns and the migration as Interfaces says; `validate()` bounds the address at 512 bytes and the locale to a BCP 47 tag of ASCII letters, digits and `-` starting with a letter.

- [ ] **Step 2: The print view**

`invoicePrintScreen()`, param `invoiceId`: `page({.width = 210, .height = 297, .margins = {20, 20, 25, 20}})` holding the letterhead region (the company's name and address from the scenario's company, pinned top), the address window (the client's name and address at DIN 5008's offset), the invoice's facts (number — or "Draft" before posting — date, client reference, the period billed), the lines table (analysis, samples, quantity, unit price, line total), the totals (net, VAT at the company's rate, gross), the payment terms and the bank details in the footer region (pinned bottom), all inside `localeScope(<the client's locale>)`, so a German client's invoice reads `8.365,11 €` and `31.10.2026` whatever the clerk's locale. Screen cases in `test_lims_billing_screens.cpp`: "the print view lays an invoice out as a German A4 page while the clerk works in English" (mutation: no locale scope — `€8,365.11` shows), "the print view scales its page to a compact window" (mutation: page not scaled — the probe's width property exceeds the window's) and "a posted invoice's print view shows its number" (mutation: always "Draft"). `billing_scenario.cpp` gives Stadtwerke Köln its address and `de`.

- [ ] **Step 3: Build, run, grab, record decision 57, commit**

Run: Build lims, Model suite `"[billing]"`, Screen suite `"[billing]"`, Render smoke; grabs beside `22-InvoicePrint` at both widths (its row added to `grabs/billing.tsv`), classified until only domain and environment items remain.

```markdown
### 57. The invoice print view is a page ui/1 lays out, in the client's locale; nothing renders it to a file (hypothesis 9, document instance)

**Hypothesis 9 is confirmed for the document instance, and morph now says it.** Measured against mockup 22 with ui/1
as it stood: [what Task 12 Step 1 found it could not do]. morph gained a fixed-size page scaled to fit, regions pinned
within it, and a locale scope for formatting (`docs/spec/ui/document_page.md`); the print view uses them. A billing
client has an address and a locale, so a German client's invoice is laid out in German whatever the clerk's locale.
The mockup's Print and Save are not built: rendering a document to a printer or a file is out of the design's scope.
```

```bash
cd /home/yaraslau/repo/morph
git add examples/lims/billing examples/lims/tests examples/lims/screens examples/lims/README.md
git commit -m "Lay out lims's invoice as a document page in the client's locale

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01No1NznqxEBvjhZBaBRLCd6"
```

---

### Task 17: Hypothesis 10 on the ledger, measured; and the database-page seam if it is confirmed

**Waits for** nothing. Changes one new lims test file and, if the measurement confirms hypothesis 10, `include/morph/table` only, so it may run beside the widget tasks. Task 18 follows it.

**Files:**
- Create: `examples/lims/tests/test_ledger_scale.cpp`; if Step 2 confirms hypothesis 10: `docs/spec/table/database_pages.md`, `tests/test_table_database_pages.cpp`
- Modify (if confirmed): `include/morph/table/query.hpp` (`plan`, `QueryPlan`, `Page::totalExact`), `docs/spec/table/engine.md`, `docs/spec/README.md`, `tests/CMakeLists.txt` (if it names files); `examples/lims/README.md` (decision 58)

**Interfaces:**
- Consumes: `LedgerModel::ListEntries`, `LedgerReportModel::TrialBalance`, the ledger's entities (`ledger_entries` and its lines), `morph::table::TableQuery`, `morph::table::apply`, `morph::table::validate`, `morph::table::QueryLimits` (`totalCap = 100'000`), `morph::table::Page` (`total` optional).
- Produces: a measured verdict on hypothesis 10's first instance; if confirmed, `std::expected<QueryPlan, TableError> morph::table::plan(TableQuery const&, std::span<Column const>, QueryLimits const&)` (the validated, normalised query: sort keys with column and direction, each filter as column, operator and typed operand, offset, limit, whether the total is wanted) and `Page::totalExact` (a flag: the source counted the total itself, above the cap).

Hypothesis 10's first instance is the journal over a large table and the trial balance over it. `ListEntries` reads every entry of a company and `TrialBalance` loads every posted line it adds up (decision 30 measured 2·10⁵ lines and inferred about 11 s at 10⁷). The design's refutation condition asks for 10⁶ database rows listed without loading them, with the total shown, and a trial balance over 10⁷ posted lines answering exactly without loading them all. This task measures first, then builds the framework seam both instances need if the measurement confirms it (the second instance, stock movements, is Task 22's and uses it). The ledger's move onto it is Task 18, a lims task.

- [ ] **Step 1: Write the measurement**

Create `examples/lims/tests/test_ledger_scale.cpp` with two `Stress:` cases (label `stress`, 600 s), each writing its rows through Lightweight's `DataMapper` in batches inside one transaction per 10,000 (the posting path's cost is not what is measured):

- "Stress: Hypothesis 10: a page of the journal over 10^6 entries": 10⁶ posted two-line entries for one company; then `ListEntries` (today's read) and a page of 50 through `table::apply` over it, timed, with the process's peak resident memory before and after (`/proc/self/status` `VmHWM`); record both.
- "Stress: Hypothesis 10: a trial balance over 10^7 posted lines": 10⁷ lines (5·10⁶ two-line entries) of up to 10⁹ minor units; `TrialBalance` timed, peak memory recorded, totals checked exactly against an independently accumulated `int64` sum.

Each case's control is in its body: the exact-total check fails if `TrialBalance` drops or double-counts a line, and the page case checks the page's first and last rows against an independently sorted id list.

- [ ] **Step 2: Measure**

Run: Build lims, then `flock /tmp/claude-1000/-home-yaraslau-repo-morph/b1b73e49-c41d-4e27-bb68-b8c0cfacf145/scratchpad/morph-build.lock timeout 900 build/linux-everything/examples/lims/ladder_lims_tests "Stress: Hypothesis 10*"`. Expected (inferred from reading `ledger_entries.cpp`, `ledger_report_model.cpp` and `table::apply`): every entry and every line is loaded; the page's time and the memory grow with the table, and the trial balance at 10⁷ takes tens of seconds or does not fit. Write the numbers down; that is the measured verdict for this instance. If both answer within the production criteria without loading every row, hypothesis 10 is refuted here: skip Step 3 and Task 18, and decision 58 says so with the numbers.

- [ ] **Step 3: If confirmed — the database-page seam: spec, review, tests, implementation**

`docs/spec/table/database_pages.md`: a model that holds its rows in a database pages them there. `morph::table` gives such a model what it needs to do so correctly, without morph knowing the database: `table::plan`, the validated and normalised query, which the model translates into its own query; and `table::Page` answered with an exact total above `QueryLimits::totalCap` when the source counted it itself (`Page::totalExact`, a flag the client shows as "1,000,000 entries", never "more than 100,000"). The engine's `apply` stays the reference implementation for rows in memory and is the oracle the seam's tests compare a database's pages against. What it does not do: translate to SQL (morph has no ORM; lims's Lightweight is the example's), aggregate. Alternatives considered: a SQL dialect in morph (couples morph to databases), streaming every row to the engine (what is measured as too slow). Index it; one paragraph in `engine.md`. One independent review (default model) with `query.hpp`, `engine.md` and the ledger's two reads as its material; address every finding. `tests/test_table_database_pages.cpp`: `plan` normalises sort and filter as `apply` interprets them (same pages over an in-memory source sorted and filtered by the plan as by `apply`, for every operator), and refuses what `validate` refuses (each with its mutation). Implement. Run: the framework suite `"[table]"`, the Docs build. Expected: all pass, no warning.

- [ ] **Step 4: Record decision 58 and commit**

```markdown
### 58. The journal and the trial balance [load every row they page or total / page and total in memory within the criteria] (hypothesis 10, first instance)

**Hypothesis 10 is [confirmed | refuted] for the journal and the trial balance.** Measured in
`tests/test_ledger_scale.cpp` on the `linux-everything` Release build: a page of the journal over 10^6 entries took
[n] ms with every entry loaded (peak memory [n] MB → [n] MB), and a trial balance over 10^7 lines [n] s ([n] MB).
[If confirmed:] morph gained the database-page seam (`docs/spec/table/database_pages.md`): a validated query plan a
model translates into its own query, and an exact total above the engine's cap.
```

```bash
cd /home/yaraslau/repo/morph
git add examples/lims/tests/test_ledger_scale.cpp examples/lims/README.md docs/spec include/morph tests
git commit -m "Measure lims's journal and trial balance at 10^6 and 10^7 rows, and give morph database pages

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01No1NznqxEBvjhZBaBRLCd6"
```

---

### Task 18: The ledger's reads in the database, and `ListSamples` on the same plan

**Runs only if** Task 17 confirmed hypothesis 10 and built the seam; otherwise it is skipped and decision 58 says why. **Waits for** Task 17 and Part B Task 14 committed (`entry_dto.hpp`, `ledger_model.hpp`, `ledger_entries.cpp`). **Never in flight with** Task 14 (`ListSamples`) or Part B Task 19.

**Files:**
- Modify: `examples/lims/ledger/include/lims/ledger/dto/entry_dto.hpp` and `report_dto.hpp`, `ledger_model.hpp`/`ledger_entries.cpp` (`ListEntriesPage`), `ledger_report_model.hpp`/`.cpp` (`TrialBalance` summed in the database), `examples/lims/lab/src/models/sample_model.cpp` (`ListSamples` on the plan), `examples/lims/tests/test_ledger_entries.cpp`, `examples/lims/tests/test_ledger_reports.cpp`, the lab suite that covers `ListSamples`, `examples/lims/README.md` (decision 58's second paragraph; a finding if the escape tier is used)

**Interfaces:**
- Consumes: `morph::table::plan`, `QueryPlan`, `Page::totalExact`, `morph::table::apply` (the oracle), Lightweight's query builder, `fitsCurrency`, `OutOfRange`.
- Produces: `ListEntriesPage { CompanyId company; ::morph::table::TableQuery query; }` → `::morph::table::Page<JournalEntryView>` (on `LedgerModel`, a read); `TrialBalance` answering from database sums; `ListSamples` paging in the database.

- [ ] **Step 1: Write the failing tests**

In `test_ledger_entries.cpp`: "ledger: a page of entries from the database equals the engine's page over the same rows, for every sort and filter the journal offers" (mutation: drop the plan's filters from the query). In `test_ledger_reports.cpp`: "ledger: a trial balance summed in the database equals the sum over the lines, and an overflowing sum is `OutOfRange`" (mutation: sum in the transaction currency). The same oracle case for `ListSamples` in its suite.

- [ ] **Step 2: Implement**

`ListEntriesPage` translates the plan into a Lightweight query over `ledger_entries` (the plan's sort to `OrderBy`, its filters to `Where`, offset and limit) and counts the filtered set with one `COUNT` for the exact total; a filter or sort the query builder cannot express is the escape tier (rule 4) with a README finding, or a refusal in words — never a fallback that loads every row. `TrialBalance` sums each account's posted lines in the database: company amounts are whole minor units (`fitsCurrency`), so the sum is an integer sum of `amount_num × (10^decimals / amount_den)` per account and side; SQLite's `SUM` raises an error on integer overflow rather than wrapping (its documentation, "Built-in Aggregate Functions"), which the ledger turns into `OutOfRange` — read that page and confirm before relying on it. If the query builder cannot express the aggregate, the raw statement is rule 4's escape tier with its README finding. `SampleModel::ListSamples` moves onto the same plan, as a third consumer.

- [ ] **Step 3: Measure again, record, commit**

Run: Build lims, Model suite `"[ledger]"` and `"[lab]"`, then Task 17's two stress cases. Write the numbers down and add decision 58's second paragraph:

```markdown
On the seam, the same page took [n] ms and the trial balance [n] s, summed in the database, exact or refused as
`OutOfRange`. The sample list pages on it too. Stock movements are the second instance (decision 61).
```

```bash
cd /home/yaraslau/repo/morph
git add examples/lims/ledger examples/lims/lab examples/lims/tests examples/lims/README.md
git commit -m "Page lims's journal and total its trial balance in the database

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01No1NznqxEBvjhZBaBRLCd6"
```

---

### Task 19: Ledger — accounts, journal, trial balance and period close (mockups 24–27)

**Waits for** Tasks 5, 7 and 18 (or Task 17's refuted verdict). **Never in flight with** Part B Task 19.

**Files:**
- Create: `examples/lims/screens/tests/test_lims_ledger_screens.cpp`, `examples/lims/ledger/include/lims/ledger/core/refusal_codes.hpp`
- Modify: `examples/lims/screens/ledger_screens.hpp`/`.cpp` (Task 2 created them empty: the four screens, `addLedgerScreens`, `ledgerScreenIds`), the ledger's refusals the mockups show (codes), `examples/lims/screens/scenario/ledger_scenario.hpp`/`.cpp`, the ledger catalogue files, `examples/lims/screens/grabs/ledger.tsv`, the kit's refusal-code case (adds `kLedgerRefusalCodes`), `examples/lims/README.md` (decision 59)

**Interfaces:**
- Consumes: `LedgerModel` (`ListAccounts`, `CreateAccount`, `ListPeriods`, `OpenPeriod`, `BeginClosePeriod`, `ClosePeriod`, `CreateDraftEntry`, `PostEntry`, `ReverseEntry`, `GetEntry`, `ListEntries` or `ListEntriesPage`, `SetExchangeRate`, `ListExchangeRates`), `LedgerReportModel` (`TrialBalance`, `AccountBalance`, `AccountLedger`), `GetBillingSettings` and `GetInventorySettings` (which modules post to an account), the kit, `screen_test_support.hpp`.
- Produces: `accountsScreen()`, `journalScreen()`, `trialBalanceScreen()`, `periodCloseScreen()`; refusal codes `lims.ledger.periodClosed`, `lims.ledger.unbalanced`, `lims.ledger.draftsRemain`.

- [ ] **Step 1: Read the mockups; list what each element needs**

`24-Accounts`, `25-Journal`, `26-TrialBalance`, `27-PeriodClose`. The README's list: tables with two currency column pairs and a totals row (columns and a final heading row: expressible); credit balances with a suffix (`kit::balance`); tree-grouped accounts (heading rows per type: expressible); a dialog whose table rows are editable account and amount inputs, kept balanced (a `state` list edited per row, the difference computed with `sum`, Post enabled only at zero: expressible); a grid of twelve period tiles with state (`grid` of `panel`s with badges: expressible); a checklist whose items carry their own actions (a `column` of rows, each with its button: expressible); a typed-confirmation dialog (a `textInput` whose value must equal the period's label to enable Close: expressible). Compact: the journal's entry detail opens as its own page with Back, and the manual-entry dialog's difference and Post sit in a pinned footer. Which modules post to an account is not a ledger fact: the accounts screen reads billing's and inventory's settings and marks each account their codes name — the ledger stays domain-free.

- [ ] **Step 2: The four screens, their tests**

`test_lims_ledger_screens.cpp`, each with its mutation:

- "the chart of accounts is grouped by type, marks the modules that post to each account, and one account's ledger runs its balance" (mutation: the running balance is the line's amount);
- "the journal pages 10,000 entries with the exact total, and an entry's lines show both currencies and the rounding line" (over `ListEntriesPage` if Task 18 ran, `ListEntries` otherwise; mutation: the total shown is the page's row count);
- "a manual entry into a closed period is refused in words" (`lims.ledger.periodClosed` with the month in words; mutation: the message shows);
- "the manual entry dialog keeps its lines balanced: Post is enabled only at zero difference" (mutation: Post always enabled);
- "the trial balance is balanced, and the yen company's year to date shows whole yen" (mutation: JPY at two decimals);
- "period close: the checklist blocks the close while a draft remains, and the typed confirmation enables it" (mutation: Close enabled without the typed label);
- "a period closed by another client reaches the open period grid" (`ClosePeriod`'s `updates<PeriodView>`, run through a second mounted copy of the screen; mutation: no effect declared).

Build each screen to its mockup: `accountsScreen()` (24), `journalScreen()` (25, entries by source with the entry detail in a side panel, the manual-entry dialog), `trialBalanceScreen()` (26, one period grouped by type with balanced totals; a company and period picker; the yen company's fiscal year to date), `periodCloseScreen()` (27, the year's twelve period tiles, the exchange rates table with an add form, the checklist — drafts remaining, rates for every foreign currency posted in the period — each item with its action, and the typed-confirmation dialog running `BeginClosePeriod` then `ClosePeriod`). `ledger_scenario.cpp` gains the ledger's records the mockups show (October closing, September closed, the CHF entry with its rounding line). Register the four in `addLedgerScreens` and list their ids in `ledgerScreenIds()`.

- [ ] **Step 3: Build, run, check, grab**

Run: Build lims, Model suite `"[ledger]"`, Screen suite `"[ledger]"`, Render smoke. Check the production criteria per screen (Task 13 Step 4's list); grabs beside 24–27 (rows in `grabs/ledger.tsv`), classified until only domain and environment items remain.

- [ ] **Step 4: Record decision 59 and commit**

```markdown
### 59. The ledger screens match their mockups

The chart of accounts (24), journal (25), trial balance (26) and period close (27) are built to their mockups; which
modules post to an account is read from billing's and inventory's settings, so the ledger stays domain-free. The
journal pages [in the database with its exact total (decision 58) / in memory]. [Domain differences from the grabs.]
```

```bash
cd /home/yaraslau/repo/morph
git add examples/lims/screens examples/lims/ledger examples/lims/README.md
git commit -m "Build lims's ledger screens to their mockups

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01No1NznqxEBvjhZBaBRLCd6"
```

---

### Task 20: The deadline effect (framework)

**Waits for** Task 4 (both edit `registry.hpp`). **Never in flight with** Part C Task 8's scoped fan-out and connection-lease seam implementations (they change the same fan-out), Task 4, or the other framework tasks. Changes no file under `examples/`.

**Files:**
- Create: `docs/spec/core/deadline_effects.md`, `tests/test_deadline_effects.cpp`
- Modify: `include/morph/core/registry.hpp` (`BRIDGE_REGISTER_RECORD` accepts `changesAt`; the result walker an action's registration generates), the server's fan-out and its timer (where `docs/spec/core/backend.md` and `docs/spec/core/registry.md` say `fanOut` lives — `include/morph/core/remote.hpp` and its server implementation), `docs/spec/core/registry.md`, `docs/spec/core/backend.md`, `docs/spec/README.md`, `tests/CMakeLists.txt` (if it names files)

**Interfaces:**
- Consumes: `BRIDGE_REGISTER_RECORD`, `::morph::record::key<…>`, `ActionTraits<A>` (the result type), glaze reflection, the server's fan-out (`changes` per watching connection, re-authorized), the server's strand and its timer facilities.
- Produces: `::morph::record::changesAt<&View::member>` — a registered record's declaration that its rows change by time alone at the instant the member holds (a `morph::time::Timestamp`, empty or unengaged when there is none); the server fans out `changes<View>` for that key at that instant, once, to every connection watching the record, each re-authorized as for any change; a server option bounding the pending instants.

Two lims modules (invoice approval, decision 35; requisition approval, decision 43) have a filtered list that changes when a deadline passes, with no action at that instant, so no watcher is told; the issue Part C Task 2 filed records it. The mechanism is the server's: a record declares which of its members is the instant it next changes by time, and the server fans out a change for that key at the instant. It knows no deadline, approval or escalation.

- [ ] **Step 1: The spec, and one review**

`docs/spec/core/deadline_effects.md` states, with complete reasoning:

- *The declaration:* `BRIDGE_REGISTER_RECORD(View, "Name", key<…>, changesAt<&View::deadline>)`.
- *How the server finds an instant inside any result:* views reach a client nested in results — `ListPendingApprovals` answers a `vector<InvoiceView>` inside a result struct — so the server cannot read the instant from the result's top level. Each `BRIDGE_REGISTER_ACTION` generates, at compile time from `ActionTraits<A>`'s result type by glaze reflection, a walker over that type: it descends struct members, `std::vector` and `std::array` elements, `std::optional` values and map values, and at every value whose type is a registered record with a `changesAt` declaration it reads the key and the instant. No JSON is parsed and no type without such a record is walked (the walker for a result that cannot contain one is empty and compiles away). The server runs the walker on every reply it serves — a query's and an action's — after the reply is built, so it learns a row's instant whenever a client could see the row.
- *The pending instants:* one per (record, key). A newly served view's instant replaces the pending one for its key; a view whose instant is empty, unengaged or not after now cancels it (the view served already reflects the change). The set is bounded by a server option (`maxPendingInstants`, default 65,536): past it the farthest instants are dropped and counted in the server's metrics; a dropped key is re-learned the next time a view of it is served. One timer, on the server's strand, waits for the earliest instant.
- *At the instant:* one `changes` for that key, to every connection watching the record, each re-authorized as for any change; then the key leaves the set.
- *What a restart loses:* the set; it is re-learned from the next views served, and a client that mounts after a restart reads the current state anyway.
- *Clock:* the server's clock (the one the server's tests override); an instant is compared with it, never with a client's.
- *What it does not do:* run an action, change data, journal anything, or know why the instant matters.
- *Alternatives considered:* a scheduled action per deadline in the application (domain code per lifecycle, and a journal entry for a change no one made); clients polling (every open inbox asks on a timer); walking the reply's JSON (a parse per reply, and a record cannot be recognised in JSON without its type).

Update `registry.md` (the declaration and the walker) and `backend.md` (the fan-out at an instant); index. One independent review (default model) with `registry.hpp`'s record and action registration, the fan-out and the server's timer facilities as its material; address every finding.

- [ ] **Step 2: Failing framework tests, then the implementation**

`tests/test_deadline_effects.cpp`, over a server rig and a probe model whose view declares `changesAt`, each with its mutation:

1. A view served directly with an instant 50 ms ahead: a watching connection hears a change for that key after the instant and not before (mutation: drop the declaration's registration — nothing is heard).
2. The same view served nested — inside a vector inside a list result: the change is heard at the instant (mutation: the walker reads only the top level).
3. A second view of the key with a later instant moves it; one with no instant cancels it (mutation: keep the first instant).
4. Past the bound, the farthest instant is dropped and counted, and the nearest still fires (mutation: drop the nearest).
5. The control in each case: an action on the probe model that declares `changes<View>` is heard on the same connection, so the connection can hear a change and a silence is the measurement.

Implement to the spec. Run: the framework suite `"[effects]"`, the Docs build. Expected: all pass, no warning; each mutation fails its case; restore.

- [ ] **Step 3: Commit**

```bash
cd /home/yaraslau/repo/morph
git add docs/spec include/morph src tests
git commit -m "Fan out a record's change at the instant its view declares it changes by time

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01No1NznqxEBvjhZBaBRLCd6"
```

---

### Task 21: The shell's screens — home, the approvals inbox across modules, search (mockups 11–13); an inbox told of a deadline

**Waits for** Tasks 7, 10, 14, 15, 19 and 20; Part C Tasks 1 and 2. **Never in flight with** Part B Task 19.

**Files:**
- Create: `examples/lims/screens/tests/test_lims_shell_screens.cpp`
- Modify: `examples/lims/billing/include/lims/billing/dto/invoice_dto.hpp` and `examples/lims/inventory/include/lims/inventory/dto/requisition_dto.hpp` (each record declares its deadline with `changesAt`), `examples/lims/screens/tests/test_lims_approval_inbox.cpp` (its h4 case now expects the inbox told), `examples/lims/tests/test_requisitions.cpp` (the same), `examples/lims/screens/shell_screens.hpp`/`.cpp` (Task 2 created them empty: the three screens, `addShellScreens`, `shellScreenIds`), the shell catalogue files, `examples/lims/screens/grabs/shell.tsv`, `examples/lims/README.md` (decision 60; decisions 35 and 43 gain one sentence each)

**Interfaces:**
- Consumes: Task 20's `changesAt`, Task 10's bars, every module's count, list and search actions (Task 7), `ListPendingApprovals`, `ListPendingRequisitions`, the lab's verification queue (`ListVerifications`), `LedgerModel::ListPeriods`, `LedgerReportModel` (the monthly chart's revenue), `GetSignedInUser` (the greeting), the kit, the issue Part C Task 2 filed.
- Produces: `homeScreen()`, `approvalsScreen()`, `searchScreen()`; `InvoiceView` and `RequisitionView` declaring `changesAt<&…::approvalDeadline>`.

- [ ] **Step 1: lims tells an inbox when a deadline passes**

`InvoiceView`'s registration declares `changesAt<&InvoiceView::approvalDeadline>` and `RequisitionView`'s `changesAt<&RequisitionView::approvalDeadline>`. `test_lims_approval_inbox.cpp`'s h4 case and `test_requisitions.cpp`'s effects case now measure the inbox told after the deadline: change each `CHECK_FALSE(told)` to `CHECK(told)` (and the inbox case's count to the escalated one), keep each case's in-body control (a change heard on the same connection after the deadline), and name the mutation "drop the `changesAt` declaration" (the check fails). Decisions 35 and 43 each gain: "An open inbox is told when the deadline passes: the view declares its deadline as the instant it changes, and the server fans out the change then (decision 60)." Close the issue Part C Task 2 filed with a comment naming the commit, once this lands.

- [ ] **Step 2: The three screens and their tests**

`test_lims_shell_screens.cpp`, each with its mutation:

- "home: one tile per module with its open work, each loading and failing on its own" (a `boundary` per tile; one module's count action made to fail by a fault hook shows that tile's error and the others' counts; mutation: one boundary around all tiles);
- "home: the greeting names the signed-in person, and the period-close warning names the closing period" (mutation: the greeting shows the login; the warning shows the period's id);
- "home: the monthly revenue chart is bars" (mutation: the default line style);
- "approvals: invoices, requisitions and samples to verify in one list, ordered by deadline" (`concatLists` of three `map`s into one row shape — kind, title, amount, deadline, the record's key — then `sort` by deadline; mutation: no sort);
- "approvals: an invoice over the approver's limit has Approve disabled with the limit as the reason" (mutation: the reason missing);
- "approvals: an escalated item reaches an open inbox when its deadline passes" (Step 1's mechanism, measured through the screen; mutation: drop `changesAt`);
- "search: results while typing, grouped by module, the highlight moved by arrows while focus stays in the field, Enter opens" (Task 7's search, its screen form; mutation: Enter opens the first hit whatever the highlight);
- "search: no results offers a way forward" (mutation: an empty list with nothing else).

Build each screen to its mockup: `homeScreen()` (11: greeting with the person's display name, the period-close warning from `ListPeriods`, the KPI tiles — label, large value (`text` role `display`), tone, sub-line — each in its own `boundary`, the monthly revenue chart as bars from the ledger's revenue accounts per month, the inbox summary); `approvalsScreen()` (12: the one list, the detail pane switching on the row's kind with that kind's actions — approve an invoice, approve a requisition, open a sample's verification — Approve disabled with "Over your limit of €10,000.00" where the limit does not cover the amount, the empty state; compact: the detail opens as its own page with Back, Approve in a pinned footer); `searchScreen()` (13: the shell's search as a page, for Shift Enter and for compact, with the same grouped results and keyboard). The mockup's "forward to an approver chosen by limit" is not built: limits are confidential (decision 29), so the screen cannot list who has a higher limit; it says that an approver at the company with a higher limit can approve it, and decision 60 records why. Register the three in `addShellScreens` and list their ids in `shellScreenIds()`.

- [ ] **Step 3: Build, run, check, grab, record decision 60, commit**

Run: Build lims, Model suite `"[requisition]"` and `"[invoice]"`, Screen suite `"[shell]"` and `"[billing]"`, Render smoke. Production criteria per screen; grabs beside 11–13 (rows in `grabs/shell.tsv`), classified until only domain and environment items remain.

```markdown
### 60. The shell's screens span every module, and an inbox is told when a deadline passes

Home (11), the approvals inbox (12) and search (13) are built to their mockups. The inbox merges invoices,
requisitions and samples to verify into one list with ui/1's `map`, `concatLists` and `sort`, and its detail pane
switches on the kind: a screen over three modules' records needed no action across them. A view declares the instant
it next changes by time (`changesAt`), and the server fans the change out then, so an open inbox is told when an
approval escalates; both approval lifecycles declare their deadline, and the issue filed for it is closed. Home's
chart is bars. Forwarding an approval to someone with a higher limit is not built: limits are confidential, so no
screen can list who holds one.
```

```bash
cd /home/yaraslau/repo/morph
git add examples/lims/billing/include/lims/billing/dto/invoice_dto.hpp \
        examples/lims/inventory/include/lims/inventory/dto/requisition_dto.hpp examples/lims/tests examples/lims/screens \
        examples/lims/README.md
git commit -m "Build lims's home, approvals and search screens, and tell an inbox when a deadline passes

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01No1NznqxEBvjhZBaBRLCd6"
```

---
### Task 22: Inventory — stock, requisitions and orders, goods receipt (mockups 28–30; hypothesis 9, second instance; hypothesis 10, second instance)

**Waits for** Tasks 9, 11 and 17 (and 18 if it ran); Part C Tasks 1–6; and Part C Task 8's connection-lease and runtime-units seams implemented, or recorded in decision 49 as none. When Part C Task 8 recorded no connection-lease seam, the told lapse the sample detail's reagents section needs is not there: this task then adds it as a framework task of its own in this branch (spec, one review, framework tests, the Docs build), placed before Step 4, rather than a screen workaround. **Never in flight with** Part B Task 19 or Part C Task 8's seam implementations.

**Files:**
- Create: `examples/lims/screens/tests/test_lims_inventory_screens.cpp`, `examples/lims/inventory/include/lims/inventory/core/refusal_codes.hpp`
- Modify: `examples/lims/inventory/include/lims/inventory/dto/stock_dto.hpp` and `stock_model.cpp` (`ListStockMovements` on the database-page seam, if Task 17 built it), `…/dto/receipt_dto.hpp` and `goods_receipt.cpp` (`PreviewReceipt`), the inventory refusals the mockups show (codes, in `kInventoryRefusalCodes`), `examples/lims/screens/inventory_screens.hpp`/`.cpp` (Task 2 created them empty: the three screens, `addInventoryScreens`, `inventoryScreenIds`), `examples/lims/screens/lab_screens.cpp` (the sample detail's reagents section), `examples/lims/screens/scenario/inventory_scenario.hpp`/`.cpp`, the inventory catalogue files, `examples/lims/screens/grabs/inventory.tsv`, the kit's refusal-code case, `examples/lims/tests/test_stock.cpp`, `examples/lims/tests/test_goods_receipt.cpp`, `examples/lims/README.md` (decision 61)

**Interfaces:**
- Consumes: `StockModel` (`StockOnHand`, `ListStockMovements`, `ListItems`, `ListLocations`, `GetSampleReservation`), `ReservationModel`, `ProcurementModel` (requisitions, suppliers, orders, `ReceiveGoods`, receipts), `LedgerModel::ListExchangeRates`, Task 17's seam, Task 8's bound decimals (a yen order's prices), Task 9's `Step::detail`, Task 11's row spans, the kit (`personName`, `refusalBanner`), `screen_test_support.hpp`.
- Produces: `stockScreen()`, `requisitionsScreen()`, `goodsReceiptScreen()` (the dialog, opened from an order); `PreviewReceipt` (the `ReceiveGoods` payload, unchanged) → `ReceiptPreview { Amount value; Amount companyValue; std::vector<PostingLine> lines; }` (a read: the posting `ReceiveGoods` would make, computed by the same `receiptPostingFor` with the ledger's rate on the date, nothing written); refusal codes `lims.inventory.noRate`, `lims.inventory.duplicateLot`, `lims.inventory.overReceipt`, `lims.inventory.reservedBy` (its `holder` argument the holder's display name, `OrgModel::displayNameOf`).

The goods-receipt dialog is hypothesis 9's second instance: order lines from procurement, new lots and locations from stock, and the posting preview from the ledger, committed by one button. In lims the commit is one inventory action (`ReceiveGoods`, Part C Task 4), and the preview a read on the same model, so the dialog spans one model's actions and a read of another's rate; the measurement is whether it needs more. Stock movements are hypothesis 10's second instance: a lot's movements as a database page.

- [ ] **Step 1: Read the mockups; list what each element needs**

`28-Stock`, `29-Requisitions`, `30-GoodsReceipt`. The README's list: a cell of several badges and tinted rows (expressible); an approval route as a timeline with a passed deadline and an escalation (`steps` with Task 9's detail: expressible); a quantity with a unit picker and its converted value (a `textInput` beside a unit `select`, the converted value computed by an expression over the unit's ratio: expressible); a line split into two lots — a row that spans rows: Task 11's `TableRowView::spans`; the posting preview (`PreviewReceipt`: a domain read). Compact: the receipt dialog's lines become collapsible sections per order line, with the preview's total and Receive in a pinned footer (Conventions).

- [ ] **Step 2: Measure hypothesis 10 on stock movements, and move them onto the seam**

A `Stress:` case in `tests/test_stock.cpp` ("Stress: Hypothesis 10: a page of a lot's movements over 10^6 movements", label `stress`): 10⁶ movements of one item written in batches; a page of 50 through `ListStockMovements`, timed, with peak memory. If Task 17 built the database-page seam, `ListStockMovements` moves onto it (the plan translated into a Lightweight query over `inventory_movements` with an exact count, and the oracle case Task 18 wrote for the journal written again for movements), and the case is run before and after; if Task 17 refuted hypothesis 10, the case records that the in-memory page meets the criteria here too.

- [ ] **Step 3: The three screens and the reagents section, their tests**

`test_lims_inventory_screens.cpp`, each with its mutation:

- "stock: items with their lots, locations, expiry, reserved quantity and value; an expired lot is tinted" (mutation: no tint);
- "stock: one lot's movements page with the exact total" (mutation: the page's count shown as the total);
- "requisitions: an escalated requisition shows its route with the passed deadline, seen by its requester" (mutation: steps without detail);
- "requisitions: purchase orders show their supplier's currency, a yen order in whole yen" (mutation: JPY at two decimals);
- "goods receipt: an order line received in millilitres shows its converted value in the item's unit" (mutation: the converted value is the entered one);
- "goods receipt: one order line split into two lots spans its rows" (Task 11; mutation: no span);
- "goods receipt: the posting preview shows the rounding line, and Receive commits" (mutation: the preview omits the rounding line);
- "goods receipt: refused in words for a missing rate and for a duplicate lot" (mutation: the messages show);
- "sample detail: reagents reserved for the sample, and a lost race naming the holder by name" (the reagents section added to Task 14's screen; mutation: the refusal's message shows);
- "sample detail: a waiting bench is told when the holder's connection closes" (Part C Task 8's connection-lease seam, or this task's framework addition; mutation: the release on close declares no effect).

Build `stockScreen()` (28), `requisitionsScreen()` (29: the requisitions with their routes; the orders by supplier in each supplier's currency; Create order from an approved requisition), `goodsReceiptScreen()` (30: the dialog's order lines; per line the lots, each with number, expiry, location, quantity with a unit picker and the converted value, Split adding a lot under the same line; the preview from `PreviewReceipt`, refreshed when a line changes; Receive running `ReceiveGoods`; loading and empty — an order with nothing left to receive), and the sample detail's reagents section (`GetSampleReservation`, Reserve and Release through a private `ReservationModel`, the refusal naming the holder). `PreviewReceipt` is added to `ProcurementModel` as a read (a Task handler, since it awaits the ledger's rate), with its model test in `test_goods_receipt.cpp` (mutation: a preview that writes the receipt). The scenario gains the inventory records the mockups show (the CHF receipt whose lines lose a cent, the escalated requisition).

- [ ] **Step 4: Build, run, check, grab**

Run: Build lims, Model suite `"[inventory]"` and `"[receipt]"`, the stress case, Screen suite `"[inventory]"` and `"[lab]"`, Render smoke. Production criteria per screen; grabs beside 28–30 and 15 (the reagents section), classified until only domain and environment items remain. Write down for the h9 decision: did the receipt dialog need one action across several models (expected: no — `ReceiveGoods` is inventory's one action, the preview inventory's read over the ledger's rate)?

- [ ] **Step 5: Record decision 61 and commit**

```markdown
### 61. The inventory screens match their mockups; the goods-receipt dialog commits one model's action (hypotheses 9 and 10, second instances)

Stock (28), requisitions and orders (29) and the goods-receipt dialog (30) are built to their mockups, and the sample
detail shows the reagents reserved for the sample, and a waiting bench is told when a holder's connection closes. A
line received as two lots spans its rows, which ui/1 says.

**Hypothesis 9 is [refuted | confirmed | mis-framed] for the goods-receipt dialog.** Its order lines, new lots and posting preview
come from inventory (`ReceiveGoods`, `PreviewReceipt`, which reads the ledger's dated rate through the posting port),
and one button commits one inventory action; [no operation needed one action across models | … | the domain put
the commit in one action, so ui/1 spanning models was never exercised: mis-framed, and what would exercise it]. With
the invoice editor (decision 56) that is both dialog instances; the print view (decision 57) needed a document layout,
which morph gained.

**Hypothesis 10, second instance.** Measured: a page of a lot's movements over 10^6 movements took [n] ms
([before / after the database-page seam]); [the seam served both instances unchanged | …].
```

```bash
cd /home/yaraslau/repo/morph
git add examples/lims
git commit -m "Build lims's inventory screens and the goods-receipt dialog to their mockups

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01No1NznqxEBvjhZBaBRLCd6"
```

---

### Task 23: Part D's definition of done, measured

**Waits for** every task above.

**Files:**
- Modify: `examples/lims/README.md` (Status, the client section, decision 62), `examples/LADDER.md` (lims's size), `codecov.yml` (if the screens' sources are a component), `examples/client/main.cpp` (`--locale`), `scripts/shoot_lims.sh` (`LIMS_LOCALE`)

**Interfaces:**
- Consumes: everything above. Produces: nothing new in code.

- [ ] **Step 1: Every screen, both widths, both frontends**

Run: Build lims, Everything labelled for the rung, the Docs build, Render smoke (every screen mounted at both widths: its expected list is `allScreenIds()`). Then the terminal frontend, each screen once, with the client's absolute path (the loop changes into a scratch directory, so a relative path would fail every screen):

```bash
cd /home/yaraslau/repo/morph
client=$(realpath "$(find build/linux-everything -name morph_lims_client -type f -perm -u+x | head -n 1)")
test -x "$client" || { echo "no client built"; exit 1; }
for screen in $(grep -ho '"lims\.[a-z.-]*"' examples/lims/screens/*_screens.hpp | tr -d '"' | sort -u); do
    scratch=$(mktemp -d)
    (cd "$scratch" && LIMS_SCENARIO=1 script -qec \
        "$client --ui=tui --app lims --user julia --screen $screen --db $scratch/app.db --exit-after-mount" \
        /dev/null) || echo "FAILED: $screen"
    rm -rf "$scratch"
done
```

Expected: no `FAILED` line — every screen mounts on the terminal without a problem. Then open three of them (`lims.billing.invoice`, `lims.ledger.journal`, `lims.inventory.goods-receipt`) interactively in a terminal and operate each by keyboard alone; write down what was usable and what was not, as measured.

- [ ] **Step 2: Every screen matches its mockup**

`scripts/shoot_lims.sh /tmp/claude-1000/-home-yaraslau-repo-morph/b1b73e49-c41d-4e27-bb68-b8c0cfacf145/scratchpad/lims-grabs` (every screen), each compared with its mockup at both widths. Expected: the review file lists only domain and environment items, and every domain item is in its task's decision.

- [ ] **Step 3: The production criteria, per screen**

For each of the 22 screens, a row in decision 62's table: loading, empty and error states (by test name), refusals in words (by refusal code), no raw ids, logins or enum names (the screen tests' checks and the grabs), money and quantities in English and German (one screen per module grabbed under `de`: the examples' client, which `morph_lims_client` is built from, gains `--locale TAG`, passed as `ShellOptions::locale` (Task 4a) so the session starts in that locale as the shell's principal menu would set it — `examples/client/main.cpp`, beside `--grab-size`, fixed in place as an example — and `scripts/shoot_lims.sh` passes it when `LIMS_LOCALE` is set), lists of 10,000 rows (the screens whose lists can reach it, by the stress measurement that covers each), keyboard and labels (the no-empty-accessible-name check over every screen, and the three interactive runs), both widths (render smoke), currency (by test). A criterion a screen does not meet is fixed before this step is done.

- [ ] **Step 4: Every Part D hypothesis has a recorded outcome**

`grep -n '^### \(5[0-9]\|6[0-2]\)\.' examples/lims/README.md` and `grep -n 'Hypothesis 9 is \|Hypothesis 10 is ' examples/lims/README.md`. Expected: decisions 50–62, outcomes for hypothesis 9 (56, 57, 61) and hypothesis 10 (58, 61), every bracket filled with a measured number or the outcome measured.

- [ ] **Step 5: Size, the README, commit**

Count models, actions and screens as Part C Task 9 Step 4 does and record them in the Status paragraph beside the design's estimate (~16 models, ~115 actions, ~18 screens). Update the README's client section to name the production screens by module, and add decision 62 (the per-screen criteria table and the terminal frontend's usability as measured). Then:

```bash
cd /home/yaraslau/repo/morph
git add examples/lims/README.md examples/LADDER.md codecov.yml examples/client/main.cpp scripts/shoot_lims.sh
git commit -m "Record lims ERP Part D as built: every screen against its mockup and the production criteria

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01No1NznqxEBvjhZBaBRLCd6"
```

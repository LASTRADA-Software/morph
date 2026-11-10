# lims ERP Part D: The Production GUI Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build lims's GUI to the standard of a commercial product — Part D of `docs/superpowers/specs/2026-11-09-lims-erp-design.md` — as ui/1 documents written with the C++ authoring API and served by the lims server, one screen per committed mockup in `examples/lims/design/mockups/`, each meeting the production criteria and matching its mockup in side-by-side grabs; and, on the way, measure hypotheses 9 and 10 and build in morph every vocabulary or frontend change a mockup needs that ui/1 cannot say.

**Architecture:** The screens stay in `examples/lims/screens/`, one source file per module (`org_screens.cpp`, `lab_screens.cpp`, `billing_screens.cpp`, `ledger_screens.cpp`, `inventory_screens.cpp`, `shell_screens.cpp`) over a small kit (`screens/kit/`: the width class, the page head, money, dates, status words, refusal banners) and two message catalogues (`screens/i18n/en.json`, `de.json`), registered by `lims::screens::bundle()` with each catalog entry's module and group. Every screen reads and writes only through the models' actions and stays current through declared effects. Where a mockup asks for something ui/1 cannot say, the change is made in `include/morph/ui` (and the frontends under `src/`), specified in `docs/spec/ui/`, reviewed once, tested in `tests/`, and the screen then uses it; no screen carries a workaround. The framework work this plan already knows of is Tasks 2–4, the framework steps of Tasks 5, 6, 8, 9, 10 and 11, and the seams Part C Task 8 specified; a gap found while building a screen is added to the framework step its subject belongs to, in the same branch, before the screen is called done.

**Tech Stack:** C++23, CMake ≥ 3.25 (Ninja), morph (header-only) with its ui/1 authoring API (`morph/ui/author.hpp`), Qt 6 Quick (the reference frontend), the terminal frontend, Catch2, the examples' screen harness (`examples/common/testkit/screen_harness.hpp`), headless Firefox for the mockups' grabs.

**Where Part D stands, and what waits.** Part D's task 1, the mockups, is done (commit `327a3a480`). The screens are ordered so that those whose models have landed come first.

| Part D task | Models it needs | Waits for | Can start |
|---|---|---|---|
| 1. Mockups | — | — | **done** (`327a3a480`) |
| 2. The screen kit, the catalogues and the grabs | none | nothing | **now** |
| 3. Refusals in words (framework) | none | nothing | **now** |
| 4. The application shell (framework) | org | nothing | **now** |
| 5. Org: companies and sites, users and roles (31, 32) | org | Tasks 2–4 | **now**, after Tasks 2–4; a limit per role waits for **Part C Task 2**, the number series' next number for **Part B Task 14** |
| 6. Lab: samples, sample detail, result entry, verification, audit (14–18) | lab, org | Tasks 2–4 | **now**, after Tasks 2–4; the sample detail's reagents section waits for **Part C Task 6** (built in Task 11) |
| 7. Billing: price lists, invoices, the invoice editor, billing runs (19–21, 23) | billing (invoices, approval, lease, runs) | Tasks 2–5 | **now**, after Tasks 2–5; the editor's Post action and the posted states wait for **Part B Task 14** |
| 8. The invoice print view and document layout (22, framework) | billing | Task 7 | after Task 7; a posted invoice's number waits for **Part B Task 14** |
| 9. Ledger: accounts, journal, trial balance, period close (24–27), and hypothesis 10 | ledger | Tasks 2–4 | **now**, after Tasks 2–4 |
| 10. Shell screens: home, approvals inbox, search (11–13), and the deadline effect (framework) | every module | Tasks 4, 6, 7, 9 and **Part C Task 2** (requisitions in the inbox); the home's inventory tile waits for **Part C Task 1** | after Part C Task 2 |
| 11. Inventory: stock, requisitions and orders, goods receipt (28–30) | inventory | **Part C Tasks 1–6**; the receipt's posting preview waits for **Part C Task 4** (so **Part B Task 14**) | after Part C |
| 12. Part D's definition of done | everything | everything above | last |

Tasks 2, 3 and 4 start now, in parallel with Part B; Tasks 5, 6, 7 and 9 follow them, still in parallel with Part B (two or three lanes at a time, one compile at a time). Each of 2–4 changes `include/morph` and wants a lane of its own; 5, 6 and 9 can then run side by side, and 7 after 5 (it uses 5's bound decimals).

## Global Constraints

Copied from the design, `examples/IMPLEMENTATION.md`, `AGENTS.md`, the mockups' README and the user's standing rules for ui/1 work. Every task's requirements include this section.

- **Part D (design, verbatim):** "lims's GUI is built to the standard of a commercial product, to find where the declarative UI stops. Its audience is still morph's regression suite, not a customer: it is production-quality because the limit being looked for is where production quality cannot be reached. `IMPLEMENTATION.md` rule 2 and `LADDER.md`'s 'no rung is a showcase' are amended to exempt lims's look and to say why; bookmarks and bank keep minimalism."
- **Constraints (design, verbatim):** "Every screen is a ui/1 document written with the C++ authoring API and served by the lims server. There is no hand-written QML and no per-screen client code. Where a design asks for something ui/1 cannot say, the fix is a vocabulary or frontend change in morph, not a screen workaround." "Mockups first … Each screen is compared to its mockup by side-by-side grabs." "Qt Quick on the desktop is the reference frontend. The terminal frontend must render every screen usably and is not judged on look."
- **Production criteria (design, verbatim), applied to every screen:** "loading, empty and error states, and a refused action explains itself in words, never an exception text; no raw ids, enum names or unformatted numbers on screen; money and quantities in the user's locale, with English and German shipped; lists of 10,000 rows scroll, sort, filter and page without blocking; full keyboard operation, visible focus, and labels for every control; compact and expanded layouts by the client's width class; every screen stays current when another client changes its data (declared effects only)."
- **Screens (design):** shell — home dashboard, approvals inbox across modules, global search; lab — sample list, sample detail with lifecycle, result entry, verification, audit trail; billing — price lists, invoice list, invoice editor and print view, billing runs; ledger — chart of accounts, journal entries with entry detail, trial balance, period close; inventory — stock by lot and location, requisitions and orders, goods receipt; org — companies and sites, users and role assignments. "The invoice editor (lines and totals from billing, the lease, approval), the goods-receipt dialog (order and new lots) and the cross-module inbox are the deliberate tests of hypothesis 10; the print view is laid out as a document." (The design's table numbers these as hypothesis 9's instances; its sentence says 10. Hypothesis 9 is "ui/1 has no action spanning several models, and no document layout", whose instances the table names as exactly these; this plan measures them as hypothesis 9 and records the slip in decision 50.)
- **Hypotheses Part D measures (design table, "Hypothesis" / "Refuted by"):**
  9. "**ui/1 has no action spanning several models, and no document layout.** Downloads (`downloadToFile`), charts, display text and `sum`/`count` exist." / "Both dialogs and the print view built in ui/1 as it stands, meeting the production criteria". Instances: invoice editor; goods-receipt dialog; invoice print view.
  10. "**Lists and reports run over rows already in memory.** `table::Query` pages with limits (1,000 rows a page, offset 10⁷, totals counted to 100,000), but `apply` runs over rows the model has loaded, and no total above the cap is shown; a report that totals a large table (a trial balance) likewise loads every row it adds up." / "A list over 10⁶ database rows meeting the production criteria without loading them, with its total shown; and a trial balance over 10⁷ posted lines answering exactly without loading them all". Instances: journal entries and the trial balance over them (Task 9); stock movements (Task 11).
- **What Parts B and C hand to Part D:** a JPY price's form advertises two decimals (decision 33), as a yen order's does (decision 44); an approvals inbox is not told when a deadline passes (decisions 35, 43; filed against `include/morph`); a lease's or a reservation's lapse on disconnect is not told to a waiting clerk or bench (decisions 37, 47). Part D meets each: the deadline effect is Task 10's framework step; the per-record decimals and the told lapse are the seams Part C Task 8 specified (if it did) and Part D's screens use them; where Part C Task 8 specified none, the screen task adds the change to its framework step.
- **ui/1 work (user, standing):** "ideally we want to be able to generate these designs using declarative ui" — classify every visual difference by the framework change needed to express the mockup; prefer a vocabulary or frontend change over a screen workaround, and make a screen fix only where the vocabulary already expresses the design. A difference found by a comparison that needs a framework addition is designed and implemented in the same branch, never parked; only a genuinely environmental difference (a font not installed on this machine) stays out. Verify by side-by-side grabs (`morph_examples_client --grab-size WxH --grab f.png`) against the mockups rendered with headless Firefox.
- **Decide, don't ask (user, standing):** no design questions to the user; take the recommended answer, record it in the task's README decision, and go on.
- **Rule 2 (IMPLEMENTATION.md):** "Screens never say what to refresh. Every action that changes data declares its effects … The rung's bundle loads with no refusal and no warning." `docs/KEEPING-SCREENS-CURRENT.md`'s checklist for every action a screen adds or changes.
- **Rule 5 (IMPLEMENTATION.md):** screen tests are part of the suite; every screen is mounted by the render smoke at both width classes.
- **AGENTS.md:** comments and specs state what the code does now and why (no history, no issue numbers); a defect in `include/morph`/`src` gets an issue (verification status, real output, what would change the verdict; no `triage:` label by hand); "The *same* example defect in a second place — File it against `include/morph`"; "Ask whether the check would still pass if the feature did nothing … Mutate the feature and confirm the check fails."
- **Design review (user):** every spec under `docs/spec/` that this plan writes or changes gets one independent review subagent on the default model (never Fable) before it is implemented.
- **Lessons Part B paid for (user):** a screen that lists a filtered set relies on its actions declaring `changes`, not `updates`, when a row can newly enter the set; a screen test that waits for a list to change after another client's action is the check. Qt keyword identifiers (`slots`, `signals`, `emit`) never name anything in `include/morph` (Qt TUs break); GCC `-Wshadow`; clang-format at 119 columns; Clang's doc warnings over headers checked through a TU that includes them.
- **Lane rules (user):** every build/configure is wrapped in `flock /tmp/claude-1000/-home-yaraslau-repo-morph/b1b73e49-c41d-4e27-bb68-b8c0cfacf145/scratchpad/morph-build.lock`, builds at `-j10` at most; tests under the same lock with `timeout 300`. Implementation lanes run on Sonnet; two or three at a time, one compile at a time; builds and tests in the foreground. Commit as the current git user with the session's attribution trailer. Another lane codes on the same branch: rebase before each commit and keep the change to the files the task names.

## Build and test commands

- Configure: `flock /tmp/claude-1000/-home-yaraslau-repo-morph/b1b73e49-c41d-4e27-bb68-b8c0cfacf145/scratchpad/morph-build.lock cmake --preset linux-everything`
- Build lims: `flock /tmp/claude-1000/-home-yaraslau-repo-morph/b1b73e49-c41d-4e27-bb68-b8c0cfacf145/scratchpad/morph-build.lock cmake --build build/linux-everything --target ladder_lims_tests ladder_lims_screen_tests ladder_lims_render_smoke ladder_lims_server morph_examples_client -j10`
- Framework suite (Tasks 2–4, 8–10): `flock /tmp/claude-1000/-home-yaraslau-repo-morph/b1b73e49-c41d-4e27-bb68-b8c0cfacf145/scratchpad/morph-build.lock cmake --build build/linux-everything --target morph_tests -j10` and `flock /tmp/claude-1000/-home-yaraslau-repo-morph/b1b73e49-c41d-4e27-bb68-b8c0cfacf145/scratchpad/morph-build.lock timeout 300 build/linux-everything/tests/morph_tests "[<tag>]"` (the target and path are `tests/CMakeLists.txt`'s; use what it names if they differ). A change under `src/` (a frontend) also builds the Qt Quick and terminal frontends' own suites, which `tests/CMakeLists.txt` names.
- Screen suite: `flock /tmp/claude-1000/-home-yaraslau-repo-morph/b1b73e49-c41d-4e27-bb68-b8c0cfacf145/scratchpad/morph-build.lock env QT_QPA_PLATFORM=offscreen timeout 300 build/linux-everything/ladder_lims_screen_tests`
- Render smoke: `flock /tmp/claude-1000/-home-yaraslau-repo-morph/b1b73e49-c41d-4e27-bb68-b8c0cfacf145/scratchpad/morph-build.lock env QT_QPA_PLATFORM=offscreen timeout 300 build/linux-everything/ladder_lims_render_smoke`
- Everything labelled for the rung: `flock /tmp/claude-1000/-home-yaraslau-repo-morph/b1b73e49-c41d-4e27-bb68-b8c0cfacf145/scratchpad/morph-build.lock ctest --test-dir build/linux-everything -j6 -L ladder-lims --output-on-failure`
- Grabs (Task 2's script): `flock /tmp/claude-1000/-home-yaraslau-repo-morph/b1b73e49-c41d-4e27-bb68-b8c0cfacf145/scratchpad/morph-build.lock scripts/shoot_lims.sh /tmp/claude-1000/-home-yaraslau-repo-morph/b1b73e49-c41d-4e27-bb68-b8c0cfacf145/scratchpad/lims-grabs <screen id>…`

## Conventions every task uses

- **Screen ids** are constants in `screens/lims_screens.hpp`, `lims.<module>.<screen>`: `lims.home`, `lims.approvals`, `lims.search`; `lims.lab.samples`, `lims.lab.sample`, `lims.lab.result-entry`, `lims.lab.verification`, `lims.lab.audit`; `lims.billing.price-lists`, `lims.billing.invoices`, `lims.billing.invoice`, `lims.billing.invoice-print`, `lims.billing.runs`; `lims.ledger.accounts`, `lims.ledger.journal`, `lims.ledger.trial-balance`, `lims.ledger.period-close`; `lims.inventory.stock`, `lims.inventory.requisitions`, `lims.inventory.goods-receipt`; the org screens keep `lims.org.companies` and `lims.org.access`. `kSample` (`lims.sample`, the sample-entry working copy) and `kIgnition` stay as they are; `kRegister`, `kLifecycle` and `kResults` are replaced by the lab screens in Task 6, and their tests move onto those screens before they are removed.
- **Catalog entries** carry `module` (`shell`, `lab`, `billing`, `ledger`, `inventory`, `org`), `group` (the module's name in words, a catalogue key) and `icon`, so the navigator groups by module; titles are catalogue references (`{"t": "lims.<id>.title", "fallback": "…"}`).
- **One file per module**, `screens/<module>_screens.hpp`/`.cpp`, each declaring a `ui::Screen <name>Screen()` per screen with a doc comment naming its mockup; `lims_screens.cpp`'s `bundle()` registers them.
- **The kit** (`screens/kit/`, Task 2) is used for the width class, the page head, money, dates, quantities, status words and refusals; a screen does not format a number, a date or an enum by hand.
- **Words, not ids.** A record is named on screen by what a person knows it by — an invoice by its number or "Draft for <client>", a sample by its reference, a lot by its number, an account by its code and name, a principal by their name — never by a database id; an enum is shown through the kit's status words (catalogue keys), never by its glaze name.
- **Test ids** are the screen's nouns in camelCase (`invoiceList`, `leaseBanner`, `postButton`); every control has an accessible name (`.a11y(...)`) and every input a label.
- **Screen tests** are `screens/tests/test_lims_<module>_screens.cpp`, tagged `[lims-screens][<module>]`, over `ScreenHarness` in `RigMode::Local` with `LocalSetup{.db = {}, .user = …}`, at both width classes where the layout differs (the harness's last constructor argument). Each case that pins a behaviour names its mutation in a `// Mutation:` comment, and the step that adds it runs that mutation once.
- **Grabs.** Every screen task ends with its screens' grabs at 1215 × the mockup's height (expanded) and 390 × the mockup's height (compact), beside the mockup's own grab, and a list of differences in `/tmp/claude-1000/-home-yaraslau-repo-morph/b1b73e49-c41d-4e27-bb68-b8c0cfacf145/scratchpad/lims-visual-review.md` (scratch, not committed), each classified as screen (the vocabulary says it and the screen did not), framework (ui/1 cannot say it: added to a framework task now), domain (the mockup shows something lims's models deliberately do not do: recorded in the task's decision), or environment. A screen task is done when its list holds only domain and environment items.
- **README decisions** continue after Part C's: Part D's start at 50, in task order, reserved even while Part C's 42–49 are still being written. **Migrations** Part D adds take ids from `20261130000001`, in task order.

## File structure after Part D

```
examples/lims/screens/
  lims_screens.hpp / .cpp              ids, prepareLocal, bundle() (catalogues, catalog entries with module and group)
  kit/kit.hpp, kit/kit.cpp             width class, page head, money, dates, quantities, status words, refusals
  i18n/en.json, i18n/de.json           every key the screens use, in English and German
  org_screens.hpp / .cpp               31-Companies, 32-Users
  lab_screens.hpp / .cpp               14-Samples, 15-SampleDetail, 16-ResultEntry, 17-Verification, 18-Audit
  billing_screens.hpp / .cpp           19-PriceLists, 20-Invoices, 21-InvoiceEditor, 22-InvoicePrint, 23-BillingRuns
  ledger_screens.hpp / .cpp            24-Accounts, 25-Journal, 26-TrialBalance, 27-PeriodClose
  inventory_screens.hpp / .cpp         28-Stock, 29-Requisitions, 30-GoodsReceipt
  shell_screens.hpp / .cpp             11-Home, 12-Approvals, 13-Search
  tests/test_lims_<module>_screens.cpp one per module
  render_smoke/test_lims_render.cpp    every screen at both widths
include/morph/ui/                      the vocabulary Tasks 2–4, 8–10 add (refusals, shell parts, document page, bars, …)
docs/spec/ui/                          each addition specified and reviewed
scripts/shoot_lims.sh                  lims's grabs beside the mockups'
```

---

### Task 1: Mockups (done)

Done in commit `327a3a480` ("examples(lims): mockups for the production GUI"): `examples/lims/design/mockups/Shell.dc.html` and `11-Home` … `32-Users`, with `README.md` holding the scenario, the design language and the list of what ui/1 may not express. Every later task reads its mockups and that list first. Nothing to do.

---
### Task 2: The screen kit, the catalogues, the scenario and the grabs

**Files:**
- Create: `examples/lims/screens/kit/kit.hpp`, `examples/lims/screens/kit/kit.cpp`, `examples/lims/screens/i18n/en.json`, `examples/lims/screens/i18n/de.json`, `examples/lims/screens/scenario.hpp`, `examples/lims/screens/scenario.cpp`, `examples/lims/screens/tests/test_lims_kit.cpp`, `scripts/shoot_lims.sh`, `tests/test_ui_width_class.cpp`
- Modify: `include/morph/ui/author.hpp` (`widthClass`, `byWidth`), `docs/spec/ui/author.md` (the two helpers), `examples/lims_gallery/src/screens/screen_layout.hpp` (delegates to them), `examples/lims/screens/lims_screens.hpp`/`.cpp` (the catalogues in the bundle; `prepareLocal` seeds the scenario on request), `examples/IMPLEMENTATION.md` (rule 2's exemption for lims), `examples/LADDER.md` ("no rung is a showcase"), `examples/lims/README.md` (decision 50)

**Interfaces:**
- Consumes: `morph::ui::author::Scope`, `env`, `coalesce`, `switchOn`, `Case`, `t`, `call`, `badge`, `banner`, `morph::ui::BundleFile`, `morph::ui::UiRegistry::addFile`, `glz::reflect`, the lims models' actions (through a `Bridge` over `LocalBackend` with each scenario person's session, as `prepareLocal` dispatches its bootstrap `AssignRole`).
- Produces: `morph::ui::author::AnyHandle morph::ui::author::widthClass(Scope&)` (the client's `{"env": "widthClass"}`, `expanded` where none is set, as a `let` named `widthClass`); `Node morph::ui::author::byWidth(AnyHandle const& width, Nodes compact, Nodes const& expanded)`; in `lims::screens::kit`: `ui::AnyHandle width(ui::Scope&)`, `void pageHead(ui::Scope&, std::string_view module, ui::AnyHandle const& title)`, `ui::Handle<std::string> money(ui::AnyHandle const& amount, ui::AnyHandle const& currency)`, `ui::Handle<std::string> balance(ui::AnyHandle const& amount, ui::AnyHandle const& currency)` (a credit as `13,196.40 Cr`), `ui::Handle<std::string> day(ui::AnyHandle const& instant)`, `ui::Handle<std::string> measure(ui::AnyHandle const& quantity, ui::AnyHandle const& unit)`, `template <typename E> ui::Handle<std::string> statusWord(ui::AnyHandle const& value)`, `template <typename E> ui::Node statusBadge(ui::AnyHandle const& value, std::map<std::string_view, std::string_view> const& tones)`, `ui::Node errorBanner(ui::AnyHandle const& message)` (Task 3 replaces its argument with a refusal); `void lims::screens::seedScenario()` (the mockups' scenario, idempotent, grown by each screen task); `LIMS_SCENARIO=1` makes `prepareLocal` call it.

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

/// @brief One screen showing the kit's five formats over literals, and a button that switches to German.
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

/// @brief One of lims's catalogues, read from the source tree.
/// @param locale `en` or `de`.
/// @return Its keys and messages.
std::map<std::string, std::string> catalogue(std::string_view locale) {
    std::ifstream file{std::string{LIMS_I18N_DIR} + "/" + std::string{locale} + ".json"};
    std::stringstream bytes;
    bytes << file.rdbuf();
    std::map<std::string, std::string> messages;
    REQUIRE_FALSE(glz::read_json(messages, bytes.str()));
    return messages;
}

}  // namespace

// Mutation: make statusWord show the enumerator's name (the English state reads "Submitted").
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
    harness.click("german");
    CHECK(harness.waitForText("eur", "13.196,40 €"));
    CHECK(harness.waitForText("jpy", "1.234.567 ¥"));
    CHECK(harness.waitForText("credit", "13.196,40 € H"));
    CHECK(harness.waitForText("day", "09.11.2026"));
    CHECK(harness.waitForText("state", "Wartet auf Freigabe"));
}

// Mutation: delete one key from de.json (the case names it).
TEST_CASE("lims kit: both catalogues hold the same keys", "[lims-screens][kit]") {
    auto const english = catalogue("en");
    auto const german = catalogue("de");
    for (auto const& [key, message] : english) {
        INFO("missing from de.json: " << key);
        CHECK(german.contains(key));
    }
    for (auto const& [key, message] : german) {
        INFO("missing from en.json: " << key);
        CHECK(english.contains(key));
    }
}
```

The literal forms (`morph::ui::Decimal{"…"}` for an exact decimal, an ISO instant as text) are the author API's for a constant; read `include/morph/ui/value.hpp` and use the constructors it declares if these differ. The expected strings are the mockups' (README, "Money", "Dates"). Where `money`'s or `date`'s locale formatting answers a different but correct form — a no-break space before `€`, or `date`'s `medium` style in English answering `Nov 9, 2026` — assert what it answers and add the difference to the grabs' review list as framework (a `date` style that gives the mockups' `09-Nov-2026`), never pad the expectation. `LIMS_I18N_DIR` is set for the screen-test target in `examples/lims/CMakeLists.txt`, after `morph_add_rung`: `if(TARGET ladder_lims_screen_tests) target_compile_definitions(ladder_lims_screen_tests PRIVATE LIMS_I18N_DIR="${CMAKE_CURRENT_SOURCE_DIR}/screens/i18n") endif()`.

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

Place it after `Scope`, `env`, `coalesce`, `switchOn` and `Case` are declared. Add a "Width class" paragraph to `docs/spec/ui/author.md` naming both and the reason (`medium` takes the expanded layout because the designs draw two). Change the gallery's `layout::widthClass` and `layout::byWidth` to call these (one line each) and keep their names, so the gallery's screens are unchanged. This is a framework change; dispatch the one independent review (default model) on the author.md paragraph and the header change before committing.

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

`statusWord` builds, from `glz::reflect<E>::keys`, one `ui::cond(ui::eq(value, name), ui::t("lims.status.<E>.<name>", <name in words>), …)` chain ending in the value itself; the type's name for the key is a `static constexpr std::string_view` the caller's enumeration gives through a small trait `kit::StatusName<E>` (`InvoiceState` → `"InvoiceState"`), specialised in `kit.hpp` for each enumeration a screen shows, because `glz::name_v` is compiler-dependent. `statusBadge` is `ui::badge(statusWord<E>(value), <the same chain over tones>)`. `balance` is `ui::cond(ui::lt(amount, 0), ui::concat({money(ui::abs(amount), currency), ui::AnyHandle{" "}, ui::t("lims.credit", "Cr")}), money(amount, currency))`. `pageHead` sets `screen.eyebrow(ui::t("lims.module." + module, …))` and `screen.title(title)`; the company in the eyebrow ("Billing · Rheinlab Analytik GmbH") is added by Task 4 once the shell's context exists.

`i18n/en.json` and `i18n/de.json`: flat objects, every key the kit and the screens use, starting with the six modules (`lims.module.lab` "Laboratory" / "Labor", `lims.module.billing` "Billing" / "Fakturierung", `lims.module.ledger` "Ledger" / "Hauptbuch", `lims.module.inventory` "Inventory" / "Lager", `lims.module.org` "Organisation" / "Organisation", `lims.module.shell` "Home" / "Start"), `lims.credit` ("Cr" / "H"), and every enumerator of every state a screen shows (`lims.status.InvoiceState.Submitted` "Waiting for approval" / "Wartet auf Freigabe", …). Each later task adds its keys to both files; the kit's catalogue case fails for a key in one and not the other.

`lims_screens.hpp` declares `std::expected<void, morph::ui::Refusal> addCatalogues(morph::ui::UiRegistry& served)`, which `bundle()` calls after the screens (and the kit's probe bundle calls too); it adds both catalogues with `served.addFile(morph::ui::BundleFile{.kind = "messages", .id = "i18n/en.json", .bytes = <the file's bytes>})` and the same for `de`. The bytes are embedded at build time — read each file with `#embed` if the toolchain has it, else from a generated header the rung's CMake writes (`file(READ …)` into a `constexpr std::string_view`); look at how `examples/` embeds a file elsewhere (`grep -rn 'embed\|file(READ' examples/*/CMakeLists.txt cmake/`) and follow it.

- [ ] **Step 5: The scenario, and the grabs**

`screens/scenario.hpp`/`.cpp`: `void seedScenario()` writes the mockups' scenario (README, "Scenario") through the models, each record by the person the scenario names, dispatched through a `Bridge` over a `LocalBackend` with that person's session (as `prepareLocal` dispatches its `AssignRole`): Rheinlab Analytik GmbH (EUR, fiscal year from January, VAT 19 %, sites Köln and Leverkusen) and Rheinlab Japan K.K. (JPY, April, Osaka); the people and their scoped roles and limits. It does nothing when a company named "Rheinlab Analytik GmbH" exists. Each screen task adds its own records to it (samples for Task 6, invoices for Task 7, and so on), so the grabs show the mockups' figures. `prepareLocal` calls it when `LIMS_SCENARIO` is `1` (read once, with the same `NOLINT(concurrency-mt-unsafe)` comment as the server's `environment`).

`scripts/shoot_lims.sh OUT_DIR [CLIENT] [SCREEN…]` (executable, the shape of `scripts/shoot_screens.sh`): for each lims screen id given (all of them by default), in a fresh scratch directory, with `LIMS_SCENARIO=1`, run `QT_QPA_PLATFORM=offscreen "$client" --ui=qt --app lims --user <the scenario person the mockup shows> --screen <id> --db "$scratch/app.db" --grab-size 1215x<h> --grab "$OUT/<id>-expanded.png"` and the same at `390x<h>` for `-compact.png`; and render the mockup with `firefox --headless --window-size=1215,<H> --screenshot "$OUT/<mockup>.png" "file://$repo/examples/lims/design/mockups/<mockup>.dc.html"`, where `<h>` and `<H>` come from a table in the script (screen id → mockup file, person, heights). The table starts with the org screens and grows with each task. It stops at the first shot that fails, naming it.

- [ ] **Step 6: The rule's exemption for lims**

In `examples/IMPLEMENTATION.md` rule 2, after its zero-styling clause: "lims is exempt from the zero-styling clause, and only lims: its GUI is built to a commercial product's standard against committed mockups (`examples/lims/design/mockups/`), because the limit it looks for is where the declarative UI stops reaching production quality, which a minimal screen never meets. Its look is still a ui/1 document with no hand-written QML and no per-screen client code; bookmarks and bank keep minimalism." In `examples/LADDER.md`'s Audience paragraph: "No rung is a showcase: `IMPLEMENTATION.md` rule 2's zero-styling rule applies to bookmarks and bank; lims is held to its mockups instead, for the reason rule 2 gives."

- [ ] **Step 7: Build, run, record decision 50 and commit**

Run: the framework suite `"[ui]"`, Build lims, Screen suite `"[kit]"`, Render smoke. Expected: all pass; run each mutation once and restore it. Then `scripts/shoot_lims.sh /tmp/claude-1000/-home-yaraslau-repo-morph/b1b73e49-c41d-4e27-bb68-b8c0cfacf145/scratchpad/lims-grabs lims.org.companies` and open the two grabs: the script works end to end.

```markdown
### 50. lims's screens are held to committed mockups through a kit, two catalogues and side-by-side grabs

Every screen formats money, balances, days, quantities and states through `screens/kit/`, so no screen shows a raw
id, an enum name or an unformatted number; English and German catalogues ship in the bundle and must hold the same
keys. The width-class helpers are morph's (`ui::widthClass`, `ui::byWidth`): the gallery held the first copy and lims
needed the second. `LIMS_SCENARIO=1` seeds the mockups' scenario through the models, and `scripts/shoot_lims.sh`
grabs each screen at both widths beside its mockup. The design names the invoice editor, the goods-receipt dialog and
the print view as hypothesis 9's instances in its table and calls them hypothesis 10's in its Part D text; they are
hypothesis 9's (an action spanning several models, a document layout), and hypothesis 10 is the large lists.
`IMPLEMENTATION.md` rule 2 exempts lims's look, and only lims's.
```

```bash
cd /home/yaraslau/repo/morph
git add include/morph/ui/author.hpp docs/spec/ui/author.md tests/test_ui_width_class.cpp tests/CMakeLists.txt \
        examples/lims_gallery/src/screens/screen_layout.hpp examples/lims/screens scripts/shoot_lims.sh \
        examples/IMPLEMENTATION.md examples/LADDER.md examples/lims/README.md examples/lims/CMakeLists.txt
git commit -m "Give lims's screens a kit, English and German catalogues, a seeded scenario and side-by-side grabs

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01No1NznqxEBvjhZBaBRLCd6"
```

---

### Task 3: Refusals in words (framework)

**Files:**
- Create: `docs/spec/ui/refusals.md`, `include/morph/core/refusal.hpp`, `tests/test_refusal.cpp`
- Modify: `include/morph/core/wire.hpp` (the `err` envelope's refusal), `include/morph/core/registry.hpp` (the server path carries it), `include/morph/core/bridge.hpp` (the local path keeps it), `include/morph/ui/author.hpp` (`refusal()` on a mutation and a query; `refusalText`), the interpreter that evaluates a mutation's state (`docs/spec/ui/interpreter.md` names it), `docs/spec/core/wire.md`, `docs/spec/ui/document.md`, `docs/spec/README.md`, `examples/lims/org/include/lims/org/core/errors.hpp` (a code per class, and an optional specific code with arguments), the lease refusal in `examples/lims/billing/src/models/invoice_edit_model.cpp` (the first specific code), `examples/lims/screens/kit/kit.hpp`/`.cpp` (`refusalBanner`), both catalogues, `examples/lims/screens/tests/test_lims_kit.cpp`, `examples/lims/README.md` (decision 51)

**Interfaces:**
- Consumes: `morph::wire::Envelope`, `makeErr`, `ActionDispatcher`'s failure path, `Bridge::localOp`/`localTaskOp`, `ui::author::MutationHandle::error()`.
- Produces: `class morph::model::Refusal : public std::runtime_error { Refusal(std::string code, std::vector<RefusalArg> args, std::string message); std::string const& code() const; std::vector<RefusalArg> const& args() const; }` with `struct morph::model::RefusalArg { std::string name; std::string value; }`; the `err` envelope's optional `refusal` member (`{code, args}`), omitted when empty so an older client reads the envelope as before; `ui::author` `refusal()` on mutation and query handles (`null` or `{code, args}`) and `ui::author::refusalText(AnyHandle const& refusal, AnyHandle const& message)` (the catalogue's text for the code with the arguments, the class's sentence when the code has no entry, never the message); in lims, `LimsError` derives from `morph::model::Refusal`, each class with a default code (`lims.refusal.validation`, `.notFound`, `.forbidden`, `.conflict`, `.outOfRange`, `.illegalTransition`, `.emptyPrincipal`), and a constructor taking a specific code and arguments; `kit::refusalBanner(ui::AnyHandle const& refusal)`.

The production criteria say a refused action explains itself in words, never an exception text, in the user's locale. The wire carries a failure as one `message` string, so a screen has only the server's text — which lims writes for logs and tests, with action names and ids ("SubmitInvoice: invoice 7 is being edited by 'alice'") and in English. A screen cannot word that, and the server cannot word it in the client's locale. What is missing is mechanism: a failure that carries a code and its arguments to the client, and an expression that words it from the bundle's catalogue. lims keeps its messages for logs and tests; the screens never show them.

- [ ] **Step 1: The spec**

`docs/spec/ui/refusals.md` states, present tense, with complete reasoning: what a refusal is (`morph::model::Refusal`: a code, named string arguments, and a message for logs); how it travels (the server's dispatcher catches any exception deriving from `Refusal` and puts `{code, args}` in the `err` envelope beside the message; the journal records the message as today; the local path rejects the completion with the exception itself, which the interpreter reads the same way); how a document reads it (`refusal()` on a mutation's or query's state; `refusalText(refusal, message)` → `t(code, <class sentence>, args)`; the class sentence is the catalogue's `<code's class>` key — the code's segment before its last dot with `.generic` — and the interpreter's built-in "This was refused." when even that is missing; the message is never shown); what an exception that is not a refusal shows (the generic sentence; its text stays in the log); why a code and not an error class over the wire (an application's set of refusals is open and its words are its own); the wire field's compatibility (omitted when empty; an older client ignores the unknown key, `decode` already reads with `error_on_unknown_keys = false`); and alternatives considered: wording on the server in the client's locale (the server would need the locale per call, and the words would live in two places), a status-code enum in morph (domain words in morph), parsing the message (fragile, and English). Index it in `docs/spec/README.md`; add a paragraph to `docs/spec/core/wire.md` (the field) and `docs/spec/ui/document.md` (the two expressions). One independent review (default model) with `wire.hpp`, `registry.hpp`'s failure path, `bridge.hpp`'s local paths and the interpreter's mutation state as its material; address every finding.

- [ ] **Step 2: Write the failing framework test**

`tests/test_refusal.cpp`: (1) a model whose action throws `Refusal{"probe.taken", {{"holder", "alice"}}, "taken by alice"}` dispatched over a server rig: the reply's envelope has `refusal.code == "probe.taken"` and the argument; (2) the same over the local path: the completion's exception is a `Refusal` with the code; (3) a thrown `std::runtime_error` yields an `err` with no `refusal`; (4) an authored screen whose mutation fails with the refusal shows `refusalText`'s words from a probe catalogue (`"probe.taken": "Held by {holder}"`), and the class sentence when the catalogue lacks the code, and never the message — on the screen harness, with a probe bundle. Each with its `// Mutation:` (drop the field from the envelope; drop the code from the local rejection; show `message` as the fallback).

- [ ] **Step 3: Implement, then move lims onto it**

Implement to the spec in the files listed. In lims, `LimsError` derives from `morph::model::Refusal`; each subclass's existing constructor keeps its signature and passes its class's default code and no arguments, so every throw site compiles unchanged; each gains a second constructor `(std::string code, std::vector<morph::model::RefusalArg> args, std::string message)`. The lease refusal in `InvoiceEditModel::execute(const LeaseInvoice&)` and `SubmitInvoice`'s "being edited by" become `Conflict{"lims.billing.invoiceLeased", {{"holder", holder}}, <the message as today>}`; both catalogues gain `lims.billing.invoiceLeased` ("{holder} is editing this invoice." / "{holder} bearbeitet diese Rechnung.") and the seven class sentences (`lims.refusal.conflict.generic` "Someone changed this meanwhile. Reload it and try again." / "Jemand hat das inzwischen geändert. Bitte neu laden und erneut versuchen.", and one each for validation, not found, forbidden, out of range, illegal transition and empty principal). `kit::refusalBanner(refusal)` is `ui::banner({.tone = "err", .text = ui::refusalText(refusal, {})})`, visible while the refusal is engaged; `kit::errorBanner` is removed and its callers moved. Each later screen task gives the refusals its mockup shows a specific code, with both catalogues' words, as part of building that screen.

- [ ] **Step 4: Build, run, record decision 51, commit**

Run: the framework suite `"[refusal]"`, Build lims, Model suite (the error set's message texts are unchanged, so every `CHECK_THROWS_WITH` holds), Screen suite. Expected: all pass; the four mutations each fail their case; restore.

```markdown
### 51. A refusal reaches a screen as a code with its arguments, and the screen words it

Every lims error is a `morph::model::Refusal`: its class gives a default code (`lims.refusal.conflict`, …) and a
refusal a screen shows gives a specific one with its arguments (`lims.billing.invoiceLeased`, holder). The code travels
in the `err` envelope beside the message; a screen shows `refusalText`, the catalogue's words for the code in the
user's locale, or its class's sentence, and never the message, which stays what lims logs and its tests read.
```

```bash
cd /home/yaraslau/repo/morph
git add docs/spec include/morph tests examples/lims/org/include/lims/org/core/errors.hpp \
        examples/lims/billing/src/models examples/lims/screens examples/lims/README.md
git commit -m "Carry refusals to the client as codes with arguments, and word them from the bundle's catalogue

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01No1NznqxEBvjhZBaBRLCd6"
```

---

### Task 4: The application shell (framework)

**Files:**
- Create: `docs/spec/ui/shell_parts.md`, `include/morph/ui/shell_parts.hpp`, `tests/test_ui_shell_parts.cpp`, `examples/lims/screens/shell_parts.cpp`
- Modify: `include/morph/ui/bundle.hpp` (the manifest's shell parts), `include/morph/ui/service.hpp` (`UiRegistry::setShellParts`), `include/morph/ui/shell_frame.hpp` and its implementation (the switcher, the search field and its results, the inbox button, module groups with counts, the principal's menu), `include/morph/ui/navigator.hpp` (collapsed groups with a count), the generic client's shell wiring (`docs/spec/client/client.md` names it), the terminal frontend's shell, `docs/spec/ui/shell_frame.md`, `docs/spec/ui/workspace.md`, `docs/spec/client/client.md`, `docs/spec/README.md`; in lims: `examples/lims/org/include/lims/org/dto/company_dto.hpp`/`org_model.hpp`/`org_model.cpp` (`ListMyScopes`), a count action per module (`CountOpenWork` on `SampleModel`, `InvoiceModel`, `LedgerModel`, later `ProcurementModel`), a search action per module (`SearchSamples`, `SearchInvoices`, `SearchAccounts`, later `SearchStock`), `examples/lims/screens/lims_screens.cpp` (`setShellParts`), `examples/lims/screens/kit/kit.cpp` (`pageHead`'s company), both catalogues, `examples/lims/README.md` (decision 52)

**Interfaces:**
- Consumes: `ui::ShellFrame`, `ui::Navigator` (`setBadge`, `menuEntries`), `ui::Workspace` (`HeadPlacement::InPage`), `CatalogEntry::module`/`group`, `Session::setLocale`, `ui::author::env`.
- Produces: `struct morph::ui::ShellParts { std::optional<ContextSwitch> context; std::vector<ModuleCount> modules; std::vector<SearchSource> search; std::optional<InboxEntry> inbox; std::optional<PrincipalMenu> principal; }`, each naming a model and a read action by id with the fields it reads (`ContextSwitch { model, action, optionsField, labelField, valueField }`, `ModuleCount { module, model, action, countField }`, `SearchSource { module, model, action, queryField, resultsField }`, `InboxEntry { screen, sources: std::vector<ModuleCount> }`, `PrincipalMenu { model, action, rolesField, locales }`); `UiRegistry::setShellParts(ShellParts)`; the chosen context as `{"env": "context"}` to every screen (its value the option's `valueField`), kept per session and sent with no action; in lims, `ListMyScopes {}` → `ListMyScopesResult { std::vector<ScopeOption> scopes; }` (`ScopeOption { CompanyId company; SiteId site; std::string label; CurrencyCode currency; }`), `CountOpenWork { CompanyId company; }` → `OpenWork { Count count; }` per module, `SearchSamples`/`SearchInvoices`/`SearchAccounts { std::string query; CompanyId company; }` → `SearchHits { std::vector<SearchHit> hits; Count total; }` (`SearchHit { std::string title; std::string subtitle; std::string screen; std::string params; }`).

The mockups' shell (`Shell.dc.html`) holds what the ShellFrame does not: a company and site switcher whose choice scopes every list and every new record; module groups that collapse to one row with a count of the module's open work, only the open module listing its screens; one search field over every module with grouped, typed results while typing (Ctrl K; arrows move the highlight while focus stays in the field; Enter opens; Shift Enter opens all results); an approvals button with its count in the top bar; and the signed-in person as a menu with roles and the locale. The frame is the client's, built from widgets, and an application cannot draw any of these in a screen. Each is mechanism — a CRM needs a scope switcher, counts, search and an inbox unchanged — and each is declared by the application as data the shell reads: which model action answers it. The domain stays in the application: morph knows no company, no role, no record kind.

- [ ] **Step 1: The spec, and one review**

`docs/spec/ui/shell_parts.md`: each part's declaration, what the client asks and when (counts and the inbox's count on mount and again on any change to the records their actions read, through the same effects screens use; search on each edit after a pause, cancelled by the next), where each is drawn on `ShellFrame` (the switcher at the sidebar's top under the application's name; module groups in the navigator, one open; the search field and the inbox button in the top bar's line before the principal; the principal's menu replacing the principal text), the compact form (the switcher and the module groups inside the navigation menu; search and the inbox stay in the bar; search opens full-screen), the terminal frontend (each part as a menu or a line; usable, not styled), keyboard (Ctrl K focuses search; the search results' highlight moves with arrows while the field keeps focus; Escape closes), accessibility names, how a screen reads the context (`env("context")`) and how the context reaches a query's body (the screen passes it; the shell never edits a body), what happens when the chosen option disappears (the first option), and alternatives considered (a screen per part: each screen would redraw the shell; an application-specific shell: per-application client code, which the design forbids). Update `shell_frame.md`, `workspace.md`, `client/client.md` and the index. One independent review (default model) with the shell frame's header and implementation, the navigator, the client's shell wiring and the mockup as its material; address every finding.

- [ ] **Step 2: Failing framework tests, then the implementation**

`tests/test_ui_shell_parts.cpp`, over a probe application on the recording backend: the switcher lists a probe action's options and the chosen value reaches a screen's `env("context")`; a module's count shows on its collapsed group and changes after an action that declares a change to the record the count action reads; search results appear grouped after typing and Enter opens the highlighted hit's screen with its params; the inbox button's count is the sum of its sources; the principal's menu lists the roles and switches the locale. Each with its mutation. Implement to the spec; the Qt Quick and terminal frontends draw each part.

- [ ] **Step 3: lims declares its shell**

`ListMyScopes` on `OrgModel` (every company the caller can see, with each of its sites, labelled "Rheinlab Analytik GmbH · Köln", a company alone as "… · all sites"); `CountOpenWork` per module (lab: samples to verify at the company; billing: drafts and submitted invoices; ledger: periods closing; inventory, after Part C: requisitions waiting for approval), each a read; `SearchSamples` (reference prefix), `SearchInvoices` (number or client name), `SearchAccounts` (code or name), each a read limited to the caller's companies and answering at most 8 hits and the total; `screens/shell_parts.cpp` builds the `ShellParts` (`inbox` from billing's `ListPendingApprovals` count and, after Part C, inventory's `ListPendingRequisitions`; a hit's `screen` and `params` open the record's screen). `kit::pageHead` adds the context's company label to the eyebrow. Model tests for each new action in their module's suite (`[org]`, `[events]`, …); a screen test in `test_lims_kit.cpp`: choosing Leverkusen in the switcher changes a probe list to Leverkusen's records (mutation: a query that ignores `env("context")`).

- [ ] **Step 4: Build, run, grab, record decision 52, commit**

Run: the framework suite `"[shell]"`, Build lims, Model suite, Screen suite, Render smoke; `scripts/shoot_lims.sh` for the org screens beside `Shell.dc.html`. Expected: all pass; the shell's grabs match the mockup's frame, differences classified per the Conventions and the framework ones fixed in this task.

```markdown
### 52. The shell's switcher, counts, search, inbox and principal menu are declared, and morph draws them

lims declares its shell as data (`ShellParts`): the scopes the caller can see (`ListMyScopes`), each module's open
work (`CountOpenWork`), a search action per module, the approvals inbox's sources, and the principal's roles. morph's
shell draws each part on both frontends and hands the chosen scope to every screen as `env("context")`; every lims
list and every new record reads it. Nothing in morph knows a company, a module's work or a record kind.
```

```bash
cd /home/yaraslau/repo/morph
git add docs/spec include/morph src tests examples/lims
git commit -m "Let an application declare its shell's scope switcher, counts, search, inbox and principal menu

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01No1NznqxEBvjhZBaBRLCd6"
```

---
### Task 5: Org — companies and sites, users and roles (mockups 31, 32)

**Files:**
- Modify: `examples/lims/screens/org_screens.hpp`/`.cpp` (both screens rebuilt to their mockups), `examples/lims/screens/tests/test_lims_org_screens.cpp`, `examples/lims/screens/scenario.cpp` (org records), `examples/lims/screens/lims_screens.cpp` (catalog entries), both catalogues, `scripts/shoot_lims.sh` (the two rows), `include/morph/ui/author.hpp` and the interpreter and both frontends (a text input's `unit` and `decimals` bound — Step 4), `docs/spec/ui/view_tree.md`, `examples/lims/org/src/models/org_model.cpp` (the last-supervisor refusal's code), `examples/lims/README.md` (decision 53)

**Interfaces:**
- Consumes: `OrgModel` (`ListCompanies`, `ListSites`, `CreateCompany`, `CreateSite`, `ListUsers`, `ListRoleAssignments`, `AssignRole`, `RevokeRole`, `ListValueLimits`, `SetValueLimit`), `PriceListModel::GetBillingSettings`, `StockModel::GetInventorySettings` (after Part C Task 1; the panel shows "not set up" before), the kit, `kit::refusalBanner`, `env("context")`.
- Produces: `companiesScreen()` and `accessScreen()` rebuilt to mockups 31 and 32 (ids unchanged); refusal code `lims.org.lastSupervisor`; a text input's `unit` and `decimals` that follow a bound value (framework).

Mockup 31 shows, for the company the switcher chose, its settings — name, base currency and fiscal year start, both marked fixed (lims has no action that changes them, so they are read-only by construction), the number series (invoices: `INV-<year>-<six digits>`, next number; receipts after Part C: `GR-…`), the posting accounts (billing's receivable, revenue, tax and rounding accounts; inventory's stock, goods-received-not-invoiced, consumption and rounding accounts) and its sites, each with a form to add one. Mockup 32 shows the users with their roles, and one selected user's scoped roles, each with its value limit in the scope's company currency and the duty rules it is subject to, an inline row to add an assignment whose limit's unit and decimals follow the chosen scope's currency, and the alert that refuses removing a company's last administrator.

The number series panel needs a read of billing's series, which Part B Task 14 adds (`NumberSeriesRecord`); until it lands the panel shows the format with no next number. A limit per role (requisition approver) needs Part C Task 2; until it lands the limit column shows the invoice approver's limit only. Neither blocks the task: the rest is built and grabbed now, and each completes when its model lands (a step at the end of Part B Task 14's and Part C Task 2's own work, or the next time this screen is touched).

- [ ] **Step 1: Read the mockups and list what each element needs**

Open `31-Companies.dc.html` and `32-Users.dc.html` in a browser and write, in the scratch review file, each element and the ui/1 node it becomes (`panel`, `keyValue`, `table` with a `TableRowView` tone, `badge`, `form`, `dialog`, `textInput` with `unit`, `banner`), or "framework" with the missing word. The README's list names one for this task: "an inline add-assignment row whose limit's unit follows the chosen scope's currency" — `TextInput`'s `unit` is bound, but its `decimals` is set once (`view_tree.md`), so a JPY scope cannot take whole yen while an EUR scope takes cents. That is Step 4. Anything else found goes into Step 4 as well.

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
    CHECK(harness.prop("TextInput", "addLimit", "unit") == "¥");
    CHECK(harness.prop("TextInput", "addLimit", "decimals") == "0");
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

The screen harness runs one client, so "another client" is measured where the harness can: a change made through one part of a screen reaches another part only through the action's declared effect, which is the mechanism a second client relies on; the remote fan-out itself is measured by the model suites over `BackendRig::connect`. `prop` is `GalleryScreen::prop`'s two lines, added to the file's anonymous namespace as a helper over `harness.find` and `harness.backend().prop`. The test ids (`userList`, `user-<name>`, `assignmentScope`, `assignmentLimit`, `revoke-<role>`, `refusalDialog`, `addAssignment`, `addLimit`) are the screen's; give them in Step 3.

- [ ] **Step 3: Build the two screens**

`accessScreen()` in `org_screens.cpp`, to mockup 32: `kit::pageHead(screen, "org", t("lims.org.access.title", "Users and roles"))`; the users table (`userList`, one row per user from `ListUsers`, the cell a column of the name and the roles as quiet badges, a row's key the principal, `selectionMode = Single`, each row's test id `user-<principal>`); beside it (expanded) or under it (compact, `kit::width` and `ui::byWidth`), the selected user's panel: their assignments (`ListRoleAssignments` filtered by `filter` over the selected principal), each row with the role in words (`kit::statusWord<lims::LimsRole>`), the scope by name (the company's or site's name from `ListCompanies`/`ListSites` through a `map` over the rows, never the id), the limit for that role and company from `ListValueLimits` (`kit::money` in the company's currency), the duty rules as catalogue sentences per role (`lims.org.duty.InvoiceApprover`: "Cannot approve an invoice they created or submitted."), and a revoke button per row (`revoke-<role>`); the inline add row (`addAssignment`: role select, scope select over `ListMyScopes`, limit input `addLimit` whose `unit` and `decimals` follow the chosen scope's currency, Add); a `dialog` (`refusalDialog`) opened by the revoke mutation's refusal, showing `kit::refusalBanner`. `companiesScreen()`, to mockup 31: the context's company from `ListCompanies`; a `keyValue` of its settings with "fixed" badges; the number series panel; the posting accounts panel from `GetBillingSettings` and `GetInventorySettings` with an empty state ("Billing is not set up for this company.") when a settings read is `NotFound`; the sites table and the create-site form. Both screens show a `busy` while their first query loads and the kit's refusal banner for a failed query.

`RevokeRole`'s last-supervisor refusal gets the code `lims.org.lastSupervisor` with arguments `principal` and `company` (the company's name, read where the refusal is thrown); both catalogues get its words ("{principal} is the last administrator of {company}. Make someone else an administrator first." / "{principal} ist die letzte Administratorin bzw. der letzte Administrator von {company}. Ernennen Sie zuerst eine weitere Person.").

`scenario.cpp` gains the org records the two mockups show (both companies, their sites, the seven people with their roles and limits as the README's scenario lists them).

- [ ] **Step 4: Framework — a text input's decimals follow a bound value**

`TextInput`'s `decimals` (and `unit`, if it is set once too) becomes a `Prop` the mount re-applies on change: `docs/spec/ui/view_tree.md`'s palette row says so, the interpreter evaluates the expression, and both frontends re-apply the entry places (Qt Quick's validator and display; the terminal's entry rule). A change of decimals with a value already typed keeps the text and re-validates it, showing the field's error when it no longer fits. One framework test per frontend over the recording backend (mutation: apply decimals at mount only). One independent review (default model) of the spec change with the mount code as its material. If the review or the reading shows `decimals` is already a `Prop`, this step is the frontend fix alone.

- [ ] **Step 5: Build, run and check the production criteria**

Run: Build lims (and the framework suite if Step 4 changed `include/morph` or `src`), Screen suite `"[org]"`, Render smoke. Then, for both screens, check and write down: loading (a `busy` while the first query runs), empty (a company with no sites shows the empty state), error (a refused query shows words), no raw id or enum name (the case above, and reading the grab), keyboard (Tab reaches every control in reading order; Enter submits the add row; focus is visible on Qt Quick), labels (every control has an accessible name: `harness.findAll` over controls with an empty `a11y` name finds none), both widths, and currency (a yen limit shows `¥`, no decimals). Run each mutation named in the file once and restore it.

- [ ] **Step 6: Grab beside the mockups**

`scripts/shoot_lims.sh /tmp/claude-1000/-home-yaraslau-repo-morph/b1b73e49-c41d-4e27-bb68-b8c0cfacf145/scratchpad/lims-grabs lims.org.companies lims.org.access`, and compare each pair at both widths. Classify every difference (Conventions). Fix every screen difference now; add every framework difference to Step 4 (or, when it belongs to another framework task's subject, to that task, which then runs before this one is called done); record every domain difference in decision 53. Repeat until the list holds only domain and environment items.

- [ ] **Step 7: Record decision 53 and commit**

```markdown
### 53. The organisation screens match their mockups; settings with no action to change them are shown fixed

Companies (mockup 31) and users (mockup 32) are rebuilt to their mockups. A company's base currency and fiscal year
start are shown fixed, since no action changes them; the posting accounts are read from billing's and inventory's
settings, each shown "not set up" when absent. A user's roles show their scope by name, their limit in the scope's
company currency, and the duty rules of the role. Removing a company's last administrator is refused in words. A
limit input's decimals follow the chosen scope's currency, which ui/1 now says (`TextInput.decimals` follows its
binding). [Domain differences from the grabs, one sentence each.]
```

```bash
cd /home/yaraslau/repo/morph
git add examples/lims/screens examples/lims/org/src/models/org_model.cpp scripts/shoot_lims.sh examples/lims/README.md \
        include/morph src docs/spec tests
git commit -m "Build lims's organisation screens to their mockups

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01No1NznqxEBvjhZBaBRLCd6"
```

---
### Task 6: Lab — samples, sample detail, result entry, verification and audit (mockups 14–18)

**Files:**
- Create: `examples/lims/screens/lab_screens.hpp`, `examples/lims/screens/lab_screens.cpp`, `examples/lims/screens/tests/test_lims_lab_screens.cpp`
- Delete (once their tests have moved): `registerScreen`, `lifecycleScreen`, `resultsScreen` from `lims_screens.cpp`, and `kRegister`, `kLifecycle`, `kResults` from `lims_screens.hpp`; `screens/tests/test_lims_screens.cpp`'s cases move to `test_lims_lab_screens.cpp`
- Modify: `examples/lims/lab/include/lims/lab/dto/result_dto.hpp` (`CaptureConcentration::reason`), `examples/lims/lab/src/models/sample_model.cpp` (a reason is required to replace a captured value), `examples/lims/tests/test_result_entry.cpp`, `include/morph/ui/view.hpp`/`author.hpp`, the interpreter and both frontends (`Step::detail` — Step 4), `docs/spec/ui/view_tree.md`, `examples/lims/screens/scenario.cpp` (1,284 samples, the mockups' results), `lims_screens.cpp` (catalog entries), both catalogues, `scripts/shoot_lims.sh`, `examples/lims/screens/render_smoke/test_lims_render.cpp` (the mounted list), `examples/lims/README.md` (decision 54)

**Interfaces:**
- Consumes: `SampleModel` (`ListSamples` with `TableQuery`, `GetSample`, the transitions, `CaptureConcentration`, `ListResults`, `VerifyResult`, `ListVerifications`, `GetAuditTrail`, `OpenSample`), `AnalysisCatalogModel` (`GetAnalysisSchema` for the result form), `InvoiceModel::ListBillableLines` (the billing a sample causes), `StockModel::GetSampleReservation` and `ReservationModel` (Part C Task 6), the gallery's screens as designs (`examples/lims_gallery/src/screens/samples.cpp`, `results.cpp`, `review.cpp`, `esignature.cpp`, `audit.cpp`), the kit, the shell's context.
- Produces: `samplesScreen()`, `sampleDetailScreen()`, `resultEntryScreen()`, `verificationScreen()`, `auditScreen()`; `CaptureConcentration::reason` (`std::string`, at most 256 bytes, required when the capture replaces a value already captured for the same analysis version); refusal codes `lims.lab.reasonRequired`, `lims.lab.sameActor`, `lims.lab.illegalTransition`; `Step::detail` (framework).

The lab screens put the gallery's designs 05, 02/10, 08, 09 with the e-signature, and 04 into the shell over lims's real models, keyed by sample rather than worksheet (decision 19). The gallery's screens are backend-free studies and stay unchanged; their layout code is the design to follow, rebuilt over queries and mutations.

Two mockup elements are lims domain, decided here: the result-entry reason-for-change dialog needs somewhere to keep the reason — a regulated lab records why a captured value was replaced — so `CaptureConcentration` gains `reason`, required by the model when the capture replaces a value (`lims.lab.reasonRequired`), and the audit trail shows it from the journal. The e-signature's password and its wrong-password state are not built: lims signs in by name (`LimsLogin`) and has no password to re-enter; the signature dialog confirms the meaning of the act ("I verify these results as correct") and records it as `VerifyResult` does now. Both are recorded in decision 54. The sample detail's reagents section (Part C Task 6's reservations, with the lost race naming its holder) waits for Part C Task 6 and is added when it lands, under this task's tests.

- [ ] **Step 1: Read the mockups and the gallery screens; list what each element needs**

For each of `14-Samples`, `15-SampleDetail`, `16-ResultEntry`, `17-Verification`, `18-Audit`, and its gallery screen, write each element and its ui/1 node in the scratch review file. The README's list for these screens: a table cell holding two lines or several badges (a `column` or a `forEach` in the cell: expressible); tinted rows by a condition (`TableRowView` tone: expressible); a selection bar with "select all 1,284" beyond the page (`table` selection plus a text over the query's total: expressible); filter chips that remove a filter (`badge` with `onRemove`: expressible); a facet column with counts (a `column` of checkboxes with counts from the query: expressible); `steps` with a second muted line per step (time and person) — `Step` has a label and a state only (`view_tree.md`): **framework**, Step 4; editable numeric cells in a grouped table with Enter moving to the next cell and a value-or-qualifier cell (the gallery's `results.cpp` does it: expressible); a dialog opened by a cell's change that can undo it (the gallery's: expressible); the signature dialog's password with an attempt count (domain: not built); a queue item disabled with its reason as text (expressible).

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

`CaptureConcentration` gains, last, `std::string reason;` with `"reason"` added to its `optionalFields`; `validate()` keeps every existing condition and adds `reason.size() <= 256`. `applyCapture` refuses with `ValidationError{"lims.lab.reasonRequired", {}, "CaptureConcentration: replacing a captured value needs a reason"}` when a result for the same analysis version exists and the reason is empty (the existing loop that deletes the earlier answer is where it knows). The audit trail's step for a capture shows the reason from the journalled payload. This changes the action's journal fingerprint; lims's shipped code never replays an older journal (README, "Expected strain points"), so no migration is registered — say so in decision 54.

The screens, each a function in `lab_screens.cpp` with a doc comment naming its mockup and gallery design:

- `samplesScreen()` (14): the page head (eyebrow, title, a "New sample" action opening a dialog with the registration fields, which runs `RegisterSample` at the context's site); filters (search, status chips, priority) feeding the `ListSamples` query's `TableQuery` (sort, filters, page of 50); the table (`sampleRow` per row: the reference and client as two lines, the status badge in words, the tests as quiet badges, the due day, the out-of-specification tint; `selectColumn`); the selection bar (`selectionBar`: count, "Select all 1,284" over the query's total, bulk actions available to the role); the pager; the compact layout as cards with the same selection; loading, empty ("No samples at Köln yet") and refused states.
- `sampleDetailScreen()` (15), param `sampleId`: `OpenSample` on mount; the lifecycle as `steps` with each step's detail (person and day from `GetAuditTrail`); the analyses and results; the transitions the state allows, each a button whose refusal shows in words; the billing the sample causes (`ListBillableLines`, money in the line's currency); the chain of custody (the trail's steps); the reagents section (Part C Task 6).
- `resultEntryScreen()` (16), param `sampleId`: the queue of the analyst's samples in progress beside the grid of analyses; per analysis the value-or-qualifier cell (a `textInput` with the version's unit and decimals from `GetAnalysisSchema`, and a qualifier select), Enter moving focus to the next cell (`keys` with `focus`); a change to a captured value opens the reason dialog (`watch` on the cell), whose Save runs `CaptureConcentration` with the reason and whose Cancel restores the cell.
- `verificationScreen()` (17): the queue of samples to verify; an item the verifier captured disabled with "You captured this result" as text; the results with their flags; the signature dialog confirming the meaning and running `VerifyResult` for each selected result.
- `auditScreen()` (18), param `sampleId` (optional; all the caller's samples otherwise): facets (who, action, day range) with counts beside the read-only trail table; active facets as removable chips; an export action (`downloadToFile` of the visible rows as CSV).

Register the five in `bundle()` with module `lab`, remove the three old screens once their moved tests pass, and update the render smoke's mounted list. `scenario.cpp` gains the lab records the mockups show (the 1,284 samples are generated: the eight the gallery design names, then routine ones with references and days that sort as the design's).

- [ ] **Step 4: Framework — a step's second line**

`morph::ui::Step` gains `std::string detail` (a muted second line: who and when); `author.hpp`'s `steps` items accept it; the interpreter and both frontends draw it (Qt Quick under the label, muted; the terminal after the label, in brackets); `view_tree.md`'s `Steps` row says so; a framework test over the recording backend (mutation: the frontend drops the detail). One independent review (default model) of the change with the steps widget's frontend code as its material.

- [ ] **Step 5: Build, run, check the production criteria, measure the list**

Run: the framework suite `"[ui]"`, Build lims, Model suite `"[result]"`, Screen suite `"[lab]"`, Render smoke. Check each screen against the production criteria as Task 5 Step 5 lists them. Measure the sample list at 10,000 samples (seed 10,000 instead of 1,284 in a `Stress:`-named case, label `stress`): the time from a sort or filter change to the page shown, three runs, written down as measured; `ListSamples` runs `table::apply` over every visible sample in memory (`sample_model.cpp`), which is hypothesis 10's subject — record the numbers in decision 54 and leave the change of `ListSamples` to the paging seam Task 9 builds, if Task 9 confirms hypothesis 10 (then `ListSamples` moves onto it in Task 9's last step).

- [ ] **Step 6: Grab beside the mockups**

`scripts/shoot_lims.sh` for the five screens (their rows added to the script's table, each with the person the mockup shows), compared and classified until only domain and environment items remain.

- [ ] **Step 7: Record decision 54 and commit**

```markdown
### 54. The lab screens are the gallery's designs over lims's models; a replaced value keeps its reason

The sample list, sample detail, result entry, verification and audit screens follow the gallery's designs 05, 02/10,
08, 09 and 04 in the shell, keyed by sample. A capture that replaces a value needs a reason, which the journal keeps
and the audit trail shows; the action's fingerprint changed with it, and lims's shipped code replays no older journal,
so no migration is registered. The e-signature confirms the meaning of verifying and asks for no password: lims signs
in by name and has none to re-enter. A lifecycle step shows who and when, which ui/1 now says (`Step::detail`). The
sample list pages in memory over every visible sample: measured at 10,000 samples, a sort or filter change shows its
page in [n] ms (three runs: [n], [n], [n]). [Domain differences from the grabs.]
```

```bash
cd /home/yaraslau/repo/morph
git add examples/lims/screens examples/lims/lab examples/lims/tests/test_result_entry.cpp scripts/shoot_lims.sh \
        examples/lims/README.md include/morph src docs/spec tests
git commit -m "Build lims's lab screens from the gallery's designs over the real models

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01No1NznqxEBvjhZBaBRLCd6"
```

---

### Task 7: Billing — price lists, invoices, the invoice editor and billing runs (mockups 19–21, 23; hypothesis 9, first instance)

**Files:**
- Create: `examples/lims/screens/billing_screens.hpp`, `examples/lims/screens/billing_screens.cpp`, `examples/lims/screens/tests/test_lims_billing_screens.cpp`
- Modify: `examples/lims/billing/include/lims/billing/dto/price_dto.hpp` and `price_list_model.hpp`/`.cpp` (`SetPrices`, one draft saved whole), `examples/lims/billing/include/lims/billing/dto/run_dto.hpp` and `billing_run_model.hpp`/`.cpp` (`ListBillingRunItems`), `examples/lims/billing/src/models/*.cpp` (the refusals the mockups show get codes), `examples/lims/tests/test_price_lists.cpp`, `examples/lims/tests/test_billing_run.cpp`, `examples/lims/billing/include/lims/billing/authorization/billing_authorization.hpp`/`.cpp`, `declared_requirements.hpp`, `test_declared_authorization.cpp`, `examples/lims/screens/scenario.cpp`, `lims_screens.cpp`, both catalogues, `scripts/shoot_lims.sh`, the render smoke, `examples/lims/README.md` (decision 55)

**Interfaces:**
- Consumes: `PriceListModel` (`ListPriceLists`, `CreatePriceList`, `SetPrice`, `QuotePrice`, `GetBillingSettings`), `InvoiceModel` (`ListInvoices`, `GetInvoice`, `GetInvoiceLease`, `CreateInvoice`, `SubmitInvoice`, `ApproveInvoice`, `ListPendingApprovals`; `PostInvoice` after Part B Task 14), `InvoiceEditModel` (`LeaseInvoice`, `ReleaseInvoice`, `RemoveInvoiceLine`, `SetInvoiceReference`), `BillingRunModel` (`ListBillingRuns`, `StartBillingRun`), `LedgerModel::ListPeriods` (the closed-period refusal's words), the kit, `kit::refusalBanner`, the shell's context.
- Produces: `priceListsScreen()`, `invoicesScreen()`, `invoiceEditorScreen()`, `billingRunsScreen()`; `SetPrices { PriceListId priceList; std::vector<PriceEntry> prices; }` (`PriceEntry { AnalysisId analysis; ClientId client; Amount amount; }`) → `PriceListView`, every price set in one transaction or none; `ListBillingRunItems { BillingRunId run; }` → `ListBillingRunItemsResult { std::vector<BillingRunItemView> items; }` (`BillingRunItemView { ClientId client; std::string clientName; SiteId site; std::string state; InvoiceId invoice; std::string error; }`, the state through the kit's words); refusal codes `lims.billing.invoiceLeased` (Task 3), `lims.billing.periodClosed`, `lims.billing.deletedMeanwhile`, `lims.billing.notDraft`, `lims.billing.overLimit`, `lims.billing.sameActor`.

The invoice editor is hypothesis 9's first instance: a screen whose header, lines grouped by analysis and expandable to their samples, totals and approval route come from billing's `InvoiceModel`, whose lease and edits come from the editor's private `InvoiceEditModel`, and whose page turns read-only while another principal holds the lease — two models of one module. What hypothesis 9 says ui/1 lacks is an *action* spanning several models; the measurement is whether the editor needs one. The screen is built with ui/1 as it stands; the decision records whether any of its operations had to commit across both models in one action (expected: none — each edit is the edit model's, each transition the invoice model's).

The price list's "next year's draft edited in place" is an input per row saved as one draft: a domain action, `SetPrices`, sets every price of one list in one transaction, so the draft is saved whole or not at all. The billing runs' per-client outcomes need the run's items, which billing holds and does not list: `ListBillingRunItems` reads them.

- [ ] **Step 1: Read the mockups; list what each element needs**

`19-PriceLists`, `20-Invoices`, `21-InvoiceEditor`, `23-BillingRuns`. The README's list: a table whose column is an input per row with a computed change column (`textInput` cells and an expression column: expressible; saved by `SetPrices`); a row badge naming the lease holder (a `GetInvoiceLease` per row would be one query per row — the invoice list reads the leases in one query instead: add `ListInvoiceLeases { CompanyId company; }` → the live leases of the company's drafts to billing, a read); status counts in the segmented filter (counts over the list: expressible with `filter` and `count`); totals under a paged list (`sum` over the page's gross is not the total of all pages: `ListInvoices` answers the total of the filtered set — add `total` to `ListInvoicesResult`, a domain read); a refusal that offers to change the scope (a banner with an action that sets the context: the context is the shell's — Task 4 gives screens a command to choose a context option; if it does not, add it there); the editor's multi-model page, read-only while leased (`enabled` bound to "this editor holds the lease": expressible); a field error and a banner from one refused action (the refusal's code decides which field: expressible with `refusal()`); live progress of a server job with a resume note (`progress` over `done`/`total`, refreshed by the run's declared effect: expressible); a nested per-client table under the selected run (expressible).

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
#include "scenario.hpp"

namespace {

using morph::client::LocalApp;
using morph::client::LocalSetup;
using morph::examples::testing::RigMode;
using morph::examples::testing::ScreenHarness;

LocalApp limsApp() { return LocalApp{.name = "lims", .bundle = &lims::screens::bundle, .setUp = {}}; }

void reportProblems(ScreenHarness const& harness) {
    for (std::string const& problem : harness.problems()) {
        UNSCOPED_INFO(problem);
    }
}

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
    CHECK(harness.prop("TextInput", "reference", "enabled") == "false");
    CHECK(harness.prop("Button", "removeLine", "enabled") == "false");
    reportProblems(harness);
    CHECK(harness.problems().empty());
}
```

`lims::screens::scenarioDraftInvoice()` is a function `scenario.hpp` declares: the id of the scenario's draft for Stadtwerke Köln (the scenario's records are named by the mockups; a test asks the scenario for the ids it needs, and never hard-codes one). `prop` is Task 5's helper. The parameter map's literal form is `ui::ParamMap`'s; read `screen_harness.hpp`'s constructor and use its type. Then, each with its mutation, written the same way:

- "the price list's draft is edited in place and saved as one draft, with the change shown per row" (mutation: Save runs `SetPrice` per row);
- "a yen price list takes whole yen" (Task 5's bound decimals; mutation: decimals 2);
- "the invoice list's out-of-scope refusal offers to change the scope" (mutation: no action on the banner);
- "the invoice list shows loading, then an empty state for a new company" (mutation: no empty state);
- "the editor's lines are grouped by analysis and expand to their samples" (mutation: lines not grouped);
- "the editor leases on open, releases on close, and another editor's list hears the release" (`ReleaseInvoice`'s `changes<InvoiceLeaseView>`; mutation: the editor does not release on close);
- "a submission another client made reaches an open approvals list" (the existing `test_lims_approval_inbox.cpp` measurement, kept);
- "the editor's draft deleted meanwhile shows words and a way back" (`NotFound` with `lims.billing.deletedMeanwhile`; mutation: the raw refusal shows);
- "posting into a closed period is refused in words" (**after Part B Task 14**; `lims.billing.periodClosed` with the period's month; mutation: the message shows);
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
                            ui::t("lims.billing.draftFor", "Draft for {client}", {{"client", invoice.at("clientName")}}),
                            invoice.at("number"));
        };
        auto const editor = ui::element("open");
        return ui::column(
            {kit::refusalBanner(ui::coalesce({listed.refusal(), leases.refusal()})),
             ui::busy(listed.loading(), ui::t("lims.loading", "Loading…")),
             ui::select({.options = kit::statusFilter<InvoiceState>(all), .selected = status,
                         .onSelect = ui::setTo(status), .style = morph::ui::SelectStyle::Segmented})
                 .testId("statusCounts")
                 .a11y("Invoice status"),
             ui::emptyState({.title = ui::t("lims.billing.noInvoices", "No invoices yet"),
                             .text = ui::t("lims.billing.noInvoicesText",
                                           "Invoices appear here once a billing run or a clerk drafts them.")})
                 .visible(ui::and_(ui::not_(listed.loading()), ui::isEmpty(all))),
             ui::byWidth(width,
                          {kit::invoiceCards(shown, nameOf, leaseOf)},
                          {ui::table(shown,
                                     {.key = ui::row("id"),
                                      .columns = {{.name = "name", .label = ui::t("lims.billing.invoice", "Invoice"),
                                                   .cell = ui::text({.text = nameOf(ui::row())})},
                                                  {.name = "state", .label = ui::t("lims.billing.status", "Status"),
                                                   .cell = kit::invoiceStateBadge(ui::row("state"))},
                                                  {.name = "lease", .label = "",
                                                   .cell = kit::leaseBadge(leaseOf(ui::row())).testId("leaseBadge")},
                                                  {.name = "gross", .label = ui::t("lims.billing.gross", "Gross"),
                                                   .cell = ui::text({.text = kit::money(ui::row("gross"),
                                                                                        ui::row("currency"))})}},
                                      .onActivate = ui::navigate(std::string{kInvoiceEditor},
                                                                 {.params = ui::obj({{"invoiceId", ui::row("id")}})})})
                               .testId("invoiceList")
                               .a11y("Invoices")}),
             ui::text({.text = ui::format(ui::t("lims.billing.total", "Total {0}"),
                                          {kit::money(listed.value().at(&ListInvoicesResult::total), context.at("currency"))})})
                 .testId("listTotal")},
            {.gap = 3});
    });
}
```

This is the shape, not a transcript of a working build: the author API's exact spellings for the pieces it uses — `screen.state`, `ui::filter`, `ui::var`, `ui::or_`/`ui::and_`/`ui::not_` (the header may spell them as operators `||`, `&&`, `!` on `Handle<bool>`, as `org_screens.cpp` and the gallery do), `ui::setTo`, `query.loading()`, `query.refusal()` (Task 3), `t` with named arguments — are `author.hpp`'s; read it, use what it declares, and keep the structure: the context's company in every query's body, every text through the kit or a catalogue key, the empty and loading states, the refusal banner, the table's `onActivate` opening the editor, and compact cards (`kit::invoiceCards`, a kit function over `forEach`) in place of the table. `kit::statusFilter<E>` (segmented options with each state's count over the rows), `kit::invoiceStateBadge` and `kit::leaseBadge` ("{holder} is editing", hidden without a live lease) are added to the kit with complete doc comments.

`priceListsScreen()` (19): the context company's lists by validity, the selected list's prices, next year's draft editable in place (an input per row, the change against the list in force as a computed column, Save running `SetPrices`), a yen list taking whole yen. `invoiceEditorScreen()` (21), param `invoiceId`: a private `InvoiceEditModel` alias (`.instance = ui::Instance::Private`), `LeaseInvoice` on mount and `ReleaseInvoice` on close; the header (client, site, reference input), lines grouped by analysis with a heading row per analysis and the samples beneath (`TableRowView` headings, collapsible by a state per analysis), totals (net, tax, gross in the invoice's currency), the approval route (`steps` with detail: drafted by, submitted by, approver due by the deadline, escalated after it), Submit / Approve / Post (Post after Part B Task 14) each enabled by the invoice's state and the caller's role, the page's inputs enabled only while this editor holds the lease (a `leaseBanner` naming the holder otherwise), and the refusals in words. `billingRunsScreen()` (23): the schedule (billing day from the settings), the run in progress with `progress` over done/total and a resume note when its start is before the server's last start (the run's `startedAt` against the newest run tick, a field `BillingRunView` carries — add `lastTickAt` to it if it does not), past runs, and the selected run's items (`ListBillingRunItems`) with each failure's text in words.

- [ ] **Step 4: Build, run, check the production criteria**

Run: Build lims, Model suite `"[billing]"`, `"[authorization]"`, Screen suite `"[billing]"`, Render smoke. Check each screen against the production criteria (Task 5 Step 5's list), the list totals and the lease badge with a second clerk holding a lease. Run every mutation once and restore it. Write down for the h9 decision: did any editor operation need one action across `InvoiceModel` and `InvoiceEditModel` (expected: no — name each operation and its one model), and did the screen meet every production criterion with ui/1 as it stands (after the framework steps Tasks 3–5 added)?

- [ ] **Step 5: Grab beside the mockups**

`scripts/shoot_lims.sh` for the four screens, compared and classified until only domain and environment items remain; a framework difference is added to the framework task its subject belongs to and fixed before this task is done.

- [ ] **Step 6: Record decision 55 and commit**

```markdown
### 55. The billing screens match their mockups, and the invoice editor needed no action across models (hypothesis 9, first instance)

Price lists (19), invoices (20), the invoice editor (21) and billing runs (23) are built to their mockups. A price
list's draft is saved whole by `SetPrices`; the invoice list reads its leases and its total in one query each
(`ListInvoiceLeases`, `ListInvoicesResult::total`); a run's per-client outcomes are `ListBillingRunItems`.

**Hypothesis 9 is [refuted | confirmed] for the invoice editor.** Built with ui/1 as it stands (with refusals in
words, the shell's context and a bound input's decimals added to morph on the way): its header, grouped lines,
totals and approval route are `InvoiceModel`'s, its lease and edits the editor's private `InvoiceEditModel`'s, and
[no operation needed one action across the two: each edit is one model's action, each transition the other's | the
operation that needed both was …]. The page turns read-only while another clerk holds the lease, and says who.
[Domain differences from the grabs.]
```

```bash
cd /home/yaraslau/repo/morph
git add examples/lims/screens examples/lims/billing examples/lims/app/include/lims/app/declared_requirements.hpp \
        examples/lims/tests scripts/shoot_lims.sh examples/lims/README.md
git commit -m "Build lims's billing screens to their mockups and measure the invoice editor against hypothesis 9

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01No1NznqxEBvjhZBaBRLCd6"
```

---
### Task 8: The invoice print view, laid out as a document (mockup 22; hypothesis 9, the document instance)

**Files:**
- Create: `docs/spec/ui/document_page.md`, `tests/test_ui_document_page.cpp`
- Modify: `include/morph/ui/view.hpp`, `include/morph/ui/author.hpp` (`page`, `pageRegion`, `localeScope`), the interpreter, the Qt Quick and terminal frontends, `docs/spec/ui/view_tree.md`, `docs/spec/ui/expressions.md` (formatting under a scope's locale), `docs/spec/README.md`, `examples/lims/billing/include/lims/billing/dto/price_dto.hpp`, `billing_entity.hpp`, `billing_schema.cpp` and `price_list_model.cpp` (a billing client's address and locale), `examples/lims/tests/test_price_lists.cpp`, `examples/lims/screens/billing_screens.hpp`/`.cpp` (`invoicePrintScreen()`), `examples/lims/screens/tests/test_lims_billing_screens.cpp`, `lims_screens.cpp`, both catalogues, `scripts/shoot_lims.sh`, the render smoke, `examples/lims/README.md` (decision 56)

**Interfaces:**
- Consumes: `InvoiceModel::GetInvoice`, `PriceListModel::GetBillingSettings`, `OrgModel::findCompany`'s read (`ListCompanies`), the billing client's address and locale (`BillingClientView`; `RegisterBillingClient` and its view gain `address` (at most 512 bytes) and `locale` (a BCP 47 tag, `en` by default), stored in `billing_clients` by migration `20261130000001` "Give billing clients an address and a locale" (`AddNotRequiredColumn` for each; a client registered before reads as no address and `en`) — a German client's invoice is in German whatever the clerk's locale), the kit.
- Produces: `ui::author::page(PageOptions{width, height, margins}, Nodes)` — a fixed-size page in millimetres, laid out at that size and scaled to the available width when the window is narrower (compact); `ui::author::pageRegion(RegionOptions{anchor: top|bottom, offset}, Nodes)` — content pinned to a page's top or bottom margin (the letterhead, the footer) or placed at an offset (the address window, DIN 5008's 45 mm × 85 mm at 20 mm from the left and 45 mm from the top); `ui::author::localeScope(AnyHandle locale, Nodes)` — `money`, `date`, `number` and `t` inside it format in that locale; `invoicePrintScreen()`.

The print view is the design's document instance of hypothesis 9: "a fixed-size A4 page laid out as a document (letterhead, address window, footer pinned to the page bottom), in a locale other than the user's, scaled to the width when compact. Nothing in ui/1 lays out a page." (the mockups' README). Rendering the page to a file or a printer is out of the design's scope ("PDF and print-file rendering"; "Document rendering … is domain output and is not built in morph at all"): the mockup's Print and Save buttons are not built, and decision 56 says so. What is built is the page on screen, which needs three words ui/1 does not have — a page of fixed physical size scaled to fit, regions pinned within it, and formatting in a locale other than the session's. Each is mechanism (a CRM's quote or a bank's statement is the same page), and none knows an invoice.

- [ ] **Step 1: Measure first: can ui/1 as it stands lay out the page?**

Try the page with the vocabulary as it stands — a `column` of fixed width (`LayoutHints` `fixed`) for the sheet, a `spacer` with `stretch` to push the footer down, `money` and `date` for the figures — and write down, against mockup 22, what it cannot do: a physical size (units are backend units, not millimetres); a footer pinned to the page's bottom when the page's height is fixed and the content short (a `stretch` spacer needs a container whose height is fixed, and a `column`'s height follows its content); an address window at an offset; the client's locale while the session's is the clerk's; the page scaled to the width when compact (nothing scales a subtree). Each point that ui/1 can say after all is dropped from Step 2; what remains is the measured gap, and the h9 verdict for this instance (expected: confirmed — no document layout).

- [ ] **Step 2: The spec, one review, failing framework tests, the implementation**

`docs/spec/ui/document_page.md`: the three nodes, their units (millimetres in the document; each frontend maps to its own units at a stated density — Qt Quick at the screen's logical DPI, the terminal at a fixed cells-per-millimetre so a page reads as a page of text), scaling (a page wider than its container is scaled down uniformly, never up; the terminal does not scale and scrolls), pinning and offsets (a region's anchor is the page's margin box; overlapping regions draw in order), locale scope (every formatting function and `t` inside evaluates with the scope's locale; the session's locale is unchanged outside), accessibility (the page is a document region; its reading order is the node order), and alternatives considered (a `custom` QML component per document: per-application client code the design forbids; rendering to an image on the server: document rendering is out of scope). Index it; amend `view_tree.md` and `expressions.md`. One independent review (default model) with the layout code of both frontends and the interpreter's formatting context as its material. `tests/test_ui_document_page.cpp` over the recording backend: a page's size and scale at a narrower width, a footer at the bottom of a short page, an address window at its offset, and `money` in `de` inside a scope while the session is `en` (each with its mutation). Implement in the files listed.

- [ ] **Step 3: The print view**

`invoicePrintScreen()`, param `invoiceId`: `page({.width = 210, .height = 297, .margins = {20, 20, 25, 20}})` holding the letterhead region (the company's name and address from the scenario's company, pinned top), the address window (the client's name and address at DIN 5008's offset), the invoice's facts (number — or "Draft" before posting — date, client reference, the period billed), the lines table (analysis, samples, quantity, unit price, line total), the totals (net, VAT at the company's rate, gross), the payment terms and the bank details in the footer region (pinned bottom), all inside `localeScope(<the client's locale>)`, so a German client's invoice reads `8.365,11 €` and `31.10.2026` whatever the clerk's locale. Screen cases in `test_lims_billing_screens.cpp`: "the print view lays an invoice out as a German A4 page while the clerk works in English" (mutation: no locale scope — `€8,365.11` shows) and "the print view scales its page to a compact window" (mutation: page not scaled — the probe's width property exceeds the window's). A posted invoice's number needs Part B Task 14; before it, the print view of an approved invoice shows "Draft", and the posted case is added to the test when Task 14 lands.

- [ ] **Step 4: Build, run, grab, record decision 56, commit**

Run: the framework suite `"[ui]"`, Build lims, Screen suite `"[billing]"`, Render smoke; grabs beside `22-InvoicePrint` at both widths, classified until only domain and environment items remain.

```markdown
### 56. The invoice print view is a page ui/1 lays out, in the client's locale; nothing renders it to a file (hypothesis 9, document instance)

**Hypothesis 9 is confirmed for the document instance, and morph now says it.** Measured against mockup 22 with ui/1
as it stood: [what Step 1 found it could not do]. morph gained a fixed-size page scaled to fit, regions pinned within
it, and a locale scope for formatting (`docs/spec/ui/document_page.md`); the print view uses them. The mockup's Print
and Save are not built: rendering a document to a printer or a file is out of the design's scope.
```

```bash
cd /home/yaraslau/repo/morph
git add docs/spec include/morph src tests examples/lims/screens examples/lims/billing scripts/shoot_lims.sh \
        examples/lims/README.md
git commit -m "Lay out lims's invoice as a document page in the client's locale

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01No1NznqxEBvjhZBaBRLCd6"
```

---

### Task 9: Ledger — accounts, journal, trial balance and period close (mockups 24–27; hypothesis 10, first instance)

**Files:**
- Create: `examples/lims/screens/ledger_screens.hpp`, `examples/lims/screens/ledger_screens.cpp`, `examples/lims/screens/tests/test_lims_ledger_screens.cpp`, `examples/lims/tests/test_ledger_scale.cpp`; if Step 2 confirms hypothesis 10: `docs/spec/table/database_pages.md`, `tests/test_table_database_pages.cpp`
- Modify: `examples/lims/ledger/include/lims/ledger/dto/entry_dto.hpp` and `report_dto.hpp`, `ledger_model.hpp`/`ledger_entries.cpp` and `ledger_report_model.hpp`/`.cpp` (the paged list and the totalled report, Step 3), the ledger's refusals the mockups show (codes), `include/morph/table/query.hpp` and `docs/spec/table/engine.md` (Step 3, if confirmed), `examples/lims/screens/scenario.cpp`, `lims_screens.cpp`, both catalogues, `scripts/shoot_lims.sh`, the render smoke, `examples/lims/README.md` (decision 57)

**Interfaces:**
- Consumes: `LedgerModel` (`ListAccounts`, `CreateAccount`, `ListPeriods`, `OpenPeriod`, `BeginClosePeriod`, `ClosePeriod`, `CreateDraftEntry`, `PostEntry`, `ReverseEntry`, `GetEntry`, `ListEntries`, `SetExchangeRate`, `ListExchangeRates`), `LedgerReportModel` (`TrialBalance`, `AccountBalance`, `AccountLedger`), `GetBillingSettings` and `GetInventorySettings` (which modules post to an account), `morph::table::TableQuery`, `morph::table::apply`, the kit.
- Produces: `accountsScreen()`, `journalScreen()`, `trialBalanceScreen()`, `periodCloseScreen()`; `ListEntriesPage { CompanyId company; ::morph::table::TableQuery query; }` → `::morph::table::Page<JournalEntryView>`; refusal codes `lims.ledger.periodClosed`, `lims.ledger.unbalanced`, `lims.ledger.draftsRemain`; if confirmed, morph's database-page seam (Step 3).

Hypothesis 10's first instance is here: the journal over a large table and the trial balance over it. `ListEntries` reads every entry of a company and `TrialBalance` loads every posted line it adds up (decision 30 measured 2·10⁵ lines and inferred about 11 s at 10⁷). The production criteria ask for a list of 10,000 rows that scrolls, sorts, filters and pages without blocking, and the design's refutation condition for hypothesis 10 asks for 10⁶ database rows listed without loading them, with the total shown, and a trial balance over 10⁷ posted lines answering exactly without loading them all. The task measures first, then builds the seam both instances need if the measurement confirms it (the second instance, stock movements, is Task 11's and uses it).

- [ ] **Step 1: Read the mockups; list what each element needs**

`24-Accounts`, `25-Journal`, `26-TrialBalance`, `27-PeriodClose`. The README's list: tables with two currency column pairs and a totals row (columns and a final heading row: expressible); credit balances with a suffix (`kit::balance`); tree-grouped accounts (heading rows per type: expressible); a dialog whose table rows are editable account and amount inputs, kept balanced (a `state` list edited per row, the difference computed with `sum`, Post enabled only at zero: expressible); a grid of twelve period tiles with state (`grid` of `panel`s with badges: expressible); a checklist whose items carry their own actions (a `column` of rows, each with its button: expressible); a typed-confirmation dialog (a `textInput` whose value must equal the period's label to enable Close: expressible). Which modules post to an account is not a ledger fact: the accounts screen reads billing's and inventory's settings and marks each account their codes name — the ledger stays domain-free.

- [ ] **Step 2: Measure hypothesis 10 on the ledger**

Create `examples/lims/tests/test_ledger_scale.cpp` with two `Stress:` cases (label `stress`, 600 s), each writing its rows through Lightweight's `DataMapper` in batches inside one transaction per 10,000 (the posting path's cost is not what is measured):

- "Stress: Hypothesis 10: a page of the journal over 10^6 entries": 10⁶ posted two-line entries for one company; then `ListEntries` (today's read) and a page of 50 through `table::apply` over it, timed, with the process's peak resident memory before and after (`/proc/self/status` `VmHWM`); record both.
- "Stress: Hypothesis 10: a trial balance over 10^7 posted lines": 10⁷ lines (5·10⁶ two-line entries) of up to 10⁹ minor units; `TrialBalance` timed, peak memory recorded, totals checked exactly against an independently accumulated `int64` sum.

Expected (inferred from reading `ledger_entries.cpp`, `ledger_report_model.cpp` and `table::apply`): every entry and every line is loaded; the page's time and the memory grow with the table, and the trial balance at 10⁷ takes tens of seconds or does not fit. Write the numbers down; that is the measured verdict for this instance. If both answer within the production criteria without loading every row, hypothesis 10 is refuted here: skip Step 3, and decision 57 says so with the numbers.

- [ ] **Step 3: If confirmed — the database-page seam, specified, reviewed, built, and the ledger on it**

`docs/spec/table/database_pages.md`: a model that holds its rows in a database pages them there. `morph::table` gives such a model what it needs to do so correctly, without morph knowing the database: `table::plan(TableQuery const&, std::span<Column const>, QueryLimits const&) → std::expected<QueryPlan, TableError>`, the validated and normalised query (sort keys with their column and direction, each filter as column, operator and typed operand, offset, limit, and whether the total is wanted), which the model translates into its own query; and `table::Page` answered with an exact total above `QueryLimits::totalCap` when the source counted it itself (`Page::totalExact`, a flag the client shows as "1,000,000 entries", never "more than 100,000"). The engine's `apply` stays the reference implementation for rows in memory and is the oracle the seam's tests compare a database's pages against. What it does not do: translate to SQL (morph has no ORM; lims's Lightweight is the example's), aggregate. Alternatives considered: a SQL dialect in morph (couples morph to databases), streaming every row to the engine (what is measured as too slow). Index it; one paragraph in `engine.md`. One independent review (default model) with `query.hpp`, `engine.md` and the ledger's two reads as its material; address every finding. `tests/test_table_database_pages.cpp`: `plan` normalises sort and filter as `apply` interprets them (same pages over an in-memory source sorted and filtered by the plan as by `apply`, for every operator), and refuses what `validate` refuses (each with its mutation). Implement.

Then the ledger: `ListEntriesPage` translates the plan into a Lightweight query over `ledger_entries` (the plan's sort to `OrderBy`, its filters to `Where`, offset and limit) and counts the filtered set with one `COUNT` for the exact total; a filter or sort the query builder cannot express is the escape tier (rule 4) with a README finding, or a refusal in words — never a fallback that loads every row. `TrialBalance` sums each account's posted lines in the database: company amounts are whole minor units (`fitsCurrency`), so the sum is an integer sum of `amount_num × (10^decimals / amount_den)` per account and side; SQLite's `SUM` raises an error on integer overflow rather than wrapping (its documentation, "Built-in Aggregate Functions"), which the ledger turns into `OutOfRange` — read that page and confirm before relying on it. If the query builder cannot express the aggregate, the raw statement is rule 4's escape tier with its README finding. Re-run Step 2's two cases on the new reads and write the numbers down. `SampleModel::ListSamples` (Task 6's measurement) moves onto the same plan in this step, as a third consumer.

- [ ] **Step 4: The four screens, their tests**

`test_lims_ledger_screens.cpp`, each with its mutation:

- "the chart of accounts is grouped by type, marks the modules that post to each account, and one account's ledger runs its balance" (mutation: the running balance is the line's amount);
- "the journal pages 10,000 entries with the exact total, and an entry's lines show both currencies and the rounding line" (over `ListEntriesPage` if Step 3 ran, `ListEntries` otherwise; mutation: the total shown is the page's row count);
- "a manual entry into a closed period is refused in words" (`lims.ledger.periodClosed` with the month in words; mutation: the message shows);
- "the manual entry dialog keeps its lines balanced: Post is enabled only at zero difference" (mutation: Post always enabled);
- "the trial balance is balanced, and the yen company's year to date shows whole yen" (mutation: JPY at two decimals);
- "period close: the checklist blocks the close while a draft remains, and the typed confirmation enables it" (mutation: Close enabled without the typed label);
- "a period closed by another client reaches the open period grid" (`ClosePeriod`'s `updates<PeriodView>`, run through a second mounted copy of the screen; mutation: no effect declared).

Build each screen to its mockup: `accountsScreen()` (24), `journalScreen()` (25, entries by source with the entry detail in a side panel, the manual-entry dialog), `trialBalanceScreen()` (26, one period grouped by type with balanced totals; a company and period picker; the yen company's fiscal year to date), `periodCloseScreen()` (27, the year's twelve period tiles, the exchange rates table with an add form, the checklist — drafts remaining, rates for every foreign currency posted in the period — each item with its action, and the typed-confirmation dialog running `BeginClosePeriod` then `ClosePeriod`). The scenario gains the ledger's records the mockups show (October closing, September closed, the CHF entry with its rounding line).

- [ ] **Step 5: Build, run, check, grab**

Run: Build lims (and the framework suite if Step 3 ran), Model suite `"[ledger]"`, the two stress cases, Screen suite `"[ledger]"`, Render smoke. Check the production criteria per screen; grabs beside 24–27, classified until only domain and environment items remain.

- [ ] **Step 6: Record decision 57 and commit**

```markdown
### 57. The ledger screens match their mockups, and the journal and the trial balance [no longer load their tables] (hypothesis 10, first instance)

The chart of accounts (24), journal (25), trial balance (26) and period close (27) are built to their mockups; which
modules post to an account is read from billing's and inventory's settings, so the ledger stays domain-free.

**Hypothesis 10 is [confirmed | refuted] for the journal and the trial balance.** Measured in
`tests/test_ledger_scale.cpp` on the `linux-everything` Release build: a page of the journal over 10^6 entries took
[n] ms with every entry loaded (peak memory [n] MB → [n] MB), and a trial balance over 10^7 lines [n] s ([n] MB).
[If confirmed:] morph gained the database-page seam (`docs/spec/table/database_pages.md`): a validated query plan a
model translates into its own query, and an exact total above the engine's cap. On it, the same page took [n] ms and
the trial balance [n] s, summed in the database, exact or refused as `OutOfRange`. The sample list moved onto it too.
Stock movements in Part C are the second instance.
```

```bash
cd /home/yaraslau/repo/morph
git add docs/spec include/morph tests examples/lims/ledger examples/lims/lab examples/lims/screens \
        examples/lims/tests scripts/shoot_lims.sh examples/lims/README.md
git commit -m "Build lims's ledger screens and page the journal and the trial balance in the database

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01No1NznqxEBvjhZBaBRLCd6"
```

---

### Task 10: The shell's screens — home, the approvals inbox across modules, search (mockups 11–13), and the deadline effect

**Waits for** Tasks 4, 6, 7, 9 and **Part C Task 2** (requisitions in the inbox); the home's inventory tile waits for **Part C Task 1**.

**Files:**
- Create: `examples/lims/screens/shell_screens.hpp`, `examples/lims/screens/shell_screens.cpp`, `examples/lims/screens/tests/test_lims_shell_screens.cpp`, `docs/spec/core/deadline_effects.md`, `tests/test_deadline_effects.cpp`, `tests/test_ui_chart_bars.cpp`
- Modify: `include/morph/core/records.hpp` (a record's change instant), `include/morph/core/remote.hpp` and its server implementation (fanning out at the instant), `docs/spec/core/` (the records and remote specs' paragraphs), `include/morph/ui/view.hpp`/`author.hpp`, the chart widget of both frontends (`ChartSeries::style` bars), `docs/spec/ui/view_tree.md`, `examples/lims/billing/include/lims/billing/dto/invoice_dto.hpp` and `examples/lims/inventory/include/lims/inventory/dto/requisition_dto.hpp` (each declares its deadline as a change instant), `examples/lims/screens/tests/test_lims_approval_inbox.cpp` (its h4 case now expects the inbox told), `examples/lims/tests/test_requisitions.cpp` (the same), `lims_screens.cpp`, both catalogues, `scripts/shoot_lims.sh`, the render smoke, `examples/lims/README.md` (decision 58; decisions 35 and 43 gain one sentence each)

**Interfaces:**
- Consumes: every module's count, list and search actions (Task 4), `ListPendingApprovals`, `ListPendingRequisitions`, the lab's verification queue (`ListVerifications`), `LedgerModel::ListPeriods`, `LedgerReportModel` (the monthly chart's revenue), the kit, the issue Part C Task 2 filed.
- Produces: `homeScreen()`, `approvalsScreen()`, `searchScreen()`; `::morph::record::changesAt<&View::member>` — a registered record's declaration that its rows change by time alone at the instant the member holds, which the server fans out as `changes<View>` for that key to the watchers it re-authorizes, at that instant, once; `ChartSeries::style` (`line`, the default, or `bars`).

The deadline effect closes the issue Part C Task 2 filed: two modules (invoice approval, decision 35; requisition approval, decision 43) have a filtered list that changes when a deadline passes, with no action at that instant, so no watcher is told. The inbox across modules is where that shows. The mechanism is the server's: a record declares which of its members is the instant it next changes by time, and the server fans out a change for that key at the instant. It knows no deadline, approval or escalation.

- [ ] **Step 1: The deadline effect — spec, one review, failing tests, implementation**

`docs/spec/core/deadline_effects.md`: the declaration (`BRIDGE_REGISTER_RECORD(View, "Name", key<…>, changesAt<&View::deadline>)`), when the server reads it (from every view of the record it serves — a query's reply or an action's — so the server learns a row's instant whenever a client could see it), what it does at the instant (one `changes` for that key, to every connection watching the record, each re-authorized as for any change), what restarts lose (nothing: the instant is re-learned from the next view served), clock source (the server's), and what it does not do (run an action, change data, know why the instant matters). Update the records and remote specs; index. One independent review (default model) with `records.hpp`, the fan-out in `remote.hpp` and the server's timer facilities as its material. `tests/test_deadline_effects.cpp`: a probe model whose view declares an instant 50 ms ahead; a watching connection hears a change for that key after the instant and not before; a second view with a later instant moves it (each with its mutation). Implement.

Then lims: `InvoiceView` declares `changesAt<&InvoiceView::approvalDeadline>` and `RequisitionView` `changesAt<&RequisitionView::approvalDeadline>`. `test_lims_approval_inbox.cpp`'s h4 case and `test_requisitions.cpp`'s effects case now measure the inbox told after the deadline: change each `CHECK_FALSE(told)` to `CHECK(told)` (and the inbox case's count to the escalated one), with the mutation "drop the `changesAt` declaration" (the check fails). Decisions 35 and 43 each gain: "An open inbox is told when the deadline passes: the view declares its deadline as the instant it changes, and the server fans out the change then (decision 58)." Close the issue Part C Task 2 filed with a comment naming the commit, once this lands.

- [ ] **Step 2: Bars — spec, one review, failing test, implementation**

`ChartSeries` gains `style` (`line`, the default, or `bars`); bars of several series stand side by side per x; the terminal draws a bar as a run of block characters per x. `view_tree.md`'s chart rows; one independent review (default model); `tests/test_ui_chart_bars.cpp` over the recording backend (mutation: the style is not passed to the widget). Implement in both frontends.

- [ ] **Step 3: The three screens and their tests**

`test_lims_shell_screens.cpp`, each with its mutation:

- "home: one tile per module with its open work, each loading and failing on its own" (a `boundary` per tile; one module's count action made to fail by a fault hook shows that tile's error and the others' counts; mutation: one boundary around all tiles);
- "home: the period-close warning names the closing period" (mutation: the warning shows the period's id);
- "home: the monthly revenue chart is bars" (mutation: the default line style);
- "approvals: invoices, requisitions and samples to verify in one list, ordered by deadline" (`concatLists` of three `map`s into one row shape — kind, title, amount, deadline, the record's key — then `sort` by deadline; mutation: no sort);
- "approvals: an invoice over the approver's limit has Approve disabled with the limit as the reason" (mutation: the reason missing);
- "approvals: an escalated item reaches an open inbox when its deadline passes" (Step 1's mechanism, measured through the screen; mutation: drop `changesAt`);
- "search: results while typing, grouped by module, the highlight moved by arrows while focus stays in the field, Enter opens" (Task 4's search, its screen form; mutation: Enter opens the first hit whatever the highlight);
- "search: no results offers a way forward" (mutation: an empty list with nothing else).

Build each screen to its mockup: `homeScreen()` (11: greeting with the person's name, the period-close warning from `ListPeriods`, the KPI tiles — label, large value (`text` role `display`), tone, sub-line — each in its own `boundary`, the monthly revenue chart as bars from the ledger's revenue accounts per month, the inbox summary); `approvalsScreen()` (12: the one list, the detail pane switching on the row's kind with that kind's actions — approve an invoice, approve a requisition, open a sample's verification — Approve disabled with "Over your limit of €10,000.00" where the limit does not cover the amount, the empty state); `searchScreen()` (13: the shell's search as a page, for Shift Enter and for compact, with the same grouped results and keyboard). The mockup's "forward to an approver chosen by limit" is not built: limits are confidential (decision 29), so the screen cannot list who has a higher limit; it says that an approver at the company with a higher limit can approve it, and decision 58 records why.

- [ ] **Step 4: Build, run, check, grab, record decision 58, commit**

Run: the framework suite `"[effects]"` and `"[ui]"`, Build lims, Model suite `"[requisition]"`, Screen suite `"[shell]"` and `"[billing]"`, Render smoke. Production criteria per screen; grabs beside 11–13, classified until only domain and environment items remain.

```markdown
### 58. The shell's screens span every module, and an inbox is told when a deadline passes

Home (11), the approvals inbox (12) and search (13) are built to their mockups. The inbox merges invoices,
requisitions and samples to verify into one list with ui/1's `map`, `concatLists` and `sort`, and its detail pane
switches on the kind: a screen over three modules' records needed no action across them. A view now declares the
instant it next changes by time (`changesAt`), and the server fans the change out then, so an open inbox is told when
an approval escalates; both approval lifecycles declare their deadline, and the issue filed for it is closed. Home's
chart is bars, which ui/1 now draws. Forwarding an approval to someone with a higher limit is not built: limits are
confidential, so no screen can list who holds one.
```

```bash
cd /home/yaraslau/repo/morph
git add docs/spec include/morph src tests examples/lims scripts/shoot_lims.sh
git commit -m "Build lims's home, approvals and search screens, and tell an inbox when a deadline passes

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01No1NznqxEBvjhZBaBRLCd6"
```

---

### Task 11: Inventory — stock, requisitions and orders, goods receipt (mockups 28–30; hypothesis 9, second instance; hypothesis 10, second instance)

**Waits for** Part C Tasks 1–6; the receipt dialog's posting preview and its commit wait for Part C Task 4 (so Part B Task 14).

**Files:**
- Create: `examples/lims/screens/inventory_screens.hpp`, `examples/lims/screens/inventory_screens.cpp`, `examples/lims/screens/tests/test_lims_inventory_screens.cpp`
- Modify: `examples/lims/inventory/include/lims/inventory/dto/stock_dto.hpp` and `stock_model.cpp` (`ListStockMovements` on the database-page seam, if Task 9 built it), `…/dto/receipt_dto.hpp` and `goods_receipt.cpp` (`PreviewReceipt`), the inventory refusals the mockups show (codes), `include/morph/ui/view.hpp`/`author.hpp` and both frontends (a table row that spans rows — Step 2), `docs/spec/ui/view_tree.md`, `examples/lims/screens/lab_screens.cpp` (the sample detail's reagents section), `examples/lims/screens/scenario.cpp`, `lims_screens.cpp`, both catalogues, `scripts/shoot_lims.sh`, the render smoke, `examples/lims/README.md` (decision 59)

**Interfaces:**
- Consumes: `StockModel` (`StockOnHand`, `ListStockMovements`, `ListItems`, `ListLocations`, `GetSampleReservation`), `ReservationModel`, `ProcurementModel` (requisitions, suppliers, orders, `ReceiveGoods`, receipts), `LedgerModel::ListExchangeRates`, Task 9's seam, the kit.
- Produces: `stockScreen()`, `requisitionsScreen()`, `goodsReceiptScreen()` (the dialog, opened from an order); `PreviewReceipt` (the `ReceiveGoods` payload, unchanged) → `ReceiptPreview { Amount value; Amount companyValue; std::vector<PostingLine> lines; }` (a read: the posting `ReceiveGoods` would make, computed by the same `receiptPostingFor` with the ledger's rate on the date, nothing written); refusal codes `lims.inventory.noRate`, `lims.inventory.duplicateLot`, `lims.inventory.overReceipt`, `lims.inventory.reservedBy`; a table row spanning rows (framework).

The goods-receipt dialog is hypothesis 9's second instance: order lines from procurement, new lots and locations from stock, and the posting preview from the ledger, committed by one button. In lims the commit is one inventory action (`ReceiveGoods`, Part C Task 4), and the preview a read on the same model, so the dialog spans one model's actions and a read of another's rate; the measurement is whether it needs more. Stock movements are hypothesis 10's second instance: a lot's movements as a database page.

- [ ] **Step 1: Read the mockups; list what each element needs**

`28-Stock`, `29-Requisitions`, `30-GoodsReceipt`. The README's list: a cell of several badges and tinted rows (expressible); an approval route as a timeline with a passed deadline and an escalation (`steps` with Task 6's detail: expressible); a quantity with a unit picker and its converted value (a `textInput` beside a unit `select`, the converted value computed by an expression over the unit's ratio: expressible); a line split into two lots — a row that spans rows: `table` has no row span (`view_tree.md`): **framework**, Step 2; the posting preview (`PreviewReceipt`: a domain read).

- [ ] **Step 2: Framework — a row that spans rows**

A table cell may span the rows below it (`TableColumn` cells answering a span from the row view: `TableRowView` gains `spans`, a list of (column, rows) the row's cells cover downward); both frontends draw it (Qt Quick merges the cells; the terminal repeats the cell's text once and leaves the covered cells blank); `view_tree.md`'s `Table` row says so; a framework test over the recording backend (mutation: span ignored); one independent review (default model) with both frontends' table code as its material.

- [ ] **Step 3: Measure hypothesis 10 on stock movements, and move them onto the seam**

A `Stress:` case in `tests/test_stock.cpp` ("Stress: Hypothesis 10: a page of a lot's movements over 10^6 movements", label `stress`): 10⁶ movements of one item written in batches; a page of 50 through `ListStockMovements`, timed, with peak memory. If Task 9 built the database-page seam, `ListStockMovements` moves onto it (the plan translated into a Lightweight query over `inventory_movements` with an exact count), and the case is run before and after; if Task 9 refuted hypothesis 10, the case records that the in-memory page meets the criteria here too.

- [ ] **Step 4: The three screens and the reagents section, their tests**

`test_lims_inventory_screens.cpp`, each with its mutation:

- "stock: items with their lots, locations, expiry, reserved quantity and value; an expired lot is tinted" (mutation: no tint);
- "stock: one lot's movements page with the exact total" (mutation: the page's count shown as the total);
- "requisitions: an escalated requisition shows its route with the passed deadline, seen by its requester" (mutation: steps without detail);
- "requisitions: purchase orders show their supplier's currency, a yen order in whole yen" (mutation: JPY at two decimals);
- "goods receipt: an order line received in millilitres shows its converted value in the item's unit" (mutation: the converted value is the entered one);
- "goods receipt: one order line split into two lots spans its rows" (Step 2; mutation: no span);
- "goods receipt: the posting preview shows the rounding line, and Receive commits" (**after Part C Task 4**; mutation: the preview omits the rounding line);
- "goods receipt: refused in words for a missing rate and for a duplicate lot" (mutation: the messages show);
- "sample detail: reagents reserved for the sample, and a lost race naming the holder" (the reagents section added to Task 6's screen; mutation: the refusal's message shows).

Build `stockScreen()` (28), `requisitionsScreen()` (29: the requisitions with their routes; the orders by supplier in each supplier's currency; Create order from an approved requisition), `goodsReceiptScreen()` (30: the dialog's order lines; per line the lots, each with number, expiry, location, quantity with a unit picker and the converted value, Split adding a lot under the same line; the preview from `PreviewReceipt`, refreshed when a line changes; Receive running `ReceiveGoods`; loading and empty — an order with nothing left to receive), and the sample detail's reagents section (`GetSampleReservation`, Reserve and Release through a private `ReservationModel`, the refusal naming the holder). `PreviewReceipt` is added to `ProcurementModel` as a read (a Task handler, since it awaits the ledger's rate), with its model test in `test_goods_receipt.cpp` (mutation: a preview that writes the receipt). The scenario gains the inventory records the mockups show (the CHF receipt whose lines lose a cent, the escalated requisition).

- [ ] **Step 5: Build, run, check, grab**

Run: Build lims (and the framework suite for Step 2), Model suite `"[inventory]"`, the stress case, Screen suite `"[inventory]"` and `"[lab]"`, Render smoke. Production criteria per screen; grabs beside 28–30 and 15 (the reagents section), classified until only domain and environment items remain. Write down for the h9 decision: did the receipt dialog need one action across several models (expected: no — `ReceiveGoods` is inventory's one action, the preview inventory's read over the ledger's rate)?

- [ ] **Step 6: Record decision 59 and commit**

```markdown
### 59. The inventory screens match their mockups; the goods-receipt dialog commits one model's action (hypotheses 9 and 10, second instances)

Stock (28), requisitions and orders (29) and the goods-receipt dialog (30) are built to their mockups, and the sample
detail shows the reagents reserved for the sample. A line received as two lots spans its rows, which ui/1 now says.

**Hypothesis 9 is [refuted | confirmed] for the goods-receipt dialog.** Its order lines, new lots and posting preview
come from inventory (`ReceiveGoods`, `PreviewReceipt`, which reads the ledger's dated rate through the posting port),
and one button commits one inventory action; [no operation needed one action across models | …]. With the invoice
editor (decision 55) that is both dialog instances; the print view (decision 56) needed a document layout, which
morph gained.

**Hypothesis 10, second instance.** Measured: a page of a lot's movements over 10^6 movements took [n] ms
([before / after the database-page seam]); [the seam served both instances unchanged | …].
```

```bash
cd /home/yaraslau/repo/morph
git add docs/spec include/morph src tests examples/lims scripts/shoot_lims.sh
git commit -m "Build lims's inventory screens and the goods-receipt dialog to their mockups

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01No1NznqxEBvjhZBaBRLCd6"
```

---

### Task 12: Part D's definition of done, measured

**Waits for** every task above.

**Files:**
- Modify: `examples/lims/README.md` (Status, the client section, decision 60), `examples/LADDER.md` (lims's size), `codecov.yml` (if the screens' sources are a component)

**Interfaces:**
- Consumes: everything above. Produces: nothing new in code.

- [ ] **Step 1: Every screen, both widths, both frontends**

Run: Build lims, Everything labelled for the rung, Render smoke (every screen mounted at both widths: its expected list is every id in `lims_screens.hpp`). Then the terminal frontend, each screen once:

```bash
cd /home/yaraslau/repo/morph
for screen in $(grep -o '"lims\.[a-z.-]*"' examples/lims/screens/lims_screens.hpp | tr -d '"'); do
    scratch=$(mktemp -d)
    (cd "$scratch" && LIMS_SCENARIO=1 script -qec \
        "build/linux-everything/examples/client/morph_examples_client --ui=tui --app lims --user julia --screen $screen --db $scratch/app.db --exit-after-mount" \
        /dev/null) || echo "FAILED: $screen"
    rm -rf "$scratch"
done
```

(Use the client's real path from the build tree, `find build/linux-everything -name morph_examples_client -type f`.) Expected: no `FAILED` line — every screen mounts on the terminal without a problem. Then open three of them (`lims.billing.invoice`, `lims.ledger.journal`, `lims.inventory.goods-receipt`) interactively in a terminal and operate each by keyboard alone; write down what was usable and what was not, as measured.

- [ ] **Step 2: Every screen matches its mockup**

`scripts/shoot_lims.sh /tmp/claude-1000/-home-yaraslau-repo-morph/b1b73e49-c41d-4e27-bb68-b8c0cfacf145/scratchpad/lims-grabs` (every screen), each compared with its mockup at both widths. Expected: the review file lists only domain and environment items, and every domain item is in its task's decision.

- [ ] **Step 3: The production criteria, per screen**

For each of the 22 screens, a row in decision 60's table: loading, empty and error states (by test name), refusals in words (by refusal code), no raw ids or enum names (the screen tests' checks and the grabs), money and quantities in English and German (one screen per module grabbed under `de`: `LIMS_SCENARIO=1` with the scenario person's locale set to `de`), lists of 10,000 rows (the screens whose lists can reach it, by the stress measurement that covers each), keyboard and labels (the no-empty-accessible-name check over every screen, and the three interactive runs), both widths (render smoke), currency (by test). A criterion a screen does not meet is fixed before this step is done.

- [ ] **Step 4: Every Part D hypothesis has a recorded outcome**

`grep -n '^### \(5[0-9]\|60\)\.' examples/lims/README.md` and `grep -n 'Hypothesis 9 is \|Hypothesis 10 is ' examples/lims/README.md`. Expected: decisions 50–60, outcomes for hypothesis 9 (55, 56, 59) and hypothesis 10 (57, 59), every bracket filled with a measured number or the outcome measured.

- [ ] **Step 5: Size, the README, commit**

Count models, actions and screens as Part C Task 9 Step 4 does and record them in the Status paragraph beside the design's estimate (~16 models, ~115 actions, ~18 screens). Update the README's client section to name the production screens by module, and add decision 60 (the per-screen criteria table and the terminal frontend's usability as measured). Then:

```bash
cd /home/yaraslau/repo/morph
git add examples/lims/README.md examples/LADDER.md codecov.yml
git commit -m "Record lims ERP Part D as built: every screen against its mockup and the production criteria

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01No1NznqxEBvjhZBaBRLCd6"
```

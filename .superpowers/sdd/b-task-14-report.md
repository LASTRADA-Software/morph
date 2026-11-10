# Task 14 report: posting an invoice, two commits or one transaction (hypothesis 1, first instance)

Base 4fae29003, branch feat/lims-erp. Labels: MEASURED means a test or command run in this session produced it;
INFERRED means read off the code or reasoned, not run.

## What was built

- `posting.hpp`: `PostingResult.date`, `RateQuery`, `RateAnswer`, and `PostingQuery` (added, see deviations).
- Ledger: `recordPostingIn` (`ledger/db/ledger_store.hpp`, defined in `ledger_entries.cpp`); `LedgerModel::record` is it
  in a transaction of its own (keeping the one retry after a lost unique-key race). New actions `RecordPosting` (service
  principal only, `Authorization<RecordPosting>` = service role at the request's company, `changes<JournalEntryView>`),
  `GetRateOn`, and `FindPosting` (both `Loggable::No`).
- Billing: `ledger_port.hpp/.cpp` (`PostingMode`, `LedgerPort`, `setLedgerPort`, `ledgerPort`), `postingFor` and
  `InvoiceToPost` in `invoice_math`, `NumberSeriesRecord` and migration 20261110000013, `assignNumberAndPost`
  (`invoice_drafts.cpp`), `PostInvoice` DTO and Task handler (`invoice_posting.cpp`), `Authorization<PostInvoice>`.
  `SubmitInvoice` is now a Task handler that values a foreign-currency invoice at `GetRateOn` on the submission date.
  It keeps the lease check (`requireNoOtherEditor`) and the gross guard on the conditional update.
- App: `installLedgerPort` (`ledger_wiring.hpp/.cpp`); `declared_requirements.hpp` (`RecordPosting`, `PostInvoice`; 40
  entries); `src/server/main.cpp` installs the port with `PostingMode::Dispatched`, clears it after `exec()`.
- Tests: `tests/test_invoice_posting.cpp` (13 cases), fixtures `books`, `billingSettings`, `PostingRig`,
  `approvedInvoice` in `billing_test_support.hpp`; `test_declared_authorization.cpp` and `test_posting_contract.cpp`
  updated.
- README decision 38 (the brief's 37).

## Deviations and why

1. `DbFixture` became `LabFixture`; `faultPoint`/the hook take `(point, SqlConnection&)` in this tree, so the cases'
   `failAt` lambda and the fault points pass `mapper.Connection()`. In the dispatched path the mapper is created before
   the point so a connection exists.
2. `entryProblem` takes a 4th argument (`Rate`); the contract case passes `request.rate`.
3. `billingFor` tolerates a second client of the same currency (the price list already exists, `Conflict` from
   `CreatePriceList` returns early; prices are list-wide). Needed for two `approvedInvoice` calls in one case.
   `grantIfAbsent` added for `carl`. `approvedInvoice` takes `std::string const&` (clang-tidy copy finding).
4. **`FindPosting` and `LedgerPort::find` were added (not in the brief).** MEASURED: with the brief's design the retry
   case "numbers the invoice in the fiscal year of the entry it converges on" fails with
   `record: source key billing.invoice:1 is already recorded for a different posting; a corrected document needs a new key`.
   The ledger answers a repeated key only for an identical request, date included (`recordedAs` compares the date), and the
   request is dated by `now()`. The brief's retry only converged in a frozen-clock test. Fix: in the dispatched mode,
   `PostInvoice` asks the ledger for the entry under its key, and if there is one sends that entry's date (and takes the
   rate on that date). Mutation: disabling the lookup makes the fiscal-year case fail with the Conflict above (MEASURED).
5. **The ledger's recording transaction opens with a write that changes no row** (`UPDATE ledger_posting_sources SET entry = ''
   WHERE id = 0`). MEASURED: without it, two clerks posting at once failed with a raw
   `HY000 (5) - [SQLite]database is locked (5)`: SQLite turns a transaction that read then wrote away at once, without
   waiting, when another holds the write lock. A residual busy after the ledger's one retry is a `Conflict`.
   `isBusy` moved from `invoice_edit_model.cpp` to `org/core/model_support.hpp` (shared by billing and ledger).
6. `assignNumberAndPost` uses conditional updates (state `Approved` -> `Posted` where state is still `Approved`; series
   advance only `WHERE next_number = <read>`), not the brief's read-modify-write; a loser gets `Conflict`. Both
   transactional halves map busy to `Conflict` (`PostInvoice: invoice N is being posted by another clerk`).
7. `PostInvoice`/`SubmitInvoice` capture the principal before the first `co_await` (a resumption may not run inside the
   dispatch's session). `SubmitInvoice` does not re-read the row after the await: its conditional update (state Draft and
   the gross read) is the atomic check, and a loser gets the existing `was submitted or edited meanwhile` Conflict.
8. `[approval]` test edits: direct `invoices.execute(SubmitInvoice)` no longer runs a Task handler, so those checks
   dispatch (`dispatchAs`). An unnamed invoice is refused by the dispatch itself (`action failed validation`), so that
   check matches the message rather than the `ValidationError` type.
9. Added two cases beyond the brief: the submission's company-currency gross is asserted (the brief's mutation 8
   was not caught by the brief's test), and a foreign-currency invoice is not submitted while the ledger has no rate.
10. The concurrency case holds both postings between the halves with a rendezvous at the `post.afterLedger` fault point.
    MEASURED: without it, mutation 9 (series read outside the transaction) survived, because the two numbering
    transactions did not overlap.
11. `ledger_port.hpp`: `find` added to `LedgerPort`; `PostInvoice` refuses (`no ledger is wired`) if any of the four
    members is empty.

## Hypothesis 1 verdict: confirmed, with the workaround's cost (branch 2 of the brief). Server keeps `Dispatched`.

MEASURED, `ladder_lims_tests "[h1]" -s` (excerpts; every assertion passed):

(a) Dispatched, both fault points (`post.afterLedger`, `post.beforeCommit`):
```
entries().size() == 1U                      for: 1 == 1
entries().at(0).sourceKey == "billing.invoice:" + id   for: "billing.invoice:1" == "billing.invoice:1"
current(invoice).state == Approved          for: 2 == 2
current(invoice).number.empty()             for: true
!(current(invoice).entry.hasValue())        for: !false
retried.number == "INV-2026-000001"         for: "INV-2026-000001" == "INV-2026-000001"
entries().size() == 1U                      (retry converges on the one entry)
```
An entry exists for an unposted invoice (the window is real); no hole (the retry takes number 1).

(b) Shared transaction, both fault points: `entries().empty()` true, state Approved (2 == 2), number empty, retry
number `INV-2026-000001`, one entry. No partial state, no hole.

(b) gives up (MEASURED, one case per mode; `mode 0` = Dispatched, `mode 1` = Shared):
```
ledgerHeard == dispatched                 mode 0: true == true      mode 1: false == false
journal->holds("InvoiceModel","PostInvoice")   both: true
journal->holds("LedgerModel","RecordPosting") == dispatched   mode 0: true == true   mode 1: false == false
role withheld: mode 0: withheld.post(invoice) throws, entries().empty() true
               mode 1: withheld.post(invoice).state == Posted (3 == 3), entries().size() == 1
```
So the shared transaction loses all three: no `Ledger_Entry` change reaches the accountant, no journal entry for
`RecordPosting`, and the ledger's authorization never runs (the posting succeeds with the posting principal holding no
role). Caveat: in the dispatched mode the withheld refusal comes from the server (the edge's declared-role table or the
ledger's rule); the test does not separate the two.

Other measured costs of the dispatched mode: the retry must repeat the request including its date (deviation 4); the
write lock must be taken first (deviation 5).

## Mutations (each applied, the named test confirmed to fail, file restored and touched)

| # | Mutation | Test | Result |
|---|---|---|---|
| 1 | `postingFor`: skip the rounding line | rounds each line half-up | FAILED `request.lines.size() == 4U` (3 == 4) |
| 2 | round company values to the transaction currency's decimals | EUR invoice into a JPY company | FAILED `request.lines.size() == 4U` (3 == 4) |
| 3 | series created with `next_number = number` (not +1) | next number of its fiscal year | FAILED `"INV-2026-000001" == "INV-2026-000002"` (both modes) |
| 4 | assign the number (committed) before the ledger's half | dispatched H1 | FAILED (number not empty, entry id set, retry `only an approved invoice is posted`, `entries().size() == 1U` 0 == 1) |
| 5 | commit billing's mapper right after `postIn` | shared H1 | FAILED (state 3 == 2, number not empty) |
| 6 | `RecordPosting` registered with no effect | gives-up case | FAILED `ledgerHeard == dispatched`, `false == true`, mode 0 |
| 7 | number by `now()`'s fiscal year, not the entry's | retry fiscal year | FAILED `"INV-2027-000001" == "INV-2026-000001"` |
| 8 | `SubmitInvoice` converts at rate one | USD case (company gross asserted) | FAILED (gross_company != 43.79) |
| 8b | `SubmitInvoice` skips the empty-rate refusal | no-rate case | FAILED (no exception thrown) |
| 9 | read the series' next number before the invoice write, on another connection | two clerks at once (with the rendezvous) | FAILED `PostInvoice: the number series moved on meanwhile`. Without the rendezvous it SURVIVED. |
| 10 (own) | `PostInvoice` ignores `FindPosting` | retry fiscal year | FAILED `already recorded for a different posting` |

Mutation 8 against the brief's USD case alone would have survived: the entry's values come from `PostInvoice`'s own
rate lookup, not from the submission. Hence deviation 9.

## Commands and results

- `ninja -C build/linux-everything -j10 ladder_lims_tests ladder_lims_server ladder_lims_screen_tests ladder_lims_render_smoke`
  (clang, `-Weverything -Werror`): clean.
- `ladder_lims_tests "[approval],[lease],[posting]"`: `All tests passed (264 assertions in 41 test cases)`.
- `ctest -L ladder-lims -j2` (first full run): 402/403 passed; the one failure was my edit of the `[approval]` test
  (deviation 8), fixed and rerun (above). Final full run recorded at the end of this file.
- clang-format on every changed file (limit 119; only two pre-existing NOLINT comment lines are longer).
- clang-tidy (local 23.1.4, not CI's 22; findings in `quantity.hpp`, `lab/core/types.hpp` and `#pragma once in main file`
  are pre-existing or header-as-main-file noise): fixed trailing commas, `std::move` of a trivially copyable query,
  `operator[]` and optional access in the new TUs and tests. The new TUs were clean on the last run for the files
  listed in the commit; `test_invoice_posting.cpp` last run was before the final `value_or` edit (not rerun).

## Concerns

- GCC and final ctest results: see the end of this file.
- The ledger's `FindPosting` is a new read action outside the brief's interface list; decision 38 records why.
- `UPDATE ledger_posting_sources ... WHERE id = 0` as a "take the write lock" idiom is SQLite-specific in effect (valid
  SQL elsewhere, where it is a harmless no-op).

## Final results (MEASURED, after the last edit)

- `ninja -C build/gcc-debug -j10 ladder_lims_tests` (GCC 16.2.1, `-Wall` strict set): exit 0, no `warning:` lines.
- `ctest -L ladder-lims -j2` in build/linux-everything with `QT_QPA_PLATFORM=offscreen`:
  `100% tests passed out of 403`, total 315.91 s.
- Not run: the Doxygen target (`WARN_AS_ERROR`), clang-tidy rerun on `test_invoice_posting.cpp` after the final
  `value_or` edit, CI's clang-tidy 22 (local is 23.1.4).

## Fix round 1

Review: b-task-14-review1.md. Builds: build/linux-everything (clang) and build/gcc-debug (`ninja ladder_lims_tests`, 0 `warning:` lines in the log).

- **I1 (retry stranded by changed settings or rates).** `PostInvoice` (dispatched) now asks `find` first, before it reads the company or
  settings, and when an entry exists calls `numberAndCommit` with it: no posting is rebuilt, no rate is looked up. What is relied on
  rather than checked: the source key is billing's own and `find` looks it up in the invoice's company (so the entry belongs to this
  invoice and company); the entry was built from the invoice's totals, which cannot change after approval (the conditional updates
  guard the state), and the settings do not enter the invoice's stored totals. The posting step is still conditional on state
  Approved. Decision 38 and the `PostInvoice` doc now state exactly when the retry converges.
  New case "a retry converges on the recorded entry after the billing settings or the rates changed" (receivable account moved to a
  new 1210; back-dated USD rate between the used rate and the entry date). Fail-first, with `if (false && earlier.entryId.hasValue())`:
  `record: source key billing.invoice:1 is already recorded for a different posting; a corrected document needs a new key`, at
  test_invoice_posting.cpp:401 in both variants; after restore (+touch) both pass.
- **M1.** The dispatched refusals are matched by message (the type does not cross the socket dispatch): "unauthorized" with no role
  (edge) and "does not hold the service role" with the role at another company only (new `PostingRole::ElsewhereOnly`), which
  separates the edge from the ledger's own rule. First run with a placeholder showed the real messages: `"unauthorized"` and
  `"RecordPosting: 'system:posting' does not hold the service role at company 1"`.
- **M2.** Busy is worded as contention: "the books were busy; invoice N has its entry recorded and is not yet numbered, post it again"
  (second commit) / "was not posted, try again" (one transaction). Still `Conflict`.
- **M3.** `GetRateOn` throws `Forbidden` for a company the caller cannot see (a missing grant is not "no rate"); `FindPosting` keeps its
  empty answer, documented: recording the posting then refuses the same caller with the reason, so it cannot mislead. Test "a rate asked
  by a caller that cannot see the company is refused". Fail-first with `return RateAnswer{};`: got
  `"SubmitInvoice: invoice 1 is in USD and no rate values it in EUR" contains "GetRateOn: the caller may not see company"`.
- **M4.** "today" removed from the doc; the message says "rate on the posting date".
- **M5.** The registry's own `morph::model::ValidationError` crosses the in-process dispatch; the check is now by type.
- **M6.** No change, as the review concludes (the existing comment gives the reason).

Tests: `ladder_lims_tests "[approval],[posting],[lease],[ledger]"`: All tests passed (841 assertions in 94 test cases).
Full ladder-lims in halves: see the final line below.
Pre-existing, not from this change: `Hypothesis 7: a company-B invoice approval sends a company-A watcher the invoice's id` fails at
test_two_company_effects.cpp:206 (`ozzy ... ApproveInvoice` kind "err") also with my changes stashed (same binary rebuilt from HEAD).
Full ladder-lims: -I 1,200: 199/200 (only the pre-existing Hypothesis 7 case failed); -I 201,: 100% of 207 passed.

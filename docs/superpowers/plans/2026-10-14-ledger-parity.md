# Ledger parity: transactions, budgets and rules on the screen

Part of #899 (spec 4 §5, "ledger"). The ledger screen gains what its presenters still held alone, so #900 can
retire them: the screen becomes tabs — the ledger (accounts, a transaction between them, a month's entries with
undo), budgets, rules, and the statement job.

## What the old tests pin, and where it goes

| Old test | Claim | Screen test |
|---|---|---|
| `test_ledger_presenter.cpp`, `test_ledger_qml_bridge.cpp` | accounts listed; balances exact, never a float; a refusal surfaces | "a transaction moves an exact amount between accounts, and undo reverses it"; "an unbalanced transaction is refused, shown on its form, and stores nothing" |
| `test_budget_presenter.cpp`, `test_budget_qml_bridge.cpp` | a budget is created; a month's limit and spend are exact; a refusal surfaces | "a budget's month shows its limit and the linked account's spend exactly"; "a limit for a month that does not exist is refused on its form" |
| `test_rule_presenter.cpp` | create, then update answers the bumped version | "a rule's edit answers it with its bumped version" |
| `test_report_presenter.cpp`, `test_report_job_poller.cpp` | the statement job polled to done or failed | the two statement tests, unchanged but for selecting the tab |

The rule bridge's refusal (updating a rule that was never created) has no screen equivalent: the edit form only
ever names the rule just created.

## Decisions

- **Money as ledger stores it.** A leg's amount and a balance are whole minor units tagged with places. The screen
  shows them through `money` over the amount divided by the currency's minor unit (1 for JPY and KRW, else 100).
- **A bare `Rational` is entered as its parts.** The forms engine renders one as `num`/`den`/`dp` integer inputs
  (the forms spec calls it unrepresentable); #961 asks whether that should change.
- **A hand-entered transaction carries no op id.** The `opId` field is omitted, so the transaction skips the
  exactly-once ledger, which exists for replayed imports.

## Found on the way

- #962: a document's `tabs` never showed any page but the first. Fixed here, in its own commit.
- #961: a bare `math::Rational` member cannot be entered as a number.

## Verification

`ladder_ledger_screen_tests`: 8 test cases, three runs green. A mutant that drops the accounts refetch after a
stored transaction fails two assertions. The full suite of the bank build passes (4256 tests), ledger's render
smoke included.

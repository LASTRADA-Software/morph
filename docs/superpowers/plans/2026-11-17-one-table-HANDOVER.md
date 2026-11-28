# The One Table — plan status and handover

The implementation plan for `docs/spec/table/table.md` and `docs/spec/table/editing.md` is split into an index and seven detailed parts. The index (`2026-11-17-one-table.md`) holds the parts table and the **Cross-part decisions** (1–29), which are binding on every part. No code has been written; only plans exist.

## Status

The plans went through review rounds with Fable: two rounds on every part, three on C. Every finding is applied, and the review is closed. Applying them added Cross-part decisions 30–53 to the index and extended #19, #20 and #28.

| Part | File | State |
|---|---|---|
| A — Tasks 0–2 | `…-a-foundation.md` | Round-2 fixes applied. |
| B — Tasks 3–7 | `…-b-slow-columns.md` | Round-2 fixes applied; aligned with #30 (`Session::read` yields text). |
| C — Task 8 | `…-c-engine-config.md` | Round-3 fixes applied. |
| D — Tasks 9–11 | `…-d-screen.md` | Round-2 fixes applied; aligned with #30, #32–#35. |
| E — Tasks 12–14, 16 | `…-e-configure.md` | Round-2 fixes applied. |
| F — Tasks 15, 17 | `…-f-display-and-acting.md` | Round-2 fixes applied; `tableMessage` points to Task 9d. |
| G — Tasks 19–24 | `…-g-editing.md` | Round-2 fixes applied; marks drawn per cell (#28). |

Nothing in the plans has been compiled. Where a part rests on reading code rather than building it, it says so. For example, G's claim that glaze cannot reflect a base class's members comes from reading `to_tuple.hpp`, and Task 24's first step checks it with a `static_assert`.

## What to do next

The plan review is finished: no further review rounds are run on the plans.

1. **Execute** with `superpowers:subagent-driven-development`, in index order. Fan out with Agent subagents, not the Workflow tool.
   - Implementers follow the user's model rules: Haiku for fully specified tasks, Opus for judgment, Opus for every review.
   - Net tests (`morph_net_tests`) run only on `linux-everything`.
2. **When a task finds the plan wrong** (a signature that does not compile, a test that cannot hold), fix the plan in that task's commit. When the fix changes a shared name or an owner, add a Cross-part decision to the index and correct the affected parts.

## Spec questions — settled

Each was settled in favour of the plans' position; table.md and editing.md now state it, and the plans no longer edit the spec for it.

1. The server options are the code's `ServerConfig`; the spec now says `ServerConfig::tableWork`.
2. Copy goes through `IViewBackend::copyText`; the spec now says so.
3. `formatCell` takes the session's `ui::FormatContext`; the spec now says so.
4. `resolve` takes `Model&`, because table functions are non-const (index #23); the spec now declares it so.
5. A typed row's schema marks a measure `x-measure` and lists no units; units come from the description alone. The spec now says so (Part C 8c Step 13's text).
6. A table id is unique in the process, because its action id `Table.<id>` is; a duplicate is refused when the registries resolve. The spec now says so (with 18).
7. `TableQuery::columns` lands in step 1; the spec's Delivery order now says so.
8. A table-state key's `model` is 0 to 256 bytes (empty for a `rows` table); the spec's "Bounds" now says so.
9. `ITableStateStore` has `publish` and the sentinel `kNoDocument`, and `MemoryTableStateStore` takes `Sharing`, defaulting to `Private`; the spec now declares them.
10. Range selection in server mode is step 5; the spec's Delivery order now lists it there and not at step 7.
11. Smaller gaps, all decided and now in the spec: a table's own cell and row edits make no row busy; a row-mode refusal moves focus back to the first refused cell while the table has focus; a deletion happens at once and the status line offers "Undo", with no question first; a page's total is `AtLeast` while undecided rows remain and absent above the count cap; the typo at table.md "apunloaded" is fixed.
12. `Session::read` resolves with the reply's JSON text, because a `Value` cannot hold a JSON number with a fraction (index #30); the spec now says so, with the reason.
13. A typed list action's description comes from the node's `columns` or the action's view document, since a client only sees the request schema; the spec now says so.
14. A `Timestamp` in a `rows` table is a `DateTime` cell of whole seconds, rounded down; the spec now says so.
15. Two model instances can overshoot the stored-state quota by one document, since a preferences store has no conditional write; the spec now says so.
16. After a `Conflict` whose reload finds nothing stored, the one retry saves with an empty `expected`; the spec now says so.
17. A `Together` group takes 0, or 2 to k, include entries; with 0 it is an exclude-only group. The spec now says so (Part C 8b Step 13's sentence).
18. A table id is unique in the process (question 6 with its evidence, `effects.hpp`); the spec now says so.
19. A page that reaches the client through the query cache keeps a fractional number as a decimal (index #46); the spec now says so.
20. A typed table with a `MeasureValue` member gets its registry from the model's `tableUnits(TableContext const&)` hook (index #49); the spec now names it.
21. The date tree is include-only (ticked buckets, touching ones merged; over 32 entries nothing applies and the menu says "Too many dates to list; filter with a condition"); the column-group chooser has "Remove group"; with equal ticked and unticked sides the unticked side is sent. The spec now says all three.
22. A `rows` column is `Number` only through a literal `kind`; a new description revision mounts the table again (window, sort, filters and selection survive; realized cells and scroll offset do not); `morph.tableState` and scopes starting `morph.table` are reserved, so a person cannot reset their own quota. The spec now says all three.

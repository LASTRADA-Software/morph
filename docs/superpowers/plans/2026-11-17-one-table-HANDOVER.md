# The One Table — plan status and handover

The implementation plan for `docs/spec/table/table.md` and `docs/spec/table/editing.md` is split into an index and seven detailed parts. The index (`2026-11-17-one-table.md`) holds the parts table and the **Cross-part decisions** (1–29), which are binding on every part. No code has been written; only plans exist.

## Status

The plans go through review rounds with Fable until a review reports no Critical or Important issue. Round 1 has run on every part. The work stopped mid-round, so some files hold fixes that are only partly applied.

| Part | File | State |
|---|---|---|
| A — Tasks 0–2 | `…-a-foundation.md` | Round-1 fixes applied (9 findings, plus `detail::serve`). Round-2 review not yet run. |
| B — Tasks 3–7 | `…-b-slow-columns.md` | Round-1 fixes applied (12 findings). Round-2 review not yet run. |
| C — Task 8 | `…-c-engine-config.md` | Round-2 review done. Its 8 findings (below) may be partly applied. |
| D — Tasks 9–11 | `…-d-screen.md` | Round-1 findings (below) may be partly applied. |
| E — Tasks 12–14, 16 | `…-e-configure.md` | Round-1 findings (below) may be partly applied. |
| F — Tasks 15, 17 | `…-f-display-and-acting.md` | Round-1 fixes applied (11 findings). Round-2 review not yet run. |
| G — Tasks 19–24 | `…-g-editing.md` | Round-1 findings (below) may be partly applied. |

## What to do next

1. **Finish the open fixes for C, D, E and G.** For each part, check every finding below against the file and apply it where it is missing. Each finding gives the location in the plan, as a line number from when the review ran, and the fix.
2. **Run a fresh Fable review of all seven parts.** Each review checks:
   - spec fidelity;
   - executability against the real code;
   - consistency with sibling parts and with the index's decisions;
   - plan quality per `superpowers:writing-plans`.

   Reviewers report only Critical or Important issues, or "NO ISSUES".
3. **Repeat fix and re-review** until every part comes back with "NO ISSUES". When a finding changes a shared name or an owner, add a Cross-part decision to the index and tell the affected parts.
4. **Settle the spec questions below with the spec author.** The plans already take a stated position on each one.
5. **Then execute** with `superpowers:subagent-driven-development`, in index order.
   - Implementers follow the user's model rules: Haiku for fully specified tasks, Opus for judgment, Opus for every review.
   - Net tests (`morph_net_tests`) run only on `linux-everything`.

## Open findings

### C — round 2

1. `compileGroup` refuses more than 32 include or 32 exclude entries. `Engine::setFilter` compiles with no limits (`engine.hpp:717`, `:996`). Add a test with 33 entries.
2. A `Together` group takes "0, or 2 to k" include entries, not "2 to k". Add a check for an empty include.
3. Edit `table.md` 2534–2536 (and 2505) for `x-units` in the same commit as 8c.
4. `readGenericPage` with `units == nullptr` resolves units through `registryOf(description)`. Add a test that reads a page with no registry passed.
5. Fix `==` comparisons that do not compile:
   - compare `*a == *b` rather than two `expected`s;
   - declare `operator== = default` on `TypedMeasure`, `Measure` and `UnitInfo`.
6. Drop the "Unknown ⇒ passes some and fails some" clause from the invariant-1 test and name its generator.
7. Order 8a → 8b → 8d (index decision #17). 8b moves `ColumnGroupInfo` into `data_source.hpp`; no include cycle remains.
8. The `Filtered::by` test helper passes `source->groups()`.

### D — round 1

- **C1 — the cell seam (index #28).**
  - Replace `rowView` with `cellView(column, rowSignal)` plus a row-level view.
  - `TableSource` names the columns it always fetches.
  - `TableColumn::id` lands in 9d.
  - Pooled cells own a row signal and are rebound by setting it.
  - Define `cellBinds`.
- I1: build the tests' `FormatContext` from the locale table, so the grouping separator is set.
- I2: name how a reply `Value` is decoded into a `GenericPage` and a description.
- I3: segment cleanup uses an intent entry `<part>.pending`, because the preferences store cannot list its keys.
- I4: specify the quota entry's key and arithmetic, and test it.
- I5: persisted state for `rows` tables, with an empty `model` allowed as 0–256 bytes (spec correction).
- I6: `tableMessage` lands in 9d (index #11).
- I7: cell load state reaches both backends (index #29), plus the row-height rule.
- I8: steps for the `model`+`action` and `highlightKey` keys.
- I9: `onMissingRowField` adds only described ids, and is posted rather than run inside a binding.
- I10: the K8 window holds 40 or more columns.
- I11: kind inference for every `ValueKind`, with a timestamp test at milliseconds against seconds.
- I12: declare the group-run index in 11a.
- I13: a save during a held load keeps the stored parts.
- I14:
  - `RecordOf` returns an owning row record;
  - add the "unknown unit" fit case;
  - define the shape of `carried`.

### E — round 1

- C: a user-formed group has no values list; its menu offers conditions and the match line only (index #26).
- I2: a column's values and conditions are exclusive. State how a reopened menu classifies them.
- I3: one rule for when a condition applies: live after `tableFilterDebounce`, and Apply commits.
- I4: a tick model for "(Select all)" under a search.
- I5: a date tree of years, months and days with server-side buckets, one `between` per ticked bucket, and a date picker.
- I6: canonicalise typed operands. Each dimension gets a heading in a measure's values list.
- I7: `kNoDocument` and `publish` go into the spec, a store that ignores the sentinel is tested, and `Sharing` gets a default.
- I8: tests assert the revision `publish` returned, not "1" or "2".
- I9: `TableViews` loads through D's `TableStates` (T5).
- I10: `clipboardText()` (index #27).
- I11: the Review Focus 5 test reaches the reload after `Conflict`.
- I12: the collection test clicks header index 1, and the `rowKeys` helper has an owning task.
- I13: the accessible text is exactly "sorted ascending, key 2 of 3".
- `tableMessage` now comes from D's 9d; E consumes it (index #11).

### G — round 1

- C1: `GridSource` is index-based (`rowCount`, `keyAt(i) -> optional`), and the walk stops at an unfetched row.
- C2:
  - `setOnCellClick(viewRow, column, doubleClick)` and `setOnFocusOut`, with recording-backend drivers;
  - `GridController::activate`;
  - define how the editor Node sits in a cell, and which key chords apply while editing;
  - a TUI case.
- I1: `EngineRows` over a `shared_ptr` (index #21), with the grid's own `RecordOf`.
- I2: "Undo" and "Recalculate" are `StatusEntry`s (index #19).
- I3: the grid's own `CellEdits` runs are not registered in `RowStates`. Test both cases.
- I4: `modified()` is false only for `x-computed` and `x-derived` members.
- I5: the schema-root `x-cellEditOf` / `x-rowEditOf` write and read path. Check that glaze reflects a base class.
- I6: interpreter tests on Windows for the Task 24 lowering, and the `editMode` refusal.
- I7: menu entries: Revert cell and Revert row, Insert row, Delete rows, Add row.
- I8: a view and backend contract for cell selection.
- I9:
  - break the `form_view.hpp` ↔ `grid.hpp` include cycle;
  - `formGrid` takes a `FormatContext`;
  - `FixedGridSource` takes a `FormSession*`.
- I10: when a row is absent from a refetch, clear its draft.
- I11: name a mutation for each test at ~313, 629, 878, 883, 888, 940, 1114.
- I12: the "row is gone" test runs capture → remove → load.
- I13: one `committed()` per row operation.

## Spec questions for the author

The plans deviate from or settle the spec on each of these. Confirm each one, then update the spec to match.

1. The spec says `RemoteServerOptions`; the code's type is `ServerConfig`.
2. `Backend::copyText` is really `IViewBackend::copyText`.
3. `formatCell` takes `ui::FormatContext`; the spec's `Locale` type does not exist.
4. `resolve` takes `Model&` rather than `Model const&`, because table functions are non-const (index #23).
5. `x-units` in the row schema contradicts "units come from the description alone" (2505 against 2534).
6. Two models cannot both offer a table with the same id, because action ids are `Table.<id>`.
7. Step 1 against step 2b: who adds `TableQuery::columns`. The plans put it in step 1.
8. An empty `model` in a table-state key (2216 says 1 to 256 bytes).
9. `ITableStateStore` needs `publish` and an "expect nothing stored" sentinel.
10. Range selection is listed at both step 5 and step 7. The plans put it at step 5.
11. Smaller gaps are noted in each part's Decisions section:
    - the busy-row exemption for a row's own queued edits (editing.md);
    - the timing of a row-mode refusal (editing.md);
    - whether a deletion asks before it deletes, or deletes and offers Undo (editing.md);
    - the `AtLeast` total;
    - the typo "apunloaded" at table.md:800.

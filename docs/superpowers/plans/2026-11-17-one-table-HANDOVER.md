# The One Table — plan status and handover

The implementation plan for `docs/spec/table/table.md` and `docs/spec/table/editing.md` is split into an index and seven detailed parts. The index (`2026-11-17-one-table.md`) holds the parts table and the **Cross-part decisions** (1–29), which are binding on every part. No code has been written; only plans exist.

## Status

The plans go through review rounds with Fable until a review reports no Critical or Important issue. Every finding from round 1 (A, B, D, E, F, G) and round 2 (C) is applied. Applying them added Cross-part decisions 30–36 to the index and extended #19 and #28.

| Part | File | State |
|---|---|---|
| A — Tasks 0–2 | `…-a-foundation.md` | Round-1 fixes applied. Next review: round 2. |
| B — Tasks 3–7 | `…-b-slow-columns.md` | Round-1 fixes applied; aligned with #30 (`Session::read` yields text). Next review: round 2. |
| C — Task 8 | `…-c-engine-config.md` | Round-2 fixes applied. Next review: round 3. |
| D — Tasks 9–11 | `…-d-screen.md` | Round-1 fixes applied; aligned with #30, #32–#35. Next review: round 2. |
| E — Tasks 12–14, 16 | `…-e-configure.md` | Round-1 fixes applied. Next review: round 2. |
| F — Tasks 15, 17 | `…-f-display-and-acting.md` | Round-1 fixes applied; `tableMessage` points to Task 9d. Next review: round 2. |
| G — Tasks 19–24 | `…-g-editing.md` | Round-1 fixes applied; marks drawn per cell (#28). Next review: round 2. |

Nothing in the plans has been compiled. Where a part rests on reading code rather than building it, it says so. For example, G's claim that glaze cannot reflect a base class's members comes from reading `to_tuple.hpp`, and Task 24's first step checks it with a `static_assert`.

## What to do next

1. **Run a fresh Fable review of all seven parts.** Each review checks:
   - spec fidelity;
   - executability against the real code;
   - consistency with sibling parts and with the index's decisions (1–36);
   - plan quality per `superpowers:writing-plans`.

   Reviewers report only Critical or Important issues, or "NO ISSUES".
2. **Repeat fix and re-review** until every part comes back with "NO ISSUES". When a finding changes a shared name or an owner, add a Cross-part decision to the index and tell the affected parts.
3. **Settle the spec questions below with the spec author.** The plans already take a stated position on each one.
4. **Then execute** with `superpowers:subagent-driven-development`, in index order.
   - Implementers follow the user's model rules: Haiku for fully specified tasks, Opus for judgment, Opus for every review.
   - Net tests (`morph_net_tests`) run only on `linux-everything`.

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
12. `Session::read` resolves with the reply's JSON text, not a `Value`, because a `Value` cannot hold a JSON number with a fraction (index #30).
13. A typed list action's description cannot come from its reply schema: a client only ever sees the request's schema (`Catalog::schema`). Task 11c takes the node's `columns`, or the action's view document, instead.
14. A `Timestamp` in a `rows` table becomes a `DateTime` cell of whole seconds, rounded down (Task 11a).
15. A preferences store has no conditional write, so two model instances can overshoot the stored-state quota by one document (Task 9c).
16. After a `Conflict`, if the reload finds nothing stored, the one retry saves with an empty `expected` (Task 14b). Question 9 is settled in the plan.

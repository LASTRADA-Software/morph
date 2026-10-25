# LIMS gallery: where the work stands and how to continue

This branch (`feat/lims-gallery`, draft PR #1045) holds the whole initiative. Everything lands here as one PR; do not
open separate PRs.

## State

| Part | State |
|---|---|
| Ten gallery designs (01–10) | done |
| Batch A: defects and responsive layout | landed; the gallery dropped its workarounds |
| Batch B: tables | designed and reviewed (`docs/superpowers/specs/2026-10-25-lims-gallery-batch-b-design.md`); part 1 landed, the gallery dropped its workarounds; part 2 next |
| Batch C: text, fields and small controls | designed and reviewed (`docs/superpowers/specs/2026-10-25-lims-gallery-batch-c-design.md`); lands after batch B |
| Batch D: layout, overlays and navigation | designed, not reviewed yet (`docs/superpowers/specs/2026-10-25-lims-gallery-batch-d-design.md`); four parts |
| Batch E: expressions and charts | not designed yet |

**Batch B part 1 ("columns and the selection") has landed:** G2 (the `mono` role and `font.mono`), G47 with a
column's `align` and `valign`, G29 (a bound selection and `onSelectionChange`), G43 (one selection model with
`SelectionChange` reports and a keyboard on every backend) and G30 (the select column), with conformance cases 41 to
43 and layout cases 8 and 9. The gallery's samples, results, review, audit and console screens use them. The plate's
well labels stay proportional: they are button labels, which take no text role.

## How to continue

1. **Batch B part 2** ("the row pipeline"): G41, G31, G28, G44, G46, as the design orders them, with G30's paged
   Ctrl+A test. A table over `rows` ignores the kind of a selection report today, since it holds every row; G28's
   paging is where an `Extend` starts keeping the keys of other pages.
2. **Batch C**, in the order its design gives (plain text first: #1058).
3. **Review batch D's design** once, by a separate agent on the default model, then implement it in its four parts.
   **Design batch E** (`batches.md` lists its gaps) and review it the same way. Batch E's chart should need no new Qt module and needs a terminal fallback.
4. When everything has landed, take the PR out of draft.

Two corrections to `gaps.md` from the batch C design, to apply when batch C lands: G10 says a field is outlined while
it has errors, but no Qt Quick input is outlined for errors at all; G13's date formatting hook is never set by any
client (related to #1056).

Issues filed along the way and fixed in these batches: #1058 (Qt Quick draws data text as markup), #1059 (Qt Quick
tables unreachable from the keyboard), #1060 (a multiline text input with an icon loads, then mount throws), #1061 (an
unknown select style loads silently), #1062 (dialog title and field unit not exposed to screen readers). Batch D
depends on #1063 (the client never mounts the app document) and touches #1064 (navigate with `onResult` to an open
screen).

## Working rules

- Read `AGENTS.md` first. Specs change in the same commit as the code; comments and specs state current behaviour
  and why, with no history and no issue numbers.
- Every new test has a control: break the feature, see the test fail, restore it (and `touch` the restored file so
  ninja rebuilds).
- Before pushing, build everything warning-free and run `morph_tests` (one "failed as expected" case is normal),
  `morph_qt_quick_tests`, `morph_tui_tests`, `morph_client_tests`, `lims_gallery_tests` and
  `lims_gallery_render_smoke`, the Qt ones with `QT_QPA_PLATFORM=offscreen`.
- CI runs clang-tidy 22 on changed lines with warnings as errors: identifiers of 3 or more characters, `T const*`,
  `auto` for casts, `contains()`, ranges algorithms, `const` locals, cognitive complexity under 25, `emplace_back`,
  non-trivial parameters by const reference.
- Commit messages: an imperative subject, a body saying what and why, `Closes #N` where an item has an issue.

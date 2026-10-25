# LIMS gallery: where the work stands and how to continue

This branch (`feat/lims-gallery`, draft PR #1045) holds the whole initiative. Everything lands here as one PR; do not
open separate PRs.

## State

| Part | State |
|---|---|
| Ten gallery designs (01–10) | done |
| Batch A: defects and responsive layout | landed; the gallery dropped its workarounds |
| Batch B: tables | designed and reviewed (`docs/superpowers/specs/2026-10-25-lims-gallery-batch-b-design.md`); part 1 in progress, see below |
| Batch C: text, fields and small controls | designed and reviewed (`docs/superpowers/specs/2026-10-25-lims-gallery-batch-c-design.md`); lands after batch B |
| Batch D: layout, overlays and navigation | not designed yet |
| Batch E: expressions and charts | not designed yet |

**Batch B part 1 is half done.** The top commit is "WIP: batch B part 1, G2 monospace role", which holds an
unfinished G2: the loader, interpreter, view tree, recording backend, theme reader, specs and tests have their
changes. The Qt Quick theme (`src/qt_quick/theme.cpp`, `context.cpp`) was still being written: `setFamilies` has to
handle both `font.family` and `font.mono`, and the Qt context has to publish `platformMonoFamily`. It has not been
built. Finish G2, build, test, and reword the commit into a proper one (amend it; the branch is not merged).

## How to continue

1. **Batch B part 1** ("columns and the selection"), one commit per item in the design's order: G2, G47 with vertical
   alignment, G29 (`Closes #1053`), G43 (`Closes #1059`; its conformance case 43 must run on Qt Quick), G30. Then one
   commit where the gallery screens drop the workarounds these items replace, and `gaps.md` marks them
   "— fixed in batch B".
2. **Batch B part 2** ("the row pipeline"): G41, G31, G28, G44, G46, as the design orders them.
3. **Batch C**, in the order its design gives (plain text first: #1058).
4. **Design batches D and E** (`batches.md` lists their gaps), each reviewed once by a separate agent on the default
   model before implementation. Batch E's chart should need no new Qt module and needs a terminal fallback.
5. When everything has landed, take the PR out of draft.

Two corrections to `gaps.md` from the batch C design, to apply when batch C lands: G10 says a field is outlined while
it has errors, but no Qt Quick input is outlined for errors at all; G13's date formatting hook is never set by any
client (related to #1056).

Issues filed along the way and fixed in these batches: #1058 (Qt Quick draws data text as markup), #1059 (Qt Quick
tables unreachable from the keyboard), #1060 (a multiline text input with an icon loads, then mount throws), #1061 (an
unknown select style loads silently), #1062 (dialog title and field unit not exposed to screen readers).

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

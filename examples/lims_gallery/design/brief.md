# LIMS gallery: brief for every implementation lane

## Status, and how to resume

| Design | State |
|---|---|
| 01 E-signature, 02 Sample receipt, 03 Instrument record | done |
| 04 Audit trail, 05 Sample list | done |
| 06 Stability study setup, 07 Plate layout | done |
| 08 Result entry | done |
| 09 Review and approval, 10 Lab operations console | not started (lane 4 stopped on request; G49–G51 logged from reading 09) |

**Next steps:**
1. Build 09 and 10, one lane at a time.
2. Group `gaps.md` into a few large framework batches. Each batch gets one design, one review, then implementation on this same branch.
3. The gallery screens drop their workarounds.

The library defects in `gaps.md` also get filed as issues (AGENTS.md): G1, G7, G12, G19, G21, G22, G25, G29, G34
and G42 (G42 inferred from reading; reproduce it before filing).

The mockups in `mockups/` open as Design Component pages; the canvas they came from is a private artifact.

## Goal

Implement ten LIMS UI designs as **ui/1 declarative UI screens** in a new, self-contained example,
`examples/lims_gallery`. The point is to find out what morph's declarative UI can and cannot express. A design
element that ui/1 cannot express is **not a failure**: it is the finding. Record it in the gap log and use the
closest honest workaround.

**There is no backend.** No models, no server, no database. Sample data lives in screen `state` (literal records
and lists), and actions only change that state (`set` and similar commands). Do not add a `morph::Model`.

## Where things are

- **Worktree:** `/home/yaraslau/repo/morph-lims-gallery`, branch `feat/lims-gallery`. Work only here.
- **Designs:** HTML mockups in `examples/lims_gallery/design/mockups/`.
  Files: `Main.dc.html` (01), then `02-Receipt`, `03-Instrument`, `04-Audit`, `05-Samples`, `06-Stability`,
  `07-Plate`, `08-Results`, `09-Review`, `10-Console`, all `.dc.html`. Read only these; do not edit them.
  - Each design shows an **Expanded** frame (window ≥ 1200 px) and a **Compact** frame (< 600 px).
  - Implement both, switching on `{"env": "widthClass"}` (`compact` < 600, `medium` < 1200, `expanded`). `medium`
    takes the expanded layout unless the design needs otherwise.
  - Bound sizes with `minWidth`/`maxWidth`, and use `align`/`justify`.
- **The ui/1 vocabulary** is authoritative in `docs/spec/ui/document.md` (node table), `expressions.md`,
  `theme.md`, `view_tree.md`, `interpreter.md` and `author.md`. Read them before writing screens.
- **The C++ author API** (`include/morph/ui/author.hpp`) builds documents. Copy the existing gallery's patterns:
  `examples/gallery/` (screens library, tests, render smoke, local app) and `examples/lims/screens/lims_screens.cpp`.

## The colour scheme

- **Green is the main colour.** Ship a `ui-theme/1` theme with the bundle; see `docs/spec/ui/theme.md` for the file
  format and how a bundle serves a theme. Use accent `#17784A`, and the designs' neutrals: text `#1C1E21`, muted
  `#4D5461`, border `#DDE1E6`, surface `#FFFFFF`, page `#F6F7F9`.
- **Status tones keep their meaning:** Err for out of spec, Warn for out of trend or due, Ok for within spec, Info
  and Neutral for workflow state. A "within spec" tag and a primary button must stay distinguishable by their text,
  not by colour alone.

## Gap log (the main output)

Append every gap to `examples/lims_gallery/design/gaps.md`.
One entry per distinct need, in this exact shape:

```
### G<n>: <short name>
- **Design(s):** 08, 10
- **Needs:** what the design shows that ui/1 must express
- **ui/1 today:** what exists, with file:line evidence (spec or code). Say if it is verified by trying it, or inferred
  from reading.
- **Workaround used:** what the screen does instead, or "none, omitted"
- **Weight:** High (core LIMS need) / Medium / Low
```

Before adding an entry, read the log: if the gap is already there, add your design number to it instead of
duplicating it. Also log **defects**: if ui/1 or a frontend behaves differently from its spec, or crashes or
misrenders, log it with a reproduction. Do not fix it.

## Hard rules

- **Do not change anything under `include/morph`, `src`, or `docs/spec`.** Framework changes come later, in batches,
  from the gap log. If a screen cannot be built at all without a framework change, log it and simplify the screen.
- **Bounds:**
  - Do not use the Agent or Workflow tools.
  - Do not run `/code-review`, `/simplify` or any skill that forks agents.
  - Do not use `run_in_background`, and no trailing `&` in shell commands.
- **One build at a time:** wrap every build, test and clang-format run in
  `flock <scratchpad>/morph-build.lock`,
  with at most `-j10`.
  - The build tree `build/linux-everything` is configured. Do not reconfigure it or change its compiler launcher.
  - Never `pkill -f`.
  - Qt Quick tests run with `QT_QPA_PLATFORM=offscreen`.
- **Scratch files:** keep them inside the worktree, or name them `lg-<lane>-*` in the scratchpad. Never `pr_body.md`.
- **Commits:**
  - one commit per design (the scaffold may share the first design's commit);
  - the subject is an imperative sentence; the body says what the screen shows and lists its gap ids;
  - commit as the configured git user;
  - end each message with:
    ```
    Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
    Claude-Session: https://claude.ai/code/session_01BmVLNhUvbWZjv85Hx2saxn
    ```
  - Do not push. Do not open a PR.
- **Comments follow AGENTS.md:** current behaviour and why, no history, no issue numbers. Examples are scaffolding,
  not production code: keep them clear, but don't gold-plate them.
- **clang-tidy (CI runs it on changed lines, warnings as errors):**
  - identifiers of 3 or more characters;
  - `T const*` where the pointee isn't modified;
  - `auto` for casts;
  - `contains()`;
  - `std::ranges` algorithms over hand loops;
  - `const` locals;
  - `.at()` rather than `operator[]`;
  - no C-style casts;
  - functions under cognitive complexity 25: split large screens into helper functions per section.
- **Before each commit:** run `clang-format -i` on the changed files. Build the lims_gallery targets and the examples
  client, and keep them warning-free. Run the lims_gallery tests.

## Per design, the minimum

1. **One screen** (`lims-gallery.<name>`) built with the author API, in its own source file under
   `examples/lims_gallery/src/screens/`.
2. **A recording-backend test** that mounts the screen with no diagnostics, at compact and at expanded width class.
   It checks the elements that define the design are present (find them by `testId`), and drives one interaction
   through it (for example, selecting a row shows the bulk bar).
3. **The render smoke** (Qt Quick, offscreen) mounts every screen at both widths and fails on any diagnostic or QML
   warning.
4. **Real fidelity:** use the design's real content (IDs, values, labels), not placeholders. Where ui/1 can't match
   the design, the gap log says so.

## Final report (your last message)

- the commits (hash and subject);
- the gap ids you added or extended;
- what you ran, and the results;
- anything that blocked you.

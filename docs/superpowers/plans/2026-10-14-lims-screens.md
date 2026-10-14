# lims and the forms demo on server-defined screens: implementation plan (part 11, #896)

**Goal.** lims and the forms demo run as UI documents through the generic client. Each meets spec 4 §7's rules:
- screens are toolkit-free by link set;
- every screen validates;
- screen tests run headless on the harness;
- one render smoke test per app passes offscreen.

The Qt GUI each one carried (presenters, QML bridges, hand-written QML) is retired, and the behaviours its tests
pin move onto screen tests file by file.

**Specs.** Spec 4 §1, §5 (lims, lims sample entry, forms demo), §7, §8 item 2; spec 6 §11 (the sample-entry
worked example), §7 (the workspace shell); spec 5 §7 (live recalculation). In-tree: `docs/spec/ui/workspace.md`,
`docs/spec/ui/interpreter.md`, `docs/spec/forms/engine.md`.

## What planning found

1. **The part is three slices, not one.** Spec 4 §5 asks for three things of different size:
   - lims' existing two surfaces as documents;
   - a new reference screen, `lims.sample`, whose model gains section, snapshot and working-copy actions;
   - the forms demo as an app shell with a wizard and a collection.

   Each lands with its own commits and its own tests, slice by slice:
   - **(a)** lims' lifecycle and results surfaces;
   - **(b)** `lims.sample`;
   - **(c)** the forms demo.
2. **lims' two surfaces share one attached sample.** The Qt shell points the results surface at the sample the
   lifecycle surface attached. As documents, each screen takes `sampleId` as a param. A workspace opens them as two
   tabs, and the lifecycle screen's "results" button navigates to `lims.results` with the same id.
3. **The tab strip is the workspace shell** (`ui::Workspace`, #944). The generic client's `ClientShell` mounts one
   start screen, so slice (a)'s render smoke opens each screen; using the workspace in the client is the hosting
   part's follow-up (d2).
4. **What the existing GUI tests pin.**
   - `test_lims_presenters.cpp` and `test_lims_qml_bridges.cpp` pin attach, refresh, the four transitions and
     their refusal messages, capture validation and the conflicts list. These become screen tests.
   - `test_lims_qml_surface.cpp` and `test_gui_qml_smoke.cpp` pin the QML files. They go with them; the render
     smoke replaces them.
   - The other ten test files drive the models and stay as they are.

## Slice (a): lims' lifecycle and results screens

- `examples/lims/screens/` (`lims_screens`, toolkit-free):
  - `lims.lifecycle`: `params: {sampleId: int?}`. It registers a client and a sample through `form` nodes, opens a
    sample, and shows its state and version. The four transitions are `serial` mutations, enabled by state. Each
    refusal is shown by `errorKind`.
  - `lims.results`: `params: {sampleId: int}`. It shows the analyses and results queries, a capture `form` per
    row with a preview through `evaluate` (spec 5 §7), and the calculated concentration shown stale while in
    flight. It verifies a result and lists conflicts.
  - `lims::bundle()` and `lims::localApp()`, registered with the generic client's local apps.
- Tests on `ScreenHarness` (Local and Simulated modes), mapped from the presenter and bridge tests:

  | Old test file | New test file | Re-expressed as |
  |---|---|---|
  | `test_lims_presenters.cpp` | `test_lims_screens.cpp` | one case per pinned behaviour |
  | `test_lims_qml_bridges.cpp` | `test_lims_screens.cpp` | one case per pinned behaviour |
  | `test_lims_qml_surface.cpp` | `test_lims_render_smoke.cpp` | the render smoke |
  | `test_gui_qml_smoke.cpp` | `test_lims_render_smoke.cpp` | the render smoke |

- Retire `examples/lims/gui/` and `examples/lims/gui_lib/`, and their CMake and CI references.

What building slice (a) found:

- **The generic client had no schemas.** Its catalog's `schema` lookup was empty, so no `form`, `collection` or
  `wizard` node could mount through it. A source now fetches every catalog model's `{actionType: schema}` document
  before it reports the bundle: over its exchange in remote mode, from the in-process registry in local mode.
- **A deferred call's arguments are read when it runs.** `morph_add_rung` defines the screen tests and the render
  smoke at the end of the top level, once `morph::examples_common` exists, and bakes the rung's name and directory in
  with `cmake_language(EVAL)`.
- **Left for a later slice:**
  - a lims server for remote mode: lims has none, so its screens run in local mode only;
  - live recalculation through `evaluate`: `CaptureConcentration` declares no computed fields yet;
  - the presenters' `busy()`: replaced by each mutation's concurrency, `serial` for the transitions.

## Slice (b): `lims.sample` (spec 4 §5, spec 6 §11)

As spec 4 §5 lists it:
- lifecycle with identity and a close guard;
- a private working copy;
- sections as forms;
- optional measurement cards;
- a server-computed dilution series;
- the `SeriesGrid` and `RecoveryCurve` host components with fallbacks;
- typed save errors;
- a "choose project" lookup table;
- surviving a backend switch.

`SampleModel` gains the section, snapshot and working-copy actions of spec 6 §11.

## Slice (c): the forms demo

`LabApp` as the app shell (forms, the intake wizard, `SamplesView` as a `collection`), `--typed` through
`forms::Form<A>` in local mode; the CLI unchanged.

## Verification

Per slice:
- GCC full;
- the example's own tests;
- the render smoke offscreen;
- clang-tidy on changed lines, with the file list split;
- the clang doc check;
- mutations against freshly linked binaries, one per re-expressed behaviour family.

# The business-app-shell vocabulary gaps — audit and design

Addresses #996, #1023, #1024, #1025, #1029 together, since #1029 itself observes that #1023-#1028 and
#996 "share one shape": the `ui/1` document vocabulary lacking a field for a presentation intent that
is ordinary for a business-app shell, forcing a hand-tuned backend-only patch with no cross-backend
equivalent. Before designing anything, this document audits each of #996's five claimed categories
against current `origin/master` (`16ed13de`), since #996 was filed 2026-10-18 and master has moved
since (notably PR #1004 "An app keeps its own Qt Quick shell and kit on ui/1 documents" and the #1022
polish batch) — treating a partially-fixed issue as fully open would waste a design pass re-solving
something already shipped.

## Part 1 — Audit of #996 against current master

| # | #996's claim (filed against `4a492fb4`) | Status on `16ed13de` | Evidence |
|---|---|---|---|
| 1 | No `minWidth`/`maxWidth` (and height) on a node; nothing caps or centres a column | **Done.** | `include/morph/ui/view.hpp`'s `LayoutHints` (~308-328) already has `minWidth`, `maxWidth`, `minHeight`, `maxHeight`, all `std::optional<int>`. `document.hpp`'s per-node key arrays carry `minWidth`/`maxWidth` (lines 669-670) with interpreter validation (`document.hpp` ~3101-3104, "The size bounds a node gives... clamped"). |
| 2 | No cross-axis `align` or main-axis `justify` on `row`/`column`/`grid`; no `alignSelf` | **Done.** | `document.hpp` has `"align"`/`"justify"` keys on row/column/grid node arrays (lines 756, 761-762) and `"alignSelf"` per-child (line 673), with dedicated validation (`checkChoice(node, "align", kAlign)` etc., ~3008-3010) and a documented placement rule (`kAlignSelfPlaces`, ~2856-2867). `LayoutHints::alignSelf` (`view.hpp` ~321-323) is the matching C++ field. |
| 3 | `form`/`collection`/`wizard` can't set `title`, `submitLabel`, `showStatus`, or a field layout (inline labels, N columns) | **Done.** | `document.hpp`'s `kFormKeys` (893-907) and `kCollectionKeys` (910-916) both already carry `title`, `submitLabel`, `showStatus`, `columns`, `fieldLayout`. |
| 4 | A `table` with no rows shows only a bare header, nothing tells the author how to show an empty state | **Done.** | `kTableKeys` (`document.hpp` 802-816) already has an `"empty"` key (`Shape::Nodes`), matching `Table`'s own `view.hpp` struct (verify the struct carries the matching field before assuming the document key alone is enough — see Part 2, item A, which depends on table internals this audit did not fully re-derive). |
| 5a | Navigator entries are generic bordered buttons, no list-item presentation, no "current" marking | **Done.** | `include/morph/ui/navigator.hpp`'s own doc comment (~50-51): "Each entry is a list item (`ButtonStyle::Item`) as wide as the navigator, and `setCurrent` marks the open screen's." `ButtonStyle::Item` is real (`button_style.hpp`), wired through `backend.hpp`'s `setStyle`, the interpreter (`interpreter.hpp:5927`), and enforced at mount (`mount.hpp:666-678`: a `selected` prop requires `ButtonStyle::Item`). |
| 5b | `ShellFrame`'s top bar: principal's name not level with the sign-out button | **Done** (or never actually true at this revision). | `shell_frame.hpp`'s top-bar construction (~62) has an explicit comment: "Centred across the bar, so the principal's text sits level with the sign-out button's label." |
| 5c | Signed-out frame still shows sidebar, principal, sign-out | **Done.** | `shell_frame.hpp`'s class doc comment (~40): "Signed out, the sidebar and the top bar are hidden and the page is all there is." |
| 5d | A tab bar shown even with one tab, no space from the page content | **Done.** | `include/morph/ui/workspace.hpp:423`: `_bar->setBarShown(captions.size() > 1);` — exactly the condition asked for. `TabsWidget::setBarShown` (`backend.hpp:408`) is implemented in the Qt Quick backend (`src/qt_quick/containers.cpp:111`). Missed on first pass by searching for `tabBar`/`hideWhenOne`-shaped names instead of the actual method name; corrected after Part 2's original item D turned out to already be fixed too. |
| 5e | A type scale the frame/navigator follows (app name > section > entry) | **Not re-verified.** | #1029's own evidence section, from a *later* (`qt-quick-frontend-polish`) worktree build, cites exactly this as issue "#996's recurring theme" without saying it's fixed — but no direct code check was done here. Flagged as unconfirmed, not claimed fixed or open; see Part 2 note. |

**Net result:** of #996's ~9 distinct asks, 8 are confirmed done on current master, 0 are confirmed
still open, 1 (type scale) is unconfirmed either way. This audit's single most important output is
this: **#996 itself is stale and should be closed, or nearly so, as a GitHub comment** (with this table
as the evidence) — it is not a live 5-category design problem. Part 2 below designs only for #1023/
#1024/#1025's two separately-filed, narrower, independently-confirmed-still-open gaps (table header
suppression, date-time presentation); item D below was dropped after the same direct-code check this
whole audit is built on found it already shipped too.

## Part 2 — Designs for what's genuinely still missing

### A. Table header suppression (#1023, #1025)

**Confirmed still open.** `Table` (`view.hpp` 875-890) has no `showHeader` field; `kTableKeys`
(`document.hpp` 802-816) has no matching key; `MorphTable.qml`'s `implicitHeight: header.implicitHeight
+ body.implicitHeight` (line ~136, current line numbers shifted slightly by the #1030 padding fix
earlier in this same session — re-check exact line before patching) unconditionally includes the header.

**Decision** (per #1023's own Scope section, option (a) vs (b)): **explicit `TableOptions`/`Table`
field**, not implicit-from-empty-labels. Implicit suppression (treat "every column label is empty" as
"no header") silently changes behavior for an author who has empty labels for a different reason (e.g.
icon-only columns with no text label but still wanting the header bar's height for visual rhythm), and
is a harder rule to document precisely than a flag. An explicit `showHeader: bool` (default `true`,
matching every existing document unchanged) is the additive, unsurprising shape every other opt-in in
this codebase follows (e.g. `form`'s `showStatus`, same shape).

**Concrete changes:**
1. `include/morph/ui/view.hpp`: `Table::showHeader = true` (bool, set once — a header's presence isn't
   reactive data, matching `Table::selectionMode`'s own "set once" precedent in the same struct).
2. `include/morph/ui/author.hpp`: `TableOptions::showHeader` (mirroring the `Table` field), wired into
   whichever `table()`/`dataTable()` builder emits the document node.
3. `include/morph/ui/document.hpp`: add `{"showHeader", Shape::Flag}` to `kTableKeys` (now 16 entries).
4. `include/morph/ui/interpreter.hpp`: the `"table"` lowering branch reads `onceBool("showHeader",
   true)` into `spec.showHeader` (precedent: `panel`'s `onceBool("collapsible", false)` read in this
   same session's #1019 fix).
5. `include/morph/qt_quick/backend.hpp`/`src/qt_quick/table.cpp`: `TableWidget::setShowHeader(bool)`
   (new virtual, default no-op body on the base so TUI/`RecordingBackend` compile unchanged, matching
   this session's own precedent for additive `TableWidget` hooks), `TableImpl::setShowHeader` calling
   through to a new `showHeader` QML property.
6. `MorphTable.qml`: `property bool showHeader: true`; `headerArea.visible: root.showHeader` (or
   collapse `implicitHeight`'s header term to 0 when false — the visible route is simpler and matches
   how `MorphPanel.qml`'s `headerRowShown` already conditionally hides a header row elsewhere in this
   same kit).
7. `docs/spec/table/engine.md` §12 ("The table node"): add `showHeader` to the property table.

**What this does NOT solve by itself:** #1023's actual repro (LicenceServer's seat-group tables reading
as *one* visual table) also needs the groups' tables to share column *widths*, not just suppress
repeated headers — two independent `ui::table` instances today compute `widestCell` independently
(`MorphTable.qml`'s `columnCache`, confirmed earlier this session while investigating #1016/#1030), so
column boundaries between group 1's rows and group 2's rows will not actually line up even once the
second header is hidden. This is a second, larger problem (cross-table column-width coordination) that
`showHeader` alone does not address; flag it as a follow-up, not silently claim `showHeader` is the
complete fix for the repro that motivated the issue.

### B. Date-time input presentation (#1024, #1025)

**Confirmed still open.** `DateTimeInput` (`view.hpp` 893-912) has `mode: DateMode` (format only — date
vs. date-time) and no presentation field; `MorphDateTimeInput.qml` is a plain `TextField`.

**Decision:** add `presentation: text | picker` (enum, set once, default `text` — unchanged behavior
for every existing document), rather than overloading `mode` (which #1025 itself correctly identifies
as already meaning something else — format, not widget). This is a vocabulary-level field per #1025's
own stated reasoning ("a schema-level fix... makes the intent portable across backends, which a
backend-only patch cannot").

**Concrete changes:**
1. `view.hpp`: new `enum class DateTimePresentation : std::uint8_t { Text, Picker };` plus
   `DateTimeInput::presentation = DateTimePresentation::Text`.
2. `author.hpp`: `DateTimeInputOptions::presentation` (mirroring `mode`'s own existing field shape in
   the same options struct).
3. `document.hpp`: `kDateTimeKeys` gains `{"presentation", Shape::Text}` (a closed-vocabulary string,
   `"text"`/`"picker"`, matching how `mode` itself is almost certainly already encoded — verify `mode`'s
   exact `Shape` before implementing, to keep the new key's encoding consistent with its sibling).
4. `interpreter.hpp`: read `presentation` the same way `mode`'s `DateMode` is read (find and mirror
   that exact lowering code before writing new code, not guessed independently).
5. **Qt Quick backend — the actual calendar widget.** This is the one part of the whole cluster that is
   real, non-trivial UI work, not plumbing: `MorphDateTimeInput.qml` needs an actual calendar popup
   (`Qt.labs.calendar`'s `DayOfWeekRow`/`MonthGrid`/`WeekNumberColumn`, or `QtQuick.Controls`'
   `DatePicker` native dialog depending on Qt version/style) wired to open from a button/icon beside the
   text field when `presentation === "picker"`, writing back through the same `onChange`/`value`
   contract the text-typed path already uses. Scope this as its own implementation task, separate from
   the vocabulary plumbing above — the vocabulary fields can land and be spec'd even before the Qt Quick
   calendar widget exists, with the TUI backend simply ignoring `presentation` (as every other backend
   -specific hint does today) until it has its own answer.
6. `docs/spec/ui/document.md`'s `dateTimeInput` row and `docs/spec/ui/backend_contract.md` gain the new
   field and the backend-optionality note (a backend may ignore `presentation` and always show text;
   `picker` is a request, not a guarantee, matching this spec's existing pattern for backend hints
   elsewhere, e.g. `table`'s `pinned` column hint which "a renderer may ignore").

### C. Cross-table column-width coordination (surfaced by A, not separately filed)

Named here because A's own limitation section surfaces it, and it would otherwise be silently assumed
solved once A ships. **Out of scope for this design** — it needs its own spec decision (does a `table`
node gain a `columnLayoutGroup` id so sibling tables with the same id share one `columnCache`? does
`showHeader: false` implicitly also mean "join the column layout of the table whose header is shown
above me"?) and should be filed as its own issue once A lands and the repro is re-tested against it,
per this session's own "discover-then-assert" discipline — don't design for a problem not yet confirmed
to still reproduce once A's narrower fix is in.

### D. Type scale (#996 item 5e, unconfirmed)

Not designed here. #1029's evidence section references "#996's recurring theme" of a type scale
without independently re-confirming it against current code in this session. Before any design work,
this needs the same direct-code-check treatment every other item in Part 1's audit got — read
`ShellFrame`'s and `Navigator`'s actual font/role assignments for the app name, section headings and
entries, and confirm whether a scale already exists (e.g. via `TextRole`, already used elsewhere in
this codebase for `TableColumn::role`) before concluding anything is missing.

## Part 3 — What this means for adapting an existing morph-based GUI to declarative UI

This audit is direct, first-party evidence for the broader question motivating this whole investigation
(an existing Lastrada GUI's realistic path onto `morph::ui`'s declarative layer): the vocabulary gaps a
real business-app shell hits (#996, #1029's "six gaps in one sitting... building a real, design-matched
Qt Quick client... against a committed design system") are **largely already closed** by the time of
this audit, days after they were filed — min/max sizing, alignment/justify, form presentation, table
empty-state and navigator list-item styling are all confirmed present on current master. What remains
open (table-header suppression for grouped tables, a real date picker, tab-bar-hides-with-one-tab) are
narrow, well-scoped, independently fixable gaps, not evidence of a vocabulary unsuited to the problem
shape. Combined with the earlier finding that `morph::table::Engine` already independently solves the
dense-grid problem Lastrada's own `TableEngine` was built for (the #1016 virtualization design's own
research), the realistic risk in adapting bmop2/`src/UI` onto `morph::ui` is concentrated in: (1) the
confirmed, still-unresolved Qt Quick row-virtualization gap (#1016, already designed separately), (2)
the two gaps this document designs for above, and (3) whatever a real migration attempt surfaces that
neither #996 nor #1029's one-sitting survey happened to hit — not a deep, structural mismatch between
the vocabulary and the problem domain.

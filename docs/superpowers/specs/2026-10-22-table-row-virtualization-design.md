# Table row virtualization — design (revision 4)

Closes #1016 (duplicates #1017, #1018). Revision 2 corrects three blocking and six significant errors
a review round found in revision 1 (preserved in git history): revision 1 assumed `ui::Table` already
rides on `morph::table::Engine`'s sort/filter/paging machinery, which is false — confirmed by `grep -rl
"morph/table" include/morph/ui include/morph/views src/qt_quick`, which returns nothing. `ui::table
<RowT>()` (`view.hpp` ~1437-1475) takes a bare `std::function<std::vector<RowT>()>`; there is no
`DataSource`, no `TableQuery`, no `Selection` type anywhere near the view tree. Several of revision
1's mitigations (the deferred-reorder interaction in its old §6, the server-mode selection story in
its old §4.5/§5) leaned on an integration that does not exist. This revision designs against the
system that actually exists: `ui::Table`/`mount.hpp`'s keyed reconciliation, with no assumption that
`morph::table::Engine` is, or will soon be, wired underneath it. Wiring that engine into the view tree
is real, valuable, future work — the "Not here yet" list in `docs/spec/table/engine.md` already says
so — but it is a separate project this spec does not depend on.

## 1. Intent

**Goal:** A `Table` (or `ForEach`) with far more rows than fit on screen mounts in bounded time and
memory, proportional to what is visible plus a small overscan margin — not to the row count.

**Non-goals:**
- Wiring `ui::Table` to `morph::table::Engine`. That integration does not exist today (§0). This spec
  virtualizes whatever `ui::Table` is today — a keyed `ForEach` over an arbitrary row vector — and
  stays correct whether or not a future engine integration lands on top of it later.
- Virtualizing every container kind. Only keyed row lists (`ForEach`, `Table`), which have the one
  property a window needs: a stable key per row.
- A generic windowed `ContainerWidget` for arbitrary non-list content.
- A TUI virtualized backend in this change (§5 explains why it needs no change, not that it was
  skipped).

**Anti-goals:**
- Breaking `view_tree.md`'s guarantee that a row whose key stays **in the window** keeps its widget,
  focus and selection exactly as today. A row that leaves the window is explicitly, visibly exempted
  from "live widget" (not from "state") by this spec — see §3 — but nothing about in-window behavior
  changes.
- Silently dropping or corrupting selection for a key that is in the data but currently out of the
  realized window. (Revision 1 missed that this is already a real risk with the design as drafted;
  §4.4 fixes it structurally rather than disclosing it as a residual risk.)
- Requiring every application author to opt in just to avoid an OOM. Virtualization is the table's own
  default above a size threshold.

## 2. Current state (verified against `origin/master` @ `16ed13de`)

- `include/morph/ui/mount.hpp` `mountRows`/`reconcile`/`mountRow`/`reorder` (~1096-1237): every key in
  the model's snapshot gets a `KeyedRow` — one `reactive::Scope` holding both the row's reactive state
  *and* its mounted widget tree together — created once, kept alive until its key disappears.
  `reconcile` runs **only inside the rows `Effect`**, when `session.pull()`'s sources (the `rows`
  binding) change (mount.hpp:1104-1112) — it has no independent trigger today.
- `src/qt_quick/qml/MorphTable.qml`: `body` is a plain `ColumnLayout`, sized to its content
  (`implicitHeight: header.implicitHeight + body.implicitHeight`); every row `reconcile` creates is a
  real, permanently-instantiated QML item. `TableImpl`/`TableRowImpl` (`table.cpp`) mirror this —
  `adopted`/`releasing` fire only when a key is added/removed from the *data*, never on scroll.
- `TableImpl::existingSelection()` (table.cpp:117-129) filters the requested selection by
  `children()` — i.e., by which rows currently have a realized widget. `backend.hpp`'s documented
  `TableWidget` contract (~452-461) already states "a key still waiting for its row is dropped by a
  user's change" — written for the case of a row whose data hasn't arrived yet, not for a row that
  exists but is scrolled out of view. Applied unchanged to a virtualized table, a multi-select
  extend-click would silently drop every selected key outside the realized window.
- `morph::table::Engine` (`include/morph/table/engine.hpp`) computes sort/filter/`ViewChange` against
  a full view and, in `server` mode, paginates via `PageRequest`/`Page<Row>`. **This is not reachable
  from `ui::Table` today** (§0) — it is a toolkit-free library a server or a hand-written client can
  call, not something the view-tree mount uses.
- `mountPass` (mount.hpp ~265-291) tracks the first `common.autofocus` widget mounted during a pass
  and focuses it once the outermost pass completes. `mountRow` (mount.hpp:1132-1143) calls `mountPass`
  per row. Any later call to `mountRow` for the *same* row — which a windowing scheme needs when a row
  scrolls back into view — re-runs this focus-stealing logic exactly as if the row were new.
- `reactive::Scope` (`include/morph/reactive/scope.hpp`): `_owned` is one flat vector of type-erased
  owned objects; `clear()`/the destructor tear down everything in it, newest first. There is no
  partial release of "half of what a scope owns."
- `table-engine-design.md` §15 "Mounting" is a **planned**, not yet executed, measurement: "needs the
  reactive runtime and the view tree, so it is measured with the `table` node... not with the engine."
  `docs/spec/table/engine.md`'s own "Not here yet" list repeats this. No code in `tests/table/`
  measures scope-only mount cost today.
- #1016's 9.4 GB/2,000-row measurement (filed 2026-10-20) predates `16ed13de`/PR #1022 (merged
  2026-10-21), which fixed a separate, measured O(rows×columns) cost in the same code path (#1007).
  Whether 9.4 GB for 2,000 rows is itself partly that defect, now fixed, or an independent cost of
  per-widget QML overhead at a scale nobody had tried, is **not yet known** — #1016's own first
  comment already flags this as unresolved.

## 3. The widget/state split, and why it needs two scopes, not one

`view_tree.md`'s "survives" guarantee exists so a sort/filter/refetch never costs a row its native
focus or in-place edit state. True virtualization (destroy a row's realized form when it scrolls out,
recreate — possibly recycled — when it scrolls back) is in real tension with "never destroy."

Revision 1 proposed keeping a `KeyedRow`'s whole `Scope` alive while only the widget came and went,
and justified it as preserving "a row's state" (an in-progress edit) across windowing. Review found two
errors in that reasoning:

1. **It is not implementable as specified.** `mount(*row.scope, ...)` (mount.hpp:1137) adopts the row's
   bindings *and* its widget into the **same** `Scope`, and `Scope` cannot selectively release part of
   what it owns (§2). "Release the widget, keep the scope" needs the row to own **two** scopes: one
   for state that must survive windowing, one for the realized widget, created and destroyed
   independently.
2. **It protects the wrong state.** A controlled input's in-progress value (`view_tree.md`,
   "Controlled inputs") lives in the *application's* signal, not in anything the row's own `Scope`
   owns — so it already survives under full teardown (destroy both scopes) just as much as under a
   partial one, because the application never forgot it. What a row's own scope actually owns is
   `TypedForEach::Slot`'s `Signal<RowT> _row` (trivially re-derivable from the snapshot on
   re-realization) and whatever signals the row's own `rowView` closure makes for itself — which, for
   most real row views, is little to nothing beyond the view tree the widget half already needs.

**Revised decision:** split `KeyedRow` into two independently-owned pieces:

```cpp
struct KeyedRow {
    Key key;
    std::unique_ptr<reactive::Scope> stateScope;   // the row's slot/signal; always alive while the key exists
    RowSlot* slot = nullptr;
    std::unique_ptr<reactive::Scope> widgetScope;  // the mounted widget tree; null when out-of-window
    Widget* widget = nullptr;                      // == widgetScope ? &rootWidget : nullptr
};
```

`stateScope` is created once per key, exactly as today's single scope is, and destroyed only when the
key leaves the data — this is what "survives" continues to mean, scoped honestly to what the row's own
state actually is, not inflated into a data-loss-prevention claim it was never earning. `widgetScope`
is created when the row enters the realized window (§4.1) and destroyed when it leaves, through
`mountPass` exactly as today, but that makes re-realizing a row **a fresh mount, not a resumed one** —
which forces an explicit answer to the autofocus and selection problems below, rather than letting
revision 1's single-scope framing paper over them.

This is a smaller, more honest claim than revision 1's: windowing does not preserve anything beyond
what re-deriving a row's signal from the current snapshot already gives you. If a future engine
integration (§0) adds row-local edit buffers that must survive scrolling, that is new state needing
its own design, not something this spec's two-scope split retroactively provides.

## 4. What changes

### 4.0 Prerequisite: re-measured #1016 post-#1022 (done)

Re-run on current `master` (`tests/bench_mount.cpp`, `RecordingBackend`, Release, `morph_tests
"[.benchmark][mount]"`), re-creating #1016/#1007's own repro shape (one checkbox cell + two text
cells per row) and a minimal one-text-cell-per-row variant as the closest available proxy for
"scope-only" cost (there is no widget-less mount path to measure directly before Task 1 builds one):

| Row shape | 500 rows | 2,000 rows | 10,000 rows | 20,000 rows |
|---|---:|---:|---:|---:|
| Multi-cell (checkbox + 2 text), #1016's own shape | 4.7 ms / 3,501 nodes | 19.2 ms / 14,001 nodes | 108.1 ms / 70,001 nodes | 222.0 ms / 140,001 nodes |
| Minimal (1 text cell) | 2.2 ms / 1,501 nodes | 9.7 ms / 6,001 nodes | 51.6 ms / 30,001 nodes | 104.6 ms / 60,001 nodes |

Both questions this measurement existed to answer are settled:

1. **The O(rows²) compounding #1016 suspected is gone.** 2,000 rows costs 19ms and 14,001 live
   nodes on `RecordingBackend` — nowhere near the original 9.4GB/"never finished." Scaling stays
   clean and linear through 20,000 rows (10x the rows costs ~10x the time, not more). The #1007 fix
   (the O(rows×columns) column-width rescan) was the dominant cost in the original measurement, not
   a second, independent defect — #1016's own speculation on this point is confirmed correct.
2. **Per-row cost scales with cell/widget count, not with the row's own reactive state.** Multi-cell
   (3 widgets/row) costs almost exactly 2x minimal (1 widget/row) at every row count tested (e.g.
   20,000 rows: 222ms/140,001 nodes vs 105ms/60,001 nodes — a ~2.1x ratio, tracking the 3:1 widget
   ratio, not the identical row count). This is the evidence Option A's premise needed: the expensive
   part of mounting a row is building its widget tree, not creating its signal/slot. Keeping a row's
   state alive while only its widget comes and goes under windowing is therefore the cheap half of
   the problem, as §3 argues — this measurement is what turns that argument from plausible into
   evidenced.

**What remains true regardless:** `RecordingBackend` has no Qt Quick item creation cost, no QML
engine overhead, no layout/paint cost — these numbers are a lower bound on the view-tree mount's own
logic cost, not a prediction of real Qt Quick wall-clock time. The real, still-unaddressed problem is
unchanged by this measurement: Qt Quick's `MorphTable.qml` realizes every row as a live `QQuickItem`
regardless of viewport, which is what actually blew up to 9.4GB — that cost lives in the Qt Quick
backend (§4.3), not in the mount-layer logic this benchmark isolates. This measurement rules out a
*second* defect in the mount layer; it does not by itself prove Qt Quick virtualization is
unnecessary — the opposite: 140,001 live reactive nodes for 20,000 rows is itself a number worth
revisiting once real tables reach the 100,000-row scale `table-engine-design.md` names as a target,
since nothing here measures real-world row counts an order of magnitude higher.

Threshold default: given 2,000 rows (`table-engine-design.md`'s existing `smallTable` constant) costs
only 19ms to mount in full on the mount layer alone, the case for virtualizing *below* 2,000 rows is
weak on this evidence — the existing `smallTable` threshold is retained as this design's default,
not revised upward or downward by this measurement.

### 4.1 Triggering: a windowing Effect, not a wider `reconcile`

`reconcile` runs from inside the rows `Effect`, driven only by `pull()`'s result changing (§2). A
scroll event is not a data change and must not re-run `reconcile`'s full `firstOccurrences`/`wanted`
pass (O(rows) per scroll tick, unacceptable at 100k rows). Revision 1 did not give windowing its own
trigger; this revision does:

- The backend hook (§4.2) writes into a `Signal<VisibleRange>` the mount owns for that table.
- A **separate Effect**, made alongside the rows Effect in `mountRows`, depends only on that signal
  and on the `rows` vector `reconcile` already maintains (the existing `KeyedRow` list, by reference —
  not a re-pull). It computes which keys should be realized now (window ± overscan) versus which
  currently-realized keys should be released, and acts only on that delta: realize newly-in-window
  rows (§4.1a), release newly-out-of-window rows (§4.1b). It does not touch `stateScope`,
  `firstOccurrences`, or `wanted` — those remain `reconcile`'s job, untouched by scrolling.
- After realizing/releasing, the windowing Effect calls the existing `reorder` (mount.hpp:1147-1189)
  restricted to the realized subset, so newly-realized widgets land in the right position among
  already-realized ones — `reorder`'s existing "a row with no widget has no place in the container"
  handling already does most of this; see §4.1c for the one addition it needs.

**4.0a — `reconcile` must not mount every new row's widget unconditionally; this was underspecified
and is corrected here (revision 3).** As written, §4.1's windowing Effect only ever *releases*
widgets `reconcile` has already realized — it never prevents `reconcile` itself from eagerly
realizing a widget for every new key on the very first mount, which is exactly the behavior §1's
goal exists to remove (a 100,000-row table would still build 100,000 widgets before the backend's
first `setOnVisibleRangeChanged` call ever arrives). `reconcile`'s "mount the new keys" loop
(mount.hpp's `reconcile`, the `for (std::size_t const index : order)` pass that currently calls
`mountRow` unconditionally) must become virtualization-aware:

- When `virtualize` is null (`ForEach`) or `keys.size() < virtualize->threshold`: unchanged — every
  new key gets a full `mountRow` (state + widget), exactly as today. This is the common case and the
  one every existing document and test exercises; nothing about it changes.
- When virtualizing and above threshold: a new key only gets a full `mountRow` when its index falls
  within the **current known window** (see 4.0b for what that is before the backend has ever
  reported one); every other new key gets a state-only row — a narrower entry point, call it
  `mountRowState`, that does what `mountRow` does minus the `mount(*row.widgetScope, ...)` call and
  the `onRow` hook, leaving `widgetScope`/`widget` null and `widgetState = Dewindowed` from the
  start. This keeps `reconcile`'s own cost at O(rows) for `stateScope` construction (cheap, per
  Task 0's measurement) regardless of row count, while widget construction — the expensive part —
  only happens for the window.

**4.0b — The bootstrap window, before the backend's first real report.** A Qt Quick item cannot
report a meaningful visible range before its first layout pass completes, and that layout pass
itself needs *some* rows realized to measure against (§4.3's row-height estimation reads real,
mounted rows once any exist) — so there is an unavoidable gap between "the table mounts" and "the
backend tells us what's actually visible." Resolution: `VisibleRange`'s default-constructed value
(`{0, 0}`, §4.1's own `Signal<VisibleRange>` initial value) is read by `reconcile`/the windowing
Effect as "no real report yet," and is treated as `{0, overscan}` instead — i.e., realize the first
`overscan` rows optimistically, the same "optimistic default, corrected once the real thing is
measured" shape §4.3 already uses for row-height estimation, not a new kind of guess. The backend's
first genuine `setOnVisibleRangeChanged` call (once Qt Quick's `Flickable` has laid out and knows
its real viewport) then corrects this through the ordinary windowing Effect path, exactly as any
later scroll would — there is no special-cased "first report" branch, only a special-cased *default*
for the signal nothing has written to yet. A table that mounts with fewer rows than `overscan`
realizes all of them regardless, which is already true of `min(overscan, rows.size())` without a
separate check.

**4.1a — Realizing a row already in `stateScope`.** A new, narrower entry point than today's
`mountRow` builds only `widgetScope` and the widget tree, against the row's *already-existing* `slot`
(no `session.makeRow`/`RowSlot` construction — that already happened when `stateScope` was made).
Crucially, this call must **not** go through `mountPass` unconditionally: §4.1d covers why.

**4.1b — Releasing a row's widget.** `widgetScope.reset()` (or a narrower subset of the destructor
behavior `Scope::clear()` already provides, since `widgetScope` owns nothing but the widget tree) runs
the container's `releasing` callback exactly as a real removal does today (`TableImpl::releasing`,
table.cpp:76-89) — reusing that hook is still correct, because from the backend's point of view a
widget genuinely is going away; only `stateScope`/`slot` staying alive is new, and the backend doesn't
see `stateScope` at all.

**4.1c — `reorder`'s existing null-widget branch is not reusable as-is.** Revision 1 claimed
dewindowed rows could reuse `reorder`'s existing `row.widget == nullptr` handling (today meant for a
row whose *view* is a null node — see `mountKind`'s `Node` can be empty, mount.hpp ~1060). Reusing the
same `nullptr` state for two different meanings ("this row's view is intentionally empty" vs. "this
row is dewindowed, temporarily") means the windowing Effect cannot tell them apart on the next pass —
it would keep trying to realize a row that is null by design. `KeyedRow` needs a third explicit state
(e.g. an enum `{Realized, Dewindowed, NullView}`, or keeping `widget == nullptr` for "no widget ever
intended" and adding a separate `bool dewindowed` alongside it) so the two cases stay distinguishable.

**4.1d — Autofocus on re-realization.** `mountPass` (mount.hpp ~265-291) focuses the first
`common.autofocus` widget found during the pass it wraps. Re-realizing a row via the ordinary mount
path on every scroll-in re-triggers this: a row with an `autofocus` cell would steal focus back every
time it scrolls into view, which is wrong — the user did not ask for that. §4.1a's narrower
realization entry point must suppress `_autofocus` capture for a re-realization (only a row's *first*
realization, when it is genuinely new data, should be eligible to receive initial autofocus) — a small
but real addition revision 1 did not have, since it did not separate "first realize" from
"re-realize" at all.

### 4.2 The backend contract — a two-way seam, not one setter

Revision 1 proposed one new virtual, `setOnVisibleRangeChanged`, and claimed this was "exactly one
new seam." Review correctly found this insufficient: with only the realized rows as children, the Qt
Quick backend has no way to size a scrollbar or place a leading/trailing spacer for the un-realized
rows before and after the window, because it does not know the *total* row count or the window's
position within it. The seam needs to carry information both ways:

```cpp
class TableWidget : public ContainerWidget {
    // existing members unchanged
    // Mount -> backend: there are `total` rows; only [first, first+count) are realized as children.
    virtual void setRealizedWindow(std::size_t first, std::size_t count, std::size_t total) {}
    // Backend -> mount: the user scrolled; please realize roughly this range (± the backend's own
    // overscan judgment, e.g. rounding to a block boundary) next.
    virtual void setOnVisibleRangeChanged(std::function<void(std::size_t first, std::size_t count)> onChange) {}
};
```

Both default to doing nothing, so every existing `TableWidget` implementer (TUI, `RecordingBackend`,
and a Qt Quick table under the threshold) needs no change to keep compiling or keep passing its
current tests — this part of revision 1's claim holds. Only the Qt Quick backend above the threshold
implements both for real. `MorphTable.qml` uses `total`/`first`/`count` to size `body`'s effective
content height (estimated row height × `total`, §4.3) with the realized block positioned at
`first × estimated height` inside it via leading/trailing spacer items — the standard virtualized-list
shape, not a novel one.

### 4.2a A virtualized table needs its own bounded viewport (found during Task 2, revision 4)

Discovered implementing Task 2: today, a `table` node is placed bare (no `scroll` wrapper) and grows
to its full content height; whatever ancestor holds it (a `panel`, a `dialog`'s own `ScrollView`, the
page itself) is what actually scrolls when content overflows — confirmed against real document
authoring, e.g. `examples/bookmarks/screens/bookmarks_screens.cpp`'s `ui::panel({ui::table(...)}, ...)`
with no `scroll` in between. This has a real consequence for virtualization that neither revision
1-3 nor the original "Flickable-based scroll wiring" phrase in §7's Task 2 line spelled out: a
virtualized table growing to its full *estimated* height (§4.0's `implicitHeight` compensation, tens
of thousands of rows × `estimatedRowHeight`) and relying on an ancestor to scroll it would mean
scrolling through, e.g., 99,950 rows of empty estimated space to reach the end of a 100,000-row table
— virtualization would bound the widget count but not fix the actual scrolling UX, and the ancestor's
own scrollbar would be meaningless at that scale.

**Decision: a virtualized table owns its own bounded internal viewport (`Flickable`) instead of
growing to full content height.** Concretely: a `Table` above its `Virtualization::threshold` must
have an explicit height from its `Common::layout` sizing (`Fixed` or `Stretch`) — this is a new,
explicit requirement for a virtualized table specifically; a non-virtualized table keeps today's
"grows to content" behavior exactly. The document/screen author of a genuinely large table already
has to decide how much screen real estate it gets (it cannot sensibly show all 100,000 rows' worth of
space at once regardless of virtualization); this makes that decision explicit in the document rather
than silently defaulting to something arbitrary. A `Table` with `virtualize.threshold` rows worth of
data but no explicit height (still using `Sizing::content()`, the default) is not refused at mount —
`LayoutHints`'s own existing refusal rules (`view.hpp`'s doc comment: "A negative bound... refused at
mount") do not currently cover "content-sized but should have been fixed," and adding that validation
is out of scope here — but it clips to whatever height its container happens to resolve it to, which
without an explicit height from the author is `0` in some containers and the container's own full
content height in others (the author gets an obviously-wrong table, not a crash); `docs/spec/qt_quick
/backend.md` should document this as a requirement, not just a consequence.

### 4.3 The Qt Quick backend: row height, column width, and selection

- **Row height estimation** is still the open sub-problem revision 1 named and left for
  implementation to spike: a fixed estimated height for scroll-position math, corrected once real rows
  are measured, versus a uniform-row-height requirement. Unchanged from revision 1; still unresolved
  here deliberately.
- **Column width becomes approximate and will oscillate, not just grow.** Revision 1 disclosed only
  that a virtualized table's cached column width "may resize to a wider cell" as new rows scroll in.
  Review found the actual behavior is worse: `TableImpl::releasing` already calls
  `invalidateAllColumns` on every row removal (table.cpp:87), which rescans *every* content column
  over the remaining `body.children`. Under windowing this fires on every dewindowed row, every scroll
  tick, and a column's cached width can shrink back down when the row that set its current maximum
  scrolls away — producing visible jitter in both directions during a scroll, not monotonic growth.
  This must be disclosed in `docs/spec/qt_quick/backend.md` as real, user-visible behavior, and
  implementation should consider whether a virtualized table needs **monotonic-only** column widths
  (never shrink once grown, accepting a column that stays wider than currently necessary) as a
  deliberate trade-off to kill the jitter, rather than reusing `invalidateAllColumns` unchanged.
- **Selection must become purely key-based when virtualized.** `TableImpl::existingSelection()`
  filters `_requested` by which rows are realized children (table.cpp:117-129); `tapped()`'s
  Multiple+extend path (136-147) rebuilds `_requested` from that filtered result, which would silently
  drop every selected key currently out of the realized window on the user's very next selection
  change. This is not a disclosed-and-accepted risk, it is a correctness bug a virtualized table
  cannot ship with. Fix: a virtualized `TableImpl` keeps `_requested` as the source of truth
  unconditionally (never narrowed to realized children) and only uses `children()` to decide which
  *currently-realized* rows should show as visually selected — `existingSelection()`'s filtering
  behavior must change for the virtualized case, not merely be documented as a known gap.

### 4.4 The declarative layer and the threshold

`ForEachModel`/`ForEachSession`/`RowSlot` need no signature change — `pull()` still returns every key
up front (§0: there is no cheaper way to know the view size without the engine integration this spec
explicitly does not assume), and `makeRow(index)` still only runs once per key, now specifically for
`stateScope` construction (§3) regardless of whether the row is currently realized.

**The threshold and overscan must be known to the mount at `reconcile` time, not deferred to document/
author-facing plumbing.** Revision 1 split this across two places — "the backend decides" (§4.2) and
"a `TableOptions` field" (§4.4) — which review correctly flagged as unworkable: the backend cannot
know the row count, only the mount can (`keys.size()` in `reconcile`). The threshold/overscan are
`Table`/`ForEach` node fields from the start (added in the same task that adds the windowing Effect,
not deferred to author/document wiring), defaulting to `table-engine-design.md`'s existing
`smallTable` constant (2,000 rows, pending §4.0's re-measurement) and a small overscan row count.
Below the threshold, `reconcile` takes the unwindowed path verbatim — **no behavior change below the
threshold**, which is the one claim from revision 1 this revision keeps unchanged.

## 5. What does NOT change

- `morph::table::Engine`'s sort/filter/`ViewChange` computation: wholly out of scope, since it is not
  wired to the view tree today (§0).
- Any table below the virtualization threshold: bit-for-bit unchanged, by construction (§4.4).
- The TUI backend: `container_widgets.hpp`'s out-of-view widgets already get no drawn area, but the
  mount still builds every row's widget regardless — the TUI backend stays eager (and therefore
  correct, if not optimal) only because the new backend hooks default to never firing for it, not
  because its rendering was already "windowed" in the sense this spec means. This is a correction to
  revision 1's framing, not a new decision.
- `RecordingBackend`/tests against it: unchanged, since both new hooks default to inert.

## 6. Risks

- **Row-height estimation accuracy** (§4.3): unresolved, as in revision 1.
- **Column-width jitter** (§4.3): now understood to be bidirectional oscillation, not just growth;
  needs a monotonic-widths decision before implementation, not just disclosure.
- **Keyboard focus across the window boundary**, independent of the autofocus-on-re-realize issue
  §4.1d already fixes structurally: arrow-key/Tab navigation off the bottom of the realized window
  into an unrealized row still needs an explicit answer (scroll-then-realize-then-focus, or clamp
  until the user scrolls manually). #937 (closed, the forms grid widget's keyboard-focus work) solved
  an adjacent problem and should be read before implementation, not re-derived.
- **A focused or being-edited row must be pinned regardless of its position relative to the window.**
  Found in review: `MorphScroll` scrolls the focused item into view (`backend.md` ~149); if a focused
  row's widget is dewindowed by ordinary scroll logic while it holds focus, the resulting focus-loss
  could trigger a scroll-back, which could re-dewindow whatever was scrolled to, forming a feedback
  loop. The windowing Effect (§4.1) must treat a row holding focus, or (once such state exists) one
  mid-edit, as always-realized regardless of the nominal window, the same way §4.0's deferred-reorder
  interaction was flagged in revision 1 — except this spec no longer claims deferred-reorder state is
  available to consult (§0), so a virtualized table's own notion of "pinned regardless of window"
  needs to be designed fresh here, not borrowed from an engine integration that doesn't exist.
- **The evidence for the threshold was pre-#1022; this is now resolved.** §4.0 re-measured on current
  master (`tests/bench_mount.cpp`) rather than letting the design proceed on a number confounded with
  an already-fixed bug. The O(rows²)-shaped catastrophe is gone; the existing `smallTable` threshold
  holds.

## 7. Suggested path to a plan

Revises revision 1's task order to fix the threshold/backend sequencing review found broken; Task 0
is now done, not merely planned.

- **Task 0 (done) — re-measured #1016 post-#1022, and measured the closest available proxy for
  scope-only mount cost** (`tests/bench_mount.cpp`, §4.0). The original 9.4GB/2,000-row catastrophe
  does not reproduce post-#1007 (2,000 rows: 19ms/14,001 nodes on `RecordingBackend`); per-row cost
  scales with cell/widget count (~2x for 3 widgets vs. 1), not with row count alone, which is the
  evidence §3's Option A needed. A true widget-less `stateScope`-only measurement (no proxy) is not
  yet possible — it needs Task 1's own two-scope split to exist first, so it is deferred to be taken
  *during* Task 1's implementation (measure the real thing once it can be built), not before it.
- **Task 1 — `KeyedRow`'s two-scope split (§3), `reconcile`'s virtualization-aware mount pass
  (§4.0a-§4.0b: `mountRowState` for a new row outside the bootstrap/current window, the `{0,
  overscan}` default before the backend's first report), and the windowing Effect (§4.1), with the
  threshold/overscan as a `Table` node field from the start** (not deferred, per §4.4's correction;
  `ForEach` does not get a field, since no backend seam exists for it — §0's audit correction),
  behind the new `TableWidget` hooks (§4.2), exercised through a `RecordingBackend` `Fake` that
  actually fires them — new test surface, since today's fakes don't need to.
- **Task 2 — Qt Quick: the two-way `setRealizedWindow`/`setOnVisibleRangeChanged` seam, the
  `Flickable`-based scroll wiring, the row-height-estimation spike, and the selection fix (§4.3,
  making `_requested` the unconditional source of truth).** Doing the selection fix here, in the same
  task as the scroll wiring, rather than as a later patch, avoids ever shipping the silently-broken
  intermediate state review found.
- **Task 3 — column-width jitter decision** (§4.3): spike monotonic-only widths against the existing
  `invalidateAllColumns` behavior, pick one, document the choice and its visible consequence in
  `docs/spec/qt_quick/backend.md`.
- **Task 4 — spec updates**: `view_tree.md`'s "ForEach and Table rows" section gains the two-scope
  windowing carve-out as normative text (§3's honest version, not revision 1's overclaim about state
  preservation); `docs/spec/qt_quick/backend.md` gains the column-width and selection-under-windowing
  disclosures; a note is added wherever `table-engine-design.md` currently implies `ui::Table` uses
  the engine, clarifying that it does not yet (§0), so a future reader does not inherit revision 1's
  mistake.
- Each task's characterization tests follow `table-engine-design.md` §16's existing pattern:
  characterize current below-threshold behavior before changing the windowed path.

This ordering keeps Task 0 first because both this design's threshold number and its core cost
argument for keeping state alive during windowing depend on measurements that do not exist yet; Task 2
now includes the selection fix specifically so no intermediate commit ships the data-loss-on-select
bug review identified.

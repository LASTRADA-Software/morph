# Program notes — obligations one part's rulings place on later parts

Each part's pre-flight scan reads this file and turns every entry addressed to it into a ruling.

## From Part 1 (reactive), Task 9 timed-refresh ruling (a tick during a call is skipped; period restarts on issue)

- **Part 6, Task 7 (Poller):** the plan's prose (P6:2314–2315) and Poller class doc (P6:2610–2611) say a tick
  supersedes a request in flight "so a reply that never comes cannot wedge the poller", and drop EventPoller's
  bridge-wide execute deadline on that basis. Under Part 1's rule a never-settling reply wedges the Poller. The
  Poller must keep a deadline (a `ClientTimeoutError`, which it already retries on the next tick). The test
  "Poller: a refetch while a page is in flight applies its events once" (P6:2441–2452) becomes "a late reply
  delivers; no second call is issued while it is in flight". State the deviation in Part 6's squash body.
- **Part 8 (P8:~8043) and Part 9 (P9:~9772–9776, ~9791–9794):** tests advance one period right after a fetch is
  issued without waiting for its reply; under Part 1's rule that tick is skipped. Make those tests settle the
  reply before advancing (inferred risk, not reproduced).
- **Part 3 (LoopScheduler):** honour Scheduler guarantees (a) cancel-returns ⇒ never runs even if due in the same
  turn, (b) cancel inside callback, (c) handle outlives scheduler, (d) blocked owner fires once, next deadline from
  the firing. Add a test where one callback cancels a sibling timer due in the same turn.
- **Part 4 (QtScheduler):** honour the same (a)–(d); pin them in its tests.
- **ManualScheduler (Part 1) models a never-blocked loop:** advance() steps deadline by deadline; every() fires
  once per elapsed period; a callback-created timer due at the current firing time waits for the next advance().

## From Part 2 (ui), pre-flight ruling 11

- **Parts 3 and 4 (TUI and Qt Quick backends):** every backend keeps a widget callback alive for the duration of
  its own call (copy it before invoking), so a handler may destroy its own widget — pin it in each backend's tests
  (ASan). A frontend's Scheduler honours `reactive::Scheduler`'s guarantees (a)–(d) — see the Part 1 entry above.
- A binding that throws is reported as `kEffectThrew` (at mount or in a flush); the widget keeps its last value.
- Part 2 ruling 16(b): §7's "application destroyed before the runtime" teardown-order test lives in Parts 3 and 4.
- **Part 3 (TUI backend), from Part 2 Task 2 review:** core::tui has only addChild/removeChild, so `moveChild` is emulated by
  remove + re-add; removing a component ends core-cpp's pointer capture (`Screen::componentDetached`), which would end a
  drag mid-gesture — the TUI backend must keep native state across a move (or not move a component under capture).
  Select must remember a requested key across `setOptions` and re-resolve it; a widget inside a hidden, disabled or
  collapsed container is not actionable; no handler fires after the widget's destructor returns.
- **Part 4 (Qt Quick backend):** same Select re-resolution rule (a ComboBox resets currentIndex on model change); no
  handler fires after the widget's destructor returns (disconnect before deleteLater; beware queued connections).
- **Parts 3 and 4, from Part 2 Task 3:** no backend setter fires a handler, `setOptions`/`setItems` included (Qt: connect
  user-only signals such as ComboBox `activated`, never `currentIndexChanged`). A backend may assume a handler never
  throws: `Mounted` catches and reports (`ui::detail::site::kCallbackThrew`).
- **Parts 3 and 4, from Part 2 Task 6 (tables):** in Multiple mode a user change reports exactly the keys of rows that
  exist and are selected (filter the stored selection; Part 3 plan's `toggle` ~P3:6237 and Part 4's `tapped` ~P4:4364
  toggle within `_selection` and would report rowless keys). A table shows the user's selection even with no
  `selection` prop bound (Part 4: highlight on user pick, not only via setSelection). A hidden cell keeps its column
  (Part 3 plan sizes columns by `shownChildren()` ~P3:6087 — use all cells).
- **Parts 3 and 4, from Part 2 Task 7:** use the ui-level run helper (added in Part 2 Task 7's fix round — check its
  final name in include/morph/ui/frontend.hpp) for factory → null-check → mount → loop → teardown, instead of
  hand-rolling it in `Session::run` (P3 ~7944–7965) / `Frontend::run` (P4 ~6400–6420).
- **Parts 3 and 4, from Part 2 Task 8 (conformance probe):** the probe interface grew — dismiss, selectRows,
  selectedRows, a mutable `childAt`, Select `textOf` = the marked option's label; amend Part 3 Task 18 (TuiProbe, plan
  ~8253, ~8336) and Part 4 Task 8. TuiProbe must: drag nested (non-root) widgets; make `type` replace the text (clear
  first); return a mutable `childAt`. Rules the probe cannot reach, which Parts 3/4 must test themselves: gating of
  choose/toggle/activateRow and drag onto a gated target; handler kinds other than click for "no handler after
  destruction" and "handler may destroy its own widget"; hidden-cell column geometry.
- **Parts 6–10 (app tests on RecordingBackend), from Part 2 final review:** RecordingBackend helpers deliver nothing
  — without an error — to a widget the user cannot reach (inside a closed dialog, hidden/disabled/collapsed
  container). A test that clicks such a widget and then asserts a state change fails confusingly; open/show first.
- **interfaces.md Part 2 section is stale** (no runApplication, kCallbackThrew, RecordingBackend/probe additions,
  childAt const): the headers in include/morph/ui are authoritative.

# Pre-flight conflict scan — Part 2 (`morph::ui`)

Plan: `docs/superpowers/plans/2026-10-04-declarative-ui-tui-2-ui.md` (cited `P:NNN`).
Authority: spec `docs/superpowers/specs/2026-10-04-declarative-ui-tui-design.md` (§5, §5b, §7, §9), contract
`docs/superpowers/plans/2026-10-04-declarative-ui-tui-interfaces.md` (cited `I:NNN`), the actual Part 1 code at
`2a22b7d9` (`include/morph/reactive/**`, `docs/spec/reactive/*.md`, `git show -s 2a22b7d9`), AGENTS.md,
CONTRIBUTING.md, `.clang-tidy`, `tests/.clang-tidy`, Doxygen `WARN_AS_ERROR`.
Repository: `feature/declarative-ui` at `2a22b7d9`, clean.

## How this was checked

**Measured** (scratchpad `…/scratchpad/pf2ui/` only; the repository was not modified):

- Every code block of the plan was extracted by a script and assembled cumulatively into eight stages,
  one per code task (stage N = Tasks 1..N, every insertion applied at the anchor the plan names). Each
  anchor resolved exactly once. `include/morph/ui/**` was overlaid on symlinks to the real
  `include/morph/*` (Part 1's actual headers).
- Each stage was compiled with the exact flags of `build/reactive/compile_commands.json` (AppleClang 21,
  `-Weverything` minus the `compiler_options.cmake` suppressions, `-Werror`, `-Wno-missing-braces` for tests).
  Each ui header was also compiled standalone, as `VERIFY_INTERFACE_HEADER_SETS` does. The final stage was
  syntax-checked with Homebrew clang 22.1.8 as well. A planted unused variable produced the expected error,
  so the clang 22 run is live. **Zero compiler diagnostics at every stage, on both compilers.**
- Each stage was linked against `build/reactive`'s Catch2 main and core-cpp libraries, then run with `[ui]`.
  Stage 1: 7 cases pass. Stage 2: 17 pass. Stages 3–8: everything passes except one case
  ("unmounting destroys children first…", row B8). The final stage has 62 cases and 222 assertions.
- A fixed variant ("stage9": the code fixes of rulings 1–7 and 9 applied) passes 62/62. It also passes
  62/62 under **ASan+UBSan** (`detect_stack_use_after_return=1`). The ASan run is live: the plan's own T2
  mutation (P:2403) produced `heap-use-after-free`.
- clang-tidy 22.1.8 (CI pins `CLANG_VERSION: "22"`, `ci.yml:87`) with the root `.clang-tidy` over the six
  header stubs, and with `tests/.clang-tidy` subtractions (inherit) over the eight test TUs.
- Doxygen 1.18.0 with `docs/CMakeLists.txt`'s settings over `include/morph/ui`.
- clang-format 22.1.8 with the repo `.clang-format`.
- Mutation probes: T1's constraint mutation (P:786), T2's ASan mutation (P:2403), and T8's "remount kept
  rows" mutation (P:5955), each run 3–5 times. The NOLINT audit stripped every `NOLINT*` in
  `include/morph/ui`, then re-ran tidy (stubs plus four test TUs).

**Not verified:** GCC and MSVC legs, Linux, a Release/`NDEBUG` build, the CMake edits executed in the real
tree, the docs build of the whole repo (only `ui/` was run through Doxygen), markdownlint itself (line
lengths were measured with `awk`), TSan, and the mutation claims of T3–T7 (inferred plausible from the code).

## Table

Status: **OK**, **CONFLICT** (the plan as written fails a gate or contradicts authority), **NOTE** (works, but
something should change or be stated). "M" = measured, "I" = inferred from reading.

### A. Pairs of tasks sharing a file or an interface

| # | Tasks | Produced → consumed (exact names) | Finding | Status |
|---|---|---|---|---|
| A1 | T1→T2 | `Key`, `Action`, `LayoutHints`, `Sizing`, `SelectOption`, `TextRole`/`TextInputMode`/`SelectStyle`/`Axis`/`DateMode`/`FilePickerMode` (P:286–431) → `backend.hpp`, `recording_backend.hpp` formatters (P:1501–1666) | Stage 2 compiles; 17/17 pass (M) | OK |
| A2 | T1→T3 | `Prop::isBound/constant/binding/evaluate`, `Common`, every leaf/container aggregate → `Mounter::bind`, `applyCommon`, `mountKind` (P:2859–3076) | Stage 3 compiles; every other T3 case passes (M) | OK |
| A3 | T2→T3 | `IViewBackend::create*`, widget setters; log lines `create/set/move/destroy`, dump format (P:832–835) → T3 log/dump asserts (P:2517–2779) | All T3 log and dump goldens match (M) | OK |
| A4 | T1→T4 | `view.hpp`: insert after `struct Scroll` (P:3393), `NodeData` replaced (P:3451–3458), builders after `scroll` (P:3460), `switchOn` before the namespace close (P:3479) | Anchors unique; stage 4 compiles (M) | OK |
| A5 | T2→T4 | `SlotWidget/TabsWidget/DialogWidget` after `ScrollWidget`; `createSlot/Tabs/Dialog` after `createScroll`; `Callbacks::dismiss`, `FakeTabs`, `FakeDialog`, `dismiss()` (P:3504–3626) | Anchors unique; compiles. T2's `chooseIndex` already special-cases kind `"Tabs"` (P:2288) and its `Callbacks::index` doc names `TabsWidget::setOnSelect` (P:1680) before T4 adds them. Both are harmless forward references (M) | OK |
| A6 | T3→T4 | `Mounter::bind`, `event`, `applyCommon`, `mount` → Switch/Tabs/Dialog `mountKind`, `showPage`; `detail::Pages` before the `// A view tree is mounted…` comment (P:3630) | Anchor present in T3's text (P:2826); all T4 logs match (M) | OK |
| A7 | T4→T5 | T5 replaces `NodeData` with a superset (P:4178–4183); inserts `site`/`RowHook`/`KeyedRow`/`stableTargets`/`reorderChildren` "before `struct Pages`" (P:4228) | Anchors resolve; stage 5 compiles (M) | OK |
| A8 | T1→T5 | `#include "../reactive/runtime.hpp"`, `signal.hpp` after the datetime include (P:4032); `detail::RowSlot/ForEachSession/ForEachModel/TypedForEach` + `ForEach` after `Busy` (P:4039) | Compiles; no include cycle (M) | OK |
| A9 | T5→T6 | `detail::TypedForEach<RowT>`, `Mounter::mountRows(Scope&, shared_ptr<ForEachModel const> const&, ContainerWidget&, RowHook)`, `RowHook = function<void(Widget&, Key const&)>` → `table<RowT>`, Table `mountKind` (P:4686, 4856) | Signatures match; T6 logs, including `set Row#8 rowKey=3`, match (M) | OK |
| A10 | T2/T4→T6 | `formatSizing/formatKey/formatList`, `FakeContainer`; `enumName(SelectionMode)` after `enumName(FilePickerMode)`; `Callbacks::selection/activate` after `dismiss`; `selectRows/activateRow` after `dismiss()` (P:4758–4841) | Anchors (T4's `dismiss`) resolve; compiles (M) | OK |
| A11 | T1/T4/T5→T6 | Final `NodeData` variant order (P:4655–4657) | Identical to I:136–138 (M by text) | OK |
| A12 | T1→T7 | `Node` → `Application::view()` (P:5145) | `frontend.hpp` includes only `view.hpp` from ui (M) | OK |
| A13 | T2→T8 | RecordingBackend additions `idOf`, `kindOf`, `children`, `widget` (P:826–828) → `RecordingProbe` (P:5379–5415) | Used exactly as declared; passes (M) | OK |
| A14 | T3–T5→T8 | `Mounted`, `root()`, `switchOf`, `tabs`, `dialog`, `forEach`, `text`, `panel` → 13 cases (P:5633–5907) | 13/13 pass (M) | OK |
| A15 | T5→T8 | T8's mutation table (P:5955): remounting every kept row must fail "a ForEach updates a kept row in place", whose only identity check is `probe.childAt(*list, 0) == before` (P:5807) | **The mutant survived 2 of 3 runs** (M). The freed widget's address is reused by the remounted one, so the pointer check is a coin flip. T5's log test kills the mutant deterministically, because ids are creation numbers | CONFLICT |
| A16 | T4→T8 | Mutation "skip `previous->setVisible(false)`" (P:5957) → `tabsMountPagesLazily` | That case's checks read the flag directly (P:5783) (I) | OK |
| A17 | T1,T2,T3,T7,T8 | Root `CMakeLists.txt` `FILE_SET HEADERS` anchors: view after `manual_scheduler.hpp`; backend+recording after view; mount after backend; frontend after mount; conformance after recording (P:110, 809, 2421, 4899, 5339) | Chain is consistent; anchor `CMakeLists.txt:426` exists (M) | OK |
| A18 | T1–T8 | `tests/CMakeLists.txt` chain from `test_reactive_scheduler.cpp` → view → recording → mount → structure → foreach → table → frontend → conformance | Anchor `tests/CMakeLists.txt:153` exists; the chain is consistent (M) | OK |
| A19 | T1–T8→T9 | Specs restate log/dump formats, error texts (P:6390–6395), mount order, `kDuplicateKey`, `widgetEvent` re-post | Match the code and Part 1's `graph.hpp:607–611` (M by reading against code) | OK |
| A20 | T7→T10 | T10 Step 2's layering grep (P:6512) vs `frontend.hpp`'s `#include "../core/executor.hpp"` (P:5075) | No match. The measured transitive morph set of every ui header is `attributes`, `core/{executor,logger,profiler,payload_shape_tag,detail/owner_*}`, `reactive/{runtime,signal,scope,scheduler,detail/graph}`, `util/datetime`. No bridge, net, completion or callback_scope (M) | OK |

### B. Each task against itself

| # | Task | Check | Finding | Status |
|---|---|---|---|---|
| B1 | T1 | Builds, tests, mutation | 7 cases pass, as claimed (P:784). Removing `(!std::invocable<U&>) &&` gives "conversion … to `const ui::Prop<bool>` is ambiguous", as claimed (P:786–788) (M) | OK |
| B2 | T1 | Tidy on `Prop`'s constant ctor (P:315–316) | `cppcoreguidelines-pro-bounds-array-to-pointer-decay` at `static_cast<T>(std::forward<U>(value))`. Any root-config TU that builds `Prop<std::string>` from a string literal triggers it: measured on the `backend_conformance.hpp` stub, and inferred for every examples/ TU in Parts 3–10. Test TUs mask it, because `tests/.clang-tidy` subtracts the check (M) | CONFLICT |
| B3 | T1 | `// NOLINT(bugprone-forwarding-reference-overload)` (P:315) | Dead: stripping it produces no finding (the template is `requires`-constrained) (M). It also gives no reason, against P:6544 | NOTE |
| B4 | T2 | Builds, tests, ASan mutation | 17 cases pass (= "seven plus ten", P:2395). The `invoke` mutation gives ASan `heap-use-after-free` (P:2403) (M) | OK |
| B5 | T2 | Doxygen (`ui::testing::detail` is not in `EXCLUDE_SYMBOLS`) | **13 warnings** (M). `FakeLeaf<I>` overrides `setVisible/setEnabled/setLayout/setDragKey/setDropHandler` (P:1906–1920), `~FakeLeaf` (P:1893, a destructor with a body and no `@brief`), and `FakeContainer::moveChild` (P:1952). The base is a template parameter, so Doxygen cannot inherit their docs. 0 warnings once documented (M) | CONFLICT |
| B6 | T2 | Test tidy | `test_ui_recording_backend.cpp`: `[](ui::Key) {}` (P:914) → `performance-unnecessary-value-param` (M) | CONFLICT |
| B7 | T2 | The file-wide `NOLINTBEGIN/END(bugprone-easily-swappable-parameters)` (P:1487–1489, 2382) | Load-bearing, but not where its comment says (M). It fires on `RecordStore::move(int, size_t)` (P:1762), `dumpInto(string&, int, size_t)` (P:1856) and `find(string_view×3)` (P:2170). It does not fire on the interaction helpers the comment names. Accurate per-site NOLINTs exist for `misc-no-recursion` (P:1855) and `bugprone-empty-catch` (P:1896), and both are load-bearing (M) | NOTE |
| B8 | T3 | "unmounting destroys children first and leaves no binding" (P:2759, 2763) and its mutation row (P:3151) | `CHECK(liveNodes() == 2)` gives `3 == 2`; `== 0` gives `1 == 0` (M). `Signal<std::string> label` (P:2754) is a `detail::Node`, and `Node`'s constructor counts it (`graph.hpp:328–331`). The mutation row's "stays 1" is "stays 2" | CONFLICT |
| B9 | T3 | Header tidy | Two findings (M). (a) `readability-convert-member-functions-to-static` on `Mounter::mount` (P:2848): the generic visit lambda's implicit `this` is not seen (header stub). (b) `bugprone-exception-escape` on the Menu binding lambda `[items]` (P:2978): `items` is a `const&`, so the capture is a `const std::vector` and the closure's move constructor copies, which can throw (every TU that mounts) | CONFLICT |
| B10 | T3 | `NOLINTBEGIN/END(misc-no-recursion)` around `Mounter` (P:2826–2827, 3082) | Dead: stripped, neither the stubs nor four test TUs report it. The cycle runs through `std::visit`'s dispatch table, which the call graph does not follow (M) | NOTE |
| B11 | T4 | Builds, tests | Every T4 log and dump matches, including Review Focus 2 and 3 (M) | OK |
| B12 | T4 | `switchOn` (P:3490–3500) | `performance-unnecessary-value-param` on `Node fallback`. In the uninstantiated template, tidy does not see the `std::move` inside a designated initialiser. Building `Switch spec{…}; spec.fallback = std::move(fallback);` clears it (M) | CONFLICT |
| B13 | T5 | Builds, tests, header tidy | All 10 cases pass (M). `modernize-use-auto` on `constexpr std::size_t kNone = static_cast<std::size_t>(-1);` (P:4259) (M) | CONFLICT |
| B14 | T5 | Test tidy | `misc-const-correctness` on `Signal<std::vector<Lap>> rows` in "mounts one row per key" (P:3861) and in "a duplicate key…" (P:3972), which never write it (M) | CONFLICT |
| B15 | T5 | `NOLINTNEXTLINE(bugprone-easily-swappable-parameters)` on `reorderChildren` (P:4287) | Dead: the two parameters have different types (`vector` by value, `vector const&`) (M) | NOTE |
| B16 | T6 | Builds, tests, tidy on `table` (P:4686–4704) | 4 cases pass (M). `performance-unnecessary-value-param` on `columns` and `options`: the same designated-initialiser blind spot as B12 (M) | CONFLICT |
| B17 | T7 | Builds, tests, test tidy | 11 cases pass (M). `bugprone-exception-escape` on `.make = [name] {…}` (P:4953): `name` is `std::string const&`, so the closure holds a `const std::string` (M) | CONFLICT |
| B18 | T7 | `std::getenv` in a public header (P:5198) | MSVC C4996 ("getenv may be unsafe") fires in any TU without `_CRT_SECURE_NO_WARNINGS`. The repo defines that only PRIVATE on `apply_warnings` targets (`compiler_options.cmake:480–482`), so a downstream consumer built with `/WX` breaks. This is the first `getenv` under `include/` (grep) (I, no MSVC here) | NOTE |
| B19 | T8 | Builds, tests; `NOLINTNEXTLINE` on `Checks::text` (P:5556) | 13 cases pass, with unique names (M). The NOLINT is dead: the three parameters are used together, so the check suppresses itself (M) | NOTE |
| B20 | T9 | markdownlint MD013 (119 columns, `.markdownlint.yaml`) vs Step 5's "expected clean" (P:6473) | P:6296 (`backend_contract.md`, "not whether it is on screen: … A case reaches widgets only through…") is **137 columns**. Every other non-table, non-code line is ≤119 (M, `awk`) | CONFLICT |
| B21 | T9 | `frontend.md`'s `scheduler()` row (P:6348) | It says nothing of the guarantees every frontend's Scheduler must honour (cancel-then-never-runs, cancel inside a callback, a handle outlives its scheduler, no catch-up; `scheduler.hpp:75–80`, `control.md:135–144`). Program notes put exactly those on Parts 3 and 4, and `frontend.md` is the spec they read | NOTE |
| B22 | T10 | Step 4 "Expected: no findings" (P:6544) for the direct stub run | False as worded (M). Pre-existing master findings show up in every ui stub because the header filter is `include/morph/.*`: `core/executor.hpp:338`, `core/logger.hpp:69/82/96/97`, `util/datetime.hpp:114`. Part 1's own stub logs show the same lines | NOTE |
| B23 | T10 | Step 5 `bash scripts/check_install_export.sh` (P:6550) | On macOS, Part 1 needed bash 5 + GNU findutils for the real script (Part 1 ledger, T11 line) (I) | NOTE |
| B24 | T3/T4/T5 | `// Review Focus N.` comments (P:2749, 3264, 3358, 3979, 3995) | They send the reader to a planning document. Against AGENTS.md "Comments"; Part 1 ruling 18 deleted the same pattern | NOTE |
| B25 | all | clang-format | 11 of 14 new files reformat under clang-format 22.1.8 with the repo `.clang-format`; worst are `test_ui_mount.cpp` (196 diff lines) and `recording_backend.hpp` (120). CI pins 22 (M) | NOTE |
| B26 | T1–T8 | Tests that assert nothing; duplicated logic | Every case asserts a log, dump, value or probe count. The one weak identity check is A15. Small helpers (`intKey`, `Lines`) repeat per test file, as is local test idiom. No header logic is duplicated (`detail::Mounter` is the one mount path, and Table reuses `mountRows`) (M by reading) | OK |
| B27 | T1–T8 | Identifier length ≥3, `.at()` in headers, Doxygen on public and `detail` symbols except B5 | No `readability-identifier-length` and no `pro-bounds-avoid-unchecked-container-access` finding in `include/morph/ui` (M) | OK |
| B28 | T10 | Squash procedure (P:6592–6607) | The `--grep="^wip(ui):"` BRE treats `(` as literal. The expected log (`ui:`, `reactive:`, `core:`, `docs:`) matches the branch (`2a22b7d9`, `7bb681fa`, `3f0ee98c`) (M by reading) | OK |

### C. Every Part 2 use of a Part 1 API, against the actual headers

| # | Part 1 API (actual) | Part 2 use | Finding | Status |
|---|---|---|---|---|
| C1 | `Runtime::batch(F&&) -> decltype(auto)` (`runtime.hpp:72–80`) | T8 `boundTextUpdatesOncePerChange` (P:5655) | Matches (M) | OK |
| C2 | `Runtime::widgetEvent(F&&)`. A flush inside it reports `kFlushInWidgetEvent`, then `requestFlush()` re-posts (`graph.hpp:607–611`) | `Mounter::event` (P:2871–2878); T3 and T4 nested-loop tests (P:2586, 3359) | Semantics match "refused and re-posted" (P:6116). Probe count 1, then `runAll` applies (M) | OK |
| C3 | `Runtime::untracked(F&&) const -> decltype(auto)` | `Mounted` ctor returns `Widget*` (P:3104); Switch, Tabs, Dialog, ForEach (P:3661, 3681, 3721, 4351) | Matches; the Review Focus 1 test passes (M) | OK |
| C4 | `Runtime::core() -> shared_ptr<RuntimeCore> const&`; `RuntimeCore::liveNodes()` counts every node made on the owner, **Signals included** (`graph.hpp:221–233, 328–331`) | T3 (P:2534, 2759, 2763) | The plan assumes only Computeds and Effects count; see B8 (M) | CONFLICT |
| C5 | `RuntimeCore::report(char const*) const noexcept` → `noteOwner` (`graph.hpp:124–126`); `OwnerProbeRecorder::count` compares strings | `_rt->core()->report(site::kDuplicateKey)` (P:4364); test (P:3975) | Matches; count 1 (M) | OK |
| C6 | `Scope::make<T>(Args&&...)`, `adopt(unique_ptr<T>) -> T&`, `effect(F&&)` (`scope.hpp:59–103`). Inside `make`: `made.reset();` (`scope.hpp:79`) | `make<std::unique_ptr<reactive::Scope>>()` (P:3653, 3715); `make<std::shared_ptr<ForEachModel const>>(model)` (P:4344) | Compiles and behaves (M). But instantiating `make` with a smart-pointer `T` makes `readability-ambiguous-smartptr-reset-call` fire at **`scope.hpp:79`**, in the mount stub, the conformance stub and four test TUs (M). That line is new on this branch, so `clang-tidy-diff` reports it. `made = nullptr;` clears it (M) | CONFLICT |
| C7 | `Scope` destroys newest-first; first-run clear keeps / destroy discards (`scope.hpp:23–35`) | Widget adopted first, then bindings, then children; child scopes for case, page, dialog, row | Teardown logs match (M) | OK |
| C8 | `Signal(Runtime&, T)`, `get/peek/set/mutate`; equality via `detail::DeeplyEqualityComparable`; no `operator()` | Row signals, test signals; `forEach` overloads `function<vector<R>()>` vs `Signal<vector<R>> const&` | No overload ambiguity, because `Signal` is not callable; equality skip works for `Lap`, `Tag`, `Item` (M) | OK |
| C9 | `Computed<T>(Runtime&, F)` keeps a failure as state; `Effect` reports `kEffectThrew` (catches in its ctor; a flush stops and re-posts the rest) | `Mounter::bind` (P:2864–2865) | Works. But `view_tree.md`'s Misuse table (P:6152–6159) is silent on a throwing binding: what is reported, and that the setter is not called. A debug build asserts on that report when no probe is installed (I) | NOTE |
| C10 | The `Effect` ctor runs the body once in its own `TrackingFrame` (`signal.hpp:332–342`), even inside `untracked` | Bindings and the ForEach Effect made under `Mounted`'s `untracked` | Rows are tracked by the ForEach Effect only (Review Focus 1 passes) (M) | OK |
| C11 | Queued Effects run in creation order (`graph.hpp:597–605`) | T4 Tabs log order (P:3325); T5 "move, then the row's set" (P:3963–3964) | Pass (M) | OK |
| C12 | Every layer survives self-destruction from its own callbacks | Switch and Dialog reset child scopes inside their binding Effect; ForEach unmounts rows inside its Effect | ASan+UBSan clean on all 62 cases (M) | OK |
| C13 | `detail::site::kFlushInWidgetEvent`, `kRuntimeOutlived` (`graph.hpp:49–51`) | T3/T4 tests; `frontend.md` (P:6375) | Names exist (M) | OK |
| C14 | `reactive::Scheduler`, `TimerHandle` (`scheduler.hpp`) | `ui::Scheduler`, `ui::TimerHandle` aliases (P:5093–5096) | Aliases exactly, as I:237–238 asks. The doc gap is B21 (M) | OK |
| C15 | `testing::ManualScheduler::advance(delta)`, `now()`, `pendingTimers()` | Not used by Part 2. Spec §7's "refreshEvery fires on a fake scheduler" is covered by Part 1's `tests/test_reactive_scheduler.cpp` (grep) | No contract name is consumed | OK |
| C16 | Off-owner operations are reported and refused | `Mounted` is documented as made and destroyed by the runtime's owner (P:3088–3090) | Consistent (I) | OK |
| C17 | `testing::StepExecutor::runOne() -> bool`, `runAll()`, `pending()`, `coreExecutor()`; `OwnerProbeRecorder(core executor)` (`tests/test_support.hpp:75–116`, `tests/owner_probe_recorder.hpp:37`) | All `[ui]` tests | Matches (M) | OK |
| C18 | libc++ lacks `std::move_only_function`; `.clang-tidy` `AllowedTypes` covers `std::exception_ptr` only | Plan uses `std::function` throughout; no `exception_ptr` by value | Compiles on libc++ (M) | OK |

### D. Plan against spec and contract

| # | Requirement | Where | Finding | Status |
|---|---|---|---|---|
| D1 | Every `view.hpp` name in I:72–157 exists with the same signature | T1, T4, T5, T6 | All present. `Sizing` adds member defaults; parameter names differ only (M by text, compiles) | OK |
| D2 | `NodeData` variant order I:136–138 | P:4655–4657 | Identical | OK |
| D3 | `backend.hpp` I:163–224 | T2, T4, T6 | All interfaces and factories match. The plan adds `[[nodiscard]]` and deleted copy/move, which the contract allows (I:4) | OK |
| D4 | `mount.hpp`: `Mounted(Runtime&, IViewBackend&, Node, ContainerWidget* = nullptr)`, `root() const noexcept`, `kDuplicateKey = "morph::ui: duplicate ForEach key"` (I:231–236) | P:3098–3114, 4234 | Identical | OK |
| D5 | `frontend.hpp` I:237–253; spec §5b precedence | T7 | All names match. The plan also accepts `--ui <name>` and lets the last one win (P:4911–4913). Part 6's `fromArgs` ignores non-own arguments (P6:678), so the extension is safe | OK |
| D6 | `RecordingBackend` I:261–277 | P:826–828 | Adds `idOf`, `kindOf`, `children`, `widget`, which the contract does not list. No later part consumes them (grep of Parts 3–10) | NOTE |
| D7 | `ConformanceProbe`, `ConformanceCase`, `conformanceCases()` (I:278–295) | T8 | Match. Parameters are spelled `widget`/`container` instead of `w`/`c` (identifier length ≥3). Part 4 hard-codes `REQUIRE(cases.size() == 13)` (P4:5526), and the plan ships 13 | OK |
| D8 | §5: immutable shared nodes, not templated on Msg; `Prop`; `Common` (visible, enabled, layout, dragKey, accepts, onDrop); `Key` never a double | T1 | Mapped and tested (P:155–246) | OK |
| D9 | §5 palette, incl. collapsible `Panel` and drag-and-drop on every node | T1, T4, T5, T6 | Every kind lands with its mount | OK |
| D10 | §5 Mount: Computed+Effect per binding, constant once with no node; Switch/Tabs/Dialog in a child scope, child-first; Tabs lazy; unchanged key never remounts; ForEach in place, mount/unmount, `moveChild`, duplicates refused; `widgetEvent`; bindings before widgets, children before parents | T3–T6 | Each has a test (P:2482, 2538, 3225, 3246, 3308, 3334, 3869–3977, 2586, 2750) | OK |
| D11 | §5 backend contract; `RecordingBackend` with click/edit/choose/toggle/drag | T2 | Mapped | OK |
| D12 | §5b `AppContext`/`Application`/`Frontend`/`FrontendOption`/`selectFrontend`; `run` ordering documented | T7, T9 | Mapped (P:5165–5170, 6356–6375) | OK |
| D13 | §7 ui list: golden dump, constant never binds, one setter call per change, visible/enabled, remount child-first, ForEach identity/minimal ops/int64+string keys/duplicates, no `setText` echo, unmount destroys all | T2–T6 | All present | OK |
| D14 | §7 frontend seam: precedence, **the application destroyed before the runtime**, `refreshEvery` on a fake scheduler | T7 (P:4891–4895) | Precedence is here. Teardown order is deferred to Part 3 (P3:7784) and Part 4 (P4:6067), both verified present. `refreshEvery` is in Part 1's tests. A placement deviation from §7 that the squash body should state | NOTE |
| D15 | §7 conformance: build, bind, update, remount, ForEach reorder, drag onto a target | T8 | 13 cases cover all of them. A15 is the weak spot | OK |
| D16 | §9 docs: `docs/spec/ui/{view_tree,backend_contract,frontend}.md`, README map, ARCHITECTURE namespace and header maps, CHANGELOG; no pinned constant | T9 | Mapped. The GETTING-STARTED pointer is Part 3's | OK |
| D17 | §2 "Depends on `morph::reactive`, never the reverse"; plan constraint "reactive and `util/datetime.hpp` only" (P:37–38) | `frontend.hpp` includes `../core/executor.hpp` directly (P:5075) | Allowed in substance (reactive already depends on it, and the contract's `exec::IExecutor&` needs it). The constraint's wording is narrower than the plan's own code (M, transitive set in A20) | NOTE |
| D18 | §5 "a widget's destructor detaches it"; Part 1 "every layer survives self-destruction from its own callbacks" | `backend_contract.md` (P:6210–6212, 6285–6286) | Only `RecordingBackend` promises to copy a callback before calling it. The rule that *every* backend keeps a callback alive for its own call, so that a handler may destroy its widget, is not in the contract, and Parts 3 and 4 do not mention it (grep). `Mounter::event`'s closure is stored in the widget (I) | NOTE |
| D19 | `program-notes.md` obligations addressed to Part 2 | — | None (entries address Parts 3, 4, 6, 8, 9) | OK |

### E. Repository facts the plan relies on

| # | Fact | Verified at | Status |
|---|---|---|---|
| E1 | `FILE_SET HEADERS` contains `include/morph/reactive/testing/manual_scheduler.hpp` (P:111) | `CMakeLists.txt:426` | OK |
| E2 | `add_executable(morph_tests …)` lists `test_reactive_scheduler.cpp` (P:113) | `tests/CMakeLists.txt:18, 153` | OK |
| E3 | Verify stubs live at `build/reactive/morph_verify_interface_header_sets/morph/<dir>/*.hpp.cxx` (P:6502, 6540) | `reactive/*.hpp.cxx` and `reactive/testing/` present | OK |
| E4 | `time::Timestamp` (`datetime.hpp:376`), `DateTime(year, month, day, hours, minutes, seconds[, ms])`, `toIso8601()` → `…T09:30:00.000Z`, `hasValue()`, `operator*`, defaulted `<=>` | `datetime.hpp:90, 127, 376–409`; goldens pass | OK |
| E5 | `exec::IExecutor` is a `struct` (P:4905 says `:68`) | `executor.hpp:70` (line drift only) | OK |
| E6 | `exec::IoLoop` is a `class` (forward-declared at P:5086–5088, so no `-Wmismatched-tags`) | `io_loop.hpp:54` | OK |
| E7 | `MORPH_LIFETIMEBOUND` in `../attributes.hpp`; Doxygen `PREDEFINED` erases it | `include/morph/attributes.hpp`; `docs/CMakeLists.txt:45` | OK |
| E8 | `tests/test_support.hpp` (`StepExecutor`) and `tests/owner_probe_recorder.hpp` | present | OK |
| E9 | Doc anchors: README "Start here" and "Reactive state and control"; ARCHITECTURE "Namespace map" with the `morph::reactive` row, "Header map" with the `reactive/` subsection; CHANGELOG `[Unreleased]` → `### Added` → `morph::reactive` entry | `docs/spec/README.md:85, 127`; `docs/ARCHITECTURE.md:31, 52, 630, 696`; `CHANGELOG.md:10, 180, 182` | OK |
| E10 | `-Wswitch-enum` + `-Wswitch-default` are on, `-Wno-covered-switch-default`, so `enumName`'s `default:` is required; `-Wno-weak-vtables`, `-Wno-missing-designated-field-initializers`, `-Wno-shadow-uncaptured-local` | `cmake/compiler_options.cmake:135–214` | OK |
| E11 | Doxygen `EXCLUDE_SYMBOLS` lacks `morph::ui::detail` and `morph::ui::testing::detail`, so they must be documented (P:35) | `docs/CMakeLists.txt:56–90` | OK |
| E12 | `tests/.clang-tidy` subtracts identifier-length, array-decay and unchecked-access, and its reach extends to headers analysed from test TUs | `tests/.clang-tidy` | OK |
| E13 | `scripts/check_install_export.sh`, `scripts/check_sanitizer_instrumentation.sh`, presets `clang-asan`/`clang-tsan` | present; `CMakePresets.json:169, 179` | OK |
| E14 | `.markdownlint.yaml` MD013 at 119, code blocks and tables exempt; `.pre-commit-config.yaml` | present | OK |
| E15 | CI pins clang 22 for tidy and format | `.github/workflows/ci.yml:87, 2788–2805` | OK |
| E16 | Part 3's and Part 4's teardown tests named in `frontend.md` (P:6370–6375) exist and assert `kRuntimeOutlived == 0` | P3:7784–7794; P4:6067–6080 | OK |

## Proposed rulings

1. **B8, C4 (T3 `liveNodes`).** Ruling: assert `liveNodes() == 3` before the reset and `== 1` after it, with the comment "the signal, and the text binding's Computed and Effect". Change P:3151's mutation row to "`liveNodes()` stays 2". (A baseline taken before mounting, `before + 2` then `before`, is equally fine.) — `liveNodes` counts every node, Signals included (`graph.hpp:328–331`). Measured `3 == 2` and `1 == 0`, and 62/62 pass with the change. — Cost if wrong: T3's own test is red, and so is every later task's run.

2. **C6 (`scope.hpp:79`).** Ruling: in T4, change Part 1's `Scope::make` from `made.reset();` to `made = nullptr;` (behaviour-identical), and name it in the squash body as a Part 1 line touched by Part 2. The alternative is holder structs instead of `make<std::unique_ptr<Scope>>` (P:3653, 3715) and `make<std::shared_ptr<ForEachModel const>>` (P:4344). — `readability-ambiguous-smartptr-reset-call` fires at a line that is new on this branch, so `clang-tidy-diff` reports it whenever `make` gets a smart-pointer `T` (measured). The one-line change clears it (measured), and Part 1's own precedent applies: unshipped work on this branch is fixed in place. — Cost if wrong: a red clang-tidy-diff gate that names a Part 1 file, which is confusing to diagnose late.

3. **B2, B3 (T1 `Prop` constant ctor).** Ruling: put `// NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-array-to-pointer-decay)` on the initialiser line, with the reason "a string literal converting to `T` decays here; that is the conversion". Remove the dead `NOLINT(bugprone-forwarding-reference-overload)`. — Measured: the decay finding appears in the `backend_conformance.hpp` stub, and would appear in every examples/ TU that writes `.text = "…"`. Measured: the forwarding-reference check does not fire on the `requires`-constrained ctor. — Cost if wrong: every root-config consumer of a string-literal prop trips the tidy gate, starting with T8 and then Parts 3–10.

4. **B12, B16 (T4 `switchOn`, T6 `table`).** Ruling: build the aggregate with the non-moved fields in the designated initialiser, then move-assign the by-value parameters: `Switch spec{.selector = …, .cases = std::move(keyed)}; spec.fallback = std::move(fallback); return switchOf(std::move(spec));`. Do the same with `Table spec{…}` for `columns`/`selection`/`onSelectionChange`/`onActivate`/`common`. — Measured: `performance-unnecessary-value-param` fires on `fallback`, `columns` and `options` as written, and the restructure clears all three without a NOLINT. — Cost if wrong: a red tidy gate on `view.hpp`.

5. **B9, B13 (T3/T5 `mount.hpp` tidy).** Ruling: write `&this->mountKind(scope, spec, parent)` inside the visit lambda (P:2852). Capture `[items = items]` in the Menu binding (P:2978), so the closure holds a non-const vector. Use `constexpr auto kNone = static_cast<std::size_t>(-1);` (P:4259). — Measured: each change clears its finding. — Cost if wrong: a red tidy gate (the exception-escape finding fires from every TU that mounts).

6. **B5 (T2 Doxygen).** Ruling: give `FakeLeaf`'s five `Widget` overrides, `~FakeLeaf` and `FakeContainer::moveChild` a `/// @brief`, plus `@param` for each parameter. — Their base is a template parameter, so Doxygen cannot inherit the interface docs. Measured: 13 warnings before, 0 after, under `WARN_AS_ERROR = FAIL_ON_WARNINGS`. The other choice, adding `morph::ui::testing::detail` to `EXCLUDE_SYMBOLS`, contradicts the plan's "full Doxygen including `detail`" and P:6470 "fix the header, not the Doxygen configuration". — Cost if wrong: the Docs workflow fails.

7. **B6, B14, B17 (test tidy).** Ruling: write `[](ui::Key const&) {}` (P:914); declare `Signal<std::vector<Lap>> const rows` in the two T5 tests that never write it (P:3861, 3972); capture `[name = name]` in T7's `option()` (P:4953). — Measured findings, and measured clean afterwards. `clang-tidy-diff` analyses changed test files (Part 1 fixed three such findings at its gate). — Cost if wrong: a red tidy gate.

8. **B3, B7, B10, B15, B19 (NOLINT audit).** Ruling: remove the NOLINTs measured dead: `Prop` forwarding-reference (P:315), `SliderWidget::setRange` (P:1329), `reorderChildren` (P:4287), `Mounter`'s `NOLINTBEGIN/END(misc-no-recursion)` (P:2826–2827, 3082) and `Checks::text` (P:5556). In `recording_backend.hpp`, replace the file-wide `NOLINTBEGIN/END(bugprone-easily-swappable-parameters)` with `NOLINTNEXTLINE` on the three measured sites, each with an accurate reason: `RecordStore::move` (an id and a position), `dumpInto` (a buffer, an id and a depth), `find` (the contract's kind/property/value lookup). If T10 Step 4, run with CI's clang-tidy 22 on Linux, fires on a removed site, restore that one NOLINT with its reason. — This follows Part 1's precedent: a dead suppression hides a future real finding. The current file-wide comment states a reason the measurement contradicts, and AGENTS.md requires comments to state what the code does now. — Cost if wrong: one NOLINT restored at T10. In the other direction, stale suppressions mask real findings.

9. **A15 (T8 conformance identity check).** Ruling: in `forEachUpdatesInPlace`, build the ForEach with a row view that counts its builds, and add `checks.that(builds == 2, "updating a kept row built its view again")` beside the pointer check (P:5807). — Measured: the remount mutant survived 2 of 3 runs on the pointer check alone, because the allocator reuses the address. With the counter, the mutant fails 5/5 and the unmutated code passes 5/5. The counter is backend-independent, so Parts 3 and 4 inherit a deterministic check. — Cost if wrong: a backend or mount that remounts kept rows (losing focus and selection, which spec §5 requires to survive) passes conformance.

10. **B20 (T9 markdown).** Ruling: rewrap P:6296 at ≤119 columns. — Measured at 137 columns; MD013 is 119 and does not exempt prose. — Cost if wrong: T9 Step 5's markdownlint and pre-commit fail.

11. **C9, B21, D18 (T9 spec text).** Ruling — add three short statements, each describing current behaviour:
    - `view_tree.md`, Misuse: a binding that throws is reported as `reactive::detail::site::kEffectThrew` (at mount or in a flush); the setter is not called, so the widget keeps its last value; the read retries on the next change.
    - `frontend.md`, `scheduler()` row: a frontend's Scheduler honours `reactive::Scheduler`'s guarantees; link `../reactive/control.md` rather than restating them.
    - `backend_contract.md`, Widgets: every backend keeps a callback alive for the duration of its own call (for example by copying it before invoking), so a handler may destroy its own widget, as `RecordingBackend` does.

    Add a program-notes entry so Parts 3 and 4 pin the last two in their own suites. — Parts 3 and 4 read these specs as authority, and neither currently states either rule (grep). The behaviour is Part 1's actual semantics (inferred, not reproduced on a real backend). — Cost if wrong: a TUI or Qt backend that frees a callback mid-call (a use-after-free), or a Scheduler that violates guarantees `Query` relies on.

12. **B22 (T10 Step 4).** Ruling: change the expectation to "no findings under `include/morph/ui/` or `include/morph/reactive/`". Pre-existing findings in `core/executor.hpp:338`, `core/logger.hpp:69/82/96/97` and `util/datetime.hpp:114` appear in every stub, because the header filter is `include/morph/.*`. They are on lines this branch does not change, so `clang-tidy-diff` (the CI gate) does not report them. — Measured, and identical in Part 1's logs. — Cost if wrong: the implementer chases master's findings, or edits unrelated headers inside this part.

13. **B23 (T10 Step 5).** Ruling: on macOS, run `check_install_export.sh` under bash 5 with GNU findutils on `PATH`, as Part 1's T11 did, and say so in the hand-off. — Part 1 ledger. — Cost if wrong: a false failure, or the gate skipped.

14. **B24 (comments).** Ruling: delete the five `// Review Focus N.` lines (P:2749, 3264, 3358, 3979, 3995); the test names already say what each case is. — AGENTS.md "Comments"; same as Part 1 ruling 18. — Cost if wrong: comments that point into an archived plan.

15. **B25 (format).** Ruling: run clang-format 22 on every new or changed file before each `wip` commit. — Measured: 11 of 14 files reformat; CI pins clang 22. Same as Part 1 ruling 19. — Cost if wrong: a red format job.

16. **D6, D14 (deviations to state).** Ruling: the squash body (P:6587) names three things. (a) `RecordingBackend`'s four additions (`idOf`, `kindOf`, `children`, `widget`), plus `--ui <name>` and last-wins in `selectFrontend`. (b) That §7's "application destroyed before the runtime" is pinned by Part 3 and Part 4, not here, and `refreshEvery` by Part 1. (c) Ruling 2's touch of `scope.hpp`. Optionally add the four members to I:261–277. — The contract says a deviation is stated in the squash commit (I:4–6). — Cost if wrong: none functional; an undocumented contract extension.

17. **D17 (layering wording).** Ruling: keep `frontend.hpp`'s direct `core/executor.hpp` include. Read the constraint at P:37–38 as "`morph::reactive` (with the core executor it already depends on) and `util/datetime.hpp`"; T10 Step 2's grep already encodes the real rule. — Measured: no bridge, net, completion or toolkit header is reachable from any ui header. — Cost if wrong: none.

18. **B18 (MSVC `getenv`).** Ruling: in `processEnvironment`, guard the call for MSVC (`#if defined(_MSC_VER)` with `#pragma warning(suppress : 4996)`, or `_dupenv_s`), with a one-line reason. — Inferred, not measured: there is no MSVC here, and the repo's `_CRT_SECURE_NO_WARNINGS` is PRIVATE to `apply_warnings` targets, so an installed consumer built with `/W4 /WX` sees C4996 from a morph header. — Cost if wrong: a downstream Windows build failure. The repo's own CI is likely unaffected.

## Per-task map of rulings

| Task | Rulings |
|---|---|
| T1 | 3, 8 (`Prop` NOLINT), 15 |
| T2 | 6, 7 (`[](ui::Key const&)`), 8 (`setRange`; recording NOLINTs), 15, 16 (additions, stated later) |
| T3 | 1, 5 (`this->mountKind`, `[items = items]`), 8 (`misc-no-recursion` block), 14, 15 |
| T4 | 2, 4 (`switchOn`), 14, 15 |
| T5 | 2 (also covers `make<shared_ptr<…>>`), 5 (`kNone`), 7 (`const rows`), 8 (`reorderChildren`), 14, 15 |
| T6 | 4 (`table`), 15 |
| T7 | 7 (`[name = name]`), 15, 17, 18 |
| T8 | 8 (`Checks::text`), 9, 15 |
| T9 | 10, 11 |
| T10 | 12, 13, 16 (squash body); re-checks 8 on CI's tidy |

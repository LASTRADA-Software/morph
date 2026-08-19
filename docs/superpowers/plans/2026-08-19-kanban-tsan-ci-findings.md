# Findings: Kanban/ThreadSanitizer CI leg surfaced two framework-level bugs

**Status:** one fixed on this branch (`917ea54`), tracked upstream as
[#127](https://github.com/LASTRADA-Software/morph/issues/127); one open,
tracked upstream as
[#128](https://github.com/LASTRADA-Software/morph/issues/128). Neither is
kanban-specific — both live in shared framework code (`morph::async::
Completion`, `morph::qt::QtExecutor`) or in the test harness
(`examples/common/testkit/backend_rig.hpp`), reached via kanban's own
`test_kanban_stress.cpp` because Task 10 (this branch) is the first time any
CI job ever built and ran kanban's Qt-linked concurrent tests under real
ThreadSanitizer instrumentation. Recorded here (not silently dropped) per
this repo's established convention (see the sibling
`2026-08-18-kanban-rung4-completion-followup-testkit-isolation.md` note for
precedent).

## Finding 1 — heap-use-after-free in `QtExecutor::post()` (fixed, #127)

**Symptom:** `Kanban / ThreadSanitizer` failed with a genuine heap-use-
after-free; the same underlying bug also crashed two **uninstrumented**
Linux CI legs outright as a plain SIGSEGV
(`Linux / clang-coverage`, `Linux / all optional features (clang)`).

**Root cause:** `QtExecutor::post()` (`include/morph/qt/qt_executor.hpp:40`)
uses `Qt::QueuedConnection` — it enqueues a callback and returns
immediately, it does not run it. `ThreadPoolExecutor`'s destructor
(`include/morph/core/executor.hpp:66-82`) joins every worker thread,
guaranteeing every `post()` call a pool task made *happened*, but not that
the resulting Qt event was ever *pumped*. `morph::bridge::Bridge::
executeVia` (`include/morph/core/bridge.hpp`) chains **three**
`Completion<T>` objects per dispatched action, each settled from *inside*
the previous one's delivered callback (`LocalBackend`'s own completion ->
`executeVia`'s per-call completion -> the caller's own attached handler). A
caller that only waits on its own top-level completion (e.g.
`test_kanban_stress.cpp`'s `pumpUntil(outstanding == 0)`) has no visibility
into the intermediate posts, so it can observe "done" and let `BackendRig`
tear down `QtExecutor` while an intermediate post is still queued,
undelivered, on the Qt event loop. When that stale event finally runs, it
issues the next nested `post()` — against an already-freed `QtExecutor`.

**Fix (this branch, commit `917ea54`):** `BackendRig::~BackendRig()` now
resets `_workerPool` explicitly (forcing every pool-issued `post()` to have
already happened) and then drains the Qt event loop for a few bounded
slices before the rest of member destruction (including `_qtExecutor`)
proceeds — mirroring `QtWebSocketServer::closeGracefully()`'s own
established `processEvents`-drain pattern.

**Verification:** a minimal, framework-only, standalone repro (no kanban, no
`Bridge` — just `ThreadPoolExecutor` + `QtExecutor` +
`Completion`/`Promise::makeSettleable`, reproducing the exact three-level
nesting `executeVia` produces) crashes 5/5 runs without the fix and exits
cleanly 10/10 with it, including at kanban's own real scale (200 concurrent
chains across 4 real worker threads, not just one). Full repro and CI
evidence are in #127.

**Why this is a framework bug, not a kanban bug:** `QtExecutor`,
`Completion`, and the executor-teardown ordering they depend on are shared
by every rung. Anything using `morph::async::Completion` chained across an
executor boundary — which `morph::bridge::Bridge` does on every call — is
exposed to this whenever the owning executor is torn down promptly after the
caller observes its own top-level completion as "done." `BackendRig` (test
infrastructure shared by every rung) is one such owner; there are likely
others (`main.cpp` in each ladder rung, any long-lived `AppContext`).

## Finding 2 — a second, distinct race survives the Finding-1 fix (open, #128)

**Symptom:** after the Finding-1 fix landed, `Kanban / ThreadSanitizer`
still failed — now with 165 warnings of a different character: not a
lifecycle use-after-free in application/testkit code, but races centered on
Qt's own internal `QCallableObject`/`invokeMethod` machinery and on
`kanban::GetBoardResult`'s move constructor, both reached exclusively
through `QtExecutor::post()`/`CompletionState::setValue`/`setException`.

**A load-bearing discovery made while investigating this:** the
`kanban-tsan` CI job's own comment claims `test_kanban_stress.cpp`'s TSan
test "runs entirely on `Mode::Local`'s `ThreadPoolExecutor{4}` with no
Qt/GUI involvement" — the stated reason this job is the *only* exception to
this repo's otherwise consistent policy ("a GUI stack under TSan is mostly
noise," `linux-sanitizers`' own comment) of keeping Qt out of the sanitizer
matrix. **This claim is false**: `BackendRig`'s `Mode::Local` unconditionally
constructs a real `morph::qt::QtExecutor`, and every one of the 165
warnings' stacks bottoms out in genuine Qt internals
(`QMetaObject::invokeMethod`, `QCallableObject`, `QObject::event`). This
test does exercise a real Qt event-loop stack, so the premise the job's
exception to the "Qt under TSan is noise" policy relies on does not hold —
which matters for triage regardless of whether the 165 warnings turn out to
be real bugs or TSan false positives from an uninstrumented, prebuilt Qt
package (Qt is not built with `-fsanitize=thread` in this CI; TSan cannot
see whatever internal locking Qt itself relies on).

**Not resolved on this branch.** Two repro attempts (a 200-chain scale-up of
Finding 1's own repro, and a targeted repro of `Bridge::publishResult`'s
exact multi-subscriber `std::any`-copy fan-out shape at kanban's own scale)
both ran clean without reproducing a crash — but neither used
ThreadSanitizer (not available in the local environment this was
investigated from), so they only rule out a plain-crash reproduction, not
the race itself. Full evidence, stack traces, and suggested next steps
(confirm/rule out the Qt-instrumentation gap; fix or retire the
`kanban-tsan` job's incorrect premise; pursue a TSan-instrumented repro
directly) are in #128.

## Why this is not fixed on `ladder-kanban-impl` / PR #121

Finding 1 is fixed here because it was reachable, cleanly scoped to test
infrastructure (`BackendRig`), and verified independently before landing.
Finding 2 remains open: it needs either confirmation that the Qt package
this CI job links is TSan-instrumented (to know whether the warnings are
real), or a redesign of the `kanban-tsan` job to actually avoid Qt (matching
what its own comment already believed to be true), or a TSan-instrumented
local repro this session could not build. None of those are kanban-rung
work — they are framework/CI-infrastructure decisions this plan is not
scoped to make unilaterally, consistent with this branch's existing
precedent of documenting (not fixing) out-of-scope framework findings (see
the testkit cross-test isolation followup note).

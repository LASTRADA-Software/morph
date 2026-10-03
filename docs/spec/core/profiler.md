# Profiler zones (`MORPH_ZONE` and friends) — design

`include/morph/core/profiler.hpp` gives morph compile-time profiler
instrumentation: [Tracy](https://github.com/wolfpld/tracy) zones on the
framework's hot paths, names for the threads morph owns, and plot and message
macros for anything else. It is off by default and costs nothing when off: the
macros are stubs that do not evaluate their arguments, and the build has no
Tracy dependency.

It is the compile-time counterpart of [`morph::observe`](observability.md),
not a part of it. `morph::observe` is a runtime seam a host wires to its own
metrics backend; this is a build option a developer turns on to look at a
timeline.

## Contents

- [Turning it on](#turning-it-on)
- [The macros](#the-macros)
- [Zones, phases and threads](#zones-phases-and-threads)
- [Zone list](#zone-list)
- [Thread names](#thread-names)
- [One definition everywhere](#one-definition-everywhere)
- [One Tracy client per process](#one-tracy-client-per-process)
- [The capture check](#the-capture-check)
- [Design decisions](#design-decisions)
- [Limitations](#limitations)

## Turning it on

```sh
cmake -S . -B build -DMORPH_ENABLE_TRACY=ON
```

CMake first looks for an installed Tracy (`find_package(Tracy CONFIG)`) and
otherwise fetches `wolfpld/tracy` at `v0.13.1` through CPM. Either way
`morph::morph` gains, as `INTERFACE` properties, the definition
`MORPH_TRACY_ENABLED` and a link to `Tracy::TracyClient`. A found
`TracyClient` that does not define `TRACY_ENABLE` is a configure error: every
Tracy macro would compile to nothing, and the build would claim profiling while
recording no zone.

A fetched client is built with `TRACY_ON_DEMAND`, so a process records only
while a profiler is connected; without it, the client buffers every event from
process start until one connects. `MORPH_TRACY_ON_DEMAND=OFF` (advanced)
builds it to record from the first zone, which is what a capture of a
short-lived program needs.

An installed morph built with Tracy on carries both properties in its exported
target, and its package config finds Tracy for the consumer.

## The macros

| Macro | Tracy on | Tracy off |
|---|---|---|
| `MORPH_ZONE(name)` | A zone named `name` (a string literal) that ends with the enclosing scope. At most one per scope. | Nothing. |
| `MORPH_ZONE_TEXT(text)` | Attaches `text` (anything convertible to `std::string_view`) to the innermost `MORPH_ZONE`; an empty text attaches nothing. | Nothing; `text` is not evaluated. |
| `MORPH_PLOT(name, value)` | Records `value`, as a `double`, on the plot `name` (a string literal). | Nothing; `value` is not evaluated. |
| `MORPH_THREAD_NAME(name)` | Names the calling thread. | Nothing. |
| `MORPH_MESSAGE(text)` | Sends `text` to the timeline. | Nothing; `text` is not evaluated. |

A disabled macro discards an empty object whose type names each argument
through `decltype`. The operand of `decltype` is unevaluated, so a zone's text
costs nothing to compute in a build without Tracy, and naming each argument
there is what keeps a local used only by a macro from tripping
`-Wunused-variable` or `-Wunused-but-set-variable`. Not `sizeof`, which would
do the same for the compiler: clang-tidy reads `sizeof` of a string or a
pointer as a mistaken size computation (`bugprone-sizeof-container`,
`bugprone-sizeof-expression`). `tests/test_profiler.cpp` holds both properties:
every local in it is used only by a macro, under the project's full warning
set, and it counts how many times a zone text was computed.

The names are morph's own. Lightweight defines stubs under Tracy's names
(`ZoneScoped`, `ZoneScopedN`, `TracyPlot`, …) when its Tracy option is off, and
an application can include Lightweight and morph in one translation unit. Were
morph to define the same names, a build with one library's Tracy on and the
other's off would have one header's stubs redefine the other's real macros.
`profiler.hpp` defines no Tracy name.

## Zones, phases and threads

A Tracy zone begins and ends on one thread, and zones on a thread must nest. A
dispatch does neither: it starts on the caller, runs on the model's strand, and
settles on whichever thread the backend settles from, with its callbacks on the
callback executor. So:

- **Each phase is its own zone, on the thread that runs it.** No zone spans a
  hand-off between threads.
- **No zone stays open across a `co_await`.** A suspended coroutine's thread
  goes on to run other work, whose zones would close out of order with the
  open one. This is why the WebSocket send side is zoned at `enqueueFrame`, not
  in the coroutine that writes the frames.
- **The phases of one call are linked by `session::Context::requestId`,**
  written as zone text where the phase can see the call's session. A request
  that carries no request id leaves its zones without text.

## Zone list

| Zone | Where | Thread | Text |
|---|---|---|---|
| `Bridge::executeVia`, `Bridge::executeAttachedVia`, `Bridge::executeCreatingVia` | `bridge.hpp` | the bridge's owner | — |
| `Bridge::dispatchNow` | `bridge.hpp` | the owner; later than `executeVia` for a call that waited for its bind | the bridge's session's `requestId` |
| `ActionTraits::toJson`, `ActionTraits::resultFromJson` | the `ActionCall` codec a remote backend calls | the backend's thread | — |
| `BridgeSink::settleValue`, `BridgeSink::settleException` | `bridge.hpp` | wherever the backend settles | — |
| `BridgeSink::forward` | `bridge.hpp` | the settling thread, or the owner when bridge-side work is posted there | — |
| `LocalBackend::executeInto` | `backend.hpp` | the caller | the call's `requestId` |
| `LocalBackend::startLocal`, `LocalBackend::startTaskLocal`, `LocalBackend::finishLocal` | `backend.hpp` | the model's strand | the call's `requestId` |
| `ModelStrands::task` | `strand.hpp`, the strands' around-task hook | the pool thread running one strand task | — |
| `ActionDispatcher::dispatch`, `ActionDispatcher::dispatchAsync` | `registry.hpp` | the model's strand | the installed session's `requestId` |
| `ActionDispatcher::prepareAction` | `registry.hpp`: decode and the pre-handler gates | the model's strand | — |
| `recordActionSuccess`, `recordActionFailure` | `registry.hpp`, the journal write of an outcome | the model's strand | — |
| `wire::encode`, `wire::decode` | `wire.hpp` | the encoding or decoding thread | the envelope's `requestId` |
| `SocketBackend::fileExecute` | `net/socket_backend.hpp` | the I/O loop | the envelope's `requestId` |
| `SocketBackend::fileControl`, `SocketBackend::dispatchIncomingEnvelope`, `SocketBackend::drainFrames` | `net/socket_backend.hpp` | the I/O loop | — |
| `ws::enqueueFrame` | `net/detail/ws_connection.hpp`, the send side | the I/O loop | — |
| `InMemoryOfflineQueue::enqueue`, `FileOfflineQueue::enqueue` | `offline/` | the queue's owner | — |
| `SyncWorker::drain`, `SyncWorker::replay` (one per item) | `offline/sync_worker.hpp` | the worker's owner | — |

`RemoteServer`'s own phases (`dispatchMessage`, `dispatchExecute`) are not
zoned yet; the dispatcher, strand and codec zones inside them are.

## Thread names

| Name | Thread |
|---|---|
| `morph.pool` | every `ThreadPoolExecutor` worker |
| `morph.io` | the thread an `IoLoop` runs; it carries every socket, every `TimeoutScheduler` deadline and the connectivity probe built on that loop |

An application runs one `IoLoop`, so there is one `morph.io` thread; a
`TimeoutScheduler` constructed without a loop owns one of its own, which is
named the same.

## One definition everywhere

morph is header-only, so these macros change the bodies of inline functions.
A program in which one translation unit is compiled with
`MORPH_TRACY_ENABLED` and another without it holds two definitions of the same
inline function — an ODR violation the linker resolves by keeping one,
silently. CMake consumers cannot get there: the definition is an `INTERFACE`
property of `morph::morph`, so every target that links it agrees. A build that
does not use morph's CMake package must define `MORPH_TRACY_ENABLED` for all
of its translation units or for none.

MSVC and clang-cl turn a mismatch into a link error: `profiler.hpp` emits
`#pragma detect_mismatch("morph_tracy_enabled", "0" | "1")`. GCC and Clang
have no equivalent, and a mismatch there is undetected.

## One Tracy client per process

Two copies of Tracy's client in one process do not work, and two versions
certainly do not. Lightweight fetches Tracy itself when its own Tracy option is
on, so morph's fetch uses Lightweight's CPM name (`tracy`), tag (`v0.13.1`) and
options (`TRACY_ENABLE`, `TRACY_ON_DEMAND`). Whichever library adds Tracy
first fetches it; the other's `CPMAddPackage` finds the package already added
under that name and reuses it. Moving morph's tag means moving Lightweight's
with it.

core-cpp has Tracy instrumentation of its own (`CORE_CPP_WITH_TRACY`, its
`CORE_*` macros) and pins `v0.14.1` when it fetches Tracy itself. Under morph
it fetches nothing (`CORE_CPP_FETCH_DEPS OFF`) and takes the
`Tracy::TracyClient` its parent has already provided, which is why morph adds
Tracy before core-cpp: a build with both options on runs the one `v0.13.1`
client, and core-cpp's instrumentation compiles against it. Such a build
cannot also install morph with a fetched Tracy — core-cpp's install rules do
not export a client it did not install — so it needs `MORPH_INSTALL=OFF` or an
installed Tracy.

## The capture check

The nightly workflow's `tracy-capture` job builds `morph_bench` and
`morph_bench_alloc` with `MORPH_ENABLE_TRACY=ON` and
`MORPH_TRACY_ON_DEMAND=OFF`, runs each under `tracy-capture` with
`TRACY_NO_EXIT=1`, exports zone statistics with `tracy-csvexport`, and fails
unless every named zone has a non-zero count
(`scripts/check_tracy_capture.sh`). `morph_bench` drives `RemoteServer`'s
dispatch and so the codec, dispatcher and strand zones; `morph_bench_alloc`
drives `Bridge` over `LocalBackend`.

The check fails closed. No CSV, a CSV with no rows, a column layout other than
the one it reads, a named zone absent or counted zero, a program that was built
without Tracy (the capture never connects), and a program or capture that does
not exit are all failures. `scripts/test_check_tracy_capture.sh` feeds the
assertion each of those as a synthetic CSV and runs first in the job.

## Design decisions

| Decision | Choice | Why |
|---|---|---|
| Macro names | `MORPH_*`, never Tracy's | Lightweight stubs Tracy's names; sharing them breaks a translation unit that includes both libraries with their Tracy options set differently. |
| Disabled form | `decltype` over the arguments | Unevaluated, so a zone's text is free when off, and still a use of every argument, so no unused-variable warnings. |
| Zone variable | `morphProfilerZone`, a nested one shadowing the outer one under a suppressed `-Wshadow` | `MORPH_ZONE_TEXT` has to find the innermost zone by name. Tracy's own `___tracy_scoped_zone` would be a reserved identifier once spelled in morph's header. |
| `MORPH_ZONE` on Tracy | `ZoneNamedN` with morph's own shadow suppression, followed by `static_assert(true)` | Tracy's `ZoneScopedN` ends in a `;` on some compilers and not on others, so the call site's `;` would be an empty statement on some. The `static_assert` consumes it on all. |
| Linking phases | `Context::requestId` as zone text | It is already the correlation id `morph::observe`'s trace sink uses, and every phase that can see the call can see it. |
| Tracy version | `v0.13.1`, Lightweight's tag and CPM name | One client per process. |
| Fetched client mode | `TRACY_ON_DEMAND` by default | A library must not make an unprofiled process buffer events forever. |

## Limitations

- **No zones in `RemoteServer` yet.** `RemoteServer::dispatchMessage` and
  `RemoteServer::dispatchExecute` are the obvious next two.
- **core-cpp is not instrumented.** Its event loop and strand pump are its own;
  morph's zones start at morph's around-task hook.
- **No statistics collector and no `morph::observe` → Tracy sink.** Both are
  separate decisions about `morph::observe`, whose spec rules out aggregation
  inside morph.
- **The ODR rule is enforced only under MSVC and clang-cl.**
- **macOS captures of a short program can be empty.** Tracy's client on Apple
  starts lazily and is never destroyed, so `TRACY_NO_EXIT` does not hold the
  process open for the capture; a program that finishes before
  `tracy-capture` connects is lost. The nightly check runs on Linux, where the
  client is a static object and waits.

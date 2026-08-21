# userver vs. morph — a survey, not a scorecard

Date: 2026-08-17
Sources: [userver GitHub](https://github.com/userver-framework/userver), [userver.tech docs](https://userver.tech/), morph specs under `docs/spec/`, morph headers under `include/morph/`.

## 1. Scope note

[userver](https://github.com/userver-framework/userver) is a full backend-services
framework: its own stackful-coroutine engine, async drivers for Postgres/Mongo/Redis/
MySQL/ClickHouse, an HTTP server and gRPC server with middleware pipelines, built-in
distributed tracing and metrics, a fleet-wide dynamic-config system, a pytest-based
"testsuite" for spinning up real services in integration tests, plus caching,
distributed-locking, and periodic-task subsystems. It is designed to be *the* runtime
a backend microservice is written against.

morph is a narrower, more focused C++23 **client-server actions framework**: typed
actions and models dispatched over a `Bridge` (WebSocket transport, in-process
`LocalBackend`, or a raw-socket reference transport), with strand-serialized shared
model instances, a journal-based action log for sync/audit, an offline queue + sync
worker for disconnected operation, and a thin observability seam for metrics/tracing
hooks. It does not have its own coroutine runtime, its own DB wire-protocol drivers,
an HTTP/gRPC server, or a config-service client — and it isn't trying to.

Given that, most of what follows is **not** "morph is missing X that userver has."
Several of userver's headline subsystems (its coroutine engine, its DB drivers, its
gRPC server, its dynamic-config fleet system) solve problems specific to being a
standalone backend-service runtime — problems morph's design deliberately routes
around by staying a client/server actions layer on top of whatever executor and DB
access the host already has. Where a userver subsystem addresses something morph
plausibly *could* care about later (observability shape, testing-fixture design,
graceful shutdown, periodic tasks), that's called out explicitly in §4. Where it's
just a different domain (gRPC transport, SQL wire drivers, coroutine scheduling),
that's called out in §5 so it isn't mistaken for a gap.

## 2. Side-by-side table

| Subsystem | userver's approach | morph's approach | Notable difference |
|---|---|---|---|
| **Concurrency** | Own stackful-coroutine engine (`engine::TaskProcessor`) M:N-schedules `engine::Task`s onto a configured pool of OS threads; cooperative yield on I/O; deadline propagation (`engine::Deadline`) and cancellation tokens (`CancellationPoint()`, `TaskCancellationBlocker`) built in. | No coroutine runtime. `IExecutor::post(std::function<void()>)` is the whole abstraction; `ThreadPoolExecutor`/`MainThreadExecutor`/`QtExecutor` implement it. Per-model-instance serialization via `StrandExecutor` (FIFO queue keyed by `ModelId`), not per-task fibers. | userver *is* a concurrency runtime; morph *consumes* whatever executor the host provides and only adds instance-level serialization on top. No cancellation tokens, no deadline propagation across the executor abstraction itself. |
| **Database layer** | Own async drivers per DB (`storages::postgres::Cluster`, Mongo, Redis, MySQL, ClickHouse) with topology-aware pooling, master/replica routing, async query execution integrated with the coroutine engine. | Delegates entirely to the externally-fetched Lightweight ORM (`Lightweight::GlobalDataMapperPool()`) over ODBC (SQLite/MSSQL/Postgres via one `SqlQueryFormatter` dispatch). Lightweight has an async coroutine path, but shipped morph model code calls the synchronous `Acquire()` path exclusively. | userver's DB layer is deeply integrated with its own coroutine scheduler (non-blocking under load); morph's DB calls block whichever strand-executor worker thread is running that model's action — acceptable because that thread is dedicated worker-pool capacity, not a giant M:N fiber pool. |
| **RPC/server layer** | Full HTTP server (`server::handlers::HttpHandlerBase`) and gRPC server (`ugrpc::server::ServiceComponentBase`) with configurable middleware chains (auth, rate-limit, deadline propagation, tracing, decompression, etc.). | `Bridge` + typed actions/models over WebSocket (`QtWebSocketBackend`/`QtWebSocketServer`) or a Qt-free raw-socket reference transport (`morph::net::SocketBackend`/`SocketServer`), plus in-process `LocalBackend`/`SimulatedRemoteBackend` for tests. No HTTP or gRPC server; `IAuthorizer` is the one mandatory choke point instead of a middleware pipeline. | Different transport model entirely: userver serves arbitrary HTTP/gRPC clients; morph is a typed action bridge for its own client/server pairing, with authorization as a single hook rather than a composable pipeline. |
| **Tracing & observability** | `tracing::Span` stack with automatic cross-task and cross-network propagation (`X-YaTraceId`/`X-YaSpanId` headers), `utils::statistics::Writer` metrics exposed via a Prometheus/Graphite endpoint, `logging::LogExtra` structured logging. | `morph::observe`: a closed `enum class Metric` (8 values) + `MetricEvent{metric,value,tags}` delivered to a host-installed `MetricSink`; a `TraceSink{beginSpan,endSpan}` pair keyed by `SpanId`/`requestId`, also host-installed. No bundled backend, no span propagation format, no sampling/aggregation. | userver ships a working tracing/metrics *system*; morph ships the *seam* (seam is intentionally thin — same "no policy, just a hook" pattern as `morph::log`). |
| **Dynamic config** | `dynamic_config::Source`/`Snapshot`, hot-reloaded fleet-wide from a config service (`components::DynamicConfigClient`) without redeploy; used for kill-switches, timeouts, experiment flags. | None. Every `*Config` type (`ReconnectCoordinatorConfig`, `PoolConfig`, `SocketServerConfig`, etc.) is a plain aggregate set once at construction; changing behavior means reconstructing the object. No fleet-wide config service concept exists. | Genuine absence, not a scope call — but morph has no long-lived fleet of server processes to reconfigure without redeploy, which is the problem this subsystem solves. |
| **Testing tooling** | pytest-based "testsuite": starts the real service binary, mocks externals via `mockserver`, drives it over HTTP with `service_client`; `UTEST`/`UBENCH` are coroutine-aware gtest/gbench replacements. | C++ fixtures under `examples/common/testkit/`: `BackendRig` (Local/LocalSingleThread/Socket modes over the *same* test body), `DbFixture`/`DbBusyFixture` (real SQLite-via-ODBC, real `SQLITE_BUSY`), `FaultProxy` (scriptable WebSocket fault injection), `OfflineRig` (real connect/disconnect/reconnect cycles), `ClientPool`. | Both favor real I/O over mocks where practical. userver's testsuite is external-process/HTTP-driven and Python-orchestrated; morph's testkit is in-process C++ fixtures parameterized over deployment topology. |
| **Other structural pieces** | Caching framework (`cache::CacheUpdateTrait`, full/incremental updates, cache dumps), distributed locking (`dist_lock::DistLockedTask` over Postgres/Mongo/YDB), periodic tasks (`utils::PeriodicTask`, cluster-wide), graceful shutdown via `ComponentBase::OnAllComponentsAreStopping()`. | No caching framework (only ad hoc memoized statics), no distributed locking exposed to applications (Lightweight has an internal `SqlAdvisoryLock` for its own migration runner only), no periodic-task facility (only narrow single-purpose timers: `TimeoutScheduler`, `NetworkMonitor`'s probe loop). `RemoteServer::beginShutdown()` gives a one-way `HealthStatus.ready` flip for drain-before-restart. | userver's "cluster of always-on services" subsystems (caches refreshed on every node, cluster-wide dist-lock, periodic jobs on every node) have no equivalent because morph doesn't assume a fleet of long-lived server processes coordinating with each other. |

## 3. Per-subsystem detail

### 3.1 Concurrency model

userver's engine is the framework's foundation: `engine::TaskProcessor` runs stackful
coroutines (`engine::Task`, `engine::TaskWithResult<T>`) cooperatively multiplexed
(M:N) onto a fixed pool of OS threads declared in static config (`worker_threads`,
`thread_name`, `os-scheduling`) — see the [task processors
guide](https://userver.tech/db/d90/md_en_2userver_2task__processors__guide.html).
Convention splits processors by workload (`main-task-processor` for non-blocking
work, `fs-task-processor` for blocking syscalls), and a coroutine yields on any I/O
wait so the OS thread underneath serves other ready coroutines instead of blocking —
this is what lets userver services hold tens of thousands of in-flight requests on a
handful of threads. `utils::Async("name", callable)` spawns tasks with structured
lifetime (a task handle's destructor cancels and awaits the task); cancellation is
cooperative via `engine::current_task::CancellationPoint()`/`ShouldCancel()`, with
`engine::TaskCancellationBlocker` to suppress it for critical sections. Deadlines
propagate through the task tree (`server::request::TaskInheritedData`) and across the
wire (`X-YaTaxi-Client-TimeoutMs` for HTTP, native `grpc-timeout` for gRPC), so a
client-side timeout can abort work several async hops downstream — see [deadline
propagation](https://userver.tech/d6/d64/md_en_2userver_2deadline__propagation.html).
Headers live under
[`core/include/userver/engine/`](https://github.com/userver-framework/userver/tree/develop/core/include/userver/engine)
(`task/task.hpp`, `cancel.hpp`, `deadline.hpp`, `async.hpp`, `mutex.hpp`,
`semaphore.hpp`).

morph has no coroutine runtime of its own — `include/morph/core/executor.hpp` defines
`IExecutor` as a single pure-virtual `post(std::function<void()>)`, and the framework
is agnostic about what runs it: `ThreadPoolExecutor` (fixed `std::thread` pool, FIFO
mutex+condvar queue), `MainThreadExecutor` (tasks collected from any thread, drained
only when the owning thread calls `runFor`/`runOnce`/`drain`), or
`morph::qt::QtExecutor` (marshals onto a Qt event loop via
`QMetaObject::invokeMethod(..., Qt::QueuedConnection)`). The piece specific to
morph's "shared model instance" design is `morph::exec::detail::StrandExecutor`
(see `docs/spec/core/executor.md` and `docs/spec/core/shared_instances.md`): a thin
wrapper keyed by `ModelId` that keeps one mutex-protected FIFO queue per live model
instance, so actions against the *same* instance run strictly one at a time while
different instances run fully in parallel across the base executor's threads —
`shared_instances.md` states this directly: "One strand per instance already gives a
shared instance the serialisation it needs; sharing an instance changes nothing about
how its actions run." There is no thread-per-model and no coroutine suspension:
whatever thread the base `IExecutor` schedules a strand's next task onto is the
thread that runs (and potentially blocks on) that action, including any synchronous
DB call inside `Model::execute`. `~StrandExecutor()` blocks until in-flight work
drains, and the spec is explicit that the base executor must outlive it and must
actually run every posted task or the destructor deadlocks — a documented ordering
invariant, not an incidental detail. There is no cancellation-token concept and no
deadline propagation at the executor layer (`docs/spec/core/executor.md` explicitly
rejects `std::executor` conformance as premature, given limited C++26 availability).

### 3.2 Database layer

userver's DB layer is a family of drivers, each async-integrated with the coroutine
engine. `storages::postgres::Cluster`
([class ref](https://userver.tech/docs/v2.0/dd/d69/classstorages_1_1postgres_1_1Cluster.html))
is the entry point: `Execute(ClusterHostTypeFlags, Query, Args...)`, several
`Begin(...)` transaction overloads, `CreateQueryQueue`, `Listen` for LISTEN/NOTIFY,
and `GetStatistics()`. Topology discovery runs every second over a dedicated
connection per host to detect master vs. replica (`select
pg_is_in_recovery()`), measure RTT, and identify synchronous standbys via `show
synchronous_standby_names` — read-write transactions route to master, read-only
transactions prefer replicas, and a `max_replication_lag` config auto-disables a
lagging replica (see the [pg topology
doc](https://userver.tech/da/d75/pg_topology.html)). Similar cluster-aware driver
components exist for Mongo, Redis, MySQL, and ClickHouse. Everything here executes
without blocking an OS thread — a query suspends the calling coroutine and resumes it
when the driver's async I/O completes.

morph does not have its own DB driver at all. It depends on the externally-fetched
[Lightweight ORM](https://github.com/) (pulled via CMake `FetchContent_Declare`, not
vendored in-tree — it lands under `_deps/lightweight-src/` at configure time), a
"thin, modern C++23 ODBC SQL API" supporting SQLite3, MSSQL, and PostgreSQL through
one `SqlQueryFormatter` dispatch point. The pool is
`Lightweight::Pool<PoolConfig>` (aliased `DataMapperPool`), reached process-wide via
`Lightweight::GlobalDataMapperPool()`, configured at compile time via `PoolConfig`
(`initialSize`, `maxSize`, `growthStrategy` — one of `BoundedWait`,
`BoundedOverflow` [morph's default: grow past `maxSize` without blocking, but shrink
back down], or `UnboundedGrow`). Lightweight does ship a genuine coroutine-async path
(`Pool::AcquireAsync()`, `Async::Task`), but every morph example model inspected
(`examples/kanban/src/models/board_model.cpp`,
`examples/bookmarks/src/models/bookmark_model.cpp`) calls the synchronous
`GlobalDataMapperPool().Acquire()` exclusively. Combined with §3.1's strand model,
the effective shipped pattern is "synchronous DB call on a worker-pool thread inside
a per-instance strand" — a blocking call stalls only that model instance's queue and
the one worker thread executing it, not the whole process. There is no
topology-aware routing, no replica read-splitting, and no retry/exactly-once layer in
morph itself — that's either not present or would need to be built on top of
Lightweight's primitives by the host application.

### 3.3 RPC / server layer

userver provides both an HTTP server and gRPC support as first-class subsystems.
HTTP handlers derive from `server::handlers::HttpHandlerBase`
([class ref](https://userver.tech/docs/v2.0/d6/d36/classserver_1_1handlers_1_1HttpHandlerBase.html));
requests pass through a configurable, ordered middleware chain — the default pipeline
is `HandlerMetrics → Tracing → SetAcceptEncoding → UnknownExceptionsHandling →
RateLimit → DeadlinePropagation → Baggage → Auth → Decompression →
ExceptionsHandling` (see [HTTP server
middlewares](https://userver.tech/docs/v2.0/d6/dcc/md_en_2userver_2http__server__middlewares.html)).
gRPC (`ugrpc::` namespace) mirrors this: `ugrpc::server::ServiceComponentBase` is the
generated-service base, `ugrpc::client::ClientFactory` provides channel-cached
clients, both sides support unary/client-stream/server-stream/bidi shapes with their
own middleware chain (see the [gRPC
guide](https://userver.tech/docs/v2.0/d1/d06/md_en_2userver_2grpc.html)).

morph's transport story is a typed action bridge, not a general-purpose RPC server.
`Bridge` (`docs/spec/core/bridge.md`) holds one active `IBackend` and can hot-swap it
via `switchBackend()`. Concrete backends: `LocalBackend` (in-process, for a
same-process client+model), `RemoteServer` paired with `QtWebSocketBackend`/
`QtWebSocketServer` (real WebSocket transport, `docs/spec/core/backend.md`), a
Qt-free `morph::net::SocketBackend`/`SocketServer` reference transport speaking the
same RFC 6455 framing (opt-in via `MORPH_BUILD_NET`), and
`SimulatedRemoteBackend` for tests. Every backend implements one contract: register/
deregister models, dispatch actions, cancel pending work, react to backend changes.
Rather than a composable middleware pipeline, morph has one mandatory choke point —
`IAuthorizer`, consulted on every `execute` envelope on the remote path
(`docs/spec/security.md`) — plus opt-in pieces layered beside it: stateless
bearer-token authentication (`session_auth.hpp`), a wire-layer envelope-size cap
(`wire::kMaxEnvelopeBytes`, 8 MiB), and a negotiated protocol-version handshake
(`wire::kind == "hello"`). This is a materially smaller and more special-purpose
surface than userver's HTTP/gRPC stack — by design, since morph's clients and
servers both speak morph's own typed-action wire protocol, not arbitrary HTTP/gRPC.

### 3.4 Distributed tracing & observability

userver's `tracing::Span`
([class ref](https://userver.tech/docs/v2.0/d7/d1a/classtracing_1_1Span.html)) forms
an implicit per-task stack; tags attach via `AddTag`/`AddTagFrozen`/
`AddNonInheritableTag`, and creating a new task via `utils::Async` automatically links
a child span to the parent, propagating trace context across coroutine boundaries.
Cross-network propagation rides HTTP headers (`X-YaTraceId`, `X-YaSpanId`,
`X-YaRequestId`) that the userver HTTP client sends automatically and the server
extracts automatically — see the [logging/tracing
doc](https://userver.tech/df/d0c/md_en_2userver_2logging.html). Metrics go through
`utils::statistics::Writer`
([class ref](https://userver.tech/d7/dd9/classutils_1_1statistics_1_1Writer.html)),
exposed on a separate monitor listener in Prometheus or Graphite format (see
[service monitor](https://userver.tech/docs/v1.0/d9/dac/md_en_2userver_2service__monitor.html)).
This is a complete, working observability stack bundled with the framework.

morph's `morph::observe` (`include/morph/core/observability.hpp`,
`docs/spec/core/observability.md`) is deliberately a seam, not a backend. Metrics are
a closed `enum class Metric` (`executeLatencyMs`, `executeInFlight`, `executeErrors`,
`registerCount`, `deregisterCount`, `queueDepth`, `reconnectAttempts`,
`reconnectOutcome`) delivered as `MetricEvent{metric, value, tags}` to a
host-installed `MetricSink`; an unconfigured build pays one relaxed-atomic load per
call site (`metricsEnabled()`). Tracing is a `TraceSink{beginSpan, endSpan}` pair
keyed by a `SpanId` (0 = "no span") and correlated via `session::Context::requestId`;
both callbacks must be set together or tracing is treated as off. Call sites
(`LocalBackend::execute`'s strand task, `RemoteServer::dispatchExecute`'s strand
task) unconditionally invoke the pair, so a host wanting real distributed tracing
plugs an OpenTelemetry/Jaeger exporter in behind this hook — morph supplies the
correlation id and call sites, not the exporter, sampler, or propagation format. A
sink is always invoked outside internal locks and wrapped in `catch (...)` — "a
throwing sink is silently ignored," per the spec's stated policy that observability
must never change program behavior. Health/readiness is a small adjacent piece on
`RemoteServer` (`HealthStatus{ready, liveModels, inFlight}`, one-way flip via
`beginShutdown()`), not part of `morph::observe` itself.

### 3.5 Dynamic config

userver's `dynamic_config::Source`/`Snapshot`
(see the [dynamic config doc](https://userver.tech/d5/d46/md_en_2userver_2dynamic__config.html))
let a whole fleet of service instances pick up new config values — kill-switches,
timeouts, experiment flags — without a redeploy. A config is declared as a typed
`dynamic_config::Key` (name, JSON parser, default) and read via
`source.GetSnapshot()[kMyConfig]`; `components::DynamicConfigClient` polls a
reference config service (e.g.
[uservice-dynconf](https://github.com/userver-framework/uservice-dynconf)) on an
interval and atomically swaps in new values fleet-wide, with a filesystem fallback
cache if the very first fetch fails.

morph has nothing like this, and it is a plain absence rather than a scope
substitute worth stretching for: every `*Config` type in morph
(`ReconnectCoordinatorConfig`, `NetworkMonitorConfig`, `QtWebSocketServerConfig`,
`QtWebSocketBackendConfig`, `SocketBackendConfig`/`SocketServerConfig`, Lightweight's
`PoolConfig`) is a plain aggregate consumed once at construction time; several are
deliberately declared outside their owning class specifically so `Config cfg =
Config{}` works as a constructor default argument. Changing a value means
reconstructing the object — there is no file-watching, no reload signal, no
config-service client, and no general "app config" subsystem at all (that's left
entirely to the host application).

### 3.6 Testing tooling

userver's ["testsuite"](https://userver.tech/df/d07/md_en_2userver_2functional__testing.html)
is a pytest-based integration harness: it starts the real service binary against a
minimal real DB with externals mocked, then drives it over HTTP via a
`service_client` fixture. Companion fixtures: `monitor_client` (metrics
introspection), `mockserver` (mocks outbound HTTP dependencies by running its own
server), `mocked_time`, and per-backend plugins
(`pytest_userver.plugins.postgresql`, `.mongo`, `.redis`, `.clickhouse`, `.kafka`,
`.grpc`, `.mysql`, `.ydb`). `TESTPOINT()` macros let C++ code call back into Python
test logic at specific points. For unit/microbenchmarks, `UTEST`/`UTEST_F`/`UTEST_P`
and `UBENCH` are coroutine-aware gtest/gbench replacements (`UTEST_MT` for
multi-threaded torture tests) — see
[testing](https://userver.tech/d4/d70/md_en_2userver_2testing.html).

morph's testkit (`examples/common/testkit/`) is a set of in-process C++ fixtures, not
an external pytest harness, built around three ideas evident across the headers:
real I/O over mocks wherever practical, one fixture body exercised across multiple
deployment topologies, and careful attention to teardown ordering. `BackendRig`
(`backend_rig.hpp`) is the central fixture: a `Mode` enum (`Local`,
`LocalSingleThread`, `Socket`) lets the *same* test body run against an in-process
`LocalBackend`, a single-threaded Qt-driven executor (WASM-constraint parity), or a
real loopback `RemoteServer` + `QtWebSocketServer`. `DbFixture` uses a real
SQLite-via-ODBC database, dropping/reapplying migrations per test case;
`DbBusyFixture` holds a genuine uncommitted `BEGIN IMMEDIATE` transaction open to
force a real `SQLITE_BUSY` rather than mocking one. `FaultProxy` is an in-process
WebSocket relay with scriptable per-`callId` fault rules (`dropReply`, `delayReply`,
`duplicateReply`, `killAfter`). `OfflineRig` drives genuine
connect→disconnect→reconnect cycles against a real `QtWebSocketServer` instead of
hand-cranking a signal. `ClientPool<Presenter>` scaffolds multi-client convergence
tests. This is a fundamentally different shape from userver's testsuite (in-process
C++ fixtures vs. an external Python-driven process harness) but shares the same
instinct: prefer real behavior (real sockets, real DB locks, real reconnect cycles)
over simulated mocks.

### 3.7 Other structurally notable pieces

userver bundles three more subsystems worth naming: a **caching framework**
(`cache::CacheUpdateTrait`/`components::CachingComponentBase`, full vs. incremental
update modes, cache dumps to survive a failed first update — see
[caches](https://userver.tech/docs/v1.0/d5/d2d/md_en_2userver_2caches.html)); **
distributed locking** (`dist_lock::DistLockedTask` over Postgres/Mongo/YDB backends,
with watchdog protection against "brain split" — see
[periodics/dist-lock](https://userver.tech/d7/dc4/md_en_2userver_2periodics.html));
and **periodic tasks** (`utils::PeriodicTask`, running user code on *every* machine
in the cluster on a configurable, runtime-mutable interval). Component lifecycle
(`components::ComponentBase::OnAllComponentsLoaded()` /
`OnAllComponentsAreStopping()`) gives graceful shutdown a standard hook for draining
in-flight requests.

morph has none of these as general facilities. There's no caching framework — every
"cache" hit in `include/morph/` is either an unrelated code comment or a
function-local `static const std::string` memoizing a schema string, not a TTL/
eviction/cache-aside abstraction. There's no distributed locking exposed to
applications (Lightweight has an internal `SqlAdvisoryLock` used only by its own
migration runner, not a public coordination primitive). There's no general periodic-
task facility — the only timer-driven internals are narrowly special-purpose:
`morph::async::detail::TimeoutScheduler` (single-shot timeouts for
`LimitPolicy::executeTimeout`/`Bridge::setExecuteDeadline`) and `NetworkMonitor`'s
fixed-interval connectivity probe. Graceful shutdown exists only as
`RemoteServer::beginShutdown()`, a one-way flip of `HealthStatus.ready` for
drain-before-restart — much narrower than userver's component-lifecycle hooks, but
proportionate to morph not having a fleet of interdependent components to sequence.

## 4. Ideas worth a closer look

- **Deadline propagation as a first-class concept** (`engine::Deadline`,
  `server::request::TaskInheritedData`, auto-cancellation of downstream HTTP/DB
  calls). morph already has a per-call `executeTimeout`/`setExecuteDeadline`
  (`LimitPolicy`), but userver's version threads one deadline through an entire
  call tree, including into DB drivers. Worth revisiting only if morph's actions
  start fanning out into multiple downstream calls per request — for a single
  `Model::execute` call, the current per-call timeout is probably sufficient.

- **The default HTTP middleware ordering as a checklist.** userver's fixed default
  chain (`Tracing → RateLimit → DeadlinePropagation → Auth → …`) is a good template
  for *reasoning about* morph's own single-choke-point `IAuthorizer`, even without
  adopting a pipeline: it names concerns (rate limiting, decompression) that
  `RemoteServer` doesn't currently enumerate. Caveat: morph's one-hook model is
  simpler and matches its transport being one typed protocol, not arbitrary HTTP —
  a full middleware *pipeline* would be over-engineering for that scope.

- **Cache dumps (survive a failed first cache load from a persisted snapshot).**
  Not directly applicable since morph has no caching framework, but the underlying
  idea — persist-last-known-good-state so a cold start with a broken dependency
  still boots — rhymes with `SqliteOfflineQueue`'s durability goal. Worth
  revisiting only if morph grows an in-memory read-cache layer in front of
  Lightweight; not worth building preemptively.

- **`utils::PeriodicTask`'s runtime-mutable interval (`SetSettings()`).** morph's
  only comparable timers (`TimeoutScheduler`, `NetworkMonitor`'s probe loop) are
  fixed at construction. If morph ever grows a general periodic-task facility (it
  doesn't currently need one), making the interval adjustable at runtime without
  reconstructing the object is a small, cheap idea to borrow.

- **Testsuite's `mockserver` + real-process integration model.** morph's testkit
  already favors real I/O over mocks in-process (`FaultProxy`, `OfflineRig`,
  `DbBusyFixture`); userver's testsuite pushes that further by running the actual
  compiled service binary and mocking only its external dependencies. Worth
  considering only if morph examples grow complex enough that in-process
  `BackendRig` fixtures stop being representative of real deployment — a real
  scope change, not a small addition.

- **Structured statistics via `utils::statistics::Writer`'s labeled multi-metric
  writer.** `morph::observe`'s `Metric` enum is closed and small (8 values) by
  design. If a host needs many more application-specific metrics, userver's
  pattern of a writer object with hierarchical/labeled paths is a reasonable model
  for a *host-side* metrics sink built on top of `MetricSink` — this doesn't
  require any change to morph itself, since `MetricEvent::tags` already carries
  labels.

## 5. Explicitly not comparable

- **Coroutine engine vs. no coroutine engine.** userver's `engine::TaskProcessor`
  is a from-scratch M:N stackful-coroutine scheduler over epoll — a huge,
  load-bearing piece of infrastructure that exists because userver *is* the
  runtime a service is written against. morph deliberately has no equivalent: it
  assumes the host already has a threading/event-loop story (Qt, a thread pool, a
  WASM main thread) and only adds `IExecutor`/`StrandExecutor` on top. This isn't
  a gap — building a coroutine runtime would be a different, much larger project
  outside morph's stated scope.

- **Native async DB wire drivers vs. an ODBC-based ORM.** userver ships
  hand-written async protocol implementations for each DB it supports, integrated
  with its own scheduler. morph uses a third-party ODBC-based ORM (Lightweight)
  that is DB-agnostic by design (SQLite/MSSQL/Postgres through one formatter).
  These solve different problems: userver optimizes for high-throughput,
  non-blocking access to a fixed set of DB engines from inside its own coroutine
  runtime; morph optimizes for "any ODBC-reachable store, simple synchronous calls
  inside a strand." Neither is a strictly better design in isolation — they follow
  from the different concurrency models in §3.1.

- **HTTP/gRPC server vs. typed-action Bridge.** gRPC is a general-purpose RPC
  transport with protobuf schemas, streaming shapes, and interoperability with any
  gRPC client in any language. morph's `Bridge` is a typed, C++-native
  action/model dispatch mechanism over its own wire protocol, meant for a morph
  client talking to a morph server (or a WASM/desktop client talking to a morph
  backend) — not a general RPC transport for arbitrary polyglot clients. Comparing
  "does morph have gRPC" is a domain-mismatch question, not a missing-feature one.

- **Dynamic config fleet system vs. none.** This assumes a fleet of long-lived,
  independently-deployed service instances that need centrally-controlled runtime
  behavior changes — a scenario morph's client-server-actions model doesn't
  currently occupy. Not comparable until (if ever) morph grows a long-lived
  multi-instance server deployment story of its own.

- **Cluster-wide distributed locking / cluster-wide periodic tasks.** Both assume
  multiple cooperating server processes coordinating over a shared DB or
  coordination service. morph's `RemoteServer` is designed around a single server
  process (with in-process strand-per-instance serialization providing the only
  concurrency control morph itself offers); there is no notion of multiple
  `RemoteServer` processes needing to agree on anything, so distributed
  locking/leader-election has no problem to solve in morph's current scope.

# Server push (spec 8) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Actions declare once what they change. Every reply and every server event updates the client cache from
those declarations. Documents stop declaring refreshes, and an agent's calls through the server update every open
GUI. All of it lands in PR #990.

**Architecture:** Two core registries (records, effects) are matched lazily on first read and give each action its
`effects` and its `records` with key fields and paths. `RemoteServer` gains per-connection writers, a watch table
keyed by connection, and fan-out in the execute path. The UI interpreter's cache applies a change the same way
whether it came from a reply (`actionSucceeded`) or from an `event`. The loader checks documents against effects,
and `ui/2` removes the refresh keys. A transport-free MCP adapter drives the server through an ordinary connection
scope.

**Tech Stack:** C++23, glaze (JSON and reflection), Catch2 v3, CMake + Ninja; `morph::net` (POSIX) and Qt6
WebSockets transports.

**Spec:** `docs/superpowers/specs/2026-10-16-server-push-design.md` (section numbers below, such as §7, are the
spec's).

## Global Constraints

- Every part keeps the branch building, with `morph_tests`, `morph_net_tests`, `morph_qt_tests` and the three
  rungs' unit and GUI tests green. Commit at the end of each task, never with a red suite.
- Every new public symbol has complete Doxygen (`@param`/`@tparam`/`@return`). The Docs workflow runs with
  `WARN_AS_ERROR = FAIL_ON_WARNINGS`.
- Comments and `docs/` state what the code does now and why: no history, no issue numbers, no commit hashes
  (AGENTS.md).
- Before changing a public type or subsystem, read its `docs/spec/` page, and update that page in the same task.
- Numbers from the spec, verbatim: backlog hold-back above **1 MiB** (`1 << 20`), drain below **256 KiB**
  (`256 << 10`); coalescing window **50 ms** or **256 keys**; watch lifetime follows the cache's `keepUnused`
  (**5 minutes**); MCP unknown method is JSON-RPC error **`-32601`**.
- Wire names, verbatim: envelope kind `event` (`callId: 0`, body `{topic, payload}`), topics `changes` and
  `resync`; core kinds `watch` and `unwatch`, body `{records, instances: [{modelType, primary}], replace?}`;
  `ui-hello` gains `events`; `ActionInfo` gains `effects: [{kind, record?}]` with kind
  `updates | changes | changesInstance | none`, and `records: [{record, keyField?, paths}]`.
- `MORPH_CLIENT_ONLY` builds compile out every new registrar, as they do the action registrars.
- `ActionTraits<A>` is not changed. No new specialisation of a trait that `BRIDGE_REGISTER_ACTION` defines.
- Do not run clang-tidy locally (it is too slow here); fix its findings from CI's clang-tidy-diff log.
- Run one local build at a time; Ninja already uses every core.

## Review Focus

1. **A result that holds a record inside `std::optional` or a `std::map` value**, such as lims's optional nested
   views. The walker must give a path (`header.rows`), and a reply patch must find rows there. Pinned in Task 2
   (`walker_paths_through_optional_and_map`) and Task 19 (`reply_patch_at_nested_path`).
2. **Sign-out, or a token that expires, while watches are live.** No event may reach the connection under the old
   principal. Pinned in Task 15 (`unauthenticated_envelope_drops_watches`) and Task 16
   (`expired_watch_dropped_at_send`).
3. **A screen unmounted while its entry's fetch is in flight and an event arrives.** The entry is kept (unused),
   marked stale, and not refetched until something mounts it again. Pinned in Task 20
   (`event_on_unmounted_entry_marks_only`).
4. **A slow reader during a write burst.** Replies keep flowing, events stop, and exactly one `resync` follows the
   drain. Pinned in Task 7 (`backlog_holds_events_not_replies`) and Task 17 (`burst_over_backlog_yields_one_resync`).
5. **Two clients whose documents both `watch` a query and run a mutation with effects.** The loader warns, and the
   example rungs contain no such watch. Pinned in Task 23 (`watch_feedback_warns`) and Task 25 (no warnings
   from loading each rung's bundle).

## Build and run

```bash
cmake -S . -B build/push -G Ninja -DCMAKE_BUILD_TYPE=Debug -DMORPH_BUILD_TESTS=ON -DMORPH_BUILD_NET=ON \
      -DMORPH_BUILD_QT=ON -DMORPH_BUILD_LADDER=ON -DMORPH_BUILD_BANK_EXAMPLE=ON
cmake --build build/push
build/push/tests/morph_tests "[push]"          # this feature's core and ui cases
build/push/tests/net/morph_net_tests "[push]"
ctest --test-dir build/push --output-on-failure  # whole suite, at the end of each part
```

Every new Catch2 case is tagged `[push]` plus its part's tag: `[push][records]`, `[push][channel]`,
`[push][catalog]`, `[push][watch]`, `[push][cache]`, `[push][documents]`, `[push][explorer]`, `[push][e2e]`,
`[push][ui2]`, `[push][agent]`. New `tests/*.cpp` files are added to `morph_tests` in `tests/CMakeLists.txt`, and
`tests/net/*.cpp` files to `morph_net_tests`.

## File structure

| File | Responsibility |
|---|---|
| `include/morph/core/records.hpp` (new) | `BRIDGE_REGISTER_RECORD`, `morph::record::{key, keyless, derivedFrom, privateKeys}`, the record registry |
| `include/morph/core/detail/result_walker.hpp` (new) | Reflection walk of `ActionTraits<A>::Result` into `{type, path}` slots |
| `include/morph/core/effects.hpp` (new) | `BRIDGE_REGISTER_EFFECTS`, `morph::effects::*`, the effects registry, lazy matching, `effectsOf`, `recordsOf`, `EffectsRegistrationError` |
| `include/morph/core/changes.hpp` (new) | `Changes` and `deriveChanges`, from an action's result JSON |
| `include/morph/core/wire.hpp` | `event` kind, `ChangesBody`, `WatchBody`, `WatchReply` |
| `include/morph/core/remote.hpp` | `ConnectionFacts`, `ConnectionWriter`, `setConnectionWriter`, `post`, `watch`/`unwatch`, fan-out, principal baseline |
| `include/morph/core/detail/watch_table.hpp` (new) | Per-connection watches with session contexts; matching a change to recipients |
| `include/morph/core/detail/change_coalescer.hpp` (new) | Per-connection 50 ms / 256-key merge |
| `include/morph/core/backend.hpp` | `IBackend::setEventHandler`, `LocalBackend::deliverEvent` |
| `include/morph/net/detail/ws_connection.hpp`, `include/morph/net/socket_server.hpp`, `include/morph/net/socket_backend.hpp` | byte counter, drain callback, event routing, `setConnectHandler` |
| `include/morph/qt/qt_websocket_server.hpp`, `src/qt/qt_websocket_server.cpp`, `src/qt/qt_websocket_backend.cpp` | the same for Qt, plus `sendTimeout` |
| `include/morph/ui/document.hpp` | `ActionInfo::effects`, `ActionInfo::records`, `Document::warnings`, the four checks, `ui/2` |
| `include/morph/ui/bundle.hpp`, `include/morph/ui/service.hpp` | `events` in `ui-hello`; the UI service sets `ConnectionFacts::events` |
| `include/morph/ui/author.hpp` | `CatalogFacts::action` fills effects and records; emits `ui/2` |
| `include/morph/ui/interpreter.hpp` | `refetchAfter`, effects on replies, events, topic counting, `watcher`, `reconnected()` |
| `include/morph/ui/service_client.hpp` | `watcherOver` |
| `include/morph/ui/frontend.hpp` | `AppSource::onConnected` |
| `include/morph/ui/explorer.hpp` | effects and records rows |
| `include/morph/ui/agent.hpp` (new) | `McpAdapter`, `serveStdio` |
| `examples/common/app/app_source.*`, `client_shell.*` | wiring of the seams |
| `examples/{bookmarks,bank,lims}` | records, effects, documents without refresh declarations |

---

## Part 1: Records and effects (core)

### Task 1: The record registry

**Files:**
- Create: `include/morph/core/records.hpp`
- Test: `tests/test_push_records.cpp`

**Interfaces:**
- Produces:
  - `namespace morph::record`: `template <auto Member> struct key {};` `struct keyless {};`
    `template <typename... S> struct derivedFrom {};` `struct privateKeys {};`
  - `struct morph::record::RecordInfo { std::string name; std::type_index type; std::optional<std::string> keyField; std::vector<std::type_index> derivedFrom; bool privateKeys = false; bool keyIsString = false; };`
  - `morph::record::detail::registerRecordOnce<T, Opts...>(std::string_view name) noexcept -> bool`, asserting the
    registration latch like `registerActionOnce`.
  - `morph::record::detail::recordRegistry() -> std::vector<RecordInfo> const&`, raw and unvalidated (Task 3
    validates it).
  - `BRIDGE_REGISTER_RECORD(T, NAME, ...)`, empty under `MORPH_CLIENT_ONLY`.

- [ ] **Step 1: Write the failing tests** (`[push][records]`)
  - `record_default_key_is_id`: register `struct Row { std::int64_t id; std::string name; }` as `"T_Row"`. Its
    `RecordInfo` has `keyField == "id"`.
  - `record_named_key_member`: `key<&Row2::code>` gives `keyField == "code"`, the glaze JSON key.
  - `record_wrapped_key`: a member of a strong-id type that satisfies `WrappedModelKey` is accepted, and
    `keyIsString` follows its raw type.
  - `record_keyless_and_derived`: `keyless, derivedFrom<Row>` gives no `keyField` and `derivedFrom == {typeid(Row)}`.
  - `record_private_keys`: the flag is set.
- [ ] **Step 2: Run** `build/push/tests/morph_tests "[records]"`. Expected: compile failure (header missing).
- [ ] **Step 3: Implement** `records.hpp`. The default key is the member reflected under the name `id`
  (`glz::reflect<T>::keys`). A type with no `id`, no `key<>` and no `keyless` is recorded with no key field. Task 3
  rejects that only when `updates<T>` names it. Static registrars follow `MORPH_DETAIL_REGISTER_ACTION_LOCAL`.
- [ ] **Step 4: Run** the tests. Expected: PASS.
- [ ] **Step 5: Commit** `core: a registry of record types, with keys, derivation and private keys`

### Task 2: The result walker

**Files:**
- Create: `include/morph/core/detail/result_walker.hpp`
- Modify: `include/morph/core/registry.hpp` (`BRIDGE_DETAIL_ACTION_REGISTRARS` gains a third registrar)
- Test: `tests/test_push_records.cpp`

**Interfaces:**
- Produces:
  - `struct morph::model::detail::ResultSlot { std::type_index type; std::string path; };`
  - `template <typename R> std::vector<ResultSlot> walkResult();`. Every visited aggregate is a slot, so matching can
    later stop at registered records. Walking stops at a type already on the current path.
  - `morph::model::detail::registerActionResultOnce<A>(std::string_view actionId) noexcept -> bool` stores
    `walkResult<ActionTraits<A>::Result>()` by action id. `morph::model::detail::actionResultSlots()` gives the
    map.

- [ ] **Step 1: Write the failing tests**
  - `walker_result_itself`: for `Row`, a slot `{Row, ""}`.
  - `walker_vector_member`: for `struct ListResult { std::vector<Row> rows; }`, a slot `{Row, "rows"}`.
  - `walker_paths_through_optional_and_map`: `std::optional<Header> header` whose `Header` holds `std::vector<Row>
    rows` gives `{Row, "header.rows"}`. A `std::map<std::string, Row>` member gives `{Row, "byName"}`.
  - `walker_skips_custom_codec`: a type with `glz::meta` `custom_read` gives no slot beneath it.
  - `walker_ends_on_recursive_type`: `struct Node { std::vector<Node> children; }` terminates.
  - `walker_renamed_member_uses_wire_name`: a `glz::meta` rename gives the renamed path.
  - `action_registration_records_result_slots`: a test action registered with `BRIDGE_REGISTER_ACTION` has its
    slots in `actionResultSlots()` under its id. *Mutation: remove the registrar line; this case fails.*
- [ ] **Step 2: Run** `"[records]"`. Expected: FAIL.
- [ ] **Step 3: Implement** with `glz::reflect` over reflectable aggregates. Paths join with `.`; vector, optional
  and map values add no segment. Add the registrar to `BRIDGE_DETAIL_ACTION_REGISTRARS` through a new
  `MORPH_DETAIL_REGISTER_RESULT_LOCAL(A)`, which is empty under `MORPH_CLIENT_ONLY`.
- [ ] **Step 4: Run** `"[records]"`, then the whole `morph_tests`, because every action registration now runs the
  walker. Expected: PASS.
- [ ] **Step 5: Commit** `core: an action's registration records where its result holds which types`

### Task 3: Effects, matching and validation

**Files:**
- Create: `include/morph/core/effects.hpp`
- Modify: `docs/spec/core/registry.md` (new sections "Records" and "Effects")
- Create: `tests/compile_checks/effects_unseen_a.cpp`, `tests/compile_checks/effects_unseen_b.cpp` (one TU sees
  `BRIDGE_REGISTER_EFFECTS`, the other does not; linked into one executable `morph_effects_unseen_links`)
- Test: `tests/test_push_effects.cpp`

**Interfaces:**
- Consumes: Tasks 1 and 2.
- Produces:
  - `namespace morph::effects`: `template <typename R> struct updates {};` `template <typename R> struct changes {};`
    `struct changesInstance {};` `struct none {};`
  - `enum class morph::effects::Kind : std::uint8_t { Updates, Changes, ChangesInstance, None };`
  - `struct morph::effects::Effect { Kind kind; std::string record; };` (`record` is empty for the instance kinds)
  - `struct morph::effects::ActionRecord { std::string record; std::optional<std::string> keyField; std::vector<std::string> paths; };`
  - `[[nodiscard]] std::optional<std::vector<Effect>> morph::effects::effectsOf(std::string_view actionId);`
    (`nullopt` means nothing is declared: the action reads)
  - `[[nodiscard]] std::vector<ActionRecord> morph::effects::recordsOf(std::string_view actionId);`
  - `[[nodiscard]] morph::record::RecordInfo const* morph::effects::recordNamed(std::string_view name);`
  - `struct morph::model::EffectsRegistrationError : std::logic_error`
  - `BRIDGE_REGISTER_EFFECTS(A, ...)`

- [ ] **Step 1: Write the failing tests**
  - `records_of_list_action`: an action returning `ListResult` gives
    `recordsOf == {{"T_Row", "id", {"rows"}}}`.
  - `registration_order_irrelevant`: records and effects registered in two TUs, with the record's TU after the
    action's (`tests/push_records_late.cpp` linked into `morph_tests`), give the same `recordsOf`.
  - `effects_twice_same_is_noop` and `effects_conflict_throws`: the conflict throws `EffectsRegistrationError` on
    the first `effectsOf`, and its `what()` names the action.
  - `updates_without_record_in_result_throws`, `updates_on_keyless_throws`, `derived_cycle_throws`,
    `duplicate_record_name_throws`: each throws, and one registry holding all four lists all four in `what()`.
  - `unregistered_action_reads`: `effectsOf("NoSuch")` is `nullopt`.
  - Validation tests run against a registry built in the test (`detail::resolveForTesting(records, slots, effects)`),
    so the process registry stays valid for the rest of the suite.
- [ ] **Step 2: Run** `"[push][records]"`. Expected: FAIL.
- [ ] **Step 3: Implement.** `effectsOf`, `recordsOf` and `recordNamed` call `detail::resolved()`, which uses
  `std::call_once` and calls `noteRegistryRead(true)`. It matches every action's slots against registered records,
  stopping at the first registered record on each path, and caches the result or the error. The error is rethrown
  on every later read. Registrars store `{kind, std::type_index}` and are idempotent per action, as
  `registerActionOnce` is.
- [ ] **Step 4: Run** the tests and build `morph_effects_unseen_links`. Expected: PASS, links.
- [ ] **Step 5: Commit** `core: actions declare their effects; matched and validated on first read`

### Task 4: Deriving changes from a result

**Files:**
- Create: `include/morph/core/changes.hpp`
- Test: `tests/test_push_effects.cpp`

**Interfaces:**
- Consumes: Task 3.
- Produces:
  - `struct morph::effects::RecordChange { std::string record; std::optional<std::vector<glz::raw_json>> keys; };`
    (`keys` absent means the whole type changed)
  - `struct morph::effects::InstanceChange { std::string modelType; std::uint64_t instance = 0; std::string primary; };`
    (`primary` is the directory key's text for a keyed shared instance, empty for a private one)
  - `struct morph::effects::Changes { std::vector<RecordChange> records; std::vector<InstanceChange> instances; [[nodiscard]] bool empty() const; };`
  - `[[nodiscard]] Changes morph::effects::deriveChanges(std::string_view actionId, std::string_view resultJson, InstanceChange const& ranOn);`

- [ ] **Step 1: Write the failing tests**
  - `derive_updates_reads_keys_at_paths`: `updates<Row>` over `{"rows":[{"id":1},{"id":2}]}` gives
    `{"T_Row", [1, 2]}`.
  - `derive_changes_has_no_keys`.
  - `derive_expands_derived_from`: `changes<TxnLike>` also yields the keyless `{"T_Report"}` derived from it.
  - `derive_changes_instance_uses_ran_on`.
  - `derive_reads_nothing_for_reader`: an undeclared action gives `empty()`.
  - *Mutation: return no keys from `updates`; the first case fails.*
- [ ] **Step 2: Run** `"[push][records]"`. Expected: FAIL.
- [ ] **Step 3: Implement** by parsing `resultJson` with `glz::generic` and walking each `ActionRecord` path; the
  key value is copied as raw JSON.
- [ ] **Step 4: Run.** Expected: PASS.
- [ ] **Step 5: Commit** `core: derive an execute's changes from its effects and its result`

---

## Part 2: The server-to-client channel

### Task 5: The `event` kind and client-side routing

**Files:**
- Modify: `include/morph/core/wire.hpp`, `include/morph/core/backend.hpp`, `include/morph/core/detail/reply_router.hpp`
- Modify: `docs/spec/core/wire.md`
- Test: `tests/test_push_channel.cpp`

**Interfaces:**
- Produces:
  - `inline constexpr std::string_view morph::wire::kEventKind = "event";`
  - `[[nodiscard]] Envelope morph::wire::makeEvent(std::string_view topic, std::string payloadJson);`
    (`callId` 0, body `{"topic":…, "payload":…}`)
  - `struct morph::wire::EventBody { std::string topic; glz::raw_json payload; };`
  - `virtual void IBackend::setEventHandler(std::function<void(wire::EventBody)> handler, exec::IExecutor* exec);`
    (default: ignore). Delivered on @p exec.
  - `void LocalBackend::deliverEvent(wire::EventBody event);` (test seam)
- [ ] **Step 1: Write the failing tests** (`[push][channel]`)
  - `reply_router_routes_event_before_call_match`: a router with a pending `callId 0`, given an event envelope,
    calls the event handler and leaves the pending call unresolved. *Mutation: route events through reply
    matching; this case fails.*
  - `local_backend_deliver_event_reaches_handler_on_exec`.
- [ ] **Step 2: Run** `"[channel]"`. Expected: FAIL.
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run.** Expected: PASS.
- [ ] **Step 5: Commit** `core: a server-initiated event kind, routed before reply matching`

### Task 6: `RemoteServer` writers, `post` and the backlog

**Files:**
- Modify: `include/morph/core/remote.hpp`, `docs/spec/core/backend.md`, `docs/spec/security.md`
- Test: `tests/test_push_channel.cpp`

**Interfaces:**
- Produces:
  - `struct morph::backend::ConnectionWriter { std::function<void(std::string)> write; std::function<std::size_t()> backlog; };`
  - `void RemoteServer::setConnectionWriter(ConnectionId cid, ConnectionWriter writer);` and
    `void RemoteServer::connectionDrained(ConnectionId cid);` (what the transport's `onDrained` calls; it posts to
    the strand)
  - `void RemoteServer::post(ConnectionId cid, std::string envelope);` (any thread; above the size cap it is
    dropped and counted)
  - `inline constexpr std::size_t kEventBacklogHigh = std::size_t{1} << 20;`
    `inline constexpr std::size_t kEventBacklogLow = std::size_t{256} << 10;`
  - The writer is dropped in `closeConnectionHere`. A throwing `write` drops the connection's watches (Part 4 adds
    those).
- [ ] **Step 1: Write the failing tests**
  - `post_reaches_connection_writer`.
  - `backlog_holds_events_not_replies`: a writer whose `backlog()` reads 2 MiB receives replies to executes and no
    posted event. After `connectionDrained`, it receives exactly one `resync` event, with nothing else sent in
    between.
  - `post_after_close_is_dropped`.
- [ ] **Step 2: Run** `"[channel]"`. Expected: FAIL.
- [ ] **Step 3: Implement.** `post` hops to the server strand, checks `backlog() > kEventBacklogHigh`, and either
  writes or sets `owesResync`. `connectionDrained` sends `makeEvent("resync", "{}")` when `owesResync` is set.
- [ ] **Step 4: Run.** Expected: PASS.
- [ ] **Step 5: Commit** `core: RemoteServer writes events to a connection, and holds them back from a slow reader`

### Task 7: The `net` transport

**Files:**
- Modify: `include/morph/net/detail/ws_connection.hpp`, `include/morph/net/socket_server.hpp`, `include/morph/net/socket_backend.hpp`
- Test: `tests/net/test_push_net.cpp`

**Interfaces:**
- Produces:
  - `LoopConnection` gains `std::atomic<std::size_t> pending` and `std::function<void()> onDrained`. The amount
    added is the size of the encoded envelope. The writer subtracts the same amount after the frame's last chunk is
    written. `onDrained` fires when `pending` falls below `kEventBacklogLow` after having been at or above it.
  - `SocketServer`'s `accepted` calls `server->setConnectionWriter(cid, …)`, and wires `onDrained` to
    `server->connectionDrained(cid)`.
  - `SocketBackend::setConnectHandler` override, fired in `onConnected()` after held requests are flushed and before
    the reconnect handler is posted, the first connect included. `setEventHandler` override.
- [ ] **Step 1: Write the failing tests** (`[push][channel]`)
  - `net_posted_event_reaches_backend_handler`.
  - `net_connect_handler_fires_on_first_and_reconnect`: the server is restarted on the same port, and the handler
    has fired twice.
  - `net_counter_returns_to_zero`: after 100 replies are written, `pending == 0`.
- [ ] **Step 2: Run** `build/push/tests/net/morph_net_tests "[push]"`. Expected: FAIL.
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run.** Expected: PASS.
- [ ] **Step 5: Commit** `net: per-connection byte backlog, drain callback, events and the connect handler`

### Task 8: The Qt transport

**Files:**
- Modify: `include/morph/qt/qt_websocket_server.hpp`, `src/qt/qt_websocket_server.cpp`, `src/qt/qt_websocket_backend.cpp`, `include/morph/qt/qt_websocket_backend.hpp`
- Test: `tests/qt/test_qt_websocket.cpp`

**Interfaces:**
- Produces: the same counter, decremented in `QWebSocket::bytesWritten` by the envelope sizes it completes (keep
  a FIFO of sizes); `QtServerConfig::sendTimeout` (default 30 s, as `net`), closing a peer whose oldest pending
  frame is older; `QtWebSocketBackend::setEventHandler`.
- [ ] **Step 1: Write the failing tests:** `qt_posted_event_reaches_handler`, `qt_connect_handler_fires_on_reconnect`,
  `qt_stalled_peer_closed_after_send_timeout` (a 200 ms timeout and a peer that never reads).
- [ ] **Step 2: Run** `build/push/tests/qt/morph_qt_tests "[push]"`. Expected: FAIL.
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run.** Expected: PASS.
- [ ] **Step 5: Commit** `qt: per-connection backlog, events, and a send timeout like net's`

### Task 9: The simulated backend and one conformance case

**Files:**
- Modify: `include/morph/core/remote.hpp` (`SimulatedRemoteBackend`)
- Test: `tests/test_push_channel.cpp`, `tests/net/test_push_net.cpp`, `tests/qt/test_qt_websocket.cpp`
- Modify: `docs/spec/core/backend.md`

- [ ] **Step 1: Write the failing test.** `channel_conformance(make)` is a helper in `tests/push_conformance.hpp`,
  instantiated by each suite. It checks: a posted event reaches the handler; it is not taken for a reply; a
  connection's events arrive in posting order.
- [ ] **Step 2: Run** all three suites with `"[push][channel]"`. Expected: the simulated one fails.
- [ ] **Step 3: Implement** `SimulatedRemoteBackend` registering a writer for its connection and routing events.
- [ ] **Step 4: Run.** Expected: PASS on all three.
- [ ] **Step 5: Commit** `core: SimulatedRemoteBackend carries events; one conformance case on every transport`

---

## Part 3: Catalog and wire shapes

### Task 10: `ActionInfo::effects` and `records`

**Files:**
- Modify: `include/morph/ui/document.hpp`, `include/morph/ui/author.hpp`, `include/morph/ui/bundle.hpp` (glz meta)
- Modify: `docs/spec/ui/document.md`, `docs/spec/ui/service.md`
- Test: `tests/test_push_catalog.cpp`

**Interfaces:**
- Consumes: `effectsOf`, `recordsOf` (Task 3).
- Produces:
  - `struct morph::ui::ActionEffect { std::string kind; std::optional<std::string> record; };`
  - `struct morph::ui::ActionRecordInfo { std::string record; std::optional<std::string> keyField; std::vector<std::string> paths; };`
  - `ActionInfo` gains `std::optional<std::vector<ActionEffect>> effects;` (absent: reads) and
    `std::vector<ActionRecordInfo> records;`
  - `CatalogFacts::action<M, A>()` fills both.
- [ ] **Step 1: Write the failing tests** (`[push][catalog]`): `catalog_facts_carry_effects_and_records`;
  `ui_catalog_reply_round_trips_effects` (encode a `CatalogReply`, read it back, and the fields are equal);
  `old_reply_without_fields_reads_as_reads` (a JSON reply with no `effects` gives `nullopt`).
- [ ] **Step 2: Run** `"[catalog]"`. Expected: FAIL.
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run.** Expected: PASS.
- [ ] **Step 5: Commit** `ui: the catalog carries each action's effects and records`

### Task 11: `events` in `ui-hello`, and `ConnectionFacts`

**Files:**
- Modify: `include/morph/core/remote.hpp`, `include/morph/ui/bundle.hpp`, `include/morph/ui/service.hpp`, `include/morph/ui/service_client.hpp`
- Modify: `docs/spec/ui/service.md`, `docs/spec/core/backend.md` ("Envelope services")
- Test: `tests/test_push_catalog.cpp`, `tests/test_remote_envelope_service.cpp`

**Interfaces:**
- Produces:
  - `struct morph::backend::ConnectionFacts { bool events = false; };`
  - `virtual std::string IEnvelopeService::handle(wire::Envelope const&, session::IAuthorizer const&, ConnectionFacts& facts)`
    is non-pure, and its default forwards to the two-parameter `handle`.
  - `HelloRequest::events` (`bool`, default false) and `HelloReply::events`.
  - `[[nodiscard]] bool RemoteServer::eventsNegotiated(ConnectionId cid) const;` (server strand only)
  - The generic UI client's `ui-hello` offers `events: true`.
- [ ] **Step 1: Write the failing tests:** `hello_with_events_sets_connection_fact`; `hello_without_events_leaves_it_false`;
  `unscoped_message_gets_throwaway_facts`; `two_param_service_still_compiles_and_serves` (the existing test
  service, unchanged).
- [ ] **Step 2: Run** `"[catalog]"`. Expected: FAIL.
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run** the tests, then `morph_tests "[ui-service]"` to catch regressions. Expected: PASS.
- [ ] **Step 5: Commit** `core, ui: negotiate events in ui-hello; core learns it through ConnectionFacts`

### Task 12: The watch and changes bodies

**Files:**
- Modify: `include/morph/core/wire.hpp`, `docs/spec/core/wire.md`, and spec 5 §15 in its `docs/spec` page (the push
  kind is core `event`, not `ui-event`)
- Test: `tests/test_push_catalog.cpp`

**Interfaces:**
- Produces:
  - `struct morph::wire::InstanceTopic { std::string modelType; std::string primary; std::uint64_t instance = 0; };`
  - `struct morph::wire::WatchBody { std::vector<std::string> records; std::vector<InstanceTopic> instances; bool replace = false; };`
  - `struct morph::wire::WatchReply { std::vector<std::string> refusedRecords; std::vector<InstanceTopic> refusedInstances; };`
  - `struct morph::wire::ChangesBody { std::vector<effects::RecordChange> records; std::vector<InstanceTopic> instances; };`
    (the payload of a `changes` event)
  - Readers that apply the bounds of `kMaxEnvelopeBytes`: `readWatchBody(std::string_view) -> std::expected<WatchBody, std::string>`, and the same for the other two.
- [ ] **Step 1: Write the failing tests:** each body round-trips; an over-long record list is refused.
- [ ] **Step 2–4:** Run, implement, run. Expected: PASS.
- [ ] **Step 5: Commit** `core: the watch, unwatch and changes bodies`

---

## Part 4: Watching and fan-out (core)

### Task 13: The watch table and the `watch`/`unwatch` kinds

**Files:**
- Create: `include/morph/core/detail/watch_table.hpp`
- Modify: `include/morph/core/remote.hpp` (`dispatchEnvelope` branches)
- Modify: `docs/spec/core/backend.md`, `docs/spec/core/shared_instances.md`
- Test: `tests/test_push_watch.cpp`

**Interfaces:**
- Produces:
  - `struct detail::Watch { session::Context context; std::string principal; };`
  - `class detail::WatchTable { void add(ConnectionId, wire::WatchBody const&, Watch); void remove(ConnectionId, wire::WatchBody const&); void dropConnection(ConnectionId); [[nodiscard]] std::vector<ConnectionId> watchersOf(std::string_view record) const; [[nodiscard]] std::vector<ConnectionId> watchersOf(std::string_view modelType, std::string_view primary) const; Watch const* watchOf(ConnectionId, std::string_view record) const; };`
  - Admission: a record only when the principal is authorised for at least one action whose `recordsOf` holds it
    (cached per principal and record). An instance only when `authorizeInstance(ctx, modelType, {}, 0, "")` admits
    it. The reply is `ok` with a `WatchReply`.
- [ ] **Step 1: Write the failing tests** (`[push][watch]`): `watch_record_admitted`; `watch_on_unreadable_type_refused`;
  `instance_watch_stored_before_instance_exists`; `replace_supersedes`; `close_connection_drops_watches`;
  `watch_before_hello_is_stored`.
- [ ] **Step 2: Run** `"[watch]"`. Expected: FAIL.
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run.** Expected: PASS.
- [ ] **Step 5: Commit** `core: connections watch record types and keyed instances`

### Task 14: Fan-out in the execute path

**Files:**
- Modify: `include/morph/core/remote.hpp` (`finishRemote`, `RemoteRun` gains `cid` and the instance's
  `sharedKey`/`mid`)
- Test: `tests/test_push_watch.cpp`

**Interfaces:**
- Consumes: `deriveChanges` (Task 4), `WatchTable` (Task 13), `eventsNegotiated` (Task 11), `post` (Task 6).
- Produces: `void RemoteServer::fanOut(effects::Changes changes, ConnectionId caller);` on the server strand. It converts
  each `InstanceChange` to a `wire::InstanceTopic` for the event body. It converts each `InstanceChange` to a `wire::InstanceTopic` for the event body.
- [ ] **Step 1: Write the failing tests:** `two_watchers_both_receive_caller_included`; `unwatched_type_posts_nothing`;
  `unnegotiated_connection_receives_nothing`; `private_instance_change_only_to_owner`;
  `keyed_instance_created_later_reaches_watcher`; `timed_out_call_still_reaches_caller_as_event` (`executeTimeout`
  10 ms, the handler sleeps 50 ms); `failed_execute_posts_nothing`.
- [ ] **Step 2: Run.** Expected: FAIL.
- [ ] **Step 3: Implement.** In `finishRemote`, on success only: derive the changes on the model strand, before
  `complete`, and post `fanOut` to the server strand. The reply path is unchanged.
- [ ] **Step 4: Run** the tests, then `"[remote]"`. Expected: PASS.
- [ ] **Step 5: Commit** `core: a successful execute tells every watching connection what changed`

### Task 15: The principal baseline per connection

**Files:**
- Modify: `include/morph/core/remote.hpp` (`stampVerifiedPrincipal(env, cid)`; every call site passes `cid`)
- Modify: `docs/spec/security.md`
- Test: `tests/test_push_watch.cpp`

- [ ] **Step 1: Write the failing tests:** `envelope_under_other_principal_drops_watches`;
  `unauthenticated_envelope_drops_watches`; `hello_does_not_reset_baseline`.
- [ ] **Step 2–4:** Run, implement, run. Expected: PASS.
- [ ] **Step 5: Commit** `core: a connection's watches drop when its principal changes`

### Task 16: Re-authorisation and private keys at send time

**Files:**
- Modify: `include/morph/core/remote.hpp`, `include/morph/core/detail/watch_table.hpp`
- Test: `tests/test_push_watch.cpp`

- [ ] **Step 1: Write the failing tests:** `expired_watch_dropped_at_send` (an authoriser whose `authenticate` stops
  accepting the token); `lister_not_writer_still_receives`; `private_keys_stripped_for_other_principals_kept_for_caller`.
  *Mutation: skip re-authorisation; the expiry case fails.*
- [ ] **Step 2–4:** Run, implement, run. Expected: PASS.
- [ ] **Step 5: Commit** `core: fan-out re-authorises each watch by the rule that admitted it`

### Task 17: Coalescing

**Files:**
- Create: `include/morph/core/detail/change_coalescer.hpp`
- Modify: `include/morph/core/remote.hpp`
- Test: `tests/test_push_watch.cpp`

**Interfaces:**
- Produces: `class detail::ChangeCoalescer { void add(effects::Changes const&); [[nodiscard]] std::size_t keyCount() const; [[nodiscard]] wire::ChangesBody take(); };`.
  A `changes` absorbs that record's `updates` keys, and instance topics are deduplicated. The server flushes after
  50 ms, through the server's `TimeoutScheduler`, or at 256 keys.
- [ ] **Step 1: Write the failing tests:** `burst_of_100_is_one_event`; `changes_absorbs_updates_keys`;
  `256_keys_flush_without_waiting`; `burst_over_backlog_yields_one_resync`.
- [ ] **Step 2–4:** Run, implement, run. Expected: PASS.
- [ ] **Step 5: Commit** `core: coalesce a connection's changes for 50 ms or 256 keys`

---

## Part 5: The client cache and reconnect seams

### Task 18: `refetchAfter`

**Files:**
- Modify: `include/morph/ui/interpreter.hpp` (`CacheEntry`, `Session::settleReply`, `QueryRuntime::refetchCurrent`)
- Modify: `docs/spec/ui/interpreter.md`
- Test: `tests/test_push_cache.cpp`

**Interfaces:**
- Produces: `CacheEntry::refetchAfter` (`bool`); `void Session::markStale(detail::CacheEntry&)` sets
  `stale = true` and, while `pending`, `refetchAfter = true`. `settleReply` keeps the entry stale and fetches once
  more when `refetchAfter` was set.
- [ ] **Step 1: Write the failing test:** `stale_mark_during_fetch_fetches_once_more`. Use a `RecordingBackend`
  screen with one list query and a held reply: invalidate, then release the reply. The fetch counter goes from 1
  to 2, and stays at 2 after the second reply. *Mutation: restore the unconditional `stale = false`; this case
  fails.*
- [ ] **Step 2–4:** Run `"[cache]"`, implement, run. Expected: PASS.
- [ ] **Step 5: Commit** `ui: a change marked during a fetch is kept until the next fetch`

### Task 19: Replies apply their effects

**Files:**
- Modify: `include/morph/ui/interpreter.hpp` (`Session::actionSucceeded`, the mutation run, `ScreenCore::aliasSubmits`, `CacheEntry` gains `instanceToken` and a `currentId` reader)
- Test: `tests/test_push_cache.cpp`

**Interfaces:**
- Produces:
  - `enum class ReplyUse : std::uint8_t { Applied, Discarded };`
  - `void Session::actionSucceeded(std::string const& typeId, std::string const& actionId, std::string const& replyJson, ReplyUse use, std::string const& instanceToken);`
    It keeps today's `refreshOn` behaviour and adds the effects from `_options.catalog.action(actionId)->effects`.
  - `void Session::applyChanges(wire::ChangesBody const& changes, ChangeSource source);` with
    `enum class ChangeSource : std::uint8_t { Reply, Event };`. The reply path patches `updates` by key field and
    path. The event path marks only the entries whose value holds one of the keys. Both run in one
    `runtime.batch`.
- [ ] **Step 1: Write the failing tests:**
  - `own_updates_reply_patches_by_key_field_without_fetch`: the record's key field is `code`, not `id`. The row
    changes and the fetch counter stays flat.
  - `reply_patch_at_nested_path`: the path is `header.rows`.
  - `changes_reply_refetches_holding_entries`.
  - `submit_applies_effects`: a form submit runs through `aliasSubmits`.
  - `discarded_latest_reply_marks_only`: a newer reply's row is kept.
  - `patch_on_fetching_entry_refetches_once`.
  - `changes_instance_refetches_only_that_instance`.
- [ ] **Step 2–4:** Run, implement, run. Expected: PASS.
- [ ] **Step 5: Commit** `ui: every reply applies its action's effects to the cache`

### Task 20: Events and `resync`

**Files:**
- Modify: `include/morph/ui/interpreter.hpp`
- Test: `tests/test_push_cache.cpp`

**Interfaces:**
- Produces: `void Session::deliverEvent(wire::EventBody const& event);` (`changes` goes to
  `applyChanges(…, Event)`; `resync` marks every watched entry stale; anything else is ignored).
- [ ] **Step 1: Write the failing tests:** `changes_event_via_local_seam_refetches` (through
  `LocalBackend::deliverEvent`, wired to `deliverEvent`); `resync_marks_all_watched`;
  `event_on_unmounted_entry_marks_only`; `event_keys_filter_entries_without_them`.
- [ ] **Step 2–4:** Run, implement, run. Expected: PASS.
- [ ] **Step 5: Commit** `ui: the cache applies change events and resync`

### Task 21: Topic counting, the watcher, and watch before fetch

**Files:**
- Modify: `include/morph/ui/interpreter.hpp` (`SessionOptions::watcher`, `Session::reconnected`), `include/morph/ui/service_client.hpp` (`watcherOver`)
- Modify: `docs/spec/ui/interpreter.md`, `docs/spec/ui/service.md`, `docs/spec/offline/offline.md`
- Test: `tests/test_push_cache.cpp`

**Interfaces:**
- Produces:
  - `using Watcher = std::function<async::Completion<std::string>(std::string const& kind, std::string const& bodyJson)>;`
    and `SessionOptions::watcher`.
  - `[[nodiscard]] Watcher watcherOver(Exchange exchange, std::function<session::Context()> currentSession, exec::IExecutor* cbExec);`
  - `void Session::reconnected();` sends the whole set with `replace: true`, marks every entry stale, and sets
    `refetchAfter` on the pending ones.
  - Reference counts are per record name and per `(modelType, primary)`. A count leaving zero queues a `watch`, and
    one returning to zero queues an `unwatch`. The counts change on entry creation and eviction. Queued changes go
    out as one message before the batch's fetches: `Session::fetch` flushes the queue first.
- [ ] **Step 1: Write the failing tests:** `watch_sent_before_first_fetch` (the recorder's order shows `watch`, then
  `execute`); `one_watch_message_per_batch`; `unwatch_after_eviction`; `no_watcher_counts_and_sends_nothing`;
  `reconnected_sends_replace_and_marks_stale`; `fetch_filed_before_reconnected_refetches_once`.
- [ ] **Step 2–4:** Run, implement, run. Expected: PASS.
- [ ] **Step 5: Commit** `ui: the session watches what its cache holds, before it fetches`

### Task 22: `AppSource::onConnected` and the generic client

**Files:**
- Modify: `include/morph/ui/frontend.hpp`, `docs/spec/ui/frontend.md`
- Modify: `examples/common/app/app_source.{hpp,cpp}`, `examples/common/app/client_shell.cpp`
- Test: `examples/common/app/tests/test_sources.cpp`

**Interfaces:**
- Produces:
  - `virtual void AppSource::onConnected(std::function<void()> listener) {}` (empty default)
  - The remote source owns the backend's `setConnectHandler`. On a reconnect, it re-sends `ui-hello` with
    `events: true`, then calls its listeners on the owner. It installs `setEventHandler` and forwards events to the
    session. It exposes `watcher()`.
  - The shell sets `sessionOptions.watcher = source.watcher()` and calls
    `source.onConnected([&] { session.reconnected(); })`.
- [ ] **Step 1: Write the failing test:** `remote_source_rehellos_then_rewatches_on_reconnect`, over the `net`
  transport against an in-process server that is dropped and reopened. Its order is `ui-hello`, then `watch`
  (replace), then the refetches.
- [ ] **Step 2–4:** Run the app tests, implement, run. Expected: PASS.
- [ ] **Step 5: Commit** `ui, examples: the generic client re-negotiates and re-watches on every reconnect`

---

## Part 6: Document checks

### Task 23: The query-must-read rule and the three warnings

**Files:**
- Modify: `include/morph/ui/document.hpp`, `docs/spec/ui/document.md`
- Test: `tests/test_push_documents.cpp`

**Interfaces:**
- Produces:
  - `enum class LoadWarningKind : std::uint8_t { WatchFeedback, Redundant, Uncovered };`
  - `struct LoadWarning { LoadWarningKind kind; std::string path; std::string text; };`
  - `Document::warnings` (`std::vector<LoadWarning>`). The bundle loader and `ScreenRegistry` log each warning
    where they already log a refusal.
- [ ] **Step 1: Write the failing tests** (`[push][documents]`): `query_of_action_with_effects_refused`;
  `redundant_invalidates_warns`; `redundant_refresh_on_warns`; `uncovered_mutation_warns`;
  `effects_none_silences_uncovered`; `watch_feedback_warns`; `watch_source_user_does_not_warn`.
- [ ] **Step 2–4:** Run, implement, run. Expected: PASS.
- [ ] **Step 5: Commit** `ui: documents are checked against action effects`

---

## Part 7: Examples, the explorer, end to end

### Task 24: The explorer's effects and records rows

**Files:**
- Modify: `include/morph/ui/explorer.hpp` (`catalogFacts`), `docs/spec/ui/explorer.md`
- Test: `tests/test_ui_explorer.cpp`

- [ ] **Step 1: Write the failing tests** (`[push][explorer]`): `facts_show_changes_record` gives the cells
  `"effects"`, `"changes T_Row"`; `facts_show_reads_when_undeclared` gives `"effects"`, `"reads"`;
  `facts_show_records_with_key_and_path` gives `"records"`, `"T_Row by id at rows"`. The wording follows spec §5.
- [ ] **Step 2–4:** Run, implement, run. Expected: PASS.
- [ ] **Step 5: Commit** `ui: the explorer shows what an action changes and which records it returns`

### Task 25: The three rungs declare records and effects

**Files:**
- Modify: bookmarks (`examples/bookmarks/include/bookmarks/models/*.hpp`, `screens/bookmarks_screens.cpp`);
  bank (`examples/bank/include/bank/models/*.hpp`, `examples/bank/memory/include/bank/models/*.hpp`,
  `screens/bank_screens.cpp`); lims (`examples/lims/include/lims/models/*.hpp`, `screens/lims_screens.cpp`);
  the three rung READMEs, `docs/spec/testing_strategy.md`
- Test: each rung's existing unit and GUI tests, plus one test per rung, `<rung>_bundle_loads_without_warnings`

- [ ] **Step 1: Write the failing tests:** each rung's `bundle_loads_without_warnings` loads every document and
  asserts `warnings.empty()`. It fails now on `uncovered`.
- [ ] **Step 2: Run** each rung's tests. Expected: the new case fails, the rest pass.
- [ ] **Step 3: Declare records and effects** beside each registration. Bank declares them in both model sets.
  Use the spec's names: `Bank_Account`, `Bank_Transaction`, `Bank_SpendingReport` (`derivedFrom<TxnInfo>`),
  `Lims_Result`, and `none` on `OpenSample`. Then remove the 9 `invalidates`, the 5 `refreshOn` and the 1 `patch`
  that the redundant warnings name. Keep `onSuccess` commands that do something other than refresh.
- [ ] **Step 4: Run** every rung's unit and GUI tests (`ctest --test-dir build/push -L ladder`). Expected: all
  PASS, no warnings, and a `grep` for `invalidates|refreshOn|\.patch` in `examples/*/screens` finds nothing.
- [ ] **Step 5: Commit** one commit per rung: `examples(<rung>): actions declare their effects; screens drop refresh declarations`

### Task 26: The two-client harness

**Files:**
- Create: `tests/net/test_push_two_clients.cpp`
- Modify: `docs/spec/testing_strategy.md`

- [ ] **Step 1: Write the test** (`[push][e2e]`). One `RemoteServer` and `SocketServer` run with test models
  `ListRows` and `EditRow` (`changes<Row>`), and two `SocketBackend`s, each with a `ui::Session` wired through
  `watcherOver` and `setEventHandler`. Each mounts one screen with a list query and no refresh declarations.
  Client A runs the mutation, and B's fetch counter and value change within 2 s, with no `refreshEvery`.
- [ ] **Step 2: Run the control.** Comment out the `BRIDGE_REGISTER_EFFECTS` line. Expected: B does not update, and
  the test fails. Restore the line.
- [ ] **Step 3: Run.** Expected: PASS.
- [ ] **Step 4: Commit** `tests: one client's mutation updates another client's screen over net`

---

## Part 8: `ui/2`

### Task 27: Remove the refresh keys in a new vocabulary

**Files:**
- Modify: `include/morph/ui/document.hpp` (`kVocabulary` stays `"ui/1"`; add `kVocabulary2 = "ui/2"`; the parser
  accepts both and refuses `invalidates`, `patch` and `refreshOn` under `ui/2`; `optimistic` gets its own
  definition with the `{query, key, with}` shape)
- Modify: `include/morph/ui/author.hpp` (emits `ui/2`, drops `.invalidates`, `.patch` and `.refreshOn`),
  `include/morph/ui/service.hpp` (serves both vocabularies), `tests/ui/golden/*.json`
- Modify: `docs/spec/ui/document.md`, `docs/spec/ui/author.md`, spec 5 §5 and §12
- Test: `tests/test_push_ui2.cpp`, `tests/test_ui_author.cpp`

- [ ] **Step 1: Write the failing tests** (`[push][ui2]`): `ui2_refuses_invalidates`; `ui2_refuses_refresh_on`;
  `ui2_keeps_optimistic`; `ui1_document_still_loads_with_refresh_on`; `author_emits_ui2`.
- [ ] **Step 2–4:** Run, implement, regenerate the goldens with the existing golden-update path, run all
  `morph_tests`, and run the rung suites. Expected: PASS.
- [ ] **Step 5: Commit** `ui: ui/2 removes invalidates, patch and refreshOn; ui/1 documents still load`

---

## Part 9: The MCP adapter

### Task 28: `McpAdapter`

**Files:**
- Create: `include/morph/ui/agent.hpp`, `docs/spec/ui/agent.md`
- Test: `tests/test_push_agent.cpp`

**Interfaces:**
- Consumes: `RemoteServer::openConnection`, `handle(msg, reply, cid)`, `CatalogReply::permitted`, the `schemas` kind.
- Produces:
  - `struct McpOptions { session::Context session; std::string serverName = "morph"; };`
  - `class McpAdapter { public: McpAdapter(std::shared_ptr<backend::RemoteServer> server, McpOptions options); ~McpAdapter(); [[nodiscard]] async::Completion<std::optional<std::string>> handle(std::string message); };`
    The destructor closes its connection.
  - `void serveStdio(McpAdapter& adapter, std::istream& in, std::ostream& out);`
- [ ] **Step 1: Write the failing tests** (`[push][agent]`): `initialize_answers_capabilities`;
  `tools_list_is_permitted_plus_evaluate`; `tools_call_runs_and_returns_result_text`;
  `refused_call_is_error_and_posts_nothing`; `call_reaches_watching_gui_connection_as_event`;
  `unknown_method_is_32601`; `notification_gets_no_reply`; `stdio_round_trip` (over `std::stringstream`).
- [ ] **Step 2–4:** Run, implement, run. Expected: PASS.
- [ ] **Step 5: Commit** `ui: an MCP adapter runs agent tool calls through an ordinary connection`

### Task 29: Bank serves MCP on stdio

**Files:**
- Modify: `examples/bank/src/server/main.cpp`, `examples/bank/README.md`
- Test: `examples/bank/tests/` (one case: the server binary with `--mcp-stdio` answers `tools/list` on a pipe)

- [ ] **Step 1–4:** Write the test, run it (FAIL), add the option (the token comes from `MORPH_MCP_TOKEN`), run it
  (PASS).
- [ ] **Step 5: Commit** `examples(bank): --mcp-stdio serves the MCP adapter beside the WebSocket server`

---

## Task 30: Whole-branch verification and the PR

- [ ] **Step 1:** `ctest --test-dir build/push --output-on-failure`. Expected: 100% pass. Record the totals.
- [ ] **Step 2:** Build the docs with the command in AGENTS.md "CI notes". Expected: no Doxygen warnings.
- [ ] **Step 3:** Build with `-DMORPH_CLIENT_ONLY=ON` (`morph_client_only_*` targets). Expected: builds; no
  registrar is emitted.
- [ ] **Step 4:** `grep -rnE 'invalidates|refreshOn' examples --include='*.cpp' | grep -v /tests/` gives no code
  occurrence.
- [ ] **Step 5:** Push, update PR #990's title and body to describe the implementation (`/update-pr`), and drive CI
  to green, fixing clang-tidy findings from CI's log.

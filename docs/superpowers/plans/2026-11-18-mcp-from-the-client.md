# An MCP Server from the Generic Client — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or
> superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** `morph_remote_client --mcp-stdio --server URL` and `morph_examples_client --mcp-stdio --app NAME` serve MCP
over stdio, so Claude Code can install either.

**Architecture:** `ui::McpAdapter` gains a second constructor over a borrowed `backend::IEnvelopeTransport`. That
constructor brings a per-call timeout, a generation-guarded reset and a weak resetter for disconnects. Both
WebSocket backends gain `retryFirstConnect`. `morph::client` gains `makeLocalServer`, which builds the in-process
server for a `LocalApp`, and `serveMcp`, which picks the source (local, `morph::net` remote, or Qt remote), builds
the adapter and serves stdio. The client's `main` calls `serveMcp` before it selects any frontend.

**Tech Stack:** C++23, Catch2, CMake/Ninja, glaze JSON, `morph::net` (POSIX sockets), Qt 6 WebSockets.

**Spec:** `docs/superpowers/specs/2026-11-18-mcp-from-the-client-design.md`. Read it before starting any task.

## Global Constraints

- **Branch.** Commit on `feat/mcp-from-client` only: one PR at the end.
- **Comments and docs** state what the code does now and why, with no history, issue numbers or commit hashes
  (`AGENTS.md`). Every public symbol gets complete `@brief`/`@param`/`@tparam`/`@return`: Doxygen runs with
  `WARN_AS_ERROR = FAIL_ON_WARNINGS`.
- **Commits.** Use the git user (Yaraslau Tamashevich). End every commit message with:
  ```
  Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
  Claude-Session: https://claude.ai/code/session_01No1NzmvnRAYBxCaZFtzNe7
  ```
- **Builds.** Run at most one build at a time, under the shared compile lock, with `-j5`:
  `flock /tmp/morph-build.lock cmake --build build/linux-everything --target <target> -j5`. Run builds in the
  foreground, never as a background shell. One build call may take up to ~580 s, so set the tool timeout to
  600000 ms. Never `pkill -f` a worktree path.
- **Build trees.** Use `build/linux-everything`, which has `MORPH_BUILD_NET=ON` and `MORPH_BUILD_QT=ON`. Use
  `build/gcc-debug` (`MORPH_BUILD_NET=OFF`, Qt on) to prove that the Qt-only path compiles.
- **Format and lint.** Run `clang-format -i` on every touched C++ file. Do not use a `slots`, `signals` or `emit`
  identifier in `include/morph`.
- **Verify, don't assert.** Every new test must fail when the feature is mutated away. Each task names the mutation
  to try. Report it as measured.
- **Stdout.** In MCP mode, stdout carries JSON-RPC only. Diagnostics go to stderr.
- **Exit codes.** A refused start writes one line to stderr and exits with code 2.
- **Identity.** In remote mode, the principal is `--user`, else `MORPH_MCP_PRINCIPAL`, and the token is
  `MORPH_MCP_TOKEN`. In local mode, the principal is `--user` and there is no token; a set `MORPH_MCP_TOKEN` prints
  a warning on stderr.
- **Default call timeout.** `McpOptions::callTimeout` defaults to `std::chrono::seconds{30}`.

## File map

| File | Task | Responsibility |
|---|---|---|
| `include/morph/ui/agent.hpp` | 1 | Adapter link, transport constructor, timeout, generation, `resetter()`, `serveStdioOnOwner` |
| `tests/test_mcp_transport.cpp` (new), `tests/CMakeLists.txt` | 1 | The adapter over a scripted fake transport and over `SimulatedRemoteBackend` |
| `include/morph/net/socket_backend.hpp`, `tests/net/test_socket_backend.cpp` | 2 | `retryFirstConnect` for `morph::net` |
| `include/morph/qt/qt_websocket_backend.hpp`, `src/qt/qt_websocket_backend.cpp`, `tests/qt/test_qt_websocket.cpp` | 2 | `retryFirstConnect` for Qt |
| `include/morph/client/source.hpp`, `src/client/source.cpp`, `examples/common/testkit/screen_harness.hpp`, `tests/client/test_sources.cpp` | 3 | `makeLocalServer`, and the test kit moved onto it |
| `include/morph/client/args.hpp`, `src/client/args.cpp`, `tests/client/test_client_args.cpp` | 4 | `--mcp-stdio` |
| `include/morph/client/mcp.hpp` (new), `src/client/mcp.cpp` (new), `src/client/transport_net.cpp`, `src/client/transport_qt.cpp`, `include/morph/client/transport.hpp`, `src/client/CMakeLists.txt` | 4 | `serveMcp`, `mcpSession`, `checkMcpArgs`, `claimStdout`, transports for MCP |
| `tests/client/test_mcp.cpp`, `tests/client/test_mcp_remote.cpp` (new), `tests/client/CMakeLists.txt` | 4 | Local and remote `serveMcp` |
| `examples/client/main.cpp` | 4 | `--mcp-stdio` dispatch |
| `examples/client/tests/test_mcp_pipe.cpp` (new), `examples/client/CMakeLists.txt` | 5 | The binary over a pipe |
| `docs/spec/ui/agent.md`, `docs/spec/client/client.md`, `docs/spec/core/backend.md`, `examples/bank/README.md` | 5 (2 for `backend.md`) | Specs and install lines |

Tasks 1, 2 and 3 are independent and run in parallel. Task 4 needs all three. Task 5 needs Task 4.

---

### Task 1: The adapter over a transport

**Files:**
- Modify: `include/morph/ui/agent.hpp`
- Create: `tests/test_mcp_transport.cpp`
- Modify: `tests/CMakeLists.txt` (add `test_mcp_transport.cpp` after `test_push_agent.cpp`, line 32)

**Interfaces:**
- Produces (Task 4 relies on these exact names):
  - `McpOptions::callTimeout`, of type `std::chrono::milliseconds`, defaulting to `std::chrono::seconds{30}`
  - `McpAdapter(backend::IEnvelopeTransport& transport, McpOptions options)`
  - `[[nodiscard]] std::function<void()> McpAdapter::resetter() const`
  - `void serveStdioOnOwner(McpAdapter& adapter, exec::IExecutor& owner, std::istream& input, std::ostream& output)`
- Unchanged: `McpAdapter(std::shared_ptr<backend::RemoteServer>, McpOptions)`, `handle`, and
  `serveStdio(McpAdapter&, exec::MainThreadExecutor&, std::istream&, std::ostream&)`.

- [ ] **Step 1: Write the failing tests**

Create `tests/test_mcp_transport.cpp`. It reuses the notes model and screen shape of `tests/test_push_agent.cpp`.
Copy that file's model, registrations, `kNotes`, `kShelf`, `Authorizer` and `Rig`, but rename every registered id
with an `Mt_` prefix: a registered name may exist only once per test binary. Then add:

```cpp
// A transport whose replies the test releases by hand: what SimulatedRemoteBackend cannot do (hold a reply, reject,
// never answer).
class ScriptedTransport final : public morph::backend::IEnvelopeTransport {
public:
    struct Sent {
        morph::wire::Envelope envelope;
        morph::async::Completion<morph::wire::Envelope>::Promise promise;  // use the settleable pair's promise type
    };

    morph::async::Completion<morph::wire::Envelope> request(morph::wire::Envelope envelope,
                                                            morph::exec::IExecutor& cbExec) override {
        auto [completion, promise] = morph::async::Completion<morph::wire::Envelope>::makeSettleable(&cbExec);
        std::scoped_lock const lock{mutex};
        sent.push_back(Sent{.envelope = std::move(envelope), .promise = std::move(promise)});
        return std::move(completion);
    }

    std::mutex mutex;
    std::vector<Sent> sent;
};
```

Use whatever promise type `makeSettleable` returns. `include/morph/core/completion.hpp` names it: copy the `auto
[completion, promise]` form from `McpAdapter::handle` and store `decltype(promise)`.

Test cases. `ask(...)` is the `test_push_agent.cpp` helper; give it a `MainThreadExecutor&` parameter, so it pumps
the executor the adapter was built with.

```cpp
TEST_CASE("an adapter over SimulatedRemoteBackend lists tools and runs a call", "[mcp][transport]") {
    Rig rig;
    auto const cid = rig.server->openConnection();
    morph::backend::SimulatedRemoteBackend transport{*rig.server, cid};
    morph::session::Context session;
    session.principal = "agent";
    session.token = "agent";
    ui::McpAdapter adapter{transport, ui::McpOptions{.session = session, .callbacks = &rig.callbacks}};
    auto names = toolNames(json(ask(rig.callbacks, adapter, R"({"jsonrpc":"2.0","id":1,"method":"tools/list"})")));
    std::ranges::sort(names);
    CHECK(names == std::vector<std::string>{"Mt_Add", "Mt_List", "Mt_Tally", "evaluate"});
    auto const added = json(ask(rig.callbacks, adapter, std::string{kAdd}));
    CHECK_FALSE(std::get<bool>(added["result"]["isError"].data));
    rig.server->closeConnection(cid);
}

TEST_CASE("an adapter over a transport reports a refused call as a tool error", "[mcp][transport]") {
    // As above, but the principal "agent" calls Mt_Wipe, which Authorizer refuses: isError true.
}

TEST_CASE("a rejected transport request is a tool error carrying the failure", "[mcp][transport]") {
    morph::exec::MainThreadExecutor owner;
    ScriptedTransport transport;
    ui::McpAdapter adapter{transport, ui::McpOptions{.callbacks = &owner}};
    std::optional<std::optional<std::string>> replied;
    adapter.handle(R"({"jsonrpc":"2.0","id":1,"method":"tools/call","params":{"name":"Mt_List"}})")
        .thenDetached([&](std::optional<std::string> const& answer) { replied = answer; });
    owner.runFor(20ms);
    REQUIRE(transport.sent.size() == 1);  // the catalog request
    transport.sent.at(0).promise.reject(std::make_exception_ptr(morph::backend::DisconnectedError{}));
    pumpUntil(owner, [&] { return replied.has_value(); });
    // An unreadable catalog is empty, so the tool is unknown: a JSON-RPC -32602 naming it, and no crash.
    CHECK(replied->value().contains("-32602"));
}

TEST_CASE("a request with no reply times out after callTimeout", "[mcp][transport]") {
    morph::exec::MainThreadExecutor owner;
    ScriptedTransport transport;
    ui::McpAdapter adapter{transport, ui::McpOptions{.callbacks = &owner, .callTimeout = 100ms}};
    std::optional<std::optional<std::string>> replied;
    adapter.handle(R"({"jsonrpc":"2.0","id":7,"method":"tools/list"})")
        .thenDetached([&](std::optional<std::string> const& answer) { replied = answer; });
    auto const started = std::chrono::steady_clock::now();
    pumpUntil(owner, [&] { return replied.has_value(); }, 5s);
    REQUIRE(replied.has_value());
    CHECK(std::chrono::steady_clock::now() - started >= 100ms);
    // The catalog timed out, so it is empty: the list holds only `evaluate`.
    CHECK(replied->value().contains(R"("name":"evaluate")"));
    // A reply that arrives after the timeout is dropped, not delivered twice.
    transport.sent.at(0).promise.resolve(morph::wire::makeErr("late"));
    owner.runFor(50ms);
}

TEST_CASE("a register reply released after a reset is not cached", "[mcp][transport]") {
    // Drive Mt_List through ScriptedTransport by hand:
    // 1. handle(tools/call Mt_List) -> sent[0] is the catalog request; resolve it with an ok envelope whose body is
    //    a catalog reply naming Mt_List on Mt_Notes (copy one from a real `ui-catalog` reply: build it with the
    //    Rig's server through `handleInline`, so the body is genuine).
    // 2. sent[1] is the register request. Call adapter.resetter()() BEFORE resolving it, then resolve it with
    //    modelId 41. sent[2] is the execute on 41; resolve it ok.
    // 3. handle(tools/call Mt_List) again. Because the reset bumped the generation before 41 arrived, 41 was not
    //    cached: the adapter fetches the catalog again (sent[3]) and then registers again (sent[4] has kind
    //    "register").
    // CHECK(transport.sent.at(4).envelope.kind == "register");
}

TEST_CASE("a resetter called after the adapter is destroyed does nothing", "[mcp][transport]") {
    morph::exec::MainThreadExecutor owner;
    ScriptedTransport transport;
    std::function<void()> reset;
    {
        ui::McpAdapter adapter{transport, ui::McpOptions{.callbacks = &owner}};
        reset = adapter.resetter();
    }
    reset();  // must not crash; run under ASan in CI
}

TEST_CASE("serveStdioOnOwner answers lines while another thread runs the owner", "[mcp][transport]") {
    Rig rig;
    morph::exec::ThreadPoolExecutor ownerPool{1};
    morph::exec::OwnerStrand owner{ownerPool};
    // The in-process constructor with `owner` as its callbacks; serveStdioOnOwner posts each handle() to it.
    morph::session::Context session;
    session.principal = "agent";
    session.token = "agent";
    ui::McpAdapter adapter{rig.server, ui::McpOptions{.session = session, .callbacks = &owner}};
    std::istringstream input{std::string{R"({"jsonrpc":"2.0","id":1,"method":"ping"})"} + "\n\n" +
                             R"({"jsonrpc":"2.0","id":2,"method":"tools/list"})" + "\n"};
    std::ostringstream output;
    ui::serveStdioOnOwner(adapter, owner, input, output);
    auto const text = output.str();
    CHECK(std::ranges::count(text, '\n') == 2);
    CHECK(text.find(R"("id":1)") < text.find(R"("id":2)"));
}
```

Write the elided bodies in full: the refused-call case and the reset case. `pumpUntil(owner, predicate, limit =
5s)` loops `owner.runFor(10ms)` until the predicate holds or the limit passes.

- [ ] **Step 2: Run the tests to verify they fail**

Run: `flock /tmp/morph-build.lock cmake --build build/linux-everything --target morph_tests -j5`
Expected: a compile error. There is no transport constructor, no `callTimeout`, no `resetter` and no
`serveStdioOnOwner`.

- [ ] **Step 3: Implement it in `include/morph/ui/agent.hpp`**

1. Add `#include <condition_variable>` and `#include "../core/envelope_transport.hpp"` and
   `#include "../core/timeout_scheduler.hpp"`.
2. In `McpOptions`, after `callbacks`:

```cpp
    /// @brief How long an envelope sent over a transport waits for its reply before it is answered as a failure.
    ///        `serveStdio` handles one message at a time, so without this bound a request a transport holds through
    ///        an outage would block every later message. Zero waits forever. The in-process constructor ignores it,
    ///        since a `RemoteServer` always replies.
    std::chrono::milliseconds callTimeout = std::chrono::seconds{30};
```

3. Replace `Shared`'s `server` and `cid` members, and its constructor:

```cpp
        explicit Shared(McpOptions given) : options{std::move(given)} {}

        McpOptions options;
        // Sends one envelope and calls back with its reply; set by the constructor that made the adapter.
        std::function<void(std::shared_ptr<Shared> const&, wire::Envelope, EnvelopeReply)> link;
        // Closes the adapter's connection; empty over a transport, whose connection is the caller's.
        std::function<void()> close;
        // The timeout scheduler of a transport adapter, owned by the McpAdapter and null once it is gone.
        async::detail::TimeoutScheduler* timer = nullptr;
        // Bumped by every reset: a catalog or model id is cached only by a reply sent in the current generation.
        std::uint64_t generation = 0;
```

4. `send` keeps stamping `callId` and the session, then calls `self->link(self, std::move(env), std::move(done))`.
5. Add the in-process link as a static member of `Shared`:

```cpp
        static void overServer(std::shared_ptr<backend::RemoteServer> const& server, backend::ConnectionId cid,
                               wire::Envelope env, EnvelopeReply done) {
            server->handle(
                wire::encode(env),
                [done = std::move(done)](std::string const& reply) {
                    try {
                        done(wire::decode(reply));
                    } catch (std::exception const& error) {
                        done(wire::makeErr(error.what()));
                    }
                },
                cid);
        }
```

6. Add the transport link as a static member of `Shared`:

```cpp
        // The reply to one transport request, settled once: by the reply, the rejection or the timeout.
        struct Pending {
            std::atomic<bool> settled{false};
            EnvelopeReply done;
            async::detail::TimeoutScheduler::Handle timer = 0;
            void settle(wire::Envelope reply) {
                if (!settled.exchange(true)) {
                    done(std::move(reply));
                }
            }
        };

        [[nodiscard]] static std::string waited(std::chrono::milliseconds timeout) {
            auto const ms = timeout.count();
            return ms % 1000 == 0 ? std::to_string(ms / 1000) + " s" : std::to_string(ms) + " ms";
        }

        [[nodiscard]] static std::string describe(std::exception_ptr const& error) {
            try {
                std::rethrow_exception(error);
            } catch (std::exception const& thrown) {
                return thrown.what();
            } catch (...) {
                return "the transport failed";
            }
        }

        static void overTransport(std::shared_ptr<Shared> const& self, backend::IEnvelopeTransport& transport,
                                  wire::Envelope env, EnvelopeReply done) {
            auto const pending = std::make_shared<Pending>();
            pending->done = std::move(done);
            exec::IExecutor* const owner = self->options.callbacks;
            std::chrono::milliseconds const timeout = self->options.callTimeout;
            {
                std::scoped_lock const lock{self->mutex};
                if (self->timer != nullptr && timeout.count() > 0) {
                    // The timeout fires on the scheduler's loop and settles on the owner, as replies do.
                    pending->timer = self->timer->schedule(timeout, [owner, pending, timeout] {
                        owner->post([pending, timeout] {
                            pending->settle(wire::makeErr("no reply from the server in " + waited(timeout)));
                        });
                    });
                }
            }
            auto const cancel = [weak = std::weak_ptr<Shared>{self}, pending] {
                if (auto const shared = weak.lock()) {
                    std::scoped_lock const lock{shared->mutex};
                    if (shared->timer != nullptr && pending->timer != 0) {
                        shared->timer->cancel(pending->timer);
                    }
                }
            };
            auto completion = transport.request(std::move(env), *owner);
            completion
                .thenDetached([pending, cancel](wire::Envelope reply) {
                    cancel();
                    pending->settle(std::move(reply));
                })
                .onErrorDetached([pending, cancel](std::exception_ptr const& error) {
                    cancel();
                    pending->settle(wire::makeErr(describe(error)));
                });
        }
```

   If `thenDetached` takes its argument as `wire::Envelope const&` or by value, match the signature
   `completion.hpp` declares. Match `onErrorDetached`'s `std::exception_ptr` parameter form the same way.

7. Add the generation guard in `withCatalog`. Read the generation together with the cache:

```cpp
            std::optional<CatalogReply> cached;
            std::uint64_t generation = 0;
            {
                std::scoped_lock const lock{self->mutex};
                cached = self->catalog;
                generation = self->generation;
            }
```

   Then cache only in the current generation:

```cpp
                {
                    std::scoped_lock const lock{self->mutex};
                    if (self->generation == generation) {
                        self->catalog = catalog;
                    }
                }
```

   Do the same in `withInstance`: read `generation` together with the cached id, and wrap the
   `insert_or_assign` calls in `if (self->generation == generation)`. Then add to `Shared`:

```cpp
        void reset() {
            std::scoped_lock const lock{mutex};
            ++generation;
            catalog.reset();
            privateIds.clear();
            sharedIds.clear();
        }
```

8. Constructors, destructor, `resetter`:

```cpp
    McpAdapter(std::shared_ptr<backend::RemoteServer> server, McpOptions options)
        : _shared{std::make_shared<Shared>(std::move(options))} {
        if (server == nullptr || _shared->options.callbacks == nullptr) {
            throw std::invalid_argument{"McpAdapter: a server and a callbacks executor are required"};
        }
        backend::ConnectionId const cid = server->openConnection();
        _shared->link = [server, cid](std::shared_ptr<Shared> const& /*self*/, wire::Envelope env,
                                      EnvelopeReply done) {
            Shared::overServer(server, cid, std::move(env), std::move(done));
        };
        _shared->close = [server, cid] { server->closeConnection(cid); };
    }

    /// @brief An adapter whose envelopes travel over @p transport: a connection to a server in another process.
    ///
    /// The transport's connection is the agent's. The adapter opens and closes nothing, and the server releases
    /// the adapter's private instances when that connection drops. Install `resetter()` as the transport's
    /// disconnect handler, so a reconnect does not reuse released ids. Each envelope waits at most
    /// `McpOptions::callTimeout` for its reply.
    /// @param transport The transport. Borrowed: it must outlive the adapter.
    /// @param options   Its options; `callbacks` must be the executor the transport may deliver replies on.
    /// @throws std::invalid_argument without a callbacks executor.
    McpAdapter(backend::IEnvelopeTransport& transport, McpOptions options)
        : _shared{std::make_shared<Shared>(std::move(options))},
          _timer{std::make_unique<async::detail::TimeoutScheduler>()} {
        if (_shared->options.callbacks == nullptr) {
            throw std::invalid_argument{"McpAdapter: a callbacks executor is required"};
        }
        _shared->timer = _timer.get();
        _shared->link = [&transport](std::shared_ptr<Shared> const& self, wire::Envelope env, EnvelopeReply done) {
            Shared::overTransport(self, transport, std::move(env), std::move(done));
        };
    }

    /// @brief Closes the adapter's connection, if it opened one, and drops the timeouts still pending.
    ~McpAdapter() {
        {
            std::scoped_lock const lock{_shared->mutex};
            _shared->timer = nullptr;
        }
        _timer.reset();
        // A destructor must not throw, so a close that fails is dropped.
        try {
            if (_shared->close) {
                _shared->close();
            }
        } catch (...) {  // NOLINT(bugprone-empty-catch)
        }
    }

    /// @brief A callable that forgets the cached catalog and every cached model id, so the next call fetches the
    ///        catalog and registers its instance again: what a transport's disconnect handler runs.
    ///
    /// It holds the adapter's state weakly, so calling it after the adapter is gone does nothing; a transport's
    /// handler outlives the adapter that borrows the transport. Callable from any thread. A reply to a request
    /// sent before the reset is not cached.
    /// @return The callable.
    [[nodiscard]] std::function<void()> resetter() const {
        return [weak = std::weak_ptr<Shared>{_shared}] {
            if (auto const shared = weak.lock()) {
                shared->reset();
            }
        };
    }
```

   Add `std::unique_ptr<async::detail::TimeoutScheduler> _timer;` after `_shared`.

9. Add after `serveStdio`:

```cpp
/// @brief Serves an adapter over MCP's stdio transport when its owner is turned by another thread, such as a Qt
///        event loop: each message's `handle` is posted to @p owner, and the calling thread waits for its reply.
///        One JSON-RPC message per line of @p input, each reply on a line of @p output, in order, until @p input
///        ends. An empty line is skipped.
/// @param adapter The adapter; its `McpOptions::callbacks` is @p owner.
/// @param owner   The adapter's owner, a serial executor another thread runs.
/// @param input   Where messages arrive.
/// @param output  Where replies go; flushed after each.
inline void serveStdioOnOwner(McpAdapter& adapter, exec::IExecutor& owner, std::istream& input,
                              std::ostream& output) {
    std::string line;
    while (std::getline(input, line)) {
        if (line.empty()) {
            continue;
        }
        std::mutex mutex;
        std::condition_variable answered;
        std::optional<std::optional<std::string>> replied;
        owner.post([&adapter, &line, &mutex, &answered, &replied] {
            adapter.handle(line).thenDetached([&mutex, &answered, &replied](std::optional<std::string> const& reply) {
                std::scoped_lock const lock{mutex};
                replied = reply;
                answered.notify_one();
            });
        });
        std::unique_lock lock{mutex};
        answered.wait(lock, [&replied] { return replied.has_value(); });
        if (*replied) {
            output << **replied << '\n' << std::flush;
        }
    }
}
```

   Update the file's `@file` comment and the class comment: the adapter runs over a server in-process, or over a
   transport.

- [ ] **Step 4: Run the tests to verify they pass**

Run: `flock /tmp/morph-build.lock cmake --build build/linux-everything --target morph_tests -j5 && build/linux-everything/tests/morph_tests "[mcp],[agent]"`
Expected: every `[mcp]` case and the existing `[push][agent]` cases pass.

- [ ] **Step 5: Mutation checks** (record the output)

- Remove the `if (self->generation == generation)` guard in `withInstance`: "a register reply released after a
  reset is not cached" must fail.
- Set the scheduled timeout to never fire (skip `schedule`): the timeout case must fail at its 5 s limit.
- In `resetter`, capture `_shared` strongly instead of weakly: the case still passes, but ASan must report no
  error. This one is a lifetime check, not a vacuity check, so note it as such.

Restore each mutation and `touch include/morph/ui/agent.hpp` after restoring.

- [ ] **Step 6: Commit**

```bash
clang-format -i include/morph/ui/agent.hpp tests/test_mcp_transport.cpp
git add include/morph/ui/agent.hpp tests/test_mcp_transport.cpp tests/CMakeLists.txt
git commit -m "ui: McpAdapter over a transport, with a call timeout and a guarded reset" -m "<trailer lines>"
```

---

### Task 2: `retryFirstConnect` for both WebSocket backends

**Files:**
- Modify: `include/morph/net/socket_backend.hpp` (`SocketBackendConfig` at line 40; the first-connect
  give-up at ~line 1233)
- Modify: `include/morph/qt/qt_websocket_backend.hpp` (`QtWebSocketBackendConfig` at line 34),
  `src/qt/qt_websocket_backend.cpp` (the `disconnected` slot at ~line 69; the never-connected request rejection)
- Test: `tests/net/test_socket_backend.cpp`, `tests/qt/test_qt_websocket.cpp`
- Modify: `docs/spec/core/backend.md`, where each backend's reconnect settings are described

**Interfaces:**
- Produces: `bool SocketBackendConfig::retryFirstConnect = false;` and
  `bool QtWebSocketBackendConfig::retryFirstConnect = false;`

- [ ] **Step 1: Write the failing tests**

In `tests/net/test_socket_backend.cpp`, after "reconnects to a fresh server on the same port":

```cpp
// With retryFirstConnect, a server that comes up after the first attempt failed is still reached, and a request made
// before it is up is held and answered rather than rejected.
TEST_CASE("SocketBackend: retryFirstConnect reaches a server that listens after the first attempt",
          "[net][socket_backend][disconnect]") {
    morph::exec::ThreadPoolExecutor serverPool{2};
    auto server = std::make_shared<morph::backend::RemoteServer>(serverPool);
    // Reserve a free port, then release it, so the first connect is refused.
    std::uint16_t port = 0;
    {
        morph::net::SocketServer probe{*server, 0};
        REQUIRE(probe.listen());
        port = probe.port();
    }
    morph::net::SocketBackend::Config cfg;
    cfg.retryFirstConnect = true;
    cfg.initialReconnectDelay = std::chrono::milliseconds{50};
    cfg.maxReconnectDelay = std::chrono::milliseconds{200};
    morph::net::SocketBackend backend{"ws://127.0.0.1:" + std::to_string(static_cast<unsigned>(port)), cfg};
    REQUIRE_FALSE(backend.waitForConnected(std::chrono::milliseconds{150}));

    morph::exec::MainThreadExecutor cbExec;
    std::optional<morph::wire::Envelope> reply;
    morph::wire::Envelope schemas = morph::wire::makeSchemas("SbEchoModel");
    backend.request(std::move(schemas), cbExec).thenDetached([&](morph::wire::Envelope const& got) { reply = got; });

    morph::net::SocketServer late{*server, port};
    REQUIRE(late.listen());
    auto const deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
    while (!reply && std::chrono::steady_clock::now() < deadline) {
        cbExec.runFor(std::chrono::milliseconds{10});
    }
    REQUIRE(reply.has_value());
    CHECK(reply->kind == "ok");
}

TEST_CASE("SocketBackend: without retryFirstConnect, a refused first connect is not retried",
          "[net][socket_backend][disconnect]") {
    // Same set-up with the default Config: after `late` listens, waitForConnected(1 s) is still false.
}
```

Add the same pair to `tests/qt/test_qt_websocket.cpp` with `QtWebSocketBackend`, `QtWebSocketServer`,
`ensureApp()` and `pumpUntil`, modelled on "reconnects to a fresh server on the same port" (line 1158). The Qt
backend's verbs run on the Qt thread, which is the test thread there.

Write the second case of each pair in full.

- [ ] **Step 2: Run them to verify they fail**

Run: `flock /tmp/morph-build.lock cmake --build build/linux-everything --target morph_net_tests morph_qt_tests -j5`
Expected: a compile error: no `retryFirstConnect` member.

- [ ] **Step 3: Implement**

Add to both config structs:

```cpp
    /// @brief Whether to keep retrying when the first connect never reaches the server, on the same backoff as a
    ///        reconnect. Off by default: a client that starts before its server is usually misconfigured, and
    ///        failing fast says so. A process that may start before its server, such as an agent's MCP server,
    ///        turns it on. While it retries, requests are held as they are during a reconnect.
    bool retryFirstConnect = false;
```

In `socket_backend.hpp`:
- Change `if (!reachedServer && !self->everConnected)` to
  `if (!reachedServer && !self->everConnected && !self->cfg.retryFirstConnect)`.
- Check every other `everConnected`/`attemptsOver` test on the request and control paths (`fileControl` at ~887
  and the execute/request filing paths). With `retryFirstConnect` set, a request made before the first connect
  must queue like one made after a drop, never reject.

In `qt_websocket_backend.cpp`:
- In the `disconnected` slot, change `_everConnected` in the reconnect condition to
  `(_everConnected || _cfg.retryFirstConnect)`.
- Confirm by test whether a refused first `open` emits `disconnected`. If it emits only `errorOccurred`, connect
  that signal to the same scheduling, but only while `!_everConnected && _cfg.retryFirstConnect`.
- Find where a request on a never-connected socket rejects at once (the "requests on a never-connected socket
  reject, without blocking" case, line 1206 of the test). With `retryFirstConnect` set, those requests are held.

Document the flag in `docs/spec/core/backend.md`, next to each backend's reconnect settings.

- [ ] **Step 4: Run to verify they pass**

Run: `build/linux-everything/tests/net/morph_net_tests "[socket_backend]" && build/linux-everything/tests/qt/morph_qt_tests "[ws]"`
Expected: PASS, including every existing case. The never-connected rejection cases must still pass with the
default config.

- [ ] **Step 5: Mutation check**

Make the condition ignore `retryFirstConnect`: the "reaches a server" case must fail for each backend. Restore it
and `touch` the file.

- [ ] **Step 6: Commit**

```bash
git add include/morph/net/socket_backend.hpp include/morph/qt/qt_websocket_backend.hpp src/qt/qt_websocket_backend.cpp tests/net/test_socket_backend.cpp tests/qt/test_qt_websocket.cpp docs/spec/core/backend.md
git commit -m "net, qt: retryFirstConnect keeps retrying a server that is not up yet" -m "<trailer lines>"
```

---

### Task 3: `client::makeLocalServer`

**Files:**
- Modify: `include/morph/client/source.hpp` (after `struct LocalApp`, line 58), `src/client/source.cpp`
- Modify: `examples/common/testkit/screen_harness.hpp` (`SimulatedAppSource::connect`, lines 77-95)
- Test: `tests/client/test_sources.cpp`

**Interfaces:**
- Produces:

```cpp
/// @brief The in-process server for @p app: a `RemoteServer` whose services hold a `ui::UiService` over the
///        application's bundle, checked by the application's `serverAuthorizer` when it has one and by allow-all
///        otherwise. What a local MCP server and a simulated test source run the application on. It does not run
///        `setUp`, which stays the caller's.
/// @param app     The application.
/// @param workers The pool the server's strands run on. Borrowed: it must outlive the server.
/// @param config  The server's other settings; the UI service is appended to its services.
/// @return The server, or why the bundle did not build.
[[nodiscard]] std::expected<std::shared_ptr<backend::RemoteServer>, std::string>
makeLocalServer(LocalApp const& app, exec::IExecutor& workers, backend::ServerConfig config = {});
```

- [ ] **Step 1: Write the failing test** in `tests/client/test_sources.cpp`:

```cpp
TEST_CASE("makeLocalServer serves the application's catalog and runs its actions", "[client][local-server]") {
    morph::exec::ThreadPoolExecutor workers{2};
    auto const server = morph::client::makeLocalServer(probe::app(), workers);
    REQUIRE(server.has_value());
    morph::wire::Envelope catalog;
    catalog.kind = std::string{morph::ui::kCatalogKind};
    catalog.callId = 1;
    auto const reply = morph::wire::decode((*server)->handleInline(morph::wire::encode(catalog), 0));
    CHECK(reply.kind == "ok");
    CHECK(reply.body.contains("Probe_Answer"));
}

TEST_CASE("makeLocalServer refuses an application whose bundle does not build", "[client][local-server]") {
    morph::exec::ThreadPoolExecutor workers{1};
    morph::client::LocalApp broken{.name = "broken",
                                   .bundle = [] -> std::expected<std::shared_ptr<morph::ui::ServedBundle const>,
                                                                 morph::ui::Refusal> {
                                       return std::unexpected(morph::ui::Refusal{/* fill from ui::Refusal's fields */});
                                   },
                                   .setUp = {}};
    auto const server = morph::client::makeLocalServer(broken, workers);
    REQUIRE_FALSE(server.has_value());
    CHECK_FALSE(server.error().empty());
}

TEST_CASE("makeLocalServer checks requests with the application's serverAuthorizer", "[client][local-server]") {
    // probe::app() with .serverAuthorizer returning an IAuthorizer whose authorize() is always false: an execute of
    // Probe_Answer through handleInline answers err.
}
```

Construct `ui::Refusal` however `include/morph/ui/` defines it; grep `struct Refusal`. Write the third case in full.

- [ ] **Step 2: Run to verify it fails**

Run: `flock /tmp/morph-build.lock cmake --build build/linux-everything --target morph_client_tests -j5`
Expected: compile error: `makeLocalServer` is not declared.

- [ ] **Step 3: Implement** in `src/client/source.cpp`:

```cpp
std::expected<std::shared_ptr<backend::RemoteServer>, std::string> makeLocalServer(LocalApp const& app,
                                                                                   exec::IExecutor& workers,
                                                                                   backend::ServerConfig config) {
    if (!app.bundle) {
        return std::unexpected("the application " + app.name + " has no bundle");
    }
    auto bundle = app.bundle();
    if (!bundle) {
        return std::unexpected(bundle.error().message());
    }
    config.services.push_back(std::make_shared<ui::UiService>(std::move(*bundle)));
    if (app.serverAuthorizer) {
        return std::make_shared<backend::RemoteServer>(workers, app.serverAuthorizer(), std::move(config));
    }
    return std::make_shared<backend::RemoteServer>(workers, std::move(config));
}
```

Update `LocalApp::serverAuthorizer`'s doc: it is called by `makeLocalServer`, which local MCP and the simulated
test source use. Remove "Only a test's simulated server calls it".

Move `SimulatedAppSource::connect` onto the helper. Keep its `setUp` call and its clock settings:

```cpp
        if (_app.setUp) {
            _app.setUp(::morph::client::LocalSetup{});
        }
        _workers = std::make_unique<exec::ThreadPoolExecutor>(2);
        backend::ServerConfig config;
        config.clock = [] { return *::morph::ladder::now().value; };
        config.clockRecheck = std::chrono::milliseconds{20};
        auto made = ::morph::client::makeLocalServer(_app, *_workers, std::move(config));
        if (!made) {
            return std::unexpected(ui::ConnectError{.kind = ui::ConnectError::Kind::Unsupported, .message = made.error()});
        }
        _server = std::move(*made);
```

The bundle is now built after `setUp`. That ordering matches local mode. Check that no screen test depended on
the bundle being built first: run the ladder screen tests.

- [ ] **Step 4: Run to verify**

Run: `flock /tmp/morph-build.lock cmake --build build/linux-everything --target morph_client_tests -j5 && build/linux-everything/tests/client/morph_client_tests "[local-server],[sources]"`,
then the ladder screen suites:
`flock /tmp/morph-build.lock cmake --build build/linux-everything -j5 && ctest --test-dir build/linux-everything -L ladder -j5 --output-on-failure`
Expected: PASS.

- [ ] **Step 5: Mutation check**

Drop the `serverAuthorizer` branch, so every server is allow-all: the authorizer case must fail.

- [ ] **Step 6: Commit**

```bash
git add include/morph/client/source.hpp src/client/source.cpp examples/common/testkit/screen_harness.hpp tests/client/test_sources.cpp
git commit -m "client: makeLocalServer builds a LocalApp's in-process server" -m "<trailer lines>"
```

---

### Task 4: `--mcp-stdio` and `serveMcp`

**Files:**
- Modify: `include/morph/client/args.hpp`, `src/client/args.cpp`, `tests/client/test_client_args.cpp`
- Create: `include/morph/client/mcp.hpp`, `src/client/mcp.cpp`
- Modify: `include/morph/client/transport.hpp`, `src/client/transport_net.cpp`, `src/client/transport_qt.cpp`
- Modify: `src/client/CMakeLists.txt` (add `mcp.cpp` and the `mcp.hpp` header)
- Create: `tests/client/test_mcp.cpp`, `tests/client/test_mcp_remote.cpp`
- Modify: `tests/client/CMakeLists.txt`
- Modify: `examples/client/main.cpp`

**Interfaces:**
- Consumes: Task 1's `McpAdapter(IEnvelopeTransport&, McpOptions)`, `resetter()`, `serveStdioOnOwner` and
  `callTimeout`; Task 2's `retryFirstConnect`; Task 3's `makeLocalServer`.
- Produces:

```cpp
namespace morph::client {
/// `Args::mcpStdio`: `--mcp-stdio`.
[[nodiscard]] std::expected<void, std::string> checkMcpArgs(Args const& args, std::vector<std::string> const& rest);
[[nodiscard]] session::Context mcpSession(Args const& args, std::function<char const*(char const*)> const& environment,
                                          std::ostream& diagnostics);
[[nodiscard]] int serveMcp(Args const& args, std::vector<LocalApp> const& apps, session::Context session,
                           std::istream& input, std::ostream& output, std::ostream& diagnostics);
[[nodiscard]] std::unique_ptr<std::ostream> claimStdout();
}
namespace morph::client::detail {
[[nodiscard]] std::expected<RemoteTransport, std::string> socketTransport(std::string const& url, bool retryFirstConnect);
[[nodiscard]] int serveMcpOverQt(std::string const& url, ui::McpOptions options, std::istream& input,
                                 std::ostream& output, std::ostream& diagnostics);
}
```

- [ ] **Step 1: Write the failing argument tests** in `tests/client/test_client_args.cpp`:

```cpp
TEST_CASE("readArgs: --mcp-stdio is a flag, off without it", "[client][args][mcp]") {
    std::vector<std::string> args{"client", "--app", "probe", "--mcp-stdio"};
    auto const read = readArgs(args);
    REQUIRE(read.has_value());
    CHECK(read->mcpStdio);
    CHECK(args == std::vector<std::string>{"client"});
}

TEST_CASE("checkMcpArgs refuses frontend options and leftover arguments", "[client][args][mcp]") {
    auto const refused = [](std::vector<std::string> args) {
        auto const read = readArgs(args);
        REQUIRE(read.has_value());
        return morph::client::checkMcpArgs(*read, args);
    };
    CHECK(refused({"c", "--app", "probe", "--mcp-stdio"}).has_value());
    CHECK_FALSE(refused({"c", "--app", "probe", "--mcp-stdio", "--screen", "s"}).has_value());
    CHECK_FALSE(refused({"c", "--app", "probe", "--mcp-stdio", "--exit-after-mount"}).has_value());
    CHECK_FALSE(refused({"c", "--app", "probe", "--mcp-stdio", "--dev"}).has_value());
    auto const leftover = refused({"c", "--app", "probe", "--mcp-stdio", "--ui=qt"});
    REQUIRE_FALSE(leftover.has_value());
    CHECK_THAT(leftover.error(), Catch::Matchers::ContainsSubstring("--ui=qt"));
}

TEST_CASE("mcpSession takes the principal and token as the spec says", "[client][mcp]") {
    std::map<std::string, std::string> env{{"MORPH_MCP_PRINCIPAL", "envuser"}, {"MORPH_MCP_TOKEN", "t0k"}};
    auto const lookup = [&env](char const* name) -> char const* {
        auto const found = env.find(name);
        return found == env.end() ? nullptr : found->second.c_str();
    };
    std::ostringstream diagnostics;
    morph::client::Args remote;
    remote.server = "ws://h:1";
    CHECK(morph::client::mcpSession(remote, lookup, diagnostics).principal == "envuser");
    CHECK(morph::client::mcpSession(remote, lookup, diagnostics).token == "t0k");
    remote.user = "ann";
    CHECK(morph::client::mcpSession(remote, lookup, diagnostics).principal == "ann");
    morph::client::Args local;
    local.app = "probe";
    local.user = "ann";
    auto const session = morph::client::mcpSession(local, lookup, diagnostics);
    CHECK(session.principal == "ann");
    CHECK(session.token.empty());
    CHECK_THAT(diagnostics.str(), Catch::Matchers::ContainsSubstring("MORPH_MCP_TOKEN"));
}
```

- [ ] **Step 2: Write the failing local-mode tests** in `tests/client/test_mcp.cpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0

#include <catch2/catch_test_macros.hpp>
#include <morph/client/mcp.hpp>
#include <morph/ui/value.hpp>
#include <sstream>
#include <string>
#include <vector>

#include "probe_app.hpp"

namespace {

std::vector<std::string> lines(std::string const& text) {
    std::vector<std::string> out;
    std::istringstream stream{text};
    for (std::string line; std::getline(stream, line);) {
        out.push_back(line);
    }
    return out;
}

}  // namespace

TEST_CASE("serveMcp in local mode lists the application's actions and runs one", "[client][mcp]") {
    morph::client::Args args;
    args.app = "probe";
    args.mcpStdio = true;
    std::istringstream input{
        R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{}})" "\n"
        R"({"jsonrpc":"2.0","method":"notifications/initialized"})" "\n"
        R"({"jsonrpc":"2.0","id":2,"method":"tools/list"})" "\n"
        R"({"jsonrpc":"2.0","id":3,"method":"tools/call","params":{"name":"Probe_Answer","arguments":{}}})" "\n"};
    std::ostringstream output;
    std::ostringstream diagnostics;
    int const code = morph::client::serveMcp(args, {probe::app()}, {}, input, output, diagnostics);
    CHECK(code == 0);
    auto const replies = lines(output.str());
    REQUIRE(replies.size() == 3);
    for (auto const& reply : replies) {
        CHECK(morph::ui::parseJson(reply).has_value());
    }
    CHECK(replies.at(0).contains(R"("serverInfo":{"name":"probe")"));
    CHECK(replies.at(1).contains(R"("name":"Probe_Answer")"));
    CHECK(replies.at(2).contains(R"("isError":false)"));
}

TEST_CASE("serveMcp refuses an application the binary does not link", "[client][mcp]") {
    morph::client::Args args;
    args.app = "nope";
    args.mcpStdio = true;
    std::istringstream input;
    std::ostringstream output;
    std::ostringstream diagnostics;
    CHECK(morph::client::serveMcp(args, {probe::app()}, {}, input, output, diagnostics) == 2);
    CHECK(output.str().empty());
    CHECK(diagnostics.str().contains("probe"));
}
```

If `Probe_Answer` is not in the principal's `permitted` list (the probe screens must name it), use whichever probe
action `tools/list` returns, and say so in the test's comment.

- [ ] **Step 3: Write the failing remote-mode test** in `tests/client/test_mcp_remote.cpp`. It is compiled only when
`TARGET morph::net`:

```cpp
// serveMcp in remote mode over morph::net: a SocketServer over an in-process server built from probe::app(), so the
// remote binary needs no application of its own.
TEST_CASE("serveMcp in remote mode answers over a WebSocket", "[client][mcp][net]") {
    morph::exec::ThreadPoolExecutor workers{2};
    auto const server = morph::client::makeLocalServer(probe::app(), workers);
    REQUIRE(server.has_value());
    morph::net::SocketServer socket{**server, 0};
    REQUIRE(socket.listen());
    morph::client::Args args;
    args.server = "ws://127.0.0.1:" + std::to_string(static_cast<unsigned>(socket.port()));
    args.mcpStdio = true;
    std::istringstream input{R"({"jsonrpc":"2.0","id":1,"method":"tools/list"})" "\n"};
    std::ostringstream output;
    std::ostringstream diagnostics;
    CHECK(morph::client::serveMcp(args, {}, {}, input, output, diagnostics) == 0);
    CHECK(output.str().contains(R"("name":"evaluate")"));
    CHECK(output.str().find(R"("name":")") != output.str().rfind(R"("name":")"));
}

TEST_CASE("serveMcp in remote mode reaches a server that starts after it", "[client][mcp][net]") {
    // Reserve a port and release it, as Task 2's test does. Run serveMcp on a std::thread fed by a pipe-backed
    // istream: start it, wait 300 ms, start a SocketServer on the port, write one tools/list line, close input.
    // The reply must list more than `evaluate`.
    // A stringstream cannot block. Use a small std::streambuf whose underflow waits on a condition variable for
    // lines the test pushes.
}

TEST_CASE("serveMcp refuses wss:// when no transport can reach it", "[client][mcp]") {
    // Only where MORPH_CLIENT_TRANSPORT_QT is 0. Skip with SUCCEED otherwise. Expect exit code 2 and a message
    // naming the missing transport.
}
```

Write all three cases in full. In `tests/client/CMakeLists.txt`, add `test_mcp.cpp` to the executable. Then add:

```cmake
if(TARGET morph::net)
    target_sources(morph_client_tests PRIVATE test_mcp_remote.cpp)
    target_link_libraries(morph_client_tests PRIVATE morph::net)
endif()
```

- [ ] **Step 4: Run them to verify they fail**

Run: `flock /tmp/morph-build.lock cmake --build build/linux-everything --target morph_client_tests -j5`
Expected: compile errors: no `mcpStdio`, and no `morph/client/mcp.hpp`.

- [ ] **Step 5: Implement `--mcp-stdio`**

In `args.hpp`, add to `Args`:

```cpp
    /// @brief `--mcp-stdio`: serve MCP on stdin and stdout instead of mounting a frontend (`client::serveMcp`).
    bool mcpStdio = false;
```

Reword `user`'s doc: the principal a local application signs in, and the principal an MCP server's session
carries.

In `args.cpp`, add `constexpr std::string_view kMcpStdio = "--mcp-stdio";`, then extend the flag branch:

```cpp
        if (arg == kExitAfterMount || arg == kDev || arg == kMcpStdio) {
            if (arg == kDev) {
                read.dev = true;
            } else if (arg == kMcpStdio) {
                read.mcpStdio = true;
            } else {
                read.exitAfterMount = true;
            }
            used.push_back(index);
            continue;
        }
```

- [ ] **Step 6: Implement transports for MCP**

In `transport.hpp`'s `detail` namespace, add `socketTransport(std::string const& url, bool retryFirstConnect)` and
`serveMcpOverQt(...)`, documented as in the Interfaces block. `transport.hpp` must not include `agent.hpp` (it
names no toolkit, and `ui::McpOptions` only needs a forward declaration). Declare `serveMcpOverQt` in
`src/client/mcp_internal.hpp` instead: a private header that `mcp.cpp` and `transport_qt.cpp` include.

`transport_net.cpp`:

```cpp
std::expected<RemoteTransport, std::string> socketTransport(std::string const& url, bool retryFirstConnect) {
    try {
        net::SocketBackend::Config config;
        config.retryFirstConnect = retryFirstConnect;
        auto made = std::make_unique<net::SocketBackend>(url, config);
        backend::IEnvelopeTransport* const requests = made.get();
        return RemoteTransport{.backend = std::move(made), .requests = requests};
    } catch (std::exception const& error) {
        return std::unexpected(std::string{error.what()});
    }
}
```

`transport_qt.cpp`:

```cpp
int serveMcpOverQt(std::string const& url, ui::McpOptions options, std::istream& input, std::ostream& output,
                   std::ostream& diagnostics) {
    QUrl const parsed{QString::fromStdString(url)};
    if (!parsed.isValid() || (parsed.scheme() != "ws" && parsed.scheme() != "wss")) {
        diagnostics << "client: not a ws:// or wss:// URL: " << url << '\n';
        return 2;
    }
    // A stdio server mounts no frontend, so nothing else made the application object the backend's socket needs.
    static int argc = 1;
    static char name[] = "morph-mcp";
    static char* argv[] = {name, nullptr};
    std::unique_ptr<QCoreApplication> application;
    if (QCoreApplication::instance() == nullptr) {
        application = std::make_unique<QCoreApplication>(argc, argv);
    }
    // The backend lives on this thread and must be called here, so its owner is this thread's event loop, and stdin
    // is read on a thread of its own.
    qt::QtExecutor owner;
    qt::QtWebSocketBackend::Config config;
    config.retryFirstConnect = true;
    qt::QtWebSocketBackend transport{parsed, config};
    options.callbacks = &owner;
    ui::McpAdapter adapter{transport, std::move(options)};
    transport.setDisconnectHandler(adapter.resetter());
    std::thread stdio{[&adapter, &owner, &input, &output] {
        ui::serveStdioOnOwner(adapter, owner, input, output);
        QMetaObject::invokeMethod(QCoreApplication::instance(), [] { QCoreApplication::quit(); },
                                  Qt::QueuedConnection);
    }};
    QCoreApplication::exec();
    stdio.join();
    transport.setDisconnectHandler(nullptr);
    return 0;
}
```

Over `wss://`, the backend's default TLS behaviour applies. Check the `QUrl, Config` constructor's documentation
for what that is. If it does not verify against the system CAs, pass `qt::tlsVerifyingConfig()` from
`qt_tls.hpp` for `wss://`.

- [ ] **Step 7: Implement `serveMcp`** in `include/morph/client/mcp.hpp` and `src/client/mcp.cpp`

Write the header with the declarations from the Interfaces block, with full Doxygen. The file comment cites
`docs/spec/client/client.md` and `docs/spec/ui/agent.md`.

`src/client/mcp.cpp`:

```cpp
namespace morph::client {

std::expected<void, std::string> checkMcpArgs(Args const& args, std::vector<std::string> const& rest) {
    if (!args.screen.empty()) {
        return std::unexpected(std::string{"--mcp-stdio mounts no screen: drop --screen"});
    }
    if (args.exitAfterMount) {
        return std::unexpected(std::string{"--mcp-stdio mounts no screen: drop --exit-after-mount"});
    }
    if (args.dev) {
        return std::unexpected(std::string{"--mcp-stdio has no development tools: drop --dev"});
    }
    if (rest.size() > 1) {
        return std::unexpected("--mcp-stdio takes no frontend arguments: " + rest.at(1));
    }
    return {};
}

session::Context mcpSession(Args const& args, std::function<char const*(char const*)> const& environment,
                            std::ostream& diagnostics) {
    session::Context session;
    session.principal = args.user;
    if (args.server) {
        if (session.principal.empty()) {
            if (char const* const principal = environment("MORPH_MCP_PRINCIPAL")) {
                session.principal = principal;
            }
        }
        if (char const* const token = environment("MORPH_MCP_TOKEN")) {
            session.token = token;
        }
    } else if (environment("MORPH_MCP_TOKEN") != nullptr) {
        diagnostics << "client: MORPH_MCP_TOKEN is ignored in local mode, whose server checks no token\n";
    }
    return session;
}

namespace {

int serveLocal(Args const& args, std::vector<LocalApp> const& apps, session::Context session, std::istream& input,
               std::ostream& output, std::ostream& diagnostics) {
    auto const found = std::ranges::find(apps, *args.app, &LocalApp::name);
    if (found == apps.end()) {
        diagnostics << "client: no application named " << *args.app << "; this client links:";
        for (LocalApp const& app : apps) {
            diagnostics << ' ' << app.name;
        }
        diagnostics << '\n';
        return 2;
    }
    if (found->setUp) {
        found->setUp(LocalSetup{.db = args.db, .user = args.user});
    }
    // Declared in the order that tears down safely: the adapter first, then the server, then the owner its replies
    // were delivered on, then the pool its strands ran on.
    exec::ThreadPoolExecutor workers{4};
    exec::MainThreadExecutor owner;
    auto server = makeLocalServer(*found, workers);
    if (!server) {
        diagnostics << "client: " << server.error() << '\n';
        return 2;
    }
    {
        ui::McpAdapter adapter{*server, ui::McpOptions{.session = std::move(session),
                                                       .serverName = found->name,
                                                       .callbacks = &owner}};
        ui::serveStdio(adapter, owner, input, output);
    }
    server->reset();
    return 0;
}

int serveRemote(Args const& args, session::Context session, std::istream& input, std::ostream& output,
                std::ostream& diagnostics) {
    std::string const& url = *args.server;
    ui::McpOptions options{.session = std::move(session), .serverName = "morph"};
#if MORPH_CLIENT_TRANSPORT_NET
    if (url.starts_with("ws://")) {
        // Declared in the order that tears down safely: the adapter, then the transport (which rejects what is
        // still pending onto the owner), then the owner.
        exec::MainThreadExecutor owner;
        auto transport = detail::socketTransport(url, true);
        if (!transport) {
            diagnostics << "client: " << transport.error() << '\n';
            return 2;
        }
        options.callbacks = &owner;
        {
            ui::McpAdapter adapter{*transport->requests, std::move(options)};
            transport->backend->setDisconnectHandler(adapter.resetter());
            ui::serveStdio(adapter, owner, input, output);
            transport->backend->setDisconnectHandler(nullptr);
        }
        return 0;
    }
#endif
#if MORPH_CLIENT_TRANSPORT_QT
    return detail::serveMcpOverQt(url, std::move(options), input, output, diagnostics);
#else
    diagnostics << "client: " << url
                << (url.starts_with("wss://") ? " needs Qt WebSockets, which are not built here\n"
                                              : " needs morph::net or Qt WebSockets, and neither is built here\n");
    return 2;
#endif
}

}  // namespace

int serveMcp(Args const& args, std::vector<LocalApp> const& apps, session::Context session, std::istream& input,
             std::ostream& output, std::ostream& diagnostics) {
    return args.app ? serveLocal(args, apps, std::move(session), input, output, diagnostics)
                    : serveRemote(args, std::move(session), input, output, diagnostics);
}

}  // namespace morph::client
```

`ui::McpOptions` is an aggregate. If designated initializers in the order written do not compile, follow the
member order in `agent.hpp`: `session`, `serverName`, `serverVersion`, `callbacks`, `callTimeout`.

`claimStdout`, in the same file:

```cpp
namespace {

// An ostream over a C stream: the duplicate of the process's original stdout.
class FileStreamBuf final : public std::streambuf {
public:
    explicit FileStreamBuf(std::FILE* file) : _file{file} {}
    ~FileStreamBuf() override { std::fclose(_file); }
    FileStreamBuf(FileStreamBuf const&) = delete;
    FileStreamBuf& operator=(FileStreamBuf const&) = delete;
    FileStreamBuf(FileStreamBuf&&) = delete;
    FileStreamBuf& operator=(FileStreamBuf&&) = delete;

protected:
    int_type overflow(int_type ch) override {
        return traits_type::eq_int_type(ch, traits_type::eof()) || std::fputc(ch, _file) != EOF ? traits_type::not_eof(ch)
                                                                                                 : traits_type::eof();
    }
    std::streamsize xsputn(char const* text, std::streamsize count) override {
        return static_cast<std::streamsize>(std::fwrite(text, 1, static_cast<std::size_t>(count), _file));
    }
    int sync() override { return std::fflush(_file) == 0 ? 0 : -1; }

private:
    std::FILE* _file;
};

class FileOStream final : public std::ostream {
public:
    explicit FileOStream(std::FILE* file) : std::ostream{nullptr}, _buffer{file} { rdbuf(&_buffer); }

private:
    FileStreamBuf _buffer;
};

}  // namespace

std::unique_ptr<std::ostream> claimStdout() {
    std::cout.flush();
    std::fflush(stdout);
#ifdef _WIN32
    int const channel = ::_dup(1);
    std::FILE* const file = channel < 0 ? nullptr : ::_fdopen(channel, "wb");
    if (file == nullptr || ::_dup2(2, 1) != 0) {
        return nullptr;
    }
#else
    int const channel = ::dup(1);
    std::FILE* const file = channel < 0 ? nullptr : ::fdopen(channel, "w");
    if (file == nullptr || ::dup2(2, 1) < 0) {
        return nullptr;
    }
#endif
    return std::make_unique<FileOStream>(file);
}
```

Document `claimStdout`: it duplicates stdout as the JSON-RPC channel and points descriptor 1 at stderr, so
anything else the process prints lands in the agent's log. It returns null when the descriptors cannot be
duplicated. Add `mcp.cpp` to `add_library(morph_client ...)` and `mcp.hpp` to the header set.

- [ ] **Step 8: Wire `main`**

In `examples/client/main.cpp`'s `run`, right after `readArgs` succeeds and **before** `MORPH_UI_DEV` is applied:

```cpp
    if (clientArgs->mcpStdio) {
        if (!grab->path.empty()) {
            std::cerr << "client: --mcp-stdio draws no window: drop --grab\n";
            return 2;
        }
        if (auto const checked = morph::client::checkMcpArgs(*clientArgs, args); !checked) {
            std::cerr << "client: " << checked.error() << '\n';
            return 2;
        }
        auto channel = morph::client::claimStdout();
        if (channel == nullptr) {
            std::cerr << "client: cannot set stdout aside for JSON-RPC\n";
            return 1;
        }
        // NOLINTNEXTLINE(concurrency-mt-unsafe): read at start, before any thread that could change it exists.
        auto const environment = [](char const* name) { return std::getenv(name); };
        auto session = morph::client::mcpSession(*clientArgs, environment, std::cerr);
        return morph::client::serveMcp(*clientArgs, examples::client::localApps(), std::move(session), std::cin,
                                       *channel, std::cerr);
    }
```

Update the file's header comment: it gains `--mcp-stdio`, which serves MCP on stdio instead of mounting a
frontend.

- [ ] **Step 9: Run to verify**

Run: `flock /tmp/morph-build.lock cmake --build build/linux-everything --target morph_client_tests morph_remote_client morph_examples_client -j5 && build/linux-everything/tests/client/morph_client_tests "[mcp]"`
Expected: PASS.

Then prove that the Qt-only path compiles:
`flock /tmp/morph-build.lock cmake --build build/gcc-debug --target morph_client morph_remote_client -j5`
Expected: it builds.

- [ ] **Step 10: Mutation checks**

- In `serveRemote`, do not install the resetter: the "reaches a server that starts after it" case should still pass
  (that case is about `retryFirstConnect`). Instead, build the transport with `retryFirstConnect = false`: that
  case must fail.
- Drop `checkMcpArgs`'s leftover check: the leftover case must fail.

- [ ] **Step 11: Commit**

```bash
git add include/morph/client src/client tests/client examples/client/main.cpp
git commit -m "client: --mcp-stdio serves MCP from the generic client, remote or local" -m "<trailer lines>"
```

---

### Task 5: The binary over a pipe, and the docs

**Files:**
- Create: `examples/client/tests/test_mcp_pipe.cpp`
- Modify: `examples/client/CMakeLists.txt`
- Modify: `docs/spec/ui/agent.md`, `docs/spec/client/client.md`, `examples/bank/README.md`

- [ ] **Step 1: The pipe test**

`examples/client/tests/test_mcp_pipe.cpp` runs the real binary through `popen`, as
`examples/bank/tests/test_mcp_stdio.cpp` does. It checks that **every** stdout line parses as JSON-RPC, which is
the stdout claim measured:

```cpp
TEST_CASE("morph_examples_client --mcp-stdio --app forms answers on a clean stdout", "[client][mcp][pipe]") {
    std::string const requests = R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{}}\n)"
                                 R"({"jsonrpc":"2.0","id":2,"method":"tools/list"}\n)";
    std::string const command = "printf '%b' '" + requests + "' | '" + std::string{MORPH_EXAMPLES_CLIENT_PATH} +
                                "' --mcp-stdio --app forms --user alice 2>/dev/null";
    auto const lines = outputLines(command);  // copy the helper from bank's test
    REQUIRE(lines.size() == 2);
    for (auto const& line : lines) {
        CHECK(morph::ui::parseJson(line).has_value());
    }
    CHECK(lines.at(1).contains(R"("name":"evaluate")"));
}

#if MORPH_CLIENT_HAS_LIMS
TEST_CASE("morph_examples_client --mcp-stdio --app lims serves lims's actions", "[client][mcp][pipe][lims]") {
    // A scratch database under temp_directory_path()/"client_mcp_lims" (removed first), --db pointing at it,
    // --user alice; tools/list must name more than `evaluate`, and every line must parse.
}
#endif

TEST_CASE("--mcp-stdio with a frontend option exits 2 and writes nothing to stdout", "[client][mcp][pipe]") {
    // printf '' | client --mcp-stdio --app forms --screen x ; capture stdout lines (empty) and the exit status (2).
}
```

Write the lims case and the refusal case in full. In `examples/client/CMakeLists.txt`, add after the client loop:

```cmake
if(NOT EMSCRIPTEN AND TARGET morph_test_main AND NOT WIN32)
    add_executable(morph_client_mcp_pipe_tests tests/test_mcp_pipe.cpp)
    target_link_libraries(morph_client_mcp_pipe_tests PRIVATE morph morph_test_main)
    target_compile_definitions(morph_client_mcp_pipe_tests PRIVATE
        MORPH_EXAMPLES_CLIENT_PATH="$<TARGET_FILE:morph_examples_client>"
        MORPH_CLIENT_HAS_LIMS=$<IF:$<TARGET_EXISTS:ladder_lims_screens>,1,0>)
    add_dependencies(morph_client_mcp_pipe_tests morph_examples_client)
    apply_warnings(morph_client_mcp_pipe_tests)
    include(Catch)
    catch_discover_tests(morph_client_mcp_pipe_tests DISCOVERY_MODE PRE_TEST PROPERTIES TIMEOUT 120 LABELS "client")
endif()
```

Run: `flock /tmp/morph-build.lock cmake --build build/linux-everything --target morph_client_mcp_pipe_tests -j5 && build/linux-everything/examples/client/morph_client_mcp_pipe_tests`
Expected: PASS.

Mutation check: comment out the `claimStdout` call in `main` and make the forms `setUp` print one line to stdout
temporarily. The parse check must fail. Restore both, then `touch` the files.

- [ ] **Step 2: The specs**

- `docs/spec/ui/agent.md`:
  - **Shape** gets the transport constructor, `callTimeout` and `resetter()`.
  - A new **Over a transport** section covers the link, the timeout, the generation guard and the weak resetter,
    with the reasoning from the spec's adapter section.
  - **stdio** gets `serveStdioOnOwner`.
  - A new **From the generic client** section links `docs/spec/client/client.md`.
  - **Design decisions** gain "reset on disconnect, guarded by a generation" and "a bound on each call, not a
    concurrent `serveStdio`".
- `docs/spec/client/client.md`: add an **MCP server (`--mcp-stdio`)** section covering both modes, the transport
  table, identity, stdout, refusals, local mode's semantics (the same as local GUI mode, not the production
  `main`), `makeLocalServer`, and both `claude mcp add` lines.
- `examples/bank/README.md`: under "An agent over MCP", add one paragraph. Bank can also be served from the
  generic client, locally, without the server, with the line
  `claude mcp add bank -- ./build/examples/client/morph_examples_client --mcp-stdio --app bank --db "$PWD/bank.db" --user alice`.

No history or issue numbers in any of them.

- [ ] **Step 3: Docs build**

Run: `cmake -S . -B build/docs -G Ninja -DMORPH_BUILD_DOCUMENTATION=ON -DMORPH_BUILD_TESTS=OFF -DMORPH_BUILD_EXAMPLES=OFF && flock /tmp/morph-build.lock cmake --build build/docs --target doc`
Expected: no warnings.

- [ ] **Step 4: Commit**

```bash
git add examples/client docs/spec/ui/agent.md docs/spec/client/client.md examples/bank/README.md
git commit -m "client: the MCP server over a real pipe, and its specs" -m "<trailer lines>"
```

---

## Final verification (controller)

- Do a full build of `build/linux-everything`, then run
  `ctest --test-dir build/linux-everything -j5 --output-on-failure`, and record the pass count.
- Build `build/gcc-debug`, the Qt-only remote path.
- Install the server for real:
  `claude mcp add morph-forms -- $PWD/build/linux-everything/examples/client/morph_examples_client --mcp-stdio --app forms --user alice`,
  then confirm `claude mcp list` shows it connected. Remove it afterwards.
- Open one PR from `feat/mcp-from-client`.

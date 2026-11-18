// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <exception>
#include <expected>
#include <functional>
#include <istream>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <ostream>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "../core/completion.hpp"
#include "../core/envelope_transport.hpp"
#include "../core/executor.hpp"
#include "../core/remote.hpp"
#include "../core/timeout_scheduler.hpp"
#include "../core/wire.hpp"
#include "../forms/forms.hpp"
#include "../session/session.hpp"
#include "bundle.hpp"
#include "value.hpp"

/// @file
/// The MCP adapter (spec 8 §11): an agent's tool calls run through the server as ordinary envelopes on a
/// connection of their own, so every check a GUI gets applies to them, and every GUI watching what they change is
/// told. The server is in the same process, or on the far side of a `backend::IEnvelopeTransport`. The adapter
/// speaks MCP's JSON-RPC 2.0 and knows no agent transport; `serveStdio` and `serveStdioOnOwner` drive it over MCP's
/// stdio transport.

namespace morph::ui {

/// @brief What an `McpAdapter` is built with.
struct McpOptions {
    /// @brief The session every envelope carries: the agent's identity, which the server authenticates on each one,
    ///        as it does a GUI's.
    session::Context session;
    /// @brief The name `initialize` answers in `serverInfo`.
    std::string serverName = "morph";
    /// @brief The version `initialize` answers in `serverInfo`.
    std::string serverVersion = "1";
    /// @brief The owner `handle`'s completions are delivered on: a serial executor that outlives the adapter and
    ///        every call still in flight, such as the `exec::MainThreadExecutor` `serveStdio` pumps. Borrowed;
    ///        required.
    exec::IExecutor* callbacks = nullptr;
    /// @brief How long an envelope sent over a transport waits for its reply before it is answered as a failure.
    ///        `serveStdio` handles one message at a time, so without this bound a request a transport holds through
    ///        an outage would block every later message. Zero waits forever. The in-process constructor ignores it,
    ///        since a `RemoteServer` always replies. A negative value is refused by the transport constructor.
    std::chrono::milliseconds callTimeout = std::chrono::seconds{30};
};

/// @brief The JSON-RPC error code of a method the adapter does not answer.
inline constexpr std::int64_t kMcpMethodNotFound = -32601;
/// @brief The JSON-RPC error code of a message that is not JSON.
inline constexpr std::int64_t kMcpParseError = -32700;
/// @brief The JSON-RPC error code of a message that is not a request.
inline constexpr std::int64_t kMcpInvalidRequest = -32600;
/// @brief The JSON-RPC error code of a call whose arguments the adapter cannot use.
inline constexpr std::int64_t kMcpInvalidParams = -32602;

/// @brief Serves MCP's tools over one server connection (spec 8 §11).
///
/// It answers `initialize`, `tools/list`, `tools/call` and `ping`, and refuses every other request with JSON-RPC error
/// `-32601`; a notification, which carries no `id`, gets no reply.
///
/// - **Tools.** One per action the catalog lists as permitted for the session, named by the action id, whose
///   `inputSchema` is the action's schema from `schemas`. A keyed model's tool also takes the instance key as the
///   argument `instance`, and runs on the shared instance with that key. Any other model's tool runs on one private
///   instance per model type, registered on the adapter's connection at its first call. A tool `evaluate`, taking
///   `{action, body}`, is a dry run through `ui-evaluate`.
/// - **A call's result** is the action's result JSON as text content, or the server's `err` message with `isError`.
/// - **The connection** of an adapter made over a server is opened when the adapter is made and closed when it is
///   destroyed, which releases its private instances. The server treats it like any other connection, so its changes
///   reach every watching GUI. Over a transport, the connection is the transport's.
///
/// `handle` is called on the owner, `McpOptions::callbacks`, and its completion is delivered there.
class McpAdapter {
public:
    /// @brief An adapter on a new connection of @p server.
    /// @param server  The server; the adapter keeps it alive.
    /// @param options Its options.
    /// @throws std::invalid_argument without a server or a callbacks executor.
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
    /// @throws std::invalid_argument without a callbacks executor, or with a negative `callTimeout`.
    McpAdapter(backend::IEnvelopeTransport& transport, McpOptions options)
        : _shared{std::make_shared<Shared>(std::move(options))} {
        if (_shared->options.callbacks == nullptr) {
            throw std::invalid_argument{"McpAdapter: a callbacks executor is required"};
        }
        if (_shared->options.callTimeout.count() < 0) {
            throw std::invalid_argument{"McpAdapter: callTimeout must not be negative"};
        }
        if (_shared->options.callTimeout.count() > 0) {
            _timer = std::make_unique<async::detail::TimeoutScheduler>();
            _shared->timer = _timer.get();
        }
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
        _timer = nullptr;
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
                shared->clear();
            }
        };
    }

    McpAdapter(McpAdapter const&) = delete;
    McpAdapter& operator=(McpAdapter const&) = delete;
    McpAdapter(McpAdapter&&) = delete;
    McpAdapter& operator=(McpAdapter&&) = delete;

    /// @brief Handles one JSON-RPC message.
    /// @param message The message's JSON text.
    /// @return Completes with the reply's JSON text, or with nothing for a notification. It never fails: a fault is
    ///         answered as a JSON-RPC error.
    [[nodiscard]] async::Completion<std::optional<std::string>> handle(std::string const& message) {
        auto [completion, promise] =
            async::Completion<std::optional<std::string>>::makeSettleable(_shared->options.callbacks);
        auto const settle = std::make_shared<decltype(promise)>(std::move(promise));
        Shared::dispatch(_shared, message,
                         [settle](std::optional<std::string> reply) { settle->resolve(std::move(reply)); });
        return std::move(completion);
    }

private:
    using Reply = std::function<void(std::optional<std::string>)>;
    using EnvelopeReply = std::function<void(wire::Envelope)>;

    // Everything a reply still in flight needs, kept alive by it after the adapter is gone.
    struct Shared {
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
        std::atomic<std::uint64_t> nextCall{0};
        std::mutex mutex;
        std::optional<CatalogReply> catalog;
        // Model ids: a private instance by model type, a shared one by model type and key.
        std::map<std::string, std::uint64_t, std::less<>> privateIds;
        std::map<std::pair<std::string, std::string>, std::uint64_t> sharedIds;

        // The schemas of every model a permitted action belongs to, gathered for one tool list.
        struct Gather {
            std::mutex mutex;
            std::size_t waiting = 0;
            std::map<std::string, Json, std::less<>> schemas;
        };

        // Sends one envelope on the adapter's connection, carrying its session.
        static void send(std::shared_ptr<Shared> const& self, wire::Envelope env, EnvelopeReply done) {
            env.callId = self->nextCall.fetch_add(1) + 1;
            env.session = self->options.session;
            self->link(self, std::move(env), std::move(done));
        }

        static void overServer(std::shared_ptr<backend::RemoteServer> const& server, backend::ConnectionId cid,
                               wire::Envelope const& env, EnvelopeReply done) {
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
            auto const millis = timeout.count();
            return millis % 1000 == 0 ? std::to_string(millis / 1000) + " s" : std::to_string(millis) + " ms";
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
                .thenDetached([pending, cancel](wire::Envelope const& reply) {
                    cancel();
                    pending->settle(wire::Envelope{reply});
                })
                .onErrorDetached([pending, cancel](std::exception_ptr const& error) {
                    cancel();
                    pending->settle(wire::makeErr(describe(error)));
                });
        }

        void clear() {
            std::scoped_lock const lock{mutex};
            ++generation;
            catalog.reset();
            privateIds.clear();
            sharedIds.clear();
        }

        static std::string write(Json const& value) { return glz::write_json(value).value_or(std::string{"null"}); }

        // A member of an object, or nullptr when the value is not an object or has no such member.
        [[nodiscard]] static Json const* member(Json const& value, std::string_view key) {
            return forms::detail::findMember(value, key);
        }

        [[nodiscard]] static std::string const* textMember(Json const& value, std::string_view key) {
            Json const* const found = member(value, key);
            return found == nullptr ? nullptr : std::get_if<std::string>(&found->data);
        }

        // The object member @p key of @p object, made an object when it is missing or is not one.
        static Json::object_t& objectMember(Json::object_t& object, std::string const& key) {
            Json& slot = object.try_emplace(key, Json::object_t{}).first->second;
            if (!std::holds_alternative<Json::object_t>(slot.data)) {
                slot = Json::object_t{};
            }
            return std::get<Json::object_t>(slot.data);
        }

        // A JSON-RPC response: @p requestId's answer, under the member @p outcome (`result` or `error`).
        static std::string answer(Json const& requestId, std::string const& outcome, Json content) {
            return write(Json::object_t{{"jsonrpc", "2.0"}, {"id", requestId}, {outcome, std::move(content)}});
        }

        static std::string result(Json const& requestId, Json value) {
            return answer(requestId, "result", std::move(value));
        }

        static std::string error(Json const& requestId, std::int64_t code, std::string const& message) {
            return answer(requestId, "error", Json::object_t{{"code", code}, {"message", message}});
        }

        // A tool call's result: the text, flagged as an error or not.
        static Json toolResult(std::string const& text, bool isError) {
            Json::array_t content;
            content.emplace_back(Json::object_t{{"type", "text"}, {"text", text}});
            return Json::object_t{{"content", std::move(content)}, {"isError", isError}};
        }

        static void dispatch(std::shared_ptr<Shared> const& self, std::string const& message, Reply const& done) {
            auto parsed = parseJson(message);
            if (!parsed) {
                done(error(Json{}, kMcpParseError, "not JSON: " + parsed.error()));
                return;
            }
            bool const isObject = std::holds_alternative<Json::object_t>(parsed->data);
            std::string const* const method = textMember(*parsed, "method");
            Json const* const givenId = member(*parsed, "id");
            Json const requestId = givenId == nullptr ? Json{} : *givenId;
            bool const notification = isObject && givenId == nullptr;
            if (method == nullptr) {
                if (notification) {
                    done(std::nullopt);
                } else {
                    done(error(requestId, kMcpInvalidRequest, "no method"));
                }
                return;
            }
            if (notification) {
                done(std::nullopt);
                return;
            }
            Json const* const givenParams = member(*parsed, "params");
            Json const params = givenParams == nullptr ? Json{} : *givenParams;
            if (*method == "initialize") {
                done(result(requestId, initialize(*self, params)));
            } else if (*method == "ping") {
                done(result(requestId, Json::object_t{}));
            } else if (*method == "tools/list") {
                listTools(self, [requestId, done](Json tools) { done(result(requestId, std::move(tools))); });
            } else if (*method == "tools/call") {
                callTool(self, params, [requestId, done](std::expected<Json, std::string> called) {
                    done(called ? result(requestId, std::move(*called))
                                : error(requestId, kMcpInvalidParams, called.error()));
                });
            } else {
                done(error(requestId, kMcpMethodNotFound, "method not found: " + *method));
            }
        }

        static Json initialize(Shared const& self, Json const& params) {
            std::string const* const asked = textMember(params, "protocolVersion");
            return Json::object_t{
                {"protocolVersion", asked != nullptr ? *asked : std::string{"2025-06-18"}},
                {"capabilities", Json::object_t{{"tools", Json::object_t{{"listChanged", false}}}}},
                {"serverInfo",
                 Json::object_t{{"name", self.options.serverName}, {"version", self.options.serverVersion}}},
            };
        }

        // The catalog for the session, fetched once; an unreadable or refused one is empty.
        static void withCatalog(std::shared_ptr<Shared> const& self, std::function<void(CatalogReply const&)> then) {
            // Copied under the lock and used after it: the continuation takes the lock again.
            std::optional<CatalogReply> cached;
            std::uint64_t generation = 0;
            {
                std::scoped_lock const lock{self->mutex};
                cached = self->catalog;
                generation = self->generation;
            }
            if (cached) {
                then(*cached);
                return;
            }
            wire::Envelope request;
            request.kind = std::string{kCatalogKind};
            send(self, std::move(request), [self, generation, then = std::move(then)](wire::Envelope const& reply) {
                // An unreadable or failed reply is empty for this call only: caching it would hide every tool
                // until the next reset.
                CatalogReply catalog;
                bool readable = false;
                if (reply.kind == "ok") {
                    if (auto read = parseCatalogReply(reply.body)) {
                        catalog = std::move(*read);
                        readable = true;
                    }
                }
                if (readable) {
                    std::scoped_lock const lock{self->mutex};
                    if (self->generation == generation) {
                        self->catalog = catalog;
                    }
                }
                then(catalog);
            });
        }

        [[nodiscard]] static bool keyed(CatalogReply const& catalog, std::string const& modelType) {
            if (!catalog.models) {
                return false;
            }
            auto const found = catalog.models->find(modelType);
            return found != catalog.models->end() && found->second.keyed;
        }

        [[nodiscard]] static std::string modelOf(CatalogReply const& catalog, std::string_view actionId) {
            if (!catalog.actions) {
                return {};
            }
            auto const found = catalog.actions->find(actionId);
            return found == catalog.actions->end() ? std::string{} : found->second.modelType;
        }

        // One tool: the action's schema, with `instance` added for a keyed model.
        static Json tool(std::string const& actionId, Json schema, bool keyedModel) {
            if (!std::holds_alternative<Json::object_t>(schema.data)) {
                schema = Json::object_t{};
            }
            auto& object = std::get<Json::object_t>(schema.data);
            object.insert_or_assign("type", "object");
            if (keyedModel) {
                objectMember(object, "properties")
                    .insert_or_assign(
                        "instance",
                        Json::object_t{{"type", "string"}, {"description", "the key of the instance to run on"}});
                Json& required = object.try_emplace("required", Json::array_t{}).first->second;
                if (!std::holds_alternative<Json::array_t>(required.data)) {
                    required = Json::array_t{};
                }
                std::get<Json::array_t>(required.data).emplace_back("instance");
            }
            return Json::object_t{{"name", actionId},
                                  {"description", "Runs " + actionId + " on the server."},
                                  {"inputSchema", std::move(schema)}};
        }

        static Json evaluateTool() {
            Json::object_t properties{{"action", Json::object_t{{"type", "string"}}},
                                      {"body", Json::object_t{{"type", "object"}}}};
            Json::array_t required{std::string{"action"}, std::string{"body"}};
            return Json::object_t{
                {"name", "evaluate"},
                {"description",
                 "A dry run: the action's body reconciled, its field errors, and whether it would run."},
                {"inputSchema",
                 Json::object_t{
                     {"type", "object"}, {"properties", std::move(properties)}, {"required", std::move(required)}}},
            };
        }

        // The tool list: one tool per permitted action of a known model, then `evaluate`.
        static Json toolList(CatalogReply const& catalog, std::map<std::string, Json, std::less<>> const& schemas) {
            Json::array_t tools;
            for (std::string const& action : catalog.permitted) {
                std::string const type = modelOf(catalog, action);
                if (type.empty()) {
                    continue;
                }
                auto const found = schemas.find(action);
                tools.push_back(tool(action, found == schemas.end() ? Json{} : found->second, keyed(catalog, type)));
            }
            tools.push_back(evaluateTool());
            return Json::object_t{{"tools", std::move(tools)}};
        }

        // Folds one model's `schemas` reply into @p gather; true when it was the last one awaited.
        [[nodiscard]] static bool gathered(Gather& gather, wire::Envelope const& reply) {
            std::scoped_lock const lock{gather.mutex};
            auto parsed = reply.kind == "ok" ? parseJson(reply.body) : std::unexpected(std::string{});
            if (parsed) {
                if (auto* const actions = std::get_if<Json::object_t>(&parsed->data)) {
                    for (auto& [action, schema] : *actions) {
                        gather.schemas.insert_or_assign(action, std::move(schema));
                    }
                }
            }
            return --gather.waiting == 0;
        }

        static void listTools(std::shared_ptr<Shared> const& self, std::function<void(Json)> done) {
            withCatalog(self, [self, done = std::move(done)](CatalogReply const& catalog) {
                std::set<std::string, std::less<>> types;
                for (std::string const& action : catalog.permitted) {
                    if (std::string type = modelOf(catalog, action); !type.empty()) {
                        types.insert(std::move(type));
                    }
                }
                if (types.empty()) {
                    done(toolList(catalog, {}));
                    return;
                }
                auto const gather = std::make_shared<Gather>();
                gather->waiting = types.size();
                for (std::string const& type : types) {
                    send(self, wire::makeSchemas(type), [catalog, done, gather](wire::Envelope const& reply) {
                        if (gathered(*gather, reply)) {
                            done(toolList(catalog, gather->schemas));
                        }
                    });
                }
            });
        }

        // The model id a tool call runs on, registering the instance at its first use.
        static void withInstance(std::shared_ptr<Shared> const& self, std::string const& type,
                                 std::optional<std::string> const& key,
                                 std::function<void(std::expected<std::uint64_t, std::string>)> then) {
            std::optional<std::uint64_t> cached;
            std::uint64_t generation = 0;
            {
                std::scoped_lock const lock{self->mutex};
                generation = self->generation;
                if (key) {
                    if (auto const found = self->sharedIds.find(std::pair{type, *key});
                        found != self->sharedIds.end()) {
                        cached = found->second;
                    }
                } else if (auto const found = self->privateIds.find(type); found != self->privateIds.end()) {
                    cached = found->second;
                }
            }
            if (cached) {
                then(*cached);
                return;
            }
            wire::Envelope request = key ? wire::makeRegisterShared(type, *key) : wire::makeRegister(type);
            send(self, std::move(request),
                 [self, generation, type, key, then = std::move(then)](wire::Envelope const& reply) {
                     if (reply.kind != "ok" || reply.modelId == 0) {
                         then(std::unexpected(reply.message.empty() ? std::string{"the instance was not registered"}
                                                                    : reply.message));
                         return;
                     }
                     {
                         std::scoped_lock const lock{self->mutex};
                         if (self->generation == generation) {
                             if (key) {
                                 self->sharedIds.insert_or_assign(std::pair{type, *key}, reply.modelId);
                             } else {
                                 self->privateIds.insert_or_assign(type, reply.modelId);
                             }
                         }
                     }
                     then(reply.modelId);
                 });
        }

        [[nodiscard]] static std::optional<std::string> instanceKey(Json const* value) {
            if (value == nullptr || std::holds_alternative<std::nullptr_t>(value->data)) {
                return std::nullopt;
            }
            if (auto const* const text = std::get_if<std::string>(&value->data)) {
                return *text;
            }
            return write(*value);
        }

        static void callTool(std::shared_ptr<Shared> const& self, Json const& params,
                             std::function<void(std::expected<Json, std::string>)> done) {
            std::string const* const name = textMember(params, "name");
            if (name == nullptr) {
                done(std::unexpected(std::string{"tools/call names no tool"}));
                return;
            }
            Json const* const given = member(params, "arguments");
            // Parentheses: braces would pick the initializer-list constructor and make `[{}]`.
            Json arguments = given == nullptr ? Json(Json::object_t{}) : *given;
            if (!std::holds_alternative<Json::object_t>(arguments.data)) {
                done(std::unexpected(std::string{"the arguments are not an object"}));
                return;
            }
            withCatalog(self, [self, tool = *name, arguments = std::move(arguments),
                               done = std::move(done)](CatalogReply const& catalog) mutable {
                if (tool == "evaluate") {
                    evaluate(self, catalog, arguments, std::move(done));
                    return;
                }
                std::string const type = modelOf(catalog, tool);
                if (type.empty()) {
                    done(std::unexpected("no tool named " + tool));
                    return;
                }
                std::optional<std::string> key;
                if (keyed(catalog, type)) {
                    key = instanceKey(member(arguments, "instance"));
                    if (!key) {
                        done(std::unexpected(tool + " runs on a keyed model: give the instance key as `instance`"));
                        return;
                    }
                    std::get<Json::object_t>(arguments.data).erase("instance");
                }
                std::string body = write(arguments);
                withInstance(
                    self, type, key,
                    [self, type, tool, body = std::move(body),
                     done = std::move(done)](std::expected<std::uint64_t, std::string> const& modelId) mutable {
                        if (!modelId) {
                            done(toolResult(modelId.error(), true));
                            return;
                        }
                        wire::Envelope request;
                        request.kind = "execute";
                        request.modelId = *modelId;
                        request.modelType = type;
                        request.actionType = tool;
                        request.body = std::move(body);
                        send(self, std::move(request), [done = std::move(done)](wire::Envelope const& reply) {
                            done(reply.kind == "ok" ? toolResult(reply.body, false) : toolResult(reply.message, true));
                        });
                    });
            });
        }

        static void evaluate(std::shared_ptr<Shared> const& self, CatalogReply const& catalog, Json const& arguments,
                             std::function<void(std::expected<Json, std::string>)> done) {
            std::string const* const action = textMember(arguments, "action");
            if (action == nullptr || modelOf(catalog, *action).empty()) {
                done(std::unexpected(std::string{"evaluate names no action the catalog knows"}));
                return;
            }
            Json const* const body = member(arguments, "body");
            wire::Envelope request;
            request.kind = std::string{kEvaluateKind};
            request.modelType = modelOf(catalog, *action);
            request.actionType = *action;
            request.body = body == nullptr ? std::string{"{}"} : write(*body);
            send(self, std::move(request), [done = std::move(done)](wire::Envelope const& reply) {
                done(reply.kind == "ok" ? toolResult(reply.body, false) : toolResult(reply.message, true));
            });
        }
    };

    std::shared_ptr<Shared> _shared;
    std::unique_ptr<async::detail::TimeoutScheduler> _timer;
};

/// @brief Serves an adapter over MCP's stdio transport: one JSON-RPC message per line of @p input, each reply on a
///        line of @p output, in order, until @p input ends. An empty line is skipped.
///
/// The calling thread is the adapter's owner: it pumps @p owner while it waits for each reply.
/// @param adapter The adapter; its `McpOptions::callbacks` is @p owner.
/// @param owner   The adapter's owner, run on the calling thread.
/// @param input   Where messages arrive.
/// @param output  Where replies go; flushed after each.
inline void serveStdio(McpAdapter& adapter, exec::MainThreadExecutor& owner, std::istream& input,
                       std::ostream& output) {
    std::string line;
    while (std::getline(input, line)) {
        if (line.empty()) {
            continue;
        }
        std::optional<std::optional<std::string>> replied;
        adapter.handle(line).then([&replied](std::optional<std::string> const& answer) { replied = answer; });
        while (!replied) {
            owner.runFor(std::chrono::milliseconds{10});
        }
        if (*replied) {
            output << **replied << '\n' << std::flush;
        }
    }
}

/// @brief Serves an adapter over MCP's stdio transport when its owner is turned by another thread, such as a Qt
///        event loop: each message's `handle` is posted to @p owner, and the calling thread waits for its reply.
///        One JSON-RPC message per line of @p input, each reply on a line of @p output, in order, until @p input
///        ends. An empty line is skipped.
///
/// The owner must keep running until @p input ends: a reply is delivered on it, and this call waits for each one.
/// @param adapter The adapter; its `McpOptions::callbacks` is @p owner.
/// @param owner   The adapter's owner, a serial executor another thread runs.
/// @param input   Where messages arrive.
/// @param output  Where replies go; flushed after each.
inline void serveStdioOnOwner(McpAdapter& adapter, exec::IExecutor& owner, std::istream& input, std::ostream& output) {
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
        auto const& reply = replied.value();
        if (reply) {
            output << *reply << '\n' << std::flush;
        }
    }
}

}  // namespace morph::ui

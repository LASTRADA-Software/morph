// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <atomic>
#include <chrono>
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
#include "../core/executor.hpp"
#include "../core/remote.hpp"
#include "../core/wire.hpp"
#include "../forms/forms.hpp"
#include "../session/session.hpp"
#include "bundle.hpp"
#include "value.hpp"

/// @file
/// The MCP adapter (spec 8 §11): an agent's tool calls run through the server as ordinary envelopes on a
/// connection of their own, so every check a GUI gets applies to them, and every GUI watching what they change is
/// told. The adapter speaks MCP's JSON-RPC 2.0 and knows no transport; `serveStdio` drives it over MCP's stdio
/// transport.

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
/// - **The connection** is opened when the adapter is made and closed when it is destroyed, which releases its
///   private instances. The server treats it like any other connection, so its changes reach every watching GUI.
///
/// `handle` is called on the owner, `McpOptions::callbacks`, and its completion is delivered there.
class McpAdapter {
public:
    /// @brief An adapter on a new connection of @p server.
    /// @param server  The server; the adapter keeps it alive.
    /// @param options Its options.
    /// @throws std::invalid_argument without a server or a callbacks executor.
    McpAdapter(std::shared_ptr<backend::RemoteServer> server, McpOptions options)
        : _shared{std::make_shared<Shared>(std::move(server), std::move(options))} {
        if (_shared->server == nullptr || _shared->options.callbacks == nullptr) {
            throw std::invalid_argument{"McpAdapter: a server and a callbacks executor are required"};
        }
        _shared->cid = _shared->server->openConnection();
    }

    /// @brief Closes the adapter's connection, releasing its private instances.
    ~McpAdapter() {
        // A destructor must not throw, so a close that fails is dropped.
        try {
            _shared->server->closeConnection(_shared->cid);
        } catch (...) {  // NOLINT(bugprone-empty-catch)
        }
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
        Shared(std::shared_ptr<backend::RemoteServer> made, McpOptions given)
            : server{std::move(made)}, options{std::move(given)} {}

        std::shared_ptr<backend::RemoteServer> server;
        McpOptions options;
        backend::ConnectionId cid{};
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
            self->server->handle(
                wire::encode(env),
                [done = std::move(done)](std::string const& reply) {
                    try {
                        done(wire::decode(reply));
                    } catch (std::exception const& error) {
                        done(wire::makeErr(error.what()));
                    }
                },
                self->cid);
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
            {
                std::scoped_lock const lock{self->mutex};
                cached = self->catalog;
            }
            if (cached) {
                then(*cached);
                return;
            }
            wire::Envelope request;
            request.kind = std::string{kCatalogKind};
            send(self, std::move(request), [self, then = std::move(then)](wire::Envelope const& reply) {
                CatalogReply catalog;
                if (reply.kind == "ok") {
                    if (auto read = parseCatalogReply(reply.body)) {
                        catalog = std::move(*read);
                    }
                }
                {
                    std::scoped_lock const lock{self->mutex};
                    self->catalog = catalog;
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
            {
                std::scoped_lock const lock{self->mutex};
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
            send(self, std::move(request), [self, type, key, then = std::move(then)](wire::Envelope const& reply) {
                if (reply.kind != "ok" || reply.modelId == 0) {
                    then(std::unexpected(reply.message.empty() ? std::string{"the instance was not registered"}
                                                               : reply.message));
                    return;
                }
                {
                    std::scoped_lock const lock{self->mutex};
                    if (key) {
                        self->sharedIds.insert_or_assign(std::pair{type, *key}, reply.modelId);
                    } else {
                        self->privateIds.insert_or_assign(type, reply.modelId);
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

}  // namespace morph::ui

// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <exception>
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
    ~McpAdapter() { _shared->server->closeConnection(_shared->cid); }

    McpAdapter(McpAdapter const&) = delete;
    McpAdapter& operator=(McpAdapter const&) = delete;
    McpAdapter(McpAdapter&&) = delete;
    McpAdapter& operator=(McpAdapter&&) = delete;

    /// @brief Handles one JSON-RPC message.
    /// @param message The message's JSON text.
    /// @return Completes with the reply's JSON text, or with nothing for a notification. It never fails: a fault is
    ///         answered as a JSON-RPC error.
    [[nodiscard]] async::Completion<std::optional<std::string>> handle(std::string message) {
        auto [completion, promise] =
            async::Completion<std::optional<std::string>>::makeSettleable(_shared->options.callbacks);
        auto settle = std::make_shared<decltype(promise)>(std::move(promise));
        Shared::dispatch(_shared, std::move(message),
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

        static std::string result(Json const& id, Json value) {
            Json reply;
            reply["jsonrpc"] = "2.0";
            reply["id"] = id;
            reply["result"] = std::move(value);
            return write(reply);
        }

        static std::string error(Json const& id, std::int64_t code, std::string const& message) {
            Json reply;
            reply["jsonrpc"] = "2.0";
            reply["id"] = id;
            reply["error"]["code"] = code;
            reply["error"]["message"] = message;
            return write(reply);
        }

        // A tool call's result: the text, flagged as an error or not.
        static Json toolResult(std::string text, bool isError) {
            Json content;
            content["type"] = "text";
            content["text"] = std::move(text);
            Json out;
            out["content"] = Json::array_t{std::move(content)};
            out["isError"] = isError;
            return out;
        }

        static void dispatch(std::shared_ptr<Shared> const& self, std::string const& message, Reply done) {
            auto parsed = parseJson(message);
            if (!parsed) {
                done(error(Json{nullptr}, kMcpParseError, "not JSON: " + parsed.error()));
                return;
            }
            Json::object_t const* const request = std::get_if<Json::object_t>(&parsed->data);
            std::string const* const method = request != nullptr && request->contains("method")
                                                  ? std::get_if<std::string>(&request->find("method")->second.data)
                                                  : nullptr;
            bool const notification = request != nullptr && !request->contains("id");
            Json const id =
                request != nullptr && request->contains("id") ? request->find("id")->second : Json{nullptr};
            if (method == nullptr) {
                if (notification && request != nullptr) {
                    done(std::nullopt);
                } else {
                    done(error(id, kMcpInvalidRequest, "no method"));
                }
                return;
            }
            if (notification) {
                done(std::nullopt);
                return;
            }
            Json const params = request->contains("params") ? request->find("params")->second : Json{};
            if (*method == "initialize") {
                done(result(id, initialize(*self, params)));
            } else if (*method == "ping") {
                done(result(id, Json::object_t{}));
            } else if (*method == "tools/list") {
                listTools(self, [id, done](Json tools) { done(result(id, std::move(tools))); });
            } else if (*method == "tools/call") {
                callTool(self, params, [id, done](std::expected<Json, std::string> called) {
                    done(called ? result(id, std::move(*called)) : error(id, kMcpInvalidParams, called.error()));
                });
            } else {
                done(error(id, kMcpMethodNotFound, "method not found: " + *method));
            }
        }

        static Json initialize(Shared const& self, Json const& params) {
            Json out;
            std::string const* const asked = params.contains("protocolVersion")
                                                 ? std::get_if<std::string>(&params["protocolVersion"].data)
                                                 : nullptr;
            out["protocolVersion"] = asked != nullptr ? *asked : std::string{"2025-06-18"};
            out["capabilities"]["tools"]["listChanged"] = false;
            out["serverInfo"]["name"] = self.options.serverName;
            out["serverInfo"]["version"] = self.options.serverVersion;
            return out;
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
            schema["type"] = "object";
            if (keyedModel) {
                schema["properties"]["instance"]["type"] = "string";
                schema["properties"]["instance"]["description"] = "the key of the instance to run on";
                if (!schema.contains("required") || !std::holds_alternative<Json::array_t>(schema["required"].data)) {
                    schema["required"] = Json::array_t{};
                }
                std::get<Json::array_t>(schema["required"].data).emplace_back("instance");
            }
            Json out;
            out["name"] = actionId;
            out["description"] = "Runs " + actionId + " on the server.";
            out["inputSchema"] = std::move(schema);
            return out;
        }

        static Json evaluateTool() {
            Json out;
            out["name"] = "evaluate";
            out["description"] =
                "A dry run: the action's body reconciled, its field errors, and whether it would run.";
            out["inputSchema"]["type"] = "object";
            out["inputSchema"]["properties"]["action"]["type"] = "string";
            out["inputSchema"]["properties"]["body"]["type"] = "object";
            out["inputSchema"]["required"] = Json::array_t{Json{"action"}, Json{"body"}};
            return out;
        }

        static void listTools(std::shared_ptr<Shared> const& self, std::function<void(Json)> done) {
            withCatalog(self, [self, done = std::move(done)](CatalogReply const& catalog) {
                std::set<std::string, std::less<>> types;
                for (std::string const& action : catalog.permitted) {
                    if (std::string type = modelOf(catalog, action); !type.empty()) {
                        types.insert(std::move(type));
                    }
                }
                // The schemas of every model a permitted action belongs to, then the tools.
                struct Gather {
                    std::mutex mutex;
                    std::size_t waiting = 0;
                    std::map<std::string, Json, std::less<>> schemas;
                };
                auto const gather = std::make_shared<Gather>();
                gather->waiting = types.size();
                auto const finish = [self, catalog, done, gather] {
                    Json::array_t tools;
                    for (std::string const& action : catalog.permitted) {
                        std::string const type = modelOf(catalog, action);
                        if (type.empty()) {
                            continue;
                        }
                        auto const found = gather->schemas.find(action);
                        tools.push_back(tool(action, found == gather->schemas.end() ? Json{} : found->second,
                                             keyed(catalog, type)));
                    }
                    tools.push_back(evaluateTool());
                    Json out;
                    out["tools"] = std::move(tools);
                    done(std::move(out));
                };
                if (types.empty()) {
                    finish();
                    return;
                }
                for (std::string const& type : types) {
                    send(self, wire::makeSchemas(type), [gather, finish](wire::Envelope const& reply) {
                        bool last = false;
                        {
                            std::scoped_lock const lock{gather->mutex};
                            if (reply.kind == "ok") {
                                if (auto parsed = parseJson(reply.body)) {
                                    if (auto* const actions = std::get_if<Json::object_t>(&parsed->data)) {
                                        for (auto& [action, schema] : *actions) {
                                            gather->schemas.insert_or_assign(action, std::move(schema));
                                        }
                                    }
                                }
                            }
                            last = --gather->waiting == 0;
                        }
                        if (last) {
                            finish();
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

        [[nodiscard]] static std::optional<std::string> instanceKey(Json const& value) {
            if (auto const* const text = std::get_if<std::string>(&value.data)) {
                return *text;
            }
            if (std::holds_alternative<std::nullptr_t>(value.data)) {
                return std::nullopt;
            }
            return write(value);
        }

        static void callTool(std::shared_ptr<Shared> const& self, Json const& params,
                             std::function<void(std::expected<Json, std::string>)> done) {
            std::string const* const name =
                params.contains("name") ? std::get_if<std::string>(&params["name"].data) : nullptr;
            if (name == nullptr) {
                done(std::unexpected(std::string{"tools/call names no tool"}));
                return;
            }
            Json arguments = params.contains("arguments") ? params["arguments"] : Json::object_t{};
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
                auto& fields = std::get<Json::object_t>(arguments.data);
                std::optional<std::string> key;
                if (keyed(catalog, type)) {
                    auto const found = fields.find("instance");
                    key = found == fields.end() ? std::nullopt : instanceKey(found->second);
                    if (!key) {
                        done(std::unexpected(tool + " runs on a keyed model: give the instance key as `instance`"));
                        return;
                    }
                    fields.erase("instance");
                }
                std::string body = write(arguments);
                withInstance(
                    self, type, key,
                    [self, type, tool, body = std::move(body),
                     done = std::move(done)](std::expected<std::uint64_t, std::string> const& id) mutable {
                        if (!id) {
                            done(toolResult(id.error(), true));
                            return;
                        }
                        wire::Envelope request;
                        request.kind = "execute";
                        request.modelId = *id;
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
            std::string const* const action =
                arguments.contains("action") ? std::get_if<std::string>(&arguments["action"].data) : nullptr;
            std::string const type = action == nullptr ? std::string{} : modelOf(catalog, *action);
            if (type.empty()) {
                done(std::unexpected(std::string{"evaluate names no action the catalog knows"}));
                return;
            }
            wire::Envelope request;
            request.kind = std::string{kEvaluateKind};
            request.modelType = type;
            request.actionType = *action;
            request.body = arguments.contains("body") ? write(arguments["body"]) : std::string{"{}"};
            send(self, std::move(request), [done = std::move(done)](wire::Envelope const& reply) {
                done(reply.kind == "ok" ? toolResult(reply.body, false) : toolResult(reply.message, true));
            });
        }
    };

    std::shared_ptr<Shared> _shared;
};

/// @brief Serves an adapter over MCP's stdio transport: one JSON-RPC message per line of @p in, each reply on a
///        line of @p out, in order, until @p in ends. An empty line is skipped.
///
/// The calling thread is the adapter's owner: it pumps @p owner while it waits for each reply.
/// @param adapter The adapter; its `McpOptions::callbacks` is @p owner.
/// @param owner   The adapter's owner, run on the calling thread.
/// @param in      Where messages arrive.
/// @param out     Where replies go; flushed after each.
inline void serveStdio(McpAdapter& adapter, exec::MainThreadExecutor& owner, std::istream& in, std::ostream& out) {
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty()) {
            continue;
        }
        std::optional<std::optional<std::string>> replied;
        adapter.handle(line).then([&replied](std::optional<std::string> const& answer) { replied = answer; });
        while (!replied) {
            owner.runFor(std::chrono::milliseconds{10});
        }
        if (*replied) {
            out << **replied << '\n' << std::flush;
        }
    }
}

}  // namespace morph::ui

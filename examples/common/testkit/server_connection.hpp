// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <future>
#include <glaze/glaze.hpp>
#include <memory>
#include <morph/core/connection_writer.hpp>
#include <morph/core/remote.hpp>
#include <morph/core/wire.hpp>
#include <morph/session/session.hpp>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

/// @file
/// A connection opened on a `RemoteServer` in process, as a socket server opens one for a client: it registers
/// models, executes, watches records and closes, and keeps every event the server writes to it. `BackendRig::connect`
/// hands these out in `Mode::Socket`, so a test can close one connection while others stay and see what each was
/// told.

namespace morph::ladder::testkit {

/// @brief Negotiates server-sent events for the connection its kind arrives on, as the UI service's `ui-hello` does.
class EventsHello final : public ::morph::backend::IEnvelopeService {
public:
    using IEnvelopeService::handle;

    /// @brief The kind it serves.
    static constexpr std::string_view kKind = "testkit-events-hello";

    /// @brief Whether it serves @p kind.
    /// @param kind The envelope's kind.
    /// @return `true` for `testkit-events-hello`.
    [[nodiscard]] bool serves(std::string_view kind) const override { return kind == kKind; }

    /// @brief Answers `ok`.
    /// @param request The envelope.
    /// @return The reply.
    [[nodiscard]] std::string handle(::morph::wire::Envelope const& request,
                                     ::morph::session::IAuthorizer const& /*authorizer*/) override {
        return ::morph::wire::encode(::morph::wire::makeOk(request.callId));
    }

    /// @brief Answers `ok` and records that the connection takes events.
    /// @param request The envelope.
    /// @param authorizer The authorizer.
    /// @param facts The connection's facts.
    /// @return The reply.
    [[nodiscard]] std::string handle(::morph::wire::Envelope const& request,
                                     ::morph::session::IAuthorizer const& authorizer,
                                     ::morph::backend::ConnectionFacts& facts) override {
        facts.events = true;
        return handle(request, authorizer);
    }
};

/// @brief One connection on a server, opened in process, and every event the server sent it.
///
/// Destroy it before the rig that made it: the server it holds runs on the rig's workers.
class ServerConnection {
public:
    /// @brief Opens a connection on @p server that takes events.
    /// @param server The server; its config must serve `EventsHello`, as `BackendRig`'s does.
    explicit ServerConnection(std::shared_ptr<::morph::backend::RemoteServer> server)
        : _server{std::move(server)}, _sent{std::make_shared<Sent>()}, _cid{_server->openConnection()} {
        _server->setConnectionWriter(_cid, {
                                               .write =
                                                   [sent = _sent](std::string message) {
                                                       std::scoped_lock const lock{sent->mutex};
                                                       sent->messages.push_back(std::move(message));
                                                   },
                                               .backlog = [] { return std::size_t{0}; },
                                           });
        ::morph::wire::Envelope hello;
        hello.kind = std::string{EventsHello::kKind};
        hello.callId = nextCall();
        _takesEvents = ::morph::wire::decode(_server->handleInline(::morph::wire::encode(hello), _cid)).kind == "ok";
    }

    // closeConnection only releases the connection's scope; nothing it does is expected to throw.
    // NOLINTNEXTLINE(bugprone-exception-escape)
    ~ServerConnection() { close(); }
    ServerConnection(ServerConnection const&) = delete;
    ServerConnection& operator=(ServerConnection const&) = delete;
    ServerConnection(ServerConnection&&) = delete;
    ServerConnection& operator=(ServerConnection&&) = delete;

    /// @brief Whether the server accepted this connection as one that takes events.
    /// @return `true` when it did; `false` when the server does not serve `EventsHello`.
    [[nodiscard]] bool takesEvents() const noexcept { return _takesEvents; }

    /// @brief Registers a private instance of @p typeId on this connection.
    /// @param typeId The model's registered id.
    /// @param session Who registers it.
    /// @return The instance id; 0 when the server refused.
    std::uint64_t registerModel(std::string_view typeId, ::morph::session::Context const& session) {
        ::morph::wire::Envelope env;
        env.kind = "register";
        env.callId = nextCall();
        env.typeId = std::string{typeId};
        env.session = session;
        return request(env).modelId;
    }

    /// @brief Executes @p actionType with @p body on instance @p modelId of @p modelType.
    /// @param modelId The instance `registerModel` gave.
    /// @param modelType The model's registered id.
    /// @param actionType The action's registered id.
    /// @param body The action as JSON.
    /// @param session Who executes it.
    /// @return The reply envelope (`ok` or `err`).
    // NOLINTNEXTLINE(bugprone-easily-swappable-parameters) -- the model and action ids are not interchangeable
    ::morph::wire::Envelope execute(std::uint64_t modelId, std::string_view modelType, std::string_view actionType,
                                    std::string body, ::morph::session::Context const& session) {
        ::morph::wire::Envelope env;
        env.kind = "execute";
        env.callId = nextCall();
        env.modelId = modelId;
        env.modelType = std::string{modelType};
        env.actionType = std::string{actionType};
        env.body = std::move(body);
        env.session = session;
        return request(env);
    }

    /// @brief Adds @p records to this connection's watch set.
    /// @param records Record wire names.
    /// @param session Who watches.
    /// @return The reply.
    ::morph::wire::Envelope watch(std::vector<std::string> records, ::morph::session::Context const& session) {
        ::morph::wire::Envelope env;
        env.kind = std::string{::morph::wire::kWatchKind};
        env.callId = nextCall();
        env.session = session;
        env.body =
            glz::write_json(::morph::wire::WatchBody{.records = std::move(records), .instances = {}, .replace = false})
                .value_or(std::string{});
        return ::morph::wire::decode(_server->handleInline(::morph::wire::encode(env), _cid));
    }

    /// @brief Every `changes` event the server sent this connection, decoded, in order.
    /// @return The bodies.
    [[nodiscard]] std::vector<::morph::wire::ChangesBody> changes() const {
        std::vector<std::string> messages;
        {
            std::scoped_lock const lock{_sent->mutex};
            messages = _sent->messages;
        }
        std::vector<::morph::wire::ChangesBody> bodies;
        for (auto const& message : messages) {
            auto const env = ::morph::wire::decode(message);
            if (env.kind != ::morph::wire::kEventKind) {
                continue;
            }
            ::morph::wire::EventBody event;
            if (glz::read_json(event, env.body) || event.topic != ::morph::wire::kChangesTopic) {
                continue;
            }
            if (auto body = ::morph::wire::readChangesBody(event.payload.str)) {
                bodies.push_back(std::move(*body));
            }
        }
        return bodies;
    }

    /// @brief Whether any `changes` event sent so far names @p record.
    /// @param record A record wire name.
    /// @return `true` when one does.
    [[nodiscard]] bool heardOf(std::string_view record) const {
        for (auto const& body : changes()) {
            for (auto const& change : body.records) {
                if (change.record == record) {
                    return true;
                }
            }
        }
        return false;
    }

    /// @brief Polls @p pred until it holds or @p budget passes.
    /// @tparam Pred A callable returning `bool`.
    /// @param pred The condition.
    /// @param budget How long to wait.
    /// @return Whether it held.
    template <class Pred>
    [[nodiscard]] static bool eventually(Pred pred,
                                         std::chrono::milliseconds budget = std::chrono::milliseconds{3000}) {
        auto const deadline = std::chrono::steady_clock::now() + budget;
        while (std::chrono::steady_clock::now() < deadline) {
            if (pred()) {
                return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds{10});
        }
        return pred();
    }

    /// @brief Closes the connection, as the transport does when its socket goes. Idempotent.
    void close() {
        if (!_closed.exchange(true)) {
            _server->closeConnection(_cid);
        }
    }

private:
    struct Sent {
        std::mutex mutex;
        std::vector<std::string> messages;
    };

    [[nodiscard]] std::uint64_t nextCall() { return ++_callId; }

    ::morph::wire::Envelope request(::morph::wire::Envelope const& env) {
        auto const promise = std::make_shared<std::promise<std::string>>();
        auto answer = promise->get_future();
        _server->handle(
            ::morph::wire::encode(env), [promise](std::string reply) { promise->set_value(std::move(reply)); }, _cid);
        return ::morph::wire::decode(answer.get());
    }

    std::shared_ptr<::morph::backend::RemoteServer> _server;
    std::shared_ptr<Sent> _sent;
    ::morph::backend::ConnectionId _cid;
    std::uint64_t _callId = 0;
    bool _takesEvents = false;
    std::atomic<bool> _closed{false};
};

}  // namespace morph::ladder::testkit

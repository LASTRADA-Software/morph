// SPDX-License-Identifier: Apache-2.0
//
// The configurable model provider, against a local server replaying canned server-sent events. No network.

#include <QByteArray>
#include <QCoreApplication>
#include <QEventLoop>
#include <QHostAddress>
#include <QObject>
#include <QTcpServer>
#include <QTcpSocket>
#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_tostring.hpp>
#include <cctype>
#include <chrono>
#include <expected>
#include <functional>
#include <memory>
#include <morph/qt/qt_chat_provider.hpp>
#include <morph/ui/chat/provider.hpp>
#include <morph/ui/value.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace chat = morph::ui::chat;

namespace {

constexpr std::string_view kKey = "sk-test-secret-key";

bool pumpUntil(std::function<bool()> const& done, std::chrono::milliseconds limit = std::chrono::seconds{10}) {
    auto const deadline = std::chrono::steady_clock::now() + limit;
    while (!done()) {
        if (std::chrono::steady_clock::now() > deadline) {
            return false;
        }
        QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
    }
    return true;
}

// Answers each request with a canned response, and keeps what it was sent.
class FakeServer {
public:
    explicit FakeServer(std::string response) : _response{std::move(response)} {
        REQUIRE(_server.listen(QHostAddress::LocalHost));
        QObject::connect(&_server, &QTcpServer::newConnection, &_server, [this] {
            while (QTcpSocket* const socket = _server.nextPendingConnection()) {
                QObject::connect(socket, &QTcpSocket::readyRead, socket, [this, socket] { read(*socket); });
                QObject::connect(socket, &QTcpSocket::disconnected, socket, [this, socket] {
                    disconnected = true;
                    socket->deleteLater();
                });
            }
        });
    }

    [[nodiscard]] std::string url() const { return "http://127.0.0.1:" + std::to_string(_server.serverPort()); }

    // Hold the response's end, so the test can act during the call.
    bool holdOpen = false;
    bool disconnected = false;
    std::string received;

private:
    void read(QTcpSocket& socket) {
        received += socket.readAll().toStdString();
        auto const headersEnd = received.find("\r\n\r\n");
        if (headersEnd == std::string::npos) {
            return;
        }
        auto const at = received.find("content-length: ");
        auto const atCapital = received.find("Content-Length: ");
        auto const start = at != std::string::npos ? at : atCapital;
        REQUIRE(start != std::string::npos);
        std::size_t const length = std::stoul(received.substr(start + 16));
        if (received.size() < headersEnd + 4 + length || _answered) {
            return;
        }
        _answered = true;
        socket.write(_response.data(), static_cast<qint64>(_response.size()));
        socket.flush();
        if (!holdOpen) {
            socket.disconnectFromHost();
        }
    }

    QTcpServer _server;
    std::string _response;
    bool _answered = false;
};

std::string streamOf(std::string const& events) {
    return "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\nConnection: close\r\n\r\n" + events;
}

std::string event(std::string const& name, std::string const& data) {
    return "event: " + name + "\ndata: " + data + "\n\n";
}

std::string const kTextStream =
    event("message_start", R"({"type":"message_start","message":{"id":"m","role":"assistant","content":[]}})") +
    event("content_block_start",
          R"({"type":"content_block_start","index":0,"content_block":{"type":"text","text":""}})") +
    event("ping", R"({"type":"ping"})") +
    event("content_block_delta",
          R"({"type":"content_block_delta","index":0,"delta":{"type":"text_delta","text":"Hel"}})") +
    event("content_block_delta",
          R"({"type":"content_block_delta","index":0,"delta":{"type":"text_delta","text":"lo"}})") +
    event("content_block_stop", R"({"type":"content_block_stop","index":0})") +
    event("message_delta",
          R"({"type":"message_delta","delta":{"stop_reason":"end_turn"},"usage":{"output_tokens":2}})") +
    event("message_stop", R"({"type":"message_stop"})");

std::string const kToolStream =
    event(
        "content_block_start",
        R"({"type":"content_block_start","index":0,"content_block":{"type":"thinking","thinking":"","signature":""}})") +
    event("content_block_delta",
          R"({"type":"content_block_delta","index":0,"delta":{"type":"signature_delta","signature":"sig-1"}})") +
    event("content_block_stop", R"({"type":"content_block_stop","index":0})") +
    event(
        "content_block_start",
        R"({"type":"content_block_start","index":1,"content_block":{"type":"tool_use","id":"call-1","name":"lab__Add","input":{}}})") +
    event(
        "content_block_delta",
        R"({"type":"content_block_delta","index":1,"delta":{"type":"input_json_delta","partial_json":"{\"text\":"}})") +
    event("content_block_delta",
          R"({"type":"content_block_delta","index":1,"delta":{"type":"input_json_delta","partial_json":"\"hi\"}"}})") +
    event("content_block_stop", R"({"type":"content_block_stop","index":1})") +
    event("message_delta", R"({"type":"message_delta","delta":{"stop_reason":"tool_use"}})") +
    event("message_stop", R"({"type":"message_stop"})");

chat::ProviderConfig configFor(std::string url) {
    chat::ProviderConfig config;
    config.name = "anthropic";
    config.baseUrl = std::move(url);
    config.apiKey = std::string{kKey};
    config.model = "the-configured-model";
    return config;
}

struct Rig {
    QObject owner;
    std::unique_ptr<FakeServer> server;
    std::shared_ptr<chat::IProvider> provider;
    std::vector<std::string> pieces;
    std::optional<std::expected<chat::Response, std::string>> done;

    explicit Rig(std::string response) : server{std::make_unique<FakeServer>(std::move(response))} {
        auto made = morph::qt::makeQtChatProvider(configFor(server->url()), &owner);
        REQUIRE(made.has_value());
        provider = *made;
    }

    std::shared_ptr<chat::IProviderCall> call(chat::Request request = plainRequest()) {
        return provider->stream(
            std::move(request),
            {.text = [this](std::string const& piece) { pieces.push_back(piece); },
             .done = [this](std::expected<chat::Response, std::string> outcome) { done = std::move(outcome); }});
    }

    static chat::Request plainRequest() {
        chat::Request request;
        request.system = "be helpful";
        request.messages.push_back({.role = "user", .text = "hi", .calls = {}, .results = {}, .opaque = {}});
        return request;
    }

    void finish() {
        REQUIRE(pumpUntil([this] { return done.has_value(); }));
    }
};

}  // namespace

// Mutation: stop forwarding text deltas.
TEST_CASE("text_deltas_arrive_in_order_and_done_has_the_text", "[qt][chat_provider]") {
    Rig rig{streamOf(kTextStream)};
    static_cast<void>(rig.call());
    rig.finish();
    CHECK(rig.pieces == std::vector<std::string>{"Hel", "lo"});
    REQUIRE(rig.done->has_value());
    CHECK((*rig.done)->text == "Hello");
    CHECK((*rig.done)->calls.empty());
}

// Mutation: read the tool's input from the start block rather than its deltas.
TEST_CASE("a_tool_use_stream_gives_a_call_with_its_assembled_arguments", "[qt][chat_provider]") {
    Rig rig{streamOf(kToolStream)};
    static_cast<void>(rig.call());
    rig.finish();
    REQUIRE(rig.done->has_value());
    REQUIRE((*rig.done)->calls.size() == 1);
    auto const& call = (*rig.done)->calls.front();
    CHECK(call.id == "call-1");
    CHECK(call.name == "lab__Add");
    CHECK(call.argumentsJson == R"({"text":"hi"})");
}

// Mutation: rebuild the assistant message from text and calls alone, which drops what the model must get back.
TEST_CASE("a_response_keeps_what_must_be_replayed_unchanged", "[qt][chat_provider]") {
    Rig rig{streamOf(kToolStream)};
    static_cast<void>(rig.call());
    rig.finish();
    REQUIRE(rig.done->has_value());
    std::string const& opaque = (*rig.done)->opaque;
    CAPTURE(opaque);
    CHECK(opaque.contains("sig-1"));
    // Content blocks, directly, as they go back in a message's `content`.
    auto const replayed = morph::ui::parseJson(opaque);
    REQUIRE(replayed.has_value());
    REQUIRE(std::holds_alternative<morph::ui::Json::array_t>(replayed->data));
    CHECK(std::holds_alternative<morph::ui::Json::object_t>(
        std::get<morph::ui::Json::array_t>(replayed->data).front().data));
    CHECK(opaque.contains("call-1"));

    Rig next{streamOf(kTextStream)};
    chat::Request request = Rig::plainRequest();
    request.messages.push_back(
        {.role = "assistant", .text = {}, .calls = (*rig.done)->calls, .results = {}, .opaque = opaque});
    request.messages.push_back({.role = "user",
                                .text = {},
                                .calls = {},
                                .results = {chat::ToolResult{.callId = "call-1", .text = "added", .isError = false}},
                                .opaque = {}});
    static_cast<void>(next.call(std::move(request)));
    next.finish();
    CHECK(next.server->received.contains("sig-1"));
    CHECK(next.server->received.contains("tool_result"));
    CHECK(next.server->received.contains("call-1"));
}

// Mutation: leave out the key's header.
TEST_CASE("a_request_carries_system_tools_key_and_model", "[qt][chat_provider]") {
    Rig rig{streamOf(kTextStream)};
    chat::Request request = Rig::plainRequest();
    request.tools.push_back(
        {.name = "lab__Add", .description = "Adds", .inputSchema = morph::ui::Json::object_t{{"type", "object"}}});
    static_cast<void>(rig.call(std::move(request)));
    rig.finish();
    std::string const& sent = rig.server->received;
    CHECK(sent.starts_with("POST /v1/messages "));
    CHECK(sent.contains(kKey));
    // Header names are case-insensitive, and Qt may send them in either form.
    std::string lowered = sent;
    std::ranges::transform(lowered, lowered.begin(), [](unsigned char letter) { return std::tolower(letter); });
    CHECK(lowered.contains("anthropic-version: 2026-10-21"));
    CHECK(lowered.contains("x-api-key: "));
    // The body is the request object itself, and not wrapped in anything.
    auto const body = sent.substr(sent.find("\r\n\r\n") + 4);
    auto const parsed = morph::ui::parseJson(body);
    REQUIRE(parsed.has_value());
    CHECK(std::holds_alternative<morph::ui::Json::object_t>(parsed->data));
    CHECK(sent.contains("the-configured-model"));
    CHECK(sent.contains("be helpful"));
    CHECK(sent.contains("lab__Add"));
    CHECK(sent.contains("input_schema"));
    CHECK(sent.contains(R"("stream":true)"));
    CHECK(sent.contains(R"("max_tokens":64000)"));
}

// Mutation: put the configuration in the reason.
TEST_CASE("an_http_error_is_a_done_error_without_the_key", "[qt][chat_provider]") {
    std::string const body = R"({"type":"error","error":{"type":"authentication_error","message":"bad credentials"}})";
    Rig rig{"HTTP/1.1 401 Unauthorized\r\nContent-Type: application/json\r\nContent-Length: " +
            std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n" + body};
    static_cast<void>(rig.call());
    rig.finish();
    REQUIRE_FALSE(rig.done->has_value());
    CHECK(rig.done->error().contains("bad credentials"));
    CHECK_FALSE(rig.done->error().contains(kKey));
}

// Mutation: treat a closed connection as the end of the message.
TEST_CASE("a_truncated_stream_is_a_done_error", "[qt][chat_provider]") {
    Rig rig{streamOf(event("content_block_start",
                           R"({"type":"content_block_start","index":0,"content_block":{"type":"text","text":""}})") +
                     event("content_block_delta",
                           R"({"type":"content_block_delta","index":0,"delta":{"type":"text_delta","text":"par"}})"))};
    static_cast<void>(rig.call());
    rig.finish();
    CHECK_FALSE(rig.done->has_value());
}

// Mutation: let the parse failure throw.
TEST_CASE("a_garbage_event_is_a_done_error_not_a_throw", "[qt][chat_provider]") {
    Rig rig{streamOf(event("content_block_delta", "{not json"))};
    static_cast<void>(rig.call());
    rig.finish();
    CHECK_FALSE(rig.done->has_value());
}

// Mutation: ignore an error event in the stream.
TEST_CASE("an_error_event_is_a_done_error", "[qt][chat_provider]") {
    Rig rig{streamOf(event("error", R"({"type":"error","error":{"type":"overloaded_error","message":"busy now"}})"))};
    static_cast<void>(rig.call());
    rig.finish();
    REQUIRE_FALSE(rig.done->has_value());
    CHECK(rig.done->error().contains("busy now"));
}

// Mutation: return the empty content as an answer.
TEST_CASE("a_refusal_is_a_done_error", "[qt][chat_provider]") {
    Rig rig{streamOf(event("message_delta", R"({"type":"message_delta","delta":{"stop_reason":"refusal"}})") +
                     event("message_stop", R"({"type":"message_stop"})"))};
    static_cast<void>(rig.call());
    rig.finish();
    CHECK_FALSE(rig.done->has_value());
}

// Mutation: run the calls of a response cut off by its token limit.
TEST_CASE("a_call_cut_off_by_the_token_limit_is_a_done_error", "[qt][chat_provider]") {
    std::string stream = kToolStream;
    auto const at = stream.find(R"("stop_reason":"tool_use")");
    REQUIRE(at != std::string::npos);
    stream.replace(at, std::string_view{R"("stop_reason":"tool_use")"}.size(), R"("stop_reason":"max_tokens")");
    Rig rig{streamOf(stream)};
    static_cast<void>(rig.call());
    rig.finish();
    CHECK_FALSE(rig.done->has_value());
}

// Mutation: leave the reply running after a cancel.
TEST_CASE("cancel_aborts_the_request_and_sends_no_done", "[qt][chat_provider]") {
    Rig rig{
        streamOf(event("content_block_start",
                       R"({"type":"content_block_start","index":0,"content_block":{"type":"text","text":""}})") +
                 event("content_block_delta",
                       R"({"type":"content_block_delta","index":0,"delta":{"type":"text_delta","text":"first"}})"))};
    rig.server->holdOpen = true;
    auto const call = rig.call();
    REQUIRE(pumpUntil([&] { return !rig.pieces.empty(); }));
    call->cancel();
    CHECK(pumpUntil([&] { return rig.server->disconnected; }));
    QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    CHECK_FALSE(rig.done.has_value());
}

// Mutation: accept any name as the one provider there is.
TEST_CASE("an_unknown_name_or_missing_key_is_refused", "[qt][chat_provider]") {
    QObject owner;
    auto config = configFor("http://127.0.0.1:1");
    config.name = "no-such-provider";
    CHECK_FALSE(morph::qt::makeQtChatProvider(config, &owner).has_value());
    config = configFor("http://127.0.0.1:1");
    config.apiKey.clear();
    CHECK_FALSE(morph::qt::makeQtChatProvider(config, &owner).has_value());
    config = configFor("http://127.0.0.1:1");
    config.model.clear();
    CHECK_FALSE(morph::qt::makeQtChatProvider(config, &owner).has_value());
    config = configFor("");
    CHECK(morph::qt::makeQtChatProvider(config, &owner).has_value());
}

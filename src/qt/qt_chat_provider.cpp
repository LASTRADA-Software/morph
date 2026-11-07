// SPDX-License-Identifier: Apache-2.0
#include <QByteArray>
#include <QCoreApplication>
#include <QMetaObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QUrl>
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <expected>
#include <map>
#include <memory>
#include <morph/qt/qt_chat_provider.hpp>
#include <morph/ui/bundle.hpp>
#include <morph/ui/chat/provider.hpp>
#include <morph/ui/value.hpp>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace morph::qt {

namespace {

namespace chat = ui::chat;
using ui::Json;

// A call makes no progress for this long before it is given up on. Reset by every byte, so a long answer is not cut
// off.
constexpr int kTransferTimeoutMs = 300000;

// This API requires a limit, and a streamed answer has no timeout for a large one to run into.
constexpr int kDefaultMaxTokens = 64000;

constexpr std::string_view kPublicAddress = "https://api.anthropic.com";

[[nodiscard]] Json const* member(Json const& value, std::string_view key) {
    auto const* const object = std::get_if<Json::object_t>(&value.data);
    if (object == nullptr) {
        return nullptr;
    }
    auto const* const found = object->find(key);
    return found == object->end() ? nullptr : &found->second;
}

[[nodiscard]] std::string textOf(Json const& value, std::string_view key) {
    Json const* const found = member(value, key);
    auto const* const text = found == nullptr ? nullptr : std::get_if<std::string>(&found->data);
    return text == nullptr ? std::string{} : *text;
}

[[nodiscard]] Json objectOrEmpty(std::string_view text) {
    if (auto parsed = ui::parseJson(text); parsed && std::holds_alternative<Json::object_t>(parsed->data)) {
        return std::move(*parsed);
    }
    return Json::object_t{};
}

// An assistant message goes back as the provider gave it when it can be, so its reasoning stays valid. Otherwise, and
// for a person's message, it is built from the text, the calls and the results.
[[nodiscard]] Json contentOf(chat::Message const& message) {
    if (!message.opaque.empty()) {
        if (auto parsed = ui::parseJson(message.opaque);
            parsed && std::holds_alternative<Json::array_t>(parsed->data)) {
            return std::move(*parsed);
        }
    }
    Json::array_t blocks;
    for (chat::ToolResult const& result : message.results) {
        blocks.emplace_back(Json::object_t{{"type", "tool_result"},
                                           {"tool_use_id", result.callId},
                                           {"content", result.text},
                                           {"is_error", result.isError}});
    }
    if (!message.text.empty()) {
        blocks.emplace_back(Json::object_t{{"type", "text"}, {"text", message.text}});
    }
    for (chat::ToolCall const& call : message.calls) {
        blocks.emplace_back(Json::object_t{
            {"type", "tool_use"}, {"id", call.id}, {"name", call.name}, {"input", objectOrEmpty(call.argumentsJson)}});
    }
    return blocks;
}

[[nodiscard]] QByteArray bodyOf(chat::Request const& request, chat::ProviderConfig const& config) {
    Json::array_t messages;
    for (chat::Message const& message : request.messages) {
        Json content = contentOf(message);
        // A message with nothing in it is refused, and a turn stopped early can leave one behind.
        if (std::get<Json::array_t>(content.data).empty()) {
            continue;
        }
        messages.emplace_back(Json::object_t{{"role", message.role}, {"content", std::move(content)}});
    }
    Json::object_t body{{"model", config.model},
                        {"max_tokens", static_cast<std::int64_t>(config.maxTokens.value_or(kDefaultMaxTokens))},
                        {"stream", true},
                        {"system", request.system},
                        {"messages", std::move(messages)}};
    // Reruns a request a safety classifier declined on a model suited to that category. Only the provider's own
    // address is known to take it: a service speaking the same format elsewhere can refuse the whole request for it.
    if (config.baseUrl == kPublicAddress) {
        body.insert_or_assign("fallbacks", "default");
    }
    if (!request.tools.empty()) {
        Json::array_t tools;
        for (chat::ToolSpec const& tool : request.tools) {
            tools.emplace_back(Json::object_t{{"name", tool.name},
                                              {"description", tool.description},
                                              {"input_schema", tool.inputSchema},
                                              {"eager_input_streaming", true}});
        }
        body.insert_or_assign("tools", std::move(tools));
    }
    // Parentheses: braces would pick the initializer-list constructor and wrap the body in an array.
    return QByteArray::fromStdString(ui::detail::writeMessage(Json(std::move(body))));
}

// One call's progress. Runs on the owner's thread, except `cancel`.
class Call final : public chat::IProviderCall, public std::enable_shared_from_this<Call> {
public:
    Call(QPointer<QObject> owner, chat::ProviderSink sink) : _owner{std::move(owner)}, _sink{std::move(sink)} {}

    void cancel() override {
        _cancelled.store(true);
        QPointer<QObject> const owner = _owner;
        if (owner != nullptr) {
            QMetaObject::invokeMethod(
                owner.data(), [self = shared_from_this()] { self->abort(); }, Qt::QueuedConnection);
        }
    }

    void start(QNetworkAccessManager& network, QNetworkRequest const& request, QByteArray const& body) {
        if (_cancelled.load()) {
            return;
        }
        _reply = network.post(request, body);
        QObject::connect(_reply, &QNetworkReply::readyRead, _reply, [self = shared_from_this()] { self->consume(); });
        QObject::connect(_reply, &QNetworkReply::finished, _reply, [self = shared_from_this()] { self->finished(); });
    }

    void fail(std::string reason) {
        if (_ended) {
            return;
        }
        _ended = true;
        if (!_cancelled.load() && _sink.done) {
            _sink.done(std::unexpected(std::move(reason)));
        }
        abort();
    }

private:
    void abort() {
        _ended = true;
        if (_reply != nullptr) {
            _reply->abort();
        }
    }

    [[nodiscard]] int status() const {
        return _reply == nullptr ? 0 : _reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    }

    void consume() {
        if (_ended || _reply == nullptr) {
            return;
        }
        _buffer += _reply->readAll().toStdString();
        // An error's body is one JSON document, read when the reply finishes.
        if (status() >= 400) {
            return;
        }
        std::string::size_type end = 0;
        while (!_ended && (end = eventEnd()) != std::string::npos) {
            std::string const event = _buffer.substr(0, end);
            _buffer.erase(0, _buffer.find_first_not_of("\r\n", end));
            handleEvent(event);
        }
    }

    [[nodiscard]] std::string::size_type eventEnd() const {
        auto const plain = _buffer.find("\n\n");
        auto const crlf = _buffer.find("\r\n\r\n");
        return std::min(plain, crlf);
    }

    void handleEvent(std::string const& event) {
        std::string data;
        std::string::size_type position = 0;
        while (position < event.size()) {
            auto next = event.find('\n', position);
            next = next == std::string::npos ? event.size() : next;
            std::string_view line = std::string_view{event}.substr(position, next - position);
            if (line.ends_with('\r')) {
                line.remove_suffix(1);
            }
            if (line.starts_with("data:")) {
                line.remove_prefix(5);
                if (line.starts_with(' ')) {
                    line.remove_prefix(1);
                }
                data += line;
            }
            position = next + 1;
        }
        if (data.empty()) {
            return;
        }
        auto const parsed = ui::parseJson(data);
        if (!parsed) {
            fail("the model provider sent something unreadable");
            return;
        }
        handleData(*parsed);
    }

    void handleData(Json const& data) {
        std::string const type = textOf(data, "type");
        if (type == "content_block_start") {
            start(data);
        } else if (type == "content_block_delta") {
            delta(data);
        } else if (type == "message_delta") {
            if (Json const* const change = member(data, "delta")) {
                if (std::string reason = textOf(*change, "stop_reason"); !reason.empty()) {
                    _stopReason = std::move(reason);
                }
            }
        } else if (type == "message_stop") {
            stop();
        } else if (type == "error") {
            Json const* const error = member(data, "error");
            fail("the model provider failed: " +
                 (error == nullptr ? std::string{"no reason"} : textOf(*error, "message")));
        }
    }

    [[nodiscard]] static std::int64_t indexOf(Json const& data) {
        Json const* const found = member(data, "index");
        if (found == nullptr) {
            return -1;
        }
        // The JSON layer picks the number's representation, so this takes whichever one it chose.
        return std::visit(
            []<typename Held>(Held const& held) -> std::int64_t {
                if constexpr (std::is_arithmetic_v<Held> && !std::is_same_v<Held, bool>) {
                    return static_cast<std::int64_t>(held);
                } else {
                    return -1;
                }
            },
            found->data);
    }

    void start(Json const& data) {
        if (Json const* const block = member(data, "content_block")) {
            _blocks.insert_or_assign(indexOf(data), *block);
        }
    }

    void delta(Json const& data) {
        Json const* const change = member(data, "delta");
        auto const block = _blocks.find(indexOf(data));
        if (change == nullptr || block == _blocks.end() ||
            !std::holds_alternative<Json::object_t>(block->second.data)) {
            return;
        }
        auto& object = std::get<Json::object_t>(block->second.data);
        std::string const type = textOf(*change, "type");
        auto const append = [&](std::string const& key, std::string const& piece) {
            auto* const current = object.find(key);
            std::string const joined = current == object.end() ? std::string{} : textOf(block->second, key);
            object.insert_or_assign(key, joined + piece);
        };
        if (type == "text_delta") {
            std::string const piece = textOf(*change, "text");
            append("text", piece);
            _text += piece;
            if (!_cancelled.load() && _sink.text) {
                _sink.text(piece);
            }
        } else if (type == "thinking_delta") {
            append("thinking", textOf(*change, "thinking"));
        } else if (type == "signature_delta") {
            append("signature", textOf(*change, "signature"));
        } else if (type == "input_json_delta") {
            _partials[block->first] += textOf(*change, "partial_json");
        }
    }

    void stop() {
        if (_stopReason == "refusal") {
            fail("the model declined this request");
            return;
        }
        chat::Response response;
        response.text = _text;
        Json::array_t content;
        for (auto& [index, block] : _blocks) {
            if (textOf(block, "type") == "tool_use") {
                auto const partial = _partials.find(index);
                std::string arguments =
                    partial == _partials.end() || partial->second.empty() ? std::string{"{}"} : partial->second;
                std::get<Json::object_t>(block.data).insert_or_assign("input", objectOrEmpty(arguments));
                response.calls.push_back(chat::ToolCall{
                    .id = textOf(block, "id"), .name = textOf(block, "name"), .argumentsJson = std::move(arguments)});
            }
            content.push_back(block);
        }
        // A call cut off by the limit usually still parses, as a partial object, so this is what catches it.
        if (_stopReason == "max_tokens" && !response.calls.empty()) {
            fail("the model's answer was cut off by its token limit");
            return;
        }
        response.opaque = ui::detail::writeMessage(Json(std::move(content)));
        _ended = true;
        if (!_cancelled.load() && _sink.done) {
            _sink.done(std::move(response));
        }
    }

    void finished() {
        QPointer<QNetworkReply> const reply = _reply;
        if (!_ended) {
            finishUnended();
        }
        _reply = nullptr;
        if (reply != nullptr) {
            reply->deleteLater();
        }
    }

    void finishUnended() {
        consume();
        if (_ended) {
            return;
        }
        if (int const code = status(); code >= 400) {
            std::string message;
            if (auto const parsed = ui::parseJson(_buffer)) {
                if (Json const* const error = member(*parsed, "error")) {
                    message = textOf(*error, "message");
                }
            }
            fail("the model provider answered " + std::to_string(code) + (message.empty() ? "" : ": " + message));
        } else if (_reply->error() != QNetworkReply::NoError) {
            fail("the model provider could not be reached: " + _reply->errorString().toStdString());
        } else {
            fail("the model provider's answer ended early");
        }
    }

    QPointer<QObject> _owner;
    chat::ProviderSink _sink;
    QPointer<QNetworkReply> _reply;
    std::atomic<bool> _cancelled{false};
    bool _ended = false;
    std::string _buffer;
    std::string _text;
    std::string _stopReason;
    std::map<std::int64_t, Json> _blocks;
    std::map<std::int64_t, std::string> _partials;
};

class AnthropicProvider final : public chat::IProvider {
public:
    AnthropicProvider(chat::ProviderConfig config, QObject* owner)
        : _config{std::move(config)}, _owner{owner}, _network{new QNetworkAccessManager(owner)} {
        if (_config.baseUrl.empty()) {
            _config.baseUrl = std::string{kPublicAddress};
        }
    }

    [[nodiscard]] std::shared_ptr<chat::IProviderCall> stream(chat::Request request,
                                                              chat::ProviderSink sink) override {
        auto call = std::make_shared<Call>(_owner, std::move(sink));
        QPointer<QObject> const owner = _owner;
        if (owner == nullptr) {
            call->fail("the model provider has shut down");
            return call;
        }
        QByteArray body = bodyOf(request, _config);
        QMetaObject::invokeMethod(
            owner.data(),
            [call, network = _network, body = std::move(body), config = _config] {
                if (network == nullptr) {
                    call->fail("the model provider has shut down");
                    return;
                }
                QNetworkRequest http{QUrl{QString::fromStdString(config.baseUrl + "/v1/messages")}};
                http.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
                http.setRawHeader("x-api-key", QByteArray::fromStdString(config.apiKey));
                http.setRawHeader("anthropic-version", "2026-10-21");
                if (config.baseUrl == kPublicAddress) {
                    http.setRawHeader("anthropic-beta", "server-side-fallback-2026-07-01");
                }
                http.setTransferTimeout(kTransferTimeoutMs);
                call->start(*network, http, body);
            },
            Qt::QueuedConnection);
        return call;
    }

private:
    chat::ProviderConfig _config;
    QPointer<QObject> _owner;
    QPointer<QNetworkAccessManager> _network;
};

}  // namespace

std::expected<std::shared_ptr<ui::chat::IProvider>, std::string> makeQtChatProvider(ui::chat::ProviderConfig config,
                                                                                    QObject* owner) {
    if (config.name != "anthropic") {
        return std::unexpected("no model provider is named `" + config.name + "`");
    }
    if (config.apiKey.empty()) {
        return std::unexpected(std::string{"the model provider needs an API key"});
    }
    if (config.model.empty()) {
        return std::unexpected(std::string{"the model provider needs a model"});
    }
    if (owner == nullptr) {
        return std::unexpected(std::string{"the model provider needs an owner on a Qt thread"});
    }
    return std::make_shared<AnthropicProvider>(std::move(config), owner);
}

}  // namespace morph::qt

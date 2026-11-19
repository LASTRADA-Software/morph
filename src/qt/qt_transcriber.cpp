// SPDX-License-Identifier: Apache-2.0
#include <QByteArray>
#include <QHttpMultiPart>
#include <QHttpPart>
#include <QMetaObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QUrl>
#include <QVariant>
#include <atomic>
#include <expected>
#include <memory>
#include <morph/qt/qt_transcriber.hpp>
#include <morph/ui/bundle.hpp>
#include <morph/ui/chat/config.hpp>
#include <morph/ui/chat/transcriber.hpp>
#include <morph/ui/value.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

namespace morph::qt {

namespace {

namespace chat = ui::chat;
using ui::Json;

// A transcription makes no progress for this long before it is given up on; the service's own deadline is shorter.
constexpr int kTransferTimeoutMs = 120000;

constexpr std::string_view kPublicAddress = "https://api.openai.com";

[[nodiscard]] Json const* memberOf(Json const& value, std::string_view key) {
    auto const* const object = std::get_if<Json::object_t>(&value.data);
    if (object == nullptr) {
        return nullptr;
    }
    auto const* const found = object->find(key);
    return found == object->end() ? nullptr : &found->second;
}

[[nodiscard]] std::string const* textAt(Json const& value, std::string_view key) {
    Json const* const found = memberOf(value, key);
    return found == nullptr ? nullptr : std::get_if<std::string>(&found->data);
}

[[nodiscard]] QHttpPart field(QString const& name, QByteArray const& value) {
    QHttpPart part;
    part.setHeader(QNetworkRequest::ContentDispositionHeader,
                   QVariant{QStringLiteral("form-data; name=\"%1\"").arg(name)});
    part.setBody(value);
    return part;
}

[[nodiscard]] QHttpPart clip(QByteArray const& audio) {
    QHttpPart part;
    part.setHeader(QNetworkRequest::ContentDispositionHeader,
                   QVariant{QStringLiteral("form-data; name=\"file\"; filename=\"audio.wav\"")});
    part.setHeader(QNetworkRequest::ContentTypeHeader, QVariant{QStringLiteral("audio/wav")});
    part.setBody(audio);
    return part;
}

// One transcription. Runs on the owner's thread, except `cancel`.
class Call final : public chat::ITranscriberCall, public std::enable_shared_from_this<Call> {
public:
    Call(QPointer<QObject> owner, chat::TranscriberSink sink) : _owner{std::move(owner)}, _sink{std::move(sink)} {}

    void cancel() override {
        _cancelled.store(true);
        QPointer<QObject> const owner = _owner;
        if (owner != nullptr) {
            QMetaObject::invokeMethod(
                owner.data(), [self = shared_from_this()] { self->abort(); }, Qt::QueuedConnection);
        }
    }

    void start(QNetworkAccessManager& network, QNetworkRequest const& request, QHttpMultiPart* body) {
        if (_cancelled.load()) {
            delete body;  // NOLINT(cppcoreguidelines-owning-memory): never handed to a reply.
            return;
        }
        _reply = network.post(request, body);
        body->setParent(_reply);
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
    }

private:
    void abort() {
        _ended = true;
        if (_reply != nullptr) {
            _reply->abort();
        }
    }

    void finished() {
        QPointer<QNetworkReply> const reply = _reply;
        _reply = nullptr;
        if (reply == nullptr) {
            return;
        }
        reply->deleteLater();
        if (_ended) {
            return;
        }
        int const code = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (code == 0) {
            fail("the transcription provider could not be reached: " + reply->errorString().toStdString());
            return;
        }
        std::string const body = reply->readAll().toStdString();
        auto const parsed = ui::parseJson(body);
        if (code < 200 || code >= 300) {
            std::string message;
            if (parsed) {
                if (Json const* const error = memberOf(*parsed, "error")) {
                    if (std::string const* const text = textAt(*error, "message")) {
                        message = *text;
                    }
                }
            }
            fail("the transcription provider answered " + std::to_string(code) +
                 (message.empty() ? "" : ": " + message));
            return;
        }
        std::string const* const text = parsed ? textAt(*parsed, "text") : nullptr;
        if (text == nullptr) {
            fail("the transcription provider sent something unreadable");
            return;
        }
        _ended = true;
        if (!_cancelled.load() && _sink.done) {
            _sink.done(*text);
        }
    }

    QPointer<QObject> _owner;
    chat::TranscriberSink _sink;
    QPointer<QNetworkReply> _reply;
    std::atomic<bool> _cancelled{false};
    bool _ended = false;
};

class OpenAiTranscriber final : public chat::ITranscriber {
public:
    OpenAiTranscriber(chat::TranscriptionConfig config, QObject* owner)
        : _config{std::move(config)}, _owner{owner}, _network{new QNetworkAccessManager(owner)} {
        if (_config.baseUrl.empty()) {
            _config.baseUrl = std::string{kPublicAddress};
        }
    }

    [[nodiscard]] std::shared_ptr<chat::ITranscriberCall> transcribe(chat::TranscriptionInput input,
                                                                     chat::TranscriberSink sink) override {
        auto call = std::make_shared<Call>(_owner, std::move(sink));
        QPointer<QObject> const owner = _owner;
        if (owner == nullptr) {
            call->fail("the transcription provider has shut down");
            return call;
        }
        QMetaObject::invokeMethod(
            owner.data(),
            [call, network = _network, config = _config, audio = QByteArray::fromStdString(input.audio),
             language = std::move(input.language)] {
                if (network == nullptr) {
                    call->fail("the transcription provider has shut down");
                    return;
                }
                // NOLINTNEXTLINE(cppcoreguidelines-owning-memory): handed to the reply, which owns it.
                auto* const body = new QHttpMultiPart(QHttpMultiPart::FormDataType);
                body->append(clip(audio));
                body->append(field(QStringLiteral("model"), QByteArray::fromStdString(config.model)));
                body->append(field(QStringLiteral("response_format"), QByteArrayLiteral("json")));
                if (language) {
                    body->append(field(QStringLiteral("language"), QByteArray::fromStdString(*language)));
                }
                QNetworkRequest http{QUrl{QString::fromStdString(config.baseUrl + "/v1/audio/transcriptions")}};
                http.setRawHeader("Authorization", QByteArray::fromStdString("Bearer " + config.apiKey));
                http.setTransferTimeout(kTransferTimeoutMs);
                call->start(*network, http, body);
            },
            Qt::QueuedConnection);
        return call;
    }

private:
    chat::TranscriptionConfig _config;
    QPointer<QObject> _owner;
    QPointer<QNetworkAccessManager> _network;
};

}  // namespace

std::expected<std::shared_ptr<ui::chat::ITranscriber>, std::string> makeQtTranscriber(
    ui::chat::TranscriptionConfig config, QObject* owner) {
    if (config.name != "openai-transcriptions") {
        return std::unexpected("no transcription provider is named `" + config.name + "`");
    }
    if (config.apiKey.empty()) {
        return std::unexpected(std::string{"the transcription provider needs an API key"});
    }
    if (config.model.empty()) {
        return std::unexpected(std::string{"the transcription provider needs a model"});
    }
    if (owner == nullptr) {
        return std::unexpected(std::string{"the transcription provider needs an owner on a Qt thread"});
    }
    return std::make_shared<OpenAiTranscriber>(std::move(config), owner);
}

}  // namespace morph::qt

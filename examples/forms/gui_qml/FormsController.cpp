// SPDX-License-Identifier: Apache-2.0

#include "FormsController.hpp"

#include <QMetaObject>
#include <Qt>

#include <exception>
#include <string>
#include <utility>

#include <morph/wire.hpp>

#include "lab_model.hpp"
#include "lab_schemas.hpp"

FormsController::FormsController(QObject* parent) : QObject{parent} {
    _server = std::make_shared<morph::backend::RemoteServer>(_pool);

    // Register one LabModel instance over the wire protocol; the ok-reply
    // carries the server-assigned model id every later execute targets.
    auto registerMsg = morph::wire::encode(morph::wire::makeRegister("LabModel"));
    _server->handle(registerMsg, [this](const std::string& replyJson) {
        try {
            auto reply = morph::wire::decode(replyJson);
            if (reply.kind == "ok") {
                _modelId.store(reply.modelId);
            }
        } catch (const std::exception&) {
            // Leave _modelId at 0; submits will report "not registered".
        }
    });
}

QString FormsController::schemasJson() const {
    return QString::fromStdString(lab::schemasJson());
}

void FormsController::submit(const QString& actionType, const QString& bodyJson) {
    auto const action = actionType.toStdString();
    if (_modelId.load() == 0) {
        emit replyReceived(actionType, false, QStringLiteral("model not registered yet"));
        return;
    }

    morph::wire::Envelope envelope;
    envelope.kind = "execute";
    envelope.callId = _nextCallId.fetch_add(1);
    envelope.modelId = _modelId.load();
    envelope.modelType = "LabModel";
    envelope.actionType = action;
    envelope.body = bodyJson.toStdString();

    _server->handle(morph::wire::encode(envelope), [this, actionType](const std::string& replyJson) {
        bool ok = false;
        QString payload;
        try {
            auto reply = morph::wire::decode(replyJson);
            ok = reply.kind == "ok";
            payload = QString::fromStdString(ok ? reply.body : reply.message);
        } catch (const std::exception& error) {
            payload = QString::fromUtf8(error.what());
        }
        // The reply lands on a worker-pool thread; hop to the GUI thread.
        QMetaObject::invokeMethod(
            this, [this, actionType, ok, payload] { emit replyReceived(actionType, ok, payload); },
            Qt::QueuedConnection);
    });
}

// SPDX-License-Identifier: Apache-2.0

#include "FormsController.hpp"

#include <exception>
#include <memory>
#include <string>

#include <glaze/glaze.hpp>
#include <morph/core/backend.hpp>

#include "lab_schemas.hpp"

namespace {

QString errorText(const std::exception_ptr& err) {
    try {
        if (err) {
            std::rethrow_exception(err);
        }
    } catch (const std::exception& exc) {
        return QString::fromUtf8(exc.what());
    } catch (...) {
        return QStringLiteral("unknown error");
    }
    return {};
}

}  // namespace

FormsController::FormsController(QObject* parent)
    : QObject{parent},
      _bridge{std::make_unique<morph::backend::LocalBackend>(_pool)},
      _handler{_bridge, &_gui} {}

QString FormsController::schemasJson() const {
    return QString::fromStdString(lab::schemasJson());
}

void FormsController::submitIfValid(const QString& actionType, const QString& bodyJson) {
    auto const actionTypeStd = actionType.toStdString();
    _handler.executeJson(actionTypeStd, bodyJson.toStdString())
        .then([this, actionType](std::string resultJson) {
            emit replyReceived(actionType, true, QString::fromStdString(resultJson));
        })
        .onError([this, actionType](const std::exception_ptr& err) {
            emit replyReceived(actionType, false, errorText(err));
        });
}

void FormsController::fetchOptions(const QString& optionsAction) {
    _handler.execute(lab::ListSamples{})
        .then([this, optionsAction](lab::SampleList list) {
            std::string json;
            if (glz::write_json(list, json)) {
                emit optionsReceived(optionsAction, false, QStringLiteral("failed to encode options"));
                return;
            }
            emit optionsReceived(optionsAction, true, QString::fromStdString(json));
        })
        .onError([this, optionsAction](const std::exception_ptr& err) {
            emit optionsReceived(optionsAction, false, errorText(err));
        });
}

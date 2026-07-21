// SPDX-License-Identifier: Apache-2.0

#include "FormsController.hpp"

#include <exception>

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

FormsController::FormsController(QObject* parent) : QObject{parent}, _core{lab::schemasJson()} {}

QString FormsController::schemasJson() const { return QString::fromStdString(_core.schemasJson()); }

QString FormsController::viewsJson() const { return QString::fromStdString(lab::viewsJson()); }

void FormsController::submitIfValid(const QString& actionType, const QString& bodyJson) {
    _core.submitIfValid(
        actionType.toStdString(), bodyJson.toStdString(),
        [this, actionType](std::string resultJson) {
            emit replyReceived(actionType, true, QString::fromStdString(resultJson));
        },
        [this, actionType](const std::exception_ptr& err) { emit replyReceived(actionType, false, errorText(err)); });
}

void FormsController::fetchOptions(const QString& optionsAction, const QString& bodyJson) {
    _core.fetchOptions(
        optionsAction.toStdString(), bodyJson.toStdString(),
        [this, optionsAction](std::string resultJson) {
            emit optionsReceived(optionsAction, true, QString::fromStdString(resultJson));
        },
        [this, optionsAction](const std::exception_ptr& err) {
            emit optionsReceived(optionsAction, false, errorText(err));
        });
}

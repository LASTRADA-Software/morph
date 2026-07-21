// SPDX-License-Identifier: Apache-2.0

#include "FormsController.hpp"

#include <exception>
#include <glaze/glaze.hpp>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>

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

/// @brief Flattens @p json's top-level object keys into @p resolved, keyed
///        `"<actionType>.<key>"`. A no-op if @p json does not parse as a flat
///        JSON object (e.g. a bare scalar/array result).
void captureResolvedValues(std::unordered_map<std::string, std::string>& resolved, const std::string& actionType,
                           std::string_view json) {
    glz::generic_u64 dom{};
    if (glz::read_json(dom, json)) {
        return;
    }
    if (!dom.is_object()) {
        return;
    }
    for (auto& [key, value] : dom.get_object()) {
        std::string fieldJson;
        if (!glz::write_json(value, fieldJson)) {
            resolved[actionType + "." + key] = std::move(fieldJson);
        }
    }
}

}  // namespace

FormsController::FormsController(QObject* parent) : QObject{parent}, _core{lab::schemasJson()} {}

QString FormsController::schemasJson() const { return QString::fromStdString(_core.schemasJson()); }

QString FormsController::viewsJson() const { return QString::fromStdString(lab::viewsJson()); }

QString FormsController::wizardSchemasJson() const { return QString::fromStdString(lab::wizardSchemasJson()); }

QString FormsController::appSchemaJson() const { return QString::fromStdString(lab::appSchemaJson()); }

QString FormsController::resolvedValue(const QString& path) const {
    auto const iter = _resolved.find(path.toStdString());
    return iter == _resolved.end() ? QString{} : QString::fromStdString(iter->second);
}

void FormsController::submitIfValid(const QString& actionType, const QString& bodyJson) {
    auto const actionTypeStd = actionType.toStdString();
    auto const bodyStd = bodyJson.toStdString();
    _core.submitIfValid(
        actionTypeStd, bodyStd,
        [this, actionType, actionTypeStd, bodyStd](std::string resultJson) {
            captureResolvedValues(_resolved, actionTypeStd, bodyStd);
            captureResolvedValues(_resolved, actionTypeStd, resultJson);
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

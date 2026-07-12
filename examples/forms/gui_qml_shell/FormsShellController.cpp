// SPDX-License-Identifier: Apache-2.0

#include "FormsShellController.hpp"
#include "PageTreeModel.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QStandardPaths>

#include <exception>
#include <memory>
#include <string>

#include <glaze/glaze.hpp>
#include <morph/backend.hpp>

#include "lab_schemas.hpp"

namespace {

QString errorText(const std::exception_ptr& err) {
    try {
        if (err)
            std::rethrow_exception(err);
    } catch (const std::exception& exc) {
        return QString::fromUtf8(exc.what());
    } catch (...) {
        return QStringLiteral("unknown error");
    }
    return {};
}

} // namespace

FormsShellController::FormsShellController(QObject* parent)
    : QObject{parent},
      _pageModel{new PageTreeModel(this)},
      _bridge{std::make_unique<morph::backend::LocalBackend>(_pool)},
      _handler{_bridge, &_gui} {
    _configPath = defaultConfigPath();
    auto* dir = new QDir(QFileInfo(_configPath).absolutePath());
    if (!dir->exists())
        dir->mkpath(QStringLiteral("."));
    if (QFile::exists(_configPath))
        loadConfig();
    else
        seedDefaultTree();
}

QAbstractItemModel* FormsShellController::pageModel() const {
    return _pageModel;
}

QString FormsShellController::schemasJson() const {
    return QString::fromStdString(lab::schemasJson());
}

QString FormsShellController::configPath() const {
    return _configPath;
}

void FormsShellController::setConfigPath(const QString& path) {
    if (_configPath == path)
        return;
    _configPath = path;
    emit configPathChanged();
}

void FormsShellController::submitIfValid(const QString& actionType, const QString& bodyJson) {
    auto const actionTypeStd = actionType.toStdString();
    _handler.executeJson(actionTypeStd, bodyJson.toStdString())
        .then([this, actionType](std::string resultJson) {
            emit replyReceived(actionType, true, QString::fromStdString(resultJson));
        })
        .onError([this, actionType](const std::exception_ptr& err) {
            emit replyReceived(actionType, false, errorText(err));
        });
}

void FormsShellController::fetchOptions(const QString& optionsAction) {
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

void FormsShellController::saveConfig() {
    QFile file(_configPath);
    if (!file.open(QIODevice::WriteOnly))
        return;
    QJsonDocument doc(_pageModel->toJson());
    file.write(doc.toJson(QJsonDocument::Indented));
}

void FormsShellController::loadConfig() {
    QFile file(_configPath);
    if (!file.open(QIODevice::ReadOnly))
        return;
    auto doc = QJsonDocument::fromJson(file.readAll());
    if (!doc.isArray())
        return;
    _pageModel->fromJson(doc.array());
}

void FormsShellController::resetToDefaults() {
    seedDefaultTree();
}

QString FormsShellController::defaultConfigPath() const {
    auto base = QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation);
    return base + QStringLiteral("/morph/morph-shell/pages.json");
}

void FormsShellController::seedDefaultTree() {
    auto builtinFolder = _pageModel->addFolder({}, QStringLiteral("Built-in Forms"));
    _pageModel->addPage(builtinFolder,
        QStringLiteral("Compute Dry Density"),
        QStringLiteral("builtin://ComputeDryDensity"));
    _pageModel->addPage(builtinFolder,
        QStringLiteral("Record Measurement"),
        QStringLiteral("builtin://RecordMeasurement"));
}
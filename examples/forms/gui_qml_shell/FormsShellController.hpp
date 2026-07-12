// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// QML-facing controller for the form shell. Owns the bridge stack, the
/// page tree model, and the persistent config. Registered as both a
/// QML_ELEMENT and a context property ("morphController") so that user-
/// loaded .qml pages can access form submission.

#include <QAbstractItemModel>
#include <QObject>
#include <QString>
#include <QtQml/qqmlregistration.h>

#include "PageTreeModel.hpp"

#ifndef Q_MOC_RUN
#include <morph/bridge.hpp>
#include <morph/executor.hpp>
#include <morph/qt/qt_executor.hpp>

#include "lab_model.hpp"
#endif

class FormsShellController : public QObject {
    Q_OBJECT
    QML_ELEMENT

    Q_PROPERTY(QAbstractItemModel* pageModel READ pageModel CONSTANT)
    Q_PROPERTY(QString schemasJson READ schemasJson CONSTANT)
    Q_PROPERTY(QString configPath READ configPath WRITE setConfigPath NOTIFY configPathChanged)

public:
    explicit FormsShellController(QObject* parent = nullptr);

    QAbstractItemModel* pageModel() const;
    QString schemasJson() const;
    QString configPath() const;
    void setConfigPath(const QString& path);

    Q_INVOKABLE void submitIfValid(const QString& actionType, const QString& bodyJson);
    Q_INVOKABLE void fetchOptions(const QString& optionsAction);

    Q_INVOKABLE void saveConfig();
    Q_INVOKABLE void loadConfig();
    Q_INVOKABLE void resetToDefaults();

signals:
    void configPathChanged();
    void replyReceived(const QString& actionType, bool ok, const QString& payload);
    void optionsReceived(const QString& optionsAction, bool ok, const QString& payload);

private:
    QString defaultConfigPath() const;
    void seedDefaultTree();

    PageTreeModel* _pageModel = nullptr;
    QString _configPath;

    morph::exec::ThreadPoolExecutor _pool{2};
    morph::qt::QtExecutor _gui;
    morph::bridge::Bridge _bridge;
    morph::bridge::BridgeHandler<lab::LabModel> _handler;
};
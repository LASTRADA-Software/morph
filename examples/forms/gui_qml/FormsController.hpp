// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// QML-facing controller for the schema-driven forms demo.
///
/// The QML layer renders forms from the schemas exposed here and submits
/// fully-assembled action bodies as JSON strings via `submitIfValid`,
/// called on every edit once the body validates client-side (no submit
/// button). This controller dispatches through a real `morph::bridge::Bridge`
/// + `BridgeHandler<LabModel>` — the same client API `examples/bank`'s GUI
/// uses — via the generic `BridgeHandler::executeJson` path, so it never
/// touches `morph::wire::Envelope` or `morph::backend::RemoteServer`
/// directly.

#include <QObject>
#include <QString>
#include <QtQml/qqmlregistration.h>

// Guarded like examples/bank/gui/controllers/AccountController.hpp: MOC
// only needs the Q_OBJECT/QML_ELEMENT macros above and the Q_INVOKABLE/
// Q_PROPERTY declarations below; it must not be pointed at morph's
// template-heavy headers (bridge.hpp, glaze) or the Qt executor.
#ifndef Q_MOC_RUN
#include <morph/bridge.hpp>
#include <morph/executor.hpp>
#include <morph/qt/qt_executor.hpp>

#include "lab_model.hpp"
#endif

class FormsController : public QObject {
    Q_OBJECT
    QML_ELEMENT

    /// @brief `{actionType: schema}` JSON — everything the QML renderer needs.
    Q_PROPERTY(QString schemasJson READ schemasJson CONSTANT)

public:
    explicit FormsController(QObject* parent = nullptr);

    [[nodiscard]] QString schemasJson() const;

    /// @brief Dispatches @p bodyJson as the body of @p actionType if the
    ///        body is complete. Called by QML on every field edit once the
    ///        assembled body passes client-side validation — there is no
    ///        separate submit step. The reply arrives via `replyReceived`
    ///        on the GUI thread.
    Q_INVOKABLE void submitIfValid(const QString& actionType, const QString& bodyJson);

    /// @brief Executes @p optionsAction with an empty body to fetch combo-box
    ///        options (a `Choice` field's declared provider). The reply
    ///        arrives via `optionsReceived` on the GUI thread.
    Q_INVOKABLE void fetchOptions(const QString& optionsAction);

signals:
    /// @brief Emitted once per `submitIfValid` call. @p payload is the
    ///        result JSON when @p ok, otherwise the error message.
    void replyReceived(const QString& actionType, bool ok, const QString& payload);

    /// @brief Emitted once per `fetchOptions`. @p payload is the options
    ///        action's result JSON when @p ok, otherwise the error message.
    void optionsReceived(const QString& optionsAction, bool ok, const QString& payload);

private:
    // Declaration order matters for destruction: `_handler` and `_bridge`
    // must be torn down before `_pool`/`_gui`, and `_pool` must outlive the
    // `LocalBackend` owned inside `_bridge` (constructed from it). Declared
    // in construction order so default destruction (reverse order) is safe.
    morph::exec::ThreadPoolExecutor _pool{2};
    morph::qt::QtExecutor _gui;
    morph::bridge::Bridge _bridge;
    morph::bridge::BridgeHandler<lab::LabModel> _handler;
};

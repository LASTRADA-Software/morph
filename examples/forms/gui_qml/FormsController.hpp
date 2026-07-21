// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// QML-facing controller for the schema-driven forms demo. Thin QObject
/// wrapper around `morph::qt::forms::FormsControllerCore<lab::LabModel>` --
/// the shared, model-agnostic core now lives in
/// include/morph/qt/forms/forms_controller_core.hpp. This class exists
/// because Qt cannot register a class *template* for QML: every app that
/// wants a QML_ELEMENT-visible controller writes one small subclass like
/// this one, naming its own model type; the actual Bridge/BridgeHandler/
/// executor wiring lives once, in the core, not here.

#include <QtQml/qqmlregistration.h>

#include <QObject>
#include <QString>

// Guarded like examples/bank/gui/controllers/AccountController.hpp: MOC only
// needs the Q_OBJECT/QML_ELEMENT macros above and the Q_INVOKABLE/Q_PROPERTY
// declarations below; it must not be pointed at morph's template-heavy
// headers (bridge.hpp, glaze) or the Qt executor.
#ifndef Q_MOC_RUN
#include <morph/qt/forms/forms_controller_core.hpp>

#include "lab_model.hpp"
#endif

class FormsController : public QObject {
    Q_OBJECT
    QML_ELEMENT

    /// @brief `{actionType: schema}` JSON -- everything the QML renderer needs.
    Q_PROPERTY(QString schemasJson READ schemasJson CONSTANT)

    /// @brief `{viewType: viewSchema}` JSON -- every registered view
    ///        descriptor (list/table + master-detail screens composed from
    ///        existing action forms; see docs/spec/forms/views.md).
    Q_PROPERTY(QString viewsJson READ viewsJson CONSTANT)

public:
    explicit FormsController(QObject* parent = nullptr);

    [[nodiscard]] QString schemasJson() const;

    /// @brief Returns the view-schema document set
    ///        (`morph::views::viewSchemaJson` output per registered view),
    ///        keyed by view type id.
    [[nodiscard]] QString viewsJson() const;

    /// @brief Dispatches @p bodyJson as the body of @p actionType if the
    ///        body is complete. Called by QML on every field edit once the
    ///        assembled body passes client-side validation -- there is no
    ///        separate submit step. The reply arrives via `replyReceived`
    ///        on the GUI thread.
    Q_INVOKABLE void submitIfValid(const QString& actionType, const QString& bodyJson);

    /// @brief Executes @p optionsAction with @p bodyJson to fetch combo-box
    ///        options (a `Choice` field's declared provider). @p bodyJson is
    ///        `"{}"` for an independent `Choice`, or `{parentField: value, ...}`
    ///        built by the QML layer from a dependent `Choice`'s declared
    ///        `x-optionsDependsOn` sibling fields. The reply arrives via
    ///        `optionsReceived` on the GUI thread.
    Q_INVOKABLE void fetchOptions(const QString& optionsAction, const QString& bodyJson);

signals:
    /// @brief Emitted once per `submitIfValid` call. @p payload is the
    ///        result JSON when @p ok, otherwise the error message.
    void replyReceived(const QString& actionType, bool ok, const QString& payload);

    /// @brief Emitted once per `fetchOptions`. @p payload is the options
    ///        action's result JSON when @p ok, otherwise the error message.
    void optionsReceived(const QString& optionsAction, bool ok, const QString& payload);

private:
    morph::qt::forms::FormsControllerCore<lab::LabModel> _core;
};

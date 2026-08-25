// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <QObject>
#include <QString>
#include <QVariantMap>

// Guarded exactly like `sample_presenter.hpp`'s own includes, for the same
// reason: AUTOMOC runs moc over this header and moc must not be pointed at
// morph's template-heavy bridge header.
#ifndef Q_MOC_RUN
#include <morph/core/bridge.hpp>
#include <morph/core/executor.hpp>

#include "sample_presenter.hpp"
#endif

namespace lims::gui {

/// @brief QML-facing face of `lims::gui::SamplePresenter`.
///
/// Turns the presenter's DTO-carrying signals into `QVariantMap` property
/// bags and its typed calls into `Q_INVOKABLE`s. No decisions, only
/// translation — the same shape `kanban::gui::ProjectAdminBridge` has.
///
/// Ids cross this boundary as plain numbers because that is what QML has;
/// `-1` means "unengaged", which can never collide with a real key
/// (Lightweight's `ServerSideAutoIncrement` starts at 1).
class SampleBridge : public QObject {
    Q_OBJECT

    /// @brief The attached sample as `{id, clientId, reference, state,
    ///        version, registeredAtMs}`, or an empty map before one is
    ///        attached.
    Q_PROPERTY(QVariantMap sample READ sample NOTIFY sampleChanged)
    /// @brief The most recently registered client's id, or `-1`.
    Q_PROPERTY(qlonglong clientId READ clientId NOTIFY clientRegistered)
    /// @brief The most recent error message, or an empty string.
    Q_PROPERTY(QString lastError READ lastError NOTIFY failed)
    /// @brief The `{actionType: schema}` document the shipped `DynamicForm`
    ///        renders this surface's forms from.
    Q_PROPERTY(QString schemasJson READ schemasJson CONSTANT)

public:
    /// @param bridge   The shared `Bridge` `AppContext` owns.
    /// @param executor The executor `Completion` callbacks land on.
    /// @param parent   Optional `QObject` parent.
    SampleBridge(::morph::bridge::Bridge& bridge, ::morph::exec::IExecutor* executor, QObject* parent = nullptr);

    /// @brief The attached sample (see the `sample` property).
    /// @return Its property bag, or an empty map.
    [[nodiscard]] QVariantMap sample() const { return _sample; }
    /// @brief The most recently registered client's id.
    /// @return The id, or `-1`.
    [[nodiscard]] qlonglong clientId() const { return _clientId; }
    /// @brief The most recent error message.
    /// @return The message, or an empty string.
    [[nodiscard]] QString lastError() const { return _lastError; }

    /// @brief The schema document (see the `schemasJson` property).
    /// @return `lims::gui::limsSchemasJson()`, as a `QString`.
    [[nodiscard]] QString schemasJson() const;

    /// @brief Dispatches a schema-driven form's assembled body. The surface
    ///        `DynamicForm` expects of a controller, alongside `schemasJson`
    ///        and `replyReceived`.
    /// @param actionType One of RegisterClient / RegisterSample / RejectSample / ReturnForRework.
    /// @param bodyJson The form's assembled JSON body.
    Q_INVOKABLE void submitIfValid(const QString& actionType, const QString& bodyJson);

    /// @brief Registers a client. Emits `clientRegistered`, or `failed`.
    /// @param name The client's name.
    Q_INVOKABLE void registerClient(const QString& name);

    /// @brief Logs a new sample and attaches to it. Emits `sampleChanged`, or
    ///        `failed`.
    /// @param clientId  The owning client, as its plain number.
    /// @param reference The lab's reference for the container.
    Q_INVOKABLE void registerSample(qlonglong clientId, const QString& reference);

    /// @brief Attaches to an existing sample. Emits `sampleChanged`, or
    ///        `failed`.
    /// @param sampleId The sample, as its plain number.
    Q_INVOKABLE void openSample(qlonglong sampleId);

    /// @brief Re-reads the attached sample. Emits `sampleChanged`, or `failed`.
    Q_INVOKABLE void refresh();

    /// @brief `Registered → Received`. Emits `sampleChanged`, or `failed`.
    Q_INVOKABLE void receiveSample();

    /// @brief `Received → InProgress`. Emits `sampleChanged`, or `failed`.
    Q_INVOKABLE void startWork();

    /// @brief `InProgress → ToBeVerified`. Emits `sampleChanged`, or `failed`.
    Q_INVOKABLE void submitForVerification();

    /// @brief `ToBeVerified → InProgress`. Emits `sampleChanged`, or `failed`.
    /// @param reason Why the sample went back to the bench.
    Q_INVOKABLE void returnForRework(const QString& reason);

    /// @brief `ToBeVerified → Published`. Emits `sampleChanged`, or `failed`.
    Q_INVOKABLE void publishSample();

    /// @brief `Registered|Received → Rejected`. Emits `sampleChanged`, or
    ///        `failed`.
    /// @param reason Why the container is refused.
    Q_INVOKABLE void rejectSample(const QString& reason);

signals:
    /// @brief Emitted once the wrapped presenter's registration round trip
    ///        settles, successfully or not (`Presenter::bound()`).
    void bound();
    /// @brief The attached sample changed — see the `sample` property.
    /// @param sample The sample's property bag.
    void sampleChanged(const QVariantMap& sample);
    /// @brief A client was registered — see the `clientId` property.
    /// @param clientId The new client's id.
    void clientRegistered(qlonglong clientId);
    /// @brief Emitted once per `submitIfValid`, whichever way it resolved.
    /// @param actionType The action the reply belongs to.
    /// @param ok Whether it succeeded.
    /// @param payload The result JSON on success, the error text on failure.
    void replyReceived(const QString& actionType, bool ok, const QString& payload);
    /// @brief Any action's typed error, already rendered as a message.
    /// @param message The model's own `what()`.
    void failed(const QString& message);

private:
#ifndef Q_MOC_RUN
    SamplePresenter _presenter;
#endif
    QVariantMap _sample;
    qlonglong _clientId = -1;
    QString _lastError;
};

}  // namespace lims::gui

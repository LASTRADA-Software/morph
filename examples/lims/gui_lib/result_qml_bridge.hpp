// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <QObject>
#include <QString>
#include <QVariantList>
#include <QVariantMap>

// Guarded exactly like `sample_qml_bridge.hpp`'s own includes.
#ifndef Q_MOC_RUN
#include "result_presenter.hpp"

#include <morph/core/bridge.hpp>
#include <morph/core/executor.hpp>
#endif

namespace lims::gui {

/// @brief QML-facing face of `lims::gui::ResultPresenter`.
///
/// The result-entry surface: the analysis picker (read-only catalogue), the
/// capture form, the result table, verification, and the conflicts offline
/// replay flagged. No decisions, only translation.
class ResultBridge : public QObject {
    Q_OBJECT

    /// @brief Every analysis's current version, each a
    ///        `{versionId, analysisId, name, version, canonicalUnit,
    ///        decimalPlaces}` map.
    Q_PROPERTY(QVariantList analyses READ analyses NOTIFY analysesListed)
    /// @brief The attached sample's results, each a `{id, qualifier,
    ///        valueText, hasValue, capturedBy, ...}` map.
    Q_PROPERTY(QVariantList results READ results NOTIFY resultsListed)
    /// @brief The attached sample's flagged conflicts.
    Q_PROPERTY(QVariantList conflicts READ conflicts NOTIFY conflictsListed)
    /// @brief The most recent error message, or an empty string.
    Q_PROPERTY(QString lastError READ lastError NOTIFY failed)
    /// @brief The `{actionType: schema}` document the shipped `DynamicForm`
    ///        renders this surface's forms from.
    Q_PROPERTY(QString schemasJson READ schemasJson CONSTANT)

  public:
    /// @param bridge   The shared `Bridge` `AppContext` owns.
    /// @param executor The executor `Completion` callbacks land on.
    /// @param parent   Optional `QObject` parent.
    ResultBridge(::morph::bridge::Bridge& bridge, ::morph::exec::IExecutor* executor, QObject* parent = nullptr);

    /// @brief The analysis picker's rows (see the `analyses` property).
    /// @return The most recent listing.
    [[nodiscard]] QVariantList analyses() const { return _analyses; }
    /// @brief The result table's rows (see the `results` property).
    /// @return The most recent listing.
    [[nodiscard]] QVariantList results() const { return _results; }
    /// @brief The conflict list's rows (see the `conflicts` property).
    /// @return The most recent listing.
    [[nodiscard]] QVariantList conflicts() const { return _conflicts; }
    /// @brief The most recent error message.
    /// @return The message, or an empty string.
    [[nodiscard]] QString lastError() const { return _lastError; }

    /// @brief The schema document (see the `schemasJson` property).
    /// @return `lims::gui::limsSchemasJson()`, as a `QString`.
    [[nodiscard]] QString schemasJson() const;

    /// @brief Dispatches a schema-driven form's assembled body. The surface
    ///        `DynamicForm` expects of a controller, alongside `schemasJson`
    ///        and `replyReceived`.
    /// @param actionType One of CaptureConcentration / ResolveConflict.
    /// @param bodyJson The form's assembled JSON body.
    Q_INVOKABLE void submitIfValid(const QString& actionType, const QString& bodyJson);

    /// @brief Fetches every analysis's current version. Emits
    ///        `analysesListed`, or `failed`.
    Q_INVOKABLE void refreshAnalyses();

    /// @brief Attaches this surface to a sample. Emits `sampleAttached`, or
    ///        `failed`.
    /// @param sampleId The sample, as its plain number.
    Q_INVOKABLE void openSample(qlonglong sampleId);

    /// @brief Captures a measured reading. Emits `resultCaptured`, or
    ///        `failed`.
    /// @param versionId The analysis version, as its plain number.
    /// @param reading   The reading in mg/L.
    /// @param dilution  `"neat"`, `"diluted"`, or empty for "not stated".
    /// @param factor    The dilution factor; used only when @p dilution is
    ///        `"diluted"`.
    Q_INVOKABLE void captureReading(qlonglong versionId, double reading, const QString& dilution, double factor);

    /// @brief Captures one of the three "no number" claims. Emits
    ///        `resultCaptured`, or `failed`.
    /// @param versionId The analysis version, as its plain number.
    /// @param code      `"notMeasured"`, `"belowLOD"` or `"aboveUDL"`.
    Q_INVOKABLE void captureQualifier(qlonglong versionId, const QString& code);

    /// @brief Fetches the attached sample's results. Emits `resultsListed`,
    ///        or `failed`.
    Q_INVOKABLE void refreshResults();

    /// @brief Records the four-eyes verification of one result. Emits
    ///        `resultVerified`, or `failed`.
    /// @param resultId The result, as its plain number.
    Q_INVOKABLE void verifyResult(qlonglong resultId);

    /// @brief Fetches the attached sample's flagged conflicts. Emits
    ///        `conflictsListed`, or `failed`.
    Q_INVOKABLE void refreshConflicts();

    /// @brief Records a decision about one flagged conflict. Emits
    ///        `conflictResolved`, or `failed`.
    /// @param conflictId The conflict, as its plain number.
    /// @param resolution `"discard"` or `"apply"`.
    /// @param note       The resolver's stated rationale. Required.
    Q_INVOKABLE void resolveConflict(qlonglong conflictId, const QString& resolution, const QString& note);

  signals:
    /// @brief Emitted once the wrapped presenter's registration round trip
    ///        settles, successfully or not.
    void bound();
    /// @brief An analysis listing arrived — see the `analyses` property.
    /// @param analyses The listing's rows.
    void analysesListed(const QVariantList& analyses);
    /// @brief This surface attached to a sample.
    /// @param sample The sample's property bag.
    void sampleAttached(const QVariantMap& sample);
    /// @brief A capture succeeded.
    /// @param result The stored result's property bag.
    void resultCaptured(const QVariantMap& result);
    /// @brief A result listing arrived — see the `results` property.
    /// @param results The listing's rows.
    void resultsListed(const QVariantList& results);
    /// @brief A verification was recorded.
    /// @param verification The verification's property bag.
    void resultVerified(const QVariantMap& verification);
    /// @brief A conflict listing arrived — see the `conflicts` property.
    /// @param conflicts The listing's rows.
    void conflictsListed(const QVariantList& conflicts);
    /// @brief A conflict was resolved.
    /// @param conflict The conflict's property bag, in its resolved state.
    void conflictResolved(const QVariantMap& conflict);
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
    ResultPresenter _presenter;
#endif
    QVariantList _analyses;
    QVariantList _results;
    QVariantList _conflicts;
    QString _lastError;
};

}  // namespace lims::gui

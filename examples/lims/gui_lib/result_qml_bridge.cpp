// SPDX-License-Identifier: Apache-2.0
#include "result_qml_bridge.hpp"

#include <utility>

#include "lims_qml_conversions.hpp"
#include "lims_schemas.hpp"

namespace lims::gui {

ResultBridge::ResultBridge(::morph::bridge::Bridge& bridge, ::morph::exec::IExecutor* executor, QObject* parent)
    : QObject{parent}, _presenter{bridge, executor} {
    connect(&_presenter, &ResultPresenter::bound, this, &ResultBridge::bound);
    connect(&_presenter, &ResultPresenter::analysesListed, this, [this](ListAnalysesResult result) {
        _analyses = toVariantList(result.analyses);
        emit analysesListed(_analyses);
    });
    connect(&_presenter, &ResultPresenter::sampleAttached, this,
            [this](SampleView view) { emit sampleAttached(toVariantMap(view)); });
    connect(&_presenter, &ResultPresenter::resultCaptured, this,
            [this](ResultView view) { emit resultCaptured(toVariantMap(view)); });
    connect(&_presenter, &ResultPresenter::resultsListed, this, [this](ListResultsResult result) {
        _results = toVariantList(result.results);
        emit resultsListed(_results);
    });
    connect(&_presenter, &ResultPresenter::resultVerified, this,
            [this](VerificationView view) { emit resultVerified(toVariantMap(view)); });
    connect(&_presenter, &ResultPresenter::conflictsListed, this, [this](ListConflictsResult result) {
        _conflicts = toVariantList(result.conflicts);
        emit conflictsListed(_conflicts);
    });
    connect(&_presenter, &ResultPresenter::conflictResolved, this,
            [this](ConflictView view) { emit conflictResolved(toVariantMap(view)); });
    connect(&_presenter, &ResultPresenter::replyReceived, this, &ResultBridge::replyReceived);
    connect(&_presenter, &ResultPresenter::failed, this, [this](QString message) {
        _lastError = std::move(message);
        emit failed(_lastError);
    });
}

void ResultBridge::refreshAnalyses() { _presenter.refreshAnalyses(); }

void ResultBridge::openSample(qlonglong sampleId) {
    _presenter.openSample(SampleId{static_cast<std::int64_t>(sampleId)});
}

void ResultBridge::captureReading(qlonglong versionId, double reading, const QString& dilution, double factor) {
    _presenter.captureReading(AnalysisVersionId{static_cast<std::int64_t>(versionId)}, reading, dilution, factor);
}

void ResultBridge::captureQualifier(qlonglong versionId, const QString& code) {
    _presenter.captureQualifier(AnalysisVersionId{static_cast<std::int64_t>(versionId)}, code);
}

void ResultBridge::refreshResults() { _presenter.refreshResults(); }

void ResultBridge::verifyResult(qlonglong resultId) {
    _presenter.verifyResult(ResultId{static_cast<std::int64_t>(resultId)});
}

void ResultBridge::refreshConflicts() { _presenter.refreshConflicts(); }

void ResultBridge::resolveConflict(qlonglong conflictId, const QString& resolution, const QString& note) {
    _presenter.resolveConflict(ConflictId{static_cast<std::int64_t>(conflictId)}, resolution, note);
}

QString ResultBridge::schemasJson() const { return QString::fromStdString(limsSchemasJson()); }

void ResultBridge::submitIfValid(const QString& actionType, const QString& bodyJson) {
    _presenter.submitIfValid(actionType, bodyJson);
}

}  // namespace lims::gui

// SPDX-License-Identifier: Apache-2.0
#include "result_presenter.hpp"

#include <QStringList>

#include <string>
#include <utility>

namespace lims::gui {

ResultPresenter::ResultPresenter(::morph::bridge::Bridge& bridge, ::morph::exec::IExecutor* executor, QObject* parent)
    : Presenter{parent}, _catalog{bridge, executor}, _sample{bridge, executor} {
    // The *catalogue* handler's binding, not the sample one's: `_sample` is
    // `AllowShared` and therefore unbound until something attaches it to a
    // key, so gating the view's first call on it would wait forever. The
    // first thing this surface does is list analyses, which runs on
    // `_catalog`.
    trackBound(_catalog.whenBound());
}

void ResultPresenter::reportError(const std::exception_ptr& err) {
    try {
        std::rethrow_exception(err);
    } catch (const std::exception& ex) {
        emit failed(QString::fromStdString(ex.what()));
    }
}

void ResultPresenter::dispatchCapture(CaptureConcentration action) {
    track<ResultView>(
        _sample.execute(std::move(action)), [this](ResultView view) { emit resultCaptured(std::move(view)); },
        [this](const std::exception_ptr& err) { reportError(err); });
}

void ResultPresenter::submitIfValid(const QString& actionType, const QString& bodyJson) {
    static const QStringList kOwned{QStringLiteral("CaptureConcentration"), QStringLiteral("ResolveConflict")};
    if (!kOwned.contains(actionType)) {
        emit replyReceived(actionType, false,
                           QStringLiteral("ResultPresenter does not own the action '%1'").arg(actionType));
        return;
    }

    track<std::string>(
        _sample.executeJson(actionType.toStdString(), bodyJson.toStdString()),
        [this, actionType](std::string payload) {
            emit replyReceived(actionType, true, QString::fromStdString(payload));
            // Re-read rather than parse the raw reply back out: one decoder,
            // not two that could disagree.
            refreshResults();
            refreshConflicts();
        },
        [this, actionType](const std::exception_ptr& err) {
            try {
                std::rethrow_exception(err);
            } catch (const std::exception& ex) {
                emit replyReceived(actionType, false, QString::fromStdString(ex.what()));
            }
        });
}

void ResultPresenter::refreshAnalyses() {
    track<ListAnalysesResult>(
        _catalog.execute(ListAnalyses{}),
        [this](ListAnalysesResult result) { emit analysesListed(std::move(result)); },
        [this](const std::exception_ptr& err) { reportError(err); });
}

void ResultPresenter::openSample(SampleId sampleId) {
    track<SampleView>(
        _sample.execute(OpenSample{.sampleId = sampleId}),
        [this](SampleView view) { emit sampleAttached(std::move(view)); },
        [this](const std::exception_ptr& err) { reportError(err); });
}

void ResultPresenter::captureReading(AnalysisVersionId versionId, double reading, const QString& dilution,
                                     double factor) {
    CaptureConcentration action{.analysisVersionId = versionId,
                                // Exact at the declared precision -- see this
                                // class's own doc comment for why the double
                                // QML hands over does not cost exactness here.
                                .value = Concentration::fromDouble(reading)};
    if (!dilution.isEmpty()) {
        action.dilution = DilutionChoice{dilution.toStdString()};
        // Only stamped for a diluted preparation. Sending it regardless would
        // be harmless (the model ignores a factor whose preparation says
        // neat -- the rung README's §5 clear-on-hide decision) but it would
        // put a number in the journal that never applied to anything.
        if (dilution.toStdString() == std::string{kDilutionDiluted}) {
            action.dilutionFactor = DilutionFactor::fromDouble(factor);
        }
    }
    dispatchCapture(std::move(action));
}

void ResultPresenter::captureQualifier(AnalysisVersionId versionId, const QString& code) {
    dispatchCapture(CaptureConcentration{.analysisVersionId = versionId,
                                         .qualifier = QualifierChoice{code.toStdString()}});
}

void ResultPresenter::refreshResults() {
    track<ListResultsResult>(
        _sample.execute(ListResults{}),
        [this](ListResultsResult result) { emit resultsListed(std::move(result)); },
        [this](const std::exception_ptr& err) { reportError(err); });
}

void ResultPresenter::verifyResult(ResultId resultId) {
    track<VerificationView>(
        _sample.execute(VerifyResult{.resultId = resultId}),
        [this](VerificationView view) { emit resultVerified(std::move(view)); },
        [this](const std::exception_ptr& err) { reportError(err); });
}

void ResultPresenter::refreshConflicts() {
    track<ListConflictsResult>(
        _sample.execute(ListConflicts{}),
        [this](ListConflictsResult result) { emit conflictsListed(std::move(result)); },
        [this](const std::exception_ptr& err) { reportError(err); });
}

void ResultPresenter::resolveConflict(ConflictId conflictId, const QString& resolution, const QString& note) {
    // The two-value choice is spelled as a string at the QML boundary and
    // mapped to the enum here -- translation, which is this layer's whole job.
    // Anything that is not "apply" is a discard: the safe half of the pair,
    // since discarding leaves the server's own value standing.
    const auto decision =
        resolution == QStringLiteral("apply") ? ConflictResolution::ApplyAnyway : ConflictResolution::DiscardStale;
    track<ConflictView>(
        _sample.execute(
            ResolveConflict{.conflictId = conflictId, .resolution = decision, .note = note.toStdString()}),
        [this](ConflictView view) { emit conflictResolved(std::move(view)); },
        [this](const std::exception_ptr& err) { reportError(err); });
}

}  // namespace lims::gui

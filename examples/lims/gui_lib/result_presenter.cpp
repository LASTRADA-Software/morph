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

void ResultPresenter::refreshResults() {
    track<ListResultsResult>(
        _sample.execute(ListResults{}), [this](ListResultsResult result) { emit resultsListed(std::move(result)); },
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

}  // namespace lims::gui

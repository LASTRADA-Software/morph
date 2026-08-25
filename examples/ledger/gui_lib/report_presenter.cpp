// SPDX-License-Identifier: Apache-2.0
#include "report_presenter.hpp"

#include <glaze/glaze.hpp>
#include <utility>

#include "gui/error_text.hpp"

namespace ledger::gui {

ReportPresenter::ReportPresenter(::morph::bridge::Bridge& bridge, ::morph::exec::IExecutor* executor, QObject* parent)
    : Presenter{parent}, _bridge{bridge}, _handler{bridge, executor} {
    trackBound(_handler.whenBound());
}

void ReportPresenter::reportError(const std::exception_ptr& err) { emit failed(::morph::ladder::gui::errorText(err)); }

void ReportPresenter::submitMonthlyStatement(LedgerId ledgerId, int year, unsigned month, int timezoneOffsetMinutes) {
    // Encoded from the same type the model decodes into (report_dto.hpp), so
    // the two sides cannot drift over this JSON object's shape.
    const MonthlyStatementParams params{.year = year, .month = month, .timezoneOffsetMinutes = timezoneOffsetMinutes};
    std::string paramsJson;
    if (auto err = glz::write_json(params, paramsJson); err) {
        emit failed(QStringLiteral("could not encode report parameters"));
        return;
    }

    track<ReportJobId>(
        _handler.execute(
            SubmitReport{.ledgerId = ledgerId, .kind = ReportKind::MonthlyStatement, .params = std::move(paramsJson)}),
        [this](ReportJobId jobId) {
            emit submitted(jobId);
            // Replacing the previous poller destroys it, which stops its
            // timer and guarantees a stale job's callbacks cannot fire into
            // this presenter -- see `_poller`'s own doc comment.
            _poller = std::make_unique<ReportJobPoller>(
                _bridge, jobId,
                [this](ReportJobId id, ReportJobPoller::OnSuccess onSuccess, ReportJobPoller::OnError onError) {
                    _handler.execute(GetReportStatus{.jobId = id})
                        .then([onSuccess = std::move(onSuccess)](GetReportStatusResult result) {
                            onSuccess(std::move(result));
                        })
                        .onError([onError = std::move(onError)](std::exception_ptr err) { onError(std::move(err)); });
                },
                [this](std::string resultJson) { emit reportReady(QString::fromStdString(resultJson)); },
                [this](const QString& message) { emit failed(message); });
        },
        [this](const std::exception_ptr& err) { reportError(err); });
}

}  // namespace ledger::gui

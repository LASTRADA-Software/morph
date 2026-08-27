// SPDX-License-Identifier: Apache-2.0
#include "report_job_poller.hpp"

#include <utility>

#include "gui/error_text.hpp"

namespace ledger::gui {

ReportJobPoller::ReportJobPoller(::morph::bridge::Bridge& bridge, ReportJobId jobId, Dispatch dispatch, OnDone onDone,
                                 OnFailed onFailed, std::chrono::milliseconds interval,
                                 std::chrono::milliseconds executeDeadline)
    : _jobId{jobId}, _dispatch{std::move(dispatch)}, _onDone{std::move(onDone)}, _onFailed{std::move(onFailed)} {
    bridge.setExecuteDeadline(executeDeadline);
    // `&_timer` as the connection's context object, not `this`: this class is
    // not a QObject, and `_timer` is a member that always outlives the
    // connection it owns -- the identical idiom, and the identical reason,
    // EventPoller documents.
    QObject::connect(&_timer, &QTimer::timeout, &_timer, [this] { pollOnce(); });
    _timer.start(interval);
}

void ReportJobPoller::disarm() {
    _finished = true;
    _timer.stop();
}

void ReportJobPoller::pollOnce() {
    if (_finished) {
        return;
    }
    _dispatch(_jobId, _callbacks.guard([this](GetReportStatusResult result) {
        if (_finished) {
            return;
        }
        switch (result.status) {
            case ReportStatus::Done:
                // Disarm *before* the callback: a handler that reacts by
                // destroying this poller must not return into a live
                // timer, and a late reply from an already-dispatched tick
                // must not produce a second terminal callback.
                disarm();
                if (_onDone) {
                    _onDone(result.result.value_or(std::string{}));
                }
                return;
            case ReportStatus::Failed:
                disarm();
                if (_onFailed) {
                    _onFailed(QStringLiteral("report job failed"));
                }
                return;
            case ReportStatus::Pending:
            default:
                // Still working; the timer fires again.
                return;
        }
    }),
              _callbacks.guard([this](std::exception_ptr err) {
                  if (_finished) {
                      return;
                  }
                  // A dispatch error is terminal for this poller rather than
                  // retried: the deadline armed above already bounds one attempt,
                  // and silently retrying a failing call forever is how a "stuck at
                  // Pending" bug hides. The presenter above decides whether to
                  // resubmit.
                  disarm();
                  // The shared helper, not a fourth hand-written copy of it: this
                  // one called std::rethrow_exception with no null check, which is
                  // undefined behaviour on a null exception_ptr. `errorText`
                  // documents and handles that case, and the rung's four
                  // presenters already route through it.
                  if (_onFailed) {
                      _onFailed(::morph::ladder::gui::errorText(err));
                  }
              }));
}

}  // namespace ledger::gui

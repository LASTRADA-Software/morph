// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "gui/presenter.hpp"
#include "ledger/dto/report_dto.hpp"

#include <QObject>
#include <QString>

#include <exception>
#include <memory>

#ifndef Q_MOC_RUN
#include "ledger/models/ledger_model.hpp"
#include "report_job_poller.hpp"

#include <morph/core/bridge.hpp>
#include <morph/core/executor.hpp>
#endif

/// @file
/// `ReportPresenter` -- submit a report, then poll it to a terminal state.

namespace ledger::gui {

/// @brief Submits a report job and drives a `ReportJobPoller` until it
///        settles, re-emitting the outcome as Qt signals.
///
/// The submit->poll pair is the whole point: `SubmitReport` returns a job id
/// immediately rather than a body, so a view that wants a report has to wait
/// for one without blocking. This class owns that waiting.
class ReportPresenter : public ::morph::ladder::gui::Presenter {
    Q_OBJECT
  public:
    /// @param bridge   The shared `Bridge` `AppContext` owns.
    /// @param executor The executor `Completion` callbacks land on.
    /// @param parent   Optional `QObject` parent.
    ReportPresenter(::morph::bridge::Bridge& bridge, ::morph::exec::IExecutor* executor,
                    QObject* parent = nullptr);

    /// @brief Submits a monthly statement and begins polling it. Emits
    ///        `submitted` with the job id, then exactly one of
    ///        `reportReady` / `failed`.
    /// @param ledgerId The ledger to report on.
    /// @param year     The local calendar year.
    /// @param month    The local calendar month, 1-12.
    /// @param timezoneOffsetMinutes The client's offset from UTC, which
    ///        decides which transactions fall inside the month (Task 17).
    void submitMonthlyStatement(LedgerId ledgerId, int year, unsigned month, int timezoneOffsetMinutes);

  signals:
    /// @brief The job was accepted; polling has begun.
    /// @param jobId The job's id.
    void submitted(ledger::ReportJobId jobId);

    /// @brief The job reached `Done`.
    /// @param resultJson The serialized report body.
    void reportReady(QString resultJson);

    /// @brief The submit failed, or the job reached `Failed`, or polling
    ///        errored. Exactly one terminal signal fires per submission.
    /// @param message The failure text.
    void failed(QString message);

  private:
    /// @brief Re-emits @p err as `failed` carrying its `what()`.
    /// @param err The exception the completion carried.
    void reportError(const std::exception_ptr& err);

    ::morph::bridge::Bridge& _bridge;
    ::morph::bridge::BridgeHandler<LedgerModel, ::morph::bridge::AllowShared> _handler;

    /// @brief The in-flight poller, replaced on each submission.
    ///
    ///        Held by `unique_ptr` because a poller is single-use by design
    ///        (see `ReportJobPoller`): a second submission needs a second
    ///        poller, and destroying the previous one is what guarantees its
    ///        timer stops and its callbacks cannot fire against a stale job.
    std::unique_ptr<ReportJobPoller> _poller;
};

}  // namespace ledger::gui

// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <QString>
#include <QTimer>
#include <chrono>
#include <exception>
#include <functional>
#include <memory>
#include <string>

#ifndef Q_MOC_RUN
#include <morph/core/bridge.hpp>
#include <morph/core/callback_scope.hpp>

#include "ledger/dto/report_dto.hpp"
#endif

/// @file
/// `ReportJobPoller` -- polls one report job to one terminal state, then
/// stops for good.

namespace ledger::gui {

/// @brief Ticks `GetReportStatus(jobId)` on an interval until the job leaves
///        `Pending`, reports that outcome exactly once, and permanently
///        disarms.
///
/// Deliberately **not** `morph::ladder::gui::EventPoller`, despite the family
/// resemblance. `EventPoller` is shaped for an open-ended `GetEventsSince`
/// stream: apply N events per tick, advance a cursor, keep going, and
/// `resume()` after a resync. A report job has none of that shape -- it has
/// one job id, one terminal answer, and no meaningful "next". Forcing it into
/// `EventPoller` would mean a cursor that never advances, a `resume()` that
/// must never be called, and an event vector that is always empty or a
/// single-element stand-in for a completion. The two share a *pattern* -- the
/// `Dispatch` closure shape, the `CallbackScope`-gated callback, `&_timer` as
/// the connection's context object -- and reusing the pattern while writing a
/// distinct class is what the design brief asks for.
///
/// A finished job never restarts. There is no `resume()`: unlike an event
/// stream's resyncable cursor, a `Done` or `Failed` job is final, and a poller
/// that could be restarted would invite a second terminal callback for one
/// job.
class ReportJobPoller {
public:
    /// @brief Called with the job's serialized body once, on `Done`.
    using OnDone = std::function<void(std::string resultJson)>;

    /// @brief Called once, on `Failed` or a fatal dispatch error.
    using OnFailed = std::function<void(const QString& message)>;

    /// @brief Reports one poll's success back to the poller.
    using OnSuccess = std::function<void(GetReportStatusResult result)>;

    /// @brief Reports one poll's failure back to the poller.
    using OnError = std::function<void(std::exception_ptr)>;

    /// @brief Performs one `GetReportStatus` dispatch, answering through
    ///        exactly one of @p onSuccess / @p onError.
    ///
    ///        A closure rather than a `BridgeHandler` member for the reason
    ///        `EventPoller` documents: it keeps this class testable without a
    ///        live bridge, and keeps the handler's ownership with the
    ///        presenter that already has one.
    using Dispatch = std::function<void(ReportJobId jobId, OnSuccess onSuccess, OnError onError)>;

    /// @param bridge   Used only to arm the execute deadline -- see
    ///        `EventPoller`'s "setExecuteDeadline is bridge-wide" note, which
    ///        applies here unchanged.
    /// @param jobId    The job to poll.
    /// @param dispatch Performs one `GetReportStatus`.
    /// @param onDone   Fires once, with the body, when the job reaches `Done`.
    /// @param onFailed Fires once when the job reaches `Failed`, or when a
    ///        dispatch error arrives.
    /// @param interval How often to poll.
    /// @param executeDeadline Forwarded to `bridge.setExecuteDeadline()`.
    ReportJobPoller(::morph::bridge::Bridge& bridge, ReportJobId jobId, Dispatch dispatch, OnDone onDone,
                    OnFailed onFailed, std::chrono::milliseconds interval = std::chrono::seconds{2},
                    std::chrono::milliseconds executeDeadline = std::chrono::seconds{5});

    ReportJobPoller(const ReportJobPoller&) = delete;
    ReportJobPoller& operator=(const ReportJobPoller&) = delete;
    ReportJobPoller(ReportJobPoller&&) = delete;
    ReportJobPoller& operator=(ReportJobPoller&&) = delete;
    ~ReportJobPoller() = default;

    /// @return Whether this poller has already reported a terminal outcome.
    [[nodiscard]] bool finished() const noexcept { return _finished; }

private:
    /// @brief One tick: dispatches `GetReportStatus` unless already finished.
    void pollOnce();

    /// @brief Stops the timer and marks this poller spent, so no later
    ///        in-flight reply can produce a second terminal callback.
    void disarm();

    ReportJobId _jobId;
    Dispatch _dispatch;
    OnDone _onDone;
    OnFailed _onFailed;
    bool _finished{false};
    QTimer _timer;

    /// @brief Lifetime gate for `pollOnce()`'s `Dispatch` reply callbacks.
    ///
    /// `ReportJobPoller` is not a `QObject`; `_dispatch`'s `onSuccess`/
    /// `onError` are plain `std::function`-based callbacks with no
    /// Qt-provided auto-disconnect, and both capture raw `this`.
    /// `morph::async::CallbackScope` (`docs/spec/core/callback_scope.md`) is
    /// the framework's general answer, composing with this class exactly as
    /// it would with a `QObject` — it is a data member, not a base class.
    ///
    /// **Declared last, destroyed first**: an in-flight reply that resolves
    /// after this poller dies finds its token inactive and returns instead of
    /// touching freed members. Same declared-last requirement as
    /// `EventPoller`'s own `_liveness`.
    ::morph::async::CallbackScope _callbacks;
};

}  // namespace ledger::gui

// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <QString>
#include <exception>

#include "gui/presenter.hpp"
#include "lims/dto/analysis_dto.hpp"
#include "lims/dto/offline_dto.hpp"
#include "lims/dto/result_dto.hpp"
#include "lims/dto/verification_dto.hpp"

// See `sample_presenter.hpp`'s identical guard: moc must not be pointed at
// morph's bridge header or this rung's model headers.
#ifndef Q_MOC_RUN
#include <morph/core/bridge.hpp>
#include <morph/core/executor.hpp>

#include "lims/models/analysis_catalog_model.hpp"
#include "lims/models/sample_model.hpp"
#endif

namespace lims::gui {

/// @brief Drives the result-entry surface: the catalogue (read-only), result
///        capture, verification, and the conflicts offline replay flagged.
///
/// Two handlers, because two models are involved: the catalogue is lab-wide
/// and unkeyed, the sample is keyed and shared. Both live here rather than in
/// two presenters because the catalogue's only job on this surface is to
/// populate the analysis picker the capture form submits against — a listing
/// with no screen of its own.
///
/// Every action this surface sends to `SampleModel` runs *after* an attach,
/// so unlike `SamplePresenter` it needs no second, plain handler: there is no
/// key-less `SampleModel` action here to be defeated by an `AllowShared`
/// handler's "not bound until attached" rule.
///
/// @par Where the exactness lives: not here
/// A reading never crosses this class as a `double`. Capture is submitted as
/// a schema-driven form body -- `submitIfValid("CaptureConcentration", ...)`
/// -- so the number travels as the exact rational the shipped renderer built
/// from the served `x-decimalPlaces`, and the model checks that precision
/// against the analysis version's own declaration. An earlier
/// `captureReading(versionId, double, ...)` entry point converted the value
/// with `Concentration::fromDouble` at the field's declared precision, which
/// was exact but *rounding*: it made the model's over-precision refusal (the
/// rung README's §3 decision 7) unreachable from the GUI, and it put the only
/// `double` on this rung's QML surface, against the convention
/// `lims_qml_conversions.hpp` states. It was deleted rather than documented
/// (morph#287).
class ResultPresenter : public ::morph::ladder::gui::Presenter {
    Q_OBJECT
public:
    /// @param bridge   The shared `Bridge` `AppContext` owns.
    /// @param executor The executor `Completion` callbacks land on.
    /// @param parent   Optional `QObject` parent.
    ResultPresenter(::morph::bridge::Bridge& bridge, ::morph::exec::IExecutor* executor, QObject* parent = nullptr);

    /// @brief Lists every analysis's current version. Emits `analysesListed`,
    ///        or `failed`.
    void refreshAnalyses();

    /// @brief Attaches this surface's handler to a sample. Emits
    ///        `sampleAttached`, or `failed`.
    /// @param sampleId The sample to attach to.
    void openSample(SampleId sampleId);

    /// @brief Lists the attached sample's results. Emits `resultsListed`, or
    ///        `failed`.
    void refreshResults();

    /// @brief Records the four-eyes verification of one result. Emits
    ///        `resultVerified`, or `failed`.
    /// @param resultId The result to verify.
    void verifyResult(ResultId resultId);

    /// @brief Lists the conflicts offline replay flagged against the attached
    ///        sample. Emits `conflictsListed`, or `failed`.
    void refreshConflicts();

    /// @brief Dispatches @p bodyJson as @p actionType's body — the
    ///        schema-driven path the shipped `DynamicForm` submits through.
    ///
    /// Both actions this surface renders forms for act on the attached
    /// sample, so unlike `SamplePresenter::submitIfValid` there is no handler
    /// to choose between. An action this surface does not own is refused
    /// rather than dispatched.
    /// @param actionType `CaptureConcentration` or `ResolveConflict`.
    /// @param bodyJson The form's assembled JSON body.
    void submitIfValid(const QString& actionType, const QString& bodyJson);

signals:
    /// @brief Emitted once per `submitIfValid`, whichever way it resolved.
    /// @param actionType The action the reply belongs to.
    /// @param ok Whether it succeeded.
    /// @param payload The result JSON on success, the error text on failure.
    void replyReceived(QString actionType, bool ok, QString payload);

    /// @brief `ListAnalyses` succeeded.
    /// @param result Every analysis's current version.
    void analysesListed(lims::ListAnalysesResult result);
    /// @brief `OpenSample` succeeded on this surface's own handler.
    /// @param view The attached sample.
    void sampleAttached(lims::SampleView view);
    /// @brief `ListResults` succeeded.
    /// @param result The attached sample's results.
    void resultsListed(lims::ListResultsResult result);
    /// @brief `VerifyResult` succeeded.
    /// @param view The recorded verification.
    void resultVerified(lims::VerificationView view);
    /// @brief `ListConflicts` succeeded.
    /// @param result The attached sample's flagged conflicts.
    void conflictsListed(lims::ListConflictsResult result);
    /// @brief Any action's typed error, as `std::exception::what()`.
    /// @param message Ready for direct display.
    void failed(QString message);

private:
    /// @brief Shared error body passed as every `track()` call's third
    ///        argument.
    /// @param err The failed completion's exception.
    void reportError(const std::exception_ptr& err);

#ifndef Q_MOC_RUN
    ::morph::bridge::BridgeHandler<AnalysisCatalogModel> _catalog;
    ::morph::bridge::BridgeHandler<SampleModel, ::morph::bridge::AllowShared> _sample;
#endif
};

}  // namespace lims::gui

// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <QString>
#include <exception>

#include "gui/presenter.hpp"
#include "lims/dto/sample_dto.hpp"

// moc is not a C++ front end and mis-parses morph's template machinery, so it
// must never see `morph/core/bridge.hpp` or this rung's model headers — the
// same guard, for the same reason, as
// `kanban::gui::ProjectAdminPresenter`'s (examples/kanban/gui_lib/
// project_admin_presenter.hpp).
#ifndef Q_MOC_RUN
#include <morph/core/bridge.hpp>
#include <morph/core/executor.hpp>

#include "lims/models/sample_model.hpp"
#endif

namespace lims::gui {

/// @brief Drives `lims::SampleModel`'s registration and lifecycle surface.
///
/// Translates and routes; it never decides (`examples/IMPLEMENTATION.md`
/// rule 2). Every guard that matters — which transitions are legal, who may
/// make them, whether a sample may be published — lives in the model and is
/// re-checked there on every dispatch. This class turns a typed result into a
/// Qt signal and a typed error into a displayable string, and that is all it
/// does.
///
/// @par Two handlers, and why
/// `SampleModel` is keyed, so the handler every attached action runs on is
/// `AllowShared`: a bench client and an office client that both
/// `openSample(id)` land on the same instance, which is the property the
/// rung's shared-instance design exists for.
///
/// But an `AllowShared` handler is **not bound until it attaches to a key**,
/// and `RegisterClient` carries no key at all — dispatching it there fails
/// with "handler not bound" (confirmed empirically here, and the same
/// conclusion `polls::gui::PollPresenter` reached for `CreatePoll`). So that
/// one action runs on a plain `NoSharing` handler, which registers a fresh
/// instance on construction and is bound immediately.
///
/// `RegisterSample` needs no such help even though it too arrives before any
/// key exists: it is **result-keyed**, so `BridgeHandler::execute`'s
/// `ResultKeyed` branch runs it on an anonymous instance and promotes that
/// instance to the id the result names, before the completion resolves. An
/// earlier draft of this class dispatched it on the plain handler and then
/// re-attached the shared one by hand — a reimplementation of that branch,
/// removed once `test_backend_matrix.cpp` proved the framework's own path
/// works in all three deployment modes.
class SamplePresenter : public ::morph::ladder::gui::Presenter {
    Q_OBJECT
public:
    /// @param bridge   The shared `Bridge` `AppContext` owns.
    /// @param executor The executor `Completion` callbacks land on.
    /// @param parent   Optional `QObject` parent.
    SamplePresenter(::morph::bridge::Bridge& bridge, ::morph::exec::IExecutor* executor, QObject* parent = nullptr);

    /// @brief Registers a client samples can belong to. Emits
    ///        `clientRegistered`, or `failed`.
    /// @param name The client's name.
    void registerClient(const QString& name);

    /// @brief Logs a new sample and attaches this handler to it. Emits
    ///        `sampleChanged`, or `failed`.
    /// @param clientId  The owning client.
    /// @param reference The lab's reference for the container.
    void registerSample(ClientId clientId, const QString& reference);

    /// @brief Attaches to an existing sample. Emits `sampleChanged`, or
    ///        `failed`.
    /// @param sampleId The sample to attach to.
    void openSample(SampleId sampleId);

    /// @brief Re-reads the attached sample. Emits `sampleChanged`, or
    ///        `failed`.
    void refresh();

    /// @brief `Registered → Received`. Emits `sampleChanged`, or `failed`.
    void receiveSample();

    /// @brief `Received → InProgress`. Emits `sampleChanged`, or `failed`.
    void startWork();

    /// @brief `InProgress → ToBeVerified`. Emits `sampleChanged`, or `failed`.
    void submitForVerification();

    /// @brief `ToBeVerified → InProgress`. Emits `sampleChanged`, or `failed`.
    /// @param reason Why the sample went back to the bench.
    void returnForRework(const QString& reason);

    /// @brief `ToBeVerified → Published`. Emits `sampleChanged`, or `failed`.
    void publishSample();

    /// @brief `Registered|Received → Rejected`. Emits `sampleChanged`, or
    ///        `failed`.
    /// @param reason Why the container is refused.
    void rejectSample(const QString& reason);

    /// @brief Dispatches @p bodyJson as @p actionType's body, the
    ///        schema-driven path the shipped `DynamicForm` submits through.
    ///
    /// Routed to the right handler by action type, which is the one thing a
    /// generic `executeJson` cannot work out for itself here: the two
    /// registration actions carry no key and must go to the plain handler,
    /// everything else to the attached shared one (see the class doc
    /// comment). An action this surface does not own is refused rather than
    /// dispatched, so a typo in a QML `actionType` string surfaces as an
    /// error instead of a silent no-op.
    /// @param actionType One of `RegisterClient`, `RegisterSample`,
    ///        `RejectSample`, `ReturnForRework`.
    /// @param bodyJson The form's assembled JSON body.
    void submitIfValid(const QString& actionType, const QString& bodyJson);

signals:
    /// @brief Emitted once per `submitIfValid`, whichever way it resolved.
    /// @param actionType The action the reply belongs to.
    /// @param ok Whether it succeeded.
    /// @param payload The result JSON on success, the error text on failure.
    void replyReceived(QString actionType, bool ok, QString payload);

    /// @brief `RegisterClient` succeeded.
    /// @param result The new client's id.
    void clientRegistered(lims::RegisterClientResult result);

    /// @brief The attached sample's state, after whatever just changed it.
    ///        One signal for registration, attach, refresh and every
    ///        transition, because every one of them returns the same
    ///        `SampleView` and the view has nothing to gain from telling
    ///        them apart.
    /// @param view The sample's full current state.
    void sampleChanged(lims::SampleView view);

    /// @brief Any action's typed error, as `std::exception::what()`.
    /// @param message Ready for direct display.
    void failed(QString message);

private:
    /// @brief Shared error body passed as every `track()` call's third
    ///        argument — see `Presenter::track()`.
    /// @param err The failed completion's exception.
    void reportError(const std::exception_ptr& err);

    /// @brief Dispatches @p action and emits `sampleChanged` with its result.
    ///
    /// Every lifecycle transition has the same shape, so they share one body
    /// rather than six copies of it.
    /// @tparam Action The transition action type.
    /// @param action The transition to dispatch.
    template <typename Action>
    void dispatchTransition(Action action);

#ifndef Q_MOC_RUN
    /// @brief Plain handler for the two key-less actions — see the class doc
    ///        comment. Never used for anything attached.
    ::morph::bridge::BridgeHandler<SampleModel> _creator;

    /// @brief The shared, keyed handler every attached action runs on.
    ::morph::bridge::BridgeHandler<SampleModel, ::morph::bridge::AllowShared> _handler;
#endif
};

}  // namespace lims::gui

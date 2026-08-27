// SPDX-License-Identifier: Apache-2.0
#include "sample_presenter.hpp"

#include <QStringList>
#include <glaze/glaze.hpp>
#include <utility>

#include "gui/error_text.hpp"

namespace lims::gui {

SamplePresenter::SamplePresenter(::morph::bridge::Bridge& bridge, ::morph::exec::IExecutor* executor, QObject* parent)
    : Presenter{parent}, _creator{bridge, executor}, _handler{bridge, executor} {
    // The *plain* handler's binding is the one that gates the first dispatch
    // a view can make (`submitIfValid("RegisterClient", ...)`). The shared
    // handler is unbound by design until something attaches it, so gating on
    // it would never fire.
    trackBound(_creator.whenBound());
}

void SamplePresenter::reportError(const std::exception_ptr& err) { emit failed(::morph::ladder::gui::errorText(err)); }

template <typename Action>
void SamplePresenter::dispatchTransition(Action action) {
    track<SampleView>(
        _handler.execute(std::move(action)), [this](SampleView view) { emit sampleChanged(std::move(view)); },
        [this](const std::exception_ptr& err) { reportError(err); });
}

void SamplePresenter::submitIfValid(const QString& actionType, const QString& bodyJson) {
    // `RegisterClient` is the one action on this surface with no key at all
    // (no `BRIDGE_KEY_FROM_RESULT` names it) — dispatching it on the *shared*
    // handler fails fast with "handler not bound" (see the class doc
    // comment), so it alone runs on the plain `_creator`. Every other action
    // here, `RegisterSample` included, runs on the shared `_handler`:
    // `RegisterSample` is result-keyed, so `BridgeHandler::execute`'s
    // `ResultKeyed` branch runs it on an anonymous instance and promotes that
    // instance to the id the result names before the completion resolves —
    // the shared handler is attached to the new sample by the time this
    // returns, exactly as the direct `_handler.execute(RegisterSample{...})`
    // call this class used to expose as a typed invokable did.
    static const QString kRegisterClient = QStringLiteral("RegisterClient");
    static const QStringList kOwned{kRegisterClient, QStringLiteral("RegisterSample"), QStringLiteral("RejectSample"),
                                    QStringLiteral("ReturnForRework")};

    if (!kOwned.contains(actionType)) {
        emit replyReceived(actionType, false,
                           QStringLiteral("SamplePresenter does not own the action '%1'").arg(actionType));
        return;
    }

    // The two handlers are different *types* (`NoSharing` vs `AllowShared`),
    // so the choice cannot be a reference selected by `?:` — the completion
    // is picked instead, which is the value both branches actually produce.
    auto completion = actionType == kRegisterClient
                          ? _creator.executeJson(actionType.toStdString(), bodyJson.toStdString())
                          : _handler.executeJson(actionType.toStdString(), bodyJson.toStdString());
    track<std::string>(
        std::move(completion),
        [this, actionType](std::string payload) {
            emit replyReceived(actionType, true, QString::fromStdString(payload));
            // `RegisterClient`'s result carries the new client's id, and
            // `clientRegistered` is the only signal that ever sets
            // `SampleBridge::clientId` — `SampleView.qml`'s "Latest client
            // id" label binds it. The form path yields raw JSON rather than a
            // typed result, so it is decoded here with the same glaze
            // reflection the wire used, the way `bookmarks`' `FormsBridge`
            // decodes its own form replies.
            if (actionType == kRegisterClient) {
                if (RegisterClientResult result; !glz::read_json(result, payload)) {
                    emit clientRegistered(std::move(result));
                }
                return;
            }
            // Every other action here acts on the attached sample (or, for
            // `RegisterSample`, just attached it above), so the typed view
            // state is re-read rather than parsed back out of the reply —
            // one refresh rather than a second decoder that could disagree
            // with the first.
            refresh();
        },
        [this, actionType](const std::exception_ptr& err) {
            emit replyReceived(actionType, false, ::morph::ladder::gui::errorText(err));
        });
}

void SamplePresenter::openSample(SampleId sampleId) { dispatchTransition(OpenSample{.sampleId = sampleId}); }

void SamplePresenter::refresh() { dispatchTransition(GetSample{}); }

void SamplePresenter::receiveSample() { dispatchTransition(ReceiveSample{}); }

void SamplePresenter::startWork() { dispatchTransition(StartWork{}); }

void SamplePresenter::submitForVerification() { dispatchTransition(SubmitForVerification{}); }

void SamplePresenter::publishSample() { dispatchTransition(PublishSample{}); }

}  // namespace lims::gui

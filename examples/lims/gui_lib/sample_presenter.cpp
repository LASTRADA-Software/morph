// SPDX-License-Identifier: Apache-2.0
#include "sample_presenter.hpp"

#include <QStringList>
#include <utility>

namespace lims::gui {

SamplePresenter::SamplePresenter(::morph::bridge::Bridge& bridge, ::morph::exec::IExecutor* executor, QObject* parent)
    : Presenter{parent}, _creator{bridge, executor}, _handler{bridge, executor} {
    // The *plain* handler's binding is the one that gates the first dispatch
    // a view can make (registerClient). The shared handler is unbound by
    // design until something attaches it, so gating on it would never fire.
    trackBound(_creator.whenBound());
}

void SamplePresenter::reportError(const std::exception_ptr& err) {
    try {
        std::rethrow_exception(err);
    } catch (const std::exception& ex) {
        emit failed(QString::fromStdString(ex.what()));
    }
}

template <typename Action>
void SamplePresenter::dispatchTransition(Action action) {
    track<SampleView>(
        _handler.execute(std::move(action)), [this](SampleView view) { emit sampleChanged(std::move(view)); },
        [this](const std::exception_ptr& err) { reportError(err); });
}

void SamplePresenter::submitIfValid(const QString& actionType, const QString& bodyJson) {
    // The two key-less registrations go to the plain handler; every other
    // form on this surface acts on the already-attached sample.
    static const QStringList kKeyless{QStringLiteral("RegisterClient"), QStringLiteral("RegisterSample")};
    static const QStringList kAttached{QStringLiteral("RejectSample"), QStringLiteral("ReturnForRework")};

    if (!kKeyless.contains(actionType) && !kAttached.contains(actionType)) {
        emit replyReceived(actionType, false,
                           QStringLiteral("SamplePresenter does not own the action '%1'").arg(actionType));
        return;
    }

    // The two handlers are different *types* (`NoSharing` vs `AllowShared`),
    // so the choice cannot be a reference selected by `?:` — the completion
    // is picked instead, which is the value both branches actually produce.
    auto completion = kKeyless.contains(actionType)
                          ? _creator.executeJson(actionType.toStdString(), bodyJson.toStdString())
                          : _handler.executeJson(actionType.toStdString(), bodyJson.toStdString());
    track<std::string>(
        std::move(completion),
        [this, actionType](std::string payload) {
            emit replyReceived(actionType, true, QString::fromStdString(payload));
            // The form path yields raw JSON, so the typed view state has to
            // be re-read rather than parsed back out of it — one refresh
            // rather than a second decoder that could disagree with the
            // first.
            refresh();
        },
        [this, actionType](const std::exception_ptr& err) {
            try {
                std::rethrow_exception(err);
            } catch (const std::exception& ex) {
                emit replyReceived(actionType, false, QString::fromStdString(ex.what()));
            }
        });
}

void SamplePresenter::registerClient(const QString& name) {
    track<RegisterClientResult>(
        _creator.execute(RegisterClient{.name = name.toStdString()}),
        [this](RegisterClientResult result) { emit clientRegistered(std::move(result)); },
        [this](const std::exception_ptr& err) { reportError(err); });
}

void SamplePresenter::registerSample(ClientId clientId, const QString& reference) {
    // On the *shared* handler, and one dispatch, not two: `RegisterSample` is
    // result-keyed, so the framework runs it on an anonymous instance and
    // promotes that instance to the id the result names before the completion
    // resolves (`BridgeHandler::execute`'s `ResultKeyed` branch). The handler
    // is therefore attached to the new sample by the time this returns, with
    // no chaining of our own — an earlier draft here dispatched
    // `RegisterSample` on the plain handler and then re-attached the shared
    // one by hand, which was a reimplementation of exactly that branch.
    dispatchTransition(RegisterSample{.clientId = clientId, .reference = reference.toStdString()});
}

void SamplePresenter::openSample(SampleId sampleId) { dispatchTransition(OpenSample{.sampleId = sampleId}); }

void SamplePresenter::refresh() { dispatchTransition(GetSample{}); }

void SamplePresenter::receiveSample() { dispatchTransition(ReceiveSample{}); }

void SamplePresenter::startWork() { dispatchTransition(StartWork{}); }

void SamplePresenter::submitForVerification() { dispatchTransition(SubmitForVerification{}); }

void SamplePresenter::returnForRework(const QString& reason) {
    dispatchTransition(ReturnForRework{.reason = reason.toStdString()});
}

void SamplePresenter::publishSample() { dispatchTransition(PublishSample{}); }

void SamplePresenter::rejectSample(const QString& reason) {
    dispatchTransition(RejectSample{.reason = reason.toStdString()});
}

}  // namespace lims::gui

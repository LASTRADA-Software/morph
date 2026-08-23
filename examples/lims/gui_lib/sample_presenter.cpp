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
    // Create on the plain handler, then attach the shared one to the id that
    // came back — see the class doc comment for why this is two dispatches.
    // Only the attach's own result is announced, so the view sees one
    // `sampleChanged` describing a sample that is genuinely attached.
    track<SampleView>(
        _creator.execute(RegisterSample{.clientId = clientId, .reference = reference.toStdString()}),
        [this](SampleView created) { openSample(created.id); },
        [this](const std::exception_ptr& err) { reportError(err); });
}

void SamplePresenter::openSample(SampleId sampleId) {
    dispatchTransition(OpenSample{.sampleId = sampleId});
}

void SamplePresenter::refresh() {
    dispatchTransition(GetSample{});
}

void SamplePresenter::receiveSample() {
    dispatchTransition(ReceiveSample{});
}

void SamplePresenter::startWork() {
    dispatchTransition(StartWork{});
}

void SamplePresenter::submitForVerification() {
    dispatchTransition(SubmitForVerification{});
}

void SamplePresenter::returnForRework(const QString& reason) {
    dispatchTransition(ReturnForRework{.reason = reason.toStdString()});
}

void SamplePresenter::publishSample() {
    dispatchTransition(PublishSample{});
}

void SamplePresenter::rejectSample(const QString& reason) {
    dispatchTransition(RejectSample{.reason = reason.toStdString()});
}

}  // namespace lims::gui

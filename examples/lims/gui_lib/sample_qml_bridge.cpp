// SPDX-License-Identifier: Apache-2.0
#include "sample_qml_bridge.hpp"

#include <utility>

#include "lims_qml_conversions.hpp"
#include "lims_schemas.hpp"

namespace lims::gui {

SampleBridge::SampleBridge(::morph::bridge::Bridge& bridge, ::morph::exec::IExecutor* executor, QObject* parent)
    : QObject{parent}, _presenter{bridge, executor} {
    // Direct (same-thread) connections throughout, so no meta-type
    // registration is needed for the DTO-carrying signals -- the same note
    // kanban's bridges carry.
    connect(&_presenter, &SamplePresenter::bound, this, &SampleBridge::bound);
    connect(&_presenter, &SamplePresenter::sampleChanged, this, [this](SampleView view) {
        _sample = toVariantMap(view);
        emit sampleChanged(_sample);
    });
    connect(&_presenter, &SamplePresenter::clientRegistered, this, [this](RegisterClientResult result) {
        _clientId = idNumber(result.clientId);
        emit clientRegistered(_clientId);
    });
    connect(&_presenter, &SamplePresenter::replyReceived, this, &SampleBridge::replyReceived);
    connect(&_presenter, &SamplePresenter::failed, this, [this](QString message) {
        _lastError = std::move(message);
        emit failed(_lastError);
    });
}

void SampleBridge::registerClient(const QString& name) { _presenter.registerClient(name); }

void SampleBridge::registerSample(qlonglong clientId, const QString& reference) {
    _presenter.registerSample(ClientId{static_cast<std::int64_t>(clientId)}, reference);
}

void SampleBridge::openSample(qlonglong sampleId) {
    _presenter.openSample(SampleId{static_cast<std::int64_t>(sampleId)});
}

void SampleBridge::refresh() { _presenter.refresh(); }

void SampleBridge::receiveSample() { _presenter.receiveSample(); }

void SampleBridge::startWork() { _presenter.startWork(); }

void SampleBridge::submitForVerification() { _presenter.submitForVerification(); }

void SampleBridge::returnForRework(const QString& reason) { _presenter.returnForRework(reason); }

void SampleBridge::publishSample() { _presenter.publishSample(); }

void SampleBridge::rejectSample(const QString& reason) { _presenter.rejectSample(reason); }

QString SampleBridge::schemasJson() const { return QString::fromStdString(limsSchemasJson()); }

void SampleBridge::submitIfValid(const QString& actionType, const QString& bodyJson) {
    _presenter.submitIfValid(actionType, bodyJson);
}

}  // namespace lims::gui

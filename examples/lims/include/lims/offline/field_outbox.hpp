// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <array>
#include <cstdint>
#include <map>
#include <memory>
#include <morph/core/registry.hpp>
#include <morph/offline/offline_queue.hpp>
#include <random>
#include <string>
#include <utility>

#include "lims/dto/offline_dto.hpp"

/// @file
/// The field client's **write path** for offline capture (README §7).
///
/// @par Why this is not a model, and why that is a finding
/// `examples/IMPLEMENTATION.md` rule 1 says all domain logic lives in models.
/// It cannot here. `docs/spec/offline/offline.md` ("Ownership: who enqueues")
/// is explicit that the framework supplies no seam for it — *"detecting an
/// offline/failed `execute()` and calling `enqueue()` is the application's
/// job"*, and its own worked example puts that code at the dispatch site. A
/// disconnected field client has no model to put it in either: a rung's models
/// live server-side behind Lightweight/ODBC (rule 4's WASM clause), so the one
/// machine that must decide "queue this instead of sending it" is the one
/// machine with no model on it. See `docs/findings/008`.
///
/// @par What it actually does: chain a client's own edits
/// The trap the README names, and the one ODK Central hit: a client that edits
/// the same sample twice offline must stamp its *second* update with the
/// version its *first queued* update will produce, not with the version the
/// server last showed it. Stamp both with the server's version and replay
/// flags the client's own second edit as a conflict with itself.
///
/// `FieldOutbox` is therefore a tiny local version ledger. It records the last
/// version it saw from the server per sample, and advances that record by one
/// every time it queues an update — mirroring the single bump
/// `SampleModel` applies per captured result. The mirroring is a real coupling
/// and is named as such in the rung README: if the server's version arithmetic
/// ever stops being "+1 per applied capture", this prediction silently starts
/// manufacturing false conflicts.

namespace lims::offline {

/// @brief A field client's outbox: stamps each capture with the sample version
///        it was prepared against and queues it for replay on reconnect.
///
/// One instance per field client (it holds *that* client's local view). Not
/// thread-safe: a field client is one operator on one device.
class FieldOutbox {
public:
    /// @param queue The durable queue replay will drain. Shared with whatever
    ///        drains it; the outbox never drains.
    /// @param principal The operator capturing readings on this device.
    FieldOutbox(std::shared_ptr<::morph::offline::IOfflineQueue> queue, std::string principal)
        : _queue{std::move(queue)}, _principal{std::move(principal)} {}

    /// @brief Records the sample state this client last saw from the server.
    ///
    /// Called after any successful read while still online. Resets the local
    /// prediction to ground truth, which is what makes a client that
    /// reconnects, re-reads and goes offline again start from the right base.
    /// @param sample The sample as the server last reported it.
    void observe(const SampleView& sample) { _localVersion[*sample.id] = *sample.version; }

    /// @brief Queues @p capture against @p sampleId and advances this client's
    ///        local view of that sample's version.
    ///
    /// The returned envelope is the exact value serialised into the queue, so
    /// a caller can assert on what it queued without decoding the payload
    /// back out.
    /// @param sampleId The sample being captured against.
    /// @param capture What to record.
    /// @return The queued envelope, already enqueued.
    QueuedCapture enqueue(SampleId sampleId, const CaptureConcentration& capture) {
        const auto base = _localVersion[*sampleId];
        QueuedCapture queued{
            .sampleId = sampleId,
            .baseVersion = SampleVersion{base},
            .capturedBy = _principal,
            .operationKey = OperationKey{mintOperationKey()},
            .capture = capture,
        };
        // The same token in both places: the queue's own dedup slot, and the
        // payload, so replay can enforce at-most-once however the item arrives.
        _queue->enqueue(::morph::model::ActionTraits<QueuedCapture>::toJson(queued), *queued.operationKey);
        // The client's *own* next edit of this sample chains onto this one.
        _localVersion[*sampleId] = base + 1;
        return queued;
    }

    /// @brief This client's current local view of @p sampleId's version.
    /// @param sampleId The sample to ask about.
    /// @return The version the next queued update would be stamped with.
    [[nodiscard]] SampleVersion localVersion(SampleId sampleId) const {
        const auto found = _localVersion.find(*sampleId);
        return SampleVersion{found == _localVersion.end() ? 0 : found->second};
    }

private:
    /// @brief Mints a fresh dedup token for one logical field update.
    ///
    /// A random 128-bit id, which is exactly what
    /// `docs/spec/offline/offline.md` recommends ("a client-generated
    /// operation id (e.g. a UUID)"). It is minted **once**, stored in the
    /// payload and on the queue row, and never recomputed — which is what
    /// makes it stable across re-enqueues and process restarts, as that
    /// contract requires.
    ///
    /// A counter would be the obvious alternative and is wrong here: this
    /// object holds it in memory, so a device that is switched off (the normal
    /// end of a field shift) restarts the count at zero and mints keys that
    /// collide with genuinely different earlier edits. Replay's at-most-once
    /// check would then silently *skip* a real reading. That was not
    /// hypothetical — an earlier draft of this class used
    /// `principal/sample/base/sequence`, and the rung's own self-conflict test
    /// caught two distinct edits colliding on one key.
    /// @return A fresh, unguessable, collision-free token.
    [[nodiscard]] static std::string mintOperationKey() {
        static constexpr char kHex[] = "0123456789abcdef";
        std::random_device entropy;
        std::uniform_int_distribution<int> nibble{0, 15};
        std::string out;
        out.reserve(32);
        for (int i = 0; i < 32; ++i) {
            out.push_back(kHex[static_cast<std::size_t>(nibble(entropy))]);
        }
        return out;
    }

    std::shared_ptr<::morph::offline::IOfflineQueue> _queue;
    std::string _principal;
    std::map<std::int64_t, std::int64_t> _localVersion;
};

}  // namespace lims::offline

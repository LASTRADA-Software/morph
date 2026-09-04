// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <map>
#include <memory>
#include <morph/core/registry.hpp>
#include <morph/offline/offline_queue.hpp>
#include <random>
#include <string>
#include <utility>

#include "crm/dto/offline_dto.hpp"

/// @file
/// A field rep's **write path** for offline opportunity edits (README build
/// order §8). Same shape, and same reasoning, as `lims::offline::FieldOutbox`
/// (that rung's §7) — see that header's own doc comment for the full
/// argument (`examples/IMPLEMENTATION.md` rule 1's carve-out, morph#197):
/// the framework supplies no seam for "detect an offline `execute()` and
/// queue instead", so the app decides that at the dispatch site, and this
/// class is that decision's domain-shaped half for crm's own entity.
///
/// @par What it actually does: chain a rep's own edits
/// The same ODK Central trap lims's rung names: a rep who edits the same
/// opportunity twice offline must stamp the *second* edit with the version
/// the *first queued* edit will produce, not with the version the server
/// last showed. `FieldOutbox` is a tiny local version ledger for exactly
/// that, mirroring the single +1 `OpportunityModel::execute(UpdateOpportunity)`
/// applies per accepted edit — a real coupling, named here as it is there: if
/// the server's version arithmetic ever stops being "+1 per applied update",
/// this prediction starts manufacturing false conflicts.

namespace crm::offline {

/// @brief A field rep's outbox: stamps each opportunity edit with the version
///        it was prepared against and queues it for replay on reconnect.
///
/// One instance per field client. Not thread-safe — one operator, one device,
/// same as `lims::offline::FieldOutbox`.
class FieldOutbox {
public:
    /// @param queue The durable queue replay will drain. Shared with whatever
    ///        drains it; the outbox never drains.
    /// @param principal The operator editing opportunities on this device.
    FieldOutbox(std::shared_ptr<::morph::offline::IOfflineQueue> queue, std::string principal)
        : _queue{std::move(queue)}, _principal{std::move(principal)} {}

    /// @brief Records the opportunity state this client last saw from the
    ///        server. Called after any successful read while still online.
    /// @param opportunity The opportunity as the server last reported it.
    void observe(const OpportunityView& opportunity) { _localVersion[*opportunity.id] = opportunity.version; }

    /// @brief Queues an edit against @p opportunityId and advances this
    ///        client's local view of that opportunity's version.
    /// @param opportunityId The opportunity being edited.
    /// @param account The account choice as this edit leaves it.
    /// @param primaryContact The primary contact choice as this edit leaves it.
    /// @param name The name as this edit leaves it.
    /// @param expectedCloseValue The expected close value as this edit leaves it.
    /// @return The queued envelope, already enqueued.
    QueuedOpportunityUpdate enqueue(OpportunityId opportunityId, OpportunityAccountChoice account,
                                    PrimaryContactChoice primaryContact, std::string name, Money expectedCloseValue) {
        const auto base = _localVersion[*opportunityId];
        QueuedOpportunityUpdate queued{
            .opportunityId = opportunityId,
            .baseVersion = base,
            .capturedBy = _principal,
            .operationKey = OperationKey{mintOperationKey()},
            .account = std::move(account),
            .primaryContact = std::move(primaryContact),
            .name = std::move(name),
            .expectedCloseValue = expectedCloseValue,
        };
        // The same token in both places: the queue's own dedup slot and the
        // payload, so replay can enforce at-most-once however it arrives. The
        // queue-local id is discarded on purpose: dedup and replay both key
        // on operationKey, and this outbox's own contract returns the
        // envelope, not the queue's internal id.
        (void)_queue->enqueue(::morph::model::ActionTraits<QueuedOpportunityUpdate>::toJson(queued),
                              *queued.operationKey);
        // This client's *own* next edit of this opportunity chains onto this
        // one, not onto whatever the server last reported.
        _localVersion[*opportunityId] = base + 1;
        return queued;
    }

    /// @brief This client's current local view of @p opportunityId's version.
    /// @param opportunityId The opportunity to ask about.
    /// @return The version the next queued update would be stamped with.
    [[nodiscard]] std::int32_t localVersion(OpportunityId opportunityId) const {
        const auto found = _localVersion.find(*opportunityId);
        return found == _localVersion.end() ? 0 : found->second;
    }

private:
    /// @brief Mints a fresh dedup token for one logical field update.
    ///
    /// A random 128-bit id — the same reasoning as
    /// `lims::offline::FieldOutbox::mintOperationKey`: a counter would reset
    /// to zero across a device restart and collide with an earlier, genuinely
    /// different edit, silently *skipping* it at replay's at-most-once check.
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
    std::map<std::int64_t, std::int32_t> _localVersion;
};

}  // namespace crm::offline

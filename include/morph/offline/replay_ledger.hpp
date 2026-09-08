// SPDX-License-Identifier: Apache-2.0

#pragma once
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace morph::offline {

/// @brief Interface for an op-id-keyed exactly-once replay ledger.
///
/// Answers one question — "has this operation id already been applied?" —
/// for a host that must dedup a retried write against one it already
/// committed. Promoted from seven hand-written, near-identical copies of the
/// same table across five example rungs (morph#226): `kanban`/`ledger` store a
/// result and replay it verbatim on a hit ("response-replay"); `lims`/
/// `bookmarks`/`ledger`'s import path store nothing and report only that the
/// op was seen ("skip-only"). Both are the same mechanism with a different
/// caller-side interpretation of the stored payload — see `lookup()`'s own
/// doc comment — so there is exactly one interface here, not two.
///
/// @par Why the framework cannot supply the storage
/// Every existing occurrence stores its ledger row in the *same database and
/// the same transaction* as the write it guards, so the check-then-set commits
/// atomically with the operation's effect — see `record()`'s doc comment.
/// `include/morph` has no dependency on any SQL library, so it cannot open
/// that connection or join that transaction. What it can supply is the
/// *contract*: the shape every implementation must have, and a conformance
/// suite (`tests/replay_ledger_conformance.hpp`) any implementation can be
/// run against. The table itself stays app-side, per rung, exactly as before.
///
/// @par No base class for consumers
/// A model holds (or is handed) an `IReplayLedger&`; it never derives from
/// one. Every existing occurrence is a free function or a plain member on the
/// model, not a base class — a design that required inheriting from this
/// interface would be un-adoptable by every one of them.
// NOLINTBEGIN(cppcoreguidelines-special-member-functions)
struct IReplayLedger {
    virtual ~IReplayLedger() = default;

    /// @brief Reports whether @p opId within @p scope has already been decided.
    ///
    /// @par What a hit means is the caller's decision, not this interface's
    /// The returned string is whatever `record()` stored alongside the id. A
    /// response-replay consumer (kanban's `MoveTaskPosition`, ledger's
    /// `StoreTransaction`) stores its result's serialised form and, on a hit,
    /// returns that string verbatim instead of re-running the operation. A
    /// skip-only consumer (lims's `QueuedCapture`, bookmarks'/ledger's import
    /// path) stores an empty payload and, on a hit, reports "already applied,
    /// nothing to return" without ever looking at the string. Both read
    /// exactly the same `lookup()` — the value type reports the fact, and the
    /// layer that calls this decides what a hit means.
    ///
    /// @par `scope`
    /// An opaque caller-chosen partition — the three key shapes the seven
    /// occurrences used (`(entity_id, op_id)`, `(owner_principal, op_id)`, a
    /// bare `op_id`) are all this one string plus the id, with the bare-key
    /// case simply passing an empty `scope`. Two calls with the same `opId`
    /// but different `scope` never collide.
    ///
    /// @par Empty `opId`
    /// Never reported as decided, in every conforming implementation — see
    /// `record()`. An empty id is not an identity, and reporting one "already
    /// applied" would make every unkeyed call after the first a silent no-op.
    ///
    /// @param scope Caller-chosen partition; empty is a valid, distinct scope.
    /// @param opId  The operation id to look up. Empty always reports
    ///              `std::nullopt`, regardless of what has been recorded.
    /// @return The payload recorded with @p opId (possibly empty, for a
    ///         skip-only caller), or `std::nullopt` if it has not been
    ///         recorded — or @p opId is empty.
    // NOLINTNEXTLINE(bugprone-easily-swappable-parameters) -- scope/opId, not interchangeable
    [[nodiscard]] std::optional<std::string> lookup(std::string_view scope, std::string_view opId) const {
        if (opId.empty()) {
            return std::nullopt;
        }
        return doLookup(scope, opId);
    }

    /// @brief Records that @p opId within @p scope has been decided, with
    ///        @p payload attached.
    ///
    /// @par Atomicity is the caller's responsibility, not this interface's
    /// The framework cannot enforce this — `IReplayLedger` has no database
    /// connection of its own — so it is stated here instead: a conforming
    /// implementation MUST let its caller commit this call in the same
    /// transaction as the write it guards. An implementation backed by a SQL
    /// table does this by operating through the caller's own already-open
    /// connection/transaction handle (passed to the implementation at
    /// construction), never by opening a connection of its own. Recording
    /// outside that transaction reintroduces exactly the defect this ledger
    /// exists to prevent: a crash between the write and the record redelivers
    /// the operation and it is re-applied (morph#458 was this defect, shipped
    /// in two rungs, before this interface existed).
    ///
    /// @par Idempotent: first-write-wins
    /// Recording an @p opId within @p scope that is already decided is a
    /// no-op — the existing payload is kept, `payload` here is discarded, and
    /// no error is raised. This is the same contract
    /// `morph::journal::IActionLog::append()`'s idempotency-key dedup already
    /// ships (`journal/action_log.hpp`'s own doc comment: "if an entry with
    /// the same key was already recorded, treat the call as a no-op"): a
    /// redelivered retry that reaches `record()` a second time (e.g. because
    /// a reply was lost after the first commit) must not overwrite a payload
    /// a caller may already have replayed to someone else.
    ///
    /// @par Empty `opId`
    /// A no-op — there is no identity to record against, so `lookup()` for an
    /// empty id must keep reporting `std::nullopt` regardless of how many
    /// times `record()` is called with one.
    ///
    /// @param scope   Caller-chosen partition; must match the `scope` a later
    ///                `lookup()` uses to find this entry.
    /// @param opId    The operation id being recorded. A no-op if empty.
    /// @param payload Opaque payload to associate with @p opId; empty for a
    ///                skip-only caller that only cares whether a hit occurred.
    // NOLINTNEXTLINE(bugprone-easily-swappable-parameters) -- scope/opId, not interchangeable
    void record(std::string_view scope, std::string_view opId, std::string payload) {
        if (opId.empty()) {
            return;
        }
        doRecord(scope, opId, std::move(payload));
    }

protected:
    /// @brief Storage hook for `lookup()`. @p opId is never empty here — the
    ///        public wrapper already handled that case.
    // NOLINTNEXTLINE(bugprone-easily-swappable-parameters) -- scope/opId, not interchangeable
    [[nodiscard]] virtual std::optional<std::string> doLookup(std::string_view scope, std::string_view opId) const = 0;

    /// @brief Storage hook for `record()`. @p opId is never empty here — the
    ///        public wrapper already handled that case. Must be a no-op if
    ///        @p scope/@p opId is already recorded (first-write-wins).
    // NOLINTNEXTLINE(bugprone-easily-swappable-parameters) -- scope/opId, not interchangeable
    virtual void doRecord(std::string_view scope, std::string_view opId, std::string payload) = 0;
};
// NOLINTEND(cppcoreguidelines-special-member-functions)

// ── In-memory implementation ──────────────────────────────────────────────────

/// @brief Thread-safe in-memory implementation of `IReplayLedger`.
///
/// Suitable for testing and for a host with no durability requirement.
/// Mirrors `morph::offline::InMemoryOfflineQueue`'s shape. Not durable across
/// a process restart, and — since it is not backed by any real transaction —
/// not a stand-in for `record()`'s atomicity requirement: a real
/// implementation must be backed by the same store and transaction as the
/// write it guards.
class InMemoryReplayLedger : public IReplayLedger {
protected:
    // NOLINTNEXTLINE(bugprone-easily-swappable-parameters) -- scope/opId, not interchangeable
    [[nodiscard]] std::optional<std::string> doLookup(std::string_view scope, std::string_view opId) const override {
        std::scoped_lock const lock{_mtx};
        auto iter = _entries.find(Key{std::string{scope}, std::string{opId}});
        if (iter == _entries.end()) {
            return std::nullopt;
        }
        return iter->second;
    }

    // NOLINTNEXTLINE(bugprone-easily-swappable-parameters) -- scope/opId, not interchangeable
    void doRecord(std::string_view scope, std::string_view opId, std::string payload) override {
        std::scoped_lock const lock{_mtx};
        // emplace (not insert_or_assign): first-write-wins, per record()'s
        // own doc comment -- a second record() for the same (scope, opId)
        // must not overwrite a payload a caller may already have replayed.
        _entries.emplace(Key{std::string{scope}, std::string{opId}}, std::move(payload));
    }

private:
    using Key = std::pair<std::string, std::string>;  // (scope, opId)

    mutable std::mutex _mtx;
    std::map<Key, std::string> _entries;
};

}  // namespace morph::offline

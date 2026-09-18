// SPDX-License-Identifier: Apache-2.0

#pragma once
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "../model.hpp"

namespace morph::backend::detail {

/// @brief How far a freshly created shared instance has got through its first action.
///
/// A shared instance is created *before* the action that hydrates it runs, so
/// there is a window in which the directory holds an instance nobody yet knows
/// to be usable. `docs/spec/core/shared_instances.md`'s Failure modes section
/// says what must happen when that first action fails: the instance must not be
/// handed to a *new* attacher, and is evicted from the directory the next time
/// anyone attaches to its key.
enum class Hydration : std::uint8_t {
    /// @brief The first action has not settled yet.
    pending,
    /// @brief The first action succeeded; the instance is shareable.
    healthy,
    /// @brief The first action failed; the instance must not be handed out again.
    poisoned,
};

/// @brief The hydration phase of one shared instance, as a single atomic cell.
///
/// One cell rather than a `firstActionPending`/`poisoned` pair, because the pair
/// cannot be moved between its states atomically: a reader landing between
/// "first action is no longer pending" and "…and it failed" observes a
/// not-poisoned instance whose first action has already failed, and is handed
/// it — the exact case the spec forbids (morph#523).
///
/// Owned through a `shared_ptr` and captured that way — never via a raw pointer
/// to the owning backend — into the strand task that settles it: that task may
/// still be running after the backend itself is destroyed. See
/// `LocalBackend::execute`'s capture-by-`shared_ptr` rationale.
struct HydrationState {
    /// @brief Current phase. Only ever moves away from `pending`, exactly once.
    std::atomic<Hydration> phase{Hydration::pending};

    /// @brief Records the first action's outcome, if it has not been recorded already.
    ///
    /// A single compare-exchange, so the transition out of `pending` is
    /// indivisible: a concurrent `poisoned()` sees either the whole transition
    /// or none of it. Later actions' outcomes are ignored — only the *first*
    /// action decides hydration.
    /// @param succeeded `true` if the first action succeeded.
    void settle(bool succeeded) noexcept {
        Hydration expected = Hydration::pending;
        phase.compare_exchange_strong(expected, succeeded ? Hydration::healthy : Hydration::poisoned);
    }

    /// @brief Whether the first action has settled as a failure.
    /// @return `true` only in the `poisoned` phase; `false` while still `pending`.
    [[nodiscard]] bool poisoned() const noexcept { return phase.load() == Hydration::poisoned; }
};

/// @brief A shared instance's directory key: `(typeId, primary)`.
using DirectoryKey = std::pair<std::string, std::string>;

/// @brief Hash functor for `DirectoryKey`.
///
/// Structurally identical to `registry.hpp`'s `::morph::model::detail::PairKeyHash`,
/// but defined here rather than reused so this header depends on nothing beyond
/// `model.hpp`: `registry.hpp` pulls in glaze and the forms/schema stack, and the
/// instance directory is part of the async core that must stay usable without it
/// (morph#521).
struct DirectoryKeyHash {
    /// @brief Combines the hashes of the type id and the primary key.
    /// @param key The directory key to hash.
    /// @return The combined hash value.
    [[nodiscard]] std::size_t operator()(const DirectoryKey& key) const noexcept {
        std::size_t seed = std::hash<std::string>{}(key.first);
        // NOLINTNEXTLINE(readability-magic-numbers, cppcoreguidelines-avoid-magic-numbers)
        seed ^= std::hash<std::string>{}(key.second) + 0x9e3779b9 + (seed << 6) + (seed >> 2);
        return seed;
    }
};

/// @brief Everything a backend knows about one live model instance.
///
/// One record per instance, rather than one entry per instance in each of
/// several `ModelId`-keyed maps that had to be kept in lockstep by convention
/// (morph#523). Every field is reached through a single hash lookup, and a
/// half-updated instance is not representable.
struct Instance {
    /// @brief The model itself. Never null for a record that is in the directory.
    std::shared_ptr<::morph::model::detail::IModelHolder> holder;

    /// @brief Recorded owner principal, for `IAuthorizer::authorizeInstance`.
    ///
    /// Empty for an ownerless instance — which is every shared instance by
    /// design (see `RemoteServer::acquireSharedInstance`) and every instance in
    /// a `LocalBackend`, which has no authorizer.
    std::string owner;

    /// @brief Number of outstanding attachments, or `0` for a private instance.
    ///
    /// A private instance is not reference-counted at all: it has no directory
    /// key, cannot be attached to, and is destroyed by the first `release()`.
    /// A shared instance starts at `1` and is destroyed when it returns to `0`.
    std::size_t attachCount = 0;

    /// @brief The key this instance is filed under, or empty if it is private.
    std::optional<DirectoryKey> sharedKey;

    /// @brief First-action tracking, or null if this instance has none.
    ///
    /// Only an instance *created* by a shared register-or-attach carries one. A
    /// private instance has nothing to hydrate, and an instance promoted into
    /// the directory by `promote()` has already run the action that produced its
    /// key, so neither can be poisoned.
    ///
    /// It therefore doubles as the permanent mark of "this instance was created
    /// for a directory key", which outlives `sharedKey` — `attach()` clears that
    /// on eviction, `hydration` survives — and is what makes an evicted instance
    /// ineligible for `promote()`. See `promote()` for why that matters.
    std::shared_ptr<HydrationState> hydration;
};

/// @brief The live model instances of one backend, plus the shared-instance directory.
///
/// Replaces the six (`LocalBackend`) and eight (`RemoteServer`) parallel
/// `ModelId`-keyed containers those classes used to carry, and the
/// register-or-attach logic that was written out twice (morph#523). The
/// invariants it maintains are stated in `docs/spec/core/shared_instances.md`;
/// this class is where they are now enforced rather than asserted in comments.
///
/// @par Locking
/// **Caller-locked, deliberately.** Every operation below assumes the owning
/// backend already holds the mutex that guards it. That mutex covers more than
/// this class — `RemoteServer` mutates connection scopes and checks
/// `LimitPolicy::maxLiveModels` in the same critical section — so a lock of its
/// own would either be redundant or, worse, let those neighbouring decisions
/// straddle a directory change. The one piece of state that *is* internally
/// synchronised is `HydrationState`, because it is settled from a model strand
/// that holds no backend lock at all.
class InstanceDirectory {
public:
    /// @brief Outcome of `release()`.
    enum class Release : std::uint8_t {
        /// @brief Another attachment remains; the instance is still live.
        retained,
        /// @brief The last reference went away and the instance was destroyed.
        destroyed,
    };

    /// @brief Looks up one instance, read-only.
    ///
    /// Read-only because every field a backend reads — holder, owner, hydration
    /// state — is set when the instance is filed and changed only by the
    /// mutators below. Nothing outside this class has business writing one.
    /// @param mid Instance id.
    /// @return Pointer to the record, or `nullptr` if @p mid is not live.
    [[nodiscard]] const Instance* find(::morph::exec::detail::ModelId mid) const {
        auto iter = _instances.find(mid);
        return iter == _instances.end() ? nullptr : &iter->second;
    }

    /// @brief Number of live instances, shared and private alike.
    /// @return The instance count, which is what `LimitPolicy::maxLiveModels` bounds.
    [[nodiscard]] std::size_t size() const noexcept { return _instances.size(); }

    /// @brief Records a private instance: no key, no sharing, no hydration tracking.
    /// @param mid    Freshly allocated id.
    /// @param holder The constructed model.
    /// @param owner  Owner principal for `authorizeInstance`; empty if none.
    void insertPrivate(::morph::exec::detail::ModelId mid,
                       std::shared_ptr<::morph::model::detail::IModelHolder> holder, std::string owner = {}) {
        _instances.insert_or_assign(mid, Instance{.holder = std::move(holder), .owner = std::move(owner)});
    }

    /// @brief Records a freshly created shared instance and files it under @p key.
    ///
    /// The instance starts with one attachment and with hydration `pending`, so
    /// the outcome of its first action decides whether it may be handed to a
    /// second attacher.
    /// @param mid    Freshly allocated id.
    /// @param holder The constructed model.
    /// @param key    Directory key to file it under; must not already be taken.
    /// @param owner  Owner principal for `authorizeInstance`; empty if none.
    void insertShared(::morph::exec::detail::ModelId mid, std::shared_ptr<::morph::model::detail::IModelHolder> holder,
                      DirectoryKey key, std::string owner = {}) {
        indexKey(key, mid);
        _instances.insert_or_assign(mid, Instance{.holder = std::move(holder),
                                                  .owner = std::move(owner),
                                                  .attachCount = 1,
                                                  .sharedKey = std::move(key),
                                                  .hydration = std::make_shared<HydrationState>()});
    }

    /// @brief Takes one more attachment to the instance filed under @p key, if any.
    ///
    /// A poisoned instance reports as a *miss*: it is evicted from the directory
    /// here — lazily, at the next attach, exactly as the spec's Failure modes
    /// section specifies — and the caller goes on to create a fresh instance.
    /// The evicted instance itself is untouched and still live; whoever created
    /// it releases it normally.
    /// @param key Directory key being acquired.
    /// @return The attached instance's id, or `std::nullopt` on a miss or an eviction.
    [[nodiscard]] std::optional<::morph::exec::detail::ModelId> attach(const DirectoryKey& key) {
        auto keyIter = _byKey.find(key);
        if (keyIter == _byKey.end()) {
            return std::nullopt;
        }
        auto const mid = keyIter->second;
        Instance& inst = _instances.at(mid);
        if (inst.hydration && inst.hydration->poisoned()) {
            unindexKey(key);
            inst.sharedKey.reset();
            return std::nullopt;
        }
        inst.attachCount += 1;
        return mid;
    }

    /// @brief Files an already-live, still-anonymous instance under @p key.
    ///
    /// The promotion half of keyed instances: an action that creates its own
    /// entity runs on an instance that has no key yet, and the key only exists
    /// once the result comes back. The existing holder of a key always wins, and
    /// an instance that already holds a real key never changes it — both are
    /// silent no-ops, because instances never mutate their own identity.
    ///
    /// Only an instance that has **never** held a key can be promoted, and that
    /// is a stronger test than "has no key right now". `attach()` clears the
    /// `sharedKey` of an instance it evicts as poisoned, so an evicted instance
    /// looks anonymous from the directory's side — but it was created *for* its
    /// original key and told so once, permanently: `RemoteServer` builds its
    /// holder through `ModelRegistryFactory::create(typeId, primary)`, which
    /// calls `IModelHolder::attachIdentity(primary)`, and attaches the action
    /// log under that same key. Nothing updates either afterwards, so re-filing
    /// such an instance under a second key would hand every later attacher a
    /// model that still identifies itself as the first one — the re-keying
    /// `docs/spec/core/shared_instances.md` ("Re-pointing, not re-keying")
    /// forbids outright. A non-null `hydration` is exactly the mark of
    /// "created for a directory key", so it is what disqualifies the record.
    ///
    /// Its own Failure modes section already says what happens instead: the
    /// handler that hit the failed first action "does not self-heal… it keeps
    /// its broken instance until it releases and re-attaches from scratch".
    /// The promotion is a silent no-op like the other three, the instance stays
    /// anonymous and unshareable, and the key the host asked for is left free
    /// for the next attacher to create a healthy instance under.
    ///
    /// A promoted instance therefore never carries a `HydrationState`: the
    /// action that produced its key has already succeeded, so there is nothing
    /// left to poison.
    /// @param mid Live instance to promote.
    /// @param key Directory key to file it under.
    /// @return `true` if the promotion happened.
    bool promote(::morph::exec::detail::ModelId mid, DirectoryKey key) {
        auto instIter = _instances.find(mid);
        if (instIter == _instances.end() || _byKey.contains(key)) {
            return false;
        }
        Instance& inst = instIter->second;
        if (inst.sharedKey.has_value() || inst.hydration) {
            return false;
        }
        indexKey(key, mid);
        inst.sharedKey = std::move(key);
        // The instance was private until now — `insertPrivate` is the only way
        // in for a record with no `hydration`, and it files one with no
        // attachments at all. The directory now references it, so it gets the
        // one attachment that reference stands for.
        inst.attachCount = 1;
        return true;
    }

    /// @brief Releases one attachment, destroying the instance at zero.
    ///
    /// A private instance (`attachCount == 0`) is destroyed outright. A shared
    /// instance is unfiled from the directory in the same step that destroys it,
    /// so directory membership can never outlive the instance it names.
    /// @param mid Instance to release.
    /// @return Whether this call destroyed the instance.
    Release release(::morph::exec::detail::ModelId mid) {
        auto instIter = _instances.find(mid);
        if (instIter == _instances.end()) {
            return Release::retained;
        }
        Instance& inst = instIter->second;
        if (inst.attachCount > 0) {
            inst.attachCount -= 1;
            if (inst.attachCount > 0) {
                return Release::retained;
            }
            if (inst.sharedKey) {
                unindexKey(*inst.sharedKey);
            }
        }
        _instances.erase(instIter);
        return Release::destroyed;
    }

    /// @brief Primary keys of the live shared instances of @p typeId.
    ///
    /// Served from a per-type index rather than by scanning the whole directory,
    /// so enumerating one model type does not cost the size of every other type
    /// put together.
    /// @param typeId String type-id to enumerate.
    /// @return The canonical key strings, in unspecified order.
    [[nodiscard]] std::vector<std::string> keysOfType(const std::string& typeId) const {
        auto typeIter = _byType.find(typeId);
        if (typeIter == _byType.end()) {
            return {};
        }
        return {typeIter->second.begin(), typeIter->second.end()};
    }

private:
    /// @brief Adds @p key to both directory indexes. The single place either is grown.
    /// @param key Directory key being filed.
    /// @param mid Instance it names.
    void indexKey(const DirectoryKey& key, ::morph::exec::detail::ModelId mid) {
        _byKey.emplace(key, mid);
        _byType[key.first].insert(key.second);
    }

    /// @brief Removes @p key from both directory indexes. The single place either shrinks.
    /// @param key Directory key being unfiled.
    void unindexKey(const DirectoryKey& key) {
        _byKey.erase(key);
        // A guarded `find`, not `at`: the two indexes are only ever grown and
        // shrunk together, here and in `indexKey`, so a missing type bucket is a
        // broken invariant -- but this runs inside `release()`, which
        // `RemoteServer::closeConnection` calls, which transports in turn call
        // from a (noexcept) scope-guard destructor (`net/socket_server.hpp`). A
        // `std::out_of_range` escaping there aborts the process rather than
        // reporting anything, so the invariant is left to the tests to police.
        if (auto typeIter = _byType.find(key.first); typeIter != _byType.end()) {
            typeIter->second.erase(key.second);
            if (typeIter->second.empty()) {
                _byType.erase(typeIter);
            }
        }
    }

    /// @brief Every live instance, private and shared alike. The directory's only owner of state.
    std::unordered_map<::morph::exec::detail::ModelId, Instance, ::morph::exec::detail::ModelIdHash> _instances;

    /// @brief `(typeId, primary)` -> instance. An index over `_instances`, never an owner.
    std::unordered_map<DirectoryKey, ::morph::exec::detail::ModelId, DirectoryKeyHash> _byKey;

    /// @brief `typeId` -> that type's live primaries. An index over `_byKey`, for `keysOfType`.
    std::unordered_map<std::string, std::unordered_set<std::string>> _byType;
};

}  // namespace morph::backend::detail

// SPDX-License-Identifier: Apache-2.0

#pragma once
#include <any>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <typeindex>
#include <utility>
#include <vector>

#include "../strand.hpp"

/// @file
/// @brief Per-`Bridge` instance-subscription bookkeeping, extracted out of `Bridge`.
///
/// `Bridge`'s subscription set (`InstanceSubscription` before this extraction,
/// `_subMtx`/`_subscriptions`/`_subscriptionCount`, and
/// `addSubscription`/`removeSubscription`/`hasSubscribers`/`publishResult` --
/// all `private`) is a standalone match-and-prune data structure that was
/// trapped inside `Bridge` alongside five other, unrelated mutex-guarded
/// concerns. Its contract -- register at most one callback per (binding,
/// result type); match at publish time on the binding's *current* instance,
/// not a fixed one, so a re-pointed handler's subscriptions follow it; prune
/// entries whose binding has been destroyed without an explicit unsubscribe
/// -- touches no backend, no session, no handler registry, and is expressible
/// without `Bridge` at all.
///
/// This is a faithful, behavior-preserving port of the logic that used to
/// live directly on `Bridge`: same locking, same fields, same semantics.
/// Nothing here is new logic. The one structural change from a plain
/// extraction is that this class is templated on the binding type
/// (`Bridge` instantiates it with `morph::bridge::detail::HandlerBinding`,
/// which is itself defined inside `bridge.hpp`) rather than naming
/// `HandlerBinding` directly -- unlike `ExecuteOrderGate`'s `ModelId`, which
/// already lived in its own header, `HandlerBinding` is defined inline in
/// `bridge.hpp` with no header of its own, and a non-template class here
/// would have to either forward-declare it (making this header silently
/// depend on include order to compile) or pull in a `HandlerBinding` header
/// that does not exist. Templating avoids both: the class body only touches
/// `Binding`'s members (`currentId`) inside function templates, so nothing
/// about `Binding`'s shape needs to be known until `Bridge` instantiates this
/// with the real `HandlerBinding`, by which point it is a complete type. It
/// also means this header can be unit-tested against a lightweight
/// stand-in binding with no dependency on `bridge.hpp` at all, though the
/// test suite happens to use the real `HandlerBinding` since it is already
/// available wherever `Bridge` is being tested.
namespace morph::bridge::detail {

/// @brief Tracks, per `Binding`, at most one callback per result type, and
///        delivers matching results to every subscriber attached to the
///        instance a result was produced on.
///
/// @tparam Binding The handler-binding type subscriptions are held against.
///                 Must expose `std::atomic<std::uint64_t> currentId` (read
///                 via `.load()`) -- the instance id `publishResult` matches
///                 on. `Bridge` instantiates this with
///                 `morph::bridge::detail::HandlerBinding`.
template <typename Binding>
class SubscriptionRegistry {
public:
    /// @brief Registers a result-type subscription for @p binding.
    ///
    /// The subscription is stored against the *binding*, not against a fixed
    /// instance id, and is matched at publish time by comparing the binding's
    /// current instance. Re-pointing a handler therefore moves its
    /// subscriptions with it, which is what makes "tell me about the account
    /// I am looking at" keep working when the user switches accounts.
    ///
    /// @param binding Handler binding that owns the subscription.
    /// @param type    Result type being subscribed to.
    /// @param sink    Type-erased delivery callback; receives the boxed result.
    /// @param exec    Executor the callback is delivered on.
    void addSubscription(const std::shared_ptr<Binding>& binding, std::type_index type,
                         std::function<void(const std::any&)> sink, ::morph::exec::IExecutor* exec) {
        std::scoped_lock const lock{_mtx};
        for (auto& entry : _subscriptions) {
            auto owner = entry.binding.lock();
            if (owner && owner.get() == binding.get() && entry.type == type) {
                entry.sink = std::move(sink);  // one callback per (handler, result type)
                entry.exec = exec;
                return;
            }
        }
        _subscriptions.push_back({.binding = binding, .type = type, .sink = std::move(sink), .exec = exec});
        _count.store(_subscriptions.size(), std::memory_order_relaxed);
    }

    /// @brief Removes @p binding's subscription for @p type, if any.
    /// @param binding Handler binding that owns the subscription.
    /// @param type    Result type to stop hearing about.
    void removeSubscription(const std::shared_ptr<Binding>& binding, std::type_index type) {
        std::scoped_lock const lock{_mtx};
        std::erase_if(_subscriptions, [&](const Entry& entry) {
            auto owner = entry.binding.lock();
            return !owner || (owner.get() == binding.get() && entry.type == type);
        });
        _count.store(_subscriptions.size(), std::memory_order_relaxed);
    }

    /// @brief Whether any subscription is currently registered.
    ///
    /// A single relaxed atomic load, so the overwhelmingly common case -- a
    /// process with no subscribers at all -- pays nothing per result. Without
    /// this, every successful action would build a `std::type_index`, copy its
    /// result into a `std::any`, take the lock and walk the (empty)
    /// subscription list before its `Completion` could resolve: a throughput
    /// regression for every existing caller, on the hot path, to serve a
    /// feature they are not using.
    /// @return `true` if at least one subscription exists.
    [[nodiscard]] bool hasSubscribers() const noexcept { return _count.load(std::memory_order_relaxed) != 0U; }

    /// @brief Delivers @p value to every subscriber attached to instance @p mid.
    ///
    /// Called for every successful action result. Subscribers are matched on
    /// *the instance the result was produced on*, so a handler hears about
    /// work another handler -- or, with a shared instance, another screen
    /// entirely -- did on the model it is attached to.
    ///
    /// The producing handler is notified too: suppressing the echo would force
    /// every subscriber to special-case "was this mine", which is exactly the
    /// bookkeeping the feature exists to remove.
    ///
    /// Sinks are snapshotted under the lock and invoked outside it, so a
    /// subscriber that re-enters the registry (or the `Bridge` it backs)
    /// cannot deadlock.
    ///
    /// @param mid   Instance the result was produced on.
    /// @param type  Result type produced.
    /// @param value Boxed result.
    void publishResult(::morph::exec::detail::ModelId mid, std::type_index type, const std::any& value) {
        std::vector<std::pair<std::function<void(const std::any&)>, ::morph::exec::IExecutor*>> targets;
        {
            std::scoped_lock const lock{_mtx};
            // Prune while we are already holding the lock and walking the
            // list: a handler that is destroyed without unsubscribing would
            // otherwise leave its entry behind until some *other* handler
            // happened to call add/removeSubscription, which in a long-lived
            // app with many transient handlers is never.
            std::erase_if(_subscriptions, [](const Entry& entry) { return entry.binding.expired(); });
            _count.store(_subscriptions.size(), std::memory_order_relaxed);
            for (const auto& entry : _subscriptions) {
                auto owner = entry.binding.lock();
                if (owner && entry.type == type && owner->currentId.load() == mid.v && entry.sink) {
                    targets.emplace_back(entry.sink, entry.exec);
                }
            }
        }
        for (auto& [sink, exec] : targets) {
            if (exec != nullptr) {
                exec->post([sink, value] { sink(value); });
            } else {
                sink(value);
            }
        }
    }

    /// @brief Number of subscription entries currently stored, including any
    ///        not yet pruned.
    ///
    /// Test-only observability, mirroring `ExecuteOrderGate::gateCount()` --
    /// stale the moment the lock is released, so it must not drive a
    /// check-then-act decision. In particular, an entry whose binding has
    /// already been destroyed still counts here until the next
    /// `publishResult` prunes it: this is what lets a test observe the prune
    /// step directly, by checking `size()` before and after a `publishResult`
    /// call that follows the binding's destruction.
    /// @return Current entry count.
    [[nodiscard]] std::size_t size() const {
        std::scoped_lock const lock{_mtx};
        return _subscriptions.size();
    }

private:
    struct Entry {
        std::weak_ptr<Binding> binding;
        std::type_index type;
        std::function<void(const std::any&)> sink;
        ::morph::exec::IExecutor* exec = nullptr;
    };

    mutable std::mutex _mtx;
    std::vector<Entry> _subscriptions;
    // Mirrors _subscriptions.size() for the lock-free hasSubscribers() probe.
    // Maintained under _mtx; read relaxed off it. A stale-by-one read is
    // harmless: publishResult re-checks under the lock and finds nothing.
    std::atomic<std::size_t> _count{0};
};

}  // namespace morph::bridge::detail

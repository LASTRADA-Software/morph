// SPDX-License-Identifier: Apache-2.0

#pragma once
#include <any>
#include <atomic>
#include <cstddef>
#include <functional>
#include <memory>
#include <typeindex>
#include <utility>
#include <vector>

#include "../executor.hpp"
#include "../strand.hpp"

/// @file
/// @brief A `Bridge`'s instance subscriptions: at most one callback per
///        (binding, result type), matched at publish time on the binding's
///        *current* instance, so a re-pointed handler's subscriptions follow
///        it; entries whose binding is gone are pruned.
///
/// Templated on the binding type so this header does not depend on
/// `bridge.hpp`, which defines `HandlerBinding` inline: the body touches the
/// binding's `currentId` only inside member functions, instantiated once the
/// type is complete.

namespace morph::bridge::detail {

/// @brief Tracks, per `Binding`, at most one callback per result type, and
///        delivers matching results to every subscriber attached to the
///        instance a result was produced on.
///
/// Belongs to its `Bridge`'s owner: every member but `hasSubscribers()` is
/// called there, so the list needs no lock. `hasSubscribers()` reads an atomic
/// count and may be asked from any thread — a backend's, deciding whether a
/// result needs to reach the owner at all.
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
        for (auto& entry : _subscriptions) {
            auto owner = entry.binding.lock();
            if (owner && owner.get() == binding.get() && entry.type == type) {
                entry.sink = std::move(sink);  // one callback per (handler, result type)
                entry.exec = exec;
                return;
            }
        }
        _subscriptions.emplace_back(binding, type, std::move(sink), exec);
        _count.store(_subscriptions.size(), std::memory_order_relaxed);
    }

    /// @brief Removes @p binding's subscription for @p type, if any.
    /// @param binding Handler binding that owns the subscription.
    /// @param type    Result type to stop hearing about.
    void removeSubscription(const std::shared_ptr<Binding>& binding, std::type_index type) {
        std::erase_if(_subscriptions, [&](const Entry& entry) {
            auto owner = entry.binding.lock();
            return !owner || (owner.get() == binding.get() && entry.type == type);
        });
        _count.store(_subscriptions.size(), std::memory_order_relaxed);
    }

    /// @brief Whether any subscription is currently registered.
    ///
    /// A single relaxed atomic load, so the common case -- a process with no
    /// subscribers at all -- pays nothing per result: without subscribers a
    /// result is never boxed and never sent to the owner.
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
    /// The matching sinks are copied out before any is invoked: a sink
    /// delivered through a null executor runs here, and may subscribe or
    /// unsubscribe, which would otherwise change the list under the loop.
    /// @param mid   Instance the result was produced on.
    /// @param type  Result type produced.
    /// @param value Boxed result.
    void publishResult(::morph::exec::detail::ModelId mid, std::type_index type, const std::any& value) {
        std::erase_if(_subscriptions, [](const Entry& entry) { return entry.binding.expired(); });
        _count.store(_subscriptions.size(), std::memory_order_relaxed);
        std::vector<std::pair<std::function<void(const std::any&)>, ::morph::exec::IExecutor*>> targets;
        for (const auto& entry : _subscriptions) {
            auto owner = entry.binding.lock();
            if (owner && entry.type == type && owner->currentId.load() == mid.v && entry.sink) {
                targets.emplace_back(entry.sink, entry.exec);
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

    /// @brief Number of registered subscriptions, dead ones not yet pruned included.
    /// @return Size of the subscription list.
    [[nodiscard]] std::size_t size() const { return _subscriptions.size(); }

private:
    struct Entry {
        Entry(std::weak_ptr<Binding> bindingIn, std::type_index typeIn, std::function<void(const std::any&)> sinkIn,
              ::morph::exec::IExecutor* execIn)
            : binding{std::move(bindingIn)}, type{typeIn}, sink{std::move(sinkIn)}, exec{execIn} {}

        std::weak_ptr<Binding> binding;
        std::type_index type;
        std::function<void(const std::any&)> sink;
        ::morph::exec::IExecutor* exec = nullptr;
    };

    std::vector<Entry> _subscriptions;
    std::atomic<std::size_t> _count{0};
};

}  // namespace morph::bridge::detail

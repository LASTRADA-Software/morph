// SPDX-License-Identifier: Apache-2.0

#pragma once
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "../attributes.hpp"
#include "../core/bridge.hpp"
#include "../core/callback_scope.hpp"
#include "../core/completion.hpp"
#include "../core/registry.hpp"
#include "detail/graph.hpp"
#include "runtime.hpp"
#include "scheduler.hpp"
#include "signal.hpp"

/// @file
/// @brief Declarative control: `Query`, `Mutation` and `Subscription` turn server interaction
///        into reactive state, so a controller states what it depends on and what a write
///        invalidates instead of sequencing calls by hand.
///
/// Specified in `docs/spec/reactive/control.md`.

namespace morph::reactive {

/// @brief The one place an `exception_ptr` becomes display text.
/// @param error The captured exception, or null.
/// @return `what()` for a `std::exception`, `"unknown error"` for anything else, empty for null.
[[nodiscard]] inline std::string errorMessage(std::exception_ptr const& error) {
    if (error == nullptr) {
        return {};
    }
    try {
        std::rethrow_exception(error);
    } catch (std::exception const& exception) {
        return exception.what();
    } catch (...) {
        return "unknown error";
    }
}

/// @brief Something a `Mutation` can tell to fetch again after it succeeds.
class Refetchable {
public:
    Refetchable() = default;
    virtual ~Refetchable() = default;
    Refetchable(Refetchable const&) = delete;
    Refetchable& operator=(Refetchable const&) = delete;
    Refetchable(Refetchable&&) = delete;
    Refetchable& operator=(Refetchable&&) = delete;

    /// @brief Re-issues the current request; does nothing while idle.
    virtual void refetch() = 0;
};

/// @brief How a `Query` refreshes besides key changes, and how long it lets a key settle.
struct QueryOptions {
    /// @brief Runs the timed refresh and the debounce; required when either is positive. Borrowed: it must
    ///        outlive the query.
    Scheduler* scheduler = nullptr;
    /// @brief Re-fetch the current key this long after each call is issued, unless a call is then in flight;
    ///        zero means no timed refresh, and a negative value is refused.
    std::chrono::milliseconds refreshEvery{0};
    /// @brief Fetch a changed key only once it has been stable this long; zero fetches at once, and a negative
    ///        value is refused. The first key and a change to idle apply at once.
    std::chrono::milliseconds debounce{0};
};

/// @brief A derived async resource: the result of fetching whatever its tracked key names.
///
/// An internal Effect reads `key`. When the key changes — equality-gated when `==` is usable for `A`
/// (`kEqualityUsable`), otherwise on every re-run — the query fetches it under a fresh
/// generation of its `CallbackScope`, so a reply for a superseded key is dropped (**latest wins**) and a
/// call that carries a stop source (a bridge call whose handler returns a `Task`) is asked to stop.
/// Destroying the query does the same to the call in flight. `nullopt` means idle: nothing in flight,
/// value and error cleared. `value()` is kept while a refetch or a new key is in flight. A failure clears
/// it when the call was for a different key than the one `value()` was fetched for, so a view never shows
/// one key's result beside another key's failure; a failed refetch of the same key keeps it. Without `==` on
/// `A`, every re-run of the key counts as a different key.
///
/// With `QueryOptions::refreshEvery`, the query re-fetches its current key one period after each call is
/// issued, whatever issued it: a key change, `refetch()`, an invalidation, or the timer itself. A tick that
/// finds a call in flight issues nothing and waits another period, so a timed refresh never supersedes a call
/// and a fetch slower than the period still delivers. While idle no timer runs. A tick off the Runtime's
/// owner is reported (`detail::site::kOffOwner`) and refused, and the timer then stays stopped until the next
/// call is issued. Destroying the query cancels the timer, and a tick that arrives afterwards is dropped.
///
/// The fetcher runs untracked, whoever issues the call: a `refetch()` from inside an Effect does not
/// subscribe that Effect to what the fetcher reads. `refetch()` inside a Computed's computation is reported
/// (`detail::site::kIssueInComputed`) and refused before anything is issued.
///
/// With `QueryOptions::debounce`, a changed key is fetched only once it has stayed the same for that long: the
/// call for the key it replaces is superseded at once (its reply is dropped), `pending()` is true while the query
/// waits, and each further change restarts the wait. The first key, and a change to idle, apply at once; so does
/// `refetch()`, which also ends a wait. Destroying the query cancels the wait.
///
/// A fetcher that throws instead of returning a `Completion` fails the request: the exception becomes
/// `error()` and nothing is pending. A key that throws is an Effect that throws: reported
/// (`detail::site::kEffectThrew`), and the query keeps its state. A delivery off the Runtime's owner is
/// reported (`detail::site::kOffOwner`) and dropped.
///
/// The key or the fetcher may destroy the query (a remount), and so may another handler of the reply
/// or an Effect the reply wakes; the query is not touched afterwards. The action a fetcher receives
/// belongs to the query, so a fetcher that destroys the query must not use it afterwards.
/// @tparam A The action (request) type.
/// @tparam R The result type; defaults to the action's registered result.
// NOLINTNEXTLINE(readability-redundant-typename) -- valid everywhere; implicit typename here is unverified on MSVC
template <typename A, typename R = typename model::ActionTraits<A>::Result>
class Query final : public Refetchable {
public:
    /// @brief The tracked key: which request the query currently stands for, or `nullopt` for none.
    using Key = std::function<std::optional<A>()>;
    /// @brief Issues one request.
    using Fetch = std::function<async::Completion<R>(A const&)>;

    /// @brief Builds a query over any fetcher — the test seam, and the way to query a non-bridge source.
    /// @param runtime The runtime. Borrowed: it must outlive the query.
    /// @param fetch Issues a request; its `Completion` must deliver on the runtime's owner.
    /// @param key The tracked key.
    /// @param options Timed refresh and debounce; see QueryOptions.
    /// @throws std::invalid_argument when `options.refreshEvery` or `options.debounce` is negative, or either is
    ///         positive while `options.scheduler` is null; nothing has been fetched then.
    Query(Runtime& runtime MORPH_LIFETIMEBOUND, Fetch fetch, Key key, QueryOptions options = {})
        : _rt{&runtime},
          _fetch{std::make_shared<Fetch const>(std::move(fetch))},
          _pending{runtime, false},
          _value{runtime, std::nullopt},
          _error{runtime, nullptr},
          _refresh{options} {
        if (options.refreshEvery.count() < 0 || options.debounce.count() < 0) {
            throw std::invalid_argument{"Query: refreshEvery and debounce must not be negative"};
        }
        if ((options.refreshEvery.count() > 0 || options.debounce.count() > 0) && options.scheduler == nullptr) {
            throw std::invalid_argument{"Query: refreshEvery and debounce need a scheduler"};
        }
        // The Effect runs at once and issues through `_inflight`, which is declared after it so that it is
        // destroyed first. The key lives in the Effect's body, which the graph keeps alive for a run that
        // destroys the query.
        _effect = std::make_unique<Effect>(runtime, [this, key = std::move(key)] {
            async::CallbackToken const alive = _lifetime.token();
            std::optional<A> next = key();
            if (alive.expired()) {
                return;
            }
            _rt->untracked([&] { onKey(std::move(next)); });
        });
    }

    /// @brief Builds a query that fetches through a bridge handler.
    /// @tparam M The handler's model type.
    /// @tparam S The handler's sharing policy.
    /// @param runtime The runtime. Borrowed: it must outlive the query.
    /// @param handler Executes the requests; its GUI executor must be the runtime's owner. Borrowed.
    /// @param key The tracked key.
    /// @param options Timed refresh and debounce; see QueryOptions.
    /// @throws std::invalid_argument when `options.refreshEvery` or `options.debounce` is negative, or either is
    ///         positive while `options.scheduler` is null; nothing has been fetched then.
    template <typename M, typename S>
    Query(Runtime& runtime MORPH_LIFETIMEBOUND, bridge::BridgeHandler<M, S>& handler MORPH_LIFETIMEBOUND, Key key,
          QueryOptions options = {})
        : Query(runtime, [&handler](A const& action) { return handler.execute(action); }, std::move(key), options) {}

    ~Query() override = default;
    Query(Query const&) = delete;
    Query& operator=(Query const&) = delete;
    Query(Query&&) = delete;
    Query& operator=(Query&&) = delete;

    /// @brief Whether a request for the current key is in flight. Tracked.
    /// @return True from issue to delivery.
    [[nodiscard]] bool pending() const { return _pending.get(); }

    /// @brief The last successful result. Tracked.
    /// @return The result, kept while a refetch or a new key is in flight and through a failed refetch of
    ///         the same key; `nullopt` before the first, while idle, and after a failure for a different
    ///         key. Valid until the next delivery.
    [[nodiscard]] std::optional<R> const& value() const { return _value.get(); }

    /// @brief The last failure. Tracked.
    /// @return The exception, or null; cleared by the next success. After a key change, the previous key's
    ///         failure remains until the new key settles.
    [[nodiscard]] std::exception_ptr error() const { return _error.get(); }

    /// @brief Re-issues the current key under a new generation, ending a debounce wait; idle stays idle. Refused,
    ///        and reported,
    ///        off the Runtime's owner (`detail::site::kOffOwner`) and inside a Computed's computation
    ///        (`detail::site::kIssueInComputed`); nothing is issued then.
    void refetch() override {
        detail::RuntimeCore const& core = *_rt->core();
        if (!core.checkOwner()) {
            return;
        }
        // A computation is pure: one that fetched would fetch again on every recomputation, with the
        // pending flag and any synchronous failure it cannot write lost.
        if (core.isComputing()) {
            core.report(detail::site::kIssueInComputed);
            return;
        }
        if (_current.has_value()) {
            issue();
        }
    }

    /// @brief Re-fetches whenever the bridge publishes a @p Pub on @p handler's instance.
    ///
    /// Uses the handler's one subscription slot for `Pub`: two consumers of the same `Pub` use two
    /// handlers.
    /// @tparam Pub The published result type to listen for.
    /// @tparam M The handler's model type.
    /// @tparam S The handler's sharing policy.
    /// @param handler The handler whose instance publishes. Borrowed: it must outlive the query.
    template <typename Pub, typename M, typename S>
    void refreshOn(bridge::BridgeHandler<M, S>& handler) {
        handler.template subscribe<Pub>(_lifetime, [this](Pub const&) { refetch(); });
    }

private:
    void onKey(std::optional<A> next) {
        if constexpr (kEqualityUsable<A>) {
            if (_keyed && next == _current) {
                return;
            }
        }
        bool const first = !_keyed;
        _keyed = true;
        _current = std::move(next);
        ++_keys;
        if (_refresh.debounce.count() > 0 && !first && _current.has_value()) {
            // The call for the key being replaced is stale from now on; the new key waits until it has been
            // stable for the debounce interval. Replacing the handle cancels the previous wait.
            _inflight.reset();
            _pending.set(true);
            _debounce = _refresh.scheduler->after(_refresh.debounce, _lifetime.guard([this] { onDebounced(); }));
            return;
        }
        issue();
    }

    void onDebounced() {
        if (!_rt->core()->checkOwner()) {
            return;
        }
        issue();
    }

    void issue() {
        _debounce.cancel();
        _inflight.reset();
        if (!_current.has_value()) {
            _timer.cancel();
            _rt->batch([&] {
                _pending.set(false);
                _value.set(std::nullopt);
                _error.set(nullptr);
            });
            return;
        }
        // Armed before the fetcher runs: the period restarts even for a fetcher that throws, and a fetcher
        // may destroy the query.
        armRefresh();
        // Held here, so a fetcher that destroys the query keeps itself alive until it returns.
        std::shared_ptr<Fetch const> const fetch = _fetch;
        async::CallbackToken const alive = _lifetime.token();
        async::Completion<R> completion;
        try {
            // Untracked: a refetch() called from an Effect must not subscribe that Effect to what the
            // fetcher reads, or a change there would re-run it and issue again.
            completion = _rt->untracked([&] { return (*fetch)(*_current); });
        } catch (...) {
            if (alive.expired()) {
                return;
            }
            // The call it replaced was superseded above, so nothing is in flight any more.
            fail(std::current_exception());
            return;
        }
        if (alive.expired()) {
            return;
        }
        _pending.set(true);
        completion
            .then(_inflight,
                  [this](R const& result) {
                      if (!_rt->core()->checkOwner()) {
                          return;
                      }
                      _rt->batch([&] {
                          _value.set(result);
                          _valueKey = _keys;
                          _error.set(nullptr);
                          _pending.set(false);
                      });
                  })
            .onError(_inflight, [this](std::exception_ptr const& error) {
                if (!_rt->core()->checkOwner()) {
                    return;
                }
                fail(error);
            });
    }

    // Settles the current call with @p error. Every call in flight is for the current key, since a key change
    // supersedes, so a value fetched under an earlier key belongs to a key the query no longer stands for.
    void fail(std::exception_ptr error) {
        _rt->batch([&] {
            _error.set(std::move(error));
            if (_valueKey != _keys && _value.peek().has_value()) {
                _value.set(std::nullopt);
            }
            _pending.set(false);
        });
    }

    // (Re)starts the refresh period. The tick is gated by `_lifetime`, so one a scheduler delivers after the
    // query is gone is dropped.
    void armRefresh() {
        if (_refresh.refreshEvery.count() > 0) {
            _timer = _refresh.scheduler->after(_refresh.refreshEvery, _lifetime.guard([this] { onRefreshTick(); }));
        }
    }

    void onRefreshTick() {
        if (!_rt->core()->checkOwner() || !_current.has_value()) {
            return;
        }
        if (_pending.peek()) {
            armRefresh();
            return;
        }
        issue();
    }

    Runtime* _rt;
    // Shared, so a running issue() can keep the fetcher alive past the query.
    std::shared_ptr<Fetch const> _fetch;
    Signal<bool> _pending;
    Signal<std::optional<R>> _value;
    Signal<std::exception_ptr> _error;
    std::optional<A> _current;
    // How many keys the query has taken, and which of them `_value` was fetched for.
    std::uint64_t _keys = 0;
    std::uint64_t _valueKey = 0;
    bool _keyed = false;
    QueryOptions _refresh;
    std::unique_ptr<Effect> _effect;
    // Destroyed before the Effect and the signals a timed refresh or a debounced fetch writes, so neither
    // outlives them.
    TimerHandle _timer;
    TimerHandle _debounce;
    // Gates the subscription `refreshOn` installs. Its tokens expire with the query, which is how a run
    // that destroyed the query finds out.
    async::CallbackScope _lifetime;
    // Last member, so it is destroyed first: a reply still in flight finds it stopped.
    async::CallbackScope _inflight;
};

}  // namespace morph::reactive

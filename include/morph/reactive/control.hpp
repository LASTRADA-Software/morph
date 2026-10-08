// SPDX-License-Identifier: Apache-2.0

#pragma once
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <exception>
#include <functional>
#include <map>
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

/// @brief A query's registration with the mutations that invalidate it.
///
/// The query owns the link and clears it when it is destroyed; a mutation holds it shared
/// (`MutationOptions::invalidates`). So a mutation never refetches a query that is gone, and the two may be
/// destroyed in either order.
class InvalidationLink final {
public:
    /// @param target The query to refetch. Borrowed until `clear()`.
    explicit InvalidationLink(Refetchable& target MORPH_LIFETIMEBOUND) noexcept : _target{&target} {}

    /// @brief Refetches the query, or does nothing once it is gone.
    void refetch() const {
        if (_target != nullptr) {
            _target->refetch();
        }
    }

    /// @brief Unregisters the query; called by its destructor.
    void clear() noexcept { _target = nullptr; }

    /// @brief Whether the query is still registered.
    /// @return False once the query has been destroyed.
    [[nodiscard]] bool live() const noexcept { return _target != nullptr; }

private:
    Refetchable* _target;
};

namespace detail {

/// @brief Checks that @p handler delivers its callbacks on @p runtime's owner, reporting `site::kHandlerExecutor`
///        when it does not.
///
/// A control node writes its signals from the handler's callbacks, so they must run where the runtime runs. The
/// check is by affinity, not by pointer identity: a distinct serial executor that runs its tasks on the owner's
/// thread is accepted. An executor that is not serial is reported at once. Any other serial executor is checked
/// where it runs, by one posted task, which reports when it finds itself off the owner. No callback executor
/// means the bridge's owner.
/// @tparam M The handler's model type.
/// @tparam S The handler's sharing policy.
/// @param runtime The runtime the node belongs to.
/// @param handler The handler the node is built over.
template <typename M, typename S>
void checkHandlerExecutor(Runtime const& runtime, bridge::BridgeHandler<M, S> const& handler) {
    exec::IExecutor* const callbacks = handler.guiExecutor();
    exec::IExecutor& delivery = callbacks != nullptr ? *callbacks : handler.owner();
    std::shared_ptr<RuntimeCore> const& core = runtime.core();
    if (!delivery.isSerial()) {
        core->report(site::kHandlerExecutor);
        return;
    }
    if (&delivery == &core->owner()) {
        return;
    }
    delivery.post([weak = std::weak_ptr<RuntimeCore>{core}] {
        if (auto const live = weak.lock(); live != nullptr && !live->onOwner()) {
            live->report(site::kHandlerExecutor);
        }
    });
}

}  // namespace detail

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
          _link{std::make_shared<InvalidationLink>(*this)},
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
    ///
    /// Checks the handler's callback executor against the runtime's owner (`detail::checkHandlerExecutor`).
    /// @tparam M The handler's model type.
    /// @tparam S The handler's sharing policy.
    /// @param runtime The runtime. Borrowed: it must outlive the query.
    /// @param handler Executes the requests; its callbacks must be delivered on the runtime's owner. Borrowed.
    /// @param key The tracked key.
    /// @param options Timed refresh and debounce; see QueryOptions.
    /// @throws std::invalid_argument when `options.refreshEvery` or `options.debounce` is negative, or either is
    ///         positive while `options.scheduler` is null; nothing has been fetched then.
    template <typename M, typename S>
    Query(Runtime& runtime MORPH_LIFETIMEBOUND, bridge::BridgeHandler<M, S>& handler MORPH_LIFETIMEBOUND, Key key,
          QueryOptions options = {})
        : Query(runtime, [&handler](A const& action) { return handler.execute(action); }, std::move(key), options) {
        detail::checkHandlerExecutor(runtime, handler);
    }

    /// @brief Clears the query's invalidation link, so no mutation refetches it any more, and cancels what it
    ///        has in flight.
    ~Query() override { _link->clear(); }
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

    /// @brief This query's link, for `MutationOptions::invalidates`.
    /// @return The link; it refetches nothing once this query is destroyed.
    [[nodiscard]] std::shared_ptr<InvalidationLink> link() const { return _link; }

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
    // What the mutations that invalidate this query hold; cleared by the destructor.
    std::shared_ptr<InvalidationLink> _link;
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

/// @brief What a `Mutation` does with a run while another is in flight.
enum class Concurrency : std::uint8_t {
    /// Refuses the run: `run()` returns false and nothing is sent. The default: a second click on a pending
    /// button does nothing.
    Exclusive,
    /// Queues the run and sends it when the previous one settles, a failure included; one queue per
    /// `MutationOptions::serialKey`.
    Serial,
    /// Sends the run; only the newest run's reply is applied.
    Latest,
    /// Sends the run; every reply is applied as it arrives.
    Parallel,
};

/// @brief How a `Mutation` sequences its runs, and what a success invalidates.
/// @tparam A The action type.
template <typename A>
struct MutationOptions {
    /// @brief What a run does while another is in flight.
    Concurrency concurrency = Concurrency::Exclusive;
    /// @brief For `Serial`: the queue a run joins. Empty means one queue for every run.
    std::function<std::string(A const&)> serialKey;
    /// @brief The queries refetched, in the same batch as the result, after every applied success. A link whose
    ///        query is gone refetches nothing.
    std::vector<std::shared_ptr<InvalidationLink>> invalidates;
};

/// @brief A command: issues a write and tracks it, then refetches what the write invalidates.
///
/// `MutationOptions::concurrency` decides what a run does while another is in flight (`Concurrency`).
/// `pending()` is true while a run is in flight or queued. An applied success records `lastResult()`, clears
/// `error()`, ticks `successCount()` and refetches every live query in `MutationOptions::invalidates` — one
/// batch, so one flush and one frame. An applied failure records `error()` and invalidates nothing. A reply
/// `Latest` does not apply only stops counting as pending.
///
/// The runner runs untracked: a `run()` from inside an Effect does not subscribe that Effect to what the
/// runner reads, so a change there cannot issue the write again.
///
/// A runner that throws instead of returning a `Completion` fails that run: the exception becomes `error()`
/// at once, and nothing is left pending for it. `run()` off the Runtime's owner, and a delivery off it, are
/// reported (`detail::site::kOffOwner`) and dropped. A dropped delivery is never counted down, so `pending()`
/// stays true for good and an `Exclusive` mutation refuses every later run: it is a wiring error, not a state
/// to recover from. `run()` inside a Computed's computation is reported (`detail::site::kIssueInComputed`)
/// and refused before anything is issued.
///
/// The runner may destroy the mutation, and so may a query's fetcher during an invalidation, another handler
/// of the reply, or an Effect the reply wakes; the mutation is not touched afterwards.
/// @tparam A The action type.
/// @tparam R The result type; defaults to the action's registered result.
// NOLINTNEXTLINE(readability-redundant-typename) -- valid everywhere; implicit typename here is unverified on MSVC
template <typename A, typename R = typename model::ActionTraits<A>::Result>
class Mutation final {
public:
    /// @brief Issues one call.
    using Run = std::function<async::Completion<R>(A)>;

    /// @brief Builds a mutation over any runner — the test seam, and the interpreter's path.
    /// @param runtime The runtime. Borrowed: it must outlive the mutation.
    /// @param run Issues a call; its `Completion` must deliver on the runtime's owner.
    /// @param options Concurrency and invalidation.
    Mutation(Runtime& runtime MORPH_LIFETIMEBOUND, Run run, MutationOptions<A> options = {})
        : _rt{&runtime},
          _run{std::make_shared<Run const>(std::move(run))},
          _options{std::move(options)},
          _busy{runtime, 0},
          _error{runtime, nullptr},
          _last{runtime, std::nullopt},
          _successes{runtime, 0} {}

    /// @brief Builds a mutation that executes through a bridge handler.
    ///
    /// Checks the handler's callback executor against the runtime's owner (`detail::checkHandlerExecutor`).
    /// @tparam M The handler's model type.
    /// @tparam S The handler's sharing policy.
    /// @param runtime The runtime. Borrowed: it must outlive the mutation.
    /// @param handler Executes the calls; its callbacks must be delivered on the runtime's owner. Borrowed.
    /// @param options Concurrency and invalidation.
    template <typename M, typename S>
    Mutation(Runtime& runtime MORPH_LIFETIMEBOUND, bridge::BridgeHandler<M, S>& handler MORPH_LIFETIMEBOUND,
             MutationOptions<A> options = {})
        : Mutation(runtime, [&handler](A action) { return handler.execute(std::move(action)); }, std::move(options)) {
        detail::checkHandlerExecutor(runtime, handler);
    }

    ~Mutation() = default;
    Mutation(Mutation const&) = delete;
    Mutation& operator=(Mutation const&) = delete;
    Mutation(Mutation&&) = delete;
    Mutation& operator=(Mutation&&) = delete;

    /// @brief Issues, queues or refuses @p action, as `MutationOptions::concurrency` says. Refused, and reported,
    ///        off the Runtime's owner (`detail::site::kOffOwner`) and inside a Computed's computation
    ///        (`detail::site::kIssueInComputed`).
    /// @param action The action; built by the caller per gesture, so an idempotency key minted while building it
    ///        is fresh per click.
    /// @return False when the run was refused: `Exclusive` with a run in flight, off the owner, or inside a
    ///         Computed. A run that was sent or queued returns true, even when its runner threw.
    bool run(A action) {
        detail::RuntimeCore const& core = *_rt->core();
        if (!core.checkOwner()) {
            return false;
        }
        // A computation is pure: one that issued a write would issue it on every recomputation, and the
        // pending count it cannot write would never be counted back down.
        if (core.isComputing()) {
            core.report(detail::site::kIssueInComputed);
            return false;
        }
        if (_options.concurrency == Concurrency::Exclusive && _busy.peek() != 0) {
            return false;
        }
        if (_options.concurrency != Concurrency::Serial) {
            send(std::move(action), std::nullopt, false);
            return true;
        }
        std::string key = _options.serialKey ? _options.serialKey(action) : std::string{};
        Lane& lane = _lanes[key];
        if (lane.active) {
            lane.queued.push_back(std::move(action));
            _busy.set(_busy.peek() + 1);
            return true;
        }
        lane.active = true;
        send(std::move(action), std::move(key), false);
        return true;
    }

    /// @brief Whether a run is in flight or queued. Tracked.
    /// @return True while any is.
    [[nodiscard]] bool pending() const { return _busy.get() != 0; }

    /// @brief The most recent applied failure. Tracked.
    /// @return The exception, or null; cleared by the next applied success.
    [[nodiscard]] std::exception_ptr error() const { return _error.get(); }

    /// @brief The most recent applied result. Tracked.
    /// @return The result, or `nullopt` before the first success. Valid until the next applied success.
    [[nodiscard]] std::optional<R> const& lastResult() const { return _last.get(); }

    /// @brief How many successes have been applied. Tracked; it ticks once per applied success, so an Effect can
    ///        react to "it succeeded again" when the result is equal to the last one.
    /// @return The count.
    [[nodiscard]] std::uint64_t successCount() const { return _successes.get(); }

private:
    struct Lane {
        bool active = false;
        std::deque<A> queued;
    };

    // Sends one run. @p lane is the Serial queue it belongs to; @p counted says whether `_busy` already counts it
    // (a queued run does).
    void send(A action, std::optional<std::string> lane, bool counted) {
        std::uint64_t const generation = ++_generation;
        // Held here, so a runner that destroys the mutation keeps itself alive until it returns.
        std::shared_ptr<Run const> const runner = _run;
        async::CallbackToken const alive = _lifetime.token();
        async::Completion<R> completion;
        try {
            // Untracked: a run() called from an Effect must not subscribe that Effect to what the runner reads,
            // or a change there would re-run it and issue the write again.
            completion = _rt->untracked([&] { return (*runner)(std::move(action)); });
        } catch (...) {
            if (!alive.expired()) {
                settle(generation, lane, std::nullopt, std::current_exception(), counted);
            }
            return;
        }
        if (alive.expired()) {
            return;
        }
        if (!counted) {
            _busy.set(_busy.peek() + 1);
        }
        completion
            .then(_lifetime,
                  [this, generation, lane](R const& result) {
                      if (_rt->core()->checkOwner()) {
                          settle(generation, lane, result, nullptr, true);
                      }
                  })
            .onError(_lifetime, [this, generation, lane](std::exception_ptr const& error) {
                if (_rt->core()->checkOwner()) {
                    settle(generation, lane, std::nullopt, error, true);
                }
            });
    }

    // Applies one settled run, then sends the next queued run of its Serial lane.
    void settle(std::uint64_t generation, std::optional<std::string> const& lane, std::optional<R> result,
                std::exception_ptr const& error, bool counted) {
        bool const applies = _options.concurrency != Concurrency::Latest || generation == _generation;
        async::CallbackToken const alive = _lifetime.token();
        _rt->batch([&] {
            if (counted) {
                _busy.set(_busy.peek() - 1);
            }
            if (!applies) {
                return;
            }
            if (error != nullptr) {
                _error.set(error);
                return;
            }
            _last.set(std::move(result));
            _error.set(nullptr);
            _successes.set(_successes.peek() + 1);
            // A refetch runs the query's fetcher, which may destroy this mutation.
            for (auto const& link : _options.invalidates) {
                link->refetch();
                if (alive.expired()) {
                    return;
                }
            }
        });
        if (alive.expired() || !lane.has_value()) {
            return;
        }
        auto const found = _lanes.find(*lane);
        if (found == _lanes.end()) {
            return;
        }
        if (found->second.queued.empty()) {
            _lanes.erase(found);
            return;
        }
        A next = std::move(found->second.queued.front());
        found->second.queued.pop_front();
        send(std::move(next), lane, true);
    }

    Runtime* _rt;
    // Shared, so a running send() can keep the runner alive past the mutation.
    std::shared_ptr<Run const> _run;
    MutationOptions<A> _options;
    // Runs in flight or queued.
    Signal<std::size_t> _busy;
    Signal<std::exception_ptr> _error;
    Signal<std::optional<R>> _last;
    Signal<std::uint64_t> _successes;
    std::map<std::string, Lane> _lanes;
    std::uint64_t _generation = 0;
    // Last member, so it is destroyed first: a reply still in flight finds it stopped. Its tokens expire with the
    // mutation, which is how a runner or a refetch that destroyed the mutation finds out.
    async::CallbackScope _lifetime;
};

/// @brief The latest `R` the bridge published on a handler's instance, as reactive state.
///
/// Uses the handler's one subscription slot for `R`: two consumers of the same `R` use two handlers. The
/// subscription is gated by a scope the Subscription owns, so a publish delivered after it is destroyed —
/// one already queued included — is dropped. The handler keeps the dead entry until it is destroyed or
/// subscribes to `R` again. A delivery off the Runtime's owner is reported (`detail::site::kOffOwner`)
/// and dropped.
///
/// `latest()` is state, not an event stream: when `R` compares with `==`, a publish equal to the current
/// value does not notify. For something to happen on every publish, use `Query::refreshOn`.
/// @tparam R The published result type.
template <typename R>
class Subscription final {
public:
    /// @brief Subscribes to every `R` published on @p handler's instance, and checks the handler's callback
    ///        executor against the runtime's owner (`detail::checkHandlerExecutor`).
    /// @tparam M The handler's model type.
    /// @tparam S The handler's sharing policy.
    /// @param runtime The runtime. Borrowed: it must outlive the subscription.
    /// @param handler The handler whose instance publishes; its GUI executor must be the runtime's
    ///        owner. Borrowed: it must outlive the subscription.
    template <typename M, typename S>
    Subscription(Runtime& runtime MORPH_LIFETIMEBOUND, bridge::BridgeHandler<M, S>& handler MORPH_LIFETIMEBOUND)
        : _rt{&runtime}, _latest{runtime, std::nullopt} {
        detail::checkHandlerExecutor(runtime, handler);
        handler.template subscribe<R>(_lifetime, [this](R value) {
            if (!_rt->core()->checkOwner()) {
                return;
            }
            _latest.set(std::move(value));
        });
    }

    ~Subscription() = default;
    Subscription(Subscription const&) = delete;
    Subscription& operator=(Subscription const&) = delete;
    Subscription(Subscription&&) = delete;
    Subscription& operator=(Subscription&&) = delete;

    /// @brief The latest published value. Tracked.
    /// @return The value, or `nullopt` before the first publish. Valid until the next publish.
    [[nodiscard]] std::optional<R> const& latest() const { return _latest.get(); }

private:
    Runtime* _rt;
    Signal<std::optional<R>> _latest;
    // Last member, so it is destroyed first: a publish still in flight finds it stopped.
    async::CallbackScope _lifetime;
};

}  // namespace morph::reactive

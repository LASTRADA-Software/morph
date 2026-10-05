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

/// @brief How a `Query` refreshes besides key changes.
struct QueryOptions {
    /// @brief Runs the timed refresh; required when `refreshEvery` is positive. Borrowed: it must outlive
    ///        the query.
    Scheduler* scheduler = nullptr;
    /// @brief Re-fetch the current key this long after each call is issued, unless a call is then in flight;
    ///        zero means no timed refresh, and a negative value is refused.
    std::chrono::milliseconds refreshEvery{0};
};

/// @brief A derived async resource: the result of fetching whatever its tracked key names.
///
/// An internal Effect reads `key`. When the key changes — equality-gated when `A` compares with `==`
/// (`detail::DeeplyEqualityComparable`), otherwise on every re-run — the query fetches it under a fresh
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
    /// @param options Timed refresh; see QueryOptions.
    /// @throws std::invalid_argument when `options.refreshEvery` is negative, or positive while
    ///         `options.scheduler` is null; nothing has been fetched then.
    Query(Runtime& runtime MORPH_LIFETIMEBOUND, Fetch fetch, Key key, QueryOptions options = {})
        : _rt{&runtime},
          _fetch{std::make_shared<Fetch const>(std::move(fetch))},
          _pending{runtime, false},
          _value{runtime, std::nullopt},
          _error{runtime, nullptr},
          _refresh{options} {
        if (options.refreshEvery.count() < 0) {
            throw std::invalid_argument{"Query: refreshEvery must not be negative"};
        }
        if (options.refreshEvery.count() > 0 && options.scheduler == nullptr) {
            throw std::invalid_argument{"Query: refreshEvery needs a scheduler"};
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
    /// @param options Timed refresh; see QueryOptions.
    /// @throws std::invalid_argument when `options.refreshEvery` is negative, or positive while
    ///         `options.scheduler` is null; nothing has been fetched then.
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

    /// @brief Re-issues the current key under a new generation; idle stays idle. Refused, and reported,
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
        if constexpr (detail::DeeplyEqualityComparable<A>) {
            if (_keyed && next == _current) {
                return;
            }
        }
        _keyed = true;
        _current = std::move(next);
        ++_keys;
        issue();
    }

    void issue() {
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
    // Destroyed before the Effect and the signals a timed refresh writes, so no refresh outlives them.
    TimerHandle _timer;
    // Gates the subscription `refreshOn` installs. Its tokens expire with the query, which is how a run
    // that destroyed the query finds out.
    async::CallbackScope _lifetime;
    // Last member, so it is destroyed first: a reply still in flight finds it stopped.
    async::CallbackScope _inflight;
};

/// @brief What a `Mutation` does after it succeeds.
struct MutationOptions {
    /// @brief Queries to refetch, in the same batch as the result, after every success. None may be
    ///        destroyed while the mutation lives, so never list a query with a lifetime of its own (one
    ///        a `Scope` owns, say). Destroying them together with the mutation is fine.
    std::vector<Refetchable*> invalidates;
};

/// @brief A command: issues a write and tracks it, then refetches what the write invalidates.
///
/// `pending()` counts the calls in flight. A success records `lastResult()`, clears `error()` and
/// refetches every query in `MutationOptions::invalidates` — one batch, so one flush and one
/// frame. A failure records `error()` and invalidates nothing. Calls overlap: none supersedes
/// another, and each one's reply is delivered.
///
/// The runner runs untracked: a `run()` from inside an Effect does not subscribe that Effect to what the
/// runner reads, so a change there cannot issue the write again.
///
/// A runner that throws instead of returning a `Completion` fails that call: the exception becomes
/// `error()` and the call is not counted. `run()` off the Runtime's owner, and a delivery off it, are
/// reported (`detail::site::kOffOwner`) and dropped. A dropped delivery is never counted down, so
/// `pending()` stays true for good — a button bound to `!pending()` stays disabled. No later reply
/// clears it, since calls do not supersede one another. It is a wiring error, not a state to recover
/// from. `run()` inside a Computed's computation is reported (`detail::site::kIssueInComputed`) and
/// refused before anything is issued.
///
/// The runner may destroy the mutation, and so may a query's fetcher during an invalidation, another
/// handler of the reply, or an Effect the reply wakes; the mutation is not touched afterwards. A
/// query it invalidates must not be destroyed while the mutation lives: it holds the bare pointer.
/// Destroying them together, as members of one owner, is fine in either declaration order; declaring
/// the queries first is a convention, not the condition.
/// @tparam A The action type.
/// @tparam R The result type; defaults to the action's registered result.
// NOLINTNEXTLINE(readability-redundant-typename) -- valid everywhere; implicit typename here is unverified on MSVC
template <typename A, typename R = typename model::ActionTraits<A>::Result>
class Mutation final {
public:
    /// @brief Issues one call.
    using Run = std::function<async::Completion<R>(A)>;

    /// @brief Builds a mutation over any runner — the test seam.
    /// @param runtime The runtime. Borrowed: it must outlive the mutation.
    /// @param run Issues a call; its `Completion` must deliver on the runtime's owner.
    /// @param options The queries a success invalidates.
    Mutation(Runtime& runtime MORPH_LIFETIMEBOUND, Run run, MutationOptions options = {})
        : _rt{&runtime},
          _run{std::make_shared<Run const>(std::move(run))},
          _options{std::move(options)},
          _inFlight{runtime, 0},
          _error{runtime, nullptr},
          _last{runtime, std::nullopt} {}

    /// @brief Builds a mutation that executes through a bridge handler.
    /// @tparam M The handler's model type.
    /// @tparam S The handler's sharing policy.
    /// @param runtime The runtime. Borrowed: it must outlive the mutation.
    /// @param handler Executes the calls; its GUI executor must be the runtime's owner. Borrowed.
    /// @param options The queries a success invalidates.
    template <typename M, typename S>
    Mutation(Runtime& runtime MORPH_LIFETIMEBOUND, bridge::BridgeHandler<M, S>& handler MORPH_LIFETIMEBOUND,
             MutationOptions options = {})
        : Mutation(runtime, [&handler](A action) { return handler.execute(std::move(action)); }, std::move(options)) {}

    ~Mutation() = default;
    Mutation(Mutation const&) = delete;
    Mutation& operator=(Mutation const&) = delete;
    Mutation(Mutation&&) = delete;
    Mutation& operator=(Mutation&&) = delete;

    /// @brief Issues @p action. Refused, and reported, off the Runtime's owner
    ///        (`detail::site::kOffOwner`) and inside a Computed's computation
    ///        (`detail::site::kIssueInComputed`); nothing is issued then.
    /// @param action The action; built by the caller per gesture, so an idempotency key minted
    ///        while building it is fresh per click.
    void run(A action) {
        detail::RuntimeCore const& core = *_rt->core();
        if (!core.checkOwner()) {
            return;
        }
        // A computation is pure: one that issued a write would issue it on every recomputation, and the
        // in-flight count it cannot write would never be counted back down.
        if (core.isComputing()) {
            core.report(detail::site::kIssueInComputed);
            return;
        }
        // Held here, so a runner that destroys the mutation keeps itself alive until it returns.
        std::shared_ptr<Run const> const runner = _run;
        async::CallbackToken const alive = _lifetime.token();
        async::Completion<R> completion;
        try {
            // Untracked: a run() called from an Effect must not subscribe that Effect to what the runner
            // reads, or a change there would re-run it and issue the write again.
            completion = _rt->untracked([&] { return (*runner)(std::move(action)); });
        } catch (...) {
            if (!alive.expired()) {
                _error.set(std::current_exception());
            }
            return;
        }
        if (alive.expired()) {
            return;
        }
        _inFlight.set(_inFlight.peek() + 1);
        completion
            .then(_lifetime,
                  [this](R const& result) {
                      if (!_rt->core()->checkOwner()) {
                          return;
                      }
                      async::CallbackToken const alive = _lifetime.token();
                      _rt->batch([&] {
                          _last.set(result);
                          _error.set(nullptr);
                          _inFlight.set(_inFlight.peek() - 1);
                          // A refetch runs the query's fetcher, which may destroy this mutation.
                          for (Refetchable* const query : _options.invalidates) {
                              query->refetch();
                              if (alive.expired()) {
                                  return;
                              }
                          }
                      });
                  })
            .onError(_lifetime, [this](std::exception_ptr const& error) {
                if (!_rt->core()->checkOwner()) {
                    return;
                }
                _rt->batch([&] {
                    _error.set(error);
                    _inFlight.set(_inFlight.peek() - 1);
                });
            });
    }

    /// @brief A callable for a button: runs what @p make returns, or nothing when it returns `nullopt`.
    ///
    /// Bind the button's `enabled` to the same validity the `make` function checks, so a disabled
    /// button and a refused run agree.
    /// @param make Builds the action from current inputs, or `nullopt` when they are not valid.
    /// @return The callable; it refers to this mutation, which must outlive it.
    [[nodiscard]] std::function<void()> action(std::function<std::optional<A>()> make) {
        return [this, make = std::move(make)] {
            if (std::optional<A> built = make()) {
                run(std::move(*built));
            }
        };
    }

    /// @brief Whether at least one call is in flight. Tracked.
    /// @return True while the in-flight count is non-zero.
    [[nodiscard]] bool pending() const { return _inFlight.get() != 0; }

    /// @brief The most recent failure. Tracked.
    /// @return The exception, or null; cleared by the next success.
    [[nodiscard]] std::exception_ptr error() const { return _error.get(); }

    /// @brief The most recent successful result. Tracked.
    /// @return The result, or `nullopt` before the first success. Valid until the next success.
    [[nodiscard]] std::optional<R> const& lastResult() const { return _last.get(); }

private:
    Runtime* _rt;
    // Shared, so a running run() can keep the runner alive past the mutation.
    std::shared_ptr<Run const> _run;
    MutationOptions _options;
    Signal<std::size_t> _inFlight;
    Signal<std::exception_ptr> _error;
    Signal<std::optional<R>> _last;
    // Last member, so it is destroyed first: a reply still in flight finds it stopped. Its tokens expire
    // with the mutation, which is how a runner or a refetch that destroyed the mutation finds out.
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
    /// @brief Subscribes to every `R` published on @p handler's instance.
    /// @tparam M The handler's model type.
    /// @tparam S The handler's sharing policy.
    /// @param runtime The runtime. Borrowed: it must outlive the subscription.
    /// @param handler The handler whose instance publishes; its GUI executor must be the runtime's
    ///        owner. Borrowed: it must outlive the subscription.
    template <typename M, typename S>
    Subscription(Runtime& runtime MORPH_LIFETIMEBOUND, bridge::BridgeHandler<M, S>& handler MORPH_LIFETIMEBOUND)
        : _rt{&runtime}, _latest{runtime, std::nullopt} {
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

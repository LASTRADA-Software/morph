// SPDX-License-Identifier: Apache-2.0

#pragma once
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <type_traits>
#include <utility>

#include "detail/graph.hpp"
#include "runtime.hpp"

/// @file
/// @brief `Signal<T>`, `Computed<T>` and `Effect`: the nodes of a reactive graph.
///
/// Specified in `docs/spec/reactive/signals.md`.

namespace morph::reactive {

/// @brief Whether a `Signal` or `Computed` compares a new value with the current one before notifying.
enum class EqualityPolicy : std::uint8_t {
    /// Skip a value equal to the current one, when `==` is usable for the type (`kEqualityUsable`).
    Auto,
    /// Notify on every write or recomputation, even of an equal value.
    Always,
};

/// @brief Whether `==` is usable for @p T, which is what the `EqualityPolicy::Auto` skip needs.
///
/// Checked through element types, not by `std::equality_comparable` alone: a standard container declares `==`
/// for any element type, and comparing two of them fails to compile when the element has none. A type for which
/// this is false always notifies.
/// @tparam T The value type.
template <typename T>
inline constexpr bool kEqualityUsable = detail::EqualityUsable<std::remove_cv_t<T>>::value;

/// @brief A source value. Reading it with `get()` inside a tracked run subscribes that run.
///
/// `set()` skips a value equal to the current one when `==` is usable for `T` (`kEqualityUsable`) and the
/// policy is `EqualityPolicy::Auto`; `mutate()` always notifies. Writes are refused, and reported, off the
/// owner or inside a Computed. A read off the owner is reported and subscribes nothing.
/// @tparam T The value type; move-constructible and move-assignable.
template <typename T>
class Signal final : public detail::Node {
public:
    /// @brief Constructs a value-initialised signal.
    /// @param runtime The runtime this signal belongs to.
    explicit Signal(Runtime& runtime)
        requires std::default_initializable<T>
        : Node{runtime.core()}, _value{} {}

    /// @brief Constructs a signal holding @p initial.
    /// @param runtime The runtime this signal belongs to.
    /// @param initial The initial value.
    /// @param policy Whether a write equal to the current value is skipped.
    Signal(Runtime& runtime, T initial, EqualityPolicy policy = EqualityPolicy::Auto)
        : Node{runtime.core()}, _value{std::move(initial)}, _policy{policy} {}

    ~Signal() override = default;
    Signal(Signal const&) = delete;
    Signal& operator=(Signal const&) = delete;
    Signal(Signal&&) = delete;
    Signal& operator=(Signal&&) = delete;

    /// @brief Reads the value, subscribing the innermost tracked run.
    ///
    /// Off the owner the read is reported (`detail::site::kOffOwner`) and proceeds untracked: it
    /// subscribes nothing and leaves the tracking state alone. The value it returns is then read
    /// without synchronisation against the owner's writes.
    /// @return The current value; valid until the next write.
    [[nodiscard]] T const& get() const {
        if (core().checkOwner()) {
            trackRead();
        }
        return _value;
    }

    /// @brief Reads the value without subscribing anything.
    /// @return The current value; valid until the next write.
    [[nodiscard]] T const& peek() const noexcept { return _value; }

    /// @brief Replaces the value and notifies observers, unless it equals the current one and the policy is
    ///        `EqualityPolicy::Auto`.
    /// @param value The new value.
    void set(T value) {
        if (!writable()) {
            return;
        }
        if constexpr (kEqualityUsable<T>) {
            if (_policy == EqualityPolicy::Auto && _value == value) {
                return;
            }
        }
        _value = std::move(value);
        notify();
    }

    /// @brief Changes the value in place and always notifies observers.
    /// @tparam F A callable taking `T&`.
    /// @param fn The mutation.
    template <typename F>
        requires std::invocable<F, T&>
    void mutate(F&& fn) {
        if (!writable()) {
            return;
        }
        std::invoke(std::forward<F>(fn), _value);
        notify();
    }

private:
    [[nodiscard]] bool writable() const noexcept {
        if (!core().checkOwner()) {
            return false;
        }
        if (core().isComputing()) {
            core().report(detail::site::kSetInComputed);
            return false;
        }
        return true;
    }

    void notify() {
        detail::BatchScope const batch{core()};
        markObserversStale(detail::Colour::Dirty);
    }

    // A source has nothing to recompute: a write marks its observers itself.
    bool recompute() override { return false; }  // NOLINT(portability-template-virtual-member-function)

    T _value;
    EqualityPolicy _policy = EqualityPolicy::Auto;
};

/// @brief A derived value: computed lazily on first read, cached, and recomputed only after a source it
///        read really changed.
///
/// Observers are notified when the result changes. When `==` is usable for `T` (`kEqualityUsable`) and the
/// policy is `EqualityPolicy::Auto`, an equal result stops propagation; otherwise every recomputation
/// propagates.
///
/// A computation that throws leaves the Computed failed, and that is a change: observers re-run. While it
/// is failed, a read behaves as follows:
/// - the first read after the failing run rethrows that run's exception without computing again;
/// - every later read retries the computation, returning its value if it now succeeds and rethrowing
///   if it fails again.
///
/// A retry that fails again counts as unchanged and notifies nobody, so readers that retry do not wake
/// each other. A failure after a source changed always propagates, as does a recovery, even to the value
/// the Computed had before it failed, because its observers last saw a failure. A retry that recovers
/// outside a flush posts one, so the observers it wakes run. A failed run keeps every source it has read,
/// before and during that run, so a later change to any of them recomputes it.
///
/// Writing a signal inside the computation is reported and refused. So is reading the Computed while it
/// is being brought up to date: from inside its own computation, or from the computation of a node it
/// reads (a cycle, which may form only after the first evaluation). Such a read throws
/// `std::logic_error`.
///
/// A computation may destroy its own Computed: the callable stays alive until it returns, and the
/// Computed is not touched afterwards.
/// @tparam T The value type; move-constructible. It need not be default-constructible.
template <typename T>
class Computed final : public detail::Node {
public:
    /// @brief Constructs the Computed without computing it: the first read does.
    /// @tparam F A callable taking no arguments and returning something convertible to `T`; it may be
    ///         move-only.
    /// @param runtime The runtime this computed belongs to.
    /// @param fn The computation; every signal or computed it reads becomes a dependency.
    /// @param policy Whether a result equal to the last one stops propagation.
    template <typename F>
        requires std::invocable<F&> && std::move_constructible<F> && std::convertible_to<std::invoke_result_t<F&>, T>
    Computed(Runtime& runtime, F fn, EqualityPolicy policy = EqualityPolicy::Auto)
        : Node{runtime.core()}, _fn{std::make_shared<F>(std::move(fn))}, _compute{&compute<F>}, _policy{policy} {
        forceDirty();
    }

    ~Computed() override = default;
    Computed(Computed const&) = delete;
    Computed& operator=(Computed const&) = delete;
    Computed(Computed&&) = delete;
    Computed& operator=(Computed&&) = delete;

    /// @brief Reads the value, bringing it up to date first, and subscribes the innermost tracked run.
    ///
    /// Off the owner the read is reported (`detail::site::kOffOwner`) and refused: it recomputes
    /// nothing and subscribes nothing, and returns the value the last successful computation produced,
    /// read without synchronisation against the owner.
    /// @return The current value; valid until the next recomputation.
    /// @throws Whatever the computation throws. `std::logic_error` when the Computed is read while it is
    ///         being brought up to date (reported, `detail::site::kComputedReadsItself`), when its
    ///         computation destroys it, or off the owner when it has never been computed successfully.
    [[nodiscard]] T const& get() const {
        if (!core().checkOwner()) {
            return cached();
        }
        refuseSelfRead();
        trackRead();
        return mutableSelf().current();
    }

    /// @brief Reads the value, bringing it up to date first, without subscribing anything.
    ///
    /// Off the owner it behaves as `get()` does there.
    /// @return The current value; valid until the next recomputation.
    /// @throws Whatever `get()` throws.
    [[nodiscard]] T const& peek() const {
        if (!core().checkOwner()) {
            return cached();
        }
        refuseSelfRead();
        detail::TrackingFrame const untracked{core(), nullptr};
        return mutableSelf().current();
    }

private:
    // Tells the runtime a computation is running, so a write inside it is refused. Holds the core rather
    // than the Computed, which the run may destroy.
    class Computing {
    public:
        explicit Computing(detail::RuntimeCore& core) noexcept : _core{&core} { core.enterComputed(); }
        ~Computing() { _core->leaveComputed(); }
        Computing(Computing const&) = delete;
        Computing& operator=(Computing const&) = delete;
        Computing(Computing&&) = delete;
        Computing& operator=(Computing&&) = delete;

    private:
        detail::RuntimeCore* _core;
    };

    template <typename F>
    static T compute(void* fn) {
        return std::invoke(*static_cast<F*>(fn));
    }

    // Reading is logically const; the cache, the failure and the dependency links are bookkeeping.
    [[nodiscard]] Computed& mutableSelf() const noexcept {
        return const_cast<Computed&>(*this);  // NOLINT(cppcoreguidelines-pro-type-const-cast)
    }

    [[nodiscard]] T const& cached() const {
        if (!_value.has_value()) {
            throw std::logic_error{"morph::reactive: a Computed read off the owner before it had a value"};
        }
        return *_value;
    }

    void refuseSelfRead() const {
        if (isPulling()) {
            core().report(detail::site::kComputedReadsItself);
            throw std::logic_error{"morph::reactive: a Computed read itself"};
        }
    }

    T const& current() {
        // A retry that recovers wakes observers that are not queued yet; the batch posts the flush that
        // runs them.
        detail::BatchScope const batch{core()};
        pullOrThrow();
        // Up to date now: a source that really changed has recomputed it. A failure some reader has
        // already seen is retried, one nobody has seen yet is delivered as is, and having neither a value
        // nor a failure (storing the last result threw) is retried as well. Each retry repeats the last
        // run's inputs.
        if (_error != nullptr ? !_errorUnread : !_value.has_value()) {
            _isRetry = true;
            forceDirty();
            pullOrThrow();
        }
        if (_error != nullptr) {
            _errorUnread = false;
            std::rethrow_exception(_error);
        }
        if (!_value.has_value()) {
            throw std::logic_error{"morph::reactive: a Computed has no value"};
        }
        return *_value;
    }

    void pullOrThrow() {
        if (!updateIfNecessary()) {
            throw std::logic_error{"morph::reactive: a Computed was destroyed by its own computation"};
        }
    }

    bool recompute() override {  // NOLINT(portability-template-virtual-member-function)
        // Held by the run, so a computation that destroys this Computed keeps its own closure until it
        // returns.
        std::shared_ptr<void> const fn = _fn;
        T (*const run)(void*) = _compute;
        detail::TrackingFrame frame{core(), this};
        bool const isRetry = std::exchange(_isRetry, false);
        std::optional<T> fresh;
        std::exception_ptr error;
        {
            Computing const computing{core()};
            try {
                fresh.emplace(run(fn.get()));
            } catch (...) {
                error = std::current_exception();
            }
        }
        if (frame.observerDestroyed()) {
            return false;
        }
        if (error != nullptr) {
            bool const repeated = isRetry && _error != nullptr;
            _error = error;
            _errorUnread = true;
            mergeSources(frame.takeSources());
            return !repeated;
        }
        adoptSources(frame.takeSources());
        bool const recovered = std::exchange(_error, nullptr) != nullptr;
        if constexpr (kEqualityUsable<T>) {
            if (_policy == EqualityPolicy::Auto && !recovered && _value.has_value() && *_value == *fresh) {
                return false;
            }
        }
        _value.emplace(std::move(*fresh));
        return true;
    }

    std::shared_ptr<void> _fn;
    T (*_compute)(void*);
    EqualityPolicy _policy;
    std::optional<T> _value;
    // The latest computation's failure, or null after a success.
    std::exception_ptr _error;
    // Whether `_error` has not reached a reader yet.
    bool _errorUnread = false;
    // Whether the next run is a retry, forced by a read of an up-to-date node.
    bool _isRetry = false;
};

/// @brief A side effect: runs once on construction, then in a flush after any source it read changed.
///
/// Queued Effects run shallowest scope first: an Effect a `Scope` makes carries that scope's depth, so an
/// owner's Effects run before those of the scopes it owns. Among Effects of one depth, older ones run first.
///
/// An exception escaping it is a defect: reported (`detail::site::kEffectThrew`), and the flush stops.
/// A run that throws keeps every source it has read, before and during that run, so a later change
/// to any of them runs it again. A body may destroy its own Effect (a remount); the body's callable
/// stays alive until the body returns, and the Effect is not touched afterwards.
class Effect final : public detail::Node {
public:
    /// @brief Constructs the Effect and runs it once. Off the owner this is reported
    ///        (`detail::site::kOffOwner`) and the Effect never runs.
    /// @tparam F A callable taking no arguments; it may be move-only.
    /// @param runtime The runtime this effect belongs to.
    /// @param fn The body; every signal it reads becomes a dependency.
    /// @param depth The depth of the `Scope` that owns this Effect (`Scope::depth()`); zero for an Effect no
    ///        scope owns.
    template <typename F>
        requires std::invocable<F&> && std::move_constructible<F>
    Effect(Runtime& runtime, F fn, std::size_t depth = 0)
        : Node{runtime.core(), depth}, _body{std::make_shared<F>(std::move(fn))}, _run{&runBody<F>} {
        forceDirty();
        if (!bornOnOwner()) {
            return;
        }
        try {
            updateIfNecessary();
        } catch (...) {
            core().report(detail::site::kEffectThrew);
        }
    }

    ~Effect() override = default;
    Effect(Effect const&) = delete;
    Effect& operator=(Effect const&) = delete;
    Effect(Effect&&) = delete;
    Effect& operator=(Effect&&) = delete;

private:
    [[nodiscard]] bool isEffect() const noexcept override { return true; }

    template <typename F>
    static void runBody(void* body) {
        std::invoke(*static_cast<F*>(body));
    }

    bool recompute() override {
        // Held by the run, so a body that destroys this Effect keeps its own closure until it returns.
        std::shared_ptr<void> const body = _body;
        void (*const run)(void*) = _run;
        detail::TrackingFrame frame{core(), this};
        try {
            run(body.get());
        } catch (...) {
            if (!frame.observerDestroyed()) {
                mergeSources(frame.takeSources());
            }
            throw;
        }
        if (!frame.observerDestroyed()) {
            adoptSources(frame.takeSources());
        }
        return false;
    }

    std::shared_ptr<void> _body;
    void (*_run)(void*);
};

}  // namespace morph::reactive

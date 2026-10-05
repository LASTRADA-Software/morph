// SPDX-License-Identifier: Apache-2.0

#pragma once
#include <concepts>
#include <exception>
#include <functional>
#include <memory>
#include <type_traits>
#include <utility>
#include <variant>

#include "../attributes.hpp"
#include "../core/bridge.hpp"
#include "../core/callback_scope.hpp"
#include "../core/registry.hpp"
#include "runtime.hpp"

/// @file
/// @brief `morph::reactive::Store`: user-intent view state, changed only by exhaustive messages.
///
/// Specified in `docs/spec/reactive/store.md`.

namespace morph::reactive {

namespace detail {

/// @brief Whether @p Update handles every alternative of the variant @p Msg.
/// @tparam Update The update callable.
/// @tparam ViewState The state type.
/// @tparam Msg Anything but a `std::variant`: never exhaustive.
template <typename Update, typename ViewState, typename Msg>
struct UpdateCoversAll : std::false_type {};

/// @brief Whether @p Update handles every alternative of `std::variant<Alts...>`.
/// @tparam Update The update callable.
/// @tparam ViewState The state type.
/// @tparam Alts The message alternatives.
template <typename Update, typename ViewState, typename... Alts>
struct UpdateCoversAll<Update, ViewState, std::variant<Alts...>>
    : std::bool_constant<(std::invocable<Update&, ViewState&, Alts const&> && ...)> {};

/// @brief One update in progress, on the stack of the `send()` running it.
struct UpdateFrame {
    bool storeDied = false;  ///< Set by the Store's destructor while the update is still running.
};

/// @brief Marks a Store as inside its update for the guard's lifetime.
class UpdateGuard {
public:
    /// @param slot The Store's in-update slot; it points at @p frame until the guard ends.
    /// @param frame The update's frame.
    UpdateGuard(UpdateFrame*& slot, UpdateFrame& frame) noexcept : _slot{&slot}, _frame{&frame} { slot = &frame; }
    /// @brief Clears the Store's in-update slot, also when the update throws, unless the Store died.
    ~UpdateGuard() {
        if (!_frame->storeDied) {
            *_slot = nullptr;
        }
    }
    UpdateGuard(UpdateGuard const&) = delete;
    UpdateGuard& operator=(UpdateGuard const&) = delete;
    UpdateGuard(UpdateGuard&&) = delete;
    UpdateGuard& operator=(UpdateGuard&&) = delete;

private:
    UpdateFrame** _slot;
    UpdateFrame* _frame;
};

}  // namespace detail

/// @brief An update that handles every message: `Update&` is invocable as `(ViewState&, Alt const&)` for
///        each alternative `Alt` of the `std::variant` `Msg`. Any `Msg` that is not a `std::variant` is
///        never satisfied.
template <typename Update, typename ViewState, typename Msg>
concept ExhaustiveUpdate = detail::UpdateCoversAll<Update, ViewState, Msg>::value;

/// @brief User-intent view state — what is selected, typed, open — changed only by messages.
///
/// `ViewState` is a struct of `Signal<T>` fields, built in place by `init` (signals cannot move).
/// Every `send()` runs the update in one batch, untracked, so N field writes are one flush.
/// The view reads `state()`; only the update writes.
///
/// `send()` off the Runtime's owner is reported (`detail::site::kOffOwner`) and dropped, as is a
/// `send()` from inside this Store's own update (`detail::site::kSendInUpdate`): an update is a pure
/// state transition, and a message it sent would run against a half-applied state.
///
/// An update may destroy its own Store (a controller that closes its view): the update's callable
/// stays alive until it returns, and `send()` touches nothing of the Store afterwards. The update itself
/// must not use the state or the Store once it has destroyed them.
/// @tparam ViewState A struct of `Signal<T>` fields.
/// @tparam Msg A `std::variant` of message types.
template <typename ViewState, typename Msg>
class Store {
public:
    /// @tparam Init Callable `(Runtime&) -> ViewState`, returning by prvalue.
    /// @tparam Update Callable satisfying `ExhaustiveUpdate<Update, ViewState, Msg>`; it may be move-only.
    /// @param runtime The runtime. Borrowed: it must outlive the Store.
    /// @param init Builds the initial state.
    /// @param update Applies one message to the state.
    template <typename Init, typename Update>
        requires std::invocable<Init&, Runtime&> && std::same_as<std::invoke_result_t<Init&, Runtime&>, ViewState> &&
                     std::move_constructible<Update> && ExhaustiveUpdate<Update, ViewState, Msg>
    Store(Runtime& runtime MORPH_LIFETIMEBOUND, Init init, Update update)
        : _rt{&runtime},
          _state{init(runtime)},
          _update{std::make_shared<Update>(std::move(update))},
          _apply{&applyUpdate<Update>} {}

    /// @brief Destroys the state, and tells an update still running that the Store is gone.
    ~Store() {
        if (_updating != nullptr) {
            _updating->storeDied = true;
        }
    }
    Store(Store const&) = delete;
    Store& operator=(Store const&) = delete;
    Store(Store&&) = delete;
    Store& operator=(Store&&) = delete;

    /// @brief Applies @p msg in one batch. Refused off the owner, and inside an update.
    ///
    /// An exception from the update propagates to the caller; writes made before it still flush.
    /// @param msg The message.
    void send(Msg msg) {
        auto& core = *_rt->core();
        if (!core.checkOwner()) {
            return;
        }
        if (_updating != nullptr) {
            core.report(detail::site::kSendInUpdate);
            return;
        }
        // The update may destroy this Store: everything used after it starts is a local, and the
        // callable is held here so that it outlives the Store until it returns.
        Runtime& runtime = *_rt;
        std::shared_ptr<void> const update = _update;
        void (*const apply)(void*, ViewState&, Msg const&) = _apply;
        ViewState& state = _state;
        detail::UpdateFrame*& slot = _updating;
        detail::UpdateFrame frame;
        // The guard empties the slot when the update ends, unless the Store died, and then the slot is gone.
        // NOLINTNEXTLINE(clang-analyzer-core.StackAddressEscape)
        runtime.batch([&] {
            runtime.untracked([&] {
                detail::UpdateGuard const guard{slot, frame};
                apply(update.get(), state, msg);
            });
        });
    }

    /// @brief The state, for reading. A field's `get()` tracks exactly that field.
    /// @return The state.
    [[nodiscard]] ViewState const& state() const noexcept { return _state; }

    /// @brief A callable that sends @p msg each time it is called — what a button binds to.
    /// @param msg The message, copied into the callable.
    /// @return The callable; it refers to this Store, which must outlive it.
    [[nodiscard]] std::function<void()> action(Msg msg) {
        return [this, msg = std::move(msg)] { send(msg); };
    }

private:
    template <typename Update>
    static void applyUpdate(void* update, ViewState& state, Msg const& msg) {
        std::visit([&](auto const& alternative) { (*static_cast<Update*>(update))(state, alternative); }, msg);
    }

    Runtime* _rt;
    ViewState _state;
    // Type-erased and shared, so a move-only update fits and a running send() can keep it alive.
    std::shared_ptr<void> _update;
    void (*_apply)(void*, ViewState&, Msg const&);
    detail::UpdateFrame* _updating = nullptr;
};

/// @brief Executes @p action and sends its result, or its failure, to @p store as one message.
///
/// Callbacks land on the handler's GUI executor, which must be the Store's owner; each delivery is
/// one batch. @p scope gates both. `Query` and `Mutation` follow the same delivery rules without a
/// Store to send into.
///
/// Every call delivers: there is no latest-wins. Superseding an earlier call means stopping, resetting
/// or destroying the scope it was given; `Query` provides latest-wins.
///
/// A throw from @p toMsg or from the update it sends to, on the success path, or from @p toFailMsg, is
/// caught and logged by the `Completion` delivering it. It is not turned into a failure message.
/// @tparam ViewState The Store's state type.
/// @tparam Msg The Store's message variant.
/// @tparam Model The handler's model type.
/// @tparam Sharing The handler's sharing policy.
/// @tparam Action The action type.
/// @tparam ToMsg Callable `(Result const&) -> alternative of Msg`.
/// @tparam ToFailMsg Callable `(std::exception_ptr const&) -> alternative of Msg`.
/// @param store Receives the message. Must outlive every delivery @p scope does not gate.
/// @param handler Executes the action.
/// @param scope Gate: a reply arriving after it is stopped or destroyed is dropped.
/// @param action The action.
/// @param toMsg Maps the result to a message.
/// @param toFailMsg Maps the failure to a message.
template <typename ViewState, typename Msg, typename Model, typename Sharing, typename Action, typename ToMsg,
          typename ToFailMsg>
void request(Store<ViewState, Msg>& store, bridge::BridgeHandler<Model, Sharing>& handler,
             async::CallbackScope const& scope, Action action, ToMsg toMsg, ToFailMsg toFailMsg) {
    using Result = model::ActionTraits<Action>::Result;
    handler.execute(std::move(action))
        .then(scope, [&store, toMsg = std::move(toMsg)](Result const& result) { store.send(Msg{toMsg(result)}); })
        .onError(scope, [&store, toFailMsg = std::move(toFailMsg)](std::exception_ptr const& error) {
            store.send(Msg{toFailMsg(error)});
        });
}

}  // namespace morph::reactive

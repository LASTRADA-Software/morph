// SPDX-License-Identifier: Apache-2.0

#pragma once

/// @file forms/flows.hpp
/// @brief Multi-step wizards: an ordered sequence of registered actions that
///        share a "flow draft" spanning the whole sequence.
///
/// A `morph::flows::Wizard` names an ordered list of already-registered
/// action types (see `BRIDGE_REGISTER_ACTION`, registry.hpp) plus, per step,
/// a human title and an optional `Bind` prefill declaration mapping that
/// step's field name to a `"<PriorAction>.<field>"` path into an earlier
/// step's submitted draft or result. `wizardSchemaJson<W>()` emits the `w-*`
/// document a renderer consumes to build a stepper;
/// `morph::flows::FlowSession<Model, Steps...>` (see the `set<>`/`advance`/
/// `back` API added alongside it) is the typed C++ sequencer that fires each
/// step through the ordinary `BridgeHandler<Model>::set<>` / `subscribe<>`
/// path (bridge.hpp) and tracks the resolved values a caller (or a renderer)
/// reads to prefill a later step.
///
/// This is purely additive metadata and sequencing over the existing
/// dispatch path: no new wire format, no new execution mode. See
/// docs/spec/forms/workflows_navigation.md.

#include <atomic>
#include <cstddef>
#include <exception>
#include <functional>
#include <glaze/glaze.hpp>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <unordered_map>
#include <utility>

#include "../core/bridge.hpp"
#include "../core/logger.hpp"
#include "forms.hpp"

namespace morph::flows {

/// @brief One `field -> "<PriorAction>.<field>"` prefill binding declared on
///        a wizard step.
/// @tparam Field The step action's field name to prefill.
/// @tparam Path  Source path, `"<PriorAction>.<field>"`, into an earlier
///               step's captured values (see `FlowSession::resolved`).
template <morph::forms::FixedString Field, morph::forms::FixedString Path>
struct Bind {
    /// @brief The step action's field name this binding fills.
    /// @return The declared field name.
    [[nodiscard]] static constexpr std::string_view field() noexcept { return Field.view(); }

    /// @brief The source path into an earlier step's captured values.
    /// @return The declared `"<PriorAction>.<field>"` path.
    [[nodiscard]] static constexpr std::string_view path() noexcept { return Path.view(); }
};

/// @brief One step of a `Wizard`: a registered action, a display title, and
///        zero or more `Bind` prefill declarations.
/// @tparam Action Registered action type (`BRIDGE_REGISTER_ACTION`) this step fires.
/// @tparam Title  Human title for the step (breadcrumb / header).
/// @tparam Binds  Zero or more `Bind<Field, Path>` prefill declarations.
template <typename Action, morph::forms::FixedString Title, typename... Binds>
struct WizardStep {
    /// @brief The step's action type.
    using action = Action;

    /// @brief Tuple of this step's `Bind<...>` prefill declarations (possibly empty).
    using binds = std::tuple<Binds...>;

    /// @brief The step's display title.
    /// @return The declared title.
    [[nodiscard]] static constexpr std::string_view title() noexcept { return Title.view(); }
};

/// @brief An ordered sequence of `WizardStep`s sharing one flow.
/// @tparam Title Human title for the whole flow.
/// @tparam Steps One or more `WizardStep<Action, Title, Binds...>` types, in order.
template <morph::forms::FixedString Title, typename... Steps>
struct Wizard {
    /// @brief Tuple of this wizard's ordered `WizardStep<...>` types.
    using steps = std::tuple<Steps...>;

    /// @brief The wizard's display title.
    /// @return The declared title.
    [[nodiscard]] static constexpr std::string_view title() noexcept { return Title.view(); }
};

/// @brief Traits specialisation mapping a `Wizard` type to its string type-id.
///
/// Specialise via `BRIDGE_REGISTER_WIZARD` rather than by hand. The default is
/// a forward declaration — using it without a specialisation is an
/// incomplete-type error.
/// @tparam W Concrete `Wizard<...>` type.
template <typename W>
struct WizardTraits;  // forward — specialise or use BRIDGE_REGISTER_WIZARD

namespace detail {

/// @brief Invokes `visitor.template operator()<std::tuple_element_t<I, Tuple>, I>()`
///        for every element of @p Tuple, in order.
/// @tparam Tuple   A `std::tuple<...>` type (only its element types/arity are used).
/// @tparam Visitor Callable with a `template<typename Element, std::size_t I> operator()()`.
/// @param visitor Callable invoked once per tuple element.
template <typename Tuple, typename Visitor>
constexpr void forEachTupleElement(Visitor&& visitor) {
    []<std::size_t... I>(std::index_sequence<I...>, Visitor&& innerVisitor) {
        (innerVisitor.template operator()<std::tuple_element_t<I, Tuple>, I>(), ...);
    }(std::make_index_sequence<std::tuple_size_v<Tuple>>{}, std::forward<Visitor>(visitor));
}

/// @brief Invokes `visitor.template operator()<Step>()` for the pack element
///        of `Steps...` at runtime position @p index. A no-op when
///        `index >= sizeof...(Steps)`.
/// @tparam Steps   The pack to index into.
/// @tparam Visitor Callable with a `template<typename Step> operator()()`.
/// @param index   0-based position to visit.
/// @param visitor Callable invoked for the step at @p index.
template <typename... Steps, typename Visitor>
constexpr void forStep(std::size_t index, Visitor&& visitor) {
    std::size_t i = 0;
    (void)((i++ == index ? (visitor.template operator()<Steps>(), true) : false) || ...);
}

/// @brief Trait: `true` when every type in `Ts...` is pairwise distinct.
/// @tparam Ts Types to check for pairwise distinctness.
template <typename... Ts>
struct AllDistinct : std::true_type {};

/// @brief Recursive case: `T` distinct from every type in `Rest...`, and `Rest...` pairwise distinct.
/// @tparam T    The type being checked against `Rest...`.
/// @tparam Rest The remaining types.
template <typename T, typename... Rest>
struct AllDistinct<T, Rest...> : std::bool_constant<(!std::is_same_v<T, Rest> && ...) && AllDistinct<Rest...>::value> {
};

}  // namespace detail

/// @brief Generates the `w-*` JSON document for wizard type @p W.
///
/// Emits `w-title` and an ordered `w-steps` array; each step carries `action`
/// (the step's registered action type-id), `title`, and — only when the step
/// declares at least one `Bind` — a `prefill` object mapping field name to
/// `"<PriorAction>.<field>"` path. See docs/spec/forms/workflows_navigation.md
/// for the full key vocabulary.
/// @tparam W Concrete `Wizard<Title, Steps...>` type.
/// @return The wizard's JSON document. Empty string only if glaze's own JSON
///         writer fails on the assembled DOM (schema generation never throws).
template <typename W>
[[nodiscard]] std::string wizardSchemaJson() {
    glz::generic_u64 dom{};
    dom["w-title"] = std::string{W::title()};

    glz::generic_u64::array_t steps{};
    detail::forEachTupleElement<typename W::steps>([&]<typename StepT, std::size_t I>() {
        static_cast<void>(I);
        glz::generic_u64 step{};
        step["action"] = std::string{::morph::model::ActionTraits<typename StepT::action>::typeId()};
        step["title"] = std::string{StepT::title()};
        if constexpr (std::tuple_size_v<typename StepT::binds> != 0) {
            auto& prefillNode = step["prefill"];
            detail::forEachTupleElement<typename StepT::binds>([&]<typename BindT, std::size_t J>() {
                static_cast<void>(J);
                prefillNode[std::string{BindT::field()}] = std::string{BindT::path()};
            });
        }
        steps.emplace_back(std::move(step));
    });
    dom["w-steps"] = steps;

    return glz::write_json(dom).value_or(std::string{});
}

/// @brief Sequences an ordered list of registered actions (`Steps...`) as one
///        multi-step flow sharing captured values across steps.
///
/// Each step is fired through the handler's ordinary reactive path
/// (`BridgeHandler::set<>` / `subscribe<>`, bridge.hpp) — a `FlowSession`
/// contributes only sequencing (`advance`/`back`) and a resolved-values map
/// callers use to prefill a later step from an earlier one's submitted
/// fields or result. `Steps...` must be pairwise distinct action types: each
/// occupies one slot of the handler's per-action-type draft (bridge.md's
/// "Subscription semantics" — exactly one `SubscriberEntry` per action type),
/// so reusing the same action type twice in one flow would collide on that
/// single slot.
///
/// @tparam Model Concrete model type owning the `BridgeHandler` this flow dispatches through.
/// @tparam Steps Ordered, pairwise-distinct action types (the flow's steps).
template <typename Model, typename... Steps>
class FlowSession {
    static_assert(sizeof...(Steps) > 0, "FlowSession: a flow needs at least one step");
    static_assert(detail::AllDistinct<Steps...>::value, "FlowSession: step action types must be pairwise distinct");

public:
    /// @brief Constructs a flow over @p handler, starting at step 0.
    /// @param handler Handler the flow dispatches every step through. Must
    ///                outlive this `FlowSession` (only *destruction* order is
    ///                unconstrained; see bridge.md's Lifetime & ownership).
    /// @param onError Optional callback invoked when the current step's fire
    ///                fails (e.g. `BackendChangedError` mid-flight). When
    ///                absent, the error is logged via `morph::log::logError`,
    ///                matching `BridgeHandler`'s own no-`errSink` default.
    explicit FlowSession(::morph::bridge::BridgeHandler<Model>& handler,
                         std::function<void(std::exception_ptr)> onError = nullptr)
        : _handler{handler}, _onError{std::move(onError)} {
        subscribeCurrent();
    }

    /// @brief Flags this session as gone, then unsubscribes the current step.
    ///
    /// `unsubscribe()` only removes the sink from the handler's map; it does
    /// not guarantee a callback already copied out of that map (in flight on
    /// another thread when this runs) won't still execute afterward. `_alive`
    /// is a separate, independently-lifetimed flag the installed callbacks
    /// check *before* touching `this`, so a callback that is still in flight
    /// when this destructor runs sees it cleared and returns instead of
    /// touching a partially- or fully-destroyed object.
    ~FlowSession() {
        _alive->store(false, std::memory_order_release);
        unsubscribeCurrent();
    }

    FlowSession(const FlowSession&) = delete;
    FlowSession& operator=(const FlowSession&) = delete;
    FlowSession(FlowSession&&) = delete;
    FlowSession& operator=(FlowSession&&) = delete;

    /// @brief Sets one field of the current step's draft and forwards it to
    ///        the handler's ordinary `set<>` (auto-fires when the step's
    ///        `ActionValidator` is ready, exactly as a standalone form).
    /// @tparam FieldPtr Pointer-to-data-member of the current step's action struct.
    /// @param value New value for the field.
    /// @throws std::logic_error if @p FieldPtr's action is not the current step.
    template <auto FieldPtr>
    void set(typename ::morph::bridge::detail::MemberPointerTraits<decltype(FieldPtr)>::ValueType value) {
        using A = typename ::morph::bridge::detail::MemberPointerTraits<decltype(FieldPtr)>::ClassType;
        static_assert((std::is_same_v<A, Steps> || ...),
                      "FlowSession::set<>: field's action is not a step of this flow");
        if (::morph::model::ActionTraits<A>::typeId() != currentActionType()) {
            throw std::logic_error{"FlowSession::set<>: field belongs to an action that is not the current step"};
        }
        {
            std::scoped_lock const lock{_mtx};
            std::get<A>(_drafts).*FieldPtr = value;
        }
        _handler.template set<FieldPtr>(std::move(value));
    }

    /// @brief Moves to the next step, if the current step has already produced
    ///        a successful result (a not-ready step does not advance).
    /// @return `true` if the flow advanced, `false` if the current step is not
    ///         ready or the flow is already `finished()`.
    bool advance() {
        bool ready = false;
        {
            std::scoped_lock const lock{_mtx};
            ready = _currentReady;
        }
        if (!ready || finished()) {
            return false;
        }
        unsubscribeCurrent();
        ++_index;
        {
            std::scoped_lock const lock{_mtx};
            _currentReady = false;
        }
        if (!finished()) {
            subscribeCurrent();
        }
        return true;
    }

    /// @brief Returns to the previous step. Its draft (and the handler's own
    ///        per-action draft) were never reset, so its entered values are
    ///        intact.
    /// @return `true` if the flow moved back, `false` if already at step 0.
    bool back() {
        if (_index == 0) {
            return false;
        }
        unsubscribeCurrent();
        --_index;
        {
            std::scoped_lock const lock{_mtx};
            _currentReady = true;  // this step already produced a result once, or it could not have been left
        }
        subscribeCurrent();
        return true;
    }

    /// @brief Whether the flow has advanced past the last step.
    /// @return `true` once `advance()` has been called successfully on the last step.
    [[nodiscard]] bool finished() const noexcept { return _index >= sizeof...(Steps); }

    /// @brief Whether the current step already has a captured, successful result.
    /// @return `true` if `advance()` would move the flow forward right now.
    [[nodiscard]] bool ready() const noexcept {
        std::scoped_lock const lock{_mtx};
        return _currentReady;
    }

    /// @brief The current 0-based step position.
    /// @return `sizeof...(Steps)` once `finished()`.
    [[nodiscard]] std::size_t currentIndex() const noexcept { return _index; }

    /// @brief The number of steps in this flow.
    /// @return `sizeof...(Steps)`.
    [[nodiscard]] static constexpr std::size_t stepCount() noexcept { return sizeof...(Steps); }

    /// @brief The registered type-id of the current step's action.
    /// @return Empty when `finished()` (no current step).
    [[nodiscard]] std::string_view currentActionType() const noexcept {
        std::string_view id{};
        detail::forStep<Steps...>(_index, [&id]<typename A> { id = ::morph::model::ActionTraits<A>::typeId(); });
        return id;
    }

    /// @brief Looks up a value captured from an earlier step's submitted
    ///        draft or result.
    /// @param path `"<ActionTypeId>.<field>"`, matching a wizard step's
    ///             declared `Bind::path()`.
    /// @return The field's JSON-encoded value, or `std::nullopt` if @p path
    ///         was never captured (the step never fired, or never had that field).
    [[nodiscard]] std::optional<std::string> resolved(std::string_view path) const {
        std::scoped_lock const lock{_mtx};
        auto iter = _resolvedValues.find(std::string{path});
        if (iter == _resolvedValues.end()) {
            return std::nullopt;
        }
        return iter->second;
    }

private:
    template <typename A>
    void captureResult(const typename ::morph::model::ActionTraits<A>::Result& result) {
        std::scoped_lock const lock{_mtx};
        auto const typeId = ::morph::model::ActionTraits<A>::typeId();
        auto record = [&](const auto& value) {
            ::morph::forms::detail::forEachNamedMember(
                value, [&]<std::size_t I>(std::string_view name, const auto& member) {
                    static_cast<void>(I);
                    std::string json;
                    if (!glz::write_json(member, json)) {
                        _resolvedValues[std::string{typeId} + "." + std::string{name}] = std::move(json);
                    }
                });
        };
        record(std::get<A>(_drafts));  // submitted draft fields first...
        record(result);                // ...result fields win on name collision
        _currentReady = true;
    }

    /// @brief Installs the result/error sinks for step @p A.
    ///
    /// Both closures capture `_alive` (a copy of the `shared_ptr`, so it
    /// outlives `this` if the two race) and check it before touching
    /// anything on `this` — see `~FlowSession()`'s doc comment for why
    /// `unsubscribe()` alone is not enough.
    template <typename A>
    void installSubscription() {
        auto alive = _alive;
        _handler.template subscribe<A>(
            [this, alive](typename ::morph::model::ActionTraits<A>::Result result) {
                if (!alive->load(std::memory_order_acquire)) {
                    return;
                }
                this->template captureResult<A>(result);
            },
            [this, alive](std::exception_ptr err) {
                if (!alive->load(std::memory_order_acquire)) {
                    return;
                }
                {
                    std::scoped_lock const lock{_mtx};
                    _currentReady = false;
                }
                if (_onError) {
                    _onError(err);
                } else {
                    logUnhandledError(::morph::model::ActionTraits<A>::typeId(), err);
                }
            });
    }

    static void logUnhandledError(std::string_view typeId, const std::exception_ptr& err) {
        try {
            std::rethrow_exception(err);
        } catch (const std::exception& exc) {
            ::morph::log::logError(std::string{"[flow:"} + std::string{typeId} +
                                   "] unhandled exception: " + exc.what());
        } catch (...) {
            ::morph::log::logError(std::string{"[flow:"} + std::string{typeId} + "] unhandled unknown exception");
        }
    }

    void subscribeCurrent() {
        detail::forStep<Steps...>(_index, [this]<typename A> { this->template installSubscription<A>(); });
    }

    void unsubscribeCurrent() {
        detail::forStep<Steps...>(_index, [this]<typename A> { _handler.template unsubscribe<A>(); });
    }

    ::morph::bridge::BridgeHandler<Model>& _handler;
    std::function<void(std::exception_ptr)> _onError;
    // _index/_handler/_onError are touched only from the thread that owns
    // this FlowSession (constructor, destructor, set/advance/back); guarded
    // separately below is the state a subscription's result/error callback
    // also touches, which runs on whatever thread/executor resolves the
    // underlying BridgeHandler completion -- not necessarily this same
    // thread. See docs/spec/core/bridge.md's executor/callback model.
    std::size_t _index{0};
    mutable std::mutex _mtx;
    std::tuple<Steps...> _drafts{};
    bool _currentReady{false};
    std::unordered_map<std::string, std::string> _resolvedValues;
    // Outlives `this`: a late callback (see installSubscription) checks this
    // before touching anything else, so it survives even if `this` is
    // already gone by the time it runs.
    std::shared_ptr<std::atomic<bool>> _alive = std::make_shared<std::atomic<bool>>(true);
};

}  // namespace morph::flows

/// @brief Specialises `morph::flows::WizardTraits<W>` with the string type-id @p NAME.
///
/// Metadata only: unlike `BRIDGE_REGISTER_ACTION`, this performs no
/// static-init registration into any dispatch registry — a wizard is never
/// itself executed, only its steps' already-registered actions are (see
/// docs/spec/forms/workflows_navigation.md).
/// @param W    Concrete `morph::flows::Wizard<...>` type.
/// @param NAME String literal used as the wizard's type-id.
#define BRIDGE_REGISTER_WIZARD(W, NAME)                                      \
    template <>                                                              \
    struct morph::flows::WizardTraits<W> {                                   \
        static constexpr std::string_view typeId() noexcept { return NAME; } \
    };

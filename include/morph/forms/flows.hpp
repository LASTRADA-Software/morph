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
/// step through the ordinary `BridgeHandler<Model>::execute<A>()` dispatch
/// path (bridge.hpp) and tracks the resolved values a caller (or a renderer)
/// reads to prefill a later step.
///
/// This is purely additive metadata and sequencing over the existing
/// dispatch path: no new wire format, no new execution mode. See
/// docs/spec/forms/workflows_navigation.md.

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

#include "../attributes.hpp"
#include "../core/bridge.hpp"
#include "../core/callback_scope.hpp"
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
/// Each step is fired through the handler's ordinary one-shot dispatch
/// (`BridgeHandler::execute<A>()` → `Completion<R>`, bridge.hpp) — a
/// `FlowSession` contributes only sequencing (`advance`/`back`), the per-step
/// drafts and their readiness gate, and a resolved-values map callers use to
/// prefill a later step from an earlier one's submitted fields or result. The
/// handler holds no draft on a flow's behalf: nothing here goes through
/// handler-side draft machinery.
///
/// `Steps...` must be pairwise distinct action types: this
/// session's drafts live in a `std::tuple<Steps...>` addressed by type
/// (`std::get<A>(_drafts)`), and `std::get<T>` is ill-formed when `T` occurs
/// more than once — which is what the `AllDistinct<Steps...>` assert below
/// enforces.
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
    ///                which is how `BridgeHandler` itself reports a failure no
    ///                caller is holding a `Completion` for (bridge.hpp). A
    ///                handler offers no error callback to inherit a default
    ///                from: `subscribe<R>` is notified of results only, and an
    ///                `execute()` failure reaches the caller through the
    ///                `Completion` it returned.
    ///                Stored and invoked for this session's whole lifetime, so
    ///                anything the callable refers to must outlive the session.
    explicit FlowSession(::morph::bridge::BridgeHandler<Model>& handler MORPH_LIFETIMEBOUND,
                         std::function<void(std::exception_ptr)> onError MORPH_LIFETIMEBOUND = nullptr)
        : _handler{handler}, _onError{std::move(onError)} {
        beginStep();
    }

    /// @brief Refuses every callback this session installed.
    ///
    /// There is nothing to detach. A step's callbacks are `.then`/`.onError`
    /// continuations on the `Completion` that `BridgeHandler::execute<A>()`
    /// returned — already owned by the in-flight dispatch, not held in a map
    /// this session could remove itself from, and possibly running on another
    /// thread the moment this destructor starts. `_callbacks` gates every
    /// installed callback on a token the callback checks *before* touching
    /// `this`, so a callback still in flight when this destructor runs is
    /// refused instead of touching a partially- or fully-destroyed object.
    ///
    /// `requestStop()` is called explicitly rather than left to the member's own
    /// destruction, even though `_callbacks` is declared last: members are
    /// destroyed only *after* the destructor body, so anything this body does
    /// that can pump an event loop (a `sendSync`-style blocking call) would
    /// otherwise deliver into a half-dead session. This is the "teardown that
    /// pumps" escape hatch docs/spec/core/callback_scope.md documents.
    ~FlowSession() { _callbacks.requestStop(); }

    FlowSession(const FlowSession&) = delete;
    FlowSession& operator=(const FlowSession&) = delete;
    FlowSession(FlowSession&&) = delete;
    FlowSession& operator=(FlowSession&&) = delete;

    /// @brief Sets one field of the current step's draft and dispatches the
    ///        step as soon as its `ActionValidator` reports the draft ready.
    ///
    /// The draft is this session's own (`std::get<A>(_drafts)`), the readiness
    /// check is made here, and a ready draft is fired by this session's
    /// `fireStep<A>` through `BridgeHandler::execute<A>()`. Nothing is
    /// forwarded to any draft the handler keeps.
    ///
    /// @par No in-flight coalescing
    /// **Every `set<>` that leaves the draft ready dispatches, including one
    /// made while an earlier dispatch is still in flight.** Keystroke-rate
    /// calls on an already-complete draft therefore produce one request each,
    /// and their results arrive in whatever order the backend answers them —
    /// the last reply to land wins, which need not be the last call made. An
    /// earlier handler-side draft did collapse patches arriving during a
    /// flight; nothing does now, here or on the standalone-form path (the
    /// shipped renderer's `DynamicForm` calls `submitIfValid` on every change
    /// that leaves the form ready, with no in-flight suppression either). A
    /// caller that wants one request per pause throttles or debounces on its
    /// own side.
    ///
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
        A draft{};
        std::size_t stepIndex = 0;
        {
            std::scoped_lock const lock{_mtx};
            std::get<A>(_drafts).*FieldPtr = std::move(value);
            draft = std::get<A>(_drafts);
            stepIndex = _activeStep;
        }
        // The readiness gate that used to live in the handler's draft machinery
        // lives here now: the flow already owns the draft, so it can decide when
        // the step is complete and dispatch it itself. The absence of in-flight
        // coalescing that follows from dispatching here is published in this
        // function's @brief, not left to this comment to discover.
        if (::morph::model::ActionValidator<A>::ready(draft)) {
            fireStep<A>(std::move(draft), stepIndex);
        }
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
        ++_index;
        {
            std::scoped_lock const lock{_mtx};
            _currentReady = false;
            // Retire the old step's callbacks here, not only in beginStep():
            // on the last step this advance finishes the flow and no further
            // beginStep() follows, so nothing else would move the marker and a
            // late reply could still mark a finished flow ready.
            _activeStep = _index;
        }
        if (!finished()) {
            beginStep();
        }
        return true;
    }

    /// @brief Returns to the previous step. Its draft — this session's own
    ///        `std::get<A>(_drafts)` slot, the only draft in play — was never
    ///        reset, so its entered values are intact.
    /// @return `true` if the flow moved back, `false` if already at step 0.
    bool back() {
        if (_index == 0) {
            return false;
        }
        --_index;
        {
            std::scoped_lock const lock{_mtx};
            _currentReady = true;  // this step already produced a result once, or it could not have been left
            _activeStep = _index;
        }
        beginStep();
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
    void captureResult(const ::morph::model::ActionTraits<A>::Result& result, std::size_t stepIndex) {
        std::scoped_lock const lock{_mtx};
        if (stepIndex != _activeStep) {
            // A reply for a step the flow has already left. A dispatch in
            // flight cannot be recalled -- its `.then` continuation is already
            // owned by the completion (see ~FlowSession) -- so a step re-fired
            // just before advance() can still land here afterwards. Applying it
            // would do real damage twice over: `_resolvedValues` would be
            // overwritten with a superseded reply that advance() never saw, and
            // `_currentReady = true` below is not step-keyed, so it would mark
            // the *new* step ready before anything was entered into it --
            // letting the next advance() skip a step outright.
            return;
        }
        auto const typeId = ::morph::model::ActionTraits<A>::typeId();
        auto record = [&](const auto& value) {
            ::morph::forms::detail::forEachNamedMember(
                value, [&]<std::size_t I>(std::string_view name, const auto& member) {
                    static_cast<void>(I);
                    std::string json;
                    // The `!write_json(...)` guard's false arm (write failure)
                    // is not exercised by this file's own test suite: every
                    // step action's fields are plain, well-formed data glaze's
                    // JSON writer cannot fail on, matching the same
                    // "untestable line" already accepted for forms.hpp's own
                    // `write_json(dom)` call in `schemaJson`.
                    if (!glz::write_json(member, json)) {
                        _resolvedValues[std::string{typeId} + "." + std::string{name}] = std::move(json);
                    }
                });
        };
        record(std::get<A>(_drafts));  // submitted draft fields first...
        record(result);                // ...result fields win on name collision
        _currentReady = true;
    }

    /// @brief Dispatches step @p A's completed draft and routes its outcome.
    ///
    /// Both closures are gated on `_callbacks`, so neither touches anything on
    /// `this` once the flow has been stopped or destroyed — a completion can
    /// still resolve after the flow is gone.
    /// @tparam A Step action type.
    /// @param draft     The completed action to execute.
    /// @param stepIndex Index of the step this dispatch belongs to.
    template <typename A>
    void fireStep(A draft, std::size_t stepIndex) {
        _handler.execute(std::move(draft))
            .then(_callbacks,
                  [this, stepIndex](::morph::model::ActionTraits<A>::Result result) {
                      this->template captureResult<A>(result, stepIndex);
                  })
            .onError(_callbacks, [this, stepIndex](const std::exception_ptr& err) {
                {
                    // Only clear readiness while this really is the current
                    // step. A late failure from a step already left behind used
                    // to clear `_currentReady` for whichever step the flow had
                    // moved on to — un-readying a step that had legitimately
                    // completed. The error is still reported below either way:
                    // the action did fail, and the host wants to know.
                    std::scoped_lock const lock{_mtx};
                    if (stepIndex == _activeStep) {
                        _currentReady = false;
                    }
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

    /// @brief Publishes which step is now current.
    ///
    /// Read back under the same mutex by every dispatch callback, so one that
    /// resolves after the flow has moved on recognises itself as stale.
    void beginStep() {
        std::scoped_lock const lock{_mtx};
        _activeStep = _index;
    }

    ::morph::bridge::BridgeHandler<Model>& _handler;
    std::function<void(std::exception_ptr)> _onError;
    // _index/_handler/_onError are touched only from the thread that owns
    // this FlowSession (constructor, destructor, set/advance/back); guarded
    // separately below is the state a step's result/error continuation also
    // touches, which runs on whatever thread/executor resolves the underlying
    // BridgeHandler completion -- not necessarily this same thread. See
    // docs/spec/core/bridge.md's executor/callback model.
    std::size_t _index{0};
    mutable std::mutex _mtx;
    std::tuple<Steps...> _drafts{};
    // The step whose continuations are the ones the flow still recognises,
    // mirrored under _mtx so a callback running on the resolving executor's
    // thread can tell whether it still speaks for the current step. `_index` itself is only
    // safe to read from the owning thread.
    std::size_t _activeStep{0};
    bool _currentReady{false};
    std::unordered_map<std::string, std::string> _resolvedValues;
    // Declared last, so it is the first member destroyed: every gated callback
    // is refused before the fields it would have touched are torn down. See
    // docs/spec/core/callback_scope.md.
    ::morph::async::CallbackScope _callbacks;
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
// clang-format off -- public macro surface: hand-aligned on purpose.
// These definitions are the framework's documented API; contributors read them
// as reference, and the continuation backslashes line up so the body is legible
// as a block. Leaving them to the formatter means any unrelated edit nearby
// re-wraps the whole definition, and in one case it broke a token-paste
// invocation apart. Freeze them; realign by hand if a body changes.
#define BRIDGE_REGISTER_WIZARD(W, NAME)                                      \
    template <>                                                              \
    struct morph::flows::WizardTraits<W> {                                   \
        static constexpr std::string_view typeId() noexcept { return NAME; } \
    };
// clang-format on

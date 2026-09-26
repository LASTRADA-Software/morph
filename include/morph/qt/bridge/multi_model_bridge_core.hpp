// SPDX-License-Identifier: Apache-2.0

#pragma once

/// @file
/// Multi-model sibling of `morph::qt::bridge::GenericModelBridgeCore`: owns
/// (or composes over) one `GenericModelBridgeCore<Model, Sharing>` per `Model`
/// in the pack and routes a string action-type id to whichever one serves it,
/// so an app whose forms span more than one registered model does not have to
/// hand-write that routing table itself. See
/// `morph::qt::forms::MultiModelFormsControllerCore`, which specialises this
/// core for the shipped forms renderer exactly as `FormsControllerCore`
/// specialises `GenericModelBridgeCore`, for the reference shape.
///
/// The routing is derived, not declared: `GenericModelBridgeCore::servesAction`
/// answers "is this action registered for this Model" directly from
/// `ActionExecuteRegistry` -- the same source of truth `BRIDGE_REGISTER_ACTION`
/// already populates -- so nothing here re-states which action belongs to
/// which model. A hand-written `actionType -> Model` table would just be a
/// second, driftable copy of that registration.
///
/// Composes one `GenericModelBridgeCore` per `Model` rather than duplicating
/// its `executeJson`-plus-completion-wiring body: routing is the only thing
/// this class adds, and it is a thin layer over the single-model core rather
/// than a parallel reimplementation of it.

#include <cassert>
#include <cstddef>
#include <morph/core/bridge.hpp>
#include <morph/core/executor.hpp>
#include <morph/qt/bridge/detail/owned_local_bridge.hpp>
#include <morph/qt/bridge/generic_model_bridge_core.hpp>
#include <optional>
#include <stdexcept>
#include <string>
#include <tuple>
#include <utility>

namespace morph::qt::bridge {

/// @brief Owns, or composes over, one `GenericModelBridgeCore<Model, Sharing>`
///        per `Model` in the pack, and routes a string action-type id to
///        whichever one serves it.
///
/// @tparam Sharing The handlers' shared sharing policy -- `morph::bridge::NoSharing`
///                 or `morph::bridge::AllowShared`; see `morph::bridge::BridgeHandler`.
///                 Named first, unlike `GenericModelBridgeCore`'s `<Model, Sharing = NoSharing>`
///                 order: a template parameter pack must be the last template
///                 parameter, so with @p Model variadic, @p Sharing cannot
///                 also carry a usable default -- a default there would only
///                 ever apply when @p Model is empty, which the static
///                 assertion below already forbids. Every instantiation must
///                 name it explicitly.
/// @tparam Model   Two or more registered model types (`BRIDGE_REGISTER_MODEL`),
///                 in the order their cores are tried when routing an
///                 action-type id. A single model has no routing to do --
///                 use `GenericModelBridgeCore<Model, Sharing>` instead.
template <typename Sharing, typename... Model>
class MultiModelBridgeCore {
    static_assert(sizeof...(Model) >= 2,
                  "MultiModelBridgeCore needs at least two Model types; a single model has no "
                  "routing to do -- use GenericModelBridgeCore<Model, Sharing> instead.");

public:
    /// @brief Constructs the core with its own private, always-local `Bridge`
    ///        (`ThreadPoolExecutor` + `QtExecutor` + `LocalBackend`), shared
    ///        by every model's core.
    ///
    /// Use the `(Bridge&, IExecutor*)` overload instead when the app already
    /// has a `Bridge` (remote/socket mode, or one shared across multiple
    /// presenters) that this core should compose over rather than duplicate.
    /// Each element composes over that one shared `Bridge` via its own
    /// `(Bridge&, IExecutor*)` constructor -- never its owning default
    /// constructor, which would otherwise give every `Model` an independent,
    /// unshared `Bridge` of its own.
    MultiModelBridgeCore() : _owned{std::in_place} { emplaceAll(_owned->bridge, &_owned->gui); }

    /// @brief Constructs the core over a caller-supplied `Bridge`/executor,
    ///        instead of building a private, always-local one.
    ///
    /// Every model's core registers on @p bridge exactly as the owning
    /// constructor's internal ones do, so `execute` dispatches through
    /// whatever backend @p bridge currently has installed -- including a
    /// backend @p bridge switches to later via `Bridge::switchBackend`, since
    /// every registered handler re-registers itself automatically.
    ///
    /// @param bridge  The bridge every model's core registers on. Must
    ///                outlive this core.
    /// @param guiExec Executor used to deliver `Completion` callbacks (e.g. a
    ///                `QtExecutor` for the GUI thread). Must outlive this
    ///                core.
    MultiModelBridgeCore(::morph::bridge::Bridge& bridge, ::morph::exec::IExecutor* guiExec) {
        emplaceAll(bridge, guiExec);
    }

    /// @brief Routes @p bodyJson as @p actionType's body to whichever
    ///        `Model` in the pack serves it, invoking @p onReply / @p onError
    ///        on the GUI thread once the reply (or the routing failure)
    ///        arrives.
    ///
    /// Models are tried in the order the pack declares them; the first whose
    /// `GenericModelBridgeCore::servesAction` recognises @p actionType
    /// dispatches it, via that core's own `execute`. If none do, @p onError
    /// is invoked directly (no throw, no round trip) with a
    /// `std::runtime_error` naming the unrouted action, on the same
    /// synchronous call frame -- exactly the shape a caller's own
    /// `try`/`catch`-around-a-throwing-router used to produce, without the
    /// throw.
    /// @tparam OnReply Callable invoked with the result JSON (`std::string`) on success.
    /// @tparam OnError Callable invoked with the `std::exception_ptr` on failure.
    /// @param actionType Registered action type id.
    /// @param bodyJson   Fully-assembled JSON body for the action.
    /// @param onReply    Success callback.
    /// @param onError    Failure callback.
    template <typename OnReply, typename OnError>
    void execute(const std::string& actionType, const std::string& bodyJson, OnReply onReply, OnError onError) {
        // Debug-only: action ids are supposed to be unique per action struct,
        // so two Models in the same pack claiming the same actionType is a
        // configuration bug, not a case to route silently. Asserted rather
        // than always checked -- it costs one servesAction() probe per Model
        // in the pack beyond the one execute() needs anyway -- so a
        // misconfiguration is loud in a debug build instead of silently
        // resolving to whichever Model happens to be declared first.
        assert(countMatches(actionType) <= 1 &&
               "MultiModelBridgeCore: actionType is registered on more than one Model in this "
               "pack; routing would silently resolve to whichever is declared first");
        const bool routed = std::apply(
            [&](auto&... core) { return (tryOne(*core, actionType, bodyJson, onReply, onError) || ...); }, _cores);
        if (!routed) {
            onError(std::make_exception_ptr(
                std::runtime_error{"no model in this client serves action '" + actionType + "'"}));
        }
    }

private:
    void emplaceAll(::morph::bridge::Bridge& bridge, ::morph::exec::IExecutor* guiExec) {
        std::apply([&](auto&... core) { (core.emplace(bridge, guiExec), ...); }, _cores);
    }

    [[nodiscard]] std::size_t countMatches(const std::string& actionType) const {
        return std::apply(
            [&](const auto&... core) { return (static_cast<std::size_t>(core->servesAction(actionType)) + ...); },
            _cores);
    }

    /// @brief Dispatches through @p core if it serves @p actionType, moving
    ///        @p onReply / @p onError into its own completion wiring.
    ///
    /// A free function template over `Core` rather than a method, since it
    /// touches no member of this class -- it is the one place @p onReply /
    /// @p onError are actually moved, and that must happen at most once
    /// across the whole pack, which the `||` fold in `execute()` guarantees
    /// by short-circuiting on the first `true`.
    template <typename Core, typename OnReply, typename OnError>
    static bool tryOne(Core& core, const std::string& actionType, const std::string& bodyJson, OnReply& onReply,
                       OnError& onError) {
        if (!core.servesAction(actionType)) {
            return false;
        }
        core.execute(actionType, bodyJson, std::move(onReply), std::move(onError));
        return true;
    }

    // Declaration order matters for destruction: _cores must tear down
    // before _owned (its bridge and executor, when this core owns them), so
    // _owned is declared first and _cores after it -- same reasoning as
    // GenericModelBridgeCore. Each element is wrapped in optional because
    // GenericModelBridgeCore is neither copyable nor movable (it holds a
    // BridgeHandler, whose deleted copy constructor suppresses the implicit
    // move too), so it cannot be constructed as a tuple-constructor argument
    // the way a movable type could; emplaceAll() constructs each one in place
    // instead, right after _owned (when engaged) is available to hand it a
    // Bridge&. Every element is engaged for this object's entire lifetime
    // after construction -- optional is used here purely as
    // deferred-construction storage, not to express an absent core.
    std::optional<detail::OwnedLocalBridge> _owned;
    std::tuple<std::optional<GenericModelBridgeCore<Model, Sharing>>...> _cores;
};

}  // namespace morph::qt::bridge

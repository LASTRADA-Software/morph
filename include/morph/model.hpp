// SPDX-License-Identifier: Apache-2.0

#pragma once
#include <memory>
#include <stdexcept>
#include <typeindex>
#include <typeinfo>

#include "strand.hpp"

namespace morph::model::detail {

template <typename Model>
struct ModelHolder;

// ── Backend-change notification interface ─────────────────────────────────────

/// @brief Optional interface for models that need to react to backend switches.
///
/// Implemented automatically by `ModelHolder<M>` when `M` declares
/// `void onBackendChanged()`. `Bridge::switchBackend()` discovers this
/// capability via `dynamic_cast` without coupling to the concrete type.
struct IBackendChangedSink {
    virtual ~IBackendChangedSink() = default;

    /// @brief Called after the bridge has switched to a new backend and
    ///        re-registered all handlers on it.
    virtual void onBackendChanged() = 0;
};

// ── Concept ───────────────────────────────────────────────────────────────────

/// @brief Concept satisfied by model types that expose `void onBackendChanged()`.
template <typename M>
concept BackendChangedNotifiable = requires(M& model) {
    { model.onBackendChanged() } -> std::same_as<void>;
};

// ── Conditional mixin ─────────────────────────────────────────────────────────

/// @brief Empty base when `M` does not declare `onBackendChanged()`.
template <typename M, bool = BackendChangedNotifiable<M>>
struct BackendChangedMixin {};

/// @brief Specialisation that wires `IBackendChangedSink` to `M::onBackendChanged()`.
template <typename M>
struct BackendChangedMixin<M, true> : IBackendChangedSink {
    /// @brief Forwards the virtual call to the concrete model instance.
    void onBackendChanged() override { static_cast<ModelHolder<M>*>(this)->model.onBackendChanged(); }
};

// ── Core type-erasure ─────────────────────────────────────────────────────────

/// @brief Type-erased wrapper that owns a single model instance.
///
/// Used internally by backends to store heterogeneous models in a single map.
struct IModelHolder {
    virtual ~IModelHolder() = default;

    /// @brief Returns the `std::type_index` of the concrete model type.
    [[nodiscard]] virtual std::type_index type() const noexcept = 0;

    /// @brief Down-casts to a concrete `Model` reference.
    ///
    /// @tparam Model The expected concrete type.
    /// @throws std::bad_cast if the stored type is not `Model`.
    template <typename Model>
    Model& into() {
        if (type() != std::type_index(typeid(Model))) {
            throw std::bad_cast{};
        }
        return static_cast<ModelHolder<Model>*>(this)->model;
    }
};

/// @brief Concrete holder that stores a `Model` by value.
///
/// Inherits `BackendChangedMixin<Model>` so that backend-change notifications
/// are forwarded automatically when `Model` opts in.
template <typename Model>
struct ModelHolder : IModelHolder, BackendChangedMixin<Model> {
    /// @brief The owned model instance.
    Model model;

    /// @brief Constructs the model by forwarding all arguments.
    template <typename... Args>
    explicit ModelHolder(Args&&... args) : model{std::forward<Args>(args)...} {}

    /// @brief Returns `typeid(Model)` wrapped in a `std::type_index`.
    [[nodiscard]] std::type_index type() const noexcept override { return typeid(Model); }
};

/// @brief Factory that creates default-constructed `ModelHolder<Model>` instances.
class ModelFactory {
public:
    /// @brief Creates a new `ModelHolder<Model>` on the heap.
    /// @tparam Model The model type to instantiate.
    /// @return Owning pointer to the new holder.
    template <typename Model>
    static std::unique_ptr<IModelHolder> create() {
        return std::make_unique<ModelHolder<Model>>();
    }
};

}  // namespace morph::model::detail

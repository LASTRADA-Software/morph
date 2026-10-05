// SPDX-License-Identifier: Apache-2.0

#pragma once
#include <cstddef>
#include <memory>
#include <type_traits>
#include <utility>
#include <vector>

#include "../attributes.hpp"
#include "runtime.hpp"
#include "signal.hpp"

/// @file
/// @brief `morph::reactive::Scope`: owns reactive nodes and objects, and destroys them in reverse.
///
/// Specified in `docs/spec/reactive/signals.md`.

namespace morph::reactive {

/// @brief Owns effects, computeds and adopted objects; destroys them in reverse creation order.
///
/// A mounted view, a Switch case and a ForEach row each own one, so tearing one down stops every
/// binding it made before anything those bindings point at goes away.
///
/// An Effect the scope owns may clear or destroy the scope from its own body (a remount): the graph
/// keeps the running body alive, and the body must not touch the scope or anything it owned afterwards.
/// That holds for an Effect's first run too, which happens inside `effect()` before the scope stores it:
/// - If the first run clears the scope, the new Effect is stored after the clear and stays.
/// - If it destroys the scope, the new Effect is destroyed at once, since a dead scope owns nothing, and
///   the reference `effect()` returns dangles (as does every reference to an object the scope owned).
///   That destructor runs after the scope is gone, so it must not use the scope.
///
/// Only the nodes check the owner (`detail::site::kOffOwner`); the scope's own list is unguarded. Using a
/// scope off its Runtime's owner is a misuse that is not reported.
///
/// An owned object's destructor may call back into the scope. It leaves the list before it is destroyed.
/// A destructor that keeps calling `make()` while the scope is clearing never lets `clear()` finish.
class Scope {
public:
    /// @param runtime The runtime the nodes made here belong to. Borrowed: it must outlive the scope.
    explicit Scope(Runtime& runtime MORPH_LIFETIMEBOUND) noexcept : _rt{&runtime} {}

    /// @brief Destroys everything owned, newest first, and tells every `make()` still constructing that
    ///        the scope is gone.
    ~Scope() {
        for (Construction* frame = _constructing; frame != nullptr; frame = frame->previous) {
            frame->scopeDied = true;
        }
        clear();
    }
    Scope(Scope const&) = delete;
    Scope& operator=(Scope const&) = delete;
    Scope(Scope&&) = delete;
    Scope& operator=(Scope&&) = delete;

    /// @brief Constructs a `T` from @p args and owns it.
    /// @tparam T The object type.
    /// @tparam Args Constructor argument types.
    /// @param args Forwarded to `T`'s constructor.
    /// @return The new object, alive until the scope is cleared or destroyed.
    template <typename T, typename... Args>
    T& make(Args&&... args) {
        // The constructor may run user code (an Effect's first run) that destroys this scope. The frame
        // is how the destructor tells this call, and every make enclosing it, so none touches `this`
        // afterwards.
        Construction frame{.previous = _constructing};
        _constructing = &frame;
        std::unique_ptr<T> made;
        try {
            made = std::make_unique<T>(std::forward<Args>(args)...);
        } catch (...) {
            leave(frame);
            throw;
        }
        if (frame.scopeDied) {
            // A dead scope owns nothing: the object goes now, and the reference is already dangling.
            T& dead = *made;
            made = nullptr;
            return dead;  // NOLINT(clang-analyzer-cplusplus.NewDelete): documented dangling return
        }
        leave(frame);
        return adopt(std::move(made));
    }

    /// @brief Takes ownership of @p owned.
    /// @tparam T The object type.
    /// @param owned The object; must not be null.
    /// @return The object, alive until the scope is cleared or destroyed.
    template <typename T>
    T& adopt(std::unique_ptr<T> owned) {
        T& ref = *owned;
        // The slot is made empty first, and only then given the pointer: the emplace may throw while
        // @p owned still holds the object, and `reset` cannot throw.
        _owned.emplace_back(nullptr, [](void* ptr) noexcept { std::default_delete<T>{}(static_cast<T*>(ptr)); });
        _owned.back().reset(owned.release());
        return ref;
    }

    /// @brief Makes an Effect owned by this scope.
    /// @tparam F A callable taking no arguments.
    /// @param fn The effect body.
    /// @return The Effect.
    template <typename F>
    Effect& effect(F&& fn) {
        return make<Effect>(*_rt, std::forward<F>(fn));
    }

    /// @brief Makes a Computed owned by this scope; its type is what @p fn returns.
    /// @tparam F A callable taking no arguments.
    /// @param fn The computation.
    /// @return The Computed.
    template <typename F>
    auto& computed(F&& fn) {
        using Value = std::remove_cvref_t<std::invoke_result_t<F&>>;
        return make<Computed<Value>>(*_rt, std::forward<F>(fn));
    }

    /// @brief Destroys everything owned, newest first.
    ///
    /// Each object leaves the list before it is destroyed, so a destructor or a running Effect body
    /// that calls back into the scope (`clear()`, `make()`) sees a consistent list.
    void clear() noexcept {
        while (!_owned.empty()) {
            auto const last = std::move(_owned.back());
            _owned.pop_back();
        }
    }

    /// @brief How many objects the scope owns.
    /// @return The count.
    [[nodiscard]] std::size_t size() const noexcept { return _owned.size(); }

    /// @brief The runtime.
    /// @return The runtime passed at construction.
    [[nodiscard]] Runtime& runtime() const noexcept { return *_rt; }

private:
    /// @brief One `make()` in progress; frames nest in call order.
    struct Construction {
        Construction* previous = nullptr;  ///< The enclosing `make()`, or null.
        bool scopeDied = false;            ///< Set by the destructor while this `make()` is still constructing.
    };

    /// @brief Ends @p frame, unless the scope died meanwhile and `this` is no longer valid.
    /// @param frame The innermost frame.
    void leave(Construction const& frame) noexcept {
        if (!frame.scopeDied) {
            _constructing = frame.previous;
        }
    }

    Runtime* _rt;
    Construction* _constructing = nullptr;
    std::vector<std::unique_ptr<void, void (*)(void*)>> _owned;
};

}  // namespace morph::reactive

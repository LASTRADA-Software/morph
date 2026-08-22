// SPDX-License-Identifier: Apache-2.0

#pragma once
#include <concepts>
#include <memory>
#include <utility>

namespace morph::async {

/// @brief Opt-in base giving a receiver a lifetime token `Completion` can
///        guard a callback on.
///
/// A `Completion<T>` always resolves *through an executor* — even a local
/// backend's immediate resolution is posted rather than delivered inline — so
/// the object that registered a callback can always be destroyed before that
/// callback runs. Today every receiver that cares reimplements the same
/// two-part ritual by hand: declare a `std::shared_ptr<const void>` member,
/// capture its `weak_ptr` at the call site, and re-check it inside the
/// callback before touching `this`. Five classes in this repository do
/// exactly that across 23 call sites, and two more use `QPointer` for the
/// same hazard.
///
/// Inheriting from `HasLifetime` replaces the ritual with a base class:
///
/// ```cpp
/// class BoardPresenter : public morph::async::HasLifetime {
///     void load() {
///         handler.execute(GetBoard{}).then(this, [this](GetBoardResult r) {
///             // reached only while `this` is alive
///             render(r);
///         });
///     }
/// };
/// ```
///
/// The token is destroyed at the start of `~HasLifetime()`, so a callback
/// that observes it as alive is running while the derived object is still
/// valid *provided* the callback and the destructor do not run concurrently.
/// That holds for the intended use — a receiver destroyed on the same
/// executor its callbacks land on, which is the case for every presenter and
/// bridge in the ladder. A receiver destroyed on a different thread from its
/// callback executor still needs external synchronisation; the guard closes
/// the ordinary "destroyed before the reply arrived" hole, not a genuine data
/// race.
///
/// Deliberately *not* `enable_shared_from_this`: that would force heap
/// allocation and shared ownership on every receiver, and stack-allocated
/// presenters and QML-owned bridges cannot comply.
class HasLifetime {
  public:
    /// @brief Creates a fresh lifetime token for this object.
    HasLifetime() : _token{std::make_shared<char>()} {}

    /// @brief Copy-constructs with a *new* token rather than sharing the source's.
    ///
    /// A token is an identity, not a value: sharing one would let a copy's
    /// destruction silence callbacks bound to the original. The same reasoning
    /// applies to every other special member below, which is why none of them
    /// is defaulted.
    HasLifetime(const HasLifetime&) : _token{std::make_shared<char>()} {}

    /// @brief Copy-assigns, leaving this object's own token untouched.
    /// @return `*this`.
    HasLifetime& operator=(const HasLifetime&) { return *this; }

    /// @brief Move-constructs with a new token; the source keeps its own.
    ///
    /// A moved-from receiver is still a live object, and callbacks already
    /// bound to it must keep firing until it is actually destroyed.
    HasLifetime(HasLifetime&&) noexcept : _token{std::make_shared<char>()} {}

    /// @brief Move-assigns, leaving this object's own token untouched.
    /// @return `*this`.
    HasLifetime& operator=(HasLifetime&&) noexcept { return *this; }

    /// @brief Destroys the token, so every guarded callback becomes a no-op.
    ~HasLifetime() = default;

    /// @brief A weak observer of this object's lifetime.
    /// @return A `weak_ptr` that expires when this object is destroyed.
    [[nodiscard]] std::weak_ptr<const void> lifetimeToken() const noexcept { return _token; }

  private:
    // Not `shared_ptr<const void>` directly: `make_shared<char>` gives one
    // allocation and a non-null control block, and the pointee is never read.
    std::shared_ptr<const char> _token;
};

/// @brief A receiver that can be passed to `Completion::then`/`onError` --
///        satisfied by any type deriving from `HasLifetime`.
///
/// No `@tparam` here on purpose: clang's `-Wdocumentation` does not treat a
/// concept as a template declaration, and the repository builds with
/// `-Werror`.
template <typename Owner>
concept LifetimeBound = std::derived_from<Owner, HasLifetime>;

}  // namespace morph::async

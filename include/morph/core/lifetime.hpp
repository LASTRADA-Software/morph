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
    HasLifetime() : _token{std::make_shared<char>()} {}

    HasLifetime(const HasLifetime&) : _token{std::make_shared<char>()} {}
    HasLifetime& operator=(const HasLifetime&) { return *this; }
    HasLifetime(HasLifetime&&) noexcept : _token{std::make_shared<char>()} {}
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

/// @brief A receiver that can be passed to `Completion::then`/`onError`.
/// @tparam Owner Candidate receiver type.
template <typename Owner>
concept LifetimeBound = std::derived_from<Owner, HasLifetime>;

}  // namespace morph::async

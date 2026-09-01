// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <atomic>
#include <functional>
#include <string>
#include <utility>

/// @file
/// The injectable source of candidate paste ids — the same shape, and for the
/// same reason, as the ladder-wide injectable clock in
/// `examples/common/clock.hpp`.
///
/// `CreatePaste` allocates an id by *guessing*: it mints a candidate
/// `<adjective>-<animal>-<0..999>` and lets the primary key adjudicate,
/// retrying a bounded number of times when the insert collides. Randomness is
/// an ambient dependency like time is, so a test that wants to exercise the
/// retry path has only two options without a seam: occupy a share of the
/// keyspace and *sample* the resulting collision distribution, or nothing.
/// Sampling is what morph#365 was filed about — the case's outcome then
/// depends on `std::random_device`, which no Catch2 `--rng-seed` reaches, so
/// it can neither be reproduced nor pinned.
///
/// A **process-global** provider with a scoped override, rather than a
/// constructor parameter, for exactly the reason `clock.hpp` records for
/// `now()`: `PasteModel` is registered with `BRIDGE_REGISTER_MODEL`, which
/// auto-registers a default-construction factory, so every action dispatch
/// gets a freshly default-constructed instance and there is no call site a
/// test could hand a generator to. Injecting per instance means bypassing
/// that registration in favour of a manual
/// `ModelRegistryFactory::registerModel<Model>(modelId, factory)` call at
/// startup — a switch no rung has made.
///
/// **Production behaviour is the no-override path and is unchanged by this
/// seam.** With no `ScopedPasteIdSource` alive, `nextPasteId()` calls the same
/// `std::random_device`-seeded `std::mt19937_64` generator
/// `src/models/paste_model.cpp` has always used; the only addition on that
/// path is one relaxed atomic load of a null pointer. A shipped server never
/// installs an override — nothing outside test code constructs one.

namespace pastebin {

/// @brief A source of *candidate* paste ids, one per call.
///
/// A source is not asked for a *free* id: it may return an id that is already
/// taken, and `CreatePaste` will retry it against the store exactly as it
/// retries an unlucky random guess. That is what makes a scripted source able
/// to stage a collision sequence exactly.
using PasteIdSource = std::function<std::string()>;

namespace detail {

/// @brief The process-global override slot; `nullptr` means "no override —
///        mint from the built-in random generator".
///
/// A pointer to a caller-owned `PasteIdSource` rather than the source itself:
/// `std::function` is not trivially copyable and so cannot live in a
/// `std::atomic`, and `nextPasteId()` runs on whatever pool thread the
/// dispatch landed on, not on the thread that installed the override.
/// `ScopedPasteIdSource` owns the callable and keeps it alive for exactly as
/// long as the slot points at it.
/// @return The single slot, shared process-wide.
[[nodiscard]] inline std::atomic<const PasteIdSource*>& pasteIdSourceSlot() noexcept {
    static std::atomic<const PasteIdSource*> slot{nullptr};
    return slot;
}

}  // namespace detail

/// @brief The next candidate paste id: from the installed override if one is
///        live, otherwise from the built-in random animal-name generator.
///
/// Defined in `src/models/paste_model.cpp`, beside the keyspace arrays and
/// the generator it falls back to — those stay that translation unit's own
/// implementation detail rather than becoming public API for a test's sake
/// (see `tests/test_paste_model.cpp`'s mirrored copy and the guard that keeps
/// it honest).
/// @return A candidate id, which may or may not already be taken.
[[nodiscard]] std::string nextPasteId();

/// @brief Installs @p source as the paste-id generator for the guard's
///        lifetime; restores the previous source (nests correctly) on
///        destruction.
///
/// Cross-thread visible (a `std::atomic`, not `thread_local`) for the same
/// reason `ScopedClockOverride` is: a model under test may run on a pool
/// thread rather than the thread that constructed the guard. The installed
/// callable is therefore invoked from those threads, and must be safe to call
/// from more than one — a scripted source that walks a list needs an atomic
/// cursor, not a bare `int`.
class ScopedPasteIdSource {
public:
    /// @param source The generator `nextPasteId()` reads for the guard's
    ///        lifetime. Taken by value and owned by the guard, so a lambda
    ///        with captures is safe to pass as a temporary.
    explicit ScopedPasteIdSource(PasteIdSource source)
        : _source{std::move(source)}, _previous{detail::pasteIdSourceSlot().exchange(&_source)} {}

    ~ScopedPasteIdSource() { detail::pasteIdSourceSlot().store(_previous); }

    ScopedPasteIdSource(const ScopedPasteIdSource&) = delete;
    ScopedPasteIdSource& operator=(const ScopedPasteIdSource&) = delete;
    ScopedPasteIdSource(ScopedPasteIdSource&&) = delete;
    ScopedPasteIdSource& operator=(ScopedPasteIdSource&&) = delete;

private:
    // Declared before `_previous`: the member initializer for `_previous`
    // publishes `&_source`, which must already name a constructed object.
    PasteIdSource _source;
    const PasteIdSource* _previous;
};

}  // namespace pastebin

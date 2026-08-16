// SPDX-License-Identifier: Apache-2.0

#pragma once
#include <cstddef>

namespace morph::testkit {

/// @brief RAII scope that makes the @c matchToFail -th `operator new` call
///        whose requested size is at least @c minSize throw `std::bad_alloc`
///        on the current thread, so a test can drive a real
///        allocation-failure path (a `catch (...)` around a
///        `std::string`/container assignment, for example) without
///        exhausting process memory or forking/mocking the standard
///        allocator.
///
/// Test-only tooling, not part of the shipped library: `oom_injector.cpp`
/// defines `operator new`/`operator delete` for the whole binary it links
/// into, so this lives under `tests/` and is only ever linked into test
/// executables, never into `morph`'s own interface library or any example
/// app -- a real consumer's process must never get its allocator replaced
/// by test-only code.
///
/// @par Why this exists
/// Several `catch (...)` blocks across the codebase (see
/// `LASTRADA-Software/morph#108`) only ever fire on `std::bad_alloc` from a
/// real allocation failure -- there is no other way into them. That is not
/// portably reachable from a unit test without a seam: this class overrides
/// the process-wide `operator new`/`operator new[]` (defined once in
/// `oom_injector.cpp`) to consult thread-local state before delegating to
/// the real allocator.
///
/// @par Why a size predicate, not a raw allocation count
/// An earlier design counted "the Nth `operator new` call after arming" —
/// rejected before this shipped, because that count depends on exactly how
/// many heap allocations the standard library, the test's own setup code,
/// and even unrelated `std::string`/container growth perform first, which
/// varies across STL implementations and can shift with an unrelated
/// compiler/library upgrade. A test would silently start failing the wrong
/// allocation (or none at all) the moment that count changed, without
/// necessarily failing loudly. A size predicate instead targets what the
/// allocation actually *is*: e.g. "an allocation of at least 64 bytes"
/// reliably matches a specific long string's heap buffer (a
/// short-string-optimized `std::string` never calls `operator new` at all),
/// so a test picks a source string long enough to defeat SSO on every
/// supported standard library and lets the predicate find that allocation
/// regardless of how many smaller, unrelated allocations happen around it.
///
/// @par Why @c matchToFail, not always the first match
/// Some call paths copy the same long value more than once before reaching
/// the statement a test actually wants to fail (e.g. a setup copy taken
/// before a dispatch call, followed by the copy a success callback makes
/// inside that same call stack, when the callback fires inline rather than
/// on a separate later call). @c matchToFail lets a test skip past known,
/// earlier same-shape copies to reach the one it means — still robust,
/// because it counts only allocations that already match @p minSize, not
/// every allocation the standard library happens to make; a copy of a
/// small, short-string-optimized helper value never shifts this count.
///
/// @par Usage
/// ```cpp
/// {
///     // "expected" must be long enough that copying it heap-allocates on
///     // every supported STL (well past libstdc++/libc++/MSVC's ~15-23
///     // byte SSO buffers).
///     std::string expected(128, 'x');
///     morph::testkit::OomInjector inject{/*minSize=*/64};
///     REQUIRE_THROWS_AS(codeThatCopiesExpectedSomewhere(expected), std::bad_alloc);
/// }  // scope ends here: later allocations succeed normally again, even if
///    // the matching allocation never actually happened.
/// ```
/// The @p matchToFail -th `operator new` call after construction whose
/// `size >= minSize` throws `std::bad_alloc` instead of allocating; every
/// smaller allocation, every earlier matching-size allocation before the
/// target occurrence, and every allocation of any size after it fires
/// (including inside the `catch` block itself) allocates normally: this is
/// a one-shot trigger, not a standing size filter. Only one `OomInjector`
/// may be active per thread at a time — nesting two throws
/// `std::logic_error` from the inner constructor, since both would
/// otherwise race over the same thread-local state.
///
/// @par Thread safety
/// The injection state is `thread_local`: an `OomInjector` constructed on
/// one thread only affects `operator new` calls made from that same thread.
/// Other threads' allocations are never affected, so a test that spins up
/// worker threads around the code under test must construct the injector on
/// whichever thread actually performs the allocation it wants to fail.
///
/// @par Why this isn't a `morph::core::FileIoOps`-shaped injectable parameter
/// `FileIoOps` works because `FileActionLog`/`FileOfflineQueue` call the raw
/// syscall directly and can take an injectable strategy object in their own
/// constructor. The `catch (...)` blocks this class targets guard a plain
/// `std::string` copy-assignment with no such seam to thread through -- the
/// allocation happens inside `std::string::operator=` itself, several layers
/// below any parameter `Bridge` could plausibly accept. Overriding the
/// global allocator is the standard technique for exactly this shape of gap.
class OomInjector {
  public:
    /// @brief Arms the injector so the @p matchToFail -th future `operator
    ///        new` call on this thread (counting from 1) whose requested
    ///        size is at least @p minSize throws `std::bad_alloc` instead of
    ///        allocating. Smaller allocations, and matching-size allocations
    ///        before the target occurrence, succeed normally and do not
    ///        otherwise affect the trigger.
    /// @param minSize Minimum allocation size, in bytes, that counts toward
    ///        @p matchToFail.
    /// @param matchToFail Which occurrence (counting from 1) of an
    ///        allocation `>= minSize` should fail. Defaults to 1: the first
    ///        one.
    /// @throws std::logic_error if another `OomInjector` is already active
    ///         on this thread.
    // NOLINTNEXTLINE(bugprone-easily-swappable-parameters) -- both are named
    // for exactly what they mean at any call site (minSize=.../matchToFail=...),
    // and matchToFail's default makes the common one-argument call unambiguous.
    explicit OomInjector(std::size_t minSize, std::size_t matchToFail = 1);

    /// @brief Disarms the injector. Allocations on this thread after this
    ///        point succeed normally again, whether or not the matching
    ///        allocation ever actually happened.
    ~OomInjector();

    OomInjector(const OomInjector&) = delete;
    OomInjector& operator=(const OomInjector&) = delete;
    OomInjector(OomInjector&&) = delete;
    OomInjector& operator=(OomInjector&&) = delete;
};

}  // namespace morph::testkit

// SPDX-License-Identifier: Apache-2.0

// The value-handling contract of morph::async::Completion<T> (morph#553),
// pinned by counting rather than by reading the header:
//
//   - `T` need only be move-constructible. `Completion<std::unique_ptr<int>>`
//     instantiates and fans out.
//   - A handler taking `const T&` costs exactly **zero** copies.
//   - A handler taking `T` by value costs exactly **one**.
//   - Neither number depends on how many handlers are attached, nor on whether
//     they attached before or after the completion settled.
//
// The counts are asserted **exactly**, never as an upper bound: an upper bound
// quietly absorbs the regression this file exists to catch. Before the fix the
// budget was N + 2M -- N copies for the handlers attached before settling and
// two for each one attached after -- regardless of handler signature, because
// every handler was erased as `std::function<void(T)>`.
//
// Every by-value `Probe` parameter below is deliberate and carries a NOLINT:
// `performance-unnecessary-value-param` is right that the copy is avoidable,
// and the copy is exactly what is being measured.
//
// `InlineExecutor` runs each posted closure synchronously on the calling
// thread, so nothing here measures scheduling and every count is deterministic.

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <functional>
#include <memory>
#include <morph/core/callback_scope.hpp>
#include <morph/core/completion.hpp>
#include <stdexcept>
#include <utility>

#include "test_support.hpp"

using SyncExecutor = morph::testing::InlineExecutor;

namespace {

struct Counts {
    int copies = 0;
    int moves = 0;
};

// File-scope rather than a member of `Probe`: the whole point is to count
// constructions of a type the framework moves and copies behind our back, so
// the counter cannot live in the object being counted. Reset before each
// measurement, and every test here runs on one thread.
Counts gCounts;  // NOLINT(cppcoreguidelines-avoid-non-const-global-variables)

// Copy/move *constructors* only: those are what dispatch can charge. Assignment
// is deliberately not declared -- nothing on the value path assigns a settled
// `T` (`value` is engaged exactly once, by construction), so an assignment
// operator here would be a counter no test could ever read.
struct Probe {
    int payload = 0;

    Probe() = default;
    explicit Probe(int value) : payload{value} {}
    Probe(const Probe& other) : payload{other.payload} { ++gCounts.copies; }
    Probe(Probe&& other) noexcept : payload{other.payload} { ++gCounts.moves; }
    Probe& operator=(const Probe&) = delete;
    Probe& operator=(Probe&&) = delete;
    ~Probe() = default;
};

enum class Sig : std::uint8_t {
    ConstRef,      // [](const Probe&)
    ByValue,       // [](Probe)
    ErasedByValue  // an existing std::function<void(Probe)> object
};

/// How many handlers attach before the completion settles, and how many after.
struct Shape {
    int before = 0;
    int after = 0;
};

void attach(morph::async::Completion<Probe>& comp, Sig sig, int& fired) {
    if (sig == Sig::ConstRef) {
        comp.then([&fired](const Probe&) { ++fired; });
    } else if (sig == Sig::ByValue) {
        // NOLINTNEXTLINE(performance-unnecessary-value-param)
        comp.then([&fired](Probe) { ++fired; });
    } else {
        // The legacy spelling: a caller that already holds a
        // `std::function<void(T)>`. It must still convert, because
        // `std::function<void(const T&)>` is constructible from anything
        // invocable with `const T&`.
        // NOLINTNEXTLINE(performance-unnecessary-value-param)
        std::function<void(Probe)> legacy = [&fired](Probe) { ++fired; };
        comp.then(std::move(legacy));
    }
}

/// Settles a fresh completion with @p shape.before handlers attached first and
/// @p shape.after attached once it is ready, and reports the copies/moves of
/// `Probe` charged to the whole sequence.
Counts measure(Shape shape, Sig sig, int& fired) {
    SyncExecutor exec;
    auto pair = morph::async::Completion<Probe>::makeSettleable(&exec);
    auto& comp = pair.first;
    auto& promise = pair.second;

    fired = 0;
    // Reset *after* constructing the completion so the handle's own setup is
    // not charged to the measurement; `Probe` is not touched by it either way.
    gCounts = Counts{};

    for (int i = 0; i < shape.before; ++i) {
        attach(comp, sig, fired);
    }
    promise.resolve(Probe{42});
    for (int i = 0; i < shape.after; ++i) {
        attach(comp, sig, fired);
    }
    return gCounts;
}

constexpr std::array<Shape, 8> kShapes{{
    {.before = 0, .after = 0},
    {.before = 1, .after = 0},
    {.before = 2, .after = 0},
    {.before = 3, .after = 0},
    {.before = 0, .after = 1},
    {.before = 0, .after = 2},
    {.before = 1, .after = 1},
    {.before = 3, .after = 3},
}};

}  // namespace

TEST_CASE("Completion value contract: a const T& handler costs zero copies, at any handler count",
          "[completion][issue-553]") {
    for (const auto& shape : kShapes) {
        INFO("before=" << shape.before << " after=" << shape.after);
        int fired = 0;
        const auto counts = measure(shape, Sig::ConstRef, fired);
        CHECK(counts.copies == 0);
        CHECK(fired == shape.before + shape.after);
    }
}

TEST_CASE("Completion value contract: a by-value handler costs exactly one copy, wherever it attached",
          "[completion][issue-553]") {
    for (const auto& shape : kShapes) {
        const int handlers = shape.before + shape.after;
        INFO("before=" << shape.before << " after=" << shape.after);

        int firedValue = 0;
        const auto byValue = measure(shape, Sig::ByValue, firedValue);
        CHECK(byValue.copies == handlers);
        CHECK(firedValue == handlers);

        // A pre-existing `std::function<void(T)>` object is the same one copy,
        // charged at the same place -- that handler's own parameter binding.
        int firedErased = 0;
        const auto erased = measure(shape, Sig::ErasedByValue, firedErased);
        CHECK(erased.copies == handlers);
        CHECK(firedErased == handlers);
    }
}

TEST_CASE("Completion value contract: settling itself moves T exactly twice, whatever is attached",
          "[completion][issue-553]") {
    // Two moves, and only two: the `Probe{42}` prvalue is elided into
    // `resolve`'s by-value parameter, which moves into `setValue`'s, which
    // moves into `value`. Nothing in dispatch adds one -- which is the claim.
    // Pinned exactly so a future revision cannot slip an extra materialisation
    // of the stored value past this file.
    for (auto sig : {Sig::ConstRef, Sig::ByValue}) {
        int fired = 0;
        CHECK(measure({.before = 0, .after = 0}, sig, fired).moves == 2);
        CHECK(measure({.before = 3, .after = 0}, sig, fired).moves == 2);
        CHECK(measure({.before = 0, .after = 3}, sig, fired).moves == 2);
        CHECK(measure({.before = 3, .after = 3}, sig, fired).moves == 2);
    }
}

TEST_CASE("Completion value contract: a mixed handler set charges one copy per by-value handler and no more",
          "[completion][issue-553]") {
    SyncExecutor exec;
    auto pair = morph::async::Completion<Probe>::makeSettleable(&exec);
    auto& comp = pair.first;
    auto& promise = pair.second;

    int fired = 0;
    gCounts = Counts{};

    comp.then([&fired](const Probe&) { ++fired; });  // before, free
    // NOLINTNEXTLINE(performance-unnecessary-value-param)
    comp.then([&fired](Probe) { ++fired; });         // before, one copy
    comp.then([&fired](const Probe&) { ++fired; });  // before, free

    promise.resolve(Probe{42});

    // NOLINTNEXTLINE(performance-unnecessary-value-param)
    comp.then([&fired](Probe) { ++fired; });         // after, one copy
    comp.then([&fired](const Probe&) { ++fired; });  // after, free

    CHECK(fired == 5);
    CHECK(gCounts.copies == 2);
}

TEST_CASE("Completion value contract: T need only be move-constructible", "[completion][issue-553]") {
    // `Completion<std::unique_ptr<int>>` did not compile before morph#553 --
    // four sites copied `T` on the value path. Instantiating is not evidence on
    // its own, so this fans out to handlers attached on both sides of the
    // settle and checks each one actually ran against the real value.
    SyncExecutor exec;
    auto pair = morph::async::Completion<std::unique_ptr<int>>::makeSettleable(&exec);
    auto& comp = pair.first;
    auto& promise = pair.second;

    int sum = 0;
    comp.then([&sum](const std::unique_ptr<int>& ptr) { sum += *ptr; });
    comp.then([&sum](const std::unique_ptr<int>& ptr) { sum += *ptr; });

    promise.resolve(std::make_unique<int>(21));

    comp.then([&sum](const std::unique_ptr<int>& ptr) { sum += *ptr; });

    CHECK(sum == 63);
}

TEST_CASE("Completion value contract: a move-only T survives the CallbackScope-gated attach too",
          "[completion][issue-553][callback-scope]") {
    // The gated overloads re-erase the handler through `CallbackToken::guard`,
    // which is the one place a move-only `T` could still have been copied back
    // into existence. It is not: the guard is generic and forwards `const T&`.
    SyncExecutor exec;
    const morph::async::CallbackScope scope;
    auto pair = morph::async::Completion<std::unique_ptr<int>>::makeSettleable(&exec);
    auto& comp = pair.first;
    auto& promise = pair.second;

    int seen = 0;
    comp.then(scope, [&seen](const std::unique_ptr<int>& ptr) { seen = *ptr; });
    promise.resolve(std::make_unique<int>(7));
    CHECK(seen == 7);

    // And a stopped scope still refuses delivery on a move-only T.
    auto second = morph::async::Completion<std::unique_ptr<int>>::makeSettleable(&exec);
    int refused = 0;
    second.first.then(scope, [&refused](const std::unique_ptr<int>&) { ++refused; });
    scope.requestStop();
    second.second.resolve(std::make_unique<int>(9));
    CHECK(refused == 0);
}

TEST_CASE("Completion value contract: a throwing by-value handler does not disturb its siblings' copies",
          "[completion][issue-553]") {
    // Fan-out isolation and the copy budget are independent: a handler that
    // throws after taking its copy still costs exactly one, and the handlers
    // after it still run and still cost exactly what their own signature says.
    SyncExecutor exec;
    auto pair = morph::async::Completion<Probe>::makeSettleable(&exec);
    auto& comp = pair.first;
    auto& promise = pair.second;

    int fired = 0;
    gCounts = Counts{};

    // NOLINTNEXTLINE(performance-unnecessary-value-param)
    comp.then([](Probe) { throw std::runtime_error{"handler blew up"}; });
    comp.then([&fired](const Probe&) { ++fired; });
    // NOLINTNEXTLINE(performance-unnecessary-value-param)
    comp.then([&fired](Probe) { ++fired; });

    REQUIRE_NOTHROW(promise.resolve(Probe{42}));

    CHECK(fired == 2);
    CHECK(gCounts.copies == 2);
}

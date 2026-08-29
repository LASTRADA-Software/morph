// SPDX-License-Identifier: Apache-2.0
//
// Tests for keying a model on a *strong id* (morph#163).
//
// `examples/IMPLEMENTATION.md` rule 3 requires entity identity to be a
// per-entity strong id type exposing `hasValue()`. Before this, such a type
// satisfied no arm of `morph::model::ModelKey`, so a rung obeying rule 3 could
// not use `BRIDGE_MODEL_KEY`/`BRIDGE_KEY_FROM` at all and had to hand-write the
// traits -- which three rungs independently did.
//
// These tests pin both spellings the ladder actually uses, and the empty-id
// case, which is the one that fails dangerously if unhandled.

#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <memory>
#include <morph/attributes.hpp>
#include <morph/core/backend.hpp>
#include <morph/core/bridge.hpp>
#include <morph/core/executor.hpp>
#include <morph/core/model_key.hpp>
#include <morph/core/registry.hpp>
#include <optional>
#include <string>
#include <vector>

#include "test_support.hpp"

// Model, action and result types need **external** linkage: glaze's
// plain-aggregate reflection cannot see into an anonymous namespace, and the
// BRIDGE_REGISTER_* macros specialise templates at global scope.
// NOLINTBEGIN(misc-use-internal-linkage)

/// Optional-backed strong id, the `pastebin::PasteId` shape: `operator*`
/// returns a reference, and emptiness is a disengaged optional.
struct SikNameId {
    std::optional<std::string> value;

    constexpr SikNameId() noexcept = default;
    explicit SikNameId(std::string id) noexcept : value{std::move(id)} {}

    [[nodiscard]] bool hasValue() const noexcept { return value.has_value(); }
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    [[nodiscard]] const std::string& operator*() const noexcept MORPH_LIFETIMEBOUND { return *value; }
    [[nodiscard]] auto operator<=>(const SikNameId&) const noexcept = default;
};

/// Sentinel-backed strong id, the `polls::OptionId` shape: `operator*` returns
/// **by value**, and `0` means empty. Deliberately a different shape from
/// `SikNameId` -- a concept that only admitted one of them would leave half the
/// ladder still hand-writing traits.
struct SikRowId {
    std::int64_t value{0};

    [[nodiscard]] constexpr bool hasValue() const { return value != 0; }
    [[nodiscard]] constexpr std::int64_t operator*() const { return value; }
    [[nodiscard]] constexpr bool operator==(const SikRowId&) const = default;
};

// ── The concept itself ────────────────────────────────────────────────────────

static_assert(morph::model::WrappedModelKey<SikNameId>);
static_assert(morph::model::WrappedModelKey<SikRowId>);
static_assert(morph::model::ModelKey<SikNameId>);
static_assert(morph::model::ModelKey<SikRowId>);

// Raw keys stay raw: widening the concept must not reclassify them, or
// `keyToString` would take the wrong branch for every existing model.
static_assert(morph::model::RawModelKey<std::int64_t>);
static_assert(!morph::model::WrappedModelKey<std::int64_t>);
static_assert(!morph::model::WrappedModelKey<std::string>);

/// Has `hasValue()` and `operator*`, but unwraps to a type that is not itself a
/// key. Must stay rejected -- otherwise the wire encoding has no definition.
struct SikNotAKey {
    struct Inner {};
    [[nodiscard]] bool hasValue() const { return true; }
    [[nodiscard]] Inner operator*() const { return {}; }
};
static_assert(!morph::model::ModelKey<SikNotAKey>);

/// Unwraps to a key but cannot be rebuilt from one, so it could not survive the
/// wire round trip. Rejected at declaration rather than failing later.
struct SikNoRebuild {
    std::int64_t value{0};
    explicit SikNoRebuild(int, int) {}
    [[nodiscard]] bool hasValue() const { return value != 0; }
    [[nodiscard]] std::int64_t operator*() const { return value; }
};
static_assert(!morph::model::ModelKey<SikNoRebuild>);

/// `bool` is excluded from raw keys, so a strong id wrapping one is excluded too.
struct SikBoolId {
    bool value{false};
    [[nodiscard]] bool hasValue() const { return value; }
    [[nodiscard]] bool operator*() const { return value; }
};
static_assert(!morph::model::ModelKey<SikBoolId>);

// ── Encoding ──────────────────────────────────────────────────────────────────

TEST_CASE("a strong id encodes as the scalar it wraps", "[strong-id-keys]") {
    REQUIRE(morph::model::keyToString(SikNameId{"swift-otter"}) == "swift-otter");
    REQUIRE(morph::model::keyToString(SikRowId{42}) == "42");
    REQUIRE(morph::model::keyToString(SikRowId{-17}) == "-17");

    // Sharing an encoding with the raw key is deliberate: the directory is one
    // map keyed on strings, so a strong id and its scalar must not disagree.
    REQUIRE(morph::model::keyToString(SikRowId{42}) == morph::model::keyToString<std::int64_t>(42));
}

TEST_CASE("a strong id round-trips through its wire encoding", "[strong-id-keys]") {
    const SikNameId name{"swift-otter"};
    REQUIRE(morph::model::keyFromString<SikNameId>(morph::model::keyToString(name)) == name);

    const SikRowId row{42};
    REQUIRE(morph::model::keyFromString<SikRowId>(morph::model::keyToString(row)) == row);
}

TEST_CASE("an empty strong id is refused, not encoded", "[strong-id-keys]") {
    // The dangerous case. Encoding an unset id as "" or "0" would route every
    // caller holding one to a single shared instance -- silently, and looking
    // like it worked.
    REQUIRE_THROWS(morph::model::keyToString(SikNameId{}));
    REQUIRE_THROWS(morph::model::keyToString(SikRowId{}));

    // And an engaged one still works, so the guard is not simply refusing everything.
    REQUIRE_NOTHROW(morph::model::keyToString(SikNameId{"x"}));
    REQUIRE_NOTHROW(morph::model::keyToString(SikRowId{1}));
}

// ── End to end, through the macros rule 3 previously locked rungs out of ─────

struct SikOpenRow {
    SikRowId id;
};
struct SikBump {
    std::int64_t by = 0;
};
struct SikCount {
    std::int64_t value = 0;
};

struct SikRowModel {
    std::int64_t value = 0;

    SikCount execute(const SikOpenRow&) { return SikCount{.value = value}; }
    SikCount execute(const SikBump& act) {
        value += act.by;
        return SikCount{.value = value};
    }
};
// NOLINTEND(misc-use-internal-linkage)

BRIDGE_REGISTER_MODEL(SikRowModel, "SIK_RowModel")
BRIDGE_REGISTER_ACTION(SikRowModel, SikOpenRow, "SIK_OpenRow")
BRIDGE_REGISTER_ACTION(SikRowModel, SikBump, "SIK_Bump")

// The line this issue is about: before morph#163 this did not compile, because
// `SikRowId` satisfied neither arm of `ModelKey`.
BRIDGE_MODEL_KEY(SikRowModel, SikOpenRow, &SikOpenRow::id);

namespace {

using morph::bridge::AllowShared;
using morph::bridge::Bridge;
using morph::bridge::BridgeHandler;

template <typename T>
T settleSik(morph::async::Completion<T> comp) {
    auto out = std::make_shared<T>();
    auto done = std::make_shared<std::atomic<bool>>(false);
    auto failed = std::make_shared<std::atomic<bool>>(false);
    std::move(comp)
        .then([out, done](T value) {
            *out = std::move(value);
            done->store(true);
        })
        .onError([failed, done](const std::exception_ptr&) {
            failed->store(true);
            done->store(true);
        });
    REQUIRE(morph::testing::waitUntil([&] { return done->load(); }));
    REQUIRE_FALSE(failed->load());
    return *out;
}

}  // namespace

TEST_CASE("a model keyed on a strong id shares instances by key", "[strong-id-keys]") {
    morph::testing::InlineExecutor exec;
    Bridge bridge{std::make_unique<morph::backend::LocalBackend>(exec)};

    BridgeHandler<SikRowModel, AllowShared> bench{bridge, &exec};
    BridgeHandler<SikRowModel, AllowShared> office{bridge, &exec};

    settleSik(bench.execute(SikOpenRow{.id = SikRowId{7}}));
    settleSik(office.execute(SikOpenRow{.id = SikRowId{7}}));

    // One instance, not two -- the property keying exists for. If the strong id
    // encoded inconsistently, these would be separate instances and the
    // assertion below would read 5 rather than 8.
    settleSik(bench.execute(SikBump{.by = 5}));
    REQUIRE(settleSik(office.execute(SikBump{.by = 3})).value == 8);

    REQUIRE(settleSik(bench.instances()) == std::vector<SikRowId>{SikRowId{7}});
    REQUIRE(bench.primary().value() == SikRowId{7});
}

TEST_CASE("a different strong id key reaches a different instance", "[strong-id-keys]") {
    morph::testing::InlineExecutor exec;
    Bridge bridge{std::make_unique<morph::backend::LocalBackend>(exec)};

    BridgeHandler<SikRowModel, AllowShared> first{bridge, &exec};
    BridgeHandler<SikRowModel, AllowShared> second{bridge, &exec};

    settleSik(first.execute(SikOpenRow{.id = SikRowId{1}}));
    settleSik(second.execute(SikOpenRow{.id = SikRowId{2}}));

    settleSik(first.execute(SikBump{.by = 10}));
    // Distinct key, so distinct state: this must not see the 10.
    REQUIRE(settleSik(second.execute(SikBump{.by = 1})).value == 1);
}

TEST_CASE("executing with an empty strong id fails the completion", "[strong-id-keys]") {
    morph::testing::InlineExecutor exec;
    Bridge bridge{std::make_unique<morph::backend::LocalBackend>(exec)};
    BridgeHandler<SikRowModel, AllowShared> handler{bridge, &exec};

    // Key extraction throws; `BridgeHandler` converts that into a rejected
    // Completion rather than letting it escape `execute()`.
    auto done = std::make_shared<std::atomic<bool>>(false);
    auto failed = std::make_shared<std::atomic<bool>>(false);
    handler.execute(SikOpenRow{.id = SikRowId{}})
        .then([done](SikCount) { done->store(true); })
        .onError([failed, done](const std::exception_ptr&) {
            failed->store(true);
            done->store(true);
        });
    REQUIRE(morph::testing::waitUntil([&] { return done->load(); }));
    REQUIRE(failed->load());

    // Nothing was attached, so no instance was created for the empty key.
    REQUIRE(settleSik(handler.instances()).empty());
}

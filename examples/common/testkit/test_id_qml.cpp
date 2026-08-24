// SPDX-License-Identifier: Apache-2.0
//
// Tests for morph::ladder::gui::idNumber/idText/idFromText (morph#169).
//
// The extraction replaced hand-written conversions in ten files across five
// rungs. It is a pure refactor -- every converted site kept the exact
// representation it already had -- so what these pin is the invariant that
// made the duplication worth removing rather than any changed behaviour:
//
//   an id the model calls EMPTY and an id the model calls ENGAGED-WITH-ZERO
//   must not arrive at QML as the same value.
//
// The two id shapes below are transcribed from the ladder's own, because the
// helper is deliberately model-independent and this suite links no rung:
//
//   OptionalId   <- kanban/core/types.hpp's KANBAN_DEFINE_STRONG_ID, and the
//                   identical macros in ledger/lims, and bookmarks::BookmarkId
//   ZeroSentinel <- polls::OptionId / polls::PollEventId / kanban::BoardEventId
//
// They differ in a way that matters here: OptionalId can hold an engaged zero,
// ZeroSentinel cannot -- `0` is its own documented empty state, in the model
// type, one layer below this header. See id_qml.hpp's "What an id is here".

#include <QString>
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <optional>

#include "gui/id_qml.hpp"

using morph::ladder::gui::idFromText;
using morph::ladder::gui::idNumber;
using morph::ladder::gui::idText;
using morph::ladder::gui::kNoId;

namespace {

/// The optional-backed shape: `hasValue()` is `has_value()`, so a disengaged
/// id and an id holding `0` are different values.
struct OptionalId {
    std::optional<std::int64_t> value;
    constexpr OptionalId() noexcept = default;
    explicit OptionalId(std::int64_t id) noexcept : value{id} {}
    [[nodiscard]] bool hasValue() const noexcept { return value.has_value(); }
    [[nodiscard]] std::int64_t operator*() const noexcept { return *value; }
    [[nodiscard]] bool operator==(const OptionalId&) const noexcept = default;
};

/// The zero-sentinel shape: `hasValue()` is `value != 0`, so `ZeroSentinel{0}`
/// *is* the empty state and no conversion can separate the two.
struct ZeroSentinel {
    std::int64_t value{0};
    [[nodiscard]] constexpr bool hasValue() const { return value != 0; }
    [[nodiscard]] constexpr std::int64_t operator*() const { return value; }
    [[nodiscard]] constexpr bool operator==(const ZeroSentinel&) const = default;
};

}  // namespace

// ── The three states that must stay distinct ────────────────────────────────

TEST_CASE("idNumber keeps unset, zero and an ordinary id apart", "[id-qml]") {
    // This is morph#169's whole reason for existing. Written as three
    // pairwise inequalities rather than three equalities, because a
    // conversion that collapsed unset onto zero would still satisfy any two
    // of the equalities on their own.
    const qlonglong unset = idNumber(OptionalId{});
    const qlonglong zero = idNumber(OptionalId{0});
    const qlonglong ordinary = idNumber(OptionalId{7});

    CHECK(unset != zero);
    CHECK(unset != ordinary);
    CHECK(zero != ordinary);

    CHECK(unset == kNoId);
    CHECK(zero == 0);
    CHECK(ordinary == 7);
}

TEST_CASE("idText keeps unset, zero and an ordinary id apart", "[id-qml]") {
    // Same three states through the QString-shaped surface. The empty case is
    // an empty string, not "0" and not "-1": the string form has a genuinely
    // empty value available, so it does not need kNoId.
    const QString unset = idText(OptionalId{});
    const QString zero = idText(OptionalId{0});
    const QString ordinary = idText(OptionalId{7});

    CHECK(unset != zero);
    CHECK(unset != ordinary);
    CHECK(zero != ordinary);

    CHECK(unset.isEmpty());
    CHECK(zero == QStringLiteral("0"));
    CHECK(ordinary == QStringLiteral("7"));
}

TEST_CASE("idFromText keeps unset, zero and an ordinary id apart", "[id-qml]") {
    // The inbound half. "0" must parse to an ENGAGED zero, not to the empty
    // id -- an inbound collapse is the same defect as an outbound one, and it
    // is the easier of the two to write by accident (`if (!value) return {};`).
    CHECK(idFromText<OptionalId>(QString{}) == OptionalId{});
    CHECK_FALSE(idFromText<OptionalId>(QString{}).hasValue());

    CHECK(idFromText<OptionalId>(QStringLiteral("0")) == OptionalId{0});
    CHECK(idFromText<OptionalId>(QStringLiteral("0")).hasValue());

    CHECK(idFromText<OptionalId>(QStringLiteral("7")) == OptionalId{7});
    CHECK(idFromText<OptionalId>(QStringLiteral("0")) != idFromText<OptionalId>(QString{}));
}

// ── The empty representation, stated explicitly ─────────────────────────────

TEST_CASE("kNoId is a value no ladder id can legitimately hold", "[id-qml]") {
    // Pinned as a value, not just as "whatever idNumber returns when empty",
    // because the QML half of the contract hard-codes it: ProjectListView's
    // `membersProjectId: -1`, BoardView's "-1 means accept any",
    // TaskDetailPopup's `taskId: "-1"`, SampleView's display fallback. If this
    // constant ever moves, those move with it.
    STATIC_REQUIRE(kNoId == -1);
    CHECK(idNumber(OptionalId{}) == kNoId);
    CHECK(idNumber(ZeroSentinel{}) == kNoId);
}

TEST_CASE("the empty id round-trips through the text form", "[id-qml]") {
    // idText -> idFromText is an identity on the empty id. The number form has
    // no matching guarantee and deliberately does not claim one: idFromText
    // parses "-1" as the engaged id -1, because a helper that special-cased the
    // sentinel on the way in would silently swallow a real negative key for a
    // model that has one. kanban's parseId is where that policy lives, since
    // kanban is the rung whose QML actually echoes the sentinel back.
    CHECK_FALSE(idFromText<OptionalId>(idText(OptionalId{})).hasValue());
    CHECK(idFromText<OptionalId>(idText(OptionalId{0})) == OptionalId{0});
    CHECK(idFromText<OptionalId>(idText(OptionalId{7})) == OptionalId{7});

    CHECK(idFromText<OptionalId>(QStringLiteral("-1")) == OptionalId{-1});
    CHECK(idFromText<OptionalId>(QStringLiteral("-1")).hasValue());
}

// ── The zero-sentinel family, told the truth about ──────────────────────────

TEST_CASE("a zero-sentinel id has no unset-vs-zero distinction to preserve", "[id-qml]") {
    // Not a bug in the conversion: polls::OptionId documents `0` as "not
    // entered" and defines hasValue() as `value != 0`, so ZeroSentinel{0} and
    // ZeroSentinel{} are one value before either reaches this header. Pinned
    // so that a future reader who finds these two mapping to kNoId looks at
    // the id type rather than at idNumber.
    STATIC_REQUIRE(ZeroSentinel{0} == ZeroSentinel{});
    CHECK(idNumber(ZeroSentinel{0}) == kNoId);
    CHECK(idText(ZeroSentinel{0}).isEmpty());

    // Everything the type does call engaged still crosses as its own payload.
    CHECK(idNumber(ZeroSentinel{7}) == 7);
    CHECK(idText(ZeroSentinel{7}) == QStringLiteral("7"));
}

// ── Parse rejection, and the range the ladder actually spans ────────────────

TEST_CASE("idFromText yields an empty id for text that is not a number", "[id-qml]") {
    CHECK_FALSE(idFromText<OptionalId>(QStringLiteral("abc")).hasValue());
    CHECK_FALSE(idFromText<OptionalId>(QStringLiteral("")).hasValue());
    CHECK_FALSE(idFromText<OptionalId>(QStringLiteral("12x")).hasValue());
    CHECK_FALSE(idFromText<OptionalId>(QStringLiteral("1.5")).hasValue());
    // Out of qlonglong range -- QString::toLongLong reports !ok rather than
    // saturating, so this is the empty id and not INT64_MAX.
    CHECK_FALSE(idFromText<OptionalId>(QStringLiteral("99999999999999999999")).hasValue());
}

TEST_CASE("the text form carries a full-width id exactly", "[id-qml]") {
    // idText/idFromText are exact across the whole int64 range because the
    // digits never become a number in between. This is NOT a claim about what
    // QML then does with the string: morph#190/#191 found that QML's
    // JSON.parse/JSON.stringify round trip rounds anything past 2^53, and
    // idNumber's qlonglong reaches QML through the same numeric engine. Fixing
    // that belongs to those issues; this only pins that the helper does not
    // add a second lossy step of its own.
    constexpr std::int64_t big = 9007199254740993;  // 2^53 + 1
    CHECK(idText(OptionalId{big}) == QStringLiteral("9007199254740993"));
    CHECK(idFromText<OptionalId>(idText(OptionalId{big})) == OptionalId{big});
    CHECK(idNumber(OptionalId{big}) == 9007199254740993LL);
}

// SPDX-License-Identifier: Apache-2.0
//
// PasteModel's model-level suite: ordinary CRUD, the burn-after-read
// semantics (including the atomicity guarantee under genuine socket
// concurrency), expiry through the injectable clock, the store-error
// classification branches, and the security/protocol cases
// `examples/pastebin/README.md`'s "Required tests" section assigns to this
// rung. Every case builds its own `DbFixture` (rung 0's convention) so it
// starts from a freshly migrated, real on-disk schema.

// Lightweight::DataMapper::CreateInternal's own if-constexpr chain
// (DataMapper.hpp) has a trailing `return {};` that MSVC's flow analysis
// proves unreachable for PasteModel's specific Record instantiation --
// entirely inside that third-party header, not any call site in this file.
// /external:W0 (this file's own target already demotes Lightweight's
// headers to SYSTEM, per morph_add_rung.cmake) does not suppress it here:
// the diagnosis is instantiation-driven and MSVC ties it to the template's
// first instantiation point in the TU, not merely "reported at a line
// inside the external header" -- a known MSVC limitation with templates in
// headers marked external. File-scoped instead of scoped to one call site,
// since several call sites in this file instantiate the same template.
#if defined(_MSC_VER)
#pragma warning(disable : 4702)
#endif

#include <Lightweight/DataMapper/DataMapper.hpp>
#include <Lightweight/SqlConnection.hpp>
#include <Lightweight/SqlError.hpp>
#include <Lightweight/SqlLogger.hpp>
#include <Lightweight/SqlStatement.hpp>
#include <algorithm>
#include <array>
#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers_exception.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <glaze/glaze.hpp>
#include <iterator>
#include <memory>
#include <morph/core/registry.hpp>
#include <morph/core/wire.hpp>
#include <morph/forms/forms.hpp>
#include <morph/qt/qt_websocket_server.hpp>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "clock.hpp"
#include "pastebin/app/app.hpp"
#include "pastebin/core/errors.hpp"
#include "pastebin/db/database.hpp"
#include "pastebin/db/paste_entity.hpp"
#include "pastebin/models/paste_id_source.hpp"
#include "pastebin/models/paste_model.hpp"
#include "testkit/backend_rig.hpp"
#include "testkit/db_busy_fixture.hpp"
#include "testkit/db_fixture.hpp"
#include "testkit/db_pool_drain.hpp"
#include "testkit/pump.hpp"

namespace {

using morph::ladder::testkit::awaitQt;
using morph::ladder::testkit::BackendRig;
using morph::ladder::testkit::DbFixture;
using morph::ladder::testkit::drainPoolIdleMappers;
using morph::ladder::testkit::Mode;
using morph::ladder::testkit::pumpUntil;

// ─────────────────────────────────────────────────────────────────────────
// Small assertion helpers
// ─────────────────────────────────────────────────────────────────────────

/// @brief An engaged `Reads` as a plain whole number; `-1` when disengaged,
///        so an unexpectedly-empty quantity fails an assertion loudly rather
///        than dereferencing an empty optional.
[[nodiscard]] std::int64_t countOf(const pastebin::Reads& reads) {
    return reads.hasValue() ? ::morph::math::floor(*reads) : -1;
}

[[nodiscard]] pastebin::CreatePaste makeCreate(std::string content, std::string syntax = "text") {
    pastebin::CreatePaste create;
    create.content = std::move(content);
    create.syntax = std::move(syntax);
    return create;
}

/// @brief The instant `morph::ladder::now()` currently reads, shifted by
///        @p delta — the standard way this suite moves time without sleeping.
[[nodiscard]] ::morph::time::DateTime nowPlus(std::chrono::milliseconds delta) {
    return *morph::ladder::now() + delta;
}

/// @brief Mirrors `paste_model.cpp`'s anonymous-namespace `toEpochMs` (not
///        visible from this TU) so a test can seed `PasteRecord::expiresAtMs`
///        directly, bypassing `CreatePaste`'s `Timestamp`-typed action field.
[[nodiscard]] std::int64_t toEpochMs(const ::morph::time::DateTime& instant) {
    return instant.value.time_since_epoch().count();
}

// ─────────────────────────────────────────────────────────────────────────
// The animal-name keyspace, mirrored from `src/models/paste_model.cpp`
// ─────────────────────────────────────────────────────────────────────────
//
// Deliberately duplicated rather than exported: those arrays are the model
// TU's own anonymous-namespace implementation detail, and making them public
// API purely for a test would widen the model's surface for no other caller.
// The duplication cannot silently rot, because the keyspace-exhaustion case
// below fills *every* id these arrays can spell and then requires
// `CreatePaste` to fail — if the real arrays ever gain an entry this copy
// lacks, that create finds a free id and the test fails loudly.

constexpr std::array<std::string_view, 16> kAnimals = {
    "cat", "dog", "fox", "owl", "bee", "ant", "elk", "ram", "yak", "cod", "eel", "hen", "pig", "cow", "bat", "jay",
};
constexpr std::array<std::string_view, 16> kAdjectives = {
    "red",  "blue", "gold", "dark", "swift", "calm",  "bold", "wild",
    "keen", "grey", "warm", "cool", "sharp", "quiet", "loud", "soft",
};
constexpr int kSuffixes = 1000;  // paste_model.cpp's uniform_int_distribution<int>{0, 999}
constexpr std::size_t kCombos = kAdjectives.size() * kAnimals.size();

/// @brief Inserts every `<adjective>-<animal>-<0..999>` id for the first
///        @p comboCount adjective/animal pairs, occupying that share of the
///        keyspace so `CreatePaste`'s allocation genuinely collides.
///
/// One `INSERT ... SELECT` over a recursive CTE rather than @p comboCount
/// x 1000 `DataMapper::Create` round trips: occupying a quarter of the
/// keyspace is 64,000 rows, which is seconds of ODBC round trips and
/// milliseconds of SQLite.
void occupyKeyspace(std::size_t comboCount) {
    std::string combos;
    std::size_t emitted = 0;
    for (const auto& adjective : kAdjectives) {
        for (const auto& animal : kAnimals) {
            if (emitted >= comboCount) {
                break;
            }
            if (emitted > 0) {
                combos += " UNION ALL ";
            }
            combos += "SELECT '";
            combos += adjective;
            combos += '-';
            combos += animal;
            combos += "' AS prefix";
            ++emitted;
        }
    }
    REQUIRE(emitted == comboCount);

    ::Lightweight::SqlStatement stmt;
    (void)stmt.ExecuteDirect(
        "WITH RECURSIVE suffix(x) AS (SELECT 0 UNION ALL SELECT x + 1 FROM suffix WHERE x < " +
        std::to_string(kSuffixes - 1) +
        ") INSERT INTO pastes (id, content, syntax, created_at_ms, expires_at_ms, burn_after_reads, "
        "read_count, is_private, is_editable) SELECT c.prefix || '-' || suffix.x, 'occupied', 'text', "
        "0, NULL, NULL, 0, 0, 0 FROM suffix, (" +
        combos + ") c");
}

// ─────────────────────────────────────────────────────────────────────────
// The injectable paste-id source (`pastebin/models/paste_id_source.hpp`)
// ─────────────────────────────────────────────────────────────────────────

/// @brief `paste_model.cpp`'s `kMaxIdAttempts`, mirrored here for the same
///        reason the keyspace above is: it is that translation unit's own
///        constant. This copy is not free to rot either — the exhaustion case
///        scripts exactly this many colliding ids followed by a free one, and
///        fails if the model draws a ninth.
constexpr std::size_t kMaxIdAttempts = 8;

/// @brief A paste-id source that hands back the same id on every call, so
///        every allocation attempt collides on the same row.
[[nodiscard]] pastebin::PasteIdSource constantIdSource(std::string id) {
    return [id = std::move(id)] { return id; };
}

/// @brief A paste-id source that hands back a written-down sequence of ids in
///        order, and counts how many `CreatePaste` actually drew.
///
/// The count is the assertion the old sampling version could not make: it
/// says how many allocation attempts happened, not merely that the call
/// eventually succeeded or failed.
class ScriptedIds {
public:
    /// @param ids The ids to hand back, in order.
    explicit ScriptedIds(std::vector<std::string> ids) : _ids{std::move(ids)} {}

    /// @brief A source drawing from this script; must not outlive it.
    /// @return The source, to install with `pastebin::ScopedPasteIdSource`.
    [[nodiscard]] pastebin::PasteIdSource source() {
        return [this] {
            const std::size_t index = _drawn.fetch_add(1);
            // Off the end means `CreatePaste` asked for more ids than the
            // script covers — i.e. it retried past the budget the case
            // encodes. Fail loudly here rather than wrap around and let the
            // case pass on a different sequence than the one it describes.
            REQUIRE(index < _ids.size());
            return _ids[index];
        };
    }

    /// @brief How many ids have been drawn from this script so far.
    [[nodiscard]] std::size_t drawn() const noexcept { return _drawn.load(); }

private:
    std::vector<std::string> _ids;
    std::atomic<std::size_t> _drawn{0};
};

/// @brief Whether @p id is spelled from the real animal-name keyspace —
///        `<adjective>-<animal>-<0..999>`, both words from the mirrored
///        arrays above.
///
/// Used to check that a create outside every `ScopedPasteIdSource` is back on
/// the built-in random generator, rather than on some id a test scripted.
[[nodiscard]] bool isAnimalNameId(std::string_view id) {
    const auto firstDash = id.find('-');
    if (firstDash == std::string_view::npos) {
        return false;
    }
    const auto secondDash = id.find('-', firstDash + 1);
    if (secondDash == std::string_view::npos) {
        return false;
    }
    const auto adjective = id.substr(0, firstDash);
    const auto animal = id.substr(firstDash + 1, secondDash - firstDash - 1);
    const auto suffix = id.substr(secondDash + 1);
    const bool digits =
        !suffix.empty() && std::ranges::all_of(suffix, [](char chr) { return chr >= '0' && chr <= '9'; });
    return std::ranges::find(kAdjectives, adjective) != kAdjectives.end() &&
           std::ranges::find(kAnimals, animal) != kAnimals.end() && digits;
}

// ─────────────────────────────────────────────────────────────────────────
// Fuzz-corpus replay support (Step 8 / README "Hostile content round-trip")
// ─────────────────────────────────────────────────────────────────────────

/// @brief Every committed fuzz finding, as raw bytes.
///
/// `MORPH_LADDER_SOURCE_ROOT` is compiled in by `morph_add_rung()` — ctest
/// runs this binary from its own build directory, so a repo-relative path
/// would not resolve. The directory is walked at runtime (not a hard-coded
/// file list) for the same reason `tests/fuzz/CMakeLists.txt` globs it:
/// a newly committed reproducer must start being replayed without anyone
/// remembering to edit a list here.
[[nodiscard]] std::vector<std::pair<std::string, std::string>> fuzzFindings() {
    const std::filesystem::path root = std::filesystem::path{MORPH_LADDER_SOURCE_ROOT} / "tests" / "fuzz" / "findings";
    std::vector<std::pair<std::string, std::string>> inputs;
    for (const auto& entry : std::filesystem::recursive_directory_iterator{root}) {
        if (!entry.is_regular_file()) {
            continue;
        }
        std::ifstream in{entry.path(), std::ios::binary};
        REQUIRE(in.good());
        inputs.emplace_back(entry.path().filename().string(),
                            std::string{std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}});
    }
    std::ranges::sort(inputs);  // stable order across filesystems, for reproducible failures
    return inputs;
}

/// @brief Whether @p text is well-formed UTF-8.
///
/// The wire protocol is JSON in a WebSocket *text* frame, and the storage
/// column is `TEXT`: bytes that are not valid UTF-8 have no faithful
/// representation anywhere along that path. Which half of the corpus a given
/// finding falls into decides which guarantee the round-trip case below can
/// honestly assert — see it for the split.
[[nodiscard]] bool isValidUtf8(std::string_view text) {
    std::size_t i = 0;
    while (i < text.size()) {
        const auto lead = static_cast<unsigned char>(text[i]);
        std::size_t extra = 0;
        if (lead < 0x80) {
            extra = 0;
        } else if ((lead & 0xE0) == 0xC0 && lead >= 0xC2) {
            extra = 1;
        } else if ((lead & 0xF0) == 0xE0) {
            extra = 2;
        } else if ((lead & 0xF8) == 0xF0 && lead <= 0xF4) {
            extra = 3;
        } else {
            return false;
        }
        if (i + extra >= text.size()) {
            return false;
        }
        for (std::size_t k = 1; k <= extra; ++k) {
            if ((static_cast<unsigned char>(text[i + k]) & 0xC0) != 0x80) {
                return false;
            }
        }
        i += extra + 1;
    }
    return true;
}

/// @brief How many rows the `pastes` table currently holds.
///
/// Read straight from SQL rather than through `ListPastes`, so it counts
/// private pastes too and is unaffected by paging.
[[nodiscard]] std::int64_t pasteRowCount() {
    ::Lightweight::SqlStatement stmt;
    return stmt.ExecuteDirectScalar<std::int64_t>("SELECT COUNT(*) FROM pastes").value_or(-1);
}

/// @brief Installs a short SQLite `busy_timeout` on every connection opened
///        while it is alive, and restores the default afterwards.
///
/// `Lightweight::SqlConnection::PostConnect()` unconditionally issues
/// `PRAGMA busy_timeout = 60000` on every new SQLite connection, so a write
/// that collides with `DbBusyFixture`'s held lock blocks for a real minute
/// before SQLite gives up. `test_db_busy_fixture.cpp` re-issues the PRAGMA on
/// the connection it owns — that is not available here, because the
/// connection `PasteModel` uses is acquired from
/// `Lightweight::GlobalDataMapperPool()` inside `execute(...)`, which no test
/// can reach directly. The post-connected hook is the seam that works from
/// the outside: it runs immediately after `PostConnect()` on every
/// newly-created connection. See `db_busy_fixture.hpp`'s
/// "`SetPostConnectedHook` and `GlobalDataMapperPool()`" note: this is only
/// guaranteed to fire if the pool actually creates a fresh connection for the
/// model under test's acquisition, not if it hands back an already-connected
/// idle one — the two call sites below accept that as a documented,
/// not-fully-deterministic tradeoff rather than a hard guarantee.
class ScopedShortBusyTimeout {
public:
    explicit ScopedShortBusyTimeout(int milliseconds) {
        ::Lightweight::SqlConnection::SetPostConnectedHook([milliseconds](::Lightweight::SqlConnection& connection) {
            ::Lightweight::SqlStatement stmt{connection};
            (void)stmt.ExecuteDirect("PRAGMA busy_timeout = " + std::to_string(milliseconds));
        });
    }
    ~ScopedShortBusyTimeout() { ::Lightweight::SqlConnection::ResetPostConnectedHook(); }

    ScopedShortBusyTimeout(const ScopedShortBusyTimeout&) = delete;
    ScopedShortBusyTimeout& operator=(const ScopedShortBusyTimeout&) = delete;
    ScopedShortBusyTimeout(ScopedShortBusyTimeout&&) = delete;
    ScopedShortBusyTimeout& operator=(ScopedShortBusyTimeout&&) = delete;
};

}  // namespace

// ═════════════════════════════════════════════════════════════════════════
// Step 1 — ordinary CRUD and validation
// ═════════════════════════════════════════════════════════════════════════

TEST_CASE("CreatePaste stores a paste under a freshly allocated animal-name id", "[pastebin][model]") {
    DbFixture fixture;
    pastebin::PasteModel model;

    const auto id = model.execute(makeCreate("hello", "cpp")).id;
    REQUIRE(id.hasValue());
    CHECK_FALSE((*id).empty());

    const auto view = model.execute(pastebin::GetPaste{.id = id});
    CHECK(view.id == id);
    CHECK(view.content == "hello");
    CHECK(view.syntax == "cpp");
    CHECK(view.visibility == pastebin::Visibility::Public);
    CHECK(view.editability == pastebin::Editability::Immutable);
    CHECK_FALSE(view.expiresAt.hasValue());
    CHECK_FALSE(view.burnAfterReads.hasValue());
}

TEST_CASE("CreatePaste and EditPaste round-trip non-ASCII content losslessly", "[pastebin][model]") {
    // `content` is stored as Light::SqlMaxDynamicWideString (paste_entity.hpp's
    // file comment explains why: SqlText/std::string are char-based and render
    // as VARCHAR(MAX) on the SQL Server backend, a single-byte-collation
    // column). This exercises both the DataMapper-bound write path
    // (CreatePaste) and the raw-prepared-statement write path (EditPaste's
    // compare-and-swap, paste_model.cpp's kEditPasteSql) that binds a
    // Light::SqlMaxDynamicWideString parameter by hand rather than through a
    // Field<>.
    DbFixture fixture;
    pastebin::PasteModel model;

    const std::string original = "héllo wörld — \xE4\xB8\xAD\xE6\x96\x87 \xF0\x9F\x8E\x89";  // Latin-1 + CJK + emoji
    auto create = makeCreate(original, "text");
    create.editability = pastebin::Editability::Editable;
    const auto id = model.execute(create).id;
    CHECK(model.execute(pastebin::GetPaste{.id = id}).content == original);

    const std::string edited = "édité — \xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E";  // Japanese
    model.execute(pastebin::EditPaste{.id = id, .content = edited, .syntax = "text"});
    CHECK(model.execute(pastebin::GetPaste{.id = id}).content == edited);
}

TEST_CASE("CreatePaste's validate() rejects empty content and empty syntax", "[pastebin][model]") {
    DbFixture fixture;
    pastebin::PasteModel model;

    REQUIRE_THROWS_AS(model.execute(makeCreate("", "text")), pastebin::ValidationError);
    REQUIRE_THROWS_AS(model.execute(makeCreate("body", "")), pastebin::ValidationError);
    REQUIRE_THROWS_AS(model.execute(makeCreate("", "")), pastebin::ValidationError);

    // Nothing was stored by any of the three rejections.
    CHECK(model.execute(pastebin::ListPastes{}).pastes.empty());
}

TEST_CASE("CreatePaste's validate() rejects a zero or negative burnAfterReads", "[pastebin][model]") {
    // A budget of 0 is a whole number, so it clears the integrality half of
    // `CreatePaste::validate()`'s burn-budget rule (the case below this one),
    // but PasteModel::execute(GetPaste)'s burn check
    // (`readCount >= *burnAfterReads`) is already true before the first read
    // ever happens — a paste born with burnAfterReads=0 would be permanently
    // Burned on its very first GetPaste, having never been read once. The two
    // halves of the rule are independent and are asserted separately.
    DbFixture fixture;
    pastebin::PasteModel model;

    auto zero = makeCreate("body", "text");
    zero.burnAfterReads = pastebin::Reads::fromDouble(0.0);
    REQUIRE_THROWS_AS(model.execute(zero), pastebin::ValidationError);

    auto negative = makeCreate("body", "text");
    negative.burnAfterReads = pastebin::Reads::fromDouble(-1.0);
    REQUIRE_THROWS_AS(model.execute(negative), pastebin::ValidationError);

    // A positive budget is unaffected by the new check.
    auto positive = makeCreate("body", "text");
    positive.burnAfterReads = pastebin::Reads::fromDouble(1.0);
    REQUIRE_NOTHROW(model.execute(positive));

    // Nothing was stored by either rejection — only the positive create.
    CHECK(model.execute(pastebin::ListPastes{}).pastes.size() == 1);
}

TEST_CASE("CreatePaste's validate() rejects a fractional burnAfterReads", "[pastebin][model]") {
    // The whole-number premise `Reads` documents (`pastebin/units.hpp`) and
    // `paste_model.cpp`'s `countOf` relies on is a *DTO* obligation — the type
    // cannot carry it, because `Quantity` requires `DeclaredDecimals >= 1` and
    // so `Reads` represents one tenth exactly. A fractional budget therefore
    // travels the whole path intact: it survives the wire codec (a `Rational`
    // serialises as its own num/den pair), reaches `validate()`, and — before
    // this case existed — was accepted, then floored to a *different* budget by
    // `countOf`'s `math::floor` on the way into the row. `2.5` became `2`, and
    // `GetPaste` reported `2` back. That is the same silent-data-loss class the
    // `syntax` bound is validated to prevent (`kMaxSyntaxBytes`' own doc
    // comment), arriving through an unguarded door, so the answer is the same:
    // refuse the input rather than quietly rewrite it.
    DbFixture fixture;
    pastebin::PasteModel model;

    auto fractional = makeCreate("body", "text");
    fractional.burnAfterReads = pastebin::Reads::fromDouble(2.5);

    // The premise this whole case rests on: 2.5 really is exactly representable
    // in `Reads`, so nothing upstream of `validate()` has already rejected or
    // rounded it. If `Reads` ever gains a zero declared precision this fails
    // here rather than silently turning the assertions below into tautologies.
    REQUIRE(fractional.burnAfterReads.hasValue());
    REQUIRE_FALSE(fractional.burnAfterReads.value()->isInteger());
    CHECK(fractional.burnAfterReads.value()->numerator == 5);
    CHECK(fractional.burnAfterReads.value()->denominator == 2);

    REQUIRE_THROWS_AS(model.execute(fractional), pastebin::ValidationError);

    // A negative fraction is refused by the integrality rule too, not only by
    // the sign rule — the two checks are independent.
    auto negativeFraction = makeCreate("body", "text");
    negativeFraction.burnAfterReads = pastebin::Reads::fromDouble(-0.5);
    REQUIRE_THROWS_AS(model.execute(negativeFraction), pastebin::ValidationError);

    // The neighbouring whole numbers are both still accepted, so the rejection
    // above is the fractional part and not a blanket refusal of the field.
    for (const double whole : {2.0, 3.0}) {
        auto accepted = makeCreate("body", "text");
        accepted.burnAfterReads = pastebin::Reads::fromDouble(whole);
        REQUIRE_NOTHROW(model.execute(accepted));
    }

    // Nothing was stored by either rejection — only the two whole-number
    // creates. Had 2.5 been accepted and floored, this would read three.
    const auto listed = model.execute(pastebin::ListPastes{}).pastes;
    REQUIRE(listed.size() == 2);
    for (const auto& summary : listed) {
        const auto budget = model.execute(pastebin::GetPaste{.id = summary.id}).burnAfterReads;
        CHECK(budget.hasValue());
        CHECK(budget.value()->isInteger());
    }
}

TEST_CASE("The burn-budget rules reach the served CreatePaste schema", "[pastebin][model][forms]") {
    // `IMPLEMENTATION.md` rule 3: the DTO *is* the form definition, and there
    // is no second source of truth. Until morph#310 there was one here — the
    // three conditions above (>= 1, non-negative, whole) lived only in
    // `validate()`, server-side, with nothing a client could gate on. The form
    // therefore auto-fired into a guaranteed rejection, and the only remaining
    // way to stop it was a hand-written QML conditional, which
    // `examples/TESTING.md` presenter rule 6 forbids.
    //
    // `FieldMeta::minimum`/`::multipleOf` are the vocabulary that closes it:
    // one declaration on the DTO, served as standard JSON-Schema keys and
    // evaluated by the same `validate()` the model already runs.
    auto const schema = morph::forms::schemaJson<pastebin::CreatePaste>();
    auto parsed = glz::read_json<glz::generic>(schema);
    REQUIRE(parsed.has_value());
    // NOLINTBEGIN(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access) -- glaze DOM requires operator[]
    auto const& properties = parsed.value()["properties"];
    REQUIRE(properties.contains("burnAfterReads"));
    auto const& burn = properties["burnAfterReads"];
    // ">= 1" — which is also what refuses 0 and every negative budget.
    REQUIRE(burn.contains("minimum"));
    CHECK(burn["minimum"].get<double>() == 1.0);
    // "is a whole number", in the JSON-Schema spelling.
    REQUIRE(burn.contains("multipleOf"));
    CHECK(burn["multipleOf"].get<double>() == 1.0);
    // Per *field*, not per unit: `PasteView::readCount` is the same `Reads`
    // type over the same `Unit::count` and legitimately starts at 0, which is
    // why `UnitTraits::bounds` could not have carried this.
    auto viewParsed = glz::read_json<glz::generic>(morph::forms::schemaJson<pastebin::PasteView>());
    REQUIRE(viewParsed.has_value());
    auto const& readCount = viewParsed.value()["properties"]["readCount"];
    CHECK_FALSE(readCount.contains("minimum"));
    CHECK_FALSE(readCount.contains("multipleOf"));
    // NOLINTEND(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
}

TEST_CASE("An over-length syntax is rejected, not silently truncated into the column", "[pastebin][model]") {
    // `PasteRecord::syntax` is a `Light::SqlAnsiString<32>`, whose constructor
    // is `_size{std::min(N, s.size())}` — no throw, no diagnostic. Before
    // `kMaxSyntaxBytes` was validated, a 33-byte label was cut to 32 on the way
    // into the row and the client was told the create succeeded, and a cut
    // landing mid-UTF-8-sequence put ill-formed UTF-8 into both the TEXT column
    // and the JSON frame carrying the resulting PasteView back. Both halves are
    // asserted here: the boundary still fits, one byte past it is refused, and
    // nothing was stored by any refusal.
    DbFixture fixture;
    pastebin::PasteModel model;

    static constexpr std::size_t kMax = pastebin::kMaxSyntaxBytes;
    const std::string atLimit(kMax, 'x');
    const std::string overLimit(kMax + 1, 'x');

    // The boundary itself is accepted and round-trips whole — the bound is
    // "<= capacity", not an off-by-one that rejects a label that would fit.
    // Editable, so the EditPaste assertion below is genuinely about the syntax
    // bound and not about `EditPaste: paste is not editable`.
    auto create = makeCreate("at the limit", atLimit);
    create.editability = pastebin::Editability::Editable;
    const auto id = model.execute(create).id;
    CHECK(model.execute(pastebin::GetPaste{.id = id}).syntax == atLimit);

    // One byte past it is a typed rejection, on both actions that write the
    // column.
    REQUIRE_THROWS_AS(model.execute(makeCreate("one too many", overLimit)), pastebin::ValidationError);
    REQUIRE_THROWS_AS(model.execute(pastebin::EditPaste{.id = id, .content = "body", .syntax = overLimit}),
                      pastebin::ValidationError);
    // ... and the still-valid boundary length is accepted by EditPaste too, so
    // the rejection above is the length rule, not a blanket refusal.
    REQUIRE_NOTHROW(model.execute(pastebin::EditPaste{.id = id, .content = "body", .syntax = atLimit}));

    // A multi-byte label whose truncation point falls *inside* a codepoint —
    // the ill-formed-UTF-8 case specifically. Thirty-three 2-byte characters is
    // 66 bytes, so a 32-byte cut would sever the 17th one.
    std::string multiByte;
    for (int i = 0; i < 33; ++i) {
        multiByte += "é";  // U+00E9, two bytes in UTF-8
    }
    REQUIRE(multiByte.size() > kMax);
    REQUIRE_THROWS_AS(model.execute(makeCreate("mid-codepoint", multiByte)), pastebin::ValidationError);

    // Exactly one paste exists: the at-limit one. No refusal wrote a row, and
    // no refused edit changed the one that did.
    const auto listed = model.execute(pastebin::ListPastes{});
    REQUIRE(listed.pastes.size() == 1);
    CHECK(listed.pastes.front().syntax == atLimit);
}

TEST_CASE("CreatePaste round-trips visibility and editability", "[pastebin][model]") {
    DbFixture fixture;
    pastebin::PasteModel model;

    auto create = makeCreate("private and editable");
    create.visibility = pastebin::Visibility::Private;
    create.editability = pastebin::Editability::Editable;
    const auto id = model.execute(create).id;

    const auto view = model.execute(pastebin::GetPaste{.id = id});
    CHECK(view.visibility == pastebin::Visibility::Private);
    CHECK(view.editability == pastebin::Editability::Editable);
}

TEST_CASE("GetPaste returns a freshly created paste and counts the read", "[pastebin][model]") {
    DbFixture fixture;
    pastebin::PasteModel model;
    const auto id = model.execute(makeCreate("secret")).id;

    const auto first = model.execute(pastebin::GetPaste{.id = id});
    CHECK(first.content == "secret");
    CHECK(countOf(first.readCount) == 1);

    const auto second = model.execute(pastebin::GetPaste{.id = id});
    CHECK(second.content == "secret");
    CHECK(countOf(second.readCount) == 2);  // the count is real state, not a per-call constant
}

TEST_CASE("GetPaste against an unknown id throws NotFound, and an empty id is a ValidationError",
          "[pastebin][model]") {
    DbFixture fixture;
    pastebin::PasteModel model;

    REQUIRE_THROWS_AS(model.execute(pastebin::GetPaste{.id = pastebin::PasteId{"no-such-paste"}}), pastebin::NotFound);
    REQUIRE_THROWS_AS(model.execute(pastebin::GetPaste{}), pastebin::ValidationError);
}

TEST_CASE("EditPaste replaces an editable paste's content and syntax", "[pastebin][model]") {
    DbFixture fixture;
    pastebin::PasteModel model;

    auto create = makeCreate("before", "text");
    create.editability = pastebin::Editability::Editable;
    const auto id = model.execute(create).id;

    const auto edited = model.execute(pastebin::EditPaste{.id = id, .content = "after", .syntax = "cpp"});
    CHECK(edited.content == "after");
    CHECK(edited.syntax == "cpp");

    // Persisted, not merely reflected back from the action.
    const auto refetched = model.execute(pastebin::GetPaste{.id = id});
    CHECK(refetched.content == "after");
    CHECK(refetched.syntax == "cpp");
}

TEST_CASE("EditPaste refuses an immutable paste, an unknown id, and an incomplete action", "[pastebin][model]") {
    DbFixture fixture;
    pastebin::PasteModel model;
    const auto id = model.execute(makeCreate("immutable")).id;  // Editability::Immutable by default

    REQUIRE_THROWS_AS(model.execute(pastebin::EditPaste{.id = id, .content = "nope", .syntax = "text"}),
                      pastebin::ValidationError);
    REQUIRE_THROWS_AS(
        model.execute(pastebin::EditPaste{.id = pastebin::PasteId{"ghost"}, .content = "nope", .syntax = "text"}),
        pastebin::NotFound);
    REQUIRE_THROWS_AS(model.execute(pastebin::EditPaste{.id = id, .content = "", .syntax = "text"}),
                      pastebin::ValidationError);
    REQUIRE_THROWS_AS(model.execute(pastebin::EditPaste{.id = id, .content = "body", .syntax = ""}),
                      pastebin::ValidationError);

    // The refused edits left the stored paste untouched.
    CHECK(model.execute(pastebin::GetPaste{.id = id}).content == "immutable");
}

TEST_CASE("A concurrent write between EditPaste's read and its write is a Conflict, not a lost update",
          "[pastebin][model]") {
    // EditPaste used to be a plain read-then-write: whichever caller's
    // UPDATE landed last would silently discard whatever an earlier caller
    // had just written, with no error to either side. The fix makes the
    // write a compare-and-swap (`kEditPasteSql`'s `content = ? AND syntax
    // = ?` guard): the write only applies if the row still holds what this
    // call read.
    //
    // Provoked deterministically — no `sleep_for` (examples/TESTING.md)
    // and no guessing at thread-scheduling order. `WaitForGuardedUpdate`
    // is a `Lightweight::SqlLogger` that fires `OnExecute()` on whatever
    // thread runs a statement, strictly before that statement's actual
    // (and here, blocking) ODBC call — a real hook Lightweight already
    // exposes, not new instrumentation added to PasteModel. It lets the
    // main thread wait on a condition variable for the precise moment
    // `contendedModel`'s guarded UPDATE is about to run — which can only
    // happen after its own `before` SELECT has already completed — before
    // committing a *different* write through a lock held open on a second
    // connection. `contendedModel`'s guarded UPDATE then blocks on that
    // lock; when it is finally released, the guard compares against
    // content that is no longer there.
    class WaitForGuardedUpdate : public ::Lightweight::SqlLogger::Null {
    public:
        void OnExecute(std::string_view const& query) override {
            if (query.find("SET content = ?, syntax = ?") == std::string_view::npos) {
                return;
            }
            {
                const std::lock_guard lock{_mutex};
                _reached = true;
            }
            _cv.notify_all();
        }

        void wait() {
            std::unique_lock lock{_mutex};
            _cv.wait(lock, [this] { return _reached; });
        }

    private:
        std::mutex _mutex;
        std::condition_variable _cv;
        bool _reached = false;
    };

    DbFixture fixture;
    pastebin::PasteModel seedModel;

    auto create = makeCreate("seed", "text");
    create.editability = pastebin::Editability::Editable;
    const auto id = seedModel.execute(create).id;

    // `contendedModel`'s execute() below must acquire a genuinely new pooled
    // connection *while* the short busy-timeout hook is installed for this
    // hook to actually apply to it (see db_busy_fixture.hpp's
    // GlobalDataMapperPool() note above `ScopedShortBusyTimeout`'s own doc
    // comment) — same requirement as the SQLITE_BUSY cases below. Draining
    // the pool's idle mappers first (see drainPoolIdleMappers's own doc
    // comment) turns that into a hard guarantee rather than the "correct in
    // practice, not guaranteed" caveat a shared pool would otherwise leave:
    // held alive across the hook install and the racy execute() below, then
    // released once this test no longer needs a forced-fresh acquisition.
    const ScopedShortBusyTimeout shortTimeout{5000};
    auto drained = drainPoolIdleMappers();
    pastebin::PasteModel contendedModel;

    ::Lightweight::SqlConnection lockingConnection;
    {
        ::Lightweight::SqlStatement stmt{lockingConnection};
        (void)stmt.ExecuteDirect("BEGIN IMMEDIATE");
        (void)stmt.ExecuteDirect("UPDATE pastes SET id = id WHERE id = '" + *id + "'");
    }

    WaitForGuardedUpdate probe;
    ::Lightweight::SqlLogger& previousLogger = ::Lightweight::SqlLogger::GetLogger();
    ::Lightweight::SqlLogger::SetLogger(probe);

    std::optional<pastebin::PasteView> succeeded;
    std::exception_ptr failure;
    std::thread editor{[&] {
        try {
            succeeded = contendedModel.execute(pastebin::EditPaste{.id = id, .content = "mine", .syntax = "text"});
        } catch (...) {
            failure = std::current_exception();
        }
    }};

    // Blocks until `contendedModel`'s guarded UPDATE is about to execute —
    // which is only reachable after its own `before` SELECT has already
    // returned "seed". Only past this point is it safe to commit a
    // different write through the lock: the SELECT is guaranteed done.
    probe.wait();

    {
        ::Lightweight::SqlStatement stmt{lockingConnection};
        (void)stmt.ExecuteDirect("UPDATE pastes SET content = 'concurrent writer' WHERE id = '" + *id + "'");
        (void)stmt.ExecuteDirect("COMMIT");
    }

    editor.join();
    // Safe to stop forcing fresh acquisitions now: contendedModel's one and
    // only execute() call (and so its one pool acquisition) already
    // happened, inside the joined editor thread above.
    drained.clear();
    // Restored only after the editor thread is done issuing statements —
    // `probe` must not be touched by another thread once it goes out of
    // scope below.
    ::Lightweight::SqlLogger::SetLogger(previousLogger);

    REQUIRE_FALSE(succeeded.has_value());
    REQUIRE(failure);
    bool sawConflict = false;
    try {
        std::rethrow_exception(failure);
    } catch (const pastebin::Conflict&) {
        sawConflict = true;
    } catch (...) {
        // Falls through to the REQUIRE below with sawConflict still false.
    }
    REQUIRE(sawConflict);

    // Not a lost update: the concurrent writer's content survived, untouched
    // by the rejected edit.
    CHECK(seedModel.execute(pastebin::GetPaste{.id = id}).content == "concurrent writer");
}

TEST_CASE(
    "A concurrent delete between EditPaste's read and its guarded write is a NotFound, "
    "not a lost update or a Conflict",
    "[pastebin][model]") {
    // Same forced-interleaving idiom as "A concurrent write between
    // EditPaste's read and its write is a Conflict" above, but the
    // concurrent writer deletes the row outright instead of editing its
    // content. This exercises EditPaste's *post-CAS-miss* classification
    // path (paste_model.cpp's "Zero rows matched: classify why" block) --
    // distinct from the earlier, pre-CAS existing.empty() check the "refuses
    // an unknown id" test above already covers, since that one never reaches
    // the guarded UPDATE at all (the row was never there to begin with). This
    // one has the row present and readable at EditPaste's first SELECT, and
    // only disappears in the window the CAS UPDATE itself is blocked in.
    class WaitForGuardedUpdate : public ::Lightweight::SqlLogger::Null {
    public:
        void OnExecute(std::string_view const& query) override {
            if (query.find("SET content = ?, syntax = ?") == std::string_view::npos) {
                return;
            }
            {
                const std::lock_guard lock{_mutex};
                _reached = true;
            }
            _cv.notify_all();
        }

        void wait() {
            std::unique_lock lock{_mutex};
            _cv.wait(lock, [this] { return _reached; });
        }

    private:
        std::mutex _mutex;
        std::condition_variable _cv;
        bool _reached = false;
    };

    DbFixture fixture;
    pastebin::PasteModel seedModel;

    auto create = makeCreate("about to vanish", "text");
    create.editability = pastebin::Editability::Editable;
    const auto id = seedModel.execute(create).id;

    const ScopedShortBusyTimeout shortTimeout{5000};
    auto drained = drainPoolIdleMappers();
    pastebin::PasteModel contendedModel;

    ::Lightweight::SqlConnection lockingConnection;
    {
        ::Lightweight::SqlStatement stmt{lockingConnection};
        (void)stmt.ExecuteDirect("BEGIN IMMEDIATE");
        (void)stmt.ExecuteDirect("UPDATE pastes SET id = id WHERE id = '" + *id + "'");
    }

    WaitForGuardedUpdate probe;
    ::Lightweight::SqlLogger& previousLogger = ::Lightweight::SqlLogger::GetLogger();
    ::Lightweight::SqlLogger::SetLogger(probe);

    std::optional<pastebin::PasteView> succeeded;
    std::exception_ptr failure;
    std::thread editor{[&] {
        try {
            succeeded = contendedModel.execute(pastebin::EditPaste{.id = id, .content = "mine", .syntax = "text"});
        } catch (...) {
            failure = std::current_exception();
        }
    }};

    probe.wait();

    {
        ::Lightweight::SqlStatement stmt{lockingConnection};
        (void)stmt.ExecuteDirect("DELETE FROM pastes WHERE id = '" + *id + "'");
        (void)stmt.ExecuteDirect("COMMIT");
    }

    editor.join();
    drained.clear();
    ::Lightweight::SqlLogger::SetLogger(previousLogger);

    REQUIRE_FALSE(succeeded.has_value());
    REQUIRE(failure);
    bool sawNotFound = false;
    try {
        std::rethrow_exception(failure);
    } catch (const pastebin::NotFound&) {
        sawNotFound = true;
    } catch (...) {
        // Falls through to the REQUIRE below with sawNotFound still false.
    }
    REQUIRE(sawNotFound);
}

TEST_CASE(
    "A concurrent DeletePaste is not the only way to reach EditPaste's post-CAS \"not editable\" "
    "classification, but flipping is_editable underneath a pending edit reaches it too",
    "[pastebin][model]") {
    // Mirrors the delete case above, but the concurrent writer clears
    // is_editable instead of removing the row -- the other branch of the
    // same "Zero rows matched: classify why" block (paste_model.cpp).
    // is_editable has no ordinary action that flips it after creation (only
    // CreatePaste sets it, permanently, in this rung), so this reaches into
    // the row directly through the locking connection, the same way the
    // Conflict/NotFound tests above simulate "some other write landed" --
    // there is no in-API way to un-edit a paste, which is exactly why this
    // classification branch has no other route to it.
    class WaitForGuardedUpdate : public ::Lightweight::SqlLogger::Null {
    public:
        void OnExecute(std::string_view const& query) override {
            if (query.find("SET content = ?, syntax = ?") == std::string_view::npos) {
                return;
            }
            {
                const std::lock_guard lock{_mutex};
                _reached = true;
            }
            _cv.notify_all();
        }

        void wait() {
            std::unique_lock lock{_mutex};
            _cv.wait(lock, [this] { return _reached; });
        }

    private:
        std::mutex _mutex;
        std::condition_variable _cv;
        bool _reached = false;
    };

    DbFixture fixture;
    pastebin::PasteModel seedModel;

    auto create = makeCreate("about to be locked", "text");
    create.editability = pastebin::Editability::Editable;
    const auto id = seedModel.execute(create).id;

    const ScopedShortBusyTimeout shortTimeout{5000};
    auto drained = drainPoolIdleMappers();
    pastebin::PasteModel contendedModel;

    ::Lightweight::SqlConnection lockingConnection;
    {
        ::Lightweight::SqlStatement stmt{lockingConnection};
        (void)stmt.ExecuteDirect("BEGIN IMMEDIATE");
        (void)stmt.ExecuteDirect("UPDATE pastes SET id = id WHERE id = '" + *id + "'");
    }

    WaitForGuardedUpdate probe;
    ::Lightweight::SqlLogger& previousLogger = ::Lightweight::SqlLogger::GetLogger();
    ::Lightweight::SqlLogger::SetLogger(probe);

    std::optional<pastebin::PasteView> succeeded;
    std::exception_ptr failure;
    std::thread editor{[&] {
        try {
            succeeded = contendedModel.execute(pastebin::EditPaste{.id = id, .content = "mine", .syntax = "text"});
        } catch (...) {
            failure = std::current_exception();
        }
    }};

    probe.wait();

    {
        ::Lightweight::SqlStatement stmt{lockingConnection};
        (void)stmt.ExecuteDirect("UPDATE pastes SET is_editable = 0 WHERE id = '" + *id + "'");
        (void)stmt.ExecuteDirect("COMMIT");
    }

    editor.join();
    drained.clear();
    ::Lightweight::SqlLogger::SetLogger(previousLogger);

    REQUIRE_FALSE(succeeded.has_value());
    REQUIRE(failure);
    bool sawValidationError = false;
    try {
        std::rethrow_exception(failure);
    } catch (const pastebin::ValidationError&) {
        sawValidationError = true;
    } catch (...) {
        // Falls through to the REQUIRE below with sawValidationError still false.
    }
    REQUIRE(sawValidationError);
}

TEST_CASE("DeletePaste removes the paste, and a follow-up GetPaste throws NotFound", "[pastebin][model]") {
    DbFixture fixture;
    pastebin::PasteModel model;
    const auto id = model.execute(makeCreate("doomed")).id;

    REQUIRE_NOTHROW(model.execute(pastebin::DeletePaste{.id = id}));
    REQUIRE_THROWS_AS(model.execute(pastebin::GetPaste{.id = id}), pastebin::NotFound);

    // Deleting an absent paste is a no-op acknowledgement, not an error —
    // the operation is idempotent by design.
    REQUIRE_NOTHROW(model.execute(pastebin::DeletePaste{.id = id}));
    REQUIRE_THROWS_AS(model.execute(pastebin::DeletePaste{}), pastebin::ValidationError);
}

TEST_CASE("ListPastes returns only public pastes, one page at a time, and its cursor round-trips",
          "[pastebin][model]") {
    DbFixture fixture;
    pastebin::PasteModel model;

    constexpr int kPublic = 25;  // one full 20-row page plus a partial second one
    constexpr int kPrivate = 3;
    std::vector<pastebin::PasteId> publicIds;
    for (int i = 0; i < kPublic; ++i) {
        publicIds.push_back(model.execute(makeCreate("public " + std::to_string(i))).id);
    }
    std::vector<pastebin::PasteId> privateIds;
    for (int i = 0; i < kPrivate; ++i) {
        auto create = makeCreate("private " + std::to_string(i));
        create.visibility = pastebin::Visibility::Private;
        privateIds.push_back(model.execute(create).id);
    }

    const auto page1 = model.execute(pastebin::ListPastes{});
    REQUIRE(page1.pastes.size() == 20);
    REQUIRE(page1.nextCursor.hasValue());

    const auto page2 = model.execute(pastebin::ListPastes{.cursor = page1.nextCursor});
    REQUIRE(page2.pastes.size() == static_cast<std::size_t>(kPublic - 20));
    CHECK_FALSE(page2.nextCursor.hasValue());  // exhausted — no third page

    std::vector<pastebin::PasteId> walked;
    for (const auto& summary : page1.pastes) {
        walked.push_back(summary.id);
    }
    for (const auto& summary : page2.pastes) {
        walked.push_back(summary.id);
    }

    // Every public paste exactly once, no private paste at all.
    std::ranges::sort(walked);
    CHECK(std::ranges::adjacent_find(walked) == walked.end());  // no overlap between the two pages
    CHECK(walked.size() == static_cast<std::size_t>(kPublic));
    for (const auto& id : publicIds) {
        CHECK(std::ranges::find(walked, id) != walked.end());
    }
    for (const auto& id : privateIds) {
        CHECK(std::ranges::find(walked, id) == walked.end());
    }

    // A summary is deliberately narrower than a view: it carries no content.
    CHECK(page1.pastes.front().syntax == "text");
    CHECK(page1.pastes.front().visibility == pastebin::Visibility::Public);
}

TEST_CASE("ListPastes does not consume a read budget — listing is not reading", "[pastebin][model]") {
    DbFixture fixture;
    pastebin::PasteModel model;

    auto create = makeCreate("listed but unread");
    create.burnAfterReads = pastebin::Reads::fromDouble(1.0);
    const auto id = model.execute(create).id;

    REQUIRE(model.execute(pastebin::ListPastes{}).pastes.size() == 1);
    REQUIRE(model.execute(pastebin::ListPastes{}).pastes.size() == 1);

    // The one allowed read is still available.
    CHECK(model.execute(pastebin::GetPaste{.id = id}).content == "listed but unread");
}

// ═════════════════════════════════════════════════════════════════════════
// Step 2 — burn-after-read semantics, single client
// ═════════════════════════════════════════════════════════════════════════

TEST_CASE("GetPaste spends the burn budget and deletes the paste on the last allowed read", "[pastebin][model]") {
    DbFixture fixture;
    pastebin::PasteModel model;

    auto create = makeCreate("secret");
    create.burnAfterReads = pastebin::Reads::fromDouble(2.0);
    const auto id = model.execute(create).id;

    const auto first = model.execute(pastebin::GetPaste{.id = id});
    CHECK(first.content == "secret");
    CHECK(countOf(first.readCount) == 1);

    // Read 2 of 2 still returns the content: burn-after-read destroys the
    // paste *on* the Nth read, after building the result — not before it.
    const auto second = model.execute(pastebin::GetPaste{.id = id});
    CHECK(second.content == "secret");
    CHECK(countOf(second.readCount) == 2);

    REQUIRE_THROWS_AS(model.execute(pastebin::GetPaste{.id = id}), pastebin::NotFound);
}

TEST_CASE("GetPaste against a row already at its burn budget throws Burned, not NotFound", "[pastebin][model]") {
    // Seeds the row directly at the storage layer with read_count already at
    // burn_after_reads, bypassing the delete-on-last-read step that would
    // normally have removed it. This is the "conditional UPDATE matched zero
    // rows, and the row still exists" classification branch — reachable no
    // other way from the model's own API.
    //
    // It is also the *only* case in this suite that pins the burn clause of
    // `kConsumeReadSql`'s `WHERE` on its own: with that clause deleted, this
    // read matches the row, increments past the budget, and hands back
    // content that was already spent. Verified by doing exactly that. See the
    // concurrent case below for why the socket race does not catch it on
    // SQLite, and why the two belong together.
    DbFixture fixture;
    {
        Lightweight::DataMapper mapper;
        pastebin::db::PasteRecord rec;
        rec.id = Light::SqlAnsiString<32>{"test-burned-paste"};
        rec.content = Light::SqlMaxDynamicWideString{L"gone"};
        rec.syntax = Light::SqlAnsiString<32>{"text"};
        rec.createdAtMs = std::int64_t{0};
        rec.burnAfterReads = std::optional<std::int64_t>{1};
        rec.readCount = std::int64_t{1};  // already at budget
        mapper.Create(rec);
    }

    pastebin::PasteModel model;
    REQUIRE_THROWS_AS(model.execute(pastebin::GetPaste{.id = pastebin::PasteId{"test-burned-paste"}}),
                      pastebin::Burned);
}

TEST_CASE("GetPaste against a row that is both burn-exhausted and still-future-expiring throws Burned",
          "[pastebin][model]") {
    // Same "conditional UPDATE matched zero rows, and the row still exists"
    // classification branch as the burned-row test above, but with
    // `expiresAtMs` engaged and still in the future rather than disengaged.
    // That combination is what forces the classifier's expiry check
    // (`row.expiresAtMs.Value() && *row.expiresAtMs.Value() <= readAtMs`) to
    // evaluate its first operand true and its second false -- the row *has*
    // an expiry, it just has not arrived yet -- so the check falls through to
    // the burn check below it instead of short-circuiting on a disengaged
    // `expiresAtMs`, as the burned-row test above does. A row that is merely
    // burn-exhausted (not also expiring) cannot reach this arm: the atomic
    // UPDATE's own `WHERE` only excludes an *already-past* expiry, so an
    // engaged-but-future `expiresAtMs` never changes whether the UPDATE
    // matches -- the burn guard alone is what sends this row to the
    // classifier, and the still-future expiry is what makes the classifier
    // itself walk both operands of its own `&&` instead of stopping at a
    // null check.
    DbFixture fixture;
    {
        Lightweight::DataMapper mapper;
        pastebin::db::PasteRecord rec;
        rec.id = Light::SqlAnsiString<32>{"test-burned-not-expired-paste"};
        rec.content = Light::SqlMaxDynamicWideString{L"gone"};
        rec.syntax = Light::SqlAnsiString<32>{"text"};
        rec.createdAtMs = std::int64_t{0};
        rec.expiresAtMs = toEpochMs(nowPlus(std::chrono::hours{1}));
        rec.burnAfterReads = std::optional<std::int64_t>{1};
        rec.readCount = std::int64_t{1};  // already at budget
        mapper.Create(rec);
    }

    pastebin::PasteModel model;
    REQUIRE_THROWS_AS(model.execute(pastebin::GetPaste{.id = pastebin::PasteId{"test-burned-not-expired-paste"}}),
                      pastebin::Burned);
}

// ═════════════════════════════════════════════════════════════════════════
// Step 3 — burn atomicity under genuine socket concurrency
// ═════════════════════════════════════════════════════════════════════════

TEST_CASE("BackendRig::Socket: concurrent GetPaste against a burn-after-N paste — exactly N clients win",
          "[pastebin][model][socket-only]") {
    // The end-to-end regression test for the burn-after-read guarantee under
    // genuine concurrency: N clients, each on its own socket, its own model
    // instance, its own strand and its own database connection, all reading
    // one burn-after-N paste with nothing awaited until every call is issued.
    // Exactly N of them may ever see the content, no matter how the four
    // dispatches interleave. Budgets 1..3 are all exercised, because the
    // interesting boundary (the last allowed read, which both returns content
    // *and* destroys the paste) sits at a different client each time.
    //
    // Two honesty notes, both established empirically by rebuilding the model
    // with its guard deliberately broken and re-running this case:
    //
    // * This case does *not*, on SQLite, discriminate the conditional
    //   `UPDATE`'s `read_count < burn_after_reads` clause. Deleting that
    //   clause outright leaves this case passing, because SQLite serializes
    //   writers: a losing client's `UPDATE` cannot interleave with the
    //   winner's transaction, and by the time it runs the winner has already
    //   committed the burn-delete, so it matches no row and the client gets
    //   `NotFound` anyway. The clause is what keeps that true on a store with
    //   row-level locking or MVCC, and the case that pins it directly is
    //   "GetPaste against a row already at its burn budget throws Burned" —
    //   deleting the clause fails *that* case immediately. Read the two
    //   together; neither alone covers the guarantee.
    //
    //   The residual gap that leaves, stated plainly for whoever next touches
    //   `execute(GetPaste)`: **nothing in this suite would catch the atomic
    //   `UPDATE ... WHERE read_count < burn_after_reads` being refactored into
    //   a separate check-then-act (a `SELECT` of the budget, then an
    //   unguarded `UPDATE`).** That refactor keeps *both* cases green on
    //   SQLite — the `Burned` case because the pre-check rejects the read just
    //   as the `WHERE` clause did, and this case because losing clients still
    //   find the row already deleted, whether the winner's check-then-act was
    //   genuinely atomic or merely got lucky with SQLite's write
    //   serialization. It only becomes observably wrong under a store with
    //   real row-level locking/MVCC contention windows (Postgres), which this
    //   rung does not test against. So: keep the check inside the `UPDATE`.
    //   The tests will not tell you if you move it out.
    //
    // * What this case genuinely does cover is everything above the SQL: that
    //   the whole stack — four sockets, four strands, four connections, the
    //   transaction, the read-back and the burn-delete — composes into the
    //   invariant the README promises, with no client ever handed content
    //   belonging to a spent budget, and no client left hanging.
    DbFixture fixture;
    pastebin::PasteModel seedModel;

    constexpr std::size_t kClients = 4;
    constexpr int kRounds = 12;
    BackendRig rig{Mode::Socket, kClients};

    // BridgeHandler is neither copyable nor movable, so the handlers are
    // named locals rather than a vector. Held for the whole case: registering
    // once per client (not once per round) keeps each client's model instance
    // — and therefore its database connection — alive across the rounds,
    // which is what makes the rounds cheap enough to run many of.
    auto handler0 = rig.client<pastebin::PasteModel>(0);
    auto handler1 = rig.client<pastebin::PasteModel>(1);
    auto handler2 = rig.client<pastebin::PasteModel>(2);
    auto handler3 = rig.client<pastebin::PasteModel>(3);
    const std::array<morph::bridge::BridgeHandler<pastebin::PasteModel>*, kClients> handlers{&handler0, &handler1,
                                                                                             &handler2, &handler3};

    struct Tally {
        std::atomic<int> successes{0};
        std::atomic<int> failures{0};
        std::atomic<int> wrongContent{0};
    };

    for (int budget = 1; budget <= 3; ++budget) {
        CAPTURE(budget);
        for (int round = 0; round < kRounds; ++round) {
            CAPTURE(round);
            const std::string content = "budget " + std::to_string(budget) + ", round " + std::to_string(round);
            auto create = makeCreate(content);
            create.burnAfterReads = pastebin::Reads::fromDouble(static_cast<double>(budget));
            const auto id = seedModel.execute(create).id;

            // Heap-allocated (and captured by value) rather than a stack
            // local: if the pump below ever timed out, a late callback would
            // otherwise write through a dangling reference — the same
            // reasoning `pump.hpp`'s `awaitQt` documents.
            auto tally = std::make_shared<Tally>();

            // Every call is issued before any of them is awaited — that is
            // the race-provoking property this case exists for.
            for (auto* handler : handlers) {
                handler->execute(pastebin::GetPaste{.id = id})
                    .then([tally, content](pastebin::PasteView view) {
                        if (view.content != content) {
                            tally->wrongContent.fetch_add(1);
                        }
                        tally->successes.fetch_add(1);
                    })
                    .onError([tally](const std::exception_ptr&) { tally->failures.fetch_add(1); });
            }

            REQUIRE(pumpUntil(
                [tally] { return tally->successes.load() + tally->failures.load() == static_cast<int>(kClients); }));
            // Exactly `budget` clients get the content — never one more, no
            // matter how the four dispatches interleave.
            REQUIRE(tally->successes.load() == budget);
            REQUIRE(tally->failures.load() == static_cast<int>(kClients) - budget);
            REQUIRE(tally->wrongContent.load() == 0);

            // And the paste really is gone afterwards, for everyone.
            REQUIRE_THROWS_AS(seedModel.execute(pastebin::GetPaste{.id = id}), pastebin::NotFound);
        }
    }
}

// ═════════════════════════════════════════════════════════════════════════
// Step 4 — expiry, driven by the injectable clock rather than by sleeping
// ═════════════════════════════════════════════════════════════════════════

TEST_CASE("A paste past its expiresAt throws Expired from GetPaste, before any sweep runs", "[pastebin][model]") {
    DbFixture fixture;
    pastebin::PasteModel model;

    auto create = makeCreate("expiring");
    create.expiresAt = morph::ladder::now();
    const auto id = model.execute(create).id;

    // No sweep is involved: `GetPaste`'s own conditional UPDATE excludes the
    // expired row, which is exactly what makes correctness independent of
    // sweep timing (README, "How does expiry replay?").
    const morph::ladder::ScopedClockOverride later{nowPlus(std::chrono::hours{1})};
    REQUIRE_THROWS_AS(model.execute(pastebin::GetPaste{.id = id}), pastebin::Expired);

    // Repeatable: a failed read consumes nothing, so the same error comes back.
    REQUIRE_THROWS_AS(model.execute(pastebin::GetPaste{.id = id}), pastebin::Expired);
}

TEST_CASE("Expiry edges: an expiresAt at the epoch, and one already in the past at creation time",
          "[pastebin][model]") {
    DbFixture fixture;
    pastebin::PasteModel model;

    // The epoch is a legal instant, not a sentinel for "no expiry" — that is
    // what a disengaged `Timestamp` means. A paste stamped with it is simply
    // long expired.
    auto atEpoch = makeCreate("epoch");
    atEpoch.expiresAt = ::morph::time::Timestamp{
        ::morph::time::DateTime{std::chrono::sys_time<std::chrono::milliseconds>{std::chrono::milliseconds{0}}}};
    const auto epochId = model.execute(atEpoch).id;
    REQUIRE_THROWS_AS(model.execute(pastebin::GetPaste{.id = epochId}), pastebin::Expired);

    auto inThePast = makeCreate("already stale");
    inThePast.expiresAt = ::morph::time::Timestamp{nowPlus(-std::chrono::hours{1})};
    const auto staleId = model.execute(inThePast).id;
    REQUIRE_THROWS_AS(model.execute(pastebin::GetPaste{.id = staleId}), pastebin::Expired);

    // A still-future expiry is untouched by any of this.
    auto live = makeCreate("still live");
    live.expiresAt = ::morph::time::Timestamp{nowPlus(std::chrono::hours{1})};
    const auto liveId = model.execute(live).id;
    CHECK(model.execute(pastebin::GetPaste{.id = liveId}).content == "still live");
}

TEST_CASE("A malformed expiresAt on the wire is a decode error, never a clamped value", "[pastebin][model]") {
    // The third expiry edge the README requires, and the one the two cases
    // above cannot reach: past and epoch are *values*, but "malformed" is not
    // representable as a `Timestamp` at all, so it can only be exercised where
    // the wire text is still text — the action codec
    // (`ActionTraits<CreatePaste>::fromJson`, which is what
    // `Bridge`/`RemoteServer` call on an execute envelope's `body`). No
    // `DbFixture` is needed: a malformed action must be rejected before any
    // model, transaction or row is involved.
    using Traits = ::morph::model::ActionTraits<pastebin::CreatePaste>;

    // Positive control first, in exactly the wire shape the negatives use, so
    // none of them can pass for an unrelated reason (a rejected sibling field,
    // a changed key name). A well-formed instant decodes, and `null` is the
    // legal "never expires" encoding of a disengaged `Timestamp`.
    const auto wellFormed =
        Traits::fromJson(R"({"content":"x","syntax":"text","expiresAt":"2026-08-06T12:30:15.000Z"})");
    REQUIRE(wellFormed.expiresAt.hasValue());
    CHECK((*wellFormed.expiresAt).toIso8601() == "2026-08-06T12:30:15.000Z");
    CHECK_FALSE(Traits::fromJson(R"({"content":"x","syntax":"text","expiresAt":null})").expiresAt.hasValue());

    // Every one of these must throw rather than yield a `CreatePaste` at all.
    // The failure mode being pinned is silent coercion: a decoder that shrugged
    // and left `expiresAt` disengaged would turn "expires at a time I got
    // wrong" into "never expires" — a paste that outlives its author's intent
    // with no error anywhere — and one that rounded 2026-02-30 forward to
    // March 2nd, or read "T-5:30:15" as a negative hour, would shift the
    // instant to a *different valid* one just as silently.
    const auto malformed = GENERATE(as<std::string_view>{},
                                    R"("garbage")",                   // not a date in any format
                                    R"("")",                          // empty string
                                    R"("2026-08-06")",                // date with no clock part
                                    R"("2026-02-30T00:00:00.000Z")",  // date that does not exist
                                    R"("2026-08-06T-5:30:15Z")",      // sign injection into the hour
                                    R"("2026-08-06t12:30:15Z")",      // lowercase separator
                                    R"(1754483415000)",               // epoch millis, not an ISO string
                                    R"(true)");                       // wrong JSON type entirely
    CAPTURE(malformed);
    const auto body = std::string{R"({"content":"x","syntax":"text","expiresAt":)"} + std::string{malformed} + "}";
    CHECK_THROWS_AS(Traits::fromJson(body), ::morph::model::detail::ParseError);
}

TEST_CASE("ExpirePaste reclaims only a genuinely expired paste, so replaying it is safe", "[pastebin][model]") {
    DbFixture fixture;
    pastebin::PasteModel model;

    auto live = makeCreate("not expired");
    live.expiresAt = ::morph::time::Timestamp{nowPlus(std::chrono::hours{1})};
    const auto liveId = model.execute(live).id;

    auto neverExpires = makeCreate("no expiry at all");
    const auto eternalId = model.execute(neverExpires).id;

    // Replaying the journaled entry against pastes that are not (or not yet)
    // expired must delete nothing — the payload carries only the id, so the
    // guard has to live in the statement.
    REQUIRE_NOTHROW(model.execute(pastebin::ExpirePaste{.id = liveId}));
    REQUIRE_NOTHROW(model.execute(pastebin::ExpirePaste{.id = eternalId}));
    CHECK(model.execute(pastebin::GetPaste{.id = liveId}).content == "not expired");
    CHECK(model.execute(pastebin::GetPaste{.id = eternalId}).content == "no expiry at all");

    REQUIRE_THROWS_AS(model.execute(pastebin::ExpirePaste{}), pastebin::ValidationError);

    {
        const morph::ladder::ScopedClockOverride later{nowPlus(std::chrono::hours{2})};
        REQUIRE_NOTHROW(model.execute(pastebin::ExpirePaste{.id = liveId}));
        REQUIRE_THROWS_AS(model.execute(pastebin::GetPaste{.id = liveId}), pastebin::NotFound);
        // Still nothing to reclaim for the paste that never expires.
        REQUIRE_NOTHROW(model.execute(pastebin::ExpirePaste{.id = eternalId}));
        CHECK(model.execute(pastebin::GetPaste{.id = eternalId}).content == "no expiry at all");
    }

    // Replaying the entry a second time, after the paste is already gone, is
    // still an acknowledgement rather than an error.
    REQUIRE_NOTHROW(model.execute(pastebin::ExpirePaste{.id = liveId}));
}

TEST_CASE("App's periodic sweep dispatches ExpirePaste for a past-expiry paste, and it is gone afterward",
          "[pastebin][app]") {
    DbFixture fixture;
    pastebin::PasteModel model;

    auto create = makeCreate("to be swept");
    create.expiresAt = morph::ladder::now();
    const auto sweptId = model.execute(create).id;
    const auto survivorId = model.execute(makeCreate("no expiry")).id;

    const morph::ladder::ScopedClockOverride later{nowPlus(std::chrono::hours{1})};

    const auto logPath = std::filesystem::temp_directory_path() / "pastebin_sweep_test.jsonl";
    std::filesystem::remove(logPath);
    {
        // A one-hour interval effectively disables the timer; the pass is
        // driven directly instead, so nothing here depends on wall-clock
        // timing. `App` reaches the same database this test does because both
        // go through Lightweight's process-global default connection string,
        // which `DbFixture` (constructed above, before `App`) already set.
        pastebin::app::App app{logPath, std::chrono::hours{1}};
        app.sweepExpiredOnce();

        // The sweep dispatches fire-and-forget through its internal client, so
        // the effect is observed by pumping rather than by the call returning.
        REQUIRE(pumpUntil([&] {
            try {
                (void)model.execute(pastebin::GetPaste{.id = sweptId});
                return false;  // still there
            } catch (const pastebin::NotFound&) {
                return true;  // reclaimed
            } catch (const pastebin::PastebinError&) {
                return false;  // Expired: found but not yet swept
            }
        }));
        // The rows being gone is not the same as the dispatches having
        // settled — see App::sweepInFlight(). Settle before letting the App
        // go, or its completion callbacks outlive it.
        REQUIRE(pumpUntil([&] { return !app.sweepInFlight(); }));
    }
    std::filesystem::remove(logPath);

    // The sweep is targeted: an unexpiring paste is untouched by it.
    CHECK(model.execute(pastebin::GetPaste{.id = survivorId}).content == "no expiry");
}

TEST_CASE("A sweep firing between two pages of a ListPastes cursor walk skips no surviving paste", "[pastebin][app]") {
    // Keyset pagination on the primary key is what makes this safe: the
    // cursor is the previous page's last id, so rows reclaimed mid-walk
    // cannot shift a later page's offset the way LIMIT/OFFSET would.
    DbFixture fixture;
    pastebin::PasteModel model;

    constexpr int kSurvivors = 25;
    constexpr int kDoomed = 10;
    std::vector<pastebin::PasteId> survivors;
    for (int i = 0; i < kSurvivors; ++i) {
        survivors.push_back(model.execute(makeCreate("survivor " + std::to_string(i))).id);
    }
    // Scattered among them (ids are random, so their ranks interleave), the
    // pastes the sweep will reclaim halfway through the walk.
    for (int i = 0; i < kDoomed; ++i) {
        auto doomed = makeCreate("doomed " + std::to_string(i));
        doomed.expiresAt = morph::ladder::now();
        (void)model.execute(doomed);
    }
    REQUIRE(pasteRowCount() == kSurvivors + kDoomed);

    const morph::ladder::ScopedClockOverride later{nowPlus(std::chrono::hours{1})};

    const auto page1 = model.execute(pastebin::ListPastes{});
    REQUIRE(page1.pastes.size() == 20);
    REQUIRE(page1.nextCursor.hasValue());

    const auto logPath = std::filesystem::temp_directory_path() / "pastebin_sweep_paging_test.jsonl";
    std::filesystem::remove(logPath);
    std::vector<pastebin::PasteId> walked;
    {
        pastebin::app::App app{logPath, std::chrono::hours{1}};
        app.sweepExpiredOnce();
        // The whole sweep lands between the two pages — the most disruptive
        // moment it could possibly fire.
        REQUIRE(pumpUntil([&] { return pasteRowCount() == kSurvivors; }));
        REQUIRE(pumpUntil([&] { return !app.sweepInFlight(); }));

        for (const auto& summary : page1.pastes) {
            walked.push_back(summary.id);
        }
        const auto page2 = model.execute(pastebin::ListPastes{.cursor = page1.nextCursor});
        for (const auto& summary : page2.pastes) {
            walked.push_back(summary.id);
        }
    }
    std::filesystem::remove(logPath);

    // Every survivor appears exactly once across the two pages: none was
    // skipped by rows vanishing underneath the walk, and none was served
    // twice. (Page 1 may still name reclaimed pastes — it was read before the
    // sweep — which is staleness, not a paging defect.)
    std::ranges::sort(walked);
    CHECK(std::ranges::adjacent_find(walked) == walked.end());
    for (const auto& id : survivors) {
        CHECK(std::ranges::find(walked, id) != walked.end());
    }
}

// ═════════════════════════════════════════════════════════════════════════
// Step 5 — duplicate create on retry (this rung's honest, weaker behavior)
// ═════════════════════════════════════════════════════════════════════════

TEST_CASE("Two CreatePaste calls with identical content mint two distinct pastes at this rung", "[pastebin][model]") {
    // Documents a known limitation rather than a guarantee. The README's
    // "duplicate create on retry" bullet points at idempotency-key discipline,
    // but rung 1's `CreatePaste` has no such key — LADDER.md scopes
    // exactly-once delivery to rung 4, and the fault-injection proxy that
    // could stage a genuine lost reply frame does not exist yet either. So
    // today two identical creates really are two pastes, and this asserts
    // that plainly: the day rung 4's idempotency discipline lands here, this
    // case fails loudly and gets updated alongside the comment, instead of
    // silently drifting into a guarantee nobody implemented.
    DbFixture fixture;
    pastebin::PasteModel model;

    const auto create = makeCreate("resent");
    const auto first = model.execute(create).id;
    const auto second = model.execute(create).id;

    CHECK(first != second);
    CHECK(model.execute(pastebin::ListPastes{}).pastes.size() == 2);
}

// ═════════════════════════════════════════════════════════════════════════
// Step 6 — id-collision handling in the tiny animal-name keyspace
// ═════════════════════════════════════════════════════════════════════════

TEST_CASE("CreatePaste retries past colliding animal-name ids instead of failing the call", "[pastebin][model]") {
    // ── Why this case cannot be flaky ───────────────────────────────────────
    // Every id `CreatePaste` tries here is written down below, in order, and
    // handed to it through `pastebin::ScopedPasteIdSource`. No random number
    // is drawn while that guard is alive, so there is no distribution to
    // sample, no seed to reproduce, and no outcome that can differ between
    // runs: the retry path is taken on every run, and the case can only fail
    // if the retry logic itself is wrong.
    //
    // The previous version occupied a quarter of the keyspace and let the real
    // generator roll (morph#365). That made an eight-attempt exhaustion a
    // 1-in-1,640 event across its 40 creates — a red build on an unrelated PR
    // that often, unreproducible when it fired, because `randomPasteId()`
    // seeds from `std::random_device` and Catch2's `--rng-seed` cannot reach
    // it. Its odds were the thing under test; here the retry *logic* is. The
    // collisions are a real primary-key violation from the real store either
    // way — the seam supplies the candidate id, not the verdict on it.
    DbFixture fixture;
    pastebin::PasteModel model;

    // One stored paste whose id every scripted collision below lands on.
    const std::string taken = "gold-fox-7";
    {
        pastebin::ScopedPasteIdSource occupy{constantIdSource(taken)};
        REQUIRE(*model.execute(makeCreate("already here")).id == taken);
    }

    // Three collisions, then a free id. Exactly four ids are drawn: fewer
    // would mean an attempt was skipped, more that the loop kept going past
    // the insert that succeeded.
    ScriptedIds script{{taken, taken, taken, "keen-owl-3"}};
    {
        pastebin::ScopedPasteIdSource scripted{script.source()};
        pastebin::CreatePasteResult result;
        REQUIRE_NOTHROW(result = model.execute(makeCreate("minted after three collisions")));
        CHECK(*result.id == "keen-owl-3");
    }
    CHECK(script.drawn() == 4);

    // The retries went *past* the occupied row rather than through it: the
    // paste stored under the colliding id is still the original one.
    CHECK(model.execute(pastebin::GetPaste{.id = pastebin::PasteId{taken}}).content == "already here");
    CHECK(model.execute(pastebin::GetPaste{.id = pastebin::PasteId{"keen-owl-3"}}).content ==
          "minted after three collisions");

    // The seam is scoped, and production is its no-override path: with every
    // guard destroyed, the next create mints from the built-in random
    // generator again — an id nothing scripted, spelled from the real
    // keyspace.
    const auto unscripted = model.execute(makeCreate("back on the real generator")).id;
    CHECK(*unscripted != taken);
    CHECK(*unscripted != "keen-owl-3");
    CHECK(isAnimalNameId(*unscripted));
}

TEST_CASE("CreatePaste stops after exactly eight colliding ids and fails with the error it can name",
          "[pastebin][model]") {
    // The exhaustion half of the same retry loop, deterministic for the same
    // reason: the ids are scripted, not sampled. The script is one id longer
    // than the budget and that last id is *free*, so a budget that grew — or
    // a loop that stopped bounding itself — would allocate it and fail this
    // case twice over: the create would not throw, and `drawn()` would read
    // nine.
    //
    // Complements rather than duplicates the whole-keyspace case below. That
    // one drives the real generator, and so also guards the keyspace copy at
    // the top of this file, but it can say nothing about how many attempts
    // were made or what the failure said.
    DbFixture fixture;
    pastebin::PasteModel model;

    const std::string taken = "wild-yak-1";
    {
        pastebin::ScopedPasteIdSource occupy{constantIdSource(taken)};
        REQUIRE(*model.execute(makeCreate("already here")).id == taken);
    }

    std::vector<std::string> ids(kMaxIdAttempts, taken);
    ids.emplace_back("soft-bee-2");  // free, and one draw beyond the budget
    ScriptedIds script{ids};
    {
        pastebin::ScopedPasteIdSource scripted{script.source()};
        REQUIRE_THROWS_MATCHES(model.execute(makeCreate("no id left to try")), pastebin::ValidationError,
                               Catch::Matchers::MessageMatches(
                                   Catch::Matchers::Equals("CreatePaste: could not allocate a unique paste id")));
    }
    CHECK(script.drawn() == kMaxIdAttempts);

    // Nothing was stored under the free id the model never asked for, and the
    // occupied row is still the only paste in the table.
    CHECK_THROWS_AS(model.execute(pastebin::GetPaste{.id = pastebin::PasteId{"soft-bee-2"}}), pastebin::NotFound);
    CHECK(model.execute(pastebin::ListPastes{}).pastes.size() == 1);
}

TEST_CASE("CreatePaste gives up with a ValidationError once the whole keyspace is occupied", "[pastebin][model]") {
    // The other side of the retry budget, and the guard that keeps the
    // keyspace mirrored at the top of this file honest: with every id the
    // model can spell already taken, all eight attempts must collide and the
    // call must surface a plain ValidationError rather than leaking the
    // driver's constraint-violation exception.
    DbFixture fixture;
    occupyKeyspace(kCombos);

    pastebin::PasteModel model;
    REQUIRE_THROWS_AS(model.execute(makeCreate("no room left")), pastebin::ValidationError);
}

// ═════════════════════════════════════════════════════════════════════════
// Step 7 — size-limit UX
// ═════════════════════════════════════════════════════════════════════════

TEST_CASE("An oversized CreatePaste is refused by the transport with a typed, readable error",
          "[pastebin][model][socket-only]") {
    // The bound is a transport concern, not a model one: `QtWebSocketServer`
    // rejects the frame before `RemoteServer::handle()` ever decodes it, so
    // no `PasteModel` runs and nothing is stored. The client still gets an
    // error addressed to its own call, which is what makes the failure
    // renderable rather than a silent hang.
    DbFixture fixture;

    morph::qt::QtWebSocketServerConfig serverConfig;
    serverConfig.maxMessageBytes = 4096;
    BackendRig rig{Mode::Socket, 1, /*authorizer=*/nullptr, serverConfig};
    auto handler = rig.client<pastebin::PasteModel>(0);

    // A comfortably-under-the-cap paste still works, so the case below is
    // about the size and nothing else.
    const auto smallId = awaitQt(handler.execute(makeCreate(std::string(64, 'a')))).id;
    CHECK(awaitQt(handler.execute(pastebin::GetPaste{.id = smallId})).content == std::string(64, 'a'));

    REQUIRE_THROWS_WITH(awaitQt(handler.execute(makeCreate(std::string(64 * 1024, 'a')))),
                        Catch::Matchers::ContainsSubstring("message exceeds maxMessageBytes"));

    // Refused at the transport: exactly one paste exists, the small one.
    pastebin::PasteModel model;
    CHECK(model.execute(pastebin::ListPastes{}).pastes.size() == 1);
}

// ═════════════════════════════════════════════════════════════════════════
// Step 8 — hostile content round-trip
// ═════════════════════════════════════════════════════════════════════════

TEST_CASE("Fuzz-corpus findings survive CreatePaste/GetPaste as paste content, both backends", "[pastebin][model]") {
    const auto mode = GENERATE(Mode::Local, Mode::Socket);
    DbFixture fixture;
    BackendRig rig{mode, 1};
    auto handler = rig.client<pastebin::PasteModel>(0);

    const auto findings = fuzzFindings();
    REQUIRE_FALSE(findings.empty());

    for (const auto& [name, content] : findings) {
        CAPTURE(name);
        if (isValidUtf8(content)) {
            // Control bytes, embedded quotes, JSON-looking payloads: all of
            // these must survive the JSON envelope, the socket, and the TEXT
            // column byte for byte. This is the bug class fuzzing already
            // caught once in the wire layer.
            const auto id = awaitQt(handler.execute(makeCreate(content))).id;
            const auto fetched = awaitQt(handler.execute(pastebin::GetPaste{.id = id}));
            CHECK(fetched.content == content);
        } else {
            // Ill-formed UTF-8 has no faithful representation in a JSON text
            // frame or a `TEXT` column, and does not come back byte for byte
            // (observed: the ill-formed sequences are re-encoded, so the
            // stored content is longer than what was sent). That loss is
            // inherent to a text protocol over a text column, not a defect —
            // but it has to be *stable and convergent*, which is what this
            // asserts: the paste reads back identically every time, and
            // re-pasting what came back round-trips byte for byte. A stack
            // that mangled a little more on every hop, or handed out a
            // different string on the second read, would fail here.
            pastebin::PasteId id;
            try {
                id = awaitQt(handler.execute(makeCreate(content))).id;
            } catch (const std::exception&) {
                continue;  // refused outright: an acceptable, well-behaved outcome
            }
            const auto first = awaitQt(handler.execute(pastebin::GetPaste{.id = id}));
            const auto second = awaitQt(handler.execute(pastebin::GetPaste{.id = id}));
            CHECK(first.content == second.content);

            const auto reId = awaitQt(handler.execute(makeCreate(first.content))).id;
            CHECK(awaitQt(handler.execute(pastebin::GetPaste{.id = reId})).content == first.content);
        }
    }
}

// ═════════════════════════════════════════════════════════════════════════
// Step 9 — security posture: the fail-open delta
// ═════════════════════════════════════════════════════════════════════════

TEST_CASE("Fail-open default: an unauthenticated client registers and reads a paste it knows the id of",
          "[pastebin][security][socket-only]") {
    // Executable documentation of `docs/spec/security.md`'s fail-open
    // default. Rung 1 deliberately configures no authorizer, so this asserts
    // the *documented* posture, not a bug: knowing an id is the entire access
    // control story at this rung. LADDER.md's security matrix is where that
    // changes; when it does, this case is the one that fails first and gets
    // rewritten alongside the rung that hardens it.
    DbFixture fixture;
    pastebin::PasteModel seedModel;
    auto create = makeCreate("no auth configured");
    create.visibility = pastebin::Visibility::Private;  // not even "private" gates a direct read
    const auto id = seedModel.execute(create).id;

    BackendRig rig{Mode::Socket, 1};  // no authorizer -> RemoteServer's allow-all default
    auto handler = rig.client<pastebin::PasteModel>(0);

    const auto fetched = awaitQt(handler.execute(pastebin::GetPaste{.id = id}));
    CHECK(fetched.content == "no auth configured");

    // And the same session-less client can mutate, not merely read.
    REQUIRE_NOTHROW(awaitQt(handler.execute(pastebin::DeletePaste{.id = id})));
    REQUIRE_THROWS_AS(seedModel.execute(pastebin::GetPaste{.id = id}), pastebin::NotFound);
}

// ═════════════════════════════════════════════════════════════════════════
// Step 10 — `hello` protocol-version negotiation
// ═════════════════════════════════════════════════════════════════════════

TEST_CASE("hello negotiates the protocol version the server is built against", "[pastebin][security][socket-only]") {
    // No example exercised the `hello` handshake before this rung (README's
    // "Required tests"). `negotiateProtocolVersion()` is transport-level and
    // blocks on a nested QEventLoop, which is exactly what a native Catch2
    // test wants; `BackendRig::socketBackend()` exists to reach it.
    DbFixture fixture;
    BackendRig rig{Mode::Socket, 1};

    REQUIRE(rig.socketBackend(0).negotiateProtocolVersion() == morph::wire::ProtocolNegotiationResult::Negotiated);

    // Negotiation is not a one-way door: the same connection goes on to serve
    // ordinary traffic.
    pastebin::PasteModel seedModel;
    const auto id = seedModel.execute(makeCreate("after negotiation")).id;
    auto handler = rig.client<pastebin::PasteModel>(0);
    CHECK(awaitQt(handler.execute(pastebin::GetPaste{.id = id})).content == "after negotiation");

    // Idempotent — a second handshake over a live connection negotiates the
    // same version rather than failing.
    REQUIRE(rig.socketBackend(0).negotiateProtocolVersion() == morph::wire::ProtocolNegotiationResult::Negotiated);
}

// ═════════════════════════════════════════════════════════════════════════
// Step 11 — store-error branch coverage
// ═════════════════════════════════════════════════════════════════════════

TEST_CASE("GetPaste surfaces a real SQLITE_BUSY as a thrown error, not as silent data loss", "[pastebin][model]") {
    // The designated resolution for the busy class, per the README's
    // "Store-error branch coverage" bullet: a genuine competing write
    // transaction on a second connection (`testkit/db_busy_fixture.hpp`),
    // not a mock — `db_fault_fixture.hpp`'s `SqlScopedLock` contention
    // cannot fault an ordinary `DataMapper` call at all. The
    // model must let that failure reach the client as itself — treating a
    // contended update as "zero rows matched" would silently downgrade an
    // outage into a NotFound, and a burn budget could be spent (or not) with
    // nobody able to tell.
    DbFixture fixture;
    pastebin::PasteModel seedModel;
    const auto id = seedModel.execute(makeCreate("contended")).id;

    // Same requirement as the EditPaste contention test above:
    // contendedModel's execute() below must acquire its connection while
    // this hook is installed for the hook to actually apply — draining the
    // pool's idle mappers first (drainPoolIdleMappers's own doc comment)
    // makes that a hard guarantee rather than a "usually true" assumption.
    const ScopedShortBusyTimeout shortTimeout{200};
    auto drained = drainPoolIdleMappers();
    pastebin::PasteModel contendedModel;

    const morph::ladder::testkit::DbBusyFixture busy{"pastes"};
    const auto start = std::chrono::steady_clock::now();
    REQUIRE_THROWS(contendedModel.execute(pastebin::GetPaste{.id = id}));
    // contendedModel's one and only execute() call (and so its one pool
    // acquisition) already happened on this thread, synchronously, above.
    drained.clear();
    // Fast, not a sixty-second block: without the hook above, Lightweight's
    // own `PRAGMA busy_timeout = 60000` would make this "pass" by waiting out
    // a real minute.
    CHECK(std::chrono::steady_clock::now() - start < std::chrono::seconds{30});
}

TEST_CASE("CreatePaste surfaces a real SQLITE_BUSY rather than mistaking it for an id collision",
          "[pastebin][model]") {
    // The other half of the classifier in `CreatePaste`'s retry loop: only a
    // unique-constraint violation is retryable. A busy database must not be
    // swallowed into "could not allocate a unique paste id" — that would
    // report an outage as keyspace exhaustion.
    DbFixture fixture;
    {
        pastebin::PasteModel warmup;
        (void)warmup.execute(makeCreate("seed"));
    }

    // Same requirement as the other two SQLITE_BUSY tests in this file:
    // contendedModel's execute() below must acquire its connection while
    // this hook is installed for the hook to actually apply -- draining the
    // pool's idle mappers first (drainPoolIdleMappers's own doc comment)
    // makes that a hard guarantee. Without this, `warmup`'s own earlier
    // acquisition above can leave an idle, already-connected mapper in the
    // pool for contendedModel to receive instead of a fresh one, silently
    // skipping the short busy-timeout PRAGMA and blocking on the real 60s
    // default -- observed as a 120s CTest timeout on CI, not a local
    // failure, since it depends on the pool's prior state.
    const ScopedShortBusyTimeout shortTimeout{200};
    auto drained = drainPoolIdleMappers();
    pastebin::PasteModel contendedModel;

    const morph::ladder::testkit::DbBusyFixture busy{"pastes"};
    REQUIRE_THROWS_AS(contendedModel.execute(makeCreate("cannot be written")), Lightweight::SqlException);
    drained.clear();
}

// ═════════════════════════════════════════════════════════════════════════
// Coverage completeness (examples/IMPLEMENTATION.md rule 5)
// ═════════════════════════════════════════════════════════════════════════
//
// Small surfaces the behavioural cases above never happen to reach, pinned
// directly rather than left as coverage holes: each is real, shipped API
// another rung (or this rung's own server binary) calls.

TEST_CASE("PasteId and PasteCursor adopt an optional payload as-is", "[pastebin][model]") {
    // The named factory that exists because a second same-arity constructor
    // would make `PasteId{"literal"}` ambiguous — see core/types.hpp.
    CHECK_FALSE(pastebin::PasteId::fromOptional(std::nullopt).hasValue());
    const auto engaged = pastebin::PasteId::fromOptional(std::optional<std::string>{"swift-otter"});
    REQUIRE(engaged.hasValue());
    CHECK(*engaged == "swift-otter");
    CHECK(engaged == pastebin::PasteId{"swift-otter"});

    CHECK_FALSE(pastebin::PasteCursor::fromOptional(std::nullopt).hasValue());
    const auto cursor = pastebin::PasteCursor::fromOptional(std::optional<std::string>{"page-2"});
    REQUIRE(cursor.hasValue());
    CHECK(*cursor == "page-2");
    CHECK(cursor == pastebin::PasteCursor{"page-2"});
}

TEST_CASE("The read-count unit carries its schema id, display text and precision", "[pastebin][model]") {
    const auto meta = morph::units::UnitTraits<pastebin::Unit>::meta(pastebin::Unit::count);
    CHECK(meta.id == "count");
    CHECK(meta.display.empty());  // a read count is dimensionless — no unit symbol to render
    CHECK(meta.defaultDecimals == 1U);
}

TEST_CASE("db::setup points the default connection at a database and applies the schema", "[pastebin][model]") {
    // The entry point the server/GUI binaries call at startup, in place of a
    // DbFixture. Pointed at the same database this suite already uses, so it
    // is idempotent here: both of its migration calls are no-ops against an
    // already-migrated schema.
    DbFixture fixture;
    REQUIRE_NOTHROW(pastebin::db::setup(DbFixture::computeConnectionString(std::getenv("ODBC_CONNECTION_STRING"))));

    pastebin::PasteModel model;
    const auto id = model.execute(makeCreate("after setup")).id;
    CHECK(model.execute(pastebin::GetPaste{.id = id}).content == "after setup");
}

TEST_CASE("App teardown survives sweep dispatches still outstanding (the member-order guard)", "[pastebin][app]") {
    // The guard for `App`'s member declaration order, which is the entire
    // protection against a `post()` on a freed `QtExecutor`
    // (`pastebin/app/app.hpp`'s comment above `_sweepExecutor`, and morph#127,
    // which was a shipped bug of exactly that class). `_sweepExecutor` is
    // declared *first*, so it is destroyed *last* — after `~_pool` has joined
    // its worker threads. Move it to its natural reading position at the end
    // of the member list and this case segfaults deterministically:
    // `CompletionState<Ack>::setException` dereferences the freed executor at
    // `include/morph/core/completion.hpp:135`.
    //
    // **The failure mode is a crash, not an assertion** — ctest reports
    // SEGFAULT. That is the point, and it is why there is nothing to CHECK
    // here: do not "fix" this case by wrapping it in one. There is no static
    // alternative either; `offsetof` is not usable on these QObject-derived,
    // non-standard-layout types.
    //
    // No sleep, no retry, no repeat loop, and none is needed: the dispatches
    // are outstanding by construction (issued, never pumped), which is exactly
    // the state `sweepInFlight()`'s "pump until false, then destroy" contract
    // does *not* cover.
    //
    // The name deliberately does not begin with `~`: ctest hands a test's name
    // to Catch2 verbatim, and a leading `~` there is a *negation* filter — a
    // case called "~App ..." registers a ctest entry that runs everything
    // except itself, and reports green while measuring nothing.
    DbFixture fixture;
    pastebin::PasteModel model;

    for (int i = 0; i < 64; ++i) {
        // 64, not one: enough that the pool is reliably still busy when
        // `~App` runs on a fast machine.
        auto create = makeCreate("to be swept " + std::to_string(i));
        create.expiresAt = morph::ladder::now();
        static_cast<void>(model.execute(create).id);
    }
    const morph::ladder::ScopedClockOverride later{nowPlus(std::chrono::hours{1})};

    const auto logPath = std::filesystem::temp_directory_path() / "pastebin_teardown_inflight.jsonl";
    std::filesystem::remove(logPath);
    {
        pastebin::app::App app{logPath, std::chrono::hours{1}};
        app.sweepExpiredOnce();  // 64 dispatches issued into the pool
        // Deliberately no pumping: the App is destroyed with completions
        // outstanding.
    }
    std::filesystem::remove(logPath);
}

TEST_CASE("A sweep with nothing expired dispatches nothing at all", "[pastebin][app]") {
    DbFixture fixture;
    pastebin::PasteModel model;
    const auto id = model.execute(makeCreate("nothing to reclaim")).id;

    const auto logPath = std::filesystem::temp_directory_path() / "pastebin_empty_sweep_test.jsonl";
    std::filesystem::remove(logPath);
    {
        pastebin::app::App app{logPath, std::chrono::hours{1}};
        // The server every transport wraps — what a real deployment reaches
        // for right after construction.
        CHECK(app.server() != nullptr);

        app.sweepExpiredOnce();
        // The early return, not merely "no rows were deleted": a pass that
        // found nothing must not stand up an internal client and dispatch.
        CHECK_FALSE(app.sweepInFlight());
    }
    std::filesystem::remove(logPath);

    CHECK(model.execute(pastebin::GetPaste{.id = id}).content == "nothing to reclaim");
}

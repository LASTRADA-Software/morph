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

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include "clock.hpp"
#include "testkit/backend_rig.hpp"
#include "testkit/db_busy_fixture.hpp"
#include "testkit/db_fixture.hpp"
#include "testkit/pump.hpp"

#include "pastebin/app/app.hpp"
#include "pastebin/core/errors.hpp"
#include "pastebin/db/database.hpp"
#include "pastebin/db/paste_entity.hpp"
#include "pastebin/models/paste_model.hpp"

#include <morph/core/registry.hpp>
#include <morph/core/wire.hpp>
#include <morph/qt/qt_websocket_server.hpp>

#include <Lightweight/DataMapper/DataMapper.hpp>
#include <Lightweight/SqlConnection.hpp>
#include <Lightweight/SqlError.hpp>
#include <Lightweight/SqlLogger.hpp>
#include <Lightweight/SqlStatement.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

using morph::ladder::testkit::BackendRig;
using morph::ladder::testkit::DbFixture;
using morph::ladder::testkit::Mode;
using morph::ladder::testkit::awaitQt;
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
    "cat", "dog", "fox", "owl", "bee", "ant", "elk", "ram",
    "yak", "cod", "eel", "hen", "pig", "cow", "bat", "jay",
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
    (void) stmt.ExecuteDirect("WITH RECURSIVE suffix(x) AS (SELECT 0 UNION ALL SELECT x + 1 FROM suffix WHERE x < " +
                       std::to_string(kSuffixes - 1) +
                       ") INSERT INTO pastes (id, content, syntax, created_at_ms, expires_at_ms, burn_after_reads, "
                       "read_count, is_private, is_editable) SELECT c.prefix || '-' || suffix.x, 'occupied', 'text', "
                       "0, NULL, NULL, 0, 0, 0 FROM suffix, (" +
                       combos + ") c");
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
/// connection that must fail fast is the one `PasteModel` opens lazily inside
/// itself (`db::WithMapper`), which no test can reach. The post-connected
/// hook is the seam that works from the outside: it runs immediately after
/// `PostConnect()` on every connection, including that one, so long as the
/// model's first `execute(...)` happens while this guard is alive.
class ScopedShortBusyTimeout {
  public:
    explicit ScopedShortBusyTimeout(int milliseconds) {
        ::Lightweight::SqlConnection::SetPostConnectedHook([milliseconds](::Lightweight::SqlConnection& connection) {
            ::Lightweight::SqlStatement stmt{connection};
            (void) stmt.ExecuteDirect("PRAGMA busy_timeout = " + std::to_string(milliseconds));
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
    // A budget of 0 is a whole number, so it passes Reads' own whole-number
    // constraint, but PasteModel::execute(GetPaste)'s burn check
    // (`readCount >= *burnAfterReads`) is already true before the first read
    // ever happens — a paste born with burnAfterReads=0 would be permanently
    // Burned on its very first GetPaste, having never been read once.
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

    REQUIRE_THROWS_AS(model.execute(pastebin::GetPaste{.id = pastebin::PasteId{"no-such-paste"}}),
                      pastebin::NotFound);
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

TEST_CASE("EditPaste refuses an immutable paste, an unknown id, and an incomplete action",
          "[pastebin][model]") {
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

    // The model under test must open its connection *while* the short
    // busy-timeout hook is installed (db::WithMapper connects lazily), same
    // requirement as the SQLITE_BUSY cases below.
    const ScopedShortBusyTimeout shortTimeout{5000};
    pastebin::PasteModel contendedModel;

    ::Lightweight::SqlConnection lockingConnection;
    {
        ::Lightweight::SqlStatement stmt{lockingConnection};
        (void) stmt.ExecuteDirect("BEGIN IMMEDIATE");
        (void) stmt.ExecuteDirect("UPDATE pastes SET id = id WHERE id = '" + *id + "'");
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
        (void) stmt.ExecuteDirect("UPDATE pastes SET content = 'concurrent writer' WHERE id = '" + *id + "'");
        (void) stmt.ExecuteDirect("COMMIT");
    }

    editor.join();
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

    constexpr int kPublic = 25;   // one full 20-row page plus a partial second one
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

TEST_CASE("GetPaste spends the burn budget and deletes the paste on the last allowed read",
          "[pastebin][model]") {
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

TEST_CASE("GetPaste against a row already at its burn budget throws Burned, not NotFound",
          "[pastebin][model]") {
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
        rec.content = std::string{"gone"};
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

            REQUIRE(pumpUntil([tally] {
                return tally->successes.load() + tally->failures.load() == static_cast<int>(kClients);
            }));
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

TEST_CASE("A paste past its expiresAt throws Expired from GetPaste, before any sweep runs",
          "[pastebin][model]") {
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
    atEpoch.expiresAt = ::morph::time::Timestamp{::morph::time::DateTime{
        std::chrono::sys_time<std::chrono::milliseconds>{std::chrono::milliseconds{0}}}};
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

TEST_CASE("A malformed expiresAt on the wire is a decode error, never a clamped value",
          "[pastebin][model]") {
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

TEST_CASE("ExpirePaste reclaims only a genuinely expired paste, so replaying it is safe",
          "[pastebin][model]") {
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
                (void) model.execute(pastebin::GetPaste{.id = sweptId});
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

TEST_CASE("A sweep firing between two pages of a ListPastes cursor walk skips no surviving paste",
          "[pastebin][app]") {
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
        (void) model.execute(doomed);
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

TEST_CASE("Two CreatePaste calls with identical content mint two distinct pastes at this rung",
          "[pastebin][model]") {
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

TEST_CASE("CreatePaste retries past colliding animal-name ids instead of failing the call",
          "[pastebin][model]") {
    DbFixture fixture;

    // A quarter of the keyspace is occupied up front, so roughly one
    // allocation attempt in four collides on a real primary-key violation and
    // has to be retried. Across the creates below a collision is effectively
    // certain (P(none) = 0.75^40 ~= 1e-5), while exhausting the eight-attempt
    // budget for any single create is not (P = 0.25^8 ~= 1.5e-5 per create) —
    // the retry path is genuinely exercised without the case becoming flaky.
    occupyKeyspace(kCombos / 4);

    pastebin::PasteModel model;
    std::vector<pastebin::PasteId> minted;
    for (int i = 0; i < 40; ++i) {
        pastebin::CreatePasteResult result;
        REQUIRE_NOTHROW(result = model.execute(makeCreate("attempt " + std::to_string(i))));
        REQUIRE(result.id.hasValue());
        minted.push_back(result.id);
    }

    // Every id is distinct, and none of them landed on an occupied row (which
    // would mean an allocation overwrote a stored paste rather than retrying).
    std::ranges::sort(minted);
    CHECK(std::ranges::adjacent_find(minted) == minted.end());
    for (const auto& id : minted) {
        CHECK(model.execute(pastebin::GetPaste{.id = id}).content.starts_with("attempt "));
    }
}

TEST_CASE("CreatePaste gives up with a ValidationError once the whole keyspace is occupied",
          "[pastebin][model]") {
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

TEST_CASE("Fuzz-corpus findings survive CreatePaste/GetPaste as paste content, both backends",
          "[pastebin][model]") {
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

TEST_CASE("hello negotiates the protocol version the server is built against",
          "[pastebin][security][socket-only]") {
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

TEST_CASE("GetPaste surfaces a real SQLITE_BUSY as a thrown error, not as silent data loss",
          "[pastebin][model]") {
    // Finding 018's designated resolution for the busy class: a genuine
    // competing write transaction on a second connection, not a mock. The
    // model must let that failure reach the client as itself — treating a
    // contended update as "zero rows matched" would silently downgrade an
    // outage into a NotFound, and a burn budget could be spent (or not) with
    // nobody able to tell.
    DbFixture fixture;
    pastebin::PasteModel seedModel;
    const auto id = seedModel.execute(makeCreate("contended")).id;

    // The model under test must open its connection *while* the short
    // busy-timeout hook is installed, so it is a model that has not executed
    // anything yet (`db::WithMapper` connects lazily, on first use).
    const ScopedShortBusyTimeout shortTimeout{200};
    pastebin::PasteModel contendedModel;

    const morph::ladder::testkit::DbBusyFixture busy{"pastes"};
    const auto start = std::chrono::steady_clock::now();
    REQUIRE_THROWS(contendedModel.execute(pastebin::GetPaste{.id = id}));
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
        (void) warmup.execute(makeCreate("seed"));
    }

    const ScopedShortBusyTimeout shortTimeout{200};
    pastebin::PasteModel contendedModel;

    const morph::ladder::testkit::DbBusyFixture busy{"pastes"};
    REQUIRE_THROWS_AS(contendedModel.execute(makeCreate("cannot be written")), Lightweight::SqlException);
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

TEST_CASE("db::setup points the default connection at a database and applies the schema",
          "[pastebin][model]") {
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

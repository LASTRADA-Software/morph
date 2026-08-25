// SPDX-License-Identifier: Apache-2.0
#include "pastebin/models/paste_model.hpp"

// The entity is an implementation detail of this TU: `paste_model.hpp` exposes
// only DTOs, so nothing outside this file ever sees `db::PasteRecord`.
#include "pastebin/db/paste_entity.hpp"

// examples/common is on the include path as a root (see
// examples/common/CMakeLists.txt's target_include_directories), so the ladder
// clock is "clock.hpp" — the same spelling testkit/test_clock.cpp uses.
#include <Lightweight/DataBinder/UnicodeConverter.hpp>
#include <Lightweight/DataMapper/DataMapper.hpp>
#include <Lightweight/DataMapper/Pool.hpp>
#include <Lightweight/SqlError.hpp>
#include <Lightweight/SqlErrorDetection.hpp>
#include <Lightweight/SqlStatement.hpp>
#include <Lightweight/SqlTransaction.hpp>
#include <array>
#include <cstddef>
#include <cstdint>
#include <format>
#include <optional>
#include <random>
#include <string>
#include <string_view>
#include <vector>

#include "clock.hpp"

namespace pastebin {

// The one place the DTO layer's `syntax` bound and the storage layer's real
// column capacity are checked against each other. `kMaxSyntaxBytes` exists so
// `CreatePaste::validate()`/`EditPaste::validate()` can reject an over-long
// label instead of letting `SqlFixedString`'s `_size{std::min(N, s.size())}`
// truncate it silently (see that constant's own doc comment for the two harms
// that follow); this assertion is what keeps the number honest. Widening the
// column without widening the constant — or the reverse — fails the build
// here rather than silently reopening the gap in production.
static_assert(decltype(db::PasteRecord::syntax)::ValueType{}.capacity() == kMaxSyntaxBytes,
              "pastebin::kMaxSyntaxBytes must equal PasteRecord::syntax's SqlAnsiString capacity — otherwise "
              "CreatePaste/EditPaste either reject labels that would have fit, or accept ones that get "
              "silently truncated on the way into the row.");

namespace {

// ---------------------------------------------------------------------------
// DTO <-> entity conversions (IMPLEMENTATION.md rule 4's DTO<->entity mapping
// layer). Both directions are exact: an instant is a whole number of
// milliseconds, and every `Reads` value that ever reaches the database is a
// whole-number count, so the conversions go through `std::int64_t` and an
// exact `math::Rational` rather than through `double`. `Reads::fromDouble` /
// `math::Rational::toDouble` do exist and would work for the magnitudes
// involved, but they round-trip through binary floating point for values that
// are integers by construction — there is nothing to gain and a rounding step
// to lose.
// ---------------------------------------------------------------------------

[[nodiscard]] std::int64_t toEpochMs(const ::morph::time::DateTime& instant) noexcept {
    return instant.value.time_since_epoch().count();
}

[[nodiscard]] std::int64_t nowMs() noexcept { return toEpochMs(*::morph::ladder::now().value); }

[[nodiscard]] ::morph::time::Timestamp fromEpochMs(const std::optional<std::int64_t>& epochMs) noexcept {
    if (!epochMs) {
        return ::morph::time::Timestamp{};
    }
    return ::morph::time::Timestamp{::morph::time::DateTime{
        std::chrono::sys_time<std::chrono::milliseconds>{std::chrono::milliseconds{*epochMs}}}};
}

/// @brief An exact whole-number read count as a `Reads` quantity.
[[nodiscard]] Reads readsOf(std::int64_t count) {
    return Reads{::morph::math::Rational{count, Reads::declaredPrecision()}};
}

/// @brief An engaged `Reads` back as a whole-number count.
///
/// `math::floor` is exact on a `Rational` (integer division on the stored
/// numerator/denominator) — no floating-point step. `Reads` only ever carries
/// whole numbers here, so flooring and truncating agree.
[[nodiscard]] std::int64_t countOf(const Reads& reads) noexcept { return ::morph::math::floor(*reads); }

[[nodiscard]] std::string textOf(const Light::SqlAnsiString<32>& stored) { return std::string{stored.str()}; }

// `content` is stored wide (Light::SqlMaxDynamicWideString — see
// paste_entity.hpp's file comment for why); the DTO layer stays UTF-8
// std::string per IMPLEMENTATION.md rule 4, so every read/write of `content`
// converts here, at the model boundary, rather than leaking the storage
// representation into the DTO or the caller.
[[nodiscard]] std::string utf8Of(const Light::SqlMaxDynamicWideString& stored) {
    return std::string{reinterpret_cast<const char*>(Lightweight::ToUtf8(stored.ToStringView()).c_str())};
}

[[nodiscard]] Light::SqlMaxDynamicWideString wideOf(const std::string& utf8) {
    return Light::SqlMaxDynamicWideString{
        Lightweight::ToStdWideString(std::u8string_view{reinterpret_cast<const char8_t*>(utf8.data()), utf8.size()})};
}

/// @brief Builds the read-only view sent back to a client from a fully loaded
///        `PasteRecord`.
[[nodiscard]] PasteView toView(const db::PasteRecord& rec) {
    PasteView view;
    view.id = PasteId{textOf(rec.id.Value())};
    view.content = utf8Of(rec.content.Value());
    view.syntax = textOf(rec.syntax.Value());
    view.createdAt = fromEpochMs(rec.createdAtMs.Value());
    view.expiresAt = fromEpochMs(rec.expiresAtMs.Value());
    view.burnAfterReads = rec.burnAfterReads.Value() ? readsOf(*rec.burnAfterReads.Value()) : Reads{};
    view.readCount = readsOf(rec.readCount.Value());
    view.visibility = rec.isPrivate.Value() ? Visibility::Private : Visibility::Public;
    view.editability = rec.isEditable.Value() ? Editability::Editable : Editability::Immutable;
    return view;
}

/// @brief The tiny animal-name id keyspace (MicroBin-style). Deliberately
///        small — the required tests exercise the id-collision retry path,
///        which needs collisions to be reachable in a bounded number of
///        `CreatePaste` calls, not astronomically unlikely.
constexpr std::array<std::string_view, 16> kAnimals = {
    "cat", "dog", "fox", "owl", "bee", "ant", "elk", "ram", "yak", "cod", "eel", "hen", "pig", "cow", "bat", "jay",
};
constexpr std::array<std::string_view, 16> kAdjectives = {
    "red",  "blue", "gold", "dark", "swift", "calm",  "bold", "wild",
    "keen", "grey", "warm", "cool", "sharp", "quiet", "loud", "soft",
};

[[nodiscard]] std::string randomPasteId() {
    static thread_local std::mt19937_64 rng{std::random_device{}()};
    std::uniform_int_distribution<std::size_t> adjIdx{0, kAdjectives.size() - 1};
    std::uniform_int_distribution<std::size_t> animalIdx{0, kAnimals.size() - 1};
    std::uniform_int_distribution<int> suffix{0, 999};
    return std::string{kAdjectives[adjIdx(rng)]} + "-" + std::string{kAnimals[animalIdx(rng)]} + "-" +
           std::to_string(suffix(rng));
}

/// @brief Bounded retry budget for allocating a free animal-name id.
constexpr int kMaxIdAttempts = 8;

/// @brief `ListPastes` page size (rows per page, excluding the has-more probe).
constexpr std::size_t kPageSize = 20;

/// @brief The one conditional statement burn-after-read atomicity rests on.
///
/// Every guard a read must respect lives in this single `WHERE`: the row must
/// exist, must not have expired, and must still have burn budget left. The
/// increment and the guard are therefore evaluated by the database in one
/// statement — no read-then-write window exists for a second client to slip
/// through. See `PasteModel::execute(const GetPaste&)` for the full argument.
///
/// **Not** `... RETURNING`: the sqliteodbc driver this rung runs against
/// reports the RETURNING column count but then fails `SQLFetch` with SQLSTATE
/// 24000 ("Invalid cursor state") — filed upstream as
/// `LASTRADA-Software/Lightweight#545`. The row is read back by a second
/// statement inside the same transaction instead; the atomicity argument is
/// unchanged because the guard still lives in the `UPDATE` itself.
constexpr std::string_view kConsumeReadSql = R"(UPDATE pastes
       SET read_count = read_count + 1
     WHERE id = ?
       AND (expires_at_ms IS NULL OR expires_at_ms > ?)
       AND (burn_after_reads IS NULL OR read_count < burn_after_reads))";

/// @brief `EditPaste`'s compare-and-swap guard: the write only applies if the
///        row's content/syntax still equal what this client last read. Same
///        shape and same argument as `kConsumeReadSql` above — the guard and
///        the write are one indivisible statement, so there is no
///        read-then-write window a second concurrent edit can land in. See
///        `PasteModel::execute(const EditPaste&)` for the full argument.
constexpr std::string_view kEditPasteSql = R"(UPDATE pastes
       SET content = ?, syntax = ?
     WHERE id = ?
       AND is_editable = 1
       AND content = ?
       AND syntax = ?)";

}  // namespace

CreatePasteResult PasteModel::execute(const CreatePaste& action) {
    if (!action.validate()) {
        throw ValidationError{
            std::format("CreatePaste: content and syntax are required, syntax must be at most {} "
                        "bytes, and burnAfterReads (if given) must be a positive count",
                        kMaxSyntaxBytes)};
    }

    // One connection for this call, acquired from the pool and returned when
    // it goes out of scope at the end of this function — not a member this
    // model instance holds for its own lifetime (see paste_model.hpp's file
    // comment for why the model must not own database state).
    auto mapper = ::Lightweight::GlobalDataMapperPool().Acquire();

    // Bounded retry on the (small, deliberately-collidable) animal-name
    // keyspace. The insert itself is the collision test — a pre-check would be
    // a time-of-check/time-of-use window between two model instances on two
    // connections; the primary key is the only authority.
    for (int attempt = 0; attempt < kMaxIdAttempts; ++attempt) {
        db::PasteRecord rec;
        rec.id = Light::SqlAnsiString<32>{randomPasteId()};
        rec.content = wideOf(action.content);
        rec.syntax = Light::SqlAnsiString<32>{action.syntax};
        rec.createdAtMs = nowMs();
        rec.expiresAtMs = action.expiresAt.hasValue() ? std::optional{toEpochMs(*action.expiresAt)} : std::nullopt;
        rec.burnAfterReads =
            action.burnAfterReads.hasValue() ? std::optional{countOf(action.burnAfterReads)} : std::nullopt;
        rec.readCount = std::int64_t{0};
        rec.isPrivate = action.visibility == Visibility::Private;
        rec.isEditable = action.editability == Editability::Editable;

        try {
            mapper->Create(rec);
        } catch (const ::Lightweight::SqlException& error) {
            // Only a primary-key collision on the animal-name id is retryable.
            // Every other store error (a lock, a dropped connection, a broken
            // schema) must reach the client as itself — swallowing it here
            // would mis-report an outage as "keyspace exhausted", and the
            // required store-error branch tests distinguish the two.
            // sqliteodbc reports both under SQLSTATE HY000, so the message-based
            // classifier Lightweight ships is the only discriminator available.
            if (!::Lightweight::IsUniqueConstraintViolation(error.info(), mapper->Connection().ServerType())) {
                throw;
            }
            continue;
        }
        return CreatePasteResult{.id = PasteId{textOf(rec.id.Value())}};
    }
    throw ValidationError{"CreatePaste: could not allocate a unique paste id"};
}

PasteView PasteModel::execute(const GetPaste& action) {
    if (!action.validate()) {
        throw ValidationError{"GetPaste: id is required"};
    }
    const std::string& id = *action.id;
    const std::int64_t readAtMs = nowMs();

    // One connection for this whole call — the transaction below and the
    // fallback classification read after it must run on the same connection.
    auto mapper = ::Lightweight::GlobalDataMapperPool().Acquire();

    // ── The atomic read-consumption ─────────────────────────────────────────
    // The conditional UPDATE is the whole race-safety argument: SQLite
    // evaluates its WHERE and applies its increment as one indivisible
    // statement under a write lock, so of two clients racing for the last
    // allowed read of a burn-after-N paste exactly one gets a non-zero
    // affected-row count. The loser's UPDATE finds `read_count < burn_after_reads`
    // already false and touches nothing.
    //
    // The transaction exists for the *read-back*, not for the guard: it holds
    // the write lock the UPDATE took until the SELECT has seen the row the
    // UPDATE produced, so no other connection can delete or re-read it in
    // between. It also makes the burn-delete below part of the same commit.
    std::optional<PasteView> view;
    {
        ::Lightweight::SqlTransaction transaction{mapper->Connection(), ::Lightweight::SqlTransactionMode::ROLLBACK};

        std::size_t consumed = 0;
        {
            ::Lightweight::SqlStatement consume{mapper->Connection()};
            consume.Prepare(kConsumeReadSql);
            auto cursor = consume.Execute(id, readAtMs);
            consumed = cursor.NumRowsAffected();
        }

        // `== 1`, not `!= 0`: `id` is the primary key, so the UPDATE's
        // `WHERE id = ?` can affect at most one row — 1 is the only possible
        // non-zero outcome. Testing for it exactly also closes the one
        // theoretical hole in this gate: `NumRowsAffected()` casts ODBC's
        // signed `SQLLEN` to `size_t` unguarded, and `SQLRowCount` may report
        // -1 when the count is unavailable, which would arrive here as
        // SIZE_MAX — non-zero, and so would disclose content without a read
        // having actually been consumed. This one comparison is the sole gate
        // on the burn-atomicity guarantee; it must not admit a sentinel.
        if (consumed == 1) {
            auto rows = mapper->Query<db::PasteRecord>()
                            .Where(::Lightweight::FieldNameOf<&db::PasteRecord::id>, "=", id)
                            .All();
            if (rows.empty()) {
                // Unreachable in practice: the UPDATE just matched this row and
                // holds the write lock. Treated as "gone" rather than asserted.
                throw NotFound{"GetPaste: no such paste"};
            }
            const db::PasteRecord& rec = rows.front();
            view = toView(rec);

            // Burn-after-read destroys the paste *on* the Nth read, not before:
            // the read that just consumed the last unit of budget still returns
            // its content, and only then removes the row.
            const std::optional<std::int64_t>& budget = rec.burnAfterReads.Value();
            if (budget && rec.readCount.Value() >= *budget) {
                ::Lightweight::SqlStatement burn{mapper->Connection()};
                burn.Prepare("DELETE FROM pastes WHERE id = ?");
                (void)burn.Execute(id);
            }
            transaction.Commit();
        }
    }
    if (view) {
        return *view;
    }

    // ── Zero rows matched: classify why ─────────────────────────────────────
    // A plain, unprotected read. This does not reopen the window the atomic
    // UPDATE closed: it decides only *which* error to throw and mutates
    // nothing. A row that changes underneath it can at worst turn one
    // truthful-a-moment-ago error into another.
    auto existing =
        mapper->Query<db::PasteRecord>().Where(::Lightweight::FieldNameOf<&db::PasteRecord::id>, "=", id).All();
    if (existing.empty()) {
        throw NotFound{"GetPaste: no such paste"};
    }
    const db::PasteRecord& row = existing.front();
    if (row.expiresAtMs.Value() && *row.expiresAtMs.Value() <= readAtMs) {
        throw Expired{"GetPaste: paste has expired"};
    }
    if (row.burnAfterReads.Value() && row.readCount.Value() >= *row.burnAfterReads.Value()) {
        throw Burned{"GetPaste: paste's burn-after-reads budget is exhausted"};
    }
    throw NotFound{"GetPaste: no such paste"};
}

PasteView PasteModel::execute(const EditPaste& action) {
    if (!action.validate()) {
        throw ValidationError{
            std::format("EditPaste: id, content, and syntax are required, and syntax must be at "
                        "most {} bytes",
                        kMaxSyntaxBytes)};
    }
    const std::string& id = *action.id;

    // One connection for this whole call — the CAS transaction below and the
    // reads before/after it must run on the same connection.
    auto mapper = ::Lightweight::GlobalDataMapperPool().Acquire();

    // A first, unprotected read: it decides the common-case NotFound /
    // not-editable errors, and supplies the compare-and-swap guard's expected
    // "before" values for the atomic write below. A stale read here does not
    // reopen a race — it just means the guarded UPDATE below affects 0 rows,
    // which is classified as `Conflict`, never silently applied.
    auto before =
        mapper->Query<db::PasteRecord>().Where(::Lightweight::FieldNameOf<&db::PasteRecord::id>, "=", id).All();
    if (before.empty()) {
        throw NotFound{"EditPaste: no such paste"};
    }
    if (!before.front().isEditable.Value()) {
        throw ValidationError{"EditPaste: paste is not editable"};
    }
    const Light::SqlMaxDynamicWideString previousContent = before.front().content.Value();
    const std::string previousSyntax = textOf(before.front().syntax.Value());

    // ── The atomic compare-and-swap write ───────────────────────────────────
    // Same structure as `PasteModel::execute(const GetPaste&)`'s burn
    // consumption: the guard (content/syntax still equal what was just read)
    // and the write are one indivisible statement, so a second concurrent
    // `EditPaste` racing against this one cannot land in a read-then-write
    // window — it either wins the CAS or is told `Conflict`, never silently
    // discarded.
    std::optional<PasteView> view;
    {
        ::Lightweight::SqlTransaction transaction{mapper->Connection(), ::Lightweight::SqlTransactionMode::ROLLBACK};

        std::size_t consumed = 0;
        {
            ::Lightweight::SqlStatement stmt{mapper->Connection()};
            stmt.Prepare(kEditPasteSql);
            auto cursor = stmt.Execute(wideOf(action.content), action.syntax, id, previousContent, previousSyntax);
            consumed = cursor.NumRowsAffected();
        }

        // `== 1`, not `!= 0` — same rationale as GetPaste's burn-consumption
        // gate: `id` is the primary key, so at most one row can ever match,
        // and testing for exactly 1 closes the `NumRowsAffected()`
        // signed-to-unsigned `-1` -> `SIZE_MAX` hole.
        if (consumed == 1) {
            auto rows = mapper->Query<db::PasteRecord>()
                            .Where(::Lightweight::FieldNameOf<&db::PasteRecord::id>, "=", id)
                            .All();
            if (rows.empty()) {
                // Unreachable in practice: the UPDATE just matched this row
                // and holds the write lock. Treated as "gone" rather than
                // asserted, matching GetPaste's equivalent branch.
                throw NotFound{"EditPaste: no such paste"};
            }
            view = toView(rows.front());
            transaction.Commit();
        }
    }
    if (view) {
        return *view;
    }

    // ── Zero rows matched: classify why ─────────────────────────────────────
    auto existing =
        mapper->Query<db::PasteRecord>().Where(::Lightweight::FieldNameOf<&db::PasteRecord::id>, "=", id).All();
    if (existing.empty()) {
        throw NotFound{"EditPaste: no such paste"};
    }
    if (!existing.front().isEditable.Value()) {
        throw ValidationError{"EditPaste: paste is not editable"};
    }
    // Still exists, still editable, but the CAS guard didn't match: some
    // other write landed between the read above and this one.
    throw Conflict{"EditPaste: paste was modified by another edit since it was last read"};
}

Ack PasteModel::execute(const DeletePaste& action) {
    if (!action.validate()) {
        throw ValidationError{"DeletePaste: id is required"};
    }
    auto mapper = ::Lightweight::GlobalDataMapperPool().Acquire();
    ::Lightweight::SqlStatement stmt{mapper->Connection()};
    stmt.Prepare("DELETE FROM pastes WHERE id = ?");
    (void)stmt.Execute(*action.id);
    return Ack{};
}

ListPastesResult PasteModel::execute(const ListPastes& action) {
    // One connection for this call: the query is built up across several
    // statements below and must run against the same connection throughout.
    auto mapper = ::Lightweight::GlobalDataMapperPool().Acquire();

    // Keyset pagination on the primary key, descending: the cursor is the last
    // id of the previous page, so a row created or reclaimed mid-walk can never
    // shift a later page's offset (the required "sweep fires between two pages"
    // test depends on exactly this).
    auto query = mapper->Query<db::PasteRecord>();
    (void)query.Where(::Lightweight::FieldNameOf<&db::PasteRecord::isPrivate>, "=", false);
    if (action.cursor.hasValue()) {
        (void)query.Where(::Lightweight::FieldNameOf<&db::PasteRecord::id>, "<", *action.cursor);
    }
    // One row beyond the page is the has-more probe; it is never returned.
    auto rows =
        query.OrderBy(::Lightweight::FieldNameOf<&db::PasteRecord::id>, ::Lightweight::SqlResultOrdering::DESCENDING)
            .First(kPageSize + 1);

    const bool hasMore = rows.size() > kPageSize;
    if (hasMore) {
        rows.resize(kPageSize);
    }

    ListPastesResult result;
    result.pastes.reserve(rows.size());
    for (const db::PasteRecord& row : rows) {
        result.pastes.push_back(PasteSummary{
            .id = PasteId{textOf(row.id.Value())},
            .syntax = textOf(row.syntax.Value()),
            .createdAt = fromEpochMs(row.createdAtMs.Value()),
            .visibility = row.isPrivate.Value() ? Visibility::Private : Visibility::Public,
        });
    }
    result.nextCursor = hasMore ? PasteCursor{textOf(rows.back().id.Value())} : PasteCursor{};
    return result;
}

Ack PasteModel::execute(const ExpirePaste& action) {
    if (!action.validate()) {
        throw ValidationError{"ExpirePaste: id is required"};
    }
    // The `expires_at_ms <= ?` guard is what makes this replay-safe: the action
    // payload carries only the id, so re-running a journaled entry against a
    // paste that is not (or no longer) expired deletes nothing.
    auto mapper = ::Lightweight::GlobalDataMapperPool().Acquire();
    ::Lightweight::SqlStatement stmt{mapper->Connection()};
    stmt.Prepare("DELETE FROM pastes WHERE id = ? AND expires_at_ms IS NOT NULL AND expires_at_ms <= ?");
    (void)stmt.Execute(*action.id, nowMs());
    return Ack{};
}

}  // namespace pastebin

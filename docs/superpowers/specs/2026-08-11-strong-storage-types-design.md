# Strong storage types across the ladder rungs

Status: proposed, pending review.

## Origin

PR #41 review comments (Yaraslaut) on `examples/pastebin/include/pastebin/db/{db_model,paste_entity}.hpp`:

1. `db_model.hpp` — "I am not sure why would you need this type, just use DataMapperPool"
2. `paste_entity.hpp` (id) — "I think it is better to create GUID id"
3. `paste_entity.hpp` (content) — "please do not use std::string as a type, only strong types provided from a Lightweight library itself"
4. `paste_entity.hpp` (createdAtMs) — "this should be a timestamp, not an integer"

All four `db_model.hpp` files (bank, bookmarks, pastebin, polls) are byte-for-byte the same `WithMapper` mixin, and the string/timestamp patterns repeat across every rung's entities. Scoping this to pastebin alone would leave the identical issue in the other three rungs — this design applies all four fixes ladder-wide.

## 1. `WithMapper` → `DataMapperPool`

**Current shape** (identical in all four rungs): `WithMapper::mapper()` lazily `.emplace()`s a `std::optional<Lightweight::DataMapper>` held as a member for the model's entire lifetime — one uniquely-owned connection per model instance, opened on first use on whatever strand thread runs that model.

**Change**: hold a `std::optional<Lightweight::Pool<Config>::PooledDataMapper>` instead, acquired from `Lightweight::GlobalDataMapperPool()` on first use. `mapper()` still returns `Lightweight::DataMapper&` (via `PooledDataMapper::Get()`) — no call site in any of the ~16 model `.cpp` files changes.

This does not fight the single-threaded-per-model design: each model still acquires and holds one mapper for its own lifetime, on its own strand. Pooling changes *where the connection comes from* (a shared, capped pool instead of an unconditional `new`), not the ownership/threading model:

- Caps total live ODBC connections across every model in a process instead of one-per-model-forever.
- Reuses connections when models are recreated (registry restart, reattach) instead of leaking a fresh one each time.
- `GlobalDataMapperPool()` defaults (`Pool<DefaultPoolConfig>`) are adopted as-is — no rung needs custom pool sizing today, and inventing one would be scope creep.

Applies identically to all four `db_model.hpp` files (the Emscripten `#else` branch is untouched — it never had a mapper to begin with). No test changes expected: `DbFixture`/`DbBusyFixture` interact with `WithMapper` only through `mapper()`'s existing signature.

## 2. Pastebin's id: animal-name string → `Light::SqlGuid`

Confirmed with the user: this is a deliberate product-facing change, not a misunderstanding of the animal-name feature. The public `PasteId` share-link value moves from a short memorable string (`"swift-otter-42"`) to a GUID.

**What changes:**
- `PasteRecord::id`: `Light::Field<Light::SqlAnsiString<32>, Light::PrimaryKey::AutoAssign, ...>` → `Light::Field<Light::SqlGuid, Light::PrimaryKey::AutoAssign, ...>`.
- `randomPasteId()`, `kAnimals`, `kAdjectives`, `kMaxIdAttempts`, and the collision-retry loop in `PasteModel::execute(const CreatePaste&)` are deleted outright — `SqlGuid::Create()` produces a fresh GUID with no realistic collision, so there is nothing to retry. The insert becomes a single `mapper().Create(rec)` call with no loop; the `IsUniqueConstraintViolation` retry branch's *test* (the one exercising the collision path) is removed along with it, since the collision path no longer exists.
- `textOf(const Light::SqlAnsiString<32>&)` is replaced by a `Light::SqlGuid` ↔ `std::string` pair: `Lightweight::to_string(guid)` for entity→DTO, `Lightweight::SqlGuid::TryParse(text)` for DTO→entity (id lookups in `GetPaste`/`EditPaste`/`DeletePaste`/`ExpirePaste` all parse the incoming `PasteId` string into a `SqlGuid` before querying; an unparseable id is a `NotFound`, not a crash — `TryParse` returns `std::optional`).

**What does not change:** `PasteId` itself (`pastebin/core/types.hpp`) stays `std::optional<std::string>` on the wire — its own doc comment already states the strong-typing is C++-only and the wire form is a plain nullable string. No DTO, no QML file, no glaze `meta` specialization changes. `PasteCursor` (pagination) also stays a string — it already opaquely wraps whatever `id` stringifies to, GUID or animal-name alike.

**Not touched elsewhere:** every other rung's primary keys (bank, bookmarks, polls: all `ServerSideAutoIncrement` surrogate integers) are correctly-designed surrogate keys already, not analogous to pastebin's caller-assigned case. Polls' `pollId`/`adminToken`/`participantToken` are server-generated random tokens, not the table's primary key, and converting them to GUID is out of scope — nothing in the review comments asks for it and they serve a different purpose (short URL-safe tokens, not row identity).

## 3. Plain `std::string` entity fields → Lightweight strong string types

Every `Light::Field<std::string, ...>` across all four rungs' `db/*_entity.hpp` files moves to a Lightweight string type. Two cases:

**Bounded fields** (a `kMax*Bytes` DTO-level cap already exists, or a natural small cap is obvious for an internal/program-controlled field): `Light::SqlAnsiString<N>`, with `N` set to the existing constant. Follow the existing `paste_model.cpp` precedent — a `static_assert(decltype(Entity::field)::ValueType{}.capacity() == kMaxFooBytes, ...)` pins the two together so a future change to one without the other fails the build, not silently truncates or silently rejects.

**Unbounded fields** (no natural cap — arbitrary-length user content or serialized blobs): `Light::SqlMaxDynamicAnsiString` (Lightweight's near-2GB-capacity dynamic string), per the user's decision — no new business limit is invented where none exists today.

Full inventory (grouped by disposition; `N` values for fields with no existing DTO constant are proposed here, not invented arbitrarily — matched to a sibling field's existing bound where one is analogous, otherwise called out for confirmation during planning):

| Rung | Entity | Field | Disposition |
|---|---|---|---|
| pastebin | `PasteRecord` | `content` | `SqlMaxDynamicAnsiString` (unbounded paste body) |
| bookmarks | `BookmarkRecord` | `ownerPrincipal` | `SqlAnsiString<N>` — no existing bound; use auth's existing principal-length convention (check `auth_dto.hpp`/`bookmarks_authorizer.hpp` during planning) |
| bookmarks | `BookmarkRecord` | `url` | `SqlAnsiString<kMaxUrlBytes>` (2048) |
| bookmarks | `BookmarkRecord` | `title` | `SqlAnsiString<kMaxTitleBytes>` (512) |
| bookmarks | `BookmarkRecord` | `description` | No existing `kMax*Bytes` — needs a new bound or `SqlMaxDynamicAnsiString`; flag for planning decision |
| bookmarks | `BookmarkRecord` | `notes` | Same as `description` |
| bookmarks | `BookmarkRecord` | `faviconPath` | `SqlAnsiString<kMaxUrlBytes>` (it is a URL) |
| bookmarks | `ImportedOpRecord` | `ownerPrincipal` | Same disposition as `BookmarkRecord::ownerPrincipal` |
| bookmarks | `ImportedOpRecord` | `opId` | `SqlAnsiString<N>` — small caller-chosen idempotency token; propose 128 |
| bookmarks | `BookmarkOutboxRecord` | `modelType`, `entityKey`, `actionType`, `principal` | `SqlAnsiString<N>` — short, program-controlled identifiers; propose 64 |
| bookmarks | `BookmarkOutboxRecord` | `payload`, `result` | `SqlMaxDynamicAnsiString` (serialized JSON, unbounded) |
| bookmarks | `BookmarkOutboxRecord` | `idempotencyKey` | `SqlAnsiString<N>`; propose 128 |
| bookmarks | `TagRecord` | `ownerPrincipal` | Same disposition as above |
| bookmarks | `TagRecord` | `name` | `SqlAnsiString<kMaxTagNameBytes>` (128 — already exists, `tag_dto.hpp`) |
| polls | `PollRecord` | `title` | `SqlAnsiString<kMaxTitleBytes>` (200) |
| polls | `OptionRecord` | `label` | `SqlAnsiString<kMaxOptionLabelBytes>` (100) |
| polls | `VoteRecord`, `CommentRecord`, `VoteHistoryRecord` | `participantName` | `SqlAnsiString<kMaxParticipantNameBytes>` (80) |
| polls | `CommentRecord` | `body` | `SqlAnsiString<kMaxCommentBytes>` (500) |
| polls | `VoteHistoryRecord` | `previousVotesJson` | `SqlMaxDynamicAnsiString` (serialized JSON, unbounded) |
| polls | `PollEventRecord` | `kind` | `SqlAnsiString<N>` — short internal enum-like tag; propose 32 |
| polls | `PollEventRecord` | `summary` | No existing bound — free text; propose `SqlMaxDynamicAnsiString` |

Bank has zero plain-`std::string` entity fields today (already fully on `SqlAnsiString<N>`) — no changes needed there for this item.

`poll_entity.hpp`'s existing WASM stub branch (`#else` empty structs) needs no changes — the stub fields don't exist at all under Emscripten, so there's nothing to retype.

## 4. `std::int64_t` epoch-ms fields → `Light::SqlDateTime`

morph already has a proper domain timestamp type wired end-to-end on the wire (`morph::time::DateTime`/`Timestamp`, `include/morph/util/datetime.hpp`) — ISO-8601 JSON on the wire, `std::chrono::sys_time<milliseconds>` as the value. Every rung's `*AtMs`/`timestampMs` entity field is that same value degraded to a raw `std::int64_t` at the storage boundary for no documented reason. `Light::SqlDateTime` (native type `std::chrono::system_clock::time_point`, per Lightweight) is the direct storage counterpart — same millisecond-scale instant, just typed instead of a bare integer.

**Change, per field:** `Light::Field<std::int64_t, ...>` (or `std::optional<std::int64_t>`) → `Light::Field<Light::SqlDateTime, ...>` (or `std::optional<Light::SqlDateTime>`). The model-layer conversion helpers collapse from the current two-step (`DateTime` → `int64_t` epoch-ms → column, and back) to a direct `sys_time<milliseconds>` ↔ `SqlDateTime::native_type` conversion — e.g. pastebin's `toEpochMs`/`fromEpochMs`/`nowMs` helpers are replaced by a single pair of `DateTime` ↔ `SqlDateTime` converters, reused verbatim across all four rungs the way `WithMapper`'s doc comments already say small internal details are duplicated per-TU.

Full inventory:

| Rung | Entity | Field(s) |
|---|---|---|
| bank | `LoanRecord` | `createdAtMs` |
| bank | `NotificationRecord` | `createdAtMs` |
| bank | `PaymentRecord` | `dueAtMs` |
| bank | `TxnRecord` | `createdAtMs` |
| bookmarks | `BookmarkRecord` | `createdAtMs`, `updatedAtMs` |
| bookmarks | `ImportedOpRecord` | `appliedAtMs` |
| bookmarks | `BookmarkOutboxRecord` | `timestampMs` |
| pastebin | `PasteRecord` | `createdAtMs`, `expiresAtMs` |
| polls | `PollRecord`, `CommentRecord`, `VoteHistoryRecord`, `PollEventRecord` | `createdAtMs` (each) |

Not touched: every `*Minor` monetary field (bank) and every plain ordering/counter integer (`sortOrder`, `finalizedOptionId`, `readCount`, `burnAfterReads`) — none of these are point-in-time values.

`examples/common/clock.hpp`'s `morph::ladder::now()` is unaffected — it already returns a proper `Timestamp`; only the entity-layer degradation to `int64_t` goes away.

## What does not change

- Wire protocol / DTOs / glaze `meta` specializations — every field listed above is a **storage-layer** retyping only. `PasteId`, `BookmarkDto`, `PollDto`, etc. keep their existing JSON shapes exactly.
- QML forms, presenters, bridges — none of them see `db::*Record` types directly (`IMPLEMENTATION.md`'s two-type-layer rule keeps entities out of the wire/UI layers already).
- Pool sizing/config, migration DDL generation strategy, Emscripten guard structure.
- Any rung's *surrogate* auto-increment primary keys (bank, bookmarks, polls) — GUID conversion is pastebin-only, per the reviewer's comment and the user's confirmation.

## Test impact (survey during planning, not exhaustive here)

- `test_paste_model.cpp`: the animal-name collision-retry test is deleted; new/updated GUID-format assertions; every hard-coded literal id in test fixtures needs to become a `SqlGuid`-shaped string or `SqlGuid::Create()` call.
- Every rung's model test file that constructs a `*Record` directly (rather than through DTOs) touches the retyped fields — a mechanical but wide-reaching update.
- `DataMapperPool`/`GlobalDataMapperPool()` is process-global and shared across every model everywhere, including different rungs' test binaries linked into the same process — needs a check that pool exhaustion isn't newly reachable under the ladder test suite's concurrency (multiple `DbFixture`-backed tests running models in the same process).

## Open items for planning

1. `bookmarks::db::*Record::ownerPrincipal`'s bound: no existing `kMax*Bytes` constant — check `auth_dto.hpp`/`bookmarks_authorizer.hpp` for an existing principal-length convention before inventing one.
2. `BookmarkRecord::description`/`notes`: no existing DTO-level cap at all today (the DTO fields are unbounded `std::string`) — decide bounded-with-new-constant vs. `SqlMaxDynamicAnsiString` during planning.
3. `PollEventRecord::summary`: same open question as above.
4. Confirm final `N` for the "propose N" internal-identifier fields (opId, outbox columns, idempotencyKey, PollEventRecord::kind) against actual observed value lengths in the existing code (e.g. `idempotencyKey`'s current format is `owner + "-action-" + nowMs + "-" + seq`, which bounds it in practice).

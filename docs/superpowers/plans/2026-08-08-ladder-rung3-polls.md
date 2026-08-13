# polls (rung 3) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build rung 3 of the [application ladder](../../../examples/LADDER.md)
— a Doodle-style scheduling-poll app anchored to
[Rallly](https://github.com/lukevella/rallly): one organizer creates a poll
with candidate dates, shares one link, participants vote yes/if-need-be/no
with no account, the organizer finalizes a date. The framework's first
`AllowShared`-over-real-WebSocket coverage, its first anonymous (tokenless
principal) authorization scheme, and the debut of the Zulip-pattern event
log every later rung reuses.

**Architecture:** One `PollModel`, keyed by `pollId` (`BRIDGE_MODEL_KEY`,
`BridgeHandler<PollModel, AllowShared>`), registered plain (not
`AllowShared` at the *authorization* layer — the shared *instance* directory
is what `AllowShared` opts into; ownership/admin-vs-participant gating is
entirely the model's own job, per this rung's own resolved design
decisions). SQLite via Lightweight, mirroring Rallly's Prisma models plus a
`poll_events` append-only log and a `vote_history` table for undo. Two
client executables (desktop `--server`/`Local`, WASM) sharing one QML/
presenter/model layer, per `IMPLEMENTATION.md`/`TESTING.md`.

**Tech Stack:** C++23, `morph::backend`/`bridge`/`session`/`journal`, Qt6
(desktop + WASM), SQLite via Lightweight ORM, Catch2.

## Global Constraints

- C++23 throughout.
- **DTO type discipline** (`examples/IMPLEMENTATION.md` rule 3): the only
  plain type permitted in an action/result field is `std::string`.
  Everything else is a strong type — with **exactly one, narrow, documented
  exception**: `OpenPoll::pollId` (and nowhere else) must be plain
  `std::string`, because `morph::model::ModelKey`'s concept
  (`include/morph/core/model_key.hpp:38-39`) requires an exact
  `std::same_as<K, std::string>` or `std::integral<K>` match — a wrapper
  type does not satisfy it, since `BRIDGE_MODEL_KEY`/`BRIDGE_KEY_FROM`
  deduce `PrimaryKey` directly from the member's own declared type. This
  is consistent with rule 3's own existing carve-out for natural-string
  identities (URLs, titles) — `pollId` is a shareable link token, never a
  user-typed value, never confused with an ordinary integer id precisely
  *because* it is a string. `OptionId`, `PollEventId`, and every other
  identity field in this rung are never the target of a keying macro and
  stay strong types, per the usual rule.
- **Persistence exclusively through Lightweight** (`IMPLEMENTATION.md` rule
  4). `SqlTransaction{mapper().Connection(), SqlTransactionMode::ROLLBACK}`
  wraps every multi-write mutation (a vote + its event-log row + its
  vote-history row are three writes that must commit or roll back
  together), the same pattern rung 1/2 already proved
  (`examples/bookmarks/src/models/bookmark_model.cpp:256-258`).
- **Shared instances are ownerless** (`docs/spec/core/shared_instances.md`,
  "Ownership and authorization" section): `authorizeInstance` gains nothing
  from being taught about admin/participant tokens — `PollModel` re-checks
  every admin-gated action's caller against the poll row's own
  `adminToken` column itself, the same shape rung 2's
  `authorizeInstance`-is-inert-for-finding-027, model-re-checks-ownership
  pattern already established.
- **No signed tokens, no `SigningAuthorizer`.** Unlike rung 1/2's
  HMAC-signed session tokens, this rung's admin/participant tokens are
  bare, server-generated random opaque strings compared directly against
  the poll row's own stored columns — there is no framework authorizer
  that verifies a *bare* shared secret (confirmed during this rung's design
  research: `docs/spec/security.md` has zero "capability"/"anonymous"
  content), so `PollModel::execute()` does the comparison itself,
  end to end. `PollsAuthorizer`'s job is narrower than
  `BookmarksAuthorizer`'s: `authorizeRegister`/`authorizeInstance` are both
  unconditionally permissive (finding 027 applies to shared/keyed
  registration too — see the README's design decisions), and there is no
  `authenticate()`-verified token at all, since nothing here is signed.
- **`CreatePoll` is native-client-only.** A result-keyed creating action's
  promote step (`Bridge::assignHandlerPrimary` → `IBackend::assignPrimary`)
  has no async path (finding 032, filed during this rung's framework-prereq
  work) — a WASM tab dispatching `CreatePoll` would still abort the page.
  Every WASM-facing task in this plan treats `CreatePoll` as
  desktop/`Local`-only; the WASM client task never wires a "create a poll"
  UI, only "join a poll" (`OpenPoll`, payload-keyed, fully async-safe after
  this rung's own framework prerequisite work).
- **Event log**: a genuine `poll_events` SQLite table (sequence id +
  payload per mutation), **table-wide monotonic autoincrement, not a
  timestamp** — rung 2's `BulkEdit`/`MergeTags` fix rounds both hit
  millisecond-collision bugs from timestamp-keyed uniqueness; an
  autoincrement primary key sidesteps that class of bug entirely. No epoch
  token (the README's resolved design decision 4: durable persistence
  alone closes the instance-rebirth gap the epoch token existed for).
- **Undo is 100% app-level.** `PollModel` owns its own `vote_history` table;
  `UndoLastVoteChange` reads and reverses the caller's own most recent
  entry via ordinary mutation. The framework's `SessionLog::undoLast()` is
  never called anywhere in this rung (it pops the newest entry regardless
  of principal and returns a detached, uninstallable holder — see the
  README's resolved design decision 3).
- Every public symbol needs complete Doxygen (`@param`/`@return`/`@tparam`)
  — the Docs CI workflow enforces `WARN_AS_ERROR = FAIL_ON_WARNINGS`.
- Model tests use the `morph::ladder::testkit` fixtures (`DbFixture`,
  `BackendRig`, `pumpUntil`, `awaitQt`) exactly as rung 1/2 established —
  no new testkit primitives needed for this rung's own model layer (the
  GUI/polling-helper task is the one place a new, reusable primitive is
  produced, per the DoD).

---

## Corrections to the plan's own source material

The polls README (`examples/polls/README.md`) already carries five resolved
design-decision corrections and two framework-prerequisite records, written
*before* this plan, per `LADDER.md`'s discipline rule. This plan does not
repeat that reasoning — read the README's "Design decisions" section first;
every task below assumes it.

---

### Task 1: Core types, units, and errors

**Files:**
- Create: `examples/polls/include/polls/core/types.hpp`
- Create: `examples/polls/include/polls/core/errors.hpp`
- Test: `examples/polls/tests/test_polls_types.cpp`

**Interfaces:**
- Produces: `PollId` (plain `std::string` — see Global Constraints), `OptionId`,
  `PollEventId`, `Count` quantity, `VoteChoice` enum, `ArchiveState`-analogue
  none needed (polls has no archive concept). `PollsError`, `NotFound`,
  `ValidationError`, `Forbidden`, `Conflict` — mirroring rung 2's exact
  hierarchy shape (`examples/bookmarks/include/bookmarks/core/errors.hpp`).

`OptionId`/`PollEventId` are ordinary strong types wrapping `std::int64_t`
(auto-increment SQLite row ids), following `BookmarkId`'s exact pattern
(`examples/bookmarks/include/bookmarks/core/types.hpp`). `PollId` is
**not** a strong type — see Global Constraints — but this header still
declares `kPollIdBytes` (the generated token's fixed length, e.g. 22 bytes
of URL-safe base64 from 16 random bytes, matching a nanoid-shaped
unguessable identifier) as a `constexpr std::size_t` so `CreatePoll`'s
implementation (Task 5) and its tests share one source of truth.

- [ ] **Step 1: Write `types.hpp`**

```cpp
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace polls {

/// @brief Length in bytes of a generated `pollId`/admin-token/participant-token
///        string: 22 URL-safe base64 characters encoding 16 random bytes,
///        matching a nanoid-shaped unguessable identifier. Shared by
///        `CreatePoll`'s implementation (Task 5) and its tests so the two
///        never drift.
inline constexpr std::size_t kTokenBytes = 22;

/// @brief Strong identifier for one candidate date/time option within a poll.
///        Never the target of a `BRIDGE_MODEL_KEY`/`BRIDGE_KEY_FROM` macro —
///        `PollModel` is keyed by `pollId` alone (see `OpenPoll` in
///        `dto/poll_dto.hpp`), so this stays an ordinary strong type per
///        `IMPLEMENTATION.md` rule 3.
struct OptionId {
    std::int64_t value{0};
    [[nodiscard]] constexpr std::int64_t operator*() const { return value; }
    [[nodiscard]] constexpr bool hasValue() const { return value != 0; }
    [[nodiscard]] constexpr bool operator==(const OptionId&) const = default;
};

/// @brief Strong identifier for one row in the `poll_events` append-only log.
///        Table-wide monotonic (not per-poll), autoincrement — see this
///        plan's Global Constraints on why a sequence id, not a timestamp.
struct PollEventId {
    std::int64_t value{0};
    [[nodiscard]] constexpr std::int64_t operator*() const { return value; }
    [[nodiscard]] constexpr bool hasValue() const { return value != 0; }
    [[nodiscard]] constexpr bool operator==(const PollEventId&) const = default;
};

/// @brief One participant's answer for one option.
enum class VoteChoice { Yes, IfNeedBe, No };

}  // namespace polls
```

Follow `BookmarkId`'s exact Doxygen/reflection pattern
(`examples/bookmarks/include/bookmarks/core/types.hpp`) for `OptionId`/
`PollEventId` — including whatever `glz::meta`/reflection registration that
file uses to make the strong type (de)serializable; read that file in full
before writing this one, since this plan does not repeat its exact
boilerplate here to avoid drift between the two.

- [ ] **Step 2: Write `errors.hpp`**

Mirror `examples/bookmarks/include/bookmarks/core/errors.hpp`'s exact
shape (`PollsError` base, `NotFound`/`ValidationError`/`Forbidden`/
`Conflict` derived, each with a `std::string` message member and the same
constructor/accessor pattern) — read that file first and reuse its
structure verbatim, renaming only the namespace and base class name. This
rung additionally needs `Conflict` for `FinalizePoll` racing a second
finalize attempt (the poll is already finalized) and for
`UndoLastVoteChange` when there is nothing to undo.

- [ ] **Step 3: Write the failing tests**

```cpp
// test_polls_types.cpp
TEST_CASE("OptionId/PollEventId are independently hasValue()-capable", "[polls][types]") {
    CHECK_FALSE(polls::OptionId{}.hasValue());
    CHECK(polls::OptionId{.value = 1}.hasValue());
    CHECK_FALSE(polls::PollEventId{}.hasValue());
    CHECK(polls::PollEventId{.value = 1}.hasValue());
}

TEST_CASE("OptionId equality follows the payload", "[polls][types]") {
    CHECK(polls::OptionId{.value = 5} == polls::OptionId{.value = 5});
    CHECK_FALSE(polls::OptionId{.value = 5} == polls::OptionId{.value = 6});
}

TEST_CASE("kTokenBytes is a plausible unguessable-token length", "[polls][types]") {
    STATIC_REQUIRE(polls::kTokenBytes >= 16);  // enough entropy to resist guessing
}

TEST_CASE("PollsError hierarchy: each derived type carries its own message", "[polls][types]") {
    CHECK(std::string_view{polls::NotFound{"poll not found"}.what()} == "poll not found");
    CHECK(std::string_view{polls::Forbidden{"not the admin"}.what()} == "not the admin");
    CHECK(std::string_view{polls::Conflict{"already finalized"}.what()} == "already finalized");
}
```

- [ ] **Step 4: Run to verify it fails, then passes**

Manual compile (no CMakeLists yet — Task 12 adds it):
```bash
clang++ -std=c++23 -Iinclude -I../../include ... -fsyntax-only tests/test_polls_types.cpp
```
(Use the manual clang++ recipe rung 1/2 established for pre-CMakeLists
tasks — vendored Lightweight/glaze/reflection-cpp/Qt include paths — see
this plan's Task 12 for when the real CMake target replaces it.)

- [ ] **Step 5: Commit**

```bash
git add examples/polls/include/polls/core/types.hpp examples/polls/include/polls/core/errors.hpp \
        examples/polls/tests/test_polls_types.cpp
git commit -m "polls: add core strong types and error hierarchy"
```

---

### Task 2: Poll and vote DTOs

**Files:**
- Create: `examples/polls/include/polls/dto/poll_dto.hpp`
- Test: `examples/polls/tests/test_poll_dto.cpp`

**Interfaces:**
- Consumes: `OptionId`, `VoteChoice`, `PollsError` hierarchy (Task 1).
- Produces: `CreatePoll`/`CreatePollResult`, `OpenPoll`, `GetPollState`/
  `GetPollStateResult`, `PollOptionView`, `PollView` — consumed by every
  model task (5-9) and every later task.

```cpp
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "polls/core/types.hpp"

#include <string>
#include <vector>

namespace polls {

constexpr std::size_t kMaxTitleBytes = 200;
constexpr std::size_t kMaxOptionLabelBytes = 100;
constexpr std::size_t kMinOptions = 2;
constexpr std::size_t kMaxOptions = 20;

/// @brief One candidate date/time, as free text (Rallly stores these as
///        ISO-ish date strings; this rung follows suit rather than parsing
///        into `morph::time::Timestamp`, since `morph::time` is UTC-only
///        and per-participant local rendering is explicitly GUI logic per
///        the README's "Expected strain points").
struct CreatePollOption {
    std::string label;
};

struct CreatePoll {
    std::string title;
    std::vector<CreatePollOption> options;

    [[nodiscard]] bool validate() const noexcept {
        if (title.empty() || title.size() > kMaxTitleBytes) {
            return false;
        }
        if (options.size() < kMinOptions || options.size() > kMaxOptions) {
            return false;
        }
        for (const auto& opt : options) {
            if (opt.label.empty() || opt.label.size() > kMaxOptionLabelBytes) {
                return false;
            }
        }
        return true;
    }
};

struct CreatePollResult {
    std::string pollId;         // the shareable link id -- see Global Constraints
    std::string adminToken;     // kept by the organizer only
    std::string participantToken;  // handed out with the shared link
};

/// @brief The keyed attach action -- `BRIDGE_MODEL_KEY(PollModel, OpenPoll, &OpenPoll::pollId)`.
struct OpenPoll {
    std::string pollId;

    [[nodiscard]] bool validate() const noexcept { return !pollId.empty(); }
};

struct GetPollState {
    [[nodiscard]] bool validate() const noexcept { return true; }
};

struct PollOptionView {
    OptionId id;
    std::string label;
    Count yesCount;
    Count ifNeedBeCount;
    Count noCount;
};

struct ParticipantVoteView {
    std::string participantName;
    OptionId optionId;
    VoteChoice choice;
};

struct CommentView {
    std::string participantName;
    std::string body;
};

struct GetPollStateResult {
    std::string pollId;
    std::string title;
    bool finalized{false};
    OptionId finalizedOptionId;  // hasValue() == false unless finalized
    std::vector<PollOptionView> options;
    std::vector<ParticipantVoteView> votes;
    std::vector<CommentView> comments;
    PollEventId lastEventId;  // GetEventsSince's starting cursor for a fresh client
};

}  // namespace polls
```

`Count` here is the same dimensionless quantity type rung 2 defined
(`examples/bookmarks/units.hpp`) — this task adds a polls-local copy
following that exact pattern (or, if the two rungs' `Count` types are
identical in shape, this task's implementer should check whether promoting
it to `examples/common/` is warranted; if the shapes match exactly and no
other rung currently shares it, define a local copy here rather than
introduce a cross-rung dependency this plan does not otherwise need —
default to the local copy unless it is trivially a one-line `using`).

- [ ] **Step 1: Write the failing tests**

```cpp
// test_poll_dto.cpp
TEST_CASE("CreatePoll requires a bounded title and 2-20 bounded-label options", "[polls][dto]") {
    polls::CreatePoll action;
    CHECK_FALSE(action.validate());  // no title, no options
    action.title = "Team offsite";
    CHECK_FALSE(action.validate());  // still no options
    action.options = {{"2026-09-01"}};
    CHECK_FALSE(action.validate());  // only one option
    action.options.push_back({"2026-09-02"});
    CHECK(action.validate());
    action.options.push_back({""});
    CHECK_FALSE(action.validate());  // empty label
    action.title = std::string(polls::kMaxTitleBytes + 1, 't');
    action.options = {{"a"}, {"b"}};
    CHECK_FALSE(action.validate());  // title too long
}

TEST_CASE("OpenPoll requires a non-empty pollId", "[polls][dto]") {
    CHECK_FALSE(polls::OpenPoll{}.validate());
    CHECK(polls::OpenPoll{.pollId = "abc"}.validate());
}

TEST_CASE("GetPollStateResult round-trips through JSON with every nested view populated", "[polls][dto]") {
    polls::GetPollStateResult result;
    result.pollId = "abc";
    result.title = "Team offsite";
    result.options.push_back({.id = polls::OptionId{.value = 1}, .label = "2026-09-01",
                               .yesCount = polls::Count::fromDouble(2.0)});
    result.votes.push_back({.participantName = "alice", .optionId = polls::OptionId{.value = 1},
                             .choice = polls::VoteChoice::Yes});
    result.comments.push_back({.participantName = "alice", .body = "works for me"});
    // Round-trip via ActionTraits<GetPollState>::resultToJson/resultFromJson once Task 3's
    // reflection registration exists -- this test moves to test_poll_dto.cpp's final form
    // only after that registration lands; if written before it, assert field values directly
    // instead of round-tripping, and extend with the JSON round-trip once Task 3 lands.
}
```

- [ ] **Step 2-4: Run to verify fail/pass, commit**

```bash
git add examples/polls/include/polls/dto/poll_dto.hpp examples/polls/tests/test_poll_dto.cpp
git commit -m "polls: add poll/vote/comment DTOs"
```

---

### Task 3: Bulk/undo/event DTOs and `ActionTraits`/`ModelTraits` reflection

**Files:**
- Create: `examples/polls/include/polls/dto/vote_dto.hpp`
- Create: `examples/polls/include/polls/dto/event_dto.hpp`
- Modify: `examples/polls/include/polls/dto/poll_dto.hpp` (add `BRIDGE_MODEL_KEY`)
- Test: `examples/polls/tests/test_vote_event_dto.cpp`

**Interfaces:**
- Consumes: Task 2's DTOs.
- Produces: `SubmitVotes`/`UpdateVotes`/`AddComment`, `FinalizePoll`,
  `UndoLastVoteChange`/`UndoLastVoteChangeResult`, `GetEventsSince`/
  `GetEventsSinceResult`, `PollEvent` (the event log's own payload shape).

```cpp
// vote_dto.hpp
#pragma once
#include "polls/core/types.hpp"
#include <string>
#include <vector>

namespace polls {

constexpr std::size_t kMaxParticipantNameBytes = 80;
constexpr std::size_t kMaxCommentBytes = 500;

struct OneVote {
    OptionId optionId;
    VoteChoice choice;
};

/// @brief First-time vote submission for one participant. Idempotent on
///        retry: a duplicate submission with the same participantName is
///        rejected by the option-uniqueness invariant (Task 6), never
///        double-counted.
struct SubmitVotes {
    std::string participantName;
    std::vector<OneVote> votes;

    [[nodiscard]] bool validate() const noexcept {
        return !participantName.empty() && participantName.size() <= kMaxParticipantNameBytes && !votes.empty();
    }
};

/// @brief Replaces an existing participant's votes wholesale.
struct UpdateVotes {
    std::string participantName;
    std::vector<OneVote> votes;

    [[nodiscard]] bool validate() const noexcept {
        return !participantName.empty() && participantName.size() <= kMaxParticipantNameBytes && !votes.empty();
    }
};

struct AddComment {
    std::string participantName;
    std::string body;

    [[nodiscard]] bool validate() const noexcept {
        return !participantName.empty() && participantName.size() <= kMaxParticipantNameBytes && !body.empty() &&
               body.size() <= kMaxCommentBytes;
    }
};

/// @brief Admin-token-gated: the poll becomes read-only.
struct FinalizePoll {
    OptionId optionId;

    [[nodiscard]] bool validate() const noexcept { return optionId.hasValue(); }
};

/// @brief Reverses the calling participant's own most recent vote change --
///        a compensating action against `vote_history`, never
///        `SessionLog::undoLast()`. See the README's resolved design
///        decision 3.
struct UndoLastVoteChange {
    std::string participantName;

    [[nodiscard]] bool validate() const noexcept {
        return !participantName.empty() && participantName.size() <= kMaxParticipantNameBytes;
    }
};

struct UndoLastVoteChangeResult {
    bool restored{false};  // false if there was nothing to undo (Conflict is thrown instead -- see Task 8)
};

}  // namespace polls
```

```cpp
// event_dto.hpp
#pragma once
#include "polls/core/types.hpp"
#include <string>
#include <vector>

namespace polls {

/// @brief One row of `poll_events` -- the Zulip-pattern generic polling
///        payload. `kind` is a small closed set (`"vote"`, `"comment"`,
///        `"finalize"`) a client switches on to know how to apply the
///        increment without re-fetching `GetPollState`.
struct PollEvent {
    PollEventId id;
    std::string kind;
    std::string summary;  // human-readable, e.g. "alice voted", "poll finalized"
};

struct GetEventsSince {
    PollEventId lastEventId;  // {} (value 0) means "from the beginning"

    [[nodiscard]] bool validate() const noexcept { return true; }
};

struct GetEventsSinceResult {
    std::vector<PollEvent> events;  // oldest first, every id > lastEventId
};

}  // namespace polls
```

Modify `poll_dto.hpp` to add the keying declaration immediately after
`OpenPoll`'s definition:

```cpp
}  // namespace polls

BRIDGE_MODEL_KEY(polls::PollModel, polls::OpenPoll, &polls::OpenPoll::pollId);
```

(`PollModel` is forward-declared or fully declared by the point this macro
is reached — confirm the exact forward-declaration/include shape rung 2's
`bookmark_model.hpp`/`BRIDGE_MODEL_KEY` usage follows, since `PollModel`
itself is not defined until Task 5; the macro only needs the type named,
matching `docs/spec/core/shared_instances.md`'s own example. Place this
`BRIDGE_MODEL_KEY` invocation in whichever header the model-key
research/spec shows is the conventional location — likely `poll_dto.hpp`
itself if `bookmarks::BookmarkModel`'s `BRIDGE_REGISTER_ACTION` macros set
the precedent of living beside the model class, or `models/poll_model.hpp`
if `BRIDGE_MODEL_KEY` specifically wants to live beside the model's own
declaration — check `docs/spec/core/shared_instances.md`'s worked example
for the established convention before choosing.)

- [ ] **Step 1: Write the failing tests**

```cpp
// test_vote_event_dto.cpp
TEST_CASE("SubmitVotes/UpdateVotes require a bounded participantName and at least one vote", "[polls][dto]") {
    polls::SubmitVotes action;
    CHECK_FALSE(action.validate());
    action.participantName = "alice";
    CHECK_FALSE(action.validate());  // no votes yet
    action.votes.push_back({.optionId = polls::OptionId{.value = 1}, .choice = polls::VoteChoice::Yes});
    CHECK(action.validate());
}

TEST_CASE("AddComment requires a bounded body", "[polls][dto]") {
    polls::AddComment action{.participantName = "alice", .body = ""};
    CHECK_FALSE(action.validate());
    action.body = std::string(polls::kMaxCommentBytes + 1, 'x');
    CHECK_FALSE(action.validate());
    action.body = "works for me";
    CHECK(action.validate());
}

TEST_CASE("FinalizePoll requires a real optionId", "[polls][dto]") {
    CHECK_FALSE(polls::FinalizePoll{}.validate());
    CHECK(polls::FinalizePoll{.optionId = polls::OptionId{.value = 1}}.validate());
}

TEST_CASE("GetEventsSince{} (lastEventId unset) validates -- it means \"from the beginning\"", "[polls][dto]") {
    CHECK(polls::GetEventsSince{}.validate());
}
```

- [ ] **Step 2-4: Run to verify fail/pass, commit**

```bash
git add examples/polls/include/polls/dto/vote_dto.hpp examples/polls/include/polls/dto/event_dto.hpp \
        examples/polls/include/polls/dto/poll_dto.hpp examples/polls/tests/test_vote_event_dto.cpp
git commit -m "polls: add vote/undo/event DTOs and the BRIDGE_MODEL_KEY declaration"
```

---

### Task 4: Entities, schema, and `db_model.hpp`

**Files:**
- Create: `examples/polls/include/polls/db/poll_entity.hpp`
- Create: `examples/polls/include/polls/db/db_model.hpp`
- Create: `examples/polls/include/polls/db/database.hpp`
- Create: `examples/polls/src/db/schema.cpp`
- Test: `examples/polls/tests/test_polls_schema.cpp`

**Interfaces:**
- Produces: `db::PollRecord`, `db::OptionRecord`, `db::VoteRecord`,
  `db::CommentRecord`, `db::VoteHistoryRecord`, `db::PollEventRecord`,
  `db::WithMapper`, `db::setup(connectionString)`.

`db_model.hpp` is a byte-for-byte copy of
`examples/bookmarks/include/bookmarks/db/db_model.hpp`'s `WithMapper`
mixin (the `#ifndef __EMSCRIPTEN__` two-branch pattern, finding 025) —
read that file and reuse it verbatim, renaming only the namespace.

```cpp
// poll_entity.hpp
#pragma once
#ifndef __EMSCRIPTEN__
#include <Lightweight/DataMapper/DataMapper.hpp>
#endif
#include <cstdint>
#include <string>

namespace polls::db {

#ifndef __EMSCRIPTEN__

struct PollRecord {
    Lightweight::PrimaryKey<std::uint64_t, Lightweight::AutoIncrement> id;
    Lightweight::SqlAnsiString<22> pollId;         // unique-indexed shareable link id
    Lightweight::SqlAnsiString<22> adminToken;     // unique-indexed
    Lightweight::SqlAnsiString<22> participantToken;  // unique-indexed
    Lightweight::SqlAnsiString<200> title;
    bool finalized{false};
    std::uint64_t finalizedOptionId{0};  // 0 = not finalized; FK-shaped but not FK-enforced (SQLite)
    std::uint64_t createdAtMs{0};
};

struct OptionRecord {
    Lightweight::PrimaryKey<std::uint64_t, Lightweight::AutoIncrement> id;
    Lightweight::BelongsTo<&PollRecord::id> poll;
    Lightweight::SqlAnsiString<100> label;
    std::uint64_t sortOrder{0};  // preserves CreatePoll's option order across storage/query
};

/// @brief One participant's current vote for one option. Unique on
///        (pollId, participantName, optionId) so a retried SubmitVotes
///        cannot double-count -- see Task 6's own doc comment on the exact
///        index this rung's DoD names.
struct VoteRecord {
    Lightweight::PrimaryKey<std::uint64_t, Lightweight::AutoIncrement> id;
    Lightweight::BelongsTo<&PollRecord::id> poll;
    Lightweight::BelongsTo<&OptionRecord::id> option;
    Lightweight::SqlAnsiString<80> participantName;
    std::uint8_t choice{0};  // VoteChoice's underlying value
};

struct CommentRecord {
    Lightweight::PrimaryKey<std::uint64_t, Lightweight::AutoIncrement> id;
    Lightweight::BelongsTo<&PollRecord::id> poll;
    Lightweight::SqlAnsiString<80> participantName;
    Lightweight::SqlAnsiString<500> body;
    std::uint64_t createdAtMs{0};
};

/// @brief Undo's own history, one row per vote-changing call
///        (`SubmitVotes`/`UpdateVotes`), storing the *previous* state so
///        `UndoLastVoteChange` can restore it. Never read by anything but
///        `UndoLastVoteChange` -- not the audit trail (the framework
///        journal covers that separately).
struct VoteHistoryRecord {
    Lightweight::PrimaryKey<std::uint64_t, Lightweight::AutoIncrement> id;
    Lightweight::BelongsTo<&PollRecord::id> poll;
    Lightweight::SqlAnsiString<80> participantName;
    Lightweight::SqlAnsiString<4096> previousVotesJson;  // the pre-change vote set, JSON-encoded
    std::uint64_t createdAtMs{0};
};

/// @brief The event log. Table-wide autoincrement `id` is `PollEventId`'s
///        wire value directly -- see this plan's Global Constraints.
struct PollEventRecord {
    Lightweight::PrimaryKey<std::uint64_t, Lightweight::AutoIncrement> id;
    Lightweight::BelongsTo<&PollRecord::id> poll;
    Lightweight::SqlAnsiString<16> kind;
    Lightweight::SqlAnsiString<200> summary;
    std::uint64_t createdAtMs{0};
};

#else
// Client-only (WASM) build: entity shapes are never instantiated, only
// referenced by type in code that never runs there. See finding 025.
struct PollRecord {};
struct OptionRecord {};
struct VoteRecord {};
struct CommentRecord {};
struct VoteHistoryRecord {};
struct PollEventRecord {};
#endif

}  // namespace polls::db
```

Follow `examples/bookmarks/include/bookmarks/db/bookmark_entity.hpp` and
`bookmark_tag_entity.hpp` for the exact `BelongsTo`/`SqlAnsiString`/
`PrimaryKey<..., AutoIncrement>` syntax this plan's sketch above
approximates — read both files in full and correct any field-declaration
syntax mismatches against the real Lightweight API before writing this
file, since this plan's own sketch is illustrative of the *shape*, not a
verified compile of the exact Lightweight template arguments.

**Global Constraints reminder** (from this plan's own Global Constraints
section, and rung 2's own hard-won Task 5 finding): entities carry **zero
relation-typed members** beyond `BelongsTo` (never `HasMany`/
`HasManyThrough` — incompatible with `DataMapper::Update()`, confirmed
against Lightweight's vendored source during rung 2's own Task 5 research).
`OptionRecord`/`VoteRecord`/`CommentRecord`/`PollEventRecord` are read via
plain `Query<T>().Where(FieldNameOf<&T::poll>, "=", pollDbId)` calls in the
model, never through an embedded relation field.

- [ ] **Step 1: Write `db/database.hpp` and `src/db/schema.cpp`**

Mirror `examples/bookmarks/include/bookmarks/db/database.hpp` and
`src/db/schema.cpp` exactly: `setup(connectionString)` opens the
connection and calls `CreateSchema` (or whatever exact Lightweight
schema-migration entry point bookmarks' `schema.cpp` uses) once, idempotent
on repeated calls (tests construct a fresh `DbFixture` per case, matching
rung 1/2's own established pattern — read `examples/common/testkit/db_fixture.hpp`
if unfamiliar with how `setup()` composes with it).

- [ ] **Step 2: Write the failing tests**

```cpp
// test_polls_schema.cpp
TEST_CASE("The polls schema creates all six tables and a poll round-trips", "[polls][db]") {
    DbFixture fixture;
    Lightweight::DataMapper mapper;

    polls::db::PollRecord poll;
    poll.pollId = "poll-abc";
    poll.adminToken = "admin-xyz";
    poll.participantToken = "part-xyz";
    poll.title = "Team offsite";
    poll.createdAtMs = 1000;
    mapper.Create(poll);
    REQUIRE(poll.id.Value() != 0);

    polls::db::OptionRecord opt;
    opt.poll = poll;
    opt.label = "2026-09-01";
    opt.sortOrder = 0;
    mapper.Create(opt);

    auto loaded = mapper.Query<polls::db::OptionRecord>()
                      .Where(::Lightweight::FieldNameOf<&polls::db::OptionRecord::poll>, "=", poll.id.Value())
                      .All();
    REQUIRE(loaded.size() == 1);
    CHECK(loaded.front().label.value() == "2026-09-01");
}
```

- [ ] **Step 3-4: Run to verify fail/pass, commit**

```bash
git add examples/polls/include/polls/db/ examples/polls/src/db/schema.cpp \
        examples/polls/tests/test_polls_schema.cpp
git commit -m "polls: add entities, schema, and db_model.hpp"
```

---

### Task 5: `PollModel` — `CreatePoll`, `OpenPoll`/`GetPollState`

**Files:**
- Create: `examples/polls/include/polls/models/poll_model.hpp`
- Create: `examples/polls/src/models/poll_model.cpp`
- Test: `examples/polls/tests/test_poll_model.cpp`

**Interfaces:**
- Consumes: Tasks 1-4.
- Produces: `PollModel` class, `PollModel::execute(CreatePoll)`,
  `execute(OpenPoll)`, `execute(GetPollState)`, `requireAdmin()`/
  `requireParticipant()` (private helpers every later model task reuses),
  `nowMs()` (via `examples/common/clock.hpp`, the same injectable-time
  convention rung 1/2 established).

```cpp
// poll_model.hpp
#pragma once
#include "polls/db/db_model.hpp"
#include "polls/dto/event_dto.hpp"
#include "polls/dto/poll_dto.hpp"
#include "polls/dto/vote_dto.hpp"

#include <morph/core/registry.hpp>

namespace polls {

class PollModel : public db::WithMapper {
  public:
    CreatePollResult execute(const CreatePoll& action);
    GetPollStateResult execute(const OpenPoll& action);
    GetPollStateResult execute(const GetPollState& action);
    GetPollStateResult execute(const SubmitVotes& action);
    GetPollStateResult execute(const UpdateVotes& action);
    GetPollStateResult execute(const AddComment& action);
    GetPollStateResult execute(const FinalizePoll& action);
    UndoLastVoteChangeResult execute(const UndoLastVoteChange& action);
    GetEventsSinceResult execute(const GetEventsSince& action);
};

}  // namespace polls

// PollModel is keyed by OpenPoll::pollId -- see Task 3's BRIDGE_MODEL_KEY
// (relocated here if Task 3's placeholder placement pointed at this file;
// confirm against the shared_instances.md worked example, as noted there).

BRIDGE_REGISTER_MODEL(polls::PollModel, "PollModel");
BRIDGE_REGISTER_ACTION(polls::PollModel, polls::CreatePoll, "CreatePoll", Loggable::Yes);
BRIDGE_REGISTER_ACTION(polls::PollModel, polls::OpenPoll, "OpenPoll", Loggable::No);
BRIDGE_REGISTER_ACTION(polls::PollModel, polls::GetPollState, "GetPollState", Loggable::No);
BRIDGE_REGISTER_ACTION(polls::PollModel, polls::SubmitVotes, "SubmitVotes", Loggable::Yes);
BRIDGE_REGISTER_ACTION(polls::PollModel, polls::UpdateVotes, "UpdateVotes", Loggable::Yes);
BRIDGE_REGISTER_ACTION(polls::PollModel, polls::AddComment, "AddComment", Loggable::Yes);
BRIDGE_REGISTER_ACTION(polls::PollModel, polls::FinalizePoll, "FinalizePoll", Loggable::Yes);
BRIDGE_REGISTER_ACTION(polls::PollModel, polls::UndoLastVoteChange, "UndoLastVoteChange", Loggable::Yes);
BRIDGE_REGISTER_ACTION(polls::PollModel, polls::GetEventsSince, "GetEventsSince", Loggable::No);
```

(Confirm the exact `BRIDGE_REGISTER_ACTION`/`Loggable` enum spelling against
`examples/bookmarks/include/bookmarks/models/bookmark_model.hpp`'s own
macro invocations before writing this verbatim — this plan's sketch
follows that file's shape from memory, not a fresh read.)

`poll_model.cpp`'s `CreatePoll`/`OpenPoll` implementations:

```cpp
namespace {
std::string randomToken() {
    // 16 random bytes -> 22-char URL-safe base64, matching kTokenBytes.
    // Use whatever CSPRNG primitive the codebase already has (check
    // morph::session::TokenIssuer's own random-generation for a
    // precedent, or std::random_device seeding a byte buffer directly if
    // no shared helper exists) -- do NOT use std::rand() or a
    // time-seeded PRNG, since these tokens are the whole security
    // boundary for admin/participant identity in this rung.
}
}  // namespace

CreatePollResult PollModel::execute(const CreatePoll& action) {
    if (!action.validate()) {
        throw ValidationError{"CreatePoll: a bounded title and 2-20 bounded-label options are required"};
    }
    db::PollRecord poll;
    poll.pollId = randomToken();
    poll.adminToken = randomToken();
    poll.participantToken = randomToken();
    poll.title = action.title;
    poll.createdAtMs = nowMs();

    ::Lightweight::SqlTransaction transaction{mapper().Connection(), ::Lightweight::SqlTransactionMode::ROLLBACK};
    mapper().Create(poll);
    std::uint64_t order = 0;
    for (const auto& opt : action.options) {
        db::OptionRecord rec;
        rec.poll = poll;
        rec.label = opt.label;
        rec.sortOrder = order++;
        mapper().Create(rec);
    }
    transaction.Commit();

    return CreatePollResult{
        .pollId = poll.pollId.value(), .adminToken = poll.adminToken.value(), .participantToken = poll.participantToken.value()};
}

namespace {
db::PollRecord loadPollByPollId(::Lightweight::DataMapper& mapper, const std::string& pollId) {
    auto rows = mapper.Query<db::PollRecord>()
                    .Where(::Lightweight::FieldNameOf<&db::PollRecord::pollId>, "=", pollId)
                    .All();
    if (rows.empty()) {
        throw NotFound{"poll not found"};
    }
    return std::move(rows.front());
}

GetPollStateResult buildState(::Lightweight::DataMapper& mapper, const db::PollRecord& poll) {
    GetPollStateResult result;
    result.pollId = poll.pollId.value();
    result.title = poll.title.value();
    result.finalized = poll.finalized;
    if (poll.finalized) {
        result.finalizedOptionId = OptionId{.value = static_cast<std::int64_t>(poll.finalizedOptionId)};
    }
    auto options = mapper.Query<db::OptionRecord>()
                       .Where(::Lightweight::FieldNameOf<&db::OptionRecord::poll>, "=", poll.id.Value())
                       .OrderBy(::Lightweight::FieldNameOf<&db::OptionRecord::sortOrder>)
                       .All();
    auto votes = mapper.Query<db::VoteRecord>()
                     .Where(::Lightweight::FieldNameOf<&db::VoteRecord::poll>, "=", poll.id.Value())
                     .All();
    for (const auto& opt : options) {
        PollOptionView view{.id = OptionId{.value = static_cast<std::int64_t>(opt.id.Value())}, .label = opt.label.value()};
        for (const auto& vote : votes) {
            if (vote.option.RecordId() != opt.id.Value()) {
                continue;
            }
            switch (static_cast<VoteChoice>(vote.choice)) {
                case VoteChoice::Yes: view.yesCount = view.yesCount + Count::fromDouble(1.0); break;
                case VoteChoice::IfNeedBe: view.ifNeedBeCount = view.ifNeedBeCount + Count::fromDouble(1.0); break;
                case VoteChoice::No: view.noCount = view.noCount + Count::fromDouble(1.0); break;
            }
            result.votes.push_back({.participantName = vote.participantName.value(),
                                     .optionId = view.id, .choice = static_cast<VoteChoice>(vote.choice)});
        }
        result.options.push_back(std::move(view));
    }
    auto comments = mapper.Query<db::CommentRecord>()
                        .Where(::Lightweight::FieldNameOf<&db::CommentRecord::poll>, "=", poll.id.Value())
                        .All();
    for (const auto& c : comments) {
        result.comments.push_back({.participantName = c.participantName.value(), .body = c.body.value()});
    }
    auto lastEvent = mapper.Query<db::PollEventRecord>()
                          .Where(::Lightweight::FieldNameOf<&db::PollEventRecord::poll>, "=", poll.id.Value())
                          .OrderByDescending(::Lightweight::FieldNameOf<&db::PollEventRecord::id>)
                          .First();
    result.lastEventId = lastEvent ? PollEventId{.value = static_cast<std::int64_t>(lastEvent->id.Value())} : PollEventId{};
    return result;
}
}  // namespace

GetPollStateResult PollModel::execute(const OpenPoll& action) {
    if (!action.validate()) {
        throw ValidationError{"OpenPoll: pollId is required"};
    }
    return buildState(mapper(), loadPollByPollId(mapper(), action.pollId));
}

GetPollStateResult PollModel::execute(const GetPollState& /*action*/) {
    // GetPollState carries no pollId of its own -- it is dispatched against
    // an already-attached handler (attach happens via OpenPoll, a
    // payload-keyed action, per BridgeHandler<Model, AllowShared>::attach()
    // or execute(OpenPoll{...})). Re-derive the poll from the handler's own
    // bound instance: since this is a keyed model, `this` IS the poll's
    // instance -- but PollModel as sketched above has no member state
    // naming which poll it is. Resolve this before implementing: either
    // (a) PollModel caches its own pollId once OpenPoll first attaches it
    // (a private member set in execute(OpenPoll), read here), matching
    // how a keyed model instance is conceptually "the poll" for its whole
    // lifetime once attached, or (b) GetPollState is redundant with OpenPoll
    // and should be removed from the plan/README (OpenPoll already returns
    // full state). Recommended: (a) -- add a private std::optional<std::string>
    // _pollId member, set (once) at the top of execute(OpenPoll) before
    // dispatching to the shared buildState() helper, and have
    // execute(GetPollState) throw NotFound if _pollId is unset (the handler
    // was never attached via OpenPoll -- a caller error) or look up the
    // cached id otherwise. Implement this exact shape; do not leave
    // GetPollState unable to find its own poll.
    ...
}
```

The `execute(GetPollState)` ambiguity above is a genuine open design
question this plan's own research did not fully resolve — the brief's
recommendation (cache `pollId` on first `OpenPoll` attach) is the
implementer's concrete instruction; if a review finds a better shape,
that is a normal task-review finding, not a plan defect requiring human
arbitration (this is an implementation-detail choice, not a value
judgment the plan deliberately left open).

- [ ] **Step 2: Write the failing tests**

```cpp
// test_poll_model.cpp
TEST_CASE("CreatePoll returns three distinct tokens and OpenPoll finds the same poll", "[polls][model]") {
    DbFixture fixture;
    PollModel model;
    auto created = model.execute(CreatePoll{.title = "Team offsite", .options = {{"2026-09-01"}, {"2026-09-02"}}});
    CHECK_FALSE(created.pollId.empty());
    CHECK_FALSE(created.adminToken.empty());
    CHECK_FALSE(created.participantToken.empty());
    CHECK(created.pollId != created.adminToken);
    CHECK(created.adminToken != created.participantToken);

    auto state = model.execute(OpenPoll{.pollId = created.pollId});
    CHECK(state.title == "Team offsite");
    CHECK(state.options.size() == 2);
    CHECK_FALSE(state.finalized);
}

TEST_CASE("OpenPoll against an unknown pollId throws NotFound", "[polls][model]") {
    DbFixture fixture;
    PollModel model;
    CHECK_THROWS_AS(model.execute(OpenPoll{.pollId = "no-such-poll"}), NotFound);
}

TEST_CASE("Two CreatePoll calls never collide on pollId/adminToken/participantToken", "[polls][model]") {
    DbFixture fixture;
    PollModel model;
    auto a = model.execute(CreatePoll{.title = "A", .options = {{"1"}, {"2"}}});
    auto b = model.execute(CreatePoll{.title = "B", .options = {{"1"}, {"2"}}});
    CHECK(a.pollId != b.pollId);
    CHECK(a.adminToken != b.adminToken);
    CHECK(a.participantToken != b.participantToken);
}
```

- [ ] **Step 3-4: Run to verify fail/pass, commit**

```bash
git add examples/polls/include/polls/models/poll_model.hpp examples/polls/src/models/poll_model.cpp \
        examples/polls/tests/test_poll_model.cpp
git commit -m "polls: add PollModel -- CreatePoll, OpenPoll, GetPollState"
```

---

### Task 6: `PollModel` — `SubmitVotes`/`UpdateVotes`/`AddComment`

**Files:**
- Modify: `examples/polls/include/polls/models/poll_model.hpp` (private helpers)
- Modify: `examples/polls/src/models/poll_model.cpp`
- Modify: `examples/polls/include/polls/db/poll_entity.hpp` (unique index)
- Test: `examples/polls/tests/test_poll_model.cpp` (append)

**Interfaces:**
- Consumes: Task 5's `_pollId` cache pattern, `loadPollByPollId`/`buildState`.
- Produces: `execute(SubmitVotes)`/`execute(UpdateVotes)`/`execute(AddComment)`,
  each writing a `VoteHistoryRecord` first (undo's data source, Task 8).

Add a unique constraint (or unique index, whichever Lightweight's schema
declaration supports — check `examples/bookmarks/include/bookmarks/db/bookmark_tag_entity.hpp`
for the precedent, since `BookmarkTagRecord` already has a
"never duplicate this pairing" invariant) on
`(poll, participantName, option)` in `VoteRecord` — this is the DoD's
"participant-token + option uniqueness is a model invariant, tested under
retry" requirement.

`execute(SubmitVotes)`/`execute(UpdateVotes)` share almost all their logic
(delete-then-recreate the participant's vote rows, wrapped in one
transaction with a `VoteHistoryRecord` write and a `PollEventRecord`
write) — factor a private `applyVotes(participantName, votes, kind)`
helper both call, `kind` distinguishing the event summary text
("submitted votes" vs. "updated votes"). Both throw `Conflict` if
`poll.finalized` is true (a vote after finalize is a real dead-letter
scenario the DoD names: "A vote in flight ... when FinalizePoll lands must
dead-letter with a user-visible outcome, not vanish" — `Conflict` IS that
visible outcome, delivered through the caller's `.onError(...)`).

`execute(AddComment)` similarly writes a `CommentRecord` + `PollEventRecord`
in one transaction, but writes no `VoteHistoryRecord` (comments are not
undoable per the README's scope — only vote *changes* are, matching
`UndoLastVoteChange`'s own name).

Every one of these three actions returns the freshly-rebuilt
`GetPollStateResult` via `buildState()` (Task 5) — the DoD wants a client
to see its own change reflected immediately, not only via the next
`GetEventsSince` poll.

- [ ] **Step 1: Write the failing tests**

```cpp
TEST_CASE("SubmitVotes writes one vote per option, visible in the next GetPollState", "[polls][model]") {
    DbFixture fixture;
    PollModel model;
    auto created = model.execute(CreatePoll{.title = "T", .options = {{"1"}, {"2"}}});
    model.execute(OpenPoll{.pollId = created.pollId});
    auto opts = model.execute(GetPollState{}).options;

    auto state = model.execute(SubmitVotes{.participantName = "alice",
        .votes = {{.optionId = opts[0].id, .choice = VoteChoice::Yes},
                  {.optionId = opts[1].id, .choice = VoteChoice::No}}});
    CHECK(state.options[0].yesCount == Count::fromDouble(1.0));
    CHECK(state.options[1].noCount == Count::fromDouble(1.0));
    REQUIRE(state.votes.size() == 2);
}

TEST_CASE("A retried SubmitVotes for the same participant does not double-count", "[polls][model]") {
    DbFixture fixture;
    PollModel model;
    auto created = model.execute(CreatePoll{.title = "T", .options = {{"1"}, {"2"}}});
    model.execute(OpenPoll{.pollId = created.pollId});
    auto opts = model.execute(GetPollState{}).options;
    SubmitVotes action{.participantName = "alice", .votes = {{.optionId = opts[0].id, .choice = VoteChoice::Yes}}};
    model.execute(action);
    // The DoD names this as a retry scenario: the strand serializes but
    // does not dedup by itself, so the model's own unique constraint (or
    // UpdateVotes-shaped upsert logic) must be what actually prevents
    // double-counting -- assert on the real outcome, not the mechanism:
    auto state = model.execute(action);  // retried identically
    CHECK(state.options[0].yesCount == Count::fromDouble(1.0));  // still 1, not 2
}

TEST_CASE("UpdateVotes replaces a participant's prior votes wholesale", "[polls][model]") {
    DbFixture fixture;
    PollModel model;
    auto created = model.execute(CreatePoll{.title = "T", .options = {{"1"}, {"2"}}});
    model.execute(OpenPoll{.pollId = created.pollId});
    auto opts = model.execute(GetPollState{}).options;
    model.execute(SubmitVotes{.participantName = "alice", .votes = {{.optionId = opts[0].id, .choice = VoteChoice::Yes}}});
    auto state = model.execute(UpdateVotes{.participantName = "alice", .votes = {{.optionId = opts[1].id, .choice = VoteChoice::Yes}}});
    CHECK(state.options[0].yesCount == Count::fromDouble(0.0));  // alice's old vote is gone
    CHECK(state.options[1].yesCount == Count::fromDouble(1.0));
}

TEST_CASE("SubmitVotes against a finalized poll throws Conflict, a visible dead-letter outcome", "[polls][model]") {
    DbFixture fixture;
    PollModel model;
    auto created = model.execute(CreatePoll{.title = "T", .options = {{"1"}, {"2"}}});
    model.execute(OpenPoll{.pollId = created.pollId});
    auto opts = model.execute(GetPollState{}).options;
    ScopedPrincipal admin{created.adminToken};  // or however the admin-token context is threaded -- see Task 7
    model.execute(FinalizePoll{.optionId = opts[0].id});
    CHECK_THROWS_AS(model.execute(SubmitVotes{.participantName = "bob",
        .votes = {{.optionId = opts[0].id, .choice = VoteChoice::Yes}}}), Conflict);
}

TEST_CASE("AddComment writes a comment visible in the next GetPollState", "[polls][model]") {
    DbFixture fixture;
    PollModel model;
    auto created = model.execute(CreatePoll{.title = "T", .options = {{"1"}, {"2"}}});
    model.execute(OpenPoll{.pollId = created.pollId});
    auto state = model.execute(AddComment{.participantName = "alice", .body = "works for me"});
    REQUIRE(state.comments.size() == 1);
    CHECK(state.comments.front().body == "works for me");
}
```

(The `FinalizePoll`-needs-admin-context line above is a forward reference
to Task 7's authorization mechanism — if Task 6 is implemented before
Task 7 lands, either stub `FinalizePoll` minimally first or reorder so
Task 7 lands before this test is written; the plan lists them in this
order for narrative clarity, not a hard dependency the implementer must
preserve if reordering is cleaner.)

- [ ] **Step 2-4: Run to verify fail/pass, commit**

```bash
git add examples/polls/include/polls/models/poll_model.hpp examples/polls/src/models/poll_model.cpp \
        examples/polls/include/polls/db/poll_entity.hpp examples/polls/tests/test_poll_model.cpp
git commit -m "polls: add SubmitVotes, UpdateVotes, AddComment"
```

---

### Task 7: `PollModel` — `FinalizePoll` and admin/participant token verification

**Files:**
- Modify: `examples/polls/include/polls/models/poll_model.hpp`
- Modify: `examples/polls/src/models/poll_model.cpp`
- Create: `examples/polls/include/polls/auth/polls_authorizer.hpp`
- Create: `examples/polls/src/auth/polls_authorizer.cpp`
- Test: `examples/polls/tests/test_poll_model.cpp` (append), `examples/polls/tests/test_polls_authorizer.cpp`

**Interfaces:**
- Produces: `PollsAuthorizer` (implements `morph::session::IAuthorizer`,
  `authorizeRegister`/`authorizeInstance` both unconditionally `true` — see
  Global Constraints), `PollModel::requireAdminToken(const std::string&)`
  (private, throws `Forbidden` on mismatch against the cached poll row's
  `adminToken`).

`FinalizePoll` is the one action in this rung that genuinely needs the
caller to *prove* they hold the admin token, not merely name a
participant. `session::Context::token` (design decision 1) carries it.
`PollModel::execute(const FinalizePoll&)`:

```cpp
GetPollStateResult PollModel::execute(const FinalizePoll& action) {
    if (!action.validate()) {
        throw ValidationError{"FinalizePoll: a real optionId is required"};
    }
    auto poll = loadPollByPollId(mapper(), requirePollId());  // requirePollId(): see Task 5's _pollId resolution
    const auto* ctx = ::morph::session::current();
    if (ctx == nullptr || ctx->token != poll.adminToken.value()) {
        throw Forbidden{"FinalizePoll requires the admin token"};
    }
    if (poll.finalized) {
        throw Conflict{"poll is already finalized"};
    }
    ::Lightweight::SqlTransaction transaction{mapper().Connection(), ::Lightweight::SqlTransactionMode::ROLLBACK};
    poll.finalized = true;
    poll.finalizedOptionId = static_cast<std::uint64_t>(*action.optionId);
    mapper().Update(poll);
    db::PollEventRecord event;
    event.poll = poll;
    event.kind = "finalize";
    event.summary = "poll finalized";
    event.createdAtMs = nowMs();
    mapper().Create(event);
    transaction.Commit();
    return buildState(mapper(), poll);
}
```

`PollsAuthorizer` mirrors `BookmarksAuthorizer`'s minimal shape
(`examples/bookmarks/include/bookmarks/auth/bookmarks_authorizer.hpp`) but
is even narrower: since nothing here is a signed token,
`authorizeRegister`/`authorizeInstance` are the whole class — read
`BookmarksAuthorizer`'s doc comments on why `authorizeRegister` must stay
permissive (finding 027) and reuse that reasoning verbatim, extended to
cover the shared/keyed registration path too (design decision 2 in the
README — `registerModelShared`/`attachModel`'s wire form is still a
`register` envelope carrying no session, per finding 027's scope).

- [ ] **Step 1: Write the failing tests**

```cpp
// test_poll_model.cpp (append)
TEST_CASE("FinalizePoll requires the admin token in Context::token", "[polls][model]") {
    DbFixture fixture;
    PollModel model;
    auto created = model.execute(CreatePoll{.title = "T", .options = {{"1"}, {"2"}}});
    model.execute(OpenPoll{.pollId = created.pollId});
    auto opts = model.execute(GetPollState{}).options;

    // No token at all:
    CHECK_THROWS_AS(model.execute(FinalizePoll{.optionId = opts[0].id}), Forbidden);

    // Wrong token (the participant token, not the admin token):
    {
        morph::session::Context ctx;
        ctx.token = created.participantToken;
        morph::session::ScopedContext scoped{ctx};  // or whichever RAII context-installer this codebase uses -- match ScopedPrincipal's pattern
        CHECK_THROWS_AS(model.execute(FinalizePoll{.optionId = opts[0].id}), Forbidden);
    }

    // Right token:
    {
        morph::session::Context ctx;
        ctx.token = created.adminToken;
        morph::session::ScopedContext scoped{ctx};
        auto state = model.execute(FinalizePoll{.optionId = opts[0].id});
        CHECK(state.finalized);
        CHECK(state.finalizedOptionId == opts[0].id);
    }
}

TEST_CASE("Finalizing an already-finalized poll throws Conflict", "[polls][model]") {
    DbFixture fixture;
    PollModel model;
    auto created = model.execute(CreatePoll{.title = "T", .options = {{"1"}, {"2"}}});
    model.execute(OpenPoll{.pollId = created.pollId});
    auto opts = model.execute(GetPollState{}).options;
    morph::session::Context ctx;
    ctx.token = created.adminToken;
    morph::session::ScopedContext scoped{ctx};
    model.execute(FinalizePoll{.optionId = opts[0].id});
    CHECK_THROWS_AS(model.execute(FinalizePoll{.optionId = opts[1].id}), Conflict);
}
```

```cpp
// test_polls_authorizer.cpp
TEST_CASE("PollsAuthorizer::authorizeRegister admits every register, per finding 027's shared-registration scope",
          "[polls][auth]") {
    polls::auth::PollsAuthorizer authorizer;
    // Exercise the real IAuthorizer::authorizeRegister signature -- confirm
    // its exact parameters against morph::session::IAuthorizer's real
    // declaration (include/morph/session/session.hpp) before writing this
    // call, matching how rung 2's own authorizer tests verified their
    // signatures against the real interface rather than guessing.
}
```

- [ ] **Step 2-4: Run to verify fail/pass, commit**

```bash
git add examples/polls/include/polls/models/poll_model.hpp examples/polls/src/models/poll_model.cpp \
        examples/polls/include/polls/auth/ examples/polls/src/auth/ \
        examples/polls/tests/test_poll_model.cpp examples/polls/tests/test_polls_authorizer.cpp
git commit -m "polls: add FinalizePoll and PollsAuthorizer"
```

---

### Task 8: `PollModel` — `UndoLastVoteChange` (the rung's headline design record)

**Files:**
- Modify: `examples/polls/include/polls/models/poll_model.hpp`
- Modify: `examples/polls/src/models/poll_model.cpp`
- Test: `examples/polls/tests/test_poll_model.cpp` (append)

**Interfaces:**
- Consumes: `VoteHistoryRecord` (Task 4/6 — every `SubmitVotes`/`UpdateVotes`
  call writes one, storing the pre-change vote set as JSON).
- Produces: `execute(UndoLastVoteChange)`.

This is the test the README calls "the rung's headline design record":
*"Write the interleaving test first (A votes, B votes, A undoes → assert
whose vote died) — its outcome is the rung's headline design record."*
Write and run that test **before** implementing `execute()`'s body, and
record its outcome in this rung's README once it passes (a follow-up
one-line edit to `examples/polls/README.md`'s own "Definition of done"
checklist, confirming the compensating-action shape actually delivers
principal-scoped undo — not a plan step, but do it as part of closing this
task, matching how rung 2's design records were confirmed in the README
after the fact).

```cpp
UndoLastVoteChangeResult PollModel::execute(const UndoLastVoteChange& action) {
    if (!action.validate()) {
        throw ValidationError{"UndoLastVoteChange: participantName is required"};
    }
    auto poll = loadPollByPollId(mapper(), requirePollId());
    auto history = mapper().Query<db::VoteHistoryRecord>()
                       .Where(::Lightweight::FieldNameOf<&db::VoteHistoryRecord::poll>, "=", poll.id.Value())
                       .Where(::Lightweight::FieldNameOf<&db::VoteHistoryRecord::participantName>, "=", action.participantName)
                       .OrderByDescending(::Lightweight::FieldNameOf<&db::VoteHistoryRecord::id>)
                       .First();
    if (!history.has_value()) {
        throw Conflict{"nothing to undo for this participant"};
    }
    // Decode history->previousVotesJson (the pre-change vote set) and
    // restore it via the same delete-then-recreate logic applyVotes()
    // (Task 6) already implements -- reuse that helper directly rather
    // than duplicating the write pattern. Then delete the consumed
    // VoteHistoryRecord row (undo is one-shot, not a redo stack) and
    // write a PollEventRecord ("kind": "vote", summary naming the undo)
    // inside the same transaction.
    ...
    return UndoLastVoteChangeResult{.restored = true};
}
```

- [ ] **Step 1: Write the interleaving test FIRST, before the implementation above**

```cpp
TEST_CASE("Principal-scoped undo: A votes, B votes, A undoes -> only A's vote dies (the rung's headline design record)",
          "[polls][model]") {
    DbFixture fixture;
    PollModel model;
    auto created = model.execute(CreatePoll{.title = "T", .options = {{"1"}, {"2"}}});
    model.execute(OpenPoll{.pollId = created.pollId});
    auto opts = model.execute(GetPollState{}).options;

    model.execute(SubmitVotes{.participantName = "alice", .votes = {{.optionId = opts[0].id, .choice = VoteChoice::Yes}}});
    model.execute(SubmitVotes{.participantName = "bob", .votes = {{.optionId = opts[0].id, .choice = VoteChoice::Yes}}});
    // Both voted yes on option 0: count should be 2.
    auto before = model.execute(GetPollState{});
    REQUIRE(before.options[0].yesCount == Count::fromDouble(2.0));

    auto undoResult = model.execute(UndoLastVoteChange{.participantName = "alice"});
    CHECK(undoResult.restored);

    auto after = model.execute(GetPollState{});
    // Alice's vote is gone; Bob's survives. This is the assertion that
    // SessionLog::undoLast() could never make true: it pops the newest
    // entry regardless of principal, which would have killed Bob's vote
    // (the more recent of the two), not Alice's own.
    CHECK(after.options[0].yesCount == Count::fromDouble(1.0));
    const bool bobStillVotes =
        std::ranges::any_of(after.votes, [](const auto& v) { return v.participantName == "bob"; });
    const bool aliceStillVotes =
        std::ranges::any_of(after.votes, [](const auto& v) { return v.participantName == "alice"; });
    CHECK(bobStillVotes);
    CHECK_FALSE(aliceStillVotes);
}

TEST_CASE("UndoLastVoteChange with nothing to undo throws Conflict", "[polls][model]") {
    DbFixture fixture;
    PollModel model;
    auto created = model.execute(CreatePoll{.title = "T", .options = {{"1"}, {"2"}}});
    model.execute(OpenPoll{.pollId = created.pollId});
    CHECK_THROWS_AS(model.execute(UndoLastVoteChange{.participantName = "nobody-voted"}), Conflict);
}

TEST_CASE("Undo is one-shot: undoing twice in a row throws Conflict the second time", "[polls][model]") {
    DbFixture fixture;
    PollModel model;
    auto created = model.execute(CreatePoll{.title = "T", .options = {{"1"}, {"2"}}});
    model.execute(OpenPoll{.pollId = created.pollId});
    auto opts = model.execute(GetPollState{}).options;
    model.execute(SubmitVotes{.participantName = "alice", .votes = {{.optionId = opts[0].id, .choice = VoteChoice::Yes}}});
    model.execute(UndoLastVoteChange{.participantName = "alice"});
    CHECK_THROWS_AS(model.execute(UndoLastVoteChange{.participantName = "alice"}), Conflict);
}
```

- [ ] **Step 2: Run to verify these fail (no implementation yet)**

- [ ] **Step 3: Implement `execute(UndoLastVoteChange)` per the sketch above**

- [ ] **Step 4: Run to verify all pass**

- [ ] **Step 5: Record the design record in the README**

Add one sentence to `examples/polls/README.md`'s "Definition of done"
section confirming the interleaving test's outcome (A's undo restores only
A's prior state; B's vote survives untouched) — this is what the DoD's own
bullet asks for ("verified by the two-principal interleaving test").

- [ ] **Step 6: Commit**

```bash
git add examples/polls/include/polls/models/poll_model.hpp examples/polls/src/models/poll_model.cpp \
        examples/polls/tests/test_poll_model.cpp examples/polls/README.md
git commit -m "polls: add UndoLastVoteChange -- principal-scoped compensating action"
```

---

### Task 9: `PollModel` — `GetEventsSince`

**Files:**
- Modify: `examples/polls/include/polls/models/poll_model.hpp`
- Modify: `examples/polls/src/models/poll_model.cpp`
- Test: `examples/polls/tests/test_poll_model.cpp` (append)

**Interfaces:**
- Consumes: `PollEventRecord` (already written by Tasks 6-8's own mutations).
- Produces: `execute(GetEventsSince)`.

```cpp
GetEventsSinceResult PollModel::execute(const GetEventsSince& action) {
    if (!action.validate()) {
        throw ValidationError{"GetEventsSince: malformed request"};
    }
    auto poll = loadPollByPollId(mapper(), requirePollId());
    auto rows = mapper().Query<db::PollEventRecord>()
                    .Where(::Lightweight::FieldNameOf<&db::PollEventRecord::poll>, "=", poll.id.Value())
                    .Where(::Lightweight::FieldNameOf<&db::PollEventRecord::id>, ">", static_cast<std::uint64_t>(*action.lastEventId))
                    .OrderBy(::Lightweight::FieldNameOf<&db::PollEventRecord::id>)
                    .All();
    GetEventsSinceResult result;
    for (const auto& row : rows) {
        result.events.push_back({.id = PollEventId{.value = static_cast<std::int64_t>(row.id.Value())},
                                  .kind = row.kind.value(), .summary = row.summary.value()});
    }
    return result;
}
```

- [ ] **Step 1: Write the failing tests**

```cpp
TEST_CASE("GetEventsSince{} (from the beginning) returns every event in order", "[polls][model]") {
    DbFixture fixture;
    PollModel model;
    auto created = model.execute(CreatePoll{.title = "T", .options = {{"1"}, {"2"}}});
    model.execute(OpenPoll{.pollId = created.pollId});
    auto opts = model.execute(GetPollState{}).options;
    model.execute(SubmitVotes{.participantName = "alice", .votes = {{.optionId = opts[0].id, .choice = VoteChoice::Yes}}});
    model.execute(AddComment{.participantName = "alice", .body = "hi"});

    auto events = model.execute(GetEventsSince{}).events;
    REQUIRE(events.size() == 2);
    CHECK(events[0].kind == "vote");
    CHECK(events[1].kind == "comment");
    CHECK(events[0].id.value < events[1].id.value);  // strictly increasing
}

TEST_CASE("GetEventsSince{lastEventId} returns only strictly-newer events", "[polls][model]") {
    DbFixture fixture;
    PollModel model;
    auto created = model.execute(CreatePoll{.title = "T", .options = {{"1"}, {"2"}}});
    model.execute(OpenPoll{.pollId = created.pollId});
    auto opts = model.execute(GetPollState{}).options;
    model.execute(SubmitVotes{.participantName = "alice", .votes = {{.optionId = opts[0].id, .choice = VoteChoice::Yes}}});
    auto firstEvents = model.execute(GetEventsSince{}).events;
    REQUIRE(firstEvents.size() == 1);

    model.execute(AddComment{.participantName = "alice", .body = "hi"});
    auto newEvents = model.execute(GetEventsSince{.lastEventId = firstEvents.front().id}).events;
    REQUIRE(newEvents.size() == 1);
    CHECK(newEvents.front().kind == "comment");
}

TEST_CASE("The event log survives full detach/reattach (instance rebirth), and a stale cursor "
          "gets everything after it -- no epoch token needed",
          "[polls][model]") {
    // This is the DoD's own required test: "Event log survives full
    // detach/reattach (instance rebirth) and a stale cursor triggers a
    // clean full resync, verified by test." Given this rung's resolved
    // design decision (durable persistence alone closes the gap, no
    // epoch token), "clean full resync" here means: the stale cursor
    // simply gets every real event since it, correctly, because the
    // event log's sequence id survived the instance's death regardless
    // of which in-memory PollModel wrote which row. Use BackendRig to
    // attach N handlers to the same key, detach all (verify destruction
    // via instances()), attach again with the pre-death cursor, and
    // assert every event since that cursor comes back -- not merely that
    // it doesn't crash.
    DbFixture fixture;
    BackendRig rig{Mode::Local, 1};  // or Mode::Socket -- either demonstrates real backend-owned instance lifetime
    // ... construct a handler, CreatePoll, OpenPoll, SubmitVotes once,
    // capture lastEventId, drop every handler referencing this poll,
    // confirm rig's instances() (or equivalent) shows the instance gone,
    // construct a fresh handler, OpenPoll again, GetEventsSince with the
    // pre-death cursor, assert the events since then are still there.
}
```

- [ ] **Step 2-4: Run to verify fail/pass, commit**

```bash
git add examples/polls/include/polls/models/poll_model.hpp examples/polls/src/models/poll_model.cpp \
        examples/polls/tests/test_poll_model.cpp
git commit -m "polls: add GetEventsSince -- the Zulip-pattern event log read path"
```

---

### Task 10: `App` — server bootstrap

**Files:**
- Create: `examples/polls/include/polls/app/app.hpp`
- Create: `examples/polls/src/app/app.cpp`
- Test: `examples/polls/tests/test_app.cpp`

**Interfaces:**
- Produces: `app::App` (owns `RemoteServer` + `PollsAuthorizer` +
  `FileActionLog`), mirroring `bookmarks::app::App`'s shape
  (`examples/bookmarks/include/bookmarks/app/app.hpp`) minus the
  background-worker/`TokenIssuer` pieces this rung does not need (no
  signed tokens, no background metadata-fetch job — polls has no
  equivalent asynchronous job).

This task is the most mechanical of the model-layer tasks — read
`bookmarks::app::App`'s constructor and member shape and reuse the parts
that apply (action-log path, `RemoteServer` construction with
`PollsAuthorizer`, `maxLiveModels` cap sized to this rung's own model
count — polls registers exactly one model type, `PollModel`, so
`maxLiveModels` should be set generously relative to expected concurrent
polls, e.g. 256, matching rung 2's own reasoning for its own cap), and
drop everything about `TokenIssuer`/background fetch workers that has no
polls equivalent.

- [ ] **Step 1: Write the failing test**

```cpp
TEST_CASE("App boots, registers PollModel, and a real client can CreatePoll/OpenPoll over it", "[polls][app]") {
    DbFixture fixture;
    app::App app{fixture.actionLogPath()};
    // Real client dispatch through app.server(), mirroring
    // bookmarks::app::App's own equivalent test -- confirm the exact
    // helper/rig shape that test uses and mirror it here.
}
```

- [ ] **Step 2-4: Run to verify fail/pass, commit**

```bash
git add examples/polls/include/polls/app/ examples/polls/src/app/ examples/polls/tests/test_app.cpp
git commit -m "polls: add App -- server bootstrap"
```

---

### Task 11: `CMakeLists.txt`

**Files:**
- Create: `examples/polls/CMakeLists.txt`

Mirror `examples/bookmarks/CMakeLists.txt` exactly: `morph_add_rung(NAME polls)`
plus an explicit `target_sources(ladder_polls_lib PRIVATE .../src/auth/polls_authorizer.cpp .../src/db/schema.cpp)`
guarded by `if(TARGET ladder_polls_lib)` (`morph_add_rung()` only globs
`src/models`, `src/db`, `src/app` — `src/auth` needs the same explicit
`target_sources` treatment rung 2's `src/import`/`src/dto` needed, per
`cmake/morph_add_rung.cmake:91-92`'s confirmed glob scope). Add
`examples/polls` to `examples/CMakeLists.txt`'s subdirectory list (find
where `bookmarks`/`pastebin` are added and follow the identical pattern).

- [ ] **Step 1: Write `CMakeLists.txt`**, add the subdirectory line.

- [ ] **Step 2: Build and confirm every test target from Tasks 1-10 now
  builds and runs via the real CMake target** (`cmake --build build/clang-coverage
  --target ladder_polls_tests`), replacing every manual-clang++ compile
  step those tasks used. Fix any warnings under strict compilation the
  same way rung 2's Task 13 did (designated-initializer completeness,
  etc. — expect similar findings; fix them here rather than carrying them
  forward, matching rung 2's own precedent of not repeating Task 13's
  cleanup debt into later tasks).

- [ ] **Step 3: Commit**

```bash
git add examples/polls/CMakeLists.txt examples/CMakeLists.txt
git commit -m "polls: add CMakeLists.txt, completing the buildable rung skeleton"
```

---

### Task 12: Model tests — backend-mode matrix, shared-instance lifetime, and poisoned-instance attach

**Files:**
- Create: `examples/polls/tests/test_shared_instance_lifecycle.cpp`

**Interfaces:** Consumes `BackendRig` (all three modes), `DbFixture`.

Three genuinely new pieces of coverage this rung's README names as
"Expected strain points" that no task above already covers:

1. **Backend-mode matrix**: `CreatePoll` (native/`Local`-only per Global
   Constraints) → `OpenPoll` → `SubmitVotes` round trip across
   `Mode::Local`, `Mode::LocalSingleThread`, `Mode::Socket`, mirroring
   rung 2's Task 14 exactly (`examples/bookmarks/tests/test_bookmark_model.cpp`'s
   own `GENERATE(Mode::Local, Mode::LocalSingleThread, Mode::Socket)`
   pattern) — but for `PollModel`, every case after `CreatePoll` (which
   stays a direct, non-keyed call, matching how Task 5's own tests
   already do it) uses `handler.execute(OpenPoll{pollId})` to attach,
   proving the *keyed* attach path works identically across all three
   modes, not just the plain-registration path rung 2 proved.
2. **Shared-instance lifetime**: N `BridgeHandler<PollModel, AllowShared>`
   instances attach to the same `pollId`; confirm they observe each
   other's writes (one submits a vote, all N see it on their next
   `GetPollState`); detach all N; confirm the instance is gone via
   `handler.instances()` (construct one more handler first, call
   `instances()`, then detach every prior handler, then call `instances()`
   again and confirm the key is absent) — this is the DoD's own
   "`handler.instances()` for an organizer dashboard" requirement,
   proven, not just declared.
3. **Poisoned-instance attach**: opening a stale/mistyped `pollId`
   (`OpenPoll{.pollId = "not-a-real-poll"}`) throws `NotFound` through the
   returned `Completion`'s `.onError(...)` (not a crash, not a silently
   half-hydrated instance) — and per `docs/spec/core/shared_instances.md`'s
   documented failure mode, a *second* attach attempt to the same bad key
   gets a **fresh** instance (the poisoned one was evicted on this second
   attach, per spec), which also fails identically — write both attempts
   explicitly, asserting both fail the same way, to prove eviction-then-
   retry doesn't somehow succeed on stale poisoned state.

```cpp
TEST_CASE("PollModel over the full backend-mode matrix: create -> keyed-attach -> submit-vote round trip",
          "[polls][model]") {
    const auto mode = GENERATE(Mode::Local, Mode::LocalSingleThread, Mode::Socket);
    CAPTURE(mode);
    DbFixture fixture;
    BackendRig rig{mode, 1, std::make_shared<polls::auth::PollsAuthorizer>()};
    // CreatePoll direct (native-only, no keying involved -- Task 5's own
    // shape), then attach the rig's handler via execute(OpenPoll{pollId}),
    // then SubmitVotes, then GetPollState, asserting the vote landed.
}

TEST_CASE("N shared handlers on one pollId observe each other's writes, and instances() reflects "
          "the instance's real lifetime",
          "[polls][model][shared-instances]") {
    DbFixture fixture;
    BackendRig rig{Mode::Socket, 4, std::make_shared<polls::auth::PollsAuthorizer>()};
    // Construct 4 handlers attached to the same pollId (via OpenPoll); one
    // submits a vote; assert the other 3 see it via GetPollState; confirm
    // handler.instances() lists the key while at least one handler holds
    // it; destroy all 4; construct a 5th purely to call instances() and
    // confirm the key is now absent.
}

TEST_CASE("Opening a stale pollId is NotFound through .onError(), not a crash, and a second attempt "
          "to the same bad key gets a fresh (still-failing) instance, not stale poisoned state",
          "[polls][model][shared-instances]") {
    DbFixture fixture;
    BackendRig rig{Mode::Socket, 1, std::make_shared<polls::auth::PollsAuthorizer>()};
    auto handler = rig.client<PollModel>(0);
    bool firstFailed = false;
    handler.execute(OpenPoll{.pollId = "not-a-real-poll"}).onError([&firstFailed](auto) { firstFailed = true; });
    REQUIRE(pumpUntil([&firstFailed] { return firstFailed; }));

    bool secondFailed = false;
    handler.execute(OpenPoll{.pollId = "not-a-real-poll"}).onError([&secondFailed](auto) { secondFailed = true; });
    REQUIRE(pumpUntil([&secondFailed] { return secondFailed; }));
}
```

- [ ] **Step 2-4: Run to verify fail/pass, commit**

```bash
git add examples/polls/tests/test_shared_instance_lifecycle.cpp
git commit -m "polls: add backend-mode matrix, shared-instance lifetime, and poisoned-attach tests"
```

---

### Task 13: Cross-user isolation, `messagesPerSecond`-configured harness, and the cross-model rename-race analogue

**Files:**
- Modify: `examples/polls/tests/test_shared_instance_lifecycle.cpp` (append)

**Interfaces:** Consumes `QtWebSocketServerConfig::messagesPerSecond`
(configured ON, per the README's "Expected strain points" and this
plan's design-decision resolution 5 — a harness config, not new framework
work).

1. **Cross-user isolation over Socket**: two participants attach to the
   same poll (this is expected — the whole point of sharing), but a
   participant token from poll A must not let its holder finalize poll B
   or read poll B's `adminToken`-gated state. Since `PollModel` is keyed
   per-poll (each poll is its own instance), this reduces to: a
   `FinalizePoll` call using poll A's admin token, dispatched against a
   handler attached to poll B, must fail — write this explicitly rather
   than assuming it's implied by the per-instance keying, since a bug
   in `requireAdminToken`'s poll-row lookup (e.g., checking against the
   wrong cached `_pollId`) could silently pass.
2. **`messagesPerSecond` configured ON**: run at least one real
   `SubmitVotes` dispatch through a `QtWebSocketServerConfig` with
   `messagesPerSecond` set low enough to guarantee a drop under a small
   burst, and confirm `Bridge::setExecuteDeadline` (this rung's own
   framework-prerequisite work, Task 1 of the framework-prereqs plan)
   actually recovers the caller via `ClientTimeoutError` rather than
   hanging forever — this is the DoD's "run this rung's harness with
   `messagesPerSecond` configured ON" requirement, and the first real
   proof (beyond the framework-prereqs plan's own unit tests) that the
   deadline mechanism and the rate limiter combine correctly end to end
   in a real app.
3. **The cross-model rename-race analogue**: this rung's README does not
   name an exact analogue to rung 2's `TagModel`-renames-while-
   `BookmarkModel`-writes race (there is only one model type here), so
   skip this specific test class — note in this task's commit message
   that it was considered and is not applicable, rather than silently
   omitting it (matching this session's established discipline of never
   silently dropping a checklist item without a stated reason).

```cpp
TEST_CASE("A poll's admin token does not finalize a different poll", "[polls][model][shared-instances]") {
    DbFixture fixture;
    BackendRig rig{Mode::Socket, 2, std::make_shared<polls::auth::PollsAuthorizer>()};
    auto handlerA = rig.client<PollModel>(0);
    auto handlerB = rig.client<PollModel>(1);
    auto createdA = awaitQt(handlerA.execute(CreatePoll{.title = "A", .options = {{"1"}, {"2"}}}));
    auto createdB = awaitQt(handlerB.execute(CreatePoll{.title = "B", .options = {{"1"}, {"2"}}}));
    awaitQt(handlerB.execute(OpenPoll{.pollId = createdB.pollId}));
    auto optsB = awaitQt(handlerB.execute(GetPollState{})).options;

    morph::session::Context ctx;
    ctx.token = createdA.adminToken;  // poll A's admin token, used against poll B
    rig.bridge(1).setDefaultSession(ctx);
    bool failed = false;
    handlerB.execute(FinalizePoll{.optionId = optsB[0].id}).onError([&failed](auto) { failed = true; });
    REQUIRE(pumpUntil([&failed] { return failed; }));
}

TEST_CASE("Bridge::setExecuteDeadline recovers a call the real rate limiter silently drops",
          "[polls][model][shared-instances]") {
    DbFixture fixture;
    // Configure a real QtWebSocketServerConfig with messagesPerSecond set
    // low (e.g. 1) and a real QtWebSocketBackend-based BridgeRig whose
    // Bridge has bridge.setExecuteDeadline(std::chrono::milliseconds{500})
    // set. Burst several SubmitVotes calls in quick succession -- at least
    // one must be dropped by the limiter (confirm via the server's own
    // logged drop, or by observing more calls than replies). Assert the
    // dropped call's Completion resolves via ClientTimeoutError within the
    // configured deadline, not hung.
}
```

- [ ] **Step 2-4: Run to verify fail/pass, commit**

```bash
git add examples/polls/tests/test_shared_instance_lifecycle.cpp
git commit -m "polls: add cross-poll admin-token isolation and messagesPerSecond+deadline integration test"
```

---

### Task 14: Presenters

**Files:**
- Create: `examples/polls/gui_lib/poll_presenter.hpp`
- Create: `examples/polls/gui_lib/poll_presenter.cpp`
- Test: `examples/polls/tests/test_poll_presenter.cpp`

**Interfaces:** Mirrors `bookmarks::gui::BookmarkPresenter`'s exact shape
(`examples/bookmarks/gui_lib/bookmark_presenter.hpp`) — one presenter
method per `PollModel` action, each `track<T>()`-wrapped with an `onErr`
callback for GUI error display, exactly rung 1/2's established pattern.
`PollPresenter` additionally needs an `openPoll(pollId)` convenience method
that calls `handler_.execute(OpenPoll{pollId})` (the payload-keyed attach)
and, on success, kicks off the polling helper's first `GetEventsSince`
call (Task 15 builds the actual polling helper; this task's presenter
exposes the primitive it needs — a `getEventsSince(lastEventId)` method —
without yet wiring the timer).

- [ ] **Step 1: Write the failing tests** — mirror
  `examples/bookmarks/tests/test_bookmark_presenter.cpp`'s exact structure:
  one test case per presenter method across all three backend modes, plus
  a "no session at all emits failed, not a crash" case, plus a
  "every validation-driven action routes its failure to failed(), not just
  the first one" case — read that file in full and produce the equivalent
  9-action-shaped (`createPoll`/`openPoll`/`getPollState`/`submitVotes`/
  `updateVotes`/`addComment`/`finalizePoll`/`undoLastVoteChange`/
  `getEventsSince`) coverage for `PollPresenter`.

- [ ] **Step 2-4: Run to verify fail/pass, commit**

```bash
git add examples/polls/gui_lib/poll_presenter.hpp examples/polls/gui_lib/poll_presenter.cpp \
        examples/polls/tests/test_poll_presenter.cpp
git commit -m "polls: add PollPresenter"
```

---

### Task 15: The event-polling helper — this rung's framework-level deliverable

**Files:**
- Create: `examples/common/gui/event_poller.hpp`
- Create: `examples/common/gui/event_poller.cpp`
- Test: `examples/common/tests/test_event_poller.cpp` (or
  `examples/polls/tests/`, whichever this codebase's convention places
  cross-rung-reusable `examples/common/` code's own tests in — check for
  precedent, e.g. `examples/common/testkit/`'s own test placement, before
  choosing)

**Interfaces:** Produces `morph::ladder::gui::EventPoller<Model, Action, Result>`
(or a narrower, polls-specific-but-easily-generalized type if a fully
generic template proves awkward to write cleanly in one task — the DoD's
requirement is that it is "factored so kanban can lift it," which a
well-documented, narrowly-polls-shaped-but-clearly-reusable class also
satisfies if a template turns out over-engineered for a first use; use
your judgment, but document the choice either way).

This is explicitly named in the README as **"this rung's framework-level
deliverable"** and **"every later rung inherits this helper; get it right
here."** Design:

- Owns a `QTimer` (or the platform-appropriate periodic-callback
  primitive `examples/common/gui/` already uses elsewhere — check
  `AppContext`/`Presenter`'s own timer usage, if any, for the established
  pattern before introducing a new one) that calls `GetEventsSince` on a
  configurable interval.
- **Must use `Bridge::setExecuteDeadline`** (this rung's own framework
  prerequisite, already landed) — without it, a rate-limited server
  silently dropping a poll frame hangs the poller's in-flight call
  forever, exactly the failure mode the README's "Expected strain points"
  section names. Confirm the `Bridge` the poller's `BridgeHandler` is
  constructed against has a deadline configured (either the poller
  requires this as a precondition, documented loudly, or the poller itself
  calls `setExecuteDeadline` on construction with a sensible default —
  prefer the latter, since a caller forgetting to configure it is exactly
  the mistake this helper exists to make impossible).
- On each tick: dispatch `GetEventsSince{lastEventId}`; on success, apply
  each returned event via a caller-supplied callback and advance
  `lastEventId` to the last event's id; on `ClientTimeoutError`
  specifically, log and retry on the next tick (do not treat a timeout as
  a fatal error — a single slow round trip should not stop polling); on
  any other error (e.g. the poll was deleted, `NotFound`), stop the timer
  and surface the failure once via a caller-supplied `onFatalError`
  callback, matching how a stale client should "fall back to `GetPollState`"
  per the README's own Zulip-pattern description — this task does not
  need to implement the fallback-to-full-resync behavior itself (that is
  presenter/GUI-layer policy, informed by `onFatalError`), only to
  surface the signal cleanly.
- Measure and document the default poll interval (the README's own
  "Expected strain points" asks: "Poll-interval latency: two voters
  editing simultaneously see each other only on the next tick — measure
  and document acceptable intervals." A reasonable default, e.g. 2-3
  seconds, balancing responsiveness against server load — document the
  choice and its trade-off in this class's own doc comment, not just in
  a commit message).

- [ ] **Step 1: Write the failing tests**

```cpp
TEST_CASE("EventPoller applies every event returned since the last tick and advances its cursor", "[gui][event-poller]") {
    // Deterministic executor / fake clock, matching examples/common/testkit's
    // established dual-mode testing conventions -- drive the timer manually
    // rather than sleeping in the test.
}

TEST_CASE("EventPoller survives a ClientTimeoutError -- retries on the next tick, does not stop", "[gui][event-poller]") {
    // A test double whose GetEventsSince never replies once, forcing the
    // deadline to fire; assert the poller ticks again afterward rather
    // than giving up.
}

TEST_CASE("EventPoller stops and reports onFatalError exactly once on a non-timeout failure (e.g. NotFound)",
          "[gui][event-poller]") {
}
```

- [ ] **Step 2-4: Run to verify fail/pass, commit**

```bash
git add examples/common/gui/event_poller.hpp examples/common/gui/event_poller.cpp \
        examples/common/tests/test_event_poller.cpp
git commit -m "ladder: add the event-polling helper (this rung's framework-level deliverable)"
```

---

### Task 16: GUI shell — schema-driven forms + the polling helper wired to a real view

**Files:**
- Create: `examples/polls/gui_lib/poll_schemas.hpp`
- Create: `examples/polls/gui_lib/poll_forms_controller.{hpp,cpp}`
- Create: `examples/polls/gui_lib/poll_qml_bridges.{hpp,cpp}`
- Create: `examples/polls/gui/qml/{Main,CreatePollView,VoteView}.qml`
- Test: `examples/polls/tests/test_gui_qml_smoke.cpp`, `examples/polls/tests/test_poll_qml_bridges.cpp`

**Interfaces:** Mirrors `bookmarks::gui`'s exact shape (`bookmark_schemas.hpp`,
`bookmark_forms_controller.*`, `bookmark_qml_bridges.*`) — one schema
document routing `{actionType: schema}` to `PollModel`'s actions, one QML
bridge (`PollBridge`) wrapping `PollPresenter`, `Main.qml`'s `StackView`
switching between a create-poll form (native-only per Global Constraints —
either omit this view entirely from the WASM build target, or gate it
behind a compile-time/runtime check, following whatever precedent rung 2's
GUI established for a native-only capability, if any; if no such precedent
exists, the simplest correct choice is: the WASM `main_wasm.cpp` simply
never loads `CreatePollView.qml` into its `StackView`'s reachable states,
since nothing routes to it without a UI affordance) and a vote view
(`OpenPoll` + `SubmitVotes`/`UpdateVotes`/`AddComment` forms + the live
event-driven results display, wired to Task 15's `EventPoller`).

Given `DynamicForm` has no control for array-typed JSON fields (finding
031, discovered during rung 2), `CreatePoll::options` (an array of
`CreatePollOption`) cannot be a schema-driven form field — mirror rung 2's
own workaround for `BulkEdit` (excluded from the schema document, driven
by a small hand-written QML list-editor instead, not a `DynamicForm`
field). Document this in the same "known gaps" style rung 2's README
adopted, in this rung's own README, once this task lands.

- [ ] **Step 1-6**: mirror rung 2's Task 18's exact step shape (schema
  document → forms controller → QML bridges → QML views → offscreen smoke
  test → adapter-layer unit tests with `QMetaObject` surface assertions)
  — read `examples/bookmarks/gui_lib/bookmark_schemas.hpp` through
  `bookmark_qml_bridges.cpp` and `examples/bookmarks/tests/test_bookmark_qml_bridges.cpp`
  in full before starting, and produce the polls-shaped equivalent of
  every one of those files, including the adapter-layer test file from
  the start this time (rung 2 shipped it late, in a fix round, after
  review caught the gap — this plan builds it into the task from the
  beginning instead, avoiding that repeat).

- [ ] **Step 7: Update `examples/polls/README.md`** with the `CreatePoll`-array-field
  workaround note and any other known-gaps this task surfaces (matching
  rung 2's "Known gaps this rung ships with" section's style and
  location).

- [ ] **Step 8: Commit**

```bash
git add examples/polls/gui_lib/ examples/polls/gui/ examples/polls/tests/test_gui_qml_smoke.cpp \
        examples/polls/tests/test_poll_qml_bridges.cpp examples/polls/README.md
git commit -m "polls: add the schema-driven GUI shell wired to the event-polling helper"
```

---

### Task 17: Server binary

**Files:**
- Create: `examples/polls/src/server/main.cpp`

**Interfaces:** Env-var configured (`POLLS_DB`, `POLLS_PORT` — **no**
`POLLS_TOKEN_SECRET`, since this rung has no signed-token issuer; the
admin/participant tokens are per-poll, generated by `CreatePoll` itself,
not a process-wide secret). Mirror `bookmarks::src::server::main.cpp`'s
exact SIGTERM-poll shutdown shape, minus the metadata-worker drain (polls
has no background worker to drain).

- [ ] **Step 1-4**: mirror rung 2's Task 18 server-binary steps exactly
  (env-var parsing with `std::from_chars` for the port, hard failure on
  malformed input — matching the final-review-fix-wave lesson from rung 2
  rather than repeating `std::atoi`'s mistake fresh), manual smoke test
  (start the real binary, confirm it listens and shuts down cleanly on
  SIGTERM), commit.

```bash
git add examples/polls/src/server/main.cpp
git commit -m "polls: add the server binary"
```

---

### Task 18: WASM client — the payoff of this rung's entire framework-prerequisite detour

**Files:**
- Create: `examples/polls/gui_wasm/main_wasm.cpp`
- Modify: `.github/workflows/wasm-ladder.yml`

**Interfaces:** Mirrors `examples/bookmarks/gui_wasm/main_wasm.cpp` exactly
(always-`Remote` `AppContext`, no hand-rolled retry timer — `AppContext`/
`Main.qml`'s shared bootstrap-retry timer already covers finding 024
generically, confirmed by both rung 1 and rung 2's own WASM tasks) —
**with one load-bearing addition neither prior rung's WASM client needed**:
this is the file where `QtWebSocketBackendConfig::asyncRegistrationEnabled`
actually matters for a *keyed* attach, not just plain registration. Confirm
(read `examples/common/gui/app_context.cpp:37`, already cited during this
rung's framework-prerequisite review as setting `asyncRegistrationEnabled = true`
for every ladder GUI/WASM app) that this flag is already on by the time
`OpenPoll{pollId}` dispatches — if so, no new wiring is needed here beyond
what `AppContext` already provides; if the research citation turns out
stale by the time this task runs, set it explicitly and document why.

This task's QML never loads `CreatePollView` (Global Constraints:
`CreatePoll` is native-only) — only the vote/join view, reached via
whatever mechanism the app expects a participant to arrive at a poll link
(e.g. a URL query parameter naming the `pollId`, parsed in `main_wasm.cpp`
the same way `examples/common/wasm_spike`'s own URL-parameter handling, if
any, already establishes a precedent for — check before inventing a new
mechanism).

- [ ] **Step 1: Write `main_wasm.cpp`**, mirroring rung 2's WASM file's
  header-comment density and structure (mode rationale, no-bootstrap
  rationale, "note what is not here," verification status) — adapted to
  name this rung's own actually-different fact: unlike rung 1/2's WASM
  clients, this one exercises a genuinely new framework code path
  (`Bridge::attachHandlerAsync`'s async branch, previously unreached by
  any real WASM binary in this repo) for the first time, and should say so.

- [ ] **Step 2: Extend `.github/workflows/wasm-ladder.yml`** with
  `ladder_polls_gui_wasm` as a named target, following the exact pattern
  rung 2's own Task 19 already established (a named target build plus the
  trailing plain `cmake --build build-wasm-ladder` pass that already
  covers every further rung automatically — confirm this rung's addition
  is genuinely needed as a *named* target for the same "fails loud if a
  target silently stops being generated" reason, even though the trailing
  plain build would technically also catch it, matching rung 2's own
  stated rationale for keeping named targets alongside the catch-all).

- [ ] **Step 3: Verify what can be verified locally** (no Emscripten
  toolchain in this environment, per rung 1/2's own precedent) — confirm
  `ladder_polls_gui_wasm` would plausibly be generated by reading
  `cmake/morph_add_rung.cmake`'s own logic, state plainly what remains
  CI-only.

- [ ] **Step 4: Commit**

```bash
git add examples/polls/gui_wasm/main_wasm.cpp .github/workflows/wasm-ladder.yml
git commit -m "polls: add the WASM client -- the first real exercise of async keyed attach"
```

---

## Self-Review

**Spec coverage against `examples/polls/README.md`:**

| README section | Covered by |
|---|---|
| `CreatePoll`, `OpenPoll`/`GetPollState` | Task 5 |
| `SubmitVotes`/`UpdateVotes`/`AddComment` | Task 6 |
| `FinalizePoll` | Task 7 |
| `UndoLastVoteChange` (principal-scoped compensating action) | Task 8 |
| `GetEventsSince` (Zulip-pattern event log) | Task 9 |
| Shared instances end-to-end, `instances()` | Task 12 |
| Anonymous principals (admin/participant tokens) | Task 7 |
| Event polling — the reusable pattern | Task 15 |
| WASM + shared handlers [framework prerequisite] | Closed by the separate `2026-08-07-ladder-rung3-framework-prereqs.md` plan, exercised for real by Task 18 |
| Client-side execute deadline [framework prerequisite] | Same, exercised by Task 13's `messagesPerSecond` integration test and Task 15's poller |
| Poisoned-instance attach | Task 12 |
| Duplicate `SubmitVotes` on retry | Task 6 |
| Dead-letter on `FinalizePoll` racing an in-flight vote | Task 6 |
| Timezone display | Explicitly GUI-layer, out of scope for the model/test tasks — flagged for Task 16's own QML if a reviewer judges it load-bearing; not separately tasked here since the README itself calls it "GUI logic," matching how rung 2 treated analogous client-only concerns |
| Shared-instance churn soak (framework-grade, `tests/soak/`) | **Gap, stated plainly**: not tasked in this plan. This is explicitly framework-grade coverage (threads racing register-or-attach/deregister/closeConnection/execute under TSan), arguably belonging with the framework-prerequisites plan rather than an app plan — flagged here as a follow-up the framework-prerequisites plan's own workspace (already closed) did not include either. A future task, not silently dropped. |
| DoD: live demo, one organizer + three participants | Manual verification step, not a task — perform during final review, mirroring rung 2's own manual server/GUI sanity checks |
| DoD: principal-scoped undo verified by the interleaving test | Task 8 |
| DoD: event log survives detach/reattach, stale cursor resyncs | Task 9, Task 12 |
| DoD: polling helper factored for kanban reuse | Task 15 |

**Placeholder scan**: one intentional exception, flagged explicitly rather
than smoothed over — Task 5's `execute(GetPollState)` sketch contains a
genuine open implementation-detail question (how the model recovers its
own `pollId` once attached) with a concrete recommended resolution, not a
`TBD`. This is the one place this plan asks an implementer to make a
documented judgment call rather than handing over verbatim code, and it is
called out as such, matching this plan's own "No Placeholders" standard's
spirit (a real recommendation with reasoning, not an empty box).

**Type/signature consistency check**: `PollId` (plain `std::string`,
Global Constraints) is used identically in `OpenPoll::pollId`,
`CreatePollResult::pollId`, and `GetPollStateResult::pollId` throughout
Tasks 2-9. `OptionId`/`PollEventId` (Task 1) are used identically at every
DTO/entity boundary (`static_cast<std::int64_t>`/`static_cast<std::uint64_t>`
conversions at each crossing, matching rung 1/2's own established
boundary-casting convention). `VoteChoice`'s three-way enum is used
identically in `OneVote`, `ParticipantVoteView`, and `VoteRecord::choice`'s
`std::uint8_t` encoding (Tasks 2-4, 6).

**Judgment calls this plan made that the original README did not fully
specify:**

1. **`PollModel` is registered plain, not gated by a per-instance
   `authorizeInstance` check** — mirrors rung 2's own corrected design
   (shared instances are ownerless per spec; the model re-checks the
   caller's admin token itself for `FinalizePoll`). Not a new pattern,
   reused from rung 2's own hard-won correction.
2. **No `TokenIssuer`/signed tokens anywhere in this rung** — a
   deliberate, stated departure from rung 1/2's pattern, forced by there
   being no framework authorizer for bare shared secrets (this plan's
   Global Constraints).
3. **`GetPollState`'s pollId-recovery mechanism** (Task 5) is the one
   place this plan hands the implementer a judgment call instead of
   verbatim code, with a concrete recommendation.
4. **The event-polling helper's generality** (Task 15) — template vs.
   narrower-but-documented class — left to the implementer's judgment,
   with the DoD's actual requirement (kanban can lift it) stated as the
   bar to clear either way.
5. **Shared-instance churn soak testing is out of scope for this plan** —
   named as a real, disclosed gap rather than silently dropped (see the
   Self-Review table above).

## Execution order

This plan assumes `docs/superpowers/plans/2026-08-07-ladder-rung3-framework-prereqs.md`
is fully complete and merged (confirmed: both of its tasks are done,
reviewed, fixed, and closed as of this plan's writing) — every task above
that touches `AllowShared`/`Bridge::setExecuteDeadline` depends on that
work already existing.

## Execution Handoff

**Plan complete and saved to `docs/superpowers/plans/2026-08-08-ladder-rung3-polls.md`.
Two execution options:**

**1. Subagent-Driven (recommended)** — I dispatch a fresh subagent per task,
review between tasks, fast iteration.

**2. Inline Execution** — Execute tasks in this session using
`executing-plans`, batch execution with checkpoints.

**If Subagent-Driven chosen:**
- **REQUIRED SUB-SKILL:** Use `superpowers:subagent-driven-development`
- Fresh subagent per task + two-stage review

**If Inline Execution chosen:**
- **REQUIRED SUB-SKILL:** Use `superpowers:executing-plans`
- Batch execution with checkpoints for review

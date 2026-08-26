// SPDX-License-Identifier: Apache-2.0
#include "polls/models/poll_model.hpp"

// The entity is an implementation detail of this TU: `poll_model.hpp` exposes
// only DTOs, so nothing outside this file ever sees `db::PollRecord` -- see
// `pastebin::PasteModel`'s identical `paste_model.cpp` precedent.
#include "polls/db/poll_entity.hpp"

// examples/common is on the include path as a root (see
// examples/common/CMakeLists.txt's target_include_directories), so the ladder
// clock is "clock.hpp" -- the same spelling testkit/test_clock.cpp uses.
#include <Lightweight/DataMapper/DataMapper.hpp>
#include <Lightweight/DataMapper/Pool.hpp>
#include <Lightweight/SqlTransaction.hpp>
#include <array>
#include <cstddef>
#include <cstdint>
#include <glaze/glaze.hpp>
#include <morph/session/session.hpp>
#include <optional>
#include <random>
#include <string>
#include <utility>
#include <vector>

#include "clock.hpp"

namespace polls {

// The one place each free-form text field's DTO-level bound and its real
// storage-column capacity are checked against each other -- see
// `pastebin::kMaxSyntaxBytes`'s identical `static_assert` (`paste_model.cpp`)
// for the two harms this guards against (a widened column silently
// outrunning the DTO's own reject-if-too-long check, or the reverse:
// `CreatePoll`/`SubmitVotes`/`AddComment`'s `validate()` rejecting input that
// would actually have fit). `PollEventRecord::kind`/`summary` have no DTO-level
// constant to pin against (see poll_entity.hpp's doc comments on both) and so
// are not asserted here.
static_assert(decltype(db::PollRecord::title)::ValueType{}.capacity() == kMaxTitleBytes,
              "polls::kMaxTitleBytes must equal PollRecord::title's SqlAnsiString capacity -- otherwise "
              "CreatePoll either rejects a title that would have fit, or accepts one that gets silently "
              "truncated on the way into the row.");
static_assert(decltype(db::OptionRecord::label)::ValueType{}.capacity() == kMaxOptionLabelBytes,
              "polls::kMaxOptionLabelBytes must equal OptionRecord::label's SqlAnsiString capacity -- otherwise "
              "CreatePoll either rejects an option label that would have fit, or accepts one that gets silently "
              "truncated on the way into the row.");
static_assert(decltype(db::VoteRecord::participantName)::ValueType{}.capacity() == kMaxParticipantNameBytes,
              "polls::kMaxParticipantNameBytes must equal VoteRecord::participantName's SqlAnsiString capacity -- "
              "otherwise SubmitVotes/UpdateVotes either reject a participantName that would have fit, or accept "
              "one that gets silently truncated on the way into the row.");
static_assert(decltype(db::CommentRecord::participantName)::ValueType{}.capacity() == kMaxParticipantNameBytes,
              "polls::kMaxParticipantNameBytes must equal CommentRecord::participantName's SqlAnsiString capacity "
              "-- otherwise AddComment either rejects a participantName that would have fit, or accepts one that "
              "gets silently truncated on the way into the row.");
static_assert(decltype(db::VoteHistoryRecord::participantName)::ValueType{}.capacity() == kMaxParticipantNameBytes,
              "polls::kMaxParticipantNameBytes must equal VoteHistoryRecord::participantName's SqlAnsiString "
              "capacity -- otherwise applyVotes() either rejects a participantName that would have fit, or "
              "accepts one that gets silently truncated on the way into the row.");
static_assert(decltype(db::CommentRecord::body)::ValueType{}.capacity() == kMaxCommentBytes,
              "polls::kMaxCommentBytes must equal CommentRecord::body's SqlAnsiString capacity -- otherwise "
              "AddComment either rejects a body that would have fit, or accepts one that gets silently truncated "
              "on the way into the row.");

namespace {

// ---------------------------------------------------------------------------
// SqlAnsiString<kTokenBytes> <-> std::string conversion, mirroring
// pastebin::textOf() (examples/pastebin/src/models/paste_model.cpp) exactly:
// `pollId`/`adminToken`/`participantToken` are `Light::SqlAnsiString<kTokenBytes>`-
// typed columns (fixed after Task 4's own review found no sibling entity
// justification for plain std::string on an id/token-shaped field), so every
// read of one of these three fields goes through this helper and every write
// goes through the equivalent `Light::SqlAnsiString<kTokenBytes>{...}`
// construction at the call site.
// ---------------------------------------------------------------------------
[[nodiscard]] std::string textOf(const Light::SqlAnsiString<kTokenBytes>& stored) { return std::string{stored.str()}; }

// ---------------------------------------------------------------------------
// Free-form Unicode text field conversions (title/label/participantName/body):
// every one of these entity columns is `Light::SqlAnsiString<kMaxFooBytes>`,
// bounded to the same constant `CreatePoll`/`SubmitVotes`/`AddComment`
// (`polls/dto/poll_dto.hpp`, `polls/dto/vote_dto.hpp`) already validate
// against at the DTO boundary -- the static_asserts right below pin entity
// capacity and DTO bound together so a future change to one without the
// other fails the build (see pastebin::PasteModel's `kMaxSyntaxBytes`
// static_assert, `paste_model.cpp`, for the identical pattern). All four
// share this one conversion pair since all four are the same
// `SqlAnsiString<N>`-shaped case, just with different `N`.
// ---------------------------------------------------------------------------
template <std::size_t N>
[[nodiscard]] std::string textOf(const Light::SqlAnsiString<N>& stored) {
    return std::string{stored.str()};
}

// `previousVotesJson`/`summary` are `Light::SqlMaxDynamicAnsiString` (no
// fixed bound -- see poll_entity.hpp's doc comments on each), so they get
// their own conversion pair rather than the templated `textOf()` above.
[[nodiscard]] std::string textOf(const Light::SqlMaxDynamicAnsiString& stored) { return stored.ToString(); }

/// @brief The injectable-time convention rung 1/2 established
///        (`examples/bookmarks/src/models/bookmark_model.cpp`,
///        `examples/pastebin/src/models/paste_model.cpp`): a private,
///        per-TU helper reading `morph::ladder::now()`, never exported.
[[nodiscard]] std::int64_t nowMs() noexcept {
    return (*::morph::ladder::now().value).value.time_since_epoch().count();
}

/// @brief Number of raw random bytes base64url-encoded (without padding)
///        into a `kTokenBytes`-long token. See `kTokenBytes`'s own doc
///        comment (`polls/core/types.hpp`) for why 16 bytes -> 22 chars.
constexpr std::size_t kRandomTokenBytes = 16;
static_assert((kRandomTokenBytes * 8 + 5) / 6 == kTokenBytes,
              "polls::kTokenBytes must equal the base64url-without-padding length of "
              "kRandomTokenBytes random bytes -- keeps CreatePoll's generated pollId/"
              "adminToken/participantToken length matching the documented contract in "
              "core/types.hpp.");

/// @brief A cryptographically-unguessable `pollId`/admin-or-participant
///        token: `kRandomTokenBytes` bytes drawn directly from
///        `std::random_device` (never used merely to seed a deterministic
///        PRNG, and never `std::rand()`/a time-seeded generator) and
///        base64url-encoded without padding. Unlike pastebin's
///        `randomPasteId()` (a deliberately small, collidable, human-typo-
///        tolerant keyspace) or bank's card-number generator, these three
///        tokens ARE the entire security boundary for admin/participant
///        identity in this rung (see the rung README's resolved design
///        decision 1) -- there is no signed
///        `SigningAuthorizer` token backing them up, only a bare secret
///        compared directly against the poll row's own stored columns, so
///        the byte source itself must be a real entropy source, not a
///        seeded-once convenience PRNG.
[[nodiscard]] std::string randomToken() {
    static constexpr char kAlphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

    std::random_device rd;
    std::uniform_int_distribution<int> byteDist{0, 255};
    std::array<std::uint8_t, kRandomTokenBytes> bytes{};
    for (auto& b : bytes) {
        b = static_cast<std::uint8_t>(byteDist(rd));
    }

    std::string out;
    out.reserve(kTokenBytes);
    for (std::size_t i = 0; i < bytes.size(); i += 3) {
        std::uint32_t chunk = static_cast<std::uint32_t>(bytes[i]) << 16;
        int chunkBytes = 1;
        if (i + 1 < bytes.size()) {
            chunk |= static_cast<std::uint32_t>(bytes[i + 1]) << 8;
            chunkBytes = 2;
        }
        if (i + 2 < bytes.size()) {
            chunk |= static_cast<std::uint32_t>(bytes[i + 2]);
            chunkBytes = 3;
        }
        out.push_back(kAlphabet[(chunk >> 18) & 0x3FU]);
        out.push_back(kAlphabet[(chunk >> 12) & 0x3FU]);
        if (chunkBytes >= 2) {
            out.push_back(kAlphabet[(chunk >> 6) & 0x3FU]);
        }
        if (chunkBytes >= 3) {
            out.push_back(kAlphabet[chunk & 0x3FU]);
        }
    }
    return out;
}

/// @brief Loads the poll named by @p pollId, or throws `NotFound`.
[[nodiscard]] db::PollRecord loadPollByPollId(::Lightweight::DataMapper& mapper, const std::string& pollId) {
    auto rows =
        mapper.Query<db::PollRecord>().Where(::Lightweight::FieldNameOf<&db::PollRecord::pollId>, "=", pollId).All();
    if (rows.empty()) {
        throw NotFound{"poll not found"};
    }
    return std::move(rows.front());
}

/// @brief Confirms @p optionId names a real option row belonging to @p poll
///        -- not merely a row that exists *somewhere* in `poll_options`.
///
/// `VoteRecord::option`/`PollRecord::finalizedOptionId` are FK-shaped but not
/// FK-enforced (SQLite; see `poll_entity.hpp`'s own note on this), so the
/// database alone never rejects an option id that belongs to a *different*
/// poll. Without this check, `FinalizePoll` could finalize with an option
/// nothing in this poll's own option list matches, and a vote naming another
/// poll's option would be written but never counted by `buildState()`'s
/// per-option tally loop (which only matches votes against options loaded
/// for `pollDbId`) -- silently discarding the participant's vote instead of
/// rejecting it.
/// @param mapper The active `DataMapper`.
/// @param poll The poll @p optionId is claimed to belong to.
/// @param optionId The option id to verify.
/// @throws NotFound if no option row with that id exists under this poll.
void requireOptionBelongsToPoll(::Lightweight::DataMapper& mapper, const db::PollRecord& poll, OptionId optionId) {
    if (optionId.value < 0) {
        throw NotFound{"option does not belong to this poll"};
    }
    auto rows =
        mapper.Query<db::OptionRecord>()
            .Where(::Lightweight::FieldNameOf<&db::OptionRecord::id>, "=", static_cast<std::uint64_t>(optionId.value))
            .Where(::Lightweight::FieldNameOf<&db::OptionRecord::poll>, "=", poll.id.Value())
            .All();
    if (rows.empty()) {
        throw NotFound{"option does not belong to this poll"};
    }
}

/// @brief Builds the full state view sent back to a client from a loaded
///        `PollRecord`: its options (with tallies), every vote, every
///        comment, and the id of the most recent event (a fresh client's
///        starting cursor for `GetEventsSince`).
[[nodiscard]] GetPollStateResult buildState(::Lightweight::DataMapper& mapper, const db::PollRecord& poll) {
    GetPollStateResult result;
    result.pollId = textOf(poll.pollId.Value());
    result.title = textOf(poll.title.Value());
    result.finalized = poll.finalized.Value() ? Finalized::Yes : Finalized::No;
    if (result.finalized == Finalized::Yes) {
        result.finalizedOptionId = OptionId::fromRowId(poll.finalizedOptionId.Value());
    }

    const std::uint64_t pollDbId = poll.id.Value();
    auto options = mapper.Query<db::OptionRecord>()
                       .Where(::Lightweight::FieldNameOf<&db::OptionRecord::poll>, "=", pollDbId)
                       .OrderBy(::Lightweight::FieldNameOf<&db::OptionRecord::sortOrder>)
                       .All();
    auto votes =
        mapper.Query<db::VoteRecord>().Where(::Lightweight::FieldNameOf<&db::VoteRecord::poll>, "=", pollDbId).All();
    for (const auto& opt : options) {
        PollOptionView view;
        view.id = OptionId::fromRowId(static_cast<std::int64_t>(opt.id.Value()));
        view.label = textOf(opt.label.Value());
        // Explicit zero, not default-constructed: a default `Count{}` is
        // Quantity's *empty* state (no payload), and Quantity arithmetic
        // propagates empty (empty + fromDouble(1.0) == empty, forever) --
        // see morph/util/quantity.hpp's own "Arithmetic. Empty propagates"
        // doc comment. Without this, no option's tally could ever leave
        // empty no matter how many votes matched below. Task 5 never caught
        // this because its own tests never exercised a poll with actual
        // votes; Task 6's SubmitVotes/UpdateVotes tests are what surfaced it.
        view.yesCount = Count::fromDouble(0.0);
        view.ifNeedBeCount = Count::fromDouble(0.0);
        view.noCount = Count::fromDouble(0.0);
        for (const auto& vote : votes) {
            if (vote.option.Value() != opt.id.Value()) {
                continue;
            }
            switch (static_cast<VoteChoice>(vote.choice.Value())) {
                case VoteChoice::Yes:
                    view.yesCount = view.yesCount + Count::fromDouble(1.0);
                    break;
                case VoteChoice::IfNeedBe:
                    view.ifNeedBeCount = view.ifNeedBeCount + Count::fromDouble(1.0);
                    break;
                case VoteChoice::No:
                    view.noCount = view.noCount + Count::fromDouble(1.0);
                    break;
                default:
                    break;
            }
            result.votes.push_back({.participantName = textOf(vote.participantName.Value()),
                                    .optionId = view.id,
                                    .choice = static_cast<VoteChoice>(vote.choice.Value())});
        }
        result.options.push_back(std::move(view));
    }

    auto comments = mapper.Query<db::CommentRecord>()
                        .Where(::Lightweight::FieldNameOf<&db::CommentRecord::poll>, "=", pollDbId)
                        .All();
    for (const auto& c : comments) {
        result.comments.push_back(
            {.participantName = textOf(c.participantName.Value()), .body = textOf(c.body.Value())});
    }

    auto lastEvent = mapper.Query<db::PollEventRecord>()
                         .Where(::Lightweight::FieldNameOf<&db::PollEventRecord::poll>, "=", pollDbId)
                         .OrderBy(::Lightweight::FieldNameOf<&db::PollEventRecord::id>,
                                  ::Lightweight::SqlResultOrdering::DESCENDING)
                         .First();
    result.lastEventId =
        lastEvent ? PollEventId::fromRowId(static_cast<std::int64_t>(lastEvent->id.Value())) : PollEventId{};
    return result;
}

/// @brief Encodes @p votes as JSON for `VoteHistoryRecord::previousVotesJson`.
///        `std::vector<OneVote>` is a plain aggregate of plain aggregates
///        (`OptionId` already has its own `glz::meta`), so Glaze reflects it
///        with no `glz::meta` specialization of its own -- the same
///        automatic reflection `BRIDGE_REGISTER_ACTION` relies on for user
///        action structs.
/// @throws PollsError on encode failure (structurally unreachable for this
///         flat a shape -- see `morph::journal::detail::throwOnGlazeError`'s
///         identical rationale for `LogEntry`, `morph/journal/action_log.hpp`).
[[nodiscard]] std::string encodeVotesJson(const std::vector<OneVote>& votes) {
    std::string out;
    if (auto errCode = glz::write_json(votes, out); errCode) {
        throw PollsError{glz::format_error(errCode, out)};
    }
    return out;
}

/// @brief The symmetric decode of `encodeVotesJson()` above, for
///        `UndoLastVoteChange` (Task 8) to reconstitute a
///        `VoteHistoryRecord::previousVotesJson` payload back into the vote
///        set `applyVotes()` can restore.
/// @throws PollsError on decode failure -- structurally unreachable in
///         practice (the only writer of this column is `encodeVotesJson()`
///         itself, in this same TU), but a stored value must still be
///         handled like any other fallible parse, not blindly trusted.
[[nodiscard]] std::vector<OneVote> decodeVotesJson(const std::string& json) {
    std::vector<OneVote> votes;
    if (auto errCode = glz::read_json(votes, json); errCode) {
        throw PollsError{glz::format_error(errCode, json)};
    }
    return votes;
}

/// @brief Whether @p a and @p b are equal, comparing every byte regardless
///        of an early mismatch -- unlike `std::string::operator==`/`!=`,
///        which short-circuits at the first differing byte and so leaks how
///        many leading bytes matched through response timing.
///
/// This is example/demo code whose whole security boundary is already just
/// the bare admin token (see this rung's README, resolved design decision
/// 1), so the practical bar for exploiting a timing side channel here is
/// low -- but every comparison against a secret token should still not be
/// the one place in the codebase that makes that side channel easy.
/// @param a One string to compare.
/// @param b The other string to compare.
/// @return `true` if @p a and @p b hold the same bytes.
[[nodiscard]] bool constantTimeEquals(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) {
        // The length itself is not treated as secret here (an admin token's
        // length is fixed and public -- kTokenBytes -- so this branch never
        // executes for a real token of the right length; a caller who sends
        // the wrong length learns nothing more than "wrong length", already
        // implied by kTokenBytes being a known, documented constant).
        return false;
    }
    unsigned char diff = 0;
    for (std::size_t i = 0; i < a.size(); ++i) {
        diff |= static_cast<unsigned char>(a[i]) ^ static_cast<unsigned char>(b[i]);
    }
    return diff == 0;
}

}  // namespace

void PollModel::requireAdmin(const AdminToken& adminToken) const {
    const auto* ctx = ::morph::session::current();
    if (ctx == nullptr || ctx->token.empty() || !adminToken.hasValue() ||
        !constantTimeEquals(ctx->token, *adminToken)) {
        throw Forbidden{"admin token required"};
    }
}

CreatePollResult PollModel::execute(const CreatePoll& action) {
    if (!action.validate()) {
        throw ValidationError{"CreatePoll: a bounded title and 2-20 bounded-label options are required"};
    }

    db::PollRecord poll;
    poll.pollId = Light::SqlAnsiString<kTokenBytes>{randomToken()};
    poll.adminToken = Light::SqlAnsiString<kTokenBytes>{randomToken()};
    poll.participantToken = Light::SqlAnsiString<kTokenBytes>{randomToken()};
    poll.title = action.title;
    poll.createdAtMs = nowMs();

    auto mapper = ::Lightweight::GlobalDataMapperPool().Acquire();
    ::Lightweight::SqlTransaction transaction{mapper->Connection(), ::Lightweight::SqlTransactionMode::ROLLBACK};
    mapper->Create(poll);
    std::int64_t order = 0;
    for (const auto& opt : action.options) {
        db::OptionRecord rec;
        rec.poll = poll;
        rec.label = opt.label;
        rec.sortOrder = order++;
        mapper->Create(rec);
    }
    transaction.Commit();

    return CreatePollResult{.pollId = textOf(poll.pollId.Value()),
                            .adminToken = AdminToken{textOf(poll.adminToken.Value())},
                            .participantToken = ParticipantToken{textOf(poll.participantToken.Value())}};
}

GetPollStateResult PollModel::execute(const OpenPoll& action) {
    if (!action.validate()) {
        throw ValidationError{"OpenPoll: pollId is required"};
    }
    auto mapper = ::Lightweight::GlobalDataMapperPool().Acquire();
    db::PollRecord poll = loadPollByPollId(mapper.Get(), action.pollId);
    // Cache the pollId once this handler has proven it names a real poll,
    // before dispatching to buildState() -- execute(GetPollState) below
    // reads this cache to re-derive which poll it is, since GetPollState
    // itself carries no pollId of its own (it is dispatched against an
    // already-OpenPoll-attached handler).
    _pollId = action.pollId;
    return buildState(mapper.Get(), poll);
}

GetPollStateResult PollModel::execute(const GetPollState& /*action*/) {
    // GetPollState carries no pollId of its own -- it is dispatched against
    // an already-attached handler (attach happens via OpenPoll, the keyed
    // action). If this handler was never attached via OpenPoll first, that
    // is a caller error: there is no poll to report state for.
    if (!_pollId.has_value()) {
        throw NotFound{"GetPollState: handler was never attached via OpenPoll"};
    }
    auto mapper = ::Lightweight::GlobalDataMapperPool().Acquire();
    return buildState(mapper.Get(), loadPollByPollId(mapper.Get(), *_pollId));
}

GetPollStateResult PollModel::applyVotes(const std::string& participantName, const std::vector<OneVote>& votes,
                                         const std::string& summaryVerb, WriteHistory writeHistory,
                                         std::optional<std::uint64_t> historyRowIdToDelete) {
    // Both callers (execute(SubmitVotes)/execute(UpdateVotes)) act against
    // this handler's attached poll, exactly like execute(GetPollState) --
    // never attached via OpenPoll is a caller error, not a NotFound-worthy
    // poll lookup failure.
    if (!_pollId.has_value()) {
        throw NotFound{"applyVotes: handler was never attached via OpenPoll"};
    }
    // One connection for this whole call: the pre-transaction reads below
    // inform the transaction's own writes (the prior-votes read in
    // particular must see the same data the delete-then-recreate loop
    // deletes), so everything here runs against a single acquisition.
    auto mapper = ::Lightweight::GlobalDataMapperPool().Acquire();
    db::PollRecord poll = loadPollByPollId(mapper.Get(), *_pollId);
    if (poll.finalized.Value()) {
        // A vote in flight when FinalizePoll lands must dead-letter with a
        // user-visible outcome, not vanish -- Conflict IS that outcome,
        // delivered through the caller's .onError(...).
        throw Conflict{"poll is finalized"};
    }

    // Validated before any row is touched, not interleaved with the
    // delete-then-recreate loop below: a vote naming another poll's option
    // must reject the *whole* submission, not delete the participant's prior
    // votes and then partially apply the new ones before hitting a bad
    // entry. See requireOptionBelongsToPoll's own doc comment for why this
    // check exists at all (the DB's own FK is not enforced here).
    for (const auto& ov : votes) {
        requireOptionBelongsToPoll(mapper.Get(), poll, ov.optionId);
    }

    const std::uint64_t pollDbId = poll.id.Value();
    auto priorVotes = mapper->Query<db::VoteRecord>()
                          .Where(::Lightweight::FieldNameOf<&db::VoteRecord::poll>, "=", pollDbId)
                          .Where(::Lightweight::FieldNameOf<&db::VoteRecord::participantName>, "=", participantName)
                          .All();

    // Captured before any row is deleted: the *pre-change* vote set is what
    // UndoLastVoteChange (Task 8) needs to restore.
    std::vector<OneVote> previousVotes;
    previousVotes.reserve(priorVotes.size());
    for (const auto& v : priorVotes) {
        previousVotes.push_back({.optionId = OptionId::fromRowId(static_cast<std::int64_t>(v.option.Value())),
                                 .choice = static_cast<VoteChoice>(v.choice.Value())});
    }
    const std::string previousVotesJson = encodeVotesJson(previousVotes);

    ::Lightweight::SqlTransaction transaction{mapper->Connection(), ::Lightweight::SqlTransactionMode::ROLLBACK};

    // Delete-then-recreate: replaces the participant's votes wholesale
    // rather than diffing old vs. new, so a retried SubmitVotes for the same
    // participant (the DoD's own retry scenario) converges on one row per
    // option instead of ever risking a duplicate -- backed by
    // idx_votes_poll_participant_option's unique index as the last line of
    // defense, not the primary mechanism.
    for (auto& prior : priorVotes) {
        mapper->Delete(prior);
    }
    for (const auto& ov : votes) {
        db::VoteRecord rec;
        rec.poll = poll;
        rec.option = static_cast<std::uint64_t>(ov.optionId.value);
        rec.participantName = participantName;
        rec.choice = static_cast<std::uint8_t>(ov.choice);
        mapper->Create(rec);
    }

    if (writeHistory == WriteHistory::Yes) {
        db::VoteHistoryRecord history;
        history.poll = poll;
        history.participantName = participantName;
        history.previousVotesJson = previousVotesJson;
        history.createdAtMs = nowMs();
        mapper->Create(history);
    }

    // Folded into this same transaction (not deleted by the caller
    // afterward) so the restore write and the consumed history row's
    // deletion commit together or not at all -- see this method's own doc
    // comment (poll_model.hpp) and execute(UndoLastVoteChange)'s call site.
    if (historyRowIdToDelete.has_value()) {
        auto rowsToDelete =
            mapper->Query<db::VoteHistoryRecord>()
                .Where(::Lightweight::FieldNameOf<&db::VoteHistoryRecord::id>, "=", *historyRowIdToDelete)
                .All();
        for (auto& row : rowsToDelete) {
            mapper->Delete(row);
        }
    }

    db::PollEventRecord event;
    event.poll = poll;
    event.kind = "vote";
    event.summary = participantName + " " + summaryVerb;
    event.createdAtMs = nowMs();
    mapper->Create(event);

    transaction.Commit();

    return buildState(mapper.Get(), poll);
}

GetPollStateResult PollModel::execute(const SubmitVotes& action) {
    if (!action.validate()) {
        throw ValidationError{
            "SubmitVotes: a bounded participantName and at least one vote (with no repeated optionId) are required"};
    }
    return applyVotes(action.participantName, action.votes, "submitted votes", WriteHistory::Yes);
}

GetPollStateResult PollModel::execute(const UpdateVotes& action) {
    if (!action.validate()) {
        throw ValidationError{
            "UpdateVotes: a bounded participantName and at least one vote (with no repeated optionId) are required"};
    }
    return applyVotes(action.participantName, action.votes, "updated votes", WriteHistory::Yes);
}

GetPollStateResult PollModel::execute(const AddComment& action) {
    if (!action.validate()) {
        throw ValidationError{"AddComment: a bounded participantName and body are required"};
    }
    if (!_pollId.has_value()) {
        throw NotFound{"AddComment: handler was never attached via OpenPoll"};
    }
    auto mapper = ::Lightweight::GlobalDataMapperPool().Acquire();
    db::PollRecord poll = loadPollByPollId(mapper.Get(), *_pollId);
    if (poll.finalized.Value()) {
        // FinalizePoll's own doc comment: finalizing makes the poll
        // read-only -- that applies to every write, not only votes.
        throw Conflict{"poll is finalized"};
    }

    ::Lightweight::SqlTransaction transaction{mapper->Connection(), ::Lightweight::SqlTransactionMode::ROLLBACK};

    db::CommentRecord comment;
    comment.poll = poll;
    comment.participantName = action.participantName;
    comment.body = action.body;
    comment.createdAtMs = nowMs();
    mapper->Create(comment);

    db::PollEventRecord event;
    event.poll = poll;
    event.kind = "comment";
    event.summary = action.participantName + " commented";
    event.createdAtMs = nowMs();
    mapper->Create(event);

    transaction.Commit();

    return buildState(mapper.Get(), poll);
}

GetPollStateResult PollModel::execute(const FinalizePoll& action) {
    if (!action.validate()) {
        throw ValidationError{"FinalizePoll: a real optionId is required"};
    }
    if (!_pollId.has_value()) {
        throw NotFound{"FinalizePoll: handler was never attached via OpenPoll"};
    }
    auto mapper = ::Lightweight::GlobalDataMapperPool().Acquire();
    db::PollRecord poll = loadPollByPollId(mapper.Get(), *_pollId);

    // Token check strictly before the already-finalized check: a caller who
    // does not hold the admin token must get the same Forbidden regardless
    // of the poll's current state, never a Conflict that would leak "this
    // poll is already finalized" to someone who has not proven they may act
    // on it at all. See this rung's README design decision 1 and this
    // method's own header doc comment.
    requireAdmin(AdminToken{textOf(poll.adminToken.Value())});

    if (poll.finalized.Value()) {
        throw Conflict{"poll is already finalized"};
    }
    requireOptionBelongsToPoll(mapper.Get(), poll, action.optionId);

    ::Lightweight::SqlTransaction transaction{mapper->Connection(), ::Lightweight::SqlTransactionMode::ROLLBACK};
    poll.finalized = true;
    poll.finalizedOptionId = *action.optionId;
    mapper->Update(poll);

    db::PollEventRecord event;
    event.poll = poll;
    event.kind = "finalize";
    event.summary = "poll finalized";
    event.createdAtMs = nowMs();
    mapper->Create(event);

    transaction.Commit();

    return buildState(mapper.Get(), poll);
}

// ---------------------------------------------------------------------------
// UndoLastVoteChange -- this rung's headline design record (Task 8). See
// the README's resolved design decision 3: `SessionLog::undoLast()`
// (docs/spec/journal/journal.md) pops the newest journal entry regardless
// of which principal made it, and hands back a fresh, detached model
// holder no API can install into a live shared instance -- neither
// property this action needs is available from the framework journal, so
// `PollModel` owns its own small `vote_history` table (Task 4) and this
// method reads/reverses it directly, entirely at the app level.
// ---------------------------------------------------------------------------

UndoLastVoteChangeResult PollModel::execute(const UndoLastVoteChange& action) {
    if (!action.validate()) {
        throw ValidationError{"UndoLastVoteChange: participantName is required"};
    }
    if (!_pollId.has_value()) {
        throw NotFound{"UndoLastVoteChange: handler was never attached via OpenPoll"};
    }
    // Read-only lookup, its own single acquisition: only historyRowId (a
    // plain integer) crosses into applyVotes() below, which does its own
    // separate acquisition for the actual restore transaction -- nothing
    // here depends on being on the same physical connection as that write.
    auto mapper = ::Lightweight::GlobalDataMapperPool().Acquire();
    db::PollRecord poll = loadPollByPollId(mapper.Get(), *_pollId);
    const std::uint64_t pollDbId = poll.id.Value();

    // "Most recent row for this participant" -- same OrderBy(...DESCENDING)
    // + First() shape buildState()'s own lastEvent lookup above uses for
    // "most recent PollEventRecord", the established precedent in this TU
    // for this exact query pattern.
    auto history =
        mapper->Query<db::VoteHistoryRecord>()
            .Where(::Lightweight::FieldNameOf<&db::VoteHistoryRecord::poll>, "=", pollDbId)
            .Where(::Lightweight::FieldNameOf<&db::VoteHistoryRecord::participantName>, "=", action.participantName)
            .OrderBy(::Lightweight::FieldNameOf<&db::VoteHistoryRecord::id>,
                     ::Lightweight::SqlResultOrdering::DESCENDING)
            .First();
    if (!history.has_value()) {
        // Nothing to undo: this participant never changed their vote on this
        // poll, or a prior UndoLastVoteChange already consumed the one entry
        // that existed -- either way, a Conflict, not a silent no-op.
        throw Conflict{"nothing to undo for this participant"};
    }

    const std::vector<OneVote> previousVotes = decodeVotesJson(textOf(history->previousVotesJson.Value()));
    const std::uint64_t historyRowId = history->id.Value();

    // Reuse applyVotes() (Task 6) directly for the restore itself, exactly
    // like SubmitVotes/UpdateVotes: same delete-then-recreate write, same
    // fresh PollEventRecord as this call's own audit entry (its own summary
    // verb naming the undo, per the brief) -- not a duplicated write path.
    //
    // WriteHistory::No: restoring must not itself append a new
    // VoteHistoryRecord -- left in place, a fresh row capturing "what the
    // participant had immediately before the undo" would let a second
    // UndoLastVoteChange silently undo the undo, turning a one-shot
    // compensating action into an unbounded ping-pong.
    //
    // historyRowId: the one row this call itself just read above (the
    // consumed history entry) is deleted by applyVotes() inside its own
    // transaction, alongside the restore write -- so the restore and the
    // one-shot cleanup commit together, atomically, never in two separate
    // transactions with a window between them where the vote set is
    // restored but the consumed row (or a spurious new one) still exists.
    // This is what makes "undo is one-shot, not a redo stack" (the brief's
    // own words) true, and it is exactly what the "undoing twice in a row"
    // test below verifies.
    (void)applyVotes(action.participantName, previousVotes, "undid their last vote change", WriteHistory::No,
                     historyRowId);

    return UndoLastVoteChangeResult{.restored = Restored::Yes};
}

// ---------------------------------------------------------------------------
// GetEventsSince (Task 9) -- the Zulip-pattern event log's read side. Every
// mutating action above (applyVotes()'s SubmitVotes/UpdateVotes/
// UndoLastVoteChange callers, execute(AddComment), execute(FinalizePoll))
// already appends a PollEventRecord inside its own write transaction; this is
// the last piece, reading that log back out from a cursor.
// ---------------------------------------------------------------------------

GetEventsSinceResult PollModel::execute(const GetEventsSince& action) {
    if (!action.validate()) {
        throw ValidationError{"GetEventsSince: malformed request"};
    }
    // Carries no pollId of its own -- dispatched against an already-attached
    // handler, exactly like execute(GetPollState)/execute(FinalizePoll)/
    // execute(UndoLastVoteChange) above.
    if (!_pollId.has_value()) {
        throw NotFound{"GetEventsSince: handler was never attached via OpenPoll"};
    }
    auto mapper = ::Lightweight::GlobalDataMapperPool().Acquire();
    db::PollRecord poll = loadPollByPollId(mapper.Get(), *_pollId);
    const std::uint64_t pollDbId = poll.id.Value();

    // Opposite direction and full-result-set counterpart of buildState()'s
    // own lastEvent lookup above (Where(poll=...).OrderBy(id, DESCENDING)
    // .First()): ascending by id, every row, not just the newest one.
    // action.lastEventId defaults to PollEventId{} (value 0); poll_events.id
    // is a ServerSideAutoIncrement primary key starting at 1, so
    // `id > 0` already matches every row -- "from the beginning" falls out of
    // this same query with no special-case branch.
    auto rows = mapper->Query<db::PollEventRecord>()
                    .Where(::Lightweight::FieldNameOf<&db::PollEventRecord::poll>, "=", pollDbId)
                    .Where(::Lightweight::FieldNameOf<&db::PollEventRecord::id>, ">",
                           static_cast<std::uint64_t>(*action.lastEventId))
                    .OrderBy(::Lightweight::FieldNameOf<&db::PollEventRecord::id>)
                    .All();

    GetEventsSinceResult result;
    result.events.reserve(rows.size());
    for (const auto& row : rows) {
        result.events.push_back({.id = PollEventId::fromRowId(static_cast<std::int64_t>(row.id.Value())),
                                 .kind = textOf(row.kind.Value()),
                                 .summary = textOf(row.summary.Value())});
    }
    return result;
}

}  // namespace polls

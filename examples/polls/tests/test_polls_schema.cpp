// SPDX-License-Identifier: Apache-2.0
#include "polls/db/poll_entity.hpp"
#include "testkit/db_fixture.hpp"

#include <Lightweight/DataMapper/DataMapper.hpp>
#include <catch2/catch_test_macros.hpp>

using morph::ladder::testkit::DbFixture;

TEST_CASE("The polls schema creates all six tables and a poll round-trips", "[polls][db]") {
    DbFixture fixture;
    Lightweight::DataMapper mapper;

    polls::db::PollRecord poll;
    poll.pollId = Light::SqlAnsiString<polls::kTokenBytes>{"poll-abc"};
    poll.adminToken = Light::SqlAnsiString<polls::kTokenBytes>{"admin-xyz"};
    poll.participantToken = Light::SqlAnsiString<polls::kTokenBytes>{"part-xyz"};
    poll.title = "Team offsite";
    poll.createdAtMs = 1000;
    mapper.Create(poll);
    REQUIRE(poll.id.Value() != 0);

    polls::db::OptionRecord opt;
    opt.poll = poll;
    opt.label = "2026-09-01";
    opt.sortOrder = 0;
    mapper.Create(opt);
    REQUIRE(opt.id.Value() != 0);

    auto loadedOptions = mapper.Query<polls::db::OptionRecord>()
                              .Where(::Lightweight::FieldNameOf<&polls::db::OptionRecord::poll>, "=", poll.id.Value())
                              .All();
    REQUIRE(loadedOptions.size() == 1);
    CHECK(loadedOptions.front().label.Value() == "2026-09-01");

    // The remaining four tables are read via a plain Query<T>().Where(...)
    // on the poll's own id, never through an embedded relation field on
    // PollRecord -- see this rung's Global Constraints, and poll_entity.hpp's
    // file comment.
    polls::db::VoteRecord vote;
    vote.poll = poll;
    vote.option = opt;
    vote.participantName = "alice";
    vote.choice = std::uint8_t{0};
    mapper.Create(vote);
    REQUIRE(vote.id.Value() != 0);

    polls::db::CommentRecord comment;
    comment.poll = poll;
    comment.participantName = "alice";
    comment.body = "See you there!";
    comment.createdAtMs = 1001;
    mapper.Create(comment);
    REQUIRE(comment.id.Value() != 0);

    polls::db::VoteHistoryRecord history;
    history.poll = poll;
    history.participantName = "alice";
    history.previousVotesJson = "[]";
    history.createdAtMs = 1002;
    mapper.Create(history);
    REQUIRE(history.id.Value() != 0);

    polls::db::PollEventRecord event;
    event.poll = poll;
    event.kind = "vote";
    event.summary = "alice voted";
    event.createdAtMs = 1003;
    mapper.Create(event);
    REQUIRE(event.id.Value() != 0);

    auto loadedVotes = mapper.Query<polls::db::VoteRecord>()
                           .Where(::Lightweight::FieldNameOf<&polls::db::VoteRecord::poll>, "=", poll.id.Value())
                           .All();
    REQUIRE(loadedVotes.size() == 1);
    CHECK(loadedVotes.front().participantName.Value() == "alice");
}

TEST_CASE("Duplicate (pollId, participantName, optionId) votes are rejected by the unique index",
          "[polls][db]") {
    DbFixture fixture;
    Lightweight::DataMapper mapper;

    polls::db::PollRecord poll;
    poll.pollId = Light::SqlAnsiString<polls::kTokenBytes>{"poll-dup"};
    poll.adminToken = Light::SqlAnsiString<polls::kTokenBytes>{"admin-dup"};
    poll.participantToken = Light::SqlAnsiString<polls::kTokenBytes>{"part-dup"};
    poll.title = "Dup test";
    poll.createdAtMs = 1000;
    mapper.Create(poll);

    polls::db::OptionRecord opt;
    opt.poll = poll;
    opt.label = "2026-09-02";
    opt.sortOrder = 0;
    mapper.Create(opt);

    polls::db::VoteRecord first;
    first.poll = poll;
    first.option = opt;
    first.participantName = "bob";
    first.choice = std::uint8_t{0};
    mapper.Create(first);

    // A retried SubmitVotes (Task 6) must not double-count -- this is the
    // exact index the VoteRecord doc comment names.
    polls::db::VoteRecord second;
    second.poll = poll;
    second.option = opt;
    second.participantName = "bob";
    second.choice = std::uint8_t{1};
    CHECK_THROWS_AS(mapper.Create(second), Lightweight::SqlException);
}

TEST_CASE("PollRecord has no relation-typed member -- Update() must compile", "[polls][db]") {
    // A compile-time proof, not a runtime assertion: if PollRecord ever grows
    // an embedded HasMany/HasManyThrough field, this line stops compiling
    // with the exact "no member IsModified" error the Global Constraints
    // section documents.
    DbFixture fixture;
    Lightweight::DataMapper mapper;

    polls::db::PollRecord poll;
    poll.pollId = Light::SqlAnsiString<polls::kTokenBytes>{"poll-upd"};
    poll.adminToken = Light::SqlAnsiString<polls::kTokenBytes>{"admin-upd"};
    poll.participantToken = Light::SqlAnsiString<polls::kTokenBytes>{"part-upd"};
    poll.title = "Before";
    poll.createdAtMs = 1;
    mapper.Create(poll);
    poll.title = "After";
    CHECK_NOTHROW(mapper.Update(poll));
}

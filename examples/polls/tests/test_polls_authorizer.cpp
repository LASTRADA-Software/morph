// SPDX-License-Identifier: Apache-2.0
//
// PollsAuthorizer's own suite (Task 7). Unlike bookmarks' authorizer, there
// is no signed-token verification to exercise here (see the header's own
// @file comment) -- every hook is unconditionally permissive, so these
// tests confirm exactly that against the real morph::session::IAuthorizer
// signatures, not against a guessed shape.
#include <catch2/catch_test_macros.hpp>
#include <morph/session/session.hpp>

#include "polls/auth/polls_authorizer.hpp"

using morph::session::Context;
using polls::auth::PollsAuthorizer;

TEST_CASE("PollsAuthorizer::authorize admits every call -- there is no signed token to verify in this rung",
          "[polls][auth]") {
    const PollsAuthorizer authorizer;
    const Context anonymous;  // no token at all
    CHECK(authorizer.authorize(anonymous, "PollModel", "FinalizePoll"));
    CHECK(authorizer.authorize(anonymous, "PollModel", "SubmitVotes"));

    Context withToken;
    withToken.token = "not-a-signed-anything";
    CHECK(authorizer.authorize(withToken, "PollModel", "FinalizePoll"));
}

TEST_CASE("PollsAuthorizer::authorizeRegister admits every register, by this rung's own design", "[polls][auth]") {
    const PollsAuthorizer authorizer;

    // `anonymous` is one real input among others now that
    // wire::makeRegister/wire::makeRegisterShared both carry the caller's
    // session; authorizeRegister here stays permissive by choice, not
    // because there is no identity to check (see the header's own @file
    // comment and the rung README's design decision 2).
    const Context anonymous;
    CHECK(authorizer.authorizeRegister(anonymous, "PollModel"));

    // A stamped principal changes nothing -- the decision does not key on it.
    Context authenticated;
    authenticated.principal = "alice";
    CHECK(authorizer.authorizeRegister(authenticated, "PollModel"));
}

TEST_CASE("PollsAuthorizer::authorizeInstance admits every instance operation -- no owner concept in this rung",
          "[polls][auth]") {
    const PollsAuthorizer authorizer;
    const Context asAlice = [] {
        Context ctx;
        ctx.principal = "alice";
        return ctx;
    }();

    // No recorded owner -- the only case reachable here, since PollModel is
    // always shared/keyed by pollId, which the framework records ownerless
    // by design (unrelated to whether register envelopes carry a session)...
    CHECK(authorizer.authorizeInstance(asAlice, "PollModel", "FinalizePoll", 1, ""));
    // ...and even a non-empty ownerPrincipal (hypothetical -- see the header's
    // own doc comment: PollModel has no per-caller ownership concept at all,
    // only the admin/participant token check FinalizePoll performs itself).
    CHECK(authorizer.authorizeInstance(asAlice, "PollModel", "FinalizePoll", 1, "someone-else"));
}

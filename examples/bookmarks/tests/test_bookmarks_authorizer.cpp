// SPDX-License-Identifier: Apache-2.0
#include "bookmarks/auth/bookmarks_authorizer.hpp"

#include "bookmarks/models/auth_model.hpp"
#include "bookmarks/models/bookmark_model.hpp"
#include "testkit/backend_rig.hpp"
#include "testkit/db_fixture.hpp"
#include "testkit/pump.hpp"

#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <string>

using bookmarks::auth::BookmarksAuthorizer;
using bookmarks::auth::isValidPrincipal;
using bookmarks::auth::kMetadataFetcherPrincipal;
using morph::ladder::testkit::awaitQt;
using morph::ladder::testkit::BackendRig;
using morph::ladder::testkit::DbFixture;
using morph::ladder::testkit::Mode;
using morph::session::Context;
using morph::session::SessionToken;
using morph::session::TokenIssuer;

namespace {
constexpr std::string_view kSecret = "test-only-shared-secret";

/// @brief Installs a process-global `TokenIssuer` for a scope and clears it
///        again on the way out, whether the scope exits normally or through a
///        failing Catch2 assertion.
class ScopedTokenIssuer {
  public:
    explicit ScopedTokenIssuer(std::shared_ptr<TokenIssuer> issuer) {
        bookmarks::auth::setTokenIssuer(std::move(issuer));
    }
    ~ScopedTokenIssuer() { bookmarks::auth::setTokenIssuer(nullptr); }
    ScopedTokenIssuer(const ScopedTokenIssuer&) = delete;
    ScopedTokenIssuer& operator=(const ScopedTokenIssuer&) = delete;
    ScopedTokenIssuer(ScopedTokenIssuer&&) = delete;
    ScopedTokenIssuer& operator=(ScopedTokenIssuer&&) = delete;
};
}  // namespace

TEST_CASE("isValidPrincipal accepts ordinary usernames and the service principal",
          "[bookmarks][auth]") {
    CHECK(isValidPrincipal("alice"));
    CHECK(isValidPrincipal("alice_2"));
    CHECK(isValidPrincipal("alice.smith-99"));
    CHECK(isValidPrincipal(kMetadataFetcherPrincipal));
}

TEST_CASE("isValidPrincipal rejects the empty string, control bytes, and overlong input",
          "[bookmarks][auth]") {
    // Empty: never a valid identity to register as.
    CHECK_FALSE(isValidPrincipal(""));
    // A raw control byte -- the class of input TokenIssuer::issue()'s
    // glz::write_json now escapes correctly, but rejected here too, at this
    // rung's own boundary, as an independent line of defense regardless.
    // Split into two adjacent string-literal tokens: `\x` escapes consume
    // every following hex digit, and `c`/`e` are valid hex digits, so an
    // unsplit "ali\x01ce" is parsed as the single out-of-range escape
    // `\x01ce` rather than `\x01` followed by literal "ce".
    CHECK_FALSE(isValidPrincipal(std::string_view{"ali\x01"
                                                   "ce",
                                                   6}));
    CHECK_FALSE(isValidPrincipal(std::string_view{"ali\nce", 6}));
    // 65 bytes -- one past the 64-byte bound.
    const std::string tooLong(65, 'a');
    CHECK_FALSE(isValidPrincipal(tooLong));
    // 64 bytes -- the boundary itself is accepted.
    const std::string atLimit(64, 'a');
    CHECK(isValidPrincipal(atLimit));
}

TEST_CASE("BookmarksAuthorizer authenticates and authorizes a validly signed token",
          "[bookmarks][auth]") {
    const BookmarksAuthorizer authz{std::string{kSecret}, morph::session::hmacSha256};
    const TokenIssuer issuer{std::string{kSecret}, morph::session::hmacSha256};

    const std::string token = issuer.issue(SessionToken{
        .principal = "alice",
        .issuedAtMs = 0,
        .expiresAtMs = 4102444800000,  // year 2100, far future
        .roles = {},
    });

    Context ctx;
    ctx.token = token;

    CHECK(authz.authorize(ctx, "BookmarkModel", "CreateBookmark"));
    const auto principal = authz.authenticate(ctx);
    REQUIRE(principal.has_value());
    CHECK(*principal == "alice");
}

TEST_CASE("BookmarksAuthorizer rejects a tampered or expired token", "[bookmarks][auth]") {
    const BookmarksAuthorizer authz{std::string{kSecret}, morph::session::hmacSha256};
    const TokenIssuer issuer{std::string{kSecret}, morph::session::hmacSha256};

    const std::string expired = issuer.issue(SessionToken{
        .principal = "alice",
        .issuedAtMs = 0,
        .expiresAtMs = 1,  // 1970-01-01T00:00:00.001Z -- long expired
        .roles = {},
    });
    Context expiredCtx;
    expiredCtx.token = expired;
    CHECK_FALSE(authz.authorize(expiredCtx, "BookmarkModel", "CreateBookmark"));

    const std::string valid = issuer.issue(SessionToken{
        .principal = "alice",
        .issuedAtMs = 0,
        .expiresAtMs = 4102444800000,
        .roles = {},
    });
    Context tamperedCtx;
    tamperedCtx.token = valid + "x";  // corrupt the signature
    CHECK_FALSE(authz.authorize(tamperedCtx, "BookmarkModel", "CreateBookmark"));

    Context noTokenCtx;  // empty token: malformed
    CHECK_FALSE(authz.authorize(noTokenCtx, "BookmarkModel", "CreateBookmark"));
}

TEST_CASE("BookmarksAuthorizer::authorizeRegister admits an anonymous register, by choice "
          "rather than necessity",
          "[bookmarks][auth]") {
    const BookmarksAuthorizer authz{std::string{kSecret}, morph::session::hmacSha256};

    // `anonymous` is a real, reachable input -- an unauthenticated client's
    // first construction -- but no longer the *only* one now that
    // `register`/`attach`/`assign`/`deregister` envelopes carry the caller's
    // session: an authenticated caller's `ctx.principal` is populated here
    // too. This hook stays unconditionally permissive regardless of which
    // one it sees -- see authorizeRegister's own doc comment for why.
    Context anonymous;
    CHECK(authz.authorizeRegister(anonymous, "BookmarkModel"));
    CHECK(authz.authorizeRegister(anonymous, "TagModel"));
    CHECK(authz.authorizeRegister(anonymous, "SharedFeedModel"));
    CHECK(authz.authorizeRegister(anonymous, "AuthModel"));

    // A stamped principal changes nothing -- the decision does not key on it
    // in either direction.
    Context authenticated;
    authenticated.principal = "alice";
    CHECK(authz.authorizeRegister(authenticated, "BookmarkModel"));
}

TEST_CASE("Registering is not authorizing: an anonymous caller's execute is still refused",
          "[bookmarks][auth]") {
    // The property that actually carries this rung's trust boundary now that
    // authorizeRegister admits everyone. `authorize()` is consulted on every
    // single execute (remote.hpp:1160), before authenticate() and before any
    // model runs, and it is the inherited SigningAuthorizer one.
    const BookmarksAuthorizer authz{std::string{kSecret}, morph::session::hmacSha256};

    Context anonymous;  // no token at all -- exactly what an un-logged-in client has
    CHECK_FALSE(authz.authorize(anonymous, "BookmarkModel", "CreateBookmark"));
    CHECK_FALSE(authz.authorize(anonymous, "BookmarkModel", "RecordMetadata"));
    CHECK_FALSE(authz.authorize(anonymous, "TagModel", "RenameTag"));

    // A token signed with the wrong secret is refused just as flatly -- an
    // instance registered anonymously buys a caller no shortcut here.
    const TokenIssuer wrongIssuer{std::string{"not-the-server-secret"}, morph::session::hmacSha256};
    Context forged;
    forged.token = wrongIssuer.issue(SessionToken{
        .principal = "alice",
        .issuedAtMs = 0,
        .expiresAtMs = 4102444800000,
        .roles = {},
    });
    CHECK_FALSE(authz.authorize(forged, "BookmarkModel", "CreateBookmark"));
    CHECK_FALSE(authz.authenticate(forged).has_value());
}

TEST_CASE("BookmarksAuthorizer::authorizeInstance enforces real ownership for a "
          "plain-registered instance, and passes through an ownerless (shared) one",
          "[bookmarks][auth]") {
    // `register` envelopes now carry the caller's session, so RemoteServer
    // records a real, non-empty `ownerPrincipal` for a plain-registered
    // instance -- all three CHECKs below are reachable in production
    // (against a real `RemoteServer`, not just at this unit level), not
    // merely illustrations of hypothetical future behavior. See the
    // function's own doc comment for what this does and does not protect.
    const BookmarksAuthorizer authz{std::string{kSecret}, morph::session::hmacSha256};

    Context asAlice;
    asAlice.principal = "alice";
    Context asMallory;
    asMallory.principal = "mallory";

    // A plain-registered instance genuinely recorded "alice" as its owner
    // (RemoteServer's real register path, verified in this plan's own
    // research -- see remote.hpp:1011): the owner may act on it...
    CHECK(authz.authorizeInstance(asAlice, "BookmarkModel", "EditBookmark", 42, "alice"));
    // ...a different, real, authenticated principal may not.
    CHECK_FALSE(authz.authorizeInstance(asMallory, "BookmarkModel", "EditBookmark", 42, "alice"));

    // An empty recorded owner -- what a *shared* instance always gets
    // (remote.hpp:800, "shared instances are ownerless, by design") -- must
    // pass through for anyone, matching the framework's own documented
    // rationale for why authorizeInstance cannot reject shared access.
    CHECK(authz.authorizeInstance(asMallory, "SharedFeedModel", "ListSharedFeed", 7, ""));
}

TEST_CASE("setTokenIssuer/tokenIssuer share one process-global slot", "[bookmarks][auth]") {
    CHECK(bookmarks::auth::tokenIssuer() == nullptr);
    auto issuer = std::make_shared<morph::session::TokenIssuer>(std::string{kSecret}, morph::session::hmacSha256);
    bookmarks::auth::setTokenIssuer(issuer);
    CHECK(bookmarks::auth::tokenIssuer() == issuer);
    bookmarks::auth::setTokenIssuer(nullptr);
    CHECK(bookmarks::auth::tokenIssuer() == nullptr);
}

TEST_CASE("BookmarksAuthorizer::authorize admits Login without a token, and nothing else",
          "[bookmarks][auth]") {
    // The carve-out that makes login possible at all. Without it
    // SigningAuthorizer::authorize() rejects every tokenless execute --
    // including the one action whose whole purpose is handing out the first
    // token -- and a fresh client can never get past `err "unauthorized"`.
    // See BookmarksAuthorizer::authorize's own doc comment.
    const BookmarksAuthorizer authz{std::string{kSecret}, morph::session::hmacSha256};
    const Context anonymous;  // no token at all, like a just-launched client

    CHECK(authz.authorize(anonymous, "AuthModel", "Login"));

    // Nothing else is reachable anonymously -- not another action on the same
    // model, not the same action name on another model, and not any real
    // domain action.
    CHECK_FALSE(authz.authorize(anonymous, "AuthModel", "SomethingElse"));
    CHECK_FALSE(authz.authorize(anonymous, "BookmarkModel", "Login"));
    CHECK_FALSE(authz.authorize(anonymous, "BookmarkModel", "CreateBookmark"));
    CHECK_FALSE(authz.authorize(anonymous, "TagModel", "ListTags"));
    CHECK_FALSE(authz.authorize(anonymous, "SharedFeedModel", "ListSharedFeed"));

    // A garbage token is still a rejection everywhere but the carve-out --
    // the carve-out ignores the token rather than accepting a bad one.
    Context forged;
    forged.principal = "alice";
    forged.token = "not.a.real.token";
    CHECK_FALSE(authz.authorize(forged, "BookmarkModel", "CreateBookmark"));
    CHECK(authz.authorize(forged, "AuthModel", "Login"));
    CHECK_FALSE(authz.authenticate(forged).has_value());
}

TEST_CASE("A tokenless client logs in over a real RemoteServer and its token unlocks the rest",
          "[bookmarks][auth]") {
    // The end-to-end shape of the bug above, at the wire level: this is the
    // exact sequence a freshly launched desktop client performs, and the one
    // no test covered before task 18 drove the real client against the real
    // server (every previous Login test called AuthModel::execute() directly,
    // which never consults an authorizer at all).
    DbFixture fixture;
    const auto authorizer = std::make_shared<BookmarksAuthorizer>(std::string{kSecret}, morph::session::hmacSha256);
    // RAII, not a trailing reset: a failing REQUIRE below throws, and a
    // leaked process-global issuer would then break the sibling case that
    // asserts none is installed ("AuthModel::execute(Login) throws when no
    // App has installed a TokenIssuer", test_app.cpp) under any run order.
    const ScopedTokenIssuer issuer{std::make_shared<TokenIssuer>(std::string{kSecret}, morph::session::hmacSha256)};
    BackendRig rig{Mode::Socket, 1, authorizer};

    // Deliberately no setDefaultSession: this bridge carries no credential.
    morph::bridge::BridgeHandler<bookmarks::AuthModel> auth{rig.bridge(0), rig.executor()};
    morph::bridge::BridgeHandler<bookmarks::BookmarkModel> bookmarksHandler{rig.bridge(0), rig.executor()};

    // Without a token, a domain action is refused by the server.
    bookmarks::CreateBookmark beforeLogin;
    beforeLogin.url = "https://example.com/before";
    CHECK_THROWS(awaitQt(bookmarksHandler.execute(beforeLogin)));

    const auto result = awaitQt(auth.execute(bookmarks::Login{.username = "alice"}));
    REQUIRE(result.token.hasValue());
    CHECK(result.principal == "alice");

    // Exactly what FormsBridge::onLoginSucceeded does with the reply.
    morph::session::Context session;
    session.principal = result.principal;
    session.token = *result.token;
    rig.bridge(0).setDefaultSession(session);

    bookmarks::CreateBookmark afterLogin;
    afterLogin.url = "https://example.com/after";
    const auto created = awaitQt(bookmarksHandler.execute(afterLogin));
    REQUIRE(created.id.hasValue());

    const auto listed = awaitQt(bookmarksHandler.execute(bookmarks::ListBookmarks{}));
    REQUIRE(listed.bookmarks.size() == 1);
    CHECK(listed.bookmarks.front().url == "https://example.com/after");
}

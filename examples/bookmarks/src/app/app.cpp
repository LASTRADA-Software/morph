// SPDX-License-Identifier: Apache-2.0
#include "bookmarks/app/app.hpp"

#include "bookmarks/auth/bookmarks_authorizer.hpp"
#include "bookmarks/db/outbox_entity.hpp"
#include "bookmarks/dto/bookmark_dto.hpp"
// Every model this server hosts is included here, not only the one the
// metadata worker dispatches against. `BRIDGE_REGISTER_MODEL`/
// `BRIDGE_REGISTER_ACTION` place their registrars in the *header*, so a
// translation unit that includes the header both registers the type with the
// process-wide registry/dispatcher and emits a reference to that model's
// `execute` bodies — which is what pulls each model's object file out of the
// static library for a binary (a server `main()`) whose own code names
// nothing but `App`. Without this, such a binary would either fail to link or
// come up serving no models at all.
#include "bookmarks/models/auth_model.hpp"
#include "bookmarks/models/bookmark_model.hpp"
#include "bookmarks/models/shared_feed_model.hpp"
#include "bookmarks/models/tag_model.hpp"

#include <morph/core/logger.hpp>
#include <morph/journal/outbox.hpp>
#include <morph/session/session_auth.hpp>

#include <Lightweight/DataMapper/DataMapper.hpp>
#include <Lightweight/SqlStatement.hpp>

#include <cstdint>
#include <exception>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace bookmarks::app {

namespace {

/// @brief Expiry stamped into the metadata worker's own service token.
///
/// The same far-future constant `AuthModel` uses, for the same reason
/// (`SessionToken::expiresAtMs` must be strictly positive, so "no expiry" is
/// not expressible) — and with an additional one here: the worker has no
/// login to repeat, so a token that expired mid-run would silently stop the
/// background job on a long-lived server with nothing to renew it. The
/// process's own lifetime is the real bound; the token is never written down,
/// never leaves this process, and dies with it.
constexpr std::int64_t kServiceTokenExpiresAtMs = 4102444800000;  // 2100-01-01T00:00:00Z

/// @brief Live-instance cap this server installs.
///
/// This rung's `authorizeRegister` is unconditionally permissive by choice
/// (`bookmarks/auth/bookmarks_authorizer.hpp` — the framework can gate
/// registration on identity now that `register` envelopes carry the
/// caller's session, this rung's authorizer just doesn't), so an
/// unauthenticated client *can* make the server create model instances even
/// though it can never execute anything on them. `maxLiveModels` is the
/// framework's own answer to that shape of churn: past the cap a `register`
/// is answered `err "too many models"` and no instance is constructed. The
/// value is generous on purpose — the shipped client registers six instances
/// (the forms controller owns an `AuthModel`, a `BookmarkModel` and a
/// `TagModel` handler; the three presenters own a `BookmarkModel`, a
/// `TagModel` and a `SharedFeedModel` handler — see the README's "Six model
/// instances per client, not four" gap for why they cannot be shared), so
/// this is ~42 concurrent clients, not a limit a real session will meet.
constexpr std::size_t kMaxLiveModels = 256;

}  // namespace

App::App(std::filesystem::path actionLogPath, std::string tokenSecret,
         std::shared_ptr<IBookmarkMetadataFetcher> fetcher, std::chrono::milliseconds fetchInterval,
         std::chrono::milliseconds relayInterval, std::size_t workers, QObject* parent)
    // Initialiser order follows the declaration order in app.hpp, which is
    // itself chosen for teardown safety — see that header's comment.
    : QObject{parent},
      _actionLog{std::make_shared<::morph::journal::FileActionLog>(std::move(actionLogPath))},
      _pool{workers},
      // hmacSha256 named explicitly -- same reason as the two TokenIssuer
      // call sites below: BookmarksAuthorizer inherits SigningAuthorizer's
      // constructor, whose MacFunction default is dropped entirely under
      // MORPH_REQUIRE_VETTED_HMAC.
      _server{std::make_shared<::morph::backend::RemoteServer>(
          _pool, std::make_shared<auth::BookmarksAuthorizer>(tokenSecret, ::morph::session::hmacSha256))},
      _fetchBridge{std::make_unique<::morph::backend::SimulatedRemoteBackend>(*_server)},
      _fetcher{std::move(fetcher)} {
    ::morph::journal::setActionLog(_actionLog);

    // Installed process-wide so AuthModel::execute(const Login&) can mint
    // tokens against this exact secret — the same "registry-constructed
    // models are always default-constructed, so there is no DI seam" answer
    // morph::journal::setActionLog already uses one line above.
    // hmacSha256 named explicitly (not relying on TokenIssuer's default):
    // this rung wires no vetted MAC adapter (see examples/vetted_hmac/), so
    // under MORPH_REQUIRE_VETTED_HMAC -- which drops the default entirely,
    // by design (see TokenIssuer's own doc comment) -- this call site must
    // still compile with the identical MAC it always used.
    auth::setTokenIssuer(
        std::make_shared<::morph::session::TokenIssuer>(tokenSecret, ::morph::session::hmacSha256));

    ::morph::backend::LimitPolicy limits;
    limits.maxLiveModels = kMaxLiveModels;
    _server->setLimitPolicy(limits);

    // The worker's own service-principal session. Minted here rather than
    // through AuthModel deliberately: AuthModel *refuses* to mint a token in
    // the reserved `system:` namespace (see auth::isReservedPrincipal), which
    // is exactly the property that keeps a client from obtaining this
    // authority. The server process minting its own is the one legitimate
    // path, and it shares `tokenSecret` with the authorizer installed above,
    // so it verifies exactly like a real user's token.
    // hmacSha256 named explicitly for the identical reason as the
    // setTokenIssuer() call above -- and so both issuers stay verifiably the
    // same MAC, which they must be: the authorizer this rung installs
    // verifies every token (including this service one) against whichever
    // MAC minted it.
    const ::morph::session::TokenIssuer serviceIssuer{tokenSecret, ::morph::session::hmacSha256};
    ::morph::session::Context session;
    session.principal = std::string{auth::kMetadataFetcherPrincipal};
    session.token = serviceIssuer.issue(::morph::session::SessionToken{
        .principal = std::string{auth::kMetadataFetcherPrincipal},
        .issuedAtMs = 0,
        .expiresAtMs = kServiceTokenExpiresAtMs,
        .roles = {},
    });
    _fetchBridge.setDefaultSession(session);

    // Both timer slots are wrapped rather than connected to the methods
    // directly. An exception escaping a Qt slot is unsupported — Qt's event
    // dispatcher propagates it out of `exec()` at best and calls
    // `std::terminate` at worst — so a background pass that throws would take
    // the whole server process down with it, taking every connected client's
    // session with it, for a failure that only ever concerns one pass.
    // Neither body is exception-free: `fetchMetadataOnce()` constructs a
    // `BridgeHandler`, which throws if the register is refused (reachable
    // here, because this server caps `maxLiveModels`), and
    // `relayOutboxOnce()` can throw from its `Query<>()` or from the action
    // log's own sink. Logging and dropping the pass is the right response to
    // both: the next tick simply retries, since neither pass consumes the
    // work it failed on. The public methods themselves keep throwing, so a
    // test that calls one directly still sees the failure.
    connect(&_fetchTimer, &QTimer::timeout, this, [this] {
        try {
            fetchMetadataOnce();
        } catch (const std::exception& e) {
            ::morph::log::logError(std::string{"[bookmarks::App] metadata-fetch pass threw, pass abandoned: "} +
                                   e.what());
        } catch (...) {
            ::morph::log::logError("[bookmarks::App] metadata-fetch pass threw a non-std exception, pass abandoned");
        }
    });
    _fetchTimer.start(fetchInterval);
    connect(&_relayTimer, &QTimer::timeout, this, [this] {
        try {
            (void) relayOutboxOnce();
        } catch (const std::exception& e) {
            ::morph::log::logError(std::string{"[bookmarks::App] outbox-relay pass threw, pass abandoned: "} +
                                   e.what());
        } catch (...) {
            ::morph::log::logError("[bookmarks::App] outbox-relay pass threw a non-std exception, pass abandoned");
        }
    });
    _relayTimer.start(relayInterval);
}

void App::stopBackgroundJobs() {
    _fetchTimer.stop();
    _relayTimer.stop();
}

App::~App() {
    // Stop first: a tick landing while the members below are being torn down
    // would dispatch a pass into a half-destroyed App. A shutting-down owner
    // will normally have called stopBackgroundJobs() already, before its own
    // drain loop started pumping (see that method's doc comment); calling it
    // again here is a no-op, and keeps this destructor correct for every owner
    // that does not.
    stopBackgroundJobs();
    ::morph::journal::setActionLog(nullptr);
    // Matches setActionLog's own clear-on-destruction discipline: a later
    // test (or a second App in the same process) must see
    // auth::tokenIssuer() == nullptr rather than a previous App's still-live
    // issuer, which would be holding a *different* secret than whatever
    // authorizer is current.
    auth::setTokenIssuer(nullptr);
}

void App::fetchMetadataOnce() {
    std::vector<std::pair<std::int64_t, std::string>> needsFetch;
    {
        ::Lightweight::SqlStatement stmt;
        stmt.Prepare("SELECT id, url FROM bookmarks WHERE title = ''");
        auto cursor = stmt.Execute();
        while (cursor.FetchRow()) {
            needsFetch.emplace_back(cursor.GetColumn<std::int64_t>(1), cursor.GetColumn<std::string>(2));
        }
    }
    if (needsFetch.empty()) {
        return;
    }

    // `handler` is kept alive by every dispatched call's own completion, not
    // by this function's stack frame — the identical pattern (and identical
    // race) pastebin::app::App::sweepExpiredOnce() documents at length.
    // `BridgeHandler::execute()` posts to the worker pool and returns
    // immediately, so this loop routinely returns before RemoteServer has so
    // much as looked up the model instance for the first dispatch. A
    // `handler` destroyed synchronously here would deregister its instance
    // (a synchronous "deregister" in ~BridgeHandler) and race those pending
    // dispatches, which would then find the instance missing and reply "model
    // not found" instead of ever running RecordMetadata — silently dropping
    // the pass. Capturing `handler` in every completion below closes that
    // window: the instance is released only once every dispatch this pass
    // issued has settled, whichever of .then()/.onError() that turns out to
    // be for each.
    //
    // Constructing it here (per pass) rather than once in the constructor is
    // also what keeps an idle server from holding a live model instance
    // against `maxLiveModels` between passes.
    auto handler = std::make_shared<::morph::bridge::BridgeHandler<BookmarkModel>>(_fetchBridge, &_fetchExecutor);
    // Captured by value, never through `this`: the callbacks below can
    // outlive this App (see fetchInFlight()'s doc comment), and a late one
    // must still be able to decrement the counter safely.
    auto inFlight = _fetchInFlight;
    for (const auto& [id, url] : needsFetch) {
        // Synchronous by design — see metadata_fetcher.hpp.
        const auto metadata = _fetcher->fetch(url);
        if (metadata.title.empty() && metadata.faviconPath.empty()) {
            // Nothing was found. Dispatching anyway would be a write with no
            // content: RecordMetadata ignores empty fields but still stamps
            // `updated_at_ms`, which would show up as a spurious change in
            // every client's GetChangesSince poll on every pass — and with
            // the shipped NullMetadataFetcher, that is *every* untitled
            // bookmark on *every* tick, forever. The bookmark stays in the
            // "needs fetch" set and is retried next pass, which is the
            // correct outcome for a fetch that found nothing.
            continue;
        }
        // The raise has to precede the dispatch — a completion delivered from
        // a worker thread could otherwise lower a count this loop had not
        // raised yet — which leaves a window the `catch` below closes.
        inFlight->fetch_add(1);
        try {
            handler
                ->execute(RecordMetadata{.id = BookmarkId{id},
                                         .title = metadata.title,
                                         .faviconPath = metadata.faviconPath})
                .then([handler, inFlight](Ack) { inFlight->fetch_sub(1); })
                .onError([handler, inFlight, id](const std::exception_ptr&) {
                    inFlight->fetch_sub(1);
                    ::morph::log::logError("[bookmarks::App] metadata fetch: RecordMetadata failed for bookmark " +
                                           std::to_string(id));
                });
        } catch (const std::exception& e) {
            // `execute()` threw instead of returning a `Completion`, so
            // neither callback above was ever attached and nothing else will
            // ever lower the count the line above raised. Leaving it raised
            // wedges `fetchInFlight()` at `true` permanently, and with it
            // every consumer that drains on it — `server/main.cpp`'s
            // `drainMetadataFetches` would then burn its whole 5s budget on
            // every subsequent shutdown and still report failure.
            inFlight->fetch_sub(1);
            ::morph::log::logError("[bookmarks::App] metadata fetch: dispatch for bookmark " + std::to_string(id) +
                                   " threw: " + e.what());
        }
    }
}

std::size_t App::relayOutboxOnce() {
    ::Lightweight::DataMapper mapper;
    ::morph::journal::OutboxRelay relay;
    relay.drainOutbox = [&mapper] {
        auto rows = mapper.Query<db::BookmarkOutboxRecord>().All();
        std::vector<::morph::journal::LogEntry> entries;
        entries.reserve(rows.size());
        for (const auto& row : rows) {
            ::morph::journal::LogEntry entry;
            entry.modelType = row.modelType.Value();
            entry.entityKey = row.entityKey.Value();
            entry.actionType = row.actionType.Value();
            entry.payload = row.payload.Value();
            entry.result = row.result.Value();
            entry.principal = row.principal.Value();
            entry.timestampMs = row.timestampMs.Value();
            entry.idempotencyKey = row.idempotencyKey.Value();
            entries.push_back(std::move(entry));
        }
        return entries;
    };
    // Deleting the row rather than flagging it is what outbox_entity.hpp's
    // own doc comment specifies: the table then only ever holds genuinely
    // unrelayed work. OutboxRelay calls this only after `sink->flush()`
    // returned normally, so a crash before this point simply re-drains the
    // same rows next pass and the sink's idempotencyKey dedup absorbs the
    // repeat (`FileActionLog` does this out of the box).
    relay.markRelayed = [&mapper](std::span<const ::morph::journal::LogEntry> rows) {
        for (const auto& row : rows) {
            ::Lightweight::SqlStatement stmt{mapper.Connection()};
            stmt.Prepare("DELETE FROM bookmark_outbox WHERE idempotency_key = ?");
            (void) stmt.Execute(row.idempotencyKey);
        }
    };
    relay.sink = _actionLog;
    return relay.relay().relayed;
}

}  // namespace bookmarks::app

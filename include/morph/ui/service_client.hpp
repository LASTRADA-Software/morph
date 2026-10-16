// SPDX-License-Identifier: Apache-2.0

#pragma once
#include <algorithm>
#include <cstddef>
#include <exception>
#include <expected>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "../attributes.hpp"
#include "../core/completion.hpp"
#include "../core/envelope_transport.hpp"
#include "../core/executor.hpp"
#include "../core/wire.hpp"
#include "../session/session.hpp"
#include "../ui/frontend.hpp"
#include "bundle.hpp"
#include "file_store.hpp"

/// @file
/// @brief The client side of the UI service (spec 5 §12, "Content addressing"): `UiClient` runs `ui-hello`,
///        `ui-catalog` and `ui-fetch` over a transport the caller supplies, verifies every file against the manifest
///        and the manifest against the `ui-hello` digest, and keeps what it verified in a `VerifiedCache`.
///
/// Specified in `docs/spec/ui/service.md`.

namespace morph::ui {

/// @brief Where a client keeps verified files between runs, by content hash, and the digest of each application's
///        last verified bundle.
///
/// A store is not trusted: `VerifiedCache` hashes what it reads back. A client calls it from whichever thread its
/// exchange calls back on, and runs of several `open`s may overlap, so an implementation is safe to call from any
/// thread.
class IBundleStore {
public:
    IBundleStore() = default;
    virtual ~IBundleStore() = default;
    IBundleStore(IBundleStore const&) = delete;
    IBundleStore& operator=(IBundleStore const&) = delete;
    IBundleStore(IBundleStore&&) = delete;
    IBundleStore& operator=(IBundleStore&&) = delete;

    /// @brief Reads a file.
    /// @param hash The file's content hash.
    /// @return Its bytes, or nothing when the store has none.
    [[nodiscard]] virtual std::optional<std::string> get(std::string_view hash) const = 0;

    /// @brief Keeps a file.
    /// @param hash  The file's content hash.
    /// @param bytes The file.
    // NOLINTNEXTLINE(bugprone-easily-swappable-parameters): the key, then the value.
    virtual void put(std::string const& hash, std::string bytes) = 0;

    /// @brief Drops a file.
    /// @param hash The file's content hash.
    virtual void remove(std::string_view hash) = 0;

    /// @brief The digest of an application's last verified bundle.
    /// @param application The application's id.
    /// @return The digest, or nothing.
    [[nodiscard]] virtual std::optional<std::string> lastDigest(std::string_view application) const = 0;

    /// @brief Records an application's last verified bundle.
    /// @param application The application's id.
    /// @param digest      The manifest's content hash.
    // NOLINTNEXTLINE(bugprone-easily-swappable-parameters): the key, then the value.
    virtual void setLastDigest(std::string const& application, std::string digest) = 0;
};

/// @brief An `IBundleStore` in memory: it lasts as long as the process. Safe to call from any thread.
class MemoryBundleStore final : public IBundleStore {
public:
    /// @brief Reads a file.
    /// @param hash The file's content hash.
    /// @return Its bytes, or nothing.
    [[nodiscard]] std::optional<std::string> get(std::string_view hash) const override {
        std::scoped_lock const lock{_mutex};
        auto const found = _files.find(hash);
        if (found == _files.end()) {
            return std::nullopt;
        }
        return found->second;
    }

    /// @brief Keeps a file.
    /// @param hash  The file's content hash.
    /// @param bytes The file.
    // NOLINTNEXTLINE(bugprone-easily-swappable-parameters): the key, then the value.
    void put(std::string const& hash, std::string bytes) override {
        std::scoped_lock const lock{_mutex};
        _files.insert_or_assign(hash, std::move(bytes));
    }

    /// @brief Drops a file.
    /// @param hash The file's content hash.
    void remove(std::string_view hash) override {
        std::scoped_lock const lock{_mutex};
        if (auto const found = _files.find(hash); found != _files.end()) {
            _files.erase(found);
        }
    }

    /// @brief The digest of an application's last verified bundle.
    /// @param application The application's id.
    /// @return The digest, or nothing.
    [[nodiscard]] std::optional<std::string> lastDigest(std::string_view application) const override {
        std::scoped_lock const lock{_mutex};
        auto const found = _digests.find(application);
        if (found == _digests.end()) {
            return std::nullopt;
        }
        return found->second;
    }

    /// @brief Records an application's last verified bundle.
    /// @param application The application's id.
    /// @param digest      The manifest's content hash.
    // NOLINTNEXTLINE(bugprone-easily-swappable-parameters): the key, then the value.
    void setLastDigest(std::string const& application, std::string digest) override {
        std::scoped_lock const lock{_mutex};
        _digests.insert_or_assign(application, std::move(digest));
    }

    /// @brief How many files the store holds.
    /// @return The count.
    [[nodiscard]] std::size_t size() const {
        std::scoped_lock const lock{_mutex};
        return _files.size();
    }

private:
    mutable std::mutex _mutex;
    std::map<std::string, std::string, std::less<>> _files;
    std::map<std::string, std::string, std::less<>> _digests;
};

/// @brief A content-addressed cache that hands out only bytes that hash to the hash they are asked for.
class VerifiedCache {
public:
    /// @brief A cache over @p store.
    /// @param store The store; shared with the caller.
    /// @throws std::invalid_argument if @p store is null.
    explicit VerifiedCache(std::shared_ptr<IBundleStore> store) : _store{std::move(store)} {
        if (!_store) {
            throw std::invalid_argument("VerifiedCache: the store is null");
        }
    }

    /// @brief Reads a file, verified. A stored file that does not hash to @p hash is dropped and reads as a miss.
    /// @param hash The content hash.
    /// @return The bytes, or nothing.
    [[nodiscard]] std::optional<std::string> get(std::string const& hash) {
        auto bytes = _store->get(hash);
        if (bytes && contentHash(*bytes) != hash) {
            _store->remove(hash);
            return std::nullopt;
        }
        return bytes;
    }

    /// @brief Keeps a file under its content hash.
    /// @param bytes The bytes.
    /// @return The hash they are kept under.
    std::string put(std::string bytes) {
        std::string hash = contentHash(bytes);
        _store->put(hash, std::move(bytes));
        return hash;
    }

    /// @brief The store.
    /// @return It.
    [[nodiscard]] IBundleStore& store() noexcept { return *_store; }

private:
    std::shared_ptr<IBundleStore> _store;
};

/// @brief A bundle the client verified: what the shell mounts, and what it was verified against.
struct LoadedBundle {
    /// @brief The application's id, the manifest digest, and the documents (the screens it may open and the app
    ///        document) by id.
    Bundle bundle;
    /// @brief The `ui-hello` reply; empty when the bundle was opened offline.
    HelloReply hello;
    /// @brief The manifest.
    Manifest manifest;
    /// @brief The catalog for the principal; empty when the bundle was opened offline.
    CatalogReply catalog;
    /// @brief Every verified file the bundle holds, by content hash: documents, themes, message catalogues and,
    ///        for a client that loads them, components.
    std::map<std::string, std::string, std::less<>> files;
};

/// @brief One reply, or why the transport could not deliver it.
using ExchangeReply = std::expected<::morph::wire::Envelope, std::string>;

/// @brief Sends one request and calls its callback once with the reply. The transport assigns the request's `callId`
///        and matches the reply to it; a failure to deliver either is the error.
using Exchange = std::function<void(::morph::wire::Envelope request, std::function<void(ExchangeReply reply)> done)>;

/// @brief An `Exchange` over a transport: each request is one `IEnvelopeTransport::request`, and its reply, `ok` or
///        `err`, is the exchange's reply; a failure to deliver either is the error, as its message.
///
/// Call it on @p cbExec's thread, where its replies arrive: `UiClient` sends each request from the reply before it,
/// so a client opened there stays there.
/// @param backend The transport: `SocketBackend` or `QtWebSocketBackend`. Borrowed: it must outlive every request
///                sent through the exchange, so a caller that may destroy it first guards the exchange itself.
/// @param cbExec  The executor replies are delivered on: the interpreter runtime's owner. Borrowed.
/// @return The exchange.
[[nodiscard]] inline Exchange exchangeOver(::morph::backend::IEnvelopeTransport& backend MORPH_LIFETIMEBOUND,
                                           ::morph::exec::IExecutor& cbExec MORPH_LIFETIMEBOUND) {
    return [transport = &backend, owner = &cbExec](::morph::wire::Envelope request,
                                                   std::function<void(ExchangeReply reply)> done) {
        auto const answer = std::make_shared<std::function<void(ExchangeReply reply)>>(std::move(done));
        transport->request(std::move(request), *owner)
            .thenDetached([answer](::morph::wire::Envelope const& reply) { (*answer)(reply); })
            .onErrorDetached([answer](std::exception_ptr const& failure) {
                std::string message = "the request was not delivered";
                try {
                    std::rethrow_exception(failure);
                } catch (std::exception const& error) {
                    message = error.what();
                } catch (...) {  // NOLINT(bugprone-empty-catch): the default message stands for a non-std failure
                }
                (*answer)(std::unexpected(std::move(message)));
            });
    };
}

/// @brief Opens an application's bundle through the UI service, verifying everything it fetches.
///
/// `open` runs `ui-hello`, refuses a bundle that needs a vocabulary the client does not speak, takes the manifest from
/// the cache or fetches it by the digest and verifies it, runs `ui-catalog`, and fetches the files the principal may
/// use that the cache does not hold, all at once, verifying each. A file that does not hash to its manifest entry,
/// or a manifest that does not hash to the digest, refuses the whole bundle (`Unverified`). An unchanged manifest
/// fetches nothing.
class UiClient {
public:
    /// @brief A client.
    /// @param exchange The transport.
    /// @param client   What the client speaks and carries, sent with `ui-hello`.
    /// @param store    Where verified files are kept.
    /// @throws std::invalid_argument if @p exchange is empty or @p store is null.
    UiClient(Exchange exchange, HelloRequest client, std::shared_ptr<IBundleStore> store)
        : _shared{std::make_shared<Shared>(std::move(exchange), std::move(client), VerifiedCache{std::move(store)})} {
        if (!_shared->exchange) {
            throw std::invalid_argument("UiClient: the exchange is empty");
        }
    }

    /// @brief What `open` delivers.
    using Done = std::function<void(std::expected<LoadedBundle, ConnectError>)>;

    /// @brief Opens the bundle. @p done is called once, from whichever thread the exchange calls back on; a
    ///        synchronous exchange calls it before `open` returns.
    /// @param session The session every request carries: the principal's token, or none before sign-in.
    /// @param done    Called with the bundle, or why there is none.
    void open(::morph::session::Context session, Done done) const {
        auto const run = std::make_shared<Run>(_shared, std::move(session), std::move(done));
        sendHello(run);
    }

    /// @brief What `poll` delivers: the changed bundle, nothing when the digest is the one already held, or why
    ///        there is none.
    using PollDone = std::function<void(std::expected<std::optional<LoadedBundle>, ConnectError>)>;

    /// @brief One step of a development client's reload (spec 5 §12): sends `ui-hello`, and when the manifest digest
    ///        differs from @p knownDigest, opens the changed bundle as `open` does, fetching only what the cache
    ///        does not hold. @p done is called once, as `open`'s is.
    /// @param session     The session every request carries.
    /// @param knownDigest The digest of the bundle the client has, as `HelloReply::digest` spells it.
    /// @param done        Called with the changed bundle, with nothing when the digest is unchanged, or with why the
    ///                    poll failed.
    void poll(::morph::session::Context session, std::string knownDigest, PollDone done) const {
        auto const answer = std::make_shared<PollDone>(std::move(done));
        auto const run = std::make_shared<Run>(_shared, std::move(session),
                                               [answer](std::expected<LoadedBundle, ConnectError> opened) {
                                                   if (!opened) {
                                                       (*answer)(std::unexpected(std::move(opened.error())));
                                                       return;
                                                   }
                                                   (*answer)(std::optional<LoadedBundle>{std::move(*opened)});
                                               });
        run->knownDigest = std::move(knownDigest);
        run->unchanged = [answer] { (*answer)(std::optional<LoadedBundle>{}); };
        sendHello(run);
    }

    /// @brief Rebuilds the last bundle this store verified for @p application, without a connection. Files the
    ///        store no longer holds, or that no longer verify, are left out; the catalog is empty.
    /// @param application The application's id.
    /// @return The bundle, or `Unreachable` when no verified manifest is cached.
    [[nodiscard]] std::expected<LoadedBundle, ConnectError> openOffline(std::string_view application) const {
        auto const digest = _shared->cache.store().lastDigest(application);
        if (!digest) {
            return std::unexpected(
                ConnectError{.kind = ConnectError::Kind::Unreachable,
                             .message = "no verified bundle of `" + std::string{application} + "` is cached"});
        }
        auto const text = _shared->cache.get(*digest);
        if (!text) {
            return std::unexpected(ConnectError{
                .kind = ConnectError::Kind::Unreachable,
                .message = "the cached manifest of `" + std::string{application} + "` is gone or does not verify"});
        }
        auto manifest = parseManifest(*text);
        if (!manifest) {
            return std::unexpected(ConnectError{.kind = ConnectError::Kind::Unsupported, .message = manifest.error()});
        }
        LoadedBundle loaded;
        loaded.manifest = std::move(*manifest);
        for (ManifestFile const& file : loaded.manifest.files) {
            if (!usable(_shared->client, file.kind)) {
                continue;
            }
            if (auto bytes = _shared->cache.get(file.hash)) {
                loaded.files.emplace(file.hash, std::move(*bytes));
            }
        }
        assemble(loaded, *digest);
        return loaded;
    }

private:
    /// @brief What the client and every run share; a run holds it, so a run outlives the client that started it.
    struct Shared {
        Shared(Exchange exchangeFn, HelloRequest clientInfo, VerifiedCache verified)
            : exchange{std::move(exchangeFn)}, client{std::move(clientInfo)}, cache{std::move(verified)} {}

        Exchange exchange;
        HelloRequest client;
        VerifiedCache cache;
    };

    /// @brief One `open`: its replies so far, and the files still in flight.
    ///
    /// The steps up to the catalog run one at a time. The fetches after it are in flight together, and a transport
    /// may answer them on different threads, so what they change is guarded by `mutex`.
    struct Run {
        Run(std::shared_ptr<Shared> sharedState, ::morph::session::Context sessionContext, Done doneFn)
            : shared{std::move(sharedState)}, session{std::move(sessionContext)}, done{std::move(doneFn)} {}

        std::shared_ptr<Shared> shared;
        ::morph::session::Context session;
        Done done;
        // A poll's: the digest it already has, and what it calls instead of `done` when the reply carries it.
        std::string knownDigest;
        std::function<void()> unchanged;
        LoadedBundle loaded;
        std::mutex mutex;
        std::size_t pending = 0;
        std::optional<ConnectError> failure;
    };

    /// @brief A file a run expects: its hash, and how a refusal names it.
    struct Expected {
        std::string hash;
        std::string what;
    };

    /// @brief Whether a client fetches files of @p kind: every kind it knows, components only when it loads them.
    [[nodiscard]] static bool usable(HelloRequest const& client, std::string_view kind) {
        if (kind == kComponentFile) {
            return client.components;
        }
        return kind == kScreenFile || kind == kAppFile || kind == kThemeFile || kind == kMessagesFile;
    }

    /// @brief Fills the bundle the shell reads from the verified files.
    static void assemble(LoadedBundle& loaded, std::string const& digest) {
        loaded.bundle.applicationId = loaded.manifest.application;
        loaded.bundle.manifestDigest = digest.substr(kHashPrefix.size());
        for (ManifestFile const& file : loaded.manifest.files) {
            if (file.kind != kScreenFile && file.kind != kAppFile) {
                continue;
            }
            if (auto const found = loaded.files.find(file.hash); found != loaded.files.end()) {
                loaded.bundle.documents.insert_or_assign(file.id, found->second);
            }
        }
    }

    /// @brief A request of @p kind carrying the run's session.
    [[nodiscard]] static ::morph::wire::Envelope requestOf(std::shared_ptr<Run> const& run, std::string_view kind) {
        ::morph::wire::Envelope request;
        request.kind = std::string{kind};
        request.session = run->session;
        return request;
    }

    /// @brief Sends the `ui-hello`.
    static void sendHello(std::shared_ptr<Run> const& run) {
        ::morph::wire::Envelope request = requestOf(run, kHelloKind);
        request.body = detail::writeMessage(run->shared->client);
        run->shared->exchange(std::move(request), [run](ExchangeReply reply) { onHello(run, std::move(reply)); });
    }

    /// @brief Sends the `ui-catalog`.
    static void sendCatalog(std::shared_ptr<Run> const& run) {
        run->shared->exchange(requestOf(run, kCatalogKind),
                              [run](ExchangeReply reply) { onCatalog(run, std::move(reply)); });
    }

    /// @brief Sends a `ui-fetch` for @p hash.
    static void fetch(std::shared_ptr<Run> const& run, std::string const& hash,
                      std::function<void(ExchangeReply)> then) {
        ::morph::wire::Envelope request = requestOf(run, kFetchKind);
        request.body = detail::writeMessage(FetchRequest{.hash = hash});
        run->shared->exchange(std::move(request), std::move(then));
    }

    static void fail(std::shared_ptr<Run> const& run, ConnectError error) {
        run->done(std::unexpected(std::move(error)));
    }

    static void fail(std::shared_ptr<Run> const& run, ConnectError::Kind kind, std::string message) {
        fail(run, ConnectError{.kind = kind, .message = std::move(message)});
    }

    /// @brief The reply to a request, or the error it carries: a transport failure is `Unreachable`, an `err` is
    ///        `Refused` with the server's message.
    [[nodiscard]] static std::expected<std::string, ConnectError> bodyOf(ExchangeReply reply) {
        if (!reply) {
            return std::unexpected(ConnectError{.kind = ConnectError::Kind::Unreachable, .message = reply.error()});
        }
        if (reply->kind != "ok") {
            return std::unexpected(
                ConnectError{.kind = ConnectError::Kind::Refused, .message = std::move(reply->message)});
        }
        return std::move(reply->body);
    }

    /// @brief A fetched file, verified against its hash, or why it is refused.
    [[nodiscard]] static std::expected<std::string, ConnectError> verified(ExchangeReply reply,
                                                                           Expected const& expected) {
        auto body = bodyOf(std::move(reply));
        if (!body) {
            body.error().message = expected.what + ": " + body.error().message;
            return body;
        }
        if (body->size() > kMaxFileBytes) {
            return std::unexpected(ConnectError{
                .kind = ConnectError::Kind::Unverified,
                .message = expected.what + " is larger than " + std::to_string(kMaxFileBytes) + " bytes"});
        }
        if (contentHash(*body) != expected.hash) {
            return std::unexpected(ConnectError{.kind = ConnectError::Kind::Unverified,
                                                .message = expected.what + " does not match its hash"});
        }
        return body;
    }

    static void onHello(std::shared_ptr<Run> const& run, ExchangeReply reply) {
        auto body = bodyOf(std::move(reply));
        if (!body) {
            ConnectError error = std::move(body.error());
            if (error.message == "unknown envelope kind: " + std::string{kHelloKind}) {
                error.kind = ConnectError::Kind::NoUiService;
                error.message = "the server serves no UI";
            } else if (error.message.starts_with("missing vocabulary: ")) {
                error.kind = ConnectError::Kind::Unsupported;
            }
            fail(run, std::move(error));
            return;
        }
        auto hello = parseHelloReply(*body);
        if (!hello) {
            fail(run, ConnectError::Kind::Unsupported, std::move(hello.error()));
            return;
        }
        HelloRequest const& client = run->shared->client;
        for (std::string const& vocabulary : hello->vocabularies) {
            if (std::ranges::find(client.vocabularies, vocabulary) == client.vocabularies.end()) {
                fail(run, ConnectError::Kind::Unsupported,
                     "the application needs the vocabulary " + vocabulary + ", which this client does not speak");
                return;
            }
        }
        if (run->unchanged && hello->digest == run->knownDigest) {
            run->unchanged();
            return;
        }
        run->loaded.hello = std::move(*hello);
        std::string const& digest = run->loaded.hello.digest;
        if (auto cached = run->shared->cache.get(digest)) {
            onManifest(run, std::move(*cached));
            return;
        }
        fetch(run, digest, [run](ExchangeReply fetched) {
            auto text =
                verified(std::move(fetched), Expected{.hash = run->loaded.hello.digest, .what = "the manifest"});
            if (!text) {
                fail(run, std::move(text.error()));
                return;
            }
            onManifest(run, std::move(*text));
        });
    }

    static void onManifest(std::shared_ptr<Run> const& run, std::string text) {
        auto manifest = parseManifest(text);
        if (!manifest) {
            fail(run, ConnectError::Kind::Unsupported, std::move(manifest.error()));
            return;
        }
        static_cast<void>(run->shared->cache.put(std::move(text)));
        run->loaded.manifest = std::move(*manifest);
        sendCatalog(run);
    }

    static void onCatalog(std::shared_ptr<Run> const& run, ExchangeReply reply) {
        auto body = bodyOf(std::move(reply));
        if (!body) {
            fail(run, std::move(body.error()));
            return;
        }
        auto catalog = parseCatalogReply(*body);
        if (!catalog) {
            fail(run, ConnectError::Kind::Unsupported, std::move(catalog.error()));
            return;
        }
        run->loaded.catalog = std::move(*catalog);
        std::vector<Expected> missing;
        for (ManifestFile const& file : run->loaded.manifest.files) {
            if (!wanted(run->shared->client, run->loaded.catalog, file)) {
                continue;
            }
            if (auto cached = run->shared->cache.get(file.hash)) {
                run->loaded.files.insert_or_assign(file.hash, std::move(*cached));
            } else {
                missing.push_back(Expected{.hash = file.hash, .what = "the " + file.kind + " `" + file.id + "`"});
            }
        }
        // One count for each fetch and one for this loop, so a synchronous exchange that answers every fetch
        // before the next is sent cannot settle the run early.
        run->pending = missing.size() + 1;
        for (Expected& expected : missing) {
            std::string const hash = expected.hash;
            fetch(run, hash, [run, expected = std::move(expected)](ExchangeReply fetched) {
                auto bytes = verified(std::move(fetched), expected);
                {
                    std::scoped_lock const lock{run->mutex};
                    if (!bytes) {
                        if (!run->failure) {
                            run->failure = std::move(bytes.error());
                        }
                    } else {
                        static_cast<void>(run->shared->cache.put(*bytes));
                        run->loaded.files.insert_or_assign(expected.hash, std::move(*bytes));
                    }
                }
                settle(run);
            });
        }
        settle(run);
    }

    /// @brief Whether the run fetches @p file: a kind the client uses, and for a screen, one the catalog lists as
    ///        available or public, which is what the server serves this principal.
    [[nodiscard]] static bool wanted(HelloRequest const& client, CatalogReply const& catalog,
                                     ManifestFile const& file) {
        if (!usable(client, file.kind)) {
            return false;
        }
        if (file.kind != kScreenFile) {
            return true;
        }
        return std::ranges::any_of(catalog.entries, [&file](CatalogEntry const& entry) {
            return entry.id == file.id && (entry.available || entry.isPublic);
        });
    }

    /// @brief Counts one fetch, or the loop that sent them, as settled; the last delivers the run.
    static void settle(std::shared_ptr<Run> const& run) {
        {
            std::scoped_lock const lock{run->mutex};
            if (--run->pending != 0) {
                return;
            }
        }
        // Past the last count nothing else touches the run, so it is read without the lock.
        if (run->failure) {
            fail(run, std::move(*run->failure));
            return;
        }
        assemble(run->loaded, run->loaded.hello.digest);
        run->shared->cache.store().setLastDigest(run->loaded.hello.application, run->loaded.hello.digest);
        run->done(std::move(run->loaded));
    }

    std::shared_ptr<Shared> _shared;
};

/// @brief The value an `evaluate` query takes from a `ui-evaluate` reply: the evaluated body's members, with the field
///        errors under `errors` and the verdict under `ready`, where `errors.<field>` attaches each to its element.
///
/// An action field named `errors` or `ready` is shadowed by them.
/// @param reply The reply, or why the transport could not deliver it.
/// @return The value as JSON text, or why there is none: the transport's reason, the server's `err` message, or why
///         the reply body was refused.
[[nodiscard]] inline std::expected<std::string, std::string> evaluationValue(ExchangeReply reply) {
    if (!reply) {
        return std::unexpected(std::move(reply.error()));
    }
    if (reply->kind != "ok") {
        return std::unexpected(std::move(reply->message));
    }
    auto evaluated = parseEvaluateReply(reply->body);
    if (!evaluated) {
        return std::unexpected(std::move(evaluated.error()));
    }
    Json::object_t errors;
    for (auto const& [field, message] : evaluated->errors) {
        errors.insert_or_assign(field, Json(message));
    }
    auto& members = evaluated->body.get<Json::object_t>();
    members.insert_or_assign("errors", Json(std::move(errors)));
    members.insert_or_assign("ready", Json(evaluated->ready));
    auto text = glz::write_json(evaluated->body);
    if (!text) {
        return std::unexpected("the ui-evaluate reply's body cannot be written");
    }
    return std::move(*text);
}

/// @brief An evaluator over the UI service: each evaluation is one `ui-evaluate` request through @p exchange, and
///        resolves with `evaluationValue` of its reply. What `BridgeDispatcher` takes as its `Evaluator`.
/// @param exchange The transport.
/// @param session  The session every request carries.
/// @param cbExec   The executor the completion is delivered on: the interpreter runtime's owner. Borrowed.
/// @return The evaluator: the model's type id, the action id and its JSON body in, the query's value out.
[[nodiscard]] inline std::function<::morph::async::Completion<std::string>(std::string const&, std::string const&,
                                                                           std::string const&)>
evaluatorOver(Exchange exchange, ::morph::session::Context session, ::morph::exec::IExecutor* cbExec) {
    return [exchange = std::move(exchange), session = std::move(session), cbExec](
               std::string const& typeId, std::string const& actionId, std::string const& bodyJson) {
        auto [completion, promise] = ::morph::async::Completion<std::string>::makeSettleable(cbExec);
        auto settle = std::make_shared<decltype(promise)>(std::move(promise));
        ::morph::wire::Envelope request;
        request.kind = std::string{kEvaluateKind};
        request.session = session;
        request.modelType = typeId;
        request.actionType = actionId;
        request.body = bodyJson;
        exchange(std::move(request), [settle](ExchangeReply reply) {
            auto value = evaluationValue(std::move(reply));
            if (value) {
                settle->resolve(std::move(*value));
            } else {
                settle->reject(std::make_exception_ptr(std::runtime_error{value.error()}));
            }
        });
        return std::move(completion);
    };
}

/// @brief A file a download brought back.
struct DownloadedFile {
    /// @brief Its bytes.
    std::string bytes;
    /// @brief Its name.
    std::string name;
    /// @brief Its media type.
    std::string type;
};

/// @brief The client of the file side channel (`files/1`, spec 6 §18): uploads and downloads through `ui-file`, one
///        chunk of at most `kMaxFileChunkBytes` in flight at a time, reporting progress after each.
///
/// What it moves is bytes: reading the file the user picked is the platform's, and an application's
/// `InterpreterHost::upload` reads it and hands the bytes here.
class FileTransfer {
public:
    /// @brief A transfer over @p exchange, its requests carrying @p session.
    /// @param exchange The transport.
    /// @param session  The session every request carries.
    /// @throws std::invalid_argument for an empty @p exchange.
    FileTransfer(Exchange exchange, ::morph::session::Context session)
        : _shared{std::make_shared<Shared>(Shared{.exchange = std::move(exchange), .session = std::move(session)})} {
        if (!_shared->exchange) {
            throw std::invalid_argument("FileTransfer: the exchange is empty");
        }
    }

    /// @brief Uploads a file, authorised as the mutation that will reference it.
    /// @param info     Its name, type and size; `size` is taken from @p bytes.
    /// @param bytes    The file.
    /// @param model    The model type of the mutation that will reference it.
    /// @param action   That mutation's action.
    /// @param progress Called with 0 to 1 after each chunk; may be empty.
    /// @param done     Called once with the file's reference, or why the upload failed.
    void upload(FileInfo info, std::string bytes, std::string model, std::string action,
                std::function<void(double)> progress,
                std::function<void(std::expected<std::string, std::string>)> done) const {
        auto const run = std::make_shared<Upload>(Upload{.shared = _shared,
                                                         .bytes = std::move(bytes),
                                                         .upload = {},
                                                         .progress = std::move(progress),
                                                         .done = std::move(done)});
        FileRequest begin;
        begin.op = "begin";
        begin.name = std::move(info.name);
        begin.type = std::move(info.type);
        begin.size = run->bytes.size();
        begin.model = std::move(model);
        begin.action = std::move(action);
        send(*_shared, begin, [run](std::expected<FileReply, std::string> reply) {
            if (!reply || !reply->upload) {
                run->fail(reply ? std::string{"the ui-file begin reply has no upload id"} : std::move(reply.error()));
                return;
            }
            run->upload = std::move(*reply->upload);
            sendChunk(run, 0);
        });
    }

    /// @brief Downloads a file by its reference.
    /// @param ref      The reference a reply carried.
    /// @param progress Called with 0 to 1 after each chunk; may be empty.
    /// @param done     Called once with the file, or why the download failed.
    void download(std::string ref, std::function<void(double)> progress,
                  std::function<void(std::expected<DownloadedFile, std::string>)> done) const {
        auto const run = std::make_shared<Download>(Download{.shared = _shared,
                                                             .ref = std::move(ref),
                                                             .file = {},
                                                             .progress = std::move(progress),
                                                             .done = std::move(done)});
        readChunk(run);
    }

private:
    struct Shared {
        Exchange exchange;
        ::morph::session::Context session;
    };

    struct Upload {
        std::shared_ptr<Shared> shared;
        std::string bytes;
        std::string upload;
        std::function<void(double)> progress;
        std::function<void(std::expected<std::string, std::string>)> done;

        void fail(std::string reason) const {
            if (done) {
                done(std::unexpected(std::move(reason)));
            }
        }
    };

    struct Download {
        std::shared_ptr<Shared> shared;
        std::string ref;
        DownloadedFile file;
        std::function<void(double)> progress;
        std::function<void(std::expected<DownloadedFile, std::string>)> done;
    };

    /// Sends one `ui-file` request and hands its reply body, or why there is none, to @p then.
    static void send(Shared const& shared, FileRequest const& request,
                     std::function<void(std::expected<FileReply, std::string>)> then) {
        ::morph::wire::Envelope envelope;
        envelope.kind = std::string{kFileKind};
        envelope.session = shared.session;
        envelope.body = detail::writeMessage(request);
        shared.exchange(std::move(envelope), [then = std::move(then)](ExchangeReply reply) {
            if (!reply) {
                then(std::unexpected(std::move(reply.error())));
                return;
            }
            if (reply->kind != "ok") {
                then(std::unexpected(std::move(reply->message)));
                return;
            }
            then(parseFileReply(reply->body));
        });
    }

    /// Sends the chunk at @p offset, then the next, then `end`.
    static void sendChunk(std::shared_ptr<Upload> const& run, std::size_t offset) {
        if (offset >= run->bytes.size()) {
            FileRequest end;
            end.op = "end";
            end.upload = run->upload;
            send(*run->shared, end, [run](std::expected<FileReply, std::string> reply) {
                if (!reply || !reply->ref) {
                    run->fail(reply ? std::string{"the ui-file end reply has no reference"}
                                    : std::move(reply.error()));
                    return;
                }
                if (run->done) {
                    run->done(std::move(*reply->ref));
                }
            });
            return;
        }
        FileRequest chunk;
        chunk.op = "chunk";
        chunk.upload = run->upload;
        chunk.offset = offset;
        std::string_view const all{run->bytes};
        chunk.data = ::morph::session::detail::base64UrlEncode(all.substr(offset, kMaxFileChunkBytes));
        send(*run->shared, chunk, [run](std::expected<FileReply, std::string> reply) {
            if (!reply || !reply->received) {
                run->fail(reply ? std::string{"the ui-file chunk reply has no count"} : std::move(reply.error()));
                return;
            }
            if (run->progress) {
                run->progress(static_cast<double>(*reply->received) / static_cast<double>(run->bytes.size()));
            }
            sendChunk(run, *reply->received);
        });
    }

    /// Reads the next chunk, until the whole file has arrived.
    static void readChunk(std::shared_ptr<Download> const& run) {
        FileRequest read;
        read.op = "read";
        read.ref = run->ref;
        read.offset = run->file.bytes.size();
        send(*run->shared, read, [run](std::expected<FileReply, std::string> reply) {
            std::optional<std::string> bytes;
            if (reply && reply->data && reply->total) {
                bytes = ::morph::session::detail::base64UrlDecode(*reply->data);
            }
            if (!bytes) {
                if (run->done) {
                    run->done(std::unexpected(reply ? std::string{"the ui-file read reply is malformed"}
                                                    : std::move(reply.error())));
                }
                return;
            }
            run->file.bytes += *bytes;
            run->file.name = reply->name.value_or("");
            run->file.type = reply->type.value_or("");
            std::size_t const total = *reply->total;
            if (run->progress && total > 0) {
                run->progress(static_cast<double>(run->file.bytes.size()) / static_cast<double>(total));
            }
            if (run->file.bytes.size() >= total || bytes->empty()) {
                if (run->done) {
                    run->done(std::move(run->file));
                }
                return;
            }
            readChunk(run);
        });
    }

    std::shared_ptr<Shared> _shared;
};

}  // namespace morph::ui

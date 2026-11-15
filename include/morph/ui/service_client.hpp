// SPDX-License-Identifier: Apache-2.0

#pragma once
#include <algorithm>
#include <cstddef>
#include <exception>
#include <expected>
#include <functional>
#include <map>
#include <memory>
#include <morph/export.hpp>
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
#include "../core/logger.hpp"
#include "../core/wire.hpp"
#include "../session/session.hpp"
#include "../ui/frontend.hpp"
#include "bundle.hpp"
#include "file_store.hpp"
#include "theme.hpp"

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
class MORPH_API MemoryBundleStore final : public IBundleStore {
public:
    /// @brief Reads a file.
    /// @param hash The file's content hash.
    /// @return Its bytes, or nothing.
    [[nodiscard]] std::optional<std::string> get(std::string_view hash) const override;

    /// @brief Keeps a file.
    /// @param hash  The file's content hash.
    /// @param bytes The file.
    // NOLINTNEXTLINE(bugprone-easily-swappable-parameters): the key, then the value.
    void put(std::string const& hash, std::string bytes) override;

    /// @brief Drops a file.
    /// @param hash The file's content hash.
    void remove(std::string_view hash) override;

    /// @brief The digest of an application's last verified bundle.
    /// @param application The application's id.
    /// @return The digest, or nothing.
    [[nodiscard]] std::optional<std::string> lastDigest(std::string_view application) const override;

    /// @brief Records an application's last verified bundle.
    /// @param application The application's id.
    /// @param digest      The manifest's content hash.
    // NOLINTNEXTLINE(bugprone-easily-swappable-parameters): the key, then the value.
    void setLastDigest(std::string const& application, std::string digest) override;

    /// @brief How many files the store holds.
    /// @return The count.
    [[nodiscard]] std::size_t size() const;

private:
    mutable std::mutex _mutex;
    std::map<std::string, std::string, std::less<>> _files;
    std::map<std::string, std::string, std::less<>> _digests;
};

/// @brief A content-addressed cache that hands out only bytes that hash to the hash they are asked for.
class MORPH_API VerifiedCache {
public:
    /// @brief A cache over @p store.
    /// @param store The store; shared with the caller.
    /// @throws std::invalid_argument if @p store is null.
    explicit VerifiedCache(std::shared_ptr<IBundleStore> store);

    /// @brief Reads a file, verified. A stored file that does not hash to @p hash is dropped and reads as a miss.
    /// @param hash The content hash.
    /// @return The bytes, or nothing.
    [[nodiscard]] std::optional<std::string> get(std::string const& hash);

    /// @brief Keeps a file under its content hash.
    /// @param bytes The bytes.
    /// @return The hash they are kept under.
    std::string put(std::string bytes);

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
[[nodiscard]] MORPH_API Exchange exchangeOver(::morph::backend::IEnvelopeTransport& backend MORPH_LIFETIMEBOUND,
                                              ::morph::exec::IExecutor& cbExec MORPH_LIFETIMEBOUND);

/// @brief Opens an application's bundle through the UI service, verifying everything it fetches.
///
/// `open` runs `ui-hello`, refuses a bundle that needs a vocabulary the client does not speak, takes the manifest from
/// the cache or fetches it by the digest and verifies it, runs `ui-catalog`, and fetches the files the principal may
/// use that the cache does not hold, all at once, verifying each. A file that does not hash to its manifest entry,
/// or a manifest that does not hash to the digest, refuses the whole bundle (`Unverified`). An unchanged manifest
/// fetches nothing.
class MORPH_API UiClient {
public:
    /// @brief A client.
    /// @param exchange The transport.
    /// @param client   What the client speaks and carries, sent with `ui-hello`.
    /// @param store    Where verified files are kept.
    /// @throws std::invalid_argument if @p exchange is empty or @p store is null.
    UiClient(Exchange exchange, HelloRequest client, std::shared_ptr<IBundleStore> store);

    /// @brief What `open` delivers.
    using Done = std::function<void(std::expected<LoadedBundle, ConnectError>)>;

    /// @brief Opens the bundle. @p done is called once, from whichever thread the exchange calls back on; a
    ///        synchronous exchange calls it before `open` returns.
    /// @param session The session every request carries: the principal's token, or none before sign-in.
    /// @param done    Called with the bundle, or why there is none.
    void open(::morph::session::Context session, Done done) const;

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
    void poll(::morph::session::Context session, std::string knownDigest, PollDone done) const;

    /// @brief Rebuilds the last bundle this store verified for @p application, without a connection. Files the
    ///        store no longer holds, or that no longer verify, are left out; the catalog is empty.
    /// @param application The application's id.
    /// @return The bundle, or `Unreachable` when no verified manifest is cached.
    [[nodiscard]] std::expected<LoadedBundle, ConnectError> openOffline(std::string_view application) const;

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
    [[nodiscard]] static bool usable(HelloRequest const& client, std::string_view kind);

    /// @brief Fills the bundle the shell reads from the verified files.
    static void assemble(LoadedBundle& loaded, std::string const& digest);

    /// @brief A request of @p kind carrying the run's session.
    [[nodiscard]] static ::morph::wire::Envelope requestOf(std::shared_ptr<Run> const& run, std::string_view kind);

    /// @brief Sends the `ui-hello`.
    static void sendHello(std::shared_ptr<Run> const& run);

    /// @brief Sends the `ui-catalog`.
    static void sendCatalog(std::shared_ptr<Run> const& run);

    /// @brief Sends a `ui-fetch` for @p hash.
    static void fetch(std::shared_ptr<Run> const& run, std::string const& hash,
                      std::function<void(ExchangeReply)> then);

    static void fail(std::shared_ptr<Run> const& run, ConnectError error);

    static void fail(std::shared_ptr<Run> const& run, ConnectError::Kind kind, std::string message) {
        fail(run, ConnectError{.kind = kind, .message = std::move(message)});
    }

    /// @brief The reply to a request, or the error it carries: a transport failure is `Unreachable`, an `err` is
    ///        `Refused` with the server's message.
    [[nodiscard]] static std::expected<std::string, ConnectError> bodyOf(ExchangeReply reply);

    /// @brief A fetched file, verified against its hash, or why it is refused.
    [[nodiscard]] static std::expected<std::string, ConnectError> verified(ExchangeReply reply,
                                                                           Expected const& expected);

    static void onHello(std::shared_ptr<Run> const& run, ExchangeReply reply);

    static void onManifest(std::shared_ptr<Run> const& run, std::string text);

    static void onCatalog(std::shared_ptr<Run> const& run, ExchangeReply reply);

    /// @brief Whether the run fetches @p file: a kind the client uses, and for a screen, one the catalog lists as
    ///        available or public, which is what the server serves this principal.
    [[nodiscard]] static bool wanted(HelloRequest const& client, CatalogReply const& catalog,
                                     ManifestFile const& file);

    /// @brief Counts one fetch, or the loop that sent them, as settled; the last delivers the run.
    static void settle(std::shared_ptr<Run> const& run);

    std::shared_ptr<Shared> _shared;
};

/// @brief The value an `evaluate` query takes from a `ui-evaluate` reply: the evaluated body's members, with the field
///        errors under `errors` and the verdict under `ready`, where `errors.<field>` attaches each to its element.
///
/// An action field named `errors` or `ready` is shadowed by them.
/// @param reply The reply, or why the transport could not deliver it.
/// @return The value as JSON text, or why there is none: the transport's reason, the server's `err` message, or why
///         the reply body was refused.
[[nodiscard]] MORPH_API std::expected<std::string, std::string> evaluationValue(ExchangeReply reply);

/// @brief An evaluator over the UI service: each evaluation is one `ui-evaluate` request through @p exchange, and
///        resolves with `evaluationValue` of its reply. What `BridgeDispatcher` takes as its `Evaluator`.
/// @param exchange The transport.
/// @param session  The session every request carries.
/// @param cbExec   The executor the completion is delivered on: the interpreter runtime's owner. Borrowed.
/// @return The evaluator: the model's type id, the action id and its JSON body in, the query's value out.
[[nodiscard]] MORPH_API
    std::function<::morph::async::Completion<std::string>(std::string const&, std::string const&, std::string const&)>
    evaluatorOver(Exchange exchange, ::morph::session::Context session, ::morph::exec::IExecutor* cbExec);

/// @brief A watcher over the server (spec 8 §7): each `watch` or `unwatch` is one request of that kind through
///        @p exchange, carrying the session @p currentSession returns when it is sent. What `SessionOptions::watcher`
///        takes.
///
/// The session is read at each send, not fixed when the watcher is built, because a `watch` must carry whoever
/// is signed in now: the exchange does not stamp it, and the server refuses an unsigned one.
/// @param exchange       The transport.
/// @param currentSession The session to stamp on each request, read when it is sent.
/// @param cbExec         The executor the completion is delivered on: the interpreter runtime's owner. Borrowed.
/// @return The watcher: the kind and the `WatchBody` JSON in; the reply body out, or the transport's or the
///         server's reason as the failure.
[[nodiscard]] MORPH_API std::function<::morph::async::Completion<std::string>(std::string const&, std::string const&)>
watcherOver(Exchange exchange, std::function<::morph::session::Context()> currentSession,
            ::morph::exec::IExecutor* cbExec);

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
class MORPH_API FileTransfer {
public:
    /// @brief A transfer over @p exchange, its requests carrying @p session.
    /// @param exchange The transport.
    /// @param session  The session every request carries.
    /// @throws std::invalid_argument for an empty @p exchange.
    FileTransfer(Exchange exchange, ::morph::session::Context session);

    /// @brief Uploads a file, authorised as the mutation that will reference it.
    /// @param info     Its name, type and size; `size` is taken from @p bytes.
    /// @param bytes    The file.
    /// @param model    The model type of the mutation that will reference it.
    /// @param action   That mutation's action.
    /// @param progress Called with 0 to 1 after each chunk; may be empty.
    /// @param done     Called once with the file's reference, or why the upload failed.
    void upload(FileInfo info, std::string bytes, std::string model, std::string action,
                std::function<void(double)> progress,
                std::function<void(std::expected<std::string, std::string>)> done) const;

    /// @brief Downloads a file by its reference.
    /// @param ref      The reference a reply carried.
    /// @param progress Called with 0 to 1 after each chunk; may be empty.
    /// @param done     Called once with the file, or why the download failed.
    void download(std::string ref, std::function<void(double)> progress,
                  std::function<void(std::expected<DownloadedFile, std::string>)> done) const;

private:
    struct Shared {
        Exchange exchange;
        ::morph::session::Context session;
    };

    struct MORPH_API Upload {
        std::shared_ptr<Shared> shared;
        std::string bytes;
        std::string upload;
        std::function<void(double)> progress;
        std::function<void(std::expected<std::string, std::string>)> done;

        void fail(std::string reason) const;
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
                     std::function<void(std::expected<FileReply, std::string>)> then);

    /// Sends the chunk at @p offset, then the next, then `end`.
    static void sendChunk(std::shared_ptr<Upload> const& run, std::size_t offset);

    /// Reads the next chunk, until the whole file has arrived.
    static void readChunk(std::shared_ptr<Download> const& run);

    std::shared_ptr<Shared> _shared;
};

}  // namespace morph::ui

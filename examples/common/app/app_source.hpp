// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <chrono>
#include <expected>
#include <functional>
#include <map>
#include <memory>
#include <morph/core/backend.hpp>
#include <morph/core/bridge.hpp>
#include <morph/core/executor.hpp>
#include <morph/reactive/scheduler.hpp>
#include <morph/session/session.hpp>
#include <morph/ui/document.hpp>
#include <morph/ui/explorer.hpp>
#include <morph/ui/frontend.hpp>
#include <morph/ui/interpreter.hpp>
#include <morph/ui/queueing_dispatcher.hpp>
#include <morph/ui/service.hpp>
#include <morph/ui/service_client.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

/// @file
/// Where the generic client's screens and models come from: the command line it reads, a server (remote) or an
/// application linked in-process (local). Spec 4 §3.

namespace morph::examples {

/// @brief What the generic client reads from its command line.
struct ClientArgs {
    /// @brief `--server URL`: the server to fetch the screens from.
    std::optional<std::string> server;
    /// @brief `--app NAME`: the linked application to run in-process.
    std::optional<std::string> app;
    /// @brief `--db PATH`: the database a local application opens.
    std::string db;
    /// @brief `--user NAME`: the development principal a local application signs in.
    std::string user;
    /// @brief `--screen ID`: the screen mounted first; empty for the catalog's first.
    std::string screen;
    /// @brief `--exit-after-mount`: quit once the first screen mounted, with 0 when it mounted cleanly.
    bool exitAfterMount = false;
    /// @brief `--dev`, or `MORPH_UI_DEV=1`: development mode, which reloads the screens when the server's documents
    ///        change (spec 5 §12). A client built without the development tools ignores it.
    bool dev = false;
};

/// @brief Reads the client's options from @p args and removes them, leaving the rest for the frontend.
///
/// Each value option is `--name value` or `--name=value`, and the last one given wins. `args[0]` is the program
/// name, and nothing after a `--` is read.
/// @param args The command line; what was read is removed.
/// @return The options, or why they are refused: a value option without its value, or not exactly one of
///         `--server` and `--app`. @p args is unchanged on a refusal.
[[nodiscard]] std::expected<ClientArgs, std::string> readClientArgs(std::vector<std::string>& args);

/// @brief What a local application is set up with.
struct LocalSetup {
    /// @brief The database path, from `--db`; empty for the application's default.
    std::string db;
    /// @brief The development principal, from `--user`.
    std::string user;
};

/// @brief An application the client links and runs in-process.
struct LocalApp {
    /// @brief The name `--app` selects it by.
    std::string name;
    /// @brief Builds its bundle: the same function its server calls.
    std::function<std::expected<std::shared_ptr<ui::ServedBundle const>, ui::Refusal>()> bundle;
    /// @brief Run once before the first model call, with the client's options; empty for none.
    std::function<void(LocalSetup const&)> setUp;
    /// @brief The authorizer the application's own server checks requests with, for an in-process server to check
    ///        them the same way; empty for one that lets every request through. Only a test's simulated server calls
    ///        it.
    std::function<std::shared_ptr<session::IAuthorizer>()> serverAuthorizer;
};

/// @brief The vocabularies the generic client speaks.
[[nodiscard]] std::vector<std::string> clientVocabularies();

/// @brief The `ui-hello` the generic client opens with, and sends again on each poll: its vocabularies, and the
///        offer of server-sent events on the connection.
[[nodiscard]] ui::HelloRequest clientHello();

/// @brief An `AppSource` that also hands the shell what it mounts screens with, once it delivered a bundle.
///
/// Opened, it holds a bridge over its backend, owned by the context's executor, and the verified bundle with its
/// catalog. Everything it hands out is valid from a successful `ready` until `close`.
class ClientSource : public ui::AppSource {
public:
    /// @brief Where the screens' model calls go.
    /// @return The dispatcher over the source's bridge.
    /// @throws std::logic_error before a bundle was delivered.
    [[nodiscard]] ui::Dispatcher& dispatcher();

    /// @brief The catalog facts the screens load with.
    /// @return The facts of the catalog reply; empty before a bundle was delivered.
    [[nodiscard]] ui::Catalog catalog() const;

    /// @brief The screens the principal may open, in catalog order: each catalog entry that is available or public
    ///        and whose document the bundle holds.
    /// @return Their ids.
    [[nodiscard]] std::vector<std::string> screens() const;

    /// @brief Whether the catalog marks @p screenId public: a screen served before sign-in, such as a login screen.
    /// @param screenId The screen.
    /// @return Whether it is listed and public.
    [[nodiscard]] bool isPublic(std::string_view screenId) const;

    /// @brief Opens the source: the transport, the bridge and the bundle, verified.
    /// @param context The running frontend's context.
    /// @param ready   Called once, on the owner, unless `close` comes first.
    void open(ui::AppContext& context, std::function<void(std::expected<ui::Bundle, ui::ConnectError>)> ready) final;

    /// @brief Refused: a switch between backends is the hosting layer's (spec 6 §9).
    /// @param backend Ignored.
    /// @param done    Called with the refusal.
    void switchBackend(ui::Backend backend, std::function<void(std::expected<void, std::string>)> done) final;

    /// @brief Drops a `ready` not yet delivered and releases the bridge, the transport and the bundle.
    void close() final;

    /// @brief Installs a sign-in (spec 6 §5): a session built from the current one with @p principal and @p token
    ///        replaced, carried by every later model call and UI-service request, then the catalog reloaded for it.
    /// @param principal The principal the sign-in mutation answered.
    /// @param token     The session token it answered; empty when the source's server needs none, as in process.
    /// @param done      Called once, on the owner, once the catalog for the new session is current, with whether it
    ///                  loaded; not called after `close`.
    void signIn(std::string principal, std::string token, std::function<void(bool)> done);

    /// @brief Ends the session: back to the principal before any sign-in, with no token, and the catalog reloaded
    ///        for it.
    /// @param done As `signIn`'s.
    void signOut(std::function<void(bool)> done);

    /// @brief The bundle the catalog currently lists.
    /// @return It; null before a bundle was delivered.
    [[nodiscard]] ui::Bundle const* bundle() const noexcept { return _loaded ? &_loaded->bundle : nullptr; }

    /// @brief The catalog for the current principal: its entries, what the principal may run, and the facts.
    /// @return It; null before a bundle was delivered.
    [[nodiscard]] ui::CatalogReply const* catalogReply() const noexcept {
        return _loaded ? &_loaded->catalog : nullptr;
    }

    /// @brief The `schemas` document of every model the catalog names, by model type, or why it did not arrive: what
    ///        the explorer lists each model's actions from.
    /// @return Them; empty before a bundle was delivered.
    [[nodiscard]] ui::SchemasByModel const& schemasByModel() const noexcept { return _byModel; }

    /// @brief The principal the source's session carries now.
    /// @return It; empty for none.
    [[nodiscard]] std::string const& principal() const noexcept { return _session.principal; }

    /// @brief The file side channel (`files/1`) over the source's exchange, carrying its session: what a screen's
    ///        `upload` and `download` move bytes through.
    /// @return It, or nothing before the source is open.
    [[nodiscard]] std::optional<ui::FileTransfer> files() const;

    /// @brief One step of development reload (spec 5 §12): sends `ui-hello`, and when the manifest digest changed,
    ///        fetches the changed bundle and the catalog for it. Nothing is sent while a poll is still in flight.
    /// @param changed Called on the owner once the changed bundle is current, with the ids of the documents it
    ///                changed; not called for an unchanged digest, a failed poll, or after `close`.
    void poll(std::function<void(std::vector<std::string> changed)> changed);

    /// @brief Adds a listener called on the owner after each successful connect, the first included. On a reconnect
    ///        the source first sends `ui-hello` again, so the new connection negotiates events, then calls the
    ///        listeners.
    /// @param listener Called on the owner; it must stay callable while the source is open.
    void onConnected(std::function<void()> const& listener) final;

    /// @brief Adds a listener for the events the server sends: what a shell hands to `Session::deliverEvent`.
    /// @param listener Called on the owner, once per event, in the order they arrived.
    void onEvent(std::function<void(wire::EventBody const&)> listener);

    /// @brief What the session's `SessionOptions::watcher` sends the cache's watch changes through: `watcherOver` the
    ///        source's exchange, stamping the session current at each send. Empty for a source whose transport
    ///        carries no server events, such as an in-process one.
    /// @return The watcher, or empty.
    [[nodiscard]] ui::Watcher watcher();

    /// @brief The principal before any sign-in, which `signOut` returns to.
    /// @return It; empty for none.
    [[nodiscard]] virtual std::string signedOutPrincipal() const { return {}; }

protected:
    /// @brief What a concrete source opens with: the bridge's backend and the exchange the UI service is reached by.
    struct Transport {
        /// @brief The backend; owned by the bridge from here on.
        std::unique_ptr<backend::detail::IBackend> backend;
        /// @brief The exchange; it may borrow the backend, which the source guards.
        ui::Exchange exchange;
        /// @brief Whether the server behind the transport sends events and handles `watch`: true for a server, false
        ///        in process.
        bool serverEvents = false;
    };

    /// @brief Makes the transport on @p context.
    /// @param context The running frontend's context.
    /// @return The transport, or why there is none.
    [[nodiscard]] virtual std::expected<Transport, ui::ConnectError> connect(ui::AppContext& context) = 0;

    /// @brief How long `open` waits for the bundle before it reports the source unreachable; zero waits forever.
    /// @return The bound.
    [[nodiscard]] virtual std::chrono::milliseconds openTimeout() const { return std::chrono::milliseconds{0}; }

    /// @brief Releases what `connect` made beside the backend; called by `close`, after the bridge is gone.
    virtual void disconnect() {}

    /// @brief Fetches the `{actionType: schema}` document of one model type, which a `form`, `collection` or
    ///        `wizard` node is built from. The default sends a `schemas` envelope over the source's exchange.
    /// @param modelType The model type id.
    /// @param done      Called once with the document's JSON text, or why there is none.
    virtual void fetchSchemas(std::string const& modelType,
                              std::function<void(std::expected<std::string, std::string>)> done);

    /// @brief The session every request carries, for a source to fill before `open` sends anything.
    /// @return It.
    [[nodiscard]] session::Context& requestSession() noexcept { return _session; }

private:
    session::Context _session;
    std::shared_ptr<bool> _alive;
    std::unique_ptr<bridge::Bridge> _bridge;
    std::unique_ptr<ui::BridgeDispatcher> _dispatcher;
    // The offline queue in front of the dispatcher: what a screen's calls go through.
    std::unique_ptr<ui::QueueingDispatcher> _queue;
    // Retries the queue while it holds anything: the transports reconnect on their own and tell no one.
    std::optional<reactive::TimerHandle> _replay;
    ui::Exchange _exchange;
    // Whether the open transport's server sends events and handles `watch`.
    bool _serverEvents = false;
    // Called after each connect, and with each event.
    std::vector<std::function<void()>> _connectListeners;
    std::vector<std::function<void(wire::EventBody const&)>> _eventListeners;
    // A connect, on the owner: `ui-hello` again once the first bundle loaded, then the listeners.
    void connected();
    // Fetches the schemas of every model type the catalog names, then calls @p done; nothing after `close`.
    void loadSchemas(std::shared_ptr<bool> const& alive, std::function<void()> done);
    // The model types the loaded catalog names: its models, and the model of each action it has facts for.
    [[nodiscard]] std::vector<std::string> modelTypes() const;
    // Records each action's schema of one `{actionType: schema}` document; a document that does not parse adds none.
    void recordSchemas(std::string const& document);
    // The schema of each action, by action id, fetched for every model type the catalog names.
    std::map<std::string, std::string, std::less<>> _schemas;
    // Each model's schemas document as it arrived, or why it did not.
    ui::SchemasByModel _byModel;
    std::optional<ui::LoadedBundle> _loaded;
    // The catalog the screens read through `catalog()`: replaced in place when a sign-in reloads it, so a session
    // built over the first catalog sees the facts of the current one.
    struct CurrentCatalog {
        ui::Catalog facts;
        std::map<std::string, std::string, std::less<>> schemas;
    };
    std::shared_ptr<CurrentCatalog> _current = std::make_shared<CurrentCatalog>();
    // Verified files, kept across reloads: a reload fetches only what the new catalog adds.
    std::shared_ptr<ui::IBundleStore> _store;
    // Fetches the catalog and the bundle for the current session, then the schemas; `done` once they are current.
    void reload(std::shared_ptr<bool> const& alive, std::function<void(std::expected<void, ui::ConnectError>)> done);
    reactive::TimerHandle _timeout;
    // Whether a development poll is in flight.
    bool _polling = false;
};

/// @brief Fetches the screens from a server (`--server`), over the transport the frontend calls for.
class RemoteAppSource final : public ClientSource {
public:
    /// @brief A source for @p url.
    /// @param url     `ws://host:port`.
    /// @param timeout How long to wait for the bundle before reporting the server unreachable.
    explicit RemoteAppSource(std::string url, std::chrono::milliseconds timeout = std::chrono::seconds{10});

protected:
    [[nodiscard]] std::expected<Transport, ui::ConnectError> connect(ui::AppContext& context) override;
    [[nodiscard]] std::chrono::milliseconds openTimeout() const override { return _timeout; }

private:
    std::string _url;
    std::chrono::milliseconds _timeout;
};

/// @brief Runs a linked application in-process (`--app`): its models over a `LocalBackend`, its bundle answered by
///        a `UiService` in the same process, and verified by the same client as a remote one.
class LocalAppSource final : public ClientSource {
public:
    /// @brief A source for @p app.
    /// @param app   The application.
    /// @param setup Its set-up options.
    LocalAppSource(LocalApp app, LocalSetup setup);
    ~LocalAppSource() override;
    LocalAppSource(LocalAppSource const&) = delete;
    LocalAppSource& operator=(LocalAppSource const&) = delete;
    LocalAppSource(LocalAppSource&&) = delete;
    LocalAppSource& operator=(LocalAppSource&&) = delete;

    /// @brief The development user, from `--user`.
    /// @return It.
    [[nodiscard]] std::string signedOutPrincipal() const override { return _setup.user; }

protected:
    [[nodiscard]] std::expected<Transport, ui::ConnectError> connect(ui::AppContext& context) override;
    void disconnect() override;
    void fetchSchemas(std::string const& modelType,
                      std::function<void(std::expected<std::string, std::string>)> done) override;

private:
    LocalApp _app;
    LocalSetup _setup;
    bool _setUp = false;
    std::unique_ptr<exec::ThreadPoolExecutor> _workers;
    std::shared_ptr<ui::UiService> _service;
};

/// @brief The source the client's arguments name: remote for `--server`, local for `--app`.
/// @param args The client's options.
/// @param apps The applications linked in-process.
/// @return The source, or why there is none: an `--app` that names no linked application.
[[nodiscard]] std::expected<std::unique_ptr<ClientSource>, std::string> makeSource(ClientArgs const& args,
                                                                                   std::vector<LocalApp> const& apps);

}  // namespace morph::examples

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
#include <morph/ui/frontend.hpp>
#include <morph/ui/interpreter.hpp>
#include <morph/ui/service.hpp>
#include <morph/ui/service_client.hpp>
#include <optional>
#include <string>
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
};

/// @brief The vocabularies the generic client speaks.
[[nodiscard]] std::vector<std::string> clientVocabularies();

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

protected:
    /// @brief What a concrete source opens with: the bridge's backend and the exchange the UI service is reached by.
    struct Transport {
        /// @brief The backend; owned by the bridge from here on.
        std::unique_ptr<backend::detail::IBackend> backend;
        /// @brief The exchange; it may borrow the backend, which the source guards.
        ui::Exchange exchange;
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
    ui::Exchange _exchange;
    // Fetches the schemas of every model type the catalog names, then calls @p done; nothing after `close`.
    void loadSchemas(std::shared_ptr<bool> const& alive, std::function<void()> done);
    // The schema of each action, by action id, fetched for every model type the catalog names.
    std::map<std::string, std::string, std::less<>> _schemas;
    std::optional<ui::LoadedBundle> _loaded;
    reactive::TimerHandle _timeout;
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

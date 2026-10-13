// SPDX-License-Identifier: Apache-2.0

#include "app/app_source.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <exception>
#include <expected>
#include <functional>
#include <memory>
#include <morph/core/wire.hpp>
#include <morph/session/session.hpp>
#include <morph/ui/bundle.hpp>
#include <morph/ui/document.hpp>
#include <ranges>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "app/transport.hpp"

namespace morph::examples {

namespace {

/// The value options, by name, and where each is read into.
struct ValueOption {
    std::string_view name;
    std::function<void(ClientArgs&, std::string)> store;
};

std::array<ValueOption, 5> const& valueOptions() {
    static std::array<ValueOption, 5> const options{{
        {"--server", [](ClientArgs& out, std::string value) { out.server = std::move(value); }},
        {"--app", [](ClientArgs& out, std::string value) { out.app = std::move(value); }},
        {"--db", [](ClientArgs& out, std::string value) { out.db = std::move(value); }},
        {"--user", [](ClientArgs& out, std::string value) { out.user = std::move(value); }},
        {"--screen", [](ClientArgs& out, std::string value) { out.screen = std::move(value); }},
    }};
    return options;
}

constexpr std::string_view kExitAfterMount = "--exit-after-mount";

}  // namespace

std::expected<ClientArgs, std::string> readClientArgs(std::vector<std::string>& args) {
    ClientArgs read;
    std::vector<std::size_t> used;
    for (std::size_t index = 1; index < args.size(); ++index) {
        std::string_view const arg = args.at(index);
        if (arg == "--") {
            break;
        }
        if (arg == kExitAfterMount) {
            read.exitAfterMount = true;
            used.push_back(index);
            continue;
        }
        for (ValueOption const& option : valueOptions()) {
            if (arg == option.name) {
                if (index + 1 >= args.size() || std::string_view{args.at(index + 1)}.starts_with("--")) {
                    return std::unexpected(std::string{option.name} + " needs a value");
                }
                option.store(read, args.at(index + 1));
                used.push_back(index);
                used.push_back(index + 1);
                ++index;
                break;
            }
            if (arg.starts_with(option.name) && arg.size() > option.name.size() && arg.at(option.name.size()) == '=') {
                std::string value{arg.substr(option.name.size() + 1)};
                if (value.empty()) {
                    return std::unexpected(std::string{option.name} + " needs a value");
                }
                option.store(read, std::move(value));
                used.push_back(index);
                break;
            }
        }
    }
    if (read.server.has_value() == read.app.has_value()) {
        return std::unexpected(std::string{"give exactly one of --server URL and --app NAME"});
    }
    for (std::size_t const index : used | std::views::reverse) {
        args.erase(args.begin() + static_cast<std::ptrdiff_t>(index));
    }
    return read;
}

std::vector<std::string> clientVocabularies() {
    return {std::string{ui::kVocabulary}, std::string{ui::kExprVocabulary}};
}

// ── ClientSource ────────────────────────────────────────────────────────────

ui::Dispatcher& ClientSource::dispatcher() {
    if (!_dispatcher || !_loaded) {
        throw std::logic_error{"ClientSource::dispatcher: the source has delivered no bundle"};
    }
    return *_dispatcher;
}

ui::Catalog ClientSource::catalog() const { return _loaded ? _loaded->catalog.facts() : ui::Catalog{}; }

std::vector<std::string> ClientSource::screens() const {
    std::vector<std::string> ids;
    if (!_loaded) {
        return ids;
    }
    for (ui::CatalogEntry const& entry : _loaded->catalog.entries) {
        if ((entry.available || entry.isPublic) && _loaded->bundle.documents.contains(entry.id)) {
            ids.push_back(entry.id);
        }
    }
    return ids;
}

void ClientSource::open(ui::AppContext& context,
                        std::function<void(std::expected<ui::Bundle, ui::ConnectError>)> ready) {
    close();
    auto const alive = std::make_shared<bool>(true);
    _alive = alive;
    auto transport = connect(context);
    if (!transport) {
        ready(std::unexpected(std::move(transport.error())));
        return;
    }
    exec::IExecutor& owner = context.executor();
    _bridge = std::make_unique<bridge::Bridge>(std::move(transport->backend), owner);
    _bridge->setDefaultSession(session);
    // A reply already queued when the source closes still reaches the exchange's continuation; from then on it sends
    // nothing, because the transport it borrows is gone with the bridge.
    ui::Exchange guarded = [alive, inner = std::move(transport->exchange)](
                               wire::Envelope request, std::function<void(ui::ExchangeReply)> done) {
        if (!*alive) {
            done(std::unexpected(std::string{"the source is closed"}));
            return;
        }
        inner(std::move(request), std::move(done));
    };
    _dispatcher =
        std::make_unique<ui::BridgeDispatcher>(*_bridge, &owner, ui::evaluatorOver(guarded, session, &owner));

    // Shared by the client's answer and the timeout: whichever comes first is delivered, once.
    auto const deliver = std::make_shared<std::function<void(std::expected<ui::LoadedBundle, ui::ConnectError>)>>(
        [this, alive, delivered = false,
         ready = std::move(ready)](std::expected<ui::LoadedBundle, ui::ConnectError> opened) mutable {
            if (!*alive || delivered) {
                return;
            }
            delivered = true;
            if (!opened) {
                ready(std::unexpected(std::move(opened.error())));
                return;
            }
            _loaded = std::move(*opened);
            ready(_loaded->bundle);
        });
    if (std::chrono::milliseconds const bound = openTimeout(); bound.count() > 0) {
        _timeout = context.scheduler().after(bound, [deliver, bound] {
            (*deliver)(std::unexpected(ui::ConnectError{
                .kind = ui::ConnectError::Kind::Unreachable,
                .message = "no answer from the server within " + std::to_string(bound.count()) + " ms"}));
        });
    }
    ui::HelloRequest hello;
    hello.vocabularies = clientVocabularies();
    ui::UiClient const client{std::move(guarded), std::move(hello), std::make_shared<ui::MemoryBundleStore>()};
    client.open(session, [deliver](std::expected<ui::LoadedBundle, ui::ConnectError> opened) {
        (*deliver)(std::move(opened));
    });
}

void ClientSource::switchBackend(ui::Backend backend, std::function<void(std::expected<void, std::string>)> done) {
    done(std::unexpected("this client does not switch backends; the switch to `" + backend.name +
                         "` is the hosting layer's"));
}

void ClientSource::close() {
    if (_alive) {
        *_alive = false;
    }
    _timeout = {};
    // The dispatcher and the bridge before what the concrete source made: the bridge's backend may run on it.
    _dispatcher.reset();
    _bridge.reset();
    _loaded.reset();
    disconnect();
}

// ── RemoteAppSource ─────────────────────────────────────────────────────────

RemoteAppSource::RemoteAppSource(std::string url, std::chrono::milliseconds timeout)
    : _url{std::move(url)}, _timeout{timeout} {}

std::expected<ClientSource::Transport, ui::ConnectError> RemoteAppSource::connect(ui::AppContext& context) {
    auto made = makeTransport(context, _url);
    if (!made) {
        return std::unexpected(ui::ConnectError{.kind = ui::ConnectError::Kind::Unreachable,
                                                .message = "cannot connect to " + _url + ": " + made.error()});
    }
    ui::Exchange exchange = ui::exchangeOver(*made->requests, context.executor());
    return Transport{.backend = std::move(made->backend), .exchange = std::move(exchange)};
}

// ── LocalAppSource ──────────────────────────────────────────────────────────

LocalAppSource::LocalAppSource(LocalApp app, LocalSetup setup) : _app{std::move(app)}, _setup{std::move(setup)} {
    session.principal = _setup.user;
}

LocalAppSource::~LocalAppSource() {
    // Here, not in the base: the bridge's LocalBackend runs on this class's workers, which go first otherwise.
    close();
}

std::expected<ClientSource::Transport, ui::ConnectError> LocalAppSource::connect(ui::AppContext& /*context*/) {
    try {
        if (!_setUp && _app.setUp) {
            _app.setUp(_setup);
        }
        _setUp = true;
    } catch (std::exception const& error) {
        return std::unexpected(ui::ConnectError{.kind = ui::ConnectError::Kind::Refused,
                                                .message = _app.name + " could not be set up: " + error.what()});
    }
    if (!_app.bundle) {
        return std::unexpected(ui::ConnectError{.kind = ui::ConnectError::Kind::NoUiService,
                                                .message = _app.name + " serves no screens"});
    }
    auto bundle = _app.bundle();
    if (!bundle) {
        return std::unexpected(ui::ConnectError{.kind = ui::ConnectError::Kind::Unsupported,
                                                .message = _app.name + ": " + bundle.error().message()});
    }
    _service = std::make_shared<ui::UiService>(std::move(*bundle));
    _workers = std::make_unique<exec::ThreadPoolExecutor>(2);
    // The UI service answers in process, synchronously, exactly as the server's strand would.
    ui::Exchange exchange = [service = _service, authorizer = session::allowAllAuthorizer()](
                                wire::Envelope request, std::function<void(ui::ExchangeReply)> const& done) {
        try {
            done(wire::decode(service->handle(request, *authorizer)));
        } catch (std::exception const& error) {
            done(std::unexpected(std::string{error.what()}));
        }
    };
    return Transport{.backend = std::make_unique<backend::LocalBackend>(*_workers), .exchange = std::move(exchange)};
}

void LocalAppSource::disconnect() {
    _workers.reset();
    _service.reset();
}

// ── makeSource ──────────────────────────────────────────────────────────────

std::expected<std::unique_ptr<ClientSource>, std::string> makeSource(ClientArgs const& args,
                                                                     std::vector<LocalApp> const& apps) {
    if (args.server) {
        return std::make_unique<RemoteAppSource>(*args.server);
    }
    std::string const name = args.app.value_or(std::string{});
    auto const found = std::ranges::find(apps, name, &LocalApp::name);
    if (found == apps.end()) {
        std::string linked;
        for (LocalApp const& app : apps) {
            linked += linked.empty() ? app.name : ", " + app.name;
        }
        return std::unexpected("no application named '" + name + "' is linked into this client (linked: " +
                               (linked.empty() ? std::string{"none"} : linked) + ")");
    }
    return std::make_unique<LocalAppSource>(*found, LocalSetup{.db = args.db, .user = args.user});
}

}  // namespace morph::examples

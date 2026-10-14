// SPDX-License-Identifier: Apache-2.0

#include "app/app_source.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <exception>
#include <expected>
#include <functional>
#include <map>
#include <memory>
#include <morph/core/registry.hpp>
#include <morph/core/wire.hpp>
#include <morph/session/session.hpp>
#include <morph/ui/bundle.hpp>
#include <morph/ui/document.hpp>
#include <morph/ui/file_store.hpp>
#include <morph/ui/service.hpp>
#include <morph/ui/service_client.hpp>
#include <optional>
#include <ranges>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
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
        {.name = "--server", .store = [](ClientArgs& out, std::string value) { out.server = std::move(value); }},
        {.name = "--app", .store = [](ClientArgs& out, std::string value) { out.app = std::move(value); }},
        {.name = "--db", .store = [](ClientArgs& out, std::string value) { out.db = std::move(value); }},
        {.name = "--user", .store = [](ClientArgs& out, std::string value) { out.user = std::move(value); }},
        {.name = "--screen", .store = [](ClientArgs& out, std::string value) { out.screen = std::move(value); }},
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

ui::Catalog ClientSource::catalog() const {
    if (!_loaded) {
        return ui::Catalog{};
    }
    ui::Catalog catalog = _loaded->catalog.facts();
    catalog.schema = [schemas = std::make_shared<std::map<std::string, std::string, std::less<>> const>(_schemas)](
                         std::string_view actionId) -> std::optional<std::string> {
        auto const found = schemas->find(actionId);
        if (found == schemas->end()) {
            return std::nullopt;
        }
        return found->second;
    };
    return catalog;
}

void ClientSource::fetchSchemas(std::string const& modelType,
                                std::function<void(std::expected<std::string, std::string>)> done) {
    if (!_exchange) {
        done(std::unexpected(std::string{"the source is not open"}));
        return;
    }
    wire::Envelope request = wire::makeSchemas(modelType);
    request.session = _session;
    _exchange(std::move(request), [done = std::move(done)](ui::ExchangeReply reply) {
        if (!reply) {
            done(std::unexpected(std::move(reply.error())));
        } else if (reply->kind != "ok") {
            done(std::unexpected(std::move(reply->message)));
        } else {
            done(std::move(reply->body));
        }
    });
}

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

std::vector<std::string> ClientSource::modelTypes() const {
    std::vector<std::string> types;
    if (_loaded && _loaded->catalog.models) {
        for (auto const& [type, info] : *_loaded->catalog.models) {
            static_cast<void>(info);
            types.push_back(type);
        }
    }
    return types;
}

void ClientSource::recordSchemas(std::string const& document) {
    auto parsed = ui::parseJson(document);
    if (!parsed) {
        return;
    }
    if (auto const* const actions = std::get_if<ui::Json::object_t>(&parsed->data)) {
        for (auto const& [action, schema] : *actions) {
            _schemas.insert_or_assign(action, glz::write_json(schema).value_or(std::string{}));
        }
    }
}

void ClientSource::loadSchemas(std::shared_ptr<bool> const& alive, std::function<void()> done) {
    _schemas.clear();
    std::vector<std::string> const types = modelTypes();
    if (types.empty()) {
        done();
        return;
    }
    // A type whose schemas do not arrive leaves its actions without one: their forms report it when they mount.
    auto const pending = std::make_shared<std::size_t>(types.size());
    auto const finished = std::make_shared<std::function<void()>>(std::move(done));
    for (std::string const& type : types) {
        fetchSchemas(type, [this, alive, pending, finished](std::expected<std::string, std::string> const& document) {
            if (!*alive) {
                return;
            }
            if (document) {
                recordSchemas(*document);
            }
            if (--*pending == 0) {
                (*finished)();
            }
        });
    }
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
    _bridge->setDefaultSession(_session);
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
        std::make_unique<ui::BridgeDispatcher>(*_bridge, &owner, ui::evaluatorOver(guarded, _session, &owner));
    _exchange = guarded;

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
            loadSchemas(alive, [this, alive, ready = std::move(ready)] {
                if (*alive) {
                    ready(_loaded->bundle);
                }
            });
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
    client.open(_session, [deliver](std::expected<ui::LoadedBundle, ui::ConnectError> opened) {
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

std::optional<ui::FileTransfer> ClientSource::files() const {
    if (!_exchange) {
        return std::nullopt;
    }
    return ui::FileTransfer{_exchange, _session};
}

bool ClientSource::adoptPrincipal(std::string principal) {
    _session.principal = std::move(principal);
    if (_bridge) {
        _bridge->setDefaultSession(_session);
    }
    return true;
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
    requestSession().principal = _setup.user;
}

LocalAppSource::~LocalAppSource() {
    // Here, not in the base: the bridge's LocalBackend runs on this class's workers, which go first otherwise.
    close();
}

void LocalAppSource::fetchSchemas(std::string const& modelType,
                                  std::function<void(std::expected<std::string, std::string>)> done) {
    // In process, the registry the local backend dispatches through answers directly, as a server's does.
    done(model::detail::defaultDispatcher().schemasJson(modelType));
}

std::expected<ClientSource::Transport, ui::ConnectError> LocalAppSource::connect(ui::AppContext& context) {
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
    // In process, the files a screen uploads are kept in memory for the run, as a server's store keeps them.
    _service = std::make_shared<ui::UiService>(std::move(*bundle), model::detail::defaultDispatcher(),
                                               std::make_shared<ui::MemoryFileStore>());
    // The UI service answers in process, synchronously, exactly as the server's strand would.
    ui::Exchange exchange = [service = _service, authorizer = session::allowAllAuthorizer()](
                                wire::Envelope const& request, std::function<void(ui::ExchangeReply)> const& done) {
        try {
            done(wire::decode(service->handle(request, *authorizer)));
        } catch (std::exception const& error) {
            done(std::unexpected(std::string{error.what()}));
        }
    };
#ifdef __EMSCRIPTEN__
    // A single-threaded build has no worker to run the models on: they run on the owner, between frames.
    return Transport{.backend = std::make_unique<backend::LocalBackend>(context.executor()),
                     .exchange = std::move(exchange)};
#else
    static_cast<void>(context);
    _workers = std::make_unique<exec::ThreadPoolExecutor>(2);
    return Transport{.backend = std::make_unique<backend::LocalBackend>(*_workers), .exchange = std::move(exchange)};
#endif
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

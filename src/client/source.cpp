// SPDX-License-Identifier: Apache-2.0

#include <algorithm>
#include <array>
#include <cstddef>
#include <exception>
#include <expected>
#include <functional>
#include <map>
#include <memory>
#include <morph/client/source.hpp>
#include <morph/client/transport.hpp>
#include <morph/core/registry.hpp>
#include <morph/core/wire.hpp>
#include <morph/session/session.hpp>
#include <morph/ui/bundle.hpp>
#include <morph/ui/dev_reload.hpp>
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

namespace morph::client {

namespace {

// How often the offline queue is retried while it holds a call.
constexpr std::chrono::seconds kReplayEvery{2};

}  // namespace

std::vector<std::string> clientVocabularies() {
    return {std::string{ui::kVocabulary}, std::string{ui::kExprVocabulary}};
}

ui::HelloRequest clientHello() {
    ui::HelloRequest hello;
    hello.vocabularies = clientVocabularies();
    hello.events = true;
    return hello;
}

// ── Source ────────────────────────────────────────────────────────────

ui::Dispatcher& Source::dispatcher() {
    if (!_queue || !_loaded) {
        throw std::logic_error{"Source::dispatcher: the source has delivered no bundle"};
    }
    return *_queue;
}

ui::Catalog Source::catalog() const {
    if (!_loaded) {
        return ui::Catalog{};
    }
    // Every fact reads the current catalog, so one taken before a sign-in follows the reload after it. A lookup the
    // catalog does not answer stays empty, which skips the rules that need it.
    std::shared_ptr<CurrentCatalog const> const current = _current;
    ui::Catalog catalog;
    if (current->facts.model) {
        catalog.model = [current](std::string_view typeId) {
            return current->facts.model ? current->facts.model(typeId) : std::nullopt;
        };
    }
    if (current->facts.action) {
        catalog.action = [current](std::string_view actionId) {
            return current->facts.action ? current->facts.action(actionId) : std::nullopt;
        };
    }
    if (current->facts.view) {
        catalog.view = [current](std::string_view actionId) {
            return current->facts.view ? current->facts.view(actionId) : std::nullopt;
        };
    }
    if (current->facts.wizard) {
        catalog.wizard = [current](std::string_view actionId) {
            return current->facts.wizard ? current->facts.wizard(actionId) : std::nullopt;
        };
    }
    catalog.schema = [current](std::string_view actionId) -> std::optional<std::string> {
        auto const found = current->schemas.find(actionId);
        if (found == current->schemas.end()) {
            return std::nullopt;
        }
        return found->second;
    };
    return catalog;
}

void Source::fetchSchemas(std::string const& modelType,
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

std::function<std::shared_ptr<ui::Document const>(std::string_view)> Source::documents(
    std::function<void(ui::Refusal const&)> onRefused) {
    return [this, onRefused = std::move(onRefused)](std::string_view screenId) -> std::shared_ptr<ui::Document const> {
        ui::Bundle const* const loaded = bundle();
        std::vector<std::string> const listed = screens();
        if (loaded == nullptr || std::ranges::find(listed, screenId) == listed.end()) {
            return nullptr;
        }
        auto const text = loaded->documents.find(screenId);
        if (text == loaded->documents.end()) {
            return nullptr;
        }
        auto const cached = _documents.find(screenId);
        if (cached != _documents.end() && cached->second.catalogLoad == _catalogLoads &&
            cached->second.text == text->second) {
            return cached->second.document;
        }
        auto document = ui::loadDocumentText(text->second, ui::LoadOptions{.catalog = catalog()});
        if (!document) {
            if (onRefused) {
                onRefused(document.error());
            }
            return nullptr;
        }
        auto shared = std::make_shared<ui::Document const>(std::move(*document));
        _documents.insert_or_assign(
            std::string{screenId},
            LoadedDocument{.text = text->second, .catalogLoad = _catalogLoads, .document = shared});
        return shared;
    };
}

std::vector<std::string> Source::screens() const {
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

bool Source::isPublic(std::string_view screenId) const {
    return _loaded && std::ranges::any_of(_loaded->catalog.entries, [screenId](ui::CatalogEntry const& entry) {
               return entry.id == screenId && entry.isPublic;
           });
}

std::vector<std::string> Source::modelTypes() const {
    std::vector<std::string> types;
    if (!_loaded) {
        return types;
    }
    if (_loaded->catalog.models) {
        for (auto const& [type, info] : *_loaded->catalog.models) {
            static_cast<void>(info);
            types.push_back(type);
        }
    }
    if (_loaded->catalog.actions) {
        for (auto const& [action, info] : *_loaded->catalog.actions) {
            static_cast<void>(action);
            if (!info.modelType.empty() && std::ranges::find(types, info.modelType) == types.end()) {
                types.push_back(info.modelType);
            }
        }
    }
    return types;
}

void Source::recordSchemas(std::string const& document) {
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

void Source::loadSchemas(std::shared_ptr<bool> const& alive, std::function<void()> done) {
    _schemas.clear();
    _byModel.clear();
    std::vector<std::string> const types = modelTypes();
    if (types.empty()) {
        done();
        return;
    }
    // A type whose schemas do not arrive leaves its actions without one: their forms report it when they mount.
    auto const pending = std::make_shared<std::size_t>(types.size());
    auto const finished = std::make_shared<std::function<void()>>(std::move(done));
    for (std::string const& type : types) {
        fetchSchemas(type,
                     [this, type, alive, pending, finished](std::expected<std::string, std::string> const& document) {
                         if (!*alive) {
                             return;
                         }
                         if (document) {
                             recordSchemas(*document);
                         }
                         _byModel.insert_or_assign(type, document);
                         if (--*pending == 0) {
                             (*finished)();
                         }
                     });
    }
}

void Source::open(ui::AppContext& context, std::function<void(std::expected<ui::Bundle, ui::ConnectError>)> ready) {
    close();
    auto const alive = std::make_shared<bool>(true);
    _alive = alive;
    auto transport = connect(context);
    if (!transport) {
        ready(std::unexpected(std::move(transport.error())));
        return;
    }
    exec::IExecutor& owner = context.executor();
    _serverEvents = transport->serverEvents;
    // The source owns the backend's connect hook and its event handler; both reach the owner before anything else
    // sees them, and nothing after `close`.
    transport->backend->setEventHandler(
        [this, alive](wire::EventBody const& event) {
            if (*alive) {
                for (auto const& listener : _eventListeners) {
                    listener(event);
                }
            }
        },
        &owner);
    transport->backend->setConnectHandler([this, alive, &owner] {
        owner.post([this, alive] {
            if (*alive) {
                connected();
            }
        });
    });
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
    // An evaluation carries the session current when it is made, which a sign-in replaces.
    ui::Evaluator evaluate = [this, &owner](std::string const& typeId, std::string const& actionId,
                                            std::string const& bodyJson) {
        return ui::evaluatorOver(_exchange, _session, &owner)(typeId, actionId, bodyJson);
    };
    _dispatcher = std::make_unique<ui::BridgeDispatcher>(*_bridge, &owner, std::move(evaluate));
    // A mutation the screen marks for the offline queue waits while the server cannot be reached, and is retried every
    // few seconds until the replay reaches it.
    _queue = std::make_unique<ui::QueueingDispatcher>(*_dispatcher, &owner);
    _queue->setOnHeld([this, &scheduler = context.scheduler()](std::size_t held) {
        if (held == 0) {
            _replay.reset();
        } else if (!_replay) {
            _replay = scheduler.every(kReplayEvery, [this] { _queue->replay(); });
        }
    });
    _exchange = guarded;
    _store = std::make_shared<ui::MemoryBundleStore>();

    // Shared by the reload's answer and the timeout: whichever comes first is delivered, once.
    auto const deliver = std::make_shared<std::function<void(std::expected<void, ui::ConnectError>)>>(
        [this, alive, delivered = false,
         ready = std::move(ready)](std::expected<void, ui::ConnectError> loaded) mutable {
            if (!*alive || delivered) {
                return;
            }
            delivered = true;
            if (!loaded) {
                ready(std::unexpected(std::move(loaded.error())));
                return;
            }
            ready(_loaded->bundle);
        });
    if (std::chrono::milliseconds const bound = openTimeout(); bound.count() > 0) {
        _timeout = context.scheduler().after(bound, [deliver, bound] {
            (*deliver)(std::unexpected(ui::ConnectError{
                .kind = ui::ConnectError::Kind::Unreachable,
                .message = "no answer from the server within " + std::to_string(bound.count()) + " ms"}));
        });
    }
    reload(alive, [deliver](std::expected<void, ui::ConnectError> loaded) { (*deliver)(std::move(loaded)); });
}

void Source::reload(std::shared_ptr<bool> const& alive,
                    std::function<void(std::expected<void, ui::ConnectError>)> done) {
    ui::UiClient const client{_exchange, clientHello(), _store};
    client.open(_session, [this, alive,
                           done = std::move(done)](std::expected<ui::LoadedBundle, ui::ConnectError> opened) mutable {
        if (!*alive) {
            return;
        }
        if (!opened) {
            done(std::unexpected(std::move(opened.error())));
            return;
        }
        _loaded = std::move(*opened);
        loadSchemas(alive, [this, alive, done = std::move(done)] {
            if (*alive) {
                *_current = CurrentCatalog{.facts = _loaded->catalog.facts(), .schemas = _schemas};
                ++_catalogLoads;
                done({});
            }
        });
    });
}

void Source::poll(std::function<void(std::vector<std::string> changed)> changed) {
    if (!_exchange || !_loaded || !_alive || _polling) {
        return;
    }
    _polling = true;
    ui::UiClient const client{_exchange, clientHello(), _store};
    client.poll(_session, _loaded->hello.digest,
                [this, alive = _alive, changed = std::move(changed)](
                    std::expected<std::optional<ui::LoadedBundle>, ui::ConnectError> polled) mutable {
                    if (!*alive) {
                        return;
                    }
                    _polling = false;
                    // An unreachable server and an unchanged digest are both tried again by the next poll.
                    if (!polled || !polled->has_value()) {
                        return;
                    }
                    std::vector<std::string> ids = ui::changedDocuments(_loaded->manifest, (*polled)->manifest);
                    _loaded = std::move(**polled);
                    loadSchemas(alive, [this, alive, ids = std::move(ids), changed = std::move(changed)] {
                        if (*alive) {
                            *_current = CurrentCatalog{.facts = _loaded->catalog.facts(), .schemas = _schemas};
                            ++_catalogLoads;
                            changed(ids);
                        }
                    });
                });
}

void Source::signIn(std::string principal, std::string token, std::function<void(bool)> done) {
    _session.principal = std::move(principal);
    _session.token = std::move(token);
    if (_bridge) {
        _bridge->setDefaultSession(_session);
    }
    if (!_exchange || !_alive) {
        done(false);
        return;
    }
    reload(_alive, [done = std::move(done)](std::expected<void, ui::ConnectError> const& loaded) {
        done(loaded.has_value());
    });
}

void Source::signOut(std::function<void(bool)> done) { signIn(signedOutPrincipal(), {}, std::move(done)); }

void Source::connected() {
    // The first connect's `ui-hello` is the one `open` filed, and its answer loads the first bundle; every later
    // connect is a new connection, which negotiates events again. Only the handshake: the documents did not change
    // because the connection dropped.
    if (_loaded && _exchange) {
        wire::Envelope hello;
        hello.kind = std::string{ui::kHelloKind};
        hello.session = _session;
        hello.body = ui::detail::writeMessage(clientHello());
        _exchange(std::move(hello), [](ui::ExchangeReply const&) {});
    }
    // The listeners re-watch and refetch. Posted, not run here: a transport posts the bridge's reconnect handler to
    // this same owner right after the connect hook, so it runs first, clears the old connection's model ids and starts
    // rebinding, and a refetch then waits for its new binding. The `ui-hello` above still goes out before any watch.
    if (_bridge == nullptr) {
        return;
    }
    _bridge->owner().post([this, alive = std::weak_ptr<bool>{_alive}] {
        if (auto const live = alive.lock(); !live || !*live) {
            return;
        }
        std::vector<std::function<void()>> const listeners = _connectListeners;
        for (auto const& listener : listeners) {
            listener();
        }
    });
}

void Source::onConnected(std::function<void()> const& listener) { _connectListeners.push_back(listener); }

void Source::onEvent(std::function<void(wire::EventBody const&)> listener) {
    _eventListeners.push_back(std::move(listener));
}

ui::Watcher Source::watcher() {
    if (!_serverEvents || !_exchange || _bridge == nullptr) {
        return {};
    }
    // The session is read at each send, so a watch after a sign-in carries the new principal.
    return ui::watcherOver(_exchange, [this] { return _session; }, &_bridge->owner());
}

void Source::switchBackend(ui::Backend backend, std::function<void(std::expected<void, std::string>)> done) {
    done(std::unexpected("this client does not switch backends; the switch to `" + backend.name +
                         "` is the hosting layer's"));
}

void Source::close() {
    if (_alive) {
        *_alive = false;
    }
    _timeout = {};
    // The dispatcher and the bridge before what the concrete source made: the bridge's backend may run on it.
    _replay.reset();
    _queue.reset();
    _dispatcher.reset();
    _bridge.reset();
    _loaded.reset();
    disconnect();
}

std::optional<ui::FileTransfer> Source::files() const {
    if (!_exchange) {
        return std::nullopt;
    }
    return ui::FileTransfer{_exchange, _session};
}

// ── RemoteSource ─────────────────────────────────────────────────────────

RemoteSource::RemoteSource(std::string url, std::chrono::milliseconds timeout)
    : _url{std::move(url)}, _timeout{timeout} {}

std::expected<Source::Transport, ui::ConnectError> RemoteSource::connect(ui::AppContext& context) {
    auto made = makeTransport(context, _url);
    if (!made) {
        return std::unexpected(ui::ConnectError{.kind = ui::ConnectError::Kind::Unreachable,
                                                .message = "cannot connect to " + _url + ": " + made.error()});
    }
    ui::Exchange exchange = ui::exchangeOver(*made->requests, context.executor());
    return Transport{.backend = std::move(made->backend), .exchange = std::move(exchange), .serverEvents = true};
}

// ── LocalSource ──────────────────────────────────────────────────────────

LocalSource::LocalSource(LocalApp app, LocalSetup setup) : _app{std::move(app)}, _setup{std::move(setup)} {
    requestSession().principal = _setup.user;
}

LocalSource::~LocalSource() {
    // Here, not in the base: the bridge's LocalBackend runs on this class's workers, which go first otherwise.
    close();
}

void LocalSource::fetchSchemas(std::string const& modelType,
                               std::function<void(std::expected<std::string, std::string>)> done) {
    // In process, the registry the local backend dispatches through answers directly, as a server's does; a model
    // whose schemas cannot be emitted (an unsatisfiable form) is reported as not described, as a refusal would be.
    std::expected<std::string, std::string> document;
    try {
        document = model::detail::defaultDispatcher().schemasJson(modelType);
    } catch (std::exception const& error) {
        document = std::unexpected(std::string{error.what()});
    }
    done(std::move(document));
}

std::expected<Source::Transport, ui::ConnectError> LocalSource::connect(ui::AppContext& context) {
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

void LocalSource::disconnect() {
    _workers.reset();
    _service.reset();
}

// ── makeSource ──────────────────────────────────────────────────────────────

std::expected<std::unique_ptr<Source>, std::string> makeSource(Args const& args, std::vector<LocalApp> const& apps) {
    if (args.server) {
        return std::make_unique<RemoteSource>(*args.server);
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
    return std::make_unique<LocalSource>(*found, LocalSetup{.db = args.db, .user = args.user});
}

}  // namespace morph::client

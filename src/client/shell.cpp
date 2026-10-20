// SPDX-License-Identifier: Apache-2.0

#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <map>
#include <memory>
#include <morph/client/shell.hpp>
#include <morph/core/wire.hpp>
#include <morph/reactive/scheduler.hpp>
#include <morph/ui/dev_reload.hpp>
#include <morph/ui/document.hpp>
#include <morph/ui/explorer.hpp>
#include <morph/ui/host.hpp>
#include <morph/ui/inspector.hpp>
#include <morph/ui/interpreter.hpp>
#include <morph/ui/mount.hpp>
#include <morph/ui/route_history.hpp>
#include <morph/ui/service_client.hpp>
#include <morph/ui/shell_frame.hpp>
#include <morph/ui/view.hpp>
#include <morph/ui/workspace.hpp>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace morph::client {

namespace {

// How often a development shell polls the source's digest (spec 5 §12).
constexpr std::chrono::seconds kDevPollEvery{1};

// The explorer's screen id, and the host screen it names.
constexpr std::string_view kExplorer = "explorer";

/// The library's document host, with `navigate` handed to the shell.
class Host final : public ui::DocumentScreenHost {
public:
    Host(ui::Session& session, ui::Dispatcher& dispatcher, ui::DocumentScreenHostOptions options,
         std::function<void(std::string, ui::ParamMap)> navigate)
        : DocumentScreenHost{session, dispatcher, std::move(options)}, _navigate{std::move(navigate)} {}

    void onNavigate(std::string_view screenId, ui::ParamMap const& params) override {
        _navigate(std::string{screenId}, params);
    }

    using DocumentScreenHost::onNavigate;

private:
    std::function<void(std::string, ui::ParamMap)> _navigate;
};

/// The path a picked file is: the path itself, or the first of a pick of several.
std::optional<std::filesystem::path> pickedPath(ui::Value const& file) {
    ui::ValueList const* const list = file.asList();
    ui::Value const& first = list != nullptr && !list->empty() ? list->front() : file;
    if (std::string const* const path = first.asString(); path != nullptr && !path->empty()) {
        return std::filesystem::path{*path};
    }
    return std::nullopt;
}

/// Writes @p bytes under the suggested name, else @p own, in @p directory, and answers the saved path. Only the name's
/// last component is kept: a file never lands outside the downloads directory.
std::expected<ui::Value, std::string> save(std::filesystem::path const& directory, std::string const& suggested,
                                           // NOLINTNEXTLINE(bugprone-easily-swappable-parameters): names, then bytes.
                                           std::string const& own, std::string const& bytes) {
    auto const leaf = std::filesystem::path{suggested.empty() ? own : suggested}.filename();
    auto const target = directory / (leaf.empty() ? std::filesystem::path{"download"} : leaf);
    std::ofstream out{target, std::ios::binary};
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    if (!out) {
        return std::unexpected("cannot write " + target.string());
    }
    return ui::Value{target.string()};
}

class Shell final : public ui::MountedShell {
public:
    Shell(ui::AppContext& context, ui::IViewBackend& backend, ShellOptions options)
        : _context{&context}, _backend{&backend}, _options{std::move(options)} {}

    ~Shell() override {
        *_alive = false;
        // The tabs first, then the frame they sit in.
        _workspace.reset();
        _frame.reset();
    }
    Shell(Shell const&) = delete;
    Shell& operator=(Shell const&) = delete;
    Shell(Shell&&) = delete;
    Shell& operator=(Shell&&) = delete;

    /// Hands the session the source's connects and events. Registered once: they reach whichever session is
    /// current, and nothing once the shell is gone.
    void listen(Source& source) {
        if (_listening) {
            return;
        }
        _listening = true;
        source.onConnected([this, alive = std::weak_ptr<bool>{_alive}] {
            if (auto const live = alive.lock(); live != nullptr && *live && _session) {
                _session->reconnected();
            }
        });
        source.onEvent([this, alive = std::weak_ptr<bool>{_alive}](wire::EventBody const& event) {
            if (auto const live = alive.lock(); live != nullptr && *live && _session) {
                _session->deliverEvent(event);
            }
        });
    }

    /// Loads the bundle's screens and mounts the start screen.
    void start(ui::Bundle const& bundle, Source& source) {
        _source = &source;
        loadDocuments(bundle);
        std::string const start = _options.startScreen.empty() ? firstScreen(false) : _options.startScreen;
        if (start.empty()) {
            show("The application has no screen this client can open.");
            return;
        }
        ui::SessionOptions sessionOptions;
        sessionOptions.scheduler = &_context->scheduler();
        sessionOptions.catalog = source.catalog();
        sessionOptions.developmentTools = _options.dev ? ui::DevelopmentTools::On : ui::DevelopmentTools::Off;
        sessionOptions.watcher = source.watcher();
        _session = std::make_unique<ui::Session>(_context->runtime(), std::move(sessionOptions));
        listen(source);
        showPermitted();
        ui::DocumentScreenHostOptions hostOptions;
        hostOptions.documents = [this](std::string_view screenId) -> std::shared_ptr<ui::Document const> {
            auto const found = _documents.find(screenId);
            if (found != _documents.end()) {
                return found->second;
            }
            // The explorer's "Try it" documents.
            auto const generated = _generated.find(screenId);
            return generated == _generated.end() ? nullptr : generated->second;
        };
        hostOptions.backend = _backend;
        _application = std::make_unique<ApplicationHost>(
            source, _options.signIn, _options.downloads,
            [this](SignedIn signedIn) {
                reloadThen(true, [this, signedIn = std::move(signedIn)](auto done) mutable {
                    _source->signIn(std::move(signedIn.principal), std::move(signedIn.token), std::move(done));
                });
            },
            [this] { signOut(); }, [this](std::string const& text) { problem(text); });
        hostOptions.application = _application.get();
        hostOptions.diagnostics = [this](ui::Refusal const& refusal) { problem(refusal.message()); };
        _host = std::make_unique<Host>(
            *_session, source.dispatcher(), std::move(hostOptions), [this](std::string screenId, ui::ParamMap params) {
                // A navigation a sign-in asks for waits for the catalog the sign-in reloads, which lists the screen.
                if (_reloading) {
                    _pending = std::pair{std::move(screenId), std::move(params)};
                    return;
                }
                // Posted: the current screen is replaced, which must not happen inside
                // the handler of one of its own elements.
                _context->executor().post([alive = std::weak_ptr<bool>{_alive}, this, screenId = std::move(screenId),
                                           params = std::move(params)] {
                    if (auto const live = alive.lock(); live != nullptr && *live) {
                        mount(screenId, params);
                    }
                });
            });
        _hosts.add(std::string{kExplorer}, [this](ui::ParamMap const& params, ui::ContainerWidget& page) {
            static_cast<void>(params);
            return std::expected<std::unique_ptr<ui::ScreenHandle>, ui::MountError>{openExplorer(page)};
        });
        if (_options.history != nullptr) {
            _routes.emplace(*_options.history, [this](std::string const& route) { openRoute(route); });
            // A start screen asked for by name wins over the route the history holds.
            if (_options.startScreen.empty()) {
                _wanted = _routes->initial();
            }
        }
        // A start screen that is not public is used by the principal the source opened with, so the frame shows its
        // chrome from the start.
        _signedIn = !_source->isPublic(start);
        buildFrame();
        if (!openWanted()) {
            mount(start, _options.startParams);
        }
        if (_options.dev) {
            _poll = _context->scheduler().every(kDevPollEvery, [this, alive = std::weak_ptr<bool>{_alive}] {
                _source->poll([this, alive](std::vector<std::string> const& changed) {
                    if (auto const live = alive.lock(); live != nullptr && *live) {
                        reloadChanged(changed);
                    }
                });
            });
        }
    }

    void inspectFocused(std::function<void(ui::Inspector&)> const& visit) override {
        ui::ScreenHandle* const shown = _workspace ? _workspace->activeHandle() : nullptr;
        if (shown == nullptr || !visit) {
            return;
        }
        if (std::optional<ui::Inspector> inspector = ui::DocumentScreenHost::inspector(*shown)) {
            visit(*inspector);
        }
    }

    /// Shows @p text in place of any screen, and reports it.
    void show(std::string const& text) {
        problem(text);
        unmount();
        _message.emplace(_context->runtime(), *_backend,
                         ui::text(ui::Text{.text = text, .role = ui::TextRole::Error}));
    }

private:
    // The explorer over what the source holds now: the catalog, every model's schemas, and the loaded documents.
    std::unique_ptr<ui::ScreenHandle> openExplorer(ui::ContainerWidget& page) {
        ui::ExplorerOptions explorer;
        if (ui::CatalogReply const* const catalog = _source->catalogReply()) {
            explorer.catalog = *catalog;
        }
        explorer.facts = _source->catalog();
        explorer.schemas = _source->schemasByModel();
        explorer.documents = _documents;
        if (_appDocument) {
            explorer.documents.emplace(std::string{ui::kAppFile}, _appDocument);
        }
        explorer.host = _host.get();
        explorer.provide = [this](std::string const& screenId, std::shared_ptr<ui::Document const> document) {
            _generated.insert_or_assign(screenId, std::move(document));
        };
        explorer.onOpen = [this](std::string const& screenId) {
            // Posted: the button that asked is a widget of the explorer's tab.
            _context->executor().post([this, alive = std::weak_ptr<bool>{_alive}, screenId] {
                if (auto const live = alive.lock(); live != nullptr && *live) {
                    mount(screenId, {});
                }
            });
        };
        return std::make_unique<ui::Explorer>(_context->runtime(), *_backend, page, std::move(explorer));
    }

    // What `{"can": …}` reads: the actions the current catalog lets the principal run.
    void showPermitted() {
        ui::CatalogReply const* const catalog = _source->catalogReply();
        if (_session == nullptr || catalog == nullptr) {
            return;
        }
        _session->setPermitted(
            std::set<std::string, std::less<>>{catalog->permitted.begin(), catalog->permitted.end()});
    }

    // Signing out, from the frame's button or a document's `signOut`: every screen goes first, so their private
    // instances are released under the session that owns them; from a posted task, since the screen that signed out
    // may still be running its commands.
    void signOut() {
        reloadThen(false, [this](auto done) {
            _context->executor().post([this, alive = std::weak_ptr<bool>{_alive}, done = std::move(done)] {
                if (auto const live = alive.lock(); live != nullptr && *live) {
                    unmount();
                    _source->signOut(done);
                }
            });
        });
    }

    // The frame and its workspace for the catalog the source holds now: the navigator lists its entries, so a new
    // session's catalog gets a new frame. Every tab of the old one is closed.
    void buildFrame() {
        _workspace.reset();
        _frame.reset();
        ui::CatalogReply const* const catalog = _source->catalogReply();
        std::vector<ui::CatalogEntry> entries;
        if (catalog != nullptr) {
            for (ui::CatalogEntry const& entry : catalog->entries) {
                // The navigator lists what a signed-in person opens; before sign-in the public screens are opened by
                // the shell itself.
                if (_documents.contains(entry.id) && !entry.isPublic) {
                    entries.push_back(entry);
                }
            }
        }
        // The explorer, once there is a navigator to list it in.
        if (!entries.empty()) {
            entries.push_back(ui::CatalogEntry{.id = std::string{kExplorer},
                                               .title = "Explorer",
                                               .module = "Tools",
                                               .singleton = true,
                                               .host = std::string{kExplorer}});
        }
        ui::ShellFrameOptions frame;
        frame.applicationName = _source->bundle() != nullptr ? _source->bundle()->applicationId : std::string{};
        frame.navigator.entries = entries;
        frame.navigator.onOpen = [this](std::string const& screenId) {
            // Posted: the navigator's button is a widget of the frame, which a mount may rebuild.
            _context->executor().post([this, alive = std::weak_ptr<bool>{_alive}, screenId] {
                if (auto const live = alive.lock(); live != nullptr && *live) {
                    mount(screenId, {});
                }
            });
        };
        frame.onSignOut = [this] { signOut(); };
        _frame = std::make_unique<ui::ShellFrame>(*_backend, nullptr, std::move(frame));
        // Whether a sign-in happened is the shell's own fact: a local source's signed-out principal is its `--user`
        // name, and its signed-out catalog lists the screens, so neither tells signed in from signed out.
        showSignedIn();
        ui::WorkspaceOptions tabs;
        tabs.screens = [entries = catalog != nullptr ? catalog->entries
                                                     : std::vector<ui::CatalogEntry>{}](std::string_view screenId) {
            if (screenId == kExplorer) {
                return ui::WorkspaceScreen{.singleton = true, .host = std::string{kExplorer}};
            }
            for (ui::CatalogEntry const& entry : entries) {
                if (entry.id == screenId) {
                    return ui::WorkspaceScreen{.singleton = entry.singleton,
                                               .reuseEmpty = entry.reuseEmpty,
                                               .host = entry.host.value_or(std::string{}),
                                               .available = entry.available};
                }
            }
            return ui::WorkspaceScreen{};
        };
        tabs.onChanged = [this] { showActiveTitle(); };
        tabs.hosts = &_hosts;
        _workspace =
            std::make_unique<ui::Workspace>(_context->runtime(), *_backend, *_host, std::move(tabs), &_frame->page());
    }

    // The sidebar and the top bar, with the principal, while the shell is signed in; the page alone otherwise.
    void showSignedIn() {
        _frame->showSignedIn(_signedIn);
        _frame->setPrincipal(_signedIn ? _source->principal() : std::string{});
    }

    // The top bar follows the active tab: its caption is the screen's title.
    void showActiveTitle() {
        if (!_frame || !_workspace) {
            return;
        }
        std::optional<std::size_t> const shown = _workspace->active();
        auto const tabs = _workspace->tabs();
        _frame->setTitle(shown && *shown < tabs.size() ? tabs.at(*shown).caption : std::string{});
    }

    // The screen id of the active tab; empty with none.
    [[nodiscard]] std::string activeScreen() const {
        if (!_workspace) {
            return {};
        }
        std::optional<std::size_t> const shown = _workspace->active();
        auto const tabs = _workspace->tabs();
        return shown && *shown < tabs.size() ? tabs.at(*shown).screenId : std::string{};
    }

    // The documents of every screen the catalog lists, loaded against its facts; a screen whose document does not
    // load is reported and left out.
    void loadDocuments(ui::Bundle const& bundle) {
        _documents.clear();
        _generated.clear();
        _appDocument.reset();
        ui::Catalog const catalog = _source->catalog();
        if (auto const app = bundle.documents.find(ui::kAppFile); app != bundle.documents.end()) {
            auto loaded = ui::loadDocumentText(app->second, ui::LoadOptions{.catalog = catalog});
            if (loaded) {
                _appDocument = std::make_shared<ui::Document const>(std::move(*loaded));
            } else {
                problem(loaded.error().message());
            }
        }
        for (std::string const& screenId : _source->screens()) {
            auto loaded = ui::loadDocumentText(bundle.documents.at(screenId), ui::LoadOptions{.catalog = catalog});
            if (!loaded) {
                problem(loaded.error().message());
                continue;
            }
            _documents.emplace(screenId, std::make_shared<ui::Document const>(std::move(*loaded)));
        }
    }

    // The first screen the catalog lists that loaded: signed in, the first that is not public; signed out, the first
    // that is, such as the login screen; either way, the first at all when there is none such.
    [[nodiscard]] std::string firstScreen(bool signedIn) const {
        std::string fallback;
        for (std::string const& screenId : _source->screens()) {
            if (!_documents.contains(screenId)) {
                continue;
            }
            if (_source->isPublic(screenId) != signedIn) {
                return screenId;
            }
            if (fallback.empty()) {
                fallback = screenId;
            }
        }
        return fallback;
    }

    // Runs @p change, which installs a session and reloads the catalog, holding navigation until the catalog is
    // current; then mounts the navigation that waited, or keeps a screen that signed in in place, or mounts the start
    // screen for the new session: after a sign-in the first screen that is not public, after a sign-out the first that
    // is.
    void reloadThen(bool signingIn, std::function<void(std::function<void(bool)>)> const& change) {
        _reloading = true;
        _pending.reset();
        change([this, signingIn, alive = std::weak_ptr<bool>{_alive}](bool loaded) {
            // Posted, even when the reload answered at once: the document that signed in may still ask to navigate
            // in the same handler, and that navigation is what this mounts.
            _context->executor().post([this, signingIn, loaded, alive] {
                auto const live = alive.lock();
                if (live == nullptr || !*live) {
                    return;
                }
                _reloading = false;
                if (!loaded || _source->bundle() == nullptr) {
                    show("The session changed, and its screens could not be loaded.");
                    return;
                }
                loadDocuments(*_source->bundle());
                showPermitted();
                _changed = true;
                _signedIn = signingIn;
                // A screen that signed in in place stays when it is still listed and the new session has no screen
                // of its own to start on; the frame is rebuilt for the new catalog either way, so it is reopened.
                std::string const kept = activeScreen();
                buildFrame();
                // The route the page was opened with, which the signed-out catalog did not list, is tried once more
                // with the new session's, before a navigation the sign-in asked for: the link is what the user came
                // for. A move back or forward made during the reload is tried the same way.
                bool const opened = openWanted();
                if (signingIn) {
                    _wanted.clear();
                }
                auto const pending = std::exchange(_pending, std::nullopt);
                if (opened) {
                    return;
                }
                if (pending) {
                    mount(pending->first, pending->second);
                    return;
                }
                // A screen that signed in in place, and is still listed, stays as it is, unless it is a public login
                // screen and the new session has a screen of its own to start on.
                std::string const start = firstScreen(signingIn);
                bool const keep = !kept.empty() && _documents.contains(kept) &&
                                  (!_source->isPublic(kept) || start.empty() || _source->isPublic(start));
                mount(keep ? kept : start, {});
            });
        });
    }

    // Opens a screen as a tab, or focuses the tab that already shows it.
    void mount(std::string const& screenId, ui::ParamMap const& params) {
        _message.reset();
        if (!_workspace) {
            buildFrame();
        }
        // Opening a screen that is not public, by a route or a navigation, is using it as the principal the source
        // holds: the frame shows its chrome as it does for a start screen named so.
        if (!_signedIn && !_source->isPublic(screenId)) {
            _signedIn = true;
            showSignedIn();
        }
        auto const opened = _workspace->open(screenId, params);
        if (!opened) {
            show("Cannot open " + opened.error().screenId + ": " + opened.error().reason);
            return;
        }
        if (_routes) {
            auto const route = _host->routeOf(screenId, params);
            _routes->shown(route ? *route : std::string{});
        }
        if (_options.onMounted) {
            _options.onMounted(screenId);
        }
    }

    // Opens the route the history held at start, once the catalog lists its screen.
    bool openWanted() {
        if (_wanted.empty()) {
            return false;
        }
        auto const route = _host->parseRoute(_wanted);
        if (!route || !_documents.contains(route->screen)) {
            return false;
        }
        _wanted.clear();
        mount(route->screen, route->params);
        return true;
    }

    // Opens the route of the history entry the user moved to, or the start screen for a route this session cannot
    // open: an unknown screen, a param that does not parse, a screen the catalog does not list.
    void openRoute(std::string const& route) {
        if (_host == nullptr) {
            return;
        }
        if (_reloading) {
            _wanted = route;  // opened once the catalog is current
            return;
        }
        auto const parsed = _host->parseRoute(route);
        if (parsed && _documents.contains(parsed->screen)) {
            mount(parsed->screen, parsed->params);
            return;
        }
        // The start screen: the one asked for at start, until a session change; then the new session's.
        if (!_changed) {
            mount(_options.startScreen.empty() ? firstScreen(false) : _options.startScreen,
                  _options.startScreen.empty() ? ui::ParamMap{} : _options.startParams);
        } else {
            mount(firstScreen(_signedIn), {});
        }
    }

    // Loads the documents a development poll found changed and remounts the shown screen when its own changed; a
    // document that does not load, or that the interpreter refuses, keeps the old one and is reported.
    void reloadChanged(std::vector<std::string> const& changed) {
        if (changed.empty() || _host == nullptr || _source->bundle() == nullptr || _reloading) {
            return;
        }
        std::vector<ui::MountedScreen> mounted;
        if (_workspace) {
            auto const tabs = _workspace->tabs();
            for (std::size_t index = 0; index < tabs.size(); ++index) {
                mounted.push_back(
                    ui::MountedScreen{.screenId = tabs.at(index).screenId, .handle = _workspace->handleOf(index)});
            }
        }
        ui::ReloadOutcome const outcome = ui::reloadDocuments(
            changed, *_source->bundle(), ui::LoadOptions{.catalog = _source->catalog()}, _documents, *_host, mounted);
        for (ui::Refusal const& refusal : outcome.refused) {
            problem("reload refused: " + refusal.message());
        }
        if (_options.onReloaded) {
            _options.onReloaded(outcome.remounted);
        }
    }

    void unmount() {
        _message.reset();
        if (_workspace) {
            _workspace->forceCloseAll();
        }
    }

    void problem(std::string const& text) const {
        if (_options.onProblem) {
            _options.onProblem(text);
        }
    }

    ui::AppContext* _context;
    ui::IViewBackend* _backend;
    ShellOptions _options;
    std::shared_ptr<bool> _alive = std::make_shared<bool>(true);
    Source* _source = nullptr;
    bool _reloading = false;
    std::optional<std::pair<std::string, ui::ParamMap>> _pending;
    std::map<std::string, std::shared_ptr<ui::Document const>, std::less<>> _documents;
    // The app document, which the explorer reads its uses from.
    std::shared_ptr<ui::Document const> _appDocument;
    // The documents the explorer generates for "Try it", by their screen ids.
    std::map<std::string, std::shared_ptr<ui::Document const>, std::less<>> _generated;
    ui::ScreenHostRegistry _hosts;
    std::unique_ptr<ui::Session> _session;
    // Whether the source's connect and event listeners are registered.
    bool _listening = false;
    std::unique_ptr<ApplicationHost> _application;
    std::unique_ptr<Host> _host;
    std::unique_ptr<ui::ShellFrame> _frame;
    std::unique_ptr<ui::Workspace> _workspace;
    std::optional<ui::Mounted> _message;
    // Whether a sign-in or sign-out has reloaded the catalog, and whether the session is now signed in.
    bool _changed = false;
    bool _signedIn = false;
    std::string _wanted;
    // The development poll's timer.
    std::optional<reactive::TimerHandle> _poll;
    // Last, so it stops following the history before anything it opens screens with is gone.
    std::optional<ui::RouteHistory> _routes;
};

}  // namespace

std::optional<SignedIn> defaultSignIn(ui::Value const& reply) {
    ui::ValueRecord const* const record = reply.asRecord();
    if (record == nullptr) {
        return std::nullopt;
    }
    auto const accepted = record->find("ok");
    if (accepted != record->end() && accepted->second.asBool() != nullptr && !*accepted->second.asBool()) {
        return std::nullopt;
    }
    auto const text = [record](char const* field) {
        auto const found = record->find(field);
        std::string const* const value = found == record->end() ? nullptr : found->second.asString();
        return value == nullptr ? std::string{} : *value;
    };
    std::string principal = text("principal");
    if (principal.empty()) {
        return std::nullopt;
    }
    return SignedIn{.principal = std::move(principal), .token = text("token")};
}

ApplicationHost::ApplicationHost(Source& source, SignInMapper signIn, std::filesystem::path downloads,
                                 std::function<void(SignedIn)> signedIn, std::function<void()> signedOut,
                                 std::function<void(std::string const&)> onProblem)
    : _source{&source},
      _signIn{std::move(signIn)},
      _downloads{std::move(downloads)},
      _signedIn{std::move(signedIn)},
      _signedOut{std::move(signedOut)},
      _onProblem{std::move(onProblem)} {
    if (!_signIn) {
        throw std::invalid_argument{"morph::client::ApplicationHost: the sign-in mapper is empty"};
    }
}

bool ApplicationHost::signIn(ui::Value const& reply) {
    std::optional<SignedIn> mapped = _signIn(reply);
    if (!mapped) {
        return false;
    }
    // A mapper that answers no principal would install an anonymous session as if it were a sign-in.
    if (mapped->principal.empty()) {
        if (_onProblem) {
            _onProblem("sign-in mapper answered no principal");
        }
        return false;
    }
    if (_signedIn) {
        _signedIn(*std::move(mapped));
    }
    return true;
}

void ApplicationHost::signOut() {
    if (_signedOut) {
        _signedOut();
    }
}

bool ApplicationHost::uploadFor(ui::UploadRequest const& request, std::function<void(ui::Value)> progress,
                                std::function<void(std::expected<ui::Value, std::string>)> done) {
    auto const files = _source->files();
    auto const path = pickedPath(request.file);
    if (!files || !path) {
        return false;
    }
    std::ifstream input{*path, std::ios::binary};
    if (!input) {
        done(std::unexpected("cannot read " + path->string()));
        return true;
    }
    std::string bytes{std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
    files->upload(
        ui::FileInfo{.name = path->filename().string(), .type = "application/octet-stream", .size = 0},
        std::move(bytes), request.modelType, request.action,
        [progress = std::move(progress)](double fraction) {
            if (progress) {
                // An exact fraction at two places: a document's values are never floating point.
                auto const percent = static_cast<std::int64_t>(std::llround(fraction * 100.0));
                progress(ui::Value{ui::Decimal{
                    math::Rational{math::Numerator{percent}, math::Denominator{100}, math::DecimalPlaces{2}}}});
            }
        },
        [done = std::move(done)](std::expected<std::string, std::string> ref) {
            done(ref ? std::expected<ui::Value, std::string>{ui::Value{*ref}}
                     : std::expected<ui::Value, std::string>{std::unexpected(ref.error())});
        });
    return true;
}

bool ApplicationHost::download(ui::Value const& file, std::string const& option, ui::Value const& name,
                               std::function<void(std::expected<ui::Value, std::string>)> done) {
    auto const files = _source->files();
    std::string const* const ref = file.asString();
    if (!files || ref == nullptr || option == "preview") {
        return false;
    }
    std::string const* const suggested = name.asString();
    files->download(*ref, {},
                    [directory = _downloads, suggested = suggested == nullptr ? std::string{} : *suggested,
                     done = std::move(done)](std::expected<ui::DownloadedFile, std::string> got) {
                        if (!got) {
                            done(std::unexpected(got.error()));
                            return;
                        }
                        done(save(directory, suggested, got->name, got->bytes));
                    });
    return true;
}

bool ApplicationHost::readFile(ui::Value const& file, std::size_t maxBytes,
                               std::function<void(std::expected<ui::InlineFile, std::string>)> done) {
    auto const path = pickedPath(file);
    if (!path) {
        return false;
    }
    std::error_code error;
    std::uintmax_t const size = std::filesystem::file_size(*path, error);
    if (error) {
        done(std::unexpected("cannot read " + path->string()));
        return true;
    }
    if (size > maxBytes) {
        done(std::unexpected(path->filename().string() + " is " + std::to_string(size) + " bytes, over the " +
                             std::to_string(maxBytes) + "-byte inline cap"));
        return true;
    }
    std::ifstream input{*path, std::ios::binary};
    if (!input) {
        done(std::unexpected("cannot read " + path->string()));
        return true;
    }
    std::string bytes{std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
    done(ui::InlineFile{.name = path->filename().string(), .mediaType = {}, .bytes = std::move(bytes)});
    return true;
}

bool ApplicationHost::saveFile(ui::InlineFile const& file, std::string const& option,
                               std::function<void(std::expected<ui::Value, std::string>)> done) {
    if (option == "preview") {
        return false;
    }
    done(save(_downloads, file.name, file.name, file.bytes));
    return true;
}

std::unique_ptr<ui::MountedShell> mountShell(ui::AppContext& context, ui::IViewBackend& backend,
                                             std::expected<ui::Bundle, ui::ConnectError> opened, Source& source,
                                             ShellOptions options) {
    if (!options.signIn) {
        throw std::invalid_argument{
            "morph::client::mountShell: ShellOptions::signIn is empty; it maps a sign-in reply"};
    }
    auto shell = std::make_unique<Shell>(context, backend, std::move(options));
    if (!opened) {
        shell->show("Cannot open the application: " + opened.error().message);
    } else {
        shell->start(*opened, source);
    }
    return shell;
}

}  // namespace morph::client

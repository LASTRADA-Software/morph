// SPDX-License-Identifier: Apache-2.0

#include "app/client_shell.hpp"

#include <chrono>
#include <cmath>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <map>
#include <memory>
#include <morph/reactive/scheduler.hpp>
#include <morph/ui/dev_reload.hpp>
#include <morph/ui/document.hpp>
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
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace morph::examples {

namespace {

// How often a development shell polls the source's digest (spec 5 §12).
constexpr std::chrono::seconds kDevPollEvery{1};

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

/// The application's side of the commands the library's host does not answer itself: sign-in and sign-out, which
/// install the principal a sign-in mutation answered into the source; and the file commands, which read a picked file
/// from disk to upload it and save a downloaded one, moving the bytes over the source's file side channel.
class Application final : public ui::InterpreterHost {
public:
    /// The shell's side of a sign-in: install the principal and token, reload, then mount.
    using SignedIn = std::function<void(std::string principal, std::string token)>;

    Application(ClientSource& source, std::filesystem::path downloads, SignedIn signedIn,
                std::function<void()> signedOut)
        : _source{&source},
          _downloads{std::move(downloads)},
          _signedIn{std::move(signedIn)},
          _signedOut{std::move(signedOut)} {}

    // The reply's `principal`, and its `token` when it carries one, unless it says `ok: false`.
    bool signIn(ui::Value const& reply) override {
        ui::ValueRecord const* const record = reply.asRecord();
        if (record == nullptr) {
            return false;
        }
        auto const accepted = record->find("ok");
        if (accepted != record->end() && accepted->second.asBool() != nullptr && !*accepted->second.asBool()) {
            return false;
        }
        auto const text = [record](char const* field) {
            auto const found = record->find(field);
            std::string const* const value = found == record->end() ? nullptr : found->second.asString();
            return value == nullptr ? std::string{} : *value;
        };
        std::string principal = text("principal");
        if (principal.empty()) {
            return false;
        }
        _signedIn(std::move(principal), text("token"));
        return true;
    }

    void signOut() override { _signedOut(); }

    bool uploadFor(ui::UploadRequest const& request, std::function<void(ui::Value)> progress,
                   std::function<void(std::expected<ui::Value, std::string>)> done) override {
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

    // Saves the file under the name the screen suggested, else its own, in the downloads directory, and answers the
    // saved path. Only saving is supported: a preview needs a viewer the shell does not have.
    bool download(ui::Value const& file, std::string const& option, ui::Value const& name,
                  std::function<void(std::expected<ui::Value, std::string>)> done) override {
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
                            // Only the name's last component: a file never lands outside the downloads directory.
                            auto const leaf =
                                std::filesystem::path{suggested.empty() ? got->name : suggested}.filename();
                            auto const target = directory / (leaf.empty() ? std::filesystem::path{"download"} : leaf);
                            std::ofstream out{target, std::ios::binary};
                            out.write(got->bytes.data(), static_cast<std::streamsize>(got->bytes.size()));
                            if (!out) {
                                done(std::unexpected("cannot write " + target.string()));
                                return;
                            }
                            done(ui::Value{target.string()});
                        });
        return true;
    }

private:
    ClientSource* _source;
    std::filesystem::path _downloads;
    SignedIn _signedIn;
    std::function<void()> _signedOut;
};

class Shell final : public ui::MountedShell {
public:
    Shell(ui::AppContext& context, ui::IViewBackend& backend, ClientShellOptions options)
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

    /// Loads the bundle's screens and mounts the start screen.
    void start(ui::Bundle const& bundle, ClientSource& source) {
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
        _session = std::make_unique<ui::Session>(_context->runtime(), std::move(sessionOptions));
        ui::DocumentScreenHostOptions hostOptions;
        hostOptions.documents = [this](std::string_view screenId) -> std::shared_ptr<ui::Document const> {
            auto const found = _documents.find(screenId);
            return found == _documents.end() ? nullptr : found->second;
        };
        hostOptions.backend = _backend;
        _application = std::make_unique<Application>(
            source, _options.downloads,
            [this](std::string principal, std::string token) {
                reloadThen(true,
                           [this, principal = std::move(principal), token = std::move(token)](auto done) mutable {
                               _source->signIn(std::move(principal), std::move(token), std::move(done));
                           });
            },
            [this] { signOut(); });
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
        if (_options.history != nullptr) {
            _routes.emplace(*_options.history, [this](std::string const& route) { openRoute(route); });
            // A start screen asked for by name wins over the route the history holds.
            if (_options.startScreen.empty()) {
                _wanted = _routes->initial();
            }
        }
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
        // The sidebar and the top bar show once the catalog lists a screen beyond the public ones: before a sign-in
        // that is none, and an application without one lists its screens from the start.
        _frame->showSignedIn(!entries.empty());
        _frame->setPrincipal(_source->principal());
        ui::WorkspaceOptions tabs;
        tabs.screens = [entries = catalog != nullptr ? catalog->entries
                                                     : std::vector<ui::CatalogEntry>{}](std::string_view screenId) {
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
        _workspace =
            std::make_unique<ui::Workspace>(_context->runtime(), *_backend, *_host, std::move(tabs), &_frame->page());
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
        ui::Catalog const catalog = _source->catalog();
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
    ClientShellOptions _options;
    std::shared_ptr<bool> _alive = std::make_shared<bool>(true);
    ClientSource* _source = nullptr;
    bool _reloading = false;
    std::optional<std::pair<std::string, ui::ParamMap>> _pending;
    std::map<std::string, std::shared_ptr<ui::Document const>, std::less<>> _documents;
    std::unique_ptr<ui::Session> _session;
    std::unique_ptr<Application> _application;
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

std::unique_ptr<ui::MountedShell> mountClientShell(ui::AppContext& context, ui::IViewBackend& backend,
                                                   std::expected<ui::Bundle, ui::ConnectError> opened,
                                                   ClientSource& source, ClientShellOptions options) {
    auto shell = std::make_unique<Shell>(context, backend, std::move(options));
    if (!opened) {
        shell->show("Cannot open the application: " + opened.error().message);
    } else {
        shell->start(*opened, source);
    }
    return shell;
}

}  // namespace morph::examples

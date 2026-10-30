// SPDX-License-Identifier: Apache-2.0

#include <algorithm>
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
#include <morph/reactive/signal.hpp>
#include <morph/ui/dev_reload.hpp>
#include <morph/ui/document.hpp>
#include <morph/ui/explorer.hpp>
#include <morph/ui/host.hpp>
#include <morph/ui/inspector.hpp>
#include <morph/ui/interpreter.hpp>
#include <morph/ui/mount.hpp>
#include <morph/ui/navigator.hpp>
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

/// The library's document host, with `navigate` and a screen that closed itself handed to the shell, and the call of a
/// navigation that mounted nothing settled by it.
class Host final : public ui::DocumentScreenHost {
public:
    Host(ui::Session& session, ui::Dispatcher& dispatcher, ui::DocumentScreenHostOptions options,
         std::function<void(ui::NavigateRequest)> navigate, std::function<void(ui::ScreenHandle&)> closed)
        : DocumentScreenHost{session, dispatcher, std::move(options)},
          _navigate{std::move(navigate)},
          _closed{std::move(closed)} {}

    void onNavigate(std::string_view screenId, ui::ParamMap const& params) override {
        _navigate(ui::NavigateRequest{
            .screen = std::string{screenId}, .route = {}, .params = ui::Value{params}, .as = {}, .call = false});
    }

    void onNavigate(ui::NavigateRequest const& request) override { _navigate(request); }

    void onScreenClosed(ui::ScreenHandle& screen) override { _closed(screen); }

    using DocumentScreenHost::settleCall;

private:
    std::function<void(ui::NavigateRequest)> _navigate;
    std::function<void(ui::ScreenHandle&)> _closed;
};

/// A navigation's params, as a record.
ui::ParamMap paramsOf(ui::NavigateRequest const& request) {
    ui::ValueRecord const* const params = request.params.asRecord();
    return params == nullptr ? ui::ParamMap{} : *params;
}

// notify's role ("info", "success", "warning", "error"; empty for the default) as a Tone, matching Banner::tone's
// own default for an unknown or empty role.
ui::Tone roleToTone(std::string const& role) {
    if (role == "success") {
        return ui::Tone::Ok;
    }
    if (role == "warning") {
        return ui::Tone::Warn;
    }
    if (role == "error") {
        return ui::Tone::Err;
    }
    return ui::Tone::Info;
}

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
        _backend->setOnWidthClass({});
        // The head row's own mount first, since it is built from the Workspace's data and lives in the ShellFrame;
        // then the tabs, then the app document's root the tabs read, then the frame they sit in. The root goes before
        // the document host, whose interpreter runs its onUnmount.
        resetHead();
        _badges.reset();
        _workspace.reset();
        _appRoot.reset();
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
        followWidthClass();
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
            *_session, source.dispatcher(), std::move(hostOptions),
            [this](ui::NavigateRequest request) { navigate(std::move(request)); },
            [this](ui::ScreenHandle& screen) {
                if (_workspace) {
                    _workspace->screenClosed(screen);
                }
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

    /// Writes the backend's width class into the session before the first mount, then each change of it, so every
    /// `{"env": "widthClass"}` in every tab follows the window.
    void followWidthClass() {
        _session->setEnvironmentValue("widthClass", ui::Value{_backend->widthClass()});
        _backend->setOnWidthClass([this, alive = std::weak_ptr<bool>{_alive}](std::string const& widthClass) {
            if (auto const live = alive.lock(); live != nullptr && *live && _session) {
                _session->setEnvironmentValue("widthClass", ui::Value{widthClass});
                if (_workspace) {
                    _workspace->setWidthClass(widthClass);
                }
                if (_frame) {
                    _frame->setCompact(compactFrame(widthClass));
                }
            }
        });
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
                    // After the screens that read it, and before the session changes hands; its counts go first.
                    _badges.reset();
                    _appRoot.reset();
                    _source->signOut(done);
                }
            });
        });
    }

    // The frame and its workspace for the catalog the source holds now: the navigator lists its entries, so a new
    // session's catalog gets a new frame. Every tab of the old one is closed.
    void buildFrame() {
        resetHead();
        _badges.reset();
        _workspace.reset();
        _appRoot.reset();
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
        _frame->setCompact(compactFrame(_backend->widthClass()));
        // Re-wired on every rebuild, since _frame is replaced here but _application is not: the sink must
        // dereference _frame at call time, never capture it directly, so it keeps working across a rebuild.
        _application->setNotifySink([this](std::string const& text, std::string const& role) {
            if (_frame == nullptr) {
                return false;
            }
            _frame->showToast(text, roleToTone(role));
            return true;
        });
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
        tabs.diagnostics = [this](ui::Refusal const& refusal) { problem(refusal.message()); };
        tabs.hosts = &_hosts;
        // The shell mounts the head row itself, in its top bar: an in-page head would show it a second time.
        _workspace = std::make_unique<ui::Workspace>(_context->runtime(), *_backend, *_host, std::move(tabs),
                                                     &_frame->page(), ui::HeadPlacement::External);
        mountApp();
        followBadges();
    }

    // The frame is compact below `expanded`: with its sidebar, an 80-unit `medium` window would leave the page
    // narrower than any `compact` one, and without it the page is nearly the window.
    [[nodiscard]] static bool compactFrame(std::string_view widthClass) { return widthClass != "expanded"; }

    // The app document's navigation counts, copied onto the navigator's entries as they change: made after the root
    // and the frame, and destroyed before either goes. A count for a screen the navigator does not list is reported
    // once.
    void followBadges() {
        if (_appRoot == nullptr) {
            return;
        }
        _badged.clear();
        _badges = std::make_unique<reactive::Effect>(_context->runtime(), [this] {
            std::vector<ui::NavigationBadge> const badges = _appRoot->navigationBadges().get();
            _context->runtime().untracked([this, &badges] { showBadges(badges); });
        });
    }

    // Puts each screen's first visible count on its entry, and takes away the counts no longer given.
    void showBadges(std::vector<ui::NavigationBadge> const& badges) {
        ui::Navigator& navigator = _frame->navigator();
        // A signed-out frame's navigator lists nothing, and its counts are not a mistake of the document's.
        bool const listing = !navigator.menuEntries().empty();
        std::set<std::string, std::less<>> shown;
        for (ui::NavigationBadge const& badge : badges) {
            if (!shown.insert(badge.screen).second) {
                continue;
            }
            bool const listed =
                navigator.setBadge(badge.screen, ui::NavigatorBadge{.text = badge.text, .tone = badge.tone});
            if (!listed && listing && _unlistedBadges.insert(badge.screen).second) {
                problem("the app document's navigation counts " + badge.screen +
                        ", which the navigator does not list");
            }
        }
        for (std::string const& screenId : _badged) {
            if (!shown.contains(screenId)) {
                navigator.setBadge(screenId, std::nullopt);
            }
        }
        _badged = std::move(shown);
    }

    // The app document's root for the frame just built, before its first tab: every screen's `app.*` reads it. A
    // refused one is reported, and the screens mount without it.
    void mountApp() {
        if (!_appDocument) {
            return;
        }
        auto root = _host->mountApp(_appDocument);
        if (!root) {
            problem("the app document: " + root.error().reason);
            return;
        }
        _appRoot = std::move(*root);
    }

    // The sidebar and the top bar, with the principal, while the shell is signed in; the page alone otherwise.
    void showSignedIn() {
        _frame->showSignedIn(_signedIn);
        _frame->setPrincipal(_signedIn ? _source->principal() : std::string{});
    }

    // The top bar and the navigator follow the active tab: the bar's caption is the screen's title, and its entry is
    // the navigator's current one.
    void showActiveTitle() {
        if (!_frame || !_workspace) {
            return;
        }
        std::optional<std::size_t> const shown = _workspace->active();
        auto const tabs = _workspace->tabs();
        _frame->setTitle(shown && *shown < tabs.size() ? tabs.at(*shown).caption : std::string{});
        _frame->navigator().setCurrent(activeScreen());
        showActiveHead();
    }

    // The top bar's head slot follows the active tab, but only re-mounts on a genuine tab change: onChanged also
    // fires on title/dirty/identity changes, and the mounted row is itself reactive to a head's own
    // subtitle/status/actions, so those update in place with no re-mount needed. Comparing the tab's id, not its
    // index: an index is a position in the tab bar, not an identity, and is unsafe to compare across a close (two
    // different tabs can share the same index at different times, and the same tab can shift index without
    // changing).
    void showActiveHead() {
        std::optional<ui::WorkspaceTabId> const active = _workspace->model().active().peek();
        if (active == _headTab) {
            return;
        }
        _headTab = active;
        _titleSpans.reset();
        _headShown.reset();
        ui::HeadParts parts = _workspace->headPartsForActive();
        _headEyebrow.emplace(_context->runtime(), *_backend, std::move(parts.eyebrow), &_frame->headEyebrowSlot());
        _headLead.emplace(_context->runtime(), *_backend, std::move(parts.lead), &_frame->headLeadSlot());
        _head.emplace(_context->runtime(), *_backend, std::move(parts.trail), &_frame->headSlot());
        // The head's slots show while the active tab's head shows anything; the frame cannot tell from the rows
        // they hold.
        _headShown = std::make_unique<reactive::Effect>(
            _context->runtime(), [this] { _frame->setHeadShown(_workspace->activeHeadShown()); });
        // The title's runs follow the screen's state, which a callback on the tab's caption does not hear.
        _titleSpans = std::make_unique<reactive::Effect>(
            _context->runtime(), [this] { _frame->setTitleSpans(_workspace->activeTitleSpans()); });
    }

    // The head row's own mount and its comparison key, always reset together: resetting one without the other would
    // leave the guard above comparing against a tab id from a Workspace instance that no longer exists.
    void resetHead() {
        _titleSpans.reset();
        _headShown.reset();
        _head.reset();
        _headLead.reset();
        _headEyebrow.reset();
        _headTab.reset();
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
        if (std::optional<ui::NavigateRequest> const dropped = std::exchange(_pending, std::nullopt)) {
            settle(*dropped);
        }
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
                    open(*pending);
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

    // A document's navigation. One a sign-in asks for waits for the catalog the sign-in reloads, which lists the
    // screen, and one that replaces it mounts nothing. Otherwise it is posted: the current screen is replaced, which
    // must not happen inside the handler of one of its own elements.
    void navigate(ui::NavigateRequest request) {
        if (_reloading) {
            if (_pending) {
                settle(*_pending);
            }
            _pending = std::move(request);
            return;
        }
        _context->executor().post([alive = std::weak_ptr<bool>{_alive}, this, request = std::move(request)] {
            if (auto const live = alive.lock(); live != nullptr && *live) {
                open(request);
            }
        });
    }

    // Opens what a document's navigation asks for: a document screen as a dialog or a drawer over the tabs when `as`
    // says so, anything else as a tab. A host screen and `as: window` open as a tab, which is reported once. A call
    // whose navigation mounted no document screen (a focused tab, a refused mount, a host screen) is cancelled, so
    // its opener is answered and no later mount answers it.
    void open(ui::NavigateRequest const& request) {
        ui::ParamMap const params = paramsOf(request);
        bool const overlay = request.as == "dialog" || request.as == "drawer";
        bool const hostScreen = isHostScreen(request.screen);
        if (overlay && !hostScreen && _workspace) {
            _message.reset();
            if (auto const shown = _workspace->openOverlay(request); !shown) {
                problem("Cannot open " + shown.error().screenId + ": " + shown.error().reason);
                settle(request);
            }
            return;
        }
        if (overlay && _hostAsOverlay.insert(request.screen).second) {
            problem(request.screen + " is a host screen: it opens as a tab, not as a " + request.as);
        }
        if (request.as == "window" && !_windowReported) {
            _windowReported = true;
            problem("`as: window` opens a tab: this client has one window");
        }
        bool const mounted = mount(request.screen, params);
        if (!mounted || hostScreen) {
            settle(request);
        }
    }

    // Cancels the call of a navigation that mounted no document screen; one that filed none is left alone.
    void settle(ui::NavigateRequest const& request) {
        if (request.call && _host) {
            _host->settleCall(request.screen, paramsOf(request));
        }
    }

    // Whether the catalog names a host screen for @p screenId, as the explorer is.
    [[nodiscard]] bool isHostScreen(std::string_view screenId) const {
        if (screenId == kExplorer) {
            return true;
        }
        ui::CatalogReply const* const catalog = _source == nullptr ? nullptr : _source->catalogReply();
        if (catalog == nullptr) {
            return false;
        }
        auto const entry = std::ranges::find(catalog->entries, screenId, &ui::CatalogEntry::id);
        return entry != catalog->entries.end() && entry->host.has_value() && !entry->host->empty();
    }

    // Opens a screen as a tab, or focuses the tab that already shows it; answers whether a screen was mounted.
    bool mount(std::string const& screenId, ui::ParamMap const& params) {
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
        auto const opened = _workspace->openTab(screenId, params);
        if (!opened) {
            show("Cannot open " + opened.error().screenId + ": " + opened.error().reason);
            return false;
        }
        if (_routes) {
            auto const route = _host->routeOf(screenId, params);
            _routes->shown(route ? *route : std::string{});
        }
        if (_options.onMounted) {
            _options.onMounted(screenId);
        }
        return opened->mounted;
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
        reloadApp(changed);
        ui::ReloadOutcome const outcome = ui::reloadDocuments(
            changed, *_source->bundle(), ui::LoadOptions{.catalog = _source->catalog()}, _documents, *_host, mounted);
        for (ui::Refusal const& refusal : outcome.refused) {
            problem("reload refused: " + refusal.message());
        }
        if (_options.onReloaded) {
            _options.onReloaded(outcome.remounted);
        }
    }

    // A changed app document is loaded and reported; the mounted root keeps the old one, and the new one is mounted
    // at the next frame build, since remounting the root would reset the state every open screen reads.
    void reloadApp(std::vector<std::string> const& changed) {
        if (std::ranges::find(changed, ui::kAppFile) == changed.end()) {
            return;
        }
        auto const text = _source->bundle()->documents.find(ui::kAppFile);
        if (text == _source->bundle()->documents.end()) {
            return;
        }
        auto loaded = ui::loadDocumentText(text->second, ui::LoadOptions{.catalog = _source->catalog()});
        if (!loaded) {
            problem("reload refused: " + loaded.error().message());
            return;
        }
        _appDocument = std::make_shared<ui::Document const>(std::move(*loaded));
        problem("the app document changed; it takes effect at the next sign-in or sign-out");
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
    // The navigation a sign-in asked for, held until the catalog it reloads is current.
    std::optional<ui::NavigateRequest> _pending;
    // The host screens a navigation asked for as a dialog or drawer, each reported once, and whether `as: window`
    // was.
    std::set<std::string, std::less<>> _hostAsOverlay;
    bool _windowReported = false;
    std::map<std::string, std::shared_ptr<ui::Document const>, std::less<>> _documents;
    // The app document: mounted as the session's root scope with each frame, and read by the explorer.
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
    // The app document's root for the current frame; destroyed after the tabs that read it.
    std::unique_ptr<ui::AppRoot> _appRoot;
    // Copies the root's navigation counts onto the frame's navigator; goes before either.
    std::unique_ptr<reactive::Effect> _badges;
    // The screens whose entries hold a count now, and those a count named that the navigator does not list, each
    // reported once.
    std::set<std::string, std::less<>> _badged;
    std::set<std::string, std::less<>> _unlistedBadges;
    std::optional<ui::Mounted> _message;
    // The active tab's head row, mounted into the frame's top-bar slot; reset together with its own comparison key
    // (never one without the other) ahead of _workspace, since both are built from the Workspace's own data and
    // mounted into the ShellFrame.
    std::optional<ui::Mounted> _head;
    // The eyebrow of the head, mounted beside `_head`, the trail.
    std::optional<ui::Mounted> _headEyebrow;
    // The lead of the head, mounted beside `_head`, the trail.
    std::optional<ui::Mounted> _headLead;
    // Shows the frame's head slots while the active tab's head has content; goes with `_head`.
    std::unique_ptr<reactive::Effect> _headShown;
    // Shows the frame's title as the active tab's runs; goes with `_head`.
    std::unique_ptr<reactive::Effect> _titleSpans;
    std::optional<ui::WorkspaceTabId> _headTab;
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

void ApplicationHost::setNotifySink(std::function<bool(std::string const&, std::string const&)> sink) {
    _notifySink = std::move(sink);
}

bool ApplicationHost::notify(std::string const& text, std::string const& role) {
    return _notifySink ? _notifySink(text, role) : InterpreterHost::notify(text, role);
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

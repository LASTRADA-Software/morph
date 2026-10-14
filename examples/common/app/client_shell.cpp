// SPDX-License-Identifier: Apache-2.0

#include "app/client_shell.hpp"

#include <cmath>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <map>
#include <memory>
#include <morph/ui/document.hpp>
#include <morph/ui/host.hpp>
#include <morph/ui/interpreter.hpp>
#include <morph/ui/mount.hpp>
#include <morph/ui/service_client.hpp>
#include <morph/ui/view.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace morph::examples {

namespace {

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
        if (_current) {
            _current->forceClose();
        }
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
            [this] {
                // Every screen goes first, so their private instances are released under the session that owns them;
                // from a posted task, since the screen that signed out is still running its commands.
                reloadThen(false, [this](auto done) {
                    _context->executor().post([this, alive = std::weak_ptr<bool>{_alive}, done = std::move(done)] {
                        if (auto const live = alive.lock(); live != nullptr && *live) {
                            unmount();
                            _source->signOut(done);
                        }
                    });
                });
            });
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
        mount(start, _options.startParams);
    }

    /// Shows @p text in place of any screen, and reports it.
    void show(std::string const& text) {
        problem(text);
        unmount();
        _message.emplace(_context->runtime(), *_backend,
                         ui::text(ui::Text{.text = text, .role = ui::TextRole::Error}));
    }

private:
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
                auto const pending = std::exchange(_pending, std::nullopt);
                if (pending) {
                    mount(pending->first, pending->second);
                    return;
                }
                // A screen that signed in in place, and is still listed, stays as it is, unless it is a public login
                // screen and the new session has a screen of its own to start on.
                std::string const start = firstScreen(signingIn);
                bool const keep = _current && _documents.contains(_currentId) &&
                                  (!_source->isPublic(_currentId) || start.empty() || _source->isPublic(start));
                if (keep) {
                    return;
                }
                mount(start, {});
            });
        });
    }

    void mount(std::string const& screenId, ui::ParamMap const& params) {
        unmount();
        auto handle = _host->mount(screenId, params);
        if (!handle) {
            show("Cannot open " + handle.error().screenId + ": " + handle.error().reason);
            return;
        }
        _current = std::move(*handle);
        _currentId = screenId;
        if (_options.onMounted) {
            _options.onMounted(screenId);
        }
    }

    void unmount() {
        _message.reset();
        if (_current) {
            _current->forceClose();
            _current.reset();
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
    std::unique_ptr<ui::ScreenHandle> _current;
    std::string _currentId;
    std::optional<ui::Mounted> _message;
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

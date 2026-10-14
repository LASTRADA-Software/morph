// SPDX-License-Identifier: Apache-2.0

#include "app/client_shell.hpp"

#include <expected>
#include <functional>
#include <map>
#include <memory>
#include <morph/ui/document.hpp>
#include <morph/ui/host.hpp>
#include <morph/ui/interpreter.hpp>
#include <morph/ui/mount.hpp>
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
        ui::Catalog const catalog = source.catalog();
        for (std::string const& screenId : source.screens()) {
            auto loaded = ui::loadDocumentText(bundle.documents.at(screenId), ui::LoadOptions{.catalog = catalog});
            if (!loaded) {
                problem(loaded.error().message());
                continue;
            }
            _documents.emplace(screenId, std::make_shared<ui::Document const>(std::move(*loaded)));
        }
        std::string start = _options.startScreen;
        if (start.empty() && !_documents.empty()) {
            for (std::string const& screenId : source.screens()) {
                if (_documents.contains(screenId)) {
                    start = screenId;
                    break;
                }
            }
        }
        if (start.empty()) {
            show("The application has no screen this client can open.");
            return;
        }
        ui::SessionOptions sessionOptions;
        sessionOptions.scheduler = &_context->scheduler();
        sessionOptions.catalog = catalog;
        _session = std::make_unique<ui::Session>(_context->runtime(), std::move(sessionOptions));
        ui::DocumentScreenHostOptions hostOptions;
        hostOptions.documents = [this](std::string_view screenId) -> std::shared_ptr<ui::Document const> {
            auto const found = _documents.find(screenId);
            return found == _documents.end() ? nullptr : found->second;
        };
        hostOptions.backend = _backend;
        hostOptions.diagnostics = [this](ui::Refusal const& refusal) { problem(refusal.message()); };
        _host = std::make_unique<Host>(
            *_session, source.dispatcher(), std::move(hostOptions), [this](std::string screenId, ui::ParamMap params) {
                // Posted: the current screen is replaced, which must not happen inside
                // the handler of one of its own elements.
                _context->executor().post([alive = std::weak_ptr<bool>{_alive}, this, screenId = std::move(screenId),
                                           params = std::move(params)] {
                    if (auto const live = alive.lock(); live != nullptr && *live) {
                        mount(screenId, params);
                    }
                });
            });
        mount(start, {});
    }

    /// Shows @p text in place of any screen, and reports it.
    void show(std::string const& text) {
        problem(text);
        unmount();
        _message.emplace(_context->runtime(), *_backend,
                         ui::text(ui::Text{.text = text, .role = ui::TextRole::Error}));
    }

private:
    void mount(std::string const& screenId, ui::ParamMap const& params) {
        unmount();
        auto handle = _host->mount(screenId, params);
        if (!handle) {
            show("Cannot open " + handle.error().screenId + ": " + handle.error().reason);
            return;
        }
        _current = std::move(*handle);
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
    std::map<std::string, std::shared_ptr<ui::Document const>, std::less<>> _documents;
    std::unique_ptr<ui::Session> _session;
    std::unique_ptr<Host> _host;
    std::unique_ptr<ui::ScreenHandle> _current;
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

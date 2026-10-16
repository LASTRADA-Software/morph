// SPDX-License-Identifier: Apache-2.0
//
// The generic client (spec 4 §3): renders any application's screens, fetched from a server (`--server URL`) or linked
// in-process (`--app NAME`), on the frontend `ui::selectFrontend` picks (`--ui=qt|tui`, `MORPH_UI`, or the first
// usable). `--screen ID` picks the first screen; `--exit-after-mount` quits once it mounted, with 0 when it mounted
// without a problem and 1 otherwise. `--dev`, or `MORPH_UI_DEV=1`, reloads the screens when the server's documents
// change, in a client built with the development tools (spec 6 §21).

#include <app/app_source.hpp>
#include <app/client_shell.hpp>
#include <cstddef>
#include <cstdlib>
#include <exception>
#include <expected>
#include <iostream>
#include <memory>
#include <morph/ui/backend.hpp>
#include <morph/ui/dev_tools.hpp>
#include <morph/ui/frontend.hpp>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "local_apps.hpp"

#if MORPH_CLIENT_HAS_QT_QUICK
#include <morph/qt_quick/frontend.hpp>
#ifdef __EMSCRIPTEN__
#include <morph/qt_quick/browser_history.hpp>
#endif
#endif
#if MORPH_CLIENT_HAS_TUI
#include <morph/tui/frontend.hpp>
#endif

namespace {

namespace examples = morph::examples;
namespace ui = morph::ui;

/// The shell factory both frontends use: the client shell, and with `--exit-after-mount`, a quit once it mounted.
struct ShellFactory {
    examples::ClientSource* source = nullptr;
    examples::ClientArgs const* args = nullptr;

    std::unique_ptr<ui::MountedShell> operator()(ui::AppContext& context, ui::IViewBackend& backend,
                                                 std::expected<ui::Bundle, ui::ConnectError> opened) const {
        auto problems = std::make_shared<int>(0);
        auto const mounted = std::make_shared<bool>(false);
        bool const exitAfterMount = args->exitAfterMount;
        examples::ClientShellOptions options{
            .startScreen = args->screen,
            .onProblem =
                [&context, problems, mounted, exitAfterMount](std::string const& problem) {
                    ++*problems;
                    std::cerr << "client: " << problem << '\n';
                    // Nothing will mount: a check that waits for one ends now.
                    if (exitAfterMount && !*mounted) {
                        context.executor().post([&context] { context.quit(1); });
                    }
                },
            .onMounted = {}};
        options.dev = args->dev;
#if MORPH_CLIENT_HAS_QT_QUICK && defined(__EMSCRIPTEN__)
        // The page's history, made once the frontend's event loop exists. It is never destroyed, so its timer never
        // outlives the event loop; a page's runtime does not exit.
        static auto* const history = new morph::qt_quick::BrowserHistory{};  // NOLINT(cppcoreguidelines-owning-memory)
        options.history = history;
#endif
        if (args->exitAfterMount) {
            options.onMounted = [&context, problems, mounted](std::string const& /*screenId*/) {
                *mounted = true;
                // Posted, so the mount's own diagnostics are counted first.
                context.executor().post([&context, problems] { context.quit(*problems == 0 ? 0 : 1); });
            };
        }
        bool const failed = !opened.has_value();
        auto shell = examples::mountClientShell(context, backend, std::move(opened), *source, std::move(options));
        if (failed && args->exitAfterMount) {
            context.quit(1);
        }
        return shell;
    }
};

int run(int argc, char** argv) {
    std::span<char*> const raw{argv, static_cast<std::size_t>(argc)};
    std::vector<std::string> args{raw.begin(), raw.end()};
#ifdef MORPH_CLIENT_DEFAULT_APP
    // A build with no command line, such as the browser's, opens its default application.
    if (args.size() <= 1) {
        args.emplace_back("--app");
        args.emplace_back(MORPH_CLIENT_DEFAULT_APP);
    }
#endif
    auto clientArgs = examples::readClientArgs(args);
    if (!clientArgs) {
        std::cerr << "client: " << clientArgs.error() << '\n';
        return 2;
    }
    // Read once, at start, before any thread that could change the environment exists.
    // NOLINTNEXTLINE(concurrency-mt-unsafe)
    if (char const* const dev = std::getenv("MORPH_UI_DEV"); dev != nullptr && std::string_view{dev} == "1") {
        clientArgs->dev = true;
    }
#if !MORPH_UI_DEV_TOOLS
    if (clientArgs->dev) {
        std::cerr << "client: development mode is not built into this client (MORPH_UI_DEV_TOOLS is off)\n";
        clientArgs->dev = false;
    }
#endif
    auto source = examples::makeSource(*clientArgs, examples::client::localApps());
    if (!source) {
        std::cerr << "client: " << source.error() << '\n';
        return 2;
    }
    ShellFactory const factory{.source = source->get(), .args = &*clientArgs};
    std::vector<ui::FrontendOption> built;
#if MORPH_CLIENT_HAS_QT_QUICK
    built.push_back(morph::qt_quick::frontendOption(
        argc, argv,
        morph::qt_quick::FrontendConfig{
            .mountShell = factory, .components = nullptr, .title = "morph", .onWidthClass = {}}));
#endif
#if MORPH_CLIENT_HAS_TUI
    built.push_back(morph::tui::frontendOption(morph::tui::FrontendConfig{
        .mountShell = factory, .terminal = nullptr, .input = nullptr, .mouse = ::core::tui::MouseTracking::Drag}));
#endif
    auto frontend = ui::selectFrontend(built, args);
    if (!frontend) {
        std::cerr << frontend.error().message() << '\n';
        return 2;
    }
    return (*frontend)->run(**source);
}

}  // namespace

int main(int argc, char** argv) {
    try {
        return run(argc, argv);
    } catch (std::exception const& error) {
        std::cerr << "client: " << error.what() << '\n';
        return 1;
    } catch (...) {
        std::cerr << "client: an unknown failure\n";
        return 1;
    }
}

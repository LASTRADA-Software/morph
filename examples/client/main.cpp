// SPDX-License-Identifier: Apache-2.0
//
// The generic client (spec 4 §3): renders any application's screens, fetched from a server (`--server URL`) or linked
// in-process (`--app NAME`), on the frontend `ui::selectFrontend` picks (`--ui=qt|tui`, `MORPH_UI`, or the first
// usable). `--screen ID` picks the first screen; `--exit-after-mount` quits once it mounted, with 0 when it mounted
// without a problem and 1 otherwise. `--grab FILE.png` (Qt Quick only) sizes the window 1280×900, saves what it draws
// once the first screen mounted, and quits as `--exit-after-mount` does; `--grab-size WIDTHxHEIGHT` sizes it
// otherwise. `--dev`, or `MORPH_UI_DEV=1`, turns development mode on (spec 6 §21): the screens reload when the
// server's documents change, and Ctrl+Shift+I opens the inspector on Qt Quick.

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <expected>
#include <functional>
#include <iostream>
#include <memory>
#include <morph/client/shell.hpp>
#include <morph/client/source.hpp>
#include <morph/ui/backend.hpp>
#include <morph/ui/frontend.hpp>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "local_apps.hpp"

#if MORPH_CLIENT_HAS_QT_QUICK
#include <QCoreApplication>
#include <QEvent>
#include <QImage>
#include <QMouseEvent>
#include <QQuickItem>
#include <QQuickWindow>
#include <QString>
#include <QTimer>
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

#if MORPH_CLIENT_HAS_QT_QUICK
/// How long `--grab` lets the window settle after the first mount.
constexpr std::chrono::milliseconds kGrabSettle{1000};
#endif

/// `--grab`: where the window's image goes, and the window, once the Qt Quick frontend made it.
struct Grab {
    std::string path;
    int width = 1280;
    int height = 900;
    /// Test ids of the widgets to click, in order, before the image is taken: a menu that is open in the grab.
    std::vector<std::string> clicks;
#if MORPH_CLIENT_HAS_QT_QUICK
    QQuickWindow* window = nullptr;
#endif
};

/// Takes `--grab FILE` (or `--grab=FILE`) out of @p args, before the client's own options are read.
/// @return The file; empty when not given, and `nullopt` for a `--grab` without one.
std::optional<std::string> takeGrab(std::vector<std::string>& args) {
    std::string path;
    for (std::size_t index = 1; index < args.size();) {
        std::string_view const arg = args.at(index);
        if (arg == "--") {
            break;
        }
        if (arg == "--grab") {
            if (index + 1 >= args.size()) {
                return std::nullopt;
            }
            path = args.at(index + 1);
            args.erase(args.begin() + static_cast<std::ptrdiff_t>(index),
                       args.begin() + static_cast<std::ptrdiff_t>(index) + 2);
        } else if (arg.starts_with("--grab=")) {
            path = std::string{arg.substr(7)};
            args.erase(args.begin() + static_cast<std::ptrdiff_t>(index));
        } else {
            ++index;
        }
    }
    return path;
}

/// Takes `--grab-size WIDTHxHEIGHT` (or `--grab-size=WIDTHxHEIGHT`) out of @p args into @p grab.
/// @return Whether the size, when given, reads as two positive numbers.
bool takeGrabSize(std::vector<std::string>& args, Grab& grab) {
    for (std::size_t index = 1; index < args.size();) {
        std::string_view const arg = args.at(index);
        if (arg == "--") {
            break;
        }
        std::string size;
        if (arg == "--grab-size" && index + 1 < args.size()) {
            size = args.at(index + 1);
            args.erase(args.begin() + static_cast<std::ptrdiff_t>(index),
                       args.begin() + static_cast<std::ptrdiff_t>(index) + 2);
        } else if (arg.starts_with("--grab-size=")) {
            size = std::string{arg.substr(12)};
            args.erase(args.begin() + static_cast<std::ptrdiff_t>(index));
        } else if (arg == "--grab-size") {
            return false;
        } else {
            ++index;
            continue;
        }
        std::size_t const cross = size.find('x');
        if (cross == std::string::npos) {
            return false;
        }
        std::span<char const> const all{size};
        std::span<char const> const widthText = all.first(cross);
        std::span<char const> const heightText = all.subspan(cross + 1);
        auto const [widthEnd, widthError] =
            std::from_chars(widthText.data(), std::to_address(widthText.end()), grab.width);
        auto const [heightEnd, heightError] =
            std::from_chars(heightText.data(), std::to_address(heightText.end()), grab.height);
        if (widthError != std::errc{} || heightError != std::errc{} || widthEnd != std::to_address(widthText.end()) ||
            heightEnd != std::to_address(heightText.end()) || grab.width <= 0 || grab.height <= 0) {
            return false;
        }
    }
    return true;
}

/// Takes `--grab-click ID[,ID...]` (or `--grab-click=ID[,ID...]`) out of @p args into @p grab.
/// @return Whether the option, when given, has a value.
bool takeGrabClicks(std::vector<std::string>& args, Grab& grab) {
    for (std::size_t index = 1; index < args.size();) {
        std::string_view const arg = args.at(index);
        if (arg == "--") {
            break;
        }
        std::string ids;
        if (arg == "--grab-click" && index + 1 < args.size()) {
            ids = args.at(index + 1);
            args.erase(args.begin() + static_cast<std::ptrdiff_t>(index),
                       args.begin() + static_cast<std::ptrdiff_t>(index) + 2);
        } else if (arg.starts_with("--grab-click=")) {
            ids = std::string{arg.substr(13)};
            args.erase(args.begin() + static_cast<std::ptrdiff_t>(index));
        } else if (arg == "--grab-click") {
            return false;
        } else {
            ++index;
            continue;
        }
        for (std::size_t from = 0; from <= ids.size();) {
            std::size_t const comma = std::min(ids.find(',', from), ids.size());
            if (comma > from) {
                grab.clicks.push_back(ids.substr(from, comma - from));
            }
            from = comma + 1;
        }
    }
    return true;
}

#if MORPH_CLIENT_HAS_QT_QUICK
/// @param item The item to search from.
/// @param name The object name, which is a widget's test id.
/// @return The first item at or under @p item with that name, or null.
QQuickItem* findItem(QQuickItem* item, QString const& name) {
    if (item->objectName() == name) {
        return item;
    }
    for (QQuickItem* const child : item->childItems()) {
        if (QQuickItem* const found = findItem(child, name)) {
            return found;
        }
    }
    return nullptr;
}

/// Clicks the middle of the widget with the test id @p id in the grab's window.
/// @return Whether such a widget was found.
bool clickItem(Grab const& grab, std::string const& id) {
    QQuickItem* const item = findItem(grab.window->contentItem(), QString::fromStdString(id));
    if (item == nullptr) {
        std::cerr << "client: --grab-click: no widget " << id << '\n';
        return false;
    }
    QPointF const at = item->mapToScene(QPointF{item->width() / 2, item->height() / 2});
    QMouseEvent press{QEvent::MouseButtonPress, at, at, Qt::LeftButton, Qt::LeftButton, Qt::NoModifier};
    QCoreApplication::sendEvent(grab.window, &press);
    QMouseEvent release{QEvent::MouseButtonRelease, at, at, Qt::LeftButton, Qt::NoButton, Qt::NoModifier};
    QCoreApplication::sendEvent(grab.window, &release);
    return true;
}
#endif

/// Saves the window's image to the grab's file; reports why not, and answers whether it did.
bool saveGrab(Grab const& grab) {
#if MORPH_CLIENT_HAS_QT_QUICK
    if (grab.window == nullptr) {
        std::cerr << "client: --grab draws the Qt Quick window, and this frontend made none\n";
        return false;
    }
    // The offscreen window's pointer rests at its origin, over whatever sits there; leaving the window clears every
    // hover, so no tooltip is drawn over the grab.
    QEvent leave{QEvent::Leave};
    QCoreApplication::sendEvent(grab.window, &leave);
    QCoreApplication::processEvents();
    QImage const image = grab.window->grabWindow();
    if (image.isNull()) {
        std::cerr << "client: --grab: the window gave no image\n";
        return false;
    }
    if (!image.save(QString::fromStdString(grab.path))) {
        std::cerr << "client: --grab: could not write " << grab.path << '\n';
        return false;
    }
    return true;
#else
    static_cast<void>(grab);
    std::cerr << "client: --grab draws the Qt Quick window, and this client is built without it\n";
    return false;
#endif
}

/// Runs @p then once the window has had time to lay out and draw what was mounted: the event loop polishes the scene
/// and delivers the first answers of the screen's queries in later turns than the mount's, so a grab in the mount's
/// own turn, or the next, shows the items where they were made, unplaced.
void afterLayout(Grab const& grab, std::function<void()> then) {
#if MORPH_CLIENT_HAS_QT_QUICK
    if (grab.window != nullptr) {
        QTimer::singleShot(kGrabSettle, grab.window, std::move(then));
        return;
    }
#else
    static_cast<void>(grab);
#endif
    then();
}

/// Clicks the grab's widgets from @p next on, each given the time to lay out what it opened, then saves the image and
/// quits with 0 when nothing went wrong.
void clickThenSave(ui::AppContext& context, std::shared_ptr<int> problems, std::shared_ptr<Grab> shot,
                   std::size_t next) {
#if MORPH_CLIENT_HAS_QT_QUICK
    if (next < shot->clicks.size() && shot->window != nullptr) {
        if (!clickItem(*shot, shot->clicks.at(next))) {
            context.quit(1);
            return;
        }
        afterLayout(*shot, [&context, problems, shot, next] { clickThenSave(context, problems, shot, next + 1); });
        return;
    }
#else
    static_cast<void>(next);
#endif
    bool const saved = saveGrab(*shot);
    context.quit(*problems == 0 && saved ? 0 : 1);
}

/// The first mount's handler for `--exit-after-mount` and `--grab`: once the first screen mounted, quits with 0 when
/// nothing went wrong, after saving the window's image when @p shot names a file.
std::function<void(std::string const&)> quitOnceMounted(ui::AppContext& context, std::shared_ptr<int> problems,
                                                        std::shared_ptr<bool> mounted, std::shared_ptr<Grab> shot) {
    return [&context, problems = std::move(problems), mounted = std::move(mounted),
            shot = std::move(shot)](std::string const& /*screenId*/) {
        if (*mounted) {
            return;
        }
        *mounted = true;
        // Posted, so the mount's own diagnostics are counted, and its own posted work done, first.
        context.executor().post([&context, problems, shot] {
            if (shot == nullptr) {
                context.quit(*problems == 0 ? 0 : 1);
                return;
            }
            afterLayout(*shot, [&context, problems, shot] { clickThenSave(context, problems, shot, 0); });
        });
    };
}

/// The shell factory both frontends use: the client shell, and with `--exit-after-mount` or `--grab`, a quit once it
/// mounted.
struct ShellFactory {
    morph::client::Source* source = nullptr;
    morph::client::Args const* args = nullptr;
    std::shared_ptr<Grab> grab;

    std::unique_ptr<ui::MountedShell> operator()(ui::AppContext& context, ui::IViewBackend& backend,
                                                 std::expected<ui::Bundle, ui::ConnectError> opened) const {
        auto problems = std::make_shared<int>(0);
        auto const mounted = std::make_shared<bool>(false);
        bool const grabbing = grab != nullptr && !grab->path.empty();
        bool const exitAfterMount = args->exitAfterMount || grabbing;
        morph::client::ShellOptions options{
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
        if (exitAfterMount) {
            options.onMounted = quitOnceMounted(context, problems, mounted, grabbing ? grab : nullptr);
        }
        bool const failed = !opened.has_value();
        auto shell = morph::client::mountShell(context, backend, std::move(opened), *source, std::move(options));
        if (failed && exitAfterMount) {
            context.quit(1);
        }
        return shell;
    }
};

int run(int argc, char** argv) {
    std::span<char*> const raw{argv, static_cast<std::size_t>(argc)};
    std::vector<std::string> args{raw.begin(), raw.end()};
    auto const grab = std::make_shared<Grab>();
    if (std::optional<std::string> path = takeGrab(args)) {
        grab->path = std::move(*path);
    } else {
        std::cerr << "client: --grab needs the file to write\n";
        return 2;
    }
    if (!takeGrabClicks(args, *grab)) {
        std::cerr << "client: --grab-click takes the test ids of the widgets to click, such as shell_nav_menu\n";
        return 2;
    }
    if (!takeGrabSize(args, *grab)) {
        std::cerr << "client: --grab-size takes WIDTHxHEIGHT, such as 390x844\n";
        return 2;
    }
#ifdef MORPH_CLIENT_DEFAULT_APP
    // A build with no command line, such as the browser's, opens its default application.
    if (args.size() <= 1) {
        args.emplace_back("--app");
        args.emplace_back(MORPH_CLIENT_DEFAULT_APP);
    }
#endif
    auto clientArgs = morph::client::readArgs(args);
    if (!clientArgs) {
        std::cerr << "client: " << clientArgs.error() << '\n';
        return 2;
    }
    // Read once, at start, before any thread that could change the environment exists.
    // NOLINTNEXTLINE(concurrency-mt-unsafe)
    if (char const* const dev = std::getenv("MORPH_UI_DEV"); dev != nullptr && std::string_view{dev} == "1") {
        clientArgs->dev = true;
    }
    auto source = morph::client::makeSource(*clientArgs, examples::client::localApps());
    if (!source) {
        std::cerr << "client: " << source.error() << '\n';
        return 2;
    }
    ShellFactory const factory{.source = source->get(), .args = &*clientArgs, .grab = grab};
    std::vector<ui::FrontendOption> built;
#if MORPH_CLIENT_HAS_QT_QUICK
    morph::qt_quick::FrontendConfig qtConfig{.mountShell = factory, .components = nullptr, .title = "morph"};
    qtConfig.devTools = clientArgs->dev ? ui::DevelopmentTools::On : ui::DevelopmentTools::Off;
    if (!grab->path.empty()) {
        qtConfig.onEngine = [grab](QQmlEngine& /*engine*/, QQuickWindow& window) {
            window.resize(grab->width, grab->height);
            grab->window = &window;
        };
    }
    built.push_back(morph::qt_quick::frontendOption(argc, argv, std::move(qtConfig)));
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

// SPDX-License-Identifier: Apache-2.0
//
// The LIMS gallery's render smoke: every screen opened in the Qt Quick renderer, offscreen, through the generic
// client's source and shell, once at each width class. The testkit's smoke mounts each screen once at the window's
// default size; this one resizes the window before each mount, so the backend reports `compact` or `expanded` and the
// shell hands that class to the screen, as a user resizing the window would. It fails when a mount saw another class,
// on any problem the shell reports, and on any Qt warning, as the testkit's does.

#include <QPointF>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickWindow>
#include <QString>
#include <QtGlobal>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <expected>
#include <memory>
#include <morph/client/shell.hpp>
#include <morph/client/source.hpp>
#include <morph/qt_quick/frontend.hpp>
#include <morph/ui/frontend.hpp>
#include <string>
#include <string_view>
#include <testkit/render_smoke.hpp>
#include <utility>
#include <vector>

#include "lims_gallery/lims_gallery.hpp"

namespace {

// Each width class and a window width in units inside it: below 80 units is compact, 160 and over expanded.
constexpr std::array<std::pair<std::string_view, double>, 2> kWidths{{{"compact", 60.0}, {"expanded", 180.0}}};

// The window of the footer probe, in pixels: a phone.
constexpr int kProbeWidth = 390;
constexpr int kProbeHeight = 844;

// How long a mount waits for the backend to report the class its window was resized to.
constexpr int kResizeAttempts = 50;

// QGuiApplication keeps a reference to the count, so both live as long as the process.
int& argCount() {
    static int count = 1;
    return count;
}

char** argValues() {
    static std::array<char, 28> name{"lims_gallery_render_smoke"};
    static std::array<char*, 2> values{name.data(), nullptr};
    return values.data();
}

// The window the frontend shows and its engine, as `onEngine` hands them over.
struct ShownWindow {
    QQuickWindow* window = nullptr;
    QQmlEngine* engine = nullptr;
};

// The window's width in pixels for @p units of the engine's `Theme`.
int pixelsFor(QQmlEngine& engine, double units) {
    auto const* const theme = engine.singletonInstance<QObject*>(QStringLiteral("MorphUi"), QStringLiteral("Theme"));
    double const unitWidth = theme != nullptr ? theme->property("unitWidth").toDouble() : 0.0;
    return static_cast<int>(std::ceil(units * unitWidth)) + 1;
}

// What the smoke saw: each mount is `screen@class`, the class being the one the backend reported as it mounted.
struct WidthReport {
    std::vector<std::string> expected;
    std::vector<std::string> mounted;
    std::vector<std::string> failures;
    int exitCode = -1;
    // The footer probe: the receipt page's Receive button at the probe window's size.
    bool footerFound = false;
    double footerBottom = 0.0;
    double windowHeight = 0.0;
    double unitHeight = 0.0;
};

// The first item under @p root whose object name is @p name, depth first.
// NOLINTNEXTLINE(misc-no-recursion): the walk follows the item tree to any depth.
QQuickItem* findItem(QQuickItem* root, QString const& name) {
    if (root->objectName() == name) {
        return root;
    }
    for (QQuickItem* child : root->childItems()) {
        if (QQuickItem* const found = findItem(child, name); found != nullptr) {
            return found;
        }
    }
    return nullptr;
}

// Mounts each screen at each width in turn, one per settle period, and quits after the last.
class WidthSmokeShell final : public morph::ui::MountedShell {
public:
    WidthSmokeShell(morph::ui::AppContext& context, morph::ui::IViewBackend& backend, morph::client::Source& source,
                    ShownWindow const& shown, WidthReport& report)
        : _context{&context}, _backend{&backend}, _source{&source}, _shown{&shown}, _report{&report} {}

    ~WidthSmokeShell() override = default;
    WidthSmokeShell(WidthSmokeShell const&) = delete;
    WidthSmokeShell& operator=(WidthSmokeShell const&) = delete;
    WidthSmokeShell(WidthSmokeShell&&) = delete;
    WidthSmokeShell& operator=(WidthSmokeShell&&) = delete;

    void start(std::expected<morph::ui::Bundle, morph::ui::ConnectError> opened) {
        _opened = std::move(opened);
        if (!_opened) {
            _report->failures.push_back("the source did not open: " + _opened.error().message);
            _context->quit(1);
            return;
        }
        for (std::string const& screen : _source->screens()) {
            for (auto const& [width, units] : kWidths) {
                _mounts.push_back(Mount{.screen = screen, .width = std::string{width}, .units = units});
                _report->expected.push_back(screen + "@" + std::string{width});
            }
        }
        // Last, so the window is left at the probe's size only after every other mount.
        Mount probeMount{.screen = "lims-gallery.receipt", .width = "compact", .units = 0.0, .probe = true};
        _mounts.emplace_back(std::move(probeMount));
        _report->expected.push_back("lims-gallery.receipt@compact");
        next();
    }

private:
    struct Mount {
        std::string screen;
        std::string width;
        double units = 0.0;
        bool probe = false;
    };

    void next() {
        _shell.reset();
        if (_index == _mounts.size()) {
            _context->quit(0);
            return;
        }
        Mount const& mount = _mounts.at(_index++);
        if (mount.probe) {
            _shown->window->resize(kProbeWidth, kProbeHeight);
        } else {
            _shown->window->resize(pixelsFor(*_shown->engine, mount.units), 900);
        }
        awaitWidth(mount, 0);
    }

    // The backend reports the new class once the resized window has laid out; the shell reads it as it mounts.
    void awaitWidth(Mount const& mount, int attempt) {
        if (_backend->widthClass() != mount.width && attempt < kResizeAttempts) {
            later(std::chrono::milliseconds{10}, [this, &mount, attempt] { awaitWidth(mount, attempt + 1); });
            return;
        }
        std::string const label = mount.screen + "@" + mount.width;
        _shell = morph::client::mountShell(
            *_context, *_backend, _opened, *_source,
            morph::client::ShellOptions{
                .startScreen = mount.screen,
                .onProblem =
                    [this, label](std::string const& problem) { _report->failures.push_back(label + ": " + problem); },
                .onMounted =
                    [this](std::string const& mounted) {
                        _report->mounted.push_back(mounted + "@" + _backend->widthClass());
                    }});
        later(std::chrono::milliseconds{200}, [this, &mount] {
            if (mount.probe) {
                probe();
            }
            next();
        });
    }

    // Reads where the receipt page's Receive button ended up in the probe window.
    void probe() {
        QQuickItem const* const button = findItem(_shown->window->contentItem(), QStringLiteral("receive"));
        _report->footerFound = button != nullptr;
        if (button == nullptr) {
            return;
        }
        _report->footerBottom = button->mapToScene(QPointF{0.0, button->height()}).y();
        _report->windowHeight = _shown->window->height();
        auto const* const theme =
            _shown->engine->singletonInstance<QObject*>(QStringLiteral("MorphUi"), QStringLiteral("Theme"));
        _report->unitHeight = theme != nullptr ? theme->property("unitHeight").toDouble() : 0.0;
    }

    // Runs @p action after @p delay, posted from the timer rather than run in it: the action replaces the timer,
    // which must not be cancelled from inside its own callback.
    template <typename Action>
    void later(std::chrono::milliseconds delay, Action action) {
        _timer = _context->scheduler().after(delay, [this, action, alive = std::weak_ptr<bool>{_alive}] {
            _context->executor().post([action, alive] {
                if (auto const live = alive.lock(); live != nullptr) {
                    action();
                }
            });
        });
    }

    morph::ui::AppContext* _context;
    morph::ui::IViewBackend* _backend;
    morph::client::Source* _source;
    ShownWindow const* _shown;
    WidthReport* _report;
    std::expected<morph::ui::Bundle, morph::ui::ConnectError> _opened;
    std::vector<Mount> _mounts;
    std::size_t _index = 0;
    std::unique_ptr<morph::ui::MountedShell> _shell;
    std::shared_ptr<bool> _alive = std::make_shared<bool>(true);
    morph::ui::TimerHandle _timer;
};

WidthReport runWidthSmoke(morph::client::Source& source) {
    namespace detail = morph::examples::testing::detail;
    WidthReport report;
    std::vector<std::string> warnings;
    detail::smokeWarnings() = &warnings;
    QtMessageHandler const previous = qInstallMessageHandler(&detail::recordQtMessage);
    ShownWindow shown;
    morph::qt_quick::FrontendConfig config;
    config.title = "lims gallery render smoke";
    config.onEngine = [&shown](QQmlEngine& engine, QQuickWindow& window) {
        shown.window = &window;
        shown.engine = &engine;
    };
    config.mountShell = [&source, &shown, &report](morph::ui::AppContext& context, morph::ui::IViewBackend& backend,
                                                   std::expected<morph::ui::Bundle, morph::ui::ConnectError> opened)
        -> std::unique_ptr<morph::ui::MountedShell> {
        auto shell = std::make_unique<WidthSmokeShell>(context, backend, source, shown, report);
        shell->start(std::move(opened));
        return shell;
    };
    morph::qt_quick::Frontend frontend{argCount(), argValues(), std::move(config)};
    report.exitCode = frontend.run(source);
    qInstallMessageHandler(previous);
    detail::smokeWarnings() = nullptr;
    for (std::string& warning : warnings) {
        report.failures.push_back(std::move(warning));
    }
    return report;
}

}  // namespace

TEST_CASE("lims gallery render smoke: every screen at both width classes, offscreen", "[lims-gallery][render-smoke]") {
    morph::client::LocalSource source{lims_gallery::localApp(), morph::client::LocalSetup{}};
    WidthReport const report = runWidthSmoke(source);
    CHECK(report.exitCode == 0);
    CHECK(report.expected.size() >= 4);
    CHECK(report.mounted == report.expected);
    // Last, so each failure is shown with the verdict.
    for (std::string const& failure : report.failures) {
        UNSCOPED_INFO(failure);
    }
    CHECK(report.failures.empty());
    // The compact receipt page is a pinned page in a switch case: its bottom bar sits at the window's bottom, so
    // "Receive" is on the screen at a phone's size. A case that does not stretch cuts the page short, and the bar
    // ends far above the bottom.
    REQUIRE(report.footerFound);
    CHECK(report.footerBottom <= report.windowHeight);
    // The page's padding is a few units.
    CHECK(report.footerBottom >= report.windowHeight - (4.0 * report.unitHeight));
}

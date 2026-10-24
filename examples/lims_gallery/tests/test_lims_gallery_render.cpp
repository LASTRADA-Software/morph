// SPDX-License-Identifier: Apache-2.0
//
// The LIMS gallery's render smoke: every screen opened in the Qt Quick renderer, offscreen, through the generic
// client's source and shell, once at each width class. The testkit's smoke mounts each screen once, with no params;
// this one mounts each at `compact` and at `expanded` through the screens' width param, and fails on any problem the
// shell reports and on any Qt warning, as the testkit's does.

#include <QtGlobal>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstddef>
#include <expected>
#include <memory>
#include <morph/client/shell.hpp>
#include <morph/client/source.hpp>
#include <morph/qt_quick/frontend.hpp>
#include <morph/ui/frontend.hpp>
#include <morph/ui/value.hpp>
#include <string>
#include <string_view>
#include <testkit/render_smoke.hpp>
#include <utility>
#include <vector>

#include "lims_gallery/lims_gallery.hpp"

namespace {

constexpr std::array<std::string_view, 2> kWidths{"compact", "expanded"};

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

// What the smoke saw: each mount is `screen@width`.
struct WidthReport {
    std::vector<std::string> expected;
    std::vector<std::string> mounted;
    std::vector<std::string> failures;
    int exitCode = -1;
};

// Mounts each screen at each width in turn, one per settle period, and quits after the last.
class WidthSmokeShell final : public morph::ui::MountedShell {
public:
    WidthSmokeShell(morph::ui::AppContext& context, morph::ui::IViewBackend& backend, morph::client::Source& source,
                    WidthReport& report)
        : _context{&context}, _backend{&backend}, _source{&source}, _report{&report} {}

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
            for (std::string_view const width : kWidths) {
                _mounts.emplace_back(screen, std::string{width});
                _report->expected.push_back(screen + "@" + std::string{width});
            }
        }
        next();
    }

private:
    void next() {
        _shell.reset();
        if (_index == _mounts.size()) {
            _context->quit(0);
            return;
        }
        auto const [screen, width] = _mounts.at(_index++);
        std::string const label = screen + "@" + width;
        _shell = morph::client::mountShell(
            *_context, *_backend, _opened, *_source,
            morph::client::ShellOptions{
                .startScreen = screen,
                .startParams = morph::ui::ParamMap{{std::string{lims_gallery::kWidthParam}, morph::ui::Value{width}}},
                .onProblem =
                    [this, label](std::string const& problem) { _report->failures.push_back(label + ": " + problem); },
                .onMounted =
                    [this, width](std::string const& mounted) { _report->mounted.push_back(mounted + "@" + width); }});
        // Posted from the timer, not run in it: `next` replaces the timer, which must not be cancelled from inside
        // its own callback.
        _timer =
            _context->scheduler().after(std::chrono::milliseconds{200}, [this, alive = std::weak_ptr<bool>{_alive}] {
                _context->executor().post([this, alive] {
                    if (auto const live = alive.lock(); live != nullptr) {
                        next();
                    }
                });
            });
    }

    morph::ui::AppContext* _context;
    morph::ui::IViewBackend* _backend;
    morph::client::Source* _source;
    WidthReport* _report;
    std::expected<morph::ui::Bundle, morph::ui::ConnectError> _opened;
    std::vector<std::pair<std::string, std::string>> _mounts;
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
    morph::qt_quick::FrontendConfig config;
    config.title = "lims gallery render smoke";
    config.mountShell = [&source, &report](morph::ui::AppContext& context, morph::ui::IViewBackend& backend,
                                           std::expected<morph::ui::Bundle, morph::ui::ConnectError> opened)
        -> std::unique_ptr<morph::ui::MountedShell> {
        auto shell = std::make_unique<WidthSmokeShell>(context, backend, source, report);
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

TEST_CASE("lims gallery render smoke: every screen at both widths, offscreen", "[lims-gallery][render-smoke]") {
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
}

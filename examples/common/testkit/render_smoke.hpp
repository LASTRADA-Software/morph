// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <QMessageLogContext>
#include <QString>
#include <QtGlobal>
#include <app/app_source.hpp>
#include <app/client_shell.hpp>
#include <chrono>
#include <cstddef>
#include <expected>
#include <memory>
#include <morph/qt_quick/frontend.hpp>
#include <morph/ui/frontend.hpp>
#include <string>
#include <utility>
#include <vector>

/// @file
/// The render smoke of spec 4 §4: opens every screen of an application in the Qt Quick renderer, one after another,
/// and reports a source that did not open, a document that did not load, a refused mount, an interpreter diagnostic
/// and every Qt warning (a QML error, a component that failed to load). Header-only: a test that includes it links
/// `morph::qt_quick`; `morph_examples_common` itself does not.

namespace morph::examples::testing {

/// @brief What a render smoke saw.
struct SmokeReport {
    /// @brief Whether the source delivered a bundle.
    bool opened = false;
    /// @brief The screens the source listed.
    std::vector<std::string> screens;
    /// @brief The screens that mounted, in order.
    std::vector<std::string> mounted;
    /// @brief Every problem, in order.
    std::vector<std::string> failures;
    /// @brief What `Frontend::run` returned.
    int exitCode = -1;

    /// @brief Whether every listed screen mounted, at least one did, and nothing went wrong.
    /// @return The verdict.
    [[nodiscard]] bool clean() const {
        return opened && !screens.empty() && mounted == screens && failures.empty() && exitCode == 0;
    }
};

namespace detail {

/// @brief Where the message handler records Qt's warnings while a smoke runs.
inline std::vector<std::string>*& smokeWarnings() {
    static std::vector<std::string>* sink = nullptr;
    return sink;
}

inline void recordQtMessage(QtMsgType type, QMessageLogContext const& /*context*/, QString const& message) {
    if (type != QtDebugMsg && type != QtInfoMsg && smokeWarnings() != nullptr) {
        smokeWarnings()->push_back("Qt: " + message.toStdString());
    }
}

/// @brief Mounts each screen in turn, one per settle period, and quits when the last has been shown.
class SmokeShell final : public ui::MountedShell {
public:
    SmokeShell(ui::AppContext& context, ui::IViewBackend& backend, ClientSource& source, SmokeReport& report,
               std::chrono::milliseconds settle)
        : _context{&context}, _backend{&backend}, _source{&source}, _report{&report}, _settle{settle} {}

    ~SmokeShell() override = default;
    SmokeShell(SmokeShell const&) = delete;
    SmokeShell& operator=(SmokeShell const&) = delete;
    SmokeShell(SmokeShell&&) = delete;
    SmokeShell& operator=(SmokeShell&&) = delete;

    void start(std::expected<ui::Bundle, ui::ConnectError> opened) {
        _opened = std::move(opened);
        _report->opened = _opened.has_value();
        if (!_opened) {
            _report->failures.push_back("the source did not open: " + _opened.error().message);
            _context->quit(1);
            return;
        }
        _report->screens = _source->screens();
        next();
    }

private:
    void next() {
        _shell.reset();
        if (_index == _report->screens.size()) {
            _context->quit(0);
            return;
        }
        std::string const screenId = _report->screens.at(_index++);
        _shell = mountClientShell(
            *_context, *_backend, _opened, *_source,
            ClientShellOptions{
                .startScreen = screenId,
                .onProblem =
                    [this, screenId](std::string const& problem) {
                        _report->failures.push_back(screenId + ": " + problem);
                    },
                .onMounted = [this](std::string const& mounted) { _report->mounted.push_back(mounted); }});
        // Posted from the timer, not run in it: `next` replaces the timer, which must not be cancelled from inside
        // its own callback.
        _timer = _context->scheduler().after(_settle, [this, alive = std::weak_ptr<bool>{_alive}] {
            _context->executor().post([this, alive] {
                if (auto const live = alive.lock(); live != nullptr) {
                    next();
                }
            });
        });
    }

    ui::AppContext* _context;
    ui::IViewBackend* _backend;
    ClientSource* _source;
    SmokeReport* _report;
    std::chrono::milliseconds _settle;
    std::expected<ui::Bundle, ui::ConnectError> _opened;
    std::size_t _index = 0;
    std::unique_ptr<ui::MountedShell> _shell;
    std::shared_ptr<bool> _alive = std::make_shared<bool>(true);
    ui::TimerHandle _timer;
};

}  // namespace detail

/// @brief Runs a Qt Quick frontend over @p source and mounts every screen it lists, each for @p settle.
/// @param argc   The argument count; `QGuiApplication` keeps a reference to it.
/// @param argv   The arguments.
/// @param source The source; the frontend opens and closes it.
/// @param settle How long each screen stays mounted, rendering, before the next.
/// @return The report.
[[nodiscard]] inline SmokeReport runRenderSmoke(int& argc, char** argv, ClientSource& source,
                                                std::chrono::milliseconds settle = std::chrono::milliseconds{200}) {
    SmokeReport report;
    std::vector<std::string> warnings;
    detail::smokeWarnings() = &warnings;
    QtMessageHandler const previous = qInstallMessageHandler(&detail::recordQtMessage);
    qt_quick::FrontendConfig config;
    config.title = "render smoke";
    config.mountShell = [&source, &report, settle](
                            ui::AppContext& context, ui::IViewBackend& backend,
                            std::expected<ui::Bundle, ui::ConnectError> opened) -> std::unique_ptr<ui::MountedShell> {
        auto shell = std::make_unique<detail::SmokeShell>(context, backend, source, report, settle);
        shell->start(std::move(opened));
        return shell;
    };
    qt_quick::Frontend frontend{argc, argv, std::move(config)};
    report.exitCode = frontend.run(source);
    qInstallMessageHandler(previous);
    detail::smokeWarnings() = nullptr;
    for (std::string& warning : warnings) {
        report.failures.push_back(std::move(warning));
    }
    return report;
}

}  // namespace morph::examples::testing

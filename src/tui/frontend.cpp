// SPDX-License-Identifier: Apache-2.0

#include <core/platform/Types.hpp>
#include <core/tui/Screen.hpp>
#include <core/tui/runtime/TuiRuntime.hpp>
#include <memory>
#include <morph/core/io_loop.hpp>
#include <morph/tui/frontend.hpp>
#include <morph/tui/loop_executor.hpp>
#include <optional>
#include <stdexcept>
#include <string>

#include "tui/session.hpp"

namespace morph::tui {

Frontend::Frontend(FrontendConfig config) : _config{config} {}

std::string_view Frontend::name() const { return "tui"; }

// Construction order is destruction order reversed, and it matters: the session (and with it the application)
// goes before the input runtime and the screen, those before the executor, and the executor before the loop.
// Everything is destroyed here, on the driving thread, outside a loop turn.
int Frontend::run(ui::ApplicationFactory const& factory) {
    exec::IoLoop ioLoop{exec::IoLoopDriver::Caller};
    LoopExecutor executor{ioLoop.loop()};
    std::unique_ptr<::core::tui::Terminal> owned;
    auto* terminal = _config.terminal;
    if (terminal == nullptr) {
        owned = std::make_unique<::core::tui::Terminal>();
        terminal = owned.get();
    }
    terminal->setMouseTracking(_config.mouse);
    if (auto const ready = terminal->initialize(); !ready) {
        throw std::runtime_error{"morph::tui::Frontend: the terminal did not initialise: " + ready.error()};
    }
    ::core::tui::Screen screen{*terminal, ::core::tui::ScreenConfig{.alternateScreen = owned != nullptr}};
    std::optional<::core::tui::runtime::TuiRuntime> input;
    if (_config.input != nullptr) {
        input.emplace(ioLoop.loop(), *_config.input);
    } else {
        input.emplace(ioLoop.loop(), *terminal);
    }
    detail::Session session{ioLoop, executor, screen, *input};
    return session.run(factory);
}

ui::FrontendOption frontendOption(FrontendConfig config) {
    return ui::FrontendOption{
        .name = "tui",
        .usable = [] { return ::core::platform::isTerminal(::core::platform::standardInput()); },
        .make = [config] { return std::unique_ptr<ui::Frontend>{std::make_unique<Frontend>(config)}; },
    };
}

}  // namespace morph::tui

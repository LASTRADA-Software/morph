// SPDX-License-Identifier: Apache-2.0

#pragma once
#include <core/tui/MouseTracking.hpp>
#include <core/tui/Terminal.hpp>
#include <core/tui/runtime/InputSource.hpp>
#include <string_view>

#include "../ui/frontend.hpp"

/// @file
/// @brief `morph::tui::Frontend`: a morph application in a terminal.
///
/// Specified in `docs/spec/tui/frontend.md`.

namespace morph::tui {

/// @brief The exit code `Frontend::run` returns when Ctrl+C ended the run: the shell's 128 + SIGINT, so a script can
///        tell a user abort from a normal quit.
inline constexpr int kInterruptExitCode = 130;

/// @brief What a `tui::Frontend` runs on.
struct FrontendConfig {
    /// @brief The terminal to draw on; null means the process's terminal. Either way the frontend sets its mouse
    ///        mode and initialises it; only the process's own terminal is put on the alternate screen and restored.
    ::core::tui::Terminal* terminal = nullptr;
    /// @brief Where input comes from; null means the terminal's own input. A test passes a scripted source.
    ::core::tui::runtime::InputSource* input = nullptr;
    /// @brief How much mouse input to ask the terminal for: `Drag` reports presses, releases and motion while a
    ///        button is held, which drag-and-drop needs; while it is on, a plain click-drag no longer selects
    ///        terminal text (Shift+drag still does in most terminals).
    ::core::tui::MouseTracking mouse = ::core::tui::MouseTracking::Drag;
};

/// @brief Runs a morph application in a terminal, on one thread.
///
/// `run` builds a caller-driven `exec::IoLoop`, a `LoopExecutor` on it (the reactive runtime's owner and every
/// bridge handler's callback executor), the runtime, a `core::tui::Screen` and a `TuiRuntime`, then calls the
/// factory, mounts its view and pumps input until `AppContext::quit()` or the end of input. Sockets, timers,
/// bridge callbacks and input all run on the thread that called `run`. Tab and Shift+Tab move the focus (inside
/// an open dialog only, while one is open); Esc ends a drag wherever the focus is; Ctrl+C quits; a resize re-fits
/// the view. Each input event is followed by a frame before the next one is handled, and redraws are coalesced to
/// one per loop turn.
class Frontend final : public ui::Frontend {
public:
    /// @param config The terminal, the input and the mouse mode; the defaults use the process's terminal.
    explicit Frontend(FrontendConfig config = {});

    /// @brief The name `--ui=` and `MORPH_UI` select this frontend by.
    /// @return `"tui"`.
    [[nodiscard]] std::string_view name() const override;

    /// @brief Runs the application on the calling thread until it quits or the input ends.
    /// @param factory Builds the application from the `ui::AppContext`; called once, after the runtime exists.
    /// @return The exit code given to `AppContext::quit()`; `kInterruptExitCode` (130) when Ctrl+C ended the run,
    ///         0 when the end of input did.
    /// @throws std::runtime_error when the terminal cannot be initialised; std::invalid_argument when the factory
    ///         makes no application.
    int run(ui::ApplicationFactory const& factory) override;

private:
    FrontendConfig _config;
};

/// @brief The TUI's entry for `ui::selectFrontend`.
/// @param config Handed to every `Frontend` the option makes.
/// @return An option named `"tui"`, usable when standard input is a terminal.
[[nodiscard]] ui::FrontendOption frontendOption(FrontendConfig config = {});

}  // namespace morph::tui

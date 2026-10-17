// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <expected>
#include <filesystem>
#include <functional>
#include <memory>
#include <morph/ui/backend.hpp>
#include <morph/ui/frontend.hpp>
#include <morph/ui/host.hpp>
#include <morph/ui/route_history.hpp>
#include <string>
#include <vector>

#include "app/app_source.hpp"

/// @file
/// What the generic client mounts once its source is open: the library's `ShellFrame` (the sidebar with the
/// navigator, the top bar, the page) with a `Workspace` of tabs in its page, each a screen mounted through the
/// library's `DocumentScreenHost`. Signed out, only the page shows, with the start screen.

namespace morph::examples {

/// @brief What a client shell reports, and how it starts.
struct ClientShellOptions {
    /// @brief The screen mounted first; empty for the first the source lists.
    std::string startScreen;
    /// @brief The start screen's params.
    ui::ParamMap startParams;
    /// @brief Called with each problem: why the source did not open, a document that did not load, a refused mount,
    ///        an interpreter diagnostic. Empty ignores them; the shell shows the first three either way.
    std::function<void(std::string const& problem)> onProblem;
    /// @brief Called with the id of each screen once it is mounted.
    std::function<void(std::string const& screenId)> onMounted;
    /// @brief Where a screen's `download` saves a file, under the name the screen suggests; the working directory
    ///        when empty.
    std::filesystem::path downloads;
    /// @brief The history the shown screen's route is kept in (spec 3 §10), such as the browser's: back and forward
    ///        open the route of the entry moved to, and the route it holds at start is opened once the session has
    ///        the screen. Borrowed: it outlives the shell; null keeps no history.
    ui::IHistory* history = nullptr;
    /// @brief Development mode (spec 5 §12): every second the shell polls the source's `ui-hello` digest, and when it
    ///        changed, remounts every open tab whose document changed, keeping its state. A shell built without
    ///        the development tools never polls. A production client leaves it off: the digest is then read once.
    bool dev = false;
    /// @brief Called after each development reload with the ids of the screens it remounted; empty ignores it.
    std::function<void(std::vector<std::string> const& remounted)> onReloaded;
};

/// @brief Mounts the client's shell for what @p source delivered.
///
/// On a bundle: loads each screen of `source.screens()` with the source's catalog facts, opens the start screen as a
/// tab, and on a document's `navigate` or a navigator choice opens the screen asked for as a tab (or focuses the tab
/// that shows it), from a task posted to the owner, never from inside the handler that asked. A sign-in or sign-out
/// closes every tab and rebuilds the frame for the new catalog. On a `ConnectError`, or when no screen can be mounted,
/// shows why as text.
/// @param context The frontend's context.
/// @param backend The view backend the screens mount on. Borrowed: it outlives the shell.
/// @param opened  What the source delivered.
/// @param source  The source; it outlives the shell.
/// @param options The start screen and the observers.
/// @return The shell; destroying it unmounts everything.
[[nodiscard]] std::unique_ptr<ui::MountedShell> mountClientShell(ui::AppContext& context, ui::IViewBackend& backend,
                                                                 std::expected<ui::Bundle, ui::ConnectError> opened,
                                                                 ClientSource& source, ClientShellOptions options);

}  // namespace morph::examples

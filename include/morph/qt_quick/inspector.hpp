// SPDX-License-Identifier: Apache-2.0

#pragma once
#include <functional>
#include <memory>
#include <string>

#include "../ui/dev_tools.hpp"
#include "../ui/inspector.hpp"

/// @file
/// @brief `morph::qt_quick::InspectorWindow`: the development inspector's window (spec 6 §21), opened with
///        Ctrl+Shift+I over the focused screen.
///
/// Built only with `MORPH_UI_DEV_TOOLS`; without it this header declares nothing. Specified in
/// `docs/spec/ui/inspector.md`.

#if MORPH_UI_DEV_TOOLS

class QQuickWindow;

namespace morph::qt_quick {

/// @brief Hands the inspector over the focused screen to a visitor, or calls nothing when no screen is shown:
///        `ui::MountedShell::inspectFocused`, bound to a shell.
using InspectFocused = std::function<void(std::function<void(ui::Inspector& inspector)> const& visit)>;

/// @brief A window that shows the focused screen's mounted nodes with their evaluated properties, its queries and
///        mutations with a refetch button each, its command log and the reactive counters.
///
/// It reads the screen through `ui::Inspector` each time it refreshes, twice a second while it is shown, and keeps
/// nothing of it in between, so a screen that closes or is remounted leaves nothing behind. Its refetch button runs
/// the document's `refetch` command, as a user's event would. Use it on the GUI thread.
class InspectorWindow {
public:
    /// @brief A hidden inspector window.
    /// @param focused Where it finds the focused screen's inspector.
    explicit InspectorWindow(InspectFocused focused);
    ~InspectorWindow();
    InspectorWindow(InspectorWindow const&) = delete;
    InspectorWindow& operator=(InspectorWindow const&) = delete;
    InspectorWindow(InspectorWindow&&) = delete;
    InspectorWindow& operator=(InspectorWindow&&) = delete;

    /// @brief Makes Ctrl+Shift+I in @p window show or hide this inspector, whichever item has focus.
    /// @param window The application's window; it outlives the inspector.
    void attachTo(QQuickWindow& window);

    /// @brief Shows the window if it is hidden, hides it otherwise.
    void toggle();

    /// @brief Hides the window and stops refreshing it.
    void hide();

    /// @brief Whether the window is shown.
    /// @return It.
    [[nodiscard]] bool isVisible() const;

    /// @brief Reads the focused screen again and redraws.
    void refresh();

    /// @brief What the window shows, as plain text, section by section: what a test reads.
    /// @return The text.
    [[nodiscard]] std::string text() const;

    /// @brief What the refetch button of a query does: runs `{"refetch": "<query>"}` on the focused screen.
    /// @param query The query's name.
    void refetch(std::string const& query);

private:
    struct Impl;
    std::unique_ptr<Impl> _impl;
};

}  // namespace morph::qt_quick

#endif

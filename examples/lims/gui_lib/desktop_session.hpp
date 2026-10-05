// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <morph/core/bridge.hpp>
#include <morph/session/session.hpp>
#include <string>
#include <string_view>

/// @file
/// The session the single-user desktop client dispatches under.
///
/// Every mutating lims action refuses an empty principal
/// (`lims::requirePrincipal`), so a client that builds its bridges without a
/// session can do nothing but read. In local mode no authorizer runs, so a
/// bare principal is the whole session; a fixed one is enough because a
/// single-process desktop deployment has no second user to tell apart.
///
/// It lives here rather than in `gui/main.cpp` so the tests can install the
/// same session the executable does, and an action dispatched through it is
/// checked end to end.

namespace lims::gui {

/// @brief The principal the local desktop client dispatches under.
inline constexpr std::string_view kDesktopPrincipal = "demo";

/// @brief Installs the local desktop client's session on @p bridge, before
///        any handler is built against it.
/// @param bridge The bridge every desktop handler dispatches through.
inline void installDesktopSession(::morph::bridge::Bridge& bridge) {
    bridge.setDefaultSession(::morph::session::Context{.principal = std::string{kDesktopPrincipal}});
}

}  // namespace lims::gui

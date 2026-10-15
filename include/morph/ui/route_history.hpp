// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

/// @file
/// @brief Route history (spec 3 §10, spec 6 §20): the route of the screen a shell shows, kept in a history the user
///        moves through with back and forward, such as a browser's.
///
/// Specified in `docs/spec/ui/host.md`, "Route history".

namespace morph::ui {

/// @brief A history of routes: a browser's session history, or a test's.
class IHistory {
public:
    IHistory() = default;
    IHistory(IHistory const&) = delete;
    IHistory& operator=(IHistory const&) = delete;
    IHistory(IHistory&&) = delete;
    IHistory& operator=(IHistory&&) = delete;
    virtual ~IHistory() = default;

    /// @brief The route the current entry holds.
    /// @return It, or empty when the entry holds none.
    [[nodiscard]] virtual std::string current() const = 0;

    /// @brief Adds an entry after the current one, dropping any entries forward of it, and makes it current.
    /// @param route The route it holds.
    virtual void push(std::string const& route) = 0;

    /// @brief Replaces the route the current entry holds.
    /// @param route The route.
    virtual void replace(std::string const& route) = 0;

    /// @brief Sets what is called when the current entry changes from outside: back, forward, or an address the user
    ///        edited. Replaces the previous callback; an empty one stops the calls.
    /// @param popped Called with the new current entry's route, empty when it holds none.
    virtual void onPop(std::function<void(std::string const& route)> popped) = 0;
};

/// @brief The URL fragment that holds a route.
/// @param route The route.
/// @return `#/` followed by the route.
[[nodiscard]] inline std::string fragmentOf(std::string_view route) {
    std::string fragment{"#/"};
    fragment += route;
    return fragment;
}

/// @brief The route a URL fragment holds.
/// @param fragment The fragment, with its `#`.
/// @return The route after `#/`, or empty for a fragment of any other form.
[[nodiscard]] inline std::string routeOfFragment(std::string_view fragment) {
    constexpr std::string_view prefix{"#/"};
    return fragment.starts_with(prefix) ? std::string{fragment.substr(prefix.size())} : std::string{};
}

/// @brief Keeps the route of the screen a shell shows in a history, and opens the route of an entry the user moves
///        to (spec 3 §10).
///
/// The shell opens `initial()` first, when it can, and calls `shown` after it mounts each screen. The first `shown`
/// replaces the entry the history started on, so opening the first screen does not add a step back to an empty page;
/// each later call adds an entry, unless the route is already the current entry's. Moving back or forward calls
/// `open`, which shows the route's screen, or the start screen when the route cannot be opened, before it returns.
/// The first screen it shows replaces the entry instead of adding one, so a refused route is not left in the history;
/// a screen shown after that, such as a redirect, adds an entry as it would anywhere else.
class RouteHistory {
public:
    /// @brief A history over @p history, which it keeps and follows from now on.
    /// @param history The history. Borrowed: it outlives this.
    /// @param open    Shows the screen of a route the user moved to, calling `shown` before it returns. It may destroy
    ///                this.
    RouteHistory(IHistory& history, std::function<void(std::string const& route)> open)
        : _history{&history}, _open{std::move(open)}, _initial{history.current()} {
        _history->onPop([this, alive = std::weak_ptr<bool const>{_alive}](std::string const& route) {
            if (alive.expired()) {
                return;
            }
            bool const outer = std::exchange(_replaceNext, true);
            auto const restore = [&] {
                if (!alive.expired()) {
                    _replaceNext = outer;
                }
            };
            try {
                _open(route);
            } catch (...) {
                restore();
                throw;
            }
            restore();
        });
    }

    RouteHistory(RouteHistory const&) = delete;
    RouteHistory& operator=(RouteHistory const&) = delete;
    RouteHistory(RouteHistory&&) = delete;
    RouteHistory& operator=(RouteHistory&&) = delete;

    // The history keeps its callback, which does nothing once this is gone: clearing it here could clear the
    // callback of a newer route history over the same history.
    ~RouteHistory() = default;

    /// @brief The route the history held when this was made: a reloaded page's, or a shared link's.
    /// @return It, or empty for none.
    [[nodiscard]] std::string const& initial() const noexcept { return _initial; }

    /// @brief The shell now shows a screen whose route is @p route.
    /// @param route The route; empty, for a screen with none, leaves the history as it is, so after a move back or
    ///              forward the next screen with a route replaces the moved-to entry.
    void shown(std::string const& route) {
        if (route.empty()) {
            return;
        }
        bool const replace = std::exchange(_replaceNext, false);
        if (route == _history->current()) {
            return;
        }
        if (replace) {
            _history->replace(route);
        } else {
            _history->push(route);
        }
    }

private:
    IHistory* _history;
    std::function<void(std::string const&)> _open;
    std::string _initial;
    bool _replaceNext = true;
    // Expires with this, so a pop whose `open` destroyed it does not touch it again.
    std::shared_ptr<bool const> _alive = std::make_shared<bool const>(true);
};

}  // namespace morph::ui

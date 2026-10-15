// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// @brief `BrowserHistory`: the browser's session history as an `ui::IHistory`, on WebAssembly only (spec 3 §10).
///
/// Specified in `docs/spec/ui/host.md`, "Route history".

#ifdef __EMSCRIPTEN__

#include <emscripten/val.h>

#include <QObject>
#include <QTimer>
#include <chrono>
#include <functional>
#include <string>
#include <utility>

#include "../ui/route_history.hpp"

namespace morph::qt_quick {

/// @brief The page's history: each entry's route is its URL fragment, `#/<route>`.
///
/// `push` and `replace` go through `history.pushState` and `history.replaceState`, which change the fragment without
/// a `hashchange`. A move back or forward, or an address the user edited, changes the fragment from outside; it is
/// found by reading `location.hash` on a timer on the Qt event loop, so `onPop`'s callback runs there, never inside
/// a JavaScript event handler. Reading it costs one property read per tick; a `popstate` listener would need a
/// JavaScript function bound to C++, which a header cannot register.
class BrowserHistory final : public ui::IHistory {
public:
    /// @brief The page's history, read every @p interval.
    /// @param interval How often the fragment is read.
    explicit BrowserHistory(std::chrono::milliseconds interval = std::chrono::milliseconds{100}) : _seen{hash()} {
        _timer.setInterval(interval);
        QObject::connect(&_timer, &QTimer::timeout, [this] { poll(); });
        _timer.start();
    }

    [[nodiscard]] std::string current() const override { return ui::routeOfFragment(hash()); }

    void push(std::string const& route) override { write("pushState", route); }

    void replace(std::string const& route) override { write("replaceState", route); }

    void onPop(std::function<void(std::string const& route)> popped) override { _popped = std::move(popped); }

private:
    [[nodiscard]] static std::string hash() { return emscripten::val::global("location")["hash"].as<std::string>(); }

    // Skipped while the address holds a move the timer has not reported yet: the user's move wins, and the poll
    // reports it.
    void write(char const* method, std::string const& route) {
        if (hash() != _seen) {
            return;
        }
        emscripten::val::global("history").call<void>(method, emscripten::val::null(), std::string{},
                                                      ui::fragmentOf(route));
        _seen = hash();
    }

    // A copy of the callback: it may replace it while it runs.
    void poll() {
        std::string now = hash();
        if (now == _seen) {
            return;
        }
        _seen = now;
        if (auto const popped = _popped) {
            popped(ui::routeOfFragment(now));
        }
    }

    QTimer _timer;
    std::string _seen;
    std::function<void(std::string const&)> _popped;
};

}  // namespace morph::qt_quick

#endif

// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstddef>
#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "../route_history.hpp"

/// @file
/// @brief `MemoryHistory`: an `IHistory` held in memory, which a test moves through as a user moves through a
///        browser's history.

namespace morph::ui::testing {

/// @brief A history of routes in memory: the entries, the current one, and back, forward and an edited address.
class MemoryHistory final : public IHistory {
public:
    /// @brief A history of one entry.
    /// @param initial The route the first entry holds, as a reloaded page or a shared link has it; empty for none.
    explicit MemoryHistory(std::string initial = {}) { _entries.push_back(std::move(initial)); }

    [[nodiscard]] std::string current() const override { return _entries.at(_index); }

    void push(std::string const& route) override {
        _entries.resize(_index + 1);
        _entries.push_back(route);
        ++_index;
    }

    void replace(std::string const& route) override { _entries.at(_index) = route; }

    void onPop(std::function<void(std::string const& route)> popped) override { _popped = std::move(popped); }

    /// @brief Moves to the previous entry, as the browser's back does.
    /// @return Whether there was one.
    bool back() {
        if (_index == 0) {
            return false;
        }
        --_index;
        pop();
        return true;
    }

    /// @brief Moves to the next entry, as the browser's forward does.
    /// @return Whether there was one.
    bool forward() {
        if (_index + 1 >= _entries.size()) {
            return false;
        }
        ++_index;
        pop();
        return true;
    }

    /// @brief Follows an address the user typed: adds an entry for @p route and reports the change, unless it is the
    ///        current entry's.
    /// @param route The route.
    void follow(std::string const& route) {
        if (route == current()) {
            return;  // a browser reports no change for the address it is already at
        }
        push(route);
        pop();
    }

    /// @brief Every entry, oldest first.
    /// @return They.
    [[nodiscard]] std::vector<std::string> const& entries() const noexcept { return _entries; }

    /// @brief The current entry's index.
    /// @return It.
    [[nodiscard]] std::size_t index() const noexcept { return _index; }

private:
    // A copy of the callback and of the route: the callback may replace either while it runs.
    void pop() const {
        if (auto const popped = _popped) {
            popped(std::string{_entries.at(_index)});
        }
    }

    std::vector<std::string> _entries;
    std::size_t _index = 0;
    std::function<void(std::string const&)> _popped;
};

}  // namespace morph::ui::testing

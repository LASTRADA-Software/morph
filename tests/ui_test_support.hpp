// SPDX-License-Identifier: Apache-2.0

#pragma once
#include <cstdint>
#include <functional>
#include <morph/ui/view.hpp>
#include <optional>
#include <stdexcept>

namespace morph::testing {

/// An integer key, spelled once instead of a `Key` constructor at every use.
inline ::morph::ui::Key intKey(std::int64_t value) { return ::morph::ui::Key{value}; }

/// A factory hook for `EchoingBackend`: while @p spare is engaged, each widget made spends one, and the factory that
/// finds none left throws instead of making its widget. That disengages @p spare, so the next attempt succeeds.
inline std::function<void()> failWhenSpent(std::optional<int>& spare) {
    return [&spare] {
        if (spare.has_value() && (*spare)-- == 0) {
            spare.reset();
            throw std::runtime_error{"the backend could not make the widget"};
        }
    };
}

}  // namespace morph::testing

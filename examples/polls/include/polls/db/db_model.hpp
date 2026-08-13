// SPDX-License-Identifier: Apache-2.0
#pragma once

#ifndef __EMSCRIPTEN__
#include <Lightweight/DataMapper/DataMapper.hpp>

#include <optional>
#endif

/// @file
/// See `pastebin::db::WithMapper`'s file comment
/// (`examples/pastebin/include/pastebin/db/db_model.hpp`) for the full
/// rationale this mixin reuses verbatim, including why
/// `BRIDGE_REGISTER_ACTION_FOR_CLIENT`'s header-avoidance seam applies
/// identically to this rung's `PollModel` but is not adopted here either.

namespace polls::db {

#ifndef __EMSCRIPTEN__

/// @brief Base providing `mapper()` — one lazily-constructed DataMapper per model.
class WithMapper {
protected:
    WithMapper() = default;

    /// @brief Returns this model's DataMapper, opening it on first use.
    [[nodiscard]] Lightweight::DataMapper& mapper() {
        if (!_mapper.has_value()) {
            _mapper.emplace();
        }
        return *_mapper;
    }

private:
    std::optional<Lightweight::DataMapper> _mapper;
};

#else

/// @brief Persistence-free base for the browser build. No `mapper()`.
class WithMapper {
protected:
    WithMapper() = default;
};

#endif

}  // namespace polls::db

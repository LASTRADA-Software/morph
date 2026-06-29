// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <Lightweight/DataMapper/DataMapper.hpp>

#include <optional>

/// @file
/// Small mixin that gives a model a lazily-opened Lightweight `DataMapper`.
///
/// morph runs each model single-threaded on its own strand, so a model can own
/// its own database connection with no synchronisation. The connection is
/// created on first use (i.e. on the strand thread, during the first
/// `execute(...)`) rather than at construction, keeping ODBC handles on the
/// thread that actually uses them.

namespace bank::db {

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

}  // namespace bank::db

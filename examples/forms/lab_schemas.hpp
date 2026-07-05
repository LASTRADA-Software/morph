// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// The `{actionType: schema}` object shared by every forms client (HTML page,
/// REPL `--schemas`, QML renderer). One source; all renderers see the same
/// contract. A remote deployment would serve this from a `describe` endpoint
/// instead of compiling it in.

#include <string>

#include "lab_model.hpp"

namespace lab {

/// @brief Schemas of every LabModel action, keyed by action type id.
[[nodiscard]] inline std::string schemasJson() {
    std::string out;
    out += "{\"ComputeDryDensity\":";
    out += morph::forms::schemaJson<ComputeDryDensity>();
    out += ",\"RecordMeasurement\":";
    out += morph::forms::schemaJson<RecordMeasurement>();
    out += "}";
    return out;
}

}  // namespace lab

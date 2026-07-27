// SPDX-License-Identifier: Apache-2.0

#pragma once
#include <string_view>

/// @brief morph's major version component (semantic-versioning MAJOR:
/// incremented for a breaking source change to the stable public surface —
/// see docs/spec/VERSIONING.md).
#define MORPH_VERSION_MAJOR 0

/// @brief morph's minor version component (semantic-versioning MINOR:
/// incremented for additive, source-compatible changes).
#define MORPH_VERSION_MINOR 1

/// @brief morph's patch version component (semantic-versioning PATCH: bug
/// fixes and doc corrections that change neither the stable surface nor its
/// documented behavior).
#define MORPH_VERSION_PATCH 0

/// @brief Packs (major, minor, patch) into one comparable integer
/// (`major * 10000 + minor * 100 + patch`), for `#if`-testable feature
/// checks against a specific morph release, e.g.
/// `#if MORPH_VERSION >= MORPH_MAKE_VERSION(1, 2, 0)`.
/// @param major Major version component.
/// @param minor Minor version component.
/// @param patch Patch version component.
#define MORPH_MAKE_VERSION(major, minor, patch) ((major) * 10000 + (minor) * 100 + (patch))

/// @brief morph's current release, packed via `MORPH_MAKE_VERSION`. Mirrors
/// the `VERSION` field of the top-level `project(morph VERSION ...)` in
/// `CMakeLists.txt` — the two are cross-checked by `tests/test_version.cpp`.
#define MORPH_VERSION MORPH_MAKE_VERSION(MORPH_VERSION_MAJOR, MORPH_VERSION_MINOR, MORPH_VERSION_PATCH)

namespace morph::version {

/// @brief morph's major version component, as a compile-time constant. See `MORPH_VERSION_MAJOR`.
inline constexpr int kMajor = MORPH_VERSION_MAJOR;

/// @brief morph's minor version component, as a compile-time constant. See `MORPH_VERSION_MINOR`.
inline constexpr int kMinor = MORPH_VERSION_MINOR;

/// @brief morph's patch version component, as a compile-time constant. See `MORPH_VERSION_PATCH`.
inline constexpr int kPatch = MORPH_VERSION_PATCH;

/// @brief morph's current release as a dotted string, e.g. `"0.1.0"`.
inline constexpr std::string_view kString = "0.1.0";

}  // namespace morph::version

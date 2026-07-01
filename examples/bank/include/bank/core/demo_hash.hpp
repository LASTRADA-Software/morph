// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <format>
#include <functional>
#include <string>
#include <string_view>

/// @file
/// One demo-grade hash shared by the PIN and password paths.

namespace bank {

/// @brief Demo-grade, **non-secure** hash of @p material.
///
/// A real app would use a slow, salted KDF (Argon2/bcrypt). Centralised here so
/// the PIN and password hashes cannot drift onto different implementations when
/// this is later upgraded — both go through this one function. Callers are
/// responsible for salting @p material (e.g. with a username or a field tag).
[[nodiscard]] inline std::string demoHash(std::string_view material) {
    return std::format("{:016x}", std::hash<std::string>{}(std::string{material}));
}

}  // namespace bank

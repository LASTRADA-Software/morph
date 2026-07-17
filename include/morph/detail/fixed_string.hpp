// SPDX-License-Identifier: Apache-2.0

#pragma once

/// @file detail/fixed_string.hpp
/// @brief Shared compile-time, NTTP-capable fixed string.

#include <array>
#include <cstddef>
#include <string_view>

namespace morph::detail {

/// @brief A structural, NTTP-capable compile-time string.
///
/// A literal type with public array storage, so it satisfies the rules for a
/// non-type template parameter. Two `FixedString` values of the same length
/// with the same characters compare equal as template arguments, so a type
/// parameterised on a `FixedString` written identically in two translation
/// units is one and the same type.
///
/// This is the single canonical definition shared by the forms layer
/// (`morph::forms::FixedString`, used by `Choice`) and the units layer
/// (`morph::units::detail::FixedString`, used by `NamedQuantity`), which alias
/// it rather than redefining their own copies.
///
/// @tparam N Storage size including the terminating null.
template <std::size_t N>
struct FixedString {
    /// @brief Character storage (null-terminated).
    std::array<char, N> data{};

    /// @brief Captures a string literal.
    /// @param literal The literal to copy, e.g. `"ListSamples"`.
    // NOLINTNEXTLINE(modernize-avoid-c-arrays, cppcoreguidelines-avoid-c-arrays, cppcoreguidelines-pro-bounds-constant-array-index, cppcoreguidelines-pro-bounds-avoid-unchecked-container-access) — string literals ARE C arrays.
    consteval FixedString(const char (&literal)[N]) noexcept {
        for (std::size_t index = 0; index < N; ++index) {
            // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-constant-array-index, cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
            data[index] = literal[index];
        }
    }

    /// @brief A view of the string (without the terminating null).
    /// @return The string contents.
    [[nodiscard]] constexpr std::string_view view() const noexcept { return {data.data(), N - 1}; }
};

}  // namespace morph::detail

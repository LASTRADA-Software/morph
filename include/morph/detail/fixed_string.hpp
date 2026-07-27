// SPDX-License-Identifier: Apache-2.0

#pragma once

/// @file detail/fixed_string.hpp
/// @brief Shared compile-time, NTTP-capable fixed string.

#include <array>
#include <concepts>
#include <cstddef>
#include <string>
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
/// This is the single canonical definition. The forms layer aliases it twice —
/// as `morph::forms::FixedString` (used by `Choice`) and as
/// `morph::forms::detail::LiteralString` (which captures an `equals` rule
/// literal) — and the units layer once, as
/// `morph::units::detail::FixedString` (used by `NamedQuantity`). None of them
/// redefine their own copy.
///
/// @tparam N Storage size including the terminating null.
template <std::size_t N>
struct FixedString {
    /// @brief Character storage (null-terminated).
    std::array<char, N> data{};

    /// @brief Captures a string literal.
    ///
    /// `constexpr`, not `consteval`: as an NTTP this is still only ever
    /// evaluated at compile time (that context demands it regardless), but
    /// `constexpr` additionally lets a *function parameter* of type
    /// `const char (&)[N]` be captured — `morph::forms::equals(&A::code, "X")`
    /// forwards its literal exactly that way, and a `consteval` constructor
    /// cannot be called with a reference parameter, which is not itself a
    /// constant expression.
    ///
    /// @param literal The literal to copy, e.g. `"ListSamples"`.
    // A NOLINTNEXTLINE directive must sit on ONE physical line to apply to the
    // next one; wrapped, it silently annotates the comment instead. Hence the
    // long lines below, and the clang-format guard around them.
    // String literals ARE C arrays — that is the whole point of this type.
    // clang-format off
    // NOLINTNEXTLINE(modernize-avoid-c-arrays, cppcoreguidelines-avoid-c-arrays, cppcoreguidelines-pro-bounds-constant-array-index, cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    constexpr FixedString(const char (&literal)[N]) noexcept {
        for (std::size_t index = 0; index < N; ++index) {
            // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-constant-array-index, cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
            data[index] = literal[index];
        }
    }
    // clang-format on

    /// @brief A view of the string (without the terminating null).
    /// @return The string contents.
    [[nodiscard]] constexpr std::string_view view() const noexcept { return {data.data(), N - 1}; }

    /// @brief Compares against any string-like value.
    ///
    /// A hidden friend, so it is reachable by ADL from wherever a
    /// `FixedString` is compared without polluting ordinary name lookup. Being
    /// declared here rather than in an aliasing namespace matters: ADL keys on
    /// the *type's* namespace (`morph::detail`), which an alias such as
    /// `morph::forms::LiteralString` does not change.
    ///
    /// @tparam S String-like operand type (`std::string` or `std::string_view`).
    /// @param lhs Value to compare.
    /// @param rhs Captured literal to compare against.
    /// @return `true` if both hold the same characters.
    template <typename S>
        requires std::same_as<S, std::string> || std::same_as<S, std::string_view>
    [[nodiscard]] friend constexpr bool operator==(const S& lhs, const FixedString& rhs) noexcept {
        return std::string_view{lhs} == rhs.view();
    }
};

}  // namespace morph::detail

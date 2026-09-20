// SPDX-License-Identifier: Apache-2.0
//
// Fixture for scripts/check_nolint_directives.sh: every shape the gate must
// ACCEPT. Not compiled -- it is scanned as text.

#pragma once

#include <cstddef>

namespace morph::lint_fixture {

// The reason sits above the directive, so the directive is the last comment
// line before the code. This is the preferred remedy and the shape the four
// #627 sites were converted to.
// NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
inline int firstOf(const int* values) { return values[0]; }

// The reason shares the directive's physical line, guarded so clang-format
// cannot wrap it. This is fixed_string.hpp's shape.
// clang-format off
// NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access) -- index bounded by the caller's contract
inline int at(const int* values, std::size_t index) { return values[index]; }
// clang-format on

// Prose that NAMES the directive without being one. The gate anchors on
// `// NOLINTNEXTLINE` at the start of the comment, so a sentence like this --
// which is what include/morph/detail/fixed_string.hpp:48 is, and which is the
// in-tree documentation of the whole hazard -- must not be flagged even though
// the line that follows it is another comment.
//
// A NOLINTNEXTLINE directive must sit on ONE physical line to apply to the
// next one; wrapped, it silently annotates the comment instead.
inline int identity(int value) { return value; }

// NOLINTBEGIN and NOLINTEND are not line-scoped and are out of this gate's
// scope; a wrapped reason after either of them is harmless.
// NOLINTBEGIN(readability-identifier-length)
// The block form applies until NOLINTEND regardless of what follows it on the
// next line, so this comment costs nothing.
inline int id(int v) { return v; }
// NOLINTEND(readability-identifier-length)

}  // namespace morph::lint_fixture

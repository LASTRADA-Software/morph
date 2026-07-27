#pragma once
// Fixture for scripts/check_deprecated_markers.sh: a message spanning lines
// that names neither a removal version nor a replacement. A line-scoped
// checker cannot see this one at all. The checker must reject this file.
// Never compiled.
//
// clang-format must not rejoin the marker onto one line -- the line split is
// the property under test.

namespace morph::lint_fixture {

// clang-format off
[[deprecated(
    "this message explains nothing "
    "a caller could act on")]]
void wrappedButUseless();
// clang-format on

}  // namespace morph::lint_fixture

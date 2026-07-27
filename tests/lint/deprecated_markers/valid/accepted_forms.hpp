#pragma once
// Fixture for scripts/check_deprecated_markers.sh. Every `[[deprecated]]`
// marker below is well-formed per docs/spec/VERSIONING.md; the checker must
// accept this file. Never compiled -- it exists only to be scanned.

namespace morph::lint_fixture {

/// Plain single-line marker.
struct Simple {
    [[deprecated("removed in 2.0.0; use morph::bridge::NewThing instead")]]
    void old();
};

/// Two-component version (patch omitted).
[[deprecated("removed in 1.2; use morph::core::Replacement instead")]]
void twoComponentVersion();

/// Combined with another attribute in one list.
[[nodiscard, deprecated("removed in 3.0.0; use betterName instead")]]
int combinedAttributeList();

/// Wrapped across lines with adjacent string-literal concatenation -- the
/// shape a message long enough to exceed the column limit actually takes.
/// clang-format must not rejoin it: the line split is the property under test.
// clang-format off
[[deprecated(
    "removed in 3.1.4; use the considerably longer replacement spelling "
    "morph::core::SomethingElseEntirely instead")]]
void wrappedMessage();
// clang-format on

// A comment may show the required shape as an example without tripping the
// checker: [[deprecated("removed in 9.9.9; use nothing instead")]] and even a
// bare [[deprecated]] in prose are ignored on comment lines.

}  // namespace morph::lint_fixture

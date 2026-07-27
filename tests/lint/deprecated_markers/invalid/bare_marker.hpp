#pragma once
// Fixture for scripts/check_deprecated_markers.sh: the bare form carries no
// removal version and no replacement, which docs/spec/VERSIONING.md forbids.
// The checker must reject this file. Never compiled.

namespace morph::lint_fixture {

[[deprecated]] void noMessageAtAll();

}  // namespace morph::lint_fixture

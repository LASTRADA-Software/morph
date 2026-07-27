#pragma once
// Fixture for scripts/check_deprecated_markers.sh: bare marker hiding in a
// combined attribute list. The checker must reject this file. Never compiled.

namespace morph::lint_fixture {

[[deprecated, nodiscard]] int noMessageBesideAnotherAttribute();

}  // namespace morph::lint_fixture

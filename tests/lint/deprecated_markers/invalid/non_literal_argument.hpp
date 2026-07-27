#pragma once
// Fixture for scripts/check_deprecated_markers.sh: the message is hidden
// behind a macro, so its format cannot be verified by scanning. Unverifiable
// must not mean accepted. The checker must reject this file. Never compiled.

#define MORPH_LINT_FIXTURE_MSG "removed in 2.0.0; use somethingElse instead"

namespace morph::lint_fixture {

[[deprecated(MORPH_LINT_FIXTURE_MSG)]] void messageBehindAMacro();

}  // namespace morph::lint_fixture

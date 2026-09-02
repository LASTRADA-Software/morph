// Fixture for scripts/check_automoc_includes.sh: the shape moc emits with
// CMAKE_AUTOMOC_PATH_PREFIX on when the header sits directly in one of the
// target's include directories. Resolves through -I, cannot ascend.
//
// One include only: this file is tracked, so ci.yml's clang-format job checks
// it, and .clang-format regroups and sorts include blocks.
#include "budget_presenter.hpp"

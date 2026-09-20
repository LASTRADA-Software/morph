// SPDX-License-Identifier: Apache-2.0
//
// Fixture for scripts/check_nolint_directives.sh: the #627 shape. The reason is
// wrapped onto a second comment line, so the directive annotates that comment
// and the finding on the statement below leaks. Not compiled -- scanned as text.

#include <cstdlib>

void* allocate(unsigned long size) {
    // NOLINTNEXTLINE(cppcoreguidelines-no-malloc, cppcoreguidelines-owning-memory) --
    // this *is* the process-wide operator new/delete pair
    return std::malloc(size);
}

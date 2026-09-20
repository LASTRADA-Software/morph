// SPDX-License-Identifier: Apache-2.0
//
// Fixture for scripts/check_nolint_directives.sh: a directive separated from the
// code it means to annotate by a blank line. Same defect as the wrapped-reason
// case, different cause -- an edit inserted the blank rather than the formatter
// wrapping the line. Not compiled -- scanned as text.

#include <cstdlib>

void* allocate(unsigned long size) {
    // NOLINTNEXTLINE(cppcoreguidelines-no-malloc)

    return std::malloc(size);
}

// SPDX-License-Identifier: Apache-2.0
//
// Fixture for scripts/check_nolint_directives.sh: a directive as the final line
// of a file, annotating nothing at all -- what is left when the code it guarded
// is deleted and the directive is not. Not compiled -- scanned as text.

#include <cstdlib>

void* allocate(unsigned long size) { return std::malloc(size); }

// NOLINTNEXTLINE(cppcoreguidelines-no-malloc)

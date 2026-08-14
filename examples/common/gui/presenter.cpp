// SPDX-License-Identifier: Apache-2.0
#include "gui/presenter.hpp"

// Q_OBJECT (via the header) needs at least one non-header translation unit in
// its target for moc's generated file to link against; this file exists for
// that reason even though Presenter's own logic is fully inline above.

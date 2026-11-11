// SPDX-License-Identifier: Apache-2.0
//
// The control for the inventory_includes_* probes: compiled as part of lims/inventory, it includes the posting
// contract inventory uses and must build, which shows the probes' header path is right and the refusals beside it are
// the boundary.
#ifdef LIMS_MODULE_BOUNDARY_PROBE
#include <lims/posting/posting.hpp>
#endif

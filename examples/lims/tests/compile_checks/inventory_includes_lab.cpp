// SPDX-License-Identifier: Apache-2.0
//
// Compiled as part of lims/inventory, it includes the lab's header and must fail to build: inventory knows nothing of
// samples. Outside its test the macro is undefined and this is empty.
#ifdef LIMS_MODULE_BOUNDARY_PROBE
#include <lims/lab/models/sample_model.hpp>
#endif

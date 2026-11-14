// SPDX-License-Identifier: Apache-2.0
//
// Compiled as part of lims/org by the test lims.module_boundary.org_includes_lab, which passes only when this fails
// because no other module's headers are on a shared module's include path. Outside that test the macro is undefined
// and this is an empty translation unit.
#ifdef LIMS_MODULE_BOUNDARY_PROBE
#include <lims/lab/models/sample_model.hpp>
#endif

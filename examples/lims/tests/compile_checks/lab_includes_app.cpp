// SPDX-License-Identifier: Apache-2.0
//
// Compiled as part of lims/lab by the test lims.module_boundary.lab_includes_app, which passes only when this fails
// because lims/app's headers are not on lims/lab's include path. Outside that test the macro is undefined and this
// is an empty translation unit, so a tool that analyses every changed source sees nothing to report.
#ifdef LIMS_MODULE_BOUNDARY_PROBE
#include <lims/app/lims_authorizer.hpp>
#endif

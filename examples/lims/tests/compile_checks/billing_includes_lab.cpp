// SPDX-License-Identifier: Apache-2.0
//
// Compiled as part of lims/billing, it includes lab's header and must fail to build: billing knows lab only through
// the ids org shares. Outside its test the macro is undefined and this is empty.
#ifdef LIMS_MODULE_BOUNDARY_PROBE
#include <lims/lab/models/sample_model.hpp>
#endif

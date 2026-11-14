// SPDX-License-Identifier: Apache-2.0
//
// Compiled as part of lims/billing, it includes inventory's header and must fail to build: billing knows nothing of
// stock. Outside its test the macro is undefined and this is empty.
#ifdef LIMS_MODULE_BOUNDARY_PROBE
#include <lims/inventory/models/stock_model.hpp>
#endif

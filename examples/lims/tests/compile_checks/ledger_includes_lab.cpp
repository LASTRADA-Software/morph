// SPDX-License-Identifier: Apache-2.0
//
// Compiled as part of lims/ledger, it includes lab's header and must fail to build: the ledger knows no other feature
// module. Outside its test the macro is undefined and this is empty.
#ifdef LIMS_MODULE_BOUNDARY_PROBE
#include <lims/lab/models/sample_model.hpp>
#endif

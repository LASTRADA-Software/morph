// SPDX-License-Identifier: Apache-2.0
//
// Compiled as part of lims/posting, it includes a feature module's header and must fail to build: the posting
// contract is shared, so it may depend on org alone. Outside its test the macro is undefined and this is empty.
#ifdef LIMS_MODULE_BOUNDARY_PROBE
#include <lims/lab/models/sample_model.hpp>
#endif

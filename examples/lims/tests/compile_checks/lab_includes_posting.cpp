// SPDX-License-Identifier: Apache-2.0
//
// Compiled as part of lims/lab, it includes the posting contract and must fail to build: lab posts nothing, so its
// USES names org alone and posting is not on its include path. Outside its test the macro is undefined and this is
// empty.
#ifdef LIMS_MODULE_BOUNDARY_PROBE
#include <lims/posting/posting.hpp>
#endif

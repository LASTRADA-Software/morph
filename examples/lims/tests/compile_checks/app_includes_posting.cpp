// SPDX-License-Identifier: Apache-2.0
//
// The control for lab_includes_posting: compiled as part of lims/app, which links every module, it includes the
// posting contract and must build, which shows the probe's header path is right and the refusal above is the link
// rule's. Outside its test the macro is undefined and this is empty.
#ifdef LIMS_MODULE_BOUNDARY_PROBE
#include <lims/posting/posting.hpp>
#endif

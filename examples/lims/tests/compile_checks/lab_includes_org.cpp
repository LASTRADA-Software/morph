// SPDX-License-Identifier: Apache-2.0
//
// The control for the two boundary probes beside it: compiled as part of lims/lab, it includes a shared module's
// header and must build. It shows the probes compile through the module's real include path, so their failures are
// the boundary and not a probe that can never build. Outside its test the macro is undefined and this is empty.
#ifdef LIMS_MODULE_BOUNDARY_PROBE
#include <lims/org/core/errors.hpp>
#endif

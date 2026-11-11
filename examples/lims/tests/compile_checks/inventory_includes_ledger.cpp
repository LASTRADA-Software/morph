// SPDX-License-Identifier: Apache-2.0
//
// Compiled as part of lims/inventory, it includes the ledger's header and must fail to build: inventory reaches the
// ledger only through the posting contract. Outside its test the macro is undefined and this is empty.
#ifdef LIMS_MODULE_BOUNDARY_PROBE
#include <lims/ledger/models/ledger_model.hpp>
#endif

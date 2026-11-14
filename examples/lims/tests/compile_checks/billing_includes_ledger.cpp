// SPDX-License-Identifier: Apache-2.0
//
// Compiled as part of lims/billing, it includes the ledger's header and must fail to build: billing reaches the ledger
// only through the posting contract and the app's port. Outside its test the macro is undefined and this is empty.
#ifdef LIMS_MODULE_BOUNDARY_PROBE
#include <lims/ledger/models/ledger_model.hpp>
#endif

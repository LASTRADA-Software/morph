// SPDX-License-Identifier: Apache-2.0
//
// Compiled as part of lims/ledger, it includes billing's header and must fail to build: the ledger knows nothing of
// invoices. Outside its test the macro is undefined and this is empty.
#ifdef LIMS_MODULE_BOUNDARY_PROBE
#include <lims/billing/models/price_list_model.hpp>
#endif

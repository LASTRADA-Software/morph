// SPDX-License-Identifier: Apache-2.0
//
// The people the shell tests sign in, seeded in a translation unit of their own so that the tests that sign them in
// need not include the models that seeding calls.
#pragma once

namespace lims::shelltest {

/// @brief The lab as the scenario has it: anna supervises the seeded company, the scenario's people hold their roles,
///        and the first company julia sees has one account.
void seedPeople();

}  // namespace lims::shelltest

// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// The mockups' scenario (`examples/lims/design/mockups/README.md`): a German contract laboratory and its Japanese
/// subsidiary, their people, and each module's records, written through the models so the grabs show the mockups'
/// figures.

namespace lims::screens {

/// @brief Writes the scenario unless it is already there.
///
/// Does nothing when a company named "Rheinlab Analytik GmbH" exists. Otherwise it writes the organisation, then the
/// laboratory, billing, ledger and inventory parts in that order, each record as the person the scenario names. The
/// first supervisor, `anna`, must already be appointed (`prepareLocal` does it).
void seedScenario();

}  // namespace lims::screens

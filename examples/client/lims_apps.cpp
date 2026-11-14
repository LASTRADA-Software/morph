// SPDX-License-Identifier: Apache-2.0
//
// morph_lims_client links lims alone for local mode, so scripts/shoot_lims.sh opens lims without the other rungs.
// lims registers its own action ids (`ListLimsSamples`, `ListLedgerAccounts`) because the registry refuses one id on
// two action types and examples/forms and examples/bank register `ListSamples` and `ListAccounts`.

#include <lims_screens.hpp>
#include <morph/client/source.hpp>
#include <string>
#include <vector>

#include "lims/lab/db/database.hpp"
#include "local_apps.hpp"

namespace morph::examples::client {

std::vector<::morph::client::LocalApp> localApps() {
    return {::morph::client::LocalApp{
        .name = "lims", .bundle = &lims::screens::bundle, .setUp = [](::morph::client::LocalSetup const& setup) {
            std::string const file = setup.db.empty() ? std::string{"lims.db"} : setup.db;
            lims::db::setup("DRIVER=SQLite3;Database=" + file);
            lims::screens::prepareLocal(setup.user);
        }}};
}

}  // namespace morph::examples::client

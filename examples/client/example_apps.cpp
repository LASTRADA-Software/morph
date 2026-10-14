// SPDX-License-Identifier: Apache-2.0
//
// morph_examples_client links every example application for local mode.

#include <app/app_source.hpp>
#include <forms_screens.hpp>
#include <gallery/gallery.hpp>
#include <vector>

#if MORPH_CLIENT_HAS_BANK
#include <bank_screens.hpp>
#include <string>

#include "bank/db/database.hpp"
#endif

#if MORPH_CLIENT_HAS_LIMS
#include <lims_screens.hpp>
#include <string>

#include "lims/db/database.hpp"
#endif

#include "local_apps.hpp"

namespace morph::examples::client {

std::vector<LocalApp> localApps() {
    std::vector<LocalApp> apps{gallery::localApp()};
    apps.push_back(LocalApp{.name = "forms", .bundle = &lab::screens::bundle, .setUp = {}});
#if MORPH_CLIENT_HAS_BANK
    apps.push_back(LocalApp{.name = "bank", .bundle = &bank::screens::bundle, .setUp = [](LocalSetup const& setup) {
                                std::string const file = setup.db.empty() ? std::string{"bank.db"} : setup.db;
                                bank::db::setup("DRIVER=SQLite3;Database=" + file);
                            }});
#endif
#if MORPH_CLIENT_HAS_LIMS
    apps.push_back(LocalApp{.name = "lims", .bundle = &lims::screens::bundle, .setUp = [](LocalSetup const& setup) {
                                std::string const file = setup.db.empty() ? std::string{"lims.db"} : setup.db;
                                lims::db::setup("DRIVER=SQLite3;Database=" + file);
                            }});
#endif
    return apps;
}

}  // namespace morph::examples::client

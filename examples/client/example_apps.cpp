// SPDX-License-Identifier: Apache-2.0
//
// morph_examples_client links every example application for local mode.

#include <forms_screens.hpp>
#include <gallery/gallery.hpp>
#include <lims_gallery/lims_gallery.hpp>
#include <morph/client/source.hpp>
#include <string>
#include <vector>

#if MORPH_CLIENT_HAS_BANK
#include <bank_screens.hpp>
#ifdef __EMSCRIPTEN__
#include "bank/wasm/demo_seed.hpp"
#else
#include "bank/db/database.hpp"
#endif
#endif

#if MORPH_CLIENT_HAS_BOOKMARKS
#include <bookmarks_screens.hpp>

#include "bookmarks/db/database.hpp"
#endif

#if MORPH_CLIENT_HAS_LIMS
#include <lims_screens.hpp>

#include "lims/db/database.hpp"
#endif

#include "local_apps.hpp"

namespace morph::examples::client {

std::vector<::morph::client::LocalApp> localApps() {
    std::vector<::morph::client::LocalApp> apps{gallery::localApp(), lims_gallery::localApp()};
    apps.push_back(::morph::client::LocalApp{.name = "forms", .bundle = &lab::screens::bundle, .setUp = {}});
#if MORPH_CLIENT_HAS_BANK
#ifdef __EMSCRIPTEN__
    // In the browser: bank's in-memory models, seeded with the hosted demo's customer (demo / demo1234).
    apps.push_back(::morph::client::LocalApp{
        .name = "bank", .bundle = &bank::screens::bundle, .setUp = [](::morph::client::LocalSetup const&) {
            bank::wasm::seedDemo();
        }});
#else
    apps.push_back(::morph::client::LocalApp{
        .name = "bank", .bundle = &bank::screens::bundle, .setUp = [](::morph::client::LocalSetup const& setup) {
            std::string const file = setup.db.empty() ? std::string{"bank.db"} : setup.db;
            bank::db::setup("DRIVER=SQLite3;Database=" + file);
        }});
#endif
#endif
#if MORPH_CLIENT_HAS_BOOKMARKS
    apps.push_back(::morph::client::LocalApp{.name = "bookmarks",
                                             .bundle = &bookmarks::screens::bundle,
                                             .setUp = [](::morph::client::LocalSetup const& setup) {
                                                 std::string const file =
                                                     setup.db.empty() ? std::string{"bookmarks.db"} : setup.db;
                                                 bookmarks::db::setup("DRIVER=SQLite3;Database=" + file);
                                                 bookmarks::screens::prepareLocal();
                                             }});
#endif
#if MORPH_CLIENT_HAS_LIMS
    apps.push_back(::morph::client::LocalApp{
        .name = "lims", .bundle = &lims::screens::bundle, .setUp = [](::morph::client::LocalSetup const& setup) {
            std::string const file = setup.db.empty() ? std::string{"lims.db"} : setup.db;
            lims::db::setup("DRIVER=SQLite3;Database=" + file);
            lims::screens::prepareLocal();
        }});
#endif
    return apps;
}

}  // namespace morph::examples::client

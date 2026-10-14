// SPDX-License-Identifier: Apache-2.0
//
// morph_examples_client links every example application for local mode.

#include <app/app_source.hpp>
#include <forms_screens.hpp>
#include <gallery/gallery.hpp>
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

#if MORPH_CLIENT_HAS_LEDGER
#include <ledger_screens.hpp>

#include "ledger/db/database.hpp"
#endif

#if MORPH_CLIENT_HAS_KANBAN
#include <kanban_screens.hpp>

#include "kanban/db/database.hpp"
#endif

#if MORPH_CLIENT_HAS_POLLS
#include <polls_screens.hpp>

#include "polls/db/database.hpp"
#endif

#if MORPH_CLIENT_HAS_BOOKMARKS
#include <bookmarks_screens.hpp>

#include "bookmarks/db/database.hpp"
#endif

#if MORPH_CLIENT_HAS_PASTEBIN
#include <pastebin_screens.hpp>

#include "pastebin/db/database.hpp"
#endif

#if MORPH_CLIENT_HAS_LIMS
#include <lims_screens.hpp>

#include "lims/db/database.hpp"
#endif

#include "local_apps.hpp"

namespace morph::examples::client {

std::vector<LocalApp> localApps() {
    std::vector<LocalApp> apps{gallery::localApp()};
    apps.push_back(LocalApp{.name = "forms", .bundle = &lab::screens::bundle, .setUp = {}});
#if MORPH_CLIENT_HAS_BANK
#ifdef __EMSCRIPTEN__
    // In the browser: bank's in-memory models, seeded with the hosted demo's customer (demo / demo1234).
    apps.push_back(LocalApp{
        .name = "bank", .bundle = &bank::screens::bundle, .setUp = [](LocalSetup const&) { bank::wasm::seedDemo(); }});
#else
    apps.push_back(LocalApp{.name = "bank", .bundle = &bank::screens::bundle, .setUp = [](LocalSetup const& setup) {
                                std::string const file = setup.db.empty() ? std::string{"bank.db"} : setup.db;
                                bank::db::setup("DRIVER=SQLite3;Database=" + file);
                            }});
#endif
#endif
#if MORPH_CLIENT_HAS_LEDGER
    apps.push_back(
        LocalApp{.name = "ledger", .bundle = &ledger::screens::bundle, .setUp = [](LocalSetup const& setup) {
                     std::string const file = setup.db.empty() ? std::string{"ledger.db"} : setup.db;
                     ledger::db::setup("DRIVER=SQLite3;Database=" + file);
                     ledger::screens::prepareLocal();
                 }});
#endif
#if MORPH_CLIENT_HAS_KANBAN
    apps.push_back(
        LocalApp{.name = "kanban", .bundle = &kanban::screens::bundle, .setUp = [](LocalSetup const& setup) {
                     std::string const file = setup.db.empty() ? std::string{"kanban.db"} : setup.db;
                     kanban::db::setup("DRIVER=SQLite3;Database=" + file);
                     kanban::screens::prepareLocal();
                 }});
#endif
#if MORPH_CLIENT_HAS_POLLS
    apps.push_back(LocalApp{.name = "polls", .bundle = &polls::screens::bundle, .setUp = [](LocalSetup const& setup) {
                                std::string const file = setup.db.empty() ? std::string{"polls.db"} : setup.db;
                                polls::db::setup("DRIVER=SQLite3;Database=" + file);
                            }});
#endif
#if MORPH_CLIENT_HAS_BOOKMARKS
    apps.push_back(
        LocalApp{.name = "bookmarks", .bundle = &bookmarks::screens::bundle, .setUp = [](LocalSetup const& setup) {
                     std::string const file = setup.db.empty() ? std::string{"bookmarks.db"} : setup.db;
                     bookmarks::db::setup("DRIVER=SQLite3;Database=" + file);
                     bookmarks::screens::prepareLocal();
                 }});
#endif
#if MORPH_CLIENT_HAS_PASTEBIN
    apps.push_back(
        LocalApp{.name = "pastebin", .bundle = &pastebin::screens::bundle, .setUp = [](LocalSetup const& setup) {
                     std::string const file = setup.db.empty() ? std::string{"pastebin.db"} : setup.db;
                     pastebin::db::setup("DRIVER=SQLite3;Database=" + file);
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

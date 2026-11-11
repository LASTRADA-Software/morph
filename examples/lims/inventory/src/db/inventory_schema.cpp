// SPDX-License-Identifier: Apache-2.0
#include <Lightweight/Lightweight.hpp>
#include <Lightweight/SqlMigration.hpp>

using namespace Lightweight::SqlColumnTypeDefinitions;

// The macro names a struct and a static object whose constructor registers the migration, so neither can be const,
// local to an anonymous namespace, or free of a throwing initializer.
// NOLINTBEGIN(bugprone-throwing-static-initialization,cert-err58-cpp,misc-use-internal-linkage,misc-use-anonymous-namespace,cppcoreguidelines-avoid-non-const-global-variables)
LIGHTWEIGHT_SQL_MIGRATION(20261120000001, "Create inventory settings, items, locations and lots") {
    plan.CreateTableIfNotExists("inventory_settings")
        .PrimaryKeyWithAutoIncrement("id", Bigint())
        .RequiredColumn("company_id", Bigint())
        .RequiredColumn("stock_account", Varchar(16))
        .RequiredColumn("grni_account", Varchar(16))
        .RequiredColumn("consumption_account", Varchar(16))
        .RequiredColumn("rounding_account", Varchar(16))
        .RequiredColumn("approval_hours", Bigint());
    plan.CreateUniqueIndex("idx_inventory_settings_company", "inventory_settings", {"company_id"});
    plan.CreateTableIfNotExists("inventory_items")
        .PrimaryKeyWithAutoIncrement("id", Bigint())
        .RequiredColumn("company_id", Bigint())
        .RequiredColumn("code", Varchar(32))
        .RequiredColumn("name", Varchar(128))
        .RequiredColumn("unit", Integer());
    // An item code names one item per company: the index refuses the second of two creates that raced.
    plan.CreateUniqueIndex("idx_inventory_items_company_code", "inventory_items", {"company_id", "code"});
    plan.CreateTableIfNotExists("inventory_locations")
        .PrimaryKeyWithAutoIncrement("id", Bigint())
        .RequiredColumn("company_id", Bigint())
        .RequiredColumn("site_id", Bigint())
        .RequiredColumn("name", Varchar(64));
    plan.CreateUniqueIndex("idx_inventory_locations_site_name", "inventory_locations", {"site_id", "name"});
    plan.CreateTableIfNotExists("inventory_lots")
        .PrimaryKeyWithAutoIncrement("id", Bigint())
        .RequiredColumn("item_id", Bigint())
        .RequiredColumn("location_id", Bigint())
        .RequiredColumn("company_id", Bigint())
        .RequiredColumn("site_id", Bigint())
        .RequiredColumn("lot_number", Varchar(32))
        .RequiredColumn("expires_at", Bigint())
        .RequiredColumn("on_hand_num", Bigint())
        .RequiredColumn("on_hand_den", Bigint())
        .RequiredColumn("unit_cost_num", Bigint())
        .RequiredColumn("unit_cost_den", Bigint())
        .RequiredColumn("reservation_version", Bigint());
    // A supplier's lot number names one lot of an item: a second receipt of it is refused, not merged.
    plan.CreateUniqueIndex("idx_inventory_lots_item_number", "inventory_lots", {"item_id", "lot_number"});
}

LIGHTWEIGHT_SQL_MIGRATION(20261120000002, "Create inventory_movements") {
    plan.CreateTableIfNotExists("inventory_movements")
        .PrimaryKeyWithAutoIncrement("id", Bigint())
        .RequiredColumn("company_id", Bigint())
        .RequiredColumn("item_id", Bigint())
        .Column("lot_id", Bigint())
        .RequiredColumn("kind", Integer())
        .RequiredColumn("quantity_num", Bigint())
        .RequiredColumn("quantity_den", Bigint())
        .RequiredColumn("at", Bigint())
        .RequiredColumn("by", Varchar(64))
        .RequiredColumn("source", Varchar(128));
    plan.CreateIndex("idx_inventory_movements_item", "inventory_movements", {"item_id"});
}
// NOLINTEND(bugprone-throwing-static-initialization,cert-err58-cpp,misc-use-internal-linkage,misc-use-anonymous-namespace,cppcoreguidelines-avoid-non-const-global-variables)

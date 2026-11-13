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

LIGHTWEIGHT_SQL_MIGRATION(20261120000004, "Create inventory_requisitions and their lines") {
    plan.CreateTableIfNotExists("inventory_requisitions")
        .PrimaryKeyWithAutoIncrement("id", Bigint())
        .RequiredColumn("company_id", Bigint())
        .RequiredColumn("site_id", Bigint())
        .RequiredColumn("reason", Varchar(128))
        .RequiredColumn("state", Integer())
        .RequiredColumn("requested_by", Varchar(64))
        .RequiredColumn("submitted_by", Varchar(64))
        .RequiredColumn("approved_by", Varchar(64))
        .RequiredColumn("created_at", Bigint())
        .RequiredColumn("submitted_at", Bigint())
        .RequiredColumn("deadline_at", Bigint())
        .RequiredColumn("total_num", Bigint())
        .RequiredColumn("total_den", Bigint())
        .RequiredColumn("currency", Integer());
    plan.CreateTableIfNotExists("inventory_requisition_lines")
        .PrimaryKeyWithAutoIncrement("id", Bigint())
        .RequiredColumn("requisition_id", Bigint())
        .RequiredColumn("item_id", Bigint())
        .RequiredColumn("quantity_num", Bigint())
        .RequiredColumn("quantity_den", Bigint())
        .RequiredColumn("estimate_num", Bigint())
        .RequiredColumn("estimate_den", Bigint());
}
LIGHTWEIGHT_SQL_MIGRATION(20261120000005, "Create inventory suppliers, orders and order lines") {
    plan.AlterTable("inventory_requisitions").AddNotRequiredColumn("order_id", Bigint());
    plan.CreateTableIfNotExists("inventory_suppliers")
        .PrimaryKeyWithAutoIncrement("id", Bigint())
        .RequiredColumn("company_id", Bigint())
        .RequiredColumn("name", Varchar(128))
        .RequiredColumn("currency", Integer());
    plan.CreateUniqueIndex("idx_inventory_suppliers_company_name", "inventory_suppliers", {"company_id", "name"});
    plan.CreateTableIfNotExists("inventory_orders")
        .PrimaryKeyWithAutoIncrement("id", Bigint())
        .RequiredColumn("company_id", Bigint())
        .RequiredColumn("site_id", Bigint())
        .RequiredColumn("supplier_id", Bigint())
        .RequiredColumn("requisition_id", Bigint())
        .RequiredColumn("currency", Integer())
        .RequiredColumn("state", Integer())
        .RequiredColumn("total_num", Bigint())
        .RequiredColumn("total_den", Bigint())
        .RequiredColumn("ordered_by", Varchar(64))
        .RequiredColumn("ordered_at", Bigint());
    // A requisition becomes one order: the index refuses the second of two orders that raced.
    plan.CreateUniqueIndex("idx_inventory_orders_requisition", "inventory_orders", {"requisition_id"});
    plan.CreateTableIfNotExists("inventory_order_lines")
        .PrimaryKeyWithAutoIncrement("id", Bigint())
        .RequiredColumn("order_id", Bigint())
        .RequiredColumn("item_id", Bigint())
        .RequiredColumn("ordered_num", Bigint())
        .RequiredColumn("ordered_den", Bigint())
        .RequiredColumn("received_num", Bigint())
        .RequiredColumn("received_den", Bigint())
        .RequiredColumn("unit_price_num", Bigint())
        .RequiredColumn("unit_price_den", Bigint())
        .RequiredColumn("line_total_num", Bigint())
        .RequiredColumn("line_total_den", Bigint());
}
LIGHTWEIGHT_SQL_MIGRATION(20261120000006, "Create inventory receipts, their lots and number series") {
    plan.CreateTableIfNotExists("inventory_receipts")
        .PrimaryKeyWithAutoIncrement("id", Bigint())
        .RequiredColumn("company_id", Bigint())
        .RequiredColumn("order_id", Bigint())
        .RequiredColumn("number", Varchar(32))
        .RequiredColumn("currency", Integer())
        .RequiredColumn("value_num", Bigint())
        .RequiredColumn("value_den", Bigint())
        .RequiredColumn("company_value_num", Bigint())
        .RequiredColumn("company_value_den", Bigint())
        .Column("entry_id", Bigint())
        .RequiredColumn("received_at", Bigint())
        .RequiredColumn("received_by", Varchar(64));
    // A number is issued once per company: the index refuses a second receipt with one number.
    plan.CreateUniqueIndex("idx_inventory_receipts_company_number", "inventory_receipts", {"company_id", "number"});
    plan.CreateTableIfNotExists("inventory_receipt_lots")
        .PrimaryKeyWithAutoIncrement("id", Bigint())
        .RequiredColumn("receipt_id", Bigint())
        .RequiredColumn("lot_id", Bigint())
        .RequiredColumn("order_line_id", Bigint());
    plan.CreateTableIfNotExists("inventory_receipt_numbers")
        .PrimaryKeyWithAutoIncrement("id", Bigint())
        .RequiredColumn("company_id", Bigint())
        .RequiredColumn("fiscal_year", Bigint())
        .RequiredColumn("next", Bigint());
    plan.CreateUniqueIndex("idx_inventory_receipt_numbers_year", "inventory_receipt_numbers",
                           {"company_id", "fiscal_year"});
}
LIGHTWEIGHT_SQL_MIGRATION(20261120000007, "Create inventory recipes and consumed events") {
    plan.CreateTableIfNotExists("inventory_recipes")
        .PrimaryKeyWithAutoIncrement("id", Bigint())
        .RequiredColumn("company_id", Bigint())
        .RequiredColumn("analysis_id", Bigint())
        .RequiredColumn("item_id", Bigint())
        .RequiredColumn("quantity_num", Bigint())
        .RequiredColumn("quantity_den", Bigint());
    // One quantity of an item per analysis and company; setting it again replaces it.
    plan.CreateUniqueIndex("idx_inventory_recipes_line", "inventory_recipes",
                           {"company_id", "analysis_id", "item_id"});
    plan.CreateTableIfNotExists("inventory_consumed_events")
        .PrimaryKeyWithAutoIncrement("id", Bigint())
        .RequiredColumn("event_id", Varchar(128))
        .RequiredColumn("company_id", Bigint())
        .RequiredColumn("sample_id", Bigint())
        .RequiredColumn("shortfall_num", Bigint())
        .RequiredColumn("shortfall_den", Bigint())
        .RequiredColumn("cost_num", Bigint())
        .RequiredColumn("cost_den", Bigint())
        .Column("entry_id", Bigint());
    // An event is consumed once: the index refuses the second of two deliveries that raced past the lookup.
    plan.CreateUniqueIndex("idx_inventory_consumed_events_event", "inventory_consumed_events", {"event_id"});
    plan.AlterTable("inventory_movements").AddNotRequiredColumn("event_id", Varchar(128));
    plan.CreateIndex("idx_inventory_movements_event", "inventory_movements", {"event_id"});
}
// NOLINTEND(bugprone-throwing-static-initialization,cert-err58-cpp,misc-use-internal-linkage,misc-use-anonymous-namespace,cppcoreguidelines-avoid-non-const-global-variables)

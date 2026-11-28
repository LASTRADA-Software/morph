// SPDX-License-Identifier: Apache-2.0

// A table's actions in the catalog, and the builders that refuse them.
//
// Part of the client table model tests; see docs/spec/table/table.md.

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstdint>
#include <format>
#include <functional>
#include <initializer_list>
#include <iterator>
#include <map>
#include <memory>
#include <morph/core/registry.hpp>
#include <morph/forms/forms.hpp>
#include <morph/reactive/signal.hpp>
#include <morph/table/data_source.hpp>
#include <morph/table/description.hpp>
#include <morph/table/edits.hpp>
#include <morph/table/query.hpp>
#include <morph/table/sort.hpp>
#include <morph/table/view_change.hpp>
#include <morph/ui/author.hpp>
#include <morph/ui/mount.hpp>
#include <morph/ui/table_rows.hpp>
#include <morph/ui/value.hpp>
#include <numeric>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "table/wide_table_model.hpp"
#include "ui_table_model_support.hpp"
#include "ui_table_support.hpp"

namespace table = morph::table;

using namespace table_model_support;

using namespace std::chrono_literals;

// Mutations: `Runs` not admitting an action that runs itself (`CatalogFacts::action` of `Table.small` does not
// compile); `recordsInfo` dropping `opaque` (the catalog's record of `Table.small` is not opaque).

TEST_CASE("table catalog: a table's actions are catalogued, holding the record opaquely", "[ui][table]") {
    morph::ui::author::CatalogFacts facts;
    facts.action<bench::WideTableModel, table::QueryTable<&bench::WideTableModel::small>>();
    facts.action<bench::WideTableModel, table::DescribeTable<&bench::WideTableModel::small>>();
    morph::ui::Catalog const catalog = facts.catalog();
    auto const query = catalog.action("Table.small");
    REQUIRE(query.has_value());
    CHECK(query->modelType == "WideTableModel");
    REQUIRE(query->records.size() == 1);
    CHECK(query->records.front().record == "WideTableModel.small");
    CHECK(query->records.front().opaque);
    CHECK(query->records.front().paths.empty());
    auto const describe = catalog.action("Table.small.describe");
    REQUIRE(describe.has_value());
    CHECK(describe->modelType == "WideTableModel");
    CHECK(describe->records.empty());
}

// A table's actions only read: the builder's `evaluate` and `mutation` refuse them at compile time, while `query`
// and the catalog take them.

template <typename Action>
concept TableEvaluatable =
    requires(morph::ui::author::DeclScope& scope, morph::ui::author::ModelHandle<bench::WideTableModel> const& model) {
        scope.evaluate<Action>("e", model);
    };

template <typename Action>
concept TableMutable =
    requires(morph::ui::author::DeclScope& scope, morph::ui::author::ModelHandle<bench::WideTableModel> const& model) {
        scope.mutation<Action>("m", model);
    };

template <typename Action>
concept TableQueryable =
    requires(morph::ui::author::DeclScope& scope, morph::ui::author::ModelHandle<bench::WideTableModel> const& model) {
        scope.query<Action>("q", model);
    };

// Mutation: `evaluate` and `mutation` constrained by `Runs` (both static_asserts on them fail to hold).

TEST_CASE("table catalog: a table's actions are no builder's evaluate or mutation", "[ui][table]") {
    using Query = table::QueryTable<&bench::WideTableModel::small>;
    STATIC_CHECK_FALSE(TableEvaluatable<Query>);
    STATIC_CHECK_FALSE(TableMutable<Query>);
    STATIC_CHECK(TableQueryable<Query>);
}

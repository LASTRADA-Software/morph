// SPDX-License-Identifier: Apache-2.0

// The non-template definitions of <morph/table/row_selection.hpp>.

#include <algorithm>
#include <core/async/ExecutorContext.hpp>
#include <core/async/Task.hpp>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <expected>
#include <functional>
#include <glaze/glaze.hpp>
#include <memory>
#include <morph/core/action_refusal.hpp>
#include <morph/core/completion.hpp>
#include <morph/core/executor.hpp>
#include <morph/core/records.hpp>
#include <morph/table/description.hpp>
#include <morph/table/query.hpp>
#include <morph/table/record_name.hpp>
#include <morph/table/row_selection.hpp>
#include <morph/table/runtime.hpp>
#include <morph/table/view_cache.hpp>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <typeindex>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace morph::table::detail {

std::string compactFilterJson(FilterSpec const& filters) {
    glz::generic_u64 json;
    if (glz::read_json(json, glz::write_json(filters).value_or("{}"))) {
        return "{}";
    }
    std::function<void(glz::generic_u64&)> prune = [&prune](glz::generic_u64& node) {
        if (node.is_array()) {
            for (auto& item : node.get_array()) {
                prune(item);
            }
            return;
        }
        if (!node.is_object()) {
            return;
        }
        auto& members = node.get_object();
        // NOLINTNEXTLINE(readability-qualified-auto): the iterator is a pointer in some standard libraries and a class in others.
        for (auto at = members.begin(); at != members.end();) {
            auto& value = at->second;
            prune(value);
            bool const empty = value.is_null() || (value.is_array() && value.get_array().empty()) ||
                               (value.is_string() && value.get_string().empty()) ||
                               (at->first == "combine" && value.is_string() && value.get_string() == "any");
            at = empty && at->first != "columns" ? members.erase(at) : std::next(at);
        }
    };
    prune(json);
    return glz::write_json(json).value_or("{}");
}

std::string selectionJson(std::span<RowId const> keys, FilterSpec const* filters, std::span<RowId const> retained,
                          std::string_view revision, std::span<RowId const> except) {
    auto const list = [](std::span<RowId const> items) {
        return glz::write_json(std::vector<RowId>(items.begin(), items.end())).value_or("[]");
    };
    if (filters == nullptr) {
        return R"({"keys":)" + list(keys) + "}";
    }
    return R"({"view":{"filters":)" + compactFilterJson(*filters) + R"(,"retained":)" + list(retained) +
           R"(,"revision":)" + glz::write_json(revision).value_or(R"("")") + R"(},"except":)" + list(except) + "}";
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity): one sequential pass whose steps share its running state; splitting it would thread that state through parameters.
std::expected<Membership, TableError> membership(ServedTable const& served, FilterSpec const& filters,
                                                 std::span<RowId const> retained, ViewCache* views,
                                                 std::string_view record) {
    if (served.source == nullptr) {
        return std::unexpected(
            TableError{.code = TableErrorCode::Unavailable, .column = {}, .message = "the table serves no source"});
    }
    // A table built by hand may describe no columns: its source's are then the table's.
    auto infos = columnInfos(served.description);
    if (infos.empty()) {
        auto const own = served.source->columns();
        infos.assign(own.begin(), own.end());
    }
    auto const groups = served.source->groups();
    if (auto error = checkGroups(served.source->columns(), groups)) {
        return std::unexpected(std::move(*error));
    }
    TableQuery query;
    query.filters = filters;
    auto checked = validate(std::move(query), infos, served.limits, served.mayRead, groups);
    if (!checked) {
        return std::unexpected(std::move(checked.error()));
    }
    auto const needed = levelsNeeded(*checked, infos, served.mayRead, served.services.forTask(), groups);
    std::vector<LevelInput> inputs;
    for (auto const& level : needed.order) {
        for (auto const& source : served.levels) {
            if (source != nullptr && source->level() == level) {
                auto levelSnapshot = source->snapshot();
                inputs.push_back(LevelInput{.source = source, .snapshot = std::move(levelSnapshot)});
            }
        }
    }
    LeveledSource const leveled{served.source, infos, inputs};
    ApplyOptions const options{.limits = served.limits, .services = served.services, .mayRead = served.mayRead};
    auto const compute = [&]() -> std::expected<ViewComputation, TableError> {
        auto view = computeView(leveled, *checked, options);
        if (!view) {
            return std::unexpected(std::move(view.error()));
        }
        return ViewComputation{.view = std::move(*view), .awaiting = {}, .failed = {}, .orderKeys = {}};
    };
    std::shared_ptr<ViewEntry const> entry;
    // A filter that reads a level is computed here: a cached entry of it also holds what its levels want, which
    // only a page query computes.
    auto const viewQuery =
        views != nullptr && !record.empty() && needed.order.empty() ? viewQueryOf(*checked) : std::nullopt;
    if (viewQuery) {
        auto const* const pinnedSource = dynamic_cast<PinnedSource const*>(served.source.get());
        auto found = views->find(ViewKey{.record = std::string{record}, .scope = served.scope, .query = *viewQuery},
                                 served.source->snapshot(), {}, compute,
                                 pinnedSource != nullptr ? pinnedSource->pinnedAt() : 0);
        if (!found) {
            return std::unexpected(std::move(found.error()));
        }
        entry = std::move(*found);
    } else {
        auto computed = compute();
        if (!computed) {
            return std::unexpected(std::move(computed.error()));
        }
        entry = std::make_shared<ViewEntry const>(
            served.source->snapshot(), std::vector<std::pair<std::string, std::uint64_t>>{}, std::move(*computed));
    }
    Membership out{.rows = entry->view.rows, .digest = entry->view.digest};
    if (!retained.empty()) {
        auto const snapshot = leveled.snapshot();
        std::unordered_set<RowId> const wanted(retained.begin(), retained.end());
        std::unordered_set<std::uint32_t> const inView(out.rows.begin(), out.rows.end());
        std::vector<std::uint32_t> merged;
        for (std::size_t row = 0; row < snapshot->rowCount() && merged.size() < wanted.size(); ++row) {
            auto const rowIndex = static_cast<std::uint32_t>(row);
            if (wanted.contains(snapshot->rowId(row)) && !inView.contains(rowIndex)) {
                merged.push_back(rowIndex);
                out.digest.add(snapshot->rowId(row));
            }
        }
        auto const middle = static_cast<std::ptrdiff_t>(out.rows.size());
        out.rows.insert(out.rows.end(), merged.begin(), merged.end());
        std::ranges::inplace_merge(out.rows, out.rows.begin() + middle);
    }
    if (std::cmp_greater(out.rows.size(), served.limits.maxSelectedRows)) {
        return std::unexpected(TableError{.code = TableErrorCode::LimitExceeded,
                                          .column = {},
                                          .message = "a selection of the view holds at most " +
                                                     std::to_string(served.limits.maxSelectedRows) + " rows"});
    }
    return out;
}

std::expected<SelectedRows, TableError> resolveSelection(ServedTable const& served, PlainSelection const& selection,
                                                         SelectionPolicy policy, ViewCache* views,
                                                         std::string_view record, bool rowScoped) {
    // A row-scoped table with no scope would let callers share views of different rows.
    if (rowScoped && served.scope.empty()) {
        return std::unexpected(TableError{
            .code = TableErrorCode::SourceContract, .column = {}, .message = "a row-scoped table needs a scope"});
    }
    if (served.source == nullptr) {
        return std::unexpected(
            TableError{.code = TableErrorCode::Unavailable, .column = {}, .message = "the table serves no source"});
    }
    SelectedRows out;
    if (!selection.isView) {
        std::unordered_set<RowId> const wanted(selection.keys.begin(), selection.keys.end());
        if (std::cmp_greater(wanted.size(), served.limits.maxSelectedRows)) {
            return std::unexpected(TableError{
                .code = TableErrorCode::LimitExceeded,
                .column = {},
                .message = "a selection holds at most " + std::to_string(served.limits.maxSelectedRows) + " rows"});
        }
        auto const snapshot = served.source->snapshot();
        for (std::size_t row = 0; row < snapshot->rowCount() && out.keys.size() < wanted.size(); ++row) {
            if (auto key = snapshot->rowId(row); wanted.contains(key)) {
                out.keys.push_back(std::move(key));
            }
        }
        out.dropped = static_cast<std::int64_t>(wanted.size() - out.keys.size());
        return out;
    }
    auto members = membership(served, selection.filters, selection.retained, views, record);
    if (!members) {
        return std::unexpected(std::move(members.error()));
    }
    if (policy == SelectionPolicy::Exact && members->digest.text() != selection.revision) {
        return std::unexpected(TableError{.code = TableErrorCode::ViewChanged,
                                          .column = {},
                                          .message = "the rows changed since they were selected"});
    }
    std::unordered_set<RowId> const except(selection.except.begin(), selection.except.end());
    auto const snapshot = served.source->snapshot();
    out.keys.reserve(members->rows.size());
    for (auto const row : members->rows) {
        if (auto key = snapshot->rowId(row); !except.contains(key)) {
            out.keys.push_back(std::move(key));
        }
    }
    return out;
}

void ResolveInline::post(std::function<void()> task) {
    if (task) {
        ::core::async::ExecutorScope const scope{coreExecutor()};
        task();
    }
}

}  // namespace morph::table::detail

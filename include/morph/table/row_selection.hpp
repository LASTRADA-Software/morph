// SPDX-License-Identifier: Apache-2.0

#pragma once

/// @file table/row_selection.hpp
/// @brief What a user selected in a model's table, as an action receives it, and how the server turns it into
///        keys.
///
/// On a model-backed table the client holds a window, not the view, so "every row I see" is a description of the
/// view: its filter, its retained rows and its revision, less the keys the user took out. `resolve` turns it into
/// keys on the server, in the table's source order, for the caller: the table's function is called for the caller,
/// so its row scope and `mayRead` apply.
///
/// See `docs/spec/table/table.md`, "A selection of the view".

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <expected>
#include <functional>
#include <memory>
// NOLINTNEXTLINE(misc-header-include-cycle): description.hpp includes this header at its end, after the declarations this one needs.
#include <morph/export.hpp>
#include <morph/table/description.hpp>
#include <morph/table/record_name.hpp>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <typeindex>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace morph::table {

namespace detail {

/// @brief A filter's JSON with every member at its default left out: a `combine` of `any`, and empty lists, texts
///        and nulls. It reads back as the same filter, and is the shape a client writes.
/// @param filters The filter.
/// @return The JSON text.
[[nodiscard]] MORPH_API std::string compactFilterJson(FilterSpec const& filters);

/// @brief The JSON of a selection: `{"keys": […]}` without a view, `{"view": {…}, "except": […]}` with one.
/// @param keys     The keys of a key selection.
/// @param filters  The view's filter; null for a key selection.
/// @param retained The view's retained rows.
/// @param revision The view's revision.
/// @param except   The keys the view selection leaves out.
/// @return The JSON text.
[[nodiscard]] MORPH_API std::string selectionJson(std::span<RowId const> keys, FilterSpec const* filters,
                                                  std::span<RowId const> retained, std::string_view revision,
                                                  std::span<RowId const> except);

}  // namespace detail

/// @brief What a user selected in one table: exactly some keys, or every row of a view less some keys.
///
/// Its JSON is `{"keys": […]}`, or `{"view": {"filters", "retained", "revision"}, "except": […]}`; a filter is
/// written with its default members left out. Its schema property carries `x-selectionOf` with the table's record,
/// so a client knows which table an action works on.
/// @tparam Function The table's function, as `BRIDGE_REGISTER_TABLE` names it.
template <auto Function>
struct RowSelection {
    /// @brief The view a selection of the view is of.
    struct View {
        /// @brief The view's filter, as the table queried it.
        FilterSpec filters;
        /// @brief The rows the view kept although they no longer match.
        std::vector<RowId> retained;
        /// @brief The view's `viewRevision` when the user selected it.
        std::string revision;
    };

    /// @brief Without `view`: exactly these rows.
    std::vector<RowId> keys;
    /// @brief With it: every row of that view, less `except`.
    std::optional<View> view;
    /// @brief The keys a selection of the view leaves out.
    std::vector<RowId> except;

    /// @brief The schema keyword and its value that name the selection's table.
    /// @return `{"x-selectionOf", "<Model>.<id>"}`, the record's registered name; the name is empty when no such
    ///         record is registered.
    [[nodiscard]] static std::pair<std::string_view, std::string> schemaValue() {
        return {"x-selectionOf", detail::recordNameOf(std::type_index{typeid(TableOf<Function>)})};
    }
};

/// @brief How `resolve` treats a view whose rows changed since the user selected it.
enum class SelectionPolicy : std::uint8_t {
    Exact,    ///< Refuse with `ViewChanged` when the view's membership differs from the selection's revision.
    Current,  ///< Act on the view as it is when the action runs.
};

/// @brief What `resolve` returns.
struct SelectedRows {
    /// @brief The keys, in the table's source order.
    std::vector<RowId> keys;
    /// @brief Keys of a key selection the caller may no longer see, left out.
    std::int64_t dropped = 0;
};

/// @brief The body of `Table.<id>.resolve`, for an action of another model or a task off the model's strand.
/// @tparam Function The table's function.
template <auto Function>
struct ResolveRequest {
    /// @brief The selection.
    RowSelection<Function> selection;
    /// @brief How a changed view is treated.
    SelectionPolicy policy = SelectionPolicy::Exact;
};

/// @brief One row of a table, as an action's input names it. Its JSON is the key itself; its schema property
///        carries `x-rowOf` with the table's record.
/// @tparam Function The table's function.
template <auto Function>
struct RowRef {
    /// @brief The row's key.
    RowId key;

    /// @brief The schema keyword and its value that name the row's table.
    /// @return `{"x-rowOf", "<Model>.<id>"}`; the name is empty when no such record is registered.
    [[nodiscard]] static std::pair<std::string_view, std::string> schemaValue() {
        return {"x-rowOf", detail::recordNameOf(std::type_index{typeid(TableOf<Function>)})};
    }
};

namespace detail {

/// @brief A selection of any table: what the work off the strand reads.
struct PlainSelection {
    /// @brief `RowSelection::keys`.
    std::vector<RowId> keys;
    /// @brief Whether the selection is of a view.
    bool isView = false;
    /// @brief `RowSelection::View::filters`.
    FilterSpec filters;
    /// @brief `RowSelection::View::retained`.
    std::vector<RowId> retained;
    /// @brief `RowSelection::View::revision`.
    std::string revision;
    /// @brief `RowSelection::except`.
    std::vector<RowId> except;
};

/// @brief A view's membership: its rows and the digest of their keys.
struct Membership {
    /// @brief The view's source rows, in source order.
    std::vector<std::uint32_t> rows;
    /// @brief The keys of `rows`, as a set.
    MembershipDigest digest;
};

/// @brief The membership of the view of @p filters over @p served, with the retained rows merged in.
///
/// The view is the filter's rows under an empty sort, whose order is the source's. It is taken from @p views when
/// one is given and @p record is not empty, under the key a page query of the same filter and no sort uses, unless
/// the filter reads a level; otherwise it is computed for the call. Each key of @p retained that the source has
/// and the view lacks is merged in at its source row and added to the digest.
/// @param served   The table, its source pinned.
/// @param filters  The view's filter.
/// @param retained The rows the view kept although they no longer match.
/// @param views    The server's view cache; null computes the view.
/// @param record   The table's record name, the first member of a view's key; empty caches nothing.
/// @return The membership, or the error the filter or the bound gives: `LimitExceeded` for a view of more than
///         `served.limits.maxSelectedRows` rows.
// NOLINTNEXTLINE(readability-function-cognitive-complexity): one sequential pass whose steps share its running state; splitting it would thread that state through parameters.
[[nodiscard]] MORPH_API std::expected<Membership, TableError> membership(ServedTable const& served,
                                                                         FilterSpec const& filters,
                                                                         std::span<RowId const> retained,
                                                                         ViewCache* views,
                                                                         std::string_view record = {});

/// @brief Resolves @p selection over @p served in the calling thread: `resolve`'s work.
/// @param served    The table, its source pinned.
/// @param selection The selection.
/// @param policy    How a changed view is treated.
/// @param views     The server's view cache; null computes the view.
/// @param record    The table's record name; empty caches nothing.
/// @param rowScoped The registration's `rowScoped`: the table then needs a `ServedTable::scope`.
/// @return The keys in source order, or the error: `SourceContract` for a row-scoped table with no scope.
[[nodiscard]] MORPH_API std::expected<SelectedRows, TableError> resolveSelection(
    ServedTable const& served, PlainSelection const& selection, SelectionPolicy policy, ViewCache* views,
    std::string_view record, bool rowScoped = false);

/// @brief The owner of a `resolve` that runs where it is called: runs each task at once, inside a scope that names
///        it, so the completion's checks find the task on its owner.
///
/// One object serves every thread. It is serial for each completion only: every post it gets for one completion
/// comes from the thread that called `resolve` (the settle within the call, then a `then` or `co_await` attach on
/// that thread), and the scope it states is the calling thread's own. Two threads resolving at once run their tasks
/// side by side, each touching only its own completion.
class MORPH_API ResolveInline final : public exec::IExecutor {
public:
    /// @brief Runs @p task now, on the calling thread.
    /// @param task The task.
    void post(std::function<void()> task) override;
};

/// @brief The selection as the work off the strand reads it.
/// @tparam Function The table's function.
/// @param selection The selection.
/// @return Its plain copy.
template <auto Function>
[[nodiscard]] PlainSelection plainOf(RowSelection<Function> const& selection) {
    PlainSelection out;
    out.keys = selection.keys;
    out.except = selection.except;
    if (selection.view) {
        out.isView = true;
        out.filters = selection.view->filters;
        out.retained = selection.view->retained;
        out.revision = selection.view->revision;
    }
    return out;
}

}  // namespace detail

/// @brief Turns a selection into keys, for the caller.
///
/// Calls the table's function on @p model on the calling thread, the model's strand, for @p context, so the
/// caller's row scope and `mayRead` apply and a `typed<Row>` table works as a plain one. A key selection resolves
/// to the keys the caller can still see, in source order, counting the others in `dropped`. A selection of the view
/// resolves to the view's membership (the filter's rows under an empty sort, the retained rows merged in) less
/// `except`, in source order; under `Exact` a membership whose digest differs from the selection's revision is
/// `ViewChanged`. A view of more than `QueryLimits::maxSelectedRows` rows is `LimitExceeded`.
///
/// Inside a server's dispatch the work runs on the runtime's `work` with its view cache, and the completion
/// belongs to the runtime's `settle`. With no runtime, or one missing either of the two, it runs here with no cache
/// (a runtime with `work` and no `settle` included), and the completion is settled before this returns: a `then`
/// attached afterwards on this thread runs at once, and `co_await` works from any coroutine. A `then` attached from
/// a task of another executor is off the completion's owner, which a debug build asserts. A row-scoped table whose
/// function returns no `scope` is `SourceContract`, as a page query of it is.
/// @tparam Function The table's function.
/// @tparam Model    The model type.
/// @param model     The model instance; not const, as a table function is not.
/// @param selection The selection.
/// @param context   Who asks.
/// @param policy    How a changed view is treated.
/// @return The keys, or the error.
template <auto Function, typename Model>
[[nodiscard]] async::Completion<std::expected<SelectedRows, TableError>> resolve(
    Model& model, RowSelection<Function> const& selection, TableContext const& context, SelectionPolicy policy) {
    using Result = std::expected<SelectedRows, TableError>;
    auto served = detail::pinned(detail::serve<Function>(model, context));
    auto plain = detail::plainOf(selection);
    auto record = detail::recordNameOf(std::type_index{typeid(TableOf<Function>)});
    bool const rowScoped = detail::tableRecordOf<Function>().second;
    auto const* const runtime = currentTableRuntime();
    if (runtime == nullptr || runtime->work == nullptr || runtime->settle == nullptr) {
        static detail::ResolveInline owner;
        auto [completion, promise] = async::Completion<Result>::makeSettleable(&owner);
        try {
            promise.resolve(detail::resolveSelection(served, plain, policy, nullptr, record, rowScoped));
        } catch (...) {
            promise.reject(std::current_exception());
        }
        return std::move(completion);
    }
    auto [completion, promise] = async::Completion<Result>::makeSettleable(runtime->settle);
    // A posted task is a `std::function`, which copies: the promise is shared.
    auto const shared = std::make_shared<async::Completion<Result>::Promise>(std::move(promise));
    runtime->work->post([shared, served = std::move(served), plain = std::move(plain), policy, rowScoped,
                         views = runtime->views, record = std::move(record), keep = runtime->settleKeep] {
        static_cast<void>(keep);
        try {
            shared->resolve(detail::resolveSelection(served, plain, policy, views, record, rowScoped));
        } catch (...) {
            shared->reject(std::current_exception());
        }
    });
    return std::move(completion);
}

/// @brief Throws the refusal a Task handler gives for a table's error.
/// @param error The error `resolve` returned.
/// @throws model::ActionRefusal `"morph.table.viewChanged"` for `ViewChanged`, `"morph.table." + <the code's wire
///         name>` for every other code, with the error's message.
[[noreturn]] inline void refuse(TableError const& error) {
    throw model::ActionRefusal{"morph.table." + detail::errorCodeName(error.code), {}, error.message};
}

/// @brief The action `Table.<id>.resolve`: a selection resolved for the caller, as `resolve` does on the model.
///
/// Its JSON is the `ResolveRequest`'s. It runs as `Table.<id>` does: the table's function for the caller's session
/// on the model's strand, then the work on the server's table executor.
/// @tparam Function The table's function, as `BRIDGE_REGISTER_TABLE` keys it.
template <auto Function>
struct ResolveTable {
    /// @brief The selection and the policy.
    ResolveRequest<Function> request;

    /// @brief Resolves the selection.
    /// @tparam Model The model type.
    /// @param model  The model instance.
    /// @param action The action.
    /// @return The keys.
    /// @throws morph::model::ActionRefusal `table.<reason>` with the argument `column`, as `Table.<id>` refuses.
    template <typename Model>
    // NOLINTNEXTLINE(cppcoreguidelines-avoid-reference-coroutine-parameters): the dispatcher keeps the model and the action alive until the task completes.
    static ::core::async::Task<SelectedRows> invoke(Model& model, ResolveTable& action) {
        auto const context = detail::callerContext();
        // Its own statement: MSVC cannot tail-call a call that shares a full-expression with a `co_await`.
        auto completion = resolve<Function>(model, action.request.selection, context, action.request.policy);
        auto resolved = co_await std::move(completion);
        if (!resolved) {
            throw detail::refusalOf(resolved.error());
        }
        co_return std::move(*resolved);
    }
};

}  // namespace morph::table

/// @brief JSON names of `morph::table::SelectionPolicy`: `exact` and `current`.
template <>
struct glz::meta<morph::table::SelectionPolicy> {
    using enum morph::table::SelectionPolicy;
    /// @brief Each enumerator after its JSON name.
    static constexpr auto value = glz::enumerate("exact", Exact, "current", Current);
};

namespace morph::table::detail {

/// @brief What a `RowSelection` is read from: every member optional.
struct SelectionWire {
    /// @brief The view, when the selection is of one.
    struct View {
        /// @brief Its filter.
        FilterSpec filters;
        /// @brief Its retained rows.
        std::vector<RowId> retained;
        /// @brief Its revision.
        std::string revision;
    };
    /// @brief The keys of a key selection.
    std::vector<RowId> keys;
    /// @brief The view.
    std::optional<View> view;
    /// @brief The keys a selection of the view leaves out.
    std::vector<RowId> except;
};

}  // namespace morph::table::detail

/// @brief The JSON of `morph::table::RowSelection`: `{"keys"}` or `{"view", "except"}`.
/// @tparam Function The table's function.
template <auto Function>
struct glz::meta<morph::table::RowSelection<Function>> {
    /// @brief The selection type.
    using T = morph::table::RowSelection<Function>;
    /// @brief Read from the wire shape, written by `detail::selectionJson`.
    static constexpr auto value =
        glz::custom<[](T& selection, morph::table::detail::SelectionWire const& wire) {
            selection.keys = wire.keys;
            selection.except = wire.except;
            selection.view.reset();
            if (wire.view) {
                selection.view = typename T::View{
                    .filters = wire.view->filters, .retained = wire.view->retained, .revision = wire.view->revision};
            }
        },
                    [](T const& selection) {
                        return glz::raw_json{morph::table::detail::selectionJson(
                            selection.keys, selection.view ? &selection.view->filters : nullptr,
                            selection.view ? std::span<morph::table::RowId const>{selection.view->retained}
                                           : std::span<morph::table::RowId const>{},
                            selection.view ? std::string_view{selection.view->revision} : std::string_view{},
                            selection.except)};
                    }>;
};

/// @brief The JSON of `morph::table::RowRef`: its key.
/// @tparam Function The table's function.
template <auto Function>
struct glz::meta<morph::table::RowRef<Function>> {
    /// @brief The reference type.
    using T = morph::table::RowRef<Function>;
    /// @brief Written as its key.
    static constexpr auto value = &T::key;
};

/// @brief The JSON of `morph::table::ResolveTable`: its `ResolveRequest`'s.
/// @tparam Function The table's function.
template <auto Function>
struct glz::meta<morph::table::ResolveTable<Function>> {
    /// @brief The action type.
    using T = morph::table::ResolveTable<Function>;
    /// @brief The action is written as its request.
    static constexpr auto value = &T::request;
};

namespace glz::detail {

/// @brief The schema of a `RowSelection`: its wire object.
/// @tparam Function The table's function.
template <auto Function>
struct to_json_schema<morph::table::RowSelection<Function>> {
    /// @brief Writes the wire shape's schema.
    /// @tparam Opts The write options.
    /// @param outSchema The schema written.
    /// @param defs      The definitions.
    template <auto Opts>
    static void op(auto& outSchema, auto& defs) {
        to_json_schema<morph::table::detail::SelectionWire>::template op<Opts>(outSchema, defs);
    }
};

}  // namespace glz::detail

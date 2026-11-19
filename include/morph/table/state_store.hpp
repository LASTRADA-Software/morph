// SPDX-License-Identifier: Apache-2.0

#pragma once

/// @file table/state_store.hpp
/// @brief A table's persisted state: the store interface, the memory store, and the state document.
///
/// A table's arrangement (column layout, sort chain, filters, the saved view it came from) is stored as a JSON
/// document per table through an `ITableStateStore` the application injects. `readState` reads the stored
/// shape, `fitState` fits it to the description the caller receives, keeping what names a column or group the
/// description lacks as `carried`, and `writeState` writes it back with the carried parts in their places.
///
/// See `docs/spec/table/table.md`, "Persisted state".

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <glaze/glaze.hpp>
#include <iterator>
#include <map>
#include <morph/attributes.hpp>
#include <morph/core/completion.hpp>
#include <morph/core/executor.hpp>
#include <morph/core/owner_strand.hpp>
#include <morph/table/description.hpp>
#include <morph/table/engine.hpp>
#include <morph/table/filter.hpp>
#include <morph/table/sort.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace morph::table {

/// @brief Which table's state. No member names a person: a store keeps one person's state apart from another's
///        itself.
struct TableStateKey {
    /// @brief The application.
    std::string application;
    /// @brief The model's type; empty for a table over a document's rows.
    std::string model;
    /// @brief The table's id; for a `rows` table, the node's id.
    std::string table;
    /// @brief The screen's id, or what the table's `persist` names.
    std::string qualifier;
    /// @brief `"state"`, `"views"` or `"shared"`.
    std::string part;

    /// @brief Member-wise equality.
    /// @param other The key to compare with.
    /// @return `true` when every member matches.
    [[nodiscard]] bool operator==(TableStateKey const& other) const = default;
};

/// @brief A stored document and the revision that names it.
struct StoredTableState {
    /// @brief The document's JSON text.
    std::string json;
    /// @brief Names this stored document; opaque.
    std::string revision;
};

/// @brief What a save did.
enum class SaveOutcome : std::uint8_t {
    Saved,     ///< The document is stored.
    Conflict,  ///< The stored document's revision is not the expected one; nothing was written.
    Refused,   ///< The store would not keep the document; `reason` says why.
};

/// @brief The answer to a save.
struct SaveResult {
    /// @brief What the save did.
    SaveOutcome outcome = SaveOutcome::Saved;
    /// @brief The revision of what is stored now; empty when nothing is.
    std::string revision;
    /// @brief Why the store refused, for `Refused`.
    std::string reason;
};

/// @brief Where every table stores and loads its state. Completions are delivered on an executor the store owns.
class ITableStateStore {
public:
    ITableStateStore() = default;
    ITableStateStore(ITableStateStore const&) = delete;
    ITableStateStore& operator=(ITableStateStore const&) = delete;
    ITableStateStore(ITableStateStore&&) = delete;
    ITableStateStore& operator=(ITableStateStore&&) = delete;
    virtual ~ITableStateStore() = default;

    /// @brief Reads a stored document.
    /// @param key Which table's state, and which part.
    /// @return The stored document, or nothing when none is stored.
    [[nodiscard]] virtual async::Completion<std::optional<StoredTableState>> load(TableStateKey const& key) = 0;

    /// @brief Stores @p json. With @p expected empty it replaces whatever is stored; otherwise it stores only when
    ///        the stored document has that revision.
    /// @param key      Which table's state, and which part.
    /// @param json     The document.
    /// @param expected The revision the caller loaded, or empty.
    /// @return `Saved` with the new revision; `Conflict` with the current revision when @p expected does not hold.
    [[nodiscard]] virtual async::Completion<SaveResult> save(TableStateKey const& key, std::string json,
                                                             std::string expected = {}) = 0;

    /// @brief Forgets a stored document.
    /// @param key Which table's state, and which part.
    /// @return Whether a document was stored.
    [[nodiscard]] virtual async::Completion<bool> remove(TableStateKey const& key) = 0;

    /// @brief Whether the store keeps the `shared` part for everyone rather than for one person.
    /// @return `false` unless a store overrides it.
    [[nodiscard]] virtual bool shares() const { return false; }
};

/// @brief A store that keeps every document in a map, for as long as it lives.
///
/// Each call is a task on its own strand, and its completion is delivered there. Revisions are `"1"`, `"2"`, …
/// per key, counted across removals, so a revision never names two documents of one key.
class MemoryTableStateStore final : public ITableStateStore {
public:
    /// @brief A store whose strand runs on @p executor.
    /// @param executor Where the strand's tasks run and completions are delivered. Borrowed: it must outlive the
    ///                 store.
    explicit MemoryTableStateStore(exec::IExecutor& executor MORPH_LIFETIMEBOUND) : _strand{executor} {}

    /// @brief Reads a stored document.
    /// @param key Which table's state, and which part.
    /// @return The stored document, or nothing when none is stored.
    [[nodiscard]] async::Completion<std::optional<StoredTableState>> load(TableStateKey const& key) override {
        auto [completion, promise] = async::Completion<std::optional<StoredTableState>>::makeSettleable(&_strand);
        _strand.postTask([this, entry = tie(key), settle = std::move(promise)] mutable {
            auto const found = _documents.find(entry);
            settle.resolve(found == _documents.end() ? std::nullopt
                                                     : std::optional<StoredTableState>{found->second});
        });
        return std::move(completion);
    }

    /// @brief Stores @p json under the `expected` rule of `ITableStateStore::save`.
    /// @param key      Which table's state, and which part.
    /// @param json     The document.
    /// @param expected The revision the caller loaded, or empty.
    /// @return `Saved` with the new revision, or `Conflict` with the current one.
    [[nodiscard]] async::Completion<SaveResult> save(TableStateKey const& key, std::string json,
                                                     std::string expected = {}) override {
        auto [completion, promise] = async::Completion<SaveResult>::makeSettleable(&_strand);
        _strand.postTask([this, entry = tie(key), text = std::move(json), wanted = std::move(expected),
                          settle = std::move(promise)] mutable {
            auto const found = _documents.find(entry);
            std::string const current = found == _documents.end() ? std::string{} : found->second.revision;
            if (!wanted.empty() && wanted != current) {
                settle.resolve(SaveResult{.outcome = SaveOutcome::Conflict, .revision = current, .reason = {}});
                return;
            }
            std::string revision = std::to_string(++_counts[entry]);
            _documents.insert_or_assign(entry, StoredTableState{.json = std::move(text), .revision = revision});
            settle.resolve(SaveResult{.outcome = SaveOutcome::Saved, .revision = std::move(revision), .reason = {}});
        });
        return std::move(completion);
    }

    /// @brief Forgets a stored document.
    /// @param key Which table's state, and which part.
    /// @return Whether a document was stored.
    [[nodiscard]] async::Completion<bool> remove(TableStateKey const& key) override {
        auto [completion, promise] = async::Completion<bool>::makeSettleable(&_strand);
        _strand.postTask([this, entry = tie(key), settle = std::move(promise)] mutable {
            settle.resolve(_documents.erase(entry) != 0);
        });
        return std::move(completion);
    }

private:
    using Key = std::tuple<std::string, std::string, std::string, std::string, std::string>;

    [[nodiscard]] static Key tie(TableStateKey const& key) {
        return Key{key.application, key.model, key.table, key.qualifier, key.part};
    }

    std::map<Key, StoredTableState> _documents;
    std::map<Key, std::uint64_t> _counts;
    /// After the maps, so it is destroyed first: a task running on it reads them.
    exec::OwnerStrand _strand;
};

/// @brief The version of the state document this framework writes.
inline constexpr int kStateVersion = 1;

/// @brief Parts of a stored state that name a column or group the caller's description lacks. Not applied; merged
///        back by `writeState` into the places they had.
struct CarriedState {
    /// @brief A column the description lacks, with what the layout held for it.
    struct Column {
        /// @brief The column's id.
        std::string id;
        /// @brief The id it followed in the stored `order`; empty when it was first.
        std::string after;
        /// @brief Whether it was hidden.
        bool hidden = false;
        /// @brief Its width, when one was set.
        std::optional<int> width;
        /// @brief Its display units: dimension to unit id, as stored.
        std::map<std::string, std::string> units;

        /// @brief Member-wise equality.
        /// @param other The column to compare with.
        /// @return `true` when every member matches.
        [[nodiscard]] bool operator==(Column const& other) const = default;
    };
    /// @brief The columns, in stored order.
    std::vector<Column> columns;
    /// @brief Each sort key with its index in the stored chain.
    std::vector<std::pair<std::size_t, SortKey>> sort;
    /// @brief Column filters, by the absent column's id.
    std::map<std::string, ColumnFilter> filters;
    /// @brief Group filters naming an absent group or column.
    std::vector<GroupFilter> groupFilters;
    /// @brief User-formed groups that name an absent column, whole.
    std::vector<ColumnGroupInfo> groups;

    /// @brief Member-wise equality.
    /// @param other The carried state to compare with.
    /// @return `true` when every member matches.
    [[nodiscard]] bool operator==(CarriedState const& other) const = default;
};

/// @brief The `state` part of a table's persisted state: its arrangement in use.
struct StateDocument {
    /// @brief The document's version.
    int version = kStateVersion;
    /// @brief The column layout, when one is stored.
    std::optional<ColumnLayoutValue> layout;
    /// @brief The sort chain, when one is stored.
    std::optional<SortChain> sort;
    /// @brief The filters, when any are stored.
    std::optional<FilterSpec> filters;
    /// @brief The saved view the arrangement was taken from.
    std::optional<std::string> view;
    /// @brief What the caller's description lacks, written back as it was.
    CarriedState carried;
};

/// @brief One entry `fitState` dropped.
struct FitDrop {
    /// @brief What was dropped.
    enum class What : std::uint8_t {
        Column,       ///< A layout column.
        SortKey,      ///< A sort key.
        Filter,       ///< A column filter.
        GroupFilter,  ///< A group filter.
        Group,        ///< A user-formed group.
        Unit,         ///< A display unit.
    };
    /// @brief What was dropped.
    What what = What::Column;
    /// @brief The column or group; a user group's filter by its group's id; a filter over listed columns by them,
    ///        joined with `,`; a unit as `column/dimension`.
    std::string id;
    /// @brief `true`: kept in `carried`, the description lacking it; `false`: gone.
    bool carried = false;

    /// @brief Member-wise equality.
    /// @param other The drop to compare with.
    /// @return `true` when every member matches.
    [[nodiscard]] bool operator==(FitDrop const& other) const = default;
};

/// @brief What `fitState` dropped.
struct FitReport {
    /// @brief The drops, in the order of the fitting list: columns, sort keys, filters, units, user groups with
    ///        their filters.
    std::vector<FitDrop> dropped;
};

namespace detail {

/// @brief A sort key as the state document stores it: `{"column", "direction"}`, where the wire says `dir`.
struct StoredSortKey {
    /// @brief The column's id.
    std::string column;
    /// @brief The key's direction, `asc` or `desc`.
    SortDirection direction = SortDirection::Ascending;
};

/// @brief The state document as it is stored.
struct StoredState {
    /// @brief The document's version.
    int version = kStateVersion;
    /// @brief The column layout.
    std::optional<ColumnLayoutValue> layout;
    /// @brief The sort chain.
    std::optional<std::vector<StoredSortKey>> sort;
    /// @brief The filters.
    std::optional<FilterSpec> filters;
    /// @brief The saved view's name.
    std::optional<std::string> view;
};

/// @brief The version member alone, read before the rest.
struct StateVersion {
    /// @brief The version; a document without one reads as 0.
    int version = 0;
};

/// @brief Reads state documents leniently: a member this version does not know is skipped.
inline constexpr glz::opts kStateRead{.error_on_unknown_keys = false};

/// @brief Whether @p description lists unit @p unit of dimension @p dimension: whether a stored display unit still
///        names a unit the table's measures may be shown in.
/// @param description The description.
/// @param dimension   The dimension, such as `mass`.
/// @param unit        The unit's id.
/// @return `true` when `description.units` holds a unit of that id and dimension.
[[nodiscard]] inline bool listsUnit(TableDescription const& description, std::string_view dimension,
                                    std::string_view unit) {
    return std::ranges::any_of(
        description.units, [&](UnitInfo const& listed) { return listed.dimension == dimension && listed.id == unit; });
}

/// @brief Whether @p a and @p b hold the same column ids, in any order.
/// @param a One list.
/// @param b The other.
/// @return `true` when they hold the same ids.
[[nodiscard]] inline bool sameColumns(std::vector<std::string> a, std::vector<std::string> b) {
    std::ranges::sort(a);
    std::ranges::sort(b);
    return a == b;
}

/// @brief @p columns joined with `,`.
/// @param columns The ids.
/// @return The joined text.
[[nodiscard]] inline std::string joinedColumns(std::vector<std::string> const& columns) {
    std::string out;
    for (auto const& column : columns) {
        out += out.empty() ? column : "," + column;
    }
    return out;
}

}  // namespace detail

/// @brief Reads a stored state document, bringing an older version forward one version at a time.
///
/// No version before 1 exists, so a document of a lower version, or of none, has no migration and is refused, as
/// is one of a version above `kStateVersion`. A member this version does not know is skipped.
/// @param json The stored text.
/// @return The document, its `carried` empty; or a message when the text is not JSON, or its version is not one
///         this framework reads.
[[nodiscard]] inline std::expected<StateDocument, std::string> readState(std::string_view json) {
    detail::StateVersion probe;
    if (auto const error = glz::read<detail::kStateRead>(probe, json); error) {
        return std::unexpected("a table's state is not JSON: " + glz::format_error(error, json));
    }
    if (probe.version > kStateVersion) {
        return std::unexpected("a table's state is version " + std::to_string(probe.version) + "; this reads up to " +
                               std::to_string(kStateVersion));
    }
    if (probe.version < kStateVersion) {
        return std::unexpected("a table's state of version " + std::to_string(probe.version) +
                               " has no migration to version " + std::to_string(kStateVersion));
    }
    detail::StoredState stored;
    if (auto const error = glz::read<detail::kStateRead>(stored, json); error) {
        return std::unexpected("a table's state does not read: " + glz::format_error(error, json));
    }
    StateDocument document;
    document.version = stored.version;
    document.layout = std::move(stored.layout);
    if (stored.sort) {
        SortChain chain;
        chain.reserve(stored.sort->size());
        for (auto& key : *stored.sort) {
            chain.push_back(SortKey{.column = std::move(key.column), .dir = key.direction});
        }
        document.sort = std::move(chain);
    }
    document.filters = std::move(stored.filters);
    document.view = std::move(stored.view);
    return document;
}

/// @brief Writes a state document at `kStateVersion`, its carried parts merged back into their places.
///
/// A carried column goes into `order` right after its `after` id, first when `after` is empty, or at the end when
/// `after` is not in the written order, with its hidden flag, width and units; a carried sort key goes in at its
/// stored index, clamped to the chain's length; carried filters, group filters and groups are appended.
/// @param document The document.
/// @return The JSON text.
[[nodiscard]] inline std::string writeState(StateDocument const& document) {
    CarriedState const& carried = document.carried;
    detail::StoredState out;
    out.version = kStateVersion;
    out.view = document.view;
    if (document.layout || !carried.columns.empty() || !carried.groups.empty()) {
        ColumnLayoutValue layout = document.layout.value_or(ColumnLayoutValue{});
        std::unordered_set<std::string> hidden{layout.hidden.begin(), layout.hidden.end()};
        for (auto const& column : carried.columns) {
            auto place = layout.order.begin();
            if (!column.after.empty()) {
                place = std::ranges::find(layout.order, column.after);
                if (place != layout.order.end()) {
                    ++place;
                }
            }
            layout.order.insert(place, column.id);
            if (column.hidden) {
                hidden.insert(column.id);
            }
            if (column.width) {
                layout.widths.insert_or_assign(column.id, *column.width);
            }
            if (!column.units.empty()) {
                layout.units.insert_or_assign(column.id, column.units);
            }
        }
        layout.hidden.clear();
        for (auto const& id : layout.order) {
            if (hidden.contains(id)) {
                layout.hidden.push_back(id);
            }
        }
        layout.groups.insert(layout.groups.end(), carried.groups.begin(), carried.groups.end());
        out.layout = std::move(layout);
    }
    if (document.sort || !carried.sort.empty()) {
        SortChain chain = document.sort.value_or(SortChain{});
        auto keys = carried.sort;
        std::ranges::stable_sort(keys, {}, &std::pair<std::size_t, SortKey>::first);
        for (auto const& [index, key] : keys) {
            chain.insert(chain.begin() + static_cast<std::ptrdiff_t>(std::min(index, chain.size())), key);
        }
        std::vector<detail::StoredSortKey> stored;
        stored.reserve(chain.size());
        for (auto const& key : chain) {
            stored.push_back(detail::StoredSortKey{.column = key.column, .direction = key.dir});
        }
        out.sort = std::move(stored);
    }
    if (document.filters || !carried.filters.empty() || !carried.groupFilters.empty()) {
        FilterSpec filters = document.filters.value_or(FilterSpec{});
        for (auto const& [id, filter] : carried.filters) {
            filters.columns.insert_or_assign(id, filter);
        }
        filters.groups.insert(filters.groups.end(), carried.groupFilters.begin(), carried.groupFilters.end());
        out.filters = std::move(filters);
    }
    return glz::write_json(out).value_or(std::string{});
}

/// @brief Fits a stored state to the description the caller receives; the state is never refused.
///
/// In the order of the fitting list: columns are fitted as `ColumnLayout::apply` fits them; a sort key on a
/// missing or unsortable column, or repeating an earlier key's column, is dropped; a filter on a missing column or
/// group, or one that does not compile for its columns, is dropped, and so is a display unit the description does
/// not list; a user-formed group left with fewer than two columns is dropped with its filter. A user group's
/// filter is the group filter with no `group` whose `columns` are the group's, in any order.
///
/// What is dropped because the description lacks its column or group moves into `carried`; anything else is gone.
/// A user group that names an absent column is carried whole, with its filter.
/// @param document    The stored state; its own `carried` is replaced.
/// @param description The caller's description.
/// @param report      Receives every drop, appended in the order above.
/// @return The fitted document.
[[nodiscard]] inline StateDocument fitState(StateDocument document, TableDescription const& description,
                                            FitReport& report) {
    using What = FitDrop::What;
    std::unordered_map<std::string_view, TableColumn const*> columns;
    columns.reserve(description.columns.size());
    for (auto const& column : description.columns) {
        columns.emplace(column.info.id, &column);
    }
    auto const present = [&columns](std::string const& id) { return columns.contains(id); };
    auto const drop = [&report](What what, std::string id, bool isCarried) {
        report.dropped.push_back(FitDrop{.what = what, .id = std::move(id), .carried = isCarried});
    };
    CarriedState carried;

    // Columns, and the fate of every stored user group, decided here and reported last.
    enum class Fate : std::uint8_t { Kept, Carried, Gone };
    std::vector<ColumnGroupInfo> userGroups;
    std::vector<Fate> fates;
    if (document.layout) {
        ColumnLayoutValue& layout = *document.layout;
        std::unordered_set<std::string> const hidden{layout.hidden.begin(), layout.hidden.end()};
        std::unordered_set<std::string> seen;
        std::string previous;
        for (auto const& id : layout.order) {
            if (!present(id) && seen.insert(id).second) {
                CarriedState::Column column{.id = id, .after = previous, .hidden = hidden.contains(id)};
                if (auto const width = layout.widths.find(id); width != layout.widths.end()) {
                    column.width = width->second;
                }
                if (auto const units = layout.units.find(id); units != layout.units.end()) {
                    column.units = units->second;
                }
                carried.columns.push_back(std::move(column));
                drop(What::Column, id, true);
            }
            previous = id;
        }
        userGroups = std::move(layout.groups);
        layout.groups.clear();
        fates.assign(userGroups.size(), Fate::Kept);
        for (std::size_t at = 0; at < userGroups.size(); ++at) {
            if (std::ranges::all_of(userGroups[at].columns, present)) {
                layout.groups.push_back(userGroups[at]);
            } else {
                fates[at] = Fate::Carried;
            }
        }
        std::vector<std::string> ids;
        std::vector<std::string> declaredHidden;
        ids.reserve(description.columns.size());
        for (auto const& column : description.columns) {
            ids.push_back(column.info.id);
            if (!column.shown) {
                declaredHidden.push_back(column.info.id);
            }
        }
        ColumnLayout fitter{std::move(ids), std::move(declaredHidden)};
        fitter.apply(layout);
        ColumnLayoutValue fitted = fitter.value();
        // The fitted groups are the kept ones, in order, with the dropped ones left out.
        std::size_t next = 0;
        for (std::size_t at = 0; at < userGroups.size(); ++at) {
            if (fates[at] != Fate::Kept) {
                continue;
            }
            if (next < fitted.groups.size() && fitted.groups[next].id == userGroups[at].id) {
                ++next;
            } else {
                fates[at] = Fate::Gone;
            }
        }
        layout = std::move(fitted);
    }

    if (document.sort) {
        SortChain kept;
        for (std::size_t index = 0; index < document.sort->size(); ++index) {
            SortKey const& key = (*document.sort)[index];
            auto const found = columns.find(key.column);
            if (found == columns.end()) {
                carried.sort.emplace_back(index, key);
                drop(What::SortKey, key.column, true);
            } else if (!found->second->sortable || std::ranges::contains(kept, key.column, &SortKey::column)) {
                drop(What::SortKey, key.column, false);
            } else {
                kept.push_back(key);
            }
        }
        document.sort = std::move(kept);
    }

    // A user group's filter is decided with its group and reported after it.
    std::vector<std::vector<FitDrop>> groupFilterDrops(userGroups.size());
    if (document.filters) {
        FilterSpec& filters = *document.filters;
        auto const infos = columnInfos(description);
        // A measure operand compiles against the units the description lists, the only ones a client knows.
        Services services;
        services.units = registryOf(description).value_or(nullptr);
        auto const compiles = [&infos, &description, &services](FilterSpec const& spec) {
            return compileFilterGuarded(spec, infos, services, description.groups).has_value();
        };
        for (auto entry = filters.columns.begin(); entry != filters.columns.end();) {
            auto const& [id, filter] = *entry;
            if (!present(id)) {
                carried.filters.insert_or_assign(id, filter);
                drop(What::Filter, id, true);
            } else if (!compiles(FilterSpec{.columns = {{id, filter}}, .groups = {}})) {
                drop(What::Filter, id, false);
            } else {
                ++entry;
                continue;
            }
            entry = filters.columns.erase(entry);
        }
        std::vector<GroupFilter> kept;
        for (auto& filter : filters.groups) {
            bool const compiled = compiles(FilterSpec{.columns = {}, .groups = {filter}});
            if (!filter.group.empty()) {
                if (!std::ranges::contains(description.groups, filter.group, &ColumnGroupInfo::id)) {
                    drop(What::GroupFilter, filter.group, true);
                    carried.groupFilters.push_back(std::move(filter));
                } else if (!compiled) {
                    drop(What::GroupFilter, filter.group, false);
                } else {
                    kept.push_back(std::move(filter));
                }
                continue;
            }
            auto const owner = std::ranges::find_if(userGroups, [&filter](ColumnGroupInfo const& group) {
                return detail::sameColumns(group.columns, filter.columns);
            });
            if (owner != userGroups.end()) {
                auto const at = static_cast<std::size_t>(std::distance(userGroups.begin(), owner));
                if (fates[at] == Fate::Carried) {
                    groupFilterDrops[at].push_back({What::GroupFilter, owner->id, true});
                    carried.groupFilters.push_back(std::move(filter));
                } else if (fates[at] == Fate::Gone || !compiled) {
                    groupFilterDrops[at].push_back({What::GroupFilter, owner->id, false});
                } else {
                    kept.push_back(std::move(filter));
                }
                continue;
            }
            if (!std::ranges::all_of(filter.columns, present)) {
                drop(What::GroupFilter, detail::joinedColumns(filter.columns), true);
                carried.groupFilters.push_back(std::move(filter));
            } else if (!compiled) {
                drop(What::GroupFilter, detail::joinedColumns(filter.columns), false);
            } else {
                kept.push_back(std::move(filter));
            }
        }
        filters.groups = std::move(kept);
    }

    if (document.layout) {
        auto& units = document.layout->units;
        for (auto column = units.begin(); column != units.end();) {
            auto& dimensions = column->second;
            for (auto unit = dimensions.begin(); unit != dimensions.end();) {
                if (detail::listsUnit(description, unit->first, unit->second)) {
                    ++unit;
                } else {
                    drop(What::Unit, column->first + "/" + unit->first, false);
                    unit = dimensions.erase(unit);
                }
            }
            column = dimensions.empty() ? units.erase(column) : std::next(column);
        }
    }

    for (std::size_t at = 0; at < userGroups.size(); ++at) {
        if (fates[at] == Fate::Carried) {
            drop(What::Group, userGroups[at].id, true);
            carried.groups.push_back(userGroups[at]);
        } else if (fates[at] == Fate::Gone) {
            drop(What::Group, userGroups[at].id, false);
        }
        report.dropped.insert(report.dropped.end(), groupFilterDrops[at].begin(), groupFilterDrops[at].end());
    }
    document.carried = std::move(carried);
    return document;
}

}  // namespace morph::table

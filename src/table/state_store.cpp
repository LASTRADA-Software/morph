// SPDX-License-Identifier: Apache-2.0

// The non-template definitions of <morph/table/state_store.hpp>.

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
#include <morph/table/data_source.hpp>
#include <morph/table/description.hpp>
#include <morph/table/engine.hpp>
#include <morph/table/filter.hpp>
#include <morph/table/sort.hpp>
#include <morph/table/state_store.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

namespace morph::table::detail {

StoredView storedView(SavedView const& view) {
    StoredView out{.name = view.name, .layout = view.layout, .sort = {}, .filters = view.filters};
    out.sort.reserve(view.sort.size());
    for (auto const& key : view.sort) {
        out.sort.push_back(StoredSortKey{.column = key.column, .direction = key.dir});
    }
    return out;
}

SavedView savedView(StoredView stored) {
    SavedView out{.name = std::move(stored.name),
                  .layout = std::move(stored.layout),
                  .sort = {},
                  .filters = std::move(stored.filters)};
    out.sort.reserve(stored.sort.size());
    for (auto& key : stored.sort) {
        out.sort.push_back(SortKey{.column = std::move(key.column), .dir = key.direction});
    }
    return out;
}

std::unexpected<TableError> viewsError(std::string message) {
    return std::unexpected(
        TableError{.code = TableErrorCode::InvalidSpec, .column = {}, .message = std::move(message)});
}

}  // namespace morph::table::detail

namespace morph::table {

std::expected<ViewsDocument, TableError> readViews(std::string_view json) {
    auto const probe = detail::readViewsJson<detail::ViewsVersion>(json, "a views document");
    if (!probe) {
        return std::unexpected(probe.error());
    }
    if (probe->version != kViewsVersion) {
        return detail::viewsError("a views document is version " + std::to_string(probe->version) +
                                  "; this reads version " + std::to_string(kViewsVersion));
    }
    auto stored = detail::readViewsJson<detail::StoredViews>(json, "a views document");
    if (!stored) {
        return std::unexpected(stored.error());
    }
    ViewsDocument document{.version = stored->version,
                           .defaultView = std::move(stored->defaultView),
                           .defaultOnMount = stored->defaultOnMount.value_or(false),
                           .views = {}};
    for (auto& view : stored->views) {
        if (!std::ranges::contains(document.views, view.name, &SavedView::name)) {
            document.views.push_back(detail::savedView(std::move(view)));
        }
    }
    return document;
}

std::string writeViews(ViewsDocument const& document, ViewsPart part) {
    detail::StoredViews out;
    if (part == ViewsPart::Own) {
        out.defaultView = document.defaultView;
        out.defaultOnMount = document.defaultOnMount;
    }
    out.views.reserve(document.views.size());
    for (auto const& view : document.views) {
        out.views.push_back(detail::storedView(view));
    }
    return detail::viewsJson(out);
}

std::string writeViewOperation(ViewOperation const& operation) {
    struct Writer {
        std::string operator()(AddView const& add) const {
            return R"({"add":)" + detail::viewsJson(detail::storedView(add.view)) + "}";
        }
        std::string operator()(ReplaceView const& replace) const {
            return R"({"replace":)" + detail::viewsJson(detail::storedView(replace.view)) + "}";
        }
        std::string operator()(RenameView const& rename) const {
            return R"({"rename":)" + detail::viewsJson(rename) + "}";
        }
        std::string operator()(RemoveView const& remove) const {
            return R"({"remove":)" + detail::viewsJson(remove.name) + "}";
        }
        std::string operator()(SetDefaultView const& set) const {
            return R"({"default":)" + (set.name ? detail::viewsJson(*set.name) : std::string{"null"}) + "}";
        }
        std::string operator()(SetDefaultOnMount const& set) const {
            return std::string{R"({"defaultOnMount":)"} + (set.on ? "true" : "false") + "}";
        }
    };
    return std::visit(Writer{}, operation);
}

std::expected<ViewOperation, TableError> readViewOperation(std::string_view json) {
    auto const members = detail::readViewsJson<std::map<std::string, glz::raw_json>>(json, "a views operation");
    if (!members) {
        return std::unexpected(members.error());
    }
    if (members->size() != 1) {
        return detail::viewsError("a views operation has exactly one member");
    }
    auto const& [name, raw] = *members->begin();
    std::string_view const text = raw.str;
    auto const view = [text]() -> std::expected<SavedView, TableError> {
        auto stored = detail::readViewsJson<detail::StoredView>(text, "a saved view");
        if (!stored) {
            return std::unexpected(stored.error());
        }
        if (stored->name.empty()) {
            return detail::viewsError("a saved view has a name");
        }
        return detail::savedView(std::move(*stored));
    };
    if (name == "add" || name == "replace") {
        auto read = view();
        if (!read) {
            return std::unexpected(read.error());
        }
        return name == "add" ? ViewOperation{AddView{std::move(*read)}} : ViewOperation{ReplaceView{std::move(*read)}};
    }
    if (name == "rename") {
        auto read = detail::readViewsJson<RenameView>(text, "a rename");
        if (!read) {
            return std::unexpected(read.error());
        }
        return ViewOperation{std::move(*read)};
    }
    if (name == "remove") {
        auto read = detail::readViewsJson<std::string>(text, "a remove");
        if (!read) {
            return std::unexpected(read.error());
        }
        return ViewOperation{RemoveView{std::move(*read)}};
    }
    if (name == "default") {
        auto read = detail::readViewsJson<std::optional<std::string>>(text, "a default");
        if (!read) {
            return std::unexpected(read.error());
        }
        return ViewOperation{SetDefaultView{std::move(*read)}};
    }
    if (name == "defaultOnMount") {
        auto read = detail::readViewsJson<bool>(text, "a defaultOnMount");
        if (!read) {
            return std::unexpected(read.error());
        }
        return ViewOperation{SetDefaultOnMount{*read}};
    }
    return detail::viewsError("a views operation `" + name + "` is not one this framework knows");
}

std::expected<ViewsDocument, ViewError> applyViewOperation(ViewsDocument document, ViewOperation const& operation,
                                                           ViewsPart part) {
    auto const find = [&document](std::string const& name) {
        return std::ranges::find(document.views, name, &SavedView::name);
    };
    struct Applier {
        // NOLINTNEXTLINE(cppcoreguidelines-avoid-const-or-ref-data-members): the applier lives only inside this call, over its locals.
        ViewsDocument& document;
        ViewsPart part;
        // NOLINTNEXTLINE(cppcoreguidelines-avoid-const-or-ref-data-members): the applier lives only inside this call, over its locals.
        decltype(find)& at;

        std::expected<void, ViewError> operator()(AddView const& add) const {
            if (at(add.view.name) != document.views.end()) {
                return std::unexpected(ViewError::NameTaken);
            }
            document.views.push_back(add.view);
            return {};
        }
        std::expected<void, ViewError> operator()(ReplaceView const& replace) const {
            auto const found = at(replace.view.name);
            if (found == document.views.end()) {
                return std::unexpected(ViewError::NoSuchView);
            }
            *found = replace.view;
            return {};
        }
        std::expected<void, ViewError> operator()(RenameView const& rename) const {
            auto const found = at(rename.from);
            if (found == document.views.end()) {
                return std::unexpected(ViewError::NoSuchView);
            }
            if (rename.to != rename.from && at(rename.to) != document.views.end()) {
                return std::unexpected(ViewError::NameTaken);
            }
            found->name = rename.to;
            if (document.defaultView == rename.from) {
                document.defaultView = rename.to;
            }
            return {};
        }
        std::expected<void, ViewError> operator()(RemoveView const& remove) const {
            auto const found = at(remove.name);
            if (found == document.views.end()) {
                return std::unexpected(ViewError::NoSuchView);
            }
            document.views.erase(found);
            if (document.defaultView == remove.name) {
                document.defaultView.reset();
            }
            return {};
        }
        std::expected<void, ViewError> operator()(SetDefaultView const& set) const {
            if (part == ViewsPart::Shared) {
                return std::unexpected(ViewError::NotShared);
            }
            if (set.name && at(*set.name) == document.views.end()) {
                return std::unexpected(ViewError::NoSuchView);
            }
            document.defaultView = set.name;
            return {};
        }
        std::expected<void, ViewError> operator()(SetDefaultOnMount const& set) const {
            if (part == ViewsPart::Shared) {
                return std::unexpected(ViewError::NotShared);
            }
            document.defaultOnMount = set.on;
            return {};
        }
    };
    if (auto const applied = std::visit(Applier{.document = document, .part = part, .at = find}, operation);
        !applied) {
        return std::unexpected(applied.error());
    }
    return document;
}

async::Completion<SaveResult> ITableStateStore::refusePublish(exec::IExecutor& executor) {
    auto [completion, promise] = async::Completion<SaveResult>::makeSettleable(&executor);
    promise.resolve(
        SaveResult{.outcome = SaveOutcome::Refused, .revision = {}, .reason = "this store does not share views"});
    return std::move(completion);
}

std::string viewErrorText(ViewError error, ViewOperation const& operation) {
    std::string const name = std::visit(
        []<typename Operation>(Operation const& each) -> std::string {
            if constexpr (std::is_same_v<Operation, AddView> || std::is_same_v<Operation, ReplaceView>) {
                return each.view.name;
            } else if constexpr (std::is_same_v<Operation, RenameView>) {
                return each.to;
            } else if constexpr (std::is_same_v<Operation, RemoveView>) {
                return each.name;
            } else if constexpr (std::is_same_v<Operation, SetDefaultView>) {
                return each.name.value_or(std::string{});
            } else {
                return {};
            }
        },
        operation);
    switch (error) {
        case ViewError::NameTaken:
            return "A view named " + name + " exists";
        case ViewError::NoSuchView:
            return "No view named " +
                   (std::holds_alternative<RenameView>(operation) ? std::get<RenameView>(operation).from : name);
        case ViewError::NotShared:
            return "The shared views have no default";
        default:
            break;
    }
    return {};
}

async::Completion<std::optional<StoredTableState>> MemoryTableStateStore::load(TableStateKey const& key) {
    auto [completion, promise] = async::Completion<std::optional<StoredTableState>>::makeSettleable(&_strand);
    _strand.postTask([this, entry = tie(key), settle = std::move(promise)] mutable {
        auto const found = _documents.find(entry);
        settle.resolve(found == _documents.end() ? std::nullopt : std::optional<StoredTableState>{found->second});
    });
    return std::move(completion);
}

async::Completion<SaveResult> MemoryTableStateStore::save(TableStateKey const& key, std::string json,
                                                          std::string expected) {
    auto [completion, promise] = async::Completion<SaveResult>::makeSettleable(&_strand);
    _strand.postTask([this, entry = tie(key), text = std::move(json), wanted = std::move(expected),
                      settle = std::move(promise)] mutable { settle.resolve(store(entry, std::move(text), wanted)); });
    return std::move(completion);
}

async::Completion<SaveResult> MemoryTableStateStore::publish(TableStateKey const& key, ViewOperation const& operation,
                                                             std::string expected) {
    if (_sharing == Sharing::Private) {
        return refusePublish(_strand);
    }
    auto [completion, promise] = async::Completion<SaveResult>::makeSettleable(&_strand);
    _strand.postTask([this, entry = tie(key), part = key.part, change = operation, wanted = std::move(expected),
                      settle = std::move(promise)] mutable {
        auto const refuse = [&settle](std::string current, std::string reason) {
            settle.resolve(SaveResult{
                .outcome = SaveOutcome::Refused, .revision = std::move(current), .reason = std::move(reason)});
        };
        auto const found = _documents.find(entry);
        std::string const current = found == _documents.end() ? std::string{} : found->second.revision;
        if (part != "shared") {
            refuse(current, "views are published to the shared part");
            return;
        }
        if (auto conflict = conflictOf(found, wanted)) {
            settle.resolve(std::move(*conflict));
            return;
        }
        ViewsDocument document;
        if (found != _documents.end()) {
            auto read = readViews(found->second.json);
            if (!read) {
                refuse(current, read.error().message);
                return;
            }
            document = std::move(*read);
        }
        auto applied = applyViewOperation(std::move(document), change, ViewsPart::Shared);
        if (!applied) {
            refuse(current, viewErrorText(applied.error(), change));
            return;
        }
        settle.resolve(store(entry, writeViews(*applied, ViewsPart::Shared), wanted));
    });
    return std::move(completion);
}

async::Completion<bool> MemoryTableStateStore::remove(TableStateKey const& key) {
    auto [completion, promise] = async::Completion<bool>::makeSettleable(&_strand);
    _strand.postTask([this, entry = tie(key), settle = std::move(promise)] mutable {
        settle.resolve(_documents.erase(entry) != 0);
    });
    return std::move(completion);
}

std::optional<SaveResult> MemoryTableStateStore::conflictOf(Documents::const_iterator found,
                                                            std::string const& wanted) const {
    bool const stored = found != _documents.end();
    std::string const current = stored ? found->second.revision : std::string{};
    bool const holds = wanted.empty() || (wanted == kNoDocument ? !stored : stored && wanted == current);
    if (holds) {
        return std::nullopt;
    }
    return SaveResult{.outcome = SaveOutcome::Conflict, .revision = current, .reason = {}};
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters): the text to store, then the text the entry is expected to hold.
SaveResult MemoryTableStateStore::store(Key const& entry, std::string text, std::string const& wanted) {
    if (auto conflict = conflictOf(_documents.find(entry), wanted)) {
        return std::move(*conflict);
    }
    std::string revision = std::to_string(++_counts[entry]);
    _documents.insert_or_assign(entry, StoredTableState{.json = std::move(text), .revision = revision});
    return SaveResult{.outcome = SaveOutcome::Saved, .revision = std::move(revision), .reason = {}};
}

}  // namespace morph::table

namespace morph::table::detail {

bool listsUnit(TableDescription const& description, std::string_view dimension, std::string_view unit) {
    return std::ranges::any_of(
        description.units, [&](UnitInfo const& listed) { return listed.dimension == dimension && listed.id == unit; });
}

bool sameColumns(std::vector<std::string> left, std::vector<std::string> right) {
    std::ranges::sort(left);
    std::ranges::sort(right);
    return left == right;
}

std::string joinedColumns(std::vector<std::string> const& columns) {
    std::string out;
    for (auto const& column : columns) {
        out += out.empty() ? column : "," + column;
    }
    return out;
}

}  // namespace morph::table::detail

namespace morph::table {

std::expected<StateDocument, std::string> readState(std::string_view json) {
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

// NOLINTNEXTLINE(readability-function-cognitive-complexity): one sequential pass whose steps share its running state; splitting it would thread that state through parameters.
std::string writeState(StateDocument const& document) {
    CarriedState const& carried = document.carried;
    detail::StoredState out;
    out.version = kStateVersion;
    out.view = document.view;
    if (document.layout || !carried.columns.empty() || !carried.groups.empty()) {
        ColumnLayoutValue layout = document.layout.value_or(ColumnLayoutValue{});
        std::unordered_set<std::string> hidden{layout.hidden.begin(), layout.hidden.end()};
        // A stored `order` may leave columns out (fitting places them), so a hidden column it does not name is kept
        // too, after those it does, in the order the layout listed it.
        std::vector<std::string> unordered;
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
        for (auto const& columnId : layout.hidden) {
            if (std::ranges::find(layout.order, columnId) == layout.order.end() &&
                std::ranges::find(unordered, columnId) == unordered.end()) {
                unordered.push_back(columnId);
            }
        }
        layout.hidden.clear();
        for (auto const& columnId : layout.order) {
            if (hidden.contains(columnId)) {
                layout.hidden.push_back(columnId);
            }
        }
        layout.hidden.insert(layout.hidden.end(), unordered.begin(), unordered.end());
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
        for (auto const& [columnId, filter] : carried.filters) {
            filters.columns.insert_or_assign(columnId, filter);
        }
        filters.groups.insert(filters.groups.end(), carried.groupFilters.begin(), carried.groupFilters.end());
        out.filters = std::move(filters);
    }
    return glz::write_json(out).value_or(std::string{});
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity): one sequential pass whose steps share its running state; splitting it would thread that state through parameters.
StateDocument fitState(StateDocument document, TableDescription const& description, FitReport& report) {
    using What = FitDrop::What;
    std::unordered_map<std::string_view, TableColumn const*> columns;
    columns.reserve(description.columns.size());
    for (auto const& column : description.columns) {
        columns.emplace(column.info.id, &column);
    }
    auto const present = [&columns](std::string const& columnId) { return columns.contains(columnId); };
    auto const drop = [&report](What what, std::string columnId, bool isCarried) {
        report.dropped.push_back(FitDrop{.what = what, .id = std::move(columnId), .carried = isCarried});
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
        for (auto const& columnId : layout.order) {
            if (!present(columnId) && seen.insert(columnId).second) {
                CarriedState::Column column{.id = columnId, .after = previous, .hidden = hidden.contains(columnId)};
                if (auto const width = layout.widths.find(columnId); width != layout.widths.end()) {
                    column.width = width->second;
                }
                if (auto const units = layout.units.find(columnId); units != layout.units.end()) {
                    column.units = units->second;
                }
                carried.columns.push_back(std::move(column));
                drop(What::Column, columnId, true);
            }
            previous = columnId;
        }
        userGroups = std::move(layout.groups);
        layout.groups.clear();
        fates.assign(userGroups.size(), Fate::Kept);
        // NOLINTBEGIN(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access): fates has one slot per user group and at < userGroups.size()
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
        // NOLINTEND(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        layout = std::move(fitted);
    }

    if (document.sort) {
        SortChain kept;
        for (std::size_t index = 0; index < document.sort->size(); ++index) {
            // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access): index < document.sort->size()
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
            auto const& [columnId, filter] = *entry;
            if (!present(columnId)) {
                carried.filters.insert_or_assign(columnId, filter);
                drop(What::Filter, columnId, true);
            } else if (!compiles(FilterSpec{.columns = {{columnId, filter}}, .groups = {}})) {
                drop(What::Filter, columnId, false);
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
            // NOLINTBEGIN(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access): groupIndex < userGroups.size()
            if (owner != userGroups.end()) {
                auto const groupIndex = static_cast<std::size_t>(std::distance(userGroups.begin(), owner));
                if (fates[groupIndex] == Fate::Carried) {
                    groupFilterDrops[groupIndex].push_back(
                        {.what = What::GroupFilter, .id = owner->id, .carried = true});
                    carried.groupFilters.push_back(std::move(filter));
                } else if (fates[groupIndex] == Fate::Gone || !compiled) {
                    groupFilterDrops[groupIndex].push_back(
                        {.what = What::GroupFilter, .id = owner->id, .carried = false});
                } else {
                    kept.push_back(std::move(filter));
                }
                continue;
            }
            // NOLINTEND(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
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

    // NOLINTBEGIN(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access): fates and groupFilterDrops have one slot per user group and at < userGroups.size()
    for (std::size_t at = 0; at < userGroups.size(); ++at) {
        if (fates[at] == Fate::Carried) {
            drop(What::Group, userGroups[at].id, true);
            carried.groups.push_back(userGroups[at]);
        } else if (fates[at] == Fate::Gone) {
            drop(What::Group, userGroups[at].id, false);
        }
        report.dropped.insert(report.dropped.end(), groupFilterDrops[at].begin(), groupFilterDrops[at].end());
    }
    // NOLINTEND(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    document.carried = std::move(carried);
    return document;
}

}  // namespace morph::table

// SPDX-License-Identifier: Apache-2.0

#pragma once

/// @file ui/table_dialogs.hpp
/// @brief The column chooser, the sort dialog and the view name dialog a table's menu opens: what each lists, the
///        edits the sort dialog makes to a chain, and the dialogs' view trees.
///
/// The view trees hold no state of their own: the lowering of a table node gives them what to read and what to call,
/// and carries out each change through the same gesture functions the header and the menus use
/// (`applyLayoutGesture`, `applySortGesture`), so a change made in a dialog is applied, refused and reported as the
/// same change made from a header is. See `docs/spec/table/table.md`, "Configuring a table".

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <morph/table/engine.hpp>
#include <morph/table/sort.hpp>
#include <morph/ui/table_config.hpp>
#include <morph/ui/view.hpp>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace morph::ui {

// ---------------------------------------------------------------------------------------------------------------
// The column chooser
// ---------------------------------------------------------------------------------------------------------------

/// @brief One column as the column chooser lists it.
struct ChooserColumn {
    /// @brief The column's id.
    std::string id;
    /// @brief Its label, the check box's caption.
    std::string label;
    /// @brief What it holds; "Group columns…" offers only columns of the first ticked column's kind.
    table::ColumnKind kind = table::ColumnKind::Text;
    /// @brief Whether it is visible.
    bool shown = false;
    /// @brief Whether its check box can be toggled: hiding is allowed, it is not locked, and it is not the only
    ///        visible column.
    bool toggles = false;
    /// @brief Whether "Move up" can move it: as "Move left" can in its header's menu.
    bool up = false;
    /// @brief Whether "Move down" can move it: as "Move right" can in its header's menu.
    bool down = false;
    /// @brief Whether the table lets its user move columns, which draws the move buttons.
    bool movable = false;

    /// @brief Memberwise equality.
    /// @return Whether every member matches.
    bool operator==(ChooserColumn const&) const = default;
};

/// @brief One user-formed group as the column chooser lists it.
struct ChooserGroup {
    /// @brief The group's id.
    std::string id;
    /// @brief Its label.
    std::string label;
    /// @brief How many columns it has.
    std::size_t columns = 0;

    /// @brief Memberwise equality.
    /// @return Whether every member matches.
    bool operator==(ChooserGroup const&) const = default;
};

/// @brief What the column chooser shows.
struct ChooserState {
    /// @brief Every column of the layout, in its order, hidden ones included.
    std::vector<ChooserColumn> columns;
    /// @brief The groups the user formed, in the order they were formed.
    std::vector<ChooserGroup> groups;
    /// @brief Whether the table lets its user move columns: the move buttons are drawn.
    bool reorder = false;
    /// @brief Whether the table lets its user hide columns: the check boxes can be toggled.
    bool hide = false;
    /// @brief Whether "Group columns…" is offered: while `reorder` or `hide` is allowed.
    bool grouping = false;

    /// @brief Memberwise equality.
    /// @return Whether every member matches.
    bool operator==(ChooserState const&) const = default;
};

/// @brief What the column chooser lists for a layout.
///
/// A column the layout orders but @p columns does not list is left out, so a companion column, which is not among a
/// table's columns, is never listed.
/// @param layout  The layout the table holds now.
/// @param columns The table's columns.
/// @param allowed What the table lets its user change.
/// @return The columns in the layout's order, with what each can do, and the user-formed groups.
[[nodiscard]] inline ChooserState chooserState(table::ColumnLayout const& layout, std::span<LayoutColumn const> columns,
                                               Configure allowed) {
    ChooserState out;
    out.reorder = allows(allowed, Configure::Reorder);
    out.hide = allows(allowed, Configure::Hide);
    out.grouping = out.reorder || out.hide;
    std::vector<std::string> const visible = layout.visible();
    std::size_t const pinned = layout.pinned();
    for (std::string const& id : layout.order()) {
        auto const found = std::ranges::find(columns, id, &LayoutColumn::id);
        if (found == columns.end()) {
            continue;
        }
        std::size_t const at = detail::visibleIndex(visible, id);
        bool const shown = at < visible.size();
        bool const free = !found->locked;
        out.columns.push_back(ChooserColumn{
            .id = id,
            .label = found->label,
            .kind = found->kind,
            .shown = shown,
            .toggles = out.hide && free && !(shown && visible.size() == 1),
            // A move stays on its side of the pin boundary, as the header's "Move left" and "Move right" do.
            .up = out.reorder && free && shown && at > 0 && at != pinned,
            .down = out.reorder && free && shown && at + 1 < visible.size() && at + 1 != pinned,
            .movable = out.reorder,
        });
    }
    for (table::ColumnGroupInfo const& group : layout.groups()) {
        out.groups.push_back(ChooserGroup{.id = group.id, .label = group.label, .columns = group.columns.size()});
    }
    return out;
}

/// @brief The line the column chooser shows for a user-formed group.
/// @param group The group.
/// @return `{label}, {k} columns`, as `Pair, 2 columns`.
[[nodiscard]] inline std::string chooserGroupText(ChooserGroup const& group) {
    return group.label + ", " + groupedCount(static_cast<std::int64_t>(group.columns)) + " columns";
}

/// @brief The group "Group columns…" is forming: open or not, its label and the columns ticked so far.
struct GroupDraft {
    /// @brief Whether the form is shown.
    bool open = false;
    /// @brief The label typed.
    std::string label;
    /// @brief The ticked columns' ids, in the order they were ticked; the first decides the kind the rest must hold.
    std::vector<std::string> ticked;

    /// @brief Memberwise equality.
    /// @return Whether every member matches.
    bool operator==(GroupDraft const&) const = default;
};

/// @brief Whether a draft can form its group: a label with more than spaces, and 2 to 16 ticked columns.
/// @param draft The draft.
/// @return Whether "Create group" is enabled.
[[nodiscard]] inline bool canFormGroup(GroupDraft const& draft) {
    bool const named = draft.label.find_first_not_of(" \t") != std::string::npos;
    return named && draft.ticked.size() >= 2 && draft.ticked.size() <= kMaxGroupColumns;
}

/// @brief What the column chooser reads and calls. Every reader is read inside a binding, so a change of what it
///        reads redraws what shows it.
struct ColumnChooserView {
    /// @brief Whether the dialog is open.
    std::function<bool()> open;
    /// @brief What it lists.
    std::function<ChooserState()> state;
    /// @brief The group being formed.
    std::function<GroupDraft()> draft;
    /// @brief Replaces the group being formed.
    std::function<void(GroupDraft)> setDraft;
    /// @brief A check box was toggled: the column's id and whether it is to be shown.
    std::function<void(std::string const&, bool)> onShow;
    /// @brief A move button was pressed: the column's id and the places, -1 up or 1 down.
    std::function<void(std::string const&, int)> onMove;
    /// @brief "Reset columns" was pressed.
    Action onReset;
    /// @brief "Create group" was pressed: the label and the ticked columns in the layout's order. Returns whether the
    ///        group was formed, which closes and clears the form.
    std::function<bool(std::string, std::vector<std::string>)> onFormGroup;
    /// @brief "Remove group" was pressed: the group's id.
    std::function<void(std::string const&)> onRemoveGroup;
    /// @brief "Done" was pressed, or the dialog dismissed.
    Action onClose;
};

/// @brief The column chooser: a dialog, test id `table_columns`, titled "Columns".
///
/// Per column, in the layout's order: a check box `table_columns_show_<id>` (its label, checked while the column is
/// visible) and the buttons `table_columns_up_<id>` and `table_columns_down_<id>`, drawn while the table allows
/// moving. Then "Reset columns" (`table_columns_reset`) and, while moving or hiding is allowed, "Group columns…"
/// (`table_columns_group`), which shows the group form: a text input `table_group_label`, a check box
/// `table_group_column_<id>` per column (once one is ticked, only columns of its kind stay enabled) and "Create group"
/// (`table_group_create`), enabled while `canFormGroup`. Under them, one line per user-formed group: a text
/// `table_group_<id>` (`chooserGroupText`) and "Remove group" (`table_group_remove_<id>`). "Done"
/// (`table_columns_done`) and a dismissal close it.
/// @param view What it reads and calls; every member must be set.
/// @return The dialog's node.
[[nodiscard]] inline Node columnChooser(ColumnChooserView view) {
    auto const v = std::make_shared<ColumnChooserView const>(std::move(view));
    Node const rows = forEach<ChooserColumn>(
        [v] { return v->state().columns; }, [](ChooserColumn const& column) -> Key { return Key{column.id}; },
        [v](reactive::Signal<ChooserColumn> const& row) {
            std::string const id = row.peek().id;
            Checkbox show{.label = [&row] { return row.get().label; },
                          .checked = [&row] { return row.get().shown; },
                          .onToggle = [v, id](bool checked) { v->onShow(id, checked); }};
            show.common.enabled = [&row] { return row.get().toggles; };
            show.common.testId = "table_columns_show_" + id;
            show.common.layout.width = Sizing::stretch();
            Button up{.label = std::string{"Move up"}, .onClick = [v, id] { v->onMove(id, -1); }};
            up.common.enabled = [&row] { return row.get().up; };
            up.common.visible = row.peek().movable;
            up.common.testId = "table_columns_up_" + id;
            Button down{.label = std::string{"Move down"}, .onClick = [v, id] { v->onMove(id, 1); }};
            down.common.enabled = [&row] { return row.get().down; };
            down.common.visible = row.peek().movable;
            down.common.testId = "table_columns_down_" + id;
            return ui::row(Row{.children = {checkbox(std::move(show)), button(std::move(up)), button(std::move(down))},
                               .gap = 1});
        },
        Axis::Vertical, 1);

    Button reset{.label = std::string{"Reset columns"}, .onClick = [v] { v->onReset(); }};
    reset.common.testId = "table_columns_reset";
    Button group{.label = std::string{"Group columns…"}, .onClick = [v] {
                     GroupDraft draft = v->draft();
                     draft.open = true;
                     v->setDraft(std::move(draft));
                 }};
    group.common.testId = "table_columns_group";
    group.common.visible = [v] { return v->state().grouping && !v->draft().open; };

    TextInput label{.value = [v] { return v->draft().label; },
                    .onChange = [v](std::string text) {
                        GroupDraft draft = v->draft();
                        draft.label = std::move(text);
                        v->setDraft(std::move(draft));
                    },
                    .placeholder = std::string{"Group name"}};
    label.label = std::string{"Group name"};
    label.common.testId = "table_group_label";
    Node const ticks = forEach<ChooserColumn>(
        [v] { return v->state().columns; }, [](ChooserColumn const& column) -> Key { return Key{column.id}; },
        [v](reactive::Signal<ChooserColumn> const& row) {
            std::string const id = row.peek().id;
            Checkbox tick{.label = [&row] { return row.get().label; },
                          .checked = [v, id] { return std::ranges::contains(v->draft().ticked, id); },
                          .onToggle = [v, id](bool checked) {
                              GroupDraft draft = v->draft();
                              std::erase(draft.ticked, id);
                              if (checked) {
                                  draft.ticked.push_back(id);
                              }
                              v->setDraft(std::move(draft));
                          }};
            // Once a column is ticked, only columns of its kind can join it.
            tick.common.enabled = [v, &row] {
                GroupDraft const draft = v->draft();
                if (draft.ticked.empty()) {
                    return true;
                }
                std::vector<ChooserColumn> const columns = v->state().columns;
                auto const first = std::ranges::find(columns, draft.ticked.front(), &ChooserColumn::id);
                return first == columns.end() || first->kind == row.get().kind;
            };
            tick.common.testId = "table_group_column_" + id;
            return checkbox(std::move(tick));
        },
        Axis::Vertical, 0);
    Button create{.label = std::string{"Create group"}, .onClick = [v] {
                      GroupDraft const draft = v->draft();
                      if (!canFormGroup(draft)) {
                          return;
                      }
                      std::vector<std::string> columns;
                      for (ChooserColumn const& column : v->state().columns) {
                          if (std::ranges::contains(draft.ticked, column.id)) {
                              columns.push_back(column.id);
                          }
                      }
                      if (v->onFormGroup(draft.label, std::move(columns))) {
                          v->setDraft(GroupDraft{});
                      }
                  }};
    create.common.testId = "table_group_create";
    create.common.enabled = [v] { return canFormGroup(v->draft()); };
    Column form{.children = {textInput(std::move(label)), ticks, button(std::move(create))}, .gap = 1};
    form.common.visible = [v] { return v->draft().open; };

    Node const groups = forEach<ChooserGroup>(
        [v] { return v->state().groups; }, [](ChooserGroup const& line) -> Key { return Key{line.id}; },
        [v](reactive::Signal<ChooserGroup> const& line) {
            std::string const id = line.peek().id;
            Text text{.text = [&line] { return chooserGroupText(line.get()); }};
            text.common.testId = "table_group_" + id;
            text.common.layout.width = Sizing::stretch();
            Button remove{.label = std::string{"Remove group"}, .onClick = [v, id] { v->onRemoveGroup(id); }};
            remove.common.testId = "table_group_remove_" + id;
            return ui::row(Row{.children = {ui::text(std::move(text)), button(std::move(remove))}, .gap = 1});
        },
        Axis::Vertical, 1);

    Button done{.label = std::string{"Done"}, .emphasis = ButtonEmphasis::Primary, .onClick = [v] { v->onClose(); }};
    done.common.testId = "table_columns_done";
    Node const actions = ui::row(Row{.children = {button(std::move(reset)), button(std::move(group))}, .gap = 1});
    Node const content =
        ui::column(Column{.children = {rows, actions, ui::column(std::move(form)), groups, button(std::move(done))},
                          .gap = 2});
    Dialog dialog{.open = [v] { return v->open(); },
                  .title = std::string{"Columns"},
                  .child = content,
                  .onDismiss = [v] { v->onClose(); }};
    dialog.common.testId = "table_columns";
    return ui::dialog(std::move(dialog));
}

// ---------------------------------------------------------------------------------------------------------------
// The sort dialog
// ---------------------------------------------------------------------------------------------------------------

/// @brief Sets the column of the chain's key at a place.
struct SetSortColumn {
    /// @brief The key's place in the chain, from 0.
    std::size_t index = 0;
    /// @brief The column's id; a column another key holds, or one that does not sort, changes nothing.
    std::string column;
};

/// @brief Sets the direction of the chain's key at a place.
struct SetSortDirection {
    /// @brief The key's place in the chain, from 0.
    std::size_t index = 0;
    /// @brief The direction.
    table::SortDirection direction = table::SortDirection::Ascending;
};

/// @brief Moves the chain's key at a place by one place, swapping it with its neighbour.
struct MoveSortKey {
    /// @brief The key's place in the chain, from 0.
    std::size_t index = 0;
    /// @brief -1 to move it towards the first key, 1 towards the last; a move past either end changes nothing.
    int by = 0;
};

/// @brief Removes the chain's key at a place.
struct RemoveSortKey {
    /// @brief The key's place in the chain, from 0.
    std::size_t index = 0;
};

/// @brief Adds the first sortable column the chain does not hold, ascending, as its last key.
struct AddSortKey {};

/// @brief Any edit the sort dialog makes to a chain.
using SortEdit = std::variant<SetSortColumn, SetSortDirection, MoveSortKey, RemoveSortKey, AddSortKey>;

/// @brief Applies a sort dialog's edit to a chain.
///
/// Adding and removing a key, and changing its direction, are the header's sort gestures (`SortGesture::Add`,
/// `Remove` and `Extend`), so "Add key" at the bound is refused with `tableText::sortBound` and the chain is kept. An
/// edit naming a place the chain does not have changes nothing.
/// @param chain       The current chain.
/// @param sortable    The ids of the columns that sort, in the table's order.
/// @param edit        The edit.
/// @param maxSortKeys The table's bound on the chain.
/// @return The new chain, or the old one with the refusal.
[[nodiscard]] inline SortChange applySortEdit(SortChain const& chain, std::span<std::string const> sortable,
                                              SortEdit const& edit, std::size_t maxSortKeys) {
    SortChange const same{.chain = chain, .refusal = std::nullopt};
    return std::visit(
        [&](auto const& e) -> SortChange {
            using E = std::decay_t<decltype(e)>;
            if constexpr (std::is_same_v<E, AddSortKey>) {
                auto const free = std::ranges::find_if(sortable, [&](std::string const& id) {
                    return std::ranges::find(chain, id, &table::SortKey::column) == chain.end();
                });
                if (chain.size() >= maxSortKeys) {
                    return {.chain = chain, .refusal = tableText::sortBound(maxSortKeys)};
                }
                return free == sortable.end() ? same : applySortGesture(chain, *free, SortGesture::Add, maxSortKeys);
            } else {
                if (e.index >= chain.size()) {
                    return same;
                }
                table::SortKey const& key = chain[e.index];
                if constexpr (std::is_same_v<E, RemoveSortKey>) {
                    return applySortGesture(chain, key.column, SortGesture::Remove, maxSortKeys);
                } else if constexpr (std::is_same_v<E, SetSortDirection>) {
                    return key.dir == e.direction ? same
                                                  : applySortGesture(chain, key.column, SortGesture::Extend, maxSortKeys);
                } else if constexpr (std::is_same_v<E, MoveSortKey>) {
                    auto const to = static_cast<std::ptrdiff_t>(e.index) + e.by;
                    if (e.by == 0 || to < 0 || static_cast<std::size_t>(to) >= chain.size()) {
                        return same;
                    }
                    SortChain out = chain;
                    std::swap(out[e.index], out[static_cast<std::size_t>(to)]);
                    return {.chain = std::move(out), .refusal = std::nullopt};
                } else {
                    bool const sorts = std::ranges::contains(sortable, e.column);
                    auto const held = std::ranges::find(chain, e.column, &table::SortKey::column);
                    if (!sorts || (held != chain.end() && held != chain.begin() + static_cast<std::ptrdiff_t>(e.index))) {
                        return same;
                    }
                    SortChain out = chain;
                    out[e.index].column = e.column;
                    return {.chain = std::move(out), .refusal = std::nullopt};
                }
            }
        },
        edit);
}

/// @brief A sortable column as the sort dialog offers it.
struct SortColumnOption {
    /// @brief The column's id.
    std::string id;
    /// @brief Its label.
    std::string label;

    /// @brief Memberwise equality.
    /// @return Whether every member matches.
    bool operator==(SortColumnOption const&) const = default;
};

/// @brief One key as the sort dialog lists it.
struct SortDialogKey {
    /// @brief Its place in the chain, from 0.
    std::size_t index = 0;
    /// @brief Its column's id.
    std::string column;
    /// @brief Its direction.
    table::SortDirection direction = table::SortDirection::Ascending;
    /// @brief The columns its column select offers: the sortable columns no other key holds.
    std::vector<SelectOption> columns;
    /// @brief Whether it can move towards the first key.
    bool up = false;
    /// @brief Whether it can move towards the last key.
    bool down = false;

    /// @brief Memberwise equality.
    /// @return Whether every member matches.
    bool operator==(SortDialogKey const&) const = default;
};

/// @brief What the sort dialog lists for a chain.
/// @param chain    The chain.
/// @param sortable The columns that sort, in the table's order.
/// @return Its keys, in order.
[[nodiscard]] inline std::vector<SortDialogKey> sortDialogKeys(SortChain const& chain,
                                                               std::span<SortColumnOption const> sortable) {
    std::vector<SortDialogKey> out;
    out.reserve(chain.size());
    for (std::size_t at = 0; at < chain.size(); ++at) {
        SortDialogKey key{.index = at,
                          .column = chain[at].column,
                          .direction = chain[at].dir,
                          .columns = {},
                          .up = at > 0,
                          .down = at + 1 < chain.size()};
        for (SortColumnOption const& option : sortable) {
            auto const held = std::ranges::find(chain, option.id, &table::SortKey::column);
            if (held == chain.end() || held == chain.begin() + static_cast<std::ptrdiff_t>(at)) {
                key.columns.push_back(SelectOption{.key = Key{option.id}, .label = option.label});
            }
        }
        out.push_back(std::move(key));
    }
    return out;
}

/// @brief What the sort dialog reads and calls. Every reader is read inside a binding.
struct SortDialogView {
    /// @brief Whether the dialog is open.
    std::function<bool()> open;
    /// @brief The chain the table sorts by now.
    std::function<SortChain()> chain;
    /// @brief The columns that sort, in the table's order.
    std::function<std::vector<SortColumnOption>()> sortable;
    /// @brief An edit was made.
    std::function<void(SortEdit const&)> onEdit;
    /// @brief "Done" was pressed, or the dialog dismissed.
    Action onClose;
};

/// @brief The sort dialog: a dialog, test id `table_sort`, titled "Sort".
///
/// Per key `i` from 0: a select `table_sort_column_{i}` (the sortable columns no other key holds), a select
/// `table_sort_direction_{i}` (`ascending`, `descending`), and the buttons `table_sort_up_{i}`, `table_sort_down_{i}`
/// and `table_sort_remove_{i}`. Then "Add key" (`table_sort_add`), enabled while a sortable column is not in the
/// chain, and "Done" (`table_sort_done`); a dismissal closes it too.
/// @param view What it reads and calls; every member must be set.
/// @return The dialog's node.
[[nodiscard]] inline Node sortDialog(SortDialogView view) {
    auto const v = std::make_shared<SortDialogView const>(std::move(view));
    Node const keys = forEach<SortDialogKey>(
        [v] {
            std::vector<SortColumnOption> const sortable = v->sortable();
            return sortDialogKeys(v->chain(), sortable);
        },
        [](SortDialogKey const& key) -> Key { return Key{static_cast<std::int64_t>(key.index)}; },
        [v](reactive::Signal<SortDialogKey> const& row) {
            std::size_t const index = row.peek().index;
            std::string const at = std::to_string(index);
            Select column{.options = [&row] { return row.get().columns; },
                          .selected = [&row] { return std::optional<Key>{Key{row.get().column}}; },
                          .onSelect = [v, index](Key const& key) {
                              if (std::string const* const id = std::get_if<std::string>(&key)) {
                                  v->onEdit(SetSortColumn{.index = index, .column = *id});
                              }
                          }};
            column.label = std::string{"Column"};
            column.common.testId = "table_sort_column_" + at;
            column.common.layout.width = Sizing::stretch();
            Select direction{
                .options = std::vector<SelectOption>{{.key = Key{std::string{"ascending"}}, .label = "ascending"},
                                                     {.key = Key{std::string{"descending"}}, .label = "descending"}},
                .selected = [&row] {
                    return std::optional<Key>{Key{std::string{
                        row.get().direction == table::SortDirection::Descending ? "descending" : "ascending"}}};
                },
                .onSelect = [v, index](Key const& key) {
                    std::string const* const name = std::get_if<std::string>(&key);
                    if (name != nullptr && (*name == "ascending" || *name == "descending")) {
                        v->onEdit(SetSortDirection{.index = index,
                                                   .direction = *name == "descending" ? table::SortDirection::Descending
                                                                                      : table::SortDirection::Ascending});
                    }
                }};
            direction.label = std::string{"Direction"};
            direction.common.testId = "table_sort_direction_" + at;
            Button up{.label = std::string{"Move up"}, .onClick = [v, index] { v->onEdit(MoveSortKey{.index = index, .by = -1}); }};
            up.common.enabled = [&row] { return row.get().up; };
            up.common.testId = "table_sort_up_" + at;
            Button down{.label = std::string{"Move down"},
                        .onClick = [v, index] { v->onEdit(MoveSortKey{.index = index, .by = 1}); }};
            down.common.enabled = [&row] { return row.get().down; };
            down.common.testId = "table_sort_down_" + at;
            Button remove{.label = std::string{"Remove"}, .onClick = [v, index] { v->onEdit(RemoveSortKey{.index = index}); }};
            remove.common.testId = "table_sort_remove_" + at;
            return ui::row(Row{.children = {select(std::move(column)), select(std::move(direction)), button(std::move(up)),
                                            button(std::move(down)), button(std::move(remove))},
                               .gap = 1});
        },
        Axis::Vertical, 1);
    Button add{.label = std::string{"Add key"}, .onClick = [v] { v->onEdit(AddSortKey{}); }};
    add.common.testId = "table_sort_add";
    add.common.enabled = [v] {
        SortChain const chain = v->chain();
        return std::ranges::any_of(v->sortable(), [&chain](SortColumnOption const& option) {
            return std::ranges::find(chain, option.id, &table::SortKey::column) == chain.end();
        });
    };
    Button done{.label = std::string{"Done"}, .emphasis = ButtonEmphasis::Primary, .onClick = [v] { v->onClose(); }};
    done.common.testId = "table_sort_done";
    Node const content = ui::column(Column{.children = {keys, button(std::move(add)), button(std::move(done))}, .gap = 2});
    Dialog dialog{.open = [v] { return v->open(); },
                  .title = std::string{"Sort"},
                  .child = content,
                  .onDismiss = [v] { v->onClose(); }};
    dialog.common.testId = "table_sort";
    return ui::dialog(std::move(dialog));
}

// ---------------------------------------------------------------------------------------------------------------
// The view name dialog
// ---------------------------------------------------------------------------------------------------------------

/// @brief What the view name dialog reads and calls. Every reader is read inside a binding.
struct ViewNameDialogView {
    /// @brief Whether the dialog is open.
    std::function<bool()> open;
    /// @brief Its title: "Save view", "Rename view" or "Publish view".
    std::function<std::string()> title;
    /// @brief The name as typed so far.
    std::function<std::string()> name;
    /// @brief The name was edited.
    std::function<void(std::string)> onName;
    /// @brief "Save" was pressed, or Enter in the name, with a name that is not blank.
    Action onSave;
    /// @brief "Cancel" was pressed, or the dialog dismissed.
    Action onClose;
};

/// @brief Whether a view's name is blank: empty, or spaces alone.
/// @param name The name.
/// @return Whether it is.
[[nodiscard]] inline bool blankViewName(std::string_view name) {
    return name.find_first_not_of(" \t") == std::string_view::npos;
}

/// @brief The view name dialog: a dialog, test id `table_view_dialog`, that names a view to save, rename or publish.
///
/// A text input `table_view_name` (the name), a button `table_view_save` ("Save"), enabled while the name is not
/// blank, and a button `table_view_cancel` ("Cancel"); a dismissal closes it too.
/// @param view What it reads and calls; every member must be set.
/// @return The dialog's node.
[[nodiscard]] inline Node viewNameDialog(ViewNameDialogView view) {
    auto const v = std::make_shared<ViewNameDialogView const>(std::move(view));
    TextInput name{.value = [v] { return v->name(); },
                   .onChange = [v](std::string text) { v->onName(std::move(text)); },
                   .onSubmit =
                       [v](std::string text) {
                           if (!blankViewName(text)) {
                               v->onName(std::move(text));
                               v->onSave();
                           }
                       },
                   .placeholder = std::string{"View name"}};
    name.label = std::string{"View name"};
    name.common.testId = "table_view_name";
    Button save{.label = std::string{"Save"}, .emphasis = ButtonEmphasis::Primary, .onClick = [v] { v->onSave(); }};
    save.common.testId = "table_view_save";
    save.common.enabled = [v] { return !blankViewName(v->name()); };
    Button cancel{.label = std::string{"Cancel"}, .onClick = [v] { v->onClose(); }};
    cancel.common.testId = "table_view_cancel";
    Node const content = ui::column(Column{
        .children = {textInput(std::move(name)), ui::row(Row{.children = {button(std::move(save)), button(std::move(cancel))},
                                                             .gap = 1})},
        .gap = 2});
    Dialog dialog{.open = [v] { return v->open(); },
                  .title = [v] { return v->title(); },
                  .child = content,
                  .onDismiss = [v] { v->onClose(); }};
    dialog.common.testId = "table_view_dialog";
    return ui::dialog(std::move(dialog));
}

}  // namespace morph::ui

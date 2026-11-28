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
#include <morph/export.hpp>
#include <morph/ui/table_config.hpp>
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
[[nodiscard]] MORPH_API ChooserState chooserState(table::ColumnLayout const& layout,
                                                  std::span<LayoutColumn const> columns, Configure allowed);

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
[[nodiscard]] MORPH_API bool canFormGroup(GroupDraft const& draft);

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
[[nodiscard]] MORPH_API Node columnChooser(ColumnChooserView view);

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
// NOLINTNEXTLINE(readability-function-cognitive-complexity): one visit over the edit kinds.
[[nodiscard]] MORPH_API SortChange applySortEdit(SortChain const& chain, std::span<std::string const> sortable,
                                                 SortEdit const& edit, std::size_t maxSortKeys);

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
[[nodiscard]] MORPH_API std::vector<SortDialogKey> sortDialogKeys(SortChain const& chain,
                                                                  std::span<SortColumnOption const> sortable);

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
[[nodiscard]] MORPH_API Node sortDialog(SortDialogView view);

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
[[nodiscard]] MORPH_API Node viewNameDialog(ViewNameDialogView view);

}  // namespace morph::ui

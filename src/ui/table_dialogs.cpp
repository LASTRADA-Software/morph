// SPDX-License-Identifier: Apache-2.0

// The non-template definitions of <morph/ui/table_dialogs.hpp>.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <morph/table/engine.hpp>
#include <morph/table/sort.hpp>
#include <morph/ui/table_config.hpp>
#include <morph/ui/table_dialogs.hpp>
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

ChooserState chooserState(table::ColumnLayout const& layout, std::span<LayoutColumn const> columns,
                          Configure allowed) {
    ChooserState out;
    out.reorder = allows(allowed, Configure::Reorder);
    out.hide = allows(allowed, Configure::Hide);
    out.grouping = out.reorder || out.hide;
    std::vector<std::string> const visible = layout.visible();
    std::size_t const pinned = layout.pinned();
    for (std::string const& columnId : layout.order()) {
        auto const found = std::ranges::find(columns, columnId, &LayoutColumn::id);
        if (found == columns.end()) {
            continue;
        }
        std::size_t const index = detail::visibleIndex(visible, columnId);
        bool const shown = index < visible.size();
        bool const free = !found->locked;
        out.columns.push_back(ChooserColumn{
            .id = columnId,
            .label = found->label,
            .kind = found->kind,
            .shown = shown,
            .toggles = out.hide && free && (!shown || visible.size() != 1),
            // A move stays on its side of the pin boundary, as the header's "Move left" and "Move right" do.
            .up = out.reorder && free && shown && index > 0 && index != pinned,
            .down = out.reorder && free && shown && index + 1 < visible.size() && index + 1 != pinned,
            .movable = out.reorder,
        });
    }
    for (table::ColumnGroupInfo const& group : layout.groups()) {
        out.groups.push_back(ChooserGroup{.id = group.id, .label = group.label, .columns = group.columns.size()});
    }
    return out;
}

bool canFormGroup(GroupDraft const& draft) {
    bool const named = draft.label.find_first_not_of(" \t") != std::string::npos;
    return named && draft.ticked.size() >= 2 && draft.ticked.size() <= kMaxGroupColumns;
}

Node columnChooser(ColumnChooserView view) {
    auto const viewPtr = std::make_shared<ColumnChooserView const>(std::move(view));
    Node const rows = forEach<ChooserColumn>(
        [viewPtr] { return viewPtr->state().columns; },
        [](ChooserColumn const& column) -> Key { return Key{column.id}; },
        [viewPtr](reactive::Signal<ChooserColumn> const& row) {
            std::string const columnId = row.peek().id;
            Checkbox show{.label = [&row] { return row.get().label; },
                          .checked = [&row] { return row.get().shown; },
                          .onToggle = [viewPtr, columnId](bool checked) { viewPtr->onShow(columnId, checked); }};
            show.common.enabled = [&row] { return row.get().toggles; };
            show.common.testId = "table_columns_show_" + columnId;
            show.common.layout.width = Sizing::stretch();
            Button moveUp{.label = std::string{"Move up"},
                          .onClick = [viewPtr, columnId] { viewPtr->onMove(columnId, -1); }};
            moveUp.common.enabled = [&row] { return row.get().up; };
            moveUp.common.visible = row.peek().movable;
            moveUp.common.testId = "table_columns_up_" + columnId;
            Button down{.label = std::string{"Move down"},
                        .onClick = [viewPtr, columnId] { viewPtr->onMove(columnId, 1); }};
            down.common.enabled = [&row] { return row.get().down; };
            down.common.visible = row.peek().movable;
            down.common.testId = "table_columns_down_" + columnId;
            return ui::row(
                Row{.children = {checkbox(std::move(show)), button(std::move(moveUp)), button(std::move(down))},
                    .gap = 1});
        },
        Axis::Vertical, 1);

    Button reset{.label = std::string{"Reset columns"}, .onClick = [viewPtr] { viewPtr->onReset(); }};
    reset.common.testId = "table_columns_reset";
    Button group{.label = std::string{"Group columns…"}, .onClick = [viewPtr] {
                     GroupDraft draft = viewPtr->draft();
                     draft.open = true;
                     viewPtr->setDraft(std::move(draft));
                 }};
    group.common.testId = "table_columns_group";
    group.common.visible = [viewPtr] { return viewPtr->state().grouping && !viewPtr->draft().open; };

    TextInput label{.value = [viewPtr] { return viewPtr->draft().label; },
                    .onChange =
                        [viewPtr](std::string text) {
                            GroupDraft draft = viewPtr->draft();
                            draft.label = std::move(text);
                            viewPtr->setDraft(std::move(draft));
                        },
                    .placeholder = std::string{"Group name"}};
    label.label = std::string{"Group name"};
    label.common.testId = "table_group_label";
    Node const ticks = forEach<ChooserColumn>(
        [viewPtr] { return viewPtr->state().columns; },
        [](ChooserColumn const& column) -> Key { return Key{column.id}; },
        [viewPtr](reactive::Signal<ChooserColumn> const& row) {
            std::string const columnId = row.peek().id;
            Checkbox tick{
                .label = [&row] { return row.get().label; },
                .checked = [viewPtr, columnId] { return std::ranges::contains(viewPtr->draft().ticked, columnId); },
                .onToggle =
                    [viewPtr, columnId](bool checked) {
                        GroupDraft draft = viewPtr->draft();
                        std::erase(draft.ticked, columnId);
                        if (checked) {
                            draft.ticked.push_back(columnId);
                        }
                        viewPtr->setDraft(std::move(draft));
                    }};
            // Once a column is ticked, only columns of its kind can join it.
            tick.common.enabled = [viewPtr, &row] {
                GroupDraft const draft = viewPtr->draft();
                if (draft.ticked.empty()) {
                    return true;
                }
                std::vector<ChooserColumn> const columns = viewPtr->state().columns;
                auto const first = std::ranges::find(columns, draft.ticked.front(), &ChooserColumn::id);
                return first == columns.end() || first->kind == row.get().kind;
            };
            tick.common.testId = "table_group_column_" + columnId;
            return checkbox(std::move(tick));
        },
        Axis::Vertical, 0);
    Button create{.label = std::string{"Create group"}, .onClick = [viewPtr] {
                      GroupDraft const draft = viewPtr->draft();
                      if (!canFormGroup(draft)) {
                          return;
                      }
                      std::vector<std::string> columns;
                      for (ChooserColumn const& column : viewPtr->state().columns) {
                          if (std::ranges::contains(draft.ticked, column.id)) {
                              columns.push_back(column.id);
                          }
                      }
                      if (viewPtr->onFormGroup(draft.label, std::move(columns))) {
                          viewPtr->setDraft(GroupDraft{});
                      }
                  }};
    create.common.testId = "table_group_create";
    create.common.enabled = [viewPtr] { return canFormGroup(viewPtr->draft()); };
    Column form{.children = {textInput(std::move(label)), ticks, button(std::move(create))}, .gap = 1};
    form.common.visible = [viewPtr] { return viewPtr->draft().open; };

    Node const groups = forEach<ChooserGroup>(
        [viewPtr] { return viewPtr->state().groups; }, [](ChooserGroup const& line) -> Key { return Key{line.id}; },
        [viewPtr](reactive::Signal<ChooserGroup> const& line) {
            std::string const columnId = line.peek().id;
            Text text{.text = [&line] { return chooserGroupText(line.get()); }};
            text.common.testId = "table_group_" + columnId;
            text.common.layout.width = Sizing::stretch();
            Button remove{.label = std::string{"Remove group"},
                          .onClick = [viewPtr, columnId] { viewPtr->onRemoveGroup(columnId); }};
            remove.common.testId = "table_group_remove_" + columnId;
            return ui::row(Row{.children = {ui::text(std::move(text)), button(std::move(remove))}, .gap = 1});
        },
        Axis::Vertical, 1);

    Button done{.label = std::string{"Done"}, .emphasis = ButtonEmphasis::Primary, .onClick = [viewPtr] {
                    viewPtr->onClose();
                }};
    done.common.testId = "table_columns_done";
    Node const actions = ui::row(Row{.children = {button(std::move(reset)), button(std::move(group))}, .gap = 1});
    Node const content = ui::column(
        Column{.children = {rows, actions, ui::column(std::move(form)), groups, button(std::move(done))}, .gap = 2});
    Dialog dialog{.open = [viewPtr] { return viewPtr->open(); },
                  .title = std::string{"Columns"},
                  .child = content,
                  .onDismiss = [viewPtr] { viewPtr->onClose(); }};
    dialog.common.testId = "table_columns";
    return ui::dialog(std::move(dialog));
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity): one visit over the edit kinds.
SortChange applySortEdit(SortChain const& chain, std::span<std::string const> sortable, SortEdit const& edit,
                         std::size_t maxSortKeys) {
    auto const unchanged = [&chain] { return SortChange{.chain = chain, .refusal = std::nullopt}; };
    return std::visit(
        [&](auto const& alternative) -> SortChange {
            using E = std::decay_t<decltype(alternative)>;
            if constexpr (std::is_same_v<E, AddSortKey>) {
                auto const free = std::ranges::find_if(sortable, [&](std::string const& columnId) {
                    return std::ranges::find(chain, columnId, &table::SortKey::column) == chain.end();
                });
                if (chain.size() >= maxSortKeys) {
                    return {.chain = chain, .refusal = tableText::sortBound(maxSortKeys)};
                }
                return free == sortable.end() ? unchanged()
                                              : applySortGesture(chain, *free, SortGesture::Add, maxSortKeys);
            } else {
                if (alternative.index >= chain.size()) {
                    return unchanged();
                }
                // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access): index < chain.size() by the check above
                table::SortKey const& key = chain[alternative.index];
                if constexpr (std::is_same_v<E, RemoveSortKey>) {
                    return applySortGesture(chain, key.column, SortGesture::Remove, maxSortKeys);
                } else if constexpr (std::is_same_v<E, SetSortDirection>) {
                    return key.dir == alternative.direction
                               ? unchanged()
                               : applySortGesture(chain, key.column, SortGesture::Extend, maxSortKeys);
                } else if constexpr (std::is_same_v<E, MoveSortKey>) {
                    auto const target = static_cast<std::ptrdiff_t>(alternative.index) + alternative.by;
                    if (alternative.by == 0 || target < 0 || static_cast<std::size_t>(target) >= chain.size()) {
                        return unchanged();
                    }
                    SortChain out = chain;
                    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access): target < chain.size() by the test above
                    std::swap(out[alternative.index], out[static_cast<std::size_t>(target)]);
                    return {.chain = std::move(out), .refusal = std::nullopt};
                } else {
                    bool const sorts = std::ranges::contains(sortable, alternative.column);
                    auto const held = std::ranges::find(chain, alternative.column, &table::SortKey::column);
                    if (!sorts || (held != chain.end() &&
                                   held != chain.begin() + static_cast<std::ptrdiff_t>(alternative.index))) {
                        return unchanged();
                    }
                    SortChain out = chain;
                    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access): index < chain.size() by the check above
                    out[alternative.index].column = alternative.column;
                    return {.chain = std::move(out), .refusal = std::nullopt};
                }
            }
        },
        edit);
}

std::vector<SortDialogKey> sortDialogKeys(SortChain const& chain, std::span<SortColumnOption const> sortable) {
    std::vector<SortDialogKey> out;
    out.reserve(chain.size());
    for (std::size_t at = 0; at < chain.size(); ++at) {
        SortDialogKey key{
            .index = at,
            // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access): at < chain.size() by the loop
            .column = chain[at].column,
            // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access): at < chain.size() by the loop
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

Node sortDialog(SortDialogView view) {
    auto const viewPtr = std::make_shared<SortDialogView const>(std::move(view));
    Node const keys = forEach<SortDialogKey>(
        [viewPtr] {
            std::vector<SortColumnOption> const sortable = viewPtr->sortable();
            return sortDialogKeys(viewPtr->chain(), sortable);
        },
        [](SortDialogKey const& key) -> Key { return Key{static_cast<std::int64_t>(key.index)}; },
        [viewPtr](reactive::Signal<SortDialogKey> const& row) {
            std::size_t const index = row.peek().index;
            std::string const position = std::to_string(index);
            Select column{.options = [&row] { return row.get().columns; },
                          .selected = [&row] { return std::optional<Key>{Key{row.get().column}}; },
                          .onSelect =
                              [viewPtr, index](Key const& key) {
                                  if (std::string const* const columnId = std::get_if<std::string>(&key)) {
                                      viewPtr->onEdit(SetSortColumn{.index = index, .column = *columnId});
                                  }
                              }};
            column.label = std::string{"Column"};
            column.common.testId = "table_sort_column_" + position;
            column.common.layout.width = Sizing::stretch();
            Select direction{
                .options = std::vector<SelectOption>{{.key = Key{std::string{"ascending"}}, .label = "ascending"},
                                                     {.key = Key{std::string{"descending"}}, .label = "descending"}},
                .selected =
                    [&row] {
                        return std::optional<Key>{Key{std::string{
                            row.get().direction == table::SortDirection::Descending ? "descending" : "ascending"}}};
                    },
                .onSelect =
                    [viewPtr, index](Key const& key) {
                        std::string const* const name = std::get_if<std::string>(&key);
                        if (name != nullptr && (*name == "ascending" || *name == "descending")) {
                            viewPtr->onEdit(SetSortDirection{.index = index,
                                                             .direction = *name == "descending"
                                                                              ? table::SortDirection::Descending
                                                                              : table::SortDirection::Ascending});
                        }
                    }};
            direction.label = std::string{"Direction"};
            direction.common.testId = "table_sort_direction_" + position;
            Button moveUp{.label = std::string{"Move up"},
                          .onClick = [viewPtr, index] { viewPtr->onEdit(MoveSortKey{.index = index, .by = -1}); }};
            moveUp.common.enabled = [&row] { return row.get().up; };
            moveUp.common.testId = "table_sort_up_" + position;
            Button down{.label = std::string{"Move down"},
                        .onClick = [viewPtr, index] { viewPtr->onEdit(MoveSortKey{.index = index, .by = 1}); }};
            down.common.enabled = [&row] { return row.get().down; };
            down.common.testId = "table_sort_down_" + position;
            Button remove{.label = std::string{"Remove"},
                          .onClick = [viewPtr, index] { viewPtr->onEdit(RemoveSortKey{.index = index}); }};
            remove.common.testId = "table_sort_remove_" + position;
            return ui::row(
                Row{.children = {select(std::move(column)), select(std::move(direction)), button(std::move(moveUp)),
                                 button(std::move(down)), button(std::move(remove))},
                    .gap = 1});
        },
        Axis::Vertical, 1);
    Button add{.label = std::string{"Add key"}, .onClick = [viewPtr] { viewPtr->onEdit(AddSortKey{}); }};
    add.common.testId = "table_sort_add";
    add.common.enabled = [viewPtr] {
        SortChain const chain = viewPtr->chain();
        return std::ranges::any_of(viewPtr->sortable(), [&chain](SortColumnOption const& option) {
            return std::ranges::find(chain, option.id, &table::SortKey::column) == chain.end();
        });
    };
    Button done{.label = std::string{"Done"}, .emphasis = ButtonEmphasis::Primary, .onClick = [viewPtr] {
                    viewPtr->onClose();
                }};
    done.common.testId = "table_sort_done";
    Node const content =
        ui::column(Column{.children = {keys, button(std::move(add)), button(std::move(done))}, .gap = 2});
    Dialog dialog{.open = [viewPtr] { return viewPtr->open(); },
                  .title = std::string{"Sort"},
                  .child = content,
                  .onDismiss = [viewPtr] { viewPtr->onClose(); }};
    dialog.common.testId = "table_sort";
    return ui::dialog(std::move(dialog));
}

Node viewNameDialog(ViewNameDialogView view) {
    auto const viewPtr = std::make_shared<ViewNameDialogView const>(std::move(view));
    TextInput name{.value = [viewPtr] { return viewPtr->name(); },
                   .onChange = [viewPtr](std::string text) { viewPtr->onName(std::move(text)); },
                   .onSubmit =
                       [viewPtr](std::string text) {
                           if (!blankViewName(text)) {
                               viewPtr->onName(std::move(text));
                               viewPtr->onSave();
                           }
                       },
                   .placeholder = std::string{"View name"}};
    name.label = std::string{"View name"};
    name.common.testId = "table_view_name";
    Button save{.label = std::string{"Save"}, .emphasis = ButtonEmphasis::Primary, .onClick = [viewPtr] {
                    viewPtr->onSave();
                }};
    save.common.testId = "table_view_save";
    save.common.enabled = [viewPtr] { return !blankViewName(viewPtr->name()); };
    Button cancel{.label = std::string{"Cancel"}, .onClick = [viewPtr] { viewPtr->onClose(); }};
    cancel.common.testId = "table_view_cancel";
    Node const content = ui::column(
        Column{.children = {textInput(std::move(name)),
                            ui::row(Row{.children = {button(std::move(save)), button(std::move(cancel))}, .gap = 1})},
               .gap = 2});
    Dialog dialog{.open = [viewPtr] { return viewPtr->open(); },
                  .title = [viewPtr] { return viewPtr->title(); },
                  .child = content,
                  .onDismiss = [viewPtr] { viewPtr->onClose(); }};
    dialog.common.testId = "table_view_dialog";
    return ui::dialog(std::move(dialog));
}

}  // namespace morph::ui

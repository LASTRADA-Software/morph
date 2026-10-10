// SPDX-License-Identifier: Apache-2.0

#pragma once
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <memory>
#include <optional>
#include <ranges>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

#include "../attributes.hpp"
#include "../reactive/runtime.hpp"
#include "../reactive/scope.hpp"
#include "../reactive/signal.hpp"
#include "backend.hpp"
#include "view.hpp"

/// @file
/// @brief `morph::ui::Mounted`: builds a view tree's widgets through a backend once, and keeps them current through
///        reactive bindings.
///
/// Specified in `docs/spec/ui/view_tree.md`, "Mount".

namespace morph::ui {

namespace detail {

/// @brief The site names a misuse in this layer is reported under, through the runtime's owner probe.
namespace site {
/// @brief An exception escaped a widget callback; the mount caught it, so it never reaches the backend.
inline constexpr char const* kCallbackThrew = "morph::ui: an exception escaped a widget callback";
/// @brief A ForEach or Table snapshot repeated a key; the later row is refused and the first one kept.
inline constexpr char const* kDuplicateKey = "morph::ui: duplicate ForEach key";
}  // namespace site

/// @brief Called for each newly mounted row with its widget and key; a Table tells its widget the row's key.
using RowHook = std::function<void(Widget&, Key const&)>;

/// @brief One mounted row of a ForEach or Table.
struct KeyedRow {
    /// @brief The row's key.
    Key key;
    /// @brief Owns the row's slot (adopted first) and its widgets and bindings.
    std::unique_ptr<reactive::Scope> scope;
    /// @brief The row's slot, owned by `scope`.
    RowSlot* slot = nullptr;
    /// @brief The row's widget, owned by `scope`; null when the row view was a null node.
    Widget* widget = nullptr;
};

/// @brief The mounted rows of one ForEach or Table, in their container's order.
struct KeyedRows {
    KeyedRows() = default;
    /// @brief Unmounts the rows last first, as a container's children are; a vector's own destruction order differs
    ///        between standard libraries.
    ~KeyedRows() {
        while (!list.empty()) {
            list.pop_back();
        }
    }
    KeyedRows(KeyedRows const&) = delete;
    KeyedRows& operator=(KeyedRows const&) = delete;
    KeyedRows(KeyedRows&&) = delete;
    KeyedRows& operator=(KeyedRows&&) = delete;

    /// @brief The rows; those with a widget are in the container's order.
    std::vector<KeyedRow> list;
};

/// @brief Which entries of a permutation lie on one longest increasing run.
///
/// @p sequence holds, in current order, each widget's target position; the widgets on the run are already in
/// order relative to each other and stay where they are, and every other one is moved once.
/// @param sequence A permutation of `0 .. n-1`.
/// @return Indexed by target position: whether that widget stays.
[[nodiscard]] inline std::vector<bool> stableTargets(std::vector<std::size_t> const& sequence) {
    constexpr auto kNone = static_cast<std::size_t>(-1);
    std::vector<std::size_t> tails;  // tails.at(n - 1): where the smallest tail of a run of length n sits
    std::vector<std::size_t> previous(sequence.size(), kNone);
    for (std::size_t i = 0; i < sequence.size(); ++i) {
        auto const place = std::ranges::lower_bound(tails, sequence.at(i), {},
                                                    [&sequence](std::size_t entry) { return sequence.at(entry); });
        auto const length = static_cast<std::size_t>(place - tails.begin());
        if (length > 0) {
            previous.at(i) = tails.at(length - 1);
        }
        if (length == tails.size()) {
            tails.push_back(i);
        } else {
            tails.at(length) = i;
        }
    }
    std::vector<bool> stable(sequence.size(), false);
    for (std::size_t cursor = tails.empty() ? kNone : tails.back(); cursor != kNone; cursor = previous.at(cursor)) {
        stable.at(sequence.at(cursor)) = true;
    }
    return stable;
}

/// @brief Brings a container's children from their current order into the target order with `moveChild`,
///        moving only the children outside a longest run already in order.
/// @param container The container.
/// @param current Its children, in their current order. Kept equal to the container's order after every move, so
///        it is still that order when a move throws.
/// @param target The same children, in the order wanted.
inline void reorderChildren(ContainerWidget& container, std::vector<Widget*>& current,
                            std::vector<Widget*> const& target) {
    std::unordered_map<Widget const*, std::size_t> targetOf;
    for (std::size_t i = 0; i < target.size(); ++i) {
        targetOf.emplace(target.at(i), i);
    }
    std::vector<std::size_t> sequence;
    sequence.reserve(current.size());
    for (Widget const* const child : current) {
        sequence.push_back(targetOf.at(child));
    }
    std::vector<bool> const stable = stableTargets(sequence);
    auto const indexOf = [&current](Widget const* child) {
        return static_cast<std::size_t>(std::ranges::find(current, child) - current.begin());
    };
    // From the back: each moved child goes directly before its successor in the target order, which is already
    // in place, so one move per unstable child suffices.
    for (std::size_t position = target.size(); position-- > 0;) {
        if (stable.at(position)) {
            continue;
        }
        Widget* const moving = target.at(position);
        std::size_t const from = indexOf(moving);
        std::size_t destination = current.size() - 1;
        if (position + 1 < target.size()) {
            std::size_t const before = indexOf(target.at(position + 1));
            destination = from < before ? before - 1 : before;
        }
        if (destination == from) {
            continue;
        }
        container.moveChild(*moving, destination);
        current.erase(current.begin() + static_cast<std::ptrdiff_t>(from));
        current.insert(current.begin() + static_cast<std::ptrdiff_t>(destination), moving);
    }
}

/// @brief Where one Tabs keeps its pages: a slot per tab, made the first time the tab is selected and kept, hidden,
///        after.
struct Pages {
    /// @brief Each page's slot; null until the tab is first selected.
    std::vector<SlotWidget*> slots;
    /// @brief The page shown now, if any; may be past the last page, and then none is shown.
    std::optional<std::size_t> shown;
    /// @brief The Tabs' `onSelect`, run as a widget callback; empty when the Tabs has none.
    std::function<void(std::size_t)> onSelect;
};

/// @brief Builds widgets for nodes and wires their props and callbacks.
///
/// One per `Mounted`. It must outlive every scope it mounted into: a Switch, Tabs, Dialog, ForEach or Table binding
/// calls back into it to mount content later.
class Mounter {
public:
    /// @param runtime The runtime bindings are made in. Borrowed.
    /// @param backend Makes the widgets. Borrowed.
    Mounter(reactive::Runtime& runtime MORPH_LIFETIMEBOUND, IViewBackend& backend MORPH_LIFETIMEBOUND) noexcept
        : _rt{&runtime}, _backend{&backend} {}

    /// @brief Mounts @p node under @p parent; @p scope owns its widgets and bindings.
    ///
    /// A node's widget is adopted first, then its bindings, then its children, so destroying the scope destroys
    /// bindings before their widget and children before their parent.
    /// @param scope Owns everything mounted.
    /// @param node The node; null mounts nothing.
    /// @param parent The container to append to, or null for a root.
    /// @return The node's widget, or null for a null node.
    Widget* mount(reactive::Scope& scope, Node const& node, ContainerWidget* parent) {
        if (!node) {
            return nullptr;
        }
        return std::visit([&](auto const& spec) -> Widget* { return &this->mountKind(scope, spec, parent); },
                          node->kind);
    }

private:
    // A constant is applied once; a binding becomes an equality-gated Computed plus an Effect, made in that order
    // after the widget, so the Effect dies first. Returns the Computed, or null for a constant.
    template <typename T, typename Apply>
    reactive::Computed<T>* bind(reactive::Scope& scope, Prop<T> const& prop, Apply apply) {
        if (!prop.isBound()) {
            apply(prop.constant());
            return nullptr;
        }
        auto& value = scope.make<reactive::Computed<T>>(*_rt, prop.binding());
        scope.effect([&value, apply = std::move(apply)] { apply(value.get()); });
        return &value;
    }

    // Runs a widget callback, and returns whether it completed.
    // - Inside widgetEvent: one batch, and a flush that would start meanwhile is re-posted, so a remount never
    //   destroys a widget whose native handler is on the stack.
    // - Untracked: a backend may call a handler from inside a setter, and so from inside the binding Effect that
    //   called it; that Effect must not subscribe to what the handler reads.
    // - An exception is reported and stops here: the backend that called the handler need not be exception-safe.
    //   Writes made before it still flush when the batch closes.
    template <typename F>
    static bool runCallback(reactive::Runtime& runtime, F const& body) {
        return runtime.widgetEvent([&] {
            return runtime.untracked([&] {
                try {
                    body();
                    return true;
                } catch (...) {
                    runtime.core()->report(site::kCallbackThrew);
                    return false;
                }
            });
        });
    }

    template <typename... Args>
    [[nodiscard]] std::function<void(Args...)> event(std::function<void(Args...)> const& handler) const {
        if (!handler) {
            return {};
        }
        // The closure owns a copy of the handler and frees it when it dies; the analyzer loses the copy's heap block
        // in libc++'s std::function and reports it as leaked.
        // NOLINTNEXTLINE(clang-analyzer-cplusplus.NewDeleteLeaks)
        return [runtime = _rt, handler](Args... args) {
            static_cast<void>(runCallback(*runtime, [&] { handler(std::move(args)...); }));
        };
    }

    void applyCommon(reactive::Scope& scope, Widget& widget, Common const& common) {
        if (common.visible.isBound() || !common.visible.constant()) {
            bind(scope, common.visible, [&widget](bool shown) { widget.setVisible(shown); });
        }
        if (common.enabled.isBound() || !common.enabled.constant()) {
            bind(scope, common.enabled, [&widget](bool enabled) { widget.setEnabled(enabled); });
        }
        if (common.layout != LayoutHints{}) {
            widget.setLayout(common.layout);
        }
        if (common.dragKey.isBound() || common.dragKey.constant().has_value()) {
            bind(scope, common.dragKey, [&widget](std::optional<Key> const& key) { widget.setDragKey(key); });
        }
        if (common.onDrop) {
            // No predicate accepts every key; a predicate that throws refuses the drop.
            std::function<bool(Key const&)> accepts = [](Key const&) { return true; };
            if (common.accepts) {
                accepts = [runtime = _rt, predicate = common.accepts](Key const& key) {
                    bool accepted = false;
                    static_cast<void>(runCallback(*runtime, [&] { accepted = predicate(key); }));
                    return accepted;
                };
            }
            widget.setDropHandler(std::move(accepts), event(common.onDrop));
        }
    }

    void mountChildren(reactive::Scope& scope, std::vector<Node> const& children, ContainerWidget& container) {
        for (Node const& child : children) {
            static_cast<void>(mount(scope, child, &container));
        }
    }

    Widget& mountStack(reactive::Scope& scope, Axis axis, std::vector<Node> const& children, int gap,
                       Common const& common, ContainerWidget* parent) {
        StackWidget& widget = scope.adopt(_backend->createStack(parent, axis));
        applyCommon(scope, widget, common);
        widget.setGap(gap);
        mountChildren(scope, children, widget);
        return widget;
    }

    [[nodiscard]] static std::vector<std::string> labelsOf(std::vector<MenuItem> const& items) {
        std::vector<std::string> labels;
        labels.reserve(items.size());
        for (MenuItem const& item : items) {
            labels.push_back(item.label.evaluate());
        }
        return labels;
    }

    Widget& mountKind(reactive::Scope& scope, Text const& spec, ContainerWidget* parent) {
        TextWidget& widget = scope.adopt(_backend->createText(parent));
        applyCommon(scope, widget, spec.common);
        bind(scope, spec.text, [&widget](std::string const& text) { widget.setText(text); });
        bind(scope, spec.role, [&widget](TextRole role) { widget.setRole(role); });
        return widget;
    }

    Widget& mountKind(reactive::Scope& scope, Button const& spec, ContainerWidget* parent) {
        ButtonWidget& widget = scope.adopt(_backend->createButton(parent));
        applyCommon(scope, widget, spec.common);
        bind(scope, spec.label, [&widget](std::string const& label) { widget.setLabel(label); });
        widget.setOnClick(event(spec.onClick));
        return widget;
    }

    Widget& mountKind(reactive::Scope& scope, TextInput const& spec, ContainerWidget* parent) {
        TextInputWidget& widget = scope.adopt(_backend->createTextInput(parent, spec.mode));
        applyCommon(scope, widget, spec.common);
        bind(scope, spec.value, [&widget](std::string const& value) { widget.setText(value); });
        bind(scope, spec.placeholder, [&widget](std::string const& text) { widget.setPlaceholder(text); });
        widget.setOnChange(event(spec.onChange));
        widget.setOnSubmit(event(spec.onSubmit));
        return widget;
    }

    Widget& mountKind(reactive::Scope& scope, Checkbox const& spec, ContainerWidget* parent) {
        CheckboxWidget& widget = scope.adopt(_backend->createCheckbox(parent));
        applyCommon(scope, widget, spec.common);
        bind(scope, spec.label, [&widget](std::string const& label) { widget.setLabel(label); });
        bind(scope, spec.checked, [&widget](bool checked) { widget.setChecked(checked); });
        widget.setOnToggle(event(spec.onToggle));
        return widget;
    }

    Widget& mountKind(reactive::Scope& scope, Select const& spec, ContainerWidget* parent) {
        SelectWidget& widget = scope.adopt(_backend->createSelect(parent, spec.style));
        applyCommon(scope, widget, spec.common);
        bind(scope, spec.options, [&widget](std::vector<SelectOption> const& options) { widget.setOptions(options); });
        bind(scope, spec.selected, [&widget](std::optional<Key> const& key) { widget.setSelected(key); });
        widget.setOnSelect(event(spec.onSelect));
        return widget;
    }

    Widget& mountKind(reactive::Scope& scope, Menu const& spec, ContainerWidget* parent) {
        MenuWidget& widget = scope.adopt(_backend->createMenu(parent));
        applyCommon(scope, widget, spec.common);
        std::vector<MenuItem> const& items = spec.items;
        bool const anyBound = std::ranges::any_of(items, [](MenuItem const& item) { return item.label.isBound(); });
        // One prop for the whole list: a bound label re-sends every label, which keeps setItems the one setter.
        Prop<std::vector<std::string>> const labels =
            anyBound ? Prop<std::vector<std::string>>([items = items] { return labelsOf(items); })
                     : Prop<std::vector<std::string>>(labelsOf(items));
        bind(scope, labels, [&widget](std::vector<std::string> const& texts) { widget.setItems(texts); });
        std::vector<Action> actions;
        actions.reserve(items.size());
        for (MenuItem const& item : items) {
            actions.push_back(item.onSelect);
        }
        widget.setOnActivate(event(std::function<void(std::size_t)>{[actions = std::move(actions)](std::size_t index) {
            if (index < actions.size() && actions.at(index)) {
                actions.at(index)();
            }
        }}));
        return widget;
    }

    Widget& mountKind(reactive::Scope& scope, Column const& spec, ContainerWidget* parent) {
        return mountStack(scope, Axis::Vertical, spec.children, spec.gap, spec.common, parent);
    }

    Widget& mountKind(reactive::Scope& scope, Row const& spec, ContainerWidget* parent) {
        return mountStack(scope, Axis::Horizontal, spec.children, spec.gap, spec.common, parent);
    }

    Widget& mountKind(reactive::Scope& scope, Grid const& spec, ContainerWidget* parent) {
        GridWidget& widget = scope.adopt(_backend->createGrid(parent));
        applyCommon(scope, widget, spec.common);
        widget.setColumns(spec.columns);
        widget.setGap(spec.gap);
        for (GridCell const& cell : spec.cells) {
            Widget* const child = mount(scope, cell.node, &widget);
            if (child != nullptr && cell.span != 1) {
                widget.setSpan(*child, cell.span);
            }
        }
        return widget;
    }

    Widget& mountKind(reactive::Scope& scope, Spacer const& spec, ContainerWidget* parent) {
        SpacerWidget& widget = scope.adopt(_backend->createSpacer(parent));
        applyCommon(scope, widget, spec.common);
        return widget;
    }

    Widget& mountKind(reactive::Scope& scope, Panel const& spec, ContainerWidget* parent) {
        PanelWidget& widget = scope.adopt(_backend->createPanel(parent));
        applyCommon(scope, widget, spec.common);
        bind(scope, spec.title, [&widget](std::string const& title) { widget.setTitle(title); });
        widget.setPadding(spec.padding);
        widget.setCollapsible(spec.collapsible);
        if (spec.collapsible) {
            bind(scope, spec.collapsed, [&widget](bool collapsed) { widget.setCollapsed(collapsed); });
            widget.setOnToggle(event(spec.onToggle));
        }
        static_cast<void>(mount(scope, spec.child, &widget));
        return widget;
    }

    Widget& mountKind(reactive::Scope& scope, Scroll const& spec, ContainerWidget* parent) {
        ScrollWidget& widget = scope.adopt(_backend->createScroll(parent, spec.axis));
        applyCommon(scope, widget, spec.common);
        static_cast<void>(mount(scope, spec.child, &widget));
        return widget;
    }

    // Content a binding mounts (a Switch case, Dialog content), all or nothing: it is built in a scope of its own,
    // which the caller takes only once the mount completed, so content that throws partway leaves nothing behind.
    // Built untracked, so the binding's Effect depends on its own prop alone. The scope is one level deeper than the
    // one that owns the binding, so the owner's Effects run before the content's.
    [[nodiscard]] std::unique_ptr<reactive::Scope> mountContent(std::size_t depth, Node const& node,
                                                                ContainerWidget& host) {
        auto content = std::make_unique<reactive::Scope>(*_rt, depth);
        _rt->untracked([&] { static_cast<void>(mount(*content, node, &host)); });
        return content;
    }

    // reset() tears the old case down, newest first, before the new one is built. The Computed behind a bound
    // selector is equality-gated, so an unchanged key never gets here. A case that fails to mount leaves nothing for
    // its key; the next change of the key mounts again.
    Widget& mountKind(reactive::Scope& scope, Switch const& spec, ContainerWidget* parent) {
        SlotWidget& widget = scope.adopt(_backend->createSlot(parent));
        applyCommon(scope, widget, spec.common);
        auto& content = scope.make<std::unique_ptr<reactive::Scope>>();
        bind(scope, spec.selector,
             [this, &widget, &content, depth = scope.depth() + 1, cases = spec.cases,
              fallback = spec.fallback](Key const& key) {
                 content.reset();
                 auto const chosen = std::ranges::find(cases, key, &SwitchCase::key);
                 Node const& node = chosen == cases.end() ? fallback : chosen->node;
                 if (node) {
                     content = mountContent(depth, node, widget);
                 }
             });
        return widget;
    }

    // Every page is owned by `pageScope`, a child scope made before the binding, so pages are destroyed newest first,
    // after the binding that shows them and before the tab bar. A scope fixes that order; a vector's element
    // destruction order differs between standard libraries.
    //
    // A bound index is not equality-gated: the Effect reads the binding itself, not a Computed. After a failed page,
    // the application's onSelect writes the index back inside this Effect's own run; a Computed would keep the failed
    // index and find the user's next pick of that tab unchanged, while the Effect's dependency on the binding's own
    // signals sees it as the change it is. The cost is that showPage also runs when something the binding reads
    // changes but the index does not: for a page that is mounted it only re-sends the highlight, and for a page that
    // failed it tries the mount again, once per run.
    Widget& mountKind(reactive::Scope& scope, Tabs const& spec, ContainerWidget* parent) {
        TabsWidget& widget = scope.adopt(_backend->createTabs(parent));
        applyCommon(scope, widget, spec.common);
        std::vector<std::string> labels;
        labels.reserve(spec.tabs.size());
        for (Tab const& tab : spec.tabs) {
            labels.push_back(tab.label);
        }
        widget.setTabs(labels);
        widget.setOnSelect(event(spec.onSelect));
        auto& pageScope = scope.child();
        auto& pages = scope.make<Pages>();
        pages.slots.resize(spec.tabs.size(), nullptr);
        pages.onSelect = event(spec.onSelect);
        auto show = [this, &widget, &pageScope, &pages, pageSpecs = spec.tabs](std::size_t index) {
            _rt->untracked([&] { showPage(widget, pageScope, pages, pageSpecs, index); });
        };
        if (!spec.selected.isBound()) {
            show(spec.selected.constant());
        } else {
            scope.effect([binding = spec.selected.binding(), show = std::move(show)] { show(binding()); });
        }
        return widget;
    }

    // A page is mounted the first time its tab is selected and only hidden afterwards, so its widgets keep their
    // state (focus, scroll position, half-typed text) across tab switches. A new page is complete before anything
    // else changes: one that fails to mount leaves the previous page shown and selected, and the bar's highlight,
    // which the user's pick may already have moved, goes back to it. The next change of the index mounts again.
    void showPage(TabsWidget& widget, reactive::Scope& pageScope, Pages& pages, std::vector<Tab> const& pageSpecs,
                  std::size_t index) {
        bool const exists = index < pageSpecs.size();
        bool const isNew = exists && pages.slots.at(index) == nullptr;
        if (isNew) {
            try {
                mountPage(widget, pageScope, pages, pageSpecs.at(index).node, index);
            } catch (...) {
                if (pages.shown.has_value()) {
                    restoreSelection(widget, pages, *pages.shown);
                }
                throw;
            }
        }
        if (pages.shown != index) {
            if (pages.shown.has_value() && *pages.shown < pages.slots.size()) {
                if (SlotWidget* const previous = pages.slots.at(*pages.shown); previous != nullptr) {
                    previous->setVisible(false);
                }
            }
            if (exists && !isNew) {
                pages.slots.at(index)->setVisible(true);
            }
        }
        pages.shown = index;
        widget.setSelected(index);
    }

    // After a page failed to mount: puts the bar's highlight back on the page still shown, and tells the application
    // through `onSelect`, so its index matches the widget and selecting the failed tab again is a change that mounts
    // again.
    //
    // onSelect runs last, from a copy: it is a widget callback, which may destroy the Mounted and with it `pages`,
    // the widget and the Effect that called this. Nothing after it touches any of them; the caller only rethrows.
    //
    // Nothing here may replace the failure being reported: the callback's wrapper reports what the callback throws,
    // and anything else thrown here is dropped.
    static void restoreSelection(TabsWidget& widget, Pages const& pages, std::size_t shown) noexcept {
        try {
            widget.setSelected(shown);
        } catch (...) {  // NOLINT(bugprone-empty-catch)
            // The page's failure is the one reported; the bar keeps whatever highlight it had.
        }
        try {
            std::function<void(std::size_t)> const onSelect = pages.onSelect;
            if (onSelect) {
                onSelect(shown);
            }
        } catch (...) {  // NOLINT(bugprone-empty-catch)
            // Only copying the wrapper can throw here; the application then keeps the failed index.
        }
    }

    // The page is built in a scope of its own and handed to `pageScope` once complete, so one that fails to mount
    // leaves nothing behind.
    void mountPage(TabsWidget& widget, reactive::Scope& pageScope, Pages& pages, Node const& node, std::size_t index) {
        auto page = std::make_unique<reactive::Scope>(*_rt, pageScope.depth());
        SlotWidget& slot = page->adopt(_backend->createSlot(&widget));
        static_cast<void>(mount(*page, node, &slot));
        pageScope.adopt(std::move(page));
        pages.slots.at(index) = &slot;
    }

    // The content is built before the overlay opens and torn down before it closes. Content that fails to mount
    // under a bound `open` leaves the dialog closed and calls `onDismiss`, so the application's `open` follows the
    // widget and opening it again is a change that mounts again.
    //
    // A bound `open` is not equality-gated, for the reason a Tabs index is not: onDismiss writes `open` inside this
    // Effect's own run, which a Computed would miss, and then find the next opening unchanged. An Effect that runs
    // with `open` unchanged re-sends `setOpen`, and mounts content only if none is mounted: once per run while `open`
    // stays true over content that failed.
    Widget& mountKind(reactive::Scope& scope, Dialog const& spec, ContainerWidget* parent) {
        DialogWidget& widget = scope.adopt(_backend->createDialog(parent));
        applyCommon(scope, widget, spec.common);
        bind(scope, spec.title, [&widget](std::string const& title) { widget.setTitle(title); });
        std::function<void()> onDismiss = event(spec.onDismiss);
        widget.setOnDismiss(onDismiss);
        auto& content = scope.make<std::unique_ptr<reactive::Scope>>();
        auto show = [this, &widget, &content, depth = scope.depth() + 1, child = spec.child](
                        bool open, std::function<void()> const& dismiss) {
            if (!open) {
                content.reset();
            } else if (content == nullptr) {
                try {
                    content = mountContent(depth, child, widget);
                } catch (...) {
                    dismissFailed(dismiss);
                    throw;
                }
            }
            widget.setOpen(open);
        };
        if (!spec.open.isBound()) {
            // A constant has no application state to bring back in line: content that fails throws out of the mount.
            show(spec.open.constant(), {});
        } else {
            scope.effect([binding = spec.open.binding(), show = std::move(show), onDismiss = std::move(onDismiss)] {
                show(binding(), onDismiss);
            });
        }
        return widget;
    }

    // After a Dialog's content failed to mount: tells the application through `onDismiss` that the dialog is closed.
    // It runs last, from a copy: it is a widget callback, which may destroy the Mounted and with it the dialog and
    // the Effect that called this; the caller only rethrows after it. Nothing thrown here may replace the failure
    // being reported.
    static void dismissFailed(std::function<void()> const& onDismiss) noexcept {
        try {
            std::function<void()> const dismiss = onDismiss;
            if (dismiss) {
                dismiss();
            }
        } catch (...) {  // NOLINT(bugprone-empty-catch)
            // Only copying the wrapper can throw here; the application then keeps `open` true.
        }
    }

    Widget& mountKind(reactive::Scope& scope, ForEach const& spec, ContainerWidget* parent) {
        StackWidget& widget = scope.adopt(_backend->createStack(parent, spec.axis));
        applyCommon(scope, widget, spec.common);
        widget.setGap(spec.gap);
        mountRows(scope, spec.model, widget, {});
        return widget;
    }

    // Rows first, then the selection. The widget marks a selected key whenever its row arrives, so the order only
    // shapes the log: the first setSelection comes after the rows and already marks them.
    Widget& mountKind(reactive::Scope& scope, Table const& spec, ContainerWidget* parent) {
        TableWidget& widget = scope.adopt(_backend->createTable(parent));
        applyCommon(scope, widget, spec.common);
        widget.setColumns(spec.columns);
        widget.setSelectionMode(spec.selectionMode);
        widget.setOnSelectionChange(event(spec.onSelectionChange));
        widget.setOnActivate(event(spec.onActivate));
        mountRows(scope, spec.rows, widget,
                  [&widget](Widget& rowWidget, Key const& key) { widget.setRowKey(rowWidget, key); });
        bind(scope, spec.selection, [&widget](std::vector<Key> const& keys) { widget.setSelection(keys); });
        return widget;
    }

    // The model, then the session, then the rows, then the Effect: destroyed in reverse, so the Effect stops
    // first, the rows (whose slots point into the session's snapshot) go next, and the model outlives its session.
    void mountRows(reactive::Scope& scope, std::shared_ptr<ForEachModel const> const& model,
                   ContainerWidget& container, RowHook onRow) {
        if (!model) {
            return;
        }
        scope.make<std::shared_ptr<ForEachModel const>>(model);
        ForEachSession& session = scope.adopt(model->open(*_rt));
        auto& rows = scope.make<KeyedRows>();
        scope.effect([this, &session, &rows, &container, depth = scope.depth() + 1, onRow = std::move(onRow)] {
            std::vector<Key> const keys = session.pull();  // tracked: the rows are this Effect's only source
            // Untracked: what a row view reads while it is built, or what a kept row's update touches, must not
            // become a dependency of the whole list.
            _rt->untracked([&] { reconcile(depth, session, rows.list, container, keys, onRow); });
        });
    }

    // The snapshot index of each key's first occurrence, in order; every later duplicate is reported.
    [[nodiscard]] std::vector<std::size_t> firstOccurrences(std::vector<Key> const& keys) {
        std::unordered_set<Key> seen;
        std::vector<std::size_t> order;
        order.reserve(keys.size());
        for (std::size_t index = 0; index < keys.size(); ++index) {
            if (seen.insert(keys.at(index)).second) {
                order.push_back(index);
            } else {
                _rt->core()->report(site::kDuplicateKey);
            }
        }
        return order;
    }

    // All or nothing: the row is built in a scope of its own, one level deeper than the ForEach's, which the caller
    // takes only once the mount completed.
    [[nodiscard]] KeyedRow mountRow(std::size_t depth, ForEachSession& session, ContainerWidget& container,
                                    Key const& key, std::size_t index, RowHook const& onRow) {
        KeyedRow row{.key = key, .scope = std::make_unique<reactive::Scope>(*_rt, depth)};
        row.slot = &row.scope->adopt(session.makeRow(index));
        row.widget = mount(*row.scope, row.slot->view(), &container);
        if (row.widget != nullptr && onRow) {
            onRow(*row.widget, key);
        }
        return row;
    }

    // Moves the widgets into the snapshot's order, and `rows` with them. A move that throws is recorded in `failure`
    // when it is the first failure, and leaves `rows` mirroring the container as it stands.
    static void reorder(std::vector<KeyedRow>& rows, ContainerWidget& container, std::vector<Key> const& keys,
                        std::vector<std::size_t> const& order, std::exception_ptr& failure) {
        std::unordered_map<Key, std::size_t> position;  // key -> index in `rows`
        std::unordered_map<Widget const*, std::size_t> rowOf;
        std::vector<Widget*> present;
        for (std::size_t i = 0; i < rows.size(); ++i) {
            KeyedRow const& row = rows.at(i);
            position.emplace(row.key, i);
            if (row.widget != nullptr) {
                rowOf.emplace(row.widget, i);
                present.push_back(row.widget);
            }
        }
        std::vector<Widget*> target;
        target.reserve(present.size());
        for (std::size_t const index : order) {
            if (auto const found = position.find(keys.at(index)); found != position.end()) {
                if (KeyedRow const& row = rows.at(found->second); row.widget != nullptr) {
                    target.push_back(row.widget);
                }
            }
        }
        std::vector<KeyedRow> ordered;
        ordered.reserve(rows.size());
        try {
            reorderChildren(container, present, target);
        } catch (...) {
            if (!failure) {
                failure = std::current_exception();
            }
        }
        // `present` is the container's order now: the snapshot's, unless a move threw. Rows without a widget have
        // no place in the container and go last.
        for (Widget const* const widget : present) {
            ordered.push_back(std::move(rows.at(rowOf.at(widget))));
        }
        for (KeyedRow& row : rows) {
            if (row.widget == nullptr) {
                ordered.push_back(std::move(row));
            }
        }
        rows = std::move(ordered);
    }

    // Brings `rows` to the snapshot `keys`. `rows` mirrors the container's order on entry and on exit, also when
    // something throws, so a failure costs this run's remaining work and never the bookkeeping of the next one.
    // A row that fails to mount leaves nothing behind and does not stop the other rows; the first failure is
    // rethrown at the end, for the Effect to report, and the next run mounts the row again.
    void reconcile(std::size_t depth, ForEachSession& session, std::vector<KeyedRow>& rows, ContainerWidget& container,
                   std::vector<Key> const& keys, RowHook const& onRow) {
        std::vector<std::size_t> const order = firstOccurrences(keys);
        std::unordered_map<Key, std::size_t> wanted;  // key -> snapshot index
        for (std::size_t const index : order) {
            wanted.emplace(keys.at(index), index);
        }

        // Unmount the gone keys, last first as KeyedRows does, then update the kept ones in place, which keeps their
        // widgets.
        for (KeyedRow& row : std::views::reverse(rows)) {
            if (!wanted.contains(row.key)) {
                row.scope.reset();
            }
        }
        std::erase_if(rows, [](KeyedRow const& row) { return row.scope == nullptr; });
        std::unordered_set<Key> kept;
        for (KeyedRow const& row : rows) {
            kept.insert(row.key);
            row.slot->assign(wanted.at(row.key));
        }

        // Mount the new keys; each widget is appended to the container, and its row to `rows`.
        std::exception_ptr failure;
        rows.reserve(order.size());
        for (std::size_t const index : order) {
            if (kept.contains(keys.at(index))) {
                continue;
            }
            try {
                rows.push_back(mountRow(depth, session, container, keys.at(index), index, onRow));
            } catch (...) {
                if (!failure) {
                    failure = std::current_exception();
                }
            }
        }

        reorder(rows, container, keys, order, failure);
        if (failure) {
            std::rethrow_exception(failure);
        }
    }

    Widget& mountKind(reactive::Scope& scope, Busy const& spec, ContainerWidget* parent) {
        BusyWidget& widget = scope.adopt(_backend->createBusy(parent));
        applyCommon(scope, widget, spec.common);
        bind(scope, spec.active, [&widget](bool active) { widget.setActive(active); });
        bind(scope, spec.label, [&widget](std::string const& label) { widget.setLabel(label); });
        return widget;
    }

    Widget& mountKind(reactive::Scope& scope, DateTimeInput const& spec, ContainerWidget* parent) {
        DateTimeInputWidget& widget =
            scope.adopt(_backend->createDateTimeInput(parent, spec.mode, spec.offsetMinutes));
        applyCommon(scope, widget, spec.common);
        bind(scope, spec.value,
             [&widget](std::optional<morph::time::Timestamp> const& value) { widget.setValue(value); });
        widget.setOnChange(event(spec.onChange));
        return widget;
    }

    Widget& mountKind(reactive::Scope& scope, Slider const& spec, ContainerWidget* parent) {
        SliderWidget& widget = scope.adopt(_backend->createSlider(parent));
        applyCommon(scope, widget, spec.common);
        widget.setRange(spec.minimum, spec.maximum, spec.step);
        bind(scope, spec.value, [&widget](std::int64_t value) { widget.setValue(value); });
        widget.setOnChange(event(spec.onChange));
        return widget;
    }

    Widget& mountKind(reactive::Scope& scope, FilePicker const& spec, ContainerWidget* parent) {
        FilePickerWidget& widget = scope.adopt(_backend->createFilePicker(parent, spec.mode));
        applyCommon(scope, widget, spec.common);
        bind(scope, spec.path, [&widget](std::string const& path) { widget.setPath(path); });
        widget.setOnPicked(event(spec.onPicked));
        return widget;
    }

    reactive::Runtime* _rt;
    IViewBackend* _backend;
};

}  // namespace detail

/// @brief A view tree mounted on a backend: its widgets, built once, and the bindings that keep them current.
///
/// Destroying it destroys every binding before the widget it drives and every child before its parent. The
/// runtime, the backend and every signal a binding reads must outlive it; the owner of the runtime constructs and
/// destroys it. Non-copyable and non-movable: bindings point into it.
///
/// Every widget callback, a drop's `accepts` predicate included, runs as a `reactive::Runtime::widgetEvent` and
/// untracked, so a binding whose setter makes the backend call a handler does not subscribe to what the handler
/// reads. An exception a callback throws is reported (`detail::site::kCallbackThrew`) and goes no further: the
/// backend's call returns normally, a predicate that threw refuses the drop, and what the callback wrote before
/// throwing still flushes.
///
/// A mount that throws goes no further than the content it was building, and leaves none of it behind. A node mounted
/// directly, a constant Switch selector, Tabs index or Dialog `open` included, throws out of the constructor. Content
/// a binding mounts, which a bound selector, index or `open` does in an Effect even during the constructor, is
/// reported (`reactive::detail::site::kEffectThrew`) instead: a Switch shows nothing for that key, having torn the old
/// case down first; a Tabs keeps the previous page shown and selected; a Dialog stays closed. A Tabs then calls its
/// `onSelect` with the page still shown, and a Dialog its `onDismiss`, each as a widget callback and as the last thing
/// the mount does, so the application's state follows the widget and its next pick of the failed tab, or opening of
/// the dialog, mounts again. A Switch selector is equality-gated, so its next change of key mounts again. A Tabs
/// index and a Dialog `open` are not: while one still names content that failed — no `onSelect` or `onDismiss` moved
/// it — every run of its binding, a change to any signal the binding reads included, tries that mount again, once
/// per run, reporting each failure and calling `onSelect` or `onDismiss` again.
///
/// A ForEach or a Table mounts its rows in an Effect too, one row at a time: a row that fails is reported, leaves
/// nothing behind and is left out, the other rows are mounted and ordered all the same, and the next change of the
/// rows mounts it again. A key repeated in one snapshot is reported (`detail::site::kDuplicateKey`), and only its
/// first row is shown.
///
/// Bindings run in owner order. The tree's own bindings are at the depth the mount was given; the content of a Switch
/// case, a Tabs page or a Dialog, and every ForEach or Table row, is in a scope one level deeper than the binding that
/// mounted it. So an Effect that removes content runs before the content's own bindings in the same flush, and a
/// binding never evaluates against state its owner is about to take away. A mount placed under an outer scope (a
/// screen's, a row's) takes that scope's depth plus one, so the outer scope's Effects run first too.
///
/// A mount must run to completion: nothing it calls, a backend factory or setter or a binding's evaluation, may
/// destroy this object, its runtime or its backend. Widget callbacks are events, not part of a mount, and may.
class Mounted {
public:
    /// @param runtime The runtime the bindings are made in. Borrowed.
    /// @param backend Makes the widgets. Borrowed.
    /// @param root The tree; kept alive as long as the mount.
    /// @param parent The container the root widget is appended to, or null for a backend root.
    /// @param depth The scope depth of the tree's own bindings (`reactive::Scope::depth`): zero for a mount no other
    ///        scope owns, one more than the owner's for a mount placed under a scope.
    /// @throws std::invalid_argument when @p root is null.
    Mounted(reactive::Runtime& runtime MORPH_LIFETIMEBOUND, IViewBackend& backend MORPH_LIFETIMEBOUND, Node root,
            ContainerWidget* parent = nullptr, std::size_t depth = 0)
        : _root{nonNull(std::move(root))},
          _mounter{runtime, backend},
          _scope{runtime, depth},
          // Untracked, so mounting from inside an Effect does not subscribe that Effect to anything read here.
          _widget{runtime.untracked([this, parent] { return _mounter.mount(_scope, _root, parent); })} {}

    ~Mounted() = default;
    Mounted(Mounted const&) = delete;
    Mounted& operator=(Mounted const&) = delete;
    Mounted(Mounted&&) = delete;
    Mounted& operator=(Mounted&&) = delete;

    /// @brief The root node's widget.
    /// @return The widget; valid as long as this object.
    [[nodiscard]] Widget& root() const noexcept { return *_widget; }

private:
    [[nodiscard]] static Node nonNull(Node root) {
        if (!root) {
            throw std::invalid_argument{"morph::ui::Mounted: the root node is null"};
        }
        return root;
    }

    Node _root;
    detail::Mounter _mounter;
    reactive::Scope _scope;  // after _mounter: destroyed first, while the mounter its bindings call is alive
    Widget* _widget;
};

}  // namespace morph::ui

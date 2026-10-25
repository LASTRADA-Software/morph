// SPDX-License-Identifier: Apache-2.0

#pragma once
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <numeric>
#include <optional>
#include <ranges>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

#include "../attributes.hpp"
#include "../reactive/runtime.hpp"
#include "../reactive/scope.hpp"
#include "../reactive/signal.hpp"
#include "../ui/backend.hpp"
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
using RowHook = std::function<void(reactive::Scope&, Node const&, Widget&, Key const&)>;

/// @brief Whether a row's widget is realized, and if not, why -- so the windowing pass of `reconcile` never
///        confuses a row that has no widget by design (its view was a null node) with one whose widget is simply
///        not realized right now, which it should realize the next time the row enters the visible range.
enum class RowWidgetState : std::uint8_t {
    Realized,    ///< `widgetScope`/`widget` are set.
    Dewindowed,  ///< The row's view is not null, but its widget is not realized: out of the visible range.
    NullView,    ///< The row's view is a null node; it never had a widget and never will while that stays true.
};

/// @brief One mounted row of a ForEach or Table.
///
/// A row's state (its slot, and the signal it reads) and its widget are owned by two separate scopes so a
/// virtualized table can release a row's widget -- the expensive part of mounting a row -- while its key is still
/// in the data, without losing the row's place: `stateScope` is made once, when the key first appears, and lives
/// until the key is gone; `widgetScope` comes and goes with whether the row is realized. A non-virtualized
/// `ForEach`/`Table` (below its `Virtualization::threshold`, or on a backend that never dewindows a row) always
/// has `widgetScope` set alongside `stateScope`, so this costs nothing when windowing never applies.
struct KeyedRow {
    /// @brief The row's key.
    Key key;
    /// @brief Owns the row's slot; made once, for as long as the key is in the data.
    std::unique_ptr<reactive::Scope> stateScope;
    /// @brief The row's slot, owned by `stateScope`.
    RowSlot* slot = nullptr;
    /// @brief Owns the row's widget and its bindings; null while the row is dewindowed or has no view.
    std::unique_ptr<reactive::Scope> widgetScope;
    /// @brief The row's widget, owned by `widgetScope`; null exactly when `widgetScope` is null.
    Widget* widget = nullptr;
    /// @brief Which of the three states above `widget` is in.
    RowWidgetState widgetState = RowWidgetState::NullView;
    /// @brief This row's position in view order, as of the most recent `reconcile` pass -- not its position in
    ///        `KeyedRows::list`, which `reorder` groups realized rows ahead of dewindowed ones within (so the
    ///        container only ever holds the widgets that exist), and so no longer matches view order once any row
    ///        is dewindowed. `applyWindow` compares this against the visible range; `reconcile` is the only writer.
    std::size_t viewPosition = 0;
};

/// @brief What the backend's own scroll position asks a virtualized table to realize next.
struct VisibleRange {
    /// @brief The view index of the first row the backend wants realized.
    std::size_t first = 0;
    /// @brief How many rows from `first` the backend wants realized; 0 (the default) asks for nothing, which a
    ///        table below its `Virtualization::threshold` never overrides, since its windowing Effect only runs on
    ///        a backend that calls `setOnVisibleRangeChanged` in the first place.
    std::size_t count = 0;

    /// @brief Memberwise equality, so an unchanged range notifies nobody.
    /// @return Whether both `first` and `count` match.
    bool operator==(VisibleRange const&) const = default;
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
    std::vector<SlotWidget*> pageSlots;
    /// @brief The page shown now, if any; may be past the last page, and then none is shown.
    std::optional<std::size_t> shown;
    /// @brief The tab the bar highlights, as far as the mount knows: the last one it set, or the user's pick since.
    std::optional<std::size_t> highlighted;
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
        return std::visit(
            [&](auto const& spec) -> Widget* {
                if constexpr (requires { spec.common.layout.alignSelf; }) {
                    checkPlacement(spec.common, parent);
                }
                return &this->mountKind(scope, spec, parent);
            },
            node->kind);
    }

    /// @brief Mounts @p node as `mount` does, as a mount of its own: the first `autofocus` widget it mounts, in
    ///        document order, takes keyboard focus once the whole of it has mounted.
    /// @param scope Owns everything mounted.
    /// @param node The node; null mounts nothing.
    /// @param parent The container to append to, or null for a root.
    /// @return The node's widget, or null for a null node.
    Widget* mountRoot(reactive::Scope& scope, Node const& node, ContainerWidget* parent) {
        return mountPass([&] { return mount(scope, node, parent); });
    }

    /// @brief Moves keyboard focus to the mounted widget whose `focusId` is @p focusId.
    ///
    /// Of several such widgets (rows of a list), the one mounted first is taken. A backend that cannot move focus
    /// leaves it where it was; the request was still made.
    /// @param focusId The name.
    /// @return Whether a mounted widget has that name.
    bool focus(std::string_view focusId) {
        // lower_bound, not find: of equal names, find may answer any; lower_bound answers the first inserted.
        auto const found = _focusable.lower_bound(focusId);
        if (found == _focusable.end() || found->first != focusId) {
            return false;
        }
        try {
            found->second->focus();
        } catch (...) {  // NOLINT(bugprone-empty-catch)
            // Focus is a request: a backend that cannot move it leaves it where it was.
        }
        return true;
    }

private:
    // Runs @p body with @p overlay as the overlay an ActionBar mounts into the footer of, and puts the previous one
    // back after it.
    template <typename F>
    auto underOverlay(ContainerWidget* overlay, F const& body) -> std::invoke_result_t<F const&> {
        class Restore {
        public:
            Restore(Mounter& self, ContainerWidget* next) : _self{&self}, _saved{std::exchange(self._overlay, next)} {}
            Restore(Restore const&) = delete;
            Restore& operator=(Restore const&) = delete;
            Restore(Restore&&) = delete;
            Restore& operator=(Restore&&) = delete;
            ~Restore() { _self->_overlay = _saved; }

        private:
            Mounter* _self;
            ContainerWidget* _saved;
        };
        Restore const restore{*this, overlay};
        return body();
    }

    // The footer of the overlay whose content is being mounted, or null outside any.
    [[nodiscard]] ContainerWidget* overlayFooter() const {
        if (auto* const dialog = dynamic_cast<DialogWidget*>(_overlay)) {
            return &dialog->footer();
        }
        if (auto* const drawer = dynamic_cast<DrawerWidget*>(_overlay)) {
            return &drawer->footer();
        }
        return nullptr;
    }

    // One mount: the root, or content, a page or a row a binding mounts later. The outermost pass focuses the first
    // autofocus widget mounted during it, in document order, after everything in it has mounted; a pass nested in
    // another (content a binding mounts while the root mounts) leaves that to the outer one. A pass that throws gives
    // back the candidate it found, because its caller destroys what it mounted.
    template <typename F>
    auto mountPass(F const& body) -> std::invoke_result_t<F const&> {
        Widget* const before = _autofocus;
        ++_passDepth;
        try {
            auto result = body();
            --_passDepth;
            if (_passDepth == 0) {
                if (Widget* const target = std::exchange(_autofocus, nullptr); target != nullptr) {
                    try {
                        target->focus();
                    } catch (...) {  // NOLINT(bugprone-empty-catch)
                        // Focus is a request: a backend that cannot move it leaves it where it was.
                    }
                }
            }
            return result;
        } catch (...) {
            _autofocus = before;
            --_passDepth;
            throw;
        }
    }

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

    /// @brief A controlled input's re-assertion: what the widget shows as far as the mount knows, what its slot
    ///        says now, and how to show a value.
    /// @tparam T The value type.
    template <typename T>
    struct Reassertion {
        /// @brief What the widget shows: the last value the mount set, or the last value the user asked for since.
        ///        Owned by the scope, made before this, so it outlives it.
        std::optional<T>* shown;
        /// @brief What the slot says now.
        std::function<T()> read;
        /// @brief Shows a value, and records it in `*shown`.
        std::function<void(T const&)> apply;
    };

    // What a controlled input's event calls after the application's handler, with the value the user asked for.
    //
    // The widget shows the request already: it is recorded as shown. Then one turn is posted to the owner; it reads
    // the slot untracked and shows it when the widget shows anything else. The turn is posted after the event's
    // widgetEvent closed, so it runs after the flush the event caused, whose bindings record what they set: an
    // accepted request costs no setter call, a refused one snaps back, and a transformed one, which the binding has
    // shown already, costs nothing more. The check is owned by @p scope and held weakly by the event and the turn,
    // so neither does anything once the content is gone.
    template <typename T, typename Read, typename Apply>
    [[nodiscard]] std::function<void(T const&)> reasserter(reactive::Scope& scope, std::optional<T>& shown, Read read,
                                                           Apply apply) {
        auto const check = std::make_shared<Reassertion<T>>(
            Reassertion<T>{.shown = &shown, .read = std::move(read), .apply = std::move(apply)});
        scope.make<std::shared_ptr<Reassertion<T>>>(check);
        return [runtime = _rt, weak = std::weak_ptr{check}](T const& requested) {
            if (auto const live = weak.lock()) {
                *live->shown = requested;
            } else {
                return;
            }
            // Called from a widget handler, which must not throw: a post that fails leaves the widget showing the
            // user's value until the slot next changes.
            try {
                runtime->owner().post([runtime, weak] {
                    if (auto const live = weak.lock()) {
                        runtime->untracked([&live] { showSlot(*live); });
                    }
                });
            } catch (...) {  // NOLINT(bugprone-empty-catch): see above
            }
        };
    }

    // Shows the slot's value if the widget shows anything else. A slot that throws is its binding's failure, which
    // the binding's Effect reports; the widget keeps what the user did.
    template <typename T>
    static void showSlot(Reassertion<T> const& check) noexcept {
        try {
            T const current = check.read();
            if (!(check.shown->has_value() && **check.shown == current)) {
                check.apply(current);
            }
        } catch (...) {  // NOLINT(bugprone-empty-catch): reported by the binding's Effect, see above
        }
    }

    // A controlled input's value: bound like any property, recording what it shows, and not sending what it already
    // shows; a slot is also re-asserted after each user event (see `reasserter`). Returns what the event calls, or
    // nothing for a constant, which is not a slot: the widget keeps what the user did to it.
    template <typename T, typename Apply>
    [[nodiscard]] std::function<void(T const&)> controlled(reactive::Scope& scope, Prop<T> const& prop, Apply apply) {
        auto& shown = scope.make<std::optional<T>>();
        // A value the widget already shows, the user's accepted edit above all, is not sent again.
        auto show = [&shown, apply = std::move(apply)](T const& value) {
            if (shown.has_value() && *shown == value) {
                return;
            }
            apply(value);
            shown = value;
        };
        reactive::Computed<T> const* const value = bind(scope, prop, show);
        if (value == nullptr) {
            return {};
        }
        return reasserter<T>(scope, shown, [value] { return value->get(); }, std::move(show));
    }

    // An input's event: the application's handler, run as `event` runs it, then the re-assertion of the request.
    // Installed whenever either exists, so a slot without a handler is read-only rather than editable.
    // The first argument is what the user asked for; any others (a table's kind of change) only reach the handler.
    template <typename T, typename Arg, typename... Rest>
    [[nodiscard]] std::function<void(Arg, Rest...)> inputEvent(std::function<void(Arg, Rest...)> const& handler,
                                                               std::function<void(T const&)> reassert) const {
        if (!reassert) {
            return event(handler);
        }
        return [inner = event(handler), reassert = std::move(reassert)](Arg arg, Rest... rest) {
            T const requested(arg);
            if (inner) {
                inner(std::move(arg), std::move(rest)...);
            }
            reassert(requested);
        };
    }

    // The loader's rules for bounds, for a view tree built in C++: no negative bound, no minimum above its maximum,
    // and no fixed amount outside its bounds, which would leave it no room.
    static void checkLayout(Common const& common) {
        LayoutHints const& hints = common.layout;
        auto const refuse = [&common](std::string const& what) {
            throw std::logic_error{"morph::ui::mount: " + what + " on the node" +
                                   (common.testId.empty() ? std::string{} : " `" + common.testId + "`")};
        };
        auto const check = [&refuse](Sizing sizing, std::optional<int> minimum, std::optional<int> maximum,
                                     std::string const& axis) {
            if (minimum.value_or(0) < 0 || maximum.value_or(0) < 0) {
                refuse("a negative " + axis + " bound");
            }
            if (minimum && maximum && *minimum > *maximum) {
                refuse("a minimum " + axis + " above its maximum");
            }
            if (sizing.kind == Sizing::Kind::Fixed &&
                (sizing.amount < minimum.value_or(sizing.amount) || sizing.amount > maximum.value_or(sizing.amount))) {
                refuse("a fixed " + axis + " outside its bounds");
            }
        };
        check(hints.width, hints.minWidth, hints.maxWidth, "width");
        check(hints.height, hints.minHeight, hints.maxHeight, "height");
    }

    // A dialog or a drawer is sized by the frontend, not by its container: Stretch sizing on one would silently
    // change how every *other* sibling in its own parent layout is justified. Covers both the document path's own
    // computed (non-literal) `sizing` -- resolved to a Sizing only here, at mount time, since the document-level
    // checkSizing validates only a literal one -- and a C++ author calling ui::dialog()/ui::drawer() directly, both
    // of which reach this same mountKind overload before either backend ever sees the result.
    static void checkOverlayLayout(Common const& common, std::string_view kind) {
        LayoutHints const& hints = common.layout;
        auto const refuse = [&common, kind](std::string const& axis) {
            throw std::logic_error{"morph::ui::mount: a " + std::string{kind} +
                                   " is sized by the frontend, not by its container; its " + axis + " cannot stretch" +
                                   (common.testId.empty() ? std::string{} : " (node `" + common.testId + "`)")};
        };
        if (hints.width.kind == Sizing::Kind::Stretch) {
            refuse("width");
        }
        if (hints.height.kind == Sizing::Kind::Stretch) {
            refuse("height");
        }
    }

    // The loader's rule for `alignSelf`, for a view tree built in C++: only a stack's or a grid's child has a cross
    // axis to be placed on.
    static void checkPlacement(Common const& common, ContainerWidget const* parent) {
        if (common.layout.alignSelf && dynamic_cast<StackWidget const*>(parent) == nullptr &&
            dynamic_cast<GridWidget const*>(parent) == nullptr) {
            std::string const named = common.testId.empty() ? std::string{} : " `" + common.testId + "`";
            throw std::logic_error{
                "morph::ui::mount: `alignSelf` places a node in its row, column or grid, and the node" + named +
                " is not in one"};
        }
    }

    void applyCommon(reactive::Scope& scope, Widget& widget, Common const& common) {
        if (common.visible.isBound() || !common.visible.constant()) {
            bind(scope, common.visible, [&widget](bool shown) { widget.setVisible(shown); });
        }
        if (common.enabled.isBound() || !common.enabled.constant()) {
            bind(scope, common.enabled, [&widget](bool enabled) { widget.setEnabled(enabled); });
        }
        if (common.layout != LayoutHints{}) {
            checkLayout(common);
            widget.setLayout(common.layout);
        }
        if (common.dragKey.isBound() || common.dragKey.constant().has_value()) {
            bind(scope, common.dragKey, [&widget](std::optional<Key> const& key) { widget.setDragKey(key); });
        }
        if (common.onDrop) {
            applyDrop(widget, common);
        }
        if (common.a11y.name.isBound() || !common.a11y.name.constant().empty()) {
            bind(scope, common.a11y.name, [&widget](std::string const& name) { widget.setAccessibleName(name); });
        }
        if (!common.a11y.role.empty()) {
            widget.setAccessibleRole(common.a11y.role);
        }
        if (!common.testId.empty()) {
            widget.setTestId(common.testId);
        }
        if (common.tooltip.isBound() || !common.tooltip.constant().empty()) {
            bind(scope, common.tooltip, [&widget](std::string const& text) { widget.setTooltip(text); });
        }
        if (!common.surface.empty()) {
            widget.setSurface(common.surface);
        }
        if (!common.keys.empty()) {
            applyKeys(widget, common.keys);
        }
        if (common.autofocus && _autofocus == nullptr) {
            _autofocus = &widget;
        }
        if (!common.focusId.empty()) {
            scope.make<Focusable>(_focusable, _focusable.emplace(common.focusId, &widget));
        }
    }

    using FocusMap = std::multimap<std::string, Widget*, std::less<>>;

    // A widget's entry in the focus map, owned by the widget's scope after the widget, so it leaves the map before
    // the widget is destroyed.
    struct Focusable {
        Focusable(FocusMap& owner, FocusMap::iterator added) noexcept : map{&owner}, entry{added} {}
        Focusable(Focusable const&) = delete;
        Focusable& operator=(Focusable const&) = delete;
        Focusable(Focusable&&) = delete;
        Focusable& operator=(Focusable&&) = delete;
        ~Focusable() { map->erase(entry); }

        FocusMap* map;
        FocusMap::iterator entry;
    };

    // No predicate accepts every key; a predicate that throws refuses the drop.
    void applyDrop(Widget& widget, Common const& common) {
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

    // One setKeys for all the chords; a chord listed twice runs its first binding, as a widget callback.
    void applyKeys(Widget& widget, std::vector<KeyBinding> const& keys) {
        std::vector<std::string> chords;
        chords.reserve(keys.size());
        for (KeyBinding const& binding : keys) {
            chords.push_back(binding.chord);
        }
        widget.setKeys(chords, [runtime = _rt, keys](std::string const& chord) {
            auto const bound = std::ranges::find(keys, chord, &KeyBinding::chord);
            if (bound != keys.end() && bound->onPress) {
                static_cast<void>(runCallback(*runtime, bound->onPress));
            }
        });
    }

    void mountChildren(reactive::Scope& scope, std::vector<Node> const& children, ContainerWidget& container) {
        for (Node const& child : children) {
            static_cast<void>(mount(scope, child, &container));
        }
    }

    // The alignment only when it is not the default, as for a button's emphasis, so a stack that says nothing more
    // costs no call.
    Widget& mountStack(reactive::Scope& scope, Axis axis, std::vector<Node> const& children, int gap,
                       Common const& common, ContainerWidget* parent, Align align = Align::Stretch,
                       Justify justify = Justify::Start) {
        StackWidget& widget = scope.adopt(_backend->createStack(parent, axis));
        applyCommon(scope, widget, common);
        widget.setGap(spacingStep(gap));
        if (align != Align::Stretch) {
            widget.setAlign(align);
        }
        if (justify != Justify::Start) {
            widget.setJustify(justify);
        }
        mountChildren(scope, children, widget);
        return widget;
    }

    // The entries as the widget shows them, each binding read once; called inside the menu's one binding.
    // NOLINTNEXTLINE(misc-no-recursion): a submenu is a menu one level down.
    [[nodiscard]] static std::vector<MenuEntry> entriesOf(std::vector<MenuItem> const& items) {
        std::vector<MenuEntry> entries;
        entries.reserve(items.size());
        for (MenuItem const& item : items) {
            entries.push_back(MenuEntry{.label = item.label.evaluate(),
                                        .icon = item.icon,
                                        .keys = item.keys,
                                        .checked = item.checked.evaluate(),
                                        .enabled = item.enabled.evaluate(),
                                        .items = entriesOf(item.items)});
        }
        return entries;
    }

    // Whether any entry, at any depth, has a bound label, check mark or enablement.
    // NOLINTNEXTLINE(misc-no-recursion): a submenu is a menu one level down.
    [[nodiscard]] static bool anyBound(std::vector<MenuItem> const& items) {
        return std::ranges::any_of(items, entryBound);
    }

    // Whether this entry, or one in its submenu, has a bound label, check mark or enablement.
    // NOLINTNEXTLINE(misc-no-recursion): a submenu is a menu one level down.
    [[nodiscard]] static bool entryBound(MenuItem const& item) {
        return item.label.isBound() || item.checked.isBound() || item.enabled.isBound() || anyBound(item.items);
    }

    // The entry a path names, or null when the path names none.
    [[nodiscard]] static MenuItem const* itemAt(std::vector<MenuItem> const& items,
                                                std::vector<std::size_t> const& path) {
        std::vector<MenuItem> const* level = &items;
        MenuItem const* found = nullptr;
        for (std::size_t const index : path) {
            if (level == nullptr || index >= level->size()) {
                return nullptr;
            }
            found = &level->at(index);
            level = &found->items;
        }
        return found;
    }

    /// An input's label and whether it shows it: only when it has a label or shows one, so an input without a label
    /// is told nothing.
    template <class Widget>
    void applyCaption(reactive::Scope& scope, Widget& widget, Prop<std::string> const& label, bool shown) {
        if (!shown && !label.isBound() && label.constant().empty()) {
            return;
        }
        bind(scope, label, [&widget, shown](std::string const& text) { widget.setCaption(text, shown); });
    }

    // Like applyCommon, a constant default calls no setter.
    void applyField(reactive::Scope& scope, FieldWidget& widget, FieldState const& field) {
        if (field.readonly.isBound() || field.readonly.constant()) {
            bind(scope, field.readonly, [&widget](bool readonly) { widget.setReadOnly(readonly); });
        }
        if (field.required.isBound() || field.required.constant()) {
            bind(scope, field.required, [&widget](bool required) { widget.setRequired(required); });
        }
        if (field.errors.isBound() || !field.errors.constant().empty()) {
            bind(scope, field.errors, [&widget](std::vector<std::string> const& errors) { widget.setErrors(errors); });
        }
        if (field.stale.isBound() || field.stale.constant()) {
            bind(scope, field.stale, [&widget](bool stale) { widget.setStale(stale); });
        }
    }

    Widget& mountKind(reactive::Scope& scope, Text const& spec, ContainerWidget* parent) {
        TextWidget& widget = scope.adopt(_backend->createText(parent));
        applyCommon(scope, widget, spec.common);
        applyField(scope, widget, spec.field);
        bind(scope, spec.text, [&widget](std::string const& text) { widget.setText(text); });
        bind(scope, spec.role, [&widget](TextRole role) { widget.setRole(role); });
        return widget;
    }

    Widget& mountKind(reactive::Scope& scope, Button const& spec, ContainerWidget* parent) {
        ButtonWidget& widget = scope.adopt(_backend->createButton(parent));
        applyCommon(scope, widget, spec.common);
        bind(scope, spec.label, [&widget](std::string const& label) { widget.setLabel(label); });
        if (spec.emphasis != ButtonEmphasis::Normal) {
            widget.setEmphasis(spec.emphasis);
        }
        if (!spec.icon.empty()) {
            widget.setIcon(spec.icon);
        }
        if (spec.style == ButtonStyle::Item) {
            widget.setStyle(spec.style);
        }
        if (spec.selected.isBound() || spec.selected.constant()) {
            bind(scope, spec.selected, [&widget](bool selected) { widget.setSelected(selected); });
        }
        widget.setOnClick(event(spec.onClick));
        return widget;
    }

    Widget& mountKind(reactive::Scope& scope, TextInput const& spec, ContainerWidget* parent) {
        bool const hasUnit = spec.unit.isBound() || !spec.unit.constant().empty();
        if ((hasUnit || spec.decimals >= 0 || !spec.icon.empty()) && spec.mode == TextInputMode::Multiline) {
            throw std::logic_error{
                "morph::ui: a multiline text input holds no number, so it takes no unit, places or icon"};
        }
        TextInputWidget& widget = scope.adopt(_backend->createTextInput(parent, spec.mode));
        applyCommon(scope, widget, spec.common);
        applyField(scope, widget, spec.field);
        auto reassert = controlled(scope, spec.value, [&widget](std::string const& value) { widget.setText(value); });
        bind(scope, spec.placeholder, [&widget](std::string const& text) { widget.setPlaceholder(text); });
        // An input without a unit or places is not told any, so a backend sees only what a field declares.
        if (hasUnit) {
            bind(scope, spec.unit, [&widget](std::string const& unit) { widget.setUnit(unit); });
        }
        if (spec.decimals >= 0) {
            widget.setDecimals(spec.decimals);
        }
        if (!spec.icon.empty()) {
            widget.setIcon(spec.icon);
        }
        applyCaption(scope, widget, spec.label, spec.showLabel);
        widget.setOnChange(inputEvent(spec.onChange, reassert));
        // An empty submit handler tells the backend the field does not submit, so Enter goes on to its ancestors'
        // chords (a dialog's default) once the field has committed.
        widget.setOnSubmit(spec.onSubmit ? inputEvent(spec.onSubmit, std::move(reassert))
                                         : std::function<void(std::string)>{});
        widget.setOnCommit(event(spec.onCommit));
        return widget;
    }

    Widget& mountKind(reactive::Scope& scope, Checkbox const& spec, ContainerWidget* parent) {
        CheckboxWidget& widget = scope.adopt(_backend->createCheckbox(parent));
        applyCommon(scope, widget, spec.common);
        applyField(scope, widget, spec.field);
        bind(scope, spec.label, [&widget](std::string const& label) { widget.setLabel(label); });
        widget.setOnToggle(inputEvent(
            spec.onToggle, controlled(scope, spec.checked, [&widget](bool checked) { widget.setChecked(checked); })));
        return widget;
    }

    Widget& mountKind(reactive::Scope& scope, Select const& spec, ContainerWidget* parent) {
        SelectWidget& widget = scope.adopt(_backend->createSelect(parent, spec.style));
        applyCommon(scope, widget, spec.common);
        applyField(scope, widget, spec.field);
        bind(scope, spec.options, [&widget](std::vector<SelectOption> const& options) { widget.setOptions(options); });
        widget.setOnSelect(inputEvent(
            spec.onSelect,
            controlled(scope, spec.selected, [&widget](std::optional<Key> const& key) { widget.setSelected(key); })));
        widget.setOnOpen(event(spec.onOpen));
        applyCaption(scope, widget, spec.label, spec.showLabel);
        return widget;
    }

    Widget& mountKind(reactive::Scope& scope, Menu const& spec, ContainerWidget* parent) {
        MenuWidget& widget = scope.adopt(_backend->createMenu(parent));
        applyCommon(scope, widget, spec.common);
        std::vector<MenuItem> const& items = spec.items;
        // One prop for the whole tree: a bound label, mark or enablement anywhere re-sends every entry, which keeps
        // setItems the one setter.
        Prop<std::vector<MenuEntry>> const entries =
            anyBound(items) ? Prop<std::vector<MenuEntry>>([items = items] { return entriesOf(items); })
                            : Prop<std::vector<MenuEntry>>(entriesOf(items));
        bind(scope, entries, [&widget](std::vector<MenuEntry> const& shown) { widget.setItems(shown); });
        widget.setOnActivate(
            event(std::function<void(std::vector<std::size_t>)>{[items = items](std::vector<std::size_t> const& path) {
                MenuItem const* const item = itemAt(items, path);
                if (item != nullptr && item->onSelect) {
                    item->onSelect();
                }
            }}));
        return widget;
    }

    Widget& mountKind(reactive::Scope& scope, Column const& spec, ContainerWidget* parent) {
        return mountStack(scope, Axis::Vertical, spec.children, spec.gap, spec.common, parent, spec.align,
                          spec.justify);
    }

    Widget& mountKind(reactive::Scope& scope, Row const& spec, ContainerWidget* parent) {
        if ((spec.tone || spec.label || spec.heading) && dynamic_cast<TableWidget*>(parent) == nullptr) {
            throw std::logic_error{
                "morph::ui: a row's tone, label and heading are a table row's; this row is not in a table"};
        }
        return mountStack(scope, Axis::Horizontal, spec.children, spec.gap, spec.common, parent, spec.align,
                          spec.justify);
    }

    Widget& mountKind(reactive::Scope& scope, Grid const& spec, ContainerWidget* parent) {
        GridWidget& widget = scope.adopt(_backend->createGrid(parent));
        applyCommon(scope, widget, spec.common);
        widget.setColumns(spec.columns);
        widget.setGap(spacingStep(spec.gap));
        if (spec.align != Align::Stretch) {
            widget.setAlign(spec.align);
        }
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
        if (Widget* const header = mount(scope, spec.header, &widget); header != nullptr) {
            widget.setHeader(*header);
        }
        widget.setPadding(spacingStep(spec.padding));
        widget.setCollapsible(spec.collapsible);
        if (spec.collapsible) {
            widget.setOnToggle(inputEvent(spec.onToggle, controlled(scope, spec.collapsed, [&widget](bool collapsed) {
                                              widget.setCollapsed(collapsed);
                                          })));
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
        return mountPass([&] {
            auto content = std::make_unique<reactive::Scope>(*_rt, depth);
            _rt->untracked([&] { static_cast<void>(mount(*content, node, &host)); });
            return content;
        });
    }

    // reset() tears the old case down, newest first, before the new one is built. The Computed behind a bound
    // selector is equality-gated, so an unchanged key never gets here. A case that fails to mount leaves nothing for
    // its key; the next change of the key mounts again.
    Widget& mountKind(reactive::Scope& scope, Switch const& spec, ContainerWidget* parent) {
        SlotWidget& widget = scope.adopt(_backend->createSlot(parent));
        applyCommon(scope, widget, spec.common);
        auto& content = scope.make<std::unique_ptr<reactive::Scope>>();
        bind(scope, spec.selector,
             [this, &widget, &content, depth = scope.depth() + 1, cases = spec.cases, fallback = spec.fallback,
              overlay = _overlay](Key const& key) {
                 content.reset();
                 auto const chosen = std::ranges::find(cases, key, &SwitchCase::key);
                 Node const& node = chosen == cases.end() ? fallback : chosen->node;
                 if (node) {
                     content = underOverlay(overlay, [&] { return mountContent(depth, node, widget); });
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
        // One prop over every caption, as a menu's entries: a bound caption re-sends them all. The count never
        // changes, so the widget keeps its highlight, the shown page and the focus across a relabel.
        std::vector<Prop<std::string>> captions;
        captions.reserve(spec.tabs.size());
        for (Tab const& tab : spec.tabs) {
            captions.push_back(tab.label);
        }
        auto labelsOf = [captions] {
            std::vector<std::string> labels;
            labels.reserve(captions.size());
            for (Prop<std::string> const& caption : captions) {
                labels.push_back(caption.evaluate());
            }
            return labels;
        };
        Prop<std::vector<std::string>> const labels = std::ranges::any_of(captions, &Prop<std::string>::isBound)
                                                          ? Prop<std::vector<std::string>>(std::move(labelsOf))
                                                          : Prop<std::vector<std::string>>(labelsOf());
        bind(scope, labels, [&widget](std::vector<std::string> const& shown) { widget.setTabs(shown); });
        auto& pageScope = scope.child();
        auto& pages = scope.make<Pages>();
        pages.pageSlots.resize(spec.tabs.size(), nullptr);
        // A bound index is controlled: after a pick, the bar highlights the page that is shown, so a pick the
        // document refuses moves the highlight back. A constant index is not a slot, and the bar keeps the pick.
        std::function<void(std::size_t const&)> reassert;
        if (spec.selected.isBound()) {
            reassert = reasserter<std::size_t>(
                scope, pages.highlighted,
                [&pages] { return pages.shown.value_or(std::numeric_limits<std::size_t>::max()); },
                [&widget, &pages](std::size_t index) { highlight(widget, pages, index); });
        }
        widget.setOnSelect(inputEvent(spec.onSelect, reassert));
        pages.onSelect = inputEvent(spec.onSelect, std::move(reassert));
        auto show = [this, &widget, &pageScope, &pages, pageSpecs = spec.tabs, overlay = _overlay](std::size_t index) {
            _rt->untracked([&] { showPage(widget, pageScope, pages, pageSpecs, index, overlay); });
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
    //
    // Only the mount runs under the tabs' overlay, and that overlay is put back before a failure is reported:
    // reporting it may destroy the Mounted, and with it this Mounter.
    void showPage(TabsWidget& widget, reactive::Scope& pageScope, Pages& pages, std::vector<Tab> const& pageSpecs,
                  std::size_t index, ContainerWidget* overlay) {
        bool const exists = index < pageSpecs.size();
        bool const isNew = exists && pages.pageSlots.at(index) == nullptr;
        if (isNew) {
            try {
                underOverlay(overlay, [&] { mountPage(widget, pageScope, pages, pageSpecs.at(index).node, index); });
            } catch (...) {
                if (pages.shown.has_value()) {
                    restoreSelection(widget, pages, *pages.shown);
                }
                throw;
            }
        }
        if (pages.shown != index) {
            if (pages.shown.has_value() && *pages.shown < pages.pageSlots.size()) {
                if (SlotWidget* const previous = pages.pageSlots.at(*pages.shown); previous != nullptr) {
                    previous->setVisible(false);
                }
            }
            if (exists && !isNew) {
                pages.pageSlots.at(index)->setVisible(true);
            }
        }
        pages.shown = index;
        highlight(widget, pages, index);
    }

    // Moves the bar's highlight, and records it.
    static void highlight(TabsWidget& widget, Pages& pages, std::size_t index) {
        widget.setSelected(index);
        pages.highlighted = index;
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
    static void restoreSelection(TabsWidget& widget, Pages& pages, std::size_t shown) noexcept {
        try {
            highlight(widget, pages, shown);
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
        auto page = mountPass([&] {
            auto built = std::make_unique<reactive::Scope>(*_rt, pageScope.depth());
            SlotWidget& slot = built->adopt(_backend->createSlot(&widget));
            static_cast<void>(mount(*built, node, &slot));
            return std::pair{std::move(built), &slot};
        });
        pageScope.adopt(std::move(page.first));
        pages.pageSlots.at(index) = page.second;
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
        checkOverlayLayout(spec.common, "dialog");
        DialogWidget& widget = scope.adopt(_backend->createDialog(parent));
        applyCommon(scope, widget, spec.common);
        return mountOverlay(scope, spec, widget);
    }

    Widget& mountKind(reactive::Scope& scope, Drawer const& spec, ContainerWidget* parent) {
        checkOverlayLayout(spec.common, "drawer");
        DrawerWidget& widget = scope.adopt(_backend->createDrawer(parent, spec.side));
        applyCommon(scope, widget, spec.common);
        return mountOverlay(scope, spec, widget);
    }

    // A Dialog or a Drawer: the title, the dismissal, and content that exists only while open.
    template <typename Spec, typename OverlayWidget>
    Widget& mountOverlay(reactive::Scope& scope, Spec const& spec, OverlayWidget& widget) {
        bind(scope, spec.title, [&widget](std::string const& title) { widget.setTitle(title); });
        auto& content = scope.make<std::unique_ptr<reactive::Scope>>();
        auto& shownOpen = scope.make<std::optional<bool>>();
        // A bound `open` is controlled: a dismissal the document refuses opens the dialog again, over the content it
        // still holds. Content that failed to mount is not there to show, so the dialog stays closed for it.
        std::function<void()> onDismiss = event(spec.onDismiss);
        if (spec.open.isBound()) {
            auto reassert =
                reasserter<bool>(scope, shownOpen, spec.open.binding(), [&widget, &content, &shownOpen](bool open) {
                    if (!open || content != nullptr) {
                        widget.setOpen(open);
                        shownOpen = open;
                    }
                });
            onDismiss = [inner = std::move(onDismiss), reassert = std::move(reassert)] {
                if (inner) {
                    inner();
                }
                reassert(false);
            };
        }
        widget.setOnDismiss(onDismiss);
        auto show = [this, &widget, &content, &shownOpen, depth = scope.depth() + 1, child = spec.child](
                        bool open, std::function<void()> const& dismiss) {
            if (!open) {
                content.reset();
            } else if (content == nullptr) {
                try {
                    content = underOverlay(&widget, [&] { return mountContent(depth, child, widget); });
                } catch (...) {
                    dismissFailed(dismiss);
                    throw;
                }
            }
            widget.setOpen(open);
            shownOpen = open;
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
        widget.setGap(spacingStep(spec.gap));
        mountRows(scope, spec.model, widget, {}, nullptr);
        return widget;
    }

    // Rows first, then the selection. The widget marks a selected key whenever its row arrives, so the order only
    // shapes the log: the first setSelection comes after the rows and already marks them.
    Widget& mountKind(reactive::Scope& scope, Table const& spec, ContainerWidget* parent) {
        TableWidget& widget = scope.adopt(_backend->createTable(parent));
        applyCommon(scope, widget, spec.common);
        widget.setColumns(spec.columns);
        widget.setShowHeader(spec.showHeader);
        widget.setSelectionMode(spec.selectionMode);
        if (spec.selectColumn) {
            widget.setSelectColumn(true);
            bind(scope, spec.selectAllLabel, [&widget](std::string const& label) { widget.setSelectAllLabel(label); });
        }
        widget.setOnActivate(event(spec.onActivate));
        if (std::ranges::any_of(spec.columns, &TableColumn::sortable)) {
            bind(scope, spec.sort, [&widget](std::optional<TableSort> const& sort) { widget.setSort(sort); });
            widget.setOnSort(event(spec.onSort));
        }
        mountRows(
            scope, spec.rows, widget,
            [this, &widget](reactive::Scope& rowScope, Node const& node, Widget& rowWidget, Key const& key) {
                widget.setRowKey(rowWidget, key);
                // A row's tone binds in the row's scope, after its key and whether it is a heading.
                Row const* const view = node ? std::get_if<Row>(&node->kind) : nullptr;
                if (view != nullptr && view->heading) {
                    widget.setRowHeading(rowWidget);
                }
                if (view != nullptr && view->tone) {
                    bind(rowScope, *view->tone, [&widget, &rowWidget](std::optional<Tone> const& tone) {
                        widget.setRowTone(rowWidget, tone);
                    });
                }
                if (view != nullptr && view->label) {
                    bind(rowScope, *view->label,
                         [&widget, &rowWidget](std::string const& label) { widget.setRowLabel(rowWidget, label); });
                }
            },
            &spec.virtualize);
        widget.setOnSelectionChange(inputEvent(
            spec.onSelectionChange, controlled(scope, spec.selection, [&widget](std::vector<Key> const& keys) {
                widget.setSelection(keys);
            })));
        return widget;
    }

    // The model, then the session, then the rows, then the Effect(s): destroyed in reverse, so an Effect stops
    // first, the rows (whose slots point into the session's snapshot) go next, and the model outlives its session.
    // @p virtualize is null for a ForEach (no backend seam exists yet to dewindow a bare keyed stack); when
    // non-null but @p container is not also a TableWidget (today, impossible -- only Table passes a non-null
    // virtualize, and only its mountKind gives it a TableWidget -- kept as a defensive check, not a real case),
    // virtualization is silently not wired, same as if it were null.
    void mountRows(reactive::Scope& scope, std::shared_ptr<ForEachModel const> const& model,
                   ContainerWidget& container, RowHook onRow, Virtualization const* virtualize) {
        if (!model) {
            return;
        }
        scope.make<std::shared_ptr<ForEachModel const>>(model);
        ForEachSession& session = scope.adopt(model->open(*_rt));
        auto& rows = scope.make<KeyedRows>();
        TableWidget* const table = virtualize != nullptr ? dynamic_cast<TableWidget*>(&container) : nullptr;
        if (table == nullptr) {
            scope.effect([this, &session, &rows, &container, depth = scope.depth() + 1, onRow, overlay = _overlay] {
                std::vector<Key> const keys = session.pull();  // tracked: the rows are this Effect's only source
                // Untracked: what a row view reads while it is built, or what a kept row's update touches, must
                // not become a dependency of the whole list.
                underOverlay(overlay, [&] {
                    _rt->untracked([&] { reconcile(depth, session, rows.list, container, keys, onRow); });
                });
            });
            return;
        }
        // The windowing state: written by the backend's own scroll callback (via widgetEvent, so this is never
        // written from inside a flush this Effect caused), read by both Effects below -- the rows Effect, so a
        // brand-new row's very first mount already respects the window instead of needing a later windowing pass
        // to correct it; the windowing Effect, so a scroll alone realizes/releases without re-running reconcile.
        auto& range = scope.make<reactive::Signal<VisibleRange>>(*_rt, VisibleRange{});
        table->setOnVisibleRangeChanged([this, &range](std::size_t first, std::size_t count) {
            runCallback(*_rt, [&] { range.set({.first = first, .count = count}); });
        });
        scope.effect([this, &session, &rows, &container, &range, threshold = virtualize->threshold,
                      overscan = virtualize->overscan, depth = scope.depth() + 1, onRow, table, overlay = _overlay] {
            std::vector<Key> const keys = session.pull();  // tracked: the rows are this Effect's only source
            VisibleRange const window = range.peek();      // untracked: a scroll alone must not re-run this Effect
            underOverlay(overlay, [&] {
                _rt->untracked([&] {
                    if (keys.size() < threshold) {
                        reconcile(depth, session, rows.list, container, keys, onRow);
                        return;
                    }
                    std::pair<VisibleRange, std::size_t> const windowAndOverscan{window, overscan};
                    reconcile(depth, session, rows.list, container, keys, onRow, &windowAndOverscan);
                    // Only above threshold: a backend's own selection-filtering logic may need to know a key
                    // still exists even without a realized row for it right now (backend.hpp's setExistingKeys).
                    // Below threshold every key already has a realized row, so this would add nothing.
                    table->setExistingKeys(keys);
                });
            });
        });
        scope.effect([this, &rows, &container, &range, threshold = virtualize->threshold,
                      overscan = virtualize->overscan, depth = scope.depth() + 1, onRow, table, overlay = _overlay] {
            VisibleRange const window = range.get();  // tracked: this Effect's own trigger
            // Below threshold, every row is already realized (reconcile's own unconditional path, §4.4), and
            // setRealizedWindow is not called at all -- not even once with "everything realized" -- so a table
            // that never crosses its threshold is indistinguishable, on the backend, from one with no
            // virtualization machinery at all.
            if (rows.list.size() < threshold) {
                return;
            }
            underOverlay(overlay, [&] { applyWindow(depth, rows.list, container, window, overscan, onRow); });
            auto const [first, count] = realizedExtent(rows.list);
            table->setRealizedWindow(first, count, rows.list.size());
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

    // All or nothing: the row's state and its widget are each built in a scope of their own, one level deeper than
    // the ForEach's, which the caller takes only once the mount completed. A new row's widget is realized in the
    // same pass as its state (so it is eligible for autofocus like any other newly-mounted content); a widget
    // realized later, on a row whose state already exists, goes through `realizeRowWidget` instead, which is not
    // its own `mountPass` -- see that function's own comment for why.
    [[nodiscard]] KeyedRow mountRow(std::size_t depth, ForEachSession& session, ContainerWidget& container,
                                    Key const& key, std::size_t index, RowHook const& onRow) {
        return mountPass([&] {
            KeyedRow row{.key = key, .stateScope = std::make_unique<reactive::Scope>(*_rt, depth)};
            row.slot = &row.stateScope->adopt(session.makeRow(index));
            row.widgetScope = std::make_unique<reactive::Scope>(*_rt, depth);
            row.widget = mount(*row.widgetScope, row.slot->view(), &container);
            row.widgetState = row.widget != nullptr ? RowWidgetState::Realized : RowWidgetState::NullView;
            if (row.widget != nullptr && onRow) {
                onRow(*row.widgetScope, row.slot->view(), *row.widget, key);
            }
            return row;
        });
    }

    // Realizes a widget for a row whose state (`stateScope`/`slot`) already exists -- a row re-entering the
    // visible range after being dewindowed. Deliberately not a `mountPass` of its own: `mountPass` gives the first
    // `autofocus` widget it mounts the focus once it completes, which is right for a row mounting for the first
    // time (`mountRow`, inside the ForEach's own outer pass) but wrong here -- a row that already existed and
    // simply scrolled back into view must not steal focus back from wherever the user's focus already is. `mount`
    // itself carries no such behavior; only `mountPass` does, so simply not using it is what suppresses it.
    void realizeRowWidget(std::size_t depth, KeyedRow& row, ContainerWidget& container, RowHook const& onRow) {
        row.widgetScope = std::make_unique<reactive::Scope>(*_rt, depth);
        row.widget = mount(*row.widgetScope, row.slot->view(), &container);
        row.widgetState = row.widget != nullptr ? RowWidgetState::Realized : RowWidgetState::NullView;
        if (row.widget != nullptr && onRow) {
            onRow(*row.widgetScope, row.slot->view(), *row.widget, row.key);
        }
    }

    // Releases a row's widget, keeping its state: the container sees an ordinary removal (`releasing` fires exactly
    // as it does for a real removal), but `stateScope`/`slot` are untouched, so the row's signal keeps tracking the
    // snapshot and is ready for `realizeRowWidget` to mount a fresh widget against it later.
    static void dewindowRowWidget(KeyedRow& row) {
        row.widgetScope.reset();
        row.widget = nullptr;
        row.widgetState = RowWidgetState::Dewindowed;
    }

    // Builds only a new key's state -- no widget, no mount, no onRow call -- for a row `reconcile` decides is
    // outside the current window on its very first appearance (a virtualized table above its threshold). The row
    // starts `Dewindowed`, exactly as if it had been realized once and then released, so `realizeRowWidget` can
    // bring it in later with no special case for "never had a widget" versus "had one and lost it."
    [[nodiscard]] KeyedRow mountRowState(std::size_t depth, ForEachSession& session, Key const& key,
                                         std::size_t index) {
        KeyedRow row{.key = key, .stateScope = std::make_unique<reactive::Scope>(*_rt, depth)};
        row.slot = &row.stateScope->adopt(session.makeRow(index));
        row.widgetState = RowWidgetState::Dewindowed;
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
    // @p window is the virtualization window a new key's widget is realized against on its very first appearance,
    // or null for a ForEach or a Table below its threshold -- unchanged from before virtualization existed. A new
    // key outside the window (or when there is no window at all, above threshold with nothing reported yet, taken
    // as {0, overscan}; see the design's "bootstrap window") gets state only (`mountRowState`), left Dewindowed
    // for the windowing Effect to realize later if it ever enters the visible range.
    void reconcile(std::size_t depth, ForEachSession& session, std::vector<KeyedRow>& rows, ContainerWidget& container,
                   std::vector<Key> const& keys, RowHook const& onRow,
                   std::pair<VisibleRange, std::size_t> const* windowAndOverscan = nullptr) {
        std::vector<std::size_t> const order = firstOccurrences(keys);
        std::unordered_map<Key, std::size_t> wanted;          // key -> snapshot index
        std::unordered_map<Key, std::size_t> viewPositionOf;  // key -> position in view order (order's own index)
        for (std::size_t viewPos = 0; viewPos < order.size(); ++viewPos) {
            wanted.emplace(keys.at(order.at(viewPos)), order.at(viewPos));
            viewPositionOf.emplace(keys.at(order.at(viewPos)), viewPos);
        }

        // Unmount the gone keys, last first as KeyedRows does, then update the kept ones in place, which keeps
        // their widgets. A gone key's widget goes first (the Effect stops first within each row, mirroring the
        // order mountRow itself builds the two scopes in: state first, then widget), then its state.
        for (KeyedRow& row : std::views::reverse(rows)) {
            if (!wanted.contains(row.key)) {
                row.widgetScope.reset();
                row.stateScope.reset();
            }
        }
        std::erase_if(rows, [](KeyedRow const& row) { return row.stateScope == nullptr; });
        std::unordered_set<Key> kept;
        for (KeyedRow& row : rows) {
            kept.insert(row.key);
            row.slot->assign(wanted.at(row.key));
            row.viewPosition = viewPositionOf.at(row.key);
        }

        std::size_t const bootstrapFirst = 0;
        std::size_t const bootstrapCount = windowAndOverscan != nullptr ? windowAndOverscan->second : 0;
        VisibleRange const window = windowAndOverscan != nullptr && windowAndOverscan->first.count > 0
                                        ? windowAndOverscan->first
                                        : VisibleRange{.first = bootstrapFirst, .count = bootstrapCount};

        // Mount the new keys; each realized widget is appended to the container, and every new row (realized or
        // state-only) to `rows`. `viewPos` is the row's position in view order -- order's own index -- which is
        // what the window means positions in, not `index` (a position in the raw, possibly-reordered `keys`).
        std::exception_ptr failure;
        rows.reserve(order.size());
        for (std::size_t viewPos = 0; viewPos < order.size(); ++viewPos) {
            std::size_t const index = order.at(viewPos);
            if (kept.contains(keys.at(index))) {
                continue;
            }
            bool const inWindow =
                windowAndOverscan == nullptr || (viewPos >= window.first && viewPos < window.first + window.count);
            try {
                KeyedRow row = inWindow ? mountRow(depth, session, container, keys.at(index), index, onRow)
                                        : mountRowState(depth, session, keys.at(index), index);
                row.viewPosition = viewPos;
                rows.push_back(std::move(row));
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

    // Realizes a widget for every row within `window` (± `overscan`) that does not already have one, and releases
    // every realized widget outside it. A row's position in view order is its own `viewPosition`, set by the most
    // recent `reconcile` pass -- not its position in `rows` itself, which groups realized rows ahead of
    // dewindowed ones (so the container only ever holds the widgets that exist) and so no longer matches view
    // order once any row is dewindowed. Called only when `rows.size() >= threshold`, from the windowing Effect's
    // own Signal<VisibleRange>, never from reconcile, so a scroll alone costs O(rows) to scan, not the full
    // reconciliation pass.
    void applyWindow(std::size_t depth, std::vector<KeyedRow>& rows, ContainerWidget& container,
                     VisibleRange const& window, std::size_t overscan, RowHook const& onRow) {
        std::size_t const lo = window.first > overscan ? window.first - overscan : 0;
        std::size_t const hi =
            window.first + window.count + overscan;  // exclusive; saturates harmlessly past rows.size()
        std::exception_ptr failure;
        for (KeyedRow& row : rows) {
            bool const shouldBeRealized = row.viewPosition >= lo && row.viewPosition < hi;
            if (shouldBeRealized && row.widgetState != RowWidgetState::Realized &&
                row.widgetState != RowWidgetState::NullView) {
                try {
                    realizeRowWidget(depth, row, container, onRow);
                } catch (...) {
                    if (!failure) {
                        failure = std::current_exception();
                    }
                }
            } else if (!shouldBeRealized && row.widgetState == RowWidgetState::Realized) {
                dewindowRowWidget(row);
            }
        }
        // Realized widgets may now be a different subset of `rows` than before; bring the container back to the
        // rows' own view order (`viewPosition`), restricted to what is realized -- `reorder` needs `order` as
        // positions into `keys` in the target (view) order, not `rows`' own current array order, which groups
        // realized ahead of dewindowed and so is not view order once any row is dewindowed.
        std::vector<Key> const keys = [&] {
            std::vector<Key> out;
            out.reserve(rows.size());
            for (KeyedRow const& row : rows) {
                out.push_back(row.key);
            }
            return out;
        }();
        std::vector<std::size_t> order(rows.size());
        std::iota(order.begin(), order.end(), std::size_t{0});
        std::ranges::sort(order, {}, [&rows](std::size_t index) { return rows.at(index).viewPosition; });
        reorder(rows, container, keys, order, failure);
        if (failure) {
            std::rethrow_exception(failure);
        }
    }

    // The realized subset's extent in view order: the first realized row's view position and how many rows from
    // there are realized. Used only to tell the backend what to size a scrollbar against
    // (`TableWidget::setRealizedWindow`); a gap of dewindowed rows between two realized ones (the overscan margin
    // straddling a scroll position mid-transition) is not expected in steady state, so this reports the outermost
    // realized view positions, not a strict contiguous run.
    [[nodiscard]] static std::pair<std::size_t, std::size_t> realizedExtent(std::vector<KeyedRow> const& rows) {
        std::optional<std::size_t> first;
        std::size_t last = 0;
        for (KeyedRow const& row : rows) {
            if (row.widgetState == RowWidgetState::Realized) {
                first = first ? std::min(*first, row.viewPosition) : row.viewPosition;
                last = std::max(last, row.viewPosition);
            }
        }
        return first ? std::pair{*first, last - *first + 1} : std::pair{std::size_t{0}, std::size_t{0}};
    }

    Widget& mountKind(reactive::Scope& scope, Busy const& spec, ContainerWidget* parent) {
        BusyWidget& widget = scope.adopt(_backend->createBusy(parent));
        applyCommon(scope, widget, spec.common);
        bind(scope, spec.active, [&widget](bool active) { widget.setActive(active); });
        bind(scope, spec.label, [&widget](std::string const& label) { widget.setLabel(label); });
        return widget;
    }

    Widget& mountKind(reactive::Scope& scope, Banner const& spec, ContainerWidget* parent) {
        BannerWidget& widget = scope.adopt(_backend->createBanner(parent));
        applyCommon(scope, widget, spec.common);
        bind(scope, spec.tone, [&widget](Tone tone) { widget.setTone(tone); });
        bind(scope, spec.text, [&widget](std::string const& text) { widget.setText(text); });
        bind(scope, spec.action.label, [&widget](std::string const& label) { widget.setActionLabel(label); });
        widget.setOnAction(event(spec.action.onClick));
        widget.setDismissible(spec.dismissible);
        widget.setOnDismiss(event(spec.onDismiss));
        return widget;
    }

    Widget& mountKind(reactive::Scope& scope, Badge const& spec, ContainerWidget* parent) {
        BadgeWidget& widget = scope.adopt(_backend->createBadge(parent));
        applyCommon(scope, widget, spec.common);
        bind(scope, spec.tone, [&widget](Tone tone) { widget.setTone(tone); });
        bind(scope, spec.text, [&widget](std::string const& text) { widget.setText(text); });
        widget.setIcon(spec.icon);
        return widget;
    }

    Widget& mountKind(reactive::Scope& scope, Progress const& spec, ContainerWidget* parent) {
        ProgressWidget& widget = scope.adopt(_backend->createProgress(parent));
        applyCommon(scope, widget, spec.common);
        bind(scope, spec.value, [&widget](std::optional<double> value) { widget.setValue(value); });
        bind(scope, spec.label, [&widget](std::string const& label) { widget.setLabel(label); });
        return widget;
    }

    Widget& mountKind(reactive::Scope& scope, Steps const& spec, ContainerWidget* parent) {
        StepsWidget& widget = scope.adopt(_backend->createSteps(parent));
        applyCommon(scope, widget, spec.common);
        bind(scope, spec.items, [&widget](std::vector<Step> const& steps) { widget.setSteps(steps); });
        bind(scope, spec.current, [&widget](std::optional<std::size_t> index) { widget.setCurrent(index); });
        return widget;
    }

    Widget& mountKind(reactive::Scope& scope, KeyValue const& spec, ContainerWidget* parent) {
        KeyValueWidget& widget = scope.adopt(_backend->createKeyValue(parent));
        applyCommon(scope, widget, spec.common);
        bind(scope, spec.items, [&widget](std::vector<KeyValueItem> const& items) { widget.setItems(items); });
        return widget;
    }

    Widget& mountKind(reactive::Scope& scope, EmptyState const& spec, ContainerWidget* parent) {
        EmptyStateWidget& widget = scope.adopt(_backend->createEmptyState(parent));
        applyCommon(scope, widget, spec.common);
        bind(scope, spec.title, [&widget](std::string const& title) { widget.setTitle(title); });
        bind(scope, spec.text, [&widget](std::string const& text) { widget.setText(text); });
        widget.setIcon(spec.icon);
        bind(scope, spec.action.label, [&widget](std::string const& label) { widget.setActionLabel(label); });
        widget.setOnAction(event(spec.action.onClick));
        return widget;
    }

    Widget& mountKind(reactive::Scope& scope, Splitter const& spec, ContainerWidget* parent) {
        SplitterWidget& widget = scope.adopt(_backend->createSplitter(parent, spec.orientation));
        applyCommon(scope, widget, spec.common);
        mountChildren(scope, spec.children, widget);
        widget.setOnResize(inputEvent(
            spec.onResize,
            controlled(scope, spec.sizes, [&widget](std::vector<int> const& sizes) { widget.setSizes(sizes); })));
        return widget;
    }

    // The header is mounted first, so it is the first child; the content follows it.
    Widget& mountKind(reactive::Scope& scope, Collapsible const& spec, ContainerWidget* parent) {
        CollapsibleWidget& widget = scope.adopt(_backend->createCollapsible(parent));
        applyCommon(scope, widget, spec.common);
        bind(scope, spec.title, [&widget](std::string const& title) { widget.setTitle(title); });
        if (Widget* const header = mount(scope, spec.header, &widget); header != nullptr) {
            widget.setHeader(*header);
        }
        static_cast<void>(mount(scope, spec.child, &widget));
        widget.setOnToggle(
            inputEvent(spec.onToggle, controlled(scope, spec.open, [&widget](bool open) { widget.setOpen(open); })));
        return widget;
    }

    Widget& mountKind(reactive::Scope& scope, DropZone const& spec, ContainerWidget* parent) {
        DropZoneWidget& widget = scope.adopt(_backend->createDropZone(parent));
        applyCommon(scope, widget, spec.common);
        widget.setAccept(spec.accept);
        widget.setMultiple(spec.multiple);
        widget.setOnDrop(event(spec.onDrop));
        static_cast<void>(mount(scope, spec.child, &widget));
        return widget;
    }

    // One prop of a loaded component, bound to the setter its kind crosses by.
    void bindCustomProp(reactive::Scope& scope, CustomWidget& widget, CustomProp const& prop) {
        std::string const& name = prop.name;
        if (auto const* const text = std::get_if<Prop<std::string>>(&prop.value)) {
            bind(scope, *text, [&widget, name](std::string const& value) { widget.setTextProp(name, value); });
        } else if (auto const* const flag = std::get_if<Prop<bool>>(&prop.value)) {
            bind(scope, *flag, [&widget, name](bool value) { widget.setBoolProp(name, value); });
        } else if (auto const* const number = std::get_if<Prop<double>>(&prop.value)) {
            bind(scope, *number, [&widget, name](double value) { widget.setNumberProp(name, value); });
        }
    }

    // The component the backend loads in the custom node's place, or the fallback when it loads none.
    // An ActionBar is a horizontal stack in the footer of the overlay whose content holds it, not in its own parent.
    Widget& mountKind(reactive::Scope& scope, ActionBar const& spec, ContainerWidget* parent) {
        static_cast<void>(parent);
        ContainerWidget* const footer = overlayFooter();
        if (footer == nullptr) {
            throw std::logic_error{"morph::ui: an action bar mounts only in a dialog's or drawer's content"};
        }
        StackWidget& widget = scope.adopt(_backend->createStack(footer, Axis::Horizontal));
        applyCommon(scope, widget, spec.common);
        widget.setGap(spacingStep(2));
        for (Node const& child : spec.children) {
            static_cast<void>(mount(scope, child, &widget));
        }
        return widget;
    }

    Widget& mountKind(reactive::Scope& scope, Custom const& spec, ContainerWidget* parent) {
        CustomRequest request{.name = spec.name, .props = {}, .minVersion = spec.minVersion};
        request.props.reserve(spec.props.size());
        for (CustomProp const& prop : spec.props) {
            request.props.push_back(prop.name);
        }
        if (std::unique_ptr<CustomWidget> loaded = _backend->createCustom(parent, request)) {
            CustomWidget& widget = scope.adopt(std::move(loaded));
            for (CustomProp const& prop : spec.props) {
                bindCustomProp(scope, widget, prop);
            }
            widget.setOnEvent(spec.onEvent);
            return widget;
        }
        Widget* const fallback = mount(scope, spec.fallback, parent);
        if (fallback == nullptr) {
            throw std::invalid_argument{"morph::ui::Mounted: the custom node " + spec.name + " has no fallback"};
        }
        return *fallback;
    }

    Widget& mountKind(reactive::Scope& scope, DateTimeInput const& spec, ContainerWidget* parent) {
        DateTimeInputWidget& widget =
            scope.adopt(_backend->createDateTimeInput(parent, spec.mode, spec.offsetMinutes));
        applyCommon(scope, widget, spec.common);
        applyField(scope, widget, spec.field);
        widget.setOnChange(inputEvent(
            spec.onChange,
            controlled(scope, spec.value,
                       [&widget](std::optional<morph::time::Timestamp> const& value) { widget.setValue(value); })));
        applyCaption(scope, widget, spec.label, spec.showLabel);
        return widget;
    }

    Widget& mountKind(reactive::Scope& scope, Slider const& spec, ContainerWidget* parent) {
        SliderWidget& widget = scope.adopt(_backend->createSlider(parent));
        applyCommon(scope, widget, spec.common);
        applyField(scope, widget, spec.field);
        widget.setRange(spec.minimum, spec.maximum, spec.step);
        widget.setOnChange(inputEvent(
            spec.onChange, controlled(scope, spec.value, [&widget](std::int64_t value) { widget.setValue(value); })));
        widget.setOnCommit(event(spec.onCommit));
        return widget;
    }

    Widget& mountKind(reactive::Scope& scope, FilePicker const& spec, ContainerWidget* parent) {
        FilePickerWidget& widget = scope.adopt(_backend->createFilePicker(parent, spec.mode));
        applyCommon(scope, widget, spec.common);
        applyField(scope, widget, spec.field);
        widget.setOnPicked(inputEvent(spec.onPicked, controlled(scope, spec.path, [&widget](std::string const& path) {
                                          widget.setPath(path);
                                      })));
        return widget;
    }

    reactive::Runtime* _rt;
    IViewBackend* _backend;
    // The dialog or drawer whose content is being mounted, while it is: an ActionBar mounts into its footer. A binding
    // that mounts later (a Switch case, a Tabs page, a row) mounts under the overlay it was made under. The footer is
    // asked for only when an ActionBar mounts, so an overlay without actions never makes one.
    ContainerWidget* _overlay = nullptr;
    // The first autofocus widget of the mount pass in progress, and how deeply passes are nested.
    Widget* _autofocus = nullptr;
    std::size_t _passDepth = 0;
    // The mounted widgets with a focus name. A multimap keeps equal names in insertion order, so the first mounted
    // is found first.
    FocusMap _focusable;
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
/// An input whose value is a slot is controlled: a text input's text, a checkbox's state, a select's key, a slider's,
/// date-time input's or file picker's value, a collapsible panel's state, a Tabs index, a Dialog's `open` and a
/// Table's selection. After a user's request, and after the flush it caused, a posted turn shows the slot's value if
/// the widget shows anything else: a refused request snaps back, an accepted one costs no setter call, and a
/// transformed one shows the request for that one turn. That holds with no handler too, which makes a slot without one
/// read-only. A constant is not a slot: the widget keeps what the user did to it.
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
          _widget{runtime.untracked([this, parent] { return _mounter.mountRoot(_scope, _root, parent); })} {}

    ~Mounted() = default;
    Mounted(Mounted const&) = delete;
    Mounted& operator=(Mounted const&) = delete;
    Mounted(Mounted&&) = delete;
    Mounted& operator=(Mounted&&) = delete;

    /// @brief The root node's widget.
    /// @return The widget; valid as long as this object.
    [[nodiscard]] Widget& root() const noexcept { return *_widget; }

    /// @brief Moves keyboard focus to the mounted widget whose `Common::focusId` is @p focusId, at any depth,
    ///        including the content of a Switch case, a Tabs page, a Dialog and every row mounted now.
    ///
    /// Of several such widgets, the one mounted first is taken; one whose content has since been unmounted is not
    /// found.
    /// @param focusId The name.
    /// @return Whether a mounted widget has that name.
    bool focus(std::string_view focusId) { return _mounter.focus(focusId); }

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

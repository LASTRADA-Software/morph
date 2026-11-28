// SPDX-License-Identifier: Apache-2.0

// What the interpreter's translation units share, free of the table and grid headers.

#pragma once

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <core/async/StopToken.hpp>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <exception>
#include <expected>
#include <format>
#include <functional>
#include <iterator>
#include <limits>
#include <map>
#include <memory>
#include <morph/core/backend.hpp>
#include <morph/core/bridge.hpp>
#include <morph/core/callback_scope.hpp>
#include <morph/core/completion.hpp>
#include <morph/core/logger.hpp>
#include <morph/core/wire.hpp>
#include <morph/forms/engine/collection.hpp>
#include <morph/forms/engine/field_model.hpp>
#include <morph/forms/engine/form_session.hpp>
#include <morph/forms/engine/wizard.hpp>
#include <morph/reactive/runtime.hpp>
#include <morph/reactive/scheduler.hpp>
#include <morph/reactive/scope.hpp>
#include <morph/reactive/signal.hpp>
#include <morph/ui/backend.hpp>
#include <morph/ui/cell_format.hpp>
#include <morph/ui/chart_scale.hpp>
#include <morph/ui/dev_tools.hpp>
#include <morph/ui/document.hpp>
#include <morph/ui/expression.hpp>
#include <morph/ui/interpreter.hpp>
#include <morph/ui/locale_table.hpp>
#include <morph/ui/message.hpp>
#include <morph/ui/value.hpp>
#include <morph/ui/view.hpp>
#include <optional>
#include <ranges>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <variant>
#include <vector>

namespace morph::ui::detail {

/// One row a copy writes: its view index, and its cells' texts by the columns copied, none for an unloaded cell.
struct CopyRow {
    std::int64_t viewRow = 0;
    std::vector<std::optional<std::string>> cells;
};

/// What a copy writes: each column's header text, and the rows found, in any order.
struct CopyGathered {
    std::vector<std::string> headers;
    std::vector<CopyRow> rows;
};

/// Called once with what a copy gathered, or with the message that says why it could not.
using CopyDone = std::function<void(std::expected<CopyGathered, std::string>)>;

/// What the view name dialog names a view for.
enum class ViewNaming : std::uint8_t {
    Save,     ///< "Save view…": a name the user already has replaces that view.
    Rename,   ///< "Rename view…": the view the arrangement was taken from.
    Publish,  ///< "Publish view…": a name the shared views already have replaces that one.
};

/// The view name dialog: whether it is open, what for, and the name typed.
struct ViewDialog {
    bool open = false;
    ViewNaming purpose = ViewNaming::Save;
    std::string name;

    bool operator==(ViewDialog const&) const = default;
};

/// A cell of a row the table holds, as a cell menu reads it.
struct HeldCell {
    /// The row's record.
    Value record;
    /// The cell, as its column reads it.
    table::Cell cell;
    /// The column, as it shows its cells.
    table::TableColumn shown;
    /// What a `Measure` cell is shown through.
    DisplayUnits units;
    /// What `units` points into.
    std::shared_ptr<void const> keep;
};

/// A table with the dialogs its menu opens mounted beside it (`ui::besideTable`); without dialogs, the table alone.
/// @param placed  The table's common properties; its layout becomes the one inside the column.
/// @param dialogs The dialogs; null for none.
/// @param table   Makes the table, after @p placed is changed.
[[nodiscard]] Node besideDialogs(Common& placed, Node dialogs, std::function<Node()> const& table);

/// @brief A value as a view key: an `int` or a string as itself, anything else as its JSON text.
/// @param value The value.
/// @return The key.
[[nodiscard]] Key keyOf(Value const& value);

// A cell range as a document reads it: `{"anchor": {"key", "column"}, "focus": {"key", "column"}}`, `null` for none.
[[nodiscard]] Value cellRangeValue(std::optional<CellRange> const& range);

// A document's cell range: two corners, each a key and a column id; anything else is none.
[[nodiscard]] std::optional<CellRange> cellRangeOf(Value const& value);

/// @brief A list of strings out of a value: a string, a list of strings, or nothing.
/// @param value The value.
/// @return The strings.
[[nodiscard]] std::vector<std::string> stringsOf(Value const& value);

/// @brief A list of strings as a value.
/// @param texts The strings.
/// @return The list.
[[nodiscard]] Value listOfStrings(std::vector<std::string> const& texts);

/// @brief A command context without its scope: what a callback the host keeps may hold, so that the host never
///        keeps a scope alive past its screen. The callback holds the scope weakly and puts it back.
/// @param ctx The context.
/// @return The context, its scope dropped.
[[nodiscard]] CommandCtx detached(CommandCtx const& ctx);

/// @brief Collects the command lists of a screen's head: its actions', its menu entries' and its tools'.
/// @param doc The screen's document.
/// @param lists Where they go, in document order.
void headCommandLists(Document const& doc, std::vector<Commands const*>& lists);

/// @brief A kind's key spec, from the kind table.
/// @param kind The kind spec.
/// @param key  The key.
/// @return The spec, or `nullptr` when the kind has no such key of its own.
[[nodiscard]] KeySpec const* keySpec(KindSpec const& kind, std::string_view key);

/// @brief Whether a node kind keeps per-element state even without an id: the inputs, dialogs, tables and the forms
///        engine's screens.
/// @param kind The kind.
/// @return It.
[[nodiscard]] bool statefulKind(std::string_view kind);

/// @brief A value with the field at @p path replaced, creating records along the way.
/// @param base  The value written into.
/// @param path  The field path.
/// @param value The new value at the path.
/// @return The new value.
// NOLINTNEXTLINE(misc-no-recursion): one level per path segment
[[nodiscard]] Value withPath(Value const& base, std::span<std::string const> path, Value value);

/// @brief A row's identity for `patch` and `optimistic`: its `id` field.
/// @param row The row.
/// @return The `id` field, or `null`.
[[nodiscard]] Value rowId(Value const& row);

/// @brief The channel an alias is bound to for its current token, binding it when the token changed.
/// @param alias The alias.
/// @param core  The screen.
/// @return The channel, or `nullptr` while the alias is unbound.
[[nodiscard]] std::shared_ptr<ModelChannel> channelOf(ModelAlias& alias, ScreenCore& core);

// --- Declaring scopes ------------------------------------------------------------------------------------------

/// @brief The element a node of a scope keeps its state in.
/// @param scope The scope that declares it.
/// @param node  The node.
/// @return The element, or `nullptr` for a node that keeps none.
[[nodiscard]] ElementState* elementFor(RtScope const& scope, DocNode const& node);

/// @brief What a date input reads from a value: the instant it shows, and whether the value names one at all.
struct DateReading {
    /// @brief The instant shown; empty for an empty field.
    std::optional<time::DateTime> instant;
    /// @brief Whether the value names a date, an instant or nothing; `false` for text that names no date.
    bool readable = true;
};

/// @brief Reads what a date input shows for @p value, in a display zone @p offsetMinutes from UTC.
///
/// The value is read as the date functions read it (`readDateValue`): a timestamp is its instant; a `yyyy-MM-dd`
/// string is midnight of that date in the display zone; a string `DateTime::fromIso8601` reads is its instant; `null`
/// and `""` are empty. Any other value is empty and unreadable.
/// @param value         The value.
/// @param offsetMinutes The display zone's offset from UTC, in minutes.
/// @return The instant, or nothing, and whether the value was readable.
[[nodiscard]] DateReading dateInstantOf(Value const& value, std::int32_t offsetMinutes);

/// @brief A date input's date text for @p instant in a display zone @p offsetMinutes from UTC.
/// @param instant       The instant.
/// @param offsetMinutes The display zone's offset from UTC, in minutes.
/// @return `yyyy-MM-dd`.
[[nodiscard]] std::string dateTextOf(time::DateTime const& instant, std::int32_t offsetMinutes);

/// @brief A value as the places a number is entered with.
/// @param value  The value.
/// @param format The formatting facts.
/// @return The places, 0 to the most a decimal holds; -1 for a value that is not a non-negative integer.
[[nodiscard]] int placesOf(Value const& value, FormatContext const& format);

/// @brief The key of a select option: a record's `key`, `id` or `value` field, or the option itself.
/// @param option The option.
/// @return The key.
[[nodiscard]] Value optionKey(Value const& option);

/// @brief A table's sort as its element value reads it: the bound `sort` when the key binds one, else the table's
///        own.
/// @param element The table's element.
/// @return The sort. Tracked.
[[nodiscard]] Value tableSort(ElementState const& element);

/// @brief The selected keys of the rows a table holds on pages other than the one shown, in their order: what an
///        `Extend` report keeps, since the backend holds the shown page alone.
/// @param view      The table's rows and page.
/// @param selection The selection, a list of keys.
/// @param rowKey    A row's key.
/// @return The keys.
[[nodiscard]] ValueList selectedOffPage(TableView const& view, Value const& selection,
                                        std::function<Key(Value const&)> const& rowKey);

[[nodiscard]] Value tableSelection(ElementState const& element);

// --- Lowering --------------------------------------------------------------------------------------------------

/// @brief A value as a `bool` property: `true` only for `true`.
/// @param value  The value.
/// @param format Unused.
/// @return It.
[[nodiscard]] bool boolOf(Value const& value, FormatContext const& format);

/// @brief A value as a text property.
/// @param value  The value.
/// @param format The formatting facts.
/// @return Its text.
[[nodiscard]] std::string stringOf(Value const& value, FormatContext const& format);

/// @brief An exact rational as the nearest double, unrounded.
///
/// `Rational::toDouble` rounds to the value's own places, which is a display reading: `29 / 100` keeps its operands'
/// 0 places and would read as 0. The view tree wants the magnitude, so the lowering reads it here.
/// @param value The rational.
/// @return Its numerator divided by its denominator.
[[nodiscard]] double realOf(math::Rational const& value) noexcept;

/// @brief A text role by name.
/// @param name `normal`, `muted`, `heading` (or `title`), `section`, `error`, `success`, `mono`, `display`, `small`,
/// `overline`, `caption`, `label`, `headline`.
/// @return The role; `nullopt` for any other name.
[[nodiscard]] std::optional<TextRole> roleNamed(std::string_view name);

/// @brief What a role that names none is told: the names `roleNamed` reads.
inline constexpr std::string_view kRoleNames =
    "a role is normal, muted, heading, section, error, success, mono, display, small, overline, caption, label or "
    "headline";

/// @brief A text role by name, as `roleNamed` reads it.
/// @param value  The name.
/// @param format Unused.
/// @return The role; `Normal` for any other name.
[[nodiscard]] TextRole roleOf(Value const& value, FormatContext const& format);

/// @brief A tone by name.
/// @param value  `neutral`, `info`, `ok` (or `success`), `warn` (or `warning`), `err` (or `error`).
/// @param format Unused.
/// @return The tone; `Neutral` for anything else.
[[nodiscard]] Tone toneOf(Value const& value, FormatContext const& format);

/// @brief Whether a scope declares any name, so a node in it reads names the screen's own scope lacks.
/// @param scope The scope.
/// @return True when it declares a model, state, let, query, mutation, watch or check.
[[nodiscard]] bool declaresNames(ScopeDecl const& scope) noexcept;

/// @brief What a span list with a run it drops is told.
inline constexpr std::string_view kSpansReason =
    "spans is a list of runs, each a value or a record with `text`; a list, a null or a record without `text` is "
    "dropped";

/// @brief What a tone of no known name is told.
inline constexpr std::string_view kToneNames = "a tone is neutral, info, ok, warn or err, or null for none";

/// @brief What a span list with a run of a tone of no known name is told.
inline constexpr std::string_view kSpanToneReason =
    "a run's tone is neutral, info, ok, warn or err, or null for none; a run of another tone is drawn untoned";

/// @brief A splitter's `sizes`: a list of numbers, each read as `intOf` reads it.
/// @param value The value.
/// @return The sizes; `nullopt` for anything but a list whose every item is an `int` or a decimal.
[[nodiscard]] std::optional<std::vector<int>> splitterSizesOf(Value const& value);

/// @brief An alignment by name, as the loader checked it: `start`, `center`, `end`, or stretch for none.
/// @param name The name; empty for none.
/// @return The alignment.
[[nodiscard]] Align alignOf(std::string_view name);

/// @brief A table cell's place by name: across its column (`align`) or down its row (`valign`).
/// @param name `start`, `center` or `end`.
/// @return The place; `nullopt` for any other name, `stretch` included.
[[nodiscard]] std::optional<Align> cellPlaceNamed(std::string_view name);

/// @brief A justification by name, as the loader checked it: `center`, `end`, `between`, or start for none.
/// @param name The name; empty for none.
/// @return The justification.
[[nodiscard]] Justify justifyOf(std::string_view name);

/// @brief An axis by name: `horizontal`, or vertical for anything else.
/// @param value The value.
/// @return The axis.
[[nodiscard]] Axis axisOf(Value const& value);

/// @brief A step's state by name: `active`, `done`, `failed`, or pending for anything else.
/// @param name The name.
/// @return The state.
[[nodiscard]] StepState stepStateOf(std::string_view name);

/// @brief The options of a select, from a list of scalars or of records with a key and a `label`.
/// @param options The list.
/// @param format  The formatting facts.
/// @return The options.
[[nodiscard]] std::vector<SelectOption> selectOptions(Value const& options, FormatContext const& format);

/// @brief What a percent sizes, as a refusal or a report says it.
inline constexpr std::string_view kPercentPlaces =
    "`percent` sizes a width along a row or a horizontal splitter; nothing here takes a share";

/// @brief Why a percent inside a case or an overlay's content is not a share.
inline constexpr std::string_view kPercentInHolder = "size the `switch` or the overlay itself; its content fills it";

/// @brief The default spacing step between the children of a document panel or collapsible, the mockups' 8 px.
inline constexpr int kPanelGap = 2;

/// @brief The default spacing step between the children of a document dialog or drawer, the mockups' 12 px.
inline constexpr int kDialogGap = 3;

/// @brief A node's size request.
/// @param node The node; may be null.
/// @return Its layout hints, or none for a null node or a kind that has no `Common`.
[[nodiscard]] LayoutHints layoutHintsOf(Node const& node);

/// @brief A node with its width sizing replaced; a node of a kind with no layout is returned as it is.
/// @param node  The node.
/// @param width The sizing.
/// @return The node.
[[nodiscard]] Node withWidth(Node const& node, Sizing width);

/// @brief The size request of a holder of @p sizings: a stretch by the largest weight where any is a stretch, and
///        content otherwise (a share of a row is not a holder's: `listColumn` passes a lone child's on).
/// @param sizings One dimension's sizing of each child.
/// @return The holder's sizing.
[[nodiscard]] Sizing holderSizing(std::span<Sizing const> sizings);

/// @brief Content whose scope is made when it mounts and released when it unmounts: a one-row `forEach` whose
///        row view builds it.
/// @param build Builds the content, its scope included.
/// @param hints The size request of the content. A slot reads the sizing of its direct child alone, which is this
///        `forEach`, so the content's stretch reaches the slot only through these.
/// @return The node.
[[nodiscard]] Node lazy(std::function<Node()> build, LayoutHints const& hints = {});

/// @brief A column of content that holds its scope for as long as it is mounted: its `visible` is a binding
///        that keeps the scope alive and reads nothing.
/// @param children The content.
/// @param scope    The scope.
/// @param keys     The chords of the content.
/// @param gap      The spacing step between the children.
/// @return The node.
[[nodiscard]] Node holding(std::vector<Node> children, std::shared_ptr<RtScope> scope,
                           std::vector<KeyBinding> keys = {}, int gap = 0);

/// @brief Whether an expression reads a name in @p names.
/// @param expr  The expression.
/// @param names The names.
/// @return Whether any form of the expression names one.
// NOLINTBEGIN(misc-no-recursion): an expression nests its arguments and fields, and the walk follows them.
[[nodiscard]] bool readsAny(ExprPtr const& expr, std::set<std::string, std::less<>> const& names);

// NOLINTEND(misc-no-recursion)

/// @brief Collects the element ids of a document subtree.
/// @param node The root.
/// @param out  Receives the ids.
// NOLINTNEXTLINE(misc-no-recursion): a node's children and parts are nodes, and the walk follows them.
void collectIds(DocNode const& node, std::set<std::string, std::less<>>& out);

/// @brief Where a declaration is lowered, as a focus key reads it.
struct FocusPlace {
    /// @brief The focus key of the component use it is lowered for, and `>`; empty outside a component.
    std::string prefix;
    /// @brief The keys of the rows that enclose it, outermost first.
    std::vector<std::string> rows;
};

/// @brief Where @p scope is: the row keys of its chain and the component use it is lowered for.
/// @param scope The scope.
/// @return It.
[[nodiscard]] FocusPlace focusPlace(RtScope const& scope);

/// @brief A focus key: a declaration's pointer under its component use's prefix, with one key per enclosing row.
/// @param place Where the declaration is lowered.
/// @param path  The declaring node's JSON Pointer.
/// @return The key, unique per declaration, row and component use.
[[nodiscard]] std::string focusKey(FocusPlace const& place, std::string_view path);

/// @brief The focus key of the element @p path declares, lowered in @p scope.
/// @param path  The declaring node's JSON Pointer.
/// @param scope The scope it is lowered in.
/// @return The key.
[[nodiscard]] std::string focusKeyOf(std::string_view path, RtScope const& scope);

/// @brief A lowering context whose scope is held weakly: what an Effect the scope itself owns may keep, so the scope
///        does not keep itself alive.
struct DetachedLower {
    /// @brief The scope, weakly.
    std::weak_ptr<RtScope> scope;
    /// @brief The names bound there.
    std::shared_ptr<Bindings const> bindings;
    /// @brief The innermost boundary.
    std::shared_ptr<BoundaryRuntime> boundary;
};

/// @brief Detaches a lowering context's scope.
/// @param ctx The context.
/// @return The context, its scope held weakly.
[[nodiscard]] DetachedLower detachedLower(LowerCtx const& ctx);

/// @brief Puts a detached context's scope back.
/// @param held The detached context.
/// @return The context; its scope is null once the scope is gone.
[[nodiscard]] LowerCtx attached(DetachedLower const& held);

// NOLINTNEXTLINE(readability-function-cognitive-complexity): a list of rows and a table.
/// The tooltip of a cell whose value cannot be read as its column's kind.
inline constexpr std::string_view kUnreadable = "The value could not be read";

/// Whether a column part gives any of a column's display parts.
[[nodiscard]] bool givesDisplay(DocNode const& part);

/// What a model-backed table's cell reads beside its column: the document's own tone and tooltip expressions.
struct CellExprs {
    ExprPtr tone;
    ExprPtr tooltip;
    std::string path;
};

/// @brief What a table's expressions read of its row: the fields named (`{"row": "id"}`), and whether the row is
///        read whole (`{"row": ""}` passed on as a value). A path read out of the whole row with `get` is neither:
///        it is the row's own read, which a binding names at run time.
struct RowReads {
    std::set<std::string, std::less<>> ids;
    bool whole = false;
};

// NOLINTNEXTLINE(misc-no-recursion): one level per nested expression.
void rowReadsOf(Expr const& expr, RowReads& out, bool readFrom = false);

// NOLINTNEXTLINE(misc-no-recursion): one level per nested command list.
void rowReadsOf(Commands const& commands, RowReads& out);

// NOLINTNEXTLINE(misc-no-recursion): one level per nested node.
void rowReadsOf(DocNode const& node, RowReads& out);

/// @brief The status line's words for a level: its id with the first letter raised.
/// @param level The level's id.
/// @return The words.
[[nodiscard]] std::string levelWords(std::string level);

/// @brief A count of rows with its thousands grouped by commas, as a diagnostic writes it.
/// @param count The count.
/// @return The text.
[[nodiscard]] std::string groupedCount(std::size_t count);

/// @brief The most rows, and the most cells, a table over `rows` orders on the client without a warning.
inline constexpr std::size_t kClientRows = 20'000;

inline constexpr std::size_t kClientCells = 1'000'000;

/// @brief The id of the status line's "Show only matching rows", which drops the retained rows.
inline constexpr std::string_view kShowMatching = "retained.showMatching";

/// @brief The ids of the status line's "Select them", which selects the rows an action could not change, and "Clear
///        marks", which drops their marks.
inline constexpr std::string_view kSelectFailed = "failed.select";

inline constexpr std::string_view kClearMarks = "failed.clear";

/// The description's columns a model-backed table's `Table::columns` are, in order: those its column parts name, or
/// without parts every column but the companions, which travel with their columns and are never one of them.
[[nodiscard]] std::vector<table::TableColumn const*> arrangedColumns(DocNode const& node,
                                                                     table::TableDescription const& description);

std::string summaryTestId(Value const& record, std::string const& column);

}  // namespace morph::ui::detail

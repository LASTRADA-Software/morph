// SPDX-License-Identifier: Apache-2.0

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <expected>
#include <format>
#include <functional>
#include <initializer_list>
#include <limits>
#include <map>
#include <memory>
#include <morph/core/wire.hpp>
#include <morph/forms/engine/field_model.hpp>
#include <morph/forms/version.hpp>
#include <morph/table/bands.hpp>
#include <morph/table/filter.hpp>
#include <morph/table/query.hpp>
#include <morph/ui/control_shape.hpp>
#include <morph/ui/date_pattern.hpp>
#include <morph/ui/document.hpp>
#include <morph/ui/expression.hpp>
#include <morph/ui/page_layout.hpp>
#include <morph/ui/shell_parts.hpp>
#include <morph/ui/value.hpp>
#include <optional>
#include <ranges>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace morph::ui {

std::function<std::optional<std::string>(std::string_view)> documentLookup(
    std::map<std::string, std::string, std::less<>> documents) {
    return [known = std::make_shared<std::map<std::string, std::string, std::less<>> const>(std::move(documents))](
               std::string_view actionId) -> std::optional<std::string> {
        auto const found = known->find(actionId);
        if (found == known->end()) {
            return std::nullopt;
        }
        return found->second;
    };
}

namespace detail {

namespace {

/// @brief The exception the loader throws inside itself; `loadDocument` turns it into its result.
class DocumentRefused : public std::runtime_error {
public:
    /// @brief Wraps a refusal.
    /// @param refusal The refusal.
    explicit DocumentRefused(Refusal refusal) : std::runtime_error{refusal.reason}, _refusal{std::move(refusal)} {}

    /// @brief The refusal.
    /// @return It.
    [[nodiscard]] Refusal const& refusal() const noexcept { return _refusal; }

private:
    Refusal _refusal;
};

/// @brief Refuses the document.
/// @param path   Where.
/// @param reason Why.
[[noreturn]] void refuse(std::string const& path, std::string reason) {
    throw DocumentRefused{Refusal{.document = {}, .path = path, .reason = std::move(reason)}};
}

// NOLINTBEGIN(modernize-use-designated-initializers,readability-trailing-comma): the kind table reads as rows; names on every field would not. A trailing comma would put every row member on a line of its own.
/// @brief The keys every node takes.
constexpr std::array<KeySpec, 19> kCommonKeys = {{
    {"id", Shape::Name},           {"visible", Shape::Condition}, {"enabled", Shape::Condition},
    {"tooltip", Shape::Expr},      {"sizing", Shape::Expr},       {"minWidth", Shape::Expr},
    {"maxWidth", Shape::Expr},     {"minHeight", Shape::Expr},    {"maxHeight", Shape::Expr},
    {"alignSelf", Shape::Text},    {"bleed", Shape::Flag},        {"span", Shape::Expr},
    {"testId", Shape::Text},       {"surface", Shape::Expr},      {"autofocus", Shape::Expr},
    {"a11y", Shape::Part, "a11y"}, {"drag", Shape::Part, "drag"}, {"drop", Shape::Part, "drop"},
    {"keys", Shape::Keys},
}};

/// @brief The input decorations text and the inputs take; a text refuses `description` and `labelHint`.
constexpr std::array<KeySpec, 7> kFieldKeys = {{
    {"readonly", Shape::Condition},
    {"required", Shape::Condition},
    {"errors", Shape::Expr},
    {"stale", Shape::Condition},
    {"description", Shape::Expr},
    {"labelHint", Shape::Expr},
    {"tone", Shape::Expr},
}};

/// @brief The element values every element has.
constexpr std::array<std::string_view, 5> kAnyValues = {"visible", "enabled", "valid", "errors", "shownErrors"};
/// @brief The element values of a text, date-time or slider input.
constexpr std::array<std::string_view, 5> kInputValues = {"value", "draft", "dirty", "focused", "touched"};
/// @brief The element values of a checkbox.
constexpr std::array<std::string_view, 1> kCheckboxValues = {"checked"};
/// @brief The element values of a select.
constexpr std::array<std::string_view, 2> kSelectValues = {"selected", "selectedOption"};
/// @brief The element values of a tabs node: the id of the tab shown.
constexpr std::array<std::string_view, 1> kTabsValues = {"selected"};
/// @brief The element values of a table.
constexpr std::array<std::string_view, 24> kTableValues = {
    "selection",         "activeKey", "sort",          "filters",        "viewCount",   "sourceCount",
    "selectedCount",     "pending",   "error",         "hoveredKey",     "page",        "pageCount",
    "pageFirst",         "pageLast",  "layout",        "visibleColumns", "hiddenCount", "selectionSet",
    "selectedCountKind", "busyCount", "modifiedCount", "modifiedRows",   "errorCount",  "editing"};
/// @brief The element values of a dialog or drawer.
constexpr std::array<std::string_view, 3> kDialogValues = {"open", "ready", "submitted"};
/// @brief The element values of a form, beside its fields.
constexpr std::array<std::string_view, 3> kFormValues = {"ready", "body", "submitted"};
/// @brief The values of a transfer id.
constexpr std::array<std::string_view, 3> kTransferValues = {"progress", "pending", "error"};

/// @brief The keys of `text`.
constexpr std::array<KeySpec, 4> kTextKeys = {
    {{"text", Shape::Expr}, {"spans", Shape::Expr}, {"role", Shape::Expr}, {"align", Shape::Text}}};
/// @brief The keys of `button`.
constexpr std::array<KeySpec, 12> kButtonKeys = {{{"label", Shape::Expr},
                                                  {"icon", Shape::Expr},
                                                  {"emphasis", Shape::Text},
                                                  {"style", Shape::Text},
                                                  {"selected", Shape::Condition},
                                                  {"onClick", Shape::Commands},
                                                  {"onActivate", Shape::Commands},
                                                  {"children", Shape::Nodes},
                                                  {"shape", Shape::Text},
                                                  {"size", Shape::Text},
                                                  {"mono", Shape::Flag},
                                                  {"weight", Shape::Text}}};
/// @brief The keys of `textInput`.
constexpr std::array<KeySpec, 17> kTextInputKeys = {{{"value", Shape::Expr},
                                                     {"placeholder", Shape::Expr},
                                                     {"mode", Shape::Expr},
                                                     {"icon", Shape::Expr},
                                                     {"unit", Shape::Expr},
                                                     {"decimals", Shape::Expr},
                                                     {"prefix", Shape::Expr},
                                                     {"emphasis", Shape::Text},
                                                     {"mono", Shape::Flag},
                                                     {"wrap", Shape::Flag},
                                                     {"align", Shape::Text},
                                                     {"commitOn", Shape::Text},
                                                     {"label", Shape::Expr},
                                                     {"showLabel", Shape::Flag},
                                                     {"onChange", Shape::Commands},
                                                     {"onCommit", Shape::Commands},
                                                     {"onSubmit", Shape::Commands}}};
/// @brief The keys of `checkbox`.
constexpr std::array<KeySpec, 4> kCheckboxKeys = {
    {{"label", Shape::Expr}, {"checked", Shape::Expr}, {"onToggle", Shape::Commands}, {"onChange", Shape::Commands}}};
/// @brief The keys of `select`.
constexpr std::array<KeySpec, 8> kSelectKeys = {{{"options", Shape::Expr},
                                                 {"selected", Shape::Expr},
                                                 {"style", Shape::Expr},
                                                 {"label", Shape::Expr},
                                                 {"showLabel", Shape::Flag},
                                                 {"onStale", Shape::Text},
                                                 {"onChoose", Shape::Commands},
                                                 {"onChange", Shape::Commands}}};
/// @brief The keys of `menu`; `label`, `icon` and `emphasis` are a button-style menu's trigger.
constexpr std::array<KeySpec, 5> kMenuKeys = {{{"items", Shape::Parts, "menuItem"},
                                               {"style", Shape::Text},
                                               {"label", Shape::Expr},
                                               {"icon", Shape::Expr},
                                               {"emphasis", Shape::Text}}};
/// @brief The keys of a menu item part.
constexpr std::array<KeySpec, 10> kMenuItemKeys = {{{"label", Shape::Expr},
                                                    {"icon", Shape::Expr},
                                                    {"keys", Shape::Text},
                                                    {"checked", Shape::Expr},
                                                    {"enabled", Shape::Condition},
                                                    {"onClick", Shape::Commands},
                                                    {"items", Shape::Parts, "menuItem"},
                                                    {"separator", Shape::Flag},
                                                    {"description", Shape::Expr},
                                                    {"visible", Shape::Condition}}};
/// @brief The keys of a `headMenuItem` part: an entry of a screen head's own menu, a menu item with the `id` that
///        `ScreenHead::choose` takes; its submenu's entries are head menu items too.
constexpr std::array<KeySpec, 11> kHeadMenuItemKeys = {{{"id", Shape::Name},
                                                        {"label", Shape::Expr},
                                                        {"icon", Shape::Expr},
                                                        {"keys", Shape::Text},
                                                        {"checked", Shape::Expr},
                                                        {"enabled", Shape::Condition},
                                                        {"onClick", Shape::Commands},
                                                        {"items", Shape::Parts, "headMenuItem"},
                                                        {"separator", Shape::Flag},
                                                        {"description", Shape::Expr},
                                                        {"visible", Shape::Condition}}};
/// @brief The keys of a `headTool` part: a search field or a select of a screen's head.
constexpr std::array<KeySpec, 14> kHeadToolKeys = {{{"id", Shape::Name, {}, kRequired},
                                                    {"kind", Shape::Text, {}, kRequired},
                                                    {"label", Shape::Expr, {}, kRequired},
                                                    {"value", Shape::Expr},
                                                    {"placeholder", Shape::Expr},
                                                    {"onChange", Shape::Commands},
                                                    {"onSubmit", Shape::Commands},
                                                    {"options", Shape::Expr},
                                                    {"selected", Shape::Expr},
                                                    {"style", Shape::Text},
                                                    {"onChoose", Shape::Commands},
                                                    {"width", Shape::Expr},
                                                    {"visible", Shape::Condition},
                                                    {"enabled", Shape::Condition}}};
/// @brief The keys of `column` and `row`; `wrap` is a row's, read on a column only to refuse it, and `divided` a
///        column's or a row's that does not wrap.
constexpr std::array<KeySpec, 10> kContainerKeys = {{{"children", Shape::Nodes},
                                                     {"gap", Shape::Expr},
                                                     {"padding", Shape::Expr},
                                                     {"align", Shape::Text},
                                                     {"justify", Shape::Text},
                                                     {"wrap", Shape::Flag},
                                                     {"divided", Shape::Flag},
                                                     {"pageEdge", Shape::Flag},
                                                     {"headJoined", Shape::Flag},
                                                     {"selected", Shape::Condition}}};
/// @brief The keys of `grid`; `justify` and `selected` are read only to refuse them.
constexpr std::array<KeySpec, 8> kGridKeys = {{{"children", Shape::Nodes},
                                               {"gap", Shape::Expr},
                                               {"columns", Shape::Expr},
                                               {"minColumnWidth", Shape::Expr},
                                               {"tracks", Shape::Parts, "track"},
                                               {"align", Shape::Text},
                                               {"justify", Shape::Text},
                                               {"selected", Shape::Condition}}};
/// @brief The keys of a `grid` track part.
constexpr std::array<KeySpec, 2> kTrackKeys = {{{"width", Shape::Expr}, {"align", Shape::Text}}};
/// @brief The keys of `panel`.
constexpr std::array<KeySpec, 8> kPanelKeys = {{{"title", Shape::Expr},
                                                {"gap", Shape::Expr},
                                                {"header", Shape::Nodes},
                                                {"padding", Shape::Expr},
                                                {"collapsible", Shape::Expr},
                                                {"collapsed", Shape::Expr},
                                                {"onToggle", Shape::Commands},
                                                {"children", Shape::Nodes}}};
/// @brief The keys of `scroll`.
constexpr std::array<KeySpec, 2> kScrollKeys = {{{"axis", Shape::Expr}, {"children", Shape::Nodes}}};
/// @brief The keys of `switch`.
constexpr std::array<KeySpec, 3> kSwitchKeys = {
    {{"on", Shape::Expr, {}, kRequired}, {"cases", Shape::Parts, "case"}, {"default", Shape::Nodes}}};
/// @brief The keys of a `switch` case part.
constexpr std::array<KeySpec, 2> kCaseKeys = {
    {{"when", Shape::Expr, {}, kRequired}, {"children", Shape::Nodes, {}, kInner}}};
/// @brief The keys of `tabs`.
constexpr std::array<KeySpec, 7> kTabsKeys = {{{"tabs", Shape::Parts, "tab"},
                                               {"selected", Shape::Expr},
                                               {"onChoose", Shape::Commands},
                                               {"moreLabel", Shape::Expr},
                                               {"inset", Shape::Expr},
                                               {"pageInset", Shape::Expr},
                                               {"stripSurface", Shape::Text}}};
/// @brief The keys of a `tabs` page part.
constexpr std::array<KeySpec, 3> kTabKeys = {
    {{"id", Shape::Name}, {"label", Shape::Expr}, {"children", Shape::Nodes}}};
/// @brief The keys of `dialog` and `drawer`.
constexpr std::array<KeySpec, 17> kDialogKeys = {{{"open", Shape::Condition},
                                                  {"gap", Shape::Expr},
                                                  {"title", Shape::Expr},
                                                  {"description", Shape::Expr},
                                                  {"showErrors", Shape::Text},
                                                  {"children", Shape::Nodes, {}, kInner},
                                                  {"onDismiss", Shape::Commands},
                                                  {"default", Shape::Commands, {}, kInner},
                                                  {"cancel", Shape::Commands, {}, kInner},
                                                  {"actions", Shape::Parts, "actionButton", kInner},
                                                  {"side", Shape::Expr},
                                                  {"sheet", Shape::Condition},
                                                  {"dismissLabel", Shape::Expr},
                                                  {"descriptionSpans", Shape::Expr},
                                                  {"headNote", Shape::Expr},
                                                  {"footerNote", Shape::Expr},
                                                  {"closeButton", Shape::Condition}}};
/// @brief The keys of `group`: a labelled set of inputs, lowered to a column of existing kinds.
constexpr std::array<KeySpec, 6> kGroupKeys = {{{"label", Shape::Expr},
                                                {"required", Shape::Condition},
                                                {"description", Shape::Expr},
                                                {"errors", Shape::Expr},
                                                {"gap", Shape::Expr},
                                                {"children", Shape::Nodes}}};
/// @brief The keys of `busy`.
constexpr std::array<KeySpec, 2> kBusyKeys = {{{"active", Shape::Condition}, {"label", Shape::Expr}}};
/// @brief The keys of `forEach`.
constexpr std::array<KeySpec, 7> kForEachKeys = {{{"rows", Shape::Expr, {}, kRequired},
                                                  {"key", Shape::Expr, {}, kInner},
                                                  {"axis", Shape::Expr},
                                                  {"gap", Shape::Expr},
                                                  {"wrap", Shape::Flag},
                                                  {"divided", Shape::Flag},
                                                  {"children", Shape::Nodes, {}, kInner}}};
/// @brief The keys of `table`.
constexpr auto kTableKeys = std::to_array<KeySpec>({{"rows", Shape::Expr},
                                                    {"form", Shape::Text},
                                                    {"field", Shape::Text},
                                                    {"enterMoves", Shape::Text},
                                                    {"walk", Shape::Text},
                                                    {"editMode", Shape::Text},
                                                    {"appendRows", Shape::Flag},
                                                    {"rowsFixed", Shape::Flag},
                                                    {"key", Shape::Expr, {}, kInner},
                                                    {"model", Shape::Name},
                                                    {"table", Shape::Name},
                                                    {"action", Shape::Text},
                                                    {"status", Shape::Flag},
                                                    {"retainChanged", Shape::Flag},
                                                    {"persist", Shape::Expr},
                                                    {"body", Shape::Expr},
                                                    {"mode", Shape::Expr},
                                                    {"columns", Shape::PartsOrExpr, "column"},
                                                    {"showHeader", Shape::Flag},
                                                    {"virtualizeThreshold", Shape::Expr},
                                                    {"virtualizeOverscan", Shape::Expr},
                                                    {"selectionMode", Shape::Expr},
                                                    {"selection", Shape::Expr},
                                                    {"onSelectionChange", Shape::Commands},
                                                    {"selectColumn", Shape::Flag},
                                                    {"selectAllLabel", Shape::Expr},
                                                    {"rowLabel", Shape::Expr, {}, kInner},
                                                    {"sort", Shape::Expr},
                                                    {"onSortChange", Shape::Commands},
                                                    {"maxSortKeys", Shape::Expr},
                                                    {"layout", Shape::Expr},
                                                    {"onLayoutChange", Shape::Commands},
                                                    {"configurable", Shape::Expr},
                                                    {"onFiltersChange", Shape::Commands},
                                                    {"pageSize", Shape::Expr},
                                                    {"page", Shape::Expr},
                                                    {"group", Shape::Part, "group"},
                                                    {"groups", Shape::Parts, "columnGroup"},
                                                    {"filters", Shape::Expr},
                                                    {"rowTone", Shape::Expr, {}, kInner},
                                                    {"rowSpans", Shape::Expr, {}, kInner},
                                                    {"highlightKey", Shape::Expr},
                                                    {"onActivate", Shape::Commands, {}, kInner},
                                                    {"valign", Shape::Text},
                                                    {"empty", Shape::Nodes},
                                                    {"cellMenu", Shape::Parts, "menuItem", kInner},
                                                    {"headerMenu", Shape::Parts, "menuItem"},
                                                    {"headerEnabled", Shape::Condition},
                                                    {"bands", Shape::Parts, "band"},
                                                    {"summaries", Shape::Parts, "summary"},
                                                    {"layoutRows", Shape::Parts, "layoutRow"}});
/// @brief The keys of a `table` column part; `tone` and `tooltip` are read in the row's scope.
constexpr std::array<KeySpec, 29> kColumnKeys = {{{"id", Shape::Name},
                                                  {"field", Shape::Text},
                                                  {"value", Shape::Expr, {}, kInner},
                                                  {"kind", Shape::Text},
                                                  {"label", Shape::Expr},
                                                  {"width", Shape::Expr},
                                                  {"role", Shape::Expr},
                                                  {"align", Shape::Text},
                                                  {"valign", Shape::Text},
                                                  {"sortable", Shape::Flag},
                                                  {"locked", Shape::Flag},
                                                  {"filterable", Shape::Flag},
                                                  {"wrap", Shape::Flag},
                                                  {"shrink", Shape::Flag},
                                                  {"sortBy", Shape::Expr, {}, kInner},
                                                  {"cell", Shape::Nodes, {}, kInner},
                                                  {"decimals", Shape::Expr},
                                                  {"rounding", Shape::Text},
                                                  {"padPlaces", Shape::Flag},
                                                  {"grouping", Shape::Flag},
                                                  {"empty", Shape::Text},
                                                  {"unit", Shape::Text},
                                                  {"date", Shape::Text},
                                                  {"labels", Shape::Expr},
                                                  {"tone", Shape::Expr, {}, kInner},
                                                  {"tooltip", Shape::Expr, {}, kInner},
                                                  {"readonly", Shape::Expr},
                                                  {"options", Shape::Expr},
                                                  {"placeholder", Shape::Expr}}};
/// @brief The keys of a form field overlay's `grid` part: the keys a `table` over a `form` takes beyond its `form` and
///        `field`, its columns, walk and activation, its layout, menus and filters.
constexpr auto kGridPartKeys = std::to_array<KeySpec>({{"columns", Shape::Parts, "column"},
                                                       {"enterMoves", Shape::Text},
                                                       {"walk", Shape::Text},
                                                       {"appendRows", Shape::Flag},
                                                       {"rowsFixed", Shape::Flag},
                                                       {"onActivate", Shape::Commands},
                                                       {"layout", Shape::Expr},
                                                       {"onLayoutChange", Shape::Commands},
                                                       {"configurable", Shape::Expr},
                                                       {"onFiltersChange", Shape::Commands},
                                                       {"cellMenu", Shape::Parts, "menuItem", kInner},
                                                       {"headerMenu", Shape::Parts, "menuItem"},
                                                       {"headerEnabled", Shape::Condition},
                                                       {"testId", Shape::Text},
                                                       {"bands", Shape::Parts, "band"},
                                                       {"summaries", Shape::Parts, "summary"}});
/// @brief The keys of a `table` column group part: the group's id, its label and its columns' ids, a literal list.
constexpr std::array<KeySpec, 3> kColumnGroupKeys = {
    {{"id", Shape::Name, {}, kRequired}, {"label", Shape::Expr}, {"columns", Shape::Expr, {}, kRequired}}};
/// @brief The keys of a fixed grid's `layoutRows` part: a row, `{key, label, cells}`, or a block, `{forEach, rows}`,
///        whose rows repeat for each row of the form's collection `forEach` names.
constexpr auto kLayoutRowKeys = std::to_array<KeySpec>({{"key", Shape::Text},
                                                        {"label", Shape::Expr},
                                                        {"cells", Shape::ExprRecord, "layoutCells"},
                                                        {"forEach", Shape::Text},
                                                        {"rows", Shape::Parts, "layoutRow"}});
/// @brief The keys of a fixed grid's cell part, by the column id it is given under: the form field it edits, the
///        expression or the text it shows, the rows and columns it spans, and its overlays, a grid column's.
constexpr auto kLayoutCellKeys = std::to_array<KeySpec>({{"field", Shape::Text},
                                                         {"value", Shape::Expr},
                                                         {"text", Shape::Text},
                                                         {"rows", Shape::Expr},
                                                         {"columns", Shape::Expr},
                                                         {"decimals", Shape::Expr},
                                                         {"unit", Shape::Expr},
                                                         {"readonly", Shape::Expr},
                                                         {"options", Shape::Expr},
                                                         {"placeholder", Shape::Expr}});
/// @brief The keys of a `table` group part: the value its rows are grouped by, and the heading drawn before each
///        group, both read in the row scope, and the summary rows drawn after each group.
constexpr auto kTableGroupKeys = std::to_array<KeySpec>({{"by", Shape::Expr, {}, kInner | kRequired},
                                                         {"heading", Shape::Nodes, {}, kInner | kRequired},
                                                         {"summaries", Shape::Parts, "summary"}});
/// @brief The keys of a header band part: its text, its unit, its columns' ids (a literal list) and the bands one
///        level down.
constexpr auto kBandKeys = std::to_array<KeySpec>({{"label", Shape::Text},
                                                   {"unit", Shape::Text},
                                                   {"columns", Shape::Expr, {}, kRequired},
                                                   {"bands", Shape::Parts, "band"}});
/// @brief The keys of a summary row part: its key, its label, where it is drawn, and its cells' expressions by column
///        id, read in a scope with `rows` and `column`.
constexpr auto kSummaryKeys = std::to_array<KeySpec>({{"key", Shape::Text, {}, kRequired},
                                                      {"label", Shape::Expr},
                                                      {"position", Shape::Text},
                                                      {"cells", Shape::ExprRecord}});
/// @brief The keys of `dateTimeInput`.
constexpr std::array<KeySpec, 6> kDateTimeKeys = {{{"value", Shape::Expr},
                                                   {"mode", Shape::Expr},
                                                   {"label", Shape::Expr},
                                                   {"showLabel", Shape::Flag},
                                                   {"onChange", Shape::Commands},
                                                   {"onCommit", Shape::Commands}}};
/// @brief The keys of `slider`.
constexpr std::array<KeySpec, 7> kSliderKeys = {{{"value", Shape::Expr},
                                                 {"min", Shape::Expr},
                                                 {"max", Shape::Expr},
                                                 {"step", Shape::Expr},
                                                 {"label", Shape::Expr},
                                                 {"onChange", Shape::Commands},
                                                 {"onCommit", Shape::Commands}}};
/// @brief The keys of `filePicker`.
constexpr std::array<KeySpec, 5> kFilePickerKeys = {{{"accept", Shape::Expr},
                                                     {"multiple", Shape::Expr},
                                                     {"mode", Shape::Expr},
                                                     {"label", Shape::Expr},
                                                     {"onPick", Shape::Commands}}};
/// @brief The keys of `banner`.
constexpr std::array<KeySpec, 8> kBannerKeys = {{{"tone", Shape::Expr},
                                                 {"text", Shape::Expr},
                                                 {"spans", Shape::Expr},
                                                 {"dismissible", Shape::Expr},
                                                 {"action", Shape::Part, "action"},
                                                 {"actionStyle", Shape::Text},
                                                 {"actionStretch", Shape::Flag},
                                                 {"onDismiss", Shape::Commands}}};
/// @brief The keys of an `action` part.
constexpr std::array<KeySpec, 2> kActionKeys = {{{"label", Shape::Expr}, {"onClick", Shape::Commands}}};
/// @brief The keys of an `actionButton` part: a button a screen's head or an overlay's footer shows.
constexpr std::array<KeySpec, 8> kActionButtonKeys = {{{"id", Shape::Name, {}, kRequired},
                                                       {"label", Shape::Expr, {}, kRequired},
                                                       {"icon", Shape::Expr},
                                                       {"iconOnly", Shape::Condition},
                                                       {"emphasis", Shape::Text},
                                                       {"enabled", Shape::Condition},
                                                       {"visible", Shape::Condition},
                                                       {"onClick", Shape::Commands}}};
/// @brief The keys of a `headBadge` part: a status badge of a screen's head.
constexpr std::array<KeySpec, 4> kHeadBadgeKeys = {
    {{"tone", Shape::Expr}, {"text", Shape::Expr}, {"icon", Shape::Expr}, {"visible", Shape::Condition}}};
/// @brief The keys of a `navigationBadge` part: a count the app document puts on a navigator entry. `screen` is the
///        screen id the entry opens, a literal; the rest are read in the app scope.
constexpr std::array<KeySpec, 4> kNavigationBadgeKeys = {{{"screen", Shape::Text, {}, kRequired},
                                                          {"text", Shape::Expr},
                                                          {"tone", Shape::Expr},
                                                          {"visible", Shape::Condition}}};
/// @brief The keys of `badge`.
constexpr std::array<KeySpec, 8> kBadgeKeys = {{{"tone", Shape::Expr},
                                                {"text", Shape::Expr},
                                                {"icon", Shape::Expr},
                                                {"style", Shape::Text},
                                                {"onRemove", Shape::Commands},
                                                {"removeLabel", Shape::Expr},
                                                {"shape", Shape::Text},
                                                {"mono", Shape::Flag}}};
/// @brief The keys of `progress`.
constexpr std::array<KeySpec, 2> kProgressKeys = {{{"value", Shape::Expr}, {"label", Shape::Expr}}};
/// @brief The keys of `steps`.
constexpr std::array<KeySpec, 3> kStepsKeys = {
    {{"items", Shape::Expr}, {"current", Shape::Expr}, {"labelled", Shape::Condition}}};
/// @brief The keys of `keyValue`.
constexpr std::array<KeySpec, 3> kKeyValueKeys = {
    {{"items", Shape::Expr}, {"fieldLayout", Shape::Text}, {"captionWidth", Shape::Expr}}};
/// @brief The keys of `emptyState`.
constexpr std::array<KeySpec, 4> kEmptyStateKeys = {
    {{"title", Shape::Expr}, {"text", Shape::Expr}, {"icon", Shape::Expr}, {"action", Shape::Part, "action"}}};
/// @brief The keys of `chart`.
constexpr std::array<KeySpec, 7> kChartKeys = {{{"summary", Shape::Expr, {}, kRequired},
                                                {"series", Shape::Parts, "series", kRequired},
                                                {"lines", Shape::Parts, "line"},
                                                {"yMin", Shape::Expr},
                                                {"yMax", Shape::Expr},
                                                {"yPlaces", Shape::Expr},
                                                {"yLabel", Shape::Expr}}};
/// @brief The keys of a chart's `series` part. The keys read per point are evaluated in the part's row scope, with
///        `row` the point's element.
constexpr std::array<KeySpec, 9> kSeriesKeys = {{{"points", Shape::Expr, {}, kRequired},
                                                 {"y", Shape::Expr, {}, kRequired | kInner},
                                                 {"x", Shape::Expr, {}, kInner},
                                                 {"xLabel", Shape::Expr, {}, kInner},
                                                 {"tone", Shape::Expr, {}, kInner},
                                                 {"note", Shape::Expr, {}, kInner},
                                                 {"label", Shape::Expr},
                                                 {"joined", Shape::Flag},
                                                 {"style", Shape::Text}}};
/// @brief The keys of a chart's reference `line` part.
constexpr std::array<KeySpec, 4> kLineKeys = {
    {{"value", Shape::Expr, {}, kRequired}, {"label", Shape::Expr}, {"tone", Shape::Expr}, {"style", Shape::Text}}};
/// @brief The keys of `splitter`.
constexpr std::array<KeySpec, 3> kSplitterKeys = {
    {{"orientation", Shape::Expr}, {"sizes", Shape::Expr}, {"children", Shape::Nodes}}};
/// @brief The keys of `collapsible`.
constexpr std::array<KeySpec, 6> kCollapsibleKeys = {{{"title", Shape::Expr},
                                                      {"gap", Shape::Expr},
                                                      {"open", Shape::Condition},
                                                      {"header", Shape::Nodes},
                                                      {"children", Shape::Nodes},
                                                      {"onToggle", Shape::Commands}}};
/// @brief The keys of `actionBar`; `gap` is read only to refuse it: its gap is the theme's.
constexpr std::array<KeySpec, 5> kActionBarKeys = {{{"children", Shape::Nodes},
                                                    {"align", Shape::Text},
                                                    {"justify", Shape::Text},
                                                    {"gap", Shape::Expr},
                                                    {"wrap", Shape::Flag}}};
/// @brief The keys of `dropZone`.
constexpr std::array<KeySpec, 4> kDropZoneKeys = {
    {{"accept", Shape::Expr}, {"multiple", Shape::Expr}, {"children", Shape::Nodes}, {"onDrop", Shape::Commands}}};
/// @brief The keys of `page`: its sheet's size and margins as literal micrometres.
constexpr std::array<KeySpec, 4> kPageKeys = {{{"width", Shape::Expr, {}, kRequired},
                                               {"height", Shape::Expr, {}, kRequired},
                                               {"margins", Shape::Expr},
                                               {"children", Shape::Nodes}}};
/// @brief The keys of `pageRegion`: its anchor and place as literal micrometres.
constexpr std::array<KeySpec, 6> kPageRegionKeys = {{{"anchor", Shape::Text, {}, kRequired},
                                                     {"x", Shape::Expr},
                                                     {"y", Shape::Expr},
                                                     {"width", Shape::Expr},
                                                     {"height", Shape::Expr},
                                                     {"children", Shape::Nodes}}};
/// @brief The keys of `localeScope`.
constexpr std::array<KeySpec, 2> kLocaleScopeKeys = {
    {{"locale", Shape::Expr, {}, kRequired}, {"children", Shape::Nodes}}};
/// @brief The keys of `customize`.
constexpr std::array<KeySpec, 1> kCustomizeKeys = {{{"point", Shape::Name, {}, kRequired}}};
/// @brief The keys of `boundary`.
constexpr std::array<KeySpec, 3> kBoundaryKeys = {
    {{"loading", Shape::Nodes}, {"error", Shape::Nodes, {}, kBoundaryError}, {"children", Shape::Nodes}}};
/// @brief The keys of `form`.
constexpr std::array<KeySpec, 18> kFormKeys = {{{"model", Shape::Name, {}, kRequired},
                                                {"action", Shape::Text, {}, kRequired},
                                                {"prefill", Shape::Expr},
                                                {"key", Shape::Expr},
                                                {"submit", Shape::Text},
                                                {"showErrors", Shape::Text},
                                                {"fields", Shape::ExprRecord, "fields"},
                                                {"onSubmit", Shape::Commands},
                                                {"onSuccess", Shape::Commands},
                                                {"onError", Shape::Commands},
                                                {"title", Shape::Expr},
                                                {"submitLabel", Shape::Expr},
                                                {"showStatus", Shape::Flag},
                                                {"columns", Shape::Expr},
                                                {"fieldLayout", Shape::Text},
                                                {"captionWidth", Shape::Expr},
                                                {"derive", Shape::Text},
                                                {"deriveDebounce", Shape::Expr}}};
/// @brief The keys of `collection` and `wizard`: the model and action, and the presentation keys that reach every
///        form the view draws.
constexpr std::array<KeySpec, 8> kCollectionKeys = {{{"model", Shape::Name, {}, kRequired},
                                                     {"action", Shape::Text, {}, kRequired},
                                                     {"title", Shape::Expr},
                                                     {"submitLabel", Shape::Expr},
                                                     {"showStatus", Shape::Flag},
                                                     {"columns", Shape::Expr},
                                                     {"fieldLayout", Shape::Text},
                                                     {"captionWidth", Shape::Expr}}};
/// @brief The keys of `custom`.
constexpr std::array<KeySpec, 3> kCustomKeys = {
    {{"name", Shape::Text, {}, kRequired}, {"props", Shape::ExprRecord}, {"on", Shape::CommandRecord}}};
/// @brief The keys of an `a11y` part.
constexpr std::array<KeySpec, 2> kA11yKeys = {{{"name", Shape::Expr}, {"role", Shape::Text}}};
/// @brief The keys of a `drag` part.
constexpr std::array<KeySpec, 1> kDragKeys = {{{"payload", Shape::Expr, {}, kRequired}}};
/// @brief The keys of a `drop` part.
constexpr std::array<KeySpec, 2> kDropKeys = {{{"accepts", Shape::Expr}, {"onDrop", Shape::Commands}}};

/// @brief The view node kinds of `ui/1` and the part kinds inside them.
constexpr auto kKinds = std::to_array<KindSpec>({
    {"text", kTextKeys, kField},
    {"button", kButtonKeys},
    {"textInput", kTextInputKeys, kField | kWritable, kInputValues},
    {"checkbox", kCheckboxKeys, kField | kWritable, kCheckboxValues},
    {"select", kSelectKeys, kField | kWritable, kSelectValues},
    {"menu", kMenuKeys},
    {"column", kContainerKeys},
    {"row", kContainerKeys},
    {"group", kGroupKeys},
    {"grid", kGridKeys},
    {"spacer", {}},
    {"panel", kPanelKeys},
    {"scroll", kScrollKeys},
    {"switch", kSwitchKeys},
    {"tabs", kTabsKeys, 0, kTabsValues},
    {"dialog", kDialogKeys, kScope, kDialogValues},
    {"drawer", kDialogKeys, kScope, kDialogValues},
    {"busy", kBusyKeys},
    {"forEach", kForEachKeys, kScope | kRowScope},
    {"table", kTableKeys, kScope | kRowScope, kTableValues},
    {"dateTimeInput", kDateTimeKeys, kField | kWritable, kInputValues},
    {"slider", kSliderKeys, kField | kWritable, kInputValues},
    {"filePicker", kFilePickerKeys, kField},
    {"banner", kBannerKeys},
    {"badge", kBadgeKeys},
    {"progress", kProgressKeys},
    {"steps", kStepsKeys},
    {"keyValue", kKeyValueKeys},
    {"emptyState", kEmptyStateKeys},
    {"chart", kChartKeys},
    {"splitter", kSplitterKeys},
    {"collapsible", kCollapsibleKeys},
    {"dropZone", kDropZoneKeys},
    {"actionBar", kActionBarKeys},
    {"page", kPageKeys},
    {"pageRegion", kPageRegionKeys},
    {"localeScope", kLocaleScopeKeys},
    {"customize", kCustomizeKeys},
    {"boundary", kBoundaryKeys},
    {"form", kFormKeys, kOpenValues, kFormValues},
    {"collection", kCollectionKeys},
    {"wizard", kCollectionKeys},
    {"custom", kCustomKeys},
    {"menuItem", kMenuItemKeys, kPart},
    {"case", kCaseKeys, kPart | kScope},
    {"tab", kTabKeys, kPart},
    {"column", kColumnKeys, kPart},
    {"fieldGrid", kGridPartKeys, kPart},
    {"group", kTableGroupKeys, kPart},
    {"columnGroup", kColumnGroupKeys, kPart},
    {"band", kBandKeys, kPart},
    {"summary", kSummaryKeys, kPart},
    {"layoutRow", kLayoutRowKeys, kPart},
    {"layoutCells", {}, kPart},
    {"layoutCell", kLayoutCellKeys, kPart},
    {"track", kTrackKeys, kPart},
    {"action", kActionKeys, kPart},
    {"a11y", kA11yKeys, kPart},
    {"drag", kDragKeys, kPart},
    {"drop", kDropKeys, kPart},
    {"actionButton", kActionButtonKeys, kPart},
    {"headBadge", kHeadBadgeKeys, kPart},
    {"headMenuItem", kHeadMenuItemKeys, kPart},
    {"headTool", kHeadToolKeys, kPart},
    {"navigationBadge", kNavigationBadgeKeys, kPart},
    {"fields", {}, kPart},
    {"series", kSeriesKeys, kPart | kRowScope},
    {"line", kLineKeys, kPart},
});
// NOLINTEND(modernize-use-designated-initializers,readability-trailing-comma)

/// @brief A literal length of a page node: an integer count of micrometres.
/// @param expr The key's expression.
/// @param key  The key, for the refusal.
/// @return The length.
[[nodiscard]] Mm pageLengthOf(ExprPtr const& expr, std::string_view key) {
    std::int64_t const* const count = expr->kind == ExprKind::Literal ? expr->literal.asInt() : nullptr;
    if (count == nullptr || *count < 0 || *count > std::numeric_limits<std::int32_t>::max()) {
        refuse(expr->path, "`" + std::string{key} + "` is a literal integer count of micrometres, 0 or more");
    }
    return Mm::fromMicrometres(static_cast<std::int32_t>(*count));
}

/// @brief Refuses a must-understand key (`!name`) the client does not know; other unknown keys are ignored.
/// @param object The JSON object.
/// @param path   Its JSON Pointer.
void refuseMustUnderstand(Json::object_t const& object, std::string const& path) {
    for (auto const& [key, value] : object) {
        static_cast<void>(value);
        if (key.starts_with('!')) {
            refuse(pointerAppend(path, key), "the must-understand key `" + key + "` is not part of ui/1");
        }
    }
}

/// @brief Parses a key chord, and refuses one the workspace shell reserves.
/// @param chord The chord, such as `Ctrl+Shift+K`.
/// @param path  Its JSON Pointer.
// NOLINTNEXTLINE(readability-function-cognitive-complexity): a chord's grammar, modifiers then one key name.
void checkChord(std::string const& chord, std::string const& path) {
    static constexpr std::array<std::string_view, 4> kModifiers = {"Ctrl", "Shift", "Alt", "Meta"};
    static constexpr std::array<std::string_view, 9> kNamedKeys = {
        "Enter", "Esc", "Tab", "Up", "Down", "Left", "Right", "Space", "Delete",
    };
    std::vector<std::string_view> parts;
    std::size_t start = 0;
    for (std::size_t plus = chord.find('+'); plus != std::string::npos; plus = chord.find('+', start)) {
        parts.push_back(std::string_view{chord}.substr(start, plus - start));
        start = plus + 1;
    }
    std::string_view const key = std::string_view{chord}.substr(start);
    std::set<std::string_view> modifiers;
    for (std::string_view const modifier : parts) {
        if (!listed(kModifiers, modifier) || !modifiers.insert(modifier).second) {
            refuse(path, "`" + chord + "` is not a chord: modifiers are Ctrl, Shift, Alt and Meta, once each");
        }
    }
    bool const letterOrDigit =
        key.size() == 1 && ((key.front() >= 'A' && key.front() <= 'Z') || (key.front() >= 'a' && key.front() <= 'z') ||
                            (key.front() >= '0' && key.front() <= '9'));
    bool function = key.size() >= 2 && key.size() <= 3 && key.front() == 'F';
    int number = 0;
    for (char const chr : key.substr(function ? 1 : key.size())) {
        function = function && chr >= '0' && chr <= '9';
        number = (number * 10) + (chr - '0');
    }
    function = function && number >= 1 && number <= 24;
    if (!letterOrDigit && !function && !listed(kNamedKeys, key)) {
        refuse(path, "`" + chord + "` does not end in a key name");
    }
    bool const ctrlOnly = modifiers.size() == 1 && modifiers.contains("Ctrl");
    bool const ctrlShift = modifiers.size() == 2 && modifiers.contains("Ctrl") && modifiers.contains("Shift");
    bool const reserved = (key == "Tab" && (ctrlOnly || ctrlShift)) ||
                          (ctrlOnly && (key == "W" || key == "w" || key == "K" || key == "k"));
    if (reserved) {
        refuse(path, "`" + chord + "` is reserved for the workspace shell");
    }
}

// NOLINTBEGIN(misc-no-recursion): a document is a tree of nodes and command lists, and parsing descends it.
/// @brief Turns the JSON of a document into its model, refusing what does not parse.
class DocumentParser {
public:
    /// @brief Parses the whole document.
    /// @param json The document.
    /// @return The model, not yet checked against its names.
    [[nodiscard]] Document parse(Json const& json) {
        auto const* const object = std::get_if<Json::object_t>(&json.data);
        if (object == nullptr) {
            refuse("", "a document is a JSON object");
        }
        refuseMustUnderstand(*object, "");
        Document doc;
        doc.vocab = text(*object, "vocab", "", true);
        if (doc.vocab != kVocabulary) {
            refuse("/vocab", "the vocabulary `" + doc.vocab + "` is not supported; this client reads ui/1");
        }
        doc.id = text(*object, "id", "", true);
        if (doc.id.empty()) {
            refuse("/id", "a document has a non-empty id");
        }
        checkKnownVersion(*object);
        doc.title = optionalExpr(*object, "title", "");
        doc.identity = optionalExpr(*object, "identity", "");
        doc.dirty = optionalExpr(*object, "dirty", "");
        doc.titleSpans = optionalExpr(*object, "titleSpans", "");
        doc.eyebrow = optionalExpr(*object, "eyebrow", "");
        doc.eyebrowSpans = optionalExpr(*object, "eyebrowSpans", "");
        doc.subtitle = optionalExpr(*object, "subtitle", "");
        doc.subtitleSpans = optionalExpr(*object, "subtitleSpans", "");
        doc.leadPlacement = optionalExpr(*object, "leadPlacement", "");
        checkLeadPlacementLiteral(doc.leadPlacement);
        doc.titleRole = optionalExpr(*object, "titleRole", "");
        checkTitleRoleLiteral(doc.titleRole);
        doc.headSurface = optionalExpr(*object, "headSurface", "");
        doc.pageSurface = optionalExpr(*object, "pageSurface", "");
        doc.status = topParts(*object, "status", "headBadge");
        doc.actions = topParts(*object, "actions", "actionButton");
        doc.menu = topParts(*object, "menu", "headMenuItem");
        doc.menuLabel = optionalExpr(*object, "menuLabel", "");
        doc.tools = topParts(*object, "tools", "headTool");
        doc.navigation = navigation(*object, doc.id);
        doc.tours = tours(*object, doc.id);
        if (doc.id == "app") {
            refuseScreenHead(*object);
        }
        static constexpr std::array<std::string_view, 2> kPrincipalPlacements = {"sidebar", "head"};
        doc.principalPlacement = choiceOf(*object, "principalPlacement", "", kPrincipalPlacements);
        if (doc.id != "app" && member(*object, "principalPlacement") != nullptr) {
            refuse("/principalPlacement", "`principalPlacement` is the app document's; a screen has none");
        }
        doc.showErrors = showErrors(*object, "");
        datePatterns(*object, doc);
        if (Json const* const params = member(*object, "params")) {
            for (auto const& [name, type] : objectOf(*params, "/params", "`params` maps names to types")) {
                std::string const path = pointerAppend("/params", name);
                checkName(name, path);
                doc.params.emplace_back(name, typeOf(type, path));
            }
        }
        doc.scope = scope(*object, "");
        shellParts(*object, doc);
        for (std::string_view const key : {"onMount", "onUnmount", "onClose", "onSave", "onBackendChange"}) {
            if (Json const* const commands = member(*object, key)) {
                doc.lifecycle.emplace(std::string{key}, commandList(*commands, pointerAppend("", key)));
            }
        }
        if (Json const* const keys = member(*object, "keys")) {
            doc.keys = keyBindings(*keys, "/keys");
        }
        if (Json const* const prefetch = member(*object, "prefetch")) {
            doc.prefetch = nameList(*prefetch, "/prefetch");
        }
        if (Json const* const components = member(*object, "components")) {
            for (auto const& [name, entry] : objectOf(*components, "/components", "`components` is an object")) {
                doc.components.emplace(name, component(name, entry, pointerAppend("/components", name)));
            }
        }
        if (Json const* const points = member(*object, "customize")) {
            for (auto const& [name, entry] : objectOf(*points, "/customize", "`customize` is an object")) {
                doc.customize.emplace(name, point(name, entry, pointerAppend("/customize", name), doc.id == "app"));
            }
        }
        if (Json const* const view = member(*object, "view")) {
            doc.view = node(*view, "/view");
        } else if (doc.id != "app") {
            refuse("", "a screen has a `view`");
        }
        return doc;
    }

    /// @brief Parses one tour, as an entry of `tours` is parsed.
    /// @param json The tour.
    /// @param path Its JSON Pointer.
    /// @return The tour.
    [[nodiscard]] static TourDecl tour(Json const& json, std::string const& path) { return tourOf(json, path); }

private:
    // The app document's `formatting`: a record of the date pattern and the date-and-time pattern, each checked.
    static void datePatterns(Json::object_t const& object, Document& doc) {
        Json const* const formatting = member(object, "formatting");
        if (formatting == nullptr) {
            return;
        }
        if (doc.id != "app") {
            refuse("/formatting", "`formatting` is the application's; only the app document declares it");
        }
        for (auto const& [key, value] : objectOf(*formatting, "/formatting", "`formatting` is a record")) {
            std::string const path = pointerAppend("/formatting", key);
            if (key != "date" && key != "dateTime") {
                refuse(path, "`formatting` takes `date` and `dateTime`");
            }
            std::string const* const pattern = std::get_if<std::string>(&value.data);
            if (pattern == nullptr) {
                refuse(path, "a date pattern is a string, such as `dd-MMM-yyyy`");
            }
            if (auto const checked = checkDatePattern(*pattern, key == "dateTime"); !checked) {
                refuse(path, checked.error());
            }
            (key == "date" ? doc.datePattern : doc.dateTimePattern) = *pattern;
        }
    }

    [[nodiscard]] static Json const* member(Json::object_t const& object, std::string_view key) {
        auto const* const found = object.find(key);
        return found == object.end() ? nullptr : &found->second;
    }

    [[nodiscard]] static Json::object_t const& objectOf(Json const& json, std::string const& path, char const* what) {
        auto const* const object = std::get_if<Json::object_t>(&json.data);
        if (object == nullptr) {
            refuse(path, what);
        }
        return *object;
    }

    static void checkName(std::string const& name, std::string const& path) {
        if (!isName(name)) {
            refuse(path, "`" + name + "` is not a name: names match [A-Za-z_][A-Za-z0-9_]*");
        }
    }

    [[nodiscard]] static std::string text(Json::object_t const& object, std::string_view key, std::string const& path,
                                          bool required) {
        Json const* const value = member(object, key);
        std::string const keyPath = pointerAppend(path, key);
        if (value == nullptr) {
            if (required) {
                refuse(path, "`" + std::string{key} + "` is required");
            }
            return {};
        }
        auto const* const string = std::get_if<std::string>(&value->data);
        if (string == nullptr) {
            refuse(keyPath, "`" + std::string{key} + "` is a string");
        }
        return *string;
    }

    [[nodiscard]] static std::string choiceOf(Json::object_t const& object, std::string_view key,
                                              std::string const& path, std::span<std::string_view const> choices) {
        std::string value = text(object, key, path, false);
        if (!value.empty() && !listed(choices, value)) {
            refuse(pointerAppend(path, key), "`" + value + "` is not one of the values of `" + std::string{key} + "`");
        }
        return value;
    }

    [[nodiscard]] static std::string showErrors(Json::object_t const& object, std::string const& path) {
        static constexpr std::array<std::string_view, 3> kPolicies = {"touched", "submit", "always"};
        return choiceOf(object, "showErrors", path, kPolicies);
    }

    [[nodiscard]] static std::int64_t integer(Json::object_t const& object, std::string_view key,
                                              std::string const& path) {
        Json const* const value = member(object, key);
        if (value == nullptr) {
            return 0;
        }
        auto const number = detail::jsonInt(*value);
        if (!number || *number < 0) {
            refuse(pointerAppend(path, key), "`" + std::string{key} + "` is a non-negative integer");
        }
        return *number;
    }

    [[nodiscard]] static bool flag(Json::object_t const& object, std::string_view key, std::string const& path) {
        Json const* const value = member(object, key);
        if (value == nullptr) {
            return false;
        }
        bool const* const set = std::get_if<bool>(&value->data);
        if (set == nullptr) {
            refuse(pointerAppend(path, key), "`" + std::string{key} + "` is a bool");
        }
        return *set;
    }

    [[nodiscard]] static Type typeOf(Json const& json, std::string const& path) {
        auto const* const spelled = std::get_if<std::string>(&json.data);
        if (spelled == nullptr) {
            refuse(path, "a type is written as a string, such as `int?`");
        }
        auto type = parseType(*spelled);
        if (!type) {
            refuse(path, type.error());
        }
        return *type;
    }

    [[nodiscard]] static std::vector<std::string> nameList(Json const& json, std::string const& path) {
        auto const* const array = std::get_if<Json::array_t>(&json.data);
        if (array == nullptr) {
            refuse(path, "a list of names is an array of strings");
        }
        std::vector<std::string> names;
        for (std::size_t index = 0; index < array->size(); ++index) {
            auto const* const name = std::get_if<std::string>(&array->at(index).data);
            if (name == nullptr || name->empty()) {
                refuse(pointerAppend(path, index), "a list of names is an array of strings");
            }
            names.push_back(*name);
        }
        return names;
    }

    [[nodiscard]] static ExprPtr expr(Json const& json, std::string const& path) {
        auto parsed = parseExpression(json, path);
        if (!parsed) {
            throw DocumentRefused{std::move(parsed.error())};
        }
        return std::move(*parsed);
    }

    [[nodiscard]] static ExprPtr optionalExpr(Json::object_t const& object, std::string_view key,
                                              std::string const& path) {
        Json const* const value = member(object, key);
        return value == nullptr ? nullptr : expr(*value, pointerAppend(path, key));
    }

    // The head belongs to a screen's tab; the app document has no view and so no head.
    static void refuseScreenHead(Json::object_t const& object) {
        for (std::string_view const key : {
                 "titleSpans",
                 "eyebrow",
                 "eyebrowSpans",
                 "subtitle",
                 "subtitleSpans",
                 "leadPlacement",
                 "titleRole",
                 "headSurface",
                 "pageSurface",
                 "status",
                 "actions",
                 "menu",
                 "menuLabel",
                 "tools",
             }) {
            if (member(object, key) != nullptr) {
                refuse(pointerAppend("", key), "`" + std::string{key} + "` is a screen's; the app document has none");
            }
        }
    }

    // A `leadPlacement` written as a literal is one of the three places; a computed one is read at run time.
    static void checkLeadPlacementLiteral(ExprPtr const& placement) {
        if (placement == nullptr || placement->kind != ExprKind::Literal) {
            return;
        }
        std::string const* const name = placement->literal.asString();
        if (name == nullptr || (*name != "below" && *name != "inline" && *name != "end")) {
            refuse(placement->path, "`leadPlacement` is `below`, `inline` or `end`");
        }
    }

    // A `titleRole` written as a literal is one of the two roles a head's title takes; a computed one is read at run
    // time.
    static void checkTitleRoleLiteral(ExprPtr const& role) {
        if (role == nullptr || role->kind != ExprKind::Literal) {
            return;
        }
        std::string const* const name = role->literal.asString();
        if (name == nullptr || (*name != "heading" && *name != "headline")) {
            refuse(role->path, "`titleRole` is `heading` or `headline`");
        }
    }

    // The app document's `navigation` badges, each naming its screen; a screen declares none.
    [[nodiscard]] std::vector<DocNodePtr> navigation(Json::object_t const& object, std::string_view documentId) {
        std::vector<DocNodePtr> badges = topParts(object, "navigation", "navigationBadge");
        if (documentId != "app" && !badges.empty()) {
            refuse("/navigation", "`navigation` is the app document's; a screen has none");
        }
        for (DocNodePtr const& badge : badges) {
            if (badge->name("screen").empty()) {
                refuse(pointerAppend(badge->path, "screen"), "a navigation badge names the screen of its entry");
            }
        }
        return badges;
    }

    // The app document's guided tours, each checked for shape here and for the names it uses when the bundle is
    // built; a screen declares none.
    [[nodiscard]] static std::vector<TourDecl> tours(Json::object_t const& object, std::string_view documentId) {
        std::vector<TourDecl> out;
        Json const* const declared = member(object, "tours");
        if (declared == nullptr) {
            return out;
        }
        if (documentId != "app") {
            refuse("/tours", "`tours` is the app document's; a screen has none");
        }
        auto const* const array = std::get_if<Json::array_t>(&declared->data);
        if (array == nullptr) {
            refuse("/tours", "`tours` is an array of tours");
        }
        std::set<std::string, std::less<>> ids;
        for (std::size_t index = 0; index < array->size(); ++index) {
            std::string const path = pointerAppend("/tours", index);
            TourDecl tour = tourOf(array->at(index), path);
            if (!ids.insert(tour.id).second) {
                refuse(pointerAppend(path, "id"), "the tour id `" + tour.id + "` is declared twice");
            }
            out.push_back(std::move(tour));
        }
        return out;
    }

    // A tour id: lower-case letters, digits and dashes, at most `kMaxTourIdBytes`.
    [[nodiscard]] static bool isTourId(std::string_view text) {
        return !text.empty() && text.size() <= kMaxTourIdBytes && std::ranges::all_of(text, [](char chr) {
            return (chr >= 'a' && chr <= 'z') || (chr >= '0' && chr <= '9') || chr == '-';
        });
    }

    [[nodiscard]] static TourDecl tourOf(Json const& json, std::string const& path) {
        auto const& fields = objectOf(json, path, "a tour is a record of `id`, `title` and `steps`");
        refuseMustUnderstand(fields, path);
        TourDecl tour;
        tour.path = path;
        tour.id = text(fields, "id", path, true);
        if (!isTourId(tour.id)) {
            refuse(pointerAppend(path, "id"), "a tour id is [a-z0-9-]+, at most " + std::to_string(kMaxTourIdBytes) +
                                                  " characters; `" + tour.id + "` is not");
        }
        tour.screen = text(fields, "screen", path, false);
        tour.title = optionalExpr(fields, "title", path);
        if (tour.title == nullptr) {
            refuse(path, "`title` is required");
        }
        tour.summary = optionalExpr(fields, "summary", path);
        Json const* const steps = member(fields, "steps");
        if (steps == nullptr) {
            refuse(path, "`steps` is required");
        }
        std::string const where = pointerAppend(path, "steps");
        auto const* const array = std::get_if<Json::array_t>(&steps->data);
        if (array == nullptr) {
            refuse(where, "`steps` is an array of steps");
        }
        if (array->empty()) {
            refuse(where, "a tour has one step or more");
        }
        if (array->size() > kMaxTourSteps) {
            refuse(where, "a tour has at most " + std::to_string(kMaxTourSteps) + " steps");
        }
        for (std::size_t index = 0; index < array->size(); ++index) {
            tour.steps.push_back(tourStepOf(array->at(index), pointerAppend(where, index)));
        }
        return tour;
    }

    [[nodiscard]] static TourStepDecl tourStepOf(Json const& json, std::string const& path) {
        auto const& fields = objectOf(json, path, "a tour step is a record with a `title`");
        refuseMustUnderstand(fields, path);
        TourStepDecl step;
        step.path = path;
        step.title = optionalExpr(fields, "title", path);
        if (step.title == nullptr) {
            refuse(path, "`title` is required");
        }
        step.body = optionalExpr(fields, "body", path);
        step.screen = text(fields, "screen", path, false);
        step.target = text(fields, "target", path, false);
        if (Json const* const params = member(fields, "params")) {
            std::string const where = pointerAppend(path, "params");
            for (auto const& [name, value] : objectOf(*params, where, "`params` is a record of literal values")) {
                std::string const paramPath = pointerAppend(where, name);
                if (std::holds_alternative<Json::object_t>(value.data) ||
                    std::holds_alternative<Json::array_t>(value.data)) {
                    refuse(paramPath, "a tour step's param is a literal: a string, a whole number, a flag or null");
                }
                auto converted = valueFromJson(value);
                if (!converted) {
                    refuse(paramPath, "a tour step's param is a literal: " + converted.error());
                }
                step.params.insert_or_assign(name, std::move(*converted));
            }
        }
        if (Json const* const advance = member(fields, "advanceOn")) {
            std::string const where = pointerAppend(path, "advanceOn");
            auto const& record = objectOf(*advance, where, R"(`advanceOn` is a record: {"screen": id})");
            refuseMustUnderstand(record, where);
            step.advanceOn = text(record, "screen", where, true);
        }
        return step;
    }

    // A top-level list of parts: the screen head's `status` and `actions`.
    // NOLINTNEXTLINE(bugprone-easily-swappable-parameters): the document key, then its part kind, at every call.
    [[nodiscard]] std::vector<DocNodePtr> topParts(Json::object_t const& object, std::string_view key,
                                                   std::string_view kind) {
        std::vector<DocNodePtr> parts;
        Json const* const value = member(object, key);
        if (value == nullptr) {
            return parts;
        }
        std::string const path = pointerAppend("", key);
        auto const* const array = std::get_if<Json::array_t>(&value->data);
        if (array == nullptr) {
            refuse(path, "`" + std::string{key} + "` is an array");
        }
        for (std::size_t index = 0; index < array->size(); ++index) {
            parts.push_back(part(array->at(index), kind, pointerAppend(path, index)));
        }
        return parts;
    }

    /// @brief Reads the app document's `shell` member, checks it, and appends the queries and state it lowers to.
    /// @param object The document's JSON object.
    /// @param doc    The document being built; its scope receives the lowered declarations.
    void shellParts(Json::object_t const& object, Document& doc) {
        Json const* const declared = member(object, "shell");
        if (declared == nullptr) {
            return;
        }
        if (doc.id != "app") {
            refuse("/shell", "`shell` is the app document's; a screen has none");
        }
        ShellParts parts;
        auto const text = glz::write<::morph::wire::detail::EscapingWriteOpts{}>(*declared);
        if (!text || glz::read_json(parts, *text)) {
            refuse("/shell",
                   "`shell` holds the parts: context, modules, search, inbox, principal, brand, help, searchHint");
        }
        checkShellParts(parts);
        auto const checkNames = [](auto const& declarations) {
            for (auto const& entry : declarations) {
                if (shell_names::reserved(entry.name)) {
                    refuse(entry.path, "`" + entry.name + "` is reserved: the shell's parts declare it");
                }
            }
        };
        checkNames(doc.scope.models);
        checkNames(doc.scope.state);
        checkNames(doc.scope.lets);
        checkNames(doc.scope.queries);
        checkNames(doc.scope.mutations);
        auto lowered = parseJson(lowerShellParts(parts));
        auto const* const loweredObject = lowered ? std::get_if<Json::object_t>(&lowered->data) : nullptr;
        if (loweredObject == nullptr) {
            refuse("/shell", "the shell's parts do not lower");
        }
        ScopeDecl extra = scope(*loweredObject, "/shell");
        auto const append = [](auto& into, auto& from) {
            into.insert(into.end(), std::make_move_iterator(from.begin()), std::make_move_iterator(from.end()));
        };
        append(doc.scope.models, extra.models);
        append(doc.scope.state, extra.state);
        append(doc.scope.queries, extra.queries);
        append(doc.scope.mutations, extra.mutations);
        doc.shell = std::move(parts);
    }

    /// @brief Refuses a declaration that cannot be read: a repeated module, an empty required field, a repeated tag.
    /// @param parts The parts.
    static void checkShellParts(ShellParts const& parts) {
        auto required = [](std::string const& value, std::string const& path) {
            if (value.empty()) {
                refuse(path, "the shell's part needs this name");
            }
        };
        auto const distinct = [](auto const& list, std::string const& path, auto key) {
            std::set<std::string> seen;
            for (auto const& entry : list) {
                if (!seen.insert(key(entry)).second) {
                    refuse(path, "the shell's parts list `" + key(entry) + "` twice");
                }
            }
        };
        if (parts.context) {
            required(parts.context->model, "/shell/context/model");
            required(parts.context->action, "/shell/context/action");
            required(parts.context->optionsField, "/shell/context/optionsField");
            required(parts.context->labelField, "/shell/context/labelField");
            required(parts.context->valueField, "/shell/context/valueField");
        }
        auto const count = [&](ModuleCount const& entry, std::string const& path) {
            required(entry.model, path + "/model");
            required(entry.action, path + "/action");
            required(entry.countField, path + "/countField");
        };
        for (std::size_t index = 0; index < parts.modules.size(); ++index) {
            count(parts.modules.at(index), "/shell/modules/" + std::to_string(index));
        }
        distinct(parts.modules, "/shell/modules", [](ModuleCount const& entry) { return entry.module; });
        for (std::size_t index = 0; index < parts.search.size(); ++index) {
            std::string const path = "/shell/search/" + std::to_string(index);
            required(parts.search.at(index).model, path + "/model");
            required(parts.search.at(index).action, path + "/action");
            required(parts.search.at(index).queryField, path + "/queryField");
            required(parts.search.at(index).resultsField, path + "/resultsField");
        }
        distinct(parts.search, "/shell/search", [](SearchSource const& entry) { return entry.module; });
        if (parts.inbox) {
            required(parts.inbox->screen, "/shell/inbox/screen");
            if (parts.inbox->sources.empty()) {
                refuse("/shell/inbox/sources", "an inbox sums at least one source");
            }
            for (std::size_t index = 0; index < parts.inbox->sources.size(); ++index) {
                count(parts.inbox->sources.at(index), "/shell/inbox/sources/" + std::to_string(index));
            }
        }
        if (parts.principal) {
            PrincipalMenu const& menu = *parts.principal;
            required(menu.model, "/shell/principal/model");
            required(menu.action, "/shell/principal/action");
            required(menu.nameField, "/shell/principal/nameField");
            distinct(menu.locales, "/shell/principal/locales", [](std::string const& tag) { return tag; });
            distinct(menu.formats, "/shell/principal/formats", [](std::string const& tag) { return tag; });
        }
        if (parts.brand) {
            required(parts.brand->title, "/shell/brand/title");
        }
        if (parts.help) {
            HelpEntry const& help = *parts.help;
            if (help.screen.empty() == help.action.empty()) {
                refuse("/shell/help", "the help button opens a screen or runs an action: name one of the two");
            }
            if (!help.action.empty()) {
                required(help.model, "/shell/help/model");
            } else if (!help.model.empty()) {
                refuse("/shell/help/model", "a model belongs to an action; the help button opens a screen");
            }
        }
    }

    // NOLINTNEXTLINE(readability-function-cognitive-complexity): one branch per kind of declaration.
    [[nodiscard]] ScopeDecl scope(Json::object_t const& object, std::string const& path) {
        ScopeDecl decl;
        if (Json const* const models = member(object, "models")) {
            std::string const where = pointerAppend(path, "models");
            for (auto const& [name, entry] : objectOf(*models, where, "`models` maps aliases to models")) {
                decl.models.push_back(model(name, entry, pointerAppend(where, name)));
            }
        }
        if (Json const* const state = member(object, "state")) {
            std::string const where = pointerAppend(path, "state");
            for (auto const& [name, entry] : objectOf(*state, where, "`state` maps names to declarations")) {
                std::string const entryPath = pointerAppend(where, name);
                checkName(name, entryPath);
                auto const& fields = objectOf(entry, entryPath, R"(a state declaration is {"init": e, "type": T})");
                refuseMustUnderstand(fields, entryPath);
                StateDecl stateDecl{
                    .name = name,
                    .path = entryPath,
                    .type = Type::any(),
                    .typeDeclared = false,
                    .init = nullptr,
                };
                if (Json const* const type = member(fields, "type")) {
                    stateDecl.type = typeOf(*type, pointerAppend(entryPath, "type"));
                    stateDecl.typeDeclared = true;
                }
                stateDecl.init = optionalExpr(fields, "init", entryPath);
                decl.state.push_back(std::move(stateDecl));
            }
        }
        if (Json const* const lets = member(object, "let")) {
            std::string const where = pointerAppend(path, "let");
            for (auto const& [name, entry] : objectOf(*lets, where, "`let` maps names to expressions")) {
                std::string const entryPath = pointerAppend(where, name);
                checkName(name, entryPath);
                decl.lets.push_back(LetDecl{.name = name, .path = entryPath, .value = expr(entry, entryPath)});
            }
        }
        if (Json const* const queries = member(object, "queries")) {
            std::string const where = pointerAppend(path, "queries");
            for (auto const& [name, entry] : objectOf(*queries, where, "`queries` maps names to queries")) {
                decl.queries.push_back(query(name, entry, pointerAppend(where, name)));
            }
        }
        if (Json const* const mutations = member(object, "mutations")) {
            std::string const where = pointerAppend(path, "mutations");
            for (auto const& [name, entry] : objectOf(*mutations, where, "`mutations` maps names to mutations")) {
                decl.mutations.push_back(mutation(name, entry, pointerAppend(where, name)));
            }
        }
        if (Json const* const watches = member(object, "watch")) {
            std::string const where = pointerAppend(path, "watch");
            auto const* const array = std::get_if<Json::array_t>(&watches->data);
            if (array == nullptr) {
                refuse(where, "`watch` is an array");
            }
            for (std::size_t index = 0; index < array->size(); ++index) {
                decl.watches.push_back(watch(array->at(index), pointerAppend(where, index)));
            }
        }
        if (Json const* const checks = member(object, "checks")) {
            std::string const where = pointerAppend(path, "checks");
            auto const* const array = std::get_if<Json::array_t>(&checks->data);
            if (array == nullptr) {
                refuse(where, "`checks` is an array");
            }
            for (std::size_t index = 0; index < array->size(); ++index) {
                decl.checks.push_back(check(array->at(index), pointerAppend(where, index)));
            }
        }
        return decl;
    }

    [[nodiscard]] static ModelDecl model(std::string const& name, Json const& json, std::string const& path) {
        checkName(name, path);
        auto const& fields = objectOf(json, path, R"(a model alias is {"type": ..., "instance": ...})");
        refuseMustUnderstand(fields, path);
        ModelDecl decl{
            .name = name,
            .path = path,
            .type = text(fields, "type", path, true),
            .instance = {},
            .key = nullptr,
            .borrowedFrom = {},
        };
        Json const* const instance = member(fields, "instance");
        if (instance == nullptr) {
            return decl;
        }
        std::string const where = pointerAppend(path, "instance");
        auto const* const word = std::get_if<std::string>(&instance->data);
        if (word != nullptr && (*word == "private" || *word == "shared")) {
            decl.instance = *word == "private" ? InstanceMode::Private : InstanceMode::Shared;
            return decl;
        }
        if (auto const* const object = std::get_if<Json::object_t>(&instance->data)) {
            if (Json const* const from = member(*object, "from")) {
                auto const* const alias = std::get_if<std::string>(&from->data);
                if (alias == nullptr || !isName(*alias) || object->size() != 1) {
                    refuse(where, R"(a borrowed instance is {"from": alias})");
                }
                decl.instance = InstanceMode::Borrowed;
                decl.borrowedFrom = *alias;
                return decl;
            }
        }
        decl.instance = InstanceMode::Keyed;
        decl.key = expr(*instance, where);
        return decl;
    }

    /// @brief Refuses @p key of @p fields: a refresh key, which a document does not declare.
    /// @param fields The declaration's fields.
    /// @param path   The declaration's JSON Pointer.
    /// @param key    The refresh key: `invalidates`, `patch` or `refreshOn`.
    static void refuseRefreshKey(Json::object_t const& fields, std::string const& path, std::string_view key) {
        if (member(fields, key) != nullptr) {
            refuse(pointerAppend(path, key),
                   std::format("a document does not declare `{}`: an action's effects say what it changes, and every "
                               "query holding a changed record refreshes; a refresh no action declares is a `refetch` "
                               "command",
                               key));
        }
    }

    [[nodiscard]] static QueryDecl query(std::string const& name, Json const& json, std::string const& path) {
        checkName(name, path);
        auto const& fields = objectOf(json, path, "a query is an object");
        refuseMustUnderstand(fields, path);
        QueryDecl decl;
        decl.name = name;
        decl.path = path;
        decl.model = text(fields, "model", path, true);
        std::string const action = text(fields, "action", path, false);
        std::string const evaluate = text(fields, "evaluate", path, false);
        if (action.empty() == evaluate.empty()) {
            refuse(path, "a query names exactly one of `action` and `evaluate`");
        }
        decl.action = action.empty() ? evaluate : action;
        decl.evaluate = !evaluate.empty();
        decl.body = optionalExpr(fields, "body", path);
        decl.when = optionalExpr(fields, "when", path);
        decl.debounce = integer(fields, "debounce", path);
        decl.refreshEvery = integer(fields, "refreshEvery", path);
        decl.staleTime = integer(fields, "staleTime", path);
        refuseRefreshKey(fields, path, "refreshOn");
        return decl;
    }

    /// @brief Parses an `optimistic` write: `{"query": q, "at"?: m, "key": e, "with": e}`.
    /// @param json The write's JSON.
    /// @param path Its JSON Pointer.
    /// @return The write.
    [[nodiscard]] static OptimisticDecl optimistic(Json const& json, std::string const& path) {
        static constexpr char const* shape = R"(an optimistic write is {"query": q, "at"?: m, "key": e, "with": e})";
        auto const& fields = objectOf(json, path, shape);
        refuseMustUnderstand(fields, path);
        Json const* const key = member(fields, "key");
        Json const* const with = member(fields, "with");
        if (key == nullptr || with == nullptr) {
            refuse(path, shape);
        }
        std::string listMember = text(fields, "at", path, false);
        if (member(fields, "at") != nullptr && listMember.empty()) {
            refuse(pointerAppend(path, "at"), "`at` names the member of the query's record that holds the list");
        }
        return OptimisticDecl{
            .query = text(fields, "query", path, true),
            .at = std::move(listMember),
            .key = expr(*key, pointerAppend(path, "key")),
            .with = expr(*with, pointerAppend(path, "with")),
        };
    }

    [[nodiscard]] MutationDecl mutation(std::string const& name, Json const& json, std::string const& path) {
        checkName(name, path);
        auto const& fields = objectOf(json, path, "a mutation is an object");
        refuseMustUnderstand(fields, path);
        MutationDecl decl;
        decl.name = name;
        decl.path = path;
        decl.model = text(fields, "model", path, true);
        decl.action = text(fields, "action", path, true);
        decl.body = optionalExpr(fields, "body", path);
        static constexpr std::array<std::string_view, 4> kModes = {"exclusive", "serial", "latest", "parallel"};
        std::string const mode = choiceOf(fields, "concurrency", path, kModes);
        if (mode == "serial") {
            decl.concurrency = Concurrency::Serial;
        } else if (mode == "latest") {
            decl.concurrency = Concurrency::Latest;
        } else if (mode == "parallel") {
            decl.concurrency = Concurrency::Parallel;
        }
        decl.serialKey = optionalExpr(fields, "serialKey", path);
        if (decl.serialKey && decl.concurrency != Concurrency::Serial) {
            refuse(pointerAppend(path, "serialKey"), "`serialKey` applies to `serial` concurrency only");
        }
        refuseRefreshKey(fields, path, "invalidates");
        refuseRefreshKey(fields, path, "patch");
        if (Json const* const value = member(fields, "optimistic")) {
            decl.optimistic = optimistic(*value, pointerAppend(path, "optimistic"));
        }
        if (Json const* const commands = member(fields, "onSuccess")) {
            decl.onSuccess = commandList(*commands, pointerAppend(path, "onSuccess"));
        }
        if (Json const* const commands = member(fields, "onError")) {
            decl.onError = commandList(*commands, pointerAppend(path, "onError"));
        }
        static constexpr std::array<std::string_view, 1> kOffline = {"queue"};
        decl.offlineQueue = !choiceOf(fields, "offline", path, kOffline).empty();
        return decl;
    }

    [[nodiscard]] WatchDecl watch(Json const& json, std::string const& path) {
        auto const& fields = objectOf(json, path, R"(a watch is {"on": e, "do": [...]})");
        refuseMustUnderstand(fields, path);
        Json const* const watched = member(fields, "on");
        Json const* const body = member(fields, "do");
        if (watched == nullptr || body == nullptr) {
            refuse(path, R"(a watch is {"on": e, "do": [...]})");
        }
        static constexpr std::array<std::string_view, 2> kSources = {"any", "user"};
        return WatchDecl{
            .path = path,
            .on = expr(*watched, pointerAppend(path, "on")),
            .source = choiceOf(fields, "source", path, kSources) == "user" ? WatchSource::User : WatchSource::Any,
            .body = commandList(*body, pointerAppend(path, "do")),
        };
    }

    [[nodiscard]] static CheckDecl check(Json const& json, std::string const& path) {
        auto const& fields = objectOf(json, path, R"(a check is {"id": ..., "assert": e, ...})");
        refuseMustUnderstand(fields, path);
        Json const* const assertion = member(fields, "assert");
        if (assertion == nullptr) {
            refuse(path, "a check has an `assert`");
        }
        CheckDecl decl;
        decl.id = text(fields, "id", path, false);
        decl.path = path;
        decl.assertion = expr(*assertion, pointerAppend(path, "assert"));
        decl.message = optionalExpr(fields, "message", path);
        if (Json const* const targets = member(fields, "targets")) {
            decl.targets = nameList(*targets, pointerAppend(path, "targets"));
        }
        decl.blocking = flag(fields, "blocking", path);
        return decl;
    }

    struct CommandSpec {
        std::string_view key;
        CommandKind kind;
        std::array<std::string_view, 6> companions;
    };

    // NOLINTBEGIN(modernize-use-designated-initializers,readability-trailing-comma): one row per command reads as a table. A trailing comma would put every row member on a line of its own.
    static constexpr std::array<CommandSpec, 24> kCommands = {{
        {"set", CommandKind::Set, {"value"}},
        {"reset", CommandKind::Reset, {}},
        {"run", CommandKind::Run, {"body", "as", "each"}},
        {"refetch", CommandKind::Refetch, {}},
        {"open", CommandKind::Open, {}},
        {"close", CommandKind::Close, {}},
        {"navigate", CommandKind::Navigate, {"params", "as", "onResult", "onCancel"}},
        {"return", CommandKind::Return, {}},
        {"focus", CommandKind::Focus, {"row"}},
        {"host", CommandKind::Host, {"args"}},
        {"switchBackend", CommandKind::SwitchBackend, {}},
        {"resetPrefs", CommandKind::ResetPrefs, {}},
        {"setLocale", CommandKind::SetLocale, {}},
        {"setContext", CommandKind::SetContext, {}},
        {"notify", CommandKind::Notify, {"role"}},
        {"seq", CommandKind::Seq, {}},
        {"all", CommandKind::All, {}},
        {"if", CommandKind::If, {}},
        {"cancelClose", CommandKind::CancelClose, {}},
        {"retry", CommandKind::Retry, {}},
        {"signIn", CommandKind::SignIn, {}},
        {"signOut", CommandKind::SignOut, {}},
        {"upload", CommandKind::Upload, {"to", "as", "id", "into", "nameAs", "typeAs"}},
        {"download", CommandKind::Download, {"save", "name", "to", "as", "from", "type"}},
    }};
    // NOLINTEND(modernize-use-designated-initializers,readability-trailing-comma)

    [[nodiscard]] Commands commandList(Json const& json, std::string const& path) {
        Commands commands;
        if (auto const* const array = std::get_if<Json::array_t>(&json.data)) {
            for (std::size_t index = 0; index < array->size(); ++index) {
                commands.push_back(command(array->at(index), pointerAppend(path, index)));
            }
            return commands;
        }
        commands.push_back(command(json, path));
        return commands;
    }

    [[nodiscard]] static std::string nameOf(Json const& json, std::string const& path, char const* what,
                                            bool dotted = false) {
        auto const* const name = std::get_if<std::string>(&json.data);
        bool valid = name != nullptr && !name->empty();
        if (valid && dotted) {
            auto const segments = splitPath(*name);
            valid = segments.has_value() && isName(segments->front());
        } else if (valid) {
            valid = isName(*name);
        }
        if (!valid) {
            refuse(path, what);
        }
        return *name;
    }

    [[nodiscard]] static std::string nonEmpty(Json const& json, std::string const& path, char const* what) {
        auto const* const name = std::get_if<std::string>(&json.data);
        if (name == nullptr || name->empty()) {
            refuse(path, what);
        }
        return *name;
    }

    static void requireTrue(Json const& json, std::string const& path, std::string_view key) {
        bool const* const value = std::get_if<bool>(&json.data);
        if (value == nullptr || !*value) {
            refuse(path, "`" + std::string{key} + "` is written `true`");
        }
    }

    // NOLINTNEXTLINE(readability-function-cognitive-complexity): one case per command of ui/1.
    [[nodiscard]] Command command(Json const& json, std::string const& path) {
        auto const& fields = objectOf(json, path, R"(a command is an object such as {"set": ..., "value": ...})");
        CommandSpec const* chosen = nullptr;
        for (auto const& [key, value] : fields) {
            static_cast<void>(value);
            for (CommandSpec const& spec : kCommands) {
                if (spec.key == key) {
                    if (chosen != nullptr) {
                        refuse(path,
                               "a command has one kind; found `" + std::string{chosen->key} + "` and `" + key + "`");
                    }
                    chosen = &spec;
                }
            }
        }
        if (chosen == nullptr) {
            refuse(path, "not a command of ui/1");
        }
        refuseMustUnderstand(fields, path);
        Command cmd;
        cmd.kind = chosen->kind;
        cmd.path = path;
        Json const& primary = *member(fields, chosen->key);
        std::string const where = pointerAppend(path, chosen->key);
        switch (chosen->kind) {
            case CommandKind::Set: {
                cmd.target = nameOf(primary, where, "`set` names state or an element as `name.path`", true);
                Json const* const value = member(fields, "value");
                if (value == nullptr) {
                    refuse(path, "`set` needs a `value`");
                }
                cmd.value = expr(*value, pointerAppend(path, "value"));
                break;
            }
            case CommandKind::Run:
                cmd.target = nameOf(primary, where, "`run` names a mutation");
                cmd.value = optionalExpr(fields, "body", path);
                cmd.extra = optionalExpr(fields, "each", path);
                if (Json const* const binding = member(fields, "as")) {
                    cmd.binding = nameOf(*binding, pointerAppend(path, "as"), "`as` is a name");
                }
                break;
            case CommandKind::Reset:
            case CommandKind::Refetch:
            case CommandKind::Open:
                cmd.target = nameOf(primary, where, "the command names a declaration or an element");
                break;
            case CommandKind::Focus:
                cmd.target = nameOf(primary, where, "the command names a declaration or an element");
                cmd.extra = optionalExpr(fields, "row", path);
                break;
            case CommandKind::Close:
                cmd.target = nameOf(primary, where, "`close` names a dialog, or `self`");
                break;
            case CommandKind::Navigate:
                navigate(cmd, fields, primary, path);
                break;
            case CommandKind::Return:
            case CommandKind::SetContext:
                cmd.value = expr(primary, where);
                break;
            case CommandKind::Host:
                cmd.target = nonEmpty(primary, where, "`host` names a registered command");
                cmd.value = optionalExpr(fields, "args", path);
                break;
            case CommandKind::SwitchBackend:
            case CommandKind::SetLocale:
                cmd.target = nonEmpty(primary, where, "the command takes a non-empty string");
                break;
            case CommandKind::ResetPrefs:
                cmd.target = nameOf(primary, where, "`resetPrefs` names a customization point, or `all`");
                break;
            case CommandKind::Notify: {
                cmd.value = expr(primary, where);
                static constexpr std::array<std::string_view, 4> kRoles = {"info", "success", "warning", "error"};
                cmd.option = choiceOf(fields, "role", path, kRoles);
                break;
            }
            case CommandKind::Seq:
            case CommandKind::All: {
                if (!std::holds_alternative<Json::array_t>(primary.data)) {
                    refuse(where, "`" + std::string{chosen->key} + "` takes an array of commands");
                }
                cmd.commands = commandList(primary, where);
                break;
            }
            case CommandKind::If: {
                auto const* const array = std::get_if<Json::array_t>(&primary.data);
                if (array == nullptr || array->size() < 2 || array->size() > 3) {
                    refuse(where, "`if` takes [condition, [then...], [else...]]");
                }
                cmd.value = expr(array->at(0), pointerAppend(where, std::size_t{0}));
                cmd.commands = commandList(array->at(1), pointerAppend(where, 1));
                if (array->size() == 3) {
                    cmd.alternative = commandList(array->at(2), pointerAppend(where, 2));
                }
                break;
            }
            case CommandKind::CancelClose:
            case CommandKind::Retry:
            case CommandKind::SignOut:
                requireTrue(primary, where, chosen->key);
                break;
            case CommandKind::SignIn: {
                auto const* const object = std::get_if<Json::object_t>(&primary.data);
                Json const* const from = object == nullptr ? nullptr : member(*object, "from");
                if (from == nullptr) {
                    refuse(where, R"(`signIn` is {"from": mutation})");
                }
                cmd.target = nameOf(*from, pointerAppend(where, "from"), "`signIn` names its mutation");
                break;
            }
            case CommandKind::Upload: {
                cmd.value = expr(primary, where);
                Json const* const destination = member(fields, "to");
                if (destination == nullptr) {
                    refuse(path, "`upload` names its mutation with `to`");
                }
                cmd.target = nameOf(*destination, pointerAppend(path, "to"), "`to` names a mutation");
                if (Json const* const binding = member(fields, "as")) {
                    cmd.binding = nameOf(*binding, pointerAppend(path, "as"), "`as` is a name");
                }
                if (Json const* const transfer = member(fields, "id")) {
                    cmd.transfer = nameOf(*transfer, pointerAppend(path, "id"), "`id` is a name");
                }
                static constexpr std::array<std::string_view, 2> kInto = {"ref", "bytes"};
                cmd.bytes = choiceOf(fields, "into", path, kInto) == "bytes";
                for (auto const& [key, slot] : {std::pair{"nameAs", &cmd.nameAs}, std::pair{"typeAs", &cmd.typeAs}}) {
                    if (Json const* const name = member(fields, key)) {
                        if (!cmd.bytes) {
                            refuse(pointerAppend(path, key),
                                   "`" + std::string{key} +
                                       R"(` names a body member for inline bytes; give `"into": "bytes"`)");
                        }
                        *slot = nameOf(*name, pointerAppend(path, key), "a body member is a name");
                    }
                }
                break;
            }
            case CommandKind::Download:
            default:
                download(cmd, fields, primary, path);
                break;
        }
        return cmd;
    }

    void navigate(Command& cmd, Json::object_t const& fields, Json const& primary, std::string const& path) {
        std::string const where = pointerAppend(path, "navigate");
        if (auto const* const route = std::get_if<Json::object_t>(&primary.data)) {
            Json const* const value = member(*route, "route");
            if (value == nullptr || route->size() != 1) {
                refuse(where, R"(`navigate` names a screen, or is {"route": e})");
            }
            cmd.extra = expr(*value, pointerAppend(where, "route"));
        } else {
            cmd.target = nonEmpty(primary, where, R"(`navigate` names a screen, or is {"route": e})");
        }
        cmd.value = optionalExpr(fields, "params", path);
        static constexpr std::array<std::string_view, 4> kModes = {"tab", "dialog", "drawer", "window"};
        cmd.option = choiceOf(fields, "as", path, kModes);
        if (Json const* const result = member(fields, "onResult")) {
            cmd.commands = commandList(*result, pointerAppend(path, "onResult"));
        }
        if (Json const* const cancel = member(fields, "onCancel")) {
            cmd.alternative = commandList(*cancel, pointerAppend(path, "onCancel"));
        }
    }

    static void download(Command& cmd, Json::object_t const& fields, Json const& primary, std::string const& path) {
        cmd.value = expr(primary, pointerAppend(path, "download"));
        static constexpr std::array<std::string_view, 2> kFrom = {"ref", "bytes"};
        cmd.bytes = choiceOf(fields, "from", path, kFrom) == "bytes";
        if (cmd.bytes) {
            // Inline bytes carry no name or type of their own, so the document gives the name.
            if (member(fields, "name") == nullptr) {
                refuse(path, "a download from bytes names its file with `name`");
            }
            cmd.extra = optionalExpr(fields, "name", path);
            cmd.mediaType = optionalExpr(fields, "type", path);
        } else if (member(fields, "type") != nullptr) {
            refuse(pointerAppend(path, "type"), R"(`type` is the media type of inline bytes; give `"from": "bytes"`)");
        }
        std::string const save = text(fields, "save", path, false);
        std::string const destination = text(fields, "to", path, false);
        if (save == "dialog" && destination.empty()) {
            cmd.option = "dialog";
            cmd.extra = optionalExpr(fields, "name", path);
            return;
        }
        if (destination == "preview" && save.empty()) {
            Json const* const binding = member(fields, "as");
            if (binding == nullptr) {
                refuse(path, "a download to preview binds a state with `as`");
            }
            cmd.option = "preview";
            cmd.target = nameOf(*binding, pointerAppend(path, "as"), "`as` names a state");
            return;
        }
        refuse(path, R"(`download` takes `"save": "dialog"` or `"to": "preview"`)");
    }

    [[nodiscard]] std::map<std::string, Commands, std::less<>> keyBindings(Json const& json, std::string const& path) {
        std::map<std::string, Commands, std::less<>> keys;
        for (auto const& [chord, commands] : objectOf(json, path, "`keys` maps chords to commands")) {
            std::string const where = pointerAppend(path, chord);
            checkChord(chord, where);
            keys.emplace(chord, commandList(commands, where));
        }
        return keys;
    }

    // NOLINTNEXTLINE(readability-function-cognitive-complexity): a component's rules, one after another.
    [[nodiscard]] Component component(std::string const& name, Json const& json, std::string const& path) {
        checkName(name, path);
        auto const& fields = objectOf(json, path, "a component is an object");
        refuseMustUnderstand(fields, path);
        Component decl;
        decl.name = name;
        decl.path = path;
        decl.qml = text(fields, "qml", path, false);
        decl.host = text(fields, "host", path, false);
        if (decl.qml.empty() == decl.host.empty()) {
            refuse(path, "a component names exactly one of `qml` and `host`");
        }
        if (!decl.qml.empty()) {
            bool hash = decl.qml.size() == 71 && decl.qml.starts_with("sha256:");
            for (char const chr : std::string_view{decl.qml}.substr(std::min<std::size_t>(7, decl.qml.size()))) {
                hash = hash && ((chr >= '0' && chr <= '9') || (chr >= 'a' && chr <= 'f'));
            }
            if (!hash) {
                refuse(pointerAppend(path, "qml"), "`qml` is a content hash `sha256:<64 hex digits>`");
            }
        } else {
            Json const* const version = member(fields, "version");
            auto const number = version == nullptr ? std::nullopt : detail::jsonInt(*version);
            if (!number) {
                refuse(path, "a host component has an integer `version`");
            }
            decl.version = *number;
        }
        static constexpr std::array<std::string_view, 6> kPropTypes = {
            "string", "bool", "int", "exact", "number", "json",
        };
        if (Json const* const props = member(fields, "props")) {
            std::string const where = pointerAppend(path, "props");
            for (auto const& [prop, type] : objectOf(*props, where, "`props` maps names to prop types")) {
                auto const* const spelled = std::get_if<std::string>(&type.data);
                if (spelled == nullptr || !listed(kPropTypes, *spelled)) {
                    refuse(pointerAppend(where, prop), "a prop type is string, bool, int, exact, number or json");
                }
                decl.props.emplace(prop, *spelled);
            }
        }
        if (Json const* const events = member(fields, "events")) {
            std::string const where = pointerAppend(path, "events");
            for (auto const& [event, type] : objectOf(*events, where, "`events` maps names to payload types")) {
                decl.events.emplace(event, typeOf(type, pointerAppend(where, event)));
            }
        }
        Json const* const fallback = member(fields, "fallback");
        if (fallback == nullptr) {
            refuse(path, "a component names a `fallback` of built-in nodes");
        }
        decl.fallback = node(*fallback, pointerAppend(path, "fallback"));
        return decl;
    }

    [[nodiscard]] static CustomizePoint point(std::string const& name, Json const& json, std::string const& path,
                                              bool appDocument) {
        checkName(name, path);
        auto const& fields = objectOf(json, path, "a customization point is an object");
        refuseMustUnderstand(fields, path);
        static constexpr std::array<std::string_view, 7> kPointKinds = {
            "columns", "choice", "value", "layout", "saved", "pins", "arrange",
        };
        CustomizePoint decl;
        decl.name = name;
        decl.path = path;
        decl.kind = text(fields, "kind", path, true);
        if (!listed(kPointKinds, decl.kind)) {
            refuse(pointerAppend(path, "kind"), "`" + decl.kind + "` is not a kind of customization point");
        }
        if (decl.kind == "pins" && !appDocument) {
            refuse(pointerAppend(path, "kind"), "a `pins` point belongs to the app document");
        }
        // The container an `arrange` point orders is a screen's, and the app document has no view.
        if (decl.kind == "arrange" && appDocument) {
            refuse(pointerAppend(path, "kind"), "an `arrange` point belongs to the screen whose container it orders");
        }
        decl.of = text(fields, "of", path, decl.kind == "columns" || decl.kind == "arrange");
        Json const* const fallback = member(fields, "default");
        if (decl.kind == "choice") {
            Json const* const options = member(fields, "options");
            if (options == nullptr) {
                refuse(path, "a `choice` point lists its `options`");
            }
            decl.options = nameList(*options, pointerAppend(path, "options"));
            if (fallback != nullptr) {
                auto const* const chosen = std::get_if<std::string>(&fallback->data);
                if (chosen == nullptr || std::ranges::find(decl.options, *chosen) == decl.options.end()) {
                    refuse(pointerAppend(path, "default"), "the default of a `choice` is one of its options");
                }
                decl.defaultValue = *chosen;
            }
            decl.type = Type::of(TypeKind::String);
        } else if (decl.kind == "value") {
            Json const* const type = member(fields, "type");
            if (type == nullptr) {
                refuse(path, "a `value` point declares its `type`");
            }
            decl.type = typeOf(*type, pointerAppend(path, "type"));
            if (fallback != nullptr) {
                auto value = valueFromJson(*fallback, decl.type);
                if (!value) {
                    refuse(pointerAppend(path, "default"),
                           "the default does not have the point's type: " + value.error());
                }
                decl.defaultValue = std::move(*value);
            }
        }
        return decl;
    }

    [[nodiscard]] DocNodePtr node(Json const& json, std::string const& path) {
        auto const* const object = std::get_if<Json::object_t>(&json.data);
        if (object == nullptr) {
            refuse(path, "a node is an object with a `kind`");
        }
        std::string const kind = text(*object, "kind", path, true);
        KindSpec const* const spec = findKind(kind, false);
        if (spec == nullptr) {
            refuse(pointerAppend(path, "kind"), "the node kind `" + kind + "` is not in ui/1");
        }
        auto made = std::make_shared<DocNode>();
        made->kind = kind;
        made->path = path;
        refuseMustUnderstand(*object, path);
        fill(*made, *object, *spec, path);
        return made;
    }

    void fill(DocNode& made, Json::object_t const& object, KindSpec const& spec, std::string const& path) {
        bool const isPart = (spec.flags & kPart) != 0;
        auto const apply = [&](KeySpec const& key) {
            Json const* const value = member(object, key.key);
            if (value == nullptr) {
                if ((key.flags & kRequired) != 0) {
                    refuse(path, "`" + std::string{key.key} + "` is required on `" + std::string{spec.kind} + "`");
                }
                return;
            }
            entry(made, key, *value, pointerAppend(path, key.key));
        };
        if (!isPart) {
            for (KeySpec const& key : kCommonKeys) {
                apply(key);
            }
            if ((spec.flags & kField) != 0) {
                for (KeySpec const& key : kFieldKeys) {
                    apply(key);
                }
            }
        }
        for (KeySpec const& key : spec.keys) {
            apply(key);
        }
        if ((spec.flags & kScope) != 0) {
            if (Json const* const declarations = member(object, "scope")) {
                std::string const where = pointerAppend(path, "scope");
                auto const& fields = objectOf(*declarations, where, "`scope` holds a scope's declarations");
                refuseMustUnderstand(fields, where);
                made.scope = scope(fields, where);
            } else {
                made.scope = ScopeDecl{};
            }
        }
        // A fixed grid's cells: one cell part per column id, its id the column's.
        if (made.kind == "layoutCells") {
            for (auto const& [column, cell] : object) {
                auto cellNode = std::make_shared<DocNode>(*part(cell, "layoutCell", pointerAppend(path, column)));
                cellNode->id = column;
                made.parts["cells"].push_back(std::move(cellNode));
            }
        }
        if (made.kind == "fields") {
            for (auto const& [field, overlay] : object) {
                made.parts["fields"].push_back(fieldOverlay(field, overlay, pointerAppend(path, field)));
            }
        } else if (Json const* const since = member(object, "since")) {
            made.since = sinceOf(*since, made.id, pointerAppend(path, "since"));
        }
    }

    // A screen's `knownVersion`, when it has one: the base version a person's copy was last saved against.
    static void checkKnownVersion(Json::object_t const& object) {
        Json const* const known = member(object, "knownVersion");
        if (known == nullptr) {
            return;
        }
        auto const* const version = std::get_if<std::string>(&known->data);
        if (version == nullptr) {
            refuse("/knownVersion", "`knownVersion` is a version, such as \"6.11.0\"");
        }
        if (auto parsed = forms::FieldVersion::parse(*version); !parsed.has_value()) {
            refuse("/knownVersion", std::move(parsed.error()));
        }
    }

    // An element's `since`: a version, on an element that has an id, since versions are what a person's copy of the
    // screen is compared by, and an element is found there by its id.
    // NOLINTNEXTLINE(bugprone-easily-swappable-parameters): the value, the element's id, then where the value is.
    [[nodiscard]] static std::string sinceOf(Json const& value, std::string const& elementId,
                                             std::string const& where) {
        auto const* const text = std::get_if<std::string>(&value.data);
        if (text == nullptr) {
            refuse(where, "`since` is a version, such as \"6.11.0\"");
        }
        if (auto parsed = forms::FieldVersion::parse(*text); !parsed.has_value()) {
            refuse(where, std::move(parsed.error()));
        }
        if (elementId.empty()) {
            refuse(where, "an element with `since` has an `id`");
        }
        return *text;
    }

    // One field overlay of a form's `fields`: expressions by key, the host `component` that draws the field with its
    // `props`, and a grid widget's `grid` part.
    [[nodiscard]] DocNodePtr fieldOverlay(std::string const& field, Json const& overlay, std::string const& where) {
        auto const& settings = objectOf(overlay, where, "a field overlay is an object");
        auto part = std::make_shared<DocNode>();
        part->kind = "field";
        part->id = field;
        part->path = where;
        for (auto const& [key, value] : settings) {
            std::string const keyPath = pointerAppend(where, key);
            if (key == "component") {
                // The host component that draws the field, and the props it is given beside the field's.
                auto const* const name = std::get_if<std::string>(&value.data);
                if (name == nullptr || !isName(*name)) {
                    refuse(keyPath, "`component` is a name");
                }
                part->names.emplace(key, *name);
            } else if (key == "props") {
                auto record = std::make_shared<DocNode>();
                record->kind = "props";
                record->path = keyPath;
                for (auto const& [prop, propValue] : objectOf(value, keyPath, "`props` is a record of expressions")) {
                    record->props.emplace(prop, expr(propValue, pointerAppend(keyPath, prop)));
                }
                part->parts["props"].push_back(std::move(record));
            } else if (key == "grid") {
                part->parts["grid"].push_back(this->part(value, "fieldGrid", keyPath));
            } else {
                part->props.emplace(key, expr(value, keyPath));
            }
        }
        return part;
    }

    [[nodiscard]] DocNodePtr part(Json const& json, std::string_view kind, std::string const& path) {
        KindSpec const* const spec = findKind(kind, true);
        auto const& object = objectOf(json, path, "a part of a node is an object");
        refuseMustUnderstand(object, path);
        auto made = std::make_shared<DocNode>();
        made->kind = std::string{kind};
        made->path = path;
        fill(*made, object, *spec, path);
        if (kind == "actionButton" && made->prop("iconOnly") != nullptr && made->prop("icon") == nullptr) {
            refuse(pointerAppend(path, "iconOnly"), "an icon-only action names its `icon`");
        }
        return made;
    }

    // A table's `filters` written as the literal filter, an object of `columns` and `groups` alone: kept as its
    // JSON text under the node's names, checked to read as a `table::FilterSpec`. Any other value is an expression.
    [[nodiscard]] static bool literalFilter(DocNode& made, Json const& value, std::string const& path) {
        auto const* const object = std::get_if<Json::object_t>(&value.data);
        if (object == nullptr || object->empty() || !std::ranges::all_of(*object, [](auto const& entry) {
                return entry.first == "columns" || entry.first == "groups";
            })) {
            return false;
        }
        std::string const text = glz::write_json(value).value_or(std::string{});
        table::FilterSpec filter;
        if (auto const failed = glz::read_json(filter, text)) {
            refuse(path, "`filters` is not a filter: " + glz::format_error(failed, text));
        }
        made.names.emplace("filters", text);
        return true;
    }

    // NOLINTNEXTLINE(readability-function-cognitive-complexity): one case per shape a key takes.
    void entry(DocNode& made, KeySpec const& key, Json const& value, std::string const& path) {
        std::string const name{key.key};
        if (made.kind == "table" && name == "filters" && literalFilter(made, value, path)) {
            return;
        }
        if (made.kind == "table" && name == "persist") {
            auto const* const text = std::get_if<std::string>(&value.data);
            if (std::get_if<bool>(&value.data) == nullptr && (text == nullptr || text->empty())) {
                refuse(path, "`persist` is a literal: true, false or a name");
            }
        }
        // A grid column's `field` and `unit` may be expressions in the row's scope; a string stays a name.
        if (made.kind == "column" && (name == "field" || name == "unit") &&
            std::get_if<std::string>(&value.data) == nullptr) {
            made.props.emplace(name, expr(value, path));
            return;
        }
        switch (key.shape) {
            case Shape::Expr:
            case Shape::Condition:
                made.props.emplace(name, expr(value, path));
                break;
            case Shape::Commands:
                made.events.emplace(name, commandList(value, path));
                break;
            case Shape::Nodes: {
                std::vector<DocNodePtr> nodes;
                if (auto const* const array = std::get_if<Json::array_t>(&value.data)) {
                    for (std::size_t index = 0; index < array->size(); ++index) {
                        nodes.push_back(node(array->at(index), pointerAppend(path, index)));
                    }
                } else {
                    nodes.push_back(node(value, path));
                }
                made.children.emplace(name, std::move(nodes));
                break;
            }
            case Shape::Text:
            case Shape::Name: {
                auto const* const string = std::get_if<std::string>(&value.data);
                if (string == nullptr || (key.shape == Shape::Name && !isName(*string))) {
                    refuse(path, "`" + name + "` is " + (key.shape == Shape::Name ? "a name" : "a string"));
                }
                if (name == "id") {
                    made.id = *string;
                } else {
                    made.names.emplace(name, *string);
                }
                break;
            }
            case Shape::Part:
                made.parts[name].push_back(part(value, key.part, path));
                break;
            case Shape::Parts:
            case Shape::PartsOrExpr: {
                auto const* const array = std::get_if<Json::array_t>(&value.data);
                if (array == nullptr && key.shape == Shape::PartsOrExpr) {
                    made.props.emplace(name, expr(value, path));
                    break;
                }
                if (array == nullptr) {
                    refuse(path, "`" + name + "` is an array");
                }
                auto& list = made.parts[name];
                for (std::size_t index = 0; index < array->size(); ++index) {
                    list.push_back(part(array->at(index), key.part, pointerAppend(path, index)));
                }
                break;
            }
            case Shape::ExprRecord: {
                if (!key.part.empty()) {
                    made.parts[name].push_back(part(value, key.part, path));
                    break;
                }
                auto record = std::make_shared<DocNode>();
                record->kind = name;
                record->path = path;
                for (auto const& [field, fieldValue] : objectOf(value, path, "a record of expressions")) {
                    record->props.emplace(field, expr(fieldValue, pointerAppend(path, field)));
                }
                made.parts[name].push_back(std::move(record));
                break;
            }
            case Shape::CommandRecord:
                for (auto const& [event, commands] : objectOf(value, path, "a record of command lists")) {
                    made.events.emplace(std::format("{}.{}", name, event),
                                        commandList(commands, pointerAppend(path, event)));
                }
                break;
            case Shape::Flag: {
                auto const* const flag = std::get_if<bool>(&value.data);
                if (flag == nullptr) {
                    refuse(path, "`" + name + "` is a literal `true` or `false`");
                }
                made.flags.emplace(name, *flag);
                break;
            }
            case Shape::Keys:
            default:
                made.keys = keyBindings(value, path);
                break;
        }
    }
    // NOLINTEND(misc-no-recursion)
};

struct ScopeInfo;

/// @brief One declared name of a scope.
struct NameEntry {
    /// @brief What it declares.
    NameCategory category = NameCategory::Param;
    /// @brief The declaration's JSON Pointer.
    std::string path;
    /// @brief The scope that declares it.
    ScopeInfo* scope = nullptr;
    /// @brief The type of a param, of typed state, and of state or a `let` once derived.
    Type type;
    /// @brief The model of a `Model` entry.
    ModelDecl const* model = nullptr;
    /// @brief The expression a state or `let` type is derived from; `nullptr` when the type is declared.
    ExprPtr derivedFrom;
    /// @brief The query of a `Query` entry.
    QueryDecl const* query = nullptr;
    /// @brief The mutation of a `Mutation` entry.
    MutationDecl const* mutation = nullptr;
    /// @brief The element of an `Element` entry.
    DocNode const* element = nullptr;
    /// @brief 0 before its type is derived, 1 while it is, 2 after.
    int visit = 0;
};

/// @brief One scope during validation: its names and its parent.
struct ScopeInfo {
    /// @brief The enclosing scope; `nullptr` for a screen or a component fallback.
    ScopeInfo* parent = nullptr;
    /// @brief The scopes it encloses directly, in document order.
    std::vector<ScopeInfo*> children;
    /// @brief Whether it is a `forEach` or `table` row scope.
    bool row = false;
    /// @brief The `switch` whose case it is; `nullptr` for any other scope.
    DocNode const* switchNode = nullptr;
    /// @brief Whether it is the screen's own scope.
    bool screen = false;
    /// @brief The names it declares.
    std::map<std::string, NameEntry, std::less<>> names;
};

/// @brief Where an expression or command is checked: its scope and what is valid there.
struct CheckContext {
    /// @brief The scope.
    ScopeInfo* scope = nullptr;
    /// @brief Inside a command list, where `event` is valid.
    bool commands = false;
    /// @brief Inside a boundary's `error`, where `boundary.error` and `retry` are valid.
    bool boundaryError = false;
    /// @brief Inside `onClose`, where `cancelClose` is valid.
    bool onClose = false;
    /// @brief Names bound here: `as` of a sequence step or `each`, `result`, `body`, `props`.
    std::vector<std::pair<std::string, Type>> bindings;
    /// @brief The column parts of a grid, whose keys take expressions in the row's scope.
    bool gridColumns = false;
    /// @brief The rows of a fixed grid's block, whose cells are read with `row` the block's collection row.
    bool layoutBlock = false;
};

class DocumentValidator;

/// @brief Resolves an expression's names against a scope chain and a context.
class ScopeResolver final : public NameResolver {
public:
    /// @brief Resolves for @p validator in @p context.
    /// @param validator The validator, which derives state and `let` types on demand.
    /// @param context   Where the expression is.
    ScopeResolver(DocumentValidator& validator, CheckContext const& context)
        : _validator{validator}, _context{context} {}

    /// @brief Resolves a reference.
    /// @param head The first segment.
    /// @param path The rest.
    /// @return The type, or why it does not resolve.
    [[nodiscard]] std::expected<Type, std::string> ref(std::string_view head,
                                                       std::span<std::string const> path) const override;

    /// @brief Resolves a row field.
    /// @param path The field path.
    /// @return `any` inside a row, or why there is no row.
    [[nodiscard]] std::expected<Type, std::string> row(std::span<std::string const> path) const override;

    /// @brief Resolves a query or mutation name.
    /// @param form The form.
    /// @param name The name.
    /// @return The type, or why it does not resolve.
    [[nodiscard]] std::expected<Type, std::string> control(ExprKind form, std::string_view name) const override;

    /// @brief Resolves a customization point.
    /// @param point The point.
    /// @param path  The rest.
    /// @return The type, or why it does not resolve.
    [[nodiscard]] std::expected<Type, std::string> pref(std::string_view point,
                                                        std::span<std::string const> path) const override;

private:
    DocumentValidator& _validator;
    CheckContext const& _context;
};

/// @brief The type of an element value.
/// @param value The value's name.
/// @return Its type.
[[nodiscard]] Type elementValueType(std::string_view value) {
    static constexpr std::array<std::string_view, 10> kBools = {
        "visible", "enabled", "valid", "dirty", "focused", "touched", "checked", "open", "ready", "submitted",
    };
    static constexpr std::array<std::string_view, 3> kCounts = {"viewCount", "sourceCount", "selectedCount"};
    if (listed(kBools, value) || value == "pending") {
        return Type::of(TypeKind::Bool);
    }
    if (value == "errors" || value == "shownErrors") {
        return Type::listOf(Type::of(TypeKind::String));
    }
    if (listed(kCounts, value)) {
        return Type::of(TypeKind::Int);
    }
    if (value == "error") {
        return Type::of(TypeKind::String, true);
    }
    return Type::any();
}

/// @brief Whether a value of type @p actual may stand where @p declared is expected.
/// @param declared The expected type.
/// @param actual   The type found.
/// @return Whether they are compatible: equal kinds, `any` on either side, an `int` for a `decimal` or `key`, a
///         `string` for a `key`.
[[nodiscard]] bool compatible(Type const& declared, Type const& actual) {
    if (declared.isAny() || actual.isAny() || declared.kind == actual.kind) {
        return true;
    }
    bool const integer = actual.kind == TypeKind::Int;
    return (declared.kind == TypeKind::Decimal && integer) ||
           (declared.kind == TypeKind::Key && (integer || actual.kind == TypeKind::String));
}

// NOLINTBEGIN(misc-no-recursion): a document is a tree of scopes, nodes and command lists, and checking descends it.
/// @brief Checks a parsed document against its own names and the catalog: every load-time rule that needs more
///        than one place in the document.
class DocumentValidator {
public:
    /// @brief Validates @p doc.
    /// @param doc     The parsed document.
    /// @param options The catalog facts.
    DocumentValidator(Document const& doc, LoadOptions const& options) : _doc{&doc}, _options{&options} {}

    /// @brief Runs every rule; throws `DocumentRefused` on the first refusal.
    // A tour's texts are read in the app scope, as the navigation badges are.
    void checkTourTexts(CheckContext const& root) {
        for (TourDecl const& tour : _doc->tours) {
            checkExpr(*tour.title, root);
            if (tour.summary) {
                checkExpr(*tour.summary, root);
            }
            for (TourStepDecl const& step : tour.steps) {
                checkExpr(*step.title, root);
                if (step.body) {
                    checkExpr(*step.body, root);
                }
            }
        }
    }

    void run() {
        ScopeInfo& screen = newScope(nullptr, false);
        screen.screen = true;
        for (auto const& [name, type] : _doc->params) {
            NameEntry entry;
            entry.category = NameCategory::Param;
            entry.path = pointerAppend("/params", name);
            entry.type = type;
            add(screen, name, std::move(entry));
        }
        declare(_doc->scope, screen);
        for (auto const& [key, commands] : _doc->lifecycle) {
            static_cast<void>(key);
            collectTransfers(commands, screen);
        }
        for (auto const& [chord, commands] : _doc->keys) {
            static_cast<void>(chord);
            collectTransfers(commands, screen);
        }
        if (_doc->view) {
            collect(*_doc->view, screen);
        }
        for (DocNodePtr const& part : headParts()) {
            collectPart(*part, screen, screen, *part);
        }
        std::vector<ScopeInfo*> componentScopes;
        for (auto const& [name, component] : _doc->components) {
            static_cast<void>(name);
            ScopeInfo& fallbackScope = newScope(nullptr, false);
            collect(*component.fallback, fallbackScope);
            componentScopes.push_back(&fallbackScope);
        }

        CheckContext const root{
            .scope = &screen,
            .commands = false,
            .boundaryError = false,
            .onClose = false,
            .bindings = {},
        };
        checkScope(_doc->scope, root);
        if (_doc->title) {
            checkExpr(*_doc->title, root);
        }
        if (_doc->identity) {
            checkExpr(*_doc->identity, root);
        }
        if (_doc->dirty) {
            checkCondition(*_doc->dirty, root, "`dirty`");
        }
        if (_doc->titleSpans) {
            checkExpr(*_doc->titleSpans, root);
            checkSpans(_doc->titleSpans);
        }
        if (_doc->eyebrow) {
            checkExpr(*_doc->eyebrow, root);
        }
        if (_doc->eyebrowSpans) {
            checkExpr(*_doc->eyebrowSpans, root);
            checkSpans(_doc->eyebrowSpans);
        }
        if (_doc->subtitle) {
            checkExpr(*_doc->subtitle, root);
        }
        if (_doc->subtitleSpans) {
            checkExpr(*_doc->subtitleSpans, root);
            checkSpans(_doc->subtitleSpans);
        }
        if (_doc->leadPlacement) {
            checkExpr(*_doc->leadPlacement, root);
        }
        if (_doc->titleRole) {
            checkExpr(*_doc->titleRole, root);
        }
        if (_doc->headSurface) {
            checkExpr(*_doc->headSurface, root);
        }
        if (_doc->pageSurface) {
            checkExpr(*_doc->pageSurface, root);
        }
        if (_doc->menuLabel) {
            checkExpr(*_doc->menuLabel, root);
        }

        checkTourTexts(root);
        for (DocNodePtr const& part : headParts()) {
            checkElement(*part, root, true);
        }
        checkActionButtons(_doc->actions, "/actions");
        std::set<std::string_view> menuIds;
        checkHeadMenu(_doc->menu, menuIds);
        checkHeadTools(_doc->tools);
        for (auto const& [key, commands] : _doc->lifecycle) {
            CheckContext context = root;
            context.onClose = key == "onClose";
            checkCommands(commands, context);
        }
        for (auto const& [chord, commands] : _doc->keys) {
            static_cast<void>(chord);
            checkCommands(commands, root);
        }
        checkPrefetch();
        if (_doc->view) {
            checkNode(*_doc->view, root);
        }
        std::size_t index = 0;
        for (auto const& [name, component] : _doc->components) {
            static_cast<void>(name);
            CheckContext context{
                .scope = componentScopes.at(index++),
                .commands = false,
                .boundaryError = false,
                .onClose = false,
                .bindings = {},
            };
            context.bindings.emplace_back("props", Type::of(TypeKind::Record));
            checkNode(*component.fallback, context);
        }
        checkCustomizePoints();
        checkWatchGraph();
        checkEffects();
    }

    /// @brief What validation found that does not refuse the document.
    /// @return The warnings, in the order found; the validator keeps none.
    [[nodiscard]] std::vector<LoadWarning> takeWarnings() noexcept { return std::move(_warnings); }

    /// @brief The static type of each date input's `value` that the checker could decide.
    /// @return The table, keyed by node; the validator keeps none.
    [[nodiscard]] std::map<DocNode const*, TypeKind> takeDateValueKinds() noexcept {
        return std::move(_dateValueKinds);
    }

    /// @brief Where each `focus` command's target resolved, keyed by the command's JSON Pointer.
    /// @return The table; the validator keeps none.
    [[nodiscard]] std::map<std::string, FocusTarget, std::less<>> takeFocusTargets() noexcept {
        return std::move(_focusTargets);
    }

    /// @brief Finds a name in a scope chain.
    /// @param scope The innermost scope.
    /// @param name  The name.
    /// @return The entry, or `nullptr`.
    [[nodiscard]] static NameEntry* lookup(ScopeInfo* scope, std::string_view name) {
        for (ScopeInfo* current = scope; current != nullptr; current = current->parent) {
            auto const found = current->names.find(name);
            if (found != current->names.end()) {
                return &found->second;
            }
        }
        return nullptr;
    }

    /// @brief The type of a name whose type is derived from an expression, derived on first use.
    /// @param entry The state or `let` entry.
    /// @param name  Its name, for the cycle's description.
    /// @return The type, or the cycle it closes.
    [[nodiscard]] std::expected<Type, std::string> derivedType(NameEntry& entry, std::string_view name) {
        if (entry.visit == 2 || !entry.derivedFrom) {
            return entry.type;
        }
        if (entry.visit == 1) {
            std::string cycle;
            auto const start = std::ranges::find(_deriving, name);
            for (auto step = start; step != _deriving.end(); ++step) {
                cycle += *step + " → ";
            }
            std::string const graph =
                entry.category == NameCategory::Let ? "the let graph has a cycle: " : "the state inits form a cycle: ";
            return std::unexpected(graph + cycle + std::string{name});
        }
        entry.visit = 1;
        _deriving.emplace_back(name);
        CheckContext const context{
            .scope = entry.scope,
            .commands = false,
            .boundaryError = false,
            .onClose = false,
            .bindings = {},
        };
        Type type = checkExpr(*entry.derivedFrom, context);
        _deriving.pop_back();
        entry.visit = 2;
        entry.type = type;
        return type;
    }

    /// @brief The document.
    /// @return It.
    [[nodiscard]] Document const& document() const noexcept { return *_doc; }

private:
    ScopeInfo& newScope(ScopeInfo* parent, bool row) {
        ScopeInfo& made = _scopes.emplace_back();
        made.parent = parent;
        made.row = row;
        if (parent != nullptr) {
            parent->children.push_back(&made);
        }
        return made;
    }

    static void add(ScopeInfo& scope, std::string const& name, NameEntry entry) {
        static constexpr std::array<std::string_view, 6> kReserved = {
            "event", "app", "boundary", "props", "result", "body",
        };
        if (listed(kReserved, name)) {
            refuse(entry.path, "`" + name + "` is reserved and cannot be declared");
        }
        auto const found = scope.names.find(name);
        if (found != scope.names.end()) {
            refuse(entry.path, "`" + name + "` is declared twice in this scope (first at " + found->second.path + ")");
        }
        entry.scope = &scope;
        scope.names.emplace(name, std::move(entry));
    }

    /// Checks a model alias against the catalog's facts, when there is a catalog.
    void checkCatalog(ModelDecl const& model) {
        if (!_options->catalog.model) {
            return;
        }
        auto const info = _options->catalog.model(model.type);
        if (!info) {
            refuse(pointerAppend(model.path, "type"), "the model type `" + model.type + "` is not in the catalog");
        }
        if (model.instance == InstanceMode::Shared && !info->stateless) {
            refuse(pointerAppend(model.path, "instance"),
                   "the model type `" + model.type +
                       "` keeps state, so `shared` names no instance: it needs a key, or `private`");
        }
        if (model.instance == InstanceMode::Keyed && !info->keyed) {
            refuse(pointerAppend(model.path, "instance"),
                   "the model type `" + model.type + "` is not keyed, so its instance is not an expression");
        }
    }

    void declare(ScopeDecl const& decl, ScopeInfo& scope) {
        for (ModelDecl const& model : decl.models) {
            if (scope.row && model.instance == InstanceMode::Default) {
                refuse(model.path, "a model alias in a row scope names its `instance`");
            }
            if (!scope.screen && model.instance == InstanceMode::Borrowed) {
                refuse(model.path, "only a screen's own aliases borrow the opener's instance");
            }
            checkCatalog(model);
            NameEntry entry;
            entry.category = NameCategory::Model;
            entry.path = model.path;
            entry.model = &model;
            add(scope, model.name, std::move(entry));
        }
        for (StateDecl const& state : decl.state) {
            NameEntry entry;
            entry.category = NameCategory::State;
            entry.path = state.path;
            entry.type = state.type;
            entry.derivedFrom = state.typeDeclared ? nullptr : state.init;
            add(scope, state.name, std::move(entry));
        }
        for (LetDecl const& let : decl.lets) {
            NameEntry entry;
            entry.category = NameCategory::Let;
            entry.path = let.path;
            entry.derivedFrom = let.value;
            add(scope, let.name, std::move(entry));
        }
        for (QueryDecl const& query : decl.queries) {
            NameEntry entry;
            entry.category = NameCategory::Query;
            entry.path = query.path;
            entry.query = &query;
            add(scope, query.name, std::move(entry));
            _queries.emplace(query.name, &query);
            _queryScopes.emplace_back(&query, &scope);
        }
        for (MutationDecl const& mutation : decl.mutations) {
            NameEntry entry;
            entry.category = NameCategory::Mutation;
            entry.path = mutation.path;
            entry.mutation = &mutation;
            add(scope, mutation.name, std::move(entry));
            _mutations.emplace_back(&mutation, &scope);
            collectTransfers(mutation.onSuccess, scope);
            collectTransfers(mutation.onError, scope);
        }
        for (WatchDecl const& watch : decl.watches) {
            _watches.emplace_back(&watch, &scope);
            collectTransfers(watch.body, scope);
        }
    }

    void collectTransfers(Commands const& commands, ScopeInfo& scope) {
        for (Command const& cmd : commands) {
            if (cmd.kind == CommandKind::Upload && !cmd.transfer.empty()) {
                NameEntry entry;
                entry.category = NameCategory::Transfer;
                entry.path = pointerAppend(cmd.path, "id");
                add(scope, cmd.transfer, std::move(entry));
            }
            collectTransfers(cmd.commands, scope);
            collectTransfers(cmd.alternative, scope);
        }
    }

    void collect(DocNode const& node, ScopeInfo& scope) { collectIn(node, scope); }

    void collectIn(DocNode const& node, ScopeInfo& scope) {
        KindSpec const* const spec = findKind(node.kind, false);
        if (!node.id.empty()) {
            NameEntry entry;
            entry.category = NameCategory::Element;
            entry.path = pointerAppend(node.path, "id");
            entry.element = &node;
            add(scope, node.id, std::move(entry));
            _elements.emplace(node.id, &node);
        }
        ScopeInfo* inner = &scope;
        if (spec != nullptr && (spec->flags & kScope) != 0) {
            inner = &newScope(&scope, (spec->flags & kRowScope) != 0);
            _scopeOf.emplace(&node, inner);
            // NOLINTNEXTLINE(bugprone-unchecked-optional-access): the parser gives every scope-creating node a scope.
            declare(*node.scope, *inner);
        }
        _innerOf.emplace(&node, inner);
        for (auto const& [key, commands] : node.events) {
            collectTransfers(commands, isInner(spec, key) ? *inner : scope);
        }
        for (auto const& [chord, commands] : node.keys) {
            static_cast<void>(chord);
            collectTransfers(commands, scope);
        }
        for (auto const& [key, nodes] : node.children) {
            for (DocNodePtr const& child : nodes) {
                collectIn(*child, isInner(spec, key) ? *inner : scope);
            }
        }
        for (auto const& [key, parts] : node.parts) {
            for (DocNodePtr const& child : parts) {
                collectPart(*child, isInner(spec, key) ? *inner : scope, *inner, node);
            }
        }
    }

    void collectPart(DocNode const& part, ScopeInfo& outer, ScopeInfo& ownerInner, DocNode const& owner) {
        KindSpec const* const spec = findKind(part.kind, true);
        ScopeInfo* inner = &ownerInner;
        if (spec != nullptr && (spec->flags & kScope) != 0) {
            inner = &newScope(&outer, false);
            inner->switchNode = owner.kind == "switch" ? &owner : nullptr;
            _scopeOf.emplace(&part, inner);
            // NOLINTNEXTLINE(bugprone-unchecked-optional-access): the parser gives every switch case a scope.
            declare(*part.scope, *inner);
        } else if (spec != nullptr && (spec->flags & kRowScope) != 0) {
            inner = &newScope(&outer, true);
        }
        _innerOf.emplace(&part, inner);
        for (auto const& [key, commands] : part.events) {
            collectTransfers(commands, outer);
        }
        for (auto const& [key, nodes] : part.children) {
            for (DocNodePtr const& child : nodes) {
                collectIn(*child, isInner(spec, key) ? *inner : outer);
            }
        }
        for (auto const& [key, parts] : part.parts) {
            static_cast<void>(key);
            for (DocNodePtr const& child : parts) {
                collectPart(*child, outer, ownerInner, part);
            }
        }
    }

    [[nodiscard]] static bool isInner(KindSpec const* spec, std::string_view key) {
        if (spec == nullptr) {
            return false;
        }
        auto const found = std::ranges::find(spec->keys, key, &KeySpec::key);
        return found != spec->keys.end() && (found->flags & kInner) != 0;
    }

    /// The flags of a child-node key. The parser fills `children` only from a kind's own keys, so the kind and the key
    /// are always found.
    [[nodiscard]] static std::uint8_t keyFlags(KindSpec const& spec, std::string_view key) {
        return std::ranges::find(spec.keys, key, &KeySpec::key)->flags;
    }

    [[nodiscard]] static bool isCondition(KindSpec const* spec, std::string_view key) {
        auto const shapeOf = [key](std::span<KeySpec const> keys) {
            auto const found = std::ranges::find(keys, key, &KeySpec::key);
            return found == keys.end() ? Shape::Expr : found->shape;
        };
        return (spec != nullptr && shapeOf(spec->keys) == Shape::Condition) ||
               shapeOf(kCommonKeys) == Shape::Condition || shapeOf(kFieldKeys) == Shape::Condition;
    }

    Type checkExpr(Expr const& expr, CheckContext const& context) {
        ScopeResolver const resolver{*this, context};
        auto checked = checkExpression(expr, resolver);
        if (!checked) {
            throw DocumentRefused{std::move(checked.error())};
        }
        return std::move(*checked);
    }

    void checkCondition(Expr const& expr, CheckContext const& context, std::string_view what) {
        Type const type = checkExpr(expr, context);
        if (!compatible(Type::of(TypeKind::Bool), type)) {
            refuse(expr.path, std::string{what} + " is " + type.name() + "; expected bool");
        }
    }

    // A date input shows a timestamp or an ISO-8601 date or date-time string; any other type it can never show.
    void checkDateValue(DocNode const& node, Expr const& expr, Type const& type) {
        if (type.kind == TypeKind::String || type.kind == TypeKind::Timestamp) {
            _dateValueKinds.insert_or_assign(&node, type.kind);
        } else if (type.kind != TypeKind::Any && type.kind != TypeKind::Key) {
            refuse(expr.path,
                   "`dateTimeInput` shows a timestamp or an ISO-8601 date or date-time string; `value` is " +
                       type.name());
        }
    }

    [[nodiscard]] static NameEntry& require(CheckContext const& context, std::string const& name,
                                            std::string const& path, std::initializer_list<NameCategory> categories,
                                            std::string_view what) {
        NameEntry* const entry = lookup(context.scope, name);
        if (entry == nullptr || std::ranges::find(categories, entry->category) == categories.end()) {
            refuse(path, "`" + name + "` is not " + std::string{what} + " of this scope");
        }
        return *entry;
    }

    // How many row scopes enclose @p scope, itself included, up to its screen or component fallback.
    [[nodiscard]] static std::size_t rowDepth(ScopeInfo const* scope) {
        std::size_t rows = 0;
        for (; scope != nullptr; scope = scope->parent) {
            rows += scope->row ? 1U : 0U;
        }
        return rows;
    }

    // The element @p name declares in @p scope itself, or nothing.
    [[nodiscard]] static NameEntry const* elementIn(ScopeInfo const& scope, std::string_view name) {
        auto const found = scope.names.find(name);
        return found != scope.names.end() && found->second.category == NameCategory::Element ? &found->second
                                                                                             : nullptr;
    }

    // The elements named @p name in the switch cases below @p scope, at any depth through cases only.
    // NOLINTNEXTLINE(misc-no-recursion): the scope tree, walked down through switch cases.
    static void elementsInCases(ScopeInfo const& scope, std::string_view name, std::vector<NameEntry const*>& out) {
        for (ScopeInfo const* const child : scope.children) {
            if (child->switchNode == nullptr) {
                continue;
            }
            if (NameEntry const* const entry = elementIn(*child, name)) {
                out.push_back(entry);
            }
            elementsInCases(*child, name, out);
        }
    }

    // The row scopes directly below @p scope, or below it through switch cases, that declare the element @p name.
    // NOLINTNEXTLINE(misc-no-recursion): the scope tree, walked down through switch cases.
    static void rowScopesBelow(ScopeInfo const& scope, std::string_view name, std::vector<ScopeInfo const*>& out) {
        for (ScopeInfo const* const child : scope.children) {
            if (child->row) {
                if (elementIn(*child, name) != nullptr && std::ranges::find(out, child) == out.end()) {
                    out.push_back(child);
                }
            } else if (child->switchNode != nullptr) {
                rowScopesBelow(*child, name, out);
            }
        }
    }

    // A `focus` target resolves to declarations. Without `row`: the element the scope chain names, else those in the
    // switch cases below the chain, which must all be cases of one switch (at most one case is mounted). With `row`:
    // the one row scope that declares it among the chain's and those below the chain through switch cases.
    void checkFocus(Command const& cmd, CheckContext const& context) {
        FocusTarget target{.candidates = {}, .row = cmd.extra != nullptr};
        if (cmd.extra) {
            checkExpr(*cmd.extra, context);
            std::vector<ScopeInfo const*> found;
            for (ScopeInfo const* scope = context.scope; scope != nullptr; scope = scope->parent) {
                if (scope->row && elementIn(*scope, cmd.target) != nullptr &&
                    std::ranges::find(found, scope) == found.end()) {
                    found.push_back(scope);
                }
                rowScopesBelow(*scope, cmd.target, found);
            }
            if (found.empty()) {
                refuse(pointerAppend(cmd.path, "row"), "`row` names a row of a `forEach` or table; `" + cmd.target +
                                                           "` is not an element of a row scope this command reaches");
            }
            if (found.size() > 1) {
                refuse(cmd.path, "`" + cmd.target + "` is declared in two row scopes this command reaches, at " +
                                     elementIn(*found.at(0), cmd.target)->path + " and " +
                                     elementIn(*found.at(1), cmd.target)->path);
            }
            target.candidates.push_back(FocusCandidate{
                .path = elementIn(*found.front(), cmd.target)->element->path,
                .rows = rowDepth(found.front()),
            });
        } else if (lookup(context.scope, cmd.target) != nullptr) {
            NameEntry const& entry = require(context, cmd.target, cmd.path, {NameCategory::Element}, "an element");
            target.candidates.push_back(FocusCandidate{.path = entry.element->path, .rows = rowDepth(entry.scope)});
        } else {
            std::vector<NameEntry const*> inCases;
            for (ScopeInfo const* scope = context.scope; scope != nullptr; scope = scope->parent) {
                elementsInCases(*scope, cmd.target, inCases);
            }
            if (inCases.empty()) {
                static_cast<void>(require(context, cmd.target, cmd.path, {NameCategory::Element}, "an element"));
            }
            for (NameEntry const* const entry : inCases) {
                if (entry->scope->switchNode != inCases.front()->scope->switchNode) {
                    refuse(cmd.path, "`" + cmd.target + "` is declared in the cases of two switches, at " +
                                         inCases.front()->path + " and " + entry->path +
                                         "; both may be mounted, so a focus cannot choose");
                }
                target.candidates.push_back(
                    FocusCandidate{.path = entry->element->path, .rows = rowDepth(entry->scope)});
            }
        }
        _focusTargets.insert_or_assign(cmd.path, std::move(target));
    }

    // NOLINTNEXTLINE(bugprone-easily-swappable-parameters): the alias, then the action it runs, as a query reads.
    std::optional<ActionInfo> checkModelAction(CheckContext const& context, std::string const& alias,
                                               std::string const& action, std::string const& path) {
        std::string const modelPath = pointerAppend(path, "model");
        NameEntry const& model = require(context, alias, modelPath, {NameCategory::Model}, "a model alias");
        if (!_options->catalog.action) {
            return std::nullopt;
        }
        auto const info = _options->catalog.action(action);
        if (!info) {
            refuse(path, "the action `" + action + "` is not in the catalog");
        }
        if (info->modelType != model.model->type) {
            refuse(path, "the action `" + action + "` belongs to `" + info->modelType + "`, not `" +
                             model.model->type + "`");
        }
        return info;
    }

    // A model-backed table's `table`: its two actions, `Table.<table>` and `Table.<table>.describe`, are in the
    // catalog and belong to the alias's model type.
    // NOLINTNEXTLINE(bugprone-easily-swappable-parameters): the alias, then the table it names, as the node reads.
    void checkModelTable(CheckContext const& context, std::string const& alias, std::string const& table,
                         std::string const& path) {
        std::string const query = "Table." + table;
        if (_options->catalog.action &&
            (!_options->catalog.action(query) || !_options->catalog.action(query + ".describe"))) {
            static_cast<void>(
                require(context, alias, pointerAppend(path, "model"), {NameCategory::Model}, "a model alias"));
            refuse(path, "the table `" + table + "` is not in the catalog");
        }
        static_cast<void>(checkModelAction(context, alias, query, path));
        static_cast<void>(checkModelAction(context, alias, query + ".describe", path));
    }

    void checkScope(ScopeDecl const& decl, CheckContext const& context) {
        for (ModelDecl const& model : decl.models) {
            if (model.key) {
                checkExpr(*model.key, context);
            }
        }
        for (StateDecl const& state : decl.state) {
            if (!state.init) {
                continue;
            }
            Type const type = checkExpr(*state.init, context);
            bool const literalFits = state.init->kind != ExprKind::Literal || state.type.admits(state.init->literal);
            if (!literalFits || !compatible(state.type, type)) {
                refuse(state.init->path,
                       "the init of `" + state.name + "` does not have its type " + state.type.name());
            }
            // The type is derived now, so a state no expression reads is still checked.
            auto const derived = derivedType(*lookup(context.scope, state.name), state.name);
            static_cast<void>(derived);
        }
        for (LetDecl const& let : decl.lets) {
            auto const type = derivedType(*lookup(context.scope, let.name), let.name);
            static_cast<void>(type);
        }
        for (QueryDecl const& query : decl.queries) {
            checkQuery(query, context);
        }
        for (MutationDecl const& mutation : decl.mutations) {
            checkMutation(mutation, context);
        }
        for (WatchDecl const& watch : decl.watches) {
            checkExpr(*watch.on, context);
            checkCommands(watch.body, context);
        }
        for (CheckDecl const& check : decl.checks) {
            checkCondition(*check.assertion, context, "the `assert` of a check");
            if (check.message) {
                checkExpr(*check.message, context);
            }
            for (std::size_t index = 0; index < check.targets.size(); ++index) {
                static_cast<void>(require(context, check.targets.at(index),
                                          pointerAppend(pointerAppend(check.path, "targets"), index),
                                          {NameCategory::Element}, "an element"));
            }
        }
    }

    void checkQuery(QueryDecl const& query, CheckContext const& context) {
        auto const action = checkModelAction(context, query.model, query.action, query.path);
        // An evaluation runs no handler, so only a call can change anything.
        if (action && action->effects && !query.evaluate) {
            refuse(query.path, std::format("a query reads, so its action declares no effects; `{}` declares {}, "
                                           "so it runs as a mutation",
                                           query.action, describeEffects(*action->effects)));
        }
        if (query.body) {
            checkExpr(*query.body, context);
        }
        if (query.when) {
            checkCondition(*query.when, context, "`when`");
        }
    }

    void checkMutation(MutationDecl const& mutation, CheckContext const& context) {
        auto const action = checkModelAction(context, mutation.model, mutation.action, mutation.path);
        if (mutation.body) {
            checkExpr(*mutation.body, context);
        }
        if (mutation.serialKey) {
            CheckContext keyContext = context;
            keyContext.bindings.emplace_back("body", Type::any());
            checkExpr(*mutation.serialKey, keyContext);
        }
        if (mutation.optimistic) {
            OptimisticDecl const& decl = *mutation.optimistic;
            std::string const path = pointerAppend(mutation.path, "optimistic");
            auto const target = _queries.find(decl.query);
            if (target == _queries.end()) {
                refuse(pointerAppend(path, "query"), "`" + decl.query + "` is not a query of this document");
            }
            CheckContext bodyContext = context;
            bodyContext.bindings.emplace_back("body", Type::any());
            checkExpr(*decl.key, bodyContext);
            checkExpr(*decl.with, bodyContext);
            checkPatchAt(decl.query, decl.at, *target->second, path);
            checkOptimistic(decl, *target->second);
        }
        if (mutation.offlineQueue && action) {
            if (action->credential || !action->secretFields.empty()) {
                refuse(pointerAppend(mutation.path, "offline"),
                       "a mutation that sends a credential or returns a secret is never queued offline");
            }
        }
        checkCommands(mutation.onSuccess, context);
        checkCommands(mutation.onError, context);
    }

    // Only a catalog that knows the result is a record can say a patch misses its list; otherwise the interpreter
    // reports the miss when the patch runs.
    // NOLINTNEXTLINE(bugprone-easily-swappable-parameters): the query's name, then the member holding the list.
    void checkPatchAt(std::string const& queryName, std::string const& listMember, QueryDecl const& query,
                      std::string const& path) {
        if (!_options->catalog.action) {
            return;
        }
        auto const info = _options->catalog.action(query.action);
        if (!info || !info->listFields) {
            return;
        }
        if (listMember.empty()) {
            refuse(path, std::format("`{}` answers a record, so a patch of `{}` names the member holding its list "
                                     "with `at`",
                                     query.action, queryName));
        }
        if (std::ranges::find(*info->listFields, listMember) == info->listFields->end()) {
            refuse(pointerAppend(path, "at"),
                   std::format("the result of `{}` has no list member `{}`", query.action, listMember));
        }
    }

    void checkOptimistic(OptimisticDecl const& decl, QueryDecl const& query) {
        if (!_options->catalog.action || decl.with->kind != ExprKind::Object) {
            return;
        }
        auto const info = _options->catalog.action(query.action);
        if (!info) {
            return;
        }
        for (auto const& [field, value] : decl.with->fields) {
            if (std::ranges::find(info->calculatedFields, field) != info->calculatedFields.end()) {
                refuse(value->path,
                       std::format("`{}` is calculated by the server and is never written optimistically", field));
            }
        }
    }

    void checkPrefetch() {
        for (std::size_t index = 0; index < _doc->prefetch.size(); ++index) {
            std::string const& name = _doc->prefetch.at(index);
            std::string const path = pointerAppend("/prefetch", index);
            auto const found = std::ranges::find(_doc->scope.queries, name, &QueryDecl::name);
            if (found == _doc->scope.queries.end()) {
                refuse(path, "`" + name + "` is not a query of the screen");
            }
            if (found->body && !readsOnlyParams(*found->body)) {
                refuse(path, "the body of `" + name + "` reads more than the screen's params, so it cannot prefetch");
            }
        }
    }

    [[nodiscard]] bool readsOnlyParams(Expr const& expr) const {
        if (expr.kind == ExprKind::Ref) {
            return std::ranges::find(_doc->params, expr.name, &std::pair<std::string, Type>::first) !=
                   _doc->params.end();
        }
        if (expr.kind != ExprKind::Literal && expr.kind != ExprKind::Call && expr.kind != ExprKind::If &&
            expr.kind != ExprKind::Object && expr.kind != ExprKind::List && expr.kind != ExprKind::Get) {
            return false;
        }
        bool only = true;
        for (ExprPtr const& arg : expr.args) {
            only = only && readsOnlyParams(*arg);
        }
        for (auto const& [name, field] : expr.fields) {
            static_cast<void>(name);
            only = only && readsOnlyParams(*field);
        }
        return only;
    }

    void checkCustomizePoints() {
        std::set<std::string, std::less<>> arranged;
        for (auto const& [name, point] : _doc->customize) {
            static_cast<void>(name);
            if ((point.kind == "columns" || (point.kind == "saved" && !point.of.empty())) && _doc->view) {
                auto const found = _elements.find(point.of);
                if (found == _elements.end() || found->second->kind != "table") {
                    refuse(pointerAppend(point.path, "of"),
                           "a `" + point.kind + "` point names a table; `" + point.of + "` is not one");
                }
            }
            if (point.kind == "arrange") {
                checkArranged(point, arranged);
            }
        }
    }

    // The stored order names children by id, so the container is a stack whose every child has one, and one point
    // orders it: two would each claim the order.
    void checkArranged(CustomizePoint const& point, std::set<std::string, std::less<>>& arranged) {
        auto const found = _elements.find(point.of);
        if (found == _elements.end() || (found->second->kind != "column" && found->second->kind != "row") ||
            found->second->nodes("children").empty()) {
            refuse(pointerAppend(point.path, "of"),
                   "an `arrange` point names a column or row of this screen that has children; `" + point.of +
                       "` is not one");
        }
        for (DocNodePtr const& child : found->second->nodes("children")) {
            if (child->id.empty()) {
                refuse(pointerAppend(point.path, "of"),
                       "an `arrange` point orders its container's children by id; a child of `" + point.of +
                           "` has none");
            }
        }
        // An arranged container is drawn as a keyed list, which places its children as a list does.
        std::string const asList =
            "an arranged `" + found->second->kind + "` is drawn as a list, which takes no `align` or `justify`";
        for (std::string_view const key : {"align", "justify"}) {
            if (!found->second->name(key).empty()) {
                refuse(pointerAppend(found->second->path, key), asList);
            }
        }
        if (!arranged.insert(point.of).second) {
            refuse(pointerAppend(point.path, "of"), "`" + point.of + "` is arranged by another point");
        }
    }

    [[nodiscard]] CheckContext innerContext(DocNode const& node, CheckContext const& context) const {
        CheckContext inner = context;
        inner.scope = _innerOf.at(&node);
        return inner;
    }

    void checkNode(DocNode const& node, CheckContext const& context) {
        if (node.kind == "pageRegion") {
            refuse(node.path, "a page region belongs in a page");
        }
        if (!node.name("alignSelf").empty()) {
            refuse(pointerAppend(node.path, "alignSelf"), std::string{kAlignSelfPlaces});
        }
        if (node.flag("bleed")) {
            refuse(pointerAppend(node.path, "bleed"), std::string{kBleedPlaces});
        }
        checkElement(node, context, false);
    }

    // What `alignSelf` places a node in.
    static constexpr std::string_view kAlignSelfPlaces = "`alignSelf` places a node in its row, column or grid";

    // What `bleed` reaches past: the horizontal padding of the stack, panel or tab page that holds the node.
    static constexpr std::string_view kBleedPlaces = "`bleed` is a child's of a column, a panel or a tab";

    // Whether the nodes under @p key of @p parent may be sized by a share of its width: a row that does not wrap, a
    // splitter that is not literally vertical, and a `forEach` that is literally horizontal and does not wrap (its
    // rows are the children of a row).
    static bool takesPercent(DocNode const& parent, std::string_view key) {
        // The content of a switch case or of an overlay is mounted into a holder that fills its slot: the interpreter
        // reports a share there and makes the node fill, so the loader leaves it to the interpreter.
        if (parent.kind == "case" || parent.kind == "switch" || parent.kind == "dialog" || parent.kind == "drawer") {
            return true;
        }
        if (key != "children") {
            return false;
        }
        if (parent.kind == "row") {
            return !parent.flag("wrap");
        }
        if (parent.kind == "splitter") {
            ExprPtr const orientation = parent.prop("orientation");
            if (orientation != nullptr && orientation->kind == ExprKind::Literal) {
                std::string const* const named = orientation->literal.asString();
                return named == nullptr || *named != "vertical";
            }
            return true;
        }
        if (parent.kind == "forEach") {
            ExprPtr const axis = parent.prop("axis");
            if (axis == nullptr || axis->kind != ExprKind::Literal) {
                return false;
            }
            std::string const* const named = axis->literal.asString();
            return named != nullptr && *named == "horizontal" && !parent.flag("wrap");
        }
        return false;
    }

    // A child that places itself sits in a row, a column, a grid or a `forEach`; any other parent (a panel, a dialog,
    // a tab page) has no cross axis to place it on.
    static void checkPlacements(DocNode const& parent) {
        bool const places = parent.kind == "row" || parent.kind == "column" || parent.kind == "grid" ||
                            parent.kind == "forEach" || parent.kind == "actionBar";
        bool const bleeds = parent.kind == "column" || parent.kind == "panel" || parent.kind == "tab";
        for (auto const& [key, nodes] : parent.children) {
            for (DocNodePtr const& child : nodes) {
                if (child->kind == "pageRegion" && parent.kind != "page") {
                    refuse(child->path, "a page region belongs in a page");
                }
                if (!child->name("alignSelf").empty() && (!places || key != "children")) {
                    refuse(pointerAppend(child->path, "alignSelf"), std::string{kAlignSelfPlaces});
                }
                if (child->flag("bleed") && (!bleeds || key != "children")) {
                    refuse(pointerAppend(child->path, "bleed"), std::string{kBleedPlaces});
                }
                if (hasPercentWidth(*child) && !takesPercent(parent, key)) {
                    refuse(pointerAppend(child->path, "sizing"), std::string{kPercentPlaces});
                }
            }
        }
    }

    // A tab page's padding is `pageInset`, else `inset`, both literal steps. A page of no padding has nothing for a
    // child's `bleed` to reach past, so the document is refused rather than the flag silently doing nothing; a step
    // that is computed is left to the mount. Any node that is not a `tabs` is passed over.
    static void checkTabBleed(DocNode const& tabs) {
        if (tabs.kind != "tabs") {
            return;
        }
        auto const step = [&tabs](std::string_view key) -> std::optional<std::int64_t> {
            ExprPtr const expr = tabs.prop(key);
            if (expr == nullptr) {
                return std::nullopt;
            }
            std::int64_t const* const literal = expr->kind == ExprKind::Literal ? expr->literal.asInt() : nullptr;
            return literal != nullptr ? std::optional<std::int64_t>{*literal} : std::optional<std::int64_t>{-1};
        };
        auto const page = step("pageInset");
        auto const outer = step("inset");
        std::int64_t const effective = page.value_or(outer.value_or(0));
        if (effective != 0) {
            return;
        }
        for (DocNodePtr const& tab : tabs.partList("tabs")) {
            auto const found = tab->children.find("children");
            if (found == tab->children.end()) {
                continue;
            }
            for (DocNodePtr const& child : found->second) {
                if (child->flag("bleed")) {
                    refuse(pointerAppend(child->path, "bleed"),
                           "`bleed` reaches past the padding of the tab page, and this page has none: give the tabs "
                           "a `pageInset`, or an `inset`, above 0");
                }
            }
        }
    }

    // NOLINTNEXTLINE(readability-function-cognitive-complexity): checks one element against its kind: each branch refuses a different malformed shape and reports into the shared context
    void checkElement(DocNode const& node, CheckContext const& outer, bool part) {
        KindSpec const* const spec = findKind(node.kind, part);
        CheckContext const inner = innerContext(node, outer);
        if (auto const found = _scopeOf.find(&node); found != _scopeOf.end()) {
            // NOLINTNEXTLINE(bugprone-unchecked-optional-access): a node or case with a scope of its own has its declarations.
            checkScope(*node.scope, inner);
        }
        checkProps(node, spec, inner, outer);
        for (auto const& [key, commands] : node.events) {
            if (node.kind == "custom") {
                checkCustomEvent(node, key.substr(3), node.path);
            }
            checkCommands(commands, isInner(spec, key) ? inner : outer);
        }
        for (auto const& [chord, commands] : node.keys) {
            static_cast<void>(chord);
            checkCommands(commands, outer);
        }
        for (auto const& [key, nodes] : node.children) {
            CheckContext context = isInner(spec, key) ? inner : outer;
            context.boundaryError = (keyFlags(*spec, key) & kBoundaryError) != 0 || context.boundaryError;
            for (DocNodePtr const& child : nodes) {
                checkElement(*child, context, false);
            }
        }
        for (auto const& [key, parts] : node.parts) {
            CheckContext context = isInner(spec, key) ? inner : outer;
            // A table's menu entries read the column they open on, and a cell's entries its row and its value.
            if ((node.kind == "table" || node.kind == "fieldGrid") && (key == "cellMenu" || key == "headerMenu")) {
                context.bindings.emplace_back("column", Type::of(TypeKind::String));
                if (key == "cellMenu") {
                    context.bindings.emplace_back("row", Type::any());
                    context.bindings.emplace_back("value", Type::any());
                }
            }
            // A summary's cells read the records of the rows it summarises as `rows`, and their column's id as
            // `column`.
            if (key == "summaries") {
                context.bindings.emplace_back("rows", Type::any());
                context.bindings.emplace_back("column", Type::of(TypeKind::String));
            }
            // A grid's column reads its row's record as `row`: a display column's `value`, and an edit column's
            // `field` expression and overlays. A grid's group reads its row's record as `row` in its `by`, as its
            // columns do.
            if (key == "group" && node.kind == "table" && !node.name("form").empty()) {
                context.bindings.emplace_back("row", Type::any());
            }
            // A block's rows read its collection row as `row` in their cells' `value` and overlays; a row's `label` is
            // read once, for every collection row, so it reads none.
            if (key == "rows" && node.kind == "layoutRow") {
                context.layoutBlock = true;
            }
            if (key == "cells" && node.kind == "layoutRow" && context.layoutBlock) {
                context.bindings.emplace_back("row", Type::any());
            }
            if (key == "columns" && (node.kind == "fieldGrid" || (node.kind == "table" && !node.name("form").empty() &&
                                                                  node.partList("layoutRows").empty()))) {
                context.bindings.emplace_back("row", Type::any());
                context.gridColumns = true;
            }
            for (DocNodePtr const& child : parts) {
                checkElement(*child, context, true);
            }
        }
        checkPlacements(node);
        checkTabBleed(node);
        if (part) {
            checkPartNode(node, outer.gridColumns);
        } else {
            checkFlags(node);
            checkLayoutKeys(node);
            checkKindRules(node, outer);
            // An overlay's footer actions; a form's field components.
            checkActionButtons(node.partList("actions"), pointerAppend(node.path, "actions"));
            checkFieldComponents(node);
        }
    }

    static void checkPartNode(DocNode const& node, bool gridColumn) {
        if (node.kind == "column" || node.kind == "track") {
            checkNoPercentWidth(node);
        }
        if (node.kind == "column") {
            checkColumnPart(node, gridColumn);
        } else if (node.kind == "track") {
            checkCellPlace(node, "align");
        } else if (isMenuEntry(node.kind)) {
            checkMenuItem(node);
        }
    }

    void checkProps(DocNode const& node, KindSpec const* spec, CheckContext const& inner, CheckContext const& outer) {
        for (auto const& [key, expr] : node.props) {
            CheckContext const& context = isInner(spec, key) ? inner : outer;
            if (isCondition(spec, key)) {
                checkCondition(*expr, context, "`" + key + "`");
                continue;
            }
            Type const type = checkExpr(*expr, context);
            if (node.kind == "dateTimeInput" && key == "value") {
                checkDateValue(node, *expr, type);
            }
        }
    }

    void checkCustomEvent(DocNode const& node, std::string const& event, std::string const& path) {
        auto const found = _doc->components.find(node.name("name"));
        if (found != _doc->components.end() && !found->second.events.contains(event)) {
            refuse(pointerAppend(pointerAppend(path, "on"), event),
                   "the component `" + found->second.name + "` declares no event `" + event + "`");
        }
    }

    // What a chart's literal keys may be, and the shape of its series and lines. A computed `tone` or `yMin` is
    // checked when it is evaluated, and reported then.
    // NOLINTNEXTLINE(readability-function-cognitive-complexity): one check per chart key, in the order the keys are read.
    static void checkChart(DocNode const& node) {
        static constexpr std::size_t kMaxSeries = 4;
        static constexpr std::size_t kMaxLines = 16;
        std::span<DocNodePtr const> const series = node.partList("series");
        std::string const seriesPath = pointerAppend(node.path, "series");
        if (series.empty() || series.size() > kMaxSeries) {
            refuse(seriesPath, "a chart draws 1 to 4 series");
        }
        if (node.partList("lines").size() > kMaxLines) {
            refuse(pointerAppend(node.path, "lines"), "a chart draws at most 16 reference lines");
        }
        bool const positioned = series.front()->prop("x") != nullptr;
        for (DocNodePtr const& part : series) {
            DocNode const& one = *part;
            if (series.size() > 1 && one.prop("label") == nullptr) {
                refuse(one.path, "each of two or more series has a `label`, its name in the legend");
            }
            if ((one.prop("x") != nullptr) != positioned) {
                refuse(one.path, "`x` is on every series of a chart or on none");
            }
            checkChartTone(one);
            std::string_view const style = one.name("style");
            if (!style.empty() && style != "line" && style != "bars") {
                refuse(pointerAppend(one.path, "style"), "a series `style` is `line` or `bars`");
            }
            if (style == "bars" && one.flag("joined")) {
                refuse(pointerAppend(one.path, "joined"), "a `bars` series is not joined");
            }
        }
        for (DocNodePtr const& line : node.partList("lines")) {
            checkChartTone(*line);
            std::string_view const style = line->name("style");
            if (!style.empty() && style != "solid" && style != "dashed" && style != "dotted") {
                refuse(pointerAppend(line->path, "style"), "`style` is `solid`, `dashed` or `dotted`");
            }
        }
        if (ExprPtr const places = node.prop("yPlaces")) {
            std::int64_t const* const literal = places->kind == ExprKind::Literal ? places->literal.asInt() : nullptr;
            if (literal == nullptr || *literal < 0 || std::cmp_greater(*literal, math::kMaxDecimalPlaces)) {
                refuse(places->path, "`yPlaces` is a literal integer from 0 to 18");
            }
        }
        ExprPtr const low = node.prop("yMin");
        ExprPtr const high = node.prop("yMax");
        if (low && high && low->kind == ExprKind::Literal && high->kind == ExprKind::Literal) {
            std::optional<Number> const lowNumber = numeric(low->literal);
            std::optional<Number> const highNumber = numeric(high->literal);
            if (lowNumber && highNumber && highNumber->value < lowNumber->value) {
                refuse(low->path, "`yMin` is above `yMax`");
            }
        }
    }

    // A literal `tone` of a chart's series or line names one of the five tones; `null` marks nothing.
    static void checkChartTone(DocNode const& part) {
        ExprPtr const tone = part.prop("tone");
        if (!tone || tone->kind != ExprKind::Literal || tone->literal.isNull()) {
            return;
        }
        static constexpr std::array<std::string_view, 5> kTones = {"neutral", "info", "ok", "warn", "err"};
        std::string const* const name = tone->literal.asString();
        if (name == nullptr || !listed(kTones, *name)) {
            refuse(tone->path, "`tone` is `neutral`, `info`, `ok`, `warn` or `err`");
        }
    }

    // A flag that needs a key beside it.
    static void checkFlags(DocNode const& node) {
        if (node.flag("showLabel") && node.prop("label") == nullptr) {
            refuse(pointerAppend(node.path, "showLabel"), "`showLabel` shows the `label`, and the node gives none");
        }
    }

    // NOLINTNEXTLINE(readability-function-cognitive-complexity): one rule per element kind, each reading the same node and reporting to the shared check context in document order
    void checkKindRules(DocNode const& node, CheckContext const& context) {
        if (node.kind == "custom") {
            checkCustomProps(node);
        } else if (node.kind == "tabs") {
            checkTabIds(node);
            checkTabsInset(node);
        } else if (node.kind == "chart") {
            checkChart(node);
        } else if (node.kind == "page") {
            checkPage(node);
        } else if (node.kind == "localeScope") {
            checkLocaleScope(node);
        } else if (node.kind == "customize" && !_doc->customize.contains(node.name("point"))) {
            refuse(pointerAppend(node.path, "point"),
                   "`" + std::string{node.name("point")} + "` is not a customization point of this document");
        }
        checkKindValues(node);
        checkSheet(node);
        std::string_view const model = node.name("model");
        std::string_view const table = node.kind == "table" ? node.name("table") : std::string_view{};
        bool const fixedGrid = node.kind == "table" && !node.partList("layoutRows").empty();
        if (fixedGrid && (!model.empty() || !table.empty())) {
            refuse(pointerAppend(node.path, "layoutRows"),
                   "a fixed grid's rows are its `layoutRows`; a model's table takes none");
        }
        if (!table.empty()) {
            if (model.empty()) {
                refuse(node.path, "`table` names one of a model's tables; give the table its `model`");
            }
            if (!node.name("action").empty()) {
                refuse(node.path, "a table names a `table` or an `action`, not both");
            }
            checkModelTable(context, std::string{model}, std::string{table}, node.path);
        } else if (!model.empty()) {
            if (node.name("action").empty()) {
                refuse(node.path, "a `" + node.kind + "` with a `model` names its `action`");
            }
            checkModelAction(context, std::string{model}, std::string{node.name("action")}, node.path);
            if (node.kind == "form") {
                checkFormDerive(context, node);
            }
        } else if (fixedGrid) {
            checkFixedGrid(node);
        } else if (node.kind == "table" && !node.name("form").empty()) {
            checkFormGrid(node);
        } else if (node.kind == "table" && !node.prop("rows")) {
            refuse(node.path, "a table takes `rows`, or a `model` and `action`");
        }
        if (node.kind == "table") {
            checkCellSelection(node);
            checkTableSelection(node);
            checkSelectColumn(node);
            checkTableSort(node);
            checkTablePaging(node);
            checkTableSortBound(node);
            checkTableConfigurable(node);
            checkBandParts(node);
            checkSummaryParts(node);
            checkTableGroup(node);
            checkEditMode(node);
            checkColumnGroups(node);
            checkRowsTableKeys(node);
            checkColumnKinds(node);
            if (!model.empty() && table.empty()) {
                checkActionTable(node);
            }
        }
    }

    // A form's derive action is an action of the form's model; `deriveDebounce` is a literal count of milliseconds.
    // The debounce times a round's evaluation as well as its derive call, so a form that names no `derive` takes it
    // too.
    void checkFormDerive(CheckContext const& context, DocNode const& node) {
        if (std::string_view const derive = node.name("derive"); !derive.empty()) {
            static_cast<void>(checkModelAction(context, std::string{node.name("model")}, std::string{derive},
                                               pointerAppend(node.path, "derive")));
        }
        if (ExprPtr const debounce = node.prop("deriveDebounce")) {
            std::int64_t const* const millis = debounce->kind == ExprKind::Literal
                                                   ? debounce->literal.asInt()
                                                   : static_cast<std::int64_t const*>(nullptr);
            if (millis == nullptr || *millis < 0) {
                refuse(debounce->path, "`deriveDebounce` is a literal count of milliseconds, 0 or more");
            }
        }
    }

    // A grid of a form: a table over a form node's collection field, in place of `rows`. Each column's `field` names a
    // member of the field's row type, read from the form action's schema when the catalog has it.
    void checkFormGrid(DocNode const& node) const {
        if (node.prop("rows")) {
            refuse(pointerAppend(node.path, "form"), "a table takes `rows` or a `form`, not both");
        }
        std::string_view const field = node.name("field");
        if (field.empty()) {
            refuse(node.path, "a table over a `form` names its `field`, the form's collection");
        }
        auto const form = _elements.find(node.name("form"));
        if (form == _elements.end() || form->second->kind != "form") {
            refuse(pointerAppend(node.path, "form"),
                   "`" + std::string{node.name("form")} + "` is not a form of this document");
        }
        checkGridPart(node, *form->second, field, pointerAppend(node.path, "field"));
    }

    // A fixed grid: a table whose rows are its `layoutRows`, in place of `rows`, each cell a field of the form `form`
    // names, an expression or a text. Its rows are fixed, so it takes nothing that orders, groups, adds or summarises
    // rows. Keys are unique in the grid, a block's rows included, since a block row's key joins its collection row's
    // key and its own, and two collections' row keys may meet. A cell's `field` is a path of the form's fields, or in
    // a block of the collection's row type, read from the form action's schema when the catalog has it.
    // NOLINTNEXTLINE(readability-function-cognitive-complexity): one refusal per rule of a fixed grid's parts.
    void checkFixedGrid(DocNode const& node) const {
        if (node.prop("rows")) {
            refuse(pointerAppend(node.path, "layoutRows"),
                   "a fixed grid's `layoutRows` are in place of `rows`; give it one of them");
        }
        if (!node.name("field").empty()) {
            refuse(pointerAppend(node.path, "field"),
                   "a fixed grid's cells name their own fields; a table with `layoutRows` takes no `field`");
        }
        for (std::string_view const key :
             {std::string_view{"sort"}, std::string_view{"group"}, std::string_view{"summaries"},
              std::string_view{"rowSpans"}, std::string_view{"appendRows"}, std::string_view{"rowsFixed"},
              std::string_view{"key"}}) {
            bool const given = node.prop(key) != nullptr || !node.partList(key).empty() ||
                               node.flag(key, false) == node.flag(key, true);
            if (given) {
                refuse(
                    pointerAppend(node.path, key),
                    "a fixed grid's rows are fixed, in the order of its `layoutRows`, and its spans are its cells'; "
                    "it takes no `" +
                        std::string{key} + "`");
            }
        }
        if (node.prop("columns") != nullptr) {
            refuse(pointerAppend(node.path, "columns"), "a fixed grid's `columns` are a list of column parts");
        }
        std::span<DocNodePtr const> const columns = node.partList("columns");
        for (DocNodePtr const& column : columns) {
            if (!column->name("field").empty() || column->prop("field") != nullptr ||
                column->prop("value") != nullptr) {
                refuse(column->path, "a fixed grid's cells name their fields and values; its columns name neither");
            }
        }
        std::string_view const formId = node.name("form");
        DocNode const* form = nullptr;
        if (!formId.empty()) {
            auto const found = _elements.find(formId);
            if (found == _elements.end() || found->second->kind != "form") {
                refuse(pointerAppend(node.path, "form"),
                       "`" + std::string{formId} + "` is not a form of this document");
            }
            form = found->second;
        }
        std::optional<forms::FormModel> const model = form != nullptr ? fixedGridModel(*form) : std::nullopt;
        std::set<std::string, std::less<>> keys;
        for (DocNodePtr const& part : node.partList("layoutRows")) {
            std::string_view const forEach = part->name("forEach");
            if (forEach.empty()) {
                checkLayoutRow(*part, columns, form,
                               model ? std::span{model->fields()} : std::span<forms::FieldSpec const>{},
                               model.has_value(), keys);
                continue;
            }
            if (!part->name("key").empty() || !part->partList("cells").empty() || part->prop("label") != nullptr) {
                refuse(part->path, "a block's `forEach` repeats its `rows`; it takes no `key`, `label` or `cells`");
            }
            if (form == nullptr) {
                refuse(pointerAppend(part->path, "forEach"),
                       "a block repeats rows of a form's collection; give the table its `form`");
            }
            std::span<forms::FieldSpec const> members;
            if (model) {
                auto const array = std::ranges::find(model->fields(), forEach, &forms::FieldSpec::name);
                if (array == model->fields().end() || array->kind != forms::FieldKind::ObjectArray) {
                    refuse(pointerAppend(part->path, "forEach"),
                           "`" + std::string{forEach} + "` is not a list of records of the form");
                }
                members = array->children;
            }
            if (part->partList("rows").empty()) {
                refuse(pointerAppend(part->path, "rows"), "a block repeats its `rows`, and gives none");
            }
            for (DocNodePtr const& row : part->partList("rows")) {
                if (!row->name("forEach").empty()) {
                    refuse(pointerAppend(row->path, "forEach"), "a block's rows are rows; a block holds no block");
                }
                checkLayoutRow(*row, columns, form, members, model.has_value(), keys);
            }
        }
        if (form == nullptr) {
            return;
        }
        for (DocNodePtr const& part : node.partList("layoutRows")) {
            std::string_view const forEach = part->name("forEach");
            std::span<DocNodePtr const> const rows = forEach.empty() ? std::span{&part, 1} : part->partList("rows");
            for (DocNodePtr const& row : rows) {
                for (DocNodePtr const& cells : row->partList("cells")) {
                    for (DocNodePtr const& cell : cells->partList("cells")) {
                        std::string_view const field = cell->name("field");
                        ExprPtr const unit = cell->prop("unit");
                        std::string const* const literal =
                            unit != nullptr && unit->kind == ExprKind::Literal ? unit->literal.asString() : nullptr;
                        if (!field.empty() && literal != nullptr) {
                            noteUnit(
                                *form,
                                forEach.empty() ? std::string{field} : std::string{forEach} + "." + std::string{field},
                                *literal, unit->path);
                        }
                    }
                }
            }
        }
    }

    // A literal `unit` overlay on a field of a form: a field is entered in one unit, so two overlays in the document
    // that give it different literal units are refused. A unit an expression gives is read when it is shown, and not
    // compared. A field of a collection's rows is named `<collection>.<member>`.
    void noteUnit(DocNode const& form, std::string const& field, std::string const& unit,
                  std::string const& where) const {
        if (unit.empty()) {
            return;
        }
        auto const [given, added] = _unitsGiven.try_emplace(form.id + "\x1F" + field, unit);
        if (!added && given->second != unit) {
            refuse(where, "`" + field + "` is given the units `" + given->second + "` and `" + unit +
                              "` by two overlays; a field is entered in one unit");
        }
    }

    // The form model a fixed grid's cells are checked against: its form action's schema, when the catalog has it.
    [[nodiscard]] std::optional<forms::FormModel> fixedGridModel(DocNode const& form) const {
        if (!_options->catalog.schema) {
            return std::nullopt;
        }
        std::string const action{form.name("action")};
        std::optional<std::string> const schema = _options->catalog.schema(action);
        if (!schema) {
            return std::nullopt;
        }
        auto model = forms::FormModel::fromSchema(action, *schema);
        return model ? std::optional<forms::FormModel>{std::move(*model)} : std::nullopt;
    }

    // One row of a fixed grid: its key, unique in the grid, and its cells, each under a column's id and naming one of
    // `field`, `value` and `text`; a `field` needs the form, and names a field of @p fields when @p known.
    // NOLINTNEXTLINE(readability-function-cognitive-complexity): one refusal per rule of a cell.
    static void checkLayoutRow(DocNode const& row, std::span<DocNodePtr const> columns, DocNode const* form,
                               std::span<forms::FieldSpec const> fields, bool known,
                               std::set<std::string, std::less<>>& keys) {
        std::string const key{row.name("key")};
        if (key.empty()) {
            refuse(row.path, "a fixed grid's row names its `key`, or repeats `rows` for each row of a `forEach`");
        }
        if (!row.partList("rows").empty()) {
            refuse(pointerAppend(row.path, "rows"), "a row's `rows` repeat for each row of a `forEach`; give it one");
        }
        if (key.contains('/')) {
            refuse(pointerAppend(row.path, "key"),
                   "the row key `" + key + "` holds a `/`, which joins a block row's keys; give it another");
        }
        if (!keys.insert(key).second) {
            refuse(pointerAppend(row.path, "key"), "the row `" + key + "` is declared twice in the grid");
        }
        for (DocNodePtr const& cells : row.partList("cells")) {
            for (DocNodePtr const& cell : cells->partList("cells")) {
                if (std::ranges::none_of(columns,
                                         [&cell](DocNodePtr const& column) { return column->id == cell->id; })) {
                    refuse(cell->path,
                           "a fixed grid's cells are named by the table's column ids; `" + cell->id + "` is not one");
                }
                std::string_view const field = cell->name("field");
                int const given = (field.empty() ? 0 : 1) + (cell->prop("value") != nullptr ? 1 : 0) +
                                  (cell->names.contains("text") ? 1 : 0);
                if (given != 1) {
                    refuse(cell->path,
                           "a fixed grid's cell edits its `field`, or shows its `value` or its `text`: "
                           "one of them");
                }
                if (!field.empty() && form == nullptr) {
                    refuse(pointerAppend(cell->path, "field"),
                           "a cell that edits a field names the form's; give the table its `form`");
                }
                if (!field.empty() && known && !fieldAt(fields, field)) {
                    refuse(pointerAppend(cell->path, "field"), "`" + std::string{field} +
                                                                   "` is not a field of the form of `" +
                                                                   std::string{form->name("action")} + "`");
                }
                for (std::string_view const span : {std::string_view{"rows"}, std::string_view{"columns"}}) {
                    if (ExprPtr const count = cell->prop(span)) {
                        std::int64_t const* const literal = count->kind == ExprKind::Literal
                                                                ? count->literal.asInt()
                                                                : static_cast<std::int64_t const*>(nullptr);
                        if (literal == nullptr || *literal < 1) {
                            refuse(count->path, "a cell's `" + std::string{span} + "` is a literal count, 1 or more");
                        }
                    }
                }
                for (std::string_view const overlay : {std::string_view{"decimals"}, std::string_view{"unit"},
                                                       std::string_view{"options"}, std::string_view{"placeholder"}}) {
                    if (ExprPtr const set = cell->prop(overlay); set != nullptr && field.empty()) {
                        refuse(set->path, "`" + std::string{overlay} +
                                              "` is an overlay of the field a cell edits; "
                                              "this cell edits no field");
                    }
                }
            }
        }
    }

    // Whether a dotted path names a field below @p fields.
    [[nodiscard]] static bool fieldAt(std::span<forms::FieldSpec const> fields, std::string_view path) {
        while (!path.empty()) {
            std::size_t const dot = path.find('.');
            std::string_view const name = path.substr(0, dot);
            path = dot == std::string_view::npos ? std::string_view{} : path.substr(dot + 1);
            auto const member = std::ranges::find(fields, name, &forms::FieldSpec::name);
            if (member == fields.end()) {
                return false;
            }
            fields = member->children;
        }
        return true;
    }

    // What a grid takes beyond its table, on a `table` over a form or a field overlay's `grid` part: `enterMoves` and
    // `walk` by name, and columns that each edit a member, named by `field`, or show a `value`. A `field` must name a
    // member of the collection's row type, read from the form action's schema when the catalog has it.
    // NOLINTNEXTLINE(readability-function-cognitive-complexity): the grid's two options, each column's member or value and the field lookup in one pass over the parts, refusing in document order
    void checkGridPart(DocNode const& part, DocNode const& form, std::string_view field,
                       std::string const& where) const {
        for (std::string_view const key : {std::string_view{"enterMoves"}, std::string_view{"walk"}}) {
            std::string_view const given = part.name(key);
            bool const known =
                given.empty() || (key == "enterMoves" ? given == "down" || given == "right" || given == "none"
                                                      : given == "rows" || given == "columns");
            if (!known) {
                refuse(pointerAppend(part.path, key), key == "enterMoves" ? "`enterMoves` is `down`, `right` or `none`"
                                                                          : "`walk` is `rows` or `columns`");
            }
        }
        if (part.flag("appendRows") && part.flag("rowsFixed")) {
            refuse(pointerAppend(part.path, "appendRows"), "a grid with `rowsFixed` adds no rows; drop `appendRows`");
        }
        if (part.kind == "fieldGrid") {
            checkBandParts(part);
            checkSummaryParts(part);
        }
        for (DocNodePtr const& column : part.partList("columns")) {
            if (column->name("field").empty() && column->prop("field") == nullptr &&
                column->prop("value") == nullptr) {
                refuse(column->path, "a grid's column edits the member its `field` names, or shows its `value`");
            }
            if (std::string_view const member = column->name("field"); !member.empty()) {
                noteUnit(form, std::string{field} + "." + std::string{member}, std::string{column->name("unit")},
                         pointerAppend(column->path, "unit"));
            }
        }
        if (!_options->catalog.schema) {
            return;
        }
        std::string const action{form.name("action")};
        std::optional<std::string> const schema = _options->catalog.schema(action);
        if (!schema) {
            return;
        }
        auto const model = forms::FormModel::fromSchema(action, *schema);
        if (!model) {
            return;
        }
        auto const array = std::ranges::find(model->fields(), field, &forms::FieldSpec::name);
        if (array == model->fields().end() || array->kind != forms::FieldKind::ObjectArray) {
            refuse(where, "`" + std::string{field} + "` is not a list of records of the form of `" + action + "`");
        }
        for (DocNodePtr const& column : part.partList("columns")) {
            std::string_view const member = column->name("field");
            if (!member.empty() && std::ranges::none_of(array->children, [member](forms::FieldSpec const& child) {
                    return child.name == member;
                })) {
                refuse(pointerAppend(column->path, "field"),
                       "`" + std::string{member} + "` names no member of `" + std::string{field} + "`");
            }
        }
    }

    // A table over a typed list action pages through the one request member marked `x-table`, and builds its
    // description from its `columns` or the action's view document, since a catalog carries no reply schema.
    void checkActionTable(DocNode const& node) const {
        std::string const action{node.name("action")};
        if (_options->catalog.schema) {
            std::optional<std::string> const schema = _options->catalog.schema(action);
            std::vector<std::string> const members = schema ? tableQueryMembers(*schema) : std::vector<std::string>{};
            if (members.empty()) {
                refuse(node.path, "a table over an action needs a query member marked x-table");
            }
            if (members.size() > 1) {
                refuse(node.path, "a table over an action needs one query member marked x-table; `" + action +
                                      "` marks " + std::to_string(members.size()));
            }
        }
        if (_options->catalog.view && node.partList("columns").empty() && !_options->catalog.view(action)) {
            refuse(node.path, "a table over an action needs its columns, or a view document for the action");
        }
    }

    // Whether a node gives the common key @p key.
    static bool gives(DocNode const& node, std::string_view key) {
        return node.props.contains(key) || node.names.contains(key) || node.flags.contains(key) ||
               node.parts.contains(key) || (key == "keys" && !node.keys.empty()) || (key == "id" && !node.id.empty());
    }

    // The common keys a page node does not take, each refused where it is written.
    static void refuseKeys(DocNode const& node, std::span<std::string_view const> keys, std::string_view why) {
        for (std::string_view const key : keys) {
            if (gives(node, key)) {
                refuse(pointerAppend(node.path, key), "`" + std::string{key} + "` " + std::string{why});
            }
        }
    }

    // A page holds regions and nothing else, each valid on the sheet; the sheet sizes itself.
    static void checkPage(DocNode const& node) {
        static constexpr std::array<std::string_view, 9> kRefused = {
            "sizing", "minWidth", "maxWidth", "minHeight", "maxHeight", "bleed", "drag", "drop", "autofocus",
        };
        refuseKeys(node, kRefused, "is refused on a page: the sheet is sized by its geometry");
        if (node.name("alignSelf") == "stretch") {
            refuse(pointerAppend(node.path, "alignSelf"), "a page is never stretched; `alignSelf` places the sheet");
        }
        PageGeometry const page = pageGeometryOf(node);
        for (DocNodePtr const& child : node.nodes("children")) {
            if (child->kind != "pageRegion") {
                refuse(child->path, "a page holds page regions and nothing else");
            }
            static constexpr std::array<std::string_view, 11> kRegionRefused = {
                "sizing", "minWidth", "maxWidth", "minHeight", "maxHeight", "alignSelf",
                "bleed",  "span",     "drag",     "drop",      "autofocus",
            };
            refuseKeys(*child, kRegionRefused, "is refused on a page region: its geometry names its size");
            static_cast<void>(regionGeometryOf(*child, page));
        }
    }

    // A scope is not a widget, so it takes none of the keys every widget takes.
    static void checkLocaleScope(DocNode const& node) {
        for (KeySpec const& key : kCommonKeys) {
            if (gives(node, key.key)) {
                refuse(pointerAppend(node.path, key.key),
                       "`" + std::string{key.key} +
                           "` is refused on a locale scope: it is not a widget; wrap it in a "
                           "node that has one");
            }
        }
    }

    // A dialog's `sheet`: a drawer already fills the height on a phone and takes none. A `dismissLabel` labels the
    // dismiss control of a sheet or of a `closeButton`, so it needs one of them (a drawer has no sheet).
    static void checkSheet(DocNode const& node) {
        if (node.kind == "drawer" && node.prop("sheet")) {
            refuse(pointerAppend(node.path, "sheet"), "`sheet` is a dialog's; a drawer already fills the height");
        }
        bool const sheet = node.kind == "dialog" && node.prop("sheet");
        if ((node.kind == "dialog" || node.kind == "drawer") && node.prop("dismissLabel") && !sheet &&
            !node.prop("closeButton")) {
            refuse(pointerAppend(node.path, "dismissLabel"),
                   "`dismissLabel` labels a sheet's dismiss control or a `closeButton`; give the " + node.kind +
                       (node.kind == "dialog" ? " `sheet` or `closeButton`" : " `closeButton`"));
        }
    }

    // A sortable column is named by its id, which a sort key gives; `sortBy` says how a sortable column orders. A
    // literal `sort` names sortable columns, each once, with a direction, and `onSortChange` needs a header that
    // sorts. A model-backed table's engine sorts and its description says which columns do, so neither `sortable`
    // nor `sortBy` belongs on one.
    static void checkTableSort(DocNode const& node) {
        bool const modelBacked = !node.name("model").empty();
        std::set<std::string, std::less<>> sortable;
        for (DocNodePtr const& column : node.partList("columns")) {
            bool const sorts = column->flag("sortable");
            if (sorts && modelBacked) {
                refuse(pointerAppend(column->path, "sortable"),
                       "a model-backed table's engine sorts; `sortable` is for a table over `rows`");
            }
            if (sorts && column->id.empty()) {
                refuse(pointerAppend(column->path, "sortable"), "a sortable column needs an `id`, which a sort names");
            }
            if (column->prop("sortBy") != nullptr && !sorts) {
                refuse(pointerAppend(column->path, "sortBy"),
                       "`sortBy` says how a sortable column orders the rows; give the column `sortable`");
            }
            if (sorts) {
                sortable.insert(column->id);
            }
        }
        if (node.event("onSortChange") != nullptr && sortable.empty() && !modelBacked) {
            refuse(pointerAppend(node.path, "onSortChange"),
                   "`onSortChange` runs when a header sorts the table; give a column `sortable`");
        }
        ExprPtr const sort = node.prop("sort");
        if (sort != nullptr && sort->kind == ExprKind::Literal && !modelBacked && !sort->literal.isNull()) {
            checkLiteralSort(sort->literal, pointerAppend(node.path, "sort"), sortable);
        }
    }

    // A literal sort: a list of keys, each naming one of @p sortable once, with a direction.
    static void checkLiteralSort(Value const& sort, std::string const& where,
                                 std::set<std::string, std::less<>> const& sortable) {
        ValueList const* const keys = sort.asList();
        if (keys == nullptr) {
            refuse(
                where,
                R"(a table's sort is a list of keys, each {"column": <a sortable column's id>, "direction": "asc" or "desc"})");
        }
        std::set<std::string, std::less<>> named;
        for (Value const& key : *keys) {
            Value const* const column = key.field("column");
            Value const* const direction = key.field("direction");
            std::string const* const columnId = column == nullptr ? nullptr : column->asString();
            std::string const* const way = direction == nullptr ? nullptr : direction->asString();
            if (columnId == nullptr || way == nullptr || (*way != "asc" && *way != "desc")) {
                refuse(where, R"(a sort key is {"column": <a sortable column's id>, "direction": "asc" or "desc"})");
            }
            if (!sortable.contains(*columnId)) {
                refuse(where, "the sort names `" + *columnId + "`, which is not a sortable column");
            }
            if (!named.insert(*columnId).second) {
                refuse(where, "the sort names `" + *columnId + "` twice");
            }
        }
    }

    // A band part as `BandInfo`: its label, unit, literal list of column ids and the bands one level down.
    // NOLINTNEXTLINE(misc-no-recursion): a band's bands are bands one level down.
    static table::BandInfo bandOf(DocNode const& part) {
        table::BandInfo band{.label = std::string{part.name("label")},
                             .unit = std::string{part.name("unit")},
                             .columns = {},
                             .bands = {}};
        ExprPtr const members = part.prop("columns");
        ValueList const* const list =
            members != nullptr && members->kind == ExprKind::Literal ? members->literal.asList() : nullptr;
        if (list == nullptr) {
            refuse(pointerAppend(part.path, "columns"), "a band's `columns` is a literal list of its columns' ids");
        }
        for (Value const& member : *list) {
            std::string const* const columnId = member.asString();
            if (columnId == nullptr) {
                refuse(pointerAppend(part.path, "columns"),
                       "a band's `columns` is a literal list of its columns' ids");
            }
            band.columns.push_back(*columnId);
        }
        for (DocNodePtr const& child : part.partList("bands")) {
            band.bands.push_back(bandOf(*child));
        }
        return band;
    }

    // Header bands: nested at most three levels, each over columns of the table, a child over its parent's columns
    // and a column in one band per level (`table::bandRefusal`). A model's table names its columns in its
    // description, so only a table that declares its column parts has its ids checked here.
    static void checkBandParts(DocNode const& node) {
        auto const parts = node.partList("bands");
        if (parts.empty()) {
            return;
        }
        std::vector<table::BandInfo> bands;
        for (DocNodePtr const& part : parts) {
            bands.push_back(bandOf(*part));
        }
        std::vector<std::string> known;
        if (node.name("model").empty() && node.prop("columns") == nullptr) {
            for (DocNodePtr const& column : node.partList("columns")) {
                known.push_back(column->id);
            }
        }
        if (std::optional<std::string> const refused = table::bandRefusal(bands, known)) {
            refuse(pointerAppend(node.path, "bands"), *refused);
        }
    }

    // Summary rows, on a table and on its group part: a summary is a feature of a table over held rows, so a
    // model-backed table refuses both. Each has a key, unique in its list, a `position` of `bottom` or `top`, and
    // cells named by the table's column ids.
    // NOLINTNEXTLINE(readability-function-cognitive-complexity): checks each summary's key, position and cells in one pass over the list; the uniqueness set is shared
    static void checkSummaryParts(DocNode const& node) {
        bool const modelBacked = !node.name("model").empty();
        auto const check = [&node, modelBacked](std::span<DocNodePtr const> summaries, std::string const& where) {
            if (summaries.empty()) {
                return;
            }
            if (modelBacked) {
                refuse(where,
                       "a summary row summarises rows the client holds; a model-backed table refuses "
                       "`summaries`, as it refuses `group`");
            }
            std::set<std::string, std::less<>> keys;
            auto const columns = node.partList("columns");
            for (DocNodePtr const& summary : summaries) {
                std::string const key{summary->name("key")};
                if (key.empty() || !keys.insert(key).second) {
                    refuse(pointerAppend(summary->path, "key"), key.empty()
                                                                    ? std::string{"a summary names its `key`"}
                                                                    : "the summary `" + key + "` is declared twice");
                }
                std::string_view const position = summary->name("position");
                if (!position.empty() && position != "bottom" && position != "top") {
                    refuse(pointerAppend(summary->path, "position"), "a summary's `position` is `bottom` or `top`");
                }
                if (node.prop("columns") != nullptr || columns.empty()) {
                    continue;
                }
                for (DocNodePtr const& cells : summary->partList("cells")) {
                    for (auto const& [column, expr] : cells->props) {
                        static_cast<void>(expr);
                        if (std::ranges::none_of(columns,
                                                 [&column](DocNodePtr const& part) { return part->id == column; })) {
                            refuse(
                                pointerAppend(cells->path, column),
                                "a summary's cells are named by the table's column ids; `" + column + "` is not one");
                        }
                    }
                }
            }
        };
        check(node.partList("summaries"), pointerAppend(node.path, "summaries"));
        for (DocNodePtr const& group : node.partList("group")) {
            check(group->partList("summaries"), pointerAppend(group->path, "summaries"));
        }
    }

    // A table over `rows` draws its own group headings; a model-backed table's engine shapes its rows.
    static void checkTableGroup(DocNode const& node) {
        if (!node.name("model").empty() && !node.partList("group").empty()) {
            refuse(pointerAppend(node.path, "group"),
                   "a model-backed table's engine shapes its rows; `group` is for a table over `rows`");
        }
    }

    // When a model's table commits its cells' edits: `cell` or `row`. Only a model's table commits through edit
    // actions.
    static void checkEditMode(DocNode const& node) {
        std::string_view const mode = node.name("editMode");
        if (mode.empty()) {
            return;
        }
        if (node.name("model").empty() || node.name("table").empty()) {
            refuse(pointerAppend(node.path, "editMode"),
                   "`editMode` chooses how a model's table commits its cells; this table is not a model's table");
        }
        if (mode != "cell" && mode != "row") {
            refuse(pointerAppend(node.path, "editMode"),
                   "`editMode` is `cell` or `row`, not `" + std::string{mode} + "`");
        }
    }

    // A table over `rows` declares its column groups: each 2 to 16 of its column parts, of one literal kind, and a
    // column in at most one group. A model's table has its groups from its description.
    // NOLINTNEXTLINE(readability-function-cognitive-complexity): checks each group's size, kind and column membership in one pass; the "column in one group" set is shared
    static void checkColumnGroups(DocNode const& node) {
        auto const groups = node.partList("groups");
        if (groups.empty()) {
            return;
        }
        if (!node.prop("rows")) {
            refuse(
                pointerAppend(node.path, "groups"),
                "a model's table declares its column groups in its description; `groups` is for a table over `rows`");
        }
        auto const columns = node.partList("columns");
        std::set<std::string, std::less<>> grouped;
        std::set<std::string, std::less<>> ids;
        for (DocNodePtr const& group : groups) {
            std::string const where = pointerAppend(group->path, "columns");
            if (!ids.insert(group->id).second) {
                refuse(pointerAppend(group->path, "id"), "the group `" + group->id + "` is declared twice");
            }
            ExprPtr const members = group->prop("columns");
            ValueList const* const list =
                members != nullptr && members->kind == ExprKind::Literal ? members->literal.asList() : nullptr;
            if (list == nullptr) {
                refuse(where, "a group's `columns` is a literal list of its columns' ids");
            }
            if (list->size() < 2 || list->size() > 16) {
                refuse(where, "a group has 2 to 16 columns");
            }
            std::optional<std::string_view> kind;
            for (Value const& member : *list) {
                std::string const* const columnId = member.asString();
                auto const part = columnId == nullptr
                                      ? columns.end()
                                      : std::ranges::find_if(columns, [columnId](DocNodePtr const& column) {
                                            return column->id == *columnId;
                                        });
                if (part == columns.end()) {
                    refuse(where, "a group's columns are the ids of the table's column parts");
                }
                if (!grouped.insert(*columnId).second) {
                    refuse(where, "the column `" + *columnId + "` is in another group");
                }
                std::string_view const named = (*part)->name("kind");
                if (kind && *kind != named) {
                    refuse(where, "a group's columns hold one kind of value");
                }
                kind = named;
            }
        }
    }

    // A table over `rows` pages itself by `pageSize`, at `page`; a model-backed table's engine pages.
    static void checkTablePaging(DocNode const& node) {
        ExprPtr const size = node.prop("pageSize");
        ExprPtr const page = node.prop("page");
        if (!node.name("model").empty() && (size != nullptr || page != nullptr)) {
            refuse(pointerAppend(node.path, size != nullptr ? "pageSize" : "page"),
                   "a model-backed table's engine pages; `pageSize` and `page` are for a table over `rows`");
        }
        if (page != nullptr && size == nullptr) {
            refuse(pointerAppend(node.path, "page"),
                   "`page` picks a page of `pageSize` rows; give the table `pageSize`");
        }
        if (size != nullptr && size->kind == ExprKind::Literal && !size->literal.isNull()) {
            std::int64_t const* const rows = size->literal.asInt();
            if (rows == nullptr || *rows < 1) {
                refuse(pointerAppend(node.path, "pageSize"), "a page holds a positive whole number of rows");
            }
        }
    }

    // A `rows` table's sort bound: a literal 1 to 32. A model's table has its bound from its description.
    static void checkTableSortBound(DocNode const& node) {
        ExprPtr const bound = node.prop("maxSortKeys");
        if (bound == nullptr) {
            return;
        }
        std::string const path = pointerAppend(node.path, "maxSortKeys");
        if (!node.name("model").empty()) {
            refuse(path, "`maxSortKeys` is a `rows` table's; a model's table carries its bound in its description");
        }
        std::int64_t const* const keys = bound->kind == ExprKind::Literal ? bound->literal.asInt() : nullptr;
        if (keys == nullptr || *keys < 1 || *keys > 32) {
            refuse(path, "`maxSortKeys` is 1 to 32");
        }
    }

    // What a table lets its user change about its columns: a literal `true`, `false`, or a list of the four.
    static void checkTableConfigurable(DocNode const& node) {
        ExprPtr const expr = node.prop("configurable");
        if (expr == nullptr) {
            return;
        }
        std::string const path = pointerAppend(node.path, "configurable");
        std::string_view const shape =
            R"(`configurable` is a literal `true`, `false`, or a list of "reorder", "hide", "pin" and "resize")";
        if (expr->kind != ExprKind::Literal) {
            refuse(path, std::string{shape});
        }
        if (expr->literal.asBool() != nullptr) {
            return;
        }
        ValueList const* const names = expr->literal.asList();
        if (names == nullptr) {
            refuse(path, std::string{shape});
        }
        static constexpr std::array<std::string_view, 4> kConfigurable = {"reorder", "hide", "pin", "resize"};
        for (Value const& item : *names) {
            std::string const* const name = item.asString();
            if (name == nullptr) {
                refuse(path, std::string{shape});
            }
            if (!listed(kConfigurable, *name)) {
                refuse(path, "`configurable` names `" + *name + "`; it lists reorder, hide, pin and resize");
            }
        }
    }

    // The keys of a model-backed table that a table over `rows` would accept and ignore.
    static void checkRowsTableKeys(DocNode const& node) {
        if (!node.prop("rows")) {
            return;
        }
        if (node.prop("filters") != nullptr || !node.name("filters").empty()) {
            refuse(pointerAppend(node.path, "filters"),
                   "`filters` belongs to a model-backed table; filter `rows` with `filter`");
        }
        if (node.prop("highlightKey") != nullptr) {
            refuse(pointerAppend(node.path, "highlightKey"), "`highlightKey` links a model-backed table to a chart");
        }
        // Its arrangement is kept under the table's id, so a table with none persists nothing, and says so when it
        // asks to.
        if (ExprPtr const persist = node.prop("persist"); persist != nullptr && node.id.empty()) {
            bool const* const flag = persist->kind == ExprKind::Literal ? persist->literal.asBool() : nullptr;
            if (flag == nullptr || *flag) {
                refuse(pointerAppend(node.path, "persist"),
                       "a table over `rows` needs an `id` to persist its arrangement");
            }
        }
    }

    // A column part's literal `kind`, on a table over `rows` or over a typed list action: one of the seven a
    // document names.
    static void checkColumnKinds(DocNode const& node) {
        static constexpr std::array<std::string_view, 7> kColumnKinds = {"text", "integer",  "decimal", "number",
                                                                         "date", "dateTime", "bool"};
        for (DocNodePtr const& column : node.partList("columns")) {
            std::string_view const kind = column->name("kind");
            if (!kind.empty() && !listed(kColumnKinds, kind)) {
                refuse(pointerAppend(column->path, "kind"),
                       "`kind` is one of text, integer, decimal, number, date, dateTime, bool");
            }
        }
    }

    // The select column draws a `multiple` table's selection, so it needs a literal `multiple`; its header's name
    // needs the column.
    static void checkSelectColumn(DocNode const& node) {
        bool const drawn = node.flag("selectColumn");
        if (drawn) {
            ExprPtr const mode = node.prop("selectionMode");
            std::string const* const literal =
                mode != nullptr && mode->kind == ExprKind::Literal ? mode->literal.asString() : nullptr;
            if (literal == nullptr || *literal != "multiple") {
                refuse(pointerAppend(node.path, "selectColumn"),
                       "a select column draws a table's selection of several rows; give it `selectionMode` "
                       "`multiple`");
            }
        }
        if (!drawn && node.prop("selectAllLabel") != nullptr) {
            refuse(pointerAppend(node.path, "selectAllLabel"),
                   "`selectAllLabel` names the select column's header "
                   "box; give the table `selectColumn`");
        }
    }

    // `selectionMode: "cells"` selects a rectangle of cells, which a grid of a form does by default and only; a
    // model's table is in server mode and selects rows. `appendRows` and `rowsFixed` say which row operations a grid
    // offers.
    static void checkCellSelection(DocNode const& node) {
        bool const grid = !node.name("form").empty() || !node.partList("layoutRows").empty();
        ExprPtr const mode = node.prop("selectionMode");
        std::string const* const literal =
            mode != nullptr && mode->kind == ExprKind::Literal ? mode->literal.asString() : nullptr;
        std::string const where = pointerAppend(node.path, "selectionMode");
        if (literal != nullptr && *literal == "cells" && !node.name("model").empty()) {
            refuse(where,
                   "`selectionMode` `cells` selects cells of a table held on the client; a model's table "
                   "selects rows");
        }
        if (literal != nullptr && *literal == "cells" && !grid) {
            refuse(where, "`selectionMode` `cells` is a grid's; this table is not a grid of a form");
        }
        if (grid && mode != nullptr && (literal == nullptr || *literal != "cells")) {
            refuse(where, "a grid of a form selects cells: its `selectionMode` is `cells`");
        }
        for (std::string_view const key : {std::string_view{"appendRows"}, std::string_view{"rowsFixed"}}) {
            if (node.flag(key, false) == node.flag(key, true) && !grid) {
                refuse(pointerAppend(node.path, key),
                       "`" + std::string{key} + "` is a grid's; this table is not a grid of a form");
            }
        }
    }

    // A table's `selection` and `onSelectionChange` need rows the user can select, and rows the document holds: a
    // model-backed table's engine owns its selection. A grid of a form selects cells, so it takes them without a
    // `selectionMode`.
    static void checkTableSelection(DocNode const& node) {
        std::string_view key;
        if (node.prop("selection") != nullptr) {
            key = "selection";
        } else if (node.event("onSelectionChange") != nullptr) {
            key = "onSelectionChange";
        } else {
            return;
        }
        std::string const where = pointerAppend(node.path, key);
        if (!node.name("model").empty()) {
            refuse(where, "a model-backed table's engine owns its selection; `" + std::string{key} +
                              "` is for a table over `rows`");
        }
        if (!node.name("form").empty() || !node.partList("layoutRows").empty()) {
            return;
        }
        ExprPtr const mode = node.prop("selectionMode");
        std::string const* const literal =
            mode != nullptr && mode->kind == ExprKind::Literal ? mode->literal.asString() : nullptr;
        if (mode == nullptr || (literal != nullptr && *literal == "none")) {
            refuse(where, "a table with `" + std::string{key} +
                              "` selects rows; give it `selectionMode` `single` or `multiple`");
        }
    }

    // A tabs node that names its shown tab, or reports a pick, keys every tab by a unique id; a literal `selected`
    // names one of them. A literal `null` is no binding.
    static void checkTabIds(DocNode const& node) {
        ExprPtr const selected = node.prop("selected");
        bool const bound = selected != nullptr && (selected->kind != ExprKind::Literal || !selected->literal.isNull());
        bool const keyed = bound || node.event("onChoose") != nullptr;
        std::set<std::string, std::less<>> ids;
        for (DocNodePtr const& tab : node.partList("tabs")) {
            if (tab->id.empty()) {
                if (keyed) {
                    refuse(tab->path, "a tab of a tabs node with `selected` or `onChoose` has an `id`");
                }
                continue;
            }
            if (!ids.insert(tab->id).second) {
                refuse(pointerAppend(tab->path, "id"),
                       "the tab id `" + tab->id + "` is used twice; each tab has its own");
            }
        }
        if (bound && selected->kind == ExprKind::Literal) {
            std::string const* const named = selected->literal.asString();
            if (named == nullptr || !ids.contains(*named)) {
                refuse(pointerAppend(node.path, "selected"), "`selected` names no tab of this node");
            }
        }
    }

    // A tabs node's `inset` and `pageInset` are literal spacing steps, 0 to 5: the strip and the pages are laid out
    // once.
    static void checkTabsInset(DocNode const& node) {
        for (std::string_view const key : {"inset", "pageInset"}) {
            ExprPtr const inset = node.prop(key);
            if (inset == nullptr) {
                continue;
            }
            std::int64_t const* const step = inset->kind == ExprKind::Literal ? inset->literal.asInt() : nullptr;
            if (step == nullptr || *step < 0 || *step > 5) {
                refuse(pointerAppend(node.path, key),
                       "`" + std::string{key} + "` is a literal spacing step from 0 to 5");
            }
        }
    }

    // A custom node names a component of the document and gives only the props it declares.
    void checkCustomProps(DocNode const& node) const {
        auto const found = _doc->components.find(node.name("name"));
        if (found == _doc->components.end()) {
            refuse(pointerAppend(node.path, "name"),
                   "`" + std::string{node.name("name")} + "` is not a component of this document");
        }
        for (DocNodePtr const& props : node.partList("props")) {
            for (auto const& [prop, value] : props->props) {
                if (!found->second.props.contains(prop)) {
                    refuse(value->path, "the component `" + found->first + "` declares no prop `" + prop + "`");
                }
            }
        }
    }

    // A stack's literal `padding` list is two spacing steps, vertical then horizontal, each 0 to 5; a panel's is one
    // step. A computed padding is read at mount.
    static void checkPadding(DocNode const& node) {
        ExprPtr const padding = node.prop("padding");
        if (padding == nullptr || padding->kind != ExprKind::Literal) {
            return;
        }
        ValueList const* const list = padding->literal.asList();
        if (list == nullptr) {
            return;
        }
        if (node.kind == "panel") {
            refuse(pointerAppend(node.path, "padding"), "a panel's `padding` is one spacing step, not a list");
        }
        // The largest spacing step, as `kMaxSpacingStep` (view.hpp) names it.
        static constexpr std::int64_t kMostSteps = 5;
        bool const steps = list->size() == 2 && std::ranges::all_of(*list, [](Value const& step) {
                               std::int64_t const* const whole = step.asInt();
                               return whole != nullptr && *whole >= 0 && *whole <= kMostSteps;
                           });
        if (!steps) {
            refuse(pointerAppend(node.path, "padding"),
                   "a `padding` list is two spacing steps, vertical then horizontal, each from 0 to 5");
        }
    }

    // The closed-set keys of a button, and the shape and style pair that cannot go together.
    static void checkButtonValues(DocNode const& node) {
        static constexpr std::array<std::string_view, 5> kEmphasis = {
            "normal", "primary", "danger", "ghost", "dangerOutline",
        };
        checkChoice(node, "emphasis", kEmphasis);
        static constexpr std::array<std::string_view, 2> kStyles = {"push", "item"};
        checkChoice(node, "style", kStyles);
        static constexpr std::array<std::string_view, 3> kShapes = {"default", "round", "square"};
        checkChoice(node, "shape", kShapes);
        if (node.name("shape") == "square" && node.name("style") == "item") {
            refuse(pointerAppend(node.path, "shape"),
                   "an item fills its line, so it has no square outline; `square` is a push button's");
        }
        static constexpr std::array<std::string_view, 2> kSizes = {"normal", "small"};
        checkChoice(node, "size", kSizes);
        if (std::string_view const weight = node.name("weight"); !weight.empty() && !weightNamed(weight)) {
            refuse(pointerAppend(node.path, "weight"),
                   "`" + std::string{weight} + "` is not a font weight name, such as `semi-bold`");
        }
        checkEntryContent(node);
    }

    // The keys of a kind that take one of a closed set of values, and the pairs of keys that need each other.
    static void checkKindValues(DocNode const& node) {
        if (node.kind == "column" || node.kind == "row" || node.kind == "panel") {
            checkPadding(node);
        }
        if ((node.kind == "column" || node.kind == "row") && node.flag("headJoined") && !node.flag("pageEdge")) {
            refuse(pointerAppend(node.path, "headJoined"),
                   "`headJoined` needs `pageEdge`: an inset page has a margin between it and the head");
        }
        if (node.kind == "button") {
            checkButtonValues(node);
        } else if (node.kind == "grid") {
            checkGrid(node);
        } else if (node.kind == "actionBar") {
            checkActionBar(node);
        } else if (node.kind == "column" && node.flags.contains("wrap")) {
            refuse(pointerAppend(node.path, "wrap"), "only a row wraps; a column has no lines to continue on");
        } else if (node.kind == "row" && node.flag("divided") && node.flag("wrap")) {
            refuse(pointerAppend(node.path, "divided"),
                   "a wrapping row has no single line to rule between; `divided` is for a row that does not `wrap`");
        } else if (node.kind == "forEach") {
            checkForEachWrap(node);
        } else if (node.kind == "menu") {
            checkMenu(node);
        } else if (node.kind == "badge" && node.prop("removeLabel") != nullptr && node.event("onRemove") == nullptr) {
            refuse(pointerAppend(node.path, "removeLabel"),
                   "`removeLabel` names a removable badge's remove control; the badge gives no `onRemove`");
        } else if (node.kind == "textInput") {
            static constexpr std::array<std::string_view, 3> kCommit = {"change", "blur", "enter"};
            checkChoice(node, "commitOn", kCommit);
            static constexpr std::array<std::string_view, 2> kInputEmphases = {"normal", "prominent"};
            checkChoice(node, "emphasis", kInputEmphases);
            static constexpr std::array<std::string_view, 3> kInputAligns = {"start", "center", "end"};
            checkChoice(node, "align", kInputAligns);
            checkMultilineExtras(node);
        } else if (node.kind == "select") {
            static constexpr std::array<std::string_view, 2> kStale = {"clear", "keep"};
            checkChoice(node, "onStale", kStale);
            checkSelectStyle(node);
        } else if (node.kind == "text") {
            checkText(node);
        } else if (node.kind == "banner") {
            checkBanner(node);
        } else if (node.kind == "badge") {
            static constexpr std::array<std::string_view, 3> kBadgeStyles = {"filled", "outline", "quiet"};
            checkChoice(node, "style", kBadgeStyles);
            static constexpr std::array<std::string_view, 2> kBadgeShapes = {"default", "round"};
            checkChoice(node, "shape", kBadgeShapes);
        } else if (node.kind == "keyValue") {
            static constexpr std::array<std::string_view, 2> kCaptionPlaces = {"inline", "stacked"};
            checkChoice(node, "fieldLayout", kCaptionPlaces);
            checkCaptionWidth(node, node.name("fieldLayout") != "stacked");
            checkItemSpans(node.prop("items"));
        } else if (node.kind == "table") {
            checkCellPlace(node, "valign");
        }
        if (node.kind == "form" || node.kind == "collection" || node.kind == "wizard") {
            checkFormColumns(node);
            static constexpr std::array<std::string_view, 2> kFieldLayouts = {"stacked", "inline"};
            checkChoice(node, "fieldLayout", kFieldLayouts);
            checkCaptionWidth(node, node.name("fieldLayout") == "inline");
        }
        static constexpr std::array<std::string_view, 4> kAlign = {"start", "center", "end", "stretch"};
        static constexpr std::array<std::string_view, 4> kJustify = {"start", "center", "end", "between"};
        checkChoice(node, "align", kAlign);
        checkChoice(node, "justify", kJustify);
        checkChoice(node, "alignSelf", kAlign);
        checkSurfaceValues(node);
    }

    // Each form field drawn by a host component.
    void checkFieldComponents(DocNode const& form) const {
        if (form.kind != "form") {
            return;
        }
        for (DocNodePtr const& record : form.partList("fields")) {
            for (DocNodePtr const& field : record->partList("fields")) {
                if (!field->name("component").empty()) {
                    checkFieldComponent(*field);
                }
                if (std::span<DocNodePtr const> const grid = field->partList("grid"); !grid.empty()) {
                    // A literal `widget: "grid"` only: a grid part on any other control would be dropped silently.
                    ExprPtr const widget = field->prop("widget");
                    std::string const* const named =
                        widget != nullptr && widget->kind == ExprKind::Literal ? widget->literal.asString() : nullptr;
                    if (named == nullptr || *named != "grid") {
                        refuse(grid.front()->path,
                               "a field's `grid` gives a grid widget its columns; give it `widget: \"grid\"`");
                    }
                    checkGridPart(*grid.front(), form, field->id, field->path);
                    checkTableConfigurable(*grid.front());
                }
            }
        }
    }

    // A form field drawn by a host component: a declared one with a `value` prop and a `change` event, given only
    // props it declares, and not beside a `widget`, which would choose the control too.
    void checkFieldComponent(DocNode const& field) const {
        std::string_view const name = field.name("component");
        std::string const where = pointerAppend(field.path, "component");
        auto const found = _doc->components.find(name);
        if (found == _doc->components.end()) {
            refuse(where, "`" + std::string{name} + "` is not a component of this document");
        }
        Component const& component = found->second;
        if (!component.props.contains("value") || !component.events.contains("change")) {
            refuse(where, "the component `" + component.name +
                              "` draws a form field, so it declares a prop `value` and an event `change`");
        }
        if (field.prop("widget") != nullptr) {
            refuse(where, "`component` and `widget` both choose the field's control; give one");
        }
        for (DocNodePtr const& props : field.partList("props")) {
            for (auto const& [prop, value] : props->props) {
                if (!component.props.contains(prop)) {
                    refuse(value->path, "the component `" + component.name + "` declares no prop `" + prop + "`");
                }
            }
        }
    }

    // The document's top-level parts: a screen head's status badges, actions, menu entries and tools, and the app
    // document's navigation badges.
    [[nodiscard]] std::vector<DocNodePtr> headParts() const {
        std::vector<DocNodePtr> parts = _doc->status;
        parts.insert(parts.end(), _doc->actions.begin(), _doc->actions.end());
        parts.insert(parts.end(), _doc->menu.begin(), _doc->menu.end());
        parts.insert(parts.end(), _doc->tools.begin(), _doc->tools.end());
        parts.insert(parts.end(), _doc->navigation.begin(), _doc->navigation.end());
        return parts;
    }

    // The head's tools: each id once, and each one its kind's.
    static void checkHeadTools(std::span<DocNodePtr const> tools) {
        std::set<std::string_view> ids;
        for (DocNodePtr const& tool : tools) {
            if (!ids.insert(tool->id).second) {
                refuse(pointerAppend(tool->path, "id"),
                       "the head tool id `" + tool->id + "` is used twice; each tool has its own");
            }
            checkHeadTool(*tool);
        }
    }

    // One head tool: a kind of the two, only its kind's keys, a select's options in a dropdown, a literal width.
    static void checkHeadTool(DocNode const& tool) {
        static constexpr std::array<std::string_view, 4> kSearchOnly = {
            "value",
            "placeholder",
            "onChange",
            "onSubmit",
        };
        static constexpr std::array<std::string_view, 4> kSelectOnly = {"options", "selected", "style", "onChoose"};
        std::string_view const kind = tool.name("kind");
        if (kind != "search" && kind != "select") {
            refuse(pointerAppend(tool.path, "kind"),
                   "`" + std::string{kind} + "` is not one of the values of `kind`: search or select");
        }
        bool const search = kind == "search";
        for (std::string_view const key : search ? kSelectOnly : kSearchOnly) {
            if (tool.prop(key) || tool.event(key) != nullptr || !tool.name(key).empty()) {
                refuse(pointerAppend(tool.path, key),
                       "`" + std::string{key} + "` is a " + (search ? "select" : "search") + " tool's");
            }
        }
        if (!search && !tool.prop("options")) {
            refuse(tool.path, "a select tool has `options`");
        }
        if (std::string_view const style = tool.name("style"); !style.empty() && style != "dropdown") {
            refuse(pointerAppend(tool.path, "style"),
                   "`" + std::string{style} + "` is not one of the values of `style`: dropdown");
        }
        ExprPtr const width = tool.prop("width");
        std::int64_t const* const units = width && width->kind == ExprKind::Literal
                                              ? width->literal.asInt()
                                              : static_cast<std::int64_t const*>(nullptr);
        if (width && (units == nullptr || *units < 1)) {
            refuse(pointerAppend(tool.path, "width"), "`width` is a literal whole number of units, 1 or more");
        }
    }

    // The head menu's entries, at every level: an entry that is chosen has an id, unique across the whole menu, since
    // `ScreenHead::choose` names it by that id; a separator or a submenu is never chosen and takes none.
    // NOLINTNEXTLINE(misc-no-recursion): a submenu is a menu one level down.
    static void checkHeadMenu(std::span<DocNodePtr const> entries, std::set<std::string_view>& ids) {
        for (DocNodePtr const& entry : entries) {
            bool const submenu = !entry->partList("items").empty();
            if (entry->flag("separator") || submenu) {
                if (!entry->id.empty()) {
                    refuse(pointerAppend(entry->path, "id"),
                           submenu ? "a submenu is opened, not chosen, and takes no `id`"
                                   : "a separator is a line between entries and takes no other key");
                }
                checkHeadMenu(entry->partList("items"), ids);
                continue;
            }
            if (entry->id.empty()) {
                refuse(entry->path, "a head menu entry has an `id`, which choosing it names");
            }
            if (!ids.insert(entry->id).second) {
                refuse(pointerAppend(entry->path, "id"),
                       "the head menu entry id `" + entry->id + "` is used twice; each entry has its own");
            }
        }
    }

    // A list of `actionButton` parts: each id once, and each emphasis one of a button's.
    static void checkActionButtons(std::span<DocNodePtr const> buttons, std::string const& path) {
        static constexpr std::array<std::string_view, 5> kEmphasis = {
            "normal", "primary", "danger", "ghost", "dangerOutline",
        };
        std::set<std::string_view> ids;
        for (DocNodePtr const& button : buttons) {
            if (!ids.insert(button->id).second) {
                refuse(pointerAppend(button->path, "id"),
                       "the action id `" + button->id + "` is used twice in " + path + "; each action has its own");
            }
            checkChoice(*button, "emphasis", kEmphasis);
        }
    }

    // A literal sizing of one dimension: `"content"`, `{"fixed": n}` (n of 0 or more), `{"stretch": w}` (w of 1 or
    // more) or `{"percent": n}` (n from 1 to 100). Answers the fixed amount, if it is one.
    static std::optional<std::int64_t> sizingDimension(Value const& value, std::string const& path) {
        static constexpr std::string_view kForms =
            "`sizing` is `\"content\"`, `{\"fixed\": n}`, `{\"stretch\": w}`, `{\"percent\": n}` (n from 1 to 100), "
            "or a "
            "record of `width` and `height` of them";
        if (std::string const* const text = value.asString(); text != nullptr && *text == "content") {
            return std::nullopt;
        }
        ValueRecord const* const record = value.asRecord();
        if (record != nullptr && record->size() == 1) {
            auto const& [form, amount] = *record->begin();
            std::int64_t const* const units = amount.asInt();
            if (form == "fixed" && units != nullptr && *units >= 0) {
                return *units;
            }
            if (form == "stretch" && units != nullptr && *units >= 1) {
                return std::nullopt;
            }
            if (form == "percent" && units != nullptr && *units >= 1 && *units <= 100) {
                return std::nullopt;
            }
        }
        refuse(path, std::string{kForms});
    }

    // Whether a literal sizing dimension is `{"percent": n}`.
    static bool isPercent(Value const& value) {
        ValueRecord const* const record = value.asRecord();
        return record != nullptr && record->size() == 1 && record->begin()->first == "percent";
    }

    // Whether a literal `sizing` gives the node a percent width: as the single form, or as the `width` of a record.
    static bool hasPercentWidth(DocNode const& node) {
        ExprPtr const sizing = node.prop("sizing");
        if (sizing == nullptr || sizing->kind != ExprKind::Literal) {
            return false;
        }
        ValueRecord const* const record = sizing->literal.asRecord();
        if (record != nullptr && (record->contains("width") || record->contains("height"))) {
            auto const found = record->find("width");
            return found != record->end() && isPercent(found->second);
        }
        return isPercent(sizing->literal);
    }

    // A table column's or a grid track's literal `width` is no share: a track is sized by the grid, a column by its
    // table.
    static void checkNoPercentWidth(DocNode const& node) {
        ExprPtr const width = node.prop("width");
        if (width != nullptr && width->kind == ExprKind::Literal && isPercent(width->literal)) {
            refuse(width->path, std::string{kPercentPlaces});
        }
    }

    // What a percent sizes: a width along a row or a horizontal splitter's pane.
    static constexpr std::string_view kPercentPlaces =
        "`percent` sizes a width along a row or a horizontal splitter; nothing here takes a share";

    // A node's size keys: each bound a literal integer of 0 or more, a minimum no more than its maximum, no bound on
    // an overlay or a custom node; and a literal `sizing` one of the forms the interpreter reads, with a fixed amount
    // inside its dimension's bounds. A sizing that is not literal is read at mount.
    static void checkLayoutKeys(DocNode const& node) { checkSizing(node, literalBounds(node)); }

    // The size bounds a node gives, `minWidth`, `maxWidth`, `minHeight` and `maxHeight` in that order: literal
    // integers of 0 or more, no minimum above its maximum, none on an overlay or a custom node.
    static std::array<std::optional<std::int64_t>, 4> literalBounds(DocNode const& node) {
        static constexpr std::array<std::string_view, 4> kBounds = {"minWidth", "maxWidth", "minHeight", "maxHeight"};
        std::array<std::optional<std::int64_t>, 4> bounds{};
        for (std::size_t index = 0; index < kBounds.size(); ++index) {
            std::string_view const key = kBounds.at(index);
            ExprPtr const bound = node.prop(key);
            if (bound == nullptr) {
                continue;
            }
            if (node.kind == "dialog" || node.kind == "drawer") {
                refuse(bound->path, "an overlay is sized by the frontend, not by its container");
            }
            if (node.kind == "custom") {
                refuse(bound->path,
                       "a custom node carries no common properties, so it takes no `" + std::string{key} + "`");
            }
            std::int64_t const* const units =
                bound->kind == ExprKind::Literal ? bound->literal.asInt() : static_cast<std::int64_t const*>(nullptr);
            if (units == nullptr || *units < 0) {
                refuse(bound->path, "`" + std::string{key} + "` is a literal integer of 0 or more, in backend units");
            }
            bounds.at(index) = *units;
        }
        for (std::size_t axis = 0; axis < 2; ++axis) {
            auto const& minimum = bounds.at(2 * axis);
            auto const& maximum = bounds.at((2 * axis) + 1);
            if (minimum && maximum && *minimum > *maximum) {
                refuse(pointerAppend(node.path, kBounds.at(2 * axis)),
                       "`" + std::string{kBounds.at(2 * axis)} + "` is more than `" +
                           std::string{kBounds.at((2 * axis) + 1)} + "`");
            }
        }
        return bounds;
    }

    // Whether a literal sizing dimension's value is `{"stretch": w}` (as opposed to `"content"` or `{"fixed": n}`,
    // which both `sizingDimension` reports the same way -- as "no fixed amount"). An overlay is sized by the
    // frontend, not by its container, so stretching it would silently change how every *other* sibling in its own
    // parent layout is justified; checked ahead of `sizingDimension` itself, which does not distinguish the two.
    static bool isStretch(Value const& value) {
        ValueRecord const* const record = value.asRecord();
        return record != nullptr && record->size() == 1 && record->begin()->first == "stretch";
    }

    // An overlay (`dialog`/`drawer`) refuses a literal `sizing` that stretches either dimension, single-value or
    // record form: an overlay is sized by the frontend, not by its container, so stretching it would silently change
    // how every *other* sibling in its own parent layout is justified.
    static void checkOverlaySizing(DocNode const& node, ExprPtr const& sizing) {
        if (node.kind != "dialog" && node.kind != "drawer") {
            return;
        }
        ValueRecord const* const record = sizing->literal.asRecord();
        bool const stretches =
            isStretch(sizing->literal) ||
            (record != nullptr && (record->contains("width") || record->contains("height")) &&
             std::ranges::any_of(*record, [](auto const& entry) { return isStretch(entry.second); }));
        if (stretches) {
            refuse(sizing->path,
                   "an overlay is sized by the frontend, not by its container; give it `{\"fixed\": n}` instead "
                   "of `{\"stretch\": w}`");
        }
    }

    // A literal `sizing` gives a percent as neither a height nor the width of an overlay.
    static void checkPercentSizing(DocNode const& node, ExprPtr const& sizing) {
        ValueRecord const* const record = sizing->literal.asRecord();
        if (record != nullptr && record->contains("height") && isPercent(record->at("height"))) {
            refuse(sizing->path, std::string{kPercentPlaces});
        }
        if (hasPercentWidth(node) && (node.kind == "dialog" || node.kind == "drawer")) {
            refuse(sizing->path, std::string{kPercentPlaces});
        }
    }

    // A literal `sizing` is one of its forms, and a fixed amount lies inside its dimension's @p bounds.
    static void checkSizing(DocNode const& node, std::array<std::optional<std::int64_t>, 4> const& bounds) {
        ExprPtr const sizing = node.prop("sizing");
        if (sizing == nullptr || sizing->kind != ExprKind::Literal) {
            return;
        }
        checkOverlaySizing(node, sizing);
        checkPercentSizing(node, sizing);
        std::optional<std::int64_t> width;
        std::optional<std::int64_t> height;
        ValueRecord const* const record = sizing->literal.asRecord();
        if (record != nullptr && (record->contains("width") || record->contains("height"))) {
            for (auto const& [dimension, value] : *record) {
                if (dimension == "width") {
                    width = sizingDimension(value, sizing->path);
                } else if (dimension == "height") {
                    height = sizingDimension(value, sizing->path);
                } else {
                    refuse(sizing->path, "a `sizing` record has `width` and `height`; `" + dimension + "` is neither");
                }
            }
        } else {
            width = sizingDimension(sizing->literal, sizing->path);
        }
        for (auto const& [fixed, axis] : {std::pair{width, std::size_t{0}}, std::pair{height, std::size_t{1}}}) {
            auto const& minimum = bounds.at(2 * axis);
            auto const& maximum = bounds.at((2 * axis) + 1);
            if (fixed && (*fixed < minimum.value_or(*fixed) || *fixed > maximum.value_or(*fixed))) {
                refuse(sizing->path, std::string{"a fixed size has no room to bound: the "} +
                                         (axis == 0 ? "width" : "height") + " is fixed outside its bounds");
            }
        }
    }

    // A text's or a table column's literal `role` is one of the roles (`title` is `heading`'s alias); a computed one
    // is read at mount, and one of no known name is reported there.
    static void checkTextRole(DocNode const& node) {
        ExprPtr const role = node.prop("role");
        if (role == nullptr || role->kind != ExprKind::Literal) {
            return;
        }
        static constexpr std::array<std::string_view, 14> kRoles = {
            "normal", "muted",   "heading", "title",    "section", "error", "success",
            "mono",   "display", "small",   "overline", "caption", "label", "headline",
        };
        std::string const* const name = role->literal.asString();
        if (name == nullptr) {
            refuse(pointerAppend(node.path, "role"), "`role` is a name");
        }
        if (!listed(kRoles, *name)) {
            refuse(pointerAppend(node.path, "role"), "`" + *name + "` is not one of the values of `role`");
        }
    }

    // A dialog's, drawer's or form's error policy, submit mode and description spans.
    static void checkSurfaceValues(DocNode const& node) {
        if (node.kind != "dialog" && node.kind != "drawer" && node.kind != "form") {
            return;
        }
        static constexpr std::array<std::string_view, 3> kPolicies = {"touched", "submit", "always"};
        checkChoice(node, "showErrors", kPolicies);
        // Absent keeps the schema's own mode, so a misspelt mode would silently do the same.
        static constexpr std::array<std::string_view, 2> kSubmitModes = {"explicit", "auto"};
        checkChoice(node, "submit", kSubmitModes);
        checkSpans(node.prop("descriptionSpans"));
    }

    // A text's role and spans; a text carries no description, the line itself is the text.
    static void checkText(DocNode const& node) {
        checkTextRole(node);
        static constexpr std::array<std::string_view, 3> kTextAligns = {"start", "center", "end"};
        checkChoice(node, "align", kTextAligns);
        checkSpans(node.prop("spans"));
        if (node.prop("description") != nullptr) {
            refuse(pointerAppend(node.path, "description"), "a text is not described; put the line in the text");
        }
        if (node.prop("labelHint") != nullptr) {
            refuse(pointerAppend(node.path, "labelHint"), "a text has no caption to hint after; put it in the text");
        }
    }

    // A banner's action: its style is one of three; the link style places the action in the message's runs, and a
    // stretched action is a button on a line of its own, so it needs an action and is no link.
    static void checkBanner(DocNode const& node) {
        static constexpr std::array<std::string_view, 3> kStyles = {"ghost", "button", "link"};
        checkChoice(node, "actionStyle", kStyles);
        bool const link = node.name("actionStyle") == "link";
        checkSpans(node.prop("spans"), link);
        if (node.flag("actionStretch")) {
            if (link) {
                refuse(pointerAppend(node.path, "actionStretch"),
                       "`actionStretch` puts a button on a line of its own; a `link` action sits in the message");
            }
            if (!node.parts.contains("action")) {
                refuse(pointerAppend(node.path, "actionStretch"), "`actionStretch` stretches the banner's `action`");
            }
        }
    }

    // What an `action` run is told where the loader refuses it.
    static constexpr std::string_view kActionRunPlaces =
        "an `action` run is the link of a banner whose `actionStyle` is "
        "\"link\"; at most one run of its spans takes it";

    // A span list written out: each literal run a string, each record run a `text` with at most the flags `strong`
    // and `mono`, each literal flag a boolean. A computed list, or a computed element, is checked as it is drawn. A
    // run may carry `action` where @p actionAllowed, once at most.
    static void checkSpans(ExprPtr const& spans, bool actionAllowed = false) {
        if (spans == nullptr) {
            return;
        }
        int actions = 0;
        if (spans->kind == ExprKind::Literal) {
            ValueList const* const runs = spans->literal.asList();
            if (runs == nullptr) {
                refuse(spans->path, "`spans` is a list of runs");
            }
            for (Value const& run : *runs) {
                checkLiteralRun(run, spans->path, actionAllowed, actions);
            }
            return;
        }
        if (spans->kind != ExprKind::List) {
            return;
        }
        for (ExprPtr const& run : spans->args) {
            if (run->kind == ExprKind::Literal) {
                checkLiteralRun(run->literal, run->path, actionAllowed, actions);
            } else if (run->kind == ExprKind::Object) {
                checkRunRecord(*run, actionAllowed, actions);
            }
        }
    }

    // A button's `children`: only on a list entry, and display kinds only, at any depth, so the entry stays one
    // control with one activation. A list of what is allowed keeps a kind added later out until it is judged.
    static void checkEntryContent(DocNode const& button) {
        auto const found = button.children.find("children");
        if (found == button.children.end() || found->second.empty()) {
            return;
        }
        if (button.name("style") != "item") {
            refuse(pointerAppend(button.path, "children"),
                   "only a list entry holds content; a push button has a label");
        }
        for (DocNodePtr const& child : found->second) {
            checkReadOnlyContent(*child);
        }
    }

    // NOLINTNEXTLINE(misc-no-recursion): an entry's content is a tree of nodes.
    static void checkReadOnlyContent(DocNode const& node) {
        static constexpr std::array<std::string_view, 12> kDisplay = {
            "text",   "badge",  "keyValue", "progress", "steps",   "busy",
            "spacer", "column", "row",      "grid",     "forEach", "switch",
        };
        static constexpr std::string_view kReason =
            "an entry's content is read-only; put the control beside the entry";
        if (!listed(kDisplay, node.kind)) {
            refuse(node.path, std::string{kReason});
        }
        for (std::string_view const key : {std::string_view{"autofocus"}}) {
            if (node.prop(key) != nullptr) {
                refuse(pointerAppend(node.path, key), std::string{kReason});
            }
        }
        if (!node.keys.empty()) {
            refuse(pointerAppend(node.path, "keys"), std::string{kReason});
        }
        for (std::string_view const part : {std::string_view{"drag"}, std::string_view{"drop"}}) {
            if (!node.partList(part).empty()) {
                refuse(pointerAppend(node.path, part), std::string{kReason});
            }
        }
        for (auto const& [key, nodes] : node.children) {
            for (DocNodePtr const& child : nodes) {
                checkReadOnlyContent(*child);
            }
        }
        for (auto const& [key, parts] : node.parts) {
            for (DocNodePtr const& part : parts) {
                for (auto const& [childKey, nodes] : part->children) {
                    for (DocNodePtr const& child : nodes) {
                        checkReadOnlyContent(*child);
                    }
                }
            }
        }
    }

    // A select's style written as a constant: one of the three it may be. A computed one is read as it is drawn.
    static void checkSelectStyle(DocNode const& node) {
        ExprPtr const style = node.prop("style");
        if (style == nullptr || style->kind != ExprKind::Literal) {
            return;
        }
        static constexpr std::array<std::string_view, 3> kStyles = {"dropdown", "radio", "segmented"};
        std::string const* const name = style->literal.asString();
        if (name == nullptr) {
            refuse(pointerAppend(node.path, "style"), "`style` is a name");
        }
        if (!listed(kStyles, *name)) {
            refuse(pointerAppend(node.path, "style"), "`" + *name + "` is not one of the values of `style`");
        }
    }

    // A literal multiline text input with a unit or an icon: a text area holds no number and draws no icon.
    static void checkMultilineExtras(DocNode const& node) {
        ExprPtr const mode = node.prop("mode");
        std::string const* const name =
            mode != nullptr && mode->kind == ExprKind::Literal ? mode->literal.asString() : nullptr;
        if (name == nullptr || *name != "multiline") {
            if (node.flags.contains("wrap")) {
                refuse(pointerAppend(node.path, "wrap"),
                       "only a multiline input wraps; this one has no lines to wrap");
            }
            return;
        }
        for (std::string_view const key : {
                 std::string_view{"unit"},
                 std::string_view{"icon"},
                 std::string_view{"prefix"},
                 std::string_view{"decimals"},
             }) {
            if (node.prop(key) != nullptr) {
                refuse(pointerAppend(node.path, key), "a multiline input shows no " + std::string{key});
            }
        }
        if (!node.name("align").empty()) {
            refuse(pointerAppend(node.path, "align"), "a multiline input's text starts at the leading edge");
        }
        if (!node.name("emphasis").empty() && node.name("emphasis") != "normal") {
            refuse(pointerAppend(node.path, "emphasis"), "a multiline input's size is its own; it takes no emphasis");
        }
    }

    // The spans of each key-value item written as an `obj` in a `list`.
    static void checkItemSpans(ExprPtr const& items) {
        if (items == nullptr || items->kind != ExprKind::List) {
            return;
        }
        for (ExprPtr const& item : items->args) {
            if (item->kind != ExprKind::Object) {
                continue;
            }
            for (auto const& [key, value] : item->fields) {
                if (key == "spans") {
                    checkSpans(value);
                }
            }
        }
    }

    // A literal run's tone: one of the tone names, or null for none.
    static void checkLiteralRunTone(Value const& tone, std::string const& path) {
        std::string const* const name = tone.asString();
        if (tone.isNull()) {
            return;
        }
        if (name == nullptr ||
            (*name != "neutral" && *name != "info" && *name != "ok" && *name != "warn" && *name != "err")) {
            refuse(path, "a run's `tone` is neutral, info, ok, warn or err");
        }
    }

    // A run marked `action`: allowed in a link banner's spans, once.
    static void checkActionRun(std::string const& path, bool actionAllowed, int& actions) {
        if (!actionAllowed || ++actions > 1) {
            refuse(path, std::string{kActionRunPlaces});
        }
    }

    // A literal run: a string, or a record of a string `text` and boolean flags.
    static void checkLiteralRun(Value const& run, std::string const& path, bool actionAllowed, int& actions) {
        if (run.asString() != nullptr) {
            return;
        }
        ValueRecord const* const record = run.asRecord();
        if (record == nullptr) {
            refuse(path, "a run is a string or a record with `text`");
        }
        if (!record->contains("text")) {
            refuse(path, "a run's record gives its `text`");
        }
        for (auto const& [key, value] : *record) {
            if (key != "text" && key != "strong" && key != "mono" && key != "tone" && key != "action") {
                refuse(path, "a run takes `text`, `strong`, `mono` and `tone`, not `" + key + "`");
            }
            if (key == "tone") {
                checkLiteralRunTone(value, path);
            } else if (key != "text" && value.asBool() == nullptr) {
                refuse(path, "a run's `" + key + "` is true or false");
            }
            if (key == "action" && value.asBool() != nullptr && *value.asBool()) {
                checkActionRun(path, actionAllowed, actions);
            }
        }
    }

    // A run written as an `obj`: a `text`, and flags that are booleans when written as constants.
    static void checkRunRecord(Expr const& run, bool actionAllowed, int& actions) {
        bool hasText = false;
        for (auto const& [key, value] : run.fields) {
            if (key == "text") {
                hasText = true;
            } else if (key == "tone") {
                if (value->kind == ExprKind::Literal) {
                    checkLiteralRunTone(value->literal, value->path);
                }
            } else if (key != "strong" && key != "mono" && key != "action") {
                refuse(value->path, "a run takes `text`, `strong`, `mono` and `tone`, not `" + key + "`");
            } else if (value->kind == ExprKind::Literal && value->literal.asBool() == nullptr) {
                refuse(value->path, "a run's `" + key + "` is true or false");
            } else if (key == "action" && value->kind == ExprKind::Literal && *value->literal.asBool()) {
                checkActionRun(value->path, actionAllowed, actions);
            }
        }
        if (!hasText) {
            refuse(run.path, "a run's record gives its `text`");
        }
    }

    // A table cell's place across its column and down its row: `stretch`, which a stack takes, is no place for a
    // cell.
    static constexpr std::array<std::string_view, 3> kCellPlaces = {"start", "center", "end"};

    // A table column's closed sets: its default cell's text role, and its cells' places; and its display parts,
    // which are literals: `decimals` 0 to 18, `rounding` one of two names, `date` a pattern, `labels` a list of
    // `{operand, text}`.
    // NOLINTNEXTLINE(readability-function-cognitive-complexity): the column's closed sets and display parts are each checked against their allowed values and report in document order
    static void checkColumnPart(DocNode const& node, bool gridColumn) {
        checkTextRole(node);
        checkCellPlace(node, "align");
        checkCellPlace(node, "valign");
        // A grid column's overlays are per row; a table over rows has none of them.
        if (!gridColumn) {
            for (std::string_view const key :
                 {std::string_view{"readonly"}, std::string_view{"options"}, std::string_view{"placeholder"},
                  std::string_view{"field"}, std::string_view{"unit"}}) {
                if (ExprPtr const given = node.prop(key)) {
                    refuse(given->path, "`" + std::string{key} +
                                            (key == "field" || key == "unit" ? "` as an expression" : "`") +
                                            " is a grid column's, read per row; this table is not a grid of a form");
                }
            }
        }
        if (ExprPtr const decimals = node.prop("decimals");
            decimals && (!gridColumn || decimals->kind == ExprKind::Literal)) {
            std::int64_t const* const places = decimals->kind == ExprKind::Literal
                                                   ? decimals->literal.asInt()
                                                   : static_cast<std::int64_t const*>(nullptr);
            if (places == nullptr || *places < 0 || *places > 18) {
                refuse(decimals->path, "`decimals` is a literal integer, 0 to 18");
            }
        }
        if (std::string_view const rounding = node.name("rounding");
            !rounding.empty() && rounding != "halfEven" && rounding != "halfUp") {
            refuse(pointerAppend(node.path, "rounding"),
                   "`" + std::string{rounding} + "` is not one of the values of `rounding`: halfEven or halfUp");
        }
        if (std::string_view const date = node.name("date");
            !date.empty() && !checkDatePattern(date, false) && !checkDatePattern(date, true)) {
            refuse(pointerAppend(node.path, "date"), checkDatePattern(date, false).error());
        }
        if (ExprPtr const labels = node.prop("labels")) {
            ValueList const* const items =
                labels->kind == ExprKind::Literal ? labels->literal.asList() : static_cast<ValueList const*>(nullptr);
            bool const read = items != nullptr && std::ranges::all_of(*items, [](Value const& item) {
                                  Value const* const operand = item.field("operand");
                                  Value const* const text = item.field("text");
                                  return operand != nullptr && operand->asString() != nullptr && text != nullptr &&
                                         text->asString() != nullptr;
                              });
            if (!read) {
                refuse(labels->path, R"(`labels` is a literal list of `{"operand": text, "text": text}`)");
            }
        }
    }

    // Only a horizontal `forEach` wraps and only a vertical one is divided; a literal `axis` that says otherwise is
    // refused here, and a computed one is read when the screen lowers.
    static void checkForEachWrap(DocNode const& node) {
        ExprPtr const axis = node.prop("axis");
        if (axis == nullptr || axis->kind != ExprKind::Literal) {
            return;
        }
        std::string const* const named = axis->literal.asString();
        bool const horizontal = named != nullptr && *named == "horizontal";
        if (node.flag("wrap") && !horizontal) {
            refuse(pointerAppend(node.path, "wrap"),
                   "`wrap` fills lines along a row; this `forEach`'s `axis` is not \"horizontal\"");
        }
        if (node.flag("divided") && horizontal) {
            refuse(pointerAppend(node.path, "divided"),
                   "`divided` draws rules between rows of a column; this `forEach`'s `axis` is \"horizontal\"");
        }
    }

    // An action bar's gap is the theme's, and a horizontal `forEach` directly in it is part of that row.
    static void checkActionBar(DocNode const& node) {
        if (node.prop("gap") != nullptr) {
            refuse(pointerAppend(node.path, "gap"), kActionBarGapRefusal);
        }
        // A horizontal `forEach` directly in the bar is part of its row of actions, so it takes the same gap.
        for (DocNodePtr const& child : node.nodes("children")) {
            if (child->kind != "forEach" || child->prop("gap") == nullptr) {
                continue;
            }
            ExprPtr const axis = child->prop("axis");
            if (axis != nullptr && axis->kind == ExprKind::Literal) {
                std::string const* const named = axis->literal.asString();
                if (named != nullptr && *named == "horizontal") {
                    refuse(pointerAppend(child->path, "gap"), kActionBarGapRefusal);
                }
            }
        }
    }

    // A grid's `minColumnWidth`: a literal number of units that has columns to drop, and no tracks, which size their
    // own columns. Where it could do nothing, it is refused rather than ignored.
    static void checkMinColumnWidth(DocNode const& node) {
        ExprPtr const least = node.prop("minColumnWidth");
        if (least == nullptr) {
            return;
        }
        std::int64_t const* const units =
            least->kind == ExprKind::Literal ? least->literal.asInt() : static_cast<std::int64_t const*>(nullptr);
        if (units == nullptr) {
            refuse(least->path, "`minColumnWidth` is a literal integer");
        }
        if (*units < 1 || *units > 64) {
            refuse(least->path, "`minColumnWidth` is 1 to 64 units");
        }
        if (node.parts.contains("tracks")) {
            refuse(least->path, "a grid with tracks sizes its own columns; `minColumnWidth` is for equal columns");
        }
        ExprPtr const columns = node.prop("columns");
        if (columns == nullptr) {
            refuse(least->path,
                   "`minColumnWidth` needs `columns`: it drops columns from that count as the grid narrows");
        }
        if (columns->kind == ExprKind::Literal) {
            std::int64_t const* const count = columns->literal.asInt();
            if (count != nullptr && *count <= 1) {
                refuse(least->path, "`minColumnWidth` has nothing to drop from one column");
            }
        }
    }

    static void checkGrid(DocNode const& node) {
        if (!node.name("justify").empty()) {
            refuse(pointerAppend(node.path, "justify"),
                   "a grid's columns are sized by their count or tracks; there is no spare length to justify");
        }
        if (node.prop("selected") != nullptr) {
            refuse(pointerAppend(node.path, "selected"),
                   "a grid is not an entry of a list; `selected` marks a column or a row");
        }
        checkMinColumnWidth(node);
        auto const tracks = node.parts.find("tracks");
        if (tracks == node.parts.end()) {
            return;
        }
        if (node.prop("columns") != nullptr) {
            refuse(pointerAppend(node.path, "tracks"),
                   "a grid takes `columns` or `tracks`, not both; with `tracks`, the column count is their number");
        }
        if (tracks->second.empty()) {
            refuse(pointerAppend(node.path, "tracks"), "`tracks` lists at least one track");
        }
    }

    // A menu's style, and its trigger's keys, which only a button-style menu has. An icon-only trigger needs a name
    // for assistive technology, so a button-style menu has a `label` or an `a11y` name.
    static void checkMenu(DocNode const& node) {
        static constexpr std::array<std::string_view, 2> kStyles = {"bar", "button"};
        checkChoice(node, "style", kStyles);
        static constexpr std::array<std::string_view, 4> kEmphasis = {"normal", "primary", "danger", "ghost"};
        checkChoice(node, "emphasis", kEmphasis);
        if (node.name("style") != "button") {
            for (std::string_view const key : {"label", "icon"}) {
                if (node.prop(key) != nullptr) {
                    refuse(
                        pointerAppend(node.path, key),
                        "`" + std::string{key} + "` is a button-style menu's trigger; give the menu `style: button`");
                }
            }
            if (!node.name("emphasis").empty()) {
                refuse(pointerAppend(node.path, "emphasis"),
                       "`emphasis` is a button-style menu's trigger; give the menu `style: button`");
            }
            return;
        }
        auto const a11y = node.partList("a11y");
        bool const named = !a11y.empty() && a11y.front()->prop("name") != nullptr;
        if (node.prop("label") == nullptr && !named) {
            refuse(node.path,
                   "a button-style menu has a `label` or an `a11y` name, so its trigger has a name for "
                   "assistive technology");
        }
    }

    // A separator is a line between entries: `true`, and nothing else beside it.
    // Whether a part kind is a menu entry: a `menu` node's or a screen head's.
    [[nodiscard]] static bool isMenuEntry(std::string_view kind) {
        return kind == "menuItem" || kind == "headMenuItem";
    }

    static void checkMenuItem(DocNode const& node) {
        if (!node.flags.contains("separator")) {
            return;
        }
        if (!node.flag("separator")) {
            refuse(pointerAppend(node.path, "separator"), "`separator` is `true`; leave it out for an entry");
        }
        bool const alone = node.props.empty() && node.events.empty() && node.parts.empty() && node.names.empty() &&
                           node.keys.empty() && node.flags.size() == 1;
        if (!alone) {
            refuse(node.path, "a separator is a line between entries and takes no other key");
        }
    }

    // A cell place, which names the three it may be.
    static void checkCellPlace(DocNode const& node, std::string_view key) {
        std::string_view const value = node.name(key);
        if (!value.empty() && !listed(kCellPlaces, value)) {
            refuse(pointerAppend(node.path, key), "`" + std::string{value} + "` is not one of the values of `" +
                                                      std::string{key} + "`: start, center or end");
        }
    }

    // A form's `columns`: a literal integer, 1 to 12, the `x-colspan` convention's full row.
    static void checkFormColumns(DocNode const& node) {
        ExprPtr const columns = node.prop("columns");
        if (columns == nullptr) {
            return;
        }
        std::int64_t const* const count =
            columns->kind == ExprKind::Literal ? columns->literal.asInt() : static_cast<std::int64_t const*>(nullptr);
        if (count == nullptr) {
            refuse(columns->path, "`columns` is a literal integer");
        }
        if (*count < 1 || *count > 12) {
            refuse(columns->path, "`columns` is 1 to 12; a form lays its fields in at least one column");
        }
    }

    // A `captionWidth`: a literal integer, 1 to 64, which needs a caption column, so inline captions.
    static void checkCaptionWidth(DocNode const& node, bool inlineCaptions) {
        ExprPtr const width = node.prop("captionWidth");
        if (width == nullptr) {
            return;
        }
        std::int64_t const* const units =
            width->kind == ExprKind::Literal ? width->literal.asInt() : static_cast<std::int64_t const*>(nullptr);
        if (units == nullptr) {
            refuse(width->path, "`captionWidth` is a literal integer");
        }
        if (*units < 1 || *units > 64) {
            refuse(width->path, "`captionWidth` is 1 to 64 units");
        }
        if (!inlineCaptions) {
            refuse(width->path, "`captionWidth` needs inline captions");
        }
    }

    static void checkChoice(DocNode const& node, std::string_view key, std::span<std::string_view const> choices) {
        std::string_view const value = node.name(key);
        if (!value.empty() && !listed(choices, value)) {
            refuse(pointerAppend(node.path, key),
                   "`" + std::string{value} + "` is not one of the values of `" + std::string{key} + "`");
        }
    }

    void checkCommands(Commands const& commands, CheckContext const& context, bool sequence = false) {
        CheckContext current = context;
        current.commands = true;
        for (Command const& cmd : commands) {
            checkCommand(cmd, current);
            if (sequence && cmd.kind == CommandKind::Run && !cmd.binding.empty() && !cmd.extra) {
                current.bindings.emplace_back(cmd.binding, Type::any());
            }
        }
    }

    // NOLINTNEXTLINE(readability-function-cognitive-complexity): one case per command of ui/1.
    void checkCommand(Command const& cmd, CheckContext const& context) {
        std::string const where = cmd.path;
        switch (cmd.kind) {
            case CommandKind::Set:
                checkSet(cmd, context);
                break;
            case CommandKind::Reset: {
                NameEntry const& entry = require(context, cmd.target, where,
                                                 {NameCategory::State, NameCategory::Element}, "state or a dialog");
                if (entry.category == NameCategory::Element && entry.element->kind != "dialog" &&
                    entry.element->kind != "drawer") {
                    refuse(where, "`reset` restores state, or the state of a dialog; `" + cmd.target + "` is a " +
                                      entry.element->kind);
                }
                break;
            }
            case CommandKind::Run: {
                static_cast<void>(require(context, cmd.target, where, {NameCategory::Mutation}, "a mutation"));
                CheckContext body = context;
                if (cmd.extra) {
                    checkExpr(*cmd.extra, context);
                    if (!cmd.binding.empty()) {
                        body.bindings.emplace_back(cmd.binding, Type::any());
                    }
                }
                if (cmd.value) {
                    checkExpr(*cmd.value, body);
                }
                break;
            }
            case CommandKind::Refetch:
                static_cast<void>(require(context, cmd.target, where, {NameCategory::Query}, "a query"));
                break;
            case CommandKind::Open:
            case CommandKind::Close:
                if (cmd.kind == CommandKind::Close && cmd.target == "self") {
                    break;
                }
                checkDialog(cmd, context);
                break;
            case CommandKind::Focus:
                checkFocus(cmd, context);
                break;
            case CommandKind::ResetPrefs:
                if (cmd.target != "all" && !_doc->customize.contains(cmd.target)) {
                    refuse(where, "`" + cmd.target + "` is not a customization point of this document");
                }
                break;
            case CommandKind::If:
                checkCondition(*cmd.value, context, "the condition of `if`");
                checkCommands(cmd.commands, context);
                checkCommands(cmd.alternative, context);
                break;
            case CommandKind::Seq:
                checkCommands(cmd.commands, context, true);
                break;
            case CommandKind::All:
            case CommandKind::Navigate:
                if (cmd.value) {
                    checkExpr(*cmd.value, context);
                }
                if (cmd.extra) {
                    checkExpr(*cmd.extra, context);
                }
                checkCommands(cmd.commands, context);
                checkCommands(cmd.alternative, context);
                break;
            case CommandKind::CancelClose:
                if (!context.onClose) {
                    refuse(where, "`cancelClose` is valid only in `onClose`");
                }
                break;
            case CommandKind::Retry:
                if (!context.boundaryError) {
                    refuse(where, "`retry` is valid only inside a boundary's `error`");
                }
                break;
            case CommandKind::SignIn:
                static_cast<void>(require(context, cmd.target, where, {NameCategory::Mutation}, "a mutation"));
                break;
            case CommandKind::Upload:
                static_cast<void>(require(context, cmd.target, where, {NameCategory::Mutation}, "a mutation"));
                checkExpr(*cmd.value, context);
                break;
            case CommandKind::Download:
                checkExpr(*cmd.value, context);
                if (cmd.extra) {
                    checkExpr(*cmd.extra, context);
                }
                if (cmd.mediaType) {
                    checkExpr(*cmd.mediaType, context);
                }
                if (cmd.option == "preview") {
                    static_cast<void>(require(context, cmd.target, where, {NameCategory::State}, "state"));
                }
                break;
            case CommandKind::Return:
            case CommandKind::Host:
            case CommandKind::Notify:
            case CommandKind::SwitchBackend:
            case CommandKind::SetLocale:
            case CommandKind::SetContext:
            case CommandKind::SignOut:
            default:
                if (cmd.value) {
                    checkExpr(*cmd.value, context);
                }
                break;
        }
    }

    static void checkDialog(Command const& cmd, CheckContext const& context) {
        NameEntry const& entry = require(context, cmd.target, cmd.path, {NameCategory::Element}, "a dialog");
        if (entry.element->kind != "dialog" && entry.element->kind != "drawer") {
            refuse(cmd.path, "`" + cmd.target + "` is a " + entry.element->kind + ", not a dialog or drawer");
        }
    }

    void checkSet(Command const& cmd, CheckContext const& context) {
        auto const segments = splitPath(cmd.target);
        std::string const& head = segments->front();
        Type const value = checkExpr(*cmd.value, context);
        if (head == "app") {
            return;
        }
        NameEntry& entry =
            require(context, head, cmd.path, {NameCategory::State, NameCategory::Element}, "state or an input");
        if (entry.category == NameCategory::Element) {
            KindSpec const* const spec = findKind(entry.element->kind, false);
            if ((spec->flags & kWritable) == 0) {
                // A table's state is the document's: it binds its `selection`, `sort` or `page` to state, and the
                // state is set.
                std::string const hint = entry.element->kind == "table"
                                             ? ": bind its `selection`, `sort` or `page` to state and set that"
                                             : "";
                refuse(cmd.path,
                       "`set` writes state or an input's draft; `" + head + "` is a " + entry.element->kind + hint);
            }
            return;
        }
        auto const type = derivedType(entry, head);
        if (type && segments->size() == 1 && !compatible(*type, value)) {
            refuse(cmd.value->path, "`" + head + "` is " + type->name() + "; the value is " + value.name());
        }
    }

    /// The state and elements an expression reads, through the `let` entries it reads.
    void reads(Expr const& expr, ScopeInfo* scope, std::set<NameEntry const*>& out, std::vector<std::string>& locals,
               std::set<NameEntry const*>& seenLets) {
        if (expr.kind == ExprKind::Ref && std::ranges::find(locals, expr.name) == locals.end()) {
            NameEntry const* const entry = lookup(scope, expr.name);
            if (entry != nullptr &&
                (entry->category == NameCategory::State || entry->category == NameCategory::Element)) {
                out.insert(entry);
            } else if (entry != nullptr && entry->category == NameCategory::Let && seenLets.insert(entry).second) {
                std::vector<std::string> inner;
                reads(*entry->derivedFrom, entry->scope, out, inner, seenLets);
            }
        }
        bool const binds = bindsElement(expr.kind);
        for (std::size_t index = 0; index < expr.args.size(); ++index) {
            if (binds && index == 1) {
                locals.push_back(expr.name);
            }
            reads(*expr.args.at(index), scope, out, locals, seenLets);
            if (binds && index == 1) {
                locals.pop_back();
            }
        }
        for (auto const& [name, field] : expr.fields) {
            static_cast<void>(name);
            reads(*field, scope, out, locals, seenLets);
        }
    }

    /// The state and elements a command list writes.
    static void writes(Commands const& commands, ScopeInfo* scope, std::set<NameEntry const*>& out) {
        for (Command const& cmd : commands) {
            if (cmd.kind == CommandKind::Set || cmd.kind == CommandKind::Reset) {
                auto const segments = splitPath(cmd.target);
                NameEntry const* const entry = lookup(scope, segments->front());
                if (entry != nullptr) {
                    out.insert(entry);
                }
            }
            writes(cmd.commands, scope, out);
            writes(cmd.alternative, scope, out);
        }
    }

    void checkWatchGraph() {
        std::size_t const count = _watches.size();
        std::vector<std::set<NameEntry const*>> observed(count);
        std::vector<std::set<NameEntry const*>> written(count);
        for (std::size_t index = 0; index < count; ++index) {
            auto const& [watch, scope] = _watches.at(index);
            std::vector<std::string> locals;
            std::set<NameEntry const*> seenLets;
            reads(*watch->on, scope, observed.at(index), locals, seenLets);
            writes(watch->body, scope, written.at(index));
        }
        // 0 unvisited, 1 on the current path, 2 finished.
        std::vector<int> state(count, 0);
        std::vector<std::size_t> trail;
        std::function<void(std::size_t)> visit = [&](std::size_t from) {
            state.at(from) = 1;
            trail.push_back(from);
            for (std::size_t to = 0; to < count; ++to) {
                bool edge = false;
                for (NameEntry const* entry : written.at(from)) {
                    edge = edge || observed.at(to).contains(entry);
                }
                if (!edge) {
                    continue;
                }
                if (state.at(to) == 1) {
                    std::string cycle;
                    auto const start = std::ranges::find(trail, to);
                    for (auto step = start; step != trail.end(); ++step) {
                        cycle += _watches.at(*step).first->path + " → ";
                    }
                    refuse(_watches.at(to).first->path,
                           "the watch graph has a cycle: " + cycle + _watches.at(to).first->path);
                }
                if (state.at(to) == 0) {
                    visit(to);
                }
            }
            trail.pop_back();
            state.at(from) = 2;
        };
        for (std::size_t index = 0; index < count; ++index) {
            if (state.at(index) == 0) {
                visit(index);
            }
        }
    }

    // The effect checks below read only what the catalog says about each action. They warn only where the
    // effects certainly cover, or certainly miss, what the document declares, because a warning a document cannot
    // act on would teach its author to ignore the rest.

    /// The effects as a phrase, such as "`changes T_Row`, `none`".
    [[nodiscard]] static std::string describeEffects(std::vector<ActionEffect> const& effects) {
        std::string out;
        for (ActionEffect const& effect : effects) {
            out += (out.empty() ? "`" : ", `") + effect.kind + (effect.record ? " " + *effect.record : "") + "`";
        }
        return out;
    }

    /// Whether an action's effects change anything; `none` alone does not.
    [[nodiscard]] static bool changesData(ActionInfo const& info) {
        return info.effects &&
               std::ranges::any_of(*info.effects, [](ActionEffect const& effect) { return effect.kind != "none"; });
    }

    void warn(LoadWarningKind kind, std::string path, std::string text) {
        _warnings.push_back(LoadWarning{.kind = kind, .path = std::move(path), .text = std::move(text)});
    }

    void checkEffects() {
        if (!_options->catalog.action) {
            return;
        }
        for (auto const& entry : _mutations) {
            checkMutationEffects(*entry.first);
        }
        for (auto const& [watch, scope] : _watches) {
            checkWatchFeedback(*watch, scope);
        }
    }

    void checkMutationEffects(MutationDecl const& mutation) {
        auto const action = _options->catalog.action(mutation.action);
        if (!action) {
            return;
        }
        if (!action->effects && !refetches(mutation.onSuccess)) {
            warn(LoadWarningKind::Uncovered, mutation.path,
                 std::format("nothing refreshes after `{}` succeeds: `{}` declares no effects, and no `refetch` "
                             "follows it; declare what it changes, or `effects::none` when it changes nothing",
                             mutation.name, mutation.action));
        }
    }

    /// Whether a command list refetches a query, which is a refresh the document declares itself.
    [[nodiscard]] static bool refetches(Commands const& commands) {
        return std::ranges::any_of(commands, [](Command const& cmd) {
            return cmd.kind == CommandKind::Refetch || refetches(cmd.commands) || refetches(cmd.alternative);
        });
    }

    // Another client's change refetches the watched query, the watch runs the mutation, and its change reaches the
    // other client, which may do the same. Only `"source": "user"` keeps a refetch from running the watch.
    void checkWatchFeedback(WatchDecl const& watch, ScopeInfo* scope) {
        if (watch.source == WatchSource::User) {
            return;
        }
        std::vector<std::string> locals;
        std::set<NameEntry const*> seenLets;
        auto const query = readQuery(*watch.on, scope, locals, seenLets);
        if (!query) {
            return;
        }
        if (auto const mutation = runsWriter(watch.body, scope)) {
            warn(LoadWarningKind::WatchFeedback, watch.path,
                 std::format("the watch reads the query `{}` and runs `{}`, which changes data: another client's "
                             "change refetches the query and runs it again; give the watch \"source\": \"user\"",
                             *query, *mutation));
        }
    }

    /// The first query whose value an expression reads, directly or through a `let`.
    [[nodiscard]] static std::optional<std::string> readQuery(Expr const& expr, ScopeInfo* scope,
                                                              std::vector<std::string>& locals,
                                                              std::set<NameEntry const*>& seenLets) {
        if (expr.kind == ExprKind::Query) {
            return expr.name;
        }
        if (expr.kind == ExprKind::Ref && std::ranges::find(locals, expr.name) == locals.end()) {
            NameEntry const* const entry = lookup(scope, expr.name);
            if (entry != nullptr && entry->category == NameCategory::Query) {
                return expr.name;
            }
            if (entry != nullptr && entry->category == NameCategory::Let && seenLets.insert(entry).second) {
                std::vector<std::string> inner;
                if (auto found = readQuery(*entry->derivedFrom, entry->scope, inner, seenLets)) {
                    return found;
                }
            }
        }
        bool const binds = bindsElement(expr.kind);
        for (std::size_t index = 0; index < expr.args.size(); ++index) {
            if (binds && index == 1) {
                locals.push_back(expr.name);
            }
            auto found = readQuery(*expr.args.at(index), scope, locals, seenLets);
            if (binds && index == 1) {
                locals.pop_back();
            }
            if (found) {
                return found;
            }
        }
        for (auto const& [name, field] : expr.fields) {
            static_cast<void>(name);
            if (auto found = readQuery(*field, scope, locals, seenLets)) {
                return found;
            }
        }
        return std::nullopt;
    }

    /// The first mutation a command list runs whose action changes data.
    [[nodiscard]] std::optional<std::string> runsWriter(Commands const& commands, ScopeInfo* scope) const {
        for (Command const& cmd : commands) {
            bool const runs =
                cmd.kind == CommandKind::Run || cmd.kind == CommandKind::Upload || cmd.kind == CommandKind::SignIn;
            NameEntry const* const entry = runs ? lookup(scope, cmd.target) : nullptr;
            if (entry != nullptr && entry->mutation != nullptr) {
                auto const action = _options->catalog.action(entry->mutation->action);
                if (action && changesData(*action)) {
                    return cmd.target;
                }
            }
            if (auto found = runsWriter(cmd.commands, scope)) {
                return found;
            }
            if (auto found = runsWriter(cmd.alternative, scope)) {
                return found;
            }
        }
        return std::nullopt;
    }

    Document const* _doc;
    LoadOptions const* _options;
    std::deque<ScopeInfo> _scopes;
    std::map<DocNode const*, ScopeInfo*> _scopeOf;
    std::map<DocNode const*, ScopeInfo*> _innerOf;
    std::map<std::string, QueryDecl const*, std::less<>> _queries;
    std::map<std::string, DocNode const*, std::less<>> _elements;
    // The literal unit each form field is first given by an overlay, by the form's id and the field's path.
    mutable std::map<std::string, std::string, std::less<>> _unitsGiven;
    std::vector<std::pair<WatchDecl const*, ScopeInfo*>> _watches;
    std::vector<std::pair<MutationDecl const*, ScopeInfo*>> _mutations;
    std::vector<std::pair<QueryDecl const*, ScopeInfo*>> _queryScopes;
    std::vector<LoadWarning> _warnings;
    std::map<DocNode const*, TypeKind> _dateValueKinds;
    std::map<std::string, FocusTarget, std::less<>> _focusTargets;
    std::vector<std::string> _deriving;
    // NOLINTEND(misc-no-recursion)
};

// NOLINTNEXTLINE(readability-function-cognitive-complexity): one case per kind of name.
std::expected<Type, std::string> ScopeResolver::ref(std::string_view head, std::span<std::string const> path) const {
    for (auto const& [name, type] : std::views::reverse(_context.bindings)) {
        if (name == head) {
            return typeAlong(type, head, path);
        }
    }
    if (head == "event") {
        if (!_context.commands) {
            return std::unexpected(std::string{"`event` is read only inside a command list"});
        }
        return Type::any();
    }
    if (head == "app") {
        return Type::any();
    }
    if (head == "boundary") {
        if (!_context.boundaryError || path.size() != 1 || path.front() != "error") {
            return std::unexpected(std::string{"`boundary.error` is read only inside a boundary's `error`"});
        }
        return Type::of(TypeKind::String, true);
    }
    NameEntry* const entry = DocumentValidator::lookup(_context.scope, head);
    if (entry == nullptr) {
        return std::unexpected("`" + std::string{head} + "` does not resolve in this scope");
    }
    switch (entry->category) {
        case NameCategory::Param:
            return typeAlong(entry->type, head, path);
        case NameCategory::State:
        case NameCategory::Let: {
            auto type = _validator.derivedType(*entry, head);
            if (!type) {
                return type;
            }
            return typeAlong(*type, head, path);
        }
        case NameCategory::Query:
            return Type::any();
        case NameCategory::Mutation:
            return std::unexpected("`" + std::string{head} +
                                   "` is a mutation; read it with `result`, `pending` or "
                                   "`error`");
        case NameCategory::Model:
            return std::unexpected("`" + std::string{head} + "` is a model alias, not a value");
        case NameCategory::Transfer:
            if (path.empty() || !listed(kTransferValues, path.front())) {
                return std::unexpected("the transfer `" + std::string{head} +
                                       "` is read as `progress`, `pending` or `error`");
            }
            return Type::any();
        case NameCategory::Element:
        default: {
            KindSpec const* const spec = findKind(entry->element->kind, false);
            if (path.empty()) {
                return std::unexpected("the element `" + std::string{head} +
                                       "` is read through one of its values, "
                                       "such as `" +
                                       std::string{head} + ".visible`");
            }
            std::string const& value = path.front();
            bool const open = (spec->flags & kOpenValues) != 0;
            bool const known = listed(kAnyValues, value) || listed(spec->values, value) || open;
            if (!known) {
                return std::unexpected("the " + entry->element->kind + " `" + std::string{head} + "` has no value `" +
                                       value + "`");
            }
            bool const field = open && !listed(kAnyValues, value) && !listed(spec->values, value);
            Type const type = field ? Type::any() : elementValueType(value);
            return typeAlong(type, value, path.subspan(1));
        }
    }
}

std::expected<Type, std::string> ScopeResolver::row(std::span<std::string const> path) const {
    static_cast<void>(path);
    for (ScopeInfo const* scope = _context.scope; scope != nullptr; scope = scope->parent) {
        if (scope->row) {
            return Type::any();
        }
    }
    return std::unexpected(std::string{"`row` is read only inside a forEach or table row"});
}

std::expected<Type, std::string> ScopeResolver::control(ExprKind form, std::string_view name) const {
    if (form == ExprKind::Can && (name.starts_with("host:") || name == "context")) {
        return Type::of(TypeKind::Bool);
    }
    NameEntry const* const entry = DocumentValidator::lookup(_context.scope, name);
    bool const query = entry != nullptr && entry->category == NameCategory::Query;
    bool const mutation = entry != nullptr && entry->category == NameCategory::Mutation;
    if (form == ExprKind::Query && !query) {
        return std::unexpected("`" + std::string{name} + "` is not a query of this scope");
    }
    if (form == ExprKind::Result && !mutation) {
        return std::unexpected("`" + std::string{name} + "` is not a mutation of this scope");
    }
    if (!query && !mutation) {
        return std::unexpected("`" + std::string{name} + "` is not a query or mutation of this scope");
    }
    if (form == ExprKind::Pending || form == ExprKind::Can) {
        return Type::of(TypeKind::Bool);
    }
    if (form == ExprKind::Error || form == ExprKind::ErrorKind) {
        return Type::of(TypeKind::String, true);
    }
    return Type::any();
}

std::expected<Type, std::string> ScopeResolver::pref(std::string_view point, std::span<std::string const> path) const {
    auto const& points = _validator.document().customize;
    auto const found = points.find(point);
    if (found == points.end()) {
        return std::unexpected("`" + std::string{point} + "` is not a customization point of this document");
    }
    return typeAlong(found->second.type, point, path);
}

/// @brief The margins a page's `margins` key gives: one literal length for every side, or a record of sides.
/// @param margins The key's expression.
/// @return The margins.
PageMargins pageMarginsOf(ExprPtr const& margins) {
    if (margins->kind != ExprKind::Literal || margins->literal.asRecord() == nullptr) {
        return PageMargins{pageLengthOf(margins, "margins")};
    }
    PageMargins result;
    for (auto const& [side, amount] : *margins->literal.asRecord()) {
        std::int64_t const* const count = amount.asInt();
        Mm* target = nullptr;
        if (side == "top") {
            target = &result.top;
        } else if (side == "right") {
            target = &result.right;
        } else if (side == "bottom") {
            target = &result.bottom;
        } else if (side == "left") {
            target = &result.left;
        }
        if (target == nullptr || count == nullptr || *count < 0 || *count > std::numeric_limits<std::int32_t>::max()) {
            refuse(margins->path,
                   "`margins` is a literal count of micrometres, or a record of `top`, `right`, "
                   "`bottom` and `left` counts");
        }
        *target = Mm::fromMicrometres(static_cast<std::int32_t>(*count));
    }
    return result;
}

}  // namespace

PageGeometry pageGeometryOf(DocNode const& node) {
    PageGeometry geometry;
    geometry.width = pageLengthOf(node.prop("width"), "width");
    geometry.height = pageLengthOf(node.prop("height"), "height");
    if (ExprPtr const margins = node.prop("margins")) {
        geometry.margins = pageMarginsOf(margins);
    }
    if (std::optional<std::string> const problem = geometry.problem()) {
        refuse(node.path, *problem);
    }
    return geometry;
}

RegionGeometry regionGeometryOf(DocNode const& node, PageGeometry const& page) {
    RegionGeometry geometry;
    std::string_view const anchor = node.name("anchor");
    if (anchor == "top") {
        geometry.anchor = PageAnchor::Top;
    } else if (anchor == "bottom") {
        geometry.anchor = PageAnchor::Bottom;
    } else if (anchor == "offset") {
        geometry.anchor = PageAnchor::Offset;
    } else {
        refuse(pointerAppend(node.path, "anchor"), "`anchor` is `top`, `bottom` or `offset`");
    }
    if (ExprPtr const x = node.prop("x")) {
        geometry.x = pageLengthOf(x, "x");
    }
    if (ExprPtr const y = node.prop("y")) {
        geometry.y = pageLengthOf(y, "y");
    }
    if (ExprPtr const width = node.prop("width")) {
        geometry.width = pageLengthOf(width, "width");
    }
    if (ExprPtr const height = node.prop("height")) {
        geometry.height = pageLengthOf(height, "height");
    }
    if (std::optional<std::string> const problem = geometry.problem(page)) {
        refuse(node.path, *problem);
    }
    return geometry;
}

KindSpec const* findKind(std::string_view kind, bool part) {
    for (KindSpec const& spec : kKinds) {
        if (spec.kind == kind && ((spec.flags & kPart) != 0) == part) {
            return &spec;
        }
    }
    return nullptr;
}

}  // namespace detail

std::expected<Document, Refusal> loadDocument(Json const& json, LoadOptions const& options) {
    std::string documentId;
    if (auto const* const object = std::get_if<Json::object_t>(&json.data)) {
        auto const* const found = object->find("id");
        if (found != object->end()) {
            if (auto const* const text = std::get_if<std::string>(&found->second.data)) {
                documentId = *text;
            }
        }
    }
    try {
        Document doc = detail::DocumentParser{}.parse(json);
        detail::DocumentValidator validator{doc, options};
        validator.run();
        doc.warnings = validator.takeWarnings();
        doc.dateValueKinds = validator.takeDateValueKinds();
        doc.focusTargets = validator.takeFocusTargets();
        return doc;
    } catch (detail::DocumentRefused const& refused) {
        Refusal refusal = refused.refusal();
        refusal.document = documentId;
        return std::unexpected(std::move(refusal));
    }
}

std::expected<Document, Refusal> loadDocumentText(std::string_view text, LoadOptions const& options) {
    auto json = parseJson(text);
    if (!json) {
        return std::unexpected(Refusal{.document = {}, .path = "", .reason = std::move(json.error())});
    }
    return loadDocument(*json, options);
}

std::expected<TourDecl, Refusal> loadTourText(std::string_view text) {
    auto json = parseJson(text);
    if (!json) {
        return std::unexpected(Refusal{.document = {}, .path = "", .reason = std::move(json.error())});
    }
    try {
        return detail::DocumentParser::tour(*json, "");
    } catch (detail::DocumentRefused const& refused) {
        return std::unexpected(refused.refusal());
    }
}

std::vector<std::string> tableQueryMembers(std::string_view schemaJson) {
    std::vector<std::string> members;
    auto const json = parseJson(schemaJson);
    auto const* const schema = json ? std::get_if<Json::object_t>(&json->data) : nullptr;
    const auto* const properties = schema != nullptr ? schema->find("properties") : Json::object_t::const_iterator{};
    if (schema == nullptr || properties == schema->end()) {
        return members;
    }
    auto const* const entries = std::get_if<Json::object_t>(&properties->second.data);
    if (entries == nullptr) {
        return members;
    }
    for (auto const& [name, property] : *entries) {
        auto const* const fields = std::get_if<Json::object_t>(&property.data);
        if (fields == nullptr) {
            continue;
        }
        const auto* const marker = fields->find(table::TableQuery::schemaMarker());
        if (marker == fields->end()) {
            continue;
        }
        if (bool const* const flag = std::get_if<bool>(&marker->second.data); flag != nullptr && *flag) {
            members.push_back(name);
        }
    }
    return members;
}

std::vector<std::string_view> nodeKinds() {
    std::vector<std::string_view> kinds;
    for (detail::KindSpec const& spec : detail::kKinds) {
        if ((spec.flags & detail::kPart) == 0) {
            kinds.push_back(spec.kind);
        }
    }
    return kinds;
}

std::vector<std::string_view> partKinds() {
    std::vector<std::string_view> kinds;
    for (detail::KindSpec const& spec : detail::kKinds) {
        if ((spec.flags & detail::kPart) != 0) {
            kinds.push_back(spec.kind);
        }
    }
    return kinds;
}

}  // namespace morph::ui

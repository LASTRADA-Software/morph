// SPDX-License-Identifier: Apache-2.0

// Charts, summaries and arranged tables.

#include "interpreter_grid_detail.hpp"

namespace morph::ui::detail {

namespace {

/// The summary part a summary row's record names: one of the table's `summaries`, or of its group's.
DocNode const* summaryPartOf(DocNode const& table, Value const& record);

/// @brief A chart's exact value at @p places, as the active locale writes it.
/// @param value  The value.
/// @param places The places, at most 18.
/// @param format The formatting facts.
/// @return The text; the exact JSON when the rounded value does not fit.
[[nodiscard]] std::string chartNumberText(math::Rational const& value, std::uint32_t places,
                                          FormatContext const& format) {
    std::optional<math::Rational> rounded =
        math::checkedRoundToDecimalPlaces(value, math::DecimalPlaces{places}, math::RoundingMode::HalfEven);
    if (!rounded) {
        return textOf(Value{Decimal{value}}, format);
    }
    rounded->decimalPlaces = math::DecimalPlaces{places};
    return textOf(Value{Decimal{*rounded}}, format);
}

/// @brief A reference line's stroke by its document name.
/// @param name `solid`, `dashed`, `dotted`, or empty for solid; the loader refuses any other.
/// @return The stroke.
[[nodiscard]] ChartLineStyle chartLineStyle(std::string_view name) {
    if (name == "dashed") {
        return ChartLineStyle::Dashed;
    }
    return name == "dotted" ? ChartLineStyle::Dotted : ChartLineStyle::Solid;
}

/// @brief Where a point sits along a value axis that a document may have fixed, and on which side it is clipped.
/// @param value The point's value.
/// @param range The axis.
/// @return The value drawn, the axis end for a value outside, and the side it lies beyond.
[[nodiscard]] std::pair<math::Rational, ChartClip> chartClipped(math::Rational const& value, ChartRange const& range) {
    if (value < range.low) {
        return {range.low, ChartClip::Below};
    }
    if (value > range.high) {
        return {range.high, ChartClip::Above};
    }
    return {value, ChartClip::None};
}

}  // namespace

std::vector<detail::ChartRawLine> ScreenCore::chartLines(DocNode const& node, RtScope const& scope,
                                                         Bindings const* bindings, std::optional<std::string>& unit) {
    FormatContext const& format = scope.formatting();
    std::vector<detail::ChartRawLine> lines;
    for (DocNodePtr const& part : node.partList("lines")) {
        ExprPtr const valueExpr = part->prop("value");
        Value const value = evalIn(scope, bindings, valueExpr);
        std::optional<math::Rational> const magnitude =
            value.isNull() ? std::nullopt : chartMagnitude(value, valueExpr->path, unit);
        if (!magnitude) {
            continue;
        }
        detail::ChartRawLine line{.value = *magnitude, .label = std::nullopt};
        if (ExprPtr const label = part->prop("label")) {
            line.label = textOf(evalIn(scope, bindings, label), format);
        }
        if (ExprPtr const tone = part->prop("tone")) {
            line.tone = chartToneOf(evalIn(scope, bindings, tone), tone->path).value_or(Tone::Neutral);
        }
        line.style = detail::chartLineStyle(part->name("style"));
        lines.push_back(std::move(line));
    }
    return lines;
}

ChartRange ScreenCore::chartFixedRange(DocNode const& node, RtScope const& scope, Bindings const* bindings,
                                       std::optional<std::string>& unit, ChartRange const& range) {
    ChartRange fixed = range;
    for (auto const& [key, end] : {std::pair{"yMin", &fixed.low}, std::pair{"yMax", &fixed.high}}) {
        if (ExprPtr const expr = node.prop(key)) {
            Value const value = evalIn(scope, bindings, expr);
            if (!value.isNull()) {
                *end = chartMagnitude(value, expr->path, unit).value_or(*end);
            }
        }
    }
    if (fixed.high > fixed.low) {
        return fixed;
    }
    report(pointerAppend(node.path, "yMin"), "`yMin` is not below `yMax`; the chart takes its range from its values");
    return range;
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity): one branch per way a series can be drawn or clipped.
void ScreenCore::chartSeries(ChartData& data, DocNode const& node, RtScope const& scope, Bindings const* bindings,
                             std::vector<std::vector<detail::ChartRawPoint>>& series, bool positioned,
                             ChartRange const& range) {
    auto const positionOf = [positioned](detail::ChartRawPoint const& point) {
        return positioned && point.x ? *point.x
                                     : math::Rational{static_cast<std::int64_t>(point.index), math::DecimalPlaces{0}};
    };
    std::vector<math::Rational> positions;
    for (auto const& points : series) {
        std::ranges::transform(points, std::back_inserter(positions), positionOf);
    }
    // The room one position gives its bars: the least distance between two of their positions, gaps included, so a
    // bar never grows over the label of an empty position between two.
    std::vector<math::Rational> barPositions;
    {
        std::size_t seriesIndex = 0;
        for (DocNodePtr const& part : node.partList("series")) {
            if (part->name("style") == "bars") {
                std::ranges::transform(series.at(seriesIndex), std::back_inserter(barPositions), positionOf);
            }
            ++seriesIndex;
        }
    }
    std::ranges::sort(barPositions);
    std::optional<math::Rational> slot;
    for (std::size_t next = 1; next < barPositions.size(); ++next) {
        auto const distance = math::checkedSub(barPositions.at(next), barPositions.at(next - 1));
        if (distance && distance->numerator > 0 && (!slot || *distance < *slot)) {
            slot = *distance;
        }
    }
    if (!barPositions.empty()) {
        data.barSlot = realOf(slot.value_or(math::Rational{1, math::DecimalPlaces{0}}));
    }
    // A bar is centred on its position and half a slot wide each side, so the axis holds that much beyond the
    // outermost position.
    if (!barPositions.empty() && !positions.empty()) {
        math::Rational const half = math::checkedDiv(slot.value_or(math::Rational{1, math::DecimalPlaces{0}}),
                                                     math::Rational{2, math::DecimalPlaces{0}})
                                        .value_or(math::Rational{0, math::DecimalPlaces{0}});
        auto const [lowest, highest] = std::ranges::minmax(positions);
        auto const below = math::checkedSub(lowest, half);
        auto const above = math::checkedAdd(highest, half);
        if (below && above) {
            positions.push_back(*below);
            positions.push_back(*above);
        }
    }
    if (std::optional<ChartRange> const across = chartRange(positions, 0)) {
        data.xMin = realOf(across->low);
        data.xMax = realOf(across->high);
    }
    std::size_t index = 0;
    for (DocNodePtr const& part : node.partList("series")) {
        ChartSeries drawn;
        if (part->name("style") == "bars") {
            drawn.style = ChartStyle::Bars;
        }
        if (ExprPtr const label = part->prop("label")) {
            drawn.label = textOf(evalIn(scope, bindings, label), scope.formatting());
        }
        drawn.joined = drawn.style == ChartStyle::Line && (!part->flags.contains("joined") || part->flag("joined"));
        for (detail::ChartRawPoint& point : series.at(index)) {
            ChartPoint out{
                .x = realOf(positionOf(point)),
                .y = std::nullopt,
                .clip = ChartClip::None,
                .xLabel = std::move(point.xLabel),
                .tone = point.tone,
                .note = std::move(point.note),
            };
            if (point.y) {
                auto const [shown, clip] = detail::chartClipped(*point.y, range);
                out.y = realOf(shown);
                out.clip = clip;
            }
            drawn.points.push_back(std::move(out));
        }
        data.series.push_back(std::move(drawn));
        ++index;
    }
}

ChartData ScreenCore::chartData(DocNode const& node, RtScope const& scope, Bindings const* bindings) {
    FormatContext const& format = scope.formatting();
    ChartData data;
    data.summary = textOf(evalIn(scope, bindings, node.prop("summary")), format);
    if (ExprPtr const label = node.prop("yLabel")) {
        data.yLabel = textOf(evalIn(scope, bindings, label), format);
    }
    std::optional<std::string> unit;
    detail::ChartAxisKinds kinds;
    std::vector<std::vector<detail::ChartRawPoint>> series;
    for (DocNodePtr const& part : node.partList("series")) {
        series.push_back(chartPoints(*part, scope, bindings, unit, kinds));
    }
    std::vector<detail::ChartRawLine> const lines = chartLines(node, scope, bindings, unit);
    // The values the axis must hold, and the places they are shown at.
    std::vector<math::Rational> values;
    for (auto const& points : series) {
        for (detail::ChartRawPoint const& point : points) {
            if (point.y) {
                values.push_back(*point.y);
            }
        }
    }
    std::ranges::transform(lines, std::back_inserter(values), &detail::ChartRawLine::value);
    // A bar grows from 0, so the axis holds 0; the baseline's side is padded only when something lies beyond it, so
    // that the baseline is the plot's edge when nothing does.
    std::vector<math::Rational> axisValues = values;
    ChartPadding padding;
    bool barsHaveValues = false;
    {
        std::size_t seriesIndex = 0;
        for (DocNodePtr const& part : node.partList("series")) {
            barsHaveValues =
                barsHaveValues || (part->name("style") == "bars" &&
                                   std::ranges::any_of(series.at(seriesIndex), [](detail::ChartRawPoint const& point) {
                                       return point.y.has_value();
                                   }));
            ++seriesIndex;
        }
    }
    if (barsHaveValues) {
        axisValues.emplace_back(0, math::DecimalPlaces{0});
        padding.low = std::ranges::any_of(values, [](math::Rational const& value) { return value.numerator < 0; });
        padding.high = std::ranges::any_of(values, [](math::Rational const& value) { return value.numerator > 0; });
    }
    std::uint32_t widest = 0;
    for (math::Rational const& value : values) {
        widest = std::max(widest, value.decimalPlaces.value);
    }
    ExprPtr const placesExpr = node.prop("yPlaces");
    std::int64_t const* const fixedPlaces = placesExpr ? placesExpr->literal.asInt() : nullptr;
    std::uint32_t const places = fixedPlaces != nullptr ? static_cast<std::uint32_t>(*fixedPlaces) : widest;
    std::optional<ChartRange> const computed = chartRange(axisValues, places, padding);
    if (!computed) {
        report(node.path, "the chart's values overflow its arithmetic; it draws nothing");
        return data;
    }
    ChartRange const range = chartFixedRange(node, scope, bindings, unit, *computed);
    data.yMin = realOf(range.low);
    data.yMax = realOf(range.high);
    // Three or more lines are the scale a reader reads the chart against; fewer leave the values without one.
    std::optional<ChartTicks> const ticks = lines.size() < 3 ? chartTicks(range) : std::nullopt;
    if (ticks) {
        std::uint32_t const tickPlaces = std::max(places, ticks->places);
        for (math::Rational const& tick : ticks->values) {
            data.yTicks.push_back(
                ChartTick{.value = realOf(tick), .text = detail::chartNumberText(tick, tickPlaces, format)});
        }
    }
    for (detail::ChartRawLine const& line : lines) {
        data.lines.push_back(ChartLine{
            .value = realOf(line.value),
            .label = line.label.value_or(detail::chartNumberText(line.value, places, format)),
            .tone = line.tone,
            .style = line.style,
        });
    }
    if (kinds.numbers && kinds.instants) {
        report(pointerAppend(node.path, "series"),
               "a chart's `x` values are all numbers or all dates and instants; the points are spaced by index");
    }
    chartSeries(data, node, scope, bindings, series, kinds.numbers != kinds.instants, range);
    return data;
}

std::optional<Tone> ScreenCore::rowToneOf(Value const& value, std::string const& path, bool& noted) {
    if (value.isNull()) {
        return std::nullopt;
    }
    if (std::optional<Tone> const tone = toneNamed(value)) {
        return tone;
    }
    if (!noted) {
        noted = true;
        report(path, "rowTone is " + canonicalJson(value) + "; a tone is neutral, info, ok, warn or err");
    }
    return std::nullopt;
}

std::vector<RowSpan> ScreenCore::rowSpansOf(Value const& value, std::vector<std::string> const& columns,
                                            std::string const& path, bool& noted) {
    std::vector<RowSpan> spans;
    if (value.isNull()) {
        return spans;
    }
    ValueList const* const items = value.asList();
    bool bad = items == nullptr;
    for (Value const& item : items != nullptr ? *items : ValueList{}) {
        Value const* const column = item.field("column");
        Value const* const rows = item.field("rows");
        Value const* const across = item.field("columns");
        std::string const* const columnId = column != nullptr ? column->asString() : nullptr;
        std::int64_t const one = 1;
        std::int64_t const* const count = rows != nullptr ? rows->asInt() : &one;
        std::int64_t const* const wide = across != nullptr ? across->asInt() : &one;
        auto const found = columnId != nullptr ? std::ranges::find(columns, *columnId) : columns.end();
        if (found == columns.end() || count == nullptr || *count < 1 || wide == nullptr || *wide < 1) {
            bad = true;
            continue;
        }
        spans.push_back(RowSpan{
            .column = static_cast<std::size_t>(found - columns.begin()),
            .rows = static_cast<std::size_t>(*count),
            .columns = static_cast<std::size_t>(*wide),
        });
    }
    if (bad && !noted) {
        noted = true;
        report(path, "rowSpans is " + canonicalJson(value) +
                         "; a span is {column: a column's id, rows: an integer from 1, columns: an integer from 1}");
    }
    return spans;
}

namespace {

/// A summary cell's text: by the column's places when it has them and the value is a number, else the value's own
/// text; empty for `null` (a `mean` of no rows).
[[nodiscard]] std::string summaryText(Value const& value, std::optional<std::uint32_t> places,
                                      FormatContext const& format) {
    if (value.isNull()) {
        return {};
    }
    bool const number = value.asInt() != nullptr || value.asDecimal() != nullptr || value.asQuantity() != nullptr;
    if (places && number) {
        table::ColumnDisplay display;
        display.decimals = places;
        table::TableColumn const kind = columnFor(value, display);
        return formatCell(cellOfRecord(&value, kind, nullptr), kind, format);
    }
    return textOf(value, format);
}

}  // namespace

std::vector<std::pair<std::int64_t, std::int64_t>> gridRuns(GridTable& held) {
    std::vector<std::pair<std::int64_t, std::int64_t>> runs;
    held.runOrder.clear();
    GridRows const& view = held.grid.source->view();
    FormRowsSource const& source = view.source();
    std::span<table::ColumnInfo const> const columns = source.columns();
    if (columns.empty() || columns.back().id != kGroupRunColumn) {
        return runs;
    }
    std::size_t const runColumn = columns.size() - 1;
    std::shared_ptr<table::RowSnapshot const> const snapshot = source.snapshot();
    std::unordered_map<table::RowId, std::size_t> runOf;
    for (std::size_t row = 0; row < snapshot->rowCount(); ++row) {
        table::Cell const cell = snapshot->cell(row, runColumn);
        if (std::int64_t const* const run = std::get_if<std::int64_t>(&cell)) {
            runOf.insert_or_assign(snapshot->rowId(row), static_cast<std::size_t>(*run));
        }
    }
    std::map<std::size_t, std::size_t> seen;
    std::optional<std::size_t> current;
    std::int64_t const total = view.total().rows;
    for (std::int64_t index = 0; index < total; ++index) {
        std::optional<TableRowRef> const ref = view.row(index);
        auto const found = ref ? runOf.find(ref->key) : runOf.end();
        std::size_t const run = found == runOf.end() ? 0 : found->second;
        if (current != run || runs.empty()) {
            current = run;
            runs.emplace_back(index, 0);
            held.runOrder.emplace_back(run, seen[run]++);
        }
        ++runs.back().second;
    }
    return runs;
}

RowRecord gridHeading(GridTable& held, std::size_t ordinal) {
    if (ordinal >= held.runOrder.size()) {
        return RowRecord{.key = Key{std::string{"\x1Fgroup:"} + std::to_string(ordinal)}, .row = Value{}};
    }
    auto const [run, occurrence] = held.runOrder.at(ordinal);
    std::span<Value const> const records = held.grid.source->view().source().runRecords();
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access): guarded by `run < records.size()` in the same expression
    Value record = run < records.size() ? records[run] : Value{};
    Value const* const key = record.field(kGroupHeadingField);
    std::string text = key != nullptr && key->asString() != nullptr ? *key->asString() : std::string{"\x1Fgroup"};
    if (occurrence > 0) {
        text += "/" + std::to_string(occurrence);
    }
    return RowRecord{.key = Key{std::move(text)}, .row = std::move(record)};
}

// The rows a grid with summaries or a group is mounted over: the decorator's, a data row's cell as the grid draws it,
// a summary row's cell its part's expression for the column read with `rows` the records of the data rows in its
// scope and `column` the column's id, shown at the column's places, and a heading row its group's heading. Every
// column's cells are built per row, so a cell built for a data row is never rebound to a summary row.

std::vector<std::pair<std::int64_t, std::int64_t>> rowsRuns(RowsTable& table) {
    std::vector<std::pair<std::int64_t, std::int64_t>> runs;
    table.runHeadings.clear();
    std::int64_t const total = table.rows->total().rows;
    for (std::int64_t index = 0; index < total; ++index) {
        std::optional<TableRowRef> const ref = table.rows->row(index);
        Value const* const heading = ref ? ref->row.field(kGroupHeadingField) : nullptr;
        if (heading != nullptr && heading->asString() != nullptr) {
            runs.emplace_back(index, 0);
            table.runHeadings.push_back(*heading->asString());
        }
        if (!runs.empty()) {
            ++runs.back().second;
        }
    }
    return runs;
}

ValueList rowsSummaryScope(RowsTable const& table, Value const& record) {
    TableView const& view = table.view->get();
    Value const* const run = record.field(kSummaryRunField);
    if (run == nullptr || run->asInt() == nullptr) {
        // NOLINTNEXTLINE(modernize-return-braced-init-list): the explicit type states that the rows are copied from the iterator range into a ValueList
        return ValueList(view.rows.begin(), view.rows.end());
    }
    auto const ordinal = static_cast<std::size_t>(std::max<std::int64_t>(*run->asInt(), 0));
    if (ordinal >= table.runHeadings.size()) {
        return {};
    }
    std::string const& heading = table.runHeadings.at(ordinal);
    std::optional<std::size_t> runIndex;
    for (std::size_t at = 0; at < view.runs.size(); ++at) {
        Value const* const key = view.runs.at(at).field(kGroupHeadingField);
        if (key != nullptr && key->asString() != nullptr && *key->asString() == heading) {
            runIndex = at;
        }
    }
    ValueList rows;
    for (std::size_t at = 0; runIndex && at < view.rows.size() && at < view.runOf.size(); ++at) {
        if (view.runOf.at(at) == *runIndex) {
            rows.push_back(view.rows.at(at));
        }
    }
    return rows;
}

namespace {

DocNode const* summaryPartOf(DocNode const& table, Value const& record) {
    Value const* const key = record.field(kSummaryField);
    if (key == nullptr || key->asString() == nullptr) {
        return nullptr;
    }
    std::span<DocNodePtr const> parts = table.partList("summaries");
    if (record.field(kSummaryRunField) != nullptr) {
        std::span<DocNodePtr const> const groups = table.partList("group");
        parts = groups.empty() ? std::span<DocNodePtr const>{} : groups.front()->partList("summaries");
    }
    for (DocNodePtr const& part : parts) {
        if (part->name("key") == *key->asString()) {
            return part.get();
        }
    }
    return nullptr;
}

}  // namespace

std::string summaryTestId(Value const& record, std::string const& column) {
    Value const* const key = record.field(kSummaryField);
    Value const* const run = record.field(kSummaryRunField);
    std::int64_t const* const ordinal = run != nullptr ? run->asInt() : nullptr;
    return "summary_" + (ordinal != nullptr ? std::to_string(*ordinal) + "_" : std::string{}) +
           (key != nullptr && key->asString() != nullptr ? *key->asString() : std::string{}) + "_" + column;
}

// NOLINTNEXTLINE(readability-convert-member-functions-to-static): a ScreenCore member so the summary lookup stays beside the other value readers that use the core's state
std::optional<Value> ScreenCore::summaryValue(DocNode const& table, Value const& record, std::string const& column,
                                              LowerCtx const& ctx, ValueList rows) {
    DocNode const* const part = summaryPartOf(table, record);
    if (part == nullptr) {
        return std::nullopt;
    }
    ExprPtr expr;
    for (DocNodePtr const& cells : part->partList("cells")) {
        if (ExprPtr const cell = cells->prop(column)) {
            expr = cell;
        }
    }
    if (!expr) {
        return std::nullopt;
    }
    Bindings scoped = ctx.bindings != nullptr ? *ctx.bindings : Bindings{};
    scoped.push_back(Binding{.name = "rows", .read = [rows = std::move(rows)] { return Value{rows}; }});
    scoped.push_back(Binding{.name = "column", .read = [column] { return Value{column}; }});
    return evalIn(*ctx.scope, &scoped, expr);
}

TableRowLook ScreenCore::summaryLook(DocNode const& table, LowerCtx const& ctx, Value const& record) {
    TableRowLook look;
    look.summary = true;
    // The part's label names the row; a row with none is named by its key.
    if (DocNode const* const part = summaryPartOf(table, record)) {
        if (ExprPtr const label = part->prop("label")) {
            look.label = Prop<std::string>{textOf(once(ctx, label), ctx.scope->formatting())};
        }
    }
    return look;
}

std::string ScreenCore::gridSummaryText(DocNode const& part, LowerCtx const& ctx, GridTable& held, Value const& record,
                                        std::string const& column) {
    Value const* const first = record.field(kSummaryFirstField);
    Value const* const count = record.field(kSummaryCountField);
    ValueList rows =
        summaryScopeRows(held.grid.source->view(), first != nullptr && first->asInt() != nullptr ? *first->asInt() : 0,
                         count != nullptr && count->asInt() != nullptr ? *count->asInt() : 0);
    std::optional<Value> const value = summaryValue(part, record, column, ctx, std::move(rows));
    if (!value) {
        return {};
    }
    auto const places = held.places.find(column);
    return summaryText(*value, places == held.places.end() ? std::nullopt : places->second, ctx.scope->formatting());
}

// The rows a grid with summaries or a group is mounted over: the decorator's, a data row's cell as the grid draws it,
// a summary row's cell its part's expression for the column (`gridSummaryText`), and a heading row its group's
// heading. Every column's cells are built per row, so a cell built for a data row is never rebound to a summary row.
TableSource ScreenCore::summarySource(DocNode const& part, LowerCtx const& ctx, GridTable& held,
                                      std::vector<TableColumn> const& columns) {
    auto const ids = std::make_shared<std::vector<std::string>>();
    for (TableColumn const& column : columns) {
        ids->push_back(column.id);
    }
    GridTable* const grid = &held;
    TableSource made;
    made.rows = held.summaries;
    made.rowScopedCells.assign(columns.size(), true);
    made.cellView = [this, ctx, &part, grid, ids](std::size_t column, reactive::Signal<Value> const& row) -> Node {
        std::string const columnId = column < ids->size() ? ids->at(column) : std::string{};
        Value const& record = row.peek();
        if (record.field(kSummaryField) == nullptr) {
            return grid->grid.source->cellContent(columnId, row);
        }
        Text text;
        text.common.testId = summaryTestId(record, columnId);
        text.text = slotProp<std::string>([this, ctx, &part, grid, columnId, record = &row] {
            return gridSummaryText(part, ctx, *grid, record->get(), columnId);
        });
        return ui::text(std::move(text));
    };
    made.rowView = [this, ctx, &part](reactive::Signal<Value> const& row) -> TableRowLook {
        Value const& record = row.peek();
        if (record.field(kSummaryField) != nullptr) {
            return summaryLook(part, ctx, record);
        }
        if (Value const* const heading = record.field(kGroupHeadingField); heading != nullptr && heading->asString()) {
            return headingRow(part, ctx, row, *heading->asString());
        }
        return {};
    };
    return made;
}

// NOLINTBEGIN(misc-no-recursion): an expression nests its arguments and fields, and a node its children; the walks
// follow them.
void ScreenCore::ownQueriesOf(ExprPtr const& expr, RtScope const& scope, BoundaryRuntime& boundary,
                              std::set<std::string, std::less<>>& seen) {
    if (!expr) {
        return;
    }
    bool const control = expr->kind == ExprKind::Query || expr->kind == ExprKind::Pending ||
                         expr->kind == ExprKind::Error || expr->kind == ExprKind::ErrorKind ||
                         expr->kind == ExprKind::Refusal;
    if (control || expr->kind == ExprKind::Ref) {
        NameRef const* const found = scope.find(expr->name);
        if (found != nullptr && found->category == NameCategory::Query && seen.insert(expr->name).second) {
            boundary.queries.push_back(found->query);
        }
        if (expr->kind == ExprKind::Ref && found != nullptr && found->category == NameCategory::Let &&
            seen.insert("let:" + expr->name).second) {
            ownQueriesOf(found->letExpr, scope, boundary, seen);
        }
    }
    for (ExprPtr const& arg : expr->args) {
        ownQueriesOf(arg, scope, boundary, seen);
    }
    for (auto const& [name, field] : expr->fields) {
        static_cast<void>(name);
        ownQueriesOf(field, scope, boundary, seen);
    }
}

void ScreenCore::ownQueries(DocNode const& node, RtScope const& scope, BoundaryRuntime& boundary,
                            std::set<std::string, std::less<>>& seen) {
    if (node.kind == "boundary") {
        return;
    }
    for (auto const& [key, expr] : node.props) {
        static_cast<void>(key);
        ownQueriesOf(expr, scope, boundary, seen);
    }
    for (auto const& [key, nodes] : node.children) {
        static_cast<void>(key);
        for (DocNodePtr const& child : nodes) {
            ownQueries(*child, scope, boundary, seen);
        }
    }
    for (auto const& [key, parts] : node.parts) {
        static_cast<void>(key);
        for (DocNodePtr const& part : parts) {
            ownQueries(*part, scope, boundary, seen);
        }
    }
}
// NOLINTEND(misc-no-recursion)

Node ScreenCore::lowerBoundary(DocNode const& node, LowerCtx const& ctx) {
    auto const boundary = std::make_shared<BoundaryRuntime>();
    std::set<std::string, std::less<>> seen;
    for (DocNodePtr const& child : node.nodes("children")) {
        ownQueries(*child, *ctx.scope, *boundary, seen);
    }
    auto const bindings = std::make_shared<Bindings>(*ctx.bindings);
    bindings->push_back(Binding{
        .name = "boundary",
        .read =
            [boundary] {
                for (QueryRuntime const* const query : boundary->queries) {
                    Value const error = query->error();
                    if (!error.isNull() && query->value().isNull()) {
                        return Value{ValueRecord{{"error", error}}};
                    }
                }
                return Value{ValueRecord{{"error", Value{}}}};
            },
    });
    LowerCtx const errorCtx{.scope = ctx.scope, .bindings = bindings, .boundary = boundary};
    LowerCtx const childCtx{.scope = ctx.scope, .bindings = ctx.bindings, .boundary = boundary};
    auto const group = [this](std::span<DocNodePtr const> nodes, LowerCtx const& where) -> Node {
        return nodes.empty() ? ui::spacer() : lowerAll(nodes, where);
    };
    Switch spec;
    spec.cases.push_back(SwitchCase{.key = Key{std::int64_t{0}}, .node = group(node.nodes("loading"), childCtx)});
    spec.cases.push_back(SwitchCase{.key = Key{std::int64_t{1}}, .node = group(node.nodes("error"), errorCtx)});
    spec.cases.push_back(SwitchCase{.key = Key{std::int64_t{2}}, .node = group(node.nodes("children"), childCtx)});
    spec.selector = slotProp<Key>([boundary] {
        bool loading = false;
        for (QueryRuntime const* const query : boundary->queries) {
            if (!query->settled()) {
                loading = true;
            } else if (query->value().isNull() && !query->error().isNull()) {
                return Key{std::int64_t{1}};
            }
        }
        return Key{std::int64_t{loading ? 0 : 2}};
    });
    commonOf(node, ctx, nullptr, spec.common);
    return ui::switchOf(std::move(spec));
}

Node ScreenCore::lowerCustom(DocNode const& node, LowerCtx const& ctx) {
    std::string const name{node.name("name")};
    // A loaded document's custom node names a declared component, and every component has a fallback.
    Component const& component = _doc->components.at(name);
    std::vector<std::pair<std::string, ExprPtr>> props;
    for (DocNodePtr const& record : node.partList("props")) {
        for (auto const& [key, expr] : record->props) {
            props.emplace_back(key, expr);
        }
    }
    auto const bindings = std::make_shared<Bindings>();
    bindings->push_back(Binding{
        .name = "props",
        .read =
            [scope = ctx.scope, outer = ctx.bindings, props = std::move(props)] {
                ValueRecord record;
                for (auto const& [key, expr] : props) {
                    record.insert_or_assign(key, evalIn(*scope, outer.get(), expr));
                }
                return Value{std::move(record)};
            },
    });
    auto const scope = std::make_shared<RtScope>(*this, nullptr, nullptr, ctx.scope->nodes.depth() + 1);
    scope->formatSource = ctx.scope;
    scope->usePrefix = focusKeyOf(node.path, *ctx.scope) + ">";
    DocNode const& fallback = *component.fallback;
    declare(*scope, nullptr, {&fallback}, {});
    Node lowered = lower(fallback, LowerCtx{.scope = scope, .bindings = bindings, .boundary = nullptr});
    Custom spec{
        .name = name,
        .fallback = std::move(lowered),
        .props = {},
        .minVersion =
            static_cast<int>(std::clamp<std::int64_t>(component.version, 0, std::numeric_limits<int>::max())),
        .onEvent = {},
    };
    // Every declared prop is given, a missing one as its type's empty value: a component lacking one is refused.
    for (auto const& [prop, type] : component.props) {
        ExprPtr expr;
        for (DocNodePtr const& record : node.partList("props")) {
            if (ExprPtr const given = record->prop(prop)) {
                expr = given;
            }
        }
        if (type == "bool") {
            spec.props.push_back(CustomProp{.name = prop, .value = bindProp<bool>(ctx, expr, false, boolOf)});
        } else if (type == "number") {
            spec.props.push_back(CustomProp{
                .name = prop,
                .value = bindProp<double>(
                    ctx, expr, 0.0, [](Value const& value, FormatContext const&) { return componentNumber(value); }),
            });
        } else {
            spec.props.push_back(CustomProp{
                .name = prop,
                .value = bindProp<std::string>(ctx, expr, std::string{},
                                               [type](Value const& value, FormatContext const& format) {
                                                   return componentText(value, type, format);
                                               }),
            });
        }
    }
    spec.onEvent = [this, ctx, site = &node, declared = &component](std::string const& event,
                                                                    std::string const& payload) {
        Commands const* const commands = site->event("on." + event);
        if (commands == nullptr) {
            return;
        }
        auto const type = declared->events.find(event);
        runEvent(commands, ctx, componentPayload(payload, type == declared->events.end() ? nullptr : &type->second));
    };
    return ui::custom(std::move(spec));
}

namespace {

/// @brief One child of an arranged container, as the user's choice places it.
struct ArrangedChild {
    /// @brief The child's id.
    std::string id;
    /// @brief Whether the choice hides it.
    bool hidden = false;
};

/// @brief A container's children in the order a stored `arrange` choice gives, each marked shown or hidden.
///
/// A child the choice does not name keeps its declared place after the named ones, shown, and an entry that names no
/// child, names one twice or is not a `{"id", "hidden"}` record is skipped, so a new document version survives an old
/// choice (spec 5 §12b). A choice that is not a list leaves the declared order. Only `"hidden": true` hides.
/// @param stored   The point's current choice.
/// @param declared The container's children, in document order.
/// @return Every child, in order.
[[nodiscard]] std::vector<ArrangedChild> arrangedChildren(Value const& stored,
                                                          std::span<DocNode const* const> declared) {
    auto const isDeclared = [declared](std::string const& childId) {
        return std::ranges::find(declared, childId, &DocNode::id) != declared.end();
    };
    std::vector<ArrangedChild> out;
    std::set<std::string, std::less<>> placed;
    if (ValueList const* const entries = stored.asList()) {
        for (Value const& entry : *entries) {
            Value const* const named = entry.field("id");
            std::string const* const name = named == nullptr ? nullptr : named->asString();
            if (name == nullptr || !isDeclared(*name) || !placed.insert(*name).second) {
                continue;
            }
            Value const* const hidden = entry.field("hidden");
            bool const* const flag = hidden == nullptr ? nullptr : hidden->asBool();
            out.push_back(ArrangedChild{.id = *name, .hidden = flag != nullptr && *flag});
        }
    }
    for (DocNode const* const child : declared) {
        if (!placed.contains(child->id)) {
            out.push_back(ArrangedChild{.id = child->id});
        }
    }
    return out;
}

/// @brief The ids of the children an `arrange` choice shows, in its order (`arrangedChildren`, without the hidden).
/// @param stored   The point's current choice.
/// @param declared The container's children, in document order.
/// @return The ids to show, in order.
[[nodiscard]] std::vector<Value> arrangedOrder(Value const& stored, std::span<DocNode const* const> declared) {
    std::vector<Value> order;
    for (ArrangedChild const& child : arrangedChildren(stored, declared)) {
        if (!child.hidden) {
            order.emplace_back(child.id);
        }
    }
    return order;
}

/// @brief An arrangement as the choice an `arrange` point stores.
/// @param children Every child, in order.
/// @return `[{"id", "hidden"}, …]`.
[[nodiscard]] Value arrangementValue(std::vector<ArrangedChild> const& children) {
    ValueList entries;
    entries.reserve(children.size());
    for (ArrangedChild const& child : children) {
        entries.emplace_back(ValueRecord{{"id", Value{child.id}}, {"hidden", Value{child.hidden}}});
    }
    return Value{std::move(entries)};
}

/// @brief The element of a document tree with @p elementId.
/// @param node The subtree.
/// @param elementId The element's id.
/// @return The element, or `nullptr` when the subtree has none with that id.
// NOLINTNEXTLINE(misc-no-recursion): one level per level of the document tree.
[[nodiscard]] DocNode const* elementNode(DocNode const& node, std::string_view elementId) {
    if (node.id == elementId) {
        return &node;
    }
    for (auto const* const groups : {&node.children, &node.parts}) {
        for (auto const& [slot, nodes] : *groups) {
            static_cast<void>(slot);
            for (DocNodePtr const& child : nodes) {
                if (DocNode const* const found = elementNode(*child, elementId)) {
                    return found;
                }
            }
        }
    }
    return nullptr;
}

}  // namespace

std::string ScreenCore::arrangedBy(std::string const& elementId) const {
    if (elementId.empty()) {
        return {};
    }
    for (auto const& [name, point] : _doc->customize) {
        if (point.kind == "arrange" && point.of == elementId) {
            return name;
        }
    }
    return {};
}

// A keyed list over the children's ids rather than a stack, so a new choice moves the shown widgets, keeping their
// state and focus, and mounts or unmounts only what it shows or hides. A child is lowered in the container's own
// scope: arranging changes where it is, not what it reads.
Node ScreenCore::lowerArranged(DocNode const& node, LowerCtx const& ctx, ElementState* element,
                               std::string const& point, Axis axis, int gap) {
    std::vector<DocNode const*> declared;
    for (DocNodePtr const& child : node.nodes("children")) {
        declared.push_back(child.get());
    }
    auto order = [this, point, declared] { return arrangedOrder(prefs.get().at(point), declared); };
    auto key = [](Value const& childId) { return keyOf(childId); };
    LowerCtx childCtx = ctx;
    childCtx.listGap = axis == Axis::Vertical ? gap : 0;
    auto view = [this, childCtx, declared](reactive::Signal<Value> const& row) -> Node {
        // The order lists only declared ids, so the lookup always finds the child.
        auto const found = std::ranges::find(declared, textOf(row.peek(), session.formatting()), &DocNode::id);
        return lower(**found, childCtx);
    };
    ForEach spec{
        .model = std::make_shared<TypedForEach<Value> const>(std::move(order), std::move(key), std::move(view)),
        .axis = axis,
        .gap = gap,
    };
    commonOf(node, ctx, element, spec.common);
    return makeNode(std::move(spec));
}

// One row per child of the arranged container, hidden ones included: a checkbox shows or hides it, and two buttons
// move it a place. Each edit stores the whole arrangement, so the stored list always names every child.
Node ScreenCore::lowerArrangeEditor(DocNode const& node, LowerCtx const& ctx, std::string const& point,
                                    CustomizePoint const& spec, std::function<void(Value)> const& choose) {
    // The loader checked that `of` names a column or row of this screen.
    DocNode const* const container = elementNode(*_doc->view, spec.of);
    std::vector<DocNode const*> declared;
    for (DocNodePtr const& child : container->nodes("children")) {
        declared.push_back(child.get());
    }
    auto const current = [this, point, declared] { return arrangedChildren(prefs.get().at(point), declared); };
    auto const place = [current](std::string const& childId) {
        auto const children = current();
        return static_cast<std::size_t>(std::ranges::find(children, childId, &ArrangedChild::id) - children.begin());
    };
    // Applies an edit to the arrangement as it stands, read untracked: the edit is a transition, not a binding.
    auto const edit = [this, current, choose](std::string const& childId, auto const& change) {
        auto children = runtime().untracked(current);
        auto const found = std::ranges::find(children, childId, &ArrangedChild::id);
        change(children, static_cast<std::size_t>(found - children.begin()));
        choose(arrangementValue(children));
    };
    auto rows = [current] {
        std::vector<Value> ids;
        for (ArrangedChild const& child : current()) {
            ids.emplace_back(child.id);
        }
        return ids;
    };
    auto key = [](Value const& childId) { return keyOf(childId); };
    auto view = [this, current, place, edit](reactive::Signal<Value> const& row) -> Node {
        std::string const childId = textOf(row.peek(), session.formatting());
        Checkbox shown{.label = childId};
        shown.checked = slotProp<bool>([current, childId] {
            auto const children = current();
            return !std::ranges::find(children, childId, &ArrangedChild::id)->hidden;
        });
        shown.onToggle = [edit, childId](bool checked) {
            edit(childId, [checked](std::vector<ArrangedChild>& children, std::size_t index) {
                children.at(index).hidden = !checked;
            });
        };
        Button moveUp{.label = std::string{"\xE2\x86\x91"}};
        moveUp.onClick = [edit, childId] {
            // Clamped rather than refused: at the edge the child swaps with itself, which stores the same order.
            edit(childId, [](std::vector<ArrangedChild>& children, std::size_t index) {
                std::swap(children.at(index), children.at(std::max(index, std::size_t{1}) - 1));
            });
        };
        moveUp.common.enabled = slotProp<bool>([place, childId] { return place(childId) > 0; });
        moveUp.common.a11y.name = "Move " + childId + " up";
        moveUp.common.testId = "arrange-up-" + childId;
        Button moveDown{.label = std::string{"\xE2\x86\x93"}};
        moveDown.onClick = [edit, childId] {
            edit(childId, [](std::vector<ArrangedChild>& children, std::size_t index) {
                std::swap(children.at(index), children.at(std::min(index + 1, children.size() - 1)));
            });
        };
        moveDown.common.enabled =
            slotProp<bool>([current, place, childId] { return place(childId) + 1 < current().size(); });
        moveDown.common.a11y.name = "Move " + childId + " down";
        moveDown.common.testId = "arrange-down-" + childId;
        shown.common.testId = "arrange-shown-" + childId;
        return ui::row(Row{
            .children =
                {
                    ui::checkbox(std::move(shown)),
                    ui::button(std::move(moveUp)),
                    ui::button(std::move(moveDown)),
                },
            .gap = 1,
        });
    };
    ForEach editor{
        .model = std::make_shared<TypedForEach<Value> const>(std::move(rows), std::move(key), std::move(view)),
    };
    commonOf(node, ctx, nullptr, editor.common);
    return makeNode(std::move(editor));
}

Node ScreenCore::lowerCustomize(DocNode const& node, LowerCtx const& ctx) {
    std::string const point{node.name("point")};
    // A loaded document's customize node names a declared point, and every point has a choice in `prefs`.
    CustomizePoint const& spec = _doc->customize.at(point);
    auto const choose = [this, point](Value value) {
        session.asUser([&] {
            ValueRecord next = prefs.peek();
            next.insert_or_assign(point, value);
            prefs.set(std::move(next));
        });
        if (host != nullptr) {
            host->preferenceChanged(_doc->id, point, value);
        }
    };
    if (spec.kind == "arrange") {
        return lowerArrangeEditor(node, ctx, point, spec, choose);
    }
    if (spec.kind == "choice") {
        Select select;
        std::vector<SelectOption> options;
        options.reserve(spec.options.size());
        for (std::string const& option : spec.options) {
            options.push_back(SelectOption{.key = Key{option}, .label = option});
        }
        select.options = std::move(options);
        select.selected = slotProp<std::optional<Key>>([this, point] -> std::optional<Key> {
            Value const& chosen = prefs.get().at(point);
            return chosen.isNull() ? std::nullopt : std::optional<Key>{keyOf(chosen)};
        });
        select.onSelect = [choose](Key const& key) { choose(valueOfKey(key)); };
        commonOf(node, ctx, nullptr, select.common);
        return ui::select(std::move(select));
    }
    if (spec.kind == "value" && (spec.type.kind == TypeKind::String || spec.type.isAny())) {
        TextInput input;
        input.value =
            slotProp<std::string>([this, point] { return textOf(prefs.get().at(point), session.formatting()); });
        input.onChange = [choose](std::string text) { choose(Value{std::move(text)}); };
        commonOf(node, ctx, nullptr, input.common);
        return ui::textInput(std::move(input));
    }
    Text placeholder{.text = point, .role = TextRole::Muted};
    commonOf(node, ctx, nullptr, placeholder.common);
    return ui::text(std::move(placeholder));
}

}  // namespace morph::ui::detail

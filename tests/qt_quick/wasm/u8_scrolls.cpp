// SPDX-License-Identifier: Apache-2.0
//
// U8 (docs/spec/table/table.md, "Benchmarks: every surface"): the interpreter and the Qt Quick column window in the
// WebAssembly build, in a browser. Two tables are mounted in one 1,280 x 800 window, one after the other.
//
// 1. A `rows` table of 20,000 rows over document state (U1, U2, F3):
//      U1  a header click on the text column `name`, to the first frame that shows the sorted rows
//      U2  one row's name replaced in the state, to the first frame that shows it
//      then the vertical scrolls of F3.
// 2. A model-backed table of 100,000 x 300 over an in-process `bench::WideTableModel`, through a `Bridge` on a
//    `LocalBackend` (F3 and F4: the vertical and the horizontal scrolls, windowed in both directions).
//
// Output, one line each, on the console:
//
//   mounted table=<rows|model> rows=<total> realized=<rows> columns=<columns realized> ms=<time>
//   U1 handler_ms=<click to end of the owner's work> frame_ms=<click to the frame showing the result> ...
//   U2 handler_ms=... frame_ms=...
//   scroll name=<...> frames=<n> work_p50=<ms> work_p99=<ms> interval_p50=<ms> interval_p99=<ms> moved=<px>
//   within=<yes|no> verdict F3/F4 budgets hold=<yes|no> done
//
// A frame's figure is `work`: everything this program does between two frames, plus the scene graph's synchronize,
// render and swap of the frame it caused (`beforeSynchronizing` to `frameSwapped`). The program's part is the owner's
// tasks that run at the top of a frame (fetch replies), the scroll position's change and its own tasks, and the
// callbacks of the session's `QtScheduler` timers (the settle debounce) that fired since the last frame. Item polish,
// which runs before `beforeSynchronizing`, is not in it. `interval` is the time between
// two `frameSwapped` signals, which in a browser follows the display's refresh and holds the idle time, so a table
// that does no work still shows about 16.7 ms there; `work` is what the budgets of F3 and F4 (p50 <= 8 ms,
// p99 <= 16.6 ms) are compared with, and `interval` shows whether frames were dropped.
//
// Every figure has a guard that fails the run with a line starting `failed:` when the path it times was not reached:
// the table's totals, a window of fewer than the whole table, a header click that changed the sort, a first row that
// changed, a scroll that moved. A U1 or U2 whose first row did not change within kMaxFramesWaited frames is printed
// with `changed=no`.
//
// Build it in the Qt for WebAssembly tree with MORPH_BUILD_TESTS=ON, and run it in a headless browser:
//   emrun --browser chrome --browser_args=--headless=new u8_scrolls.html

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QGuiApplication>
#include <QMouseEvent>
#include <QPointF>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QSGRendererInterface>
#include <QString>
#include <QTimer>
#include <QVariant>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <format>
#include <functional>
#include <memory>
#include <morph/core/backend.hpp>
#include <morph/core/bridge.hpp>
#include <morph/qt_quick/backend.hpp>
#include <morph/qt_quick/scheduler.hpp>
#include <morph/reactive/runtime.hpp>
#include <morph/reactive/scheduler.hpp>
#include <morph/table/description.hpp>
#include <morph/table/query.hpp>
#include <morph/testing/step_executor.hpp>
#include <morph/ui/author.hpp>
#include <morph/ui/document.hpp>
#include <morph/ui/interpreter.hpp>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "qt_quick/widget.hpp"
#include "table/wide_table_model.hpp"

namespace {

namespace ui = morph::ui;
using Table = bench::WideTableModel;

// Frames the run waits for a table to appear, or for a change to show, before it reports a failure.
constexpr int kMaxFramesWaited = 600;
// Frames of one scroll: two seconds at 60 frames a second.
constexpr int kScrollFrames = 120;
// Frames the run lets a table settle for after a scroll, so the next one starts from rest.
constexpr int kSettleFrames = 30;
// The budgets of F3 and F4.
constexpr double kP50Budget = 8.0;
constexpr double kP99Budget = 16.6;
constexpr std::size_t kRowsTableRows = 20'000;
constexpr std::int64_t kModelRows = 100'000;
constexpr std::size_t kModelColumns = 300;

void print(std::string const& line) {
    std::puts(line.c_str());
    static_cast<void>(std::fflush(stdout));
}

double percentile(std::vector<double> samples, double q) {
    if (samples.empty()) {
        return 0.0;
    }
    std::ranges::sort(samples);
    auto const index = static_cast<std::size_t>(std::lround(q * static_cast<double>(samples.size() - 1)));
    return samples[std::min(index, samples.size() - 1)];
}

std::string jsonText(std::string const& text) {
    std::string out = "\"";
    for (char const character : text) {
        if (character == '"' || character == '\\') {
            out += '\\';
        }
        out += character;
    }
    return out + '"';
}

// The rows of the `rows` table: `id`, `name` from the fixture's text column `t001` and `qty` from `i001`. Returns
// the row's text too, so the run can name the row that sorts first.
struct RowsFixture {
    std::string items;
    std::int64_t firstByName = 0;
};

RowsFixture rowsFixture() {
    bench::WideTableSource const source{{.rows = static_cast<std::int64_t>(kRowsTableRows), .columns = 30, .seed = 1}};
    auto const snapshot = source.snapshot();
    auto const columns = source.columns();
    auto const index = [&](std::string_view id) {
        return static_cast<std::size_t>(std::ranges::find(columns, id, &morph::table::ColumnInfo::id) -
                                        columns.begin());
    };
    auto const name = index("t001");
    auto const qty = index("i001");
    RowsFixture out;
    std::string smallest;
    for (std::size_t row = 0; row < snapshot->rowCount(); ++row) {
        std::string const text = morph::table::displayText(snapshot->cell(row, name));
        auto const quantity = snapshot->cell(row, qty);
        out.items +=
            std::format(R"({}{{"id":{},"name":{},"qty":{}}})", row == 0 ? "" : ",", row + 1, jsonText(text),
                        std::holds_alternative<std::monostate>(quantity) ? std::string{"null"}
                                                                         : morph::table::displayText(quantity));
        if (row == 0 || text < smallest) {
            smallest = text;
            out.firstByName = static_cast<std::int64_t>(row + 1);
        }
    }
    return out;
}

// A table over state `items`, sortable on `name` by its own header, and a button that gives the row keyed
// @p renamed a name that sorts last. As tall as the window: a table given no height takes all its rows' height.
std::string rowsDocument(RowsFixture const& fixture) {
    return std::format(R"({{
  "vocab": "ui/1", "id": "u8Rows",
  "state": {{ "items": {{ "init": {{ "lit": [{}] }} }} }},
  "view": {{ "kind": "column", "children": [
    {{ "kind": "button", "testId": "rename", "label": "rename",
       "onClick": {{ "set": "items", "value": {{ "map": {{ "ref": "items" }}, "as": "r", "to": {{ "if": [
         {{ "fn": "eq", "args": [ {{ "ref": "r.id" }}, {} ] }},
         {{ "obj": {{ "id": {{ "ref": "r.id" }}, "name": "zzzz", "qty": {{ "ref": "r.qty" }} }} }},
         {{ "ref": "r" }} ] }} }} }} }},
    {{ "kind": "table", "testId": "rows", "sizing": {{ "lit": {{ "height": {{ "stretch": 1 }} }} }},
       "rows": {{ "ref": "items" }},
       "columns": [ {{ "id": "id", "label": "Id" }}, {{ "id": "name", "label": "Name", "sortable": true }},
                    {{ "id": "qty", "label": "Qty" }} ] }} ] }} }})",
                       fixture.items, fixture.firstByName);
}

// A table over the fixture's `wide` table, 100,000 x 300, as tall as the window.
constexpr std::string_view kModelDocument =
    R"({"vocab":"ui/1","id":"u8Model","models":{"m":{"type":"WideTableModel"}},)"
    R"("view":{"kind":"table","id":"list","testId":"wide","model":"m","table":"wide",)"
    R"("sizing":{"lit":{"height":{"stretch":1}}}}})";

// A document that binds no model: nothing is ever dispatched.
class NoModels final : public ui::Dispatcher {
public:
    [[nodiscard]] std::shared_ptr<ui::ModelChannel> bind(std::string const& typeId, morph::bridge::BindSharing sharing,
                                                         std::string const& instanceKey) override {
        static_cast<void>(typeId);
        static_cast<void>(sharing);
        static_cast<void>(instanceKey);
        return nullptr;
    }
};

// One screen's machinery. Declared so that it is destroyed in the order the screen needs: the screen first, then the
// interpreter and the session it uses, the scheduler and the dispatcher, the bridge last.
// A scheduler that adds the time of its callbacks to @p total, so the timers' work counts as the frame's.
class TimedScheduler final : public morph::reactive::Scheduler {
public:
    TimedScheduler(morph::reactive::Scheduler& inner, std::function<double()> clock, double& total)
        : _inner{&inner}, _clock{std::move(clock)}, _total{&total} {}

    [[nodiscard]] morph::reactive::TimerHandle after(std::chrono::milliseconds delay,
                                                     std::function<void()> fn) override {
        return _inner->after(delay, timed(std::move(fn)));
    }
    [[nodiscard]] morph::reactive::TimerHandle every(std::chrono::milliseconds period,
                                                     std::function<void()> fn) override {
        return _inner->every(period, timed(std::move(fn)));
    }

private:
    std::function<void()> timed(std::function<void()> fn) {
        return [this, fn = std::move(fn)] {
            double const began = _clock();
            fn();
            *_total += _clock() - began;
        };
    }

    morph::reactive::Scheduler* _inner;
    std::function<double()> _clock;
    double* _total;
};

struct Client {
    std::unique_ptr<morph::bridge::Bridge> bridge;
    std::unique_ptr<ui::Dispatcher> dispatcher;
    std::unique_ptr<morph::qt_quick::QtScheduler> scheduler;
    std::unique_ptr<TimedScheduler> timed;
    std::unique_ptr<ui::Session> session;
    std::unique_ptr<ui::Interpreter> interpreter;
    std::unique_ptr<ui::Screen> screen;

    // Releases the members in reverse order of declaration; a move assignment would release them in declaration
    // order, which destroys the bridge under a screen that still uses it.
    void reset() {
        screen.reset();
        interpreter.reset();
        session.reset();
        timed.reset();
        scheduler.reset();
        dispatcher.reset();
        bridge.reset();
    }
};

struct Scroll {
    std::string name;
    char const* flickable = "";  // objectName of the Flickable that scrolls
    char const* property = "";   // "contentY" or "contentX"
    double step = 0;             // per frame
    double limit = 0;            // the furthest position
};

struct Run {
    morph::testing::StepExecutor owner;
    morph::reactive::Runtime runtime{owner};
    QQmlEngine engine;
    QQuickWindow window;
    morph::qt_quick::Backend backend{engine, *window.contentItem()};
    Client client;
    RowsFixture fixture;
    std::string documentText;

    enum class Phase {
        RowsMount,
        RowsPrime,
        RowsSort,
        RowsSortWait,
        RowsRename,
        RowsRenameWait,
        Scrolling,
        Settling,
        ModelMount,
        ModelPrime,
        Finished
    };
    Phase phase = Phase::RowsMount;
    bool modelPhase = false;  // which table the scroll sequence is on

    QElapsedTimer clock;
    double lastFrame = 0;  // when the last frame was swapped
    double syncBegan = 0;  // when its synchronize began
    double carryMs = 0;    // the owner's tasks and the step since the last frame, for the next frame's work
    double timerMs = 0;    // the time of the scheduler's callbacks since the last frame
    int frames = 0;        // frames waited in the current phase
    double actionAt = 0;   // when U1 or U2's action began
    double handlerMs = 0;
    std::string before;  // the first row's text before U1 or U2's action
    QQuickItem* table = nullptr;
    qint64 primeStep = 0;
    double mountBegan = 0;

    std::vector<Scroll> scrolls;
    std::size_t scrollIndex = 0;
    int scrollFrame = 0;
    double scrollFrom = 0;
    double position = 0;
    std::vector<double> work;
    std::vector<double> interval;
    bool allWithin = true;
    bool failed = false;

    [[nodiscard]] double now() const { return static_cast<double>(clock.nsecsElapsed()) / 1.0e6; }

    void fail(std::string const& message) {
        print("failed: " + message);
        failed = true;
        phase = Phase::Finished;
    }

    // A left click at the centre of @p item.
    void tap(QQuickItem const& item) {
        QPointF const scene = item.mapToScene(QPointF{item.width() / 2, item.height() / 2});
        QPointF const global = window.mapToGlobal(scene);
        QMouseEvent press{QEvent::MouseButtonPress, scene,          scene,         global,
                          Qt::LeftButton,           Qt::LeftButton, Qt::NoModifier};
        QCoreApplication::sendEvent(&window, &press);
        QMouseEvent release{
            QEvent::MouseButtonRelease, scene, scene, global, Qt::LeftButton, Qt::NoButton, Qt::NoModifier};
        QCoreApplication::sendEvent(&window, &release);
    }

    [[nodiscard]] QQuickItem* child(QString const& name) const {
        return table == nullptr ? nullptr : table->findChild<QQuickItem*>(name);
    }

    // The texts of the first body row the table shows, left to right, joined by '|': the cells at the least vertical
    // position below the header.
    [[nodiscard]] std::string firstRow() const {
        if (table == nullptr) {
            return {};
        }
        struct Cell {
            double y;
            double x;
            std::string text;
        };
        std::vector<Cell> cells;
        double headerBottom = 0;
        for (QQuickItem* const item : table->findChildren<QQuickItem*>()) {
            QString const name = item->objectName();
            QPointF const at = item->mapToItem(table, QPointF{0, 0});
            if (name.startsWith(QStringLiteral("header"))) {
                headerBottom = std::max(headerBottom, at.y() + item->height());
                continue;
            }
            QVariant const text = item->property("text");
            if (!text.isValid() || !item->isVisible() || name == QStringLiteral("statusText") ||
                name == QStringLiteral("retry") || text.toString().isEmpty()) {
                continue;
            }
            cells.push_back({at.y(), at.x(), text.toString().toStdString()});
        }
        double top = 1.0e18;
        for (Cell const& cell : cells) {
            if (cell.y >= headerBottom - 0.5) {
                top = std::min(top, cell.y);
            }
        }
        std::vector<Cell> row;
        for (Cell const& cell : cells) {
            if (std::abs(cell.y - top) < 1.0) {
                row.push_back(cell);
            }
        }
        std::ranges::sort(row, {}, &Cell::x);
        std::string out;
        for (Cell const& cell : row) {
            out += out.empty() ? "" : "|";
            out += cell.text;
        }
        return out;
    }

    // Starts the vertical, and for the model table the horizontal, scrolls of F3 and F4.
    bool prepareScrolls() {
        QQuickItem* const flick = child(QStringLiteral("flick"));
        QQuickItem* const hflick = child(QStringLiteral("hflick"));
        if (flick == nullptr) {
            fail("no flick");
            return false;
        }
        double rowHeight = 0;
        double columnWidth = 0;
        double const contentHeight = flick->property("contentHeight").toReal();
        if (modelPhase) {
            rowHeight = table->property("meanRowHeight").toReal();
            columnWidth = table->property("windowTotalWidth").toReal() / static_cast<double>(kModelColumns);
        } else {
            rowHeight = contentHeight / static_cast<double>(kRowsTableRows);
        }
        double const bottom = std::max(0.0, contentHeight - flick->height());
        if (rowHeight <= 0 || bottom <= 0) {
            fail("no vertical extent: row " + std::to_string(rowHeight) + " px, bottom " + std::to_string(bottom));
            return false;
        }
        std::string const which = modelPhase ? "model 100,000x300" : "rows 20,000";
        scrolls.clear();
        scrolls.push_back(
            {"F3 " + which + ": vertical 20 rows/s", "flick", "contentY", rowHeight * 20.0 / 60.0, bottom});
        scrolls.push_back({"F3 " + which + ": vertical fling 2,000 rows/s", "flick", "contentY",
                           rowHeight * 2'000.0 / 60.0, bottom});
        scrolls.push_back({"F3 " + which + ": thumb drag across the table in 2 s", "flick", "contentY",
                           bottom / kScrollFrames, bottom});
        if (modelPhase) {
            if (hflick == nullptr) {
                fail("no hflick");
                return false;
            }
            double const right = std::max(0.0, hflick->property("contentWidth").toReal() - hflick->width());
            if (columnWidth <= 0 || right <= 0) {
                fail("no horizontal extent: column " + std::to_string(columnWidth) + " px, right " +
                     std::to_string(right));
                return false;
            }
            scrolls.push_back(
                {"F4 " + which + ": horizontal 10 columns/s", "hflick", "contentX", columnWidth * 10.0 / 60.0, right});
            scrolls.push_back({"F4 " + which + ": horizontal fling 100 columns/s", "hflick", "contentX",
                               columnWidth * 100.0 / 60.0, right});
        } else {
            print("scroll skipped: F4 rows 20,000: the table has three columns and does not scroll sideways");
        }
        scrollIndex = 0;
        beginScroll();
        return true;
    }

    void beginScroll() {
        QQuickItem* const flickable = child(QString::fromLatin1(scrolls[scrollIndex].flickable));
        if (flickable == nullptr) {
            fail("no " + std::string{scrolls[scrollIndex].flickable});
            return;
        }
        scrollFrom = flickable->property(scrolls[scrollIndex].property).toReal();
        position = scrollFrom;
        scrollFrame = 0;
        work.clear();
        interval.clear();
        carryMs = 0;
        timerMs = 0;
        phase = Phase::Scrolling;
    }

    void finishScroll() {
        Scroll const& scroll = scrolls[scrollIndex];
        QQuickItem* const flickable = child(QString::fromLatin1(scroll.flickable));
        double const moved = flickable == nullptr ? 0.0 : flickable->property(scroll.property).toReal() - scrollFrom;
        double const p50 = percentile(work, 0.5);
        double const p99 = percentile(work, 0.99);
        bool const within = p50 <= kP50Budget && p99 <= kP99Budget;
        allWithin = allWithin && within;
        print(
            std::format("scroll name={} frames={} work_p50={:.2f} work_p99={:.2f} interval_p50={:.2f} "
                        "interval_p99={:.2f} moved={:.0f} within={}",
                        scroll.name, work.size(), p50, p99, percentile(interval, 0.5), percentile(interval, 0.99),
                        moved, within ? "yes" : "no"));
        if (moved <= 0) {
            fail("scroll '" + scroll.name + "' did not move");
            return;
        }
        if (flickable != nullptr) {
            flickable->setProperty(scroll.property, 0.0);
        }
        frames = 0;
        phase = Phase::Settling;
    }

    // Mounts @p text on a client built by @p build.
    bool mount(std::string const& text, bool model) {
        Client built;
        ui::Catalog catalog;
        if (model) {
            ui::author::CatalogFacts facts;
            facts.action<Table, morph::table::QueryTable<&Table::wide>>();
            facts.action<Table, morph::table::DescribeTable<&Table::wide>>();
            catalog = facts.catalog();
            built.bridge =
                std::make_unique<morph::bridge::Bridge>(std::make_unique<morph::backend::LocalBackend>(owner), owner);
            built.dispatcher = std::make_unique<ui::BridgeDispatcher>(*built.bridge, &owner);
            built.scheduler = std::make_unique<morph::qt_quick::QtScheduler>(owner, &window);
            built.timed = std::make_unique<TimedScheduler>(*built.scheduler, [this] { return now(); }, timerMs);
        } else {
            built.dispatcher = std::make_unique<NoModels>();
        }
        ui::SessionOptions options;
        options.scheduler = built.timed.get();
        options.catalog = catalog;
        built.session = std::make_unique<ui::Session>(runtime, options);
        built.interpreter =
            std::make_unique<ui::Interpreter>(*built.session, *built.dispatcher, ui::InterpreterOptions{});
        auto loaded = ui::loadDocumentText(text, ui::LoadOptions{.catalog = catalog});
        if (!loaded) {
            fail("refused: " + loaded.error().message());
            return false;
        }
        auto screen = built.interpreter->mount(std::make_shared<ui::Document const>(std::move(*loaded)), {}, backend);
        if (!screen) {
            fail("refused: " + screen.error().message());
            return false;
        }
        built.screen = std::move(*screen);
        client.reset();
        client = std::move(built);
        owner.runAll();
        return true;
    }

    // Looks for the table once it exists; false while it does not, with a failure after kMaxFramesWaited frames.
    bool findTable(QString const& testId) {
        auto const* const widget = morph::qt_quick::detail::findWidget(testId);
        if (widget == nullptr || widget->item() == nullptr) {
            if (++frames >= kMaxFramesWaited) {
                fail("no table after " + std::to_string(kMaxFramesWaited) + " frames");
            } else {
                window.requestUpdate();
            }
            return false;
        }
        table = widget->item();
        return true;
    }

    // The table's range is reported to C++ only when it scrolls: one pixel down and back realizes what the viewport
    // shows.
    void prime(Phase next) {
        QQuickItem* const flick = child(QStringLiteral("flick"));
        if (flick == nullptr) {
            fail("no flick");
            return;
        }
        double const at = flick->property("contentY").toReal();
        if (primeStep == 0) {
            flick->setProperty("contentY", at + 1.0);
            primeStep = 1;
        } else {
            flick->setProperty("contentY", std::max(0.0, at - 1.0));
            primeStep = 0;
            frames = 0;
            phase = next;
        }
        window.requestUpdate();
    }

    // The action of U1 or U2: clicks @p target and runs the owner's work.
    void act(QQuickItem* target, Phase waiting) {
        if (target == nullptr) {
            fail("no item to click");
            return;
        }
        before = firstRow();
        actionAt = now();
        tap(*target);
        owner.runAll();
        handlerMs = now() - actionAt;
        frames = 0;
        phase = waiting;
        window.requestUpdate();
    }

    // Waits for the first frame whose first row differs from @p before, then prints @p label's line.
    bool shown(char const* label, Phase next) {
        std::string const after = firstRow();
        bool const changed = after != before;
        if (!changed && ++frames < kMaxFramesWaited) {
            window.requestUpdate();
            return false;
        }
        print(std::format("{} handler_ms={:.2f} frame_ms={:.2f} frames_waited={} changed={} rows={}", label, handlerMs,
                          now() - actionAt, frames, changed ? "yes" : "no", kRowsTableRows));
        frames = 0;
        phase = next;
        return true;
    }

    void onFrame() {
        double const swapped = now();
        double const renderMs = swapped - syncBegan;
        double const carried = carryMs + timerMs;
        carryMs = 0;
        timerMs = 0;
        owner.runAll();
        carryMs += now() - swapped;
        switch (phase) {
            case Phase::RowsMount: {
                if (!findTable(QStringLiteral("rows"))) {
                    return;
                }
                int const total = table->property("totalRows").toInt();
                int const realized = table->property("realizedCount").toInt();
                if (static_cast<std::size_t>(total) != kRowsTableRows || realized <= 0 || realized >= total) {
                    fail("rows=" + std::to_string(total) + " realized=" + std::to_string(realized));
                    return;
                }
                print(std::format("mounted table=rows rows={} realized={} columns=3 ms={:.0f}", total, realized,
                                  swapped - mountBegan));
                phase = Phase::RowsPrime;
                window.requestUpdate();
                return;
            }
            case Phase::RowsPrime:
                prime(Phase::RowsSort);
                return;
            case Phase::RowsSort:
                act(child(QStringLiteral("header1")), Phase::RowsSortWait);
                return;
            case Phase::RowsSortWait:
                if (table->property("sortColumn").toInt() != 1) {
                    fail("the header click did not sort the name column");
                    return;
                }
                if (shown("U1", Phase::RowsRename)) {
                    window.requestUpdate();
                }
                return;
            case Phase::RowsRename: {
                auto const* const button = morph::qt_quick::detail::findWidget(QStringLiteral("rename"));
                act(button == nullptr ? nullptr : button->item(), Phase::RowsRenameWait);
                return;
            }
            case Phase::RowsRenameWait:
                if (shown("U2", Phase::Scrolling)) {
                    modelPhase = false;
                    if (prepareScrolls()) {
                        window.requestUpdate();
                    }
                }
                return;
            case Phase::Scrolling: {
                if (scrollFrame > 0) {
                    work.push_back(carried + renderMs);
                    interval.push_back(swapped - lastFrame);
                }
                lastFrame = swapped;
                if (scrollFrame >= kScrollFrames) {
                    finishScroll();
                    window.requestUpdate();
                    return;
                }
                Scroll const& scroll = scrolls[scrollIndex];
                QQuickItem* const flickable = child(QString::fromLatin1(scroll.flickable));
                if (flickable == nullptr) {
                    fail("no " + std::string{scroll.flickable});
                    return;
                }
                double const began = now();
                position = std::min(scroll.limit, position + scroll.step);
                flickable->setProperty(scroll.property, position);
                owner.runAll();
                carryMs += now() - began;
                ++scrollFrame;
                window.requestUpdate();
                return;
            }
            case Phase::Settling:
                if (++frames < kSettleFrames) {
                    window.requestUpdate();
                    return;
                }
                if (++scrollIndex < scrolls.size()) {
                    beginScroll();
                    window.requestUpdate();
                    return;
                }
                if (modelPhase) {
                    print(std::format("verdict F3 F4 budgets (work p50 <= {} ms, p99 <= {} ms) hold={}", kP50Budget,
                                      kP99Budget, allWithin ? "yes" : "no"));
                    print("done");
                    phase = Phase::Finished;
                    return;
                }
                // The rows table is done: the model table replaces it.
                table = nullptr;
                owner.runAll();
                client.reset();
                modelPhase = true;
                frames = 0;
                mountBegan = now();
                if (!mount(std::string{kModelDocument}, true)) {
                    return;
                }
                phase = Phase::ModelMount;
                window.requestUpdate();
                return;
            case Phase::ModelMount: {
                if (!findTable(QStringLiteral("wide"))) {
                    return;
                }
                int const total = table->property("totalRows").toInt();
                int const realized = table->property("realizedCount").toInt();
                if (total <= 0 || realized <= 0) {
                    if (++frames >= kMaxFramesWaited) {
                        fail("rows=" + std::to_string(total) + " realized=" + std::to_string(realized));
                    } else {
                        window.requestUpdate();
                    }
                    return;
                }
                if (total != kModelRows) {
                    fail("rows=" + std::to_string(total) + ", expected " + std::to_string(kModelRows));
                    return;
                }
                print(std::format("mounted table=model rows={} realized={} columns=pending ms={:.0f}", total, realized,
                                  swapped - mountBegan));
                frames = 0;
                phase = Phase::ModelPrime;
                window.requestUpdate();
                return;
            }
            case Phase::ModelPrime: {
                if (primeStep == 0 && !table->property("arranged").toBool()) {
                    if (++frames >= kMaxFramesWaited) {
                        fail("the model table never arranged its column window");
                        return;
                    }
                    window.requestUpdate();
                    return;
                }
                bool const wasFirst = primeStep == 0;
                prime(Phase::Scrolling);
                if (phase == Phase::Finished) {
                    return;
                }
                if (!wasFirst) {
                    int const realized = table->property("realizedCount").toInt();
                    int const columns = table->property("realizedColumnCount").toInt();
                    if (realized <= 0 || realized >= 100 || columns <= 0 || columns >= 100) {
                        fail("not windowed both ways: rows=" + std::to_string(realized) +
                             " columns=" + std::to_string(columns));
                        return;
                    }
                    print(std::format("windowed rows={} columns={}", realized, columns));
                    if (prepareScrolls()) {
                        window.requestUpdate();
                    }
                }
                return;
            }
            case Phase::Finished:
                return;
        }
    }
};

}  // namespace

int main(int argc, char* argv[]) {
    QQuickStyle::setStyle(QStringLiteral("Basic"));
    QQuickWindow::setGraphicsApi(QSGRendererInterface::Software);
    QGuiApplication app{argc, argv};
    auto* const run = new Run;  // lives until the process ends: the browser keeps the page after main returns
    run->window.resize(1280, 800);
    run->window.show();

    QTimer::singleShot(0, [run] {
        run->fixture = rowsFixture();
        run->documentText = rowsDocument(run->fixture);
        run->clock.start();
        run->mountBegan = run->now();
        if (!run->mount(run->documentText, false)) {
            return;
        }
        run->window.requestUpdate();
    });

    QObject::connect(&run->window, &QQuickWindow::beforeSynchronizing, &run->window,
                     [run] { run->syncBegan = run->now(); });
    QObject::connect(&run->window, &QQuickWindow::frameSwapped, &run->window, [run] { run->onFrame(); });

    return QCoreApplication::exec();
}

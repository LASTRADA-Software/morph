// SPDX-License-Identifier: Apache-2.0
//
// B5 (docs/spec/table/table.md, "Benchmarks: browser"): the WebAssembly build of the Qt Quick frontend mounts the
// `rows` table of 2,000 x 300 that the native F2 to F5 baseline mounts, scrolls it down by one viewport over one
// second, and prints to the console:
//
//   mounted rows=2000 ms=<mount time>
//   frame ms=<time between frames>     (one per frame of the scroll)
//   done
//
// Run it in a headless browser: emrun --browser chrome --browser_args=--headless=new wide_rows_baseline.html
// Its guard is the `mounted rows=2000` line: a mount that realized nothing, or a table of another size, prints
// something else.

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QGuiApplication>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QSGRendererInterface>
#include <QTimer>
#include <algorithm>
#include <cstdio>
#include <memory>
#include <morph/qt_quick/backend.hpp>
#include <morph/reactive/runtime.hpp>
#include <morph/testing/step_executor.hpp>
#include <morph/ui/document.hpp>
#include <morph/ui/interpreter.hpp>
#include <string>

#include "qt_quick/widget.hpp"
#include "qt_quick/wide_rows_document.hpp"
#include "table/wide_table_model.hpp"

namespace {

// The document binds no model, so nothing is ever dispatched.
class NoModels final : public morph::ui::Dispatcher {
public:
    [[nodiscard]] std::shared_ptr<morph::ui::ModelChannel> bind(std::string const& typeId,
                                                                morph::bridge::BindSharing sharing,
                                                                std::string const& instanceKey) override {
        static_cast<void>(typeId);
        static_cast<void>(sharing);
        static_cast<void>(instanceKey);
        return nullptr;
    }
};

// Everything the run keeps alive while the event loop turns.
struct Run {
    morph::testing::StepExecutor owner;
    morph::reactive::Runtime runtime{owner};
    QQmlEngine engine;
    QQuickWindow window;
    morph::qt_quick::Backend backend{engine, *window.contentItem()};
    morph::ui::Session session{runtime, morph::ui::SessionOptions{}};
    NoModels dispatcher;
    morph::ui::Interpreter interpreter{session, dispatcher, morph::ui::InterpreterOptions{}};
    std::unique_ptr<morph::ui::Screen> screen;
    QQuickItem* flick = nullptr;
    QElapsedTimer clock;
    qint64 lastFrame = 0;
    double scrollFrom = 0;
    double scrollBy = 0;
    bool mounted = false;
    bool scrolling = false;
};

void print(std::string const& line) {
    std::puts(line.c_str());
    static_cast<void>(std::fflush(stdout));
}

}  // namespace

int main(int argc, char* argv[]) {
    QQuickStyle::setStyle(QStringLiteral("Basic"));
    QQuickWindow::setGraphicsApi(QSGRendererInterface::Software);
    QGuiApplication app{argc, argv};
    auto* const run = new Run;  // lives until the process ends: the browser keeps the page after main returns
    run->window.resize(1280, 800);
    run->window.show();

    QTimer::singleShot(0, [run] {
        bench::WideTableSource const source{{.rows = 2'000, .columns = 300, .seed = 7}};
        std::string const document = bench::wideRowsDocument(source);
        run->clock.start();
        auto loaded = morph::ui::loadDocumentText(document, morph::ui::LoadOptions{});
        if (!loaded) {
            print("refused: " + loaded.error().message());
            return;
        }
        auto screen = run->interpreter.mount(std::make_shared<morph::ui::Document const>(std::move(*loaded)), {},
                                             run->backend);
        if (!screen) {
            print("refused: " + screen.error().message());
            return;
        }
        run->screen = std::move(*screen);
        run->owner.runAll();
        run->window.requestUpdate();
    });

    // Each frame: run what the owner was given, then either report the mount or take one step of the scroll.
    QObject::connect(&run->window, &QQuickWindow::frameSwapped, &run->window, [run] {
        run->owner.runAll();
        qint64 const now = run->clock.elapsed();
        if (!run->mounted && run->screen) {
            auto const* const table = morph::qt_quick::detail::findWidget(QStringLiteral("wide"));
            if (table == nullptr || table->item() == nullptr) {
                run->window.requestUpdate();
                return;
            }
            run->mounted = true;
            print("mounted rows=" + std::to_string(table->item()->property("totalRows").toInt()) +
                  " ms=" + std::to_string(now));
            run->flick = table->item()->findChild<QQuickItem*>(QStringLiteral("flick"));
            if (run->flick == nullptr) {
                print("no flick");
                return;
            }
            run->scrollFrom = run->flick->property("contentY").toReal();
            run->scrollBy = run->flick->height();
            run->scrolling = true;
            run->clock.restart();
            run->lastFrame = 0;
            run->window.requestUpdate();
            return;
        }
        if (!run->scrolling) {
            return;
        }
        print("frame ms=" + std::to_string(now - run->lastFrame));
        run->lastFrame = now;
        double const progress = std::min(1.0, static_cast<double>(now) / 1000.0);
        run->flick->setProperty("contentY", run->scrollFrom + (run->scrollBy * progress));
        if (progress >= 1.0) {
            run->scrolling = false;
            print("done");
            return;
        }
        run->window.requestUpdate();
    });

    return QCoreApplication::exec();
}

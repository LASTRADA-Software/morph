# Declarative UI, Part 4 — `morph::qt_quick` Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or
> superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for
> tracking.

**Goal:** Ship `morph::qt_quick` — a compiled, optional Qt Quick frontend whose `Backend` implements
`ui::IViewBackend` with Qt Quick Controls items made from C++, and whose `Frontend` runs any `ui::Application` in
a window, natively and under WebAssembly.

**Architecture:** A private QML module (URI `MorphUi`, static, no plugin) holds one plain component per widget kind
plus a `Window`; each component exposes plain properties and plain signals and nothing else. `detail::Context`
loads every component once; each widget wrapper (`detail::ItemWidget<Interface>`) instantiates its component,
parents it into its container's `content` item, writes properties through `QObject::setProperty`, relays QML
signals through one `QMetaMethod` connection per signal, and maps `LayoutHints` onto `Layout.*` attached properties
in font units. `qt_quick::Frontend::run` owns `QGuiApplication`, a `QtExecutor`, a `QTimer` scheduler, the
`reactive::Runtime`, the engine and the window, mounts the application's view and runs `exec()`.

**Tech Stack:** C++23; Qt 6.5+ `Gui`, `Qml`, `Quick`, `QuickControls2` (Basic style), `QtQuick.Layouts`,
`QtQuick.Dialogs`; `morph::qt::QtExecutor`; Part 1's `morph::reactive`; Part 2's `morph::ui`; Catch2 v3 with
`QtTest`/`QtQuickTest` on the `offscreen` platform and the software scene graph.

**Spec:** `docs/superpowers/specs/2026-10-04-qtquick-frontend-design.md` (spec 3, all of it);
`docs/superpowers/specs/2026-10-04-declarative-ui-tui-design.md` (spec 1) §5, §5b, §7 (backend conformance).

This is **Part 4 of 11** of the declarative-UI program. Read the master plan
`docs/superpowers/plans/2026-10-04-declarative-ui-tui.md` first: it fixes the branch, the commit layout and the
`wip(...)` commit convention every task below follows. Parts 0–3 have landed: `morph::reactive`, `morph::ui`
(view tree, `Mounted`, the frontend seam, `RecordingBackend`, the conformance cases) and `morph::tui` exist, and the
root `CMakeLists.txt` already creates `morph_tui` before the public-header guard. This part ends by squashing its
`wip(qtquick)` commits into one.

## Global Constraints

- Everything in Part 1's "Global Constraints" applies unchanged (SPDX first line — in `.qml` files too, as
  `// SPDX-License-Identifier: Apache-2.0` —, naming, present-tense comments without history or issue numbers,
  complete Doxygen on every public header, `-Weverything -Werror`, clang-tidy clean, sign-off).
- `morph_qt_quick` is a **compiled STATIC** library behind `MORPH_BUILD_QT_QUICK` (default OFF), which requires
  `MORPH_BUILD_QT` and Qt **6.5+** (`QQmlComponent`'s module constructor, `Layout.*StretchFactor`). Configure fails
  with a message naming both when either is missing.
- Public headers live in `include/morph/qt_quick/` (`backend.hpp`, `frontend.hpp`); everything else is private to
  `src/qt_quick/`. Tests reach private headers by include path, never by installing them.
- The `MorphUi` QML files hold no application logic and no binding to anything but their own properties, the
  palette and the window overlay. Keys never enter QML as numbers: a drag key crosses as `i:<int64>` / `s:<text>`;
  a `Select` crosses as label strings plus an index.
- Every string that crosses the backend contract is UTF-8: `QString::fromUtf8` in, `QString::toUtf8` out
  (`detail::toQString` / `detail::toUtf8`).
- A C++ callback reached from a Qt signal, a QML call or a `QTimer` never lets an exception unwind through Qt: it is
  caught and logged with `morph::log::logError`.
- `src/qt_quick/` stays free of `qmlRegisterType`, `Q_IMPORT_QML_PLUGIN` and Qt private headers.
- Commits end with `Signed-off-by: Christian Parpart <christian@parpart.family>`.

## Review Focus

1. **`setText` on a text input that already shows that text** must not move the cursor and must not report an edit
   — a programmatic write is never an echo (Task 4 test "TextInput: setText with the current text keeps the cursor
   and emits nothing").
2. **A Dialog closed by code** (its `open` binding turning false, or its widget hidden) must not report a
   dismissal, while Escape reports exactly one (Task 5 test "Dialog: closed by code reports no dismissal; Escape
   reports one").
3. **A `Select` whose options are replaced** keeps showing the selected key at its new index — a ComboBox resets
   its index when its model changes (Task 4 test "Select: replacing the options re-applies the selected key").
4. **A drag key above 2^53** reaches `onDrop` exactly, and a target whose `accepts` refuses is neither highlighted
   nor dropped on (Task 2 tests "drop delivers an int64 key above 2^53 exactly" and "a refusing target is not
   highlighted and gets no drop").
5. **`quit()` called before the event loop runs** (from inside the factory) still ends `run` with its code instead
   of hanging (Task 9 test "Frontend: a quit from inside the factory still ends run with its code").

---

## File Structure

| File | Responsibility |
|---|---|
| `CMakeLists.txt` (root) | `MORPH_BUILD_QT_QUICK` option and its `MORPH_BUILD_QT` check; Qt Quick lookup and `add_subdirectory(src/qt_quick)` before the public-header guard; the guard's component entry; `tests/qt_quick`; install/export; package version file |
| `src/qt_quick/CMakeLists.txt` | `morph_qt_quick_qml` (the `MorphUi` module) and `morph_qt_quick` / `morph::qt_quick` |
| `src/qt_quick/qml/*.qml` | One component per widget kind, the drag/drop/table-row decorations, `Window` |
| `src/qt_quick/strings.hpp` | `toQString`, `toUtf8` |
| `src/qt_quick/relay.hpp`, `relay.cpp` | `SignalRelay` (QML signal → C++), `DropRelay` (DropTarget → C++) |
| `src/qt_quick/key_text.hpp`, `key_text.cpp` | `encodeKey`, `decodeKey` |
| `src/qt_quick/window.hpp`, `window.cpp` | `createWindow`, `windowContent` |
| `src/qt_quick/context.hpp`, `context.cpp` | `Kind`, `kTypeNames`, `Context` (components, units, names) |
| `src/qt_quick/item_holder.hpp`, `item_holder.cpp` | `ItemHolder`, `ContainerHolder`, `ItemWidget`, `ItemContainer`, `holderOf`, `widgetOf`, `contentOf`, `writeAttached` |
| `src/qt_quick/widgets.hpp` | Label, Spacer, Stack, Slot, Grid wrappers |
| `src/qt_quick/widgets_input.hpp` | Button, TextInput, Checkbox, Select, Menu wrappers |
| `src/qt_quick/widgets_structure.hpp` | Panel, Scroll, Tabs, Dialog, Busy wrappers |
| `src/qt_quick/widgets_table.hpp`, `widgets_table.cpp` | Table wrapper |
| `src/qt_quick/values.hpp`, `values.cpp` | `formatDateTime`, `parseDateTime`, `snapSlider` |
| `src/qt_quick/widgets_value.hpp`, `widgets_value.cpp` | DateTimeInput, Slider, FilePicker wrappers |
| `src/qt_quick/backend.cpp` | `qt_quick::Backend`, `itemOf`, `widgetOf` |
| `src/qt_quick/scheduler.hpp`, `scheduler.cpp` | `QtScheduler` |
| `src/qt_quick/frontend.cpp` | `QtAppContext`, `Frontend`, `frontendOption` |
| `include/morph/qt_quick/backend.hpp`, `frontend.hpp` | Public API |
| `tests/qt_quick/CMakeLists.txt` | `morph_qt_quick_tests`, `morph_qt_quick_frontend_tests` |
| `tests/qt_quick/qt_quick_test_main.cpp`, `frontend_test_main.cpp` | `main`s (with and without a `QGuiApplication`) |
| `tests/qt_quick/scene.hpp` | Test fixture and input helpers |
| `tests/qt_quick/qt_quick_probe.hpp` | `QtQuickProbe : ui::testing::ConformanceProbe` |
| `tests/qt_quick/test_*.cpp` | One file per task |
| `cmake/morphConfig.cmake.in`, `CMakePresets.json` | Component dependency; `*-everything` presets |
| `.github/workflows/ci.yml`, `nightly-slow-checks.yml`, `wasm-ladder.yml` | CI legs |
| `scripts/check_qt_quick_install.sh` | Builds and runs an application against an installed `qt_quick` |
| `docs/spec/qt_quick/frontend.md`, `docs/spec/README.md`, `docs/ARCHITECTURE.md`, `README.md`, `CHANGELOG.md`, `docs/CMakeLists.txt` | Spec, maps, changelog, Doxygen exclusion |

## Build and test commands (used by every task)

```bash
# once; add -DCMAKE_PREFIX_PATH=<Qt 6.5+ prefix> when Qt is not found (Homebrew: $(brew --prefix qt))
cmake -S . -B build/qt -G Ninja -DCMAKE_BUILD_TYPE=Debug -DMORPH_BUILD_EXAMPLES=OFF \
      -DMORPH_BUILD_QT=ON -DMORPH_BUILD_QT_QUICK=ON
cmake --build build/qt --target morph_qt_quick_tests
./build/qt/tests/qt_quick/morph_qt_quick_tests "[qt_quick]"
```

Configuring prints `morph: warnings: ... strict=ON`; if it does not, stop and say so (CONTRIBUTING). The suites set
`QT_QPA_PLATFORM=offscreen` themselves when it is unset, so they run headless from a shell as well as from `ctest`.

---

### Task 1: Packaging, the `MorphUi` module, relays and key text

The library, its QML module and its test suite exist and build; the two QObjects every later task connects through
and the key encoding are tested.

**Files:**
- Modify: `CMakeLists.txt` (root) — option after the `option(MORPH_BUILD_QT ...)` line; Qt Quick block immediately
  before the line `# ── Guard: every public header must belong to some target's FILE_SET ─────────`; the guard's
  `_morph_optional_components` list and its `foreach(_morph_target IN ITEMS ...)` list; `tests/qt_quick` inside the
  `if(MORPH_BUILD_QT)` block's `if(MORPH_BUILD_TESTS)`
- Create: `src/qt_quick/CMakeLists.txt`, `src/qt_quick/qml/Window.qml`
- Create: `src/qt_quick/strings.hpp`, `relay.hpp`, `relay.cpp`, `key_text.hpp`, `key_text.cpp`, `window.hpp`,
  `window.cpp`
- Create: `tests/qt_quick/CMakeLists.txt`, `tests/qt_quick/qt_quick_test_main.cpp`
- Test: `tests/qt_quick/test_relay.cpp`, `tests/qt_quick/test_key_text.cpp`, `tests/qt_quick/test_window.cpp`

**Interfaces:**
- Consumes: `morph::ui::Key` (`include/morph/ui/view.hpp`, Part 2); `morph::log::logError(std::string_view)`
  (`include/morph/core/logger.hpp:237`); `morph::log::ScopedLoggerOverride` (`logger.hpp:279`);
  `morph::testkit::runSession` (`tests/testkit/log_level.hpp:200`).
- Produces (later tasks rely on these exact names, all in `morph::qt_quick::detail`):
  `toQString(std::string_view) -> QString`, `toUtf8(QString const&) -> std::string`;
  `SignalRelay::Handler = std::function<void(QVariant const&)>`,
  `SignalRelay::listen(QObject& source, char const* signature, Handler) -> SignalRelay&`;
  `DropRelay(std::function<bool(QString const&)> accepts, std::function<void(QString const&)> drop)` with
  `Q_INVOKABLE accepts(QString) -> bool` and `Q_INVOKABLE drop(QString)`;
  `encodeKey(ui::Key const&) -> QString`, `decodeKey(QString const&) -> std::optional<ui::Key>`;
  `createWindow(QQmlEngine&) -> std::unique_ptr<QQuickWindow>`, `windowContent(QQuickWindow&) -> QQuickItem&`;
  `kModuleUrl` (`"qrc:/qt/qml/MorphUi/"`).
- CMake: `MORPH_BUILD_QT_QUICK`, targets `morph_qt_quick` / `morph::qt_quick` and `morph_qt_quick_qml`, the cache-free
  variable `MORPH_QT_QUICK_QML_OUTPUT_TARGETS` (set in the parent scope), the CMake function
  `morph_qt_quick_suite(target main sources...)` in `tests/qt_quick/CMakeLists.txt`.

**Why the library is created before the guard, and how `morph_qt` is reached.** The public-header guard (root
`CMakeLists.txt`, "Guard: every public header must belong to some target's FILE_SET") only checks headers of
components whose target exists when it runs. `morph_qt` is created much later, inside the `if(MORPH_BUILD_QT)`
block, after the tests — moving it would make the guard check `include/morph/qt/bridge/*.hpp`, which only
`morph_qt_forms` lists, and fail a `MORPH_BUILD_QT=ON MORPH_BUILD_FORMS_QML=OFF` configure. So `morph_qt_quick` is
created before the guard, as `morph_tui` is, and links `morph_qt` by its plain name: a plain target name in
`target_link_libraries` is resolved when the build system is generated, by which point `morph_qt` exists (the
namespaced `morph::qt` alias is not used for exactly that reason). The guard gets `include/morph/qt_quick/` as a
component prefix, and `morph_qt_quick` sets `VERIFY_INTERFACE_HEADER_SETS` itself, so its public headers are both
listed and compiled standalone. Its tests are added from the `MORPH_BUILD_QT` block because they need Catch2,
which is resolved after the guard.

**Why `MorphUi` is its own target.** Every source of a QML module target is generated (rcc, qmlcachegen, the type
registrar); `apply_warnings()` would hold that code to morph's `-Werror` set. `morph_qt_quick_qml` carries the
module with no warnings applied and `NO_PLUGIN` (it declares no C++ types); `morph_qt_quick` holds morph's own code
under the full warning set and links it PUBLIC, so the module's resource object libraries reach every executable.
Components are loaded by URL from `qrc:/qt/qml/MorphUi/`, so nothing depends on the QML import path or on a type
registration surviving a static link. Each QML file gets a `QT_RESOURCE_ALIAS` of its bare name, so the resource
layout is flat (no `qml/` subdirectory, no extra `qmldir`).

- [ ] **Step 1: Write the failing tests**

Create `tests/qt_quick/test_key_text.cpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0

#include <QString>
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <limits>
#include <morph/ui/view.hpp>
#include <optional>
#include <string>
#include <variant>

#include "key_text.hpp"

namespace {

using morph::qt_quick::detail::decodeKey;
using morph::qt_quick::detail::encodeKey;
namespace ui = morph::ui;

}  // namespace

TEST_CASE("qt_quick key text: integers round-trip exactly, beyond a double's 2^53", "[qt_quick][key]") {
    for (std::int64_t const value : {std::int64_t{0}, std::int64_t{-1}, (std::int64_t{1} << 53) + 1,
                                     std::numeric_limits<std::int64_t>::max(),
                                     std::numeric_limits<std::int64_t>::min()}) {
        QString const text = encodeKey(ui::Key{value});
        CHECK(text == QStringLiteral("i:") + QString::number(value));
        std::optional<ui::Key> const decoded = decodeKey(text);
        REQUIRE(decoded.has_value());
        CHECK(std::get<std::int64_t>(*decoded) == value);
    }
}

TEST_CASE("qt_quick key text: strings round-trip as UTF-8, the empty one included", "[qt_quick][key]") {
    for (std::string const& value : {std::string{}, std::string{"größe"}, std::string{"i:42"}}) {
        std::optional<ui::Key> const decoded = decodeKey(encodeKey(ui::Key{value}));
        REQUIRE(decoded.has_value());
        CHECK(std::get<std::string>(*decoded) == value);
    }
}

TEST_CASE("qt_quick key text: anything encodeKey does not produce decodes to nothing", "[qt_quick][key]") {
    for (char const* const text : {"", "i:", "i:12x", "i:+5", "i: 5", "i:99999999999999999999", "x:1", "1"}) {
        CHECK_FALSE(decodeKey(QString::fromUtf8(text)).has_value());
    }
}
```

Create `tests/qt_quick/test_relay.cpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0

#include <QMetaObject>
#include <QObject>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QString>
#include <QUrl>
#include <QVariant>
#include <catch2/catch_test_macros.hpp>
#include <memory>
#include <morph/core/logger.hpp>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "relay.hpp"

namespace {

using morph::qt_quick::detail::DropRelay;
using morph::qt_quick::detail::SignalRelay;

/// A QML object declaring one signal per parameter type the relay supports.
std::unique_ptr<QObject> makeEmitter(QQmlEngine& engine) {
    QQmlComponent component{&engine};
    component.setData(R"(
        import QtQuick
        Item {
            signal noArgs()
            signal flag(bool value)
            signal count(int value)
            signal amount(real value)
            signal word(string value)
            signal link(url value)
            signal pair(int first, int second)
        }
    )",
                      QUrl{});
    std::unique_ptr<QObject> object{component.create()};
    REQUIRE(object != nullptr);
    return object;
}

}  // namespace

TEST_CASE("qt_quick SignalRelay: each supported QML signal reaches its handler once, with its argument",
          "[qt_quick][relay]") {
    QQmlEngine engine;
    std::unique_ptr<QObject> const emitter = makeEmitter(engine);
    std::vector<QVariant> seen;
    auto const record = [&seen](QVariant const& value) { seen.push_back(value); };
    for (char const* const signature :
         {"noArgs()", "flag(bool)", "count(int)", "amount(double)", "word(QString)", "link(QUrl)"}) {
        static_cast<void>(SignalRelay::listen(*emitter, signature, record));
    }

    QMetaObject::invokeMethod(emitter.get(), "noArgs");
    QMetaObject::invokeMethod(emitter.get(), "flag", Q_ARG(bool, true));
    QMetaObject::invokeMethod(emitter.get(), "count", Q_ARG(int, 42));
    QMetaObject::invokeMethod(emitter.get(), "amount", Q_ARG(double, 2.5));
    QMetaObject::invokeMethod(emitter.get(), "word", Q_ARG(QString, QString::fromUtf8("grüße")));
    QMetaObject::invokeMethod(emitter.get(), "link", Q_ARG(QUrl, QUrl{QStringLiteral("file:///tmp/a.txt")}));

    REQUIRE(seen.size() == 6);
    CHECK_FALSE(seen[0].isValid());
    CHECK(seen[1].toBool());
    CHECK(seen[2].toInt() == 42);
    CHECK(seen[3].toDouble() == 2.5);
    CHECK(seen[4].toString() == QString::fromUtf8("grüße"));
    CHECK(seen[5].toUrl().toLocalFile() == QStringLiteral("/tmp/a.txt"));
}

TEST_CASE("qt_quick SignalRelay: an unknown signal or an unsupported signature is refused", "[qt_quick][relay]") {
    QQmlEngine engine;
    std::unique_ptr<QObject> const emitter = makeEmitter(engine);
    CHECK_THROWS_AS(SignalRelay::listen(*emitter, "missing()", [](QVariant const&) {}), std::logic_error);
    CHECK_THROWS_AS(SignalRelay::listen(*emitter, "pair(int,int)", [](QVariant const&) {}), std::logic_error);
}

TEST_CASE("qt_quick SignalRelay: a throwing handler is logged and does not unwind through Qt", "[qt_quick][relay]") {
    QQmlEngine engine;
    std::unique_ptr<QObject> const emitter = makeEmitter(engine);
    std::vector<std::string> errors;
    morph::log::ScopedLoggerOverride const guard{[&errors](morph::log::LogLevel level, std::string_view message) {
        if (level == morph::log::LogLevel::error) {
            errors.emplace_back(message);
        }
    }};
    static_cast<void>(
        SignalRelay::listen(*emitter, "noArgs()", [](QVariant const&) { throw std::runtime_error{"boom"}; }));
    CHECK_NOTHROW(QMetaObject::invokeMethod(emitter.get(), "noArgs"));
    REQUIRE(errors.size() == 1);
    CHECK(errors.front().find("boom") != std::string::npos);
}

TEST_CASE("qt_quick DropRelay: accepts and drop reach their functions; a throw is logged and refuses",
          "[qt_quick][relay]") {
    std::vector<QString> dropped;
    DropRelay relay{[](QString const& text) { return text == QStringLiteral("i:1"); },
                    [&dropped](QString const& text) { dropped.push_back(text); }};
    bool accepted = false;
    QMetaObject::invokeMethod(&relay, "accepts", Q_RETURN_ARG(bool, accepted), Q_ARG(QString, QStringLiteral("i:1")));
    CHECK(accepted);
    QMetaObject::invokeMethod(&relay, "accepts", Q_RETURN_ARG(bool, accepted), Q_ARG(QString, QStringLiteral("i:2")));
    CHECK_FALSE(accepted);
    QMetaObject::invokeMethod(&relay, "drop", Q_ARG(QString, QStringLiteral("i:1")));
    CHECK(dropped == std::vector{QStringLiteral("i:1")});

    morph::log::ScopedLoggerOverride const quiet{[](morph::log::LogLevel, std::string_view) {}};
    DropRelay throwing{[](QString const&) -> bool { throw std::runtime_error{"accepts"}; },
                       [](QString const&) { throw std::runtime_error{"drop"}; }};
    CHECK_FALSE(throwing.accepts(QStringLiteral("i:1")));
    CHECK_NOTHROW(throwing.drop(QStringLiteral("i:1")));
}
```

Create `tests/qt_quick/test_window.cpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0

#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickWindow>
#include <catch2/catch_test_macros.hpp>
#include <memory>

#include "window.hpp"

TEST_CASE("qt_quick Window: MorphUi.Window loads from the module, hidden, with a content item", "[qt_quick]") {
    QQmlEngine engine;
    std::unique_ptr<QQuickWindow> const window = morph::qt_quick::detail::createWindow(engine);
    REQUIRE(window != nullptr);
    CHECK_FALSE(window->isVisible());
    QQuickItem const& content = morph::qt_quick::detail::windowContent(*window);
    CHECK(content.inherits("QQuickColumnLayout"));
    CHECK(content.window() == window.get());
}
```

Create `tests/qt_quick/qt_quick_test_main.cpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0

#include <QCoreApplication>
#include <QEvent>
#include <QGuiApplication>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QSGRendererInterface>
#include <QString>
#include <QtGlobal>
#include <catch2/catch_session.hpp>
#include <testkit/log_level.hpp>

// The backend suite owns the process's one QGuiApplication for every case. The
// offscreen platform and the software scene graph make it run the same on a
// headless runner as on a desktop: no display, no GPU.
int main(int argc, char* argv[]) {
    if (!qEnvironmentVariableIsSet("QT_QPA_PLATFORM")) {
        qputenv("QT_QPA_PLATFORM", "offscreen");
    }
    QQuickWindow::setGraphicsApi(QSGRendererInterface::Software);
    QGuiApplication app{argc, argv};
    QQuickStyle::setStyle(QStringLiteral("Basic"));
    Catch::Session session;
    int const result = morph::testkit::runSession(session, argc, argv);
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    QCoreApplication::processEvents(QEventLoop::AllEvents);
    return result;
}
```

Create `tests/qt_quick/CMakeLists.txt`:

```cmake
# SPDX-License-Identifier: Apache-2.0
#
# morph::qt_quick's suites (docs/spec/qt_quick/frontend.md, "Testing"). Two
# executables, because a process holds one QGuiApplication at a time:
# morph_qt_quick_tests owns one in main() for the backend, layout, drag,
# scheduler and conformance cases; morph_qt_quick_frontend_tests owns none,
# because qt_quick::Frontend::run creates its own.

find_package(Qt6 6.5 REQUIRED COMPONENTS Test QuickTest)
include(Catch)

# Qt's libraries must be findable at discovery and at run time on Windows;
# DL_PATHS prepends to PATH / LD_LIBRARY_PATH / DYLD_LIBRARY_PATH.
get_target_property(_morph_qt_quick_core_dll Qt6::Core IMPORTED_LOCATION)
cmake_path(GET _morph_qt_quick_core_dll PARENT_PATH _morph_qt_quick_dl_dir)

function(morph_qt_quick_suite target main)
    add_executable(${target} ${main} ${ARGN})
    target_link_libraries(${target} PRIVATE morph::qt_quick Qt6::Test Qt6::QuickTest Catch2::Catch2
                                            morph_test_log_level)
    # The suites drive the private wrappers and helpers directly.
    target_include_directories(${target} PRIVATE "${PROJECT_SOURCE_DIR}/src/qt_quick")
    apply_warnings(${target})
    if(AF_COVERAGE)
        apply_coverage(${target})
    endif()
    if(DEFINED AF_SANITIZER)
        apply_sanitizers(${target} ${AF_SANITIZER})
    endif()
    # Discovery runs the binary at build time; main() selects the offscreen
    # platform itself when QT_QPA_PLATFORM is unset, so it needs no display.
    catch_discover_tests(${target}
        DISCOVERY_MODE POST_BUILD
        DL_PATHS "${_morph_qt_quick_dl_dir}"
        PROPERTIES TIMEOUT 120 LABELS qt_quick ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
endfunction()

morph_qt_quick_suite(morph_qt_quick_tests qt_quick_test_main.cpp
    test_key_text.cpp
    test_relay.cpp
    test_window.cpp
)
```

- [ ] **Step 2: Run the configure to verify it fails**

Run: `cmake -S . -B build/qt -G Ninja -DCMAKE_BUILD_TYPE=Debug -DMORPH_BUILD_EXAMPLES=OFF -DMORPH_BUILD_QT=ON
-DMORPH_BUILD_QT_QUICK=ON`
Expected: configure succeeds but warns `Manually-specified variables were not used by the project:
MORPH_BUILD_QT_QUICK`, and `cmake --build build/qt --target morph_qt_quick_tests` fails with
`unknown target 'morph_qt_quick_tests'`.

- [ ] **Step 3: Implement**

In the root `CMakeLists.txt`, directly after `option(MORPH_BUILD_QT            "Build Qt6 WebSocket backend and tests" OFF)`:

```cmake
option(MORPH_BUILD_QT_QUICK      "Build the morph::qt_quick Qt Quick frontend (needs MORPH_BUILD_QT and Qt 6.5+ Quick)" OFF)
if(MORPH_BUILD_QT_QUICK AND NOT MORPH_BUILD_QT)
    message(FATAL_ERROR
        "MORPH_BUILD_QT_QUICK requires MORPH_BUILD_QT=ON: morph::qt_quick runs its application "
        "on morph::qt's QtExecutor.")
endif()
```

Immediately before `# ── Guard: every public header must belong to some target's FILE_SET ─────────` (after
Part 3's `morph_tui` block):

```cmake
# ── Qt Quick frontend: library target (optional) ────────────────────────────
# Created ahead of the public-header guard below, so the guard checks
# include/morph/qt_quick/ against this target's header set. morph_qt, which this
# target links for QtExecutor, is created further down in the MORPH_BUILD_QT
# block; src/qt_quick/CMakeLists.txt names it by its plain target name, which is
# resolved when the build system is generated. The suites are added from that
# block, once Catch2 is resolved.
if(MORPH_BUILD_QT_QUICK)
    # 6.5: QQmlComponent's module constructor and Layout.*StretchFactor.
    find_package(Qt6 6.5 REQUIRED COMPONENTS Gui Qml Quick QuickControls2)
    if(EMSCRIPTEN)
        # FilePicker reads a browser file through QFileDialog::getOpenFileContent.
        find_package(Qt6 6.5 REQUIRED COMPONENTS Widgets)
    endif()
    add_subdirectory(src/qt_quick)
endif()
```

In the guard, append the entry `"include/morph/qt_quick/:morph_qt_quick"` to `set(_morph_optional_components ...)`
(as its last element) and append ` morph_qt_quick` to the end of the `foreach(_morph_target IN ITEMS ...)` list.

In the `if(MORPH_BUILD_QT)` block, replace

```cmake
    if(MORPH_BUILD_TESTS)
        add_subdirectory(tests/qt)
    endif()
```

with

```cmake
    if(MORPH_BUILD_TESTS)
        add_subdirectory(tests/qt)
        # A browser build runs no ctest binaries.
        if(MORPH_BUILD_QT_QUICK AND NOT EMSCRIPTEN)
            add_subdirectory(tests/qt_quick)
        endif()
    endif()
```

Create `src/qt_quick/CMakeLists.txt`:

```cmake
# SPDX-License-Identifier: Apache-2.0
#
# morph::qt_quick, the Qt Quick frontend (docs/spec/qt_quick/frontend.md). The
# root CMakeLists.txt finds Qt6 Gui/Qml/Quick/QuickControls2 6.5+ before adding
# this directory.

# ── MorphUi: one QML component per widget kind ──────────────────────────────
# Its own target because every source in it is generated (rcc, qmlcachegen,
# the type registrar) and apply_warnings() would hold generated code to morph's
# -Werror set. NO_PLUGIN: the module declares no C++ types. The backend loads
# each component by URL from qrc:/qt/qml/MorphUi/, which the module's resource
# object libraries carry into every executable that links morph_qt_quick.
set(_morph_qt_quick_qml_files
    qml/Window.qml
)
foreach(_morph_qt_quick_qml_file IN LISTS _morph_qt_quick_qml_files)
    cmake_path(GET _morph_qt_quick_qml_file FILENAME _morph_qt_quick_qml_name)
    # A flat resource layout: qrc:/qt/qml/MorphUi/<Name>.qml, no qml/ level.
    set_source_files_properties(${_morph_qt_quick_qml_file} PROPERTIES QT_RESOURCE_ALIAS ${_morph_qt_quick_qml_name})
endforeach()

qt_add_library(morph_qt_quick_qml STATIC)
# The type registrar qt_add_qml_module runs needs moc's metatype output.
set_target_properties(morph_qt_quick_qml PROPERTIES AUTOMOC ON)
qt_add_qml_module(morph_qt_quick_qml
    URI MorphUi
    VERSION 1.0
    RESOURCE_PREFIX /qt/qml
    NO_PLUGIN
    QML_FILES ${_morph_qt_quick_qml_files}
    # The object libraries carrying the compiled resources: an installed
    # morph::qt_quick is unusable without them (root CMakeLists.txt installs them).
    OUTPUT_TARGETS _morph_qt_quick_qml_outputs
)
set(MORPH_QT_QUICK_QML_OUTPUT_TARGETS ${_morph_qt_quick_qml_outputs} PARENT_SCOPE)
target_link_libraries(morph_qt_quick_qml PUBLIC Qt6::Qml Qt6::Quick Qt6::QuickControls2)
target_compile_features(morph_qt_quick_qml PUBLIC cxx_std_23)

# ── morph_qt_quick: morph's own code ────────────────────────────────────────
add_library(morph_qt_quick STATIC
    key_text.cpp
    key_text.hpp
    relay.cpp
    relay.hpp
    strings.hpp
    window.cpp
    window.hpp
)
add_library(morph::qt_quick ALIAS morph_qt_quick)
# AUTOMOC for the relays' Q_OBJECT; the private headers sit on this target's own
# include path so moc's generated include never climbs out of the build tree.
set_target_properties(morph_qt_quick PROPERTIES AUTOMOC ON VERIFY_INTERFACE_HEADER_SETS ON)
target_include_directories(morph_qt_quick PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
# morph_qt by its plain name: the target is created later in the root
# CMakeLists.txt and resolved at generate time.
target_link_libraries(morph_qt_quick
    PUBLIC morph morph_qt morph_qt_quick_qml Qt6::Gui Qt6::Qml Qt6::Quick Qt6::QuickControls2)
if(EMSCRIPTEN)
    target_link_libraries(morph_qt_quick PRIVATE Qt6::Widgets)
endif()
target_compile_features(morph_qt_quick PUBLIC cxx_std_23)
apply_warnings(morph_qt_quick)
if(AF_COVERAGE)
    apply_coverage(morph_qt_quick)
endif()
# Every consumer of this archive (the suites, the example applications) is
# instrumented on the same sanitizer preset; see morph_qt_impl's note in the
# root CMakeLists.txt on why that is required.
if(DEFINED AF_SANITIZER)
    apply_sanitizers(morph_qt_quick ${AF_SANITIZER})
endif()
```

Create `src/qt_quick/qml/Window.qml`:

```qml
// SPDX-License-Identifier: Apache-2.0
import QtQuick
import QtQuick.Controls as C
import QtQuick.Layouts

// The window an application runs in. The root widget goes into `content`, a
// ColumnLayout filling the window, so the root's LayoutHints size it.
C.ApplicationWindow {
    id: root
    readonly property Item content: body
    width: 960
    height: 640
    visible: false
    title: Qt.application.displayName

    ColumnLayout {
        id: body
        anchors.fill: parent
        spacing: 0
    }
}
```

Create `src/qt_quick/strings.hpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0

#pragma once
#include <QByteArray>
#include <QString>
#include <cstddef>
#include <string>
#include <string_view>

/// @file
/// @brief UTF-8 at the backend contract, `QString` inside Qt.

namespace morph::qt_quick::detail {

/// @brief @p text, decoded as UTF-8.
/// @param text UTF-8 bytes.
/// @return The string.
[[nodiscard]] inline QString toQString(std::string_view text) {
    return QString::fromUtf8(text.data(), static_cast<qsizetype>(text.size()));
}

/// @brief @p text, encoded as UTF-8.
/// @param text The string.
/// @return Its UTF-8 bytes.
[[nodiscard]] inline std::string toUtf8(QString const& text) {
    QByteArray const bytes = text.toUtf8();
    return std::string{bytes.constData(), static_cast<std::size_t>(bytes.size())};
}

}  // namespace morph::qt_quick::detail
```

Create `src/qt_quick/relay.hpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0

#pragma once
#include <QObject>
#include <QString>
#include <QUrl>
#include <QVariant>
#include <functional>

/// @file
/// @brief The QObjects through which `MorphUi` items reach C++.

namespace morph::qt_quick::detail {

/// @brief Forwards one signal of a QML object to a C++ handler, with the signal's argument as a `QVariant`.
///
/// A QML signal's C++ signature is known only at run time, so the connection is made between `QMetaMethod`s: the
/// signal is looked up by its normalized signature and paired with the one slot here whose parameter type matches.
/// The relay is a child of the object it listens to and dies with it. A handler that throws is logged, never
/// propagated: an exception must not unwind through Qt's event dispatch.
class SignalRelay final : public QObject {
    Q_OBJECT

public:
    /// @brief The C++ side: the signal's one argument, or an invalid `QVariant` for a signal without one.
    using Handler = std::function<void(QVariant const&)>;

    /// @brief Connects @p signature of @p source to @p handler.
    /// @param source The object that declares the signal; it owns the relay.
    /// @param signature The signal's C++ signature, e.g. `"edited(QString)"`; a QML `real` is `double`.
    /// @param handler Called once per emission.
    /// @return The relay, owned by @p source.
    /// @throws std::logic_error when @p source has no such signal or no slot here takes its parameters.
    static SignalRelay& listen(QObject& source, char const* signature, Handler handler);

public Q_SLOTS:
    /// @brief Receives a signal without parameters.
    void fire();
    /// @brief Receives a `bool` signal.
    /// @param value The argument.
    void fireBool(bool value);
    /// @brief Receives an `int` signal.
    /// @param value The argument.
    void fireInt(int value);
    /// @brief Receives a `real` (`double`) signal.
    /// @param value The argument.
    void fireReal(double value);
    /// @brief Receives a `string` signal.
    /// @param value The argument.
    void fireString(QString const& value);
    /// @brief Receives a `url` signal.
    /// @param value The argument.
    void fireUrl(QUrl const& value);

private:
    SignalRelay(QObject& source, Handler handler);
    void dispatch(QVariant const& value);

    Handler _handler;
};

/// @brief What a `MorphUi` `DropTarget` asks: whether it takes a key, and the key it is given.
///
/// A drop target's `handler` property holds one; QML calls `accepts` when a drag enters and `drop` on release, each
/// with the key as text (`encodeKey`). Throws from either function are logged; a throwing `accepts` refuses.
class DropRelay final : public QObject {
    Q_OBJECT

public:
    /// @param accepts Decides whether a key may be dropped.
    /// @param drop Receives a dropped key.
    DropRelay(std::function<bool(QString const&)> accepts, std::function<void(QString const&)> drop);

    /// @brief Asks the C++ side whether @p keyText may be dropped here.
    /// @param keyText The key as text.
    /// @return True when the drop is taken.
    Q_INVOKABLE bool accepts(QString const& keyText) const;

    /// @brief Delivers a dropped key.
    /// @param keyText The key as text.
    Q_INVOKABLE void drop(QString const& keyText);

private:
    std::function<bool(QString const&)> _accepts;
    std::function<void(QString const&)> _drop;
};

}  // namespace morph::qt_quick::detail
```

Create `src/qt_quick/relay.cpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0

#include "relay.hpp"

#include <QByteArray>
#include <QMetaMethod>
#include <QMetaObject>
#include <QMetaType>
#include <exception>
#include <morph/core/logger.hpp>
#include <stdexcept>
#include <string>
#include <utility>

namespace morph::qt_quick::detail {

namespace {

// The slot that takes @p signal's parameters, or nullptr when none here does.
char const* slotFor(QMetaMethod const& signal) {
    if (signal.parameterCount() == 0) {
        return "fire()";
    }
    if (signal.parameterCount() != 1) {
        return nullptr;
    }
    switch (signal.parameterMetaType(0).id()) {
        case QMetaType::Bool:
            return "fireBool(bool)";
        case QMetaType::Int:
            return "fireInt(int)";
        case QMetaType::Double:
            return "fireReal(double)";
        case QMetaType::QString:
            return "fireString(QString)";
        case QMetaType::QUrl:
            return "fireUrl(QUrl)";
        default:
            return nullptr;
    }
}

void logThrow(char const* what) {
    morph::log::logError(std::string{"[qt_quick] widget callback threw: "} + what);
}

}  // namespace

SignalRelay::SignalRelay(QObject& source, Handler handler) : QObject{&source}, _handler{std::move(handler)} {}

SignalRelay& SignalRelay::listen(QObject& source, char const* signature, Handler handler) {
    QMetaObject const& sourceMeta = *source.metaObject();
    QByteArray const normalized = QMetaObject::normalizedSignature(signature);
    int const signalIndex = sourceMeta.indexOfSignal(normalized.constData());
    if (signalIndex < 0) {
        throw std::logic_error{std::string{"qt_quick: "} + sourceMeta.className() + " has no signal " + signature};
    }
    QMetaMethod const signal = sourceMeta.method(signalIndex);
    char const* const slot = slotFor(signal);
    if (slot == nullptr) {
        throw std::logic_error{std::string{"qt_quick: no relay slot takes the parameters of "} + signature};
    }
    // NOLINTNEXTLINE(cppcoreguidelines-owning-memory): owned by its QObject parent, source.
    auto* const relay = new SignalRelay{source, std::move(handler)};
    QMetaObject const& relayMeta = *relay->metaObject();
    QObject::connect(&source, signal, relay, relayMeta.method(relayMeta.indexOfSlot(slot)));
    return *relay;
}

void SignalRelay::fire() { dispatch(QVariant{}); }
void SignalRelay::fireBool(bool value) { dispatch(QVariant{value}); }
void SignalRelay::fireInt(int value) { dispatch(QVariant{value}); }
void SignalRelay::fireReal(double value) { dispatch(QVariant{value}); }
void SignalRelay::fireString(QString const& value) { dispatch(QVariant{value}); }
void SignalRelay::fireUrl(QUrl const& value) { dispatch(QVariant{value}); }

void SignalRelay::dispatch(QVariant const& value) {
    try {
        if (_handler) {
            _handler(value);
        }
    } catch (std::exception const& error) {
        logThrow(error.what());
    } catch (...) {
        logThrow("a non-standard exception");
    }
}

DropRelay::DropRelay(std::function<bool(QString const&)> accepts, std::function<void(QString const&)> drop)
    : _accepts{std::move(accepts)}, _drop{std::move(drop)} {}

bool DropRelay::accepts(QString const& keyText) const {
    try {
        return _accepts && _accepts(keyText);
    } catch (std::exception const& error) {
        logThrow(error.what());
    } catch (...) {
        logThrow("a non-standard exception");
    }
    return false;
}

void DropRelay::drop(QString const& keyText) {
    try {
        if (_drop) {
            _drop(keyText);
        }
    } catch (std::exception const& error) {
        logThrow(error.what());
    } catch (...) {
        logThrow("a non-standard exception");
    }
}

}  // namespace morph::qt_quick::detail
```

Create `src/qt_quick/key_text.hpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0

#pragma once
#include <QString>
#include <morph/ui/view.hpp>
#include <optional>

/// @file
/// @brief A `ui::Key` as the text that crosses into QML, so no integer key ever becomes a JavaScript double.

namespace morph::qt_quick::detail {

/// @brief `i:<decimal>` for an integer key, `s:<text>` for a string key.
/// @param key The key.
/// @return Its text form.
[[nodiscard]] QString encodeKey(ui::Key const& key);

/// @brief The key @p text encodes.
/// @param text A text form `encodeKey` produced.
/// @return The key, or nullopt when @p text is not such a form (an integer that overflows included).
[[nodiscard]] std::optional<ui::Key> decodeKey(QString const& text);

}  // namespace morph::qt_quick::detail
```

Create `src/qt_quick/key_text.cpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0

#include "key_text.hpp"

#include <charconv>
#include <cstdint>
#include <string>
#include <system_error>
#include <variant>

#include "strings.hpp"

namespace morph::qt_quick::detail {

QString encodeKey(ui::Key const& key) {
    if (auto const* const number = std::get_if<std::int64_t>(&key)) {
        return QStringLiteral("i:") + QString::number(*number);
    }
    return QStringLiteral("s:") + toQString(std::get<std::string>(key));
}

std::optional<ui::Key> decodeKey(QString const& text) {
    if (text.startsWith(QStringLiteral("s:"))) {
        return ui::Key{toUtf8(text.mid(2))};
    }
    if (!text.startsWith(QStringLiteral("i:"))) {
        return std::nullopt;
    }
    std::string const digits = toUtf8(text.mid(2));
    if (digits.empty()) {
        return std::nullopt;
    }
    std::int64_t value = 0;
    char const* const first = digits.data();
    char const* const last = digits.data() + digits.size();  // NOLINT(cppcoreguidelines-pro-bounds-pointer-arithmetic)
    // from_chars takes no '+' and no whitespace, and reports overflow.
    auto const [end, error] = std::from_chars(first, last, value);
    if (error != std::errc{} || end != last) {
        return std::nullopt;
    }
    return ui::Key{value};
}

}  // namespace morph::qt_quick::detail
```

Create `src/qt_quick/window.hpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0

#pragma once
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickWindow>
#include <memory>

/// @file
/// @brief The `MorphUi` window an application runs in.

namespace morph::qt_quick::detail {

/// @brief Where every `MorphUi` component is loaded from.
inline constexpr char const* kModuleUrl = "qrc:/qt/qml/MorphUi/";

/// @brief A hidden `MorphUi` `Window` (an `ApplicationWindow`) made by @p engine; the caller owns it.
/// @param engine The engine that owns the window's QML context; it must outlive the window.
/// @return The window.
/// @throws std::runtime_error with the QML error text when the component does not load.
[[nodiscard]] std::unique_ptr<QQuickWindow> createWindow(QQmlEngine& engine);

/// @brief The item a window's root widget goes into: the window's `content` property.
/// @param window A window from `createWindow`.
/// @return The content item.
/// @throws std::logic_error when the window has no `content`.
[[nodiscard]] QQuickItem& windowContent(QQuickWindow& window);

}  // namespace morph::qt_quick::detail
```

Create `src/qt_quick/window.cpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0

#include "window.hpp"

#include <QObject>
#include <QQmlComponent>
#include <QString>
#include <QUrl>
#include <QVariant>
#include <stdexcept>
#include <string>

namespace morph::qt_quick::detail {

std::unique_ptr<QQuickWindow> createWindow(QQmlEngine& engine) {
    QQmlComponent component{&engine, QUrl{QString::fromLatin1(kModuleUrl) + QStringLiteral("Window.qml")}};
    if (!component.isReady()) {
        throw std::runtime_error{"qt_quick: cannot load MorphUi.Window: " + component.errorString().toStdString()};
    }
    std::unique_ptr<QObject> object{component.create()};
    if (object == nullptr) {
        throw std::runtime_error{"qt_quick: cannot create MorphUi.Window: " + component.errorString().toStdString()};
    }
    auto* const window = qobject_cast<QQuickWindow*>(object.get());
    if (window == nullptr) {
        throw std::runtime_error{"qt_quick: MorphUi.Window is not a QQuickWindow"};
    }
    static_cast<void>(object.release());
    return std::unique_ptr<QQuickWindow>{window};
}

QQuickItem& windowContent(QQuickWindow& window) {
    auto* const content = window.property("content").value<QQuickItem*>();
    if (content == nullptr) {
        throw std::logic_error{"qt_quick: MorphUi.Window has no content item"};
    }
    return *content;
}

}  // namespace morph::qt_quick::detail
```

- [ ] **Step 4: Run the tests to verify they pass**

```bash
cmake -S . -B build/qt -G Ninja -DCMAKE_BUILD_TYPE=Debug -DMORPH_BUILD_EXAMPLES=OFF -DMORPH_BUILD_QT=ON \
      -DMORPH_BUILD_QT_QUICK=ON
cmake --build build/qt --target morph_qt_quick_tests && ./build/qt/tests/qt_quick/morph_qt_quick_tests "[qt_quick]"
cmake -S . -B build/qt-refused -G Ninja -DMORPH_BUILD_EXAMPLES=OFF -DMORPH_BUILD_QT_QUICK=ON   # must fail
```

Expected: PASS for the suite; the last configure fails with `MORPH_BUILD_QT_QUICK requires MORPH_BUILD_QT=ON`
(remove `build/qt-refused` afterwards).

Mutation checks (one at a time, then restore):
- In `slotFor`, map `QMetaType::Int` to `"fireReal(double)"` → expected FAIL in "each supported QML signal reaches
  its handler once" (the connect is refused at run time and no `count` arrives).
- In `decodeKey`, drop the `end != last` test → expected FAIL in "anything encodeKey does not produce decodes to
  nothing" (`i:12x`).
- Temporarily create `include/morph/qt_quick/stray.hpp` (`#pragma once`) and re-configure → expected configure
  FAIL from the public-header guard naming it. Delete the file.

- [ ] **Step 5: Commit**

```bash
git add CMakeLists.txt src/qt_quick tests/qt_quick
git commit -m "wip(qtquick): MorphUi module, relays and key text

Signed-off-by: Christian Parpart <christian@parpart.family>"
```

---
### Task 2: The common widget surface — `Context`, `ItemHolder`, Label, Spacer, Stack, Slot, drag and drop

Everything `ui::Widget` and `ui::ContainerWidget` declare, implemented once for every kind: instantiation and
naming, parenting into a container's `content`, visibility, enablement, layout hints, a drag key, a drop handler,
child order, deletion. The four simplest kinds come with it so it can be tested.

**Files:**
- Create: `src/qt_quick/context.hpp`, `context.cpp`, `item_holder.hpp`, `item_holder.cpp`, `widgets.hpp`
- Create: `src/qt_quick/qml/Label.qml`, `Spacer.qml`, `ColumnStack.qml`, `RowStack.qml`, `Slot.qml`,
  `DragSource.qml`, `DropTarget.qml`
- Modify: `src/qt_quick/CMakeLists.txt` — the seven QML files in `_morph_qt_quick_qml_files` (kept alphabetical),
  and `context.cpp context.hpp item_holder.cpp item_holder.hpp widgets.hpp` in `add_library(morph_qt_quick ...)`
- Create: `tests/qt_quick/scene.hpp`
- Modify: `tests/qt_quick/CMakeLists.txt` — `test_common_surface.cpp` in `morph_qt_quick_tests`
- Test: `tests/qt_quick/test_common_surface.cpp`

**Interfaces:**
- Consumes: Task 1's relays, key text, strings, window helpers; `ui::Widget`, `ui::ContainerWidget`,
  `ui::TextWidget`, `ui::SpacerWidget`, `ui::StackWidget`, `ui::SlotWidget`, `ui::LayoutHints`, `ui::Sizing`,
  `ui::Axis`, `ui::TextRole`, `ui::Key` (Part 2, `ui/backend.hpp`, `ui/view.hpp`).
- Produces (`morph::qt_quick::detail`):
  - `enum class Kind : std::uint8_t` (`Label, Spacer, ColumnStack, RowStack, Slot, DragSource, DropTarget`, then
    each later task inserts its kinds before `Count`), `kTypeNames` (same order).
  - `Context(QQmlEngine&, QQuickItem& root)`: `create(Kind, QVariantMap const& = {}) -> QQuickItem*`,
    `decorate(Kind, QQuickItem& host, QVariantMap const& = {}) -> QQuickItem*`, `root()`, `nextName()`,
    `unitWidth()`, `unitHeight()`.
  - `ItemHolder`: `item()`, `context()`, `attach(ui::Widget&, ui::ContainerWidget*)`, `applyVisible`,
    `visibleFlag()` (the widget's own flag, what `ConformanceProbe::visibleOf` reports), `applyEnabled`,
    `applyHints`, `setDefaultWidth(std::optional<ui::Sizing>)`, `applyDragKey`, `applyDropHandler`; protected
    `rememberVisible(bool)`, `relay(char const*, SignalRelay::Handler)`, `write(char const*, QVariant const&)`,
    `invoke(char const*, QVariant const&)`.
  - `ContainerHolder : ItemHolder`: `content()`, `childWidgetItems()`, `moveChildItem(QQuickItem&, std::size_t)`.
  - `template <class Interface, class Holder = ItemHolder> class ItemWidget`; `template <class Interface> class
    ItemContainer`.
  - `holderOf(ui::Widget&)`, `holderOf(ui::Widget const&)`, `holderOf(QQuickItem const&)`,
    `widgetOf(QQuickItem const&) -> ui::Widget*`, `contentOf(ui::ContainerWidget&) -> QQuickItem&`,
    `writeAttached(QQuickItem&, char const*, QVariant const&)`.
  - Wrappers `TextLabel(Context&, ui::ContainerWidget*)`, `SpacerItem(Context&, ui::ContainerWidget*)`,
    `Stack(Context&, ui::ContainerWidget*, ui::Axis)`, `SlotBox(Context&, ui::ContainerWidget*)`.
  - QML contract: every container component has `readonly property Item content`; `Label.role` is
    `ui::TextRole` as an int; `DragSource.keyText`; `DropTarget.handler`, `DropTarget.highlighted`; decorations are
    named `dragSource` / `dropTarget`.
  - Test helpers in `morph::qt_quick::testing` (`tests/qt_quick/scene.hpp`): `Scene{engine, window, context}` with
    `show()` and `settle()`, `itemOf(ui::Widget const&) -> QQuickItem&`, `contentOf(QQuickItem const&)`,
    `widgetChildren(QQuickItem const&)`, `centre(QQuickItem const&)`, `unitWidth()`, `unitHeight()`,
    `dragAcross(window, from, destination, whileOver)`, `typeText(window, text)`, `childrenOfType(item, className)`,
    `findItem(item, objectName)`.

**Design notes.**
- A widget is created unparented and completed, then `attach` sets its `objectName` (`w<id>`, 1-based in creation
  order, per backend), stores `morphWidget` / `morphHolder` (a `void*` each) as dynamic properties so an item maps
  back to its wrapper, and only then parents it (visual and `QObject` parent) into the container's `content` or the
  root. Parenting last means anything that reacts to a new child — a table row's column widths, Task 6 — already
  finds the wrapper.
- Every component whose natural root would be a Layout is wrapped in a plain `Item` with the layout as its
  `content`: a decoration parented into a Layout would be laid out as one more child.
- The wrapper holds its item in a `QPointer` and deletes it in its destructor. Deleting the item deletes its
  `SignalRelay`s and decorations with it. The `QPointer` makes a wrapper whose item Qt already deleted (with a
  parent) safe to destroy.
- A drag is an internal Qt Quick drag: `DragSource` (a `DragHandler` with no target plus a translucent copy on the
  window overlay whose `Drag` attached property is active while the handler is) and `DropTarget` (a `DropArea` that
  asks `DropRelay::accepts` on enter, highlights while accepting, and calls `DropRelay::drop` on release). The key
  crosses as `keyText` on the drag's `source` and in `Drag.mimeData`.

- [ ] **Step 1: Write the failing test**

Create `tests/qt_quick/scene.hpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0

#pragma once
#include <QChar>
#include <QCoreApplication>
#include <QEvent>
#include <QFontMetricsF>
#include <QGuiApplication>
#include <QKeyEvent>
#include <QList>
#include <QPoint>
#include <QPointF>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickWindow>
#include <QString>
#include <QTest>
#include <QtQuickTest/quicktest.h>
#include <catch2/catch_test_macros.hpp>
#include <memory>
#include <morph/ui/backend.hpp>
#include <utility>
#include <vector>

#include "context.hpp"
#include "item_holder.hpp"
#include "window.hpp"

namespace morph::qt_quick::testing {

/// One `MorphUi` window and a backend context over its content, on the suite's `QGuiApplication`.
struct Scene {
    QQmlEngine engine;
    std::unique_ptr<QQuickWindow> window = detail::createWindow(engine);
    detail::Context context{engine, detail::windowContent(*window)};

    Scene() { window->resize(640, 480); }

    /// Shows and activates the window, then waits until it is laid out.
    void show() {
        window->show();
        REQUIRE(QTest::qWaitForWindowExposed(window.get()));
        window->requestActivate();
        REQUIRE(QTest::qWaitForWindowActive(window.get()));
        settle();
    }

    /// Runs posted events, then the pending polish (layout) pass.
    void settle() const {
        QCoreApplication::processEvents(QEventLoop::AllEvents);
        if (window->isExposed()) {
            CHECK(QQuickTest::qWaitForPolish(window.get()));
        }
        QCoreApplication::processEvents(QEventLoop::AllEvents);
    }
};

/// The item behind a wrapper this backend made.
inline QQuickItem& itemOf(ui::Widget const& widget) {
    auto const* const holder = detail::holderOf(widget);
    REQUIRE(holder != nullptr);
    REQUIRE(holder->item() != nullptr);
    return *holder->item();
}

/// A container item's `content`.
inline QQuickItem& contentOf(QQuickItem const& container) {
    auto* const content = container.property("content").value<QQuickItem*>();
    REQUIRE(content != nullptr);
    return *content;
}

/// A container item's widget children, in layout order.
inline std::vector<QQuickItem*> widgetChildren(QQuickItem const& container) {
    std::vector<QQuickItem*> children;
    for (QQuickItem* const child : contentOf(container).childItems()) {
        if (detail::widgetOf(*child) != nullptr) {
            children.push_back(child);
        }
    }
    return children;
}

/// Every item in @p root's visual subtree whose C++ class is (or derives from) @p className, depth first in
/// stacking order. The visual tree, not the QObject tree: a Repeater's delegates and a Control's parts are visual
/// children of what they show in, whatever their QObject parent.
inline std::vector<QQuickItem*> childrenOfType(QQuickItem const& root, char const* className) {
    std::vector<QQuickItem*> found;
    std::vector<QQuickItem const*> pending{&root};
    while (!pending.empty()) {
        QQuickItem const* const current = pending.back();
        pending.pop_back();
        QList<QQuickItem*> const children = current->childItems();
        for (auto child = children.rbegin(); child != children.rend(); ++child) {
            pending.push_back(*child);
        }
        if (current != &root && current->inherits(className)) {
            found.push_back(const_cast<QQuickItem*>(current));  // NOLINT(cppcoreguidelines-pro-type-const-cast)
        }
    }
    return found;
}

/// The item named @p objectName in @p root's visual subtree, or null.
inline QQuickItem* findItem(QQuickItem const& root, char const* objectName) {
    for (QQuickItem* const item : childrenOfType(root, "QQuickItem")) {
        if (item->objectName() == QString::fromLatin1(objectName)) {
            return item;
        }
    }
    return nullptr;
}

/// @p item's centre, in window coordinates.
inline QPoint centre(QQuickItem const& item) {
    return item.mapToScene(QPointF{item.width() / 2, item.height() / 2}).toPoint();
}

/// One layout unit across: the application font's average character width.
inline double unitWidth() { return QFontMetricsF{QGuiApplication::font()}.averageCharWidth(); }

/// One layout unit down: the application font's line height.
inline double unitHeight() { return QFontMetricsF{QGuiApplication::font()}.lineSpacing(); }

/// Presses at @p from, moves to @p destination in steps well past the drag threshold, calls @p whileOver with the
/// button still down over @p destination, then releases there.
template <class F>
void dragAcross(QQuickWindow& window, QPoint from, QPoint destination, F&& whileOver) {
    QTest::mousePress(&window, Qt::LeftButton, Qt::NoModifier, from);
    constexpr int kSteps = 12;
    for (int step = 1; step <= kSteps; ++step) {
        QTest::mouseMove(&window, from + ((destination - from) * step) / kSteps);
    }
    QTest::mouseMove(&window, destination);
    std::forward<F>(whileOver)();
    QTest::mouseRelease(&window, Qt::LeftButton, Qt::NoModifier, destination);
}

/// Types @p text into the window's focused item, one key event per character, the way a keyboard does.
inline void typeText(QQuickWindow& window, QString const& text) {
    for (QChar const character : text) {
        QKeyEvent press{QEvent::KeyPress, 0, Qt::NoModifier, QString{character}};
        QKeyEvent release{QEvent::KeyRelease, 0, Qt::NoModifier, QString{character}};
        QCoreApplication::sendEvent(&window, &press);
        QCoreApplication::sendEvent(&window, &release);
    }
}

}  // namespace morph::qt_quick::testing
```

Create `tests/qt_quick/test_common_surface.cpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0

#include <QColor>
#include <QFont>
#include <QObject>
#include <QPointer>
#include <QQuickItem>
#include <QString>
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <functional>
#include <memory>
#include <morph/ui/view.hpp>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "scene.hpp"
#include "widgets.hpp"

namespace {

using morph::qt_quick::detail::SlotBox;
using morph::qt_quick::detail::SpacerItem;
using morph::qt_quick::detail::Stack;
using morph::qt_quick::detail::TextLabel;
using morph::qt_quick::testing::centre;
using morph::qt_quick::testing::contentOf;
using morph::qt_quick::testing::dragAcross;
using morph::qt_quick::testing::itemOf;
using morph::qt_quick::testing::Scene;
using morph::qt_quick::testing::unitHeight;
using morph::qt_quick::testing::unitWidth;
using morph::qt_quick::testing::widgetChildren;
namespace ui = morph::ui;

constexpr std::int64_t kBeyondDouble = (std::int64_t{1} << 53) + 1;

QObject* decoration(QQuickItem const& item, char const* name) {
    return item.findChild<QObject*>(QString::fromLatin1(name), Qt::FindDirectChildrenOnly);
}

/// A source label at the left and a target column filling the rest, with @p accepts on the target.
struct DragScene {
    Scene scene;
    Stack row{scene.context, nullptr, ui::Axis::Horizontal};
    TextLabel source{scene.context, &row};
    Stack target{scene.context, &row, ui::Axis::Vertical};
    std::vector<ui::Key> dropped;

    explicit DragScene(ui::Key key, std::function<bool(ui::Key const&)> accepts) {
        row.setLayout({.width = ui::Sizing::stretch(), .height = ui::Sizing::stretch()});
        source.setText("card");
        source.setLayout({.width = ui::Sizing::fixed(10), .height = ui::Sizing::fixed(2)});
        target.setLayout({.width = ui::Sizing::stretch(), .height = ui::Sizing::stretch()});
        source.setDragKey(std::move(key));
        target.setDropHandler(std::move(accepts), [this](ui::Key value) { dropped.push_back(std::move(value)); });
        scene.show();
    }
};

}  // namespace

TEST_CASE("qt_quick widgets: named w<id> in creation order and parented into the container's content",
          "[qt_quick]") {
    Scene scene;
    Stack column{scene.context, nullptr, ui::Axis::Vertical};
    TextLabel label{scene.context, &column};
    QQuickItem const& columnItem = itemOf(column);
    QQuickItem const& labelItem = itemOf(label);
    CHECK(columnItem.objectName() == QStringLiteral("w1"));
    CHECK(labelItem.objectName() == QStringLiteral("w2"));
    CHECK(columnItem.parentItem() == &morph::qt_quick::detail::windowContent(*scene.window));
    CHECK(labelItem.parentItem() == &contentOf(columnItem));
    CHECK(labelItem.parent() == &contentOf(columnItem));
    CHECK(scene.window->contentItem()->findChild<QQuickItem*>(QStringLiteral("w2")) == &labelItem);
    CHECK(morph::qt_quick::detail::widgetOf(labelItem) == &label);
}

TEST_CASE("qt_quick Label: text arrives as UTF-8 and each role changes the look", "[qt_quick]") {
    Scene scene;
    TextLabel label{scene.context, nullptr};
    QQuickItem const& item = itemOf(label);
    label.setText("Größe ✓");
    CHECK(item.property("text").toString() == QString::fromUtf8("Größe ✓"));
    QColor const normal = item.property("color").value<QColor>();

    label.setRole(ui::TextRole::Heading);
    CHECK(item.property("role").toInt() == 2);
    CHECK(item.property("font").value<QFont>().bold());
    label.setRole(ui::TextRole::Error);
    CHECK(item.property("color").value<QColor>() == QColor{QStringLiteral("#b3261e")});
    label.setRole(ui::TextRole::Success);
    CHECK(item.property("color").value<QColor>() == QColor{QStringLiteral("#2e7d32")});
    label.setRole(ui::TextRole::Muted);
    CHECK(item.property("color").value<QColor>() != normal);
    CHECK_FALSE(item.property("font").value<QFont>().bold());
}

TEST_CASE("qt_quick widgets: visible and enabled reach the item", "[qt_quick]") {
    Scene scene;
    TextLabel label{scene.context, nullptr};
    QQuickItem const& item = itemOf(label);
    label.setVisible(false);
    CHECK_FALSE(item.isVisible());
    label.setVisible(true);
    CHECK(item.isVisible());
    label.setEnabled(false);
    CHECK_FALSE(item.isEnabled());
}

TEST_CASE("qt_quick widgets: the visibility flag is the widget's own, not its ancestors'", "[qt_quick]") {
    Scene scene;
    Stack column{scene.context, nullptr, ui::Axis::Vertical};
    TextLabel label{scene.context, &column};
    auto const* const holder = morph::qt_quick::detail::holderOf(label);
    REQUIRE(holder != nullptr);
    column.setVisible(false);
    CHECK_FALSE(itemOf(label).isVisible());
    CHECK(holder->visibleFlag());
    label.setVisible(false);
    CHECK_FALSE(holder->visibleFlag());
    column.setVisible(true);
    CHECK_FALSE(holder->visibleFlag());
}

TEST_CASE("qt_quick widgets: destroying a wrapper deletes its item", "[qt_quick]") {
    Scene scene;
    Stack column{scene.context, nullptr, ui::Axis::Vertical};
    auto label = std::make_unique<TextLabel>(scene.context, &column);
    QPointer<QQuickItem> const watch{&itemOf(*label)};
    label.reset();
    CHECK(watch.isNull());
    CHECK(contentOf(itemOf(column)).childItems().isEmpty());
}

TEST_CASE("qt_quick widgets: a child wrapper outliving its parent's item does not delete it twice", "[qt_quick]") {
    Scene scene;
    auto column = std::make_unique<Stack>(scene.context, nullptr, ui::Axis::Vertical);
    auto label = std::make_unique<TextLabel>(scene.context, column.get());
    QPointer<QQuickItem> const watch{&itemOf(*label)};
    column.reset();  // deletes the column's item and, as its QObject child, the label's
    CHECK(watch.isNull());
    label.reset();  // must not touch the deleted item; ASan observes
}

TEST_CASE("qt_quick containers: moveChild reorders the children and the layout follows", "[qt_quick]") {
    Scene scene;
    Stack column{scene.context, nullptr, ui::Axis::Vertical};
    TextLabel first{scene.context, &column};
    TextLabel second{scene.context, &column};
    TextLabel third{scene.context, &column};
    first.setText("first");
    second.setText("second");
    third.setText("third");

    column.moveChild(third, 0);
    CHECK(widgetChildren(itemOf(column)) == std::vector{&itemOf(third), &itemOf(first), &itemOf(second)});
    column.moveChild(first, 7);
    CHECK(widgetChildren(itemOf(column)) == std::vector{&itemOf(third), &itemOf(second), &itemOf(first)});

    scene.show();
    CHECK(itemOf(third).y() < itemOf(second).y());
    CHECK(itemOf(second).y() < itemOf(first).y());
}

TEST_CASE("qt_quick Stack: the gap is in units along the stack's axis", "[qt_quick]") {
    Scene scene;
    Stack column{scene.context, nullptr, ui::Axis::Vertical};
    Stack row{scene.context, nullptr, ui::Axis::Horizontal};
    column.setGap(2);
    row.setGap(3);
    CHECK(itemOf(column).property("spacing").toDouble() == Catch::Approx(2 * unitHeight()));
    CHECK(itemOf(row).property("spacing").toDouble() == Catch::Approx(3 * unitWidth()));
}

TEST_CASE("qt_quick Slot and Spacer: a slot holds its child; a spacer is an empty item", "[qt_quick]") {
    Scene scene;
    SlotBox slot{scene.context, nullptr};
    TextLabel label{scene.context, &slot};
    SpacerItem spacer{scene.context, nullptr};
    CHECK(itemOf(label).parentItem() == &contentOf(itemOf(slot)));
    CHECK(itemOf(spacer).implicitWidth() == 0.0);
    CHECK(itemOf(spacer).implicitHeight() == 0.0);
}

TEST_CASE("qt_quick drag and drop: drop delivers an int64 key above 2^53 exactly", "[qt_quick][drag]") {
    DragScene drag{ui::Key{kBeyondDouble}, [](ui::Key const&) { return true; }};
    QObject const* const target = decoration(itemOf(drag.target), "dropTarget");
    REQUIRE(target != nullptr);
    REQUIRE(decoration(itemOf(drag.source), "dragSource") != nullptr);

    dragAcross(*drag.scene.window, centre(itemOf(drag.source)), centre(itemOf(drag.target)),
               [&] { CHECK(target->property("highlighted").toBool()); });

    REQUIRE(drag.dropped.size() == 1);
    CHECK(std::get<std::int64_t>(drag.dropped.front()) == kBeyondDouble);
    CHECK_FALSE(target->property("highlighted").toBool());
}

TEST_CASE("qt_quick drag and drop: a string key crosses intact", "[qt_quick][drag]") {
    DragScene drag{ui::Key{std::string{"größe"}}, [](ui::Key const&) { return true; }};
    dragAcross(*drag.scene.window, centre(itemOf(drag.source)), centre(itemOf(drag.target)), [] {});
    REQUIRE(drag.dropped.size() == 1);
    CHECK(std::get<std::string>(drag.dropped.front()) == "größe");
}

TEST_CASE("qt_quick drag and drop: a refusing target is not highlighted and gets no drop", "[qt_quick][drag]") {
    std::vector<ui::Key> asked;
    DragScene drag{ui::Key{std::int64_t{7}}, [&asked](ui::Key const& key) {
                       asked.push_back(key);
                       return false;
                   }};
    QObject const* const target = decoration(itemOf(drag.target), "dropTarget");
    REQUIRE(target != nullptr);
    dragAcross(*drag.scene.window, centre(itemOf(drag.source)), centre(itemOf(drag.target)),
               [&] { CHECK_FALSE(target->property("highlighted").toBool()); });
    CHECK_FALSE(asked.empty());
    CHECK(drag.dropped.empty());
}

TEST_CASE("qt_quick drag and drop: clearing the key or the handler removes the decoration", "[qt_quick][drag]") {
    Scene scene;
    TextLabel label{scene.context, nullptr};
    label.setDragKey(ui::Key{std::int64_t{1}});
    label.setDropHandler({}, [](ui::Key) {});
    CHECK(decoration(itemOf(label), "dragSource") != nullptr);
    CHECK(decoration(itemOf(label), "dropTarget") != nullptr);
    label.setDragKey(std::nullopt);
    label.setDropHandler({}, {});
    CHECK(decoration(itemOf(label), "dragSource") == nullptr);
    CHECK(decoration(itemOf(label), "dropTarget") == nullptr);
}
```

- [ ] **Step 2: Run it to verify it fails**

Add `test_common_surface.cpp` to `morph_qt_quick_suite(morph_qt_quick_tests ...)` and run
`cmake --build build/qt --target morph_qt_quick_tests`.
Expected: FAIL — `'widgets.hpp' file not found`.

- [ ] **Step 3: Implement**

Create `src/qt_quick/context.hpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0

#pragma once
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQuickItem>
#include <QString>
#include <QVariantMap>
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>

/// @file
/// @brief What every widget wrapper shares: the engine, one loaded `MorphUi` component per kind, the root item,
///        the layout units and the name counter.

namespace morph::qt_quick::detail {

/// @brief Every `MorphUi` component the backend instantiates.
enum class Kind : std::uint8_t {
    Label,
    Spacer,
    ColumnStack,
    RowStack,
    Slot,
    DragSource,
    DropTarget,
    Count,  ///< Not a kind: how many there are.
};

/// @brief The `MorphUi` file (without `.qml`) of each kind, indexed by `Kind`.
inline constexpr std::array<char const*, static_cast<std::size_t>(Kind::Count)> kTypeNames{
    "Label", "Spacer", "ColumnStack", "RowStack", "Slot", "DragSource", "DropTarget",
};

/// @brief The engine, the components and the root a backend's wrappers are made from.
class Context {
public:
    /// @param engine Makes every item; must outlive this context and every item made from it.
    /// @param root Where a widget without a parent container goes.
    /// @throws std::runtime_error naming the first component that does not load.
    Context(QQmlEngine& engine, QQuickItem& root);
    ~Context();
    Context(Context const&) = delete;
    Context& operator=(Context const&) = delete;
    Context(Context&&) = delete;
    Context& operator=(Context&&) = delete;

    /// @brief A completed, unparented item of @p kind; the caller owns it.
    /// @param kind The component.
    /// @param initial Properties set before the item's bindings first run.
    /// @return The item.
    /// @throws std::runtime_error with the QML error when creation fails; std::logic_error when the component's
    ///         root is not an Item.
    [[nodiscard]] QQuickItem* create(Kind kind, QVariantMap const& initial = {});

    /// @brief A decoration of @p kind inside @p host, parented before its bindings run; @p host owns it.
    /// @param kind The component.
    /// @param host The item it decorates.
    /// @param initial Properties set before the item's bindings first run.
    /// @return The decoration.
    /// @throws as `create`.
    QQuickItem* decorate(Kind kind, QQuickItem& host, QVariantMap const& initial = {});

    /// @brief Where a widget without a parent container goes.
    /// @return The root item.
    [[nodiscard]] QQuickItem& root() const noexcept { return *_root; }

    /// @brief The next widget's `objectName`: `w1`, `w2`, … in creation order.
    /// @return The name.
    [[nodiscard]] QString nextName();

    /// @brief One layout unit across: the application font's average character width, in pixels.
    /// @return The width.
    [[nodiscard]] double unitWidth() const noexcept { return _unitWidth; }

    /// @brief One layout unit down: the application font's line height, in pixels.
    /// @return The height.
    [[nodiscard]] double unitHeight() const noexcept { return _unitHeight; }

private:
    QQuickItem* instantiate(Kind kind, QQuickItem* host, QVariantMap const& initial);

    QQmlEngine* _engine;
    QQuickItem* _root;
    double _unitWidth;
    double _unitHeight;
    std::array<std::unique_ptr<QQmlComponent>, static_cast<std::size_t>(Kind::Count)> _components;
    std::uint64_t _nextId = 1;
};

}  // namespace morph::qt_quick::detail
```

Create `src/qt_quick/context.cpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0

#include "context.hpp"

#include <QFontMetricsF>
#include <QGuiApplication>
#include <QObject>
#include <QUrl>
#include <stdexcept>
#include <string>

#include "window.hpp"

namespace morph::qt_quick::detail {

Context::Context(QQmlEngine& engine, QQuickItem& root)
    : _engine{&engine},
      _root{&root},
      _unitWidth{QFontMetricsF{QGuiApplication::font()}.averageCharWidth()},
      _unitHeight{QFontMetricsF{QGuiApplication::font()}.lineSpacing()} {
    for (std::size_t index = 0; index < kTypeNames.size(); ++index) {
        QUrl const url{QString::fromLatin1(kModuleUrl) + QString::fromLatin1(kTypeNames.at(index)) +
                       QStringLiteral(".qml")};
        auto component = std::make_unique<QQmlComponent>(&engine, url);
        if (!component->isReady()) {
            throw std::runtime_error{std::string{"qt_quick: cannot load MorphUi."} + kTypeNames.at(index) + ": " +
                                     component->errorString().toStdString()};
        }
        _components.at(index) = std::move(component);
    }
}

Context::~Context() = default;

QQuickItem* Context::create(Kind kind, QVariantMap const& initial) { return instantiate(kind, nullptr, initial); }

QQuickItem* Context::decorate(Kind kind, QQuickItem& host, QVariantMap const& initial) {
    return instantiate(kind, &host, initial);
}

QString Context::nextName() { return QStringLiteral("w") + QString::number(_nextId++); }

QQuickItem* Context::instantiate(Kind kind, QQuickItem* host, QVariantMap const& initial) {
    QQmlComponent& component = *_components.at(static_cast<std::size_t>(kind));
    QObject* const object = component.beginCreate(_engine->rootContext());
    if (object == nullptr) {
        throw std::runtime_error{std::string{"qt_quick: cannot create MorphUi."} +
                                 kTypeNames.at(static_cast<std::size_t>(kind)) + ": " +
                                 component.errorString().toStdString()};
    }
    if (!initial.isEmpty()) {
        component.setInitialProperties(object, initial);
    }
    auto* const item = qobject_cast<QQuickItem*>(object);
    if (item != nullptr && host != nullptr) {
        item->setParentItem(host);
        item->setParent(host);
    }
    // Always completed, even when about to be refused: a component left mid-creation refuses the next beginCreate.
    component.completeCreate();
    if (item == nullptr) {
        delete object;  // NOLINT(cppcoreguidelines-owning-memory): beginCreate hands the caller ownership
        throw std::logic_error{std::string{"qt_quick: MorphUi."} + kTypeNames.at(static_cast<std::size_t>(kind)) +
                               " is not an Item"};
    }
    return item;
}

}  // namespace morph::qt_quick::detail
```

Create `src/qt_quick/item_holder.hpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0

#pragma once
#include <QPointer>
#include <QQuickItem>
#include <QVariant>
#include <QVariantMap>
#include <cstddef>
#include <functional>
#include <memory>
#include <morph/ui/backend.hpp>
#include <morph/ui/view.hpp>
#include <optional>
#include <stdexcept>
#include <utility>
#include <vector>

#include "context.hpp"
#include "relay.hpp"

/// @file
/// @brief The part of every widget wrapper that is the same for every kind.

namespace morph::qt_quick::detail {

/// @brief The toolkit half of a wrapper: its item, what `ui::Widget` declares for every kind, its decorations.
class ItemHolder {
public:
    /// @param context The backend's context; must outlive this holder.
    /// @param item The item, owned from now on.
    ItemHolder(Context& context, QQuickItem* item);
    /// @brief Deletes the item, and with it its relays and decorations.
    virtual ~ItemHolder();
    ItemHolder(ItemHolder const&) = delete;
    ItemHolder& operator=(ItemHolder const&) = delete;
    ItemHolder(ItemHolder&&) = delete;
    ItemHolder& operator=(ItemHolder&&) = delete;

    /// @brief The item; null once Qt deleted it with a parent.
    /// @return The item.
    [[nodiscard]] QQuickItem* item() const noexcept { return _item.data(); }

    /// @brief The backend's context.
    /// @return The context.
    [[nodiscard]] Context& context() const noexcept { return *_context; }

    /// @brief Names the item, links it to @p self, then parents it into @p parent's content or the root.
    /// @param self The wrapper this holder is part of.
    /// @param parent The container, or null for the root.
    /// @throws std::invalid_argument when @p parent was not made by this backend.
    void attach(ui::Widget& self, ui::ContainerWidget* parent);

    /// @brief `ui::Widget::setVisible`: records the flag and shows or hides the item.
    /// @param visible Whether the item is shown.
    void applyVisible(bool visible);

    /// @brief The widget's own visibility flag: what `setVisible` last set, true for a new widget. A hidden
    ///        ancestor, a collapsed panel or a closed dialog does not change it, unlike `QQuickItem::isVisible()`.
    /// @return The flag.
    [[nodiscard]] bool visibleFlag() const noexcept { return _visible; }

    /// @brief `ui::Widget::setEnabled`: records the flag and enables or disables the item.
    /// @param enabled Whether the item takes input.
    void applyEnabled(bool enabled);

    /// @brief The widget's own enabled flag: what `setEnabled` last set, true for a new widget. A disabled
    ///        ancestor does not change it, unlike `QQuickItem::isEnabled()`.
    /// @return The flag.
    [[nodiscard]] bool enabledFlag() const noexcept { return _enabled; }

    /// @brief `ui::Widget::setLayout`: Content, Fixed and Stretch become `Layout.*` attached properties.
    /// @param hints The sizing on both axes.
    void applyHints(ui::LayoutHints const& hints);

    /// @brief The width a containing table gives this item while its own width is Content: a row stretches across
    ///        the table, a cell takes its column's sizing.
    /// @param width That sizing, or nullopt outside a table.
    void setDefaultWidth(std::optional<ui::Sizing> width);

    /// @brief `ui::Widget::setDragKey`: engaged adds or updates a `DragSource`, nullopt removes it.
    /// @param key The key a drag carries.
    void applyDragKey(std::optional<ui::Key> const& key);

    /// @brief `ui::Widget::setDropHandler`: a set @p onDrop adds or updates a `DropTarget`, an empty one removes it.
    /// @param accepts Decides per key; empty accepts every key.
    /// @param onDrop Receives a dropped key.
    void applyDropHandler(std::function<bool(ui::Key const&)> accepts, std::function<void(ui::Key)> onDrop);

protected:
    /// @brief Records the visibility flag for a wrapper whose `setVisible` does not hide the item itself.
    /// @param visible What `setVisible` was given.
    void rememberVisible(bool visible) noexcept { _visible = visible; }

    /// @brief Connects the item's QML signal @p signature to @p handler.
    /// @param signature The signal's C++ signature.
    /// @param handler Receives each emission.
    void relay(char const* signature, SignalRelay::Handler handler);

    /// @brief Writes a property the component declares.
    /// @param property Its name.
    /// @param value The value.
    /// @throws std::logic_error when the component declares no such property.
    void write(char const* property, QVariant const& value) const;

    /// @brief Calls a function the component declares with one argument.
    /// @param method Its name.
    /// @param argument The argument.
    /// @throws std::logic_error when the call fails.
    void invoke(char const* method, QVariant const& argument) const;

private:
    void applyLayout();

    Context* _context;
    QPointer<QQuickItem> _item;
    bool _visible = true;
    bool _enabled = true;
    ui::LayoutHints _hints{};
    std::optional<ui::Sizing> _defaultWidth;
    QPointer<QQuickItem> _dragSource;
    QPointer<QQuickItem> _dropTarget;
    std::unique_ptr<DropRelay> _dropRelay;
};

/// @brief An `ItemHolder` whose component has a `content` item that children go into.
class ContainerHolder : public ItemHolder {
public:
    using ItemHolder::ItemHolder;

    /// @brief Where children go: the component's `content` property.
    /// @return The content item.
    /// @throws std::logic_error when the component has no `content`.
    [[nodiscard]] QQuickItem& content() const;

    /// @brief The content's children that are widgets, in layout (stacking) order.
    /// @return The child items.
    [[nodiscard]] std::vector<QQuickItem*> childWidgetItems() const;

    /// @brief Restacks @p child so it is the @p index-th widget child (or the last, past the end).
    /// @param child A widget child of this container.
    /// @param index Its new position.
    void moveChildItem(QQuickItem& child, std::size_t index) const;
};

/// @brief The holder of a wrapper this backend made.
/// @param widget Any widget.
/// @return Its holder, or null for a widget of another backend.
[[nodiscard]] ItemHolder* holderOf(ui::Widget& widget) noexcept;

/// @brief The holder of a wrapper this backend made.
/// @param widget Any widget.
/// @return Its holder, or null for a widget of another backend.
[[nodiscard]] ItemHolder const* holderOf(ui::Widget const& widget) noexcept;

/// @brief The holder an item was attached to.
/// @param item Any item.
/// @return Its holder, or null for an item that is not a widget's.
[[nodiscard]] ItemHolder* holderOf(QQuickItem const& item) noexcept;

/// @brief The wrapper an item was attached to.
/// @param item Any item.
/// @return The widget, or null for an item that is not a widget's.
[[nodiscard]] ui::Widget* widgetOf(QQuickItem const& item) noexcept;

/// @brief Where @p container's children go.
/// @param container A container this backend made.
/// @return Its content item.
/// @throws std::invalid_argument when @p container was made by another backend.
[[nodiscard]] QQuickItem& contentOf(ui::ContainerWidget& container);

/// @brief Writes an attached property such as `Layout.fillWidth`, resolved through @p item's QML context.
/// @param item The item; its component imports the attaching type's module.
/// @param name The attached property, `Type.property`.
/// @param value The value.
/// @throws std::logic_error when the property does not resolve or refuses the value.
void writeAttached(QQuickItem& item, char const* name, QVariant const& value);

/// @brief A widget wrapper: `Interface` (a `ui::…Widget`) implemented over one `MorphUi` item.
/// @tparam Interface The widget interface.
/// @tparam Holder `ItemHolder`, or `ContainerHolder` for a container.
template <class Interface, class Holder = ItemHolder>
class ItemWidget : public Interface, public Holder {
public:
    /// @param context The backend's context.
    /// @param kind The component to instantiate.
    /// @param parent The container, or null for the root.
    /// @param initial Properties set before the item's bindings first run.
    ItemWidget(Context& context, Kind kind, ui::ContainerWidget* parent, QVariantMap const& initial = {})
        : Holder{context, context.create(kind, initial)} {
        Holder::attach(*this, parent);
    }

    void setVisible(bool visible) override { Holder::applyVisible(visible); }
    void setEnabled(bool enabled) override { Holder::applyEnabled(enabled); }
    void setLayout(ui::LayoutHints const& hints) override { Holder::applyHints(hints); }
    void setDragKey(std::optional<ui::Key> const& key) override { Holder::applyDragKey(key); }
    void setDropHandler(std::function<bool(ui::Key const&)> accepts, std::function<void(ui::Key)> onDrop) override {
        Holder::applyDropHandler(std::move(accepts), std::move(onDrop));
    }
};

/// @brief A container wrapper: `moveChild` restacks the child within `content`.
/// @tparam Interface A `ui::ContainerWidget` interface.
template <class Interface>
class ItemContainer : public ItemWidget<Interface, ContainerHolder> {
public:
    using ItemWidget<Interface, ContainerHolder>::ItemWidget;

    void moveChild(ui::Widget& child, std::size_t index) override {
        ItemHolder* const holder = holderOf(child);
        if (holder == nullptr || holder->item() == nullptr) {
            throw std::invalid_argument{"qt_quick: moveChild of a widget this backend did not make"};
        }
        this->moveChildItem(*holder->item(), index);
    }
};

}  // namespace morph::qt_quick::detail
```

Create `src/qt_quick/item_holder.cpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0

#include "item_holder.hpp"

#include <QMetaObject>
#include <QObject>
#include <QQmlProperty>
#include <QString>
#include <QtQml/qqml.h>
#include <algorithm>
#include <string>
#include <utility>

#include "key_text.hpp"

namespace morph::qt_quick::detail {

namespace {

constexpr char const* kWidgetProperty = "morphWidget";
constexpr char const* kHolderProperty = "morphHolder";

// One axis of a LayoutHints as the Layout attached properties express it.
struct AxisLayout {
    double preferred;
    bool fill;
    int stretch;
};

// Content: the implicit size. Fixed(n): n units. Stretch(w): a share of the free space in proportion to w, from a
// preferred size of 0, so weights alone decide.
AxisLayout axisLayout(ui::Sizing sizing, double unit) {
    switch (sizing.kind) {
        case ui::Sizing::Kind::Fixed:
            return {.preferred = static_cast<double>(sizing.amount) * unit, .fill = false, .stretch = -1};
        case ui::Sizing::Kind::Stretch:
            return {.preferred = 0.0, .fill = true, .stretch = std::max(sizing.amount, 1)};
        case ui::Sizing::Kind::Content:
        default:
            return {.preferred = -1.0, .fill = false, .stretch = -1};
    }
}

}  // namespace

ItemHolder::ItemHolder(Context& context, QQuickItem* item) : _context{&context}, _item{item} {}

ItemHolder::~ItemHolder() {
    delete _item.data();  // NOLINT(cppcoreguidelines-owning-memory): the holder owns its item
}

void ItemHolder::attach(ui::Widget& self, ui::ContainerWidget* parent) {
    QQuickItem& host = parent == nullptr ? _context->root() : contentOf(*parent);
    _item->setObjectName(_context->nextName());
    _item->setProperty(kWidgetProperty, QVariant::fromValue(static_cast<void*>(&self)));
    _item->setProperty(kHolderProperty, QVariant::fromValue(static_cast<void*>(this)));
    _item->setParentItem(&host);
    _item->setParent(&host);
}

void ItemHolder::applyVisible(bool visible) {
    _visible = visible;
    _item->setVisible(visible);
}

void ItemHolder::applyEnabled(bool enabled) {
    _enabled = enabled;
    _item->setEnabled(enabled);
}

void ItemHolder::applyHints(ui::LayoutHints const& hints) {
    _hints = hints;
    applyLayout();
}

void ItemHolder::setDefaultWidth(std::optional<ui::Sizing> width) {
    _defaultWidth = width;
    applyLayout();
}

void ItemHolder::applyLayout() {
    ui::Sizing const width =
        _hints.width.kind == ui::Sizing::Kind::Content && _defaultWidth ? *_defaultWidth : _hints.width;
    AxisLayout const horizontal = axisLayout(width, _context->unitWidth());
    AxisLayout const vertical = axisLayout(_hints.height, _context->unitHeight());
    writeAttached(*_item, "Layout.preferredWidth", horizontal.preferred);
    writeAttached(*_item, "Layout.fillWidth", horizontal.fill);
    writeAttached(*_item, "Layout.horizontalStretchFactor", horizontal.stretch);
    writeAttached(*_item, "Layout.preferredHeight", vertical.preferred);
    writeAttached(*_item, "Layout.fillHeight", vertical.fill);
    writeAttached(*_item, "Layout.verticalStretchFactor", vertical.stretch);
}

void ItemHolder::applyDragKey(std::optional<ui::Key> const& key) {
    if (!key) {
        delete _dragSource.data();  // NOLINT(cppcoreguidelines-owning-memory): a decoration of the owned item
        return;
    }
    if (_dragSource.isNull()) {
        _dragSource = _context->decorate(Kind::DragSource, *_item);
    }
    _dragSource->setProperty("keyText", encodeKey(*key));
}

void ItemHolder::applyDropHandler(std::function<bool(ui::Key const&)> accepts, std::function<void(ui::Key)> onDrop) {
    if (!onDrop) {
        delete _dropTarget.data();  // NOLINT(cppcoreguidelines-owning-memory): a decoration of the owned item
        _dropRelay.reset();
        return;
    }
    auto relay = std::make_unique<DropRelay>(
        [accepts = std::move(accepts)](QString const& text) {
            std::optional<ui::Key> const key = decodeKey(text);
            return key.has_value() && (!accepts || accepts(*key));
        },
        [onDrop = std::move(onDrop)](QString const& text) {
            if (std::optional<ui::Key> key = decodeKey(text)) {
                onDrop(std::move(*key));
            }
        });
    if (_dropTarget.isNull()) {
        _dropTarget = _context->decorate(Kind::DropTarget, *_item);
    }
    _dropTarget->setProperty("handler", QVariant::fromValue(static_cast<QObject*>(relay.get())));
    // The previous relay dies only after the target stopped pointing at it.
    _dropRelay = std::move(relay);
}

void ItemHolder::relay(char const* signature, SignalRelay::Handler handler) {
    static_cast<void>(SignalRelay::listen(*_item, signature, std::move(handler)));
}

void ItemHolder::write(char const* property, QVariant const& value) const {
    if (_item->metaObject()->indexOfProperty(property) < 0) {
        throw std::logic_error{std::string{"qt_quick: "} + _item->metaObject()->className() + " has no property " +
                               property};
    }
    _item->setProperty(property, value);
}

void ItemHolder::invoke(char const* method, QVariant const& argument) const {
    if (!QMetaObject::invokeMethod(_item.data(), method, Q_ARG(QVariant, argument))) {
        throw std::logic_error{std::string{"qt_quick: "} + _item->metaObject()->className() + " has no function " +
                               method};
    }
}

QQuickItem& ContainerHolder::content() const {
    auto* const content = item()->property("content").value<QQuickItem*>();
    if (content == nullptr) {
        throw std::logic_error{std::string{"qt_quick: "} + item()->metaObject()->className() + " has no content"};
    }
    return *content;
}

std::vector<QQuickItem*> ContainerHolder::childWidgetItems() const {
    std::vector<QQuickItem*> children;
    for (QQuickItem* const child : content().childItems()) {
        if (widgetOf(*child) != nullptr) {
            children.push_back(child);
        }
    }
    return children;
}

void ContainerHolder::moveChildItem(QQuickItem& child, std::size_t index) const {
    std::vector<QQuickItem*> siblings = childWidgetItems();
    std::erase(siblings, &child);
    if (siblings.empty()) {
        return;
    }
    // A Layout lays its children out in stacking order and follows restacking.
    if (index < siblings.size()) {
        child.stackBefore(siblings.at(index));
    } else {
        child.stackAfter(siblings.back());
    }
}

ItemHolder* holderOf(ui::Widget& widget) noexcept { return dynamic_cast<ItemHolder*>(&widget); }

ItemHolder const* holderOf(ui::Widget const& widget) noexcept { return dynamic_cast<ItemHolder const*>(&widget); }

ItemHolder* holderOf(QQuickItem const& item) noexcept {
    return static_cast<ItemHolder*>(item.property(kHolderProperty).value<void*>());
}

ui::Widget* widgetOf(QQuickItem const& item) noexcept {
    return static_cast<ui::Widget*>(item.property(kWidgetProperty).value<void*>());
}

QQuickItem& contentOf(ui::ContainerWidget& container) {
    auto* const holder = dynamic_cast<ContainerHolder*>(&container);
    if (holder == nullptr) {
        throw std::invalid_argument{"qt_quick: a parent container this backend did not make"};
    }
    return holder->content();
}

void writeAttached(QQuickItem& item, char const* name, QVariant const& value) {
    QQmlProperty property{&item, QString::fromLatin1(name), qmlContext(&item)};
    if (!property.isValid() || !property.write(value)) {
        throw std::logic_error{std::string{"qt_quick: cannot write "} + name + " on " + item.metaObject()->className()};
    }
}

}  // namespace morph::qt_quick::detail
```

Create `src/qt_quick/widgets.hpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0

#pragma once
#include <morph/ui/backend.hpp>
#include <morph/ui/view.hpp>
#include <string_view>

#include "context.hpp"
#include "item_holder.hpp"
#include "strings.hpp"

/// @file
/// @brief Wrappers for the text, spacing and stacking kinds.

namespace morph::qt_quick::detail {

/// @brief `ui::Text` as a `MorphUi.Label`.
class TextLabel final : public ItemWidget<ui::TextWidget> {
public:
    /// @param context The backend's context.
    /// @param parent The container, or null for the root.
    TextLabel(Context& context, ui::ContainerWidget* parent) : ItemWidget{context, Kind::Label, parent} {}

    void setText(std::string_view text) override { write("text", toQString(text)); }
    void setRole(ui::TextRole role) override { write("role", static_cast<int>(role)); }
};

/// @brief `ui::Spacer` as a `MorphUi.Spacer`.
class SpacerItem final : public ItemWidget<ui::SpacerWidget> {
public:
    /// @param context The backend's context.
    /// @param parent The container, or null for the root.
    SpacerItem(Context& context, ui::ContainerWidget* parent) : ItemWidget{context, Kind::Spacer, parent} {}
};

/// @brief `ui::Column`, `ui::Row` and a `ForEach`'s container, as a `MorphUi.ColumnStack` or `RowStack`.
class Stack final : public ItemContainer<ui::StackWidget> {
public:
    /// @param context The backend's context.
    /// @param parent The container, or null for the root.
    /// @param axis Vertical is a column, horizontal a row.
    Stack(Context& context, ui::ContainerWidget* parent, ui::Axis axis)
        : ItemContainer{context, axis == ui::Axis::Horizontal ? Kind::RowStack : Kind::ColumnStack, parent},
          _axis{axis} {}

    void setGap(int gap) override {
        double const unit = _axis == ui::Axis::Horizontal ? context().unitWidth() : context().unitHeight();
        write("spacing", static_cast<double>(gap) * unit);
    }

private:
    ui::Axis _axis;
};

/// @brief A `Switch` case's container, as a `MorphUi.Slot`.
class SlotBox final : public ItemContainer<ui::SlotWidget> {
public:
    /// @param context The backend's context.
    /// @param parent The container, or null for the root.
    SlotBox(Context& context, ui::ContainerWidget* parent) : ItemContainer{context, Kind::Slot, parent} {}
};

}  // namespace morph::qt_quick::detail
```

Create `src/qt_quick/qml/Label.qml`:

```qml
// SPDX-License-Identifier: Apache-2.0
import QtQuick
import QtQuick.Controls as C
// Every widget component imports QtQuick.Layouts: the backend writes Layout.*
// attached properties through the item's own QML context, which resolves the
// attaching type through this file's imports.
import QtQuick.Layouts

// ui::Text. `role` is morph::ui::TextRole as an int: 0 Normal, 1 Muted,
// 2 Heading, 3 Error, 4 Success.
C.Label {
    id: root
    property int role: 0
    readonly property real basePointSize: Qt.application.font.pointSize > 0 ? Qt.application.font.pointSize : 12

    color: role === 1 ? palette.placeholderText
         : role === 3 ? "#b3261e"
         : role === 4 ? "#2e7d32"
         : palette.windowText
    font.bold: role === 2
    font.pointSize: role === 2 ? basePointSize * 1.4 : basePointSize
}
```

Create `src/qt_quick/qml/Spacer.qml`:

```qml
// SPDX-License-Identifier: Apache-2.0
import QtQuick
import QtQuick.Layouts

// ui::Spacer: an empty item; its LayoutHints decide how much room it takes.
Item {}
```

Create `src/qt_quick/qml/ColumnStack.qml`:

```qml
// SPDX-License-Identifier: Apache-2.0
import QtQuick
import QtQuick.Layouts

// ui::Column and a vertical ForEach. Children go into `content`; the root is a
// plain Item so a decoration parented into it is not laid out as a child.
Item {
    id: root
    property real spacing: 0
    readonly property Item content: column
    implicitWidth: column.implicitWidth
    implicitHeight: column.implicitHeight

    ColumnLayout {
        id: column
        anchors.fill: parent
        spacing: root.spacing
    }
}
```

Create `src/qt_quick/qml/RowStack.qml`:

```qml
// SPDX-License-Identifier: Apache-2.0
import QtQuick
import QtQuick.Layouts

// ui::Row, a horizontal ForEach and a table row. Children go into `content`;
// the root is a plain Item so a decoration parented into it is not laid out.
Item {
    id: root
    property real spacing: 0
    readonly property Item content: row
    implicitWidth: row.implicitWidth
    implicitHeight: row.implicitHeight

    RowLayout {
        id: row
        anchors.fill: parent
        spacing: root.spacing
    }
}
```

Create `src/qt_quick/qml/Slot.qml`:

```qml
// SPDX-License-Identifier: Apache-2.0
import QtQuick
import QtQuick.Layouts

// The container a Switch mounts its current case into.
Item {
    id: root
    readonly property Item content: column
    implicitWidth: column.implicitWidth
    implicitHeight: column.implicitHeight

    ColumnLayout {
        id: column
        anchors.fill: parent
        spacing: 0
    }
}
```

Create `src/qt_quick/qml/DragSource.qml`:

```qml
// SPDX-License-Identifier: Apache-2.0
import QtQuick
import QtQuick.Controls as C

// Makes its parent draggable. While a drag is active a translucent copy of the
// parent follows the pointer on the window's overlay; its Drag attached property
// is active, so the DropTarget under the pointer sees the drag, and `source`
// carries the key as text.
Item {
    id: root
    objectName: "dragSource"
    anchors.fill: parent

    // "i:<int64>" or "s:<UTF-8 text>"; read by DropTarget, never parsed here.
    property string keyText: ""
    // Where in the parent the pointer went down; the copy keeps that offset.
    property point grabOffset: Qt.point(0, 0)
    readonly property Item overlayLayer: C.Overlay.overlay

    DragHandler {
        id: handler
        target: null
        onActiveChanged: {
            if (active) {
                root.grabOffset = centroid.pressPosition
                copy.Drag.active = true
            } else {
                // drop() delivers the drop and ends the drag; Drag.active
                // follows only afterwards, or the drop would be a cancel.
                copy.Drag.drop()
                copy.Drag.active = false
            }
        }
    }

    Item {
        id: copy
        objectName: "dragCopy"
        parent: root.overlayLayer
        visible: handler.active
        width: root.width
        height: root.height
        x: handler.centroid.scenePosition.x - root.grabOffset.x
        y: handler.centroid.scenePosition.y - root.grabOffset.y
        opacity: 0.6

        Drag.dragType: Drag.Internal
        Drag.keys: ["morph-key"]
        Drag.source: root
        Drag.hotSpot: root.grabOffset
        Drag.mimeData: ({ "text/plain": root.keyText })

        ShaderEffectSource {
            anchors.fill: parent
            sourceItem: handler.active ? root.parent : null
            live: false
        }
    }
}
```

Create `src/qt_quick/qml/DropTarget.qml`:

```qml
// SPDX-License-Identifier: Apache-2.0
import QtQuick

// Makes its parent a drop target. `handler` is the C++ object that decides:
// handler.accepts(keyText) when a drag enters, handler.drop(keyText) on release.
// The outline shows while the drag over it is accepted.
DropArea {
    id: root
    objectName: "dropTarget"
    anchors.fill: parent
    keys: ["morph-key"]

    property QtObject handler: null
    property bool highlighted: false

    function keyOf(event) {
        return event.source && event.source.keyText !== undefined ? event.source.keyText : ""
    }

    onEntered: (drag) => {
        root.highlighted = root.handler !== null && root.handler.accepts(root.keyOf(drag))
        drag.accepted = root.highlighted
    }
    onExited: root.highlighted = false
    onDropped: (drop) => {
        if (root.highlighted && root.handler !== null) {
            root.handler.drop(root.keyOf(drop))
            drop.accept()
        }
        root.highlighted = false
    }

    Rectangle {
        anchors.fill: parent
        color: "transparent"
        border.width: 2
        border.color: "#1e88e5"
        radius: 2
        visible: root.highlighted
    }
}
```

In `src/qt_quick/CMakeLists.txt`, the QML list becomes:

```cmake
set(_morph_qt_quick_qml_files
    qml/ColumnStack.qml
    qml/DragSource.qml
    qml/DropTarget.qml
    qml/Label.qml
    qml/RowStack.qml
    qml/Slot.qml
    qml/Spacer.qml
    qml/Window.qml
)
```

and `add_library(morph_qt_quick STATIC ...)` gains `context.cpp context.hpp item_holder.cpp item_holder.hpp
widgets.hpp` (keep the list alphabetical).

- [ ] **Step 4: Run the tests to verify they pass**

Run: `cmake --build build/qt --target morph_qt_quick_tests && ./build/qt/tests/qt_quick/morph_qt_quick_tests "[qt_quick]"`
Expected: PASS.

Mutation checks (one at a time, then restore):
- Make `ContainerHolder::moveChildItem` return at once → expected FAIL in "moveChild reorders the children and the
  layout follows".
- In `ItemHolder::applyVisible`, drop `_visible = visible;` → expected FAIL in "the visibility flag is the widget's
  own, not its ancestors'".
- In `DragSource.qml`, swap the two lines of the `else` branch (`Drag.active = false` before `Drag.drop()`) →
  expected FAIL in "drop delivers an int64 key above 2^53 exactly" (`dropped` is empty).
- In `DropTarget.qml`, replace `drag.accepted = root.highlighted` with `drag.accepted = true` and drop the
  `root.highlighted &&` from `onDropped` → expected FAIL in "a refusing target is not highlighted and gets no drop".

- [ ] **Step 5: Commit**

```bash
git add src/qt_quick tests/qt_quick
git commit -m "wip(qtquick): the common widget surface, stacks, labels and drag and drop

Signed-off-by: Christian Parpart <christian@parpart.family>"
```

---

### Task 3: Layout units and `Grid`

`LayoutHints` produce the documented geometry, and `Grid` lays cells out in columns with spans and unit gaps.

**Files:**
- Create: `src/qt_quick/qml/Grid.qml`
- Modify: `src/qt_quick/context.hpp` — `Grid` before `Count` in `Kind`, `"Grid"` last in `kTypeNames`
- Modify: `src/qt_quick/widgets.hpp` — `GridBox` after `SlotBox`
- Modify: `src/qt_quick/CMakeLists.txt` — `qml/Grid.qml` in the QML list
- Modify: `tests/qt_quick/CMakeLists.txt` — `test_layout.cpp`
- Test: `tests/qt_quick/test_layout.cpp`

**Interfaces:**
- Consumes: Task 2's `ItemContainer`, `writeAttached`, `holderOf`, `Context::unitWidth/unitHeight`;
  `ui::GridWidget` (Part 2).
- Produces: `detail::Kind::Grid`; `detail::GridBox(Context&, ui::ContainerWidget*)`; `Grid.qml` properties
  `columns`, `rowSpacing`, `columnSpacing`, `content`.

- [ ] **Step 1: Write the failing test**

Create `tests/qt_quick/test_layout.cpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0

#include <QQmlProperty>
#include <QQuickItem>
#include <QString>
#include <QtQml/qqml.h>
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <morph/ui/view.hpp>

#include "scene.hpp"
#include "widgets.hpp"

namespace {

using morph::qt_quick::detail::GridBox;
using morph::qt_quick::detail::SpacerItem;
using morph::qt_quick::detail::Stack;
using morph::qt_quick::detail::TextLabel;
using morph::qt_quick::testing::itemOf;
using morph::qt_quick::testing::Scene;
using morph::qt_quick::testing::unitHeight;
using morph::qt_quick::testing::unitWidth;
namespace ui = morph::ui;

constexpr ui::LayoutHints kFill{.width = ui::Sizing::stretch(), .height = ui::Sizing::stretch()};

}  // namespace

TEST_CASE("qt_quick layout: Fixed(n) is n character widths wide and n line heights tall", "[qt_quick][layout]") {
    Scene scene;
    Stack row{scene.context, nullptr, ui::Axis::Horizontal};
    row.setLayout(kFill);
    TextLabel label{scene.context, &row};
    label.setText("x");
    label.setLayout({.width = ui::Sizing::fixed(10), .height = ui::Sizing::fixed(3)});
    scene.show();
    CHECK(itemOf(label).width() == Catch::Approx(10 * unitWidth()).margin(0.5));
    CHECK(itemOf(label).height() == Catch::Approx(3 * unitHeight()).margin(0.5));
}

TEST_CASE("qt_quick layout: Stretch weights share the free space in proportion", "[qt_quick][layout]") {
    Scene scene;
    Stack row{scene.context, nullptr, ui::Axis::Horizontal};
    row.setLayout(kFill);
    SpacerItem narrow{scene.context, &row};
    SpacerItem wide{scene.context, &row};
    narrow.setLayout({.width = ui::Sizing::stretch(1), .height = ui::Sizing::stretch()});
    wide.setLayout({.width = ui::Sizing::stretch(3), .height = ui::Sizing::stretch()});
    scene.show();
    double const total = itemOf(row).width();
    REQUIRE(total > 100.0);
    CHECK(itemOf(narrow).width() == Catch::Approx(total / 4).margin(1.0));
    CHECK(itemOf(wide).width() == Catch::Approx(total * 3 / 4).margin(1.0));
    CHECK(itemOf(narrow).height() == Catch::Approx(itemOf(row).height()).margin(1.0));
}

TEST_CASE("qt_quick layout: Content keeps the implicit size beside a stretching sibling", "[qt_quick][layout]") {
    Scene scene;
    Stack row{scene.context, nullptr, ui::Axis::Horizontal};
    row.setLayout(kFill);
    TextLabel label{scene.context, &row};
    label.setText("content-sized");
    SpacerItem rest{scene.context, &row};
    rest.setLayout({.width = ui::Sizing::stretch(), .height = ui::Sizing::content()});
    scene.show();
    CHECK(itemOf(label).width() == Catch::Approx(itemOf(label).implicitWidth()).margin(0.5));
}

TEST_CASE("qt_quick layout: going back to Content clears a Stretch", "[qt_quick][layout]") {
    Scene scene;
    Stack row{scene.context, nullptr, ui::Axis::Horizontal};
    row.setLayout(kFill);
    TextLabel label{scene.context, &row};
    label.setText("x");
    label.setLayout(kFill);
    label.setLayout({});
    scene.show();
    CHECK(itemOf(label).width() == Catch::Approx(itemOf(label).implicitWidth()).margin(0.5));
}

TEST_CASE("qt_quick Grid: columns wrap cells into rows, a span covers columns, gaps are in units",
          "[qt_quick][layout]") {
    Scene scene;
    GridBox grid{scene.context, nullptr};
    grid.setColumns(2);
    grid.setGap(1);
    TextLabel wide{scene.context, &grid};
    TextLabel left{scene.context, &grid};
    TextLabel right{scene.context, &grid};
    wide.setText("spans both");
    left.setText("left");
    right.setText("right");
    grid.setSpan(wide, 2);
    scene.show();

    QQuickItem& wideItem = itemOf(wide);
    QQuickItem const& leftItem = itemOf(left);
    QQuickItem const& rightItem = itemOf(right);
    CHECK(QQmlProperty{&wideItem, QStringLiteral("Layout.columnSpan"), qmlContext(&wideItem)}.read().toInt() == 2);
    CHECK(leftItem.y() == Catch::Approx(rightItem.y()));
    CHECK(leftItem.y() - (wideItem.y() + wideItem.height()) == Catch::Approx(unitHeight()).margin(0.5));
    CHECK(rightItem.x() - (leftItem.x() + leftItem.width()) == Catch::Approx(unitWidth()).margin(0.5));
}
```

- [ ] **Step 2: Run it to verify it fails**

Add `test_layout.cpp` to `morph_qt_quick_tests` and build.
Expected: FAIL — `no member named 'GridBox' in namespace 'morph::qt_quick::detail'`.

- [ ] **Step 3: Implement**

In `src/qt_quick/context.hpp`, insert `Grid,` before `Count,` in `Kind` and `"Grid",` at the end of `kTypeNames`.

Append to `src/qt_quick/widgets.hpp`, after `SlotBox` (add `#include <algorithm>` and `#include <stdexcept>`):

```cpp
/// @brief `ui::Grid` as a `MorphUi.Grid`.
class GridBox final : public ItemContainer<ui::GridWidget> {
public:
    /// @param context The backend's context.
    /// @param parent The container, or null for the root.
    GridBox(Context& context, ui::ContainerWidget* parent) : ItemContainer{context, Kind::Grid, parent} {}

    void setColumns(int columns) override { write("columns", std::max(columns, 1)); }

    void setGap(int gap) override {
        write("rowSpacing", static_cast<double>(gap) * context().unitHeight());
        write("columnSpacing", static_cast<double>(gap) * context().unitWidth());
    }

    void setSpan(ui::Widget& child, int span) override {
        ItemHolder* const holder = holderOf(child);
        if (holder == nullptr || holder->item() == nullptr) {
            throw std::invalid_argument{"qt_quick: setSpan of a widget this backend did not make"};
        }
        writeAttached(*holder->item(), "Layout.columnSpan", std::max(span, 1));
    }
};
```

Create `src/qt_quick/qml/Grid.qml`:

```qml
// SPDX-License-Identifier: Apache-2.0
import QtQuick
import QtQuick.Layouts

// ui::Grid. Cells go into `content`, a GridLayout; a cell's span is its
// Layout.columnSpan, which the backend writes on the cell.
Item {
    id: root
    property int columns: 1
    property real rowSpacing: 0
    property real columnSpacing: 0
    readonly property Item content: grid
    implicitWidth: grid.implicitWidth
    implicitHeight: grid.implicitHeight

    GridLayout {
        id: grid
        anchors.fill: parent
        columns: root.columns
        rowSpacing: root.rowSpacing
        columnSpacing: root.columnSpacing
    }
}
```

Add `qml/Grid.qml` to the QML list in `src/qt_quick/CMakeLists.txt` (alphabetically, after `qml/DropTarget.qml`).

- [ ] **Step 4: Run the tests to verify they pass**

Run: `cmake --build build/qt --target morph_qt_quick_tests && ./build/qt/tests/qt_quick/morph_qt_quick_tests "[layout]"`
Expected: PASS.

Mutation checks (one at a time, then restore):
- In `axisLayout`, return `.preferred = static_cast<double>(sizing.amount)` for `Fixed` (no unit) → expected FAIL in
  "Fixed(n) is n character widths wide".
- In `axisLayout`, return `.stretch = -1` for `Stretch` → expected FAIL in "Stretch weights share the free space in
  proportion" (both spacers fill, so the split becomes 1:1).
- In `GridBox::setGap`, swap `unitHeight()` and `unitWidth()` → expected FAIL in the Grid case.

- [ ] **Step 5: Commit**

```bash
git add src/qt_quick tests/qt_quick
git commit -m "wip(qtquick): layout units and Grid

Signed-off-by: Christian Parpart <christian@parpart.family>"
```

---
### Task 4: Input leaves — Button, TextInput, Checkbox, Select, Menu

**Files:**
- Create: `src/qt_quick/qml/Button.qml`, `TextInput.qml`, `TextArea.qml`, `Checkbox.qml`, `Select.qml`,
  `RadioGroup.qml`, `Menu.qml`
- Create: `src/qt_quick/widgets_input.hpp`
- Modify: `src/qt_quick/context.hpp` — `Button, TextInput, TextArea, Checkbox, Select, RadioGroup, Menu` before
  `Count`, their names in the same order at the end of `kTypeNames`
- Modify: `src/qt_quick/CMakeLists.txt` — the seven QML files; `widgets_input.hpp` in `morph_qt_quick`
- Modify: `tests/qt_quick/CMakeLists.txt` — `test_input.cpp`
- Test: `tests/qt_quick/test_input.cpp`

**Interfaces:**
- Consumes: Task 2's `ItemWidget`, `relay`, `write`, `invoke`, `toQString`, `toUtf8`; `ui::ButtonWidget`,
  `ui::TextInputWidget`, `ui::CheckboxWidget`, `ui::SelectWidget`, `ui::MenuWidget`, `ui::TextInputMode`,
  `ui::SelectStyle`, `ui::SelectOption`, `ui::Action` (Part 2).
- Produces (`detail`): `PushButton(Context&, ui::ContainerWidget*)`,
  `TextEntry(Context&, ui::ContainerWidget*, ui::TextInputMode)`, `CheckBox(Context&, ui::ContainerWidget*)`,
  `Choice(Context&, ui::ContainerWidget*, ui::SelectStyle)`, `MenuList(Context&, ui::ContainerWidget*)`.
- QML contract: `Button.activated()`; `TextInput` / `TextArea`: `text`, `placeholderText`, `password` (TextInput),
  `applyText(value)`, `edited(string)`, `submitted(string)`; `Checkbox`: `text`, `checked`, `userToggled(bool)`;
  `Select` / `RadioGroup`: `labels`, `currentIndex`, `chosen(int)`; `Menu`: `labels`, `itemActivated(int)`.

**Design notes.** A user signal is distinct from a property write in every component: `Button.activated` comes from
`clicked`, `TextInput.edited` from `textEdited`, `Checkbox.userToggled` from `toggled`, `Select.chosen` from
`activated` — each emitted for user input only — and `TextArea`, whose control has no user-only signal, guards its
`textChanged` with an `applying` flag that `applyText` sets. `applyText` writes only a changed text, so the cursor of
an unchanged field stays put. A `Select`'s keys stay in C++: QML sees labels and an index, and every `setOptions`
re-applies the selected key's index, because a ComboBox resets its index when its model changes. Each wrapper
copies its callback before calling it, so a callback that replaces itself does not destroy the function it runs in.

- [ ] **Step 1: Write the failing test**

Create `tests/qt_quick/test_input.cpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0

#include <QMetaObject>
#include <QQuickItem>
#include <QString>
#include <QStringList>
#include <QTest>
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <morph/ui/view.hpp>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "scene.hpp"
#include "widgets_input.hpp"

namespace {

using morph::qt_quick::detail::CheckBox;
using morph::qt_quick::detail::Choice;
using morph::qt_quick::detail::MenuList;
using morph::qt_quick::detail::PushButton;
using morph::qt_quick::detail::TextEntry;
using morph::qt_quick::testing::centre;
using morph::qt_quick::testing::childrenOfType;
using morph::qt_quick::testing::itemOf;
using morph::qt_quick::testing::Scene;
using morph::qt_quick::testing::typeText;
namespace ui = morph::ui;

constexpr std::int64_t kBeyondDouble = (std::int64_t{1} << 53) + 1;

ui::SelectOption option(std::int64_t key, char const* label) { return {.key = ui::Key{key}, .label = label}; }

}  // namespace

TEST_CASE("qt_quick Button: the label shows, and each user click is one onClick", "[qt_quick][input]") {
    Scene scene;
    PushButton button{scene.context, nullptr};
    int clicks = 0;
    button.setLabel("Sprint");
    button.setOnClick([&clicks] { ++clicks; });
    scene.show();
    QQuickItem const& item = itemOf(button);
    CHECK(item.inherits("QQuickButton"));
    CHECK(item.property("text").toString() == QStringLiteral("Sprint"));
    QTest::mouseClick(scene.window.get(), Qt::LeftButton, Qt::NoModifier, centre(item));
    CHECK(clicks == 1);
    button.setLabel("Again");
    CHECK(clicks == 1);
}

TEST_CASE("qt_quick TextInput: each keystroke reports the whole text; Enter submits", "[qt_quick][input]") {
    Scene scene;
    TextEntry entry{scene.context, nullptr, ui::TextInputMode::SingleLine};
    std::vector<std::string> changes;
    std::vector<std::string> submits;
    entry.setOnChange([&changes](std::string text) { changes.push_back(std::move(text)); });
    entry.setOnSubmit([&submits](std::string text) { submits.push_back(std::move(text)); });
    entry.setPlaceholder("Rider");
    scene.show();
    QQuickItem& item = itemOf(entry);
    CHECK(item.inherits("QQuickTextField"));
    CHECK(item.property("placeholderText").toString() == QStringLiteral("Rider"));
    CHECK(item.property("echoMode").toInt() == 0);

    item.forceActiveFocus();
    typeText(*scene.window, QString::fromUtf8("äb"));
    CHECK(changes == std::vector<std::string>{"ä", "äb"});
    QTest::keyClick(scene.window.get(), Qt::Key_Return);
    CHECK(submits == std::vector<std::string>{"äb"});
}

TEST_CASE("qt_quick TextInput: setText with the current text keeps the cursor and emits nothing",
          "[qt_quick][input]") {
    Scene scene;
    TextEntry entry{scene.context, nullptr, ui::TextInputMode::SingleLine};
    int changes = 0;
    entry.setOnChange([&changes](std::string const&) { ++changes; });
    scene.show();
    QQuickItem& item = itemOf(entry);
    item.forceActiveFocus();
    typeText(*scene.window, QStringLiteral("hello"));
    REQUIRE(changes == 5);

    changes = 0;
    item.setProperty("cursorPosition", 2);
    entry.setText("hello");
    CHECK(item.property("cursorPosition").toInt() == 2);
    entry.setText("world");
    CHECK(item.property("text").toString() == QStringLiteral("world"));
    CHECK(changes == 0);
}

TEST_CASE("qt_quick TextInput: Password masks; Multiline is a TextArea that submits on Ctrl+Enter",
          "[qt_quick][input]") {
    Scene scene;
    TextEntry password{scene.context, nullptr, ui::TextInputMode::Password};
    CHECK(itemOf(password).property("echoMode").toInt() == 2);  // TextInput.Password

    TextEntry area{scene.context, nullptr, ui::TextInputMode::Multiline};
    std::vector<std::string> changes;
    std::vector<std::string> submits;
    area.setOnChange([&changes](std::string text) { changes.push_back(std::move(text)); });
    area.setOnSubmit([&submits](std::string text) { submits.push_back(std::move(text)); });
    scene.show();
    QQuickItem& item = itemOf(area);
    CHECK(item.inherits("QQuickTextArea"));

    area.setText("set by code");
    CHECK(item.property("text").toString() == QStringLiteral("set by code"));
    CHECK(changes.empty());

    item.forceActiveFocus();
    QMetaObject::invokeMethod(&item, "selectAll");
    typeText(*scene.window, QStringLiteral("ab"));
    CHECK(changes == std::vector<std::string>{"a", "ab"});
    QTest::keyClick(scene.window.get(), Qt::Key_Return, Qt::ControlModifier);
    CHECK(submits == std::vector<std::string>{"ab"});
}

TEST_CASE("qt_quick Checkbox: setChecked is silent; a user click reports the new state", "[qt_quick][input]") {
    Scene scene;
    CheckBox box{scene.context, nullptr};
    std::vector<bool> toggles;
    box.setOnToggle([&toggles](bool checked) { toggles.push_back(checked); });
    box.setLabel("Ready");
    box.setChecked(true);
    scene.show();
    QQuickItem const& item = itemOf(box);
    CHECK(item.inherits("QQuickCheckBox"));
    CHECK(item.property("text").toString() == QStringLiteral("Ready"));
    CHECK(item.property("checked").toBool());
    CHECK(toggles.empty());
    QTest::mouseClick(scene.window.get(), Qt::LeftButton, Qt::NoModifier, centre(item));
    CHECK(toggles == std::vector{false});
}

TEST_CASE("qt_quick Select: options by label, selection by key, a user choice by key", "[qt_quick][input]") {
    Scene scene;
    Choice select{scene.context, nullptr, ui::SelectStyle::Dropdown};
    std::vector<ui::Key> chosen;
    select.setOnSelect([&chosen](ui::Key key) { chosen.push_back(std::move(key)); });
    select.setOptions({option(1, "one"), option(kBeyondDouble, "big"),
                       ui::SelectOption{.key = ui::Key{std::string{"s"}}, .label = "text"}});
    select.setSelected(ui::Key{kBeyondDouble});
    QQuickItem& item = itemOf(select);
    CHECK(item.inherits("QQuickComboBox"));
    CHECK(item.property("labels").toStringList() == QStringList{QStringLiteral("one"), QStringLiteral("big"),
                                                                QStringLiteral("text")});
    CHECK(item.property("currentIndex").toInt() == 1);

    select.setSelected(ui::Key{std::int64_t{99}});
    CHECK(item.property("currentIndex").toInt() == -1);
    select.setSelected(std::nullopt);
    CHECK(item.property("currentIndex").toInt() == -1);

    QMetaObject::invokeMethod(&item, "activated", Q_ARG(int, 1));
    REQUIRE(chosen.size() == 1);
    CHECK(std::get<std::int64_t>(chosen.front()) == kBeyondDouble);
    QMetaObject::invokeMethod(&item, "activated", Q_ARG(int, 7));
    CHECK(chosen.size() == 1);
}

TEST_CASE("qt_quick Select: replacing the options re-applies the selected key", "[qt_quick][input]") {
    Scene scene;
    Choice select{scene.context, nullptr, ui::SelectStyle::Dropdown};
    select.setOptions({option(1, "a"), option(2, "b"), option(3, "c")});
    select.setSelected(ui::Key{std::int64_t{3}});
    QQuickItem const& item = itemOf(select);
    REQUIRE(item.property("currentIndex").toInt() == 2);
    select.setOptions({option(9, "x"), option(3, "c")});
    CHECK(item.property("currentIndex").toInt() == 1);
    select.setOptions({option(9, "x")});
    CHECK(item.property("currentIndex").toInt() == -1);
}

TEST_CASE("qt_quick Select: Radio is a column of radio buttons; a click chooses by key", "[qt_quick][input]") {
    Scene scene;
    Choice radio{scene.context, nullptr, ui::SelectStyle::Radio};
    std::vector<ui::Key> chosen;
    radio.setOnSelect([&chosen](ui::Key key) { chosen.push_back(std::move(key)); });
    radio.setOptions({option(1, "one"), option(2, "two"), option(3, "three")});
    radio.setSelected(ui::Key{std::int64_t{2}});
    scene.show();
    std::vector<QQuickItem*> const buttons = childrenOfType(itemOf(radio), "QQuickRadioButton");
    REQUIRE(buttons.size() == 3);
    CHECK(buttons.at(1)->property("checked").toBool());
    CHECK_FALSE(buttons.at(0)->property("checked").toBool());
    QTest::mouseClick(scene.window.get(), Qt::LeftButton, Qt::NoModifier, centre(*buttons.at(2)));
    REQUIRE(chosen.size() == 1);
    CHECK(std::get<std::int64_t>(chosen.front()) == 3);
    // Controlled: the check moves only when the application selects.
    CHECK(buttons.at(1)->property("checked").toBool());
}

TEST_CASE("qt_quick Menu: one delegate per item; a click activates its index", "[qt_quick][input]") {
    Scene scene;
    MenuList menu{scene.context, nullptr};
    std::vector<std::size_t> activated;
    menu.setOnActivate([&activated](std::size_t index) { activated.push_back(index); });
    menu.setItems({"Dashboard", "Statistics", "Quit"});
    scene.show();
    std::vector<QQuickItem*> const delegates = childrenOfType(itemOf(menu), "QQuickItemDelegate");
    REQUIRE(delegates.size() == 3);
    CHECK(delegates.at(1)->property("text").toString() == QStringLiteral("Statistics"));
    QTest::mouseClick(scene.window.get(), Qt::LeftButton, Qt::NoModifier, centre(*delegates.at(1)));
    CHECK(activated == std::vector<std::size_t>{1});
}
```

- [ ] **Step 2: Run it to verify it fails**

Add `test_input.cpp` to `morph_qt_quick_tests` and build.
Expected: FAIL — `'widgets_input.hpp' file not found`.

- [ ] **Step 3: Implement**

In `src/qt_quick/context.hpp`, before `Count,` insert `Button, TextInput, TextArea, Checkbox, Select, RadioGroup,
Menu,` (one per line) and append `"Button", "TextInput", "TextArea", "Checkbox", "Select", "RadioGroup", "Menu",` to
`kTypeNames`.

Create `src/qt_quick/widgets_input.hpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0

#pragma once
#include <QString>
#include <QStringList>
#include <QVariant>
#include <QVariantMap>
#include <cstddef>
#include <functional>
#include <morph/ui/backend.hpp>
#include <morph/ui/view.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "context.hpp"
#include "item_holder.hpp"
#include "strings.hpp"

/// @file
/// @brief Wrappers for the kinds a user types into, clicks and chooses with.

namespace morph::qt_quick::detail {

/// @brief `ui::Button` as a `MorphUi.Button`.
class PushButton final : public ItemWidget<ui::ButtonWidget> {
public:
    /// @param context The backend's context.
    /// @param parent The container, or null for the root.
    PushButton(Context& context, ui::ContainerWidget* parent) : ItemWidget{context, Kind::Button, parent} {
        relay("activated()", [this](QVariant const&) {
            if (ui::Action const onClick = _onClick) {
                onClick();
            }
        });
    }

    void setLabel(std::string_view label) override { write("text", toQString(label)); }
    void setOnClick(ui::Action onClick) override { _onClick = std::move(onClick); }

private:
    ui::Action _onClick;
};

/// @brief `ui::TextInput` as a `MorphUi.TextInput` (single line, password) or `MorphUi.TextArea` (multiline).
class TextEntry final : public ItemWidget<ui::TextInputWidget> {
public:
    /// @param context The backend's context.
    /// @param parent The container, or null for the root.
    /// @param mode Fixed for the widget's life.
    TextEntry(Context& context, ui::ContainerWidget* parent, ui::TextInputMode mode)
        : ItemWidget{context, mode == ui::TextInputMode::Multiline ? Kind::TextArea : Kind::TextInput, parent,
                     initialProperties(mode)} {
        relay("edited(QString)", [this](QVariant const& text) {
            if (auto const onChange = _onChange) {
                onChange(toUtf8(text.toString()));
            }
        });
        relay("submitted(QString)", [this](QVariant const& text) {
            if (auto const onSubmit = _onSubmit) {
                onSubmit(toUtf8(text.toString()));
            }
        });
    }

    void setText(std::string_view text) override { invoke("applyText", toQString(text)); }
    void setPlaceholder(std::string_view text) override { write("placeholderText", toQString(text)); }
    void setOnChange(std::function<void(std::string)> onChange) override { _onChange = std::move(onChange); }
    void setOnSubmit(std::function<void(std::string)> onSubmit) override { _onSubmit = std::move(onSubmit); }

private:
    static QVariantMap initialProperties(ui::TextInputMode mode) {
        if (mode == ui::TextInputMode::Multiline) {
            return {};
        }
        return QVariantMap{{QStringLiteral("password"), mode == ui::TextInputMode::Password}};
    }

    std::function<void(std::string)> _onChange;
    std::function<void(std::string)> _onSubmit;
};

/// @brief `ui::Checkbox` as a `MorphUi.Checkbox`.
class CheckBox final : public ItemWidget<ui::CheckboxWidget> {
public:
    /// @param context The backend's context.
    /// @param parent The container, or null for the root.
    CheckBox(Context& context, ui::ContainerWidget* parent) : ItemWidget{context, Kind::Checkbox, parent} {
        relay("userToggled(bool)", [this](QVariant const& checked) {
            if (auto const onToggle = _onToggle) {
                onToggle(checked.toBool());
            }
        });
    }

    void setLabel(std::string_view label) override { write("text", toQString(label)); }
    void setChecked(bool checked) override { write("checked", checked); }
    void setOnToggle(std::function<void(bool)> onToggle) override { _onToggle = std::move(onToggle); }

private:
    std::function<void(bool)> _onToggle;
};

/// @brief `ui::Select` as a `MorphUi.Select` (a ComboBox) or `MorphUi.RadioGroup`. Keys stay here: QML sees the
///        labels and the selected index.
class Choice final : public ItemWidget<ui::SelectWidget> {
public:
    /// @param context The backend's context.
    /// @param parent The container, or null for the root.
    /// @param style Fixed for the widget's life.
    Choice(Context& context, ui::ContainerWidget* parent, ui::SelectStyle style)
        : ItemWidget{context, style == ui::SelectStyle::Radio ? Kind::RadioGroup : Kind::Select, parent} {
        relay("chosen(int)", [this](QVariant const& index) { choose(index.toInt()); });
    }

    void setOptions(std::vector<ui::SelectOption> const& options) override {
        _options = options;
        QStringList labels;
        labels.reserve(static_cast<qsizetype>(options.size()));
        for (ui::SelectOption const& option : options) {
            labels.push_back(toQString(option.label));
        }
        write("labels", labels);
        // A ComboBox resets its index when its model changes.
        applySelection();
    }

    void setSelected(std::optional<ui::Key> const& key) override {
        _selected = key;
        applySelection();
    }

    void setOnSelect(std::function<void(ui::Key)> onSelect) override { _onSelect = std::move(onSelect); }

private:
    void applySelection() {
        int index = -1;
        if (_selected) {
            for (std::size_t position = 0; position < _options.size(); ++position) {
                if (_options.at(position).key == *_selected) {
                    index = static_cast<int>(position);
                    break;
                }
            }
        }
        write("currentIndex", index);
    }

    void choose(int index) {
        if (index < 0 || static_cast<std::size_t>(index) >= _options.size()) {
            return;
        }
        if (auto const onSelect = _onSelect) {
            onSelect(_options.at(static_cast<std::size_t>(index)).key);
        }
    }

    std::vector<ui::SelectOption> _options;
    std::optional<ui::Key> _selected;
    std::function<void(ui::Key)> _onSelect;
};

/// @brief `ui::Menu` as a `MorphUi.Menu`: one delegate per item, activated by index.
class MenuList final : public ItemWidget<ui::MenuWidget> {
public:
    /// @param context The backend's context.
    /// @param parent The container, or null for the root.
    MenuList(Context& context, ui::ContainerWidget* parent) : ItemWidget{context, Kind::Menu, parent} {
        relay("itemActivated(int)", [this](QVariant const& index) {
            if (index.toInt() < 0) {
                return;
            }
            if (auto const onActivate = _onActivate) {
                onActivate(static_cast<std::size_t>(index.toInt()));
            }
        });
    }

    void setItems(std::vector<std::string> const& items) override {
        QStringList labels;
        labels.reserve(static_cast<qsizetype>(items.size()));
        for (std::string const& label : items) {
            labels.push_back(toQString(label));
        }
        write("labels", labels);
    }

    void setOnActivate(std::function<void(std::size_t)> onActivate) override { _onActivate = std::move(onActivate); }

private:
    std::function<void(std::size_t)> _onActivate;
};

}  // namespace morph::qt_quick::detail
```

Create `src/qt_quick/qml/Button.qml`:

```qml
// SPDX-License-Identifier: Apache-2.0
import QtQuick
import QtQuick.Controls as C
import QtQuick.Layouts

// ui::Button. `activated` is emitted for a user click only.
C.Button {
    id: root
    signal activated()
    onClicked: root.activated()
}
```

Create `src/qt_quick/qml/TextInput.qml`:

```qml
// SPDX-License-Identifier: Apache-2.0
import QtQuick
// QtQuick again under a qualifier: inside MorphUi, an unqualified `TextInput`
// names this file's own type, not QtQuick's.
import QtQuick as Q
import QtQuick.Controls as C
import QtQuick.Layouts

// ui::TextInput, single line or password. `edited` is emitted for user edits
// only; applyText writes a text that differs and nothing else, so neither a
// write nor an unchanged text moves the cursor or reports an edit.
C.TextField {
    id: root
    property bool password: false
    signal edited(string text)
    signal submitted(string text)

    echoMode: password ? Q.TextInput.Password : Q.TextInput.Normal
    onTextEdited: root.edited(text)
    onAccepted: root.submitted(text)

    function applyText(value) {
        if (text !== value)
            text = value
    }
}
```

Create `src/qt_quick/qml/TextArea.qml`:

```qml
// SPDX-License-Identifier: Apache-2.0
import QtQuick
import QtQuick.Controls as C
import QtQuick.Layouts

// ui::TextInput, multiline. A TextArea has no user-only edit signal, so
// applyText raises `applying` around its write and textChanged reports only
// what the user typed. Ctrl+Enter submits; Enter is a newline.
C.TextArea {
    id: root
    property bool applying: false
    signal edited(string text)
    signal submitted(string text)

    onTextChanged: if (!root.applying) root.edited(text)
    Keys.onPressed: (event) => {
        if ((event.key === Qt.Key_Return || event.key === Qt.Key_Enter) && (event.modifiers & Qt.ControlModifier)) {
            root.submitted(text)
            event.accepted = true
        }
    }

    function applyText(value) {
        if (text === value)
            return
        applying = true
        text = value
        applying = false
    }
}
```

Create `src/qt_quick/qml/Checkbox.qml`:

```qml
// SPDX-License-Identifier: Apache-2.0
import QtQuick
import QtQuick.Controls as C
import QtQuick.Layouts

// ui::Checkbox. `userToggled` carries the state a user click produced; writing
// `checked` emits nothing.
C.CheckBox {
    id: root
    signal userToggled(bool checked)
    onToggled: root.userToggled(checked)
}
```

Create `src/qt_quick/qml/Select.qml`:

```qml
// SPDX-License-Identifier: Apache-2.0
import QtQuick
import QtQuick.Controls as C
import QtQuick.Layouts

// ui::Select, Dropdown. Options arrive as `labels`, the selection as
// `currentIndex` (-1: none); `chosen` is a user choice by index.
C.ComboBox {
    id: root
    property var labels: []
    signal chosen(int index)
    model: labels
    onActivated: (index) => root.chosen(index)
}
```

Create `src/qt_quick/qml/RadioGroup.qml`:

```qml
// SPDX-License-Identifier: Apache-2.0
import QtQuick
import QtQuick.Controls as C
import QtQuick.Layouts

// ui::Select, Radio. The buttons are not checkable themselves: the check
// follows `currentIndex`, which only the application moves; a click reports
// `chosen`.
Item {
    id: root
    property var labels: []
    property int currentIndex: -1
    signal chosen(int index)
    implicitWidth: column.implicitWidth
    implicitHeight: column.implicitHeight

    ColumnLayout {
        id: column
        anchors.fill: parent
        spacing: 0

        Repeater {
            model: root.labels
            delegate: C.RadioButton {
                required property int index
                required property string modelData
                text: modelData
                checkable: false
                checked: index === root.currentIndex
                onClicked: root.chosen(index)
            }
        }
    }
}
```

Create `src/qt_quick/qml/Menu.qml`:

```qml
// SPDX-License-Identifier: Apache-2.0
import QtQuick
import QtQuick.Controls as C
import QtQuick.Layouts

// ui::Menu: one delegate per item, activated by index.
Item {
    id: root
    property var labels: []
    signal itemActivated(int index)
    implicitWidth: column.implicitWidth
    implicitHeight: column.implicitHeight

    ColumnLayout {
        id: column
        anchors.fill: parent
        spacing: 0

        Repeater {
            model: root.labels
            delegate: C.ItemDelegate {
                required property int index
                required property string modelData
                text: modelData
                Layout.fillWidth: true
                onClicked: root.itemActivated(index)
            }
        }
    }
}
```

Add the seven files to `_morph_qt_quick_qml_files` (alphabetically) and `widgets_input.hpp` to `morph_qt_quick`.

- [ ] **Step 4: Run the tests to verify they pass**

Run: `cmake --build build/qt --target morph_qt_quick_tests && ./build/qt/tests/qt_quick/morph_qt_quick_tests "[input]"`
Expected: PASS.

Mutation checks (one at a time, then restore):
- In `TextInput.qml`, `onTextEdited` → `onTextChanged` → expected FAIL in "setText with the current text keeps the
  cursor and emits nothing" (`changes == 1`).
- In `TextArea.qml`, delete `applying = true` → expected FAIL in "Multiline is a TextArea that submits on
  Ctrl+Enter" (`changes` not empty after `setText`).
- In `Choice::setOptions`, delete `applySelection();` → expected FAIL in "replacing the options re-applies the
  selected key".

- [ ] **Step 5: Commit**

```bash
git add src/qt_quick tests/qt_quick
git commit -m "wip(qtquick): Button, TextInput, Checkbox, Select and Menu

Signed-off-by: Christian Parpart <christian@parpart.family>"
```

---
### Task 5: Structure — Panel, Scroll, Tabs, Dialog, Busy

**Files:**
- Create: `src/qt_quick/qml/Panel.qml`, `Scroll.qml`, `Tabs.qml`, `Dialog.qml`, `Busy.qml`
- Create: `src/qt_quick/widgets_structure.hpp`
- Modify: `src/qt_quick/context.hpp` — `Panel, Scroll, Tabs, Dialog, Busy` before `Count`, names appended to
  `kTypeNames`
- Modify: `src/qt_quick/CMakeLists.txt` — the five QML files; `widgets_structure.hpp`
- Modify: `tests/qt_quick/CMakeLists.txt` — `test_structure.cpp`
- Test: `tests/qt_quick/test_structure.cpp`

**Interfaces:**
- Consumes: Task 2's `ItemContainer`, `ItemWidget`; `ui::PanelWidget`, `ui::ScrollWidget`, `ui::TabsWidget`,
  `ui::DialogWidget`, `ui::BusyWidget` (Part 2).
- Produces (`detail`): `PanelBox(Context&, ui::ContainerWidget*)`, `ScrollArea(Context&, ui::ContainerWidget*,
  ui::Axis)`, `TabBox(Context&, ui::ContainerWidget*)`, `DialogBox(Context&, ui::ContainerWidget*)`,
  `BusyBox(Context&, ui::ContainerWidget*)`.
- QML contract: `Panel`: `title`, `padding`, `collapsible`, `collapsed`, `collapseToggled(bool)`, item
  `collapseButton`; `Scroll`: `horizontal` (initial); `Tabs`: `labels`, `currentIndex`, `tabChosen(int)`; `Dialog`:
  `open`, `permitted`, `title`, `dismissed()`, the popup object `popup`; `Busy`: `active`, `label`.

**Design notes.** These kinds are controlled: a click reports what the user asked for (`collapseToggled`,
`tabChosen`, `dismissed`) and the widget changes only when the application writes the state back. The Tabs content
is a plain `ColumnLayout`, not a `StackLayout`: `TabsWidget` gives the backend no mapping from a child to a tab, so
the mount decides which page is mounted or visible, and the layout shows what is there. A Dialog's root item is
never shown (it holds a place in its parent and nothing else); its popup lives on the window overlay, opens while
`open && permitted`, and reports `dismissed` only when it closes while the application still has it open — closing
it by code is not a dismissal. `setVisible` on a Dialog therefore writes `permitted` instead of the root's
visibility.

- [ ] **Step 1: Write the failing test**

Create `tests/qt_quick/test_structure.cpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0

#include <QObject>
#include <QQuickItem>
#include <QString>
#include <QTest>
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <morph/ui/view.hpp>
#include <vector>

#include "scene.hpp"
#include "widgets.hpp"
#include "widgets_structure.hpp"

namespace {

using morph::qt_quick::detail::BusyBox;
using morph::qt_quick::detail::DialogBox;
using morph::qt_quick::detail::PanelBox;
using morph::qt_quick::detail::ScrollArea;
using morph::qt_quick::detail::TabBox;
using morph::qt_quick::detail::TextLabel;
using morph::qt_quick::testing::centre;
using morph::qt_quick::testing::childrenOfType;
using morph::qt_quick::testing::contentOf;
using morph::qt_quick::testing::findItem;
using morph::qt_quick::testing::itemOf;
using morph::qt_quick::testing::Scene;
using morph::qt_quick::testing::unitWidth;
namespace ui = morph::ui;

}  // namespace

TEST_CASE("qt_quick Panel: title, padding in units, and a collapse the application decides",
          "[qt_quick][structure]") {
    Scene scene;
    PanelBox panel{scene.context, nullptr};
    TextLabel body{scene.context, &panel};
    std::vector<bool> toggles;
    panel.setOnToggle([&toggles](bool collapsed) { toggles.push_back(collapsed); });
    panel.setTitle("Laps");
    panel.setPadding(2);
    panel.setCollapsible(true);
    scene.show();

    QQuickItem const& item = itemOf(panel);
    CHECK(item.inherits("QQuickGroupBox"));
    CHECK(item.property("title").toString() == QStringLiteral("Laps"));
    CHECK(item.property("padding").toDouble() == Catch::Approx(2 * unitWidth()));
    CHECK(itemOf(body).parentItem() == &contentOf(item));

    QQuickItem* const toggle = findItem(item, "collapseButton");
    REQUIRE(toggle != nullptr);
    CHECK(toggle->isVisible());
    QTest::mouseClick(scene.window.get(), Qt::LeftButton, Qt::NoModifier, centre(*toggle));
    CHECK(toggles == std::vector{true});
    CHECK(contentOf(item).isVisible());

    panel.setCollapsed(true);
    CHECK_FALSE(contentOf(item).isVisible());
    CHECK(toggles.size() == 1);
    panel.setCollapsible(false);
    CHECK_FALSE(toggle->isVisible());
}

TEST_CASE("qt_quick Scroll: the child goes into the content; a vertical scroll's content follows its width",
          "[qt_quick][structure]") {
    Scene scene;
    ScrollArea vertical{scene.context, nullptr, ui::Axis::Vertical};
    vertical.setLayout({.width = ui::Sizing::stretch(), .height = ui::Sizing::fixed(5)});
    TextLabel child{scene.context, &vertical};
    child.setText("scrolled");
    ScrollArea horizontal{scene.context, nullptr, ui::Axis::Horizontal};
    scene.show();

    QQuickItem const& item = itemOf(vertical);
    CHECK(item.inherits("QQuickScrollView"));
    CHECK(itemOf(child).parentItem() == &contentOf(item));
    CHECK_FALSE(item.property("horizontal").toBool());
    CHECK(item.property("contentWidth").toDouble() == Catch::Approx(item.property("availableWidth").toDouble()));
    CHECK(itemOf(horizontal).property("horizontal").toBool());
}

TEST_CASE("qt_quick Tabs: labels, the selected tab, and a click reported by index", "[qt_quick][structure]") {
    Scene scene;
    TabBox tabs{scene.context, nullptr};
    TextLabel page{scene.context, &tabs};
    std::vector<std::size_t> selected;
    tabs.setOnSelect([&selected](std::size_t index) { selected.push_back(index); });
    tabs.setTabs({"Dashboard", "Statistics"});
    tabs.setSelected(1);
    scene.show();

    QQuickItem const& item = itemOf(tabs);
    CHECK(itemOf(page).parentItem() == &contentOf(item));
    std::vector<QQuickItem*> const bars = childrenOfType(item, "QQuickTabBar");
    REQUIRE(bars.size() == 1);
    CHECK(bars.front()->property("currentIndex").toInt() == 1);
    std::vector<QQuickItem*> const buttons = childrenOfType(item, "QQuickTabButton");
    REQUIRE(buttons.size() == 2);
    CHECK(buttons.at(0)->property("text").toString() == QStringLiteral("Dashboard"));

    QTest::mouseClick(scene.window.get(), Qt::LeftButton, Qt::NoModifier, centre(*buttons.at(0)));
    CHECK(selected == std::vector<std::size_t>{0});

    tabs.setTabs({"A", "B", "C"});
    tabs.setSelected(2);
    scene.settle();
    CHECK(bars.front()->property("currentIndex").toInt() == 2);
}

TEST_CASE("qt_quick Dialog: closed by code reports no dismissal; Escape reports one", "[qt_quick][structure]") {
    Scene scene;
    DialogBox dialog{scene.context, nullptr};
    TextLabel body{scene.context, &dialog};
    body.setText("Quit?");
    int dismissals = 0;
    dialog.setOnDismiss([&dismissals] { ++dismissals; });
    dialog.setTitle("Confirm");
    scene.show();

    QObject const* const popup = itemOf(dialog).findChild<QObject*>(QStringLiteral("popup"));
    REQUIRE(popup != nullptr);
    CHECK(itemOf(body).parentItem() == &contentOf(itemOf(dialog)));
    CHECK_FALSE(itemOf(dialog).isVisible());

    dialog.setOpen(true);
    CHECK(QTest::qWaitFor([popup] { return popup->property("opened").toBool(); }));
    CHECK(popup->property("title").toString() == QStringLiteral("Confirm"));
    dialog.setOpen(false);
    CHECK(QTest::qWaitFor([popup] { return !popup->property("visible").toBool(); }));
    CHECK(dismissals == 0);

    dialog.setOpen(true);
    CHECK(QTest::qWaitFor([popup] { return popup->property("opened").toBool(); }));
    dialog.setVisible(false);
    CHECK(QTest::qWaitFor([popup] { return !popup->property("visible").toBool(); }));
    CHECK(dismissals == 0);
    CHECK_FALSE(morph::qt_quick::detail::holderOf(dialog)->visibleFlag());

    dialog.setVisible(true);
    CHECK(QTest::qWaitFor([popup] { return popup->property("opened").toBool(); }));
    QTest::keyClick(scene.window.get(), Qt::Key_Escape);
    CHECK(QTest::qWaitFor([popup] { return !popup->property("visible").toBool(); }));
    CHECK(dismissals == 1);
}

TEST_CASE("qt_quick Busy: the indicator runs while active, with its label", "[qt_quick][structure]") {
    Scene scene;
    BusyBox busy{scene.context, nullptr};
    busy.setLabel("Saving");
    scene.show();
    std::vector<QQuickItem*> const indicators = childrenOfType(itemOf(busy), "QQuickBusyIndicator");
    REQUIRE(indicators.size() == 1);
    CHECK_FALSE(indicators.front()->property("running").toBool());
    busy.setActive(true);
    CHECK(indicators.front()->property("running").toBool());
    CHECK(itemOf(busy).property("label").toString() == QStringLiteral("Saving"));
}
```

- [ ] **Step 2: Run it to verify it fails**

Add `test_structure.cpp` to `morph_qt_quick_tests` and build.
Expected: FAIL — `'widgets_structure.hpp' file not found`.

- [ ] **Step 3: Implement**

In `src/qt_quick/context.hpp`, insert `Panel, Scroll, Tabs, Dialog, Busy,` before `Count,` and append `"Panel",
"Scroll", "Tabs", "Dialog", "Busy",` to `kTypeNames`.

Create `src/qt_quick/widgets_structure.hpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0

#pragma once
#include <QString>
#include <QStringList>
#include <QVariant>
#include <QVariantMap>
#include <cstddef>
#include <functional>
#include <morph/ui/backend.hpp>
#include <morph/ui/view.hpp>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "context.hpp"
#include "item_holder.hpp"
#include "strings.hpp"

/// @file
/// @brief Wrappers for the kinds that frame, scroll, switch between and overlay other widgets.

namespace morph::qt_quick::detail {

/// @brief `ui::Panel` as a `MorphUi.Panel` (a GroupBox, optionally collapsible).
class PanelBox final : public ItemContainer<ui::PanelWidget> {
public:
    /// @param context The backend's context.
    /// @param parent The container, or null for the root.
    PanelBox(Context& context, ui::ContainerWidget* parent) : ItemContainer{context, Kind::Panel, parent} {
        relay("collapseToggled(bool)", [this](QVariant const& collapsed) {
            if (auto const onToggle = _onToggle) {
                onToggle(collapsed.toBool());
            }
        });
    }

    void setTitle(std::string_view title) override { write("title", toQString(title)); }
    void setPadding(int padding) override { write("padding", static_cast<double>(padding) * context().unitWidth()); }
    void setCollapsible(bool collapsible) override { write("collapsible", collapsible); }
    void setCollapsed(bool collapsed) override { write("collapsed", collapsed); }
    void setOnToggle(std::function<void(bool)> onToggle) override { _onToggle = std::move(onToggle); }

private:
    std::function<void(bool)> _onToggle;
};

/// @brief `ui::Scroll` as a `MorphUi.Scroll` (a ScrollView over one axis).
class ScrollArea final : public ItemContainer<ui::ScrollWidget> {
public:
    /// @param context The backend's context.
    /// @param parent The container, or null for the root.
    /// @param axis The axis that scrolls; fixed for the widget's life.
    ScrollArea(Context& context, ui::ContainerWidget* parent, ui::Axis axis)
        : ItemContainer{context, Kind::Scroll, parent,
                        QVariantMap{{QStringLiteral("horizontal"), axis == ui::Axis::Horizontal}}} {}
};

/// @brief `ui::Tabs` as a `MorphUi.Tabs` (a TabBar over a content layout).
class TabBox final : public ItemContainer<ui::TabsWidget> {
public:
    /// @param context The backend's context.
    /// @param parent The container, or null for the root.
    TabBox(Context& context, ui::ContainerWidget* parent) : ItemContainer{context, Kind::Tabs, parent} {
        relay("tabChosen(int)", [this](QVariant const& index) {
            if (index.toInt() < 0) {
                return;
            }
            if (auto const onSelect = _onSelect) {
                onSelect(static_cast<std::size_t>(index.toInt()));
            }
        });
    }

    void setTabs(std::vector<std::string> const& labels) override {
        QStringList names;
        names.reserve(static_cast<qsizetype>(labels.size()));
        for (std::string const& label : labels) {
            names.push_back(toQString(label));
        }
        write("labels", names);
    }

    void setSelected(std::size_t index) override { write("currentIndex", static_cast<int>(index)); }
    void setOnSelect(std::function<void(std::size_t)> onSelect) override { _onSelect = std::move(onSelect); }

private:
    std::function<void(std::size_t)> _onSelect;
};

/// @brief `ui::Dialog` as a `MorphUi.Dialog` (a modal popup on the window overlay).
class DialogBox final : public ItemContainer<ui::DialogWidget> {
public:
    /// @param context The backend's context.
    /// @param parent The container, or null for the root.
    DialogBox(Context& context, ui::ContainerWidget* parent) : ItemContainer{context, Kind::Dialog, parent} {
        relay("dismissed()", [this](QVariant const&) {
            if (ui::Action const onDismiss = _onDismiss) {
                onDismiss();
            }
        });
    }

    /// @brief A hidden dialog does not show even while open; the root item itself is never shown.
    /// @param visible Whether the dialog may show.
    void setVisible(bool visible) override {
        rememberVisible(visible);
        write("permitted", visible);
    }

    void setOpen(bool open) override { write("open", open); }
    void setTitle(std::string_view title) override { write("title", toQString(title)); }
    void setOnDismiss(ui::Action onDismiss) override { _onDismiss = std::move(onDismiss); }

private:
    ui::Action _onDismiss;
};

/// @brief `ui::Busy` as a `MorphUi.Busy` (a BusyIndicator and a label).
class BusyBox final : public ItemWidget<ui::BusyWidget> {
public:
    /// @param context The backend's context.
    /// @param parent The container, or null for the root.
    BusyBox(Context& context, ui::ContainerWidget* parent) : ItemWidget{context, Kind::Busy, parent} {}

    void setActive(bool active) override { write("active", active); }
    void setLabel(std::string_view label) override { write("label", toQString(label)); }
};

}  // namespace morph::qt_quick::detail
```

Create `src/qt_quick/qml/Panel.qml`:

```qml
// SPDX-License-Identifier: Apache-2.0
import QtQuick
import QtQuick.Controls as C
import QtQuick.Layouts

// ui::Panel. Children go into `content`. A collapsible panel shows a toggle in
// its title row; a click reports the state asked for, and the panel changes
// only when `collapsed` is written.
C.GroupBox {
    id: root
    property bool collapsible: false
    property bool collapsed: false
    readonly property Item content: body
    signal collapseToggled(bool collapsed)

    contentWidth: body.implicitWidth
    contentHeight: collapsed ? 0 : body.implicitHeight

    label: RowLayout {
        x: root.leftPadding
        width: root.availableWidth
        spacing: 4

        C.ToolButton {
            objectName: "collapseButton"
            visible: root.collapsible
            text: root.collapsed ? "▸" : "▾"
            onClicked: root.collapseToggled(!root.collapsed)
        }
        C.Label {
            text: root.title
            font.bold: true
            elide: Text.ElideRight
            Layout.fillWidth: true
        }
    }

    ColumnLayout {
        id: body
        anchors.fill: parent
        visible: !root.collapsed
        spacing: 0
    }
}
```

Create `src/qt_quick/qml/Scroll.qml`:

```qml
// SPDX-License-Identifier: Apache-2.0
import QtQuick
import QtQuick.Controls as C
import QtQuick.Layouts

// ui::Scroll. The child goes into `content`. A vertical scroll's content is as
// wide as the view and scrolls down; a horizontal one is as tall and scrolls
// across.
C.ScrollView {
    id: root
    property bool horizontal: false
    readonly property Item content: body

    contentWidth: horizontal ? body.implicitWidth : availableWidth
    contentHeight: horizontal ? availableHeight : body.implicitHeight
    C.ScrollBar.horizontal.policy: horizontal ? C.ScrollBar.AsNeeded : C.ScrollBar.AlwaysOff
    C.ScrollBar.vertical.policy: horizontal ? C.ScrollBar.AlwaysOff : C.ScrollBar.AsNeeded

    ColumnLayout {
        id: body
        width: root.contentWidth
        height: root.contentHeight
        spacing: 0
    }
}
```

Create `src/qt_quick/qml/Tabs.qml`:

```qml
// SPDX-License-Identifier: Apache-2.0
import QtQuick
import QtQuick.Controls as C
import QtQuick.Layouts

// ui::Tabs. Pages go into `content`, a plain layout: which page is mounted or
// visible is the mount's decision. A click reports `tabChosen`; the bar follows
// `currentIndex`, which only the application writes.
Item {
    id: root
    property var labels: []
    property int currentIndex: 0
    readonly property Item content: body
    signal tabChosen(int index)

    implicitWidth: Math.max(bar.implicitWidth, body.implicitWidth)
    implicitHeight: bar.implicitHeight + body.implicitHeight

    function syncBar() {
        bar.currentIndex = root.currentIndex
    }
    onCurrentIndexChanged: syncBar()
    // The bar's buttons are rebuilt when the labels change; select once they exist.
    onLabelsChanged: Qt.callLater(syncBar)
    Component.onCompleted: syncBar()

    C.TabBar {
        id: bar
        width: root.width

        Repeater {
            model: root.labels
            delegate: C.TabButton {
                required property int index
                required property string modelData
                text: modelData
                width: implicitWidth
                onClicked: root.tabChosen(index)
            }
        }
    }

    ColumnLayout {
        id: body
        y: bar.height
        width: root.width
        height: Math.max(0, root.height - bar.height)
        spacing: 0
    }
}
```

Create `src/qt_quick/qml/Dialog.qml`:

```qml
// SPDX-License-Identifier: Apache-2.0
import QtQuick
import QtQuick.Controls as C
import QtQuick.Layouts

// ui::Dialog. The root holds the dialog's place and is never shown; the popup
// lives on the window overlay and is open while `open && permitted`. Children
// go into `content`. `dismissed` is reported only when the popup closes while
// the application still has it open (Escape); closing by code is not one.
Item {
    id: root
    property bool open: false
    property bool permitted: true
    property string title: ""
    readonly property Item content: body
    readonly property Item overlayLayer: C.Overlay.overlay
    signal dismissed()

    visible: false
    implicitWidth: 0
    implicitHeight: 0

    function sync() {
        if (root.open && root.permitted && root.overlayLayer !== null)
            popup.open()
        else
            popup.close()
    }
    onOpenChanged: sync()
    onPermittedChanged: sync()
    onOverlayLayerChanged: sync()

    C.Dialog {
        id: popup
        objectName: "popup"
        parent: root.overlayLayer
        anchors.centerIn: parent
        modal: true
        focus: true
        title: root.title
        closePolicy: C.Popup.CloseOnEscape
        onClosed: if (root.open && root.permitted) root.dismissed()

        ColumnLayout {
            id: body
            anchors.fill: parent
            spacing: 0
        }
    }
}
```

Create `src/qt_quick/qml/Busy.qml`:

```qml
// SPDX-License-Identifier: Apache-2.0
import QtQuick
import QtQuick.Controls as C
import QtQuick.Layouts

// ui::Busy: an indicator that runs while `active`, beside its label.
Item {
    id: root
    property bool active: false
    property string label: ""
    implicitWidth: row.implicitWidth
    implicitHeight: row.implicitHeight

    RowLayout {
        id: row
        anchors.fill: parent

        C.BusyIndicator {
            running: root.active
            visible: root.active
        }
        C.Label {
            text: root.label
            visible: root.active && root.label.length > 0
        }
    }
}
```

Add the five files to `_morph_qt_quick_qml_files` and `widgets_structure.hpp` to `morph_qt_quick`.

- [ ] **Step 4: Run the tests to verify they pass**

Run: `cmake --build build/qt --target morph_qt_quick_tests && ./build/qt/tests/qt_quick/morph_qt_quick_tests "[structure]"`
Expected: PASS.

Mutation checks (one at a time, then restore):
- In `Dialog.qml`, change `onClosed: if (root.open && root.permitted) root.dismissed()` to `onClosed:
  root.dismissed()` → expected FAIL in "closed by code reports no dismissal; Escape reports one" (`dismissals == 1`
  after the first `setOpen(false)`).
- In `DialogBox`, delete the `setVisible` override → expected FAIL in the same case (the popup stays open after
  `setVisible(false)`).
- In `Tabs.qml`, delete `onLabelsChanged: Qt.callLater(syncBar)` and `onCurrentIndexChanged: syncBar()` → expected
  FAIL in "labels, the selected tab, and a click reported by index".

- [ ] **Step 5: Commit**

```bash
git add src/qt_quick tests/qt_quick
git commit -m "wip(qtquick): Panel, Scroll, Tabs, Dialog and Busy

Signed-off-by: Christian Parpart <christian@parpart.family>"
```

---
### Task 6: Table

**Files:**
- Create: `src/qt_quick/qml/Table.qml`, `src/qt_quick/qml/TableRowDecoration.qml`
- Create: `src/qt_quick/widgets_table.hpp`, `src/qt_quick/widgets_table.cpp`
- Modify: `src/qt_quick/context.hpp` — `Table, TableRowDecoration` before `Count`, names appended
- Modify: `src/qt_quick/CMakeLists.txt` — the two QML files; `widgets_table.cpp widgets_table.hpp`
- Modify: `tests/qt_quick/CMakeLists.txt` — `test_table.cpp`
- Test: `tests/qt_quick/test_table.cpp`

**Interfaces:**
- Consumes: Task 2's `ItemContainer`, `holderOf`, `ItemHolder::setDefaultWidth`, `Context::decorate`,
  `SignalRelay::listen`; `ui::TableWidget`, `ui::TableColumn`, `ui::SelectionMode` (Part 2).
- Produces (`detail`): `TableBox(Context&, ui::ContainerWidget*)`. QML contract: `Table`: `headers`,
  `headerWidths`, `headerStretches`, `content`, `highlightLayer`; `TableRowDecoration` (`rowDecoration`): `target`,
  `selected`, `tapped()`, `doubleTapped()`.

**Design notes.** Each row is the `Row` the mount creates inside the table, so rows and their cells are ordinary
widgets. `setRowKey` adds one `TableRowDecoration` per row to a layer *behind* the rows: it follows the row's
geometry, draws the selection, and its `TapHandler` receives every tap a cell does not take itself (a label passes
it on, a button keeps it). A tap is reported as the selection it asks for — Single: that key; Multiple: the
selection with that key toggled; None: nothing — and the highlight changes only through `setSelection`. A double tap
activates. Column widths reach cells through `setDefaultWidth`: a cell whose own width is Content takes its
column's Fixed or Stretch width, and the row itself stretches across the table. The mount keys a row after its cells
exist (`ui::Mounted` builds the row, then calls `setRowKey`), and the table also re-applies the widths whenever a
row's cells change, so a cell added after the key lines up too. A Content column leaves each cell its
content width, so only Fixed and Stretch columns line up across rows.

- [ ] **Step 1: Write the failing test**

Create `tests/qt_quick/test_table.cpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0

#include <QCoreApplication>
#include <QEvent>
#include <QObject>
#include <QQuickItem>
#include <QString>
#include <QStringList>
#include <QTest>
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <memory>
#include <morph/ui/view.hpp>
#include <string>
#include <utility>
#include <vector>

#include "scene.hpp"
#include "widgets.hpp"
#include "widgets_table.hpp"

namespace {

using morph::qt_quick::detail::Stack;
using morph::qt_quick::detail::TableBox;
using morph::qt_quick::detail::TextLabel;
using morph::qt_quick::testing::centre;
using morph::qt_quick::testing::itemOf;
using morph::qt_quick::testing::Scene;
using morph::qt_quick::testing::unitWidth;
namespace ui = morph::ui;

constexpr std::int64_t kBeyondDouble = (std::int64_t{1} << 53) + 1;

/// A table with a Fixed(8) and a Stretch column, recording what it reports.
struct TableScene {
    Scene scene;
    TableBox table{scene.context, nullptr};
    std::vector<std::unique_ptr<Stack>> rows;
    std::vector<std::unique_ptr<TextLabel>> cells;
    std::vector<std::vector<ui::Key>> selections;
    std::vector<ui::Key> activations;

    TableScene() {
        table.setLayout({.width = ui::Sizing::stretch(), .height = ui::Sizing::stretch()});
        table.setColumns({ui::TableColumn{.label = "Name", .width = ui::Sizing::fixed(8)},
                          ui::TableColumn{.label = "Total", .width = ui::Sizing::stretch()}});
        table.setOnSelectionChange([this](std::vector<ui::Key> keys) { selections.push_back(std::move(keys)); });
        table.setOnActivate([this](ui::Key key) { activations.push_back(std::move(key)); });
    }

    /// A row of two cells. The mount keys a row once its cells exist; here the second cell arrives after the key, so
    /// the re-apply on a changed row is exercised as well.
    Stack& addRow(ui::Key key, char const* name, char const* total) {
        rows.push_back(std::make_unique<Stack>(scene.context, &table, ui::Axis::Horizontal));
        Stack& row = *rows.back();
        cells.push_back(std::make_unique<TextLabel>(scene.context, &row));
        cells.back()->setText(name);
        table.setRowKey(row, std::move(key));
        cells.push_back(std::make_unique<TextLabel>(scene.context, &row));
        cells.back()->setText(total);
        return row;
    }

    void click(Stack const& row) {
        QTest::mouseClick(scene.window.get(), Qt::LeftButton, Qt::NoModifier, centre(itemOf(row)));
    }

    [[nodiscard]] std::vector<QQuickItem*> decorations() const {
        return itemOf(table).findChildren<QQuickItem*>(QStringLiteral("rowDecoration"));
    }
};

}  // namespace

TEST_CASE("qt_quick Table: header labels; column widths reach cells sized by content", "[qt_quick][table]") {
    TableScene table;
    table.addRow(ui::Key{std::int64_t{1}}, "alpha", "10");
    Stack const& own = table.addRow(ui::Key{std::int64_t{2}}, "beta", "20");
    table.cells.at(2)->setLayout({.width = ui::Sizing::fixed(3), .height = ui::Sizing::content()});
    table.scene.show();

    QQuickItem const& item = itemOf(table.table);
    CHECK(item.property("headers").toStringList() == QStringList{QStringLiteral("Name"), QStringLiteral("Total")});
    CHECK(itemOf(*table.cells.at(0)).width() == Catch::Approx(8 * unitWidth()).margin(0.5));
    CHECK(itemOf(*table.cells.at(1)).width() ==
          Catch::Approx(itemOf(*table.rows.at(0)).width() - 8 * unitWidth()).margin(1.0));
    CHECK(itemOf(*table.rows.at(0)).width() == Catch::Approx(item.width()).margin(1.0));
    // A cell's own sizing wins over its column's.
    CHECK(itemOf(*table.cells.at(2)).width() == Catch::Approx(3 * unitWidth()).margin(0.5));
    CHECK(itemOf(own).width() == Catch::Approx(item.width()).margin(1.0));
}

TEST_CASE("qt_quick Table: a tap reports the selection it asks for, per the selection mode", "[qt_quick][table]") {
    TableScene table;
    Stack const& first = table.addRow(ui::Key{kBeyondDouble}, "alpha", "1");
    Stack const& second = table.addRow(ui::Key{std::string{"b"}}, "beta", "2");
    table.scene.show();

    table.click(first);
    CHECK(table.selections.empty());

    table.table.setSelectionMode(ui::SelectionMode::Single);
    table.click(second);
    REQUIRE(table.selections.size() == 1);
    CHECK(table.selections.back() == std::vector{ui::Key{std::string{"b"}}});

    table.table.setSelectionMode(ui::SelectionMode::Multiple);
    table.table.setSelection({ui::Key{kBeyondDouble}});
    table.click(second);
    CHECK(table.selections.back() == std::vector{ui::Key{kBeyondDouble}, ui::Key{std::string{"b"}}});
    table.click(first);
    CHECK(table.selections.back().empty());
}

TEST_CASE("qt_quick Table: a double tap activates the row's key", "[qt_quick][table]") {
    TableScene table;
    table.addRow(ui::Key{std::int64_t{1}}, "alpha", "1");
    Stack const& second = table.addRow(ui::Key{kBeyondDouble}, "beta", "2");
    table.scene.show();
    QTest::mouseDClick(table.scene.window.get(), Qt::LeftButton, Qt::NoModifier, centre(itemOf(second)));
    REQUIRE(table.activations.size() == 1);
    CHECK(std::get<std::int64_t>(table.activations.front()) == kBeyondDouble);
}

TEST_CASE("qt_quick Table: setSelection highlights exactly the selected rows", "[qt_quick][table]") {
    TableScene table;
    Stack const& first = table.addRow(ui::Key{std::int64_t{1}}, "alpha", "1");
    Stack const& second = table.addRow(ui::Key{std::int64_t{2}}, "beta", "2");
    table.table.setSelection({ui::Key{std::int64_t{2}}});
    std::vector<QQuickItem*> const decorations = table.decorations();
    REQUIRE(decorations.size() == 2);
    for (QQuickItem const* const decoration : decorations) {
        auto const* const target = decoration->property("target").value<QQuickItem*>();
        if (target == &itemOf(first)) {
            CHECK_FALSE(decoration->property("selected").toBool());
        } else {
            CHECK(target == &itemOf(second));
            CHECK(decoration->property("selected").toBool());
        }
    }
}

TEST_CASE("qt_quick Table: a destroyed row takes its decoration with it", "[qt_quick][table]") {
    TableScene table;
    table.addRow(ui::Key{std::int64_t{1}}, "alpha", "1");
    table.addRow(ui::Key{std::int64_t{2}}, "beta", "2");
    REQUIRE(table.decorations().size() == 2);
    table.cells.erase(table.cells.begin() + 2, table.cells.end());
    table.rows.pop_back();
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    CHECK(table.decorations().size() == 1);
}
```

- [ ] **Step 2: Run it to verify it fails**

Add `test_table.cpp` to `morph_qt_quick_tests` and build.
Expected: FAIL — `'widgets_table.hpp' file not found`.

- [ ] **Step 3: Implement**

In `src/qt_quick/context.hpp`, insert `Table, TableRowDecoration,` before `Count,` and append `"Table",
"TableRowDecoration",` to `kTypeNames`.

Create `src/qt_quick/widgets_table.hpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0

#pragma once
#include <QMetaObject>
#include <QPointer>
#include <QQuickItem>
#include <functional>
#include <map>
#include <morph/ui/backend.hpp>
#include <morph/ui/view.hpp>
#include <vector>

#include "context.hpp"
#include "item_holder.hpp"

/// @file
/// @brief The `ui::Table` wrapper.

namespace morph::qt_quick::detail {

/// @brief `ui::Table` as a `MorphUi.Table`: a header row over keyed rows, each row backed by a decoration that draws
///        its selection and reports its taps.
class TableBox final : public ItemContainer<ui::TableWidget> {
public:
    /// @param context The backend's context.
    /// @param parent The container, or null for the root.
    TableBox(Context& context, ui::ContainerWidget* parent);
    /// @brief Disconnects from rows that may outlive the table's item.
    ~TableBox() override;
    TableBox(TableBox const&) = delete;
    TableBox& operator=(TableBox const&) = delete;
    TableBox(TableBox&&) = delete;
    TableBox& operator=(TableBox&&) = delete;

    void setColumns(std::vector<ui::TableColumn> const& columns) override;
    void setSelectionMode(ui::SelectionMode mode) override;
    void setRowKey(ui::Widget& row, ui::Key const& key) override;
    void setSelection(std::vector<ui::Key> const& keys) override;
    void setOnSelectionChange(std::function<void(std::vector<ui::Key>)> onSelectionChange) override;
    void setOnActivate(std::function<void(ui::Key)> onActivate) override;

private:
    struct RowEntry {
        ui::Key key;
        QPointer<QQuickItem> decoration;
    };

    [[nodiscard]] QQuickItem& highlightLayer() const;
    void applyColumns(QQuickItem const& row) const;
    void refreshHighlights() const;
    void tapped(QQuickItem const* row);
    void activated(QQuickItem const* row);
    void forget(QQuickItem const* row);

    std::vector<ui::TableColumn> _columns;
    ui::SelectionMode _mode = ui::SelectionMode::None;
    std::vector<ui::Key> _selection;
    std::map<QQuickItem const*, RowEntry> _rows;
    std::vector<QMetaObject::Connection> _connections;
    std::function<void(std::vector<ui::Key>)> _onSelectionChange;
    std::function<void(ui::Key)> _onActivate;
};

}  // namespace morph::qt_quick::detail
```

Create `src/qt_quick/widgets_table.cpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0

#include "widgets_table.hpp"

#include <QObject>
#include <QString>
#include <QStringList>
#include <QVariant>
#include <QVariantList>
#include <QVariantMap>
#include <algorithm>
#include <cstddef>
#include <optional>
#include <stdexcept>
#include <utility>

#include "relay.hpp"
#include "strings.hpp"

namespace morph::qt_quick::detail {

TableBox::TableBox(Context& context, ui::ContainerWidget* parent) : ItemContainer{context, Kind::Table, parent} {}

TableBox::~TableBox() {
    for (QMetaObject::Connection const& connection : _connections) {
        QObject::disconnect(connection);
    }
}

void TableBox::setColumns(std::vector<ui::TableColumn> const& columns) {
    _columns = columns;
    QStringList headers;
    QVariantList widths;
    QVariantList stretches;
    for (ui::TableColumn const& column : columns) {
        headers.push_back(toQString(column.label));
        switch (column.width.kind) {
            case ui::Sizing::Kind::Fixed:
                widths.push_back(static_cast<double>(column.width.amount) * context().unitWidth());
                stretches.push_back(-1);
                break;
            case ui::Sizing::Kind::Stretch:
                widths.push_back(0.0);
                stretches.push_back(std::max(column.width.amount, 1));
                break;
            case ui::Sizing::Kind::Content:
            default:
                widths.push_back(-1.0);
                stretches.push_back(-1);
                break;
        }
    }
    write("headers", headers);
    write("headerWidths", widths);
    write("headerStretches", stretches);
    for (auto const& [row, entry] : _rows) {
        applyColumns(*row);
    }
}

void TableBox::setSelectionMode(ui::SelectionMode mode) { _mode = mode; }

void TableBox::setRowKey(ui::Widget& row, ui::Key const& key) {
    ItemHolder* const holder = holderOf(row);
    if (holder == nullptr || holder->item() == nullptr) {
        throw std::invalid_argument{"qt_quick: setRowKey of a widget this backend did not make"};
    }
    QQuickItem* const rowItem = holder->item();
    auto const [entry, inserted] = _rows.try_emplace(rowItem, RowEntry{.key = key, .decoration = {}});
    entry->second.key = key;
    if (inserted) {
        QQuickItem* const decoration =
            context().decorate(Kind::TableRowDecoration, highlightLayer(),
                               QVariantMap{{QStringLiteral("target"), QVariant::fromValue(rowItem)}});
        entry->second.decoration = decoration;
        static_cast<void>(
            SignalRelay::listen(*decoration, "tapped()", [this, rowItem](QVariant const&) { tapped(rowItem); }));
        static_cast<void>(SignalRelay::listen(*decoration, "doubleTapped()",
                                              [this, rowItem](QVariant const&) { activated(rowItem); }));
        // The decoration is each connection's context: it dies with the table's item, and with its row.
        _connections.push_back(
            QObject::connect(rowItem, &QObject::destroyed, decoration, [this, rowItem] { forget(rowItem); }));
        if (auto* const cells = rowItem->property("content").value<QQuickItem*>()) {
            _connections.push_back(QObject::connect(cells, &QQuickItem::childrenChanged, decoration,
                                                    [this, rowItem] { applyColumns(*rowItem); }));
        }
        // A row stretches across the table unless it was given a width of its own.
        holder->setDefaultWidth(ui::Sizing::stretch());
    }
    applyColumns(*rowItem);
    refreshHighlights();
}

void TableBox::setSelection(std::vector<ui::Key> const& keys) {
    _selection = keys;
    refreshHighlights();
}

void TableBox::setOnSelectionChange(std::function<void(std::vector<ui::Key>)> onSelectionChange) {
    _onSelectionChange = std::move(onSelectionChange);
}

void TableBox::setOnActivate(std::function<void(ui::Key)> onActivate) { _onActivate = std::move(onActivate); }

QQuickItem& TableBox::highlightLayer() const {
    auto* const layer = item()->property("highlightLayer").value<QQuickItem*>();
    if (layer == nullptr) {
        throw std::logic_error{"qt_quick: MorphUi.Table has no highlightLayer"};
    }
    return *layer;
}

void TableBox::applyColumns(QQuickItem const& row) const {
    auto const* const cells = row.property("content").value<QQuickItem*>();
    if (cells == nullptr) {
        return;
    }
    std::size_t column = 0;
    for (QQuickItem const* const cell : cells->childItems()) {
        ItemHolder* const holder = holderOf(*cell);
        if (holder == nullptr) {
            continue;
        }
        holder->setDefaultWidth(column < _columns.size() ? std::optional{_columns.at(column).width} : std::nullopt);
        ++column;
    }
}

void TableBox::refreshHighlights() const {
    for (auto const& [row, entry] : _rows) {
        if (!entry.decoration.isNull()) {
            entry.decoration->setProperty("selected", std::ranges::find(_selection, entry.key) != _selection.end());
        }
    }
}

void TableBox::tapped(QQuickItem const* row) {
    auto const found = _rows.find(row);
    if (found == _rows.end() || _mode == ui::SelectionMode::None) {
        return;
    }
    ui::Key const key = found->second.key;
    std::vector<ui::Key> next;
    if (_mode == ui::SelectionMode::Single) {
        next.push_back(key);
    } else {
        next = _selection;
        if (auto const existing = std::ranges::find(next, key); existing != next.end()) {
            next.erase(existing);
        } else {
            next.push_back(key);
        }
    }
    if (auto const onSelectionChange = _onSelectionChange) {
        onSelectionChange(std::move(next));
    }
}

void TableBox::activated(QQuickItem const* row) {
    auto const found = _rows.find(row);
    if (found == _rows.end()) {
        return;
    }
    ui::Key const key = found->second.key;
    if (auto const onActivate = _onActivate) {
        onActivate(key);
    }
}

void TableBox::forget(QQuickItem const* row) {
    auto const found = _rows.find(row);
    if (found == _rows.end()) {
        return;
    }
    if (!found->second.decoration.isNull()) {
        found->second.decoration->deleteLater();
    }
    _rows.erase(found);
}

}  // namespace morph::qt_quick::detail
```

Create `src/qt_quick/qml/Table.qml`:

```qml
// SPDX-License-Identifier: Apache-2.0
import QtQuick
import QtQuick.Controls as C
import QtQuick.Layouts

// ui::Table. Rows go into `content`; their decorations go into
// `highlightLayer`, which lies behind the rows and has the same geometry, so a
// decoration can follow its row and take the taps the row's cells pass on.
// headerWidths: per column, a preferred width in pixels (-1: the label's own,
// 0 with a headerStretches weight: a share of the free space).
Item {
    id: root
    property var headers: []
    property var headerWidths: []
    property var headerStretches: []
    readonly property Item content: rows
    readonly property Item highlightLayer: highlights

    implicitWidth: Math.max(header.implicitWidth, rows.implicitWidth)
    implicitHeight: header.implicitHeight + rows.implicitHeight

    RowLayout {
        id: header
        width: root.width
        spacing: 0

        Repeater {
            model: root.headers
            delegate: C.Label {
                required property int index
                required property string modelData
                text: modelData
                font.bold: true
                Layout.preferredWidth: root.headerWidths[index] ?? -1
                Layout.fillWidth: (root.headerStretches[index] ?? -1) > 0
                Layout.horizontalStretchFactor: root.headerStretches[index] ?? -1
            }
        }
    }

    Item {
        id: bodyArea
        y: header.implicitHeight
        width: root.width
        height: rows.implicitHeight

        Item {
            id: highlights
            anchors.fill: parent
        }
        ColumnLayout {
            id: rows
            anchors.fill: parent
            spacing: 0
        }
    }
}
```

Create `src/qt_quick/qml/TableRowDecoration.qml`:

```qml
// SPDX-License-Identifier: Apache-2.0
import QtQuick

// Behind one table row: covers it, shows whether it is selected, and reports
// the taps its cells do not take themselves.
Rectangle {
    id: root
    objectName: "rowDecoration"
    property Item target: null
    property bool selected: false
    signal tapped()
    signal doubleTapped()

    x: 0
    y: target !== null ? target.y : 0
    width: parent !== null ? parent.width : 0
    height: target !== null ? target.height : 0
    visible: target !== null && target.visible
    color: selected ? "#331e88e5" : "transparent"

    TapHandler {
        onTapped: root.tapped()
        onDoubleTapped: root.doubleTapped()
    }
}
```

Add the two QML files to `_morph_qt_quick_qml_files` and `widgets_table.cpp widgets_table.hpp` to `morph_qt_quick`.

- [ ] **Step 4: Run the tests to verify they pass**

Run: `cmake --build build/qt --target morph_qt_quick_tests && ./build/qt/tests/qt_quick/morph_qt_quick_tests "[table]"`
Expected: PASS.

Mutation checks (one at a time, then restore):
- Delete the `childrenChanged` connection in `setRowKey` → expected FAIL in "column widths reach cells sized by
  content" (`cells.at(1)`, added after the key, keeps its content width).
- In `TableBox::tapped`, append the key in Multiple mode without looking for it first → expected FAIL in "a tap
  reports the selection it asks for" (the click on `first` reports three keys instead of none).
- In `ItemHolder::attach` (Task 2), move `setParentItem`/`setParent` above the two `setProperty` calls → expected
  FAIL in "column widths reach cells sized by content": the `childrenChanged` for `cells.at(1)` fires before its
  item maps back to a wrapper.

- [ ] **Step 5: Commit**

```bash
git add src/qt_quick tests/qt_quick
git commit -m "wip(qtquick): Table with keyed rows, selection and column widths

Signed-off-by: Christian Parpart <christian@parpart.family>"
```

---
### Task 7: Values — DateTimeInput, Slider, FilePicker

**Files:**
- Create: `src/qt_quick/qml/DateTimeInput.qml`, `Slider.qml`, `FilePicker.qml`
- Create: `src/qt_quick/values.hpp`, `values.cpp`, `widgets_value.hpp`, `widgets_value.cpp`
- Modify: `src/qt_quick/context.hpp` — `DateTimeInput, Slider, FilePicker` before `Count`, names appended
- Modify: `src/qt_quick/CMakeLists.txt` — the three QML files; `values.cpp values.hpp widgets_value.cpp
  widgets_value.hpp`
- Modify: `tests/qt_quick/CMakeLists.txt` — `test_values.cpp`
- Test: `tests/qt_quick/test_values.cpp`

**Interfaces:**
- Consumes: Task 2's `ItemWidget`; `ui::DateTimeInputWidget`, `ui::SliderWidget`, `ui::FilePickerWidget`,
  `ui::DateMode`, `ui::FilePickerMode` (Part 2); `morph::time::DateTime::fromIso8601`, `morph::time::Timestamp`
  (`include/morph/util/datetime.hpp:148`, `:376`).
- Produces (`detail`): `ParsedDateTime{valid, value}`, `formatDateTime(std::optional<time::Timestamp> const&,
  ui::DateMode, int offsetMinutes) -> std::string`, `parseDateTime(std::string_view, ui::DateMode, int) ->
  ParsedDateTime`, `snapSlider(double, std::int64_t minimum, std::int64_t maximum, std::int64_t step) ->
  std::int64_t`; `DateTimeEntry(Context&, ui::ContainerWidget*, ui::DateMode, int offsetMinutes)`,
  `SliderControl(Context&, ui::ContainerWidget*)`, `FilePickerBox(Context&, ui::ContainerWidget*,
  ui::FilePickerMode)`. QML contract: `DateTimeInput`: `dateOnly` (initial), `invalid`, `applyText`,
  `committed(string)`; `Slider`: `from`, `to`, `stepSize`, `value`, `valueMoved(real)`; `FilePicker`: `path`,
  `saveMode` / `browserFiles` (initial), `pathEdited(string)`, `fileChosen(url)`, `browseRequested()`, items
  `pathField`, `browseButton`, object `fileDialog`.

**Design notes.**
- `DateTimeInput` is an ISO field in the display zone (`offsetMinutes`): `YYYY-MM-DD`, or `YYYY-MM-DD HH:MM` (a `T`
  separator is accepted too). A commit (Enter or focus loss) of an unchanged text reports nothing; malformed text
  sets `invalid` and reports nothing; a blank field reports `nullopt`.
- A slider position is a QML `real`. The wrapper snaps a moved position to the nearest step from `minimum`, clamps
  it to the range (both ends always reachable), and reports an `int64`; positions round-trip exactly while
  `|value| ≤ 2^53`. `long double` keeps the arithmetic free of `int64` overflow.
- `FilePicker` is a path field plus Browse. Natively Browse opens QtQuick.Dialogs' `FileDialog` and the chosen URL
  becomes a local path in C++ (`QUrl::toLocalFile`). Under WebAssembly (`browserFiles`) Browse asks C++, which calls
  `QFileDialog::getOpenFileContent`, writes the contents to `/tmp/<file name>` in Emscripten's in-memory file system
  and reports that path; a Save picker there has no Browse button, because a browser offers downloads, not paths.

- [ ] **Step 1: Write the failing test**

Create `tests/qt_quick/test_values.cpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0

#include <QMetaObject>
#include <QObject>
#include <QQuickItem>
#include <QString>
#include <QTest>
#include <QUrl>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <morph/ui/view.hpp>
#include <morph/util/datetime.hpp>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "scene.hpp"
#include "values.hpp"
#include "widgets_value.hpp"

namespace {

using morph::qt_quick::detail::DateTimeEntry;
using morph::qt_quick::detail::FilePickerBox;
using morph::qt_quick::detail::formatDateTime;
using morph::qt_quick::detail::ParsedDateTime;
using morph::qt_quick::detail::parseDateTime;
using morph::qt_quick::detail::SliderControl;
using morph::qt_quick::detail::snapSlider;
using morph::qt_quick::testing::findItem;
using morph::qt_quick::testing::itemOf;
using morph::qt_quick::testing::Scene;
using morph::qt_quick::testing::typeText;
using morph::time::DateTime;
using morph::time::Timestamp;
namespace ui = morph::ui;

Timestamp at(unsigned day, int hour, int minute) {
    return Timestamp{DateTime{std::chrono::year{2026}, std::chrono::October, std::chrono::day{day},
                              std::chrono::hours{hour}, std::chrono::minutes{minute}, std::chrono::seconds{0}}};
}

void replaceText(Scene const& scene, QQuickItem& field, char const* text) {
    field.forceActiveFocus();
    QMetaObject::invokeMethod(&field, "selectAll");
    QTest::keyClick(scene.window.get(), Qt::Key_Backspace);
    typeText(*scene.window, QString::fromUtf8(text));
    QTest::keyClick(scene.window.get(), Qt::Key_Return);
}

}  // namespace

TEST_CASE("qt_quick date text: formats in the display zone and parses back to the same instant",
          "[qt_quick][values]") {
    Timestamp const instant = at(4, 21, 30);
    CHECK(formatDateTime(instant, ui::DateMode::DateTime, 120) == "2026-10-04 23:30");
    CHECK(formatDateTime(instant, ui::DateMode::DateTime, 180) == "2026-10-05 00:30");
    CHECK(formatDateTime(instant, ui::DateMode::Date, 180) == "2026-10-05");
    CHECK(formatDateTime(std::nullopt, ui::DateMode::Date, 0).empty());
    CHECK(formatDateTime(Timestamp{}, ui::DateMode::Date, 0).empty());

    for (char const* const text : {"2026-10-05 00:30", "2026-10-05T00:30", " 2026-10-05 00:30 "}) {
        ParsedDateTime const parsed = parseDateTime(text, ui::DateMode::DateTime, 180);
        REQUIRE(parsed.valid);
        REQUIRE(parsed.value.has_value());
        CHECK(*parsed.value == instant);
    }
    ParsedDateTime const date = parseDateTime("2026-10-05", ui::DateMode::Date, 180);
    REQUIRE(date.valid);
    CHECK(date.value == std::optional{at(4, 21, 0)});
}

TEST_CASE("qt_quick date text: malformed text is invalid; a blank field is valid and empty", "[qt_quick][values]") {
    for (char const* const text : {"2026-02-30 10:00", "2026-10-4 10:00", "26-10-04 10:00", "2026-10-04 25:00",
                                   "2026-10-04x10:00", "tomorrow"}) {
        CHECK_FALSE(parseDateTime(text, ui::DateMode::DateTime, 0).valid);
    }
    CHECK_FALSE(parseDateTime("2026-10-04 10:00", ui::DateMode::Date, 0).valid);
    ParsedDateTime const blank = parseDateTime("   ", ui::DateMode::Date, 0);
    CHECK(blank.valid);
    CHECK_FALSE(blank.value.has_value());
}

TEST_CASE("qt_quick DateTimeInput: setValue shows the value silently; a commit reports what was typed",
          "[qt_quick][values]") {
    Scene scene;
    DateTimeEntry entry{scene.context, nullptr, ui::DateMode::DateTime, 120};
    std::vector<std::optional<Timestamp>> changes;
    entry.setOnChange([&changes](std::optional<Timestamp> value) { changes.push_back(value); });
    entry.setValue(at(4, 21, 30));
    scene.show();
    QQuickItem& item = itemOf(entry);
    CHECK(item.property("text").toString() == QStringLiteral("2026-10-04 23:30"));

    item.forceActiveFocus();
    QTest::keyClick(scene.window.get(), Qt::Key_Return);
    CHECK(changes.empty());

    replaceText(scene, item, "2026-10-05 08:15");
    REQUIRE(changes.size() == 1);
    CHECK(changes.back() == std::optional{at(5, 6, 15)});
    CHECK_FALSE(item.property("invalid").toBool());

    replaceText(scene, item, "tomorrow");
    CHECK(changes.size() == 1);
    CHECK(item.property("invalid").toBool());
    entry.setValue(at(4, 21, 30));
    CHECK_FALSE(item.property("invalid").toBool());

    replaceText(scene, item, "");
    REQUIRE(changes.size() == 2);
    CHECK_FALSE(changes.back().has_value());
}

TEST_CASE("qt_quick slider snapping: nearest step from the minimum, clamped, both ends reachable",
          "[qt_quick][values]") {
    CHECK(snapSlider(43.0, 0, 100, 5) == 45);
    CHECK(snapSlider(42.4, 0, 100, 5) == 40);
    CHECK(snapSlider(-3.0, 0, 100, 5) == 0);
    CHECK(snapSlider(1e30, 0, 100, 5) == 100);
    CHECK(snapSlider(std::nan(""), 10, 100, 5) == 10);
    CHECK(snapSlider(7.0, 0, 100, 0) == 7);
    CHECK(snapSlider(99.0, 0, 100, 7) == 98);
    CHECK(snapSlider(100.0, 0, 100, 7) == 100);
    CHECK(snapSlider(5.0, 10, 10, 1) == 10);
}

TEST_CASE("qt_quick Slider: range and value are written silently; a move reports the snapped value",
          "[qt_quick][values]") {
    Scene scene;
    SliderControl slider{scene.context, nullptr};
    std::vector<std::int64_t> changes;
    slider.setOnChange([&changes](std::int64_t value) { changes.push_back(value); });
    slider.setRange(0, 100, 5);
    slider.setValue(40);
    QQuickItem& item = itemOf(slider);
    CHECK(item.inherits("QQuickSlider"));
    CHECK(item.property("from").toDouble() == 0.0);
    CHECK(item.property("to").toDouble() == 100.0);
    CHECK(item.property("stepSize").toDouble() == 5.0);
    CHECK(item.property("value").toDouble() == 40.0);
    CHECK(changes.empty());

    item.setProperty("value", 43.0);
    QMetaObject::invokeMethod(&item, "moved");
    CHECK(changes == std::vector<std::int64_t>{45});
}

TEST_CASE("qt_quick FilePicker: a typed path and a chosen file both report a local path", "[qt_quick][values]") {
    Scene scene;
    FilePickerBox picker{scene.context, nullptr, ui::FilePickerMode::Open};
    std::vector<std::string> picked;
    picker.setOnPicked([&picked](std::string path) { picked.push_back(std::move(path)); });
    picker.setPath("/tmp/in.csv");
    scene.show();
    QQuickItem& item = itemOf(picker);
    CHECK(item.property("path").toString() == QStringLiteral("/tmp/in.csv"));
    CHECK_FALSE(item.property("browserFiles").toBool());
    QQuickItem* const field = findItem(item, "pathField");
    REQUIRE(field != nullptr);
    CHECK(field->property("text").toString() == QStringLiteral("/tmp/in.csv"));

    replaceText(scene, *field, "/tmp/out.csv");
    CHECK(picked == std::vector<std::string>{"/tmp/out.csv"});

    QMetaObject::invokeMethod(&item, "fileChosen", Q_ARG(QUrl, QUrl::fromLocalFile(QString::fromUtf8("/tmp/größe.txt"))));
    REQUIRE(picked.size() == 2);
    CHECK(picked.back() == "/tmp/größe.txt");

    FilePickerBox save{scene.context, nullptr, ui::FilePickerMode::Save};
    QObject const* const dialog = itemOf(save).findChild<QObject*>(QStringLiteral("fileDialog"));
    REQUIRE(dialog != nullptr);
    CHECK(dialog->property("fileMode").toInt() == 2);  // FileDialog.SaveFile
}
```

- [ ] **Step 2: Run it to verify it fails**

Add `test_values.cpp` to `morph_qt_quick_tests` and build.
Expected: FAIL — `'values.hpp' file not found`.

- [ ] **Step 3: Implement**

In `src/qt_quick/context.hpp`, insert `DateTimeInput, Slider, FilePicker,` before `Count,` and append
`"DateTimeInput", "Slider", "FilePicker",` to `kTypeNames`.

Create `src/qt_quick/values.hpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0

#pragma once
#include <cstdint>
#include <morph/ui/view.hpp>
#include <morph/util/datetime.hpp>
#include <optional>
#include <string>
#include <string_view>

/// @file
/// @brief The text and number conversions behind DateTimeInput and Slider.

namespace morph::qt_quick::detail {

/// @brief What a DateTimeInput's text means.
struct ParsedDateTime {
    /// @brief False when the text is neither blank nor a date (or date-time) in the expected form.
    bool valid = false;
    /// @brief The instant, or nullopt for a blank field.
    std::optional<time::Timestamp> value;
};

/// @brief @p value as shown in the display zone: `YYYY-MM-DD`, or `YYYY-MM-DD HH:MM`.
/// @param value The instant; nullopt or an empty Timestamp shows as blank.
/// @param mode Date or date-time.
/// @param offsetMinutes The display zone's offset from UTC.
/// @return The text.
[[nodiscard]] std::string formatDateTime(std::optional<time::Timestamp> const& value, ui::DateMode mode,
                                         int offsetMinutes);

/// @brief The instant @p text names in the display zone; surrounding spaces are ignored and `T` may separate date
///        and time.
/// @param text What the field holds.
/// @param mode Date or date-time.
/// @param offsetMinutes The display zone's offset from UTC.
/// @return Whether it parsed, and the instant (nullopt for blank text).
[[nodiscard]] ParsedDateTime parseDateTime(std::string_view text, ui::DateMode mode, int offsetMinutes);

/// @brief A slider position as the value it stands for: the nearest step from @p minimum, clamped to the range.
/// @param position The control's position; NaN counts as the minimum.
/// @param minimum The range's low end.
/// @param maximum The range's high end; at or below @p minimum the value is @p minimum.
/// @param step The step; zero or negative counts as one.
/// @return The value.
[[nodiscard]] std::int64_t snapSlider(double position, std::int64_t minimum, std::int64_t maximum,
                                      std::int64_t step) noexcept;

}  // namespace morph::qt_quick::detail
```

Create `src/qt_quick/values.cpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0

#include "values.hpp"

#include <chrono>
#include <cmath>
#include <format>

namespace morph::qt_quick::detail {

namespace {

std::string_view trimmed(std::string_view text) {
    while (!text.empty() && text.front() == ' ') {
        text.remove_prefix(1);
    }
    while (!text.empty() && text.back() == ' ') {
        text.remove_suffix(1);
    }
    return text;
}

}  // namespace

std::string formatDateTime(std::optional<time::Timestamp> const& value, ui::DateMode mode, int offsetMinutes) {
    if (!value || !value->hasValue()) {
        return {};
    }
    auto const local = (**value).value + std::chrono::minutes{offsetMinutes};
    auto const midnight = std::chrono::floor<std::chrono::days>(local);
    std::chrono::year_month_day const date{midnight};
    std::string text = std::format("{:04}-{:02}-{:02}", static_cast<int>(date.year()),
                                   static_cast<unsigned>(date.month()), static_cast<unsigned>(date.day()));
    if (mode == ui::DateMode::DateTime) {
        std::chrono::hh_mm_ss const clock{local - midnight};
        text += std::format(" {:02}:{:02}", clock.hours().count(), clock.minutes().count());
    }
    return text;
}

ParsedDateTime parseDateTime(std::string_view text, ui::DateMode mode, int offsetMinutes) {
    text = trimmed(text);
    if (text.empty()) {
        return ParsedDateTime{.valid = true, .value = std::nullopt};
    }
    std::string iso;
    if (mode == ui::DateMode::Date) {
        if (text.size() != 10) {
            return {};
        }
        iso = std::string{text} + "T00:00:00";
    } else {
        if (text.size() != 16 || (text.at(10) != ' ' && text.at(10) != 'T')) {
            return {};
        }
        iso = std::string{text.substr(0, 10)} + "T" + std::string{text.substr(11)} + ":00";
    }
    std::optional<time::DateTime> const parsed = time::DateTime::fromIso8601(iso);
    if (!parsed) {
        return {};
    }
    return ParsedDateTime{.valid = true,
                          .value = time::Timestamp{time::DateTime{parsed->value - std::chrono::minutes{offsetMinutes}}}};
}

std::int64_t snapSlider(double position, std::int64_t minimum, std::int64_t maximum, std::int64_t step) noexcept {
    if (maximum <= minimum || !(position > static_cast<double>(minimum))) {
        return minimum;
    }
    if (!(position < static_cast<double>(maximum))) {
        return maximum;
    }
    long double const stride = static_cast<long double>(step > 0 ? step : 1);
    long double const low = static_cast<long double>(minimum);
    long double const steps = std::round((static_cast<long double>(position) - low) / stride);
    long double const snapped = low + steps * stride;
    if (snapped >= static_cast<long double>(maximum)) {
        return maximum;
    }
    if (snapped <= low) {
        return minimum;
    }
    return static_cast<std::int64_t>(snapped);
}

}  // namespace morph::qt_quick::detail
```

Create `src/qt_quick/widgets_value.hpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0

#pragma once
#include <QString>
#include <cstdint>
#include <functional>
#include <memory>
#include <morph/ui/backend.hpp>
#include <morph/ui/view.hpp>
#include <morph/util/datetime.hpp>
#include <optional>
#include <string>
#include <string_view>

#include "context.hpp"
#include "item_holder.hpp"

/// @file
/// @brief Wrappers for the kinds that edit one value: a date, a number on a range, a file path.

namespace morph::qt_quick::detail {

/// @brief `ui::DateTimeInput` as a `MorphUi.DateTimeInput`: an ISO field in the display zone.
class DateTimeEntry final : public ItemWidget<ui::DateTimeInputWidget> {
public:
    /// @param context The backend's context.
    /// @param parent The container, or null for the root.
    /// @param mode Date or date-time; fixed for the widget's life.
    /// @param offsetMinutes The display zone's offset from UTC.
    DateTimeEntry(Context& context, ui::ContainerWidget* parent, ui::DateMode mode, int offsetMinutes);

    void setValue(std::optional<time::Timestamp> const& value) override;
    void setOnChange(std::function<void(std::optional<time::Timestamp>)> onChange) override;

private:
    void commit(QString const& text);

    ui::DateMode _mode;
    int _offsetMinutes;
    QString _shown;
    std::function<void(std::optional<time::Timestamp>)> _onChange;
};

/// @brief `ui::Slider` as a `MorphUi.Slider`.
class SliderControl final : public ItemWidget<ui::SliderWidget> {
public:
    /// @param context The backend's context.
    /// @param parent The container, or null for the root.
    SliderControl(Context& context, ui::ContainerWidget* parent);

    void setRange(std::int64_t minimum, std::int64_t maximum, std::int64_t step) override;
    void setValue(std::int64_t value) override;
    void setOnChange(std::function<void(std::int64_t)> onChange) override;

private:
    std::int64_t _minimum = 0;
    std::int64_t _maximum = 100;
    std::int64_t _step = 1;
    std::function<void(std::int64_t)> _onChange;
};

/// @brief `ui::FilePicker` as a `MorphUi.FilePicker`: a path field and Browse.
class FilePickerBox final : public ItemWidget<ui::FilePickerWidget> {
public:
    /// @param context The backend's context.
    /// @param parent The container, or null for the root.
    /// @param mode Open or Save; fixed for the widget's life.
    FilePickerBox(Context& context, ui::ContainerWidget* parent, ui::FilePickerMode mode);

    void setPath(std::string_view path) override;
    void setOnPicked(std::function<void(std::string)> onPicked) override;

private:
    void pick(std::string path);
#if defined(__EMSCRIPTEN__)
    void browseInBrowser();
    // Observed weakly by a pending browser file dialog, which may answer after this widget is gone.
    std::shared_ptr<char> _alive = std::make_shared<char>();
#endif

    std::function<void(std::string)> _onPicked;
};

}  // namespace morph::qt_quick::detail
```

Create `src/qt_quick/widgets_value.cpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0

#include "widgets_value.hpp"

#include <QUrl>
#include <QVariant>
#include <QVariantMap>
#include <utility>

#include "strings.hpp"
#include "values.hpp"

#if defined(__EMSCRIPTEN__)
#include <QByteArray>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QIODevice>
#include <morph/core/logger.hpp>
#endif

namespace morph::qt_quick::detail {

namespace {

#if defined(__EMSCRIPTEN__)
constexpr bool kBrowserFiles = true;
#else
constexpr bool kBrowserFiles = false;
#endif

}  // namespace

DateTimeEntry::DateTimeEntry(Context& context, ui::ContainerWidget* parent, ui::DateMode mode, int offsetMinutes)
    : ItemWidget{context, Kind::DateTimeInput, parent,
                 QVariantMap{{QStringLiteral("dateOnly"), mode == ui::DateMode::Date}}},
      _mode{mode},
      _offsetMinutes{offsetMinutes} {
    relay("committed(QString)", [this](QVariant const& text) { commit(text.toString()); });
}

void DateTimeEntry::setValue(std::optional<time::Timestamp> const& value) {
    _shown = toQString(formatDateTime(value, _mode, _offsetMinutes));
    write("invalid", false);
    invoke("applyText", _shown);
}

void DateTimeEntry::setOnChange(std::function<void(std::optional<time::Timestamp>)> onChange) {
    _onChange = std::move(onChange);
}

void DateTimeEntry::commit(QString const& text) {
    if (text == _shown) {
        return;
    }
    ParsedDateTime const parsed = parseDateTime(toUtf8(text), _mode, _offsetMinutes);
    write("invalid", !parsed.valid);
    if (!parsed.valid) {
        return;
    }
    _shown = text;
    if (auto const onChange = _onChange) {
        onChange(parsed.value);
    }
}

SliderControl::SliderControl(Context& context, ui::ContainerWidget* parent)
    : ItemWidget{context, Kind::Slider, parent} {
    relay("valueMoved(double)", [this](QVariant const& position) {
        std::int64_t const value = snapSlider(position.toDouble(), _minimum, _maximum, _step);
        if (auto const onChange = _onChange) {
            onChange(value);
        }
    });
}

void SliderControl::setRange(std::int64_t minimum, std::int64_t maximum, std::int64_t step) {
    _minimum = minimum;
    _maximum = maximum;
    _step = step;
    write("from", static_cast<double>(minimum));
    write("to", static_cast<double>(maximum));
    write("stepSize", static_cast<double>(step > 0 ? step : 1));
}

void SliderControl::setValue(std::int64_t value) { write("value", static_cast<double>(value)); }

void SliderControl::setOnChange(std::function<void(std::int64_t)> onChange) { _onChange = std::move(onChange); }

FilePickerBox::FilePickerBox(Context& context, ui::ContainerWidget* parent, ui::FilePickerMode mode)
    : ItemWidget{context, Kind::FilePicker, parent,
                 QVariantMap{{QStringLiteral("saveMode"), mode == ui::FilePickerMode::Save},
                             {QStringLiteral("browserFiles"), kBrowserFiles}}} {
    relay("pathEdited(QString)", [this](QVariant const& path) { pick(toUtf8(path.toString())); });
    relay("fileChosen(QUrl)", [this](QVariant const& url) { pick(toUtf8(url.toUrl().toLocalFile())); });
#if defined(__EMSCRIPTEN__)
    relay("browseRequested()", [this](QVariant const&) { browseInBrowser(); });
#endif
}

void FilePickerBox::setPath(std::string_view path) { write("path", toQString(path)); }

void FilePickerBox::setOnPicked(std::function<void(std::string)> onPicked) { _onPicked = std::move(onPicked); }

void FilePickerBox::pick(std::string path) {
    if (auto const onPicked = _onPicked) {
        onPicked(std::move(path));
    }
}

#if defined(__EMSCRIPTEN__)
void FilePickerBox::browseInBrowser() {
    QFileDialog::getOpenFileContent(
        QString{}, [this, alive = std::weak_ptr<char>{_alive}](QString const& fileName, QByteArray const& content) {
            if (alive.expired() || fileName.isEmpty()) {
                return;
            }
            // A browser hands over a file's contents, not a path: they become a file in the in-memory file system.
            QString const path = QStringLiteral("/tmp/") + QFileInfo{fileName}.fileName();
            QFile file{path};
            if (!file.open(QIODevice::WriteOnly) || file.write(content) != content.size()) {
                morph::log::logError("[qt_quick] cannot store the picked file at " + toUtf8(path));
                return;
            }
            file.close();
            pick(toUtf8(path));
        });
}
#endif

}  // namespace morph::qt_quick::detail
```

Create `src/qt_quick/qml/DateTimeInput.qml`:

```qml
// SPDX-License-Identifier: Apache-2.0
import QtQuick
import QtQuick.Controls as C
import QtQuick.Layouts

// ui::DateTimeInput: an ISO field in the display zone. `committed` carries the
// text on Enter or focus loss; C++ parses it and sets `invalid` when it is
// malformed. applyText writes only a text that differs.
C.TextField {
    id: root
    property bool dateOnly: false
    property bool invalid: false
    signal committed(string text)

    placeholderText: dateOnly ? "YYYY-MM-DD" : "YYYY-MM-DD HH:MM"
    color: invalid ? "#b3261e" : palette.text
    onEditingFinished: root.committed(text)

    function applyText(value) {
        if (text !== value)
            text = value
    }
}
```

Create `src/qt_quick/qml/Slider.qml`:

```qml
// SPDX-License-Identifier: Apache-2.0
import QtQuick
import QtQuick.Controls as C
import QtQuick.Layouts

// ui::Slider. `valueMoved` carries the position a user drag or key produced;
// writing `value` emits nothing.
C.Slider {
    id: root
    signal valueMoved(real value)
    snapMode: C.Slider.SnapAlways
    onMoved: root.valueMoved(value)
}
```

Create `src/qt_quick/qml/FilePicker.qml`:

```qml
// SPDX-License-Identifier: Apache-2.0
import QtQuick
import QtQuick.Controls as C
import QtQuick.Dialogs
import QtQuick.Layouts

// ui::FilePicker: a path field and Browse. A typed path is reported on commit;
// Browse opens a FileDialog, or under WebAssembly (`browserFiles`) asks C++,
// which reads the file through the browser. A browser offers no save paths, so
// a Save picker there has no Browse button.
Item {
    id: root
    property string path: ""
    property bool saveMode: false
    property bool browserFiles: false
    signal pathEdited(string path)
    signal fileChosen(url fileUrl)
    signal browseRequested()

    implicitWidth: row.implicitWidth
    implicitHeight: row.implicitHeight
    onPathChanged: field.text = root.path
    Component.onCompleted: field.text = root.path

    RowLayout {
        id: row
        anchors.fill: parent

        C.TextField {
            id: field
            objectName: "pathField"
            Layout.fillWidth: true
            onEditingFinished: if (text !== root.path) root.pathEdited(text)
        }
        C.Button {
            objectName: "browseButton"
            text: qsTr("Browse…")
            visible: !(root.browserFiles && root.saveMode)
            onClicked: root.browserFiles ? root.browseRequested() : dialog.open()
        }
    }

    FileDialog {
        id: dialog
        objectName: "fileDialog"
        fileMode: root.saveMode ? FileDialog.SaveFile : FileDialog.OpenFile
        onAccepted: root.fileChosen(selectedFile)
    }
}
```

Add the three QML files to `_morph_qt_quick_qml_files` and `values.cpp values.hpp widgets_value.cpp
widgets_value.hpp` to `morph_qt_quick`.

- [ ] **Step 4: Run the tests to verify they pass**

Run: `cmake --build build/qt --target morph_qt_quick_tests && ./build/qt/tests/qt_quick/morph_qt_quick_tests "[values]"`
Expected: PASS.

Mutation checks (one at a time, then restore):
- In `formatDateTime`, drop `+ std::chrono::minutes{offsetMinutes}` → expected FAIL in "formats in the display zone
  and parses back to the same instant".
- In `DateTimeEntry::commit`, delete the `text == _shown` early return → expected FAIL in "setValue shows the value
  silently" (the unchanged Enter reports a change).
- In `snapSlider`, replace `std::round` with `std::floor` → expected FAIL in "nearest step from the minimum"
  (`43.0` snaps to 40).

- [ ] **Step 5: Commit**

```bash
git add src/qt_quick tests/qt_quick
git commit -m "wip(qtquick): DateTimeInput, Slider and FilePicker

Signed-off-by: Christian Parpart <christian@parpart.family>"
```

---
### Task 8: The public `qt_quick::Backend`, and backend conformance

**Files:**
- Create: `include/morph/qt_quick/backend.hpp`, `src/qt_quick/backend.cpp`
- Modify: `src/qt_quick/CMakeLists.txt` — `backend.cpp` in `morph_qt_quick`, and the public header set (below)
- Create: `tests/qt_quick/qt_quick_probe.hpp`
- Modify: `tests/qt_quick/CMakeLists.txt` — `test_backend.cpp`
- Test: `tests/qt_quick/test_backend.cpp`

**Interfaces:**
- Consumes: every wrapper from Tasks 2–7; `ui::IViewBackend` and its factories, `ui::Mounted`,
  `ui::testing::ConformanceProbe`, `ui::testing::conformanceCases()`, `ui::testing::RecordingBackend`,
  `ui::button`, `ui::Button` (Part 2); `reactive::Signal`, `reactive::Runtime` (Part 1);
  `morph::qt::QtExecutor` (`include/morph/qt/qt_executor.hpp`).
- Produces (the contract's Part 4 names, plus two additions in this part's namespace):
  - `morph::qt_quick::Backend(QQmlEngine& engine, QQuickItem& root)` implementing every `ui::IViewBackend` factory.
  - **Addition:** `[[nodiscard]] QQuickItem* morph::qt_quick::itemOf(ui::Widget const&) noexcept` and
    `[[nodiscard]] ui::Widget* morph::qt_quick::widgetOf(QQuickItem const&) noexcept` — the item behind a widget and
    back, for tests and accessibility tooling (spec 3 §2: "objectName so tests and accessibility tooling can find
    it").
  - `morph::qt_quick::testing::QtQuickProbe` (tests only): a `ConformanceProbe` over a shown `MorphUi` window.

- [ ] **Step 1: Write the failing test**

Create `tests/qt_quick/qt_quick_probe.hpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0

#pragma once
#include <QCoreApplication>
#include <QEvent>
#include <QMetaObject>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickWindow>
#include <QTest>
#include <QtQuickTest/quicktest.h>
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <memory>
#include <morph/qt/qt_executor.hpp>
#include <morph/qt_quick/backend.hpp>
#include <morph/reactive/runtime.hpp>
#include <morph/ui/backend.hpp>
#include <morph/ui/testing/backend_conformance.hpp>
#include <string>
#include <string_view>
#include <vector>

#include "item_holder.hpp"
#include "scene.hpp"
#include "strings.hpp"
#include "window.hpp"

namespace morph::qt_quick::testing {

/// Spec 1's backend conformance cases against `qt_quick::Backend`, observed through the items and driven with
/// real pointer and key events on a shown window.
class QtQuickProbe final : public ui::testing::ConformanceProbe {
public:
    QtQuickProbe() {
        _window->resize(800, 600);
        _window->show();
        REQUIRE(QTest::qWaitForWindowExposed(_window.get()));
        _window->requestActivate();
        REQUIRE(QTest::qWaitForWindowActive(_window.get()));
    }

    ui::IViewBackend& backend() override { return _backend; }
    reactive::Runtime& runtime() override { return _runtime; }

    /// Runs posted flushes until none is pending, then the layout pass.
    void settle() override {
        for (int round = 0; round < 100; ++round) {
            QCoreApplication::sendPostedEvents();
            QCoreApplication::processEvents(QEventLoop::AllEvents);
            if (!_runtime.isFlushRequested()) {
                break;
            }
        }
        CHECK(QQuickTest::qWaitForPolish(_window.get()));
        QCoreApplication::processEvents(QEventLoop::AllEvents);
    }

    [[nodiscard]] std::string textOf(ui::Widget const& widget) override {
        QQuickItem const& item = required(widget);
        for (char const* const name : {"text", "currentText", "title", "label", "path"}) {
            if (item.metaObject()->indexOfProperty(name) >= 0) {
                return detail::toUtf8(item.property(name).toString());
            }
        }
        return {};
    }

    /// The widget's own flag, as `RecordingBackend` reports it: `QQuickItem::isVisible()` would fold in hidden
    /// ancestors, and a Dialog's root item is never shown at all.
    [[nodiscard]] bool visibleOf(ui::Widget const& widget) override {
        detail::ItemHolder const* const holder = detail::holderOf(widget);
        REQUIRE(holder != nullptr);
        return holder->visibleFlag();
    }

    /// The widget's own flag, as `RecordingBackend` reports it: `QQuickItem::isEnabled()` would fold in disabled
    /// ancestors.
    [[nodiscard]] bool enabledOf(ui::Widget const& widget) override {
        detail::ItemHolder const* const holder = detail::holderOf(widget);
        REQUIRE(holder != nullptr);
        return holder->enabledFlag();
    }

    [[nodiscard]] std::size_t childCount(ui::ContainerWidget const& container) override {
        return widgetChildren(required(container)).size();
    }

    [[nodiscard]] ui::Widget const* childAt(ui::ContainerWidget const& container, std::size_t index) override {
        std::vector<QQuickItem*> const children = widgetChildren(required(container));
        return index < children.size() ? qt_quick::widgetOf(*children.at(index)) : nullptr;
    }

    void click(ui::Widget& widget) override {
        QTest::mouseClick(_window.get(), Qt::LeftButton, Qt::NoModifier, centre(required(widget)));
    }

    void type(ui::Widget& widget, std::string_view text) override {
        QQuickItem& item = required(widget);
        item.forceActiveFocus();
        QMetaObject::invokeMethod(&item, "selectAll");
        typeText(*_window, detail::toQString(text));
    }

    void drag(ui::Widget& source, ui::Widget& target) override {
        dragAcross(*_window, centre(required(source)), centre(required(target)), [] {});
    }

private:
    static QQuickItem& required(ui::Widget const& widget) {
        QQuickItem* const item = qt_quick::itemOf(widget);
        REQUIRE(item != nullptr);
        return *item;
    }

    qt::QtExecutor _executor{QCoreApplication::instance()};
    reactive::Runtime _runtime{_executor};
    QQmlEngine _engine;
    std::unique_ptr<QQuickWindow> _window = detail::createWindow(_engine);
    Backend _backend{_engine, detail::windowContent(*_window)};
};

}  // namespace morph::qt_quick::testing
```

Create `tests/qt_quick/test_backend.cpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0

#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickWindow>
#include <QSet>
#include <QString>
#include <QUrl>
#include <QtQml/qqml.h>
#include <catch2/catch_test_macros.hpp>
#include <memory>
#include <morph/qt_quick/backend.hpp>
#include <morph/reactive/signal.hpp>
#include <morph/ui/mount.hpp>
#include <morph/ui/testing/backend_conformance.hpp>
#include <morph/ui/testing/recording_backend.hpp>
#include <morph/ui/view.hpp>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include "qt_quick_probe.hpp"
#include "scene.hpp"
#include "window.hpp"

namespace {

using morph::qt_quick::Backend;
using morph::qt_quick::testing::contentOf;
using morph::qt_quick::testing::QtQuickProbe;
namespace qt_quick = morph::qt_quick;
namespace ui = morph::ui;

struct BackendScene {
    QQmlEngine engine;
    std::unique_ptr<QQuickWindow> window = qt_quick::detail::createWindow(engine);
    Backend backend{engine, qt_quick::detail::windowContent(*window)};
};

struct Made {
    std::unique_ptr<ui::Widget> widget;
    char const* file;
};

}  // namespace

TEST_CASE("qt_quick Backend: every factory makes its MorphUi component, named and parented", "[qt_quick][backend]") {
    BackendScene scene;
    std::unique_ptr<ui::StackWidget> const root = scene.backend.createStack(nullptr, ui::Axis::Vertical);
    ui::ContainerWidget* const parent = root.get();
    std::vector<Made> made;
    made.push_back({scene.backend.createText(parent), "Label.qml"});
    made.push_back({scene.backend.createButton(parent), "Button.qml"});
    made.push_back({scene.backend.createTextInput(parent, ui::TextInputMode::SingleLine), "TextInput.qml"});
    made.push_back({scene.backend.createTextInput(parent, ui::TextInputMode::Password), "TextInput.qml"});
    made.push_back({scene.backend.createTextInput(parent, ui::TextInputMode::Multiline), "TextArea.qml"});
    made.push_back({scene.backend.createCheckbox(parent), "Checkbox.qml"});
    made.push_back({scene.backend.createSelect(parent, ui::SelectStyle::Dropdown), "Select.qml"});
    made.push_back({scene.backend.createSelect(parent, ui::SelectStyle::Radio), "RadioGroup.qml"});
    made.push_back({scene.backend.createMenu(parent), "Menu.qml"});
    made.push_back({scene.backend.createStack(parent, ui::Axis::Vertical), "ColumnStack.qml"});
    made.push_back({scene.backend.createStack(parent, ui::Axis::Horizontal), "RowStack.qml"});
    made.push_back({scene.backend.createGrid(parent), "Grid.qml"});
    made.push_back({scene.backend.createSpacer(parent), "Spacer.qml"});
    made.push_back({scene.backend.createPanel(parent), "Panel.qml"});
    made.push_back({scene.backend.createScroll(parent, ui::Axis::Vertical), "Scroll.qml"});
    made.push_back({scene.backend.createSlot(parent), "Slot.qml"});
    made.push_back({scene.backend.createTabs(parent), "Tabs.qml"});
    made.push_back({scene.backend.createDialog(parent), "Dialog.qml"});
    made.push_back({scene.backend.createBusy(parent), "Busy.qml"});
    made.push_back({scene.backend.createTable(parent), "Table.qml"});
    made.push_back({scene.backend.createDateTimeInput(parent, ui::DateMode::Date, 0), "DateTimeInput.qml"});
    made.push_back({scene.backend.createSlider(parent), "Slider.qml"});
    made.push_back({scene.backend.createFilePicker(parent, ui::FilePickerMode::Open), "FilePicker.qml"});

    QQuickItem* const rootItem = qt_quick::itemOf(*root);
    REQUIRE(rootItem != nullptr);
    QSet<QString> names{rootItem->objectName()};
    for (Made const& one : made) {
        INFO(one.file);
        QQuickItem* const item = qt_quick::itemOf(*one.widget);
        REQUIRE(item != nullptr);
        CHECK(qmlContext(item)->baseUrl().fileName() == QString::fromLatin1(one.file));
        CHECK(item->parentItem() == &contentOf(*rootItem));
        CHECK(item->objectName().startsWith(QLatin1Char{'w'}));
        CHECK(qt_quick::widgetOf(*item) == one.widget.get());
        names.insert(item->objectName());
    }
    CHECK(names.size() == static_cast<qsizetype>(made.size() + 1));

    made.clear();
    CHECK(contentOf(*rootItem).childItems().isEmpty());
}

TEST_CASE("qt_quick Backend: another backend's widget has no item and is refused as a parent", "[qt_quick][backend]") {
    ui::testing::RecordingBackend recording;
    std::unique_ptr<ui::StackWidget> const foreign = recording.createStack(nullptr, ui::Axis::Vertical);
    CHECK(qt_quick::itemOf(*foreign) == nullptr);
    BackendScene scene;
    CHECK_THROWS_AS(scene.backend.createText(foreign.get()), std::invalid_argument);
    CHECK(qt_quick::detail::windowContent(*scene.window).childItems().isEmpty());
}

TEST_CASE("qt_quick Backend: a mounted Button shows its bound label and reaches its Action once per click",
          "[qt_quick][backend]") {
    QtQuickProbe probe;
    morph::reactive::Signal<std::string> label{probe.runtime(), "Go"};
    int clicks = 0;
    ui::Mounted const mounted{probe.runtime(), probe.backend(),
                              ui::button({.label = [&label] { return label.get(); }, .onClick = [&clicks] { ++clicks; }})};
    probe.settle();
    CHECK(probe.textOf(mounted.root()) == "Go");
    label.set("Again");
    probe.settle();
    CHECK(probe.textOf(mounted.root()) == "Again");
    probe.click(mounted.root());
    probe.settle();
    CHECK(clicks == 1);
}

TEST_CASE("qt_quick Backend passes the backend conformance suite", "[qt_quick][backend][conformance]") {
    auto const cases = ui::testing::conformanceCases();
    // Every case the contract lists: build, bind, update, remount, Tabs, ForEach, Dialog, drag and drop.
    REQUIRE(cases.size() == 13);
    for (ui::testing::ConformanceCase const& conformanceCase : cases) {
        DYNAMIC_SECTION(conformanceCase.name) {
            QtQuickProbe probe;
            std::optional<std::string> const failure = conformanceCase.run(probe);
            if (failure) {
                FAIL(std::string{conformanceCase.name} + ": " + *failure);
            }
        }
    }
}
```

- [ ] **Step 2: Run it to verify it fails**

Add `test_backend.cpp` to `morph_qt_quick_tests` and build.
Expected: FAIL — `'morph/qt_quick/backend.hpp' file not found`.

- [ ] **Step 3: Implement**

Create `include/morph/qt_quick/backend.hpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0

#pragma once
#include <QQmlEngine>
#include <QQuickItem>
#include <memory>
#include <morph/ui/backend.hpp>
#include <morph/ui/view.hpp>

/// @file
/// @brief `morph::qt_quick::Backend`: `ui::IViewBackend` over Qt Quick Controls items made from C++.
///
/// Specified in `docs/spec/qt_quick/frontend.md`.

namespace morph::qt_quick {

namespace detail {
class Context;
}  // namespace detail

/// @brief Builds every `ui` widget kind as an item of a private QML module, `MorphUi`, one plain component per kind.
///
/// Each widget's item is parented into its container's content (or into @p root), named `w<id>` in creation
/// order, and deleted with the widget. Setters write the component's properties (strings as UTF-8); user signals
/// reach the widget's callbacks; a text setter never echoes as an edit. Layout hints become `Layout.*` attached
/// properties in units of the application font: a character's average width across, a line's height down. A
/// widget with a drag key can be dragged onto a widget with a drop handler; keys cross into QML as text, so no
/// integer key becomes a double. Every factory runs on the thread that owns @p engine.
class Backend final : public ui::IViewBackend {
public:
    /// @param engine Makes every item; must outlive this backend and every widget it makes.
    /// @param root Where a widget made without a parent goes; a Layout, so the root widget's hints size it.
    /// @throws std::runtime_error naming the first `MorphUi` component that does not load.
    Backend(QQmlEngine& engine, QQuickItem& root);
    ~Backend() override;
    Backend(Backend const&) = delete;
    Backend& operator=(Backend const&) = delete;
    Backend(Backend&&) = delete;
    Backend& operator=(Backend&&) = delete;

    [[nodiscard]] std::unique_ptr<ui::TextWidget> createText(ui::ContainerWidget* parent) override;
    [[nodiscard]] std::unique_ptr<ui::ButtonWidget> createButton(ui::ContainerWidget* parent) override;
    [[nodiscard]] std::unique_ptr<ui::TextInputWidget> createTextInput(ui::ContainerWidget* parent,
                                                                       ui::TextInputMode mode) override;
    [[nodiscard]] std::unique_ptr<ui::CheckboxWidget> createCheckbox(ui::ContainerWidget* parent) override;
    [[nodiscard]] std::unique_ptr<ui::SelectWidget> createSelect(ui::ContainerWidget* parent,
                                                                 ui::SelectStyle style) override;
    [[nodiscard]] std::unique_ptr<ui::MenuWidget> createMenu(ui::ContainerWidget* parent) override;
    [[nodiscard]] std::unique_ptr<ui::StackWidget> createStack(ui::ContainerWidget* parent, ui::Axis axis) override;
    [[nodiscard]] std::unique_ptr<ui::GridWidget> createGrid(ui::ContainerWidget* parent) override;
    [[nodiscard]] std::unique_ptr<ui::SpacerWidget> createSpacer(ui::ContainerWidget* parent) override;
    [[nodiscard]] std::unique_ptr<ui::PanelWidget> createPanel(ui::ContainerWidget* parent) override;
    [[nodiscard]] std::unique_ptr<ui::ScrollWidget> createScroll(ui::ContainerWidget* parent, ui::Axis axis) override;
    [[nodiscard]] std::unique_ptr<ui::SlotWidget> createSlot(ui::ContainerWidget* parent) override;
    [[nodiscard]] std::unique_ptr<ui::TabsWidget> createTabs(ui::ContainerWidget* parent) override;
    [[nodiscard]] std::unique_ptr<ui::DialogWidget> createDialog(ui::ContainerWidget* parent) override;
    [[nodiscard]] std::unique_ptr<ui::BusyWidget> createBusy(ui::ContainerWidget* parent) override;
    [[nodiscard]] std::unique_ptr<ui::TableWidget> createTable(ui::ContainerWidget* parent) override;
    [[nodiscard]] std::unique_ptr<ui::DateTimeInputWidget> createDateTimeInput(ui::ContainerWidget* parent,
                                                                               ui::DateMode mode,
                                                                               int offsetMinutes) override;
    [[nodiscard]] std::unique_ptr<ui::SliderWidget> createSlider(ui::ContainerWidget* parent) override;
    [[nodiscard]] std::unique_ptr<ui::FilePickerWidget> createFilePicker(ui::ContainerWidget* parent,
                                                                         ui::FilePickerMode mode) override;

private:
    std::unique_ptr<detail::Context> _context;
};

/// @brief The item behind a widget a `qt_quick::Backend` made, for tests and accessibility tooling.
/// @param widget Any widget.
/// @return Its item, or null when another backend made it or Qt already deleted the item.
[[nodiscard]] QQuickItem* itemOf(ui::Widget const& widget) noexcept;

/// @brief The widget an item stands for.
/// @param item Any item.
/// @return The widget a `qt_quick::Backend` made @p item for, or null.
[[nodiscard]] ui::Widget* widgetOf(QQuickItem const& item) noexcept;

}  // namespace morph::qt_quick
```

Create `src/qt_quick/backend.cpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0

#include <memory>
#include <morph/qt_quick/backend.hpp>

#include "context.hpp"
#include "item_holder.hpp"
#include "widgets.hpp"
#include "widgets_input.hpp"
#include "widgets_structure.hpp"
#include "widgets_table.hpp"
#include "widgets_value.hpp"

namespace morph::qt_quick {

Backend::Backend(QQmlEngine& engine, QQuickItem& root) : _context{std::make_unique<detail::Context>(engine, root)} {}

Backend::~Backend() = default;

std::unique_ptr<ui::TextWidget> Backend::createText(ui::ContainerWidget* parent) {
    return std::make_unique<detail::TextLabel>(*_context, parent);
}

std::unique_ptr<ui::ButtonWidget> Backend::createButton(ui::ContainerWidget* parent) {
    return std::make_unique<detail::PushButton>(*_context, parent);
}

std::unique_ptr<ui::TextInputWidget> Backend::createTextInput(ui::ContainerWidget* parent, ui::TextInputMode mode) {
    return std::make_unique<detail::TextEntry>(*_context, parent, mode);
}

std::unique_ptr<ui::CheckboxWidget> Backend::createCheckbox(ui::ContainerWidget* parent) {
    return std::make_unique<detail::CheckBox>(*_context, parent);
}

std::unique_ptr<ui::SelectWidget> Backend::createSelect(ui::ContainerWidget* parent, ui::SelectStyle style) {
    return std::make_unique<detail::Choice>(*_context, parent, style);
}

std::unique_ptr<ui::MenuWidget> Backend::createMenu(ui::ContainerWidget* parent) {
    return std::make_unique<detail::MenuList>(*_context, parent);
}

std::unique_ptr<ui::StackWidget> Backend::createStack(ui::ContainerWidget* parent, ui::Axis axis) {
    return std::make_unique<detail::Stack>(*_context, parent, axis);
}

std::unique_ptr<ui::GridWidget> Backend::createGrid(ui::ContainerWidget* parent) {
    return std::make_unique<detail::GridBox>(*_context, parent);
}

std::unique_ptr<ui::SpacerWidget> Backend::createSpacer(ui::ContainerWidget* parent) {
    return std::make_unique<detail::SpacerItem>(*_context, parent);
}

std::unique_ptr<ui::PanelWidget> Backend::createPanel(ui::ContainerWidget* parent) {
    return std::make_unique<detail::PanelBox>(*_context, parent);
}

std::unique_ptr<ui::ScrollWidget> Backend::createScroll(ui::ContainerWidget* parent, ui::Axis axis) {
    return std::make_unique<detail::ScrollArea>(*_context, parent, axis);
}

std::unique_ptr<ui::SlotWidget> Backend::createSlot(ui::ContainerWidget* parent) {
    return std::make_unique<detail::SlotBox>(*_context, parent);
}

std::unique_ptr<ui::TabsWidget> Backend::createTabs(ui::ContainerWidget* parent) {
    return std::make_unique<detail::TabBox>(*_context, parent);
}

std::unique_ptr<ui::DialogWidget> Backend::createDialog(ui::ContainerWidget* parent) {
    return std::make_unique<detail::DialogBox>(*_context, parent);
}

std::unique_ptr<ui::BusyWidget> Backend::createBusy(ui::ContainerWidget* parent) {
    return std::make_unique<detail::BusyBox>(*_context, parent);
}

std::unique_ptr<ui::TableWidget> Backend::createTable(ui::ContainerWidget* parent) {
    return std::make_unique<detail::TableBox>(*_context, parent);
}

std::unique_ptr<ui::DateTimeInputWidget> Backend::createDateTimeInput(ui::ContainerWidget* parent, ui::DateMode mode,
                                                                      int offsetMinutes) {
    return std::make_unique<detail::DateTimeEntry>(*_context, parent, mode, offsetMinutes);
}

std::unique_ptr<ui::SliderWidget> Backend::createSlider(ui::ContainerWidget* parent) {
    return std::make_unique<detail::SliderControl>(*_context, parent);
}

std::unique_ptr<ui::FilePickerWidget> Backend::createFilePicker(ui::ContainerWidget* parent, ui::FilePickerMode mode) {
    return std::make_unique<detail::FilePickerBox>(*_context, parent, mode);
}

QQuickItem* itemOf(ui::Widget const& widget) noexcept {
    detail::ItemHolder const* const holder = detail::holderOf(widget);
    return holder == nullptr ? nullptr : holder->item();
}

ui::Widget* widgetOf(QQuickItem const& item) noexcept { return detail::widgetOf(item); }

}  // namespace morph::qt_quick
```

In `src/qt_quick/CMakeLists.txt`, add `backend.cpp` to `add_library(morph_qt_quick STATIC ...)` and, after
`add_library(morph::qt_quick ALIAS morph_qt_quick)`:

```cmake
# The public headers. The root CMakeLists.txt's guard requires every header
# under include/morph/qt_quick/ to be listed here; VERIFY_INTERFACE_HEADER_SETS
# compiles each one standalone.
target_sources(morph_qt_quick
    PUBLIC
    FILE_SET HEADERS
    BASE_DIRS "${PROJECT_SOURCE_DIR}/include"
    FILES
        "${PROJECT_SOURCE_DIR}/include/morph/qt_quick/backend.hpp"
)
```

- [ ] **Step 4: Run the tests to verify they pass**

```bash
cmake --build build/qt && ./build/qt/tests/qt_quick/morph_qt_quick_tests "[backend]"
```

Expected: PASS; the full build also compiles `backend.hpp` standalone (`morph_qt_quick_verify_interface_header_sets`).

Mutation checks (one at a time, then restore):
- Make `ContainerHolder::moveChildItem` return at once → expected FAIL in the conformance suite's ForEach-reorder
  section (and Task 2's reorder case).
- In `Backend::createSelect`, ignore `style` (always `Dropdown`) → expected FAIL in "every factory makes its MorphUi
  component" (`RadioGroup.qml` expected).
- Drop `backend.hpp` from the `FILE_SET` and re-configure → expected configure FAIL from the public-header guard
  naming it. Restore.

- [ ] **Step 5: Commit**

```bash
git add include/morph/qt_quick src/qt_quick tests/qt_quick
git commit -m "wip(qtquick): qt_quick::Backend, passing the backend conformance suite

Signed-off-by: Christian Parpart <christian@parpart.family>"
```

---

### Task 9: The `QTimer` scheduler, `qt_quick::Frontend` and `frontendOption`

**Files:**
- Create: `src/qt_quick/scheduler.hpp`, `scheduler.cpp`, `frontend.cpp`, `include/morph/qt_quick/frontend.hpp`
- Modify: `src/qt_quick/CMakeLists.txt` — `frontend.cpp scheduler.cpp scheduler.hpp` in `morph_qt_quick`;
  `include/morph/qt_quick/frontend.hpp` in the `FILE_SET`
- Create: `tests/qt_quick/frontend_test_main.cpp`
- Modify: `tests/qt_quick/CMakeLists.txt` — `test_scheduler.cpp` in `morph_qt_quick_tests`; the frontend suite
- Test: `tests/qt_quick/test_scheduler.cpp`, `tests/qt_quick/test_frontend.cpp`

**Interfaces:**
- Consumes: `reactive::Scheduler`, `reactive::TimerHandle` (Part 1, `reactive/scheduler.hpp`);
  `reactive::detail::site::kRuntimeOutlived` (Part 1, `reactive/detail/graph.hpp`); `ui::Frontend`,
  `ui::AppContext`, `ui::Application`, `ui::ApplicationFactory`, `ui::FrontendOption`, `ui::EnvironmentReader`,
  `ui::processEnvironment`, `ui::selectFrontend`, `ui::Mounted` (Part 2); `qt::QtExecutor`;
  `exec::runningOn` (`include/morph/core/executor.hpp:132`); `morph::testing::OwnerProbeRecorder`
  (`tests/owner_probe_recorder.hpp:26`).
- Produces:
  - `morph::qt_quick::Frontend(int& argc, char** argv)` — `name()` is `"qt"`.
  - `morph::qt_quick::frontendOption(int& argc, char** argv, ui::EnvironmentReader environment =
    ui::processEnvironment())` — **deviation:** the contract's two-parameter form plus a defaulted reader, the same
    seam `ui::selectFrontend` has, so `usable()` is testable without touching the process environment. Calls
    written against the contract compile unchanged.
  - `detail::QtScheduler(exec::IExecutor& owner)`.

**Design notes.** `run` builds, in order, `QGuiApplication`, the Basic style (unless `QT_QUICK_CONTROLS_STYLE` or
the application chose one), the `QtExecutor` (the runtime's owner and every bridge's callback executor), the
engine, the window, the backend, the scheduler, the `reactive::Runtime` and the context; then calls the factory,
mounts `view()` into the window's content, shows the window and runs `exec()`. Teardown is the reverse, so the
mount dies first, then the application, then the runtime, then everything Qt. `quit(code)` posts
`QCoreApplication::exit(code)`, so a quit before the loop runs (from the factory) ends it as soon as it starts;
closing the window quits with 0 (`quitOnLastWindowClosed`). Under WebAssembly `exec()` hands control to the browser
and never returns. `RuntimeOptions::afterFlush` is unused: Qt repaints changed items itself.

- [ ] **Step 1: Write the failing tests**

Create `tests/qt_quick/test_scheduler.cpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0

#include <QCoreApplication>
#include <QTest>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <morph/core/executor.hpp>
#include <morph/core/logger.hpp>
#include <morph/qt/qt_executor.hpp>
#include <morph/reactive/scheduler.hpp>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "scheduler.hpp"

namespace {

using morph::qt::QtExecutor;
using morph::qt_quick::detail::QtScheduler;
using morph::reactive::TimerHandle;
using namespace std::chrono_literals;

/// Waits until a fresh timer of @p delay fires: by then, anything due earlier has had its chance too.
void waitPast(QtScheduler& scheduler, std::chrono::milliseconds delay) {
    bool fired = false;
    TimerHandle const clock = scheduler.after(delay, [&fired] { fired = true; });
    REQUIRE(QTest::qWaitFor([&fired] { return fired; }));
}

}  // namespace

TEST_CASE("qt_quick QtScheduler: after fires once, on the runtime's owner", "[qt_quick][scheduler]") {
    QtExecutor owner{QCoreApplication::instance()};
    QtScheduler scheduler{owner};
    int fired = 0;
    bool onOwner = false;
    TimerHandle const once = scheduler.after(5ms, [&] {
        ++fired;
        onOwner = morph::exec::runningOn(owner);
    });
    REQUIRE(QTest::qWaitFor([&fired] { return fired == 1; }));
    CHECK(onOwner);
    waitPast(scheduler, 30ms);
    CHECK(fired == 1);
}

TEST_CASE("qt_quick QtScheduler: every repeats until cancelled", "[qt_quick][scheduler]") {
    QtExecutor owner{QCoreApplication::instance()};
    QtScheduler scheduler{owner};
    int fired = 0;
    TimerHandle repeating = scheduler.every(5ms, [&fired] { ++fired; });
    REQUIRE(QTest::qWaitFor([&fired] { return fired >= 3; }));
    repeating.cancel();
    CHECK_FALSE(repeating.active());
    int const seen = fired;
    waitPast(scheduler, 30ms);
    CHECK(fired == seen);
}

TEST_CASE("qt_quick QtScheduler: destroying the handle before the deadline cancels", "[qt_quick][scheduler]") {
    QtExecutor owner{QCoreApplication::instance()};
    QtScheduler scheduler{owner};
    int fired = 0;
    {
        TimerHandle const transient = scheduler.after(5ms, [&fired] { ++fired; });
    }
    waitPast(scheduler, 30ms);
    CHECK(fired == 0);
}

TEST_CASE("qt_quick QtScheduler: a throwing callback is logged; a non-positive period is refused",
          "[qt_quick][scheduler]") {
    QtExecutor owner{QCoreApplication::instance()};
    QtScheduler scheduler{owner};
    std::vector<std::string> errors;
    morph::log::ScopedLoggerOverride const guard{[&errors](morph::log::LogLevel level, std::string_view message) {
        if (level == morph::log::LogLevel::error) {
            errors.emplace_back(message);
        }
    }};
    TimerHandle const boom = scheduler.after(0ms, [] { throw std::runtime_error{"boom"}; });
    REQUIRE(QTest::qWaitFor([&errors] { return !errors.empty(); }));
    CHECK(errors.front().find("boom") != std::string::npos);
    CHECK_THROWS_AS(scheduler.every(0ms, [] {}), std::invalid_argument);
}

TEST_CASE("qt_quick QtScheduler: a handle outliving its scheduler is safe", "[qt_quick][scheduler]") {
    QtExecutor owner{QCoreApplication::instance()};
    TimerHandle handle;
    {
        QtScheduler scheduler{owner};
        handle = scheduler.every(5ms, [] {});
    }
    handle.cancel();  // the timer died with the scheduler; ASan observes
    CHECK_FALSE(handle.active());
}
```

Create `tests/qt_quick/test_frontend.cpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0

#include <QCoreApplication>
#include <QGuiApplication>
#include <QQuickItem>
#include <QQuickWindow>
#include <QString>
#include <QWindow>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <functional>
#include <map>
#include <memory>
#include <morph/core/executor.hpp>
#include <morph/qt_quick/frontend.hpp>
#include <morph/reactive/detail/graph.hpp>
#include <morph/reactive/scheduler.hpp>
#include <morph/reactive/signal.hpp>
#include <morph/ui/frontend.hpp>
#include <morph/ui/view.hpp>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include "owner_probe_recorder.hpp"

namespace {

using morph::qt_quick::Frontend;
using morph::qt_quick::frontendOption;
using namespace std::chrono_literals;
namespace ui = morph::ui;

/// argc/argv as a program receives them.
struct Arguments {
    std::string program = "frontend_test";
    std::array<char*, 2> argv{program.data(), nullptr};
    int argc = 1;
};

QQuickItem* findWidgetItem(char const* name) {
    for (QWindow* const window : QGuiApplication::topLevelWindows()) {
        if (auto* const quick = qobject_cast<QQuickWindow*>(window)) {
            if (auto* const item = quick->contentItem()->findChild<QQuickItem*>(QString::fromLatin1(name))) {
                return item;
            }
        }
    }
    return nullptr;
}

struct Observed {
    std::string shownText;
    bool ranOnOwner = false;
    std::string frontendName;
    bool ioLoopIsNull = false;
    bool destroyed = false;
    bool qtAliveAtDestruction = false;
    bool itemGoneAtDestruction = false;
};

/// Shows "hello 0"; once the loop runs, records what it sees and quits with its code.
class ProbeApp final : public ui::Application {
public:
    ProbeApp(ui::AppContext& context, Observed& observed, int exitCode)
        : _context{&context},
          _observed{&observed},
          _counter{context.runtime(), 0},
          _inspect{context.scheduler().after(0ms, [this, exitCode] { inspect(exitCode); })} {}

    ~ProbeApp() override {
        _observed->destroyed = true;
        _observed->qtAliveAtDestruction = QCoreApplication::instance() != nullptr;
        _observed->itemGoneAtDestruction = findWidgetItem("w1") == nullptr;
    }

    ProbeApp(ProbeApp const&) = delete;
    ProbeApp& operator=(ProbeApp const&) = delete;
    ProbeApp(ProbeApp&&) = delete;
    ProbeApp& operator=(ProbeApp&&) = delete;

    [[nodiscard]] ui::Node view() override {
        return ui::text({.text = [this] { return "hello " + std::to_string(_counter.get()); }});
    }

private:
    void inspect(int exitCode) {
        QQuickItem const* const item = findWidgetItem("w1");
        _observed->shownText = item != nullptr ? item->property("text").toString().toStdString() : std::string{};
        _observed->ranOnOwner = morph::exec::runningOn(_context->executor());
        _observed->frontendName = std::string{_context->frontendName()};
        _observed->ioLoopIsNull = _context->ioLoop() == nullptr;
        _context->quit(exitCode);
    }

    ui::AppContext* _context;
    Observed* _observed;
    morph::reactive::Signal<int> _counter;
    morph::reactive::TimerHandle _inspect;
};

class QuitAtOnce final : public ui::Application {
public:
    explicit QuitAtOnce(ui::AppContext& context) { context.quit(5); }
    [[nodiscard]] ui::Node view() override { return ui::spacer(); }
};

ui::EnvironmentReader environment(std::map<std::string, std::string, std::less<>> variables) {
    return [variables = std::move(variables)](std::string_view name) -> std::optional<std::string> {
        if (auto const found = variables.find(name); found != variables.end()) {
            return found->second;
        }
        return std::nullopt;
    };
}

}  // namespace

TEST_CASE("qt_quick Frontend: run mounts the factory's view and returns quit's code", "[qt_quick][frontend]") {
    Arguments arguments;
    Frontend frontend{arguments.argc, arguments.argv.data()};
    Observed observed;
    int const code = frontend.run(
        [&observed](ui::AppContext& context) { return std::make_unique<ProbeApp>(context, observed, 7); });
    CHECK(code == 7);
    CHECK(observed.shownText == "hello 0");
    CHECK(observed.ranOnOwner);
    CHECK(observed.frontendName == "qt");
    CHECK(observed.ioLoopIsNull);
    CHECK(observed.destroyed);
    CHECK(QCoreApplication::instance() == nullptr);
}

TEST_CASE("qt_quick Frontend: a quit from inside the factory still ends run with its code", "[qt_quick][frontend]") {
    Arguments arguments;
    Frontend frontend{arguments.argc, arguments.argv.data()};
    CHECK(frontend.run([](ui::AppContext& context) { return std::make_unique<QuitAtOnce>(context); }) == 5);
}

TEST_CASE("qt_quick Frontend: the mount dies before the application, the application before the runtime, Qt last",
          "[qt_quick][frontend]") {
    morph::exec::MainThreadExecutor reference;
    morph::testing::OwnerProbeRecorder const probe{reference.coreExecutor()};
    Arguments arguments;
    Frontend frontend{arguments.argc, arguments.argv.data()};
    Observed observed;
    CHECK(frontend.run([&observed](ui::AppContext& context) {
              return std::make_unique<ProbeApp>(context, observed, 0);
          }) == 0);
    REQUIRE(observed.destroyed);
    CHECK(observed.itemGoneAtDestruction);
    CHECK(observed.qtAliveAtDestruction);
    CHECK(probe.count(morph::reactive::detail::site::kRuntimeOutlived) == 0);
}

TEST_CASE("qt_quick Frontend: run refuses to share an existing application object", "[qt_quick][frontend]") {
    Arguments arguments;
    QCoreApplication const existing{arguments.argc, arguments.argv.data()};
    Frontend frontend{arguments.argc, arguments.argv.data()};
    CHECK_THROWS_AS(frontend.run([](ui::AppContext& context) { return std::make_unique<QuitAtOnce>(context); }),
                    std::logic_error);
}

TEST_CASE("qt_quick frontendOption: named qt, makes the frontend, usable follows the environment",
          "[qt_quick][frontend]") {
    Arguments arguments;
    ui::FrontendOption const bare = frontendOption(arguments.argc, arguments.argv.data(), environment({}));
    CHECK(bare.name == "qt");
    std::unique_ptr<ui::Frontend> const made = bare.make();
    REQUIRE(made != nullptr);
    CHECK(made->name() == "qt");
    static_cast<void>(frontendOption(arguments.argc, arguments.argv.data()).usable());
#if defined(__APPLE__) || defined(_WIN32)
    CHECK(bare.usable());
#else
    CHECK_FALSE(bare.usable());
    CHECK(frontendOption(arguments.argc, arguments.argv.data(), environment({{"DISPLAY", ":0"}})).usable());
    CHECK(frontendOption(arguments.argc, arguments.argv.data(), environment({{"WAYLAND_DISPLAY", "wayland-0"}}))
              .usable());
    CHECK(frontendOption(arguments.argc, arguments.argv.data(), environment({{"QT_QPA_PLATFORM", "offscreen"}}))
              .usable());
    CHECK_FALSE(frontendOption(arguments.argc, arguments.argv.data(), environment({{"DISPLAY", ""}})).usable());
#endif
}

TEST_CASE("qt_quick frontendOption: --ui=qt selects it", "[qt_quick][frontend]") {
    Arguments arguments;
    std::array const options{frontendOption(arguments.argc, arguments.argv.data(), environment({}))};
    std::array<char const*, 2> const args{"app", "--ui=qt"};
    std::unique_ptr<ui::Frontend> const chosen = ui::selectFrontend(options, 2, args.data(), environment({}));
    REQUIRE(chosen != nullptr);
    CHECK(chosen->name() == "qt");
}
```

Create `tests/qt_quick/frontend_test_main.cpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0

#include <QQuickWindow>
#include <QSGRendererInterface>
#include <QtGlobal>
#include <catch2/catch_session.hpp>
#include <testkit/log_level.hpp>

// No QGuiApplication here: qt_quick::Frontend::run makes its own, one per run.
// ctest runs each case in its own process (catch_discover_tests).
int main(int argc, char* argv[]) {
    if (!qEnvironmentVariableIsSet("QT_QPA_PLATFORM")) {
        qputenv("QT_QPA_PLATFORM", "offscreen");
    }
    QQuickWindow::setGraphicsApi(QSGRendererInterface::Software);
    Catch::Session session;
    return morph::testkit::runSession(session, argc, argv);
}
```

Append to `tests/qt_quick/CMakeLists.txt`, and add `test_scheduler.cpp` to `morph_qt_quick_tests`:

```cmake
morph_qt_quick_suite(morph_qt_quick_frontend_tests frontend_test_main.cpp
    test_frontend.cpp
)
```

- [ ] **Step 2: Run them to verify they fail**

Run: `cmake --build build/qt --target morph_qt_quick_tests morph_qt_quick_frontend_tests`
Expected: FAIL — `'scheduler.hpp' file not found` and `'morph/qt_quick/frontend.hpp' file not found`.

- [ ] **Step 3: Implement**

Create `src/qt_quick/scheduler.hpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0

#pragma once
#include <QObject>
#include <chrono>
#include <functional>
#include <memory>
#include <morph/core/executor.hpp>
#include <morph/reactive/scheduler.hpp>

/// @file
/// @brief The Qt Quick frontend's `reactive::Scheduler`: one `QTimer` per timer.

namespace morph::qt_quick::detail {

/// @brief Timers on the GUI thread's event loop; callbacks run inside a scope naming the runtime's owner.
class QtScheduler final : public reactive::Scheduler {
public:
    /// @param owner The runtime's owner executor (the frontend's `QtExecutor`); must outlive this scheduler.
    explicit QtScheduler(exec::IExecutor& owner);
    /// @brief Stops and deletes every timer; handles still held become inert.
    ~QtScheduler() override;
    QtScheduler(QtScheduler const&) = delete;
    QtScheduler& operator=(QtScheduler const&) = delete;
    QtScheduler(QtScheduler&&) = delete;
    QtScheduler& operator=(QtScheduler&&) = delete;

    [[nodiscard]] reactive::TimerHandle after(std::chrono::milliseconds delay, std::function<void()> fn) override;
    [[nodiscard]] reactive::TimerHandle every(std::chrono::milliseconds period, std::function<void()> fn) override;

private:
    reactive::TimerHandle start(std::chrono::milliseconds interval, bool singleShot, std::function<void()> fn);

    exec::IExecutor* _owner;
    // The parent of every timer, so destroying the scheduler destroys them.
    std::unique_ptr<QObject> _timers = std::make_unique<QObject>();
};

}  // namespace morph::qt_quick::detail
```

Create `src/qt_quick/scheduler.cpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0

#include "scheduler.hpp"

#include <QPointer>
#include <QTimer>
#include <algorithm>
#include <core/async/ExecutorContext.hpp>
#include <exception>
#include <morph/core/logger.hpp>
#include <stdexcept>
#include <string>
#include <utility>

namespace morph::qt_quick::detail {

QtScheduler::QtScheduler(exec::IExecutor& owner) : _owner{&owner} {}

QtScheduler::~QtScheduler() = default;

reactive::TimerHandle QtScheduler::after(std::chrono::milliseconds delay, std::function<void()> fn) {
    return start(std::max(delay, std::chrono::milliseconds{0}), true, std::move(fn));
}

reactive::TimerHandle QtScheduler::every(std::chrono::milliseconds period, std::function<void()> fn) {
    if (period.count() <= 0) {
        throw std::invalid_argument{"qt_quick::QtScheduler::every: the period must be positive"};
    }
    return start(period, false, std::move(fn));
}

reactive::TimerHandle QtScheduler::start(std::chrono::milliseconds interval, bool singleShot,
                                         std::function<void()> fn) {
    // NOLINTNEXTLINE(cppcoreguidelines-owning-memory): owned by its QObject parent, _timers.
    auto* const timer = new QTimer{_timers.get()};
    timer->setSingleShot(singleShot);
    QObject::connect(timer, &QTimer::timeout, timer, [owner = _owner, fn = std::move(fn)] {
        ::core::async::ExecutorScope const scope{owner->coreExecutor()};
        // An exception must not unwind through Qt's event dispatch.
        try {
            fn();
        } catch (std::exception const& error) {
            morph::log::logError(std::string{"[qt_quick] timer callback threw: "} + error.what());
        } catch (...) {
            morph::log::logError("[qt_quick] timer callback threw a non-standard exception");
        }
    });
    timer->start(interval);
    return reactive::TimerHandle{[guard = QPointer<QTimer>{timer}] {
        if (!guard.isNull()) {
            guard->stop();
            // Later, not now: a callback may cancel its own timer while the timer is emitting.
            guard->deleteLater();
        }
    }};
}

}  // namespace morph::qt_quick::detail
```

Create `include/morph/qt_quick/frontend.hpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0

#pragma once
#include <morph/ui/frontend.hpp>
#include <string_view>

/// @file
/// @brief `morph::qt_quick::Frontend`: runs a `ui::Application` in a Qt Quick window.
///
/// Specified in `docs/spec/qt_quick/frontend.md`.

namespace morph::qt_quick {

/// @brief The Qt Quick frontend: one window, one `QGuiApplication`, the application's runtime on a `QtExecutor`.
///
/// `run` owns Qt for its duration: it makes the `QGuiApplication`, so none may exist when it is called. Under
/// WebAssembly `run` hands control to the browser and does not return.
class Frontend final : public ui::Frontend {
public:
    /// @param argc The program's argument count; Qt removes the arguments it consumes. Must outlive `run`.
    /// @param argv The program's arguments; must outlive `run`.
    Frontend(int& argc, char** argv);

    /// @brief This frontend's name for `ui::selectFrontend`.
    /// @return `"qt"`.
    [[nodiscard]] std::string_view name() const override;

    /// @brief Runs the application until it quits or its window is closed.
    /// @param factory Makes the application with this frontend's `ui::AppContext`.
    /// @return The code given to `quit`, or 0 when the window was closed.
    /// @throws std::logic_error when a `QCoreApplication` already exists; whatever the factory or the mount throws.
    int run(ui::ApplicationFactory const& factory) override;

private:
    int* _argc;
    char** _argv;
};

/// @brief This frontend as a `ui::selectFrontend` option.
/// @param argc The program's argument count; must outlive the frontend the option makes.
/// @param argv The program's arguments; must outlive the frontend the option makes.
/// @param environment What `usable()` reads; the process environment unless a test passes another.
/// @return The option named `"qt"`, whose `usable()` holds on macOS, Windows and WebAssembly, and elsewhere when
///         `DISPLAY`, `WAYLAND_DISPLAY` or `QT_QPA_PLATFORM` is set and not empty.
[[nodiscard]] ui::FrontendOption frontendOption(int& argc, char** argv,
                                                ui::EnvironmentReader environment = ui::processEnvironment());

}  // namespace morph::qt_quick
```

Create `src/qt_quick/frontend.cpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0

#include <QCoreApplication>
#include <QGuiApplication>
#include <QMetaObject>
#include <QQmlEngine>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QString>
#include <QtGlobal>
#include <memory>
#include <morph/qt/qt_executor.hpp>
#include <morph/qt_quick/backend.hpp>
#include <morph/qt_quick/frontend.hpp>
#include <morph/reactive/runtime.hpp>
#include <morph/ui/mount.hpp>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include "scheduler.hpp"
#include "window.hpp"

namespace morph::qt_quick {

namespace {

class QtAppContext final : public ui::AppContext {
public:
    QtAppContext(reactive::Runtime& runtime, exec::IExecutor& executor, ui::Scheduler& scheduler)
        : _runtime{&runtime}, _executor{&executor}, _scheduler{&scheduler} {}

    reactive::Runtime& runtime() override { return *_runtime; }
    exec::IExecutor& executor() override { return *_executor; }
    ui::Scheduler& scheduler() override { return *_scheduler; }
    exec::IoLoop* ioLoop() override { return nullptr; }

    void quit(int exitCode) override {
        // Posted, so a quit before the loop runs (from the factory) ends the loop as soon as it starts.
        QMetaObject::invokeMethod(
            QCoreApplication::instance(), [exitCode] { QCoreApplication::exit(exitCode); }, Qt::QueuedConnection);
    }

    [[nodiscard]] std::string_view frontendName() const override { return "qt"; }

private:
    reactive::Runtime* _runtime;
    exec::IExecutor* _executor;
    ui::Scheduler* _scheduler;
};

bool displayAvailable([[maybe_unused]] ui::EnvironmentReader const& environment) {
#if defined(__APPLE__) || defined(_WIN32) || defined(__EMSCRIPTEN__)
    return true;
#else
    auto const set = [&environment](std::string_view name) {
        std::optional<std::string> const value = environment(name);
        return value.has_value() && !value->empty();
    };
    return set("DISPLAY") || set("WAYLAND_DISPLAY") || set("QT_QPA_PLATFORM");
#endif
}

}  // namespace

Frontend::Frontend(int& argc, char** argv) : _argc{&argc}, _argv{argv} {}

std::string_view Frontend::name() const { return "qt"; }

int Frontend::run(ui::ApplicationFactory const& factory) {
    if (QCoreApplication::instance() != nullptr) {
        throw std::logic_error{"qt_quick::Frontend::run: a QCoreApplication already exists; the frontend makes its own"};
    }
    // Declaration order is teardown order, reversed: the mount, the application, the runtime, then Qt.
    QGuiApplication application{*_argc, _argv};
    if (qEnvironmentVariableIsEmpty("QT_QUICK_CONTROLS_STYLE") && QQuickStyle::name().isEmpty()) {
        QQuickStyle::setStyle(QStringLiteral("Basic"));
    }
    qt::QtExecutor executor{&application};
    QQmlEngine engine;
    std::unique_ptr<QQuickWindow> const window = detail::createWindow(engine);
    Backend backend{engine, detail::windowContent(*window)};
    detail::QtScheduler scheduler{executor};
    reactive::Runtime runtime{executor};
    QtAppContext context{runtime, executor, scheduler};

    std::unique_ptr<ui::Application> app = factory(context);
    if (app == nullptr) {
        throw std::invalid_argument{"qt_quick::Frontend::run: the factory made no application"};
    }
    int code = 0;
    {
        ui::Mounted const mounted{runtime, backend, app->view()};
        window->show();
        code = QGuiApplication::exec();
    }
    app.reset();
    return code;
}

ui::FrontendOption frontendOption(int& argc, char** argv, ui::EnvironmentReader environment) {
    return ui::FrontendOption{
        .name = "qt",
        .usable = [environment = std::move(environment)] { return displayAvailable(environment); },
        .make = [&argc, argv]() -> std::unique_ptr<ui::Frontend> { return std::make_unique<Frontend>(argc, argv); },
    };
}

}  // namespace morph::qt_quick
```

In `src/qt_quick/CMakeLists.txt`, add `frontend.cpp scheduler.cpp scheduler.hpp` to `morph_qt_quick` and
`"${PROJECT_SOURCE_DIR}/include/morph/qt_quick/frontend.hpp"` to its `FILE_SET HEADERS` after `backend.hpp`.

- [ ] **Step 4: Run the tests to verify they pass**

```bash
cmake --build build/qt --target morph_qt_quick_tests morph_qt_quick_frontend_tests
./build/qt/tests/qt_quick/morph_qt_quick_tests "[scheduler]"
ctest --test-dir build/qt -L qt_quick --output-on-failure
```

Expected: PASS. `ctest` runs each frontend case in its own process, as CI does; also run
`./build/qt/tests/qt_quick/morph_qt_quick_frontend_tests` once directly — it makes several `QGuiApplication`s in
turn in one process — and if only that run fails, record it in this part's squashed commit body as a finding
rather than masking it.

Mutation checks (one at a time, then restore):
- In `Frontend::run`, declare `std::unique_ptr<ui::Application> app;` as the first line after the
  `QCoreApplication` check, assign it with `app = factory(context);`, and delete `app.reset();` — the application
  now outlives the runtime and Qt → expected FAIL in "the mount dies before the application, the application before
  the runtime, Qt last" (`kRuntimeOutlived` reported, `qtAliveAtDestruction` false).
- In `QtAppContext::quit`, call `QCoreApplication::exit(exitCode)` directly → expected FAIL (timeout) in "a quit from
  inside the factory still ends run with its code".
- In `QtScheduler::start`, drop the `ExecutorScope` → expected FAIL in "after fires once, on the runtime's owner".

- [ ] **Step 5: Commit**

```bash
git add include/morph/qt_quick src/qt_quick tests/qt_quick
git commit -m "wip(qtquick): qt_quick::Frontend, its QTimer scheduler and frontendOption

Signed-off-by: Christian Parpart <christian@parpart.family>"
```

---
### Task 10: Install, export, presets and CI

**Files:**
- Modify: `CMakeLists.txt` (root) — a `morph_qt_quick` install block after the `if(TARGET morph_qt_impl)` block;
  the package version file's `ARCH_INDEPENDENT` condition
- Modify: `cmake/morphConfig.cmake.in` — the `qt_quick` dependency block after the `qt_forms` block
- Modify: `CMakePresets.json` — `windows-everything` and `linux-everything`
- Modify: `.github/workflows/ci.yml` — five configure steps; a new last job `install-export-qt-quick`
- Modify: `.github/workflows/nightly-slow-checks.yml` — the all-features configure
- Modify: `.github/workflows/wasm-ladder.yml` — both `paths:` lists, the configure, the named-target build
- Create: `scripts/check_qt_quick_install.sh`
- Test: `scripts/check_qt_quick_install.sh` (it builds and runs a consumer against the install)

**Interfaces:**
- Consumes: `MORPH_QT_QUICK_QML_OUTPUT_TARGETS` (Task 1); the root install section's `MORPH_INSTALLED_COMPONENTS`
  (root `CMakeLists.txt`, "Install & export"); Part 3's conditional `ARCH_INDEPENDENT`.
- Produces: install component `qt_quick` exporting `morph::qt_quick` and `morph::qt_quick_qml` (plus the module's
  object libraries, exported as `morph::qt_quick_qml_…`); `find_package(morph COMPONENTS qt_quick)`.

The `foreach(_morph_component IN ITEMS offline_sqlite qt_forms qt net)` line stays exactly as it is:
`scripts/test_check_install_export.sh` matches it textually. `morph_qt_quick` gets its own block, as
`morph_qt_impl` and the MorphForms module do, because it installs an archive and object libraries besides headers.

- [ ] **Step 1: Write the failing check**

Create `scripts/check_qt_quick_install.sh` (and `chmod +x` it):

```bash
#!/usr/bin/env bash
# Usage: bash scripts/check_qt_quick_install.sh [REPO_ROOT]
#
# Fails unless an application outside the tree can run morph::qt_quick from an
# install: find_package(morph CONFIG REQUIRED COMPONENTS qt_quick), link
# morph::qt_quick, run a ui::Application through qt_quick::Frontend on the
# offscreen platform, and find its view rendered by a MorphUi component.
#
# Needs Qt 6.5+ with Quick, QuickControls2 and WebSockets (morph::qt_quick links
# morph::qt), found the usual way (Qt6_DIR or CMAKE_PREFIX_PATH). MorphUi is a
# static QML module whose compiled resources live in object libraries the
# install has to carry: without them everything still links and the
# application fails at run time, so the consumer is run, not only built. The
# package version file must not claim ARCH_INDEPENDENT once a compiled
# component is installed.
set -euo pipefail

repo_root="${1:-$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)}"
repo_root="$(cd "$repo_root" && pwd)"
if command -v cygpath >/dev/null 2>&1; then
    repo_root="$(cygpath -m "$repo_root")"
fi
readonly repo_root

failures=0
note() { printf 'ok: %s\n' "$*"; }
fail() { printf 'error: %s\n' "$*" >&2; failures=$((failures + 1)); }

run_step() {
    local description="$1"; shift
    local output
    if output="$("$@" 2>&1)"; then
        return 0
    fi
    fail "${description}"
    printf '%s\n' "$output" >&2
    return 1
}

finish() {
    if [ "$failures" -ne 0 ]; then
        printf '\n%d qt_quick install check(s) failed.\n' "$failures" >&2
        exit 1
    fi
}

workspace="$(mktemp -d)"
trap 'rm -rf "$workspace" || true' EXIT
if command -v cygpath >/dev/null 2>&1; then
    workspace="$(cygpath -m "$workspace")"
fi
readonly build_dir="${workspace}/build"
readonly prefix="${workspace}/prefix"

generator_args=()
if command -v ninja >/dev/null 2>&1; then
    generator_args=(-G Ninja)
fi

# ── 1. Build and install the component ──────────────────────────────────────
run_step "the library did not configure with MORPH_BUILD_QT_QUICK=ON" \
    cmake -S "$repo_root" -B "$build_dir" ${generator_args[@]+"${generator_args[@]}"} \
        -DCMAKE_BUILD_TYPE=Release \
        -DMORPH_BUILD_TESTS=OFF \
        -DMORPH_BUILD_EXAMPLES=OFF \
        -DMORPH_BUILD_QT=ON \
        -DMORPH_BUILD_QT_QUICK=ON || finish
run_step "morph::qt_quick did not build" cmake --build "$build_dir" || finish
run_step "cmake --install failed" cmake --install "$build_dir" --prefix "$prefix" || finish

# ── 2. The prefix must hold the component ───────────────────────────────────
libdir="lib"
if [ ! -f "${prefix}/lib/cmake/morph/morphConfig.cmake" ] && [ -d "${prefix}/lib64/cmake/morph" ]; then
    libdir="lib64"
fi
for required in \
    "${libdir}/cmake/morph/morphTargets.cmake" \
    "${libdir}/cmake/morph/morphConfigVersion.cmake" \
    "include/morph/qt_quick/backend.hpp" \
    "include/morph/qt_quick/frontend.hpp" \
    "include/morph/qt/qt_executor.hpp"; do
    if [ ! -f "${prefix}/${required}" ]; then
        fail "the install has no ${required}"
    fi
done
readonly targets_file="${prefix}/${libdir}/cmake/morph/morphTargets.cmake"
if [ -f "$targets_file" ]; then
    for exported in morph::qt_quick morph::qt_quick_qml; do
        grep -q "^add_library(${exported} " "$targets_file" || fail "${exported} is not exported"
    done
    if grep -q '^add_library(morph::morph_' "$targets_file"; then
        fail "a target is exported as '$(grep -o 'morph::morph_[a-z0-9_]*' "$targets_file" | head -1)' -- give it an EXPORT_NAME"
    fi
fi
if [ -z "$(find "${prefix}/${libdir}" -path '*qt_quick_qml_resources*' -type f | head -1)" ]; then
    fail "the install carries no object file of the MorphUi module's resources"
fi
readonly version_file="${prefix}/${libdir}/cmake/morph/morphConfigVersion.cmake"
if [ -f "$version_file" ] && ! grep -q 'CMAKE_SIZEOF_VOID_P' "$version_file"; then
    fail "morphConfigVersion.cmake is ARCH_INDEPENDENT although it installs compiled code"
fi
finish
note "the prefix holds morph::qt_quick, the MorphUi module's object libraries and an architecture-checked version file"

# ── 3. An application runs from the install ─────────────────────────────────
readonly consumer="${workspace}/consumer"
mkdir -p "$consumer"
cat > "${consumer}/CMakeLists.txt" <<'EOF'
cmake_minimum_required(VERSION 3.25)
project(morph_qt_quick_consumer LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
find_package(morph CONFIG REQUIRED COMPONENTS qt_quick)
add_executable(consumer main.cpp)
target_link_libraries(consumer PRIVATE morph::qt_quick)
EOF
cat > "${consumer}/main.cpp" <<'EOF'
#include <morph/qt_quick/frontend.hpp>
#include <morph/reactive/scheduler.hpp>
#include <morph/ui/frontend.hpp>
#include <morph/ui/view.hpp>

#include <QGuiApplication>
#include <QQuickItem>
#include <QQuickWindow>
#include <QString>
#include <QWindow>
#include <QtGlobal>
#include <chrono>
#include <cstdio>
#include <memory>

namespace {

class Probe final : public morph::ui::Application {
public:
    explicit Probe(morph::ui::AppContext& context)
        : _context{&context},
          _check{context.scheduler().after(std::chrono::milliseconds{0}, [this] { check(); })} {}

    morph::ui::Node view() override { return morph::ui::text({.text = "installed"}); }

private:
    void check() {
        int code = 3;
        for (QWindow* const window : QGuiApplication::topLevelWindows()) {
            if (auto* const quick = qobject_cast<QQuickWindow*>(window)) {
                if (auto* const item = quick->contentItem()->findChild<QQuickItem*>(QStringLiteral("w1"))) {
                    QString const text = item->property("text").toString();
                    std::printf("morph::qt_quick from the install shows: %s\n", qPrintable(text));
                    code = text == QStringLiteral("installed") ? 0 : 4;
                }
            }
        }
        _context->quit(code);
    }

    morph::ui::AppContext* _context;
    morph::reactive::TimerHandle _check;
};

}  // namespace

int main(int argc, char** argv) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    morph::qt_quick::Frontend frontend{argc, argv};
    return frontend.run([](morph::ui::AppContext& context) { return std::make_unique<Probe>(context); });
}
EOF

if run_step "a consumer of morph::qt_quick did not build" \
    cmake -S "$consumer" -B "${consumer}/build" ${generator_args[@]+"${generator_args[@]}"} \
        -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="$prefix"; then
    if run_step "a consumer of morph::qt_quick did not build" cmake --build "${consumer}/build"; then
        if output="$("${consumer}/build/consumer" 2>&1)"; then
            note "$(printf '%s\n' "$output" | grep 'from the install shows' || echo 'consumer ran')"
        else
            fail "the consumer built but did not show its view through the installed MorphUi module"
            printf '%s\n' "$output" >&2
        fi
    fi
fi

finish
note "an application runs through morph::qt_quick from the install"
```

- [ ] **Step 2: Run it to verify it fails**

Run: `bash scripts/check_qt_quick_install.sh` (Qt 6.5+ with WebSockets on `CMAKE_PREFIX_PATH`).
Expected: FAIL — `the install has no include/morph/qt_quick/backend.hpp`, `morph::qt_quick is not exported`,
`morph::qt_quick_qml is not exported`, `... no object file of the MorphUi module's resources`, and (with Part 3's
condition not yet extended, on a build without `morph_tui`) `morphConfigVersion.cmake is ARCH_INDEPENDENT`.

- [ ] **Step 3: Implement**

In the root `CMakeLists.txt` install section, directly after the `if(TARGET morph_qt_impl) ... endif()` block:

```cmake
    # morph::qt_quick (MORPH_BUILD_QT_QUICK): the compiled frontend and its
    # headers, plus the private MorphUi QML module it is built on -- the
    # module's backing library and the object libraries carrying its compiled
    # resources. Without those the installed frontend links and then finds no
    # MorphUi component at run time. MorphUi is not installed as a QML import:
    # no application imports it; the frontend loads it from its own resources.
    if(TARGET morph_qt_quick)
        set_target_properties(morph_qt_quick PROPERTIES EXPORT_NAME qt_quick)
        set_target_properties(morph_qt_quick_qml PROPERTIES EXPORT_NAME qt_quick_qml)
        # A generated target's name starts with morph_qt_quick_qml_; exported
        # under that name it would read morph::morph_... downstream.
        foreach(_morph_qt_quick_target IN LISTS MORPH_QT_QUICK_QML_OUTPUT_TARGETS)
            string(REGEX REPLACE "^morph_" "" _morph_qt_quick_export_name "${_morph_qt_quick_target}")
            set_target_properties(${_morph_qt_quick_target} PROPERTIES EXPORT_NAME ${_morph_qt_quick_export_name})
        endforeach()
        install(TARGETS morph_qt_quick
            EXPORT morphTargets
            FILE_SET HEADERS DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}"
            ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
        )
        install(TARGETS morph_qt_quick_qml ${MORPH_QT_QUICK_QML_OUTPUT_TARGETS}
            EXPORT morphTargets
            ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
            OBJECTS DESTINATION "${CMAKE_INSTALL_LIBDIR}"
        )
        list(APPEND MORPH_INSTALLED_COMPONENTS qt_quick)
        unset(_morph_qt_quick_target)
        unset(_morph_qt_quick_export_name)
    endif()
```

The package version file: Part 3 made `ARCH_INDEPENDENT` depend on `morph_tui` not being installed. Read that
block and add `morph_qt_quick` to its condition. With Part 3's form

```cmake
    set(_morph_version_arch ARCH_INDEPENDENT)
    if(TARGET morph_tui)
        set(_morph_version_arch "")
    endif()
```

the condition becomes `if(TARGET morph_tui OR TARGET morph_qt_quick)`, and the comment above it names both
components ("A compiled component — morph::tui, morph::qt_quick — has an ABI to match, so an install that carries
one is not ARCH_INDEPENDENT."). If Part 3 wrote the condition in another shape, keep the shape and add
`OR TARGET morph_qt_quick`.

In `cmake/morphConfig.cmake.in`, after the `qt_forms` block:

```cmake
# The Qt Quick frontend: morph::qt_quick and the private MorphUi module it
# carries (morph::qt_quick_qml). It links morph::qt, whose block above resolves
# Qt WebSockets; Widgets only under Emscripten, for the browser file picker.
if("qt_quick" IN_LIST morph_KNOWN_COMPONENTS)
    find_dependency(Qt6 6.5 COMPONENTS Gui Qml Quick QuickControls2)
    if(EMSCRIPTEN)
        find_dependency(Qt6 6.5 COMPONENTS Widgets)
    endif()
endif()
```

In `CMakePresets.json`, in both `windows-everything` and `linux-everything`, add `"MORPH_BUILD_QT_QUICK": "ON",`
directly after `"MORPH_BUILD_QT": "ON",`.

In `.github/workflows/ci.yml`, add the line `-DMORPH_BUILD_QT_QUICK=ON \` directly after `-DMORPH_BUILD_QT=ON \` in
the configure step of each job that installs Qt `${{ env.QT_VERSION }}` (6.8.1) through `install-qt-action` and is
not scoped to one example: `linux-coverage`, `ladder-tests`, `ladder-sanitizers`, `linux-all-features` and
`clang-tidy`. Leave `linux-qt` alone — its distro Qt is 6.4.2, below the 6.5 floor — and `kanban-tsan` and
`bank-sanitizers`, which build one example each. The existing `QT_QPA_PLATFORM: offscreen` on those jobs' build and
test steps covers the new suites' discovery and runs. Append, as the last job:

```yaml
  install-export-qt-quick:
    name: Qt Quick frontend consumability
    runs-on: ubuntu-24.04
    steps:
      - uses: actions/checkout@v4

      - name: Install GCC 15, ninja and the Qt runtime libraries
        run: |
          sudo apt-get update -q
          sudo apt-get install -y software-properties-common
          sudo add-apt-repository -y ppa:ubuntu-toolchain-r/test
          sudo apt-get update -q
          sudo apt-get install -y gcc-15 g++-15 ninja-build \
            libgl1-mesa-dev libxkbcommon-x11-0 libxcb-cursor0 libxcb-icccm4 \
            libxcb-keysyms1 libxcb-shape0 libxcb-xinerama0

      # morph::qt_quick needs Qt 6.5+ Quick, which the runner's distro Qt (6.4.2)
      # is not, and links morph::qt, hence qtwebsockets.
      - name: Install Qt ${{ env.QT_VERSION }}
        uses: jurplel/install-qt-action@v4.3.1
        with:
          version: ${{ env.QT_VERSION }}
          modules: qtwebsockets
          cache: true

      # Installs morph with MORPH_BUILD_QT_QUICK, then builds and *runs* an
      # application against the prefix: the MorphUi module's resources travel in
      # object libraries, and only a run shows whether they arrived.
      - name: Install morph::qt_quick and run an application from the prefix
        env:
          CC: gcc-15
          CXX: g++-15
        run: bash scripts/check_qt_quick_install.sh
```

In `.github/workflows/nightly-slow-checks.yml`, add `-DMORPH_BUILD_QT_QUICK=ON \` after `-DMORPH_BUILD_QT=ON \` in the
all-features configure (the `cmake --preset ${{ matrix.preset }}` step).

In `.github/workflows/wasm-ladder.yml`: add `- 'src/qt_quick/**'` after `- 'src/qt/**'` in both `paths:` lists; add
`-DMORPH_BUILD_QT_QUICK=ON \` after `-DMORPH_BUILD_QT=ON \` in the Configure step; and in "Build the WASM-remote
spike and every rung's WASM client", directly after `export EM_CACHE="$PWD/.emcache"`:

```bash
          # The Qt Quick frontend is what the migrated browser clients run on;
          # built by name so a target that silently stops being generated fails.
          cmake --build build-wasm-ladder --target morph_qt_quick
```

`wasm-demo.yml` is left as it is: it installs no `qtwebsockets`, so it cannot configure `MORPH_BUILD_QT`; it gains
the frontend when bank's browser client moves onto it (Part 7).

- [ ] **Step 4: Run the checks to verify they pass**

```bash
bash scripts/check_qt_quick_install.sh
bash scripts/check_install_export.sh
bash scripts/test_check_install_export.sh
cmake --preset linux-everything -N >/dev/null 2>&1 || true   # presets parse (Linux)
```

Expected: the first prints `ok: an application runs through morph::qt_quick from the install`; the second and third
pass unchanged.

Mutation checks (one at a time, then restore):
- Drop `${MORPH_QT_QUICK_QML_OUTPUT_TARGETS}` from the second `install(TARGETS ...)` → expected configure FAIL
  ("install(EXPORT "morphTargets" ...) includes target "morph_qt_quick_qml" which requires target
  "morph_qt_quick_qml_resources_…" that is not in any export set") — CMake itself refuses an export without them.
- Drop `OBJECTS DESTINATION "${CMAKE_INSTALL_LIBDIR}"` → expected FAIL in step 2 of the script ("no object file of
  the MorphUi module's resources") or a configure error naming the missing `OBJECTS DESTINATION`.
- Revert the `ARCH_INDEPENDENT` condition to Part 3's → expected FAIL ("morphConfigVersion.cmake is
  ARCH_INDEPENDENT").

- [ ] **Step 5: Commit**

```bash
git add CMakeLists.txt cmake/morphConfig.cmake.in CMakePresets.json .github/workflows scripts/check_qt_quick_install.sh
git commit -m "wip(qtquick): install the qt_quick component; presets and CI legs

Signed-off-by: Christian Parpart <christian@parpart.family>"
```

---
### Task 11: Spec, maps and changelog

**Files:**
- Create: `docs/spec/qt_quick/frontend.md`
- Modify: `docs/spec/README.md` — the "Start here" map
- Modify: `docs/ARCHITECTURE.md` — "Namespace map" table; "Header map", a section after "Qt integration headers"
- Modify: `README.md` — the namespace table after the `morph::qt` row; the sentence listing the install components
- Modify: `CHANGELOG.md` — `## [Unreleased]` → `### Added`
- Modify: `docs/CMakeLists.txt` — `DOXYGEN_EXCLUDE_SYMBOLS`

**Interfaces:**
- Consumes: the API exactly as Tasks 1–10 ship it. Where this text and the code disagree, fix the text.
- Produces: the authoritative spec later parts read before touching `morph::qt_quick` (Parts 6–10 run every example
  on it).

- [ ] **Step 1: Write `docs/spec/qt_quick/frontend.md`**

```markdown
# The Qt Quick frontend — design

Design spec for `morph::qt_quick` (`include/morph/qt_quick/{backend,frontend}.hpp`, sources and the private
`MorphUi` QML module in `src/qt_quick/`): the frontend that runs a `ui::Application` in a Qt Quick window, natively
and in the browser. It implements the backend contract of `docs/spec/ui/backend_contract.md` and the frontend seam of
`docs/spec/ui/frontend.md`; an application never names it — `main` offers it to `ui::selectFrontend`.

## Contents

- [Packaging](#packaging)
- [The MorphUi module](#the-morphui-module)
- [Widgets from C++](#widgets-from-c)
- [Layout](#layout)
- [Drag and drop](#drag-and-drop)
- [The frontend](#the-frontend)
- [WebAssembly](#webassembly)
- [Testing](#testing)
- [Design decisions](#design-decisions)

## Packaging

| | |
|---|---|
| Option | `MORPH_BUILD_QT_QUICK` (default OFF); requires `MORPH_BUILD_QT` and Qt 6.5+ `Gui`, `Qml`, `Quick`, `QuickControls2` |
| Targets | `morph_qt_quick` / `morph::qt_quick` (STATIC, morph's code, full warning set); `morph_qt_quick_qml` (the `MorphUi` module, generated code only) |
| Links | `morph`, `morph::qt` (for `QtExecutor`), Qt PUBLIC; `Qt6::Widgets` privately under Emscripten |
| Install | component `qt_quick`: headers, both archives, the module's resource object libraries; `find_dependency(Qt6 6.5 COMPONENTS Gui Qml Quick QuickControls2)` |

The library is created before the root `CMakeLists.txt`'s public-header guard, so every header under
`include/morph/qt_quick/` must be in its `FILE_SET`, and `VERIFY_INTERFACE_HEADER_SETS` compiles each standalone.
An install carrying it is not `ARCH_INDEPENDENT`. `scripts/check_qt_quick_install.sh` installs it and runs an
application from the prefix.

## The MorphUi module

One component per widget kind, a `Window`, and three decorations. Each holds plain properties and plain signals,
no application logic, and no binding to anything but its own properties, the palette and the window overlay.

| Kind | Component (root) | Properties | Signals |
|---|---|---|---|
| Text | `Label` (Label) | `text`, `role` (`TextRole` as int) | — |
| Button | `Button` (Button) | `text` | `activated()` |
| TextInput | `TextInput` (TextField), `TextArea` (TextArea) | `text`, `placeholderText`, `password`; `applyText(value)` | `edited(string)`, `submitted(string)` |
| Checkbox | `Checkbox` (CheckBox) | `text`, `checked` | `userToggled(bool)` |
| Select | `Select` (ComboBox), `RadioGroup` | `labels`, `currentIndex` | `chosen(int)` |
| Menu | `Menu` | `labels` | `itemActivated(int)` |
| Column / Row / ForEach | `ColumnStack`, `RowStack` | `spacing`, `content` | — |
| Grid | `Grid` | `columns`, `rowSpacing`, `columnSpacing`, `content` | — |
| Spacer | `Spacer` | — | — |
| Panel | `Panel` (GroupBox) | `title`, `padding`, `collapsible`, `collapsed`, `content` | `collapseToggled(bool)` |
| Scroll | `Scroll` (ScrollView) | `horizontal`, `content` | — |
| Switch case | `Slot` | `content` | — |
| Tabs | `Tabs` (TabBar over a layout) | `labels`, `currentIndex`, `content` | `tabChosen(int)` |
| Dialog | `Dialog` (a modal Dialog on the overlay) | `open`, `permitted`, `title`, `content` | `dismissed()` |
| Busy | `Busy` (BusyIndicator) | `active`, `label` | — |
| Table | `Table`, `TableRowDecoration` | `headers`, `headerWidths`, `headerStretches`, `content`, `highlightLayer` | row: `tapped()`, `doubleTapped()` |
| DateTimeInput | `DateTimeInput` (TextField) | `dateOnly`, `invalid`; `applyText(value)` | `committed(string)` |
| Slider | `Slider` (Slider) | `from`, `to`, `stepSize`, `value` | `valueMoved(real)` |
| FilePicker | `FilePicker` (field, Browse, FileDialog) | `path`, `saveMode`, `browserFiles` | `pathEdited(string)`, `fileChosen(url)`, `browseRequested()` |
| drag / drop | `DragSource`, `DropTarget` | `keyText`; `handler`, `highlighted` | — |

A component whose natural root is a Layout is wrapped in a plain `Item` with the Layout as its `content`, so a
decoration parented into the root is not laid out as a child. Every widget component imports `QtQuick.Layouts`,
because `Layout.*` attached properties are written through the item's own QML context. The style is `Basic` unless
the application or `QT_QUICK_CONTROLS_STYLE` chose one.

## Widgets from C++

- `detail::Context` loads every component once, by URL from `qrc:/qt/qml/MorphUi/`, and fails the `Backend`'s
  construction naming the first that does not load. Nothing depends on the QML import path or on a type
  registration surviving a static link.
- A widget is created unparented, named `w<id>` (1-based, in creation order, per backend), linked to its wrapper
  (`itemOf` / `widgetOf`), and only then parented — visual and `QObject` parent — into its container's `content`
  or the backend's root. Parenting last means whatever reacts to a new child (a table row) already finds the
  wrapper.
- Setters write declared properties; a missing one is a `std::logic_error`, so a renamed QML property fails the
  first test that touches it. Strings are UTF-8 at the contract.
- Signals reach C++ through `detail::SignalRelay`, one per connected signal: a `QObject` child of the item,
  connected `QMetaMethod` to `QMetaMethod` (signal looked up by signature, slot chosen by parameter type: none,
  `bool`, `int`, `double`, `QString`, `QUrl`). Callbacks run as the mount wrapped them, inside
  `Runtime::widgetEvent`. A callback that throws is logged; nothing unwinds through Qt.
- **No echo.** Every user signal is distinct from a property write: `textEdited`, `toggled`, `activated`, `moved`,
  `clicked` fire for user input only; `TextArea`, which has no such signal, guards `textChanged` while `applyText`
  writes. `applyText` writes only a changed text, so the cursor of an unchanged field stays.
- **Controlled state.** Checkbox, Select, Tabs, Panel, Dialog and Table report what the user asked for and change
  only when the application writes the state back (a Checkbox's own toggle excepted: it shows the click).
- A wrapper deletes its item when destroyed, taking relays and decorations with it; it holds the item in a
  `QPointer`, so a wrapper whose item Qt already deleted is safe to destroy.
- `setVisible` records the widget's own flag and hides the item; a Dialog, whose root item is never shown, records
  the flag and writes `permitted` instead. The conformance probe reports that flag, not `QQuickItem::isVisible()`
  (which folds in hidden ancestors), so it reads what `RecordingBackend` reads.

## Layout

| Sizing | Width | Height |
|---|---|---|
| `Content` | the implicit width | the implicit height |
| `Fixed(n)` | `Layout.preferredWidth` = n × the application font's average character width | n × its line height |
| `Stretch(w)` | `Layout.fillWidth`, `Layout.horizontalStretchFactor` w, preferred width 0 | likewise vertically |

So `Fixed(n)` means "about n characters" here as in the terminal, and stretch weights alone divide the free space.
Gaps and paddings are in the same units along their axis. `moveChild` restacks (`stackBefore` / `stackAfter`) and
the Layout follows the stacking order. A grid span is the cell's `Layout.columnSpan`. In a Table a row stretches
across the table and a cell whose own width is Content takes its column's sizing; a Content column leaves each
cell its content width, so only Fixed and Stretch columns line up.

## Drag and drop

A widget with a drag key gets a `DragSource`: a `DragHandler` with no target, and a translucent copy of the widget
on the window overlay that follows the pointer while the handler is active and carries an internal Qt Quick drag
(`Drag.active`, `Drag.source`, `Drag.mimeData`). A widget with `onDrop` gets a `DropTarget`, a `DropArea` that asks
`accepts(key)` when a drag enters, outlines itself while accepting, and on release calls `onDrop(key)`. A refusing
target is not outlined and gets no drop; a drop passes to an enclosing target when an inner one refuses. Keys cross
into QML only as text, `i:<int64>` or `s:<UTF-8>`, carried on the drag's source, so no integer key becomes a double.

## The frontend

`qt_quick::Frontend(argc, argv)::run(factory)` makes, in order: `QGuiApplication` (none may exist yet), the style,
a `QtExecutor` (the runtime's owner and every bridge's callback executor), the engine, the `Window`, the
`Backend`, the `QTimer` scheduler, the `reactive::Runtime` and the `AppContext`; calls the factory; mounts `view()`
into the window's content; shows the window; runs `exec()`. It tears down in reverse: the mount, the application,
the runtime, then Qt.

- `quit(code)` posts `QCoreApplication::exit(code)`, so a quit before the loop runs ends it at once; closing the
  window quits with 0.
- The scheduler makes one `QTimer` per timer (single-shot for `after`, repeating for `every`); a callback runs
  inside an executor scope naming the `QtExecutor`; a `TimerHandle` stops and releases its timer; destroying the
  scheduler stops them all.
- `ioLoop()` is null; `afterFlush` is unused, because Qt repaints changed items itself.
- `frontendOption(argc, argv[, environment])` is named `"qt"`; `usable()` holds on macOS, Windows and WebAssembly,
  and elsewhere when `DISPLAY`, `WAYLAND_DISPLAY` or `QT_QPA_PLATFORM` is set and not empty — the last for a device
  with a framebuffer or EGL platform and no display server.

## WebAssembly

The same library builds under Emscripten, single-threaded: the `QtExecutor` is the only executor, `IoLoop` is
host-driven, and `exec()` hands control to the browser and never returns, so `run` does not return either and
nothing is torn down. `FilePicker`'s Browse uses `QFileDialog::getOpenFileContent`, because a browser exposes a
file's contents, not its path: the contents are written to `/tmp/<file name>` in Emscripten's in-memory file
system, and that path is reported. A Save picker shows no Browse button there. Each application's one `main`,
built for WebAssembly with only this frontend, is its browser client; Qt's finalizer for that executable links the
QML plugins `MorphUi` imports.

## Testing

`tests/qt_quick/` (with `MORPH_BUILD_QT_QUICK`, never under Emscripten) runs on the `offscreen` platform with the
software scene graph. `morph_qt_quick_tests` owns one `QGuiApplication`: the relays, key text, every wrapper's
properties, signals, no-echo and deletion, layout geometry, drag and drop through `QTest` mouse events (an int64
key above 2^53, a refusing target), the scheduler, and spec 1's backend conformance cases through `QtQuickProbe`.
`morph_qt_quick_frontend_tests` owns none, because `run` makes its own: run, quit, `frontendOption`, and the
teardown order — "the mount dies before the application, the application before the runtime, Qt last" pins that the
application is destroyed while the runtime is still alive.

## Design decisions

| Decision | Why |
|---|---|
| Items from C++, one plain component per kind | The application's view is C++; the QML is a widget set, not an app, and its property and signal names are a contract the per-widget tests pin. |
| A separate target for the module | Its sources are generated; morph's warning set applies to morph's code. |
| Components loaded by URL from the module's resources | Independent of the import path and of a static link keeping a registration function. |
| A relay `QObject` per signal, `QMetaMethod` connections | QML signals are known only at run time; one generic relay class needs moc once. |
| Wrapper roots around layouts | A decoration in a Layout would be laid out as a child. |
| Tabs content is a plain layout | `TabsWidget` maps no child to a tab; the mount decides what is shown. |
| Keys as text, Select keys kept in C++ | No int64 becomes a JavaScript double. |
| `Fixed(n)` in font units | "About n characters" on every frontend. |
| `quit` is posted | A quit from the factory, before the loop runs, must not be lost. |
| `frontendOption` takes an environment reader | `usable()` is testable without the process environment, like `selectFrontend`. |
```

- [ ] **Step 2: Update the maps, the README and the changelog**

In `docs/spec/README.md`, "Start here — specs by the question they answer", in the group that lists the `ui/` and
`tui/` specs (Parts 2–3), append ` ·` and `[`qt_quick/frontend.md`](qt_quick/frontend.md)` after the `tui` entry.

In `docs/ARCHITECTURE.md`, "Namespace map" table, after the `morph::qt` row:

```markdown
| `morph::qt_quick` | The Qt Quick frontend (built only when `MORPH_BUILD_QT_QUICK=ON`) | `Backend`, `Frontend`, `frontendOption`, `itemOf`, `widgetOf` |
```

and in "Header map", after the "Qt integration headers (`include/morph/qt/`)" table:

```markdown
### Qt Quick frontend headers (`include/morph/qt_quick/`)

| Header | Responsibility |
|---|---|
| `backend.hpp` | `Backend` — `ui::IViewBackend` over Qt Quick Controls items made from the private `MorphUi` QML module; `itemOf`, `widgetOf` |
| `frontend.hpp` | `Frontend` — runs a `ui::Application` in one window on a `QtExecutor` with a `QTimer` scheduler; `frontendOption` (`"qt"`) |
```

In `README.md`, the namespace table, after the `morph::qt` row:

```markdown
| `morph::qt_quick` | `qt_quick/*.hpp` | `Backend`, `Frontend`, `frontendOption` — the Qt Quick frontend (with `MORPH_BUILD_QT_QUICK=ON`) |
```

and in the sentence beginning "The components are `net` (`MORPH_BUILD_NET`, POSIX only)", add after the `qt`
entry: `` `qt_quick` (`MORPH_BUILD_QT_QUICK`, which needs `qt`), ``.

In `CHANGELOG.md`, under `## [Unreleased]` → `### Added`, after the `morph::tui` entry:

```markdown
- **`morph::qt_quick`: the Qt Quick frontend.** A compiled static library behind `MORPH_BUILD_QT_QUICK`
  (needs `MORPH_BUILD_QT` and Qt 6.5+ Quick), installed as the `qt_quick` component.
  - `qt_quick::Backend` implements `ui::IViewBackend` with Qt Quick Controls items made from C++ out of a
    private QML module; layout hints are in font units; drag-and-drop carries integer keys exactly.
  - `qt_quick::Frontend` runs a `ui::Application` in one window on a `QtExecutor` with a `QTimer` scheduler;
    `qt_quick::frontendOption` offers it to `ui::selectFrontend` as `"qt"`. It builds for WebAssembly.
  - Specified in `docs/spec/qt_quick/frontend.md`.
```

In `docs/CMakeLists.txt`, `DOXYGEN_EXCLUDE_SYMBOLS`, after `"morph::qt::detail::*"`:

```cmake
    "morph::qt_quick::detail"
    "morph::qt_quick::detail::*"
```

- [ ] **Step 3: Build the docs with warnings as errors**

```bash
cmake -S . -B build/docs -G Ninja -DMORPH_BUILD_DOCUMENTATION=ON -DMORPH_BUILD_TESTS=OFF -DMORPH_BUILD_EXAMPLES=OFF
cmake --build build/docs --target doc
```

Expected: exits 0. The Docs configure has `MORPH_BUILD_QT_QUICK` OFF but Doxygen reads every header under
`include/morph`, `qt_quick/` included. A warning naming a `morph::qt_quick` symbol is a missing
`@param`/`@return`: fix the header. Mutation check: delete the `@return` of `frontendOption` → expected FAIL naming
it. Restore.

- [ ] **Step 4: Commit**

```bash
git add docs/spec/qt_quick docs/spec/README.md docs/ARCHITECTURE.md README.md CHANGELOG.md docs/CMakeLists.txt
git commit -m "wip(qtquick): specify morph::qt_quick

Signed-off-by: Christian Parpart <christian@parpart.family>"
```

---

### Task 12: Whole-part verification and the squash

**Files:** none new; fixes land in the files they concern.

- [ ] **Step 1: Strict build and the full suite, with and without the frontend**

```bash
cmake --build build/qt && ctest --test-dir build/qt --output-on-failure
cmake --build build/reactive && ctest --test-dir build/reactive --output-on-failure
```

Expected: every test passes in both; `build/reactive` (no Qt) proves nothing in the base build reaches
`morph::qt_quick`. The `build/qt` build compiles both public headers standalone
(`VERIFY_INTERFACE_HEADER_SETS`).

- [ ] **Step 2: Sanitizers** (Linux; on macOS use an ASan configure of `build/qt`)

```bash
cmake --preset clang-asan -DMORPH_BUILD_QT=ON -DMORPH_BUILD_QT_QUICK=ON && cmake --build --preset clang-asan
bash scripts/check_sanitizer_instrumentation.sh --binary build/clang-asan/tests/qt_quick/morph_qt_quick_tests asan
QT_QPA_PLATFORM=offscreen ctest --test-dir build/clang-asan -L qt_quick --output-on-failure
```

Expected: clean. The deletion, `QPointer`, table-row and scheduler-outlived cases rely on ASan as their observer;
the instrumentation check is what makes a clean run mean something. TSan is not run on this part: everything in it
is on the GUI thread.

- [ ] **Step 3: clang-tidy over the changed lines** — the recipe in CONTRIBUTING, "Running the `clang-tidy-diff`
  gate locally", configured with `-DMORPH_BUILD_QT=ON -DMORPH_BUILD_QT_QUICK=ON` (so `src/qt_quick/*.cpp` and
  `tests/qt_quick/*.cpp` are in `compile_commands.json`), with `origin/master...HEAD` and the file count asserted
  non-zero. Also run `bash scripts/check_automoc_includes.sh build/qt`.

Expected: no findings; no moc include ascends.

- [ ] **Step 4: Install/export**

```bash
bash scripts/check_install_export.sh
bash scripts/check_qt_quick_install.sh
```

Expected: both pass.

- [ ] **Step 5: WebAssembly** (where an Emscripten toolchain and a `wasm_singlethread` Qt 6.8 are available;
  otherwise the `wasm-ladder` workflow on the pushed branch is the check, and the hand-off says so)

```bash
"$WASM/bin/qt-cmake" -S . -B build/wasm -G Ninja -DQT_HOST_PATH="$HOST" -DMORPH_BUILD_QT=ON \
    -DMORPH_BUILD_QT_QUICK=ON -DMORPH_CLIENT_ONLY=ON -DMORPH_BUILD_TESTS=OFF -DMORPH_BUILD_EXAMPLES=OFF
cmake --build build/wasm --target morph_qt_quick
```

Expected: builds, `FilePickerBox::browseInBrowser` included.

- [ ] **Step 6: Commit any fixes**

```bash
git add -A include/morph/qt_quick src/qt_quick tests/qt_quick docs scripts CMakeLists.txt
git commit -m "wip(qtquick): fixes from the sanitizer, tidy, install and WebAssembly gates

Signed-off-by: Christian Parpart <christian@parpart.family>"
```

Skip the commit if there was nothing to fix, and say so in the hand-off.

- [ ] **Step 7: Squash this part into its one commit**

Follow the master plan's "Squashing a part" procedure with key `qtquick` and this message:

```text
qt_quick: morph::qt_quick frontend

morph::qt_quick, a compiled static library behind MORPH_BUILD_QT_QUICK
(needs MORPH_BUILD_QT and Qt 6.5+ Quick). qt_quick::Backend implements
ui::IViewBackend with Qt Quick Controls items made from C++ out of a
private MorphUi QML module, with layout hints in font units and drag keys
that cross as text; it passes the backend conformance suite offscreen.
qt_quick::Frontend runs a ui::Application in one window on a QtExecutor
with a QTimer scheduler, offered to ui::selectFrontend as "qt", and builds
for WebAssembly. Installed as the qt_quick component; specified in
docs/spec/qt_quick/frontend.md.

Signed-off-by: Christian Parpart <christian@parpart.family>
```

Add to the body, before the sign-off, any deviation from this plan made during execution (what the plan said, what
was done instead, and why) and any finding from Task 9's single-process frontend run. The last line of the
procedure must list the commits so far ending in `qt_quick: morph::qt_quick frontend`.

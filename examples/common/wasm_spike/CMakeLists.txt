# SPDX-License-Identifier: Apache-2.0
#
# WASM-remote spike (examples/LADDER.md rung 0): proves QtWebSocketBackend
# works from an Emscripten build, which examples/TESTING.md says has never
# been exercised before this. Only built in an Emscripten configure.
#
# main_wasm.cpp has no QML/Quick UI at all -- it is a plain QCoreApplication +
# QTimer + morph bridge console-style program that logs to qDebug(). Unlike
# bank's gui_wasm (which does have a QML UI and pulls in Qt6::Qml/Quick), this
# target only needs Qt6::Core plus whatever morph::qt itself requires -- which
# already pulls in Qt6::WebSockets via its own target_link_libraries (see
# ../../../CMakeLists.txt's morph_qt INTERFACE target). qt_add_executable (not
# plain add_executable) is still correct/needed here independent of the
# missing QML UI: it is what makes Emscripten's HTML/JS shell generation work
# for a Qt-for-WebAssembly target in general.
find_package(Qt6 REQUIRED COMPONENTS Core)
qt_standard_project_setup(REQUIRES 6.5)

qt_add_executable(morph_ladder_wasm_spike main_wasm.cpp)
# morph::qt is header-only (INTERFACE); the compiled QtWebSocketBackend
# constructor/registerModelAsync/setConnectHandler bodies live in
# morph_qt_impl (see ../../../CMakeLists.txt's `add_library(morph_qt_impl
# STATIC ...)`). main_wasm.cpp constructs a QtWebSocketBackend directly, so
# without this the WASM link fails on undefined symbols -- every other real
# consumer in the repo (examples/common/CMakeLists.txt, tests/qt/CMakeLists.txt,
# tests/net_qt_interop/CMakeLists.txt) links both targets for the same reason.
target_link_libraries(morph_ladder_wasm_spike PRIVATE morph::morph morph::qt morph_qt_impl Qt6::Core)
target_compile_features(morph_ladder_wasm_spike PRIVATE cxx_std_23)

if(NOT DEFINED MORPH_LADDER_WASM_SPIKE_SERVER_URL)
    set(MORPH_LADDER_WASM_SPIKE_SERVER_URL "ws://127.0.0.1:9999" CACHE STRING
        "URL the WASM spike client connects to; override to point at a real out-of-band server for the browser smoke test.")
endif()
target_compile_definitions(morph_ladder_wasm_spike PRIVATE
    MORPH_LADDER_WASM_SPIKE_SERVER_URL="${MORPH_LADDER_WASM_SPIKE_SERVER_URL}"
)

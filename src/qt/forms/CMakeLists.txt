# SPDX-License-Identifier: Apache-2.0
#
# The shipped Qt/QML reference renderer for morph::forms-generated schemas --
# promoted out of examples/forms/gui_qml so an app depends on it directly
# instead of copying/forking it (docs/planned/gui_renderer_toolkit.md,
# docs/spec/forms/forms.md "Renderer contract"). Qt6 (Core/Gui/Qml/Quick) is
# already found and qt_standard_project_setup() already called by the parent
# scope (the MORPH_BUILD_FORMS_QML block in the root CMakeLists.txt) before
# this subdirectory is added.
#
# I18nCatalog (QObject, QML_ELEMENT) ships alongside DynamicForm/DateTimePicker:
# it is a generic, model-agnostic in-memory TranslationProvider realization
# (include/morph/render/i18n.hpp's seam) that DynamicForm.qml's `catalog`
# property consumes structurally -- not lab-model specific, so it belongs in
# the shipped module rather than the demo.

qt_add_library(morph_forms_module STATIC)

qt_add_qml_module(morph_forms_module
    URI MorphForms
    VERSION 1.0
    QML_FILES
        qml/JsonExact.js
        qml/DynamicForm.qml
        qml/DateTimePicker.qml
        qml/SlotRegistry.qml
        qml/CollectionView.qml
        qml/WizardView.qml
    SOURCES
        I18nCatalog.hpp
        I18nCatalog.cpp
)

target_link_libraries(morph_forms_module PUBLIC morph::morph Qt6::Quick Qt6::Qml)
target_compile_features(morph_forms_module PUBLIC cxx_std_23)

# Qt Quick Test suite for the shipped renderer's own logic (schema parsing,
# exact digit arithmetic, unit conversion, readiness) -- independent of any
# app/demo. Later tasks add more tst_*.qml files here; -input (below) picks
# up every tst_*.qml in this directory with no further CMake changes.
#
# NOT EMSCRIPTEN: the module itself builds for wasm (a WASM ladder client
# imports MorphForms), but these two test executables do not belong in a
# browser build -- ctest cannot run a .wasm binary, and MORPH_BUILD_TESTS is
# never part of a WASM configure anyway (examples/common/CMakeLists.txt's own
# Emscripten note). This keeps that true even if someone sets it.
if(MORPH_BUILD_TESTS AND NOT EMSCRIPTEN)
    find_package(Qt6 REQUIRED COMPONENTS QuickTest)

    qt_add_executable(morph_forms_qml_tests tests/tst_main.cpp)
    target_link_libraries(morph_forms_qml_tests PRIVATE morph_forms_moduleplugin Qt6::QuickTest)

    # The shared x-rules corpus (morph#176) has two readers: this suite and
    # tests/test_forms_rule_corpus.cpp. Both are pointed at the one file from
    # CMake so neither can quietly grow its own copy. tst_main.cpp reads it and
    # publishes it to the QML engine -- QML's own XMLHttpRequest refuses a
    # `file:` URL unless QML_XHR_ALLOW_FILE_READ is exported, and a suite whose
    # coverage depends on how it was invoked is exactly the "control that
    # measures nothing" this corpus exists to rule out.
    target_compile_definitions(morph_forms_qml_tests PRIVATE
        MORPH_FORMS_RULE_CORPUS="${CMAKE_CURRENT_SOURCE_DIR}/tests/data/rule_corpus.json")

    if(WIN32)
        find_program(MORPH_WINDEPLOYQT_EXECUTABLE windeployqt
            HINTS "${QT6_INSTALL_PREFIX}/bin" "${Qt6_DIR}/../../../bin"
        )
        if(MORPH_WINDEPLOYQT_EXECUTABLE)
            add_custom_command(TARGET morph_forms_qml_tests POST_BUILD
                COMMAND "${MORPH_WINDEPLOYQT_EXECUTABLE}"
                        --no-compiler-runtime
                        --qmldir "${CMAKE_CURRENT_SOURCE_DIR}/qml"
                        --include-plugins qoffscreen
                        $<TARGET_FILE:morph_forms_qml_tests>
                COMMENT "Deploying Qt runtime DLLs next to morph_forms_qml_tests.exe"
            )
        endif()
    endif()

    add_test(NAME forms_qml_logic
             COMMAND morph_forms_qml_tests -input ${CMAKE_CURRENT_SOURCE_DIR}/tests)
    set_tests_properties(forms_qml_logic PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

    # Catch2 (own main, controls QCoreApplication lifetime -- same rationale
    # as tests/qt/test_qt_websocket.cpp) coverage of FormsControllerCore<Model>.
    qt_add_executable(morph_forms_controller_core_tests tests/test_forms_controller_core.cpp)
    target_link_libraries(morph_forms_controller_core_tests PRIVATE morph::qt_forms Catch2::Catch2)
    add_test(NAME forms_controller_core COMMAND morph_forms_controller_core_tests)
endif()

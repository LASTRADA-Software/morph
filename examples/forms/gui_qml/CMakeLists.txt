# SPDX-License-Identifier: Apache-2.0
#
# Qt Quick renderer for the schema-generated forms. Built when
# -DMORPH_BUILD_FORMS_QML=ON (requires Qt 6.5+ with Quick).

find_package(Qt6 REQUIRED COMPONENTS Core Gui Qml Quick)

qt_standard_project_setup(REQUIRES 6.5)

# The QML module is a static library so both the app and the Qt Quick Test
# runner can link (and therefore import) it.
qt_add_library(morph_forms_module STATIC)

qt_add_qml_module(morph_forms_module
    URI MorphForms
    VERSION 1.0
    QML_FILES
        qml/Main.qml
        qml/DynamicForm.qml
        qml/DateTimePicker.qml
    SOURCES
        FormsController.hpp
        FormsController.cpp
        I18nCatalog.hpp
        I18nCatalog.cpp
)

# lab_model.hpp / lab_schemas.hpp live one level up, shared with the console demo.
target_include_directories(morph_forms_module PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(morph_forms_module PUBLIC morph::morph Qt6::Quick Qt6::Qml)
target_compile_features(morph_forms_module PUBLIC cxx_std_23)

qt_add_executable(morph_forms_qml main.cpp)
target_link_libraries(morph_forms_qml PRIVATE morph_forms_moduleplugin Qt6::Quick)

# Deploy Qt's runtime DLLs next to the executable so it can be launched
# directly (double-click, plain `./morph_forms_qml.exe`) without the Qt bin
# directory already on PATH.
if(WIN32)
    find_program(MORPH_WINDEPLOYQT_EXECUTABLE windeployqt
        HINTS "${QT6_INSTALL_PREFIX}/bin" "${Qt6_DIR}/../../../bin"
    )
    if(MORPH_WINDEPLOYQT_EXECUTABLE)
        # --no-compiler-runtime: do NOT deploy the MSVC redistributable. Recent
        # windeployqt otherwise tries to fetch/refresh vc_redist.x64.exe on the
        # first run, a network/elevation-sensitive step that fails
        # intermittently (and, mid-build, can abort the whole link/deploy with a
        # spurious manifest/vs_link error under Ninja). The MSVC runtime is
        # already present in any dev/CI toolchain, so skipping it makes the
        # deploy deterministic without losing anything needed to run locally.
        add_custom_command(TARGET morph_forms_qml POST_BUILD
            COMMAND "${MORPH_WINDEPLOYQT_EXECUTABLE}"
                    --no-compiler-runtime
                    --qmldir "${CMAKE_CURRENT_SOURCE_DIR}/qml"
                    $<TARGET_FILE:morph_forms_qml>
            COMMENT "Deploying Qt runtime DLLs next to morph_forms_qml.exe"
        )
    else()
        message(WARNING "windeployqt not found; morph_forms_qml.exe will need "
                        "Qt's bin directory on PATH to run.")
    endif()
endif()

# Qt Quick Test suite for the QML logic (DynamicForm descriptors, exact
# arithmetic, unit conversion, composition, readiness).
if(MORPH_BUILD_TESTS)
    find_package(Qt6 REQUIRED COMPONENTS QuickTest)

    qt_add_executable(morph_forms_qml_tests tests/tst_main.cpp)
    target_link_libraries(morph_forms_qml_tests PRIVATE morph_forms_moduleplugin Qt6::QuickTest)

    # Same rationale as morph_forms_qml above: without this, running
    # `ctest -R forms_qml_logic` (or the .exe directly) outside a shell that
    # already has Qt's bin/plugins on PATH hangs briefly then fails with
    # STATUS_DLL_NOT_FOUND, since Qt6QuickTestd.dll and the offscreen QPA
    # plugin are not otherwise deployed next to the test binary.
    if(WIN32 AND MORPH_WINDEPLOYQT_EXECUTABLE)
        add_custom_command(TARGET morph_forms_qml_tests POST_BUILD
            COMMAND "${MORPH_WINDEPLOYQT_EXECUTABLE}"
                    --no-compiler-runtime
                    --qmldir "${CMAKE_CURRENT_SOURCE_DIR}/qml"
                    --include-plugins qoffscreen
                    $<TARGET_FILE:morph_forms_qml_tests>
            COMMENT "Deploying Qt runtime DLLs next to morph_forms_qml_tests.exe"
        )
    endif()

    add_test(NAME forms_qml_logic
             COMMAND morph_forms_qml_tests -input ${CMAKE_CURRENT_SOURCE_DIR}/tests)
    set_tests_properties(forms_qml_logic PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
endif()

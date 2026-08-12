# SPDX-License-Identifier: Apache-2.0
#
# Demo app for the shipped Qt/QML forms renderer (src/qt/forms, URI
# MorphForms): registers this demo's own model/schemas via a small
# FormsController subclass and its own Main.qml, which imports MorphForms for
# DynamicForm/I18nCatalog. Built when -DMORPH_BUILD_FORMS_QML=ON (which also
# builds the shipped module itself, from src/qt/forms, independent of this
# demo). Qt6 is already found and qt_standard_project_setup() already called
# by the time this subdirectory is reached (the MORPH_BUILD_FORMS_QML block
# in the root CMakeLists.txt), and morph_forms_moduleplugin (the shipped
# MorphForms module) already exists as a target.

qt_add_library(lab_forms_demo_module STATIC)

qt_add_qml_module(lab_forms_demo_module
    URI LabFormsDemo
    VERSION 1.0
    QML_FILES
        qml/Main.qml
        qml/AppShell.qml
    SOURCES
        FormsController.hpp
        FormsController.cpp
)

# lab_model.hpp / lab_schemas.hpp live one level up, shared with the console demo.
target_include_directories(lab_forms_demo_module PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(lab_forms_demo_module PUBLIC morph::morph morph::qt_forms morph_forms_moduleplugin Qt6::Quick Qt6::Qml)
target_compile_features(lab_forms_demo_module PUBLIC cxx_std_23)

# FormsController.cpp instantiates schema/rule templates over every demo
# action type -- the same pattern examples/forms/CMakeLists.txt's
# morph_forms_demo target hit (see #80). Under MSVC Debug (no /Og folding,
# full /Zi debug info) this pushes the .obj's COFF section count past the
# 32-bit SN_LOFF format's limit, aborting with C1128 ("number of sections
# exceeded object file format limit"). /bigobj switches to the extended
# section-count format; harmless on Release and on other compilers, so it is
# unconditional for this target.
target_compile_options(lab_forms_demo_module PRIVATE
    $<$<CXX_COMPILER_ID:MSVC>:/bigobj>
)

qt_add_executable(morph_forms_qml main.cpp)
target_link_libraries(morph_forms_qml PRIVATE lab_forms_demo_moduleplugin Qt6::Quick)

# Deploy Qt's runtime DLLs next to the executable so it can be launched
# directly (double-click, plain `./morph_forms_qml.exe`) without the Qt bin
# directory already on PATH. A single --qmldir pointing at this demo's own
# qml/ is sufficient: Main.qml uses the same QtQuick/QtQuick.Controls/
# QtQuick.Layouts imports DynamicForm.qml (in the shipped module) does, so
# windeployqt discovers the same Qt-provided QML plugins either way.
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

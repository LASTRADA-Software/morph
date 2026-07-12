# SPDX-License-Identifier: Apache-2.0
#
# Qt Quick shell with dynamic page loading. Built when
# -DMORPH_BUILD_FORMS_QML_SHELL=ON (requires MORPH_BUILD_FORMS_QML=ON).

find_package(Qt6 REQUIRED COMPONENTS Core Gui Qml Quick QuickControls2)

qt_standard_project_setup(REQUIRES 6.5)

qt_add_executable(morph_forms_shell
    main.cpp
    FormsShellController.hpp FormsShellController.cpp
    PageTreeModel.hpp PageTreeModel.cpp
)

qt_add_qml_module(morph_forms_shell
    URI MorphFormsShell
    VERSION 1.0
    QML_FILES
        qml/Main.qml
        qml/FormShell.qml
        qml/FormPage.qml
        qml/PageConfigDialog.qml
    SOURCES
        FormsShellController.hpp FormsShellController.cpp
        PageTreeModel.hpp PageTreeModel.cpp
)

target_include_directories(morph_forms_shell PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(morph_forms_shell PRIVATE
    morph_forms_moduleplugin
    morph::morph
    Qt6::Quick
    Qt6::QuickControls2
)

# Ensure the MorphForms QML module plugin is built before the shell links it.
add_dependencies(morph_forms_shell morph_forms_moduleplugin)

target_compile_features(morph_forms_shell PUBLIC cxx_std_23)

if(WIN32)
    find_program(MORPH_WINDEPLOYQT_EXECUTABLE windeployqt
        HINTS "${QT6_INSTALL_PREFIX}/bin" "${Qt6_DIR}/../../../bin"
    )
    if(MORPH_WINDEPLOYQT_EXECUTABLE)
        add_custom_command(TARGET morph_forms_shell POST_BUILD
            COMMAND "${MORPH_WINDEPLOYQT_EXECUTABLE}"
                    --qmldir "${CMAKE_CURRENT_SOURCE_DIR}/qml"
                    $<TARGET_FILE:morph_forms_shell>
            COMMENT "Deploying Qt runtime DLLs next to morph_forms_shell.exe"
        )
    else()
        message(WARNING "windeployqt not found; morph_forms_shell.exe will need "
                        "Qt's bin directory on PATH to run.")
    endif()
endif()
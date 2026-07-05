# SPDX-License-Identifier: Apache-2.0
#
# Qt Quick renderer for the schema-generated forms. Built when
# -DMORPH_BUILD_FORMS_QML=ON (requires Qt 6.5+ with Quick).

find_package(Qt6 REQUIRED COMPONENTS Core Gui Qml Quick)

qt_standard_project_setup(REQUIRES 6.5)

qt_add_executable(morph_forms_qml
    main.cpp
)

qt_add_qml_module(morph_forms_qml
    URI MorphForms
    VERSION 1.0
    QML_FILES
        qml/Main.qml
        qml/DynamicForm.qml
        qml/DateTimePicker.qml
    SOURCES
        FormsController.hpp
        FormsController.cpp
)

# lab_model.hpp / lab_schemas.hpp live one level up, shared with the console demo.
target_include_directories(morph_forms_qml PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(morph_forms_qml PRIVATE morph::morph Qt6::Quick Qt6::Qml)
target_compile_features(morph_forms_qml PRIVATE cxx_std_23)

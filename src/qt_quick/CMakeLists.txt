# SPDX-License-Identifier: Apache-2.0
#
# morph::qt_quick, the Qt Quick renderer and frontend (docs/spec/qt_quick/).
# The root CMakeLists.txt finds Qt6 Core/Gui/Qml/Quick/QuickControls2 6.5+
# before adding this directory.

# The module's .qml files live in qml/, so it needs a qmldir there too for them
# to resolve the module's own types; the resource prefix is /qt/qml.
if(QT_KNOWN_POLICY_QTP0001)
    qt_policy(SET QTP0001 NEW)
endif()
if(QT_KNOWN_POLICY_QTP0004)
    qt_policy(SET QTP0004 NEW)
endif()

# ── MorphUi: one QML component per widget kind ──────────────────────────────
# Its own target because every source in it is generated (rcc, qmlcachegen),
# and apply_warnings() would hold Qt's generated code to morph's -Werror set.
# NO_PLUGIN: the module declares no C++ types. The backend loads each
# component by URL from qrc:/qt/qml/MorphUi/, which the module's resource
# object libraries carry into every executable that links morph_qt_quick.
qt_add_library(morph_qt_quick_qml STATIC)
# The type registration qt_add_qml_module runs reads moc's metatype output.
set_target_properties(morph_qt_quick_qml PROPERTIES AUTOMOC ON)
set_source_files_properties(qml/Theme.qml PROPERTIES QT_QML_SINGLETON_TYPE TRUE)
qt_add_qml_module(morph_qt_quick_qml
    URI MorphUi
    VERSION 1.0
    NO_PLUGIN
    # The output directory ends in the URI so the build tree is a valid QML
    # import path for this module, which qmllint relies on.
    OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/MorphUi"
    QML_FILES
        qml/Theme.qml
        qml/MorphScheme.qml
        qml/MorphInputBackground.qml
        qml/MorphButtonBackground.qml
        qml/MorphWidget.qml
        qml/MorphField.qml
        qml/MorphRoots.qml
        qml/MorphText.qml
        qml/MorphButton.qml
        qml/MorphTextInput.qml
        qml/MorphTextArea.qml
        qml/MorphCheckbox.qml
        qml/MorphSelect.qml
        qml/MorphMenu.qml
        qml/MorphStack.qml
        qml/MorphGrid.qml
        qml/MorphSpacer.qml
        qml/MorphPanel.qml
        qml/MorphScroll.qml
        qml/MorphScrollBar.qml
        qml/MorphFooter.qml
        qml/MorphSlot.qml
        qml/MorphTabs.qml
        qml/MorphDialog.qml
        qml/MorphDrawer.qml
        qml/MorphBusy.qml
        qml/MorphTable.qml
        qml/MorphTableRow.qml
        qml/MorphTableRowBackground.qml
        qml/MorphDateTimeInput.qml
        qml/MorphSlider.qml
        qml/MorphFilePicker.qml
        qml/MorphBanner.qml
        qml/MorphBadge.qml
        qml/MorphProgress.qml
        qml/MorphSteps.qml
        qml/MorphKeyValue.qml
        qml/MorphEmptyState.qml
        qml/MorphSplitter.qml
        qml/MorphSplitterHandle.qml
        qml/MorphCollapsible.qml
        qml/MorphDropZone.qml
    OUTPUT_TARGETS morph_qt_quick_qml_output_targets
)
set(MORPH_QT_QUICK_QML_OUTPUT_TARGETS ${morph_qt_quick_qml_output_targets} PARENT_SCOPE)
target_link_libraries(morph_qt_quick_qml PUBLIC Qt6::Quick Qt6::Qml)
set(MORPH_QT_QUICK_QML_DIR "${CMAKE_CURRENT_SOURCE_DIR}/qml" PARENT_SCOPE)
set(MORPH_QT_QUICK_QML_IMPORT_DIR "${CMAKE_CURRENT_BINARY_DIR}" PARENT_SCOPE)

# ── morph_qt_quick: morph's own code ────────────────────────────────────────
add_library(morph_qt_quick STATIC
    backend.cpp
    components.cpp
    containers.cpp
    context.cpp
    context.hpp
    fields.cpp
    frontend.cpp
    keys.cpp
    keys.hpp
    kinds.hpp
    leaves.cpp
    relay.cpp
    relay.hpp
    scheduler.cpp
    table.cpp
    theme.cpp
    widget.cpp
    widget.hpp
)
add_library(morph::qt_quick ALIAS morph_qt_quick)
target_sources(morph_qt_quick PRIVATE inspector.cpp inspector_model.hpp)
target_sources(morph_qt_quick
    PUBLIC
    FILE_SET HEADERS
    BASE_DIRS ${PROJECT_SOURCE_DIR}/include
    FILES
        ${PROJECT_SOURCE_DIR}/include/morph/qt_quick/backend.hpp
        ${PROJECT_SOURCE_DIR}/include/morph/qt_quick/browser_history.hpp
        ${PROJECT_SOURCE_DIR}/include/morph/qt_quick/components.hpp
        ${PROJECT_SOURCE_DIR}/include/morph/qt_quick/frontend.hpp
        ${PROJECT_SOURCE_DIR}/include/morph/qt_quick/inspector.hpp
        ${PROJECT_SOURCE_DIR}/include/morph/qt_quick/scheduler.hpp
        ${PROJECT_SOURCE_DIR}/include/morph/qt_quick/theme.hpp
)
# Same standalone-compile check the core target gets.
set_target_properties(morph_qt_quick PROPERTIES VERIFY_INTERFACE_HEADER_SETS ON AUTOMOC ON)
# The private headers are included as "qt_quick/<name>.hpp", so moc's generated
# include resolves through this path and never climbs out of the build tree.
target_include_directories(morph_qt_quick PRIVATE ${PROJECT_SOURCE_DIR}/src)
# morph_qt by its plain name: the root CMakeLists.txt creates it later and it is
# resolved at generate time.
target_link_libraries(morph_qt_quick
    PUBLIC morph morph_qt Qt6::Core Qt6::Gui Qt6::Qml Qt6::Quick Qt6::QuickControls2
    PRIVATE morph_qt_quick_qml)
apply_warnings(morph_qt_quick)
# Every consumer of this archive (tests/qt_quick, the clients that choose a
# frontend) carries an AF_SANITIZER block of its own, which is what makes
# instrumenting a static library safe (see morph_qt_impl in the root).
if(DEFINED AF_SANITIZER)
    apply_sanitizers(morph_qt_quick ${AF_SANITIZER})
endif()
if(AF_COVERAGE)
    apply_coverage(morph_qt_quick)
endif()

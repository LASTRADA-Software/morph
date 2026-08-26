# SPDX-License-Identifier: Apache-2.0
#
# QML (Qt Quick) GUI for the bank example. Built when -DMORPH_BUILD_BANK_GUI=ON.

find_package(Qt6 REQUIRED COMPONENTS Core Gui Qml Quick QuickControls2)

qt_standard_project_setup(REQUIRES 6.5)

# ── bank_gui_lib: the QML-facing QObject layer ───────────────────────────────
# Split out of bank_gui so something other than the desktop client can link the
# controllers. bank_gui keeps only main.cpp (the QGuiApplication, the context
# properties and the screenshot smoke path); everything a test could want to
# hold an instance of lives here.
#
# Qt6::Core only, deliberately: not one controller — nor BankClient — includes
# a Quick or Qml header. They trade in QObject/QString/QVariantList and a morph
# Bridge, so the surface audit in ../tests/gui/ links this without dragging in a
# QML engine, matching the ladder's own gui_lib convention
# (cmake/morph_add_rung.cmake, "ladder_<rung>_gui_lib": Qt6::Core, no Catch2).
#
# AUTOMOC, because every controller carries Q_OBJECT. moc is kept away from the
# heavy morph/Lightweight headers by the #ifndef Q_MOC_RUN guards in the
# controller headers themselves — see the README's "Qt GUI" section.
add_library(bank_gui_lib STATIC
    BankClient.cpp
    controllers/BankController.cpp
    controllers/AppController.cpp
    controllers/AccountController.cpp
    controllers/TransactionController.cpp
    controllers/CardController.cpp
    controllers/PayeeController.cpp
    controllers/LoanController.cpp
)
target_include_directories(bank_gui_lib PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(bank_gui_lib PUBLIC bank_lib Qt6::Core)
target_compile_features(bank_gui_lib PUBLIC cxx_std_23)
set_target_properties(bank_gui_lib PROPERTIES AUTOMOC ON)
apply_bigobj(bank_gui_lib)
# Note: deliberately NOT calling apply_warnings() here, for the same reason
# bank_lib does not — this target includes the third-party ORM headers
# transitively and they are not -Werror clean.

# ── bank_gui: the desktop client ─────────────────────────────────────────────
qt_add_executable(bank_gui
    main.cpp
)

qt_add_qml_module(bank_gui
    URI BankGui
    VERSION 1.0
    QML_FILES
        qml/Main.qml
        qml/Login.qml
        qml/AppShell.qml
        qml/AccountsPage.qml
        qml/MoveMoneyPage.qml
        qml/CardsPage.qml
        qml/PayeesPage.qml
        qml/LoansPage.qml
        qml/Panel.qml
        qml/AppButton.qml
        qml/Field.qml
        qml/Pill.qml
        qml/Picker.qml
)

target_include_directories(bank_gui PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(bank_gui PRIVATE bank_gui_lib Qt6::Quick Qt6::Qml Qt6::QuickControls2)
target_compile_features(bank_gui PRIVATE cxx_std_23)
apply_bigobj(bank_gui)

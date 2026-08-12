# SPDX-License-Identifier: Apache-2.0
#
# QML (Qt Quick) GUI for the bank example. Built when -DMORPH_BUILD_BANK_GUI=ON.

find_package(Qt6 REQUIRED COMPONENTS Core Gui Qml Quick QuickControls2)

qt_standard_project_setup(REQUIRES 6.5)

qt_add_executable(bank_gui
    main.cpp
    BankClient.cpp
    controllers/BankController.cpp
    controllers/AppController.cpp
    controllers/AccountController.cpp
    controllers/TransactionController.cpp
    controllers/CardController.cpp
    controllers/PayeeController.cpp
    controllers/LoanController.cpp
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
target_link_libraries(bank_gui PRIVATE bank_lib Qt6::Quick Qt6::Qml Qt6::QuickControls2)
target_compile_features(bank_gui PRIVATE cxx_std_23)
apply_bigobj(bank_gui)

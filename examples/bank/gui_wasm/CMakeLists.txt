# SPDX-License-Identifier: Apache-2.0
#
# Self-contained WebAssembly build of the bank QML GUI. Reuses the native QML,
# controllers and BankClient, but swaps the Lightweight/ODBC persistence for an
# in-memory store (include/bank/wasm/) via shadow model headers that shine ahead
# of the native ones on the include path. Only built in an Emscripten configure
# (see ../CMakeLists.txt) with -DMORPH_BUILD_BANK_GUI=ON.

find_package(Qt6 REQUIRED COMPONENTS Core Gui Qml Quick QuickControls2)

qt_standard_project_setup(REQUIRES 6.5)

set(GUI ${CMAKE_CURRENT_SOURCE_DIR}/../gui)

qt_add_executable(bank_gui_wasm
    main_wasm.cpp
    # Reused verbatim from the native GUI:
    ${GUI}/BankClient.cpp                       # dual-moded via __EMSCRIPTEN__
    ${GUI}/controllers/BankController.cpp
    ${GUI}/controllers/AppController.cpp
    ${GUI}/controllers/AccountController.cpp
    ${GUI}/controllers/TransactionController.cpp
    ${GUI}/controllers/CardController.cpp
    ${GUI}/controllers/PayeeController.cpp
    ${GUI}/controllers/LoanController.cpp
    # In-memory model implementations (persistence-free):
    src/models/auth_model_wasm.cpp
    src/models/account_model_wasm.cpp
    src/models/customer_model_wasm.cpp
    src/models/transaction_model_wasm.cpp
    src/models/card_model_wasm.cpp
    src/models/payee_model_wasm.cpp
    src/models/payment_model_wasm.cpp
    src/models/loan_model_wasm.cpp
    # Pure core helpers (no Lightweight):
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/core/money.cpp
)

# The QML lives in the sibling native gui/ dir, so it is referenced by absolute
# path; give each file a relative resource alias so the QML module lays them out
# under qrc:/qt/qml/BankGui/ exactly as an in-tree module would.
set(BANK_QML_FILES
    Main.qml Login.qml AppShell.qml
    AccountsPage.qml MoveMoneyPage.qml CardsPage.qml PayeesPage.qml LoansPage.qml
    Panel.qml AppButton.qml Field.qml Pill.qml Picker.qml
)
set(BANK_QML_PATHS)
foreach(qml ${BANK_QML_FILES})
    set_source_files_properties(${GUI}/qml/${qml} PROPERTIES QT_RESOURCE_ALIAS ${qml})
    list(APPEND BANK_QML_PATHS ${GUI}/qml/${qml})
endforeach()

qt_add_qml_module(bank_gui_wasm
    URI BankGui
    VERSION 1.0
    QML_FILES ${BANK_QML_PATHS}
)

# Order matters: gui_wasm/include is searched BEFORE examples/bank/include so the
# shadow "bank/models/*.hpp" (in-memory, no Lightweight) win over the native
# headers, while "bank/dto/*" and "bank/core/*" still resolve to the shared set.
target_include_directories(bank_gui_wasm BEFORE PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include            # shadow models + bank/wasm store
    ${GUI}                                         # BankClient.hpp, controllers/*
    ${CMAKE_CURRENT_SOURCE_DIR}/../include         # shared bank/dto, bank/core
)

# morph is header-only (INTERFACE); it brings glaze and the include dir. No
# Lightweight anywhere in this target.
target_link_libraries(bank_gui_wasm PRIVATE
    morph::morph
    Qt6::Core Qt6::Gui Qt6::Qml Qt6::Quick Qt6::QuickControls2
)
target_compile_features(bank_gui_wasm PRIVATE cxx_std_23)
apply_bigobj(bank_gui_wasm)

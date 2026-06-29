# SPDX-License-Identifier: Apache-2.0
#
# Qt 6 GUI for the bank example. Built when -DMORPH_BUILD_BANK_GUI=ON.

find_package(Qt6 REQUIRED COMPONENTS Widgets)

set(CMAKE_AUTOMOC ON)

add_executable(bank_gui
    main.cpp
    BankClient.cpp
    LoginView.cpp
    MainWindow.cpp
    views/AccountsView.cpp
    views/MoveMoneyView.cpp
    views/CardsView.cpp
    views/PayeesView.cpp
    views/LoansView.cpp
)
target_include_directories(bank_gui PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(bank_gui PRIVATE bank_lib Qt6::Widgets)
target_compile_features(bank_gui PRIVATE cxx_std_23)

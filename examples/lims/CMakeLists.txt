# SPDX-License-Identifier: Apache-2.0
#
# lims — rung 3 of the application ladder (examples/lims/README.md), built from modules: org and posting (shared),
# lab, ledger, and app, which composes them. All target wiring lives in morph_add_rung() (cmake/morph_add_rung.cmake); this
# file calls it and adds the compile checks that prove the module boundary.

cmake_minimum_required(VERSION 3.25)

morph_add_rung(NAME lims
    MODULES org posting lab ledger billing inventory app
    SHARED_MODULES org posting
    USES lab:org ledger:org,posting billing:org,posting inventory:org,posting
    APP_MODULE app)

# The screens' sources below screens/ (the kit and the scenario) and the catalogues. morph_add_rung globs only
# screens/*.cpp, so the subdirectories are added here. The catalogues' bytes are embedded by a script run at build
# time, so a catalogue edit rebuilds one translation unit and needs no reconfigure.
if(TARGET ladder_lims_screens)
    file(GLOB _lims_screen_subsources CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/screens/kit/*.cpp" "${CMAKE_CURRENT_SOURCE_DIR}/screens/scenario/*.cpp")
    target_sources(ladder_lims_screens PRIVATE ${_lims_screen_subsources})
    file(GLOB _lims_catalogues CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/screens/i18n/*/*.json")
    set(_lims_i18n_header "${CMAKE_CURRENT_BINARY_DIR}/generated/i18n_data.hpp")
    add_custom_command(
        OUTPUT "${_lims_i18n_header}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/generated"
        COMMAND ${CMAKE_COMMAND} "-DOUT=${_lims_i18n_header}" "-DROOT=${CMAKE_CURRENT_SOURCE_DIR}/screens/i18n"
                -P "${CMAKE_CURRENT_SOURCE_DIR}/screens/i18n/embed.cmake"
        DEPENDS ${_lims_catalogues} "${CMAKE_CURRENT_SOURCE_DIR}/screens/i18n/embed.cmake"
        COMMENT "lims: embedding the message catalogues"
        VERBATIM)
    target_sources(ladder_lims_screens PRIVATE "${_lims_i18n_header}")
    target_include_directories(ladder_lims_screens PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")
endif()

# The screen tests read the catalogue files from the source tree. The target is created when the top-level directory
# finishes configuring, so the definition is added by a call deferred behind that one.
function(_lims_screen_test_definitions _dir)
    if(TARGET ladder_lims_screen_tests)
        target_compile_definitions(ladder_lims_screen_tests PRIVATE LIMS_I18N_DIR="${_dir}/screens/i18n")
    endif()
endfunction()
cmake_language(EVAL CODE "
    cmake_language(DEFER DIRECTORY [[${PROJECT_SOURCE_DIR}]] CALL _lims_screen_test_definitions
        [[${CMAKE_CURRENT_SOURCE_DIR}]])")

# Each probe compiles one include as part of one module, and its test builds it: a probe that crosses the
# boundary must fail with the compiler naming the header, and the control must build, which shows the probes compile
# through the module's real include path.
if(MORPH_BUILD_TESTS AND NOT EMSCRIPTEN)
    # morph_add_rung creates ladder_lims_lab on every non-Emscripten build; without it the probes would vanish while
    # `ctest -L ladder-lims` stays green.
    if(NOT TARGET ladder_lims_lab)
        message(FATAL_ERROR "lims: ladder_lims_lab does not exist, so the module-boundary probes cannot be registered")
    endif()
    function(_lims_boundary_probe _name _module _expect _header)
        set(_target lims_boundary_${_name})
        add_library(${_target} OBJECT EXCLUDE_FROM_ALL "${CMAKE_CURRENT_SOURCE_DIR}/tests/compile_checks/${_name}.cpp")
        target_link_libraries(${_target} PRIVATE ladder_lims_${_module})
        target_compile_definitions(${_target} PRIVATE LIMS_MODULE_BOUNDARY_PROBE=1)
        # Kept out of compile_commands.json: a translation unit built to fail is not something to lint.
        set_target_properties(${_target} PROPERTIES EXPORT_COMPILE_COMMANDS OFF)
        add_test(NAME lims.module_boundary.${_name}
                 COMMAND ${CMAKE_COMMAND} -DBUILD_DIR=${CMAKE_BINARY_DIR} -DTARGET=${_target} -DEXPECT=${_expect}
                         -DHEADER=${_header} -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/compile_checks/expect_build.cmake)
        # One build of this tree at a time: two ninja runs in one build directory race on its logs.
        set_tests_properties(lims.module_boundary.${_name} PROPERTIES
            LABELS ladder-lims TIMEOUT 300 RESOURCE_LOCK morph_build_tree)
    endfunction()
    _lims_boundary_probe(lab_includes_app lab fail lims/app/lims_authorizer.hpp)
    _lims_boundary_probe(org_includes_lab org fail lims/lab/models/sample_model.hpp)
    _lims_boundary_probe(lab_includes_org lab pass lims/org/core/errors.hpp)
    _lims_boundary_probe(posting_includes_lab posting fail lims/lab/models/sample_model.hpp)
    _lims_boundary_probe(lab_includes_posting lab fail lims/posting/posting.hpp)
    _lims_boundary_probe(app_includes_posting app pass lims/posting/posting.hpp)
    _lims_boundary_probe(ledger_includes_lab ledger fail lims/lab/models/sample_model.hpp)
    _lims_boundary_probe(ledger_includes_posting ledger pass lims/posting/posting.hpp)
    _lims_boundary_probe(billing_includes_ledger billing fail lims/ledger/models/ledger_model.hpp)
    _lims_boundary_probe(billing_includes_lab billing fail lims/lab/models/sample_model.hpp)
    _lims_boundary_probe(billing_includes_posting billing pass lims/posting/posting.hpp)
    _lims_boundary_probe(ledger_includes_billing ledger fail lims/billing/models/price_list_model.hpp)
    _lims_boundary_probe(inventory_includes_billing inventory fail lims/billing/models/price_list_model.hpp)
    _lims_boundary_probe(inventory_includes_lab inventory fail lims/lab/models/sample_model.hpp)
    _lims_boundary_probe(inventory_includes_ledger inventory fail lims/ledger/models/ledger_model.hpp)
    _lims_boundary_probe(inventory_includes_posting inventory pass lims/posting/posting.hpp)
    _lims_boundary_probe(billing_includes_inventory billing fail lims/inventory/models/stock_model.hpp)

    # The composing code declaring one module's action's effect on another module's record: the probe adds a second
    # effects registration for PostInvoice and must be refused; the control, without it, must resolve.
    foreach(_variant IN ITEMS refused control)
        add_executable(lims_cross_module_effects_${_variant} tests/compile_checks/cross_module_effects_probe.cpp)
        target_link_libraries(lims_cross_module_effects_${_variant} PRIVATE ladder_lims_lib)
        set_target_properties(lims_cross_module_effects_${_variant} PROPERTIES EXPORT_COMPILE_COMMANDS OFF)
        add_test(NAME lims.cross_module_effects.${_variant} COMMAND lims_cross_module_effects_${_variant})
    endforeach()
    target_compile_definitions(lims_cross_module_effects_refused PRIVATE LIMS_PROBE_DECLARES_LEDGER_EFFECT=1)
    set_tests_properties(lims.cross_module_effects.refused PROPERTIES LABELS ladder-lims
        PASS_REGULAR_EXPRESSION "declares different effects in two registrations")
    set_tests_properties(lims.cross_module_effects.control PROPERTIES LABELS ladder-lims
        PASS_REGULAR_EXPRESSION "effects resolved")
endif()

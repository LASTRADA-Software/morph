# SPDX-License-Identifier: Apache-2.0
#
# lims — rung 3 of the application ladder (examples/lims/README.md), built from modules: org (shared), lab, and app,
# which composes them. All target wiring lives in morph_add_rung() (cmake/morph_add_rung.cmake); this file calls it
# and adds the compile checks that prove the module boundary.

cmake_minimum_required(VERSION 3.25)

morph_add_rung(NAME lims
    MODULES org lab app
    SHARED_MODULES org
    APP_MODULE app)

# A module's include path holds its own headers and the shared modules' only. Each probe compiles one include as part
# of one module, and its test builds it: a probe that crosses the boundary must fail with the compiler naming the
# header, and the control must build, which shows the probes compile through the module's real include path.
if(MORPH_BUILD_TESTS AND TARGET ladder_lims_lab)
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
endif()

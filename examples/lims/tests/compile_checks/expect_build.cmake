# SPDX-License-Identifier: Apache-2.0
#
# Builds TARGET in BUILD_DIR and judges the outcome. EXPECT=pass requires the build to succeed. EXPECT=fail requires
# it to fail with the compiler reporting that HEADER cannot be found, so a failure for any other reason is not taken
# for the module boundary holding. Invoked by ctest as
#   cmake -DBUILD_DIR=<dir> -DTARGET=<target> -DEXPECT=<pass|fail> -DHEADER=<header> -P expect_build.cmake
# The build is capped at four jobs: it runs inside a ctest run that other lanes share the machine with.

execute_process(
    COMMAND "${CMAKE_COMMAND}" --build "${BUILD_DIR}" --target "${TARGET}" --parallel 4
    OUTPUT_VARIABLE _output
    ERROR_VARIABLE _output
    RESULT_VARIABLE _result)
if(EXPECT STREQUAL "pass")
    if(NOT _result EQUAL 0)
        message(FATAL_ERROR "${TARGET} was expected to build and did not (exit ${_result}):\n${_output}")
    endif()
    message(STATUS "${TARGET} built")
    return()
endif()
if(_result EQUAL 0)
    message(FATAL_ERROR "${TARGET} built: its include crossed the module boundary and nothing refused it")
endif()
# GCC: `<header>: No such file or directory`; clang: `'<header>' file not found`;
# MSVC: `'<header>': No such file or directory`.
if(NOT _output MATCHES "${HEADER}['\"]?:? *(file not found|No such file or directory)")
    message(FATAL_ERROR "${TARGET} failed, but not because ${HEADER} is missing from its include path:\n${_output}")
endif()
message(STATUS "${TARGET} refused: ${HEADER} is not on its include path")

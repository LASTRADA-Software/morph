# SPDX-License-Identifier: Apache-2.0
#
# Runs the MorphUi module's qmllint target and fails on any warning: the target itself exits 0 when qmllint only
# warns. Invoked by ctest as `cmake -DBUILD_DIR=<dir> -DTARGET=<target> -P run_qmllint.cmake`.

execute_process(
    COMMAND "${CMAKE_COMMAND}" --build "${BUILD_DIR}" --target "${TARGET}"
    OUTPUT_VARIABLE _output
    ERROR_VARIABLE _output
    RESULT_VARIABLE _result)
message("${_output}")
if(NOT _result EQUAL 0)
    message(FATAL_ERROR "qmllint failed to run (exit ${_result})")
endif()
string(REGEX MATCHALL "Warning: [^\n]*" _warnings "${_output}")
list(LENGTH _warnings _count)
if(_count GREATER 0)
    message(FATAL_ERROR "qmllint reported ${_count} warning(s)")
endif()
message(STATUS "qmllint: no warnings")

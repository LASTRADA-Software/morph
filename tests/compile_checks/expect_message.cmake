# SPDX-License-Identifier: Apache-2.0
#
# Runs EXE and passes when its output contains PASS and not FAIL, whatever its exit: a process that aborts on purpose
# dies of a signal, which ctest scores as a failure before PASS_REGULAR_EXPRESSION is consulted.
execute_process(COMMAND "${EXE}" OUTPUT_VARIABLE out ERROR_VARIABLE err RESULT_VARIABLE result)
set(all "${out}${err}")
if(NOT all MATCHES "${PASS}")
    message(FATAL_ERROR "expected output matching `${PASS}`; the run (${result}) printed:\n${all}")
endif()
if(DEFINED FAIL AND NOT FAIL STREQUAL "" AND all MATCHES "${FAIL}")
    message(FATAL_ERROR "output matched `${FAIL}`; the run (${result}) printed:\n${all}")
endif()
message(STATUS "matched `${PASS}` (${result})")

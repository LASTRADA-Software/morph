# SPDX-License-Identifier: Apache-2.0
#
# The Part B regression guard for morph#573: compile-time sensitivity of
# `morph::forms::schemaJson<A>()` to *route count* through a nested-aggregate
# type graph.
#
# Run as `cmake -P`, from the ctest case wired up in tests/CMakeLists.txt.
#
# ── What is asserted, and why it is a ratio ─────────────────────────────────
#
# compile_checks/forms_dag_probe.cpp is compiled twice, differing only in one
# `-D`: with it, every member points at the same type one level down (one route
# to each node — the control); without it, every member spans all three types
# one level down (3^8 = 6,561 routes to the deepest node — the fixture). Same
# struct count, same member count, same nesting depth, same headers, same
# machine, back to back.
#
# The assertion is `fixture <= control * MORPH_DAG_MAX_RATIO_PERCENT / 100`.
# An absolute second count would encode this machine's speed and would have to
# be loosened until it asserted nothing; the ratio cancels the machine, the
# ~2.7 CPU-seconds of including forms.hpp and glaze, and glaze's own schema
# writer, leaving route-count sensitivity alone.
#
# Measured, g++ 16.2.1, `-std=c++23 -fsyntax-only`, CPU seconds, best of 2 on a
# shared machine (so treat these as upper bounds, and see morph#573 for the
# spread):
#
#   revision                     control   fixture   ratio
#   master @ a9cb5649 (before)     2.70     26.83     9.9
#   with morph#573 step 3          2.69      2.98     1.11
#
# morph#703 then removed the depth NTTP entirely, so instantiations are keyed on
# the type alone rather than on a (type, depth) pair. This guard is unchanged by
# that on purpose: it asserts a ratio and does not care how the ratio is
# achieved, which is what let the second fix be judged by the instrument built
# for the first.
#
# The default threshold of 300% therefore sits roughly 2.7x above the fixed
# ratio and 3.3x below the unfixed one. **This was verified by reverting step 3
# and watching this case fail** — see the PR for morph#573 for the output. A
# guard that passes with the fix reverted is not evidence (AGENTS.md, "Verify
# rather than assert"), and this one does not.
#
# ── Why wall time, and what that costs ─────────────────────────────────────
#
# `cmake -E time` reports elapsed wall time; CMake's script mode has no CPU
# clock. On a loaded machine that inflates both numbers, and it inflates them
# together, which is the second reason the assertion is a ratio. The runs are
# repeated MORPH_DAG_RUNS times and the *minimum* is taken, because a compile
# that was descheduled measures the scheduler, not the compiler.
#
# ── The denominator is checked, not trusted ────────────────────────────────
#
# A ratio guard fails open if its denominator is garbage: a control compile that
# somehow did nothing would make every fixture time look fine. So both compiles
# must exit zero (their output is printed in full when they do not), and the
# control must take at least MORPH_DAG_MIN_CONTROL_MS — a compile of this file
# that finished in under a quarter of a second did not parse forms.hpp, whatever
# its exit status said.

cmake_minimum_required(VERSION 3.20)

foreach(_required
        MORPH_DAG_CXX MORPH_DAG_SOURCE MORPH_DAG_INCLUDES MORPH_DAG_FLAGS
        MORPH_DAG_MAX_RATIO_PERCENT MORPH_DAG_RUNS MORPH_DAG_MIN_CONTROL_MS)
    if(NOT DEFINED ${_required})
        message(FATAL_ERROR "forms_dag_budget.cmake: -D${_required}=... was not passed")
    endif()
endforeach()

# Lists arrive pipe-joined: a semicolon inside one add_test() argument would be
# read as an argument separator long before this script sees it.
string(REPLACE "|" ";" _includes "${MORPH_DAG_INCLUDES}")
string(REPLACE "|" ";" _flags "${MORPH_DAG_FLAGS}")

set(_include_flags "")
foreach(_dir IN LISTS _includes)
    if(NOT _dir STREQUAL "")
        list(APPEND _include_flags "-I${_dir}")
    endif()
endforeach()

# "12.345678" -> 12345. Deliberately integer from here on: CMake's math(EXPR)
# has no floating point, so the threshold is a percentage of an integer
# millisecond count rather than a multiplier on a real.
function(_morph_dag_seconds_to_ms _value _out)
    if(NOT _value MATCHES "^([0-9]+)(\\.([0-9]*))?$")
        message(FATAL_ERROR "forms_dag_budget.cmake: could not read '${_value}' as a number of seconds")
    endif()
    set(_whole "${CMAKE_MATCH_1}")
    set(_frac "${CMAKE_MATCH_3}000")
    string(SUBSTRING "${_frac}" 0 3 _frac)
    # Leading zeros: strip them so math(EXPR) never has to guess at a radix,
    # and put a zero back when stripping emptied the string ("000" -> "").
    string(REGEX REPLACE "^0+" "" _whole "${_whole}")
    string(REGEX REPLACE "^0+" "" _frac "${_frac}")
    if(_whole STREQUAL "")
        set(_whole 0)
    endif()
    if(_frac STREQUAL "")
        set(_frac 0)
    endif()
    math(EXPR _ms "${_whole} * 1000 + ${_frac}")
    set(${_out} "${_ms}" PARENT_SCOPE)
endfunction()

# Compiles the probe MORPH_DAG_RUNS times and reports the fastest run, in ms.
function(_morph_dag_compile _label _extra_define _out)
    set(_command "${MORPH_DAG_CXX}" ${_flags} ${_include_flags})
    if(NOT _extra_define STREQUAL "")
        list(APPEND _command "${_extra_define}")
    endif()
    list(APPEND _command "${MORPH_DAG_SOURCE}")

    set(_best "")
    foreach(_run RANGE 1 ${MORPH_DAG_RUNS})
        execute_process(
            COMMAND ${CMAKE_COMMAND} -E time ${_command}
            RESULT_VARIABLE _result
            OUTPUT_VARIABLE _stdout
            ERROR_VARIABLE _stderr)
        if(NOT _result EQUAL 0)
            message(FATAL_ERROR
                "forms_dag_budget.cmake: the ${_label} compilation failed (exit ${_result}), so there is "
                "nothing to time. This guard asserts a compile-time ratio and cannot report one over a "
                "compilation that did not happen.\n"
                "command: ${_command}\n"
                "--- stdout ---\n${_stdout}\n--- stderr ---\n${_stderr}")
        endif()
        if(NOT _stdout MATCHES "Elapsed time \\(seconds\\): ([0-9.]+)")
            message(FATAL_ERROR
                "forms_dag_budget.cmake: `cmake -E time` did not report an elapsed time for the ${_label} "
                "compilation; its output format must have changed.\n--- stdout ---\n${_stdout}")
        endif()
        _morph_dag_seconds_to_ms("${CMAKE_MATCH_1}" _ms)
        message(STATUS "forms DAG budget: ${_label} run ${_run}/${MORPH_DAG_RUNS}: ${_ms} ms")
        if(_best STREQUAL "" OR _ms LESS _best)
            set(_best "${_ms}")
        endif()
    endforeach()
    set(${_out} "${_best}" PARENT_SCOPE)
endfunction()

_morph_dag_compile("control (one route per node)" "-DMORPH_FORMS_DAG_PROBE_TREE" _control_ms)
_morph_dag_compile("fixture (6561 routes)" "" _fixture_ms)

if(_control_ms LESS MORPH_DAG_MIN_CONTROL_MS)
    message(FATAL_ERROR
        "forms_dag_budget.cmake: the control compilation took ${_control_ms} ms, under the "
        "${MORPH_DAG_MIN_CONTROL_MS} ms floor. Compiling forms.hpp and glaze cannot be that cheap, so the "
        "control did not compile what this guard thinks it compiled -- and a ratio over a denominator that "
        "measured nothing would pass no matter what the fixture did.")
endif()

math(EXPR _budget_ms "${_control_ms} * ${MORPH_DAG_MAX_RATIO_PERCENT} / 100")
math(EXPR _ratio_percent "${_fixture_ms} * 100 / ${_control_ms}")

message(STATUS
    "forms DAG budget: control ${_control_ms} ms, fixture ${_fixture_ms} ms "
    "(${_ratio_percent}% of control; budget ${MORPH_DAG_MAX_RATIO_PERCENT}%, i.e. ${_budget_ms} ms)")

if(_fixture_ms GREATER _budget_ms)
    message(FATAL_ERROR
        "forms_dag_budget.cmake: schema generation is route-count sensitive again (morph#573, Part B).\n"
        "  control (one route per node): ${_control_ms} ms\n"
        "  fixture (6561 routes):        ${_fixture_ms} ms  = ${_ratio_percent}% of control\n"
        "  budget:                       ${MORPH_DAG_MAX_RATIO_PERCENT}% of control (${_budget_ms} ms)\n"
        "Both compilations differ only in how many routes reach each node, so a fixture that costs much "
        "more than the control means the nested-aggregate recursion is instantiating per route again "
        "rather than once per type -- see forms.hpp's recurseIntoNestedAggregateIfAny, and "
        "docs/spec/forms/forms.md, \"Nested aggregates (recursive, cycle-safe)\".")
endif()

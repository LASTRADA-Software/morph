# SPDX-License-Identifier: Apache-2.0
#
# Schema-driven forms demo: generates JSON Schemas from action structs,
# emits a self-contained HTML form renderer, and executes pasted action
# lines through the type-erased dispatcher. See also gui_qml/ for the Qt
# Quick renderer of the same schemas.

add_executable(morph_forms_demo main.cpp)
target_link_libraries(morph_forms_demo PRIVATE morph::morph)
apply_warnings(morph_forms_demo)

# main.cpp instantiates schema/rule templates over every demo action type;
# under MSVC Debug (more sections per symbol than Release: no /Og folding,
# full /Zi debug info) that pushes main.cpp.obj's COFF section count past the
# 32-bit SN_LOFF format's limit -- MSVC then aborts with C1128 ("number of
# sections exceeded object file format limit"). /bigobj switches the object
# file to the extended section-count format; harmless on Release and on
# other compilers, so it is unconditional for this target.
target_compile_options(morph_forms_demo PRIVATE
    $<$<CXX_COMPILER_ID:MSVC>:/bigobj>
)

if(DEFINED AF_SANITIZER)
    apply_sanitizers(morph_forms_demo ${AF_SANITIZER})
endif()
if(AF_COVERAGE)
    apply_coverage(morph_forms_demo)
endif()

if(MORPH_BUILD_FORMS_QML)
    add_subdirectory(gui_qml)
endif()

# Demo-level tests: they drive the *shipped* artifacts (the emitted page's JS
# math via node, and the REPL binary end to end).
if(MORPH_BUILD_TESTS)
    find_program(MORPH_NODE NAMES node nodejs)
    if(MORPH_NODE)
        add_test(NAME forms_html_math
                 COMMAND ${MORPH_NODE} ${CMAKE_CURRENT_SOURCE_DIR}/test_html_math.mjs
                         $<TARGET_FILE:morph_forms_demo>)
    endif()
    if(UNIX)
        add_test(NAME forms_repl_roundtrip
                 COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/test_repl.sh $<TARGET_FILE:morph_forms_demo>)
    endif()
endif()

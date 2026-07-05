# SPDX-License-Identifier: Apache-2.0
#
# Schema-driven forms demo: generates JSON Schemas from action structs,
# emits a self-contained HTML form renderer, and executes pasted action
# lines through the type-erased dispatcher. See also gui_qml/ for the Qt
# Quick renderer of the same schemas.

add_executable(morph_forms_demo main.cpp)
target_link_libraries(morph_forms_demo PRIVATE morph::morph)
apply_warnings(morph_forms_demo)

if(DEFINED AF_SANITIZER)
    apply_sanitizers(morph_forms_demo ${AF_SANITIZER})
endif()
if(AF_COVERAGE)
    apply_coverage(morph_forms_demo)
endif()

if(MORPH_BUILD_FORMS_QML)
    add_subdirectory(gui_qml)
endif()

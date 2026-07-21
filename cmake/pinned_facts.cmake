# Renders docs/spec/pinned_facts.toml (flat `KEY = value` pairs -- the one
# place a human states "the spec claims X") into a generated C++ header of
# `kExpected_<KEY>` constants, so tests/test_pinned_facts.cpp never hand-copies
# a value the manifest already states. See CONTRIBUTING.md, "Quality gates".

function(morph_generate_pinned_facts_header MANIFEST_PATH OUTPUT_HEADER)
    # Registers the manifest as a configure-time dependency: editing it and
    # re-running `cmake --build` triggers an automatic CMake reconfigure.
    configure_file(${MANIFEST_PATH} ${CMAKE_BINARY_DIR}/pinned_facts.stamp COPYONLY)

    get_filename_component(_output_dir ${OUTPUT_HEADER} DIRECTORY)
    file(MAKE_DIRECTORY ${_output_dir})

    # ENCODING UTF-8 is required: without it, file(STRINGS) applies its
    # binary-string-extraction heuristic (like the `strings` utility) to any
    # byte with the high bit set, silently shredding the manifest's Unicode
    # box-drawing section headers (e.g. "# -- Key constants --...") into
    # bogus fragments instead of skipping them as `#`-comment lines.
    file(STRINGS ${MANIFEST_PATH} MANIFEST_LINES ENCODING UTF-8)

    set(HEADER_BODY "")
    foreach(LINE IN LISTS MANIFEST_LINES)
        string(STRIP "${LINE}" LINE)
        if(LINE STREQUAL "" OR LINE MATCHES "^#")
            continue()
        endif()
        if(NOT LINE MATCHES "^([A-Z_][A-Z0-9_]*)[ \t]*=[ \t]*(.*)$")
            message(FATAL_ERROR "pinned_facts.toml: unparseable line: ${LINE}")
        endif()
        set(KEY "${CMAKE_MATCH_1}")
        set(RAW_VALUE "${CMAKE_MATCH_2}")
        # Strip a trailing `# comment` (values never contain '#').
        string(REGEX REPLACE "[ \t]*#.*$" "" RAW_VALUE "${RAW_VALUE}")
        string(STRIP "${RAW_VALUE}" RAW_VALUE)
        if(RAW_VALUE MATCHES "^\".*\"$")
            string(APPEND HEADER_BODY "inline constexpr std::string_view kExpected_${KEY} = ${RAW_VALUE};\n")
        elseif(RAW_VALUE MATCHES "^-?[0-9]+$")
            string(APPEND HEADER_BODY "inline constexpr long long kExpected_${KEY} = ${RAW_VALUE};\n")
        else()
            message(FATAL_ERROR "pinned_facts.toml: value for ${KEY} is neither a quoted string nor an integer: ${RAW_VALUE}")
        endif()
    endforeach()

    file(WRITE ${OUTPUT_HEADER}
"// GENERATED FILE -- do not edit by hand.
// Produced by cmake/pinned_facts.cmake from docs/spec/pinned_facts.toml at
// CMake configure time. Edit the manifest, not this file.
#pragma once
#include <string_view>

namespace morph::pinned_facts {

${HEADER_BODY}
}  // namespace morph::pinned_facts
")
endfunction()

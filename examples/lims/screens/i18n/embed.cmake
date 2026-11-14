# SPDX-License-Identifier: Apache-2.0
#
# Writes a header that holds the bytes of lims's message catalogues. Run as
#   cmake -DOUT=<header> -DROOT=<screens/i18n> -P embed.cmake
# Each file is a byte array, not a string literal: a literal is limited to a few kilobytes on MSVC and a catalogue
# grows past it.

file(GLOB _files RELATIVE "${ROOT}" "${ROOT}/*/*.json")
list(SORT _files)
set(_arrays "")
set(_table "")
foreach(_file IN LISTS _files)
    get_filename_component(_locale "${_file}" DIRECTORY)
    get_filename_component(_module "${_file}" NAME_WE)
    file(READ "${ROOT}/${_file}" _hex HEX)
    string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," _bytes "${_hex}")
    set(_name "k_${_locale}_${_module}")
    string(APPEND _arrays "inline constexpr unsigned char ${_name}[] = {${_bytes}0x00};\n")
    string(APPEND _table "    Embedded{\"${_locale}\", \"${_module}\", ${_name}, sizeof(${_name}) - 1},\n")
endforeach()
list(LENGTH _files _count)
file(WRITE "${OUT}.tmp"
"// Generated from screens/i18n/*/*.json; do not edit.
#pragma once

#include <array>
#include <cstddef>
#include <string_view>

namespace lims::screens::i18n {

/// @brief One catalogue file's bytes.
struct Embedded {
    /// @brief The locale: the file's directory.
    std::string_view locale;
    /// @brief The module: the file's name.
    std::string_view module;
    /// @brief The file's bytes.
    unsigned char const* bytes;
    /// @brief The number of bytes.
    std::size_t size;
};

${_arrays}
inline constexpr std::array<Embedded, ${_count}> kEmbedded{{
${_table}}};

}  // namespace lims::screens::i18n
")
# Rewritten only when it changed, so a catalogue edit rebuilds the one translation unit that includes it and an
# unchanged one rebuilds nothing.
execute_process(COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${OUT}.tmp" "${OUT}")
file(REMOVE "${OUT}.tmp")

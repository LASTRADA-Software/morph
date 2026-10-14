# SPDX-License-Identifier: Apache-2.0
#
# ledger — rung 5 of the application ladder (examples/ledger/README.md).
# All target wiring lives in morph_add_rung() (cmake/morph_add_rung.cmake);
# this file only adds the sources it does not glob.

cmake_minimum_required(VERSION 3.25)

morph_add_rung(NAME ledger)

# morph_add_rung() globs src/models, src/db and src/app; the auth DTO's
# validation lives outside those.
if(TARGET ladder_ledger_lib)
    target_sources(ladder_ledger_lib PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src/dto/auth_dto.cpp")
endif()

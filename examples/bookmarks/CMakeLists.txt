# SPDX-License-Identifier: Apache-2.0
#
# bookmarks — rung 2 of the application ladder (examples/bookmarks/README.md).
# All target wiring lives in morph_add_rung() (cmake/morph_add_rung.cmake);
# this file only adds the sources it does not glob.

cmake_minimum_required(VERSION 3.25)

morph_add_rung(NAME bookmarks)

# morph_add_rung() globs src/models, src/db and src/app; the Netscape importer
# and the auth DTO's validation live outside those.
if(TARGET ladder_bookmarks_lib)
    target_sources(ladder_bookmarks_lib PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src/import/netscape_bookmarks.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/dto/auth_dto.cpp")
endif()

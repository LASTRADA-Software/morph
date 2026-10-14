# SPDX-License-Identifier: Apache-2.0
#
# polls — rung 3 of the application ladder (examples/polls/README.md).
# All target wiring lives in morph_add_rung() (cmake/morph_add_rung.cmake);
# this file only adds the sources it does not glob.

cmake_minimum_required(VERSION 3.25)

morph_add_rung(NAME polls)

# morph_add_rung() globs src/models, src/db and src/app; the authorizer lives
# outside those.
if(TARGET ladder_polls_lib)
    target_sources(ladder_polls_lib PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src/auth/polls_authorizer.cpp")
endif()

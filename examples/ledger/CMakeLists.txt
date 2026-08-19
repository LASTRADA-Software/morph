# SPDX-License-Identifier: Apache-2.0
#
# ledger — rung 5 of the application ladder (examples/ledger/README.md).
# All target wiring lives in morph_add_rung() (cmake/morph_add_rung.cmake);
# this file only pulls in ledger-specific sources it doesn't know about, then
# calls it.

cmake_minimum_required(VERSION 3.25)

morph_add_rung(NAME ledger)

# No target_sources() extras yet: unlike polls' src/auth/ (PollsAuthorizer),
# this rung has no source directory morph_add_rung()'s globs
# (src/models/*.cpp, src/db/*.cpp, src/app/*.cpp) don't already cover. Add
# one here, mirroring polls' own CMakeLists.txt treatment of src/auth/, if a
# later task introduces such a directory.

# gui/*.cpp and gui_wasm/*.cpp don't exist yet (Task 22 wires the desktop
# client) -- morph_add_rung() already skips ladder_ledger_gui and
# ladder_ledger_gui_wasm silently when their source directories are empty, so
# there is nothing to comment out here. The WASM client's server-url cache
# variable (see polls' own CMakeLists.txt) has its equivalent block added
# alongside gui_wasm/ in that later task.

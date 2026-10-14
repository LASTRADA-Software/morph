# SPDX-License-Identifier: Apache-2.0
#
# pastebin — rung 1 of the application ladder (examples/pastebin/README.md).
# All target wiring lives in morph_add_rung() (cmake/morph_add_rung.cmake);
# nothing here is pastebin-specific.

cmake_minimum_required(VERSION 3.25)

morph_add_rung(NAME pastebin)

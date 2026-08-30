# SPDX-License-Identifier: Apache-2.0
#
# crm — rung 7 of the application ladder (examples/crm/README.md).
# All target wiring lives in morph_add_rung() (cmake/morph_add_rung.cmake);
# this file only calls it.

cmake_minimum_required(VERSION 3.25)

morph_add_rung(NAME crm)

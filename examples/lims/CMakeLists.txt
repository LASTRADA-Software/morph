# SPDX-License-Identifier: Apache-2.0
#
# lims — rung 6 of the application ladder (examples/lims/README.md).
# All target wiring lives in morph_add_rung() (cmake/morph_add_rung.cmake);
# this file only calls it.

cmake_minimum_required(VERSION 3.25)

morph_add_rung(NAME lims)

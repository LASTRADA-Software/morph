# SPDX-License-Identifier: Apache-2.0
#
# B5 (docs/spec/table/table.md, "Benchmarks: browser"): the Qt for WebAssembly build of the frontend baseline, run
# by hand in a headless browser with emrun. No ctest test: its result is what it prints to the console.

qt_add_executable(wide_rows_baseline wide_rows_baseline.cpp)
target_link_libraries(wide_rows_baseline PRIVATE morph::qt_quick morph_wide_table_model)
target_include_directories(wide_rows_baseline PRIVATE ${PROJECT_SOURCE_DIR}/src ${PROJECT_SOURCE_DIR}/tests)
target_link_options(wide_rows_baseline PRIVATE -sALLOW_MEMORY_GROWTH=1 -sMAXIMUM_MEMORY=4GB)
apply_warnings(wide_rows_baseline)

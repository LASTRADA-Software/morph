# SPDX-License-Identifier: Apache-2.0
#
# B5 and U8 (docs/spec/table/table.md, "Benchmarks"): the Qt for WebAssembly build of the frontend baseline, run
# by hand in a headless browser with emrun. No ctest test: its result is what it prints to the console.

qt_add_executable(wide_rows_baseline wide_rows_baseline.cpp)
target_link_libraries(wide_rows_baseline PRIVATE morph::qt_quick morph_wide_table_model)
target_include_directories(wide_rows_baseline PRIVATE ${PROJECT_SOURCE_DIR}/src ${PROJECT_SOURCE_DIR}/tests)
target_link_options(wide_rows_baseline PRIVATE -sALLOW_MEMORY_GROWTH=1 -sMAXIMUM_MEMORY=4GB)
apply_warnings(wide_rows_baseline)

# U8 (docs/spec/table/table.md, "Benchmarks: every surface"): the interpreter's sort and the column window's scrolls in
# the browser, over a `rows` table of 20,000 and a model-backed table of 100,000 x 300. The fixture's registrations are
# linked whole, as the native executables link them, so the model is found by its type id.
qt_add_executable(u8_scrolls u8_scrolls.cpp)
target_link_libraries(u8_scrolls PRIVATE morph::qt_quick "$<LINK_LIBRARY:WHOLE_ARCHIVE,morph_wide_table_model>")
target_include_directories(u8_scrolls PRIVATE ${PROJECT_SOURCE_DIR}/src ${PROJECT_SOURCE_DIR}/tests)
target_link_options(u8_scrolls PRIVATE -sALLOW_MEMORY_GROWTH=1 -sMAXIMUM_MEMORY=4GB)
apply_warnings(u8_scrolls)

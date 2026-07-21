# libFuzzer harnesses over morph::wire::decode and RemoteServer::dispatchMessage.
# Requires Clang (libFuzzer ships with its runtime via -fsanitize=fuzzer); not
# built unless MORPH_BUILD_FUZZERS=ON, so the default build never sees these
# targets. See docs/spec/testing_strategy.md.
if(NOT CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    message(FATAL_ERROR
        "MORPH_BUILD_FUZZERS=ON requires Clang (libFuzzer via -fsanitize=fuzzer); "
        "current compiler is ${CMAKE_CXX_COMPILER_ID}. Reconfigure with Clang "
        "(cmake/llvm-toolchain.cmake) or unset MORPH_BUILD_FUZZERS.")
endif()

add_executable(fuzz_wire_decode fuzz_wire_decode.cpp)
target_link_libraries(fuzz_wire_decode PRIVATE morph::morph)
apply_warnings(fuzz_wire_decode)
apply_fuzzer(fuzz_wire_decode)
# LLVMFuzzerTestOneInput has no declaring header -- libFuzzer's runtime calls it
# by symbol name -- so there is no previous prototype for Clang to match against.
target_compile_options(fuzz_wire_decode PRIVATE -Wno-missing-prototypes)

add_executable(fuzz_dispatch_execute fuzz_dispatch_execute.cpp)
target_link_libraries(fuzz_dispatch_execute PRIVATE morph::morph)
apply_warnings(fuzz_dispatch_execute)
apply_fuzzer(fuzz_dispatch_execute)
target_compile_options(fuzz_dispatch_execute PRIVATE -Wno-missing-prototypes)

# Replay-mode regression check: libFuzzer, invoked with one or more FILE
# arguments (not a directory) and no fuzzing flags, replays each input once
# and exits 0 iff none of them crash. This is a fast, deterministic CI check
# that every previously-committed seed and finding still passes -- it does
# NOT run a fuzzing campaign (that is a manual/scheduled step; see
# docs/spec/testing_strategy.md).
file(GLOB FUZZ_WIRE_DECODE_INPUTS CONFIGURE_DEPENDS
     "${CMAKE_CURRENT_SOURCE_DIR}/corpus/wire_decode/*.txt"
     "${CMAKE_CURRENT_SOURCE_DIR}/findings/wire_decode/*.txt")
file(GLOB FUZZ_DISPATCH_EXECUTE_INPUTS CONFIGURE_DEPENDS
     "${CMAKE_CURRENT_SOURCE_DIR}/corpus/dispatch_execute/*.txt"
     "${CMAKE_CURRENT_SOURCE_DIR}/findings/dispatch_execute/*.txt")

add_test(NAME fuzz_wire_decode_replay COMMAND fuzz_wire_decode ${FUZZ_WIRE_DECODE_INPUTS})
add_test(NAME fuzz_dispatch_execute_replay COMMAND fuzz_dispatch_execute ${FUZZ_DISPATCH_EXECUTE_INPUTS})
set_tests_properties(fuzz_wire_decode_replay fuzz_dispatch_execute_replay
                      PROPERTIES LABELS "fuzz" TIMEOUT 120)

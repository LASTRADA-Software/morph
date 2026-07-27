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
#
# The globs are deliberately extension-agnostic (`*`, not `*.txt`): a fuzzer
# input is an arbitrary byte string, and crash reproducers minimized by
# libFuzzer are conventionally saved as `.bin`. An extension-scoped glob
# silently skips whatever it does not match, so a committed reproducer can stop
# being replayed without any test turning red -- which is exactly what happened
# to findings/*/*.bin. Every file present is replayed; nothing else belongs in
# these directories.
function(morph_collect_fuzz_inputs out_var harness)
    file(GLOB inputs CONFIGURE_DEPENDS
         "${CMAKE_CURRENT_SOURCE_DIR}/corpus/${harness}/*"
         "${CMAKE_CURRENT_SOURCE_DIR}/findings/${harness}/*")
    # A replay test over an empty input list is a libFuzzer invocation with no
    # FILE arguments -- i.e. an unbounded fuzzing campaign, not a regression
    # check -- so an empty glob is a configuration error, not a silent pass.
    if(NOT inputs)
        message(FATAL_ERROR
            "No fuzz replay inputs found for '${harness}': both "
            "tests/fuzz/corpus/${harness}/ and tests/fuzz/findings/${harness}/ "
            "are empty or missing. The replay test would degrade into an "
            "unbounded fuzzing run.")
    endif()
    set(${out_var} "${inputs}" PARENT_SCOPE)
endfunction()

morph_collect_fuzz_inputs(FUZZ_WIRE_DECODE_INPUTS wire_decode)
morph_collect_fuzz_inputs(FUZZ_DISPATCH_EXECUTE_INPUTS dispatch_execute)

add_test(NAME fuzz_wire_decode_replay COMMAND fuzz_wire_decode ${FUZZ_WIRE_DECODE_INPUTS})
add_test(NAME fuzz_dispatch_execute_replay COMMAND fuzz_dispatch_execute ${FUZZ_DISPATCH_EXECUTE_INPUTS})
set_tests_properties(fuzz_wire_decode_replay fuzz_dispatch_execute_replay
                      PROPERTIES LABELS "fuzz" TIMEOUT 120)

include(CheckCXXCompilerFlag)

# Probed once per configure (CMake caches each check_cxx_compiler_flag() result
# in CMakeCache.txt keyed by the result variable, regardless of how many
# targets call apply_warnings()) rather than unconditionally listed below:
# these three are recent enough additions to Clang's -Weverything set that
# Emscripten's bundled clang (pinned to an older release than the Linux/
# Windows Clang this project otherwise builds with — see
# .github/workflows/wasm-ladder.yml's EMSDK_VERSION) rejects them outright
# under -Werror with "unknown warning option", turning a *suppression* flag
# into the very error it exists to silence. A version-number cutoff would be
# equally correct but more fragile (would need updating every time either
# toolchain's version changes); probing the actual compiler is the standard,
# self-maintaining way to make -Weverything portable across Clang releases.
check_cxx_compiler_flag(-Wno-nrvo MORPH_CLANG_HAS_WNO_NRVO)
check_cxx_compiler_flag(-Wno-unsafe-buffer-usage-in-libc-call MORPH_CLANG_HAS_WNO_UNSAFE_BUFFER_USAGE_IN_LIBC_CALL)
check_cxx_compiler_flag(-Wno-c2y-extensions MORPH_CLANG_HAS_WNO_C2Y_EXTENSIONS)

function(apply_warnings target)
    target_compile_options(${target} PRIVATE
        # ── MSVC ──────────────────────────────────────────────────────────────
        $<$<CXX_COMPILER_ID:MSVC>:
            /W4
            /permissive-
            /w14062     # enumerator not handled in switch
            /w14165     # HRESULT converted to bool
            /w14242     # narrowing conversion
            /w14254     # operator narrowing
            /w14263     # member function does not override base class
            /w14265     # class has virtual functions but destructor is not virtual
            /w14287     # unsigned/negative constant mismatch
            /w14296     # expression is always false/true
            /w14311     # pointer truncation
            /w14545     # expression before comma has no effect
            /w14546     # function call before comma missing argument list
            /w14547     # operator before comma has no effect
            /w14549     # operator before comma has no effect
            /w14555     # expression has no effect
            /w14619     # pragma warning: no warning number
            /w14640     # thread-unsafe static member initialization
            /w14826     # conversion is sign-extended
            /w14905     # wide string literal cast to LPSTR
            /w14906     # string literal cast to LPWSTR
            /w14928     # illegal copy-initialization
            /wd4068     # suppress: unknown pragma (e.g. clang pragmas in shared headers)
        >
        # ── Clang: -Weverything, then subtract the noise ─────────────────────
        # Enable every warning Clang has (Linux clang and clang-cl alike), so a
        # compiler upgrade opts us into new diagnostics automatically. The
        # suppressions below are the *only* categories we turn back off; anything
        # not listed stays an error under -Werror.
        $<$<CXX_COMPILER_ID:Clang>:-Weverything>
        $<$<CXX_COMPILER_ID:GNU>:
            -Wall
            -Wextra
            -Wpedantic
            -Wshadow
            -Wnon-virtual-dtor
            -Wold-style-cast
            -Wcast-align
            -Wunused
            -Woverloaded-virtual
            -Wconversion
            -Wsign-conversion
            -Wmisleading-indentation
            -Wimplicit-fallthrough
        >
        # -Wnull-dereference: Clang gets it free via -Weverything. On GCC the
        # optimizer emits false positives from inside libstdc++ (std::function)
        # at -O2+, and the diagnostic leaks out of the system header (it runs
        # post-inlining) where -isystem cannot suppress it, and it is a no-op at
        # -O0 — so it is left off for GCC entirely.
    )

    # ── Clang -Weverything suppressions (must come *after* -Weverything) ─────
    target_compile_options(${target} PRIVATE
        $<$<CXX_COMPILER_ID:Clang>:
            # (a) Back-compat warnings — irrelevant: we target C++23, so being
            #     "incompatible with C++98/14/17/20" is the whole point.
            -Wno-c++98-compat
            -Wno-c++98-compat-pedantic
            -Wno-pre-c++14-compat
            -Wno-pre-c++17-compat
            -Wno-pre-c++17-compat-pedantic
            -Wno-pre-c++20-compat
            -Wno-pre-c++20-compat-pedantic
            # (b) Inherent to a header-only, templated library.
            -Wno-weak-vtables               # vtable emitted per TU for inline-virtual classes
            -Wno-ctad-maybe-unsupported     # CTAD on types without explicit deduction guides
            -Wno-padded                     # struct tail/inter-member padding
            -Wno-exit-time-destructors      # function-local statics with non-trivial dtors
            -Wno-global-constructors        # non-trivial namespace-scope initializers
            # (c) Stylistic / opinionated noise, not defects.
            -Wno-missing-noreturn
            $<$<BOOL:${MORPH_CLANG_HAS_WNO_NRVO}>:-Wno-nrvo>  # not eliding a trivial-type copy on return
            -Wno-shadow-uncaptured-local    # lambda param shadowing an uncaptured local
            -Wno-documentation-unknown-command
            -Wno-unsafe-buffer-usage        # flags all pointer arithmetic; needs a hardened API
            $<$<BOOL:${MORPH_CLANG_HAS_WNO_UNSAFE_BUFFER_USAGE_IN_LIBC_CALL}>:-Wno-unsafe-buffer-usage-in-libc-call>
            -Wno-float-equal                # exact == is intentional in the value/rational tests
            # (d) Conflicts with a warning we deliberately keep.
            -Wno-covered-switch-default     # collides with -Wswitch-enum + -Wswitch-default
            # (e) Third-party test macros.
            $<$<BOOL:${MORPH_CLANG_HAS_WNO_C2Y_EXTENSIONS}>:-Wno-c2y-extensions>  # Catch2 TEST_CASE expands __COUNTER__
            -Wno-unused-member-function     # Catch2/test-fixture helper members
            -Wno-unneeded-member-function
            # (f) Clang 22 (Homebrew, macOS libc++) added thread-safety-analysis
            #     annotations to std::mutex itself, so -Weverything's
            #     -Wthread-safety-negative now fires on every *plain*,
            #     unannotated std::mutex use (e.g. core/executor.hpp,
            #     core/completion.hpp) even though nothing in this codebase
            #     opts into GUARDED_BY/clang::thread_safety attributes. Until we
            #     annotate the mutex-guarded members throughout, suppress the
            #     diagnostic rather than let an unrelated libc++ upgrade break
            #     every downstream target that transitively includes these
            #     headers (issue #64).
            -Wno-thread-safety-negative
        >
        # clang-cl only: driver noise from the MSVC-mode command line.
        $<$<STREQUAL:${CMAKE_CXX_COMPILER_ID},Clang>:$<$<BOOL:${MSVC}>:
            -Wno-unique-object-duplication
            -Wno-unused-command-line-argument
        >>
    )

    # ── Strict mode: warnings-as-errors + maximum diagnostics ──────────────
    # Controlled by MORPH_ENABLE_STRICT_COMPILATION (default ON in CI).
    if(MORPH_ENABLE_STRICT_COMPILATION)
        target_compile_options(${target} PRIVATE
            $<$<CXX_COMPILER_ID:GNU,Clang>:-Werror>
            $<$<CXX_COMPILER_ID:MSVC>:/WX>
            # Clang needs no extra named flags here — -Weverything (above) already
            # supersets every one of these. This block is the GCC equivalent of
            # "-Weverything": the widest practical set, since GCC has no such flag.
            $<$<CXX_COMPILER_ID:GNU>:
                # Diagnostics also carried on Clang via -Weverything.
                -Wcast-qual
                -Wformat=2
                -Wredundant-decls
                -Winit-self
                -Wmissing-include-dirs
                -Wundef
                -Wswitch-default
                -Wswitch-enum
                -Wctor-dtor-privacy
                -Wpacked
                -Wdouble-promotion
                -Wformat-security
                -Wformat-nonliteral
                -Wmissing-declarations
                -Warray-bounds
                # GCC-specific analyses.
                -Wduplicated-cond
                -Wduplicated-branches
                -Wlogical-op
                -Wuseless-cast
                -Walloc-zero
                -Wstringop-truncation
                -Wsuggest-override
                -Wsubobject-linkage
                -Wtrampolines
                -Wconditionally-supported
                # Widened set (max diagnostics).
                -Wstrict-overflow=2
                -Wformat-overflow=2
                -Wformat-truncation=2
                -Wformat-signedness
                -Wshift-overflow=2
                -Wstringop-overflow=4
                # -Wnoexcept is intentionally omitted: it leaks out of libstdc++
                # <type_traits> when Catch2's (potentially-throwing) test lambdas
                # are passed to std::is_nothrow_invocable, which is noise from
                # third-party code, not a defect in ours.
                -Wnoexcept-type
                -Wredundant-tags
                -Wmismatched-tags
                -Wvolatile
                -Wzero-as-null-pointer-constant
                -Wextra-semi
                -Wsign-promo
                -Wcatch-value=3
                -Wplacement-new=2
                -Wunused-const-variable=2
                -Wdisabled-optimization
                -Wenum-conversion
                -Warith-conversion
                -Wdate-time
                -Wattribute-alias=2
                -Wcast-align=strict
                -Wunsafe-loop-optimizations
            >
        )
    endif()
    # ${MSVC} is true for both cl and clang-cl (any compiler targeting the MSVC
    # runtime), so clang-cl also gets _CRT_SECURE_NO_WARNINGS — its CXX_COMPILER_ID
    # is "Clang", which the CXX_COMPILER_ID:MSVC genex would miss, leaving CRT
    # functions like fopen flagged as deprecated under -Werror.
    target_compile_definitions(${target} PRIVATE
        $<$<BOOL:${MSVC}>:_CRT_SECURE_NO_WARNINGS>
    )
endfunction()

function(apply_sanitizers target mode)
    if(mode STREQUAL "asan")
        target_compile_options(${target} PRIVATE
            -fsanitize=address,undefined -fno-omit-frame-pointer -g)
        target_link_options(${target} PRIVATE
            -fsanitize=address,undefined)
    elseif(mode STREQUAL "tsan")
        target_compile_options(${target} PRIVATE
            -fsanitize=thread -fno-omit-frame-pointer -g)
        target_link_options(${target} PRIVATE
            -fsanitize=thread)
    elseif(mode STREQUAL "ubsan")
        target_compile_options(${target} PRIVATE
            -fsanitize=undefined -fno-omit-frame-pointer -g)
        target_link_options(${target} PRIVATE
            -fsanitize=undefined)
    endif()
endfunction()

function(apply_coverage target)
    target_compile_options(${target} PRIVATE
        -fprofile-instr-generate -fcoverage-mapping -g -O0)
    target_link_options(${target} PRIVATE
        -fprofile-instr-generate)
endfunction()

function(apply_bigobj target)
    # Demo/example/test binaries that instantiate schema, rule, or form
    # templates over many action types push their .obj's COFF section count
    # past the 32-bit SN_LOFF format's limit under MSVC Debug (no /Og
    # folding, full /Zi debug info), aborting with C1128 ("number of
    # sections exceeded object file format limit"). Several unrelated
    # targets have hit this independently as they grew (morph_forms_demo,
    # lab_forms_demo_module, morph_tests) -- call this on any target built
    # from demo/example/ladder sources instead of waiting for it to recur.
    # /bigobj switches to the extended section-count format; harmless on
    # Release and on other compilers.
    target_compile_options(${target} PRIVATE
        $<$<CXX_COMPILER_ID:MSVC>:/bigobj>
    )
endfunction()

function(apply_fuzzer target)
    # tests/fuzz/*: libFuzzer harnesses over morph::wire::decode and
    # RemoteServer::dispatchMessage. Only meaningful on Clang (libFuzzer ships
    # with the Clang runtime via -fsanitize=fuzzer). Combined with
    # AddressSanitizer so a fuzzer-found crash is also a memory-safety finding.
    target_compile_options(${target} PRIVATE -fsanitize=fuzzer,address -fno-omit-frame-pointer -g -O1)
    target_link_options(${target} PRIVATE -fsanitize=fuzzer,address)
endfunction()
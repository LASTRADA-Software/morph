include(CheckCXXCompilerFlag)

# ═══════════════════════════════════════════════════════════════════════════
#  Compiler family
# ═══════════════════════════════════════════════════════════════════════════
# CMake reports Apple's bundled toolchain as "AppleClang", never "Clang", so a
# gate written `$<CXX_COMPILER_ID:Clang>` matches nothing on a default macOS
# toolchain. That is not hypothetical: it is how this project's *entire*
# warning set -- -Weverything, every suppression under it, and -Werror --
# silently stopped reaching the compile line on macOS, with no diagnostic of
# any kind (issue #298). A generator expression that fails to match is
# indistinguishable from one that matches nothing on purpose.
#
# The shape of this file is the fix for that failure mode, not just the extra
# id. The whole flag set is resolved *at configure time* into plain lists, so
# that:
#
#   * exactly one place decides what "clang-family" means
#     (MORPH_COMPILER_FAMILY, below);
#   * an unrecognised compiler id is reported, not silently dropped;
#   * morph_verify_warning_flags() can read the flags back off the targets
#     that apply_warnings() touched and fail the configure if they are not
#     there -- a check the old generator-expression form could not express,
#     because a genex is not evaluated until generate time.
set(MORPH_COMPILER_FAMILY "unknown")
if(CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
    set(MORPH_COMPILER_FAMILY "MSVC")
elseif(CMAKE_CXX_COMPILER_ID MATCHES "^(Apple)?Clang$")
    # "Clang" (upstream/LLVM, Emscripten's bundled clang, and clang-cl, which
    # also reports "Clang" with MSVC set) and "AppleClang" (Xcode / Command
    # Line Tools).
    set(MORPH_COMPILER_FAMILY "Clang")
elseif(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    set(MORPH_COMPILER_FAMILY "GNU")
endif()

set(MORPH_WARNING_FLAGS "")          # applied to every apply_warnings() target
set(MORPH_DROPPED_WARNING_FLAGS "")  # probed for, not supported here; reported
set(MORPH_WARNING_SENTINEL "")       # the one flag morph_verify_warning_flags()
                                     # asserts actually reached each target

# Probe a Clang suppression instead of assuming every Clang snapshot has it.
# These are recent enough additions to Clang's -Weverything set that some of
# the compilers this project builds with reject them outright under -Werror
# with "unknown warning option", turning a *suppression* flag into the very
# error it exists to silence: Emscripten's bundled clang (pinned to an older
# release than the Linux/Windows Clang this project otherwise builds with --
# see .github/workflows/wasm-ladder.yml's EMSDK_VERSION) for most of them, and
# Apple clang 17 (the stock macOS toolchain, LLVM 19-era) for -Wno-nrvo. A
# version-number cutoff would be equally correct but more fragile (would need
# updating every time any of those toolchains moves); probing the actual
# compiler is the standard, self-maintaining way to make -Weverything portable
# across Clang releases.
#
# CMake caches each check_cxx_compiler_flag() result in CMakeCache.txt keyed by
# the result variable, so each is probed once per build directory. Anything
# dropped lands in MORPH_DROPPED_WARNING_FLAGS and is named in the configure
# summary below -- a suppression may go missing, but never quietly.
macro(_morph_clang_suppression_if_supported flag)
    string(MAKE_C_IDENTIFIER "MORPH_CLANG_HAS_${flag}" _morph_probe_var)
    string(TOUPPER "${_morph_probe_var}" _morph_probe_var)
    check_cxx_compiler_flag("${flag}" ${_morph_probe_var})
    if(${_morph_probe_var})
        list(APPEND MORPH_WARNING_FLAGS "${flag}")
    else()
        list(APPEND MORPH_DROPPED_WARNING_FLAGS "${flag}")
    endif()
endmacro()

# ═══════════════════════════════════════════════════════════════════════════
#  MSVC
# ═══════════════════════════════════════════════════════════════════════════
if(MORPH_COMPILER_FAMILY STREQUAL "MSVC")
    set(MORPH_WARNING_SENTINEL "/W4")
    list(APPEND MORPH_WARNING_FLAGS
        /bigobj     # heavy template instantiation (BRIDGE_REGISTER_ACTION chains,
                    # examples/forms/main.cpp) exceeds the default object-file
                    # section limit (C1128) without this
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
    )

# ═══════════════════════════════════════════════════════════════════════════
#  Clang / AppleClang / clang-cl: -Weverything, then subtract the noise
# ═══════════════════════════════════════════════════════════════════════════
# Enable every warning Clang has (Linux clang, AppleClang and clang-cl alike),
# so a compiler upgrade opts us into new diagnostics automatically. The
# suppressions below are the *only* categories we turn back off; anything not
# listed stays an error under -Werror.
elseif(MORPH_COMPILER_FAMILY STREQUAL "Clang")
    set(MORPH_WARNING_SENTINEL "-Weverything")
    list(APPEND MORPH_WARNING_FLAGS
        -Weverything
        # (a) Back-compat warnings — irrelevant: we target C++23, so being
        #     "incompatible with C++98/14/17/20" is the whole point.
        -Wno-c++98-compat
        -Wno-c++98-compat-pedantic
        -Wno-pre-c++14-compat
        -Wno-pre-c++17-compat
        -Wno-pre-c++17-compat-pedantic
        -Wno-pre-c++20-compat
        -Wno-pre-c++20-compat-pedantic
        # -Wc++20-compat is the sibling of -Wpre-c++20-compat for a
        # narrower set of syntax (consteval, implicit `typename` in alias
        # templates) that only some Clang builds separate out from the
        # pre-c++20-compat umbrella above — same "we target C++23"
        # rationale, added once the WASM leg's Emscripten-bundled clang
        # (older than the Linux/Windows clang this project otherwise
        # builds with) was the first to actually split it out and fire it
        # on model_key.hpp/quantity.hpp/forms.hpp/bridge.hpp. Apple clang 17
        # splits it out too, and fires it ~740 times on the same headers.
        -Wno-c++20-compat
        # (b) Inherent to a header-only, templated library.
        -Wno-weak-vtables               # vtable emitted per TU for inline-virtual classes
        -Wno-ctad-maybe-unsupported     # CTAD on types without explicit deduction guides
        -Wno-padded                     # struct tail/inter-member padding
        -Wno-exit-time-destructors      # function-local statics with non-trivial dtors
        -Wno-global-constructors        # non-trivial namespace-scope initializers
        # Emscripten's sysroot stdio.h defines `#define stderr (stderr)`
        # (a legal, intentional self-referential object-like macro used
        # to make `stderr` a valid preprocessor token while still
        # resolving to the libc symbol) -- logger.hpp's
        # std::println(stderr, ...) call trips -Wdisabled-macro-expansion
        # on that expansion. Not fixable in logger.hpp itself: the macro
        # is the *platform's*, not this codebase's, and every other
        # target's libc either doesn't define stderr as a macro at all or
        # doesn't self-reference it this way.
        -Wno-disabled-macro-expansion
        # (c) Stylistic / opinionated noise, not defects.
        -Wno-missing-noreturn
    )
    # Deliberately-partial designated initialization of DTO/config-style
    # aggregates (morph::session::Context{.principal = ...} and its many
    # siblings across the ladder rungs) is this codebase's normal way to
    # construct one with everything else left at its member default -- not an
    # oversight this warning should flag.
    _morph_clang_suppression_if_supported(-Wno-missing-designated-field-initializers)
    _morph_clang_suppression_if_supported(-Wno-nrvo)  # not eliding a trivial-type copy on return
    list(APPEND MORPH_WARNING_FLAGS
        -Wno-shadow-uncaptured-local    # lambda param shadowing an uncaptured local
        -Wno-documentation-unknown-command
        -Wno-unsafe-buffer-usage        # flags all pointer arithmetic; needs a hardened API
    )
    _morph_clang_suppression_if_supported(-Wno-unsafe-buffer-usage-in-libc-call)
    list(APPEND MORPH_WARNING_FLAGS
        -Wno-float-equal                # exact == is intentional in the value/rational tests
        # (d) Conflicts with a warning we deliberately keep.
        -Wno-covered-switch-default     # collides with -Wswitch-enum + -Wswitch-default
    )
    # (e) Third-party test macros.
    _morph_clang_suppression_if_supported(-Wno-c2y-extensions)  # Catch2 TEST_CASE expands __COUNTER__
    list(APPEND MORPH_WARNING_FLAGS
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
    )

    # (g) AppleClang only, deliberately: the one place where the macOS
    #     toolchain's -Weverything set diverges from the Clang the CI legs use,
    #     and scoping it here keeps CI's set exactly as it was.
    #
    #     Xcode's driver puts /usr/local/include on the default header search
    #     path while also passing a -isysroot, and -Wpoison-system-directories
    #     then fires once per translation unit — "include location
    #     '/usr/local/include' is unsafe for cross-compilation" — before any
    #     source is read. It is a statement about the toolchain's own defaults,
    #     not about this project: nothing here adds that directory, no compile
    #     depends on anything in it, and no other platform's Clang emits it.
    #     Suppressing it project-wide would instead switch a real diagnostic
    #     off for the Linux and Windows legs.
    if(CMAKE_CXX_COMPILER_ID STREQUAL "AppleClang")
        list(APPEND MORPH_WARNING_FLAGS -Wno-poison-system-directories)
    endif()

    # clang-cl only: driver noise from the MSVC-mode command line. ${MSVC} is
    # true for any compiler targeting the MSVC runtime, which clang-cl does and
    # AppleClang never does.
    if(MSVC)
        list(APPEND MORPH_WARNING_FLAGS
            -Wno-unique-object-duplication
            -Wno-unused-command-line-argument
        )
    endif()

# ═══════════════════════════════════════════════════════════════════════════
#  GCC
# ═══════════════════════════════════════════════════════════════════════════
elseif(MORPH_COMPILER_FAMILY STREQUAL "GNU")
    set(MORPH_WARNING_SENTINEL "-Wall")
    list(APPEND MORPH_WARNING_FLAGS
        -Wall
        -Wextra
        # GCC's -Wextra implies -Wmissing-field-initializers, which (unlike
        # Clang's narrower -Wmissing-designated-field-initializers, already
        # suppressed above for the identical reason) fires on every field a
        # designated initializer leaves unset -- flagging the same
        # deliberately-partial DTO/config-style construction
        # (morph::session::Context{.principal = ...} and its many
        # siblings) as a defect, one diagnostic per omitted field.
        -Wno-missing-field-initializers
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
    )
    # -Wnull-dereference: Clang gets it free via -Weverything. On GCC the
    # optimizer emits false positives from inside libstdc++ (std::function)
    # at -O2+, and the diagnostic leaks out of the system header (it runs
    # post-inlining) where -isystem cannot suppress it, and it is a no-op at
    # -O0 — so it is left off for GCC entirely.
endif()

# ═══════════════════════════════════════════════════════════════════════════
#  Strict mode: warnings-as-errors + maximum diagnostics
# ═══════════════════════════════════════════════════════════════════════════
# Controlled by MORPH_ENABLE_STRICT_COMPILATION (default ON in CI).
if(MORPH_ENABLE_STRICT_COMPILATION)
    if(MORPH_COMPILER_FAMILY STREQUAL "MSVC")
        list(APPEND MORPH_WARNING_FLAGS /WX)
    elseif(MORPH_COMPILER_FAMILY STREQUAL "Clang")
        # Clang needs no extra named flags here — -Weverything (above) already
        # supersets every one of the GCC list below.
        list(APPEND MORPH_WARNING_FLAGS -Werror)
    elseif(MORPH_COMPILER_FAMILY STREQUAL "GNU")
        # The GCC equivalent of "-Weverything": the widest practical set, since
        # GCC has no such flag.
        list(APPEND MORPH_WARNING_FLAGS
            -Werror
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
        )
    endif()
endif()

# ═══════════════════════════════════════════════════════════════════════════
#  Configure-time report
# ═══════════════════════════════════════════════════════════════════════════
# The signal that was missing when -Weverything evaporated on macOS. Printed
# unconditionally so anyone reading the configure output can see which compiler
# was detected and whether the warning set applied to it.
list(LENGTH MORPH_WARNING_FLAGS _morph_warning_flag_count)
if(MORPH_COMPILER_FAMILY STREQUAL "unknown")
    # MORPH_ENABLE_STRICT_COMPILATION is a promise the build cannot keep on a
    # compiler nothing here recognises: strict mode means warnings-as-errors,
    # and there are no warnings to make errors of. Refuse rather than configure
    # a build that reports strict and enforces nothing — the exact shape of
    # issue #298. Turning strict off downgrades this to a warning.
    set(_morph_unknown_id_message
        "morph: warnings: CMAKE_CXX_COMPILER_ID='${CMAKE_CXX_COMPILER_ID}' matches no "
        "warning set in cmake/compiler_options.cmake, so this build would get the "
        "compiler's own defaults and no warnings-as-errors. Add the id to "
        "MORPH_COMPILER_FAMILY there if it is a compiler this project should support")
    if(MORPH_ENABLE_STRICT_COMPILATION)
        message(FATAL_ERROR ${_morph_unknown_id_message}
            ", or configure with -DMORPH_ENABLE_STRICT_COMPILATION=OFF to build without "
            "the project's diagnostics.")
    else()
        message(WARNING ${_morph_unknown_id_message} ".")
    endif()
elseif(_morph_warning_flag_count EQUAL 0)
    message(FATAL_ERROR
        "morph: warnings: compiler family '${MORPH_COMPILER_FAMILY}' produced an empty "
        "flag list — cmake/compiler_options.cmake is inconsistent.")
else()
    message(STATUS
        "morph: warnings: ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION} "
        "-> ${MORPH_COMPILER_FAMILY} set applied "
        "(${_morph_warning_flag_count} flags, ${MORPH_WARNING_SENTINEL}, "
        "strict=${MORPH_ENABLE_STRICT_COMPILATION})")
endif()
if(MORPH_DROPPED_WARNING_FLAGS)
    message(STATUS
        "morph: warnings: not supported by this compiler, omitted: "
        "${MORPH_DROPPED_WARNING_FLAGS}")
endif()

# ═══════════════════════════════════════════════════════════════════════════
#  Public functions
# ═══════════════════════════════════════════════════════════════════════════

# @brief Apply the project's warning set (and -Werror in strict mode) to a target.
function(apply_warnings target)
    target_compile_options(${target} PRIVATE ${MORPH_WARNING_FLAGS})
    # ${MSVC} is true for both cl and clang-cl (any compiler targeting the MSVC
    # runtime), so clang-cl also gets _CRT_SECURE_NO_WARNINGS — its
    # CXX_COMPILER_ID is "Clang", which an MSVC-only gate would miss, leaving
    # CRT functions like fopen flagged as deprecated under -Werror.
    if(MSVC)
        target_compile_definitions(${target} PRIVATE _CRT_SECURE_NO_WARNINGS)
    endif()
    set_property(GLOBAL APPEND PROPERTY MORPH_WARNED_TARGETS ${target})
endfunction()

# @brief Assert the warning set actually reached every apply_warnings() target.
#
# Call once from the top-level CMakeLists.txt, after every add_subdirectory().
# This is the guard issue #298 asked for: the original bug was a *silent*
# mismatch, so the fix is not only to name AppleClang but to make "the flags
# did not arrive" a configure-time failure. Reads COMPILE_OPTIONS back off
# each target rather than trusting that apply_warnings() did what it looks
# like it does.
function(morph_verify_warning_flags)
    get_property(_targets GLOBAL PROPERTY MORPH_WARNED_TARGETS)
    if(NOT _targets)
        # Nothing to check: a configure with tests and examples off builds only
        # the INTERFACE library, which carries no compile options of its own.
        return()
    endif()
    list(REMOVE_DUPLICATES _targets)
    if(NOT MORPH_WARNING_SENTINEL)
        return()  # unknown compiler; already reported above
    endif()
    foreach(_target IN LISTS _targets)
        get_target_property(_opts ${_target} COMPILE_OPTIONS)
        if(NOT _opts OR NOT "${MORPH_WARNING_SENTINEL}" IN_LIST _opts)
            message(FATAL_ERROR
                "morph: warnings: target '${_target}' went through apply_warnings() but "
                "its COMPILE_OPTIONS do not contain '${MORPH_WARNING_SENTINEL}'. The "
                "warning set is not reaching the compile line — see "
                "cmake/compiler_options.cmake (issue #298).")
        endif()
    endforeach()
    list(LENGTH _targets _morph_target_count)
    message(STATUS
        "morph: warnings: '${MORPH_WARNING_SENTINEL}' verified on "
        "${_morph_target_count} target(s)")
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

function(apply_warnings target)
    target_compile_options(${target} PRIVATE
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
        $<$<STREQUAL:${CMAKE_CXX_COMPILER_ID},Clang>:$<$<BOOL:${MSVC}>:
            -Wno-c++98-compat
            -Wno-c++98-compat-pedantic
            -Wno-pre-c++17-compat
            -Wno-ctad-maybe-unsupported
            -Wno-exit-time-destructors
            -Wno-global-constructors
            -Wno-shadow-uncaptured-local
            -Wno-unused-command-line-argument
        >>

        $<$<CXX_COMPILER_ID:GNU,Clang>:
            -Wall
            -Wextra
            -Wpedantic
        >
        $<$<CXX_COMPILER_ID:Clang>:
            -Wshadow
            -Wnon-virtual-dtor
            -Wold-style-cast
            -Wcast-align
            -Wunused
            -Woverloaded-virtual
            -Wconversion
            -Wsign-conversion
            -Wmisleading-indentation
            -Wnull-dereference
            -Wimplicit-fallthrough
        >
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
    elseif(mode STREQUAL "msan")
        # Instrument only our own targets. Catch2 is built with -stdlib=libc++ (set
        # globally for ABI compatibility) but without -fsanitize=memory to avoid its
        # known false positives. The ignorelist covers morph:: SSO hash false positives.
        target_compile_options(${target} PRIVATE
            -fsanitize=memory -fsanitize-memory-track-origins
            -fno-omit-frame-pointer -g
            -stdlib=libc++
            "-fsanitize-ignorelist=${CMAKE_SOURCE_DIR}/cmake/msan.supp")
        target_link_options(${target} PRIVATE
            -fsanitize=memory
            -stdlib=libc++)
    endif()
endfunction()

function(apply_coverage target)
    target_compile_options(${target} PRIVATE
        -fprofile-instr-generate -fcoverage-mapping -g -O0)
    target_link_options(${target} PRIVATE
        -fprofile-instr-generate)
endfunction()

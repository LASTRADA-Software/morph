# SPDX-License-Identifier: Apache-2.0
#
# morph_add_rung(NAME <rung>): scaffolds the standard target set for one
# ladder rung, per examples/TESTING.md "Build system and CI". Convention
# over configuration: every target below is created only if its source
# directory (relative to the caller's CMAKE_CURRENT_SOURCE_DIR, i.e.
# examples/<rung>/) actually has files — a rung with no gui_wasm/ yet simply
# gets no ladder_<rung>_gui_wasm target, silently, so this one function
# serves every rung from pastebin (rung 1) onward unchanged as each rung
# grows into more of the target set.
#
# Directory -> target convention:
#   src/models/*.cpp, src/db/*.cpp, src/app/*.cpp  -> ladder_<rung>_lib       STATIC (morph + Lightweight)
#   gui_lib/*.cpp                                  -> ladder_<rung>_gui_lib   STATIC (Qt6::Core only, no Catch2)
#   gui/qml/*.qml                                  -> ladder_<rung>_qml       STATIC (QML module, URI = capitalised rung name; needs MORPH_BUILD_FORMS_QML)
#   gui/*.cpp                                      -> ladder_<rung>_gui      EXE    (desktop client; skipped under Emscripten)
#   gui_wasm/*.cpp                                 -> ladder_<rung>_gui_wasm EXE    (Emscripten only; needs MORPH_CLIENT_ONLY)
#   src/server/*.cpp                                -> ladder_<rung>_server   EXE    (standalone server; skipped under Emscripten)
#   tests/*.cpp                                     -> ladder_<rung>_tests    EXE    (Catch2; skipped under Emscripten)
#   src/headless/*.cpp                              -> ladder_<rung>_headless EXE    (QProcess test-client binary, rung 4+)
#
# Every ctest case discovered from ladder_<rung>_tests gets the single label
# "ladder-<rung>", applied by catch_discover_tests itself. CI selects with
# `ctest -L ladder` (.github/workflows/ci.yml, job ladder-tests), which still
# matches: ctest's -L takes a regex, not an exact label. End-to-end journeys
# additionally get "journey" from a small generated post-pass. See the
# catch_discover_tests call below for why the label cannot be a two-value
# LABELS, and why the rung label is not applied by the post-pass (morph#173).
#
# RESOURCE_LOCK is the literal string "morph_ladder_test_db" for every rung's
# tests, matching examples/common's own ladder_common_tests — deliberately
# the *same* name across every rung/binary, not a per-rung one: ctest's
# RESOURCE_LOCK serializes any two ctest cases sharing a lock name even
# across different test *binaries*, which is exactly what's needed if two
# rungs' test binaries ever point at the same on-disk database file (e.g. a
# shared ODBC_CONNECTION_STRING override in some future CI leg) — harmless
# extra serialization if they don't.
#
# CONFIGURE_DEPENDS: every file(GLOB_RECURSE ...) below passes it so a newly
# added source file re-triggers CMake's configure step on the next build
# without an explicit reconfigure. This is a Ninja/Makefiles-generator
# feature (silently a no-op elsewhere, per CMake's own docs); every preset in
# this repo's CMakePresets.json inherits from base-linux or base-vcpkg, both
# of which pin "generator": "Ninja", so this is safe repo-wide today. If a
# non-Ninja/Makefiles preset is ever added, new ladder source files added
# under that preset would need an explicit reconfigure (`cmake --preset ...`)
# before they show up in the build — CONFIGURE_DEPENDS would silently not
# catch them.
function(morph_add_rung)
    set(options "")
    set(oneValueArgs NAME)
    set(multiValueArgs "")
    cmake_parse_arguments(RUNG "${options}" "${oneValueArgs}" "${multiValueArgs}" ${ARGN})

    if(NOT RUNG_NAME)
        message(FATAL_ERROR "morph_add_rung() requires NAME <rung>")
    endif()
    # examples/common/CMakeLists.txt returns early under Emscripten, right
    # after defining morph_ladder_gui/morph_ladder_app but before
    # morph_ladder_testkit (Catch2 + the Lightweight/ODBC-backed testkit have
    # no place in a browser build — see that file's own "WebAssembly build"
    # comment). So the "was common added" check below must not require
    # morph_ladder_testkit under Emscripten, or every rung's WASM configure
    # (ladder_<rung>_gui_wasm) fails here even though common/ was added
    # correctly and every target this function actually needs exists.
    if(EMSCRIPTEN)
        if(NOT TARGET morph_ladder_app)
            message(FATAL_ERROR "morph_add_rung(NAME ${RUNG_NAME}) called before examples/common was added "
                                "(morph_ladder_app does not exist yet) — add_subdirectory(common) first.")
        endif()
    elseif(NOT TARGET morph_ladder_testkit)
        message(FATAL_ERROR "morph_add_rung(NAME ${RUNG_NAME}) called before examples/common was added "
                            "(morph_ladder_testkit does not exist yet) — add_subdirectory(common) first.")
    endif()

    set(_dir "${CMAKE_CURRENT_SOURCE_DIR}")
    set(_rung "${RUNG_NAME}")

    # examples/common/CMakeLists.txt already calls find_package(Qt6 ...
    # COMPONENTS Core WebSockets) and qt_standard_project_setup(), but that
    # call's IMPORTED targets (Qt6::Core etc.) and qt_standard_project_setup's
    # directory-scoped defaults are visible only in common/'s own directory
    # scope and its subdirectories — CMake does not propagate find_package()
    # imported targets sideways to sibling directories. examples/<rung>/ is a
    # *sibling* of common/ (both are add_subdirectory()'d from
    # examples/CMakeLists.txt), not a descendant of it, so without this,
    # ladder_<rung>_lib's `target_link_libraries(... Qt6::Core)` below fails
    # with "target was not found" the first time this function is actually
    # exercised (verified empirically: pastebin, the first real rung, hits
    # exactly this). Calling both again here is cheap and, per Qt's own docs,
    # idempotent/harmless if some ancestor scope already ran them — this is
    # the one place in the whole rung that needs it, since every target below
    # is created in *this* function's (i.e. the calling rung directory's) scope.
    find_package(Qt6 6.5 REQUIRED COMPONENTS Core)
    qt_standard_project_setup(REQUIRES 6.5)

    # ── ladder_<rung>_lib: models + db + app bootstrap (native only) ────
    # Lightweight::Lightweight (ODBC) does not exist under Emscripten:
    # examples/common/CMakeLists.txt returns early, before its
    # FetchContent_MakeAvailable(Lightweight) call, whenever EMSCRIPTEN is
    # set. Persistence lives server-side behind the model for a WASM client
    # (IMPLEMENTATION.md rule 4's WASM clause), and ladder_<rung>_gui_wasm
    # never links ladder_<rung>_lib — so this target genuinely never needs
    # to build under Emscripten at all.
    if(NOT EMSCRIPTEN)
        file(GLOB_RECURSE _lib_sources CONFIGURE_DEPENDS
            "${_dir}/src/models/*.cpp" "${_dir}/src/db/*.cpp" "${_dir}/src/app/*.cpp")
        # The rung's public headers are listed as target sources purely so
        # AUTOMOC sees them. AUTOMOC looks for a Q_OBJECT header next to the
        # .cpp of the same basename, and a rung's layout deliberately splits
        # those apart (include/<rung>/app/app.hpp vs src/app/app.cpp), so a
        # QObject declared in include/ gets no moc output at all otherwise —
        # which a static library happily builds and only fails at the first
        # link that actually needs the vtable (pastebin::app::App, hit the
        # moment ladder_pastebin_tests linked it). Header entries are not
        # compiled; they only join the AUTOMOC scan.
        file(GLOB_RECURSE _lib_headers CONFIGURE_DEPENDS "${_dir}/include/*.hpp")
        if(_lib_sources)
            add_library(ladder_${_rung}_lib STATIC ${_lib_sources} ${_lib_headers})
            add_library(morph::ladder_${_rung}_lib ALIAS ladder_${_rung}_lib)
            # examples/common (PROJECT_SOURCE_DIR, not a "../common" relative
            # path — see examples/CMakeLists.txt's own comment on why: robust to
            # morph being embedded via add_subdirectory() in a parent project)
            # is on the include path for clock.hpp, the ladder-wide injectable
            # "now()" every rung's time-dependent model logic reads instead of
            # DateTime::now() directly (examples/common/clock.hpp's own doc
            # comment). Discovered as a real gap, not present in the original
            # sketch: unlike morph_ladder_gui/_app/_testkit (which each add
            # examples/common to their own PUBLIC include path),
            # ladder_<rung>_lib links none of those three — it is the one target
            # in this function with model/app code that needs clock.hpp but no
            # other reason to depend on morph::ladder_gui and its Qt-Core-only
            # constraint, so its own include path needs common added directly.
            target_include_directories(ladder_${_rung}_lib PUBLIC "${_dir}/include" "${PROJECT_SOURCE_DIR}/examples/common")
            target_link_libraries(ladder_${_rung}_lib PUBLIC morph::morph Lightweight::Lightweight Qt6::Core)
            target_compile_features(ladder_${_rung}_lib PUBLIC cxx_std_23)
            set_target_properties(ladder_${_rung}_lib PROPERTIES AUTOMOC ON)
            apply_bigobj(ladder_${_rung}_lib)
            # Lightweight's headers are not -Werror clean (bank's own caveat,
            # examples/bank/CMakeLists.txt) — no apply_warnings() here.
            if(AF_COVERAGE)
                apply_coverage(ladder_${_rung}_lib)
            endif()
            # AF_SANITIZER (asan/tsan/ubsan): applied the same way apply_coverage()
            # is above. Without this, a --preset clang-tsan build of the ladder
            # compiles this target (a rung's models -- the code kanban-tsan's
            # stress test actually races on) with no sanitizer instrumentation at
            # all, silently defeating the whole point of building under that
            # preset. See .github/workflows/ci.yml's kanban-tsan job.
            if(DEFINED AF_SANITIZER)
                apply_sanitizers(ladder_${_rung}_lib ${AF_SANITIZER})
            endif()
        endif()
    endif()

    # ── ladder_<rung>_gui_lib: presenters + forms-controller glue ───────
    file(GLOB_RECURSE _gui_lib_sources CONFIGURE_DEPENDS "${_dir}/gui_lib/*.cpp")
    if(_gui_lib_sources)
        add_library(ladder_${_rung}_gui_lib STATIC ${_gui_lib_sources})
        add_library(morph::ladder_${_rung}_gui_lib ALIAS ladder_${_rung}_gui_lib)
        target_include_directories(ladder_${_rung}_gui_lib PUBLIC "${_dir}/include" "${_dir}/gui_lib")
        target_link_libraries(ladder_${_rung}_gui_lib PUBLIC morph::morph morph::ladder_gui Qt6::Core)
        if(TARGET ladder_${_rung}_lib)
            target_link_libraries(ladder_${_rung}_gui_lib PUBLIC morph::ladder_${_rung}_lib)
            # ladder_${_rung}_lib links Lightweight::Lightweight PUBLIC, and
            # Lightweight's own target_include_directories() call is plain
            # PUBLIC, not SYSTEM (its CMakeLists.txt) -- so without this,
            # apply_warnings() below (-Werror included) applies in full to
            # every Lightweight header this target transitively sees, not
            # just this rung's own code. examples/bank/CMakeLists.txt's own
            # workaround for the identical problem is to skip
            # apply_warnings() entirely on the target that links Lightweight
            # directly (ladder_${_rung}_lib does the same, just above); this
            # target doesn't include any Lightweight header itself, so
            # demoting the transitive include path to SYSTEM here — rather
            # than also giving up apply_warnings() on it — keeps this rung's
            # own gui_lib/*.cpp fully warned while silencing what is,
            # from here, third-party noise.
            get_target_property(_lightweight_includes Lightweight::Lightweight INTERFACE_INCLUDE_DIRECTORIES)
            if(_lightweight_includes)
                target_include_directories(ladder_${_rung}_gui_lib SYSTEM PUBLIC ${_lightweight_includes})
            endif()
            unset(_lightweight_includes)
        endif()
        target_compile_features(ladder_${_rung}_gui_lib PUBLIC cxx_std_23)
        set_target_properties(ladder_${_rung}_gui_lib PROPERTIES AUTOMOC ON)
        apply_warnings(ladder_${_rung}_gui_lib)
        apply_bigobj(ladder_${_rung}_gui_lib)
        if(AF_COVERAGE)
            apply_coverage(ladder_${_rung}_gui_lib)
        endif()
        # See ladder_${_rung}_lib's identical AF_SANITIZER block above.
        if(DEFINED AF_SANITIZER)
            apply_sanitizers(ladder_${_rung}_gui_lib ${AF_SANITIZER})
        endif()
    endif()

    # ── ladder_<rung>_qml: the rung's own QML module ─────────────────────
    # gui/qml/*.qml becomes a proper QML module (URI = the rung name with its
    # first letter capitalised, e.g. "Pastebin"), built as its own static
    # library rather than folded into the gui executable — exactly the shape
    # examples/forms/gui_qml uses (lab_forms_demo_module + the morph_forms_qml
    # executable linking lab_forms_demo_moduleplugin). It has to be a separate
    # target because *three* consumers need those QML files: the desktop
    # client, the WASM client, and the rung's own offscreen engine-load smoke
    # test (examples/TESTING.md, presenter rule 6), which lives in the test
    # binary. Built under Emscripten too, for the WASM client's sake — the
    # ladder's "same client code" rule means the browser loads the identical
    # Main.qml, not a copy (contrast bank's gui_wasm, which re-declares its own
    # QML module over the native GUI's files).
    #
    # Gated on morph_qt_forms (i.e. MORPH_BUILD_FORMS_QML=ON, which also builds
    # the shipped MorphForms module the rung's Main.qml imports for
    # DynamicForm). Without it there is no schema-driven renderer to compose,
    # so the QML module, the desktop client, and the smoke test are all skipped
    # together — announced, never silently: the ladder CI leg's distro Qt is
    # 6.4.2, below the 6.5 floor MORPH_BUILD_FORMS_QML requires, so that leg
    # legitimately configures without any of this. This block announces the
    # half it owns (the QML module and, through it, the smoke test); the
    # desktop client's block below announces its own skip, for this and every
    # other reason it can be skipped.
    #
    # morph_forms_moduleplugin is forward-referenced: add_subdirectory(src/qt/forms)
    # runs *after* add_subdirectory(examples) in the root CMakeLists.txt (both
    # deferrals are documented there). A plain, non-namespaced target name may
    # be named before it exists; morph_qt_forms — the thing this gates on — is
    # created earlier, before the examples, so the guard itself is sound.
    set(_qml_plugin "")
    file(GLOB_RECURSE _qml_files CONFIGURE_DEPENDS "${_dir}/gui/qml/*.qml")
    if(_qml_files AND NOT TARGET morph_qt_forms)
        message(STATUS "morph_add_rung: rung '${_rung}' has gui/qml/ but MORPH_BUILD_FORMS_QML is OFF "
                       "— skipping ladder_${_rung}_qml and the QML smoke test")
    endif()
    if(_qml_files AND TARGET morph_qt_forms)
        find_package(Qt6 6.5 REQUIRED COMPONENTS Gui Qml Quick QuickControls2)
        string(SUBSTRING "${_rung}" 0 1 _uri_head)
        string(SUBSTRING "${_rung}" 1 -1 _uri_tail)
        string(TOUPPER "${_uri_head}" _uri_head)
        set(_qml_uri "${_uri_head}${_uri_tail}")
        # GLOB_RECURSE yields absolute paths, which qt_add_qml_module
        # refuses to place in a resource without an explicit alias. Alias
        # each file to its bare name so the module's resource layout is
        # flat (qrc:/qt/qml/<Uri>/Main.qml) and independent of where inside
        # gui/qml/ the file happens to live.
        foreach(_qml_file IN LISTS _qml_files)
            cmake_path(GET _qml_file FILENAME _qml_name)
            set_source_files_properties("${_qml_file}" PROPERTIES QT_RESOURCE_ALIAS "${_qml_name}")
        endforeach()
        qt_add_library(ladder_${_rung}_qml STATIC)
        qt_add_qml_module(ladder_${_rung}_qml
            URI ${_qml_uri}
            VERSION 1.0
            QML_FILES ${_qml_files}
        )
        target_link_libraries(ladder_${_rung}_qml PUBLIC morph_forms_moduleplugin Qt6::Quick Qt6::Qml)
        target_compile_features(ladder_${_rung}_qml PUBLIC cxx_std_23)
        set(_qml_plugin ladder_${_rung}_qmlplugin)
    endif()

    # ── ladder_<rung>_gui: desktop client (native only) ──────────────────
    #
    # Absence of gui/*.cpp is the silent, expected case — that is just the
    # convention this file's header describes ("a rung with no gui_wasm/ yet
    # simply gets no ladder_<rung>_gui_wasm target"). But a rung that *has*
    # gui/*.cpp clearly wants a desktop client, so every reason this target
    # can then fail to appear is announced instead: the alternative is the
    # target silently vanishing from an otherwise successful configure, which
    # surfaces only as a "no such target" much later. Each reason is collected
    # rather than short-circuited so a rung missing two prerequisites hears
    # about both in one pass.
    #
    # The `NOT _qml_files` branch is the forward-looking one: no rung today
    # ships gui/*.cpp without gui/qml/, but a future rung that builds its UI
    # with QtWidgets, or reuses another module's QML files, would land exactly
    # there — and would otherwise get no diagnostic at all, since the QML
    # block above only speaks up when gui/qml/ exists and morph_qt_forms does
    # not.
    if(NOT EMSCRIPTEN)
        file(GLOB_RECURSE _gui_sources CONFIGURE_DEPENDS "${_dir}/gui/*.cpp")
        set(_gui_skips "")
        if(_gui_sources AND NOT TARGET ladder_${_rung}_gui_lib)
            list(APPEND _gui_skips "it has no gui_lib/*.cpp, so there is no ladder_${_rung}_gui_lib to link")
        endif()
        if(_gui_sources AND NOT _qml_plugin)
            if(NOT _qml_files)
                list(APPEND _gui_skips "it has no gui/qml/*.qml, so there is no ladder_${_rung}_qml module to link")
            else()
                list(APPEND _gui_skips "MORPH_BUILD_FORMS_QML is OFF, so ladder_${_rung}_qml was not built")
            endif()
        endif()
        if(_gui_skips)
            list(JOIN _gui_skips "; and " _gui_skip_why)
            message(STATUS "morph_add_rung: rung '${_rung}' has gui/*.cpp but ladder_${_rung}_gui is skipped "
                           "— ${_gui_skip_why}")
        endif()
        if(_gui_sources AND NOT _gui_skips)
            find_package(Qt6 6.5 REQUIRED COMPONENTS Gui Qml Quick QuickControls2)
            qt_add_executable(ladder_${_rung}_gui ${_gui_sources})
            target_link_libraries(ladder_${_rung}_gui PRIVATE
                morph::ladder_${_rung}_gui_lib morph::ladder_app ${_qml_plugin}
                Qt6::Core Qt6::Gui Qt6::Qml Qt6::Quick Qt6::QuickControls2)
            target_compile_definitions(ladder_${_rung}_gui PRIVATE MORPH_LADDER_QML_URI="${_qml_uri}")
            target_compile_features(ladder_${_rung}_gui PRIVATE cxx_std_23)
            set_target_properties(ladder_${_rung}_gui PROPERTIES AUTOMOC ON)
            apply_bigobj(ladder_${_rung}_gui)
            if(AF_COVERAGE)
                apply_coverage(ladder_${_rung}_gui)
            endif()
            # See ladder_${_rung}_lib's identical AF_SANITIZER block above.
            if(DEFINED AF_SANITIZER)
                apply_sanitizers(ladder_${_rung}_gui ${AF_SANITIZER})
            endif()
        endif()
    endif()

    # ── ladder_<rung>_gui_wasm: Emscripten client ────────────────────────
    #
    # Same shape as the desktop client above, and deliberately so: it links the
    # same gui_lib, the same morph::ladder_app (AppContext), and the same
    # ladder_<rung>_qml module, so the only file that differs between the two
    # clients is main()/main_wasm.cpp (examples/TESTING.md, "same client code";
    # bank's shadow-header pattern is explicitly banned there). Its skip
    # reasons are announced for the same reason the desktop block announces
    # its own.
    if(EMSCRIPTEN)
        file(GLOB_RECURSE _gui_wasm_sources CONFIGURE_DEPENDS "${_dir}/gui_wasm/*.cpp")
        set(_gui_wasm_skips "")
        if(_gui_wasm_sources AND NOT TARGET ladder_${_rung}_gui_lib)
            list(APPEND _gui_wasm_skips "it has no gui_lib/*.cpp, so there is no ladder_${_rung}_gui_lib to link")
        endif()
        if(_gui_wasm_sources AND NOT _qml_plugin)
            if(NOT _qml_files)
                list(APPEND _gui_wasm_skips "it has no gui/qml/*.qml, so there is no ladder_${_rung}_qml module to load")
            else()
                list(APPEND _gui_wasm_skips "MORPH_BUILD_FORMS_QML is OFF, so ladder_${_rung}_qml was not built")
            endif()
        endif()
        if(_gui_wasm_skips)
            list(JOIN _gui_wasm_skips "; and " _gui_wasm_skip_why)
            message(STATUS "morph_add_rung: rung '${_rung}' has gui_wasm/*.cpp but ladder_${_rung}_gui_wasm "
                           "is skipped — ${_gui_wasm_skip_why}")
        endif()
        # A ladder WASM client is a *pure remote client* (IMPLEMENTATION.md
        # rule 4's WASM clause: persistence lives server-side), but it still
        # has to name its rung's model type — BridgeHandler<Model> is a
        # template over it. Without MORPH_CLIENT_ONLY the registrars that
        # closure over Model's constructor and execute() bodies are still
        # emitted, and the wasm link fails on every database symbol those
        # bodies reach (docs/spec/core/registry.md names a browser build as
        # the motivating case). That failure is a wall of undefined symbols
        # from inside FetchContent'd code, so it is caught here instead.
        if(_gui_wasm_sources AND NOT _gui_wasm_skips AND NOT MORPH_CLIENT_ONLY)
            message(FATAL_ERROR
                "morph_add_rung: rung '${_rung}' builds ladder_${_rung}_gui_wasm, which needs "
                "-DMORPH_CLIENT_ONLY=ON. A WASM client dispatches every action to a server and "
                "never hosts a model, but without that option morph still emits the model-owning "
                "registrars, whose closures reference the model's ODBC-backed execute() bodies — "
                "unlinkable in a browser. See docs/spec/core/registry.md, \"MORPH_CLIENT_ONLY\".")
        endif()
        if(_gui_wasm_sources AND NOT _gui_wasm_skips)
            find_package(Qt6 REQUIRED COMPONENTS Gui Qml Quick QuickControls2)
            qt_add_executable(ladder_${_rung}_gui_wasm ${_gui_wasm_sources})
            target_link_libraries(ladder_${_rung}_gui_wasm PRIVATE
                morph::morph morph::qt morph_qt_impl
                morph::ladder_${_rung}_gui_lib morph::ladder_app ${_qml_plugin}
                Qt6::Core Qt6::Gui Qt6::Qml Qt6::Quick Qt6::QuickControls2)
            target_compile_definitions(ladder_${_rung}_gui_wasm PRIVATE MORPH_LADDER_QML_URI="${_qml_uri}")
            target_compile_features(ladder_${_rung}_gui_wasm PRIVATE cxx_std_23)
            set_target_properties(ladder_${_rung}_gui_wasm PROPERTIES AUTOMOC ON)
        endif()
    endif()

    # ── ladder_<rung>_server: standalone server binary (native only) ────
    if(NOT EMSCRIPTEN)
        file(GLOB_RECURSE _server_sources CONFIGURE_DEPENDS "${_dir}/src/server/*.cpp")
        if(_server_sources AND TARGET ladder_${_rung}_lib)
            add_executable(ladder_${_rung}_server ${_server_sources})
            target_link_libraries(ladder_${_rung}_server PRIVATE
                morph::ladder_${_rung}_lib morph::qt morph_qt_impl Qt6::Core)
            target_compile_features(ladder_${_rung}_server PRIVATE cxx_std_23)
            apply_bigobj(ladder_${_rung}_server)
            if(AF_COVERAGE)
                apply_coverage(ladder_${_rung}_server)
            endif()
            # See ladder_${_rung}_lib's identical AF_SANITIZER block above.
            if(DEFINED AF_SANITIZER)
                apply_sanitizers(ladder_${_rung}_server ${AF_SANITIZER})
            endif()
        endif()
    endif()

    # ── ladder_<rung>_tests: Catch2 model + presenter tests ──────────────
    if(NOT EMSCRIPTEN)
        file(GLOB_RECURSE _test_sources CONFIGURE_DEPENDS "${_dir}/tests/*.cpp")
        if(_test_sources)
            # examples/common/testkit/testkit_main.cpp is compiled into every
            # rung's test binary rather than linked from morph_ladder_testkit:
            # that library links Catch2::Catch2 (the no-main variant), so a
            # rung whose tests/ holds only TEST_CASE translation units has no
            # `main` at all and fails to link. The main is Qt-owning (a
            # QCoreApplication that outlives every QObject Catch2 constructs —
            # see that file's own comment), which every rung needs anyway the
            # moment it touches BackendRig's Socket mode. It stays a compiled
            # source rather than a library member so ladder_common_tests, which
            # already compiles the same file directly, keeps exactly one
            # definition of `main`.
            add_executable(ladder_${_rung}_tests
                ${_test_sources}
                "${PROJECT_SOURCE_DIR}/examples/common/testkit/testkit_main.cpp")
            target_link_libraries(ladder_${_rung}_tests PRIVATE morph::ladder_testkit)
            # ctest runs a rung's test binary from its own build directory, so
            # repo-relative test data (e.g. tests/fuzz/findings/*, replayed as
            # hostile paste content by pastebin's model suite) cannot be found
            # by a relative path. Compile the source root in instead — the same
            # thing tests/fuzz/CMakeLists.txt does by passing absolute corpus
            # paths on the command line, expressed here as a macro because a
            # Catch2 binary takes no such arguments.
            target_compile_definitions(ladder_${_rung}_tests
                PRIVATE MORPH_LADDER_SOURCE_ROOT="${PROJECT_SOURCE_DIR}")
            if(TARGET ladder_${_rung}_lib)
                # WHOLE_ARCHIVE, not a plain link: a rung's schema TU
                # (src/db/schema.cpp) contributes nothing but static-init
                # side effects — LIGHTWEIGHT_SQL_MIGRATION registers the
                # rung's tables with the process-wide MigrationManager from a
                # namespace-scope initializer. No test references a symbol in
                # that TU, so an ordinary static-library link never pulls the
                # object in and DbFixture::ApplyPendingMigrations() finds no
                # migrations at all ("no such table: pastes"). Pulling the
                # whole archive is the standard fix and keeps the schema
                # exactly where IMPLEMENTATION.md rule 4 puts it, instead of
                # making every rung's test suite name a dummy symbol to force
                # the link.
                target_link_libraries(ladder_${_rung}_tests PRIVATE
                    "$<LINK_LIBRARY:WHOLE_ARCHIVE,morph::ladder_${_rung}_lib>")
                # Same SYSTEM-include demotion as ladder_${_rung}_gui_lib's own
                # identical block above, and for the identical reason:
                # Lightweight's target_include_directories() call is plain
                # PUBLIC, not SYSTEM, so apply_warnings() below (-Werror
                # included) would otherwise apply in full to every Lightweight
                # header a test TU reaches (directly, by testing the model
                # layer, or transitively through template instantiation).
                get_target_property(_lightweight_includes Lightweight::Lightweight INTERFACE_INCLUDE_DIRECTORIES)
                if(_lightweight_includes)
                    target_include_directories(ladder_${_rung}_tests SYSTEM PRIVATE ${_lightweight_includes})
                endif()
                unset(_lightweight_includes)
            endif()
            if(TARGET ladder_${_rung}_gui_lib)
                target_link_libraries(ladder_${_rung}_tests PRIVATE morph::ladder_${_rung}_gui_lib)
            endif()
            # The durable SQLite-backed IOfflineQueue, when the top-level
            # MORPH_BUILD_OFFLINE_SQLITE option built it (off by default -- see
            # the option's own comment in the root CMakeLists.txt: SQLite3 is a
            # system dev package the standard build tree does not require).
            #
            # A rung that exercises the offline queue must therefore compile
            # and pass *both* ways: against morph::offline::InMemoryOfflineQueue
            # always, and additionally against SqliteOfflineQueue when this
            # macro is defined. Gating on the macro rather than on the header's
            # presence is deliberate -- the header exists in every checkout, but
            # including it without SQLite3 on the link line fails at link time,
            # not at compile time, which is the harder failure to read.
            if(TARGET morph::offline_sqlite)
                target_link_libraries(ladder_${_rung}_tests PRIVATE morph::offline_sqlite)
                target_compile_definitions(ladder_${_rung}_tests PRIVATE MORPH_LADDER_HAVE_OFFLINE_SQLITE=1)
            endif()
            # The rung's QML module, so its offscreen engine-load smoke test
            # (examples/TESTING.md, presenter rule 6) can load the *same*
            # Main.qml the desktop client ships — not a copy.
            #
            # MORPH_LADDER_QML_URI is what makes that test compile at all: it is
            # `#ifdef`-guarded on this macro, so a configure without the QML
            # module (see the ladder_<rung>_qml block above) simply compiles it
            # to an empty translation unit instead of failing on a missing
            # <QQmlApplicationEngine>.
            #
            # MORPH_LADDER_TESTKIT_GUI_APP switches testkit_main.cpp's owned
            # application object from QCoreApplication to QGuiApplication for
            # this one binary. Qt Quick cannot instantiate an ApplicationWindow
            # under a plain QCoreApplication — QWindow needs a platform
            # integration, which only QGuiApplication creates — so without this
            # the smoke test aborts rather than failing. Presenter rule 1
            # ("presenters must instantiate under a plain QCoreApplication")
            # keeps its teeth where it is actually enforced: ladder_<rung>_gui_lib
            # links Qt6::Core and nothing else, and ladder_common_tests still
            # runs its presenter suite under a bare QCoreApplication.
            if(_qml_plugin)
                target_link_libraries(ladder_${_rung}_tests PRIVATE
                    ${_qml_plugin} Qt6::Gui Qt6::Qml Qt6::Quick Qt6::QuickControls2)
                target_compile_definitions(ladder_${_rung}_tests PRIVATE
                    MORPH_LADDER_QML_URI="${_qml_uri}" MORPH_LADDER_TESTKIT_GUI_APP)
            endif()
            target_compile_features(ladder_${_rung}_tests PRIVATE cxx_std_23)
            set_target_properties(ladder_${_rung}_tests PROPERTIES AUTOMOC ON)
            apply_warnings(ladder_${_rung}_tests)
            apply_bigobj(ladder_${_rung}_tests)
            if(AF_COVERAGE)
                apply_coverage(ladder_${_rung}_tests)
            endif()
            # AF_SANITIZER (asan/tsan/ubsan): applied the same way apply_coverage()
            # is above. This is the target kanban's [tsan]-tagged stress test
            # actually links and runs from -- without this, a --preset clang-tsan
            # build compiles it with no sanitizer instrumentation at all, and
            # .github/workflows/ci.yml's kanban-tsan job would build and pass
            # while never actually exercising ThreadSanitizer over the code it
            # claims to cover.
            if(DEFINED AF_SANITIZER)
                apply_sanitizers(ladder_${_rung}_tests ${AF_SANITIZER})
            endif()

            include(Catch)
            get_target_property(_qt_core_dll Qt6::Core IMPORTED_LOCATION)
            cmake_path(GET _qt_core_dll PARENT_PATH _qt_bin_dir)
            catch_discover_tests(ladder_${_rung}_tests
                DISCOVERY_MODE POST_BUILD
                DL_PATHS "${_qt_bin_dir}"
                # One label, not two, and the *rung* one. catch_discover_tests
                # forwards PROPERTIES as a flat CMake list through a
                # `-D VAR=a;b;c` command line, where a list separator and a
                # literal semicolon are indistinguishable and no escaping
                # survives, so a multi-value `LABELS "ladder;ladder-${_rung}"`
                # does not produce a two-label test — it shifts every following
                # name/value pair by one (examples/common/CMakeLists.txt
                # documents the same trap at length).
                #
                # Nothing is lost by dropping the generic `ladder` label:
                # ctest's `-L` is a *regex*, not an exact match, so `-L ladder`
                # matches `ladder-${_rung}` by prefix and CI's
                # `ctest -L ladder -LE stress` selects exactly what it did.
                #
                # Applying the rung label *here* rather than in a post-pass is
                # what makes per-rung selection correct for every test name.
                # A post-pass cannot do it: Catch2's discovery script appends
                # the plain test name to `<target>_TESTS`
                # (`list(APPEND tests "${prefix}${plain_name}${suffix}")`), so
                # a TEST_CASE whose name contains a `;` is flattened into two
                # fragments that name no test, and set_tests_properties then
                # silently applies to nothing. That was morph#173: the case
                # kept `ladder` and never gained `ladder-<rung>`, so
                # `ctest -L ladder-<rung>` under-selected without saying so.
                PROPERTIES LABELS ladder-${_rung} TIMEOUT 120 RESOURCE_LOCK morph_ladder_test_db
            )
            # `journey` still needs a post-pass: it applies to some cases and
            # not others, which catch_discover_tests' uniform PROPERTIES cannot
            # express. It therefore inherits the flattening limitation
            # described above — a journey case whose name contained a `;` would
            # keep its rung label but never gain `journey`. Rather than let
            # that go quiet the way morph#173 did, it is rejected at configure
            # time by the guard below.
            #
            # set_tests_properties *replaces* LABELS rather than appending, so
            # the journey branch restates the rung label alongside `journey`.
            foreach(_journey_src IN LISTS _test_sources)
                file(STRINGS "${_journey_src}" _bad_journey_names
                     REGEX "TEST_CASE\\(\"Journey: [^\"]*;")
                if(_bad_journey_names)
                    message(FATAL_ERROR
                        "morph_add_rung(${_rung}): a \"Journey: \" TEST_CASE name contains a "
                        "semicolon. CMake cannot round-trip that through Catch2's discovered "
                        "test list, so the case would silently never gain its `journey` label "
                        "(morph#173). Rename it.\n  File: ${_journey_src}\n  Name: ${_bad_journey_names}")
                endif()
            endforeach()
            file(GENERATE
                OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/ladder_${_rung}_tests_journey_label.cmake"
                CONTENT "foreach(_ladder_test IN LISTS ladder_${_rung}_tests_TESTS)
    # End-to-end user journeys carry an extra `journey` label so they can be
    # selected (ctest -L journey) or excluded the way `stress` is. Keyed off
    # the test name's own \"Journey: \" prefix rather than a separate source
    # list, so adding a journey needs no CMake edit.
    if(_ladder_test MATCHES \"Journey: \")
        set_tests_properties(\"\${_ladder_test}\" PROPERTIES LABELS \"ladder-${_rung};journey\")
    endif()
endforeach()
"
            )
            set_property(DIRECTORY APPEND PROPERTY TEST_INCLUDE_FILES
                "${CMAKE_CURRENT_BINARY_DIR}/ladder_${_rung}_tests_journey_label.cmake")
        endif()
    endif()

    # ── ladder_<rung>_headless: QProcess test-client binary (rung 4+) ────
    # Native only, like ladder_<rung>_server above: this binary exists to be
    # spawned as a separate OS process by testkit/process_pool.hpp, which has
    # no meaning under Emscripten -- and QtWebSocketBackend's constructor
    # differs there, so it would not compile anyway.
    file(GLOB_RECURSE _headless_sources CONFIGURE_DEPENDS "${_dir}/src/headless/*.cpp")
    if(NOT EMSCRIPTEN AND _headless_sources AND TARGET ladder_${_rung}_gui_lib)
        add_executable(ladder_${_rung}_headless ${_headless_sources})
        target_link_libraries(ladder_${_rung}_headless PRIVATE morph::ladder_${_rung}_gui_lib morph::ladder_app)
        target_compile_features(ladder_${_rung}_headless PRIVATE cxx_std_23)
        set_target_properties(ladder_${_rung}_headless PROPERTIES AUTOMOC ON)
        apply_bigobj(ladder_${_rung}_headless)
        # TEST although this is not a ctest case: it is the spawned far end of
        # the rung's process-separation tests (examples/kanban/tests/
        # test_kanban_process_separation.cpp spawns four of them through
        # testkit/process_pool.hpp), so the src/headless/ code they run exists
        # only in *this* binary's coverage mapping. The children inherit
        # LLVM_PROFILE_FILE through QProcess and exit normally, so they do
        # write profile data -- which llvm-cov could not map to anything until
        # this registration existed, while scripts/coverage.sh named
        # examples/<rung>/src among its SOURCES regardless. Exactly the
        # morph#403 shape, and invisible to
        # scripts/check_coverage_objects.sh, because a binary reached through a
        # compile definition appears in no ctest command. Same reasoning, and
        # the same TEST keyword, as tests/qt's qt_test_server/qt_test_client.
        if(AF_COVERAGE)
            apply_coverage(ladder_${_rung}_headless TEST)
        endif()
        # Same AF_SANITIZER block every other ladder target carries, and not
        # optional here: this executable links ladder_<rung>_gui_lib, which
        # *is* instrumented under a sanitizer preset, so without it the link
        # fails outright on undefined __ubsan_*/__asan_* references. It also
        # keeps the spawned client instrumented, which is the whole point of
        # running the process-separation tests under ASan -- an uninstrumented
        # client would be exactly the kind of sanitizer job that passes while
        # checking nothing.
        if(DEFINED AF_SANITIZER)
            apply_sanitizers(ladder_${_rung}_headless ${AF_SANITIZER})
        endif()
        # Hand the binary's path to the rung's own test suite, so a
        # process-separation test can spawn it through
        # testkit/process_pool.hpp. Same mechanism tests/qt/CMakeLists.txt
        # uses for QT_TEST_CLIENT_BIN; a generator expression rather than a
        # guessed path, because the location differs per generator and config.
        #
        # The dependency is what stops the test from failing on a clean build:
        # nothing the test *links* pulls this executable in, so without it the
        # spawn would race the build that produces the thing being spawned.
        if(TARGET ladder_${_rung}_tests)
            target_compile_definitions(ladder_${_rung}_tests PRIVATE
                MORPH_LADDER_HEADLESS_BIN="$<TARGET_FILE:ladder_${_rung}_headless>")
            add_dependencies(ladder_${_rung}_tests ladder_${_rung}_headless)
        endif()
    endif()

    message(STATUS "morph_add_rung: registered rung '${_rung}'")
endfunction()

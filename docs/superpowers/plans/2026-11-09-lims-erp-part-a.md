# lims ERP Part A: Org and Authorization Foundation Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Split today's lims into link-separated modules (`lims/org`, `lims/lab`, `lims/app`), then give it companies and sites, scoped role assignments, declared authorization, scope-filtered lists and administration screens — Part A of `docs/superpowers/specs/2026-11-09-lims-erp-design.md`.

**Architecture:** `morph_add_rung()` gains `MODULES`/`SHARED_MODULES`/`APP_MODULE`: one static (or interface) library per module, each with its own include directory, linked only to the shared modules (the app module links all of them). `lims/org` is the shared module: the error set, the org entities and `OrgModel` (companies, sites, role assignments), and the declared-authorization evaluator. `lims/lab` is today's sample/analysis/result code, unchanged in namespace (`lims`) and in action ids; every sample gains its site. `lims/app` holds the server-edge `LimsAuthorizer` and the table of declared role requirements; the server (`src/server/`), the screens (`screens/`) and the suites (`tests/`) stay at the rung root, where the ladder's tooling finds them, and link the composed `ladder_lims_lib`.

**Tech Stack:** C++23, CMake ≥ 3.25 (Ninja), morph (header-only), LASTRADA Lightweight ORM over SQLite, glaze, Catch2, Qt 6 (Core; Quick for the render smoke), morph ui/1 authoring API (`morph/ui/author.hpp`).

## Global Constraints

Copied from the design (`docs/superpowers/specs/2026-11-09-lims-erp-design.md`), `examples/IMPLEMENTATION.md` and `AGENTS.md`. Every task's requirements include this section.

- **Dependency rule (design):** "Each module is its own static library with its own include directory, DTOs, entities, migrations and models. A module's target links `lims/org` and, where it posts or records postings, `lims/posting`; nothing else. Because a module's include path holds only its own headers and those two, including another module's header is a compile error, not a lint finding."
- **Shared database (design):** "All modules use one SQLite file through the Lightweight ORM (`IMPLEMENTATION.md` rule 4), with table names prefixed per module. No module reads another's tables."
- **Part A order (design):** "1. The split. Today's lims moves into `lims/lab` and `lims/app` with no behaviour change: the existing suites pass unmodified, apart from include paths. 2. Org. Companies have a base currency and a fiscal-year start; sites belong to a company. Every lab record gains its site. 3. Scoped roles. Role assignments become `(principal, role, scope)`, where the scope is a company or a site. Roles stay non-hierarchical (lims decision 16). Existing assignments migrate to the company of the single seeded site. 4. Declared authorization. Per action: the role it needs, the payload field holding its scope, optional value limits (an approver approves up to a stated amount), and separation-of-duties pairs (one principal may not both create and approve one invoice). The capture-and-verify rule lims enforces today becomes the first declared pair. 5. Visibility. Every list query returns only records in scopes where the principal holds a role. 6. Administration screens for companies, sites, users and role assignments."
- **Hypothesis 6 (design):** "Authorization cannot see the payload. `IAuthorizer::authorize(ctx, modelType, actionType)` decides before the payload is decoded, so a scope, a value limit or a separation-of-duties rule can only be checked inside each model, by hand." Refuted by: "A declared form of rule checked once by the framework for every module, or shown to be domain logic the models must own." This plan builds instance one (lab's capture/verify) in lims and records the outcome; it does **not** design a framework seam — "a guarantee confirmed once is recorded as confirmed and built in lims; its second instance is built against a framework seam designed for both."
- **GUI (design, Part D):** "Every screen is a ui/1 document written with the C++ authoring API and served by the lims server. There is no hand-written QML and no per-screen client code." Part A's administration screens are functional and are rebuilt against the mockups in Part D; nothing under `examples/lims/design/mockups/` is touched by this plan.
- **Rule 1 (IMPLEMENTATION.md):** "All business logic, all invariants, and all persistence access live in plain, single-threaded model classes with typed actions." "Models must re-check their own preconditions and authorization (`Context::principal`) — the schema's `required` and the client gates are UX, not security." "Action failures are thrown as the app's typed error set."
- **Rule 2 (IMPLEMENTATION.md):** "Screens never say what to refresh. Every action that changes data declares its effects with `BRIDGE_REGISTER_EFFECTS` beside its registration, over records registered with `BRIDGE_REGISTER_RECORD` … The rung's bundle loads with no refusal and no warning." Work through `docs/KEEPING-SCREENS-CURRENT.md`'s checklist for every new or changed action.
- **Rule 3 (IMPLEMENTATION.md):** "The only plain type permitted in an action/result field is `std::string`"; "Entity identity: A per-entity strong id type … exposing `hasValue()`"; "Closed sets of states/options: `enum class` **with a `glz::meta`/`glz::enumerate` specialisation**". "Forbidden in any DTO field: `int`, `int64_t`, `double`, `float`, `bool`, raw enums."
- **Rule 4 (IMPLEMENTATION.md):** "All persistence goes through the Lightweight ORM"; "Schema is owned by `LIGHTWEIGHT_SQL_MIGRATION` definitions … Migrations are the *only* DDL mechanism"; "Forbidden: direct `sqlite3_*` calls; hand-written SQL strings outside Lightweight's facilities"; the sanctioned escape tier is "Lightweight's own raw-query facility, invoked from inside the model, with a mandatory finding entry". Prefer the typed `DataMapper` API (`Create`/`Query`/`Update`/`Delete`, `.Where(FieldNameOf<…>, …).All()`) over raw SQL everywhere it can express the operation.
- **Rule 5 (IMPLEMENTATION.md):** "Every model is 100% unit tested … Model tests run the full backend-mode matrix (`Local` / `LocalSingleThread` / `Socket`)"; "every invariant named in the rung README … exists as a named test before the feature is called done."
- **AGENTS.md, comments:** "A comment … states **what the code does now and why**. … No history. … No issue numbers, no commit hashes." Public API keeps complete `@param`/`@tparam`/`@return`.
- **AGENTS.md, verification:** "Ask whether the check would still pass if the feature did nothing. If it would, it is not evidence. Mutate the feature and confirm the check fails." State results as measured or inferred.
- **AGENTS.md, filing bar:** defects in `include/morph`/`src` get an issue; `examples/` and CI configuration are fixed in place and not filed; "The *same* example defect in a second place — File it against `include/morph`."
- **Lane rules (user):** every build/configure is wrapped in `flock /tmp/claude-1000/-home-yaraslau-repo-morph/b1b73e49-c41d-4e27-bb68-b8c0cfacf145/scratchpad/morph-build.lock`, builds at `-j10` at most; tests run under the same lock with `timeout 300`, or as `ctest -j6 -L ladder-lims`. Commit as the current git user, with the session's attribution trailer.

## Build and test commands

The ladder build directory is the `linux-everything` preset's (`build/linux-everything`: clang, Release, `MORPH_BUILD_LADDER=ON`, `MORPH_LADDER_RUNGS=all`, Qt and Qt Quick on). Every command below is used verbatim in the tasks.

- Configure: `flock /tmp/claude-1000/-home-yaraslau-repo-morph/b1b73e49-c41d-4e27-bb68-b8c0cfacf145/scratchpad/morph-build.lock cmake --preset linux-everything`
- Build lims: `flock /tmp/claude-1000/-home-yaraslau-repo-morph/b1b73e49-c41d-4e27-bb68-b8c0cfacf145/scratchpad/morph-build.lock cmake --build build/linux-everything --target ladder_lims_tests ladder_lims_screen_tests ladder_lims_render_smoke ladder_lims_server morph_examples_client -j10`
- Model suite: `flock /tmp/claude-1000/-home-yaraslau-repo-morph/b1b73e49-c41d-4e27-bb68-b8c0cfacf145/scratchpad/morph-build.lock timeout 300 build/linux-everything/examples/lims/ladder_lims_tests`
- Screen suite: `flock /tmp/claude-1000/-home-yaraslau-repo-morph/b1b73e49-c41d-4e27-bb68-b8c0cfacf145/scratchpad/morph-build.lock env QT_QPA_PLATFORM=offscreen timeout 300 build/linux-everything/ladder_lims_screen_tests`
- Everything labelled for the rung: `flock /tmp/claude-1000/-home-yaraslau-repo-morph/b1b73e49-c41d-4e27-bb68-b8c0cfacf145/scratchpad/morph-build.lock ctest --test-dir build/linux-everything -j6 -L ladder-lims --output-on-failure`

A Catch2 tag or name after the binary narrows a run, e.g. `… ladder_lims_tests "[org]"`.

## File structure after Part A

```
examples/lims/
  CMakeLists.txt                       morph_add_rung(NAME lims MODULES org lab app …) + boundary probes
  org/include/.clang-tidy              TableName exemption for org's records
  org/include/lims/org/core/           errors.hpp, model_support.hpp (moved), strong_id.hpp, types.hpp
  org/include/lims/org/db/             org_entity.hpp
  org/include/lims/org/dto/            auth_dto.hpp (moved), company_dto.hpp, role_dto.hpp
  org/include/lims/org/models/         auth_model.hpp (moved), org_model.hpp
  org/include/lims/org/authorization/  declared.hpp, org_authorization.hpp
  org/src/db/org_schema.cpp
  org/src/models/                      auth_model.cpp (moved), org_model.cpp
  org/src/authorization/declared.cpp
  lab/include/.clang-tidy              (moved from include/)
  lab/include/lims/lab/{core,db,dto,models,offline}/   (moved from include/lims/…)
  lab/include/lims/lab/authorization/lab_authorization.hpp
  lab/src/db/schema.cpp, lab/src/models/*.cpp          (moved from src/…)
  lab/src/authorization/lab_authorization.cpp
  app/include/lims/app/lims_authorizer.hpp             (moved from include/lims/auth/)
  app/include/lims/app/declared_requirements.hpp
  src/server/main.cpp                  the server (app-owned; stays where ci.yml and morph_add_rung look)
  screens/                             the bundle (app-owned): lims_screens.cpp + org_screens.cpp
  tests/                               model suites; tests/compile_checks/ holds the boundary probes
```

Module dependency: `org` → nothing in the rung; `lab` → `org`; `app` → `org`, `lab`. `posting` is not created in Part A (no Part A code posts); `morph_add_rung` already supports it as a second shared module.

---

### Task 1: Split lims into org, lab and app modules, with a compile-time boundary

**Files:**
- Modify: `cmake/morph_add_rung.cmake` (argument parsing at the top of `morph_add_rung`; the `ladder_<rung>_lib` block; the `_test_sources` glob; new helper `_morph_rung_module_library`)
- Modify: `examples/lims/CMakeLists.txt`
- Create: `examples/lims/tests/compile_checks/expect_build.cmake`
- Create: `examples/lims/tests/compile_checks/lab_includes_app.cpp`
- Create: `examples/lims/tests/compile_checks/org_includes_lab.cpp`
- Create: `examples/lims/tests/compile_checks/lab_includes_org.cpp`
- Move (git mv): `examples/lims/include/lims/core/errors.hpp` → `examples/lims/org/include/lims/org/core/errors.hpp`; `examples/lims/include/lims/core/model_support.hpp` → `examples/lims/org/include/lims/org/core/model_support.hpp`; `examples/lims/include/lims/dto/auth_dto.hpp` → `examples/lims/org/include/lims/org/dto/auth_dto.hpp`; `examples/lims/include/lims/models/auth_model.hpp` → `examples/lims/org/include/lims/org/models/auth_model.hpp`; `examples/lims/src/models/auth_model.cpp` → `examples/lims/org/src/models/auth_model.cpp`; `examples/lims/include/lims/auth/lims_authorizer.hpp` → `examples/lims/app/include/lims/app/lims_authorizer.hpp`; every remaining `examples/lims/include/lims/{core,db,dto,models,offline}/*.hpp` → `examples/lims/lab/include/lims/lab/{same}/`; `examples/lims/src/db/schema.cpp` → `examples/lims/lab/src/db/schema.cpp`; remaining `examples/lims/src/models/*.cpp` → `examples/lims/lab/src/models/`; `examples/lims/include/.clang-tidy` → `examples/lims/lab/include/.clang-tidy`
- Modify (include paths only): every `.cpp`/`.hpp` under `examples/lims/` that includes a moved header
- Modify: `scripts/coverage.sh:223-229`, `codecov.yml` (lims component paths), `examples/IMPLEMENTATION.md` (rule 1 reference path, rules 4–5 paths, promotion rule), `examples/TESTING.md` (target set), `examples/LADDER.md:32`, `docs/spec/offline/offline.md:798`, `examples/lims/README.md`

**Interfaces:**
- Consumes: nothing from earlier tasks.
- Produces:
  - `morph_add_rung(NAME <rung> [MODULES <m>... SHARED_MODULES <m>... APP_MODULE <m>])`: targets `ladder_<rung>_<module>` (STATIC when `<module>/src/**/*.cpp` exists, else INTERFACE), alias `morph::ladder_<rung>_<module>`, and `ladder_<rung>_lib` as an INTERFACE target whole-archiving every module.
  - Targets `ladder_lims_org`, `ladder_lims_lab`, `ladder_lims_app`; header roots `examples/lims/{org,lab,app}/include`.
  - Include spellings later tasks use: `lims/org/core/errors.hpp`, `lims/org/core/model_support.hpp`, `lims/org/dto/auth_dto.hpp`, `lims/org/models/auth_model.hpp`, `lims/lab/core/types.hpp`, `lims/lab/db/lims_entity.hpp`, `lims/lab/db/database.hpp`, `lims/lab/dto/*.hpp`, `lims/lab/models/*.hpp`, `lims/lab/offline/field_outbox.hpp`, `lims/lab/core/self_journal.hpp`, `lims/app/lims_authorizer.hpp`.
  - ctest tests `lims.module_boundary.lab_includes_app`, `lims.module_boundary.org_includes_lab` (must fail to build), `lims.module_boundary.lab_includes_org` (control, must build), label `ladder-lims`.
  - No namespace, type, action id, table or migration changes.

- [ ] **Step 1: Record the baseline the split must preserve**

Run (on the unmodified tree):
```
flock /tmp/claude-1000/-home-yaraslau-repo-morph/b1b73e49-c41d-4e27-bb68-b8c0cfacf145/scratchpad/morph-build.lock cmake --preset linux-everything
flock /tmp/claude-1000/-home-yaraslau-repo-morph/b1b73e49-c41d-4e27-bb68-b8c0cfacf145/scratchpad/morph-build.lock cmake --build build/linux-everything --target ladder_lims_tests ladder_lims_screen_tests ladder_lims_render_smoke ladder_lims_server morph_examples_client -j10
build/linux-everything/examples/lims/ladder_lims_tests --list-tests | tail -1 > /tmp/claude-1000/-home-yaraslau-repo-morph/b1b73e49-c41d-4e27-bb68-b8c0cfacf145/scratchpad/plan_a_baseline_tests.txt
env QT_QPA_PLATFORM=offscreen build/linux-everything/ladder_lims_screen_tests --list-tests | tail -1 >> /tmp/claude-1000/-home-yaraslau-repo-morph/b1b73e49-c41d-4e27-bb68-b8c0cfacf145/scratchpad/plan_a_baseline_tests.txt
flock /tmp/claude-1000/-home-yaraslau-repo-morph/b1b73e49-c41d-4e27-bb68-b8c0cfacf145/scratchpad/morph-build.lock ctest --test-dir build/linux-everything -j6 -L ladder-lims --output-on-failure
```
Expected: the build succeeds; the file holds two lines of the form `N test cases`; ctest reports `100% tests passed`. Keep the file: Step 9 compares against it.

- [ ] **Step 2: Write the boundary probes and their ctest wiring (the failing test)**

Create `examples/lims/tests/compile_checks/expect_build.cmake`:
```cmake
# SPDX-License-Identifier: Apache-2.0
#
# Builds TARGET in BUILD_DIR and judges the outcome. EXPECT=pass requires the build to succeed. EXPECT=fail requires
# it to fail with the compiler reporting that HEADER cannot be found, so a failure for any other reason is not taken
# for the module boundary holding. Invoked by ctest as
#   cmake -DBUILD_DIR=<dir> -DTARGET=<target> -DEXPECT=<pass|fail> -DHEADER=<header> -P expect_build.cmake
# The build is capped at four jobs: it runs inside a ctest run that other lanes share the machine with.

execute_process(
    COMMAND "${CMAKE_COMMAND}" --build "${BUILD_DIR}" --target "${TARGET}" --parallel 4
    OUTPUT_VARIABLE _output
    ERROR_VARIABLE _output
    RESULT_VARIABLE _result)
if(EXPECT STREQUAL "pass")
    if(NOT _result EQUAL 0)
        message(FATAL_ERROR "${TARGET} was expected to build and did not (exit ${_result}):\n${_output}")
    endif()
    message(STATUS "${TARGET} built")
    return()
endif()
if(_result EQUAL 0)
    message(FATAL_ERROR "${TARGET} built: its include crossed the module boundary and nothing refused it")
endif()
# GCC: `<header>: No such file or directory`; clang: `'<header>' file not found`;
# MSVC: `'<header>': No such file or directory`.
if(NOT _output MATCHES "${HEADER}['\"]?:? *(file not found|No such file or directory)")
    message(FATAL_ERROR "${TARGET} failed, but not because ${HEADER} is missing from its include path:\n${_output}")
endif()
message(STATUS "${TARGET} refused: ${HEADER} is not on its include path")
```

Create `examples/lims/tests/compile_checks/lab_includes_app.cpp`:
```cpp
// SPDX-License-Identifier: Apache-2.0
//
// Compiled as part of lims/lab by the test lims.module_boundary.lab_includes_app, which passes only when this fails
// because lims/app's headers are not on lims/lab's include path. Outside that test the macro is undefined and this
// is an empty translation unit, so a tool that analyses every changed source sees nothing to report.
#ifdef LIMS_MODULE_BOUNDARY_PROBE
#include <lims/app/lims_authorizer.hpp>
#endif
```

Create `examples/lims/tests/compile_checks/org_includes_lab.cpp`:
```cpp
// SPDX-License-Identifier: Apache-2.0
//
// Compiled as part of lims/org by the test lims.module_boundary.org_includes_lab, which passes only when this fails
// because no other module's headers are on a shared module's include path. Outside that test the macro is undefined
// and this is an empty translation unit.
#ifdef LIMS_MODULE_BOUNDARY_PROBE
#include <lims/lab/models/sample_model.hpp>
#endif
```

Create `examples/lims/tests/compile_checks/lab_includes_org.cpp`:
```cpp
// SPDX-License-Identifier: Apache-2.0
//
// The control for the two boundary probes beside it: compiled as part of lims/lab, it includes a shared module's
// header and must build. It shows the probes compile through the module's real include path, so their failures are
// the boundary and not a probe that can never build. Outside its test the macro is undefined and this is empty.
#ifdef LIMS_MODULE_BOUNDARY_PROBE
#include <lims/org/core/errors.hpp>
#endif
```

Replace `examples/lims/CMakeLists.txt` with:
```cmake
# SPDX-License-Identifier: Apache-2.0
#
# lims — rung 3 of the application ladder (examples/lims/README.md), built from modules: org (shared), lab, and app,
# which composes them. All target wiring lives in morph_add_rung() (cmake/morph_add_rung.cmake); this file calls it
# and adds the compile checks that prove the module boundary.

cmake_minimum_required(VERSION 3.25)

morph_add_rung(NAME lims
    MODULES org lab app
    SHARED_MODULES org
    APP_MODULE app)

# A module's include path holds its own headers and the shared modules' only. Each probe compiles one include as part
# of one module, and its test builds it: a probe that crosses the boundary must fail with the compiler naming the
# header, and the control must build, which shows the probes compile through the module's real include path.
if(MORPH_BUILD_TESTS AND TARGET ladder_lims_lab)
    function(_lims_boundary_probe _name _module _expect _header)
        set(_target lims_boundary_${_name})
        add_library(${_target} OBJECT EXCLUDE_FROM_ALL "${CMAKE_CURRENT_SOURCE_DIR}/tests/compile_checks/${_name}.cpp")
        target_link_libraries(${_target} PRIVATE ladder_lims_${_module})
        target_compile_definitions(${_target} PRIVATE LIMS_MODULE_BOUNDARY_PROBE=1)
        # Kept out of compile_commands.json: a translation unit built to fail is not something to lint.
        set_target_properties(${_target} PROPERTIES EXPORT_COMPILE_COMMANDS OFF)
        add_test(NAME lims.module_boundary.${_name}
                 COMMAND ${CMAKE_COMMAND} -DBUILD_DIR=${CMAKE_BINARY_DIR} -DTARGET=${_target} -DEXPECT=${_expect}
                         -DHEADER=${_header} -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/compile_checks/expect_build.cmake)
        # One build of this tree at a time: two ninja runs in one build directory race on its logs.
        set_tests_properties(lims.module_boundary.${_name} PROPERTIES
            LABELS ladder-lims TIMEOUT 300 RESOURCE_LOCK morph_build_tree)
    endfunction()
    _lims_boundary_probe(lab_includes_app lab fail lims/app/lims_authorizer.hpp)
    _lims_boundary_probe(org_includes_lab org fail lims/lab/models/sample_model.hpp)
    _lims_boundary_probe(lab_includes_org lab pass lims/org/core/errors.hpp)
endif()
```

- [ ] **Step 3: Run the configure to see it fail**

Run: `flock /tmp/claude-1000/-home-yaraslau-repo-morph/b1b73e49-c41d-4e27-bb68-b8c0cfacf145/scratchpad/morph-build.lock cmake --preset linux-everything`
Expected: FAIL. `morph_add_rung` ignores the unknown keywords and builds `ladder_lims_lib` as before, so generation stops with `Target "lims_boundary_lab_includes_app" links to: ladder_lims_lab but the target was not found` (one such line per probe).

- [ ] **Step 4: Teach `morph_add_rung` the `MODULES` argument**

In `cmake/morph_add_rung.cmake`, extend the header comment's "Directory -> target convention" block by appending after the `tests/*.cpp` line:
```cmake
#
# A rung built from modules passes MODULES <m>... (and SHARED_MODULES, APP_MODULE). Then each module is its own
# library, ladder_<rung>_<m>, built from <m>/src/**/*.cpp with <m>/include as its include directory (an INTERFACE
# library when it has no sources), and ladder_<rung>_lib is an INTERFACE target that links every module whole-archive.
# Link rules, which make including another module's header a compile error:
#   - a shared module links the shared modules named before it in SHARED_MODULES, and nothing else of the rung;
#   - APP_MODULE links every other module;
#   - every other module links the shared modules only.
# Every module link uses WHOLE_ARCHIVE, the feature ladder_<rung>_lib needs for the schema translation units' static
# registration; one library may carry only one link feature in a link, so all of its links use the same one.
# src/server/, screens/ and tests/ stay at the rung's root and link ladder_<rung>_lib, as for any other rung.
```

Replace the argument parsing at the top of `function(morph_add_rung)`:
```cmake
    set(options "")
    set(oneValueArgs NAME)
    set(multiValueArgs "")
    cmake_parse_arguments(RUNG "${options}" "${oneValueArgs}" "${multiValueArgs}" ${ARGN})

    if(NOT RUNG_NAME)
        message(FATAL_ERROR "morph_add_rung() requires NAME <rung>")
    endif()
```
with:
```cmake
    set(options "")
    set(oneValueArgs NAME APP_MODULE)
    set(multiValueArgs MODULES SHARED_MODULES)
    cmake_parse_arguments(RUNG "${options}" "${oneValueArgs}" "${multiValueArgs}" ${ARGN})

    if(NOT RUNG_NAME)
        message(FATAL_ERROR "morph_add_rung() requires NAME <rung>")
    endif()
    if(RUNG_UNPARSED_ARGUMENTS)
        message(FATAL_ERROR "morph_add_rung(NAME ${RUNG_NAME}): unknown arguments: ${RUNG_UNPARSED_ARGUMENTS}")
    endif()
    if(RUNG_MODULES)
        if(NOT RUNG_APP_MODULE OR NOT RUNG_APP_MODULE IN_LIST RUNG_MODULES)
            message(FATAL_ERROR "morph_add_rung(NAME ${RUNG_NAME}): MODULES needs APP_MODULE naming one of them")
        endif()
        if(RUNG_APP_MODULE IN_LIST RUNG_SHARED_MODULES)
            message(FATAL_ERROR "morph_add_rung(NAME ${RUNG_NAME}): the app module ${RUNG_APP_MODULE} cannot be shared")
        endif()
        foreach(_shared IN LISTS RUNG_SHARED_MODULES)
            if(NOT _shared IN_LIST RUNG_MODULES)
                message(FATAL_ERROR "morph_add_rung(NAME ${RUNG_NAME}): shared module ${_shared} is not in MODULES")
            endif()
        endforeach()
        foreach(_module IN LISTS RUNG_MODULES)
            if(NOT IS_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/${_module}/include")
                message(FATAL_ERROR "morph_add_rung(NAME ${RUNG_NAME}): module ${_module} has no ${_module}/include")
            endif()
        endforeach()
        # A rung-level include/ or model source would join no module and be built by nothing.
        foreach(_stray IN ITEMS include src/models src/db src/app)
            if(IS_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/${_stray}")
                message(FATAL_ERROR "morph_add_rung(NAME ${RUNG_NAME}): a rung built from MODULES has no ${_stray}/; "
                                    "move it into a module")
            endif()
        endforeach()
    elseif(RUNG_SHARED_MODULES OR RUNG_APP_MODULE)
        message(FATAL_ERROR "morph_add_rung(NAME ${RUNG_NAME}): SHARED_MODULES and APP_MODULE need MODULES")
    endif()
```

Replace the line `    if(NOT EMSCRIPTEN)` that opens the `# ── ladder_<rung>_lib` block (the first `if(NOT EMSCRIPTEN)` after `qt_standard_project_setup(REQUIRES 6.5)`) with:
```cmake
    if(RUNG_MODULES)
        foreach(_module IN LISTS RUNG_MODULES)
            if(_module STREQUAL RUNG_APP_MODULE)
                set(_deps ${RUNG_MODULES})
                list(REMOVE_ITEM _deps ${_module})
            elseif(_module IN_LIST RUNG_SHARED_MODULES)
                set(_deps "")
                foreach(_shared IN LISTS RUNG_SHARED_MODULES)
                    if(_shared STREQUAL _module)
                        break()
                    endif()
                    list(APPEND _deps ${_shared})
                endforeach()
            else()
                set(_deps ${RUNG_SHARED_MODULES})
            endif()
            _morph_rung_module_library("${_rung}" "${_dir}" "${_module}" "${_deps}")
        endforeach()
        add_library(ladder_${_rung}_lib INTERFACE)
        add_library(morph::ladder_${_rung}_lib ALIAS ladder_${_rung}_lib)
        foreach(_module IN LISTS RUNG_MODULES)
            target_link_libraries(ladder_${_rung}_lib INTERFACE
                "$<LINK_LIBRARY:WHOLE_ARCHIVE,ladder_${_rung}_${_module}>")
        endforeach()
    elseif(NOT EMSCRIPTEN)
```
(The existing body of that block and its closing `endif()` are unchanged.)

In the `ladder_<rung>_tests` block, directly after `file(GLOB_RECURSE _test_sources CONFIGURE_DEPENDS "${_dir}/tests/*.cpp")`, add:
```cmake
        # tests/compile_checks/ holds sources a rung's own compile checks build on purpose, never its suite.
        list(FILTER _test_sources EXCLUDE REGEX "/tests/compile_checks/")
```

Append the module helper after `endfunction()` of `morph_add_rung` (before `_morph_rung_screen_tests`):
```cmake
# ── ladder_<rung>_<module>: one module of a rung built from MODULES ──────────
# Built from <module>/src/**/*.cpp with <module>/include as its only own include directory, and linked whole-archive
# to @p _deps, the modules the link rules in morph_add_rung's header allow it. A module with no sources is an
# INTERFACE library: its headers and its links, nothing to compile.
function(_morph_rung_module_library _rung _dir _module _deps)
    set(_target ladder_${_rung}_${_module})
    set(_root "${_dir}/${_module}")
    file(GLOB_RECURSE _sources CONFIGURE_DEPENDS "${_root}/src/*.cpp")
    # Listed so AUTOMOC sees a Q_OBJECT declared in include/ apart from its .cpp, as for ladder_<rung>_lib.
    file(GLOB_RECURSE _headers CONFIGURE_DEPENDS "${_root}/include/*.hpp")
    if(_sources)
        add_library(${_target} STATIC ${_sources} ${_headers})
        set(_scope PUBLIC)
        set_target_properties(${_target} PROPERTIES AUTOMOC ON)
        apply_bigobj(${_target})
        # Lightweight's headers are not -Werror clean, so no apply_warnings(), as for ladder_<rung>_lib.
        if(AF_COVERAGE)
            apply_coverage(${_target})
        endif()
        if(DEFINED AF_SANITIZER)
            apply_sanitizers(${_target} ${AF_SANITIZER})
        endif()
    else()
        add_library(${_target} INTERFACE)
        set(_scope INTERFACE)
    endif()
    add_library(morph::${_target} ALIAS ${_target})
    target_include_directories(${_target} ${_scope} "${_root}/include" "${PROJECT_SOURCE_DIR}/examples/common")
    target_link_libraries(${_target} ${_scope} morph::morph Lightweight::Lightweight Qt6::Core)
    target_compile_features(${_target} ${_scope} cxx_std_23)
    foreach(_dep IN LISTS _deps)
        target_link_libraries(${_target} ${_scope} "$<LINK_LIBRARY:WHOLE_ARCHIVE,ladder_${_rung}_${_dep}>")
    endforeach()
endfunction()
```

- [ ] **Step 5: Move the files into their modules**

```bash
cd /home/yaraslau/repo/morph/examples/lims
mkdir -p org/include/lims/org/core org/include/lims/org/dto org/include/lims/org/models org/src/models \
         app/include/lims/app lab/include/lims/lab lab/src
git mv include/lims/core/errors.hpp org/include/lims/org/core/errors.hpp
git mv include/lims/core/model_support.hpp org/include/lims/org/core/model_support.hpp
git mv include/lims/dto/auth_dto.hpp org/include/lims/org/dto/auth_dto.hpp
git mv include/lims/models/auth_model.hpp org/include/lims/org/models/auth_model.hpp
git mv src/models/auth_model.cpp org/src/models/auth_model.cpp
git mv include/lims/auth/lims_authorizer.hpp app/include/lims/app/lims_authorizer.hpp
for d in core db dto models offline; do git mv include/lims/$d lab/include/lims/lab/$d; done
git mv include/.clang-tidy lab/include/.clang-tidy
git mv src/db lab/src/db
git mv src/models lab/src/models
ls include src
```
Expected: `ls: cannot access 'include': No such file or directory`, and `src` lists only `server`.

- [ ] **Step 6: Rewrite the include paths and the path references**

```bash
cd /home/yaraslau/repo/morph
files=$(git ls-files examples/lims examples/client examples/IMPLEMENTATION.md docs/spec/offline/offline.md codecov.yml \
        | grep -v '^examples/lims/design/')
perl -pi -e '
  s{examples/lims/include/lims/(core|db|dto|models|offline)/}{examples/lims/lab/include/lims/lab/$1/}g;
  s{examples/lims/src/(models|db)/}{examples/lims/lab/src/$1/}g;
  s{(?<![\w/.-])include/lims/(core|db|dto|models|offline)/}{lab/include/lims/lab/$1/}g;
  s{lims/core/(errors|model_support)\.hpp}{lims/org/core/$1.hpp}g;
  s{lims/dto/auth_dto\.hpp}{lims/org/dto/auth_dto.hpp}g;
  s{lims/models/auth_model\.hpp}{lims/org/models/auth_model.hpp}g;
  s{lims/auth/lims_authorizer\.hpp}{lims/app/lims_authorizer.hpp}g;
  s{(?<![\w/.-])lims/(core|db|dto|models|offline)/}{lims/lab/$1/}g;
' $files
git grep -nE '(^|[^[:alnum:]_/.-])lims/(core|db|dto|models|offline|auth)/' -- examples docs/spec codecov.yml ':!examples/lims/design'
git grep -n 'examples/lims/include\|examples/lims/src/models\|examples/lims/src/db' -- . ':!docs/superpowers'
```
Expected: both `git grep` commands print nothing.

Then in `examples/lims/lab/include/.clang-tidy`, replace:
```
# `examples/lims/include/` is this rung's own header tree. It holds 15 .hpp
# and no .cpp at all, so this file governs declarations and nothing else, and
```
with:
```
# `examples/lims/lab/include/` is the header tree of lims's lab module. It
# holds .hpp files and no .cpp at all, so this file governs declarations and
# nothing else, and
```

In `codecov.yml`, the lims component's `paths:` now reads (the perl pass rewrote the first two; add the org lines):
```yaml
      paths:
        - examples/lims/lab/src/models/**
        - examples/lims/lab/include/lims/lab/models/**
        - examples/lims/org/src/models/**
        - examples/lims/org/include/lims/org/models/**
```

In `scripts/coverage.sh`, replace:
```bash
for _rung in "${RUNG_TEST_EXES[@]}"; do
    for _sub in include src screens; do
        if [ -d "examples/${_rung}/${_sub}" ]; then
            SOURCES+=("examples/${_rung}/${_sub}")
        fi
    done
done
```
with:
```bash
for _rung in "${RUNG_TEST_EXES[@]}"; do
    for _sub in include src screens; do
        if [ -d "examples/${_rung}/${_sub}" ]; then
            SOURCES+=("examples/${_rung}/${_sub}")
        fi
    done
    # A rung built from modules (morph_add_rung's MODULES) keeps each module's include/ and src/ in
    # examples/<rung>/<module>/.
    for _module_include in "examples/${_rung}"/*/include; do
        [ -d "$_module_include" ] || continue
        SOURCES+=("$_module_include")
        if [ -d "${_module_include%/include}/src" ]; then
            SOURCES+=("${_module_include%/include}/src")
        fi
    done
done
```
(`src/server/` is now the only rung-level `src/` in lims; it was already in `SOURCES` before the split, so the measured set is unchanged apart from the move.)

- [ ] **Step 7: Build and run the boundary tests**

Run:
```
flock /tmp/claude-1000/-home-yaraslau-repo-morph/b1b73e49-c41d-4e27-bb68-b8c0cfacf145/scratchpad/morph-build.lock cmake --preset linux-everything
flock /tmp/claude-1000/-home-yaraslau-repo-morph/b1b73e49-c41d-4e27-bb68-b8c0cfacf145/scratchpad/morph-build.lock cmake --build build/linux-everything --target ladder_lims_tests ladder_lims_screen_tests ladder_lims_render_smoke ladder_lims_server morph_examples_client -j10
flock /tmp/claude-1000/-home-yaraslau-repo-morph/b1b73e49-c41d-4e27-bb68-b8c0cfacf145/scratchpad/morph-build.lock ctest --test-dir build/linux-everything -j6 -R 'lims\.module_boundary' --output-on-failure
```
Expected: configure prints no `Impossible to link target` error; the build links every listed binary with no `multiple definition` error; the three boundary tests pass, the two `fail` ones printing `refused: lims/... is not on its include path`.

- [ ] **Step 8: Show the boundary tests measure the boundary (mutation)**

Temporarily add, at the end of `examples/lims/CMakeLists.txt`:
```cmake
target_link_libraries(ladder_lims_lab PUBLIC "$<LINK_LIBRARY:WHOLE_ARCHIVE,ladder_lims_app>")
```
Run the configure and the ctest command of Step 7.
Expected: `lims.module_boundary.lab_includes_app` FAILS with `lims_boundary_lab_includes_app built: its include crossed the module boundary and nothing refused it`; the other two still pass. Remove the line, re-run the configure and the ctest command, and confirm all three pass again.

- [ ] **Step 9: Run every lims suite and compare with the baseline**

Run:
```
flock /tmp/claude-1000/-home-yaraslau-repo-morph/b1b73e49-c41d-4e27-bb68-b8c0cfacf145/scratchpad/morph-build.lock ctest --test-dir build/linux-everything -j6 -L ladder-lims --output-on-failure
build/linux-everything/examples/lims/ladder_lims_tests --list-tests | tail -1
env QT_QPA_PLATFORM=offscreen build/linux-everything/ladder_lims_screen_tests --list-tests | tail -1
git diff --stat -M HEAD -- examples/lims/tests examples/lims/screens/tests
```
Expected: ctest `100% tests passed` (three more tests than the Step 1 run: the boundary checks); the two counts equal the two lines in `plan_a_baseline_tests.txt`; every changed line in the diff is an `#include` line. A suite whose database tests fail with `no such table` means a module's schema translation unit was not whole-archived: fix the link, do not touch a test.

- [ ] **Step 10: Document the module layout**

In `examples/IMPLEMENTATION.md`, replace the promotion-rule paragraph's first sentence
```
**The promotion rule (rule-of-three, from the round-7 review):** an
app-built answer to a framework gap (the polling helper with its timeout,
an op-id ledger, epoch tokens, a recursive validator, redaction-on-serve)
may be built twice in `examples/`. The moment a **third** rung consumes it,
```
with
```
**The promotion rule (rule-of-three, from the round-7 review):** an
app-built answer to a framework gap (the polling helper with its timeout,
an op-id ledger, epoch tokens, a recursive validator, redaction-on-serve)
may be built twice in `examples/`. A module of a rung built from modules
(lims's `org`, `lab`, and the ERP modules that follow) counts as a consumer
of its own, and inside such a rung the **second** module's need promotes, as
`AGENTS.md`'s second-place signal says. Across rungs, the moment a **third** rung consumes it,
```
In rule 4's first bullet, replace ``include/<rung>/db/*_entity.hpp`` with ``include/<rung>/db/*_entity.hpp`` (or, in a rung built from modules, ``<module>/include/<rung>/<module>/db/*_entity.hpp``). In rule 5's first bullet replace ``src/models/` + `include/<rung>/models/`` with ``src/models/` + `include/<rung>/models/` (per module in a rung built from modules)``.

In `examples/TESTING.md`, after the sentence ending `…and a TIMEOUT on every binary.` add:
```
  A rung built from modules passes `MODULES`, `SHARED_MODULES` and
  `APP_MODULE`: each module becomes `ladder_<rung>_<module>` with its own
  include directory, linked only to the shared modules (the app module to all),
  and `ladder_<rung>_lib` links them all whole-archive
  (`cmake/morph_add_rung.cmake` states the rules); lims is built that way.
```
In `examples/LADDER.md:32` replace `bookmarks and lims are built by \`morph_add_rung()\`;` with `bookmarks and lims are built by \`morph_add_rung()\` (lims from modules, one library each);`.

In `examples/lims/README.md`, add before `## Resolved design decisions`:
```markdown
## Modules

lims is built from modules, each its own library with its own include directory (`morph_add_rung`'s `MODULES`):

| Module | Holds | Links |
|---|---|---|
| `org/` (shared) | the error set, the model support every module uses, sign-in, companies, sites, role assignments and the declared-authorization evaluator | nothing in lims |
| `lab/` | samples, analyses, results, verification, offline capture, ignition tests | `org` |
| `app/` | the server edge's `LimsAuthorizer` and the table of declared role requirements | every module |

The server (`src/server/`), the screens (`screens/`) and the suites (`tests/`) belong to the app and link every
module. A module that includes another module's header (other than a shared one) does not compile; the
`lims.module_boundary.*` tests build such an include and pass only when the compiler refuses it, beside a control that
must build.
```
and append to the resolved decisions:
```markdown
### 21. lims is built from modules, and the boundary is a link boundary

Each module is a library whose include path holds its own headers and the shared modules' only, so a module that
includes another module's header fails to compile rather than drawing a lint finding. ERP modules are written by
different teams, and the property under test is whether morph lets them cooperate without knowing each other; a
boundary that only a reviewer enforces would not test it. All modules keep the `lims` namespace and every action id,
record name, table and migration they had: the split changed where code lives, not what it does, and the suites ran
unchanged apart from their include lines. `posting`, the second shared module the design names, is added with the
first module that posts.
```

- [ ] **Step 11: Commit**

```bash
cd /home/yaraslau/repo/morph
git add -A cmake/morph_add_rung.cmake examples/lims ':!examples/lims/design' examples/IMPLEMENTATION.md \
        examples/TESTING.md examples/LADDER.md docs/spec/offline/offline.md codecov.yml scripts/coverage.sh
git diff --cached --name-only -- examples/lims/design
git commit -m "Split lims into org, lab and app modules with a compile-time boundary

morph_add_rung gains MODULES, SHARED_MODULES and APP_MODULE: one library per
module, each with its own include directory, linked only to the shared
modules. lims's code moves without changing a type, action id or table; the
boundary tests build a cross-module include and pass only when it is refused.

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01No1NznqxEBvjhZBaBRLCd6"
```
Expected: the `git diff --cached` line prints nothing: the mockups directory is another lane's and is never staged here.

---
### Task 2: Org — companies and sites, and every sample at a site

**Files:**
- Create: `examples/lims/org/include/lims/org/core/strong_id.hpp`
- Create: `examples/lims/org/include/lims/org/core/types.hpp`
- Create: `examples/lims/org/include/lims/org/db/org_entity.hpp`
- Create: `examples/lims/org/include/lims/org/dto/company_dto.hpp`
- Create: `examples/lims/org/include/lims/org/models/org_model.hpp`
- Create: `examples/lims/org/src/models/org_model.cpp`
- Create: `examples/lims/org/src/db/org_schema.cpp`
- Create: `examples/lims/org/include/.clang-tidy`
- Modify: `examples/lims/lab/include/lims/lab/core/types.hpp` (strong-id macros move to org)
- Modify: `examples/lims/lab/include/lims/lab/dto/sample_dto.hpp` (`RegisterSample::siteId`, `SampleView::siteId`)
- Modify: `examples/lims/lab/include/lims/lab/dto/sample_entry_dto.hpp` (`HeaderSection::siteId`, `HeaderView::siteId`, `SaveError::noSite`)
- Modify: `examples/lims/lab/include/lims/lab/db/lims_entity.hpp` (`SampleRecord::siteId`)
- Modify: `examples/lims/lab/src/db/schema.cpp` (migration `20261109000002`)
- Modify: `examples/lims/lab/src/models/sample_model.cpp` (`toView`, `execute(RegisterSample)`)
- Modify: `examples/lims/lab/src/models/sample_entry.cpp` (`headerView`, `LoadSample`, `SaveSample`)
- Modify: `examples/lims/screens/lims_screens.cpp` (registration picks a site; `noSite` refusal text)
- Create: `examples/lims/tests/migration_probe.hpp`
- Create: `examples/lims/tests/test_org_companies.cpp`
- Create: `examples/lims/tests/test_sample_sites.cpp`
- Modify: `examples/lims/tests/lims_test_support.hpp` (`seededSite()`, `seededCompany()`)
- Modify (call sites): every `lims::RegisterSample{` in `examples/lims/tests/*.cpp` and `examples/lims/screens/tests/*.cpp`; `examples/lims/tests/test_sample_entry.cpp` (`completeCopy`); `examples/lims/tests/test_lims_payload_shape.cpp`; `examples/lims/screens/tests/test_lims_screens.cpp`; `examples/lims/screens/tests/test_lims_sample_screen.cpp`; `examples/lims/tests/test_backend_matrix.cpp` (include)

**Interfaces:**
- Consumes (Task 1): targets `ladder_lims_org`/`ladder_lims_lab`; `lims/org/core/errors.hpp` (`lims::ValidationError`, `lims::NotFound`, `lims::EmptyPrincipalError`), `lims/org/core/model_support.hpp` (`lims::requirePrincipal()`, `lims::nowMillis()`, `lims::timestampFromMillis(std::int64_t)`).
- Produces:
  - Macros `LIMS_DEFINE_STRONG_ID(Name)`, `LIMS_DEFINE_STRONG_ID_WIRE(Name)`, `LIMS_DEFINE_STRONG_ID_SHAPE_TAG(Name, Tag)` in `lims/org/core/strong_id.hpp` (defined for every includer; no longer `#undef`'d).
  - In namespace `lims` (`lims/org/core/types.hpp`): `struct CompanyId`, `struct SiteId` (strong ids, shape tags `"lims.companyId"`, `"lims.siteId"`); `enum class CurrencyCode : std::uint8_t { EUR, USD, GBP, CHF, JPY }`; `enum class Month : std::uint8_t { January, …, December }`.
  - `lims::db::kSeededCompanyId == 1`, `lims::db::kSeededSiteId == 1`; `lims::db::CompanyRecord` (`org_companies`), `lims::db::SiteRecord` (`org_sites`).
  - DTOs (`lims/org/dto/company_dto.hpp`): `CreateCompany{std::string name; CurrencyCode baseCurrency; Month fiscalYearStart;}`, `CompanyView{CompanyId id; std::string name; CurrencyCode baseCurrency; Month fiscalYearStart;}` (record `"Org_Company"`), `ListCompanies{}`, `ListCompaniesResult{std::vector<CompanyView> companies;}`, `CreateSite{CompanyId companyId; std::string name;}`, `SiteView{SiteId id; CompanyId companyId; std::string name;}` (record `"Org_Site"`), `ListSites{}`, `ListSitesResult{std::vector<SiteView> sites;}`.
  - `class lims::OrgModel` (registered `"OrgModel"`): `CompanyView execute(const CreateCompany&)`, `ListCompaniesResult execute(const ListCompanies&)`, `SiteView execute(const CreateSite&)`, `ListSitesResult execute(const ListSites&)`, `static std::optional<SiteView> findSite(SiteId site)`.
  - Lab: `RegisterSample{SiteId siteId; ClientId clientId; std::string reference;}`, `SampleView::siteId`, `HeaderSection::siteId` (last member, optional in the form), `HeaderView::siteId`, `SaveError::noSite`, `SampleRecord::siteId` (`Light::Field<std::optional<std::int64_t>>`, column `site_id`).
  - Test helpers: `lims::test::seededSite()` → `lims::SiteId`, `lims::test::seededCompany()` → `lims::CompanyId`; `lims::test::MigrationProbe` (`migration_probe.hpp`).

- [ ] **Step 1: Write the failing org tests**

Create `examples/lims/tests/test_org_companies.cpp`:
```cpp
// SPDX-License-Identifier: Apache-2.0
//
// Companies and sites (lims/org): the scopes every later record and role is judged in. The first org migration seeds
// one company and one site, because every record that existed before companies did needs one to belong to.

#include <catch2/catch_test_macros.hpp>
#include <glaze/glaze.hpp>
#include <morph/core/registry.hpp>
#include <string>

#include "lims/org/core/errors.hpp"
#include "lims/org/dto/company_dto.hpp"
#include "lims/org/models/org_model.hpp"
#include "lims_test_support.hpp"
#include "testkit/db_fixture.hpp"

using lims::test::ScopedPrincipal;
using morph::ladder::testkit::DbFixture;

TEST_CASE("org: the first migration seeds one company with one site", "[lims][org]") {
    DbFixture fixture;
    ScopedPrincipal const alice{"alice"};
    lims::OrgModel org;

    auto const companies = org.execute(lims::ListCompanies{}).companies;
    REQUIRE(companies.size() == 1);
    CHECK(companies.front().id == lims::test::seededCompany());
    CHECK(companies.front().name == "Main laboratory");
    CHECK(companies.front().baseCurrency == lims::CurrencyCode::EUR);
    CHECK(companies.front().fiscalYearStart == lims::Month::January);

    auto const sites = org.execute(lims::ListSites{}).sites;
    REQUIRE(sites.size() == 1);
    CHECK(sites.front().id == lims::test::seededSite());
    CHECK(sites.front().companyId == lims::test::seededCompany());
    CHECK(sites.front().name == "Main site");
}

// Mutation: store the base currency as EUR whatever is asked (the JPY company reads back EUR).
TEST_CASE("org: a company keeps its base currency and fiscal-year start", "[lims][org]") {
    DbFixture fixture;
    ScopedPrincipal const alice{"alice"};
    lims::OrgModel org;

    auto const created = org.execute(lims::CreateCompany{
        .name = "Osaka branch", .baseCurrency = lims::CurrencyCode::JPY, .fiscalYearStart = lims::Month::April});
    REQUIRE(created.id.hasValue());
    CHECK(created.baseCurrency == lims::CurrencyCode::JPY);
    CHECK(created.fiscalYearStart == lims::Month::April);

    auto const companies = org.execute(lims::ListCompanies{}).companies;
    REQUIRE(companies.size() == 2);
    CHECK(companies.back().name == "Osaka branch");
    CHECK(companies.back().baseCurrency == lims::CurrencyCode::JPY);
    CHECK(companies.back().fiscalYearStart == lims::Month::April);
}

// Mutation: skip the company lookup (a site is created under company 999).
TEST_CASE("org: a site belongs to a company that exists", "[lims][org]") {
    DbFixture fixture;
    ScopedPrincipal const alice{"alice"};
    lims::OrgModel org;

    auto const site = org.execute(lims::CreateSite{.companyId = lims::test::seededCompany(), .name = "Field lab"});
    CHECK(site.companyId == lims::test::seededCompany());
    REQUIRE(lims::OrgModel::findSite(site.id).has_value());
    CHECK(lims::OrgModel::findSite(site.id)->name == "Field lab");
    CHECK_FALSE(lims::OrgModel::findSite(lims::SiteId{999}).has_value());

    CHECK_THROWS_AS(org.execute(lims::CreateSite{.companyId = lims::CompanyId{999}, .name = "Nowhere"}), lims::NotFound);
    CHECK_THROWS_AS(org.execute(lims::CreateSite{.companyId = lims::test::seededCompany(), .name = {}}),
                    lims::ValidationError);
    CHECK_THROWS_AS(org.execute(lims::CreateCompany{.name = {}, .baseCurrency = lims::CurrencyCode::EUR,
                                                    .fiscalYearStart = lims::Month::January}),
                    lims::ValidationError);
}

TEST_CASE("org: creating a company or a site needs an authenticated principal", "[lims][org]") {
    DbFixture fixture;
    lims::OrgModel org;
    CHECK_THROWS_AS(org.execute(lims::CreateCompany{.name = "Anonymous", .baseCurrency = lims::CurrencyCode::EUR,
                                                    .fiscalYearStart = lims::Month::January}),
                    lims::EmptyPrincipalError);
    CHECK_THROWS_AS(org.execute(lims::CreateSite{.companyId = lims::test::seededCompany(), .name = "Anonymous"}),
                    lims::EmptyPrincipalError);
}

// NOLINTBEGIN(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access): each key is checked before it is read.
TEST_CASE("org: the company and site forms are served with explicit submit and closed option sets", "[lims][org]") {
    std::string const document = morph::model::detail::defaultDispatcher().schemasJson("OrgModel");
    glz::generic_u64 dom{};
    REQUIRE_FALSE(glz::read_json(dom, document));
    for (char const* const actionType : {"CreateCompany", "CreateSite"}) {
        INFO("action type: " << actionType);
        REQUIRE(dom.contains(actionType));
        REQUIRE(dom[actionType].contains("x-submitMode"));
        CHECK(dom[actionType]["x-submitMode"].get<std::string>() == "explicit");
    }
    std::string const company = glz::write_json(dom["CreateCompany"]).value_or(std::string{});
    CHECK(company.contains("JPY"));
    CHECK(company.contains("December"));
}
// NOLINTEND(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
```

Create `examples/lims/tests/migration_probe.hpp`:
```cpp
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <Lightweight/Lightweight.hpp>
#include <Lightweight/SqlMigration.hpp>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <system_error>

#include "testkit/db_fixture.hpp"

/// @file
/// A database of its own for a test of a data migration: rows are written in an older schema, and the newer
/// migrations are then applied over them, which the shared test database cannot do because `DbFixture` applies every
/// migration at once.

namespace lims::test {

/// @brief Points Lightweight's default connection at a fresh probe database migrated up to one timestamp, and back at
///        the shared test database when it goes out of scope.
///
/// Construct it after a `DbFixture`, which has already configured the shared connection this restores.
class MigrationProbe {
public:
    /// @param lastApplied The newest migration to apply now; the later ones wait for `applyRest()`.
    explicit MigrationProbe(std::uint64_t lastApplied) {
        removeFile();
        ::Lightweight::SqlConnection::SetDefaultConnectionString(
            ::Lightweight::SqlConnectionString{std::string{"DRIVER=SQLite3;Database="} + kFile});
        auto& migrations = ::Lightweight::SqlMigration::MigrationManager::GetInstance();
        migrations.CloseDataMapper();
        migrations.CreateMigrationHistory();
        migrations.ApplyPendingMigrationsUpTo(::Lightweight::SqlMigration::MigrationTimestamp{lastApplied});
        migrations.CloseDataMapper();
    }

    MigrationProbe(const MigrationProbe&) = delete;
    MigrationProbe& operator=(const MigrationProbe&) = delete;
    MigrationProbe(MigrationProbe&&) = delete;
    MigrationProbe& operator=(MigrationProbe&&) = delete;

    ~MigrationProbe() {
        ::Lightweight::SqlMigration::MigrationManager::GetInstance().CloseDataMapper();
        ::Lightweight::SqlConnection::SetDefaultConnectionString(
            ::Lightweight::SqlConnectionString{::morph::ladder::testkit::DbFixture::computeConnectionString(
                std::getenv("ODBC_CONNECTION_STRING"))});  // NOLINT(concurrency-mt-unsafe): nothing sets it
        removeFile();
    }

    /// @brief Applies every migration newer than the one the probe stopped at.
    void applyRest() {
        auto& migrations = ::Lightweight::SqlMigration::MigrationManager::GetInstance();
        migrations.ApplyPendingMigrations();
        migrations.CloseDataMapper();
    }

private:
    static constexpr char const* kFile = "lims_migration_probe.db";

    /// @brief Deletes the probe file; a file still open elsewhere is left for the next probe to replace.
    static void removeFile() {
        std::error_code ignored;
        std::filesystem::remove(kFile, ignored);
    }
};

}  // namespace lims::test
```

Create `examples/lims/tests/test_sample_sites.cpp`:
```cpp
// SPDX-License-Identifier: Apache-2.0
//
// Every sample is registered at a site (lims/lab over lims/org), and every sample that existed before sites did is
// at the seeded one.

#include <Lightweight/DataMapper/DataMapper.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <optional>
#include <string_view>

#include "lims/lab/db/lims_entity.hpp"
#include "lims/lab/models/sample_model.hpp"
#include "lims/org/core/errors.hpp"
#include "lims/org/db/org_entity.hpp"
#include "lims/org/models/org_model.hpp"
#include "lims_test_support.hpp"
#include "migration_probe.hpp"
#include "testkit/db_fixture.hpp"

using lims::test::ScopedPrincipal;
using morph::ladder::testkit::DbFixture;

// A named namespace: Lightweight reflects records, which needs types with linkage.
namespace lims_site_migration_fixtures {

/// @brief `lims_clients` as the last migration before org wrote it.
struct LegacyClientRecord {
    static constexpr std::string_view TableName = "lims_clients";  // NOLINT(readability-identifier-naming): the ORM reads this spelling
    Light::Field<std::uint64_t, Light::PrimaryKey::ServerSideAutoIncrement, Light::SqlRealName{"id"}> id;
    Light::Field<Light::SqlAnsiString<128>, Light::SqlRealName{"name"}> name;
};

/// @brief `lims_samples` as the last migration before org wrote it: no `site_id`.
struct LegacySampleRecord {
    static constexpr std::string_view TableName = "lims_samples";  // NOLINT(readability-identifier-naming): the ORM reads this spelling
    Light::Field<std::uint64_t, Light::PrimaryKey::ServerSideAutoIncrement, Light::SqlRealName{"id"}> id;
    Light::Field<std::int64_t, Light::SqlRealName{"client_id"}> clientId{0};
    Light::Field<Light::SqlAnsiString<64>, Light::SqlRealName{"reference"}> reference;
    Light::Field<int, Light::SqlRealName{"state"}> state{0};
    Light::Field<std::int32_t, Light::SqlRealName{"version"}> version{1};
    Light::Field<std::int64_t, Light::SqlRealName{"registered_at"}> registeredAt{0};
};

}  // namespace lims_site_migration_fixtures

// Mutation: store no site (the view reads back an empty site).
TEST_CASE("lab: a sample is registered at a site, and its view names the site", "[lims][org][sites]") {
    DbFixture fixture;
    ScopedPrincipal const alice{"alice"};
    lims::OrgModel org;
    auto const fieldLab = org.execute(lims::CreateSite{.companyId = lims::test::seededCompany(), .name = "Field lab"});

    lims::SampleModel bench;
    auto const client = bench.execute(lims::RegisterClient{.name = "Waterworks Ltd"});
    auto const sample =
        bench.execute(lims::RegisterSample{.siteId = fieldLab.id, .clientId = client.clientId, .reference = "WW-1"});
    CHECK(sample.siteId == fieldLab.id);

    lims::SampleModel reader;
    CHECK(reader.execute(lims::OpenSample{.sampleId = sample.id}).siteId == fieldLab.id);
}

// Mutation: drop the site lookup (a sample is registered at site 999).
TEST_CASE("lab: registering at no site, or at one that does not exist, is refused", "[lims][org][sites]") {
    DbFixture fixture;
    ScopedPrincipal const alice{"alice"};
    lims::SampleModel bench;
    auto const client = bench.execute(lims::RegisterClient{.name = "Waterworks Ltd"});
    CHECK_THROWS_AS(bench.execute(lims::RegisterSample{.siteId = {}, .clientId = client.clientId, .reference = "WW-1"}),
                    lims::ValidationError);
    CHECK_THROWS_AS(
        bench.execute(lims::RegisterSample{.siteId = lims::SiteId{999}, .clientId = client.clientId, .reference = "WW-1"}),
        lims::NotFound);
}

// Mutation: skip the noSite check (the save without a site stores a sample).
TEST_CASE("lab: sample entry stores the header's site and refuses a header without one", "[lims][org][sites]") {
    DbFixture fixture;
    ScopedPrincipal const alice{"alice"};
    lims::SampleModel model;
    auto const client = model.execute(lims::RegisterClient{.name = "Waterworks Ltd"});
    model.execute(lims::NewSample{});
    model.execute(
        lims::HeaderSection{.clientId = client.clientId, .reference = "WW-1", .receivedOn = 0, .remarks = {}});
    model.execute(lims::SelectProject{.projectId = 2});
    auto const refused = model.execute(lims::SaveSample{});
    CHECK(refused.error == lims::SaveError::noSite);
    CHECK_FALSE(refused.sampleId.hasValue());

    model.execute(lims::HeaderSection{.clientId = client.clientId,
                                      .reference = "WW-1",
                                      .receivedOn = 0,
                                      .remarks = {},
                                      .siteId = lims::test::seededSite()});
    auto const saved = model.execute(lims::SaveSample{});
    REQUIRE(saved.error == lims::SaveError::none);
    lims::SampleModel other;
    CHECK(other.execute(lims::LoadSample{.id = saved.sampleId}).siteId == lims::test::seededSite());
    CHECK(other.execute(lims::GetSample{}).siteId == lims::test::seededSite());
}

// Mutation: drop the backfill from the lab migration (the legacy sample's site reads back empty).
TEST_CASE("lab: samples registered before sites existed migrate to the seeded site", "[lims][org][sites][migration]") {
    using lims_site_migration_fixtures::LegacyClientRecord;
    using lims_site_migration_fixtures::LegacySampleRecord;
    DbFixture fixture;
    lims::test::MigrationProbe probe{20261107000001};
    {
        Lightweight::DataMapper mapper;
        // The probe really is the old schema: the org tables do not exist yet.
        CHECK_THROWS(mapper.Query<lims::db::CompanyRecord>().All());
        LegacyClientRecord client;
        client.name = Lightweight::SqlAnsiString<128>{"Waterworks Ltd"};
        mapper.Create(client);
        LegacySampleRecord sample;
        sample.clientId = static_cast<std::int64_t>(client.id.Value());
        sample.reference = Lightweight::SqlAnsiString<64>{"WW-OLD"};
        mapper.Create(sample);
    }
    probe.applyRest();
    Lightweight::DataMapper mapper;
    auto const rows = mapper.Query<lims::db::SampleRecord>().All();
    REQUIRE(rows.size() == 1);
    CHECK(rows.front().siteId.Value() == std::optional<std::int64_t>{lims::db::kSeededSiteId});
}
```

In `examples/lims/tests/lims_test_support.hpp`, add includes after `#include <morph/session/session.hpp>`:
```cpp
#include "lims/org/core/types.hpp"
#include "lims/org/db/org_entity.hpp"
```
and, inside `namespace lims::test` after `class ScopedPrincipal`:
```cpp
/// @brief The site the first org migration seeds, where every sample that predates sites is.
/// @return Its id.
[[nodiscard]] inline lims::SiteId seededSite() { return lims::SiteId{lims::db::kSeededSiteId}; }

/// @brief The company the first org migration seeds, which owns the seeded site.
/// @return Its id.
[[nodiscard]] inline lims::CompanyId seededCompany() { return lims::CompanyId{lims::db::kSeededCompanyId}; }
```

- [ ] **Step 2: Run the build to see it fail**

Run: `flock /tmp/claude-1000/-home-yaraslau-repo-morph/b1b73e49-c41d-4e27-bb68-b8c0cfacf145/scratchpad/morph-build.lock cmake --build build/linux-everything --target ladder_lims_tests -j10`
Expected: FAIL — `fatal error: 'lims/org/core/types.hpp' file not found` (from `lims_test_support.hpp`).

- [ ] **Step 3: Move the strong-id macros to org and add the org types**

Create `examples/lims/org/include/lims/org/core/strong_id.hpp` holding the three macro definitions that `examples/lims/lab/include/lims/lab/core/types.hpp` defines today — `LIMS_DEFINE_STRONG_ID`, `LIMS_DEFINE_STRONG_ID_WIRE`, `LIMS_DEFINE_STRONG_ID_SHAPE_TAG` — moved verbatim together with the doc comment above each (the long `PayloadShapeTag` rationale moves with the third), under this preamble:
```cpp
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <compare>
#include <cstdint>
#include <glaze/glaze.hpp>
#include <morph/core/payload_shape_tag.hpp>
#include <optional>
#include <string_view>

/// @file
/// The strong-id boilerplate every lims module defines its entity ids with. Shared, so each module's ids have the
/// same shape on the wire and in a journal fingerprint. The macros stay defined for every includer.
```
In `lims/lab/core/types.hpp`, delete the three `#define` blocks and the three `#undef` lines (`#undef LIMS_DEFINE_STRONG_ID`, `#undef LIMS_DEFINE_STRONG_ID_WIRE`, `#undef LIMS_DEFINE_STRONG_ID_SHAPE_TAG`), keep every `LIMS_DEFINE_STRONG_ID…(…)` use, and add after its existing includes:
```cpp
#include "lims/org/core/strong_id.hpp"
#include "lims/org/core/types.hpp"
```
(lab's DTOs reach `SiteId` through `lims/lab/core/types.hpp`, which every lab DTO already includes.)

Create `examples/lims/org/include/lims/org/core/types.hpp`:
```cpp
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <glaze/glaze.hpp>

#include "lims/org/core/strong_id.hpp"

/// @file
/// The organisation's identities and closed sets: companies, sites, the currencies a company keeps its books in and
/// the month its fiscal year starts.

namespace lims {

LIMS_DEFINE_STRONG_ID(CompanyId);
LIMS_DEFINE_STRONG_ID(SiteId);

/// @brief A currency a company keeps its books in. JPY has no minor unit, so the zero-decimal case is always present.
enum class CurrencyCode : std::uint8_t {
    EUR,  ///< Euro.
    USD,  ///< US dollar.
    GBP,  ///< Pound sterling.
    CHF,  ///< Swiss franc.
    JPY,  ///< Japanese yen.
};

/// @brief A calendar month: the first month of a company's fiscal year.
enum class Month : std::uint8_t {
    January,    ///< January.
    February,   ///< February.
    March,      ///< March.
    April,      ///< April.
    May,        ///< May.
    June,       ///< June.
    July,       ///< July.
    August,     ///< August.
    September,  ///< September.
    October,    ///< October.
    November,   ///< November.
    December,   ///< December.
};

}  // namespace lims

LIMS_DEFINE_STRONG_ID_WIRE(CompanyId);
LIMS_DEFINE_STRONG_ID_WIRE(SiteId);
LIMS_DEFINE_STRONG_ID_SHAPE_TAG(CompanyId, "lims.companyId");
LIMS_DEFINE_STRONG_ID_SHAPE_TAG(SiteId, "lims.siteId");

/// @brief Reflects `CurrencyCode` as its enumerator name.
template <>
struct glz::meta<lims::CurrencyCode> {
    using enum lims::CurrencyCode;

    /// @brief The enumerator ⇄ name mapping glaze reads and writes.
    static constexpr auto value = glz::enumerate(EUR, USD, GBP, CHF, JPY);
};

/// @brief Reflects `Month` as its enumerator name.
template <>
struct glz::meta<lims::Month> {
    using enum lims::Month;

    /// @brief The enumerator ⇄ name mapping glaze reads and writes.
    static constexpr auto value = glz::enumerate(January, February, March, April, May, June, July, August, September,
                                                 October, November, December);
};
```

- [ ] **Step 4: Add the org entities, DTOs, migration and model**

Create `examples/lims/org/include/.clang-tidy`:
```yaml
# The lims org module's header tree holds Lightweight records, whose table name the ORM reads off a member spelled
# exactly `TableName`. The same single exemption, for the same reason, as examples/lims/lab/include/.clang-tidy, which
# carries the measurements behind it.

InheritParentConfig: true

CheckOptions:
  - key:   readability-identifier-naming.VariableIgnoredRegexp
    value: '^TableName$'
```

Create `examples/lims/org/include/lims/org/db/org_entity.hpp`:
```cpp
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <Lightweight/DataMapper/DataMapper.hpp>
#include <cstdint>
#include <string_view>

/// @file
/// The org module's tables, prefixed `org_`. No other module reads them; they ask `OrgModel`.

namespace lims::db {

/// @brief The company the first org migration seeds. Every record that existed before companies belongs to it.
inline constexpr std::int64_t kSeededCompanyId = 1;

/// @brief The site the first org migration seeds in the seeded company. Every sample that existed before sites is
///        at it.
inline constexpr std::int64_t kSeededSiteId = 1;

/// @brief One company: a legal unit with its own books.
struct CompanyRecord {
    static constexpr std::string_view TableName = "org_companies";
    Light::Field<std::uint64_t, Light::PrimaryKey::ServerSideAutoIncrement, Light::SqlRealName{"id"}> id;  // 0
    Light::Field<Light::SqlAnsiString<128>, Light::SqlRealName{"name"}> name;                              // 1
    /// @brief `CurrencyCode` as its underlying integer.
    Light::Field<int, Light::SqlRealName{"base_currency"}> baseCurrency{0};  // 2
    /// @brief `Month` as its underlying integer.
    Light::Field<int, Light::SqlRealName{"fiscal_year_start"}> fiscalYearStart{0};  // 3
};

/// @brief One site: a place a company works at.
struct SiteRecord {
    static constexpr std::string_view TableName = "org_sites";
    Light::Field<std::uint64_t, Light::PrimaryKey::ServerSideAutoIncrement, Light::SqlRealName{"id"}> id;  // 0
    Light::BelongsTo<&CompanyRecord::id, Light::SqlRealName{"company_id"}> company;                        // 1
    Light::Field<Light::SqlAnsiString<128>, Light::SqlRealName{"name"}> name;                              // 2
};

}  // namespace lims::db
```

Create `examples/lims/org/include/lims/org/dto/company_dto.hpp`:
```cpp
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <morph/core/records.hpp>
#include <string>
#include <vector>

#include "lims/org/core/types.hpp"

/// @file
/// Companies and sites as the wire sees them.

namespace lims {

/// @brief Creates a company.
struct CreateCompany {
    /// @brief The company's name.
    std::string name;
    /// @brief The currency its books are kept in.
    CurrencyCode baseCurrency = CurrencyCode::EUR;
    /// @brief The month its fiscal year starts.
    Month fiscalYearStart = Month::January;

    /// @brief Whether the company can be created.
    /// @return `true` when it is named, in at most 128 bytes.
    [[nodiscard]] bool validate() const noexcept { return !name.empty() && name.size() <= 128; }

    /// @brief Sent by its button, never per keystroke.
    static constexpr bool explicitSubmit = true;
};

/// @brief One company.
struct CompanyView {
    /// @brief Its id.
    CompanyId id;
    /// @brief Its name.
    std::string name;
    /// @brief The currency its books are kept in.
    CurrencyCode baseCurrency = CurrencyCode::EUR;
    /// @brief The month its fiscal year starts.
    Month fiscalYearStart = Month::January;
};

/// @brief Lists the companies.
struct ListCompanies {
    /// @brief Always ready.
    /// @return `true`.
    [[nodiscard]] bool validate() const noexcept { return true; }
};

/// @brief The companies, by id.
struct ListCompaniesResult {
    /// @brief The companies.
    std::vector<CompanyView> companies;
};

/// @brief Creates a site in a company.
struct CreateSite {
    /// @brief The company the site belongs to.
    CompanyId companyId;
    /// @brief The site's name.
    std::string name;

    /// @brief Whether the site can be created.
    /// @return `true` when a company is named and the site is named, in at most 128 bytes.
    [[nodiscard]] bool validate() const noexcept {
        return companyId.hasValue() && !name.empty() && name.size() <= 128;
    }

    /// @brief Sent by its button, never per keystroke.
    static constexpr bool explicitSubmit = true;
};

/// @brief One site.
struct SiteView {
    /// @brief Its id.
    SiteId id;
    /// @brief The company it belongs to.
    CompanyId companyId;
    /// @brief Its name.
    std::string name;
};

/// @brief Lists the sites.
struct ListSites {
    /// @brief Always ready.
    /// @return `true`.
    [[nodiscard]] bool validate() const noexcept { return true; }
};

/// @brief The sites, by id.
struct ListSitesResult {
    /// @brief The sites.
    std::vector<SiteView> sites;
};

}  // namespace lims

BRIDGE_REGISTER_RECORD(lims::CompanyView, "Org_Company")
BRIDGE_REGISTER_RECORD(lims::SiteView, "Org_Site")
```

Create `examples/lims/org/src/db/org_schema.cpp`:
```cpp
// SPDX-License-Identifier: Apache-2.0
#include <Lightweight/Lightweight.hpp>
#include <Lightweight/SqlMigration.hpp>
#include <string>

#include "lims/org/core/types.hpp"
#include "lims/org/db/org_entity.hpp"

using namespace Lightweight::SqlColumnTypeDefinitions;
using Lightweight::SqlForeignKeyReferenceDefinition;

// The macro names a struct and a static object whose constructor registers the migration, so neither can be const,
// local to an anonymous namespace, or free of a throwing initializer.
// NOLINTBEGIN(bugprone-throwing-static-initialization,cert-err58-cpp,misc-use-internal-linkage,misc-use-anonymous-namespace,cppcoreguidelines-avoid-non-const-global-variables)
LIGHTWEIGHT_SQL_MIGRATION(20261109000001, "Create org_companies and org_sites, seeded with one of each") {
    plan.CreateTableIfNotExists("org_companies")
        .PrimaryKeyWithAutoIncrement("id", Bigint())
        .RequiredColumn("name", Varchar(128))
        .RequiredColumn("base_currency", Integer())
        .RequiredColumn("fiscal_year_start", Integer());
    plan.CreateTableIfNotExists("org_sites")
        .PrimaryKeyWithAutoIncrement("id", Bigint())
        .RequiredForeignKey("company_id", Bigint(),
                            SqlForeignKeyReferenceDefinition{.tableName = "org_companies", .columnName = "id"})
        .RequiredColumn("name", Varchar(128));
    // Every record that existed before companies needs one to belong to, so a deployment starts with one company
    // and one site, which a later migration moves the existing records to.
    plan.Insert("org_companies")
        .Set("id", lims::db::kSeededCompanyId)
        .Set("name", std::string{"Main laboratory"})
        .Set("base_currency", static_cast<int>(lims::CurrencyCode::EUR))
        .Set("fiscal_year_start", static_cast<int>(lims::Month::January));
    plan.Insert("org_sites")
        .Set("id", lims::db::kSeededSiteId)
        .Set("company_id", lims::db::kSeededCompanyId)
        .Set("name", std::string{"Main site"});
}
// NOLINTEND(bugprone-throwing-static-initialization,cert-err58-cpp,misc-use-internal-linkage,misc-use-anonymous-namespace,cppcoreguidelines-avoid-non-const-global-variables)
```

Create `examples/lims/org/include/lims/org/models/org_model.hpp`:
```cpp
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <morph/core/bridge.hpp>
#include <morph/core/effects.hpp>
#include <morph/core/registry.hpp>
#include <optional>

#include "lims/org/dto/company_dto.hpp"

/// @file
/// `OrgModel`: the companies and sites every other module's records and roles are scoped by.

namespace lims {

/// @brief The organisation: companies and their sites.
///
/// Unkeyed: there is one organisation per deployment. Holds no connection; each `execute()` opens a
/// `Lightweight::DataMapper` for its own duration. Other modules never read the org tables; they call the static
/// lookups here.
class OrgModel {
public:
    /// @brief Creates a company.
    /// @param action Its name, base currency and fiscal-year start.
    /// @return The new company.
    /// @throws ValidationError if the name is empty or longer than 128 bytes.
    /// @throws EmptyPrincipalError if no principal is authenticated.
    CompanyView execute(const CreateCompany& action);

    /// @brief Lists the companies.
    /// @param action Carries nothing.
    /// @return Every company, by id.
    ListCompaniesResult execute(const ListCompanies& action);

    /// @brief Creates a site in a company.
    /// @param action The company and the site's name.
    /// @return The new site.
    /// @throws ValidationError if no company is named, or the name is empty or longer than 128 bytes.
    /// @throws NotFound if the company does not exist.
    /// @throws EmptyPrincipalError if no principal is authenticated.
    SiteView execute(const CreateSite& action);

    /// @brief Lists the sites.
    /// @param action Carries nothing.
    /// @return Every site, by id.
    ListSitesResult execute(const ListSites& action);

    /// @brief The site with @p site's id, for a module that records where something happened.
    /// @param site The site.
    /// @return It, or nothing when @p site is empty or names no site.
    [[nodiscard]] static std::optional<SiteView> findSite(SiteId site);
};

}  // namespace lims

// The registration macros declare a non-const status object each.
// NOLINTBEGIN(misc-const-correctness)
BRIDGE_REGISTER_MODEL(lims::OrgModel, "OrgModel")
BRIDGE_REGISTER_ACTION(lims::OrgModel, lims::CreateCompany, "CreateCompany")
BRIDGE_REGISTER_EFFECTS(lims::CreateCompany, ::morph::effects::changes<lims::CompanyView>)
BRIDGE_REGISTER_ACTION(lims::OrgModel, lims::ListCompanies, "ListCompanies", ::morph::model::Loggable::No)
BRIDGE_REGISTER_ACTION(lims::OrgModel, lims::CreateSite, "CreateSite")
BRIDGE_REGISTER_EFFECTS(lims::CreateSite, ::morph::effects::changes<lims::SiteView>)
BRIDGE_REGISTER_ACTION(lims::OrgModel, lims::ListSites, "ListSites", ::morph::model::Loggable::No)
// NOLINTEND(misc-const-correctness)
```

Create `examples/lims/org/src/models/org_model.cpp`:
```cpp
// SPDX-License-Identifier: Apache-2.0
#include "lims/org/models/org_model.hpp"

#include <Lightweight/DataMapper/DataMapper.hpp>
#include <algorithm>
#include <cstdint>
#include <morph/session/session.hpp>
#include <string>
#include <vector>

#include "lims/org/core/errors.hpp"
#include "lims/org/core/model_support.hpp"
#include "lims/org/db/org_entity.hpp"

namespace lims {

namespace {

/// @brief Maps a company row to its view.
/// @param row The row.
/// @return The view.
[[nodiscard]] CompanyView toView(db::CompanyRecord const& row) {
    return CompanyView{
        .id = CompanyId{static_cast<std::int64_t>(row.id.Value())},
        .name = std::string{row.name.Value().ToStringView()},
        .baseCurrency = static_cast<CurrencyCode>(row.baseCurrency.Value()),
        .fiscalYearStart = static_cast<Month>(row.fiscalYearStart.Value()),
    };
}

/// @brief Maps a site row to its view.
/// @param row The row.
/// @return The view.
[[nodiscard]] SiteView toView(db::SiteRecord const& row) {
    return SiteView{
        .id = SiteId{static_cast<std::int64_t>(row.id.Value())},
        .companyId = CompanyId{static_cast<std::int64_t>(row.company.Value())},
        .name = std::string{row.name.Value().ToStringView()},
    };
}

}  // namespace

CompanyView OrgModel::execute(const CreateCompany& action) {
    requirePrincipal();
    if (!action.validate()) {
        throw ValidationError{"CreateCompany: a name of at most 128 bytes is required"};
    }
    Lightweight::DataMapper mapper;
    db::CompanyRecord row;
    row.name = Lightweight::SqlAnsiString<128>{action.name};
    row.baseCurrency = static_cast<int>(action.baseCurrency);
    row.fiscalYearStart = static_cast<int>(action.fiscalYearStart);
    mapper.Create(row);
    return toView(row);
}

// An action handler: the registry calls it on the instance, like every other.
// NOLINTNEXTLINE(readability-convert-member-functions-to-static)
ListCompaniesResult OrgModel::execute(const ListCompanies& action) {
    static_cast<void>(action);
    Lightweight::DataMapper mapper;
    ListCompaniesResult result;
    for (auto const& row : mapper.Query<db::CompanyRecord>().All()) {
        result.companies.push_back(toView(row));
    }
    std::ranges::sort(result.companies, {}, [](CompanyView const& company) { return *company.id; });
    return result;
}

SiteView OrgModel::execute(const CreateSite& action) {
    requirePrincipal();
    if (!action.validate()) {
        throw ValidationError{"CreateSite: a company and a name of at most 128 bytes are required"};
    }
    Lightweight::DataMapper mapper;
    if (mapper.Query<db::CompanyRecord>()
            .Where(::Lightweight::FieldNameOf<&db::CompanyRecord::id>, "=", *action.companyId)
            .All()
            .empty()) {
        throw NotFound{"CreateSite: company " + std::to_string(*action.companyId) + " does not exist"};
    }
    db::SiteRecord row;
    row.company = static_cast<std::uint64_t>(*action.companyId);
    row.name = Lightweight::SqlAnsiString<128>{action.name};
    mapper.Create(row);
    return toView(row);
}

// An action handler: the registry calls it on the instance, like every other.
// NOLINTNEXTLINE(readability-convert-member-functions-to-static)
ListSitesResult OrgModel::execute(const ListSites& action) {
    static_cast<void>(action);
    Lightweight::DataMapper mapper;
    ListSitesResult result;
    for (auto const& row : mapper.Query<db::SiteRecord>().All()) {
        result.sites.push_back(toView(row));
    }
    std::ranges::sort(result.sites, {}, [](SiteView const& site) { return *site.id; });
    return result;
}

std::optional<SiteView> OrgModel::findSite(SiteId site) {
    if (!site.hasValue()) {
        return std::nullopt;
    }
    Lightweight::DataMapper mapper;
    auto const rows =
        mapper.Query<db::SiteRecord>().Where(::Lightweight::FieldNameOf<&db::SiteRecord::id>, "=", *site).All();
    if (rows.empty()) {
        return std::nullopt;
    }
    return toView(rows.front());
}

}  // namespace lims
```

- [ ] **Step 5: Give every sample its site**

In `examples/lims/lab/include/lims/lab/db/lims_entity.hpp`, append to `struct SampleRecord` after `registeredAt`:
```cpp
    /// @brief The `org_sites` row the sample is at. Nullable in the schema only because the column was added to a
    ///        table with rows; the migration that added it set every row, and every write sets it.
    Light::Field<std::optional<std::int64_t>, Light::SqlRealName{"site_id"}> siteId;  // 6
```

In `examples/lims/lab/src/db/schema.cpp`, add after the `20261107000001` migration (inside the existing `NOLINTBEGIN`/`NOLINTEND` pair) and add `#include "lims/org/db/org_entity.hpp"` to its includes:
```cpp
LIGHTWEIGHT_SQL_MIGRATION(20261109000002, "Give every lims_samples row its site") {
    plan.AlterTable("lims_samples")
        .AddNotRequiredForeignKeyColumn(
            "site_id", Bigint(),
            SqlForeignKeyReferenceDefinition{.tableName = std::string{lims::db::SiteRecord::TableName}, .columnName = "id"});
    // Every sample that predates sites was registered at the one place the lab had, which is the seeded site.
    plan.Update("lims_samples").Set("site_id", lims::db::kSeededSiteId).WhereExpression("site_id IS NULL");
}
```
(`SqlForeignKeyReferenceDefinition::tableName` is a `std::string`, which a `std::string_view` does not convert to implicitly; add `#include <string>`.)

In `examples/lims/lab/include/lims/lab/dto/sample_dto.hpp`, make `RegisterSample`:
```cpp
struct RegisterSample {
    /// @brief The site the sample is registered at.
    SiteId siteId;

    /// @brief The owning client.
    ClientId clientId;

    /// @brief The lab's own reference for the container (textual, free-form).
    std::string reference;

    /// @brief Whether this registration is well-formed.
    /// @return `true` when a site and a client are named and a reference is given.
    [[nodiscard]] bool validate() const noexcept {
        return siteId.hasValue() && clientId.hasValue() && !reference.empty();
    }

    /// @brief Same opt-out, same reason as `RegisterClient::explicitSubmit`.
    static constexpr bool explicitSubmit = true;
};
```
and in `struct SampleView`, directly after `SampleId id;`:
```cpp
    /// @brief The site the sample is at.
    SiteId siteId;
```

In `examples/lims/lab/src/models/sample_model.cpp`, add `#include "lims/org/models/org_model.hpp"`; in `toView(const db::SampleRecord& row)` add after `.id = …,`:
```cpp
        .siteId = SiteId::fromOptional(row.siteId.Value()),
```
and in `execute(const RegisterSample& action)` change the validation message and add the site check and write:
```cpp
    if (!action.validate()) {
        throw ValidationError{"RegisterSample: a site, a client and a reference are required"};
    }
    if (!OrgModel::findSite(action.siteId)) {
        throw NotFound{"RegisterSample: no such site"};
    }
```
and, after `row.reference = …;`:
```cpp
    row.siteId = std::optional<std::int64_t>{*action.siteId};
```

In `examples/lims/lab/include/lims/lab/dto/sample_entry_dto.hpp`: append to `HeaderSection` after `remarks`:
```cpp
    /// @brief The site the sample is at. A save without one is refused with `SaveError::noSite`.
    SiteId siteId;
```
and change its `optionalFields` to
```cpp
    static constexpr std::array optionalFields{std::string_view{"receivedOn"}, std::string_view{"remarks"},
                                               std::string_view{"siteId"}};
```
add to `HeaderView` after `ClientId clientId;`:
```cpp
    /// @brief The site, or empty while none is chosen.
    SiteId siteId;
```
add `noSite,  ///< The header names no site.` after `noProject` in `enum class SaveError`, and make its `glz::meta` read `glz::enumerate(none, noWorkingCopy, noClient, noReference, noProject, noSite)`.

In `examples/lims/lab/src/models/sample_entry.cpp`: add `#include "lims/org/models/org_model.hpp"`; in `headerView()` add `view.siteId = _draft->header.siteId;` after `view.clientId = …;`; in `execute(const LoadSample&)` add `loaded.header.siteId = SiteId::fromOptional(row.siteId.Value());` after the `loaded.header.reference = …;` line; in `execute(const SaveSample&)` add after the `noProject` check:
```cpp
    if (!header.siteId.hasValue()) {
        return {.error = SaveError::noSite, .sampleId = {}};
    }
```
after the client lookup's `NotFound` throw:
```cpp
    if (!OrgModel::findSite(header.siteId)) {
        throw NotFound{"SaveSample: no such site"};
    }
```
and `row.siteId = std::optional<std::int64_t>{*header.siteId};` after each of the two `row.reference = …;` lines (the update and the create branch).

- [ ] **Step 6: Let the registration screen choose a site**

In `examples/lims/screens/lims_screens.cpp`: add `#include "lims/org/dto/company_dto.hpp"` and `#include "lims/org/models/org_model.hpp"`. In `registerScreen()`, after `auto const registered = …;` add:
```cpp
        auto const org = screen.model<OrgModel>("org");
        auto const sites = screen.query<ListSites>("sites", org);
        auto const siteList = ui::element("siteList");
```
In the "Sample" panel, insert before the `Reference` text input:
```cpp
                  ui::table(sites.value().at(&ListSitesResult::sites),
                            {.key = ui::row(&SiteView::id),
                             .columns = {{.name = "name", .label = "Site", .cell = ui::text({.text = ui::row("name")})}},
                             .selectionMode = ui::Selection::Single})
                      .id(siteList)
                      .a11y("Sites")
                      .testId("sites"),
```
and make the register button's run body:
```cpp
                              .onClick = ui::run(registerSample,
                                                 {{&RegisterSample::siteId, ui::call("first", {siteList.selection()})},
                                                  {&RegisterSample::clientId,
                                                   client.at(&RegisterClientResult::clientId)},
                                                  {&RegisterSample::reference, reference.value()}})})
```
In `saveRefusal`, make the innermost fallback
```cpp
               reason("noProject", "Choose a project before saving",
                      reason("noSite", "Choose a site before saving", ui::t("lims.save.notSaved", "Not saved")))));
```
(one more closing parenthesis than today).

- [ ] **Step 7: Update the existing call sites**

```bash
cd /home/yaraslau/repo/morph
perl -pi -e 's/lims::RegisterSample\{\.clientId/lims::RegisterSample{.siteId = lims::test::seededSite(), .clientId/g' \
    examples/lims/tests/*.cpp examples/lims/screens/tests/*.cpp
grep -rn "RegisterSample{" examples/lims/tests examples/lims/screens/tests | grep -vc "siteId = lims::test::seededSite()"
```
Expected: `0` (all 23 call sites now name the seeded site; `test_sample_sites.cpp`'s own explicit ones also contain `.siteId`).

In `examples/lims/tests/test_backend_matrix.cpp`, add `#include "lims_test_support.hpp"` after `#include "lims/lab/offline/field_outbox.hpp"`.

In `examples/lims/tests/test_sample_entry.cpp`, make `completeCopy`'s header:
```cpp
    model.execute(lims::HeaderSection{.clientId = client.clientId,
                                      .reference = "WW-0001",
                                      .receivedOn = 20'000,
                                      .remarks = "cold chain",
                                      .siteId = lims::test::seededSite()});
```
In `examples/lims/screens/tests/test_lims_sample_screen.cpp`, make `storedSample()`'s header
```cpp
    model.execute(lims::HeaderSection{
        .clientId = client.clientId, .reference = "WW-3", .receivedOn = 0, .remarks = {}, .siteId = lims::test::seededSite()});
```
and in "a new sample is refused until complete, then saved", after `editField(harness, {.form = "headerForm", .name = "reference"}, "WW-9");` add:
```cpp
    editField(harness, {.form = "headerForm", .name = "siteId"}, "1");
```
In `examples/lims/screens/tests/test_lims_screens.cpp`, in "registration registers a client and a sample, then opens the sample's lifecycle", before the reference is typed, add:
```cpp
    int const sites = harness.find("Table", "sites").value();
    REQUIRE(harness.waitFor([&harness, sites] {
        auto const rows = harness.backend().children(sites);
        return std::ranges::any_of(rows, [&harness](int row) { return harness.backend().hasProp(row, "rowKey"); });
    }));
    harness.backend().selectRows(sites, {morph::ui::Key{std::int64_t{1}}});
```
(add `#include <algorithm>`).

In `examples/lims/tests/test_lims_payload_shape.cpp`, make the fixture
```cpp
struct RegisterSampleClientIdRetyped {
    lims::SiteId siteId;
    lims::SampleId clientId;
    std::string reference;
};
```
and add `payloadShapeString<lims::CompanyId>()`, `payloadShapeString<lims::SiteId>()` to the `shapes` vector of "Every lims strong id renders a distinct payload shape" (with matching `INFO` lines).

- [ ] **Step 8: Build and run**

Run:
```
flock /tmp/claude-1000/-home-yaraslau-repo-morph/b1b73e49-c41d-4e27-bb68-b8c0cfacf145/scratchpad/morph-build.lock cmake --build build/linux-everything --target ladder_lims_tests ladder_lims_screen_tests ladder_lims_render_smoke ladder_lims_server morph_examples_client -j10
flock /tmp/claude-1000/-home-yaraslau-repo-morph/b1b73e49-c41d-4e27-bb68-b8c0cfacf145/scratchpad/morph-build.lock timeout 300 build/linux-everything/examples/lims/ladder_lims_tests "[org]"
flock /tmp/claude-1000/-home-yaraslau-repo-morph/b1b73e49-c41d-4e27-bb68-b8c0cfacf145/scratchpad/morph-build.lock ctest --test-dir build/linux-everything -j6 -L ladder-lims --output-on-failure
```
Expected: `All tests passed` for `[org]` (9 test cases); ctest `100% tests passed`. If the migration `20261109000002` throws on SQLite inside `AddNotRequiredForeignKeyColumn`, replace that call with `.AddNotRequiredColumn("site_id", Bigint())`, keep the backfill, and add a finding to the README's "Findings raised by this rung" naming the Lightweight call and its error output, after checking Lightweight's issue tracker for it (IMPLEMENTATION.md's preamble).

- [ ] **Step 9: Mutation checks**

Apply each mutation named in a test's `// Mutation:` comment in `test_org_companies.cpp` and `test_sample_sites.cpp` in turn (e.g. delete `row.siteId = …` in `RegisterSample`; delete the `plan.Update(...)` backfill line), rebuild `ladder_lims_tests`, run `… ladder_lims_tests "[org]"`, and confirm the named test FAILS; revert each before the next.
Expected: every mutation fails exactly the test that names it.

- [ ] **Step 10: Record the decision and commit**

Append to `examples/lims/README.md`'s resolved decisions:
```markdown
### 22. Companies and sites live in org, and every sample is at a site

A company has a base currency and a fiscal-year start; a site belongs to one company. The first org migration seeds
one company and one site, because every record that existed before companies needs one to belong to, and the lab
migration that adds `lims_samples.site_id` puts every existing sample at the seeded site.

The site is on the sample, and on nothing hanging off it: results, verifications, conflicts and the entry record reach
their site through their sample, so a second copy of the site could only disagree with the first. Clients and
analysis definitions are reference data the whole lab shares and are not at a site; billing gives clients their
company when it needs to bill them. Ignition tests are the chat's proof and sit outside the sample workflow, so they
are not at a site either; a lab that needs them per site changes this.

A registration names its site, and a missing site is a validation error, not a default: a sample registered nowhere
would be visible to nobody once lists are scoped. Sample entry refuses a save without one as `noSite`, a reply like
its other refusals.
```
Then:
```bash
cd /home/yaraslau/repo/morph
git add -A examples/lims ':!examples/lims/design'
git commit -m "Add companies and sites to lims, and register every sample at a site

lims/org gains OrgModel with companies (base currency, fiscal-year start)
and sites, seeded with one of each; lims_samples gains site_id, and every
existing sample migrates to the seeded site.

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01No1NznqxEBvjhZBaBRLCd6"
```

---
### Task 3: Scoped role assignments `(principal, role, scope)`

**Files:**
- Create: `examples/lims/org/include/lims/org/dto/role_dto.hpp`
- Modify: `examples/lims/org/include/lims/org/core/types.hpp` (`RoleAssignmentId`, `ScopeKind`, `Scope`, `scopeName`)
- Modify: `examples/lims/org/include/lims/org/db/org_entity.hpp` (`RoleAssignmentRecord`)
- Modify: `examples/lims/org/src/db/org_schema.cpp` (migration `20261109000003`)
- Modify: `examples/lims/org/include/lims/org/models/org_model.hpp`, `examples/lims/org/src/models/org_model.cpp`
- Modify: `examples/lims/lab/include/lims/lab/dto/verification_dto.hpp` (role types move out; `GrantRole`, `RoleGrantResult`, `AuditStepKind::RoleGranted` go)
- Modify: `examples/lims/lab/include/lims/lab/models/sample_model.hpp`, `examples/lims/lab/src/models/sample_model.cpp` (`GrantRole`, `rolesOf` go; `requireRole` asks org)
- Modify: `examples/lims/lab/include/lims/lab/db/lims_entity.hpp` (`OperatorRecord` goes)
- Modify: `examples/lims/lab/src/db/schema.cpp` (migration `20261109000004`)
- Modify: `examples/lims/app/include/lims/app/lims_authorizer.hpp`, `examples/lims/src/server/main.cpp`
- Create: `examples/lims/tests/test_org_roles.cpp`
- Modify: `examples/lims/tests/lims_test_support.hpp` (`grant`), `examples/lims/tests/test_verification_audit.cpp`, `examples/lims/tests/test_result_entry.cpp`, `examples/lims/tests/test_backend_matrix.cpp`, `examples/lims/screens/tests/test_lims_screens.cpp`

**Interfaces:**
- Consumes (Task 2): `lims::CompanyId`, `lims::SiteId`, `lims::OrgModel`, `OrgModel::findSite(SiteId)`, `lims::db::kSeededCompanyId`, `lims::test::seededCompany()`, `lims::test::seededSite()`, `lims::test::MigrationProbe`.
- Produces:
  - In `lims/org/core/types.hpp`: `struct RoleAssignmentId` (tag `"lims.roleAssignmentId"`); `enum class ScopeKind : std::uint8_t { Company, Site }`; `using Scope = std::variant<CompanyId, SiteId>`; `std::string scopeName(Scope const&)`.
  - In `lims/org/dto/role_dto.hpp`: `enum class LimsRole { Analyst, Verifier, Supervisor }` and `roleName(LimsRole)` (moved, unchanged); `AssignRole{std::string principal; LimsRole role; CompanyId company; SiteId site;}` (exactly one of `company`/`site`); `RoleAssignmentView{RoleAssignmentId id; std::string principal; LimsRole role; ScopeKind scopeKind; CompanyId company; SiteId site; std::string grantedBy; morph::time::Timestamp grantedAt;}` (record `"Org_RoleAssignment"`); `RevokeRole{RoleAssignmentId assignmentId;}`; `RevokeRoleResult{RoleAssignmentId assignmentId;}`; `ListRoleAssignments{}`/`ListRoleAssignmentsResult{std::vector<RoleAssignmentView> assignments;}`; `UserView{std::string id; std::vector<LimsRole> roles;}` (record `"Org_User"`, keyed by the principal); `ListUsers{}`/`ListUsersResult{std::vector<UserView> users;}`.
  - `OrgModel`: `RoleAssignmentView execute(const AssignRole&)`, `RevokeRoleResult execute(const RevokeRole&)`, `ListRoleAssignmentsResult execute(const ListRoleAssignments&)`, `ListUsersResult execute(const ListUsers&)`, `static std::vector<LimsRole> rolesIn(std::string_view principal, Scope const& scope)` (a site scope includes the roles held at its company), `static std::vector<LimsRole> rolesAnywhere(std::string_view principal)`, `static bool supervisorExists()`.
  - `lims::test::grant(std::string principal, lims::LimsRole role)` → `lims::RoleAssignmentView`, assigning at the seeded company.
  - Removed: `lims::GrantRole`, `lims::RoleGrantResult`, `lims::SampleModel::rolesOf`, `lims::db::OperatorRecord`, `AuditStepKind::RoleGranted`, table `lims_operators`.

- [ ] **Step 1: Write the failing role tests**

Create `examples/lims/tests/test_org_roles.cpp`:
```cpp
// SPDX-License-Identifier: Apache-2.0
//
// Role assignments (lims/org): a principal holds a role in a scope, a company or one of its sites. A role held at a
// company holds at every site of it; roles themselves are not hierarchical (README decision 16).

#include <Lightweight/DataMapper/DataMapper.hpp>
#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <string_view>
#include <vector>

#include "lims/org/core/errors.hpp"
#include "lims/org/dto/role_dto.hpp"
#include "lims/org/models/org_model.hpp"
#include "lims_test_support.hpp"
#include "migration_probe.hpp"
#include "testkit/db_fixture.hpp"

using lims::test::ScopedPrincipal;
using morph::ladder::testkit::DbFixture;

namespace {

/// @brief Whether @p roles holds @p role.
/// @param roles The roles.
/// @param role The role looked for.
/// @return It does.
[[nodiscard]] bool holds(std::vector<lims::LimsRole> const& roles, lims::LimsRole role) {
    return std::ranges::find(roles, role) != roles.end();
}

}  // namespace

// A named namespace: Lightweight reflects records, which needs types with linkage.
namespace lims_role_migration_fixtures {

/// @brief `lims_operators` as lab wrote it before roles were scoped.
struct LegacyOperatorRecord {
    static constexpr std::string_view TableName = "lims_operators";  // NOLINT(readability-identifier-naming): the ORM reads this spelling
    Light::Field<std::uint64_t, Light::PrimaryKey::ServerSideAutoIncrement, Light::SqlRealName{"id"}> id;
    Light::Field<Light::SqlAnsiString<64>, Light::SqlRealName{"principal"}> principal;
    Light::Field<int, Light::SqlRealName{"role"}> role{0};
    Light::Field<Light::SqlAnsiString<64>, Light::SqlRealName{"granted_by"}> grantedBy;
    Light::Field<std::int64_t, Light::SqlRealName{"granted_at"}> grantedAt{0};
};

}  // namespace lims_role_migration_fixtures

// Mutations: drop the supervisor check once one exists (mallory's grant goes through); create a second row for a
// repeated grant (bob's assignment ids differ).
TEST_CASE("org roles: the first assignment bootstraps a lab with no supervisor; later ones need one",
          "[lims][org][roles]") {
    DbFixture fixture;
    lims::OrgModel org;
    {
        ScopedPrincipal const alice{"alice"};
        CHECK_FALSE(lims::OrgModel::supervisorExists());
        auto const first = org.execute(lims::AssignRole{
            .principal = "alice", .role = lims::LimsRole::Supervisor, .company = lims::test::seededCompany(), .site = {}});
        CHECK(first.scopeKind == lims::ScopeKind::Company);
        CHECK(first.company == lims::test::seededCompany());
        CHECK(first.grantedBy == "alice");
        CHECK(lims::OrgModel::supervisorExists());
    }
    {
        ScopedPrincipal const mallory{"mallory"};
        CHECK_THROWS_AS(org.execute(lims::AssignRole{.principal = "mallory",
                                                     .role = lims::LimsRole::Supervisor,
                                                     .company = lims::test::seededCompany(),
                                                     .site = {}}),
                        lims::Forbidden);
    }
    {
        ScopedPrincipal const alice{"alice"};
        auto const bob = org.execute(lims::AssignRole{
            .principal = "bob", .role = lims::LimsRole::Verifier, .company = {}, .site = lims::test::seededSite()});
        auto const again = org.execute(lims::AssignRole{
            .principal = "bob", .role = lims::LimsRole::Verifier, .company = {}, .site = lims::test::seededSite()});
        CHECK(again.id == bob.id);
        CHECK(org.execute(lims::ListRoleAssignments{}).assignments.size() == 2);
    }
    CHECK_THROWS_AS(org.execute(lims::AssignRole{.principal = "bob",
                                                 .role = lims::LimsRole::Verifier,
                                                 .company = lims::test::seededCompany(),
                                                 .site = {}}),
                    lims::EmptyPrincipalError);
}

// Mutation: accept both a company and a site (the assignment is created).
TEST_CASE("org roles: an assignment names exactly one scope, and the scope exists", "[lims][org][roles]") {
    DbFixture fixture;
    ScopedPrincipal const alice{"alice"};
    lims::OrgModel org;
    CHECK_THROWS_AS(org.execute(lims::AssignRole{.principal = "bob",
                                                 .role = lims::LimsRole::Analyst,
                                                 .company = lims::test::seededCompany(),
                                                 .site = lims::test::seededSite()}),
                    lims::ValidationError);
    CHECK_THROWS_AS(
        org.execute(lims::AssignRole{.principal = "bob", .role = lims::LimsRole::Analyst, .company = {}, .site = {}}),
        lims::ValidationError);
    CHECK_THROWS_AS(org.execute(lims::AssignRole{
                        .principal = {}, .role = lims::LimsRole::Analyst, .company = lims::test::seededCompany(), .site = {}}),
                    lims::ValidationError);
    CHECK_THROWS_AS(org.execute(lims::AssignRole{
                        .principal = "bob", .role = lims::LimsRole::Analyst, .company = {}, .site = lims::SiteId{999}}),
                    lims::NotFound);
    CHECK_THROWS_AS(org.execute(lims::AssignRole{
                        .principal = "bob", .role = lims::LimsRole::Analyst, .company = lims::CompanyId{999}, .site = {}}),
                    lims::NotFound);
}

// Mutations: ignore company-scoped assignments for a site (bob's company role is missing at the field lab); let a
// site role cover its company (carol is a verifier at the company).
TEST_CASE("org roles: a company role holds at its sites, and a site role holds at that site only",
          "[lims][org][roles]") {
    DbFixture fixture;
    ScopedPrincipal const alice{"alice"};
    lims::OrgModel org;
    org.execute(lims::AssignRole{
        .principal = "alice", .role = lims::LimsRole::Supervisor, .company = lims::test::seededCompany(), .site = {}});
    auto const fieldLab = org.execute(lims::CreateSite{.companyId = lims::test::seededCompany(), .name = "Field lab"});
    auto const other = org.execute(lims::CreateCompany{
        .name = "Osaka branch", .baseCurrency = lims::CurrencyCode::JPY, .fiscalYearStart = lims::Month::April});

    org.execute(lims::AssignRole{
        .principal = "bob", .role = lims::LimsRole::Verifier, .company = lims::test::seededCompany(), .site = {}});
    org.execute(
        lims::AssignRole{.principal = "carol", .role = lims::LimsRole::Verifier, .company = {}, .site = fieldLab.id});

    CHECK(holds(lims::OrgModel::rolesIn("bob", lims::Scope{fieldLab.id}), lims::LimsRole::Verifier));
    CHECK(holds(lims::OrgModel::rolesIn("bob", lims::Scope{lims::test::seededSite()}), lims::LimsRole::Verifier));
    CHECK_FALSE(holds(lims::OrgModel::rolesIn("bob", lims::Scope{other.id}), lims::LimsRole::Verifier));

    CHECK(holds(lims::OrgModel::rolesIn("carol", lims::Scope{fieldLab.id}), lims::LimsRole::Verifier));
    CHECK_FALSE(holds(lims::OrgModel::rolesIn("carol", lims::Scope{lims::test::seededSite()}), lims::LimsRole::Verifier));
    CHECK_FALSE(holds(lims::OrgModel::rolesIn("carol", lims::Scope{lims::test::seededCompany()}), lims::LimsRole::Verifier));
    CHECK(holds(lims::OrgModel::rolesAnywhere("carol"), lims::LimsRole::Verifier));
}

TEST_CASE("org roles: roles are not hierarchical", "[lims][org][roles]") {
    DbFixture fixture;
    ScopedPrincipal const alice{"alice"};
    lims::OrgModel org;
    org.execute(lims::AssignRole{
        .principal = "alice", .role = lims::LimsRole::Supervisor, .company = lims::test::seededCompany(), .site = {}});
    // A supervisor is not implicitly a verifier: the second pair of eyes on somebody's own work is never granted
    // by implication.
    auto const held = lims::OrgModel::rolesIn("alice", lims::Scope{lims::test::seededSite()});
    REQUIRE(held.size() == 1);
    CHECK(held.front() == lims::LimsRole::Supervisor);
}

// Mutations: allow revoking the last supervisor (the bootstrap reopens); skip the supervisor check (bob revokes).
TEST_CASE("org roles: a supervisor revokes an assignment, but never the last supervisor's", "[lims][org][roles]") {
    DbFixture fixture;
    lims::OrgModel org;
    ScopedPrincipal const alice{"alice"};
    auto const boss = org.execute(lims::AssignRole{
        .principal = "alice", .role = lims::LimsRole::Supervisor, .company = lims::test::seededCompany(), .site = {}});
    auto const bob = org.execute(lims::AssignRole{
        .principal = "bob", .role = lims::LimsRole::Verifier, .company = lims::test::seededCompany(), .site = {}});
    {
        ScopedPrincipal const asBob{"bob"};
        CHECK_THROWS_AS(org.execute(lims::RevokeRole{.assignmentId = boss.id}), lims::Forbidden);
    }
    CHECK(org.execute(lims::RevokeRole{.assignmentId = bob.id}).assignmentId == bob.id);
    CHECK(lims::OrgModel::rolesAnywhere("bob").empty());
    CHECK_THROWS_AS(org.execute(lims::RevokeRole{.assignmentId = boss.id}), lims::Conflict);
    CHECK_THROWS_AS(org.execute(lims::RevokeRole{.assignmentId = lims::RoleAssignmentId{999}}), lims::NotFound);
    CHECK(lims::OrgModel::supervisorExists());
}

TEST_CASE("org roles: users are the principals holding an assignment, with the roles they hold", "[lims][org][roles]") {
    DbFixture fixture;
    ScopedPrincipal const alice{"alice"};
    lims::OrgModel org;
    org.execute(lims::AssignRole{
        .principal = "alice", .role = lims::LimsRole::Supervisor, .company = lims::test::seededCompany(), .site = {}});
    org.execute(lims::AssignRole{
        .principal = "bob", .role = lims::LimsRole::Analyst, .company = {}, .site = lims::test::seededSite()});
    org.execute(lims::AssignRole{
        .principal = "bob", .role = lims::LimsRole::Verifier, .company = lims::test::seededCompany(), .site = {}});
    auto const users = org.execute(lims::ListUsers{}).users;
    REQUIRE(users.size() == 2);
    CHECK(users[0].id == "alice");
    CHECK(users[1].id == "bob");
    CHECK(users[1].roles == std::vector<lims::LimsRole>{lims::LimsRole::Analyst, lims::LimsRole::Verifier});
}

// Mutation: copy no rows in the lab migration (bob holds nothing after it).
TEST_CASE("org roles: assignments made before roles were scoped migrate to the seeded company",
          "[lims][org][roles][migration]") {
    using lims_role_migration_fixtures::LegacyOperatorRecord;
    DbFixture fixture;
    lims::test::MigrationProbe probe{20261109000002};
    {
        Lightweight::DataMapper mapper;
        LegacyOperatorRecord bob;
        bob.principal = Lightweight::SqlAnsiString<64>{"bob"};
        bob.role = static_cast<int>(lims::LimsRole::Verifier);
        bob.grantedBy = Lightweight::SqlAnsiString<64>{"alice"};
        mapper.Create(bob);
    }
    probe.applyRest();
    CHECK(holds(lims::OrgModel::rolesIn("bob", lims::Scope{lims::test::seededCompany()}), lims::LimsRole::Verifier));
    CHECK(holds(lims::OrgModel::rolesIn("bob", lims::Scope{lims::test::seededSite()}), lims::LimsRole::Verifier));
    Lightweight::DataMapper mapper;
    CHECK_THROWS(mapper.Query<LegacyOperatorRecord>().All());
}
```

In `examples/lims/tests/lims_test_support.hpp`, add includes `#include <algorithm>`, `#include "lims/org/dto/role_dto.hpp"`, `#include "lims/org/models/org_model.hpp"`, and after `seededCompany()`:
```cpp
/// @brief Assigns @p role to @p principal at the seeded company.
///
/// Made by `lab-supervisor`, a supervisor the helper appoints itself while the lab has none. A test that appoints
/// its own supervisor first assigns through `OrgModel` as that supervisor instead.
/// @param principal Who gets the role.
/// @param role The role.
/// @return The assignment.
inline lims::RoleAssignmentView grant(std::string principal, lims::LimsRole role) {
    constexpr std::string_view kGranter = "lab-supervisor";
    ScopedPrincipal const granter{std::string{kGranter}};
    lims::OrgModel org;
    auto const held = lims::OrgModel::rolesIn(kGranter, lims::Scope{seededCompany()});
    if (std::ranges::find(held, lims::LimsRole::Supervisor) == held.end()) {
        org.execute(lims::AssignRole{
            .principal = std::string{kGranter}, .role = lims::LimsRole::Supervisor, .company = seededCompany(), .site = {}});
    }
    return org.execute(
        lims::AssignRole{.principal = std::move(principal), .role = role, .company = seededCompany(), .site = {}});
}
```

- [ ] **Step 2: Run the build to see it fail**

Run: `flock /tmp/claude-1000/-home-yaraslau-repo-morph/b1b73e49-c41d-4e27-bb68-b8c0cfacf145/scratchpad/morph-build.lock cmake --build build/linux-everything --target ladder_lims_tests -j10`
Expected: FAIL — `fatal error: 'lims/org/dto/role_dto.hpp' file not found`.

- [ ] **Step 3: Add the scope types and the role DTOs**

In `examples/lims/org/include/lims/org/core/types.hpp`, add `#include <string>` and `#include <variant>`; add `LIMS_DEFINE_STRONG_ID(RoleAssignmentId);` after `SiteId`, and inside `namespace lims` after `enum class Month`:
```cpp
/// @brief What a role assignment's scope is.
enum class ScopeKind : std::uint8_t {
    Company,  ///< A company, and every site of it.
    Site,     ///< One site.
};

/// @brief A scope a role is held in or an action is judged in: a company or a site.
using Scope = std::variant<CompanyId, SiteId>;

/// @brief Names @p scope for a message: `company 1`, `site 2`.
/// @param scope The scope.
/// @return Its name.
[[nodiscard]] inline std::string scopeName(Scope const& scope) {
    if (auto const* company = std::get_if<CompanyId>(&scope)) {
        return company->hasValue() ? "company " + std::to_string(**company) : std::string{"no company"};
    }
    auto const& site = std::get<SiteId>(scope);
    return site.hasValue() ? "site " + std::to_string(*site) : std::string{"no site"};
}
```
and after the existing wire/tag lines:
```cpp
LIMS_DEFINE_STRONG_ID_WIRE(RoleAssignmentId);
LIMS_DEFINE_STRONG_ID_SHAPE_TAG(RoleAssignmentId, "lims.roleAssignmentId");

/// @brief Reflects `ScopeKind` as its enumerator name.
template <>
struct glz::meta<lims::ScopeKind> {
    using enum lims::ScopeKind;

    /// @brief The enumerator ⇄ name mapping glaze reads and writes.
    static constexpr auto value = glz::enumerate(Company, Site);
};
```

Create `examples/lims/org/include/lims/org/dto/role_dto.hpp`. Move `enum class LimsRole` with its doc comment, `roleName(LimsRole)` and `template <> struct glz::meta<lims::LimsRole>` out of `lims/lab/dto/verification_dto.hpp` verbatim into it, and add the rest:
```cpp
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <array>
#include <cstdint>
#include <glaze/glaze.hpp>
#include <morph/core/records.hpp>
#include <morph/forms/forms.hpp>
#include <morph/util/datetime.hpp>
#include <string>
#include <string_view>
#include <vector>

#include "lims/org/core/types.hpp"

/// @file
/// Roles and role assignments: who may do what, and where.

namespace lims {

// enum class LimsRole, roleName(LimsRole) — moved here verbatim from lims/lab/dto/verification_dto.hpp.

/// @brief Assigns a role to a principal in one scope: a company, or one site.
///
/// Requires `Supervisor` in that scope, except on a lab with no supervisor anywhere, where the first assignment is
/// allowed so the lab can be set up at all; that exception closes as soon as any supervisor exists.
struct AssignRole {
    /// @brief Who gets the role.
    std::string principal;
    /// @brief The role.
    LimsRole role = LimsRole::Analyst;
    /// @brief The company scope; empty when the scope is a site.
    CompanyId company;
    /// @brief The site scope; empty when the scope is a company.
    SiteId site;

    /// @brief Neither scope is required on its own; the rule below requires exactly one.
    static constexpr std::array optionalFields{std::string_view{"company"}, std::string_view{"site"}};

    /// @brief The scope is a company or a site, never both and never neither.
    static constexpr auto formRules = ::morph::forms::ruleList(::morph::forms::exactlyOneOf(&AssignRole::company,
                                                                                            &AssignRole::site));

    /// @brief Whether the assignment is well-formed.
    /// @return `true` when a principal of at most 64 bytes and exactly one scope are named.
    [[nodiscard]] bool validate() const noexcept {
        return !principal.empty() && principal.size() <= 64 && ::morph::forms::allRulesSatisfied(*this);
    }

    /// @brief Sent by its button, never per keystroke.
    static constexpr bool explicitSubmit = true;
};

/// @brief One role assignment.
struct RoleAssignmentView {
    /// @brief Its id.
    RoleAssignmentId id;
    /// @brief Who holds the role.
    std::string principal;
    /// @brief The role.
    LimsRole role = LimsRole::Analyst;
    /// @brief Whether the scope is a company or a site.
    ScopeKind scopeKind = ScopeKind::Company;
    /// @brief The company, when the scope is one.
    CompanyId company;
    /// @brief The site, when the scope is one.
    SiteId site;
    /// @brief Who made the assignment.
    std::string grantedBy;
    /// @brief When.
    ::morph::time::Timestamp grantedAt;
};

/// @brief Revokes one role assignment. Requires `Supervisor` in its scope; the last supervisor's is never revoked.
struct RevokeRole {
    /// @brief The assignment.
    RoleAssignmentId assignmentId;

    /// @brief Whether an assignment is named.
    /// @return `true` when it is.
    [[nodiscard]] bool validate() const noexcept { return assignmentId.hasValue(); }
};

/// @brief Which assignment a revoke removed.
struct RevokeRoleResult {
    /// @brief The revoked assignment.
    RoleAssignmentId assignmentId;
};

/// @brief Lists the role assignments.
struct ListRoleAssignments {
    /// @brief Always ready.
    /// @return `true`.
    [[nodiscard]] bool validate() const noexcept { return true; }
};

/// @brief The role assignments, by id.
struct ListRoleAssignmentsResult {
    /// @brief The assignments.
    std::vector<RoleAssignmentView> assignments;
};

/// @brief One user: a principal holding at least one role assignment. lims keeps no user store of its own; a
///        principal is whoever signs in.
struct UserView {
    /// @brief The principal.
    std::string id;
    /// @brief Every role they hold somewhere, in the order first assigned.
    std::vector<LimsRole> roles;
};

/// @brief Lists the users.
struct ListUsers {
    /// @brief Always ready.
    /// @return `true`.
    [[nodiscard]] bool validate() const noexcept { return true; }
};

/// @brief The users, by principal.
struct ListUsersResult {
    /// @brief The users.
    std::vector<UserView> users;
};

}  // namespace lims

// template <> struct glz::meta<lims::LimsRole> — moved here verbatim from lims/lab/dto/verification_dto.hpp.

BRIDGE_REGISTER_RECORD(lims::RoleAssignmentView, "Org_RoleAssignment")
BRIDGE_REGISTER_RECORD(lims::UserView, "Org_User")
```
(The two `// … moved here verbatim` lines are where the moved code goes; they are not left in the file.)

In `examples/lims/lab/include/lims/lab/dto/verification_dto.hpp`: delete `enum class LimsRole`, `roleName`, `struct GrantRole`, `struct RoleGrantResult` and `template <> struct glz::meta<lims::LimsRole>`; delete the `RoleGranted,  ///< A principal was granted a role.` enumerator and `RoleGranted` from the `glz::meta<lims::AuditStepKind>` list; add `#include "lims/org/dto/role_dto.hpp"`.

- [ ] **Step 4: Add the assignment table, its migration and the lab migration**

Append to `examples/lims/org/include/lims/org/db/org_entity.hpp`, inside `namespace lims::db`:
```cpp
/// @brief One principal's role in one scope. The single source of truth for what a principal may do, read by the
///        server edge's authorizer and by every model's own check.
struct RoleAssignmentRecord {
    static constexpr std::string_view TableName = "org_role_assignments";
    Light::Field<std::uint64_t, Light::PrimaryKey::ServerSideAutoIncrement, Light::SqlRealName{"id"}> id;  // 0
    Light::Field<Light::SqlAnsiString<64>, Light::SqlRealName{"principal"}> principal;                     // 1
    /// @brief `LimsRole` as its underlying integer.
    Light::Field<int, Light::SqlRealName{"role"}> role{0};  // 2
    /// @brief `ScopeKind` as its underlying integer.
    Light::Field<int, Light::SqlRealName{"scope_kind"}> scopeKind{0};  // 3
    /// @brief The company's or the site's id, as `scope_kind` says.
    Light::Field<std::int64_t, Light::SqlRealName{"scope_id"}> scopeId{0};               // 4
    Light::Field<Light::SqlAnsiString<64>, Light::SqlRealName{"granted_by"}> grantedBy;  // 5
    Light::Field<std::int64_t, Light::SqlRealName{"granted_at"}> grantedAt{0};           // 6
};
```

Append to `examples/lims/org/src/db/org_schema.cpp` (inside the `NOLINTBEGIN`/`NOLINTEND` pair):
```cpp
LIGHTWEIGHT_SQL_MIGRATION(20261109000003, "Create org_role_assignments") {
    plan.CreateTableIfNotExists("org_role_assignments")
        .PrimaryKeyWithAutoIncrement("id", Bigint())
        .RequiredColumn("principal", Varchar(64))
        .RequiredColumn("role", Integer())
        .RequiredColumn("scope_kind", Integer())
        .RequiredColumn("scope_id", Bigint())
        .RequiredColumn("granted_by", Varchar(64))
        .RequiredColumn("granted_at", Bigint());
}
```

Append to `examples/lims/lab/src/db/schema.cpp` (inside its `NOLINTBEGIN`/`NOLINTEND` pair; add `#include "lims/org/core/types.hpp"`):
```cpp
LIGHTWEIGHT_SQL_MIGRATION(20261109000004, "Move lims_operators into org_role_assignments at the seeded company") {
    // A role granted before roles were scoped was granted for the whole lab, which is the seeded company. Copying
    // rows between tables is INSERT ... SELECT, which Lightweight's migration builder cannot express (its Insert takes
    // literal values only), so this statement is raw SQL, the escape tier rule 4 allows with a finding recorded.
    static_assert(static_cast<int>(lims::ScopeKind::Company) == 0 && lims::db::kSeededCompanyId == 1,
                  "the statement spells the company scope kind and the seeded company's id as literals");
    plan.RawSql("INSERT INTO org_role_assignments (principal, role, scope_kind, scope_id, granted_by, granted_at) "
                "SELECT principal, role, 0, 1, granted_by, granted_at FROM lims_operators");
    plan.DropTable("lims_operators");
}
```
The `20260823000009` migration that creates `lims_operators` stays: a database migrated from scratch creates the table and this migration then empties and drops it, exactly as an existing database goes through it. Delete `struct OperatorRecord` from `examples/lims/lab/include/lims/lab/db/lims_entity.hpp`.

Before committing, search Lightweight's tracker for an existing report and file one if there is none:
```
gh issue list -R LASTRADA-Software/Lightweight --state all --search "migration insert select"
```
Record the issue (found or filed) in the README entry of Step 9.

- [ ] **Step 5: Implement assignments in `OrgModel`**

In `examples/lims/org/include/lims/org/models/org_model.hpp`, add `#include <string_view>`, `#include <vector>`, `#include "lims/org/dto/role_dto.hpp"`, and to the class, after `execute(const ListSites&)`:
```cpp
    /// @brief Assigns a role to a principal in a company or a site. Assigning a role the principal already holds in
    ///        that scope changes nothing and answers the existing assignment.
    /// @param action The principal, the role and the scope.
    /// @return The assignment.
    /// @throws ValidationError if no principal, or not exactly one scope, is named.
    /// @throws NotFound if the scope does not exist.
    /// @throws Forbidden if a supervisor exists and the caller is not one in that scope.
    /// @throws EmptyPrincipalError if no principal is authenticated.
    RoleAssignmentView execute(const AssignRole& action);

    /// @brief Revokes one assignment.
    /// @param action The assignment.
    /// @return Which one went.
    /// @throws ValidationError if no assignment is named.
    /// @throws NotFound if it does not exist.
    /// @throws Forbidden if the caller is not a supervisor in its scope.
    /// @throws Conflict if it is the last supervisor's: without one, nobody could assign roles again.
    /// @throws EmptyPrincipalError if no principal is authenticated.
    RevokeRoleResult execute(const RevokeRole& action);

    /// @brief Lists the role assignments.
    /// @param action Carries nothing.
    /// @return Every assignment, by id.
    ListRoleAssignmentsResult execute(const ListRoleAssignments& action);

    /// @brief Lists the users: the principals holding an assignment.
    /// @param action Carries nothing.
    /// @return Every user, by principal.
    ListUsersResult execute(const ListUsers& action);

    /// @brief The roles @p principal holds in @p scope. In a site, that is the roles held at the site and at its
    ///        company; in a company, the roles held at the company.
    /// @param principal The principal.
    /// @param scope The scope.
    /// @return The roles, each once, in the order first assigned; none for an empty or unknown scope.
    [[nodiscard]] static std::vector<LimsRole> rolesIn(std::string_view principal, Scope const& scope);

    /// @brief The roles @p principal holds in any scope: all the server edge can ask, since it never sees a payload.
    /// @param principal The principal.
    /// @return The roles, each once, in the order first assigned.
    [[nodiscard]] static std::vector<LimsRole> rolesAnywhere(std::string_view principal);

    /// @brief Whether any principal holds `Supervisor` anywhere.
    /// @return Whether one does.
    [[nodiscard]] static bool supervisorExists();
```
and the registrations, inside the `NOLINTBEGIN`/`NOLINTEND` pair:
```cpp
BRIDGE_REGISTER_ACTION(lims::OrgModel, lims::AssignRole, "AssignRole")
BRIDGE_REGISTER_EFFECTS(lims::AssignRole, ::morph::effects::changes<lims::RoleAssignmentView>,
                        ::morph::effects::changes<lims::UserView>)
BRIDGE_REGISTER_ACTION(lims::OrgModel, lims::RevokeRole, "RevokeRole")
BRIDGE_REGISTER_EFFECTS(lims::RevokeRole, ::morph::effects::changes<lims::RoleAssignmentView>,
                        ::morph::effects::changes<lims::UserView>)
BRIDGE_REGISTER_ACTION(lims::OrgModel, lims::ListRoleAssignments, "ListRoleAssignments", ::morph::model::Loggable::No)
BRIDGE_REGISTER_ACTION(lims::OrgModel, lims::ListUsers, "ListUsers", ::morph::model::Loggable::No)
```

In `examples/lims/org/src/models/org_model.cpp`, add `#include <Lightweight/SqlTransaction.hpp>`, `#include <map>`, `#include <optional>`, `#include <utility>`, `#include <variant>`, and inside the anonymous namespace:
```cpp
/// @brief Maps an assignment row to its view.
/// @param row The row.
/// @return The view.
[[nodiscard]] RoleAssignmentView toView(db::RoleAssignmentRecord const& row) {
    auto const kind = static_cast<ScopeKind>(row.scopeKind.Value());
    return RoleAssignmentView{
        .id = RoleAssignmentId{static_cast<std::int64_t>(row.id.Value())},
        .principal = std::string{row.principal.Value().ToStringView()},
        .role = static_cast<LimsRole>(row.role.Value()),
        .scopeKind = kind,
        .company = kind == ScopeKind::Company ? CompanyId{row.scopeId.Value()} : CompanyId{},
        .site = kind == ScopeKind::Site ? SiteId{row.scopeId.Value()} : SiteId{},
        .grantedBy = std::string{row.grantedBy.Value().ToStringView()},
        .grantedAt = timestampFromMillis(row.grantedAt.Value()),
    };
}

/// @brief The scope an assignment action names.
/// @param action A validated assignment.
/// @return Its company or its site.
[[nodiscard]] Scope scopeOf(AssignRole const& action) {
    return action.company.hasValue() ? Scope{action.company} : Scope{action.site};
}

/// @brief The kind and id an assignment row stores for @p scope.
/// @param scope A scope holding a value.
/// @return The kind and the id.
[[nodiscard]] std::pair<ScopeKind, std::int64_t> storedScope(Scope const& scope) {
    if (auto const* company = std::get_if<CompanyId>(&scope)) {
        return {ScopeKind::Company, **company};
    }
    return {ScopeKind::Site, *std::get<SiteId>(scope)};
}

/// @brief Whether @p scope names a company or a site that exists.
/// @param mapper The open mapper.
/// @param scope The scope.
/// @return Whether it does.
[[nodiscard]] bool scopeExists(Lightweight::DataMapper& mapper, Scope const& scope) {
    if (auto const* company = std::get_if<CompanyId>(&scope)) {
        return company->hasValue() && !mapper.Query<db::CompanyRecord>()
                                           .Where(::Lightweight::FieldNameOf<&db::CompanyRecord::id>, "=", **company)
                                           .All()
                                           .empty();
    }
    return OrgModel::findSite(std::get<SiteId>(scope)).has_value();
}

/// @brief Throws `Forbidden` unless the authenticated principal is a supervisor in @p scope.
/// @param scope The scope.
void requireSupervisorIn(Scope const& scope) {
    auto const& principal = ::morph::session::current()->principal;
    auto const held = OrgModel::rolesIn(principal, scope);
    if (std::ranges::find(held, LimsRole::Supervisor) == held.end()) {
        throw Forbidden{"'" + principal + "' does not hold the supervisor role at " + scopeName(scope)};
    }
}

/// @brief Every assignment of @p principal, by id.
/// @param mapper The open mapper.
/// @param principal The principal.
/// @return The rows.
[[nodiscard]] std::vector<db::RoleAssignmentRecord> assignmentsOf(Lightweight::DataMapper& mapper,
                                                                  std::string_view principal) {
    auto rows = mapper.Query<db::RoleAssignmentRecord>()
                    .Where(::Lightweight::FieldNameOf<&db::RoleAssignmentRecord::principal>, "=", std::string{principal})
                    .All();
    std::ranges::sort(rows, {}, [](db::RoleAssignmentRecord const& row) { return row.id.Value(); });
    return rows;
}

/// @brief Appends @p role to @p roles unless it is there already.
/// @param roles The roles so far.
/// @param role The role.
void addOnce(std::vector<LimsRole>& roles, LimsRole role) {
    if (std::ranges::find(roles, role) == roles.end()) {
        roles.push_back(role);
    }
}
```
and the members:
```cpp
RoleAssignmentView OrgModel::execute(const AssignRole& action) {
    requirePrincipal();
    if (!action.validate()) {
        throw ValidationError{"AssignRole: a principal and exactly one of a company or a site are required"};
    }
    Scope const scope = scopeOf(action);
    Lightweight::DataMapper mapper;
    if (!scopeExists(mapper, scope)) {
        throw NotFound{"AssignRole: " + scopeName(scope) + " does not exist"};
    }
    // A lab with no supervisor anywhere cannot appoint its first one any other way. The exception closes as soon as
    // one exists, and the assignment names who made it.
    if (supervisorExists()) {
        requireSupervisorIn(scope);
    }
    auto const [kind, id] = storedScope(scope);
    for (auto const& row : assignmentsOf(mapper, action.principal)) {
        if (row.role.Value() == static_cast<int>(action.role) && row.scopeKind.Value() == static_cast<int>(kind) &&
            row.scopeId.Value() == id) {
            return toView(row);
        }
    }
    db::RoleAssignmentRecord row;
    row.principal = Lightweight::SqlAnsiString<64>{action.principal};
    row.role = static_cast<int>(action.role);
    row.scopeKind = static_cast<int>(kind);
    row.scopeId = id;
    row.grantedBy = Lightweight::SqlAnsiString<64>{::morph::session::current()->principal};
    row.grantedAt = nowMillis();
    mapper.Create(row);
    return toView(row);
}

RevokeRoleResult OrgModel::execute(const RevokeRole& action) {
    requirePrincipal();
    if (!action.validate()) {
        throw ValidationError{"RevokeRole: an assignment is required"};
    }
    Lightweight::DataMapper mapper;
    auto rows = mapper.Query<db::RoleAssignmentRecord>()
                    .Where(::Lightweight::FieldNameOf<&db::RoleAssignmentRecord::id>, "=", *action.assignmentId)
                    .All();
    if (rows.empty()) {
        throw NotFound{"RevokeRole: assignment " + std::to_string(*action.assignmentId) + " does not exist"};
    }
    auto const view = toView(rows.front());
    requireSupervisorIn(view.scopeKind == ScopeKind::Company ? Scope{view.company} : Scope{view.site});
    if (view.role == LimsRole::Supervisor &&
        mapper.Query<db::RoleAssignmentRecord>()
                .Where(::Lightweight::FieldNameOf<&db::RoleAssignmentRecord::role>, "=",
                       static_cast<int>(LimsRole::Supervisor))
                .All()
                .size() == 1) {
        throw Conflict{"RevokeRole: the last supervisor's assignment cannot be revoked"};
    }
    mapper.Delete(rows.front());
    return RevokeRoleResult{.assignmentId = action.assignmentId};
}

// An action handler: the registry calls it on the instance, like every other.
// NOLINTNEXTLINE(readability-convert-member-functions-to-static)
ListRoleAssignmentsResult OrgModel::execute(const ListRoleAssignments& action) {
    static_cast<void>(action);
    Lightweight::DataMapper mapper;
    ListRoleAssignmentsResult result;
    for (auto const& row : mapper.Query<db::RoleAssignmentRecord>().All()) {
        result.assignments.push_back(toView(row));
    }
    std::ranges::sort(result.assignments, {}, [](RoleAssignmentView const& view) { return *view.id; });
    return result;
}

// An action handler: the registry calls it on the instance, like every other.
// NOLINTNEXTLINE(readability-convert-member-functions-to-static)
ListUsersResult OrgModel::execute(const ListUsers& action) {
    static_cast<void>(action);
    auto const assignments = execute(ListRoleAssignments{}).assignments;
    std::map<std::string, std::vector<LimsRole>> byPrincipal;
    for (auto const& assignment : assignments) {
        addOnce(byPrincipal[assignment.principal], assignment.role);
    }
    ListUsersResult result;
    for (auto& [principal, roles] : byPrincipal) {
        result.users.push_back(UserView{.id = principal, .roles = std::move(roles)});
    }
    return result;
}

std::vector<LimsRole> OrgModel::rolesIn(std::string_view principal, Scope const& scope) {
    std::optional<std::int64_t> company;
    std::optional<std::int64_t> site;
    if (auto const* asCompany = std::get_if<CompanyId>(&scope)) {
        company = asCompany->value;
    } else if (auto const found = findSite(std::get<SiteId>(scope))) {
        site = *found->id;
        company = *found->companyId;
    }
    Lightweight::DataMapper mapper;
    std::vector<LimsRole> roles;
    for (auto const& row : assignmentsOf(mapper, principal)) {
        auto const kind = static_cast<ScopeKind>(row.scopeKind.Value());
        bool const covers = (kind == ScopeKind::Company && company == row.scopeId.Value()) ||
                            (kind == ScopeKind::Site && site == row.scopeId.Value());
        if (covers) {
            addOnce(roles, static_cast<LimsRole>(row.role.Value()));
        }
    }
    return roles;
}

std::vector<LimsRole> OrgModel::rolesAnywhere(std::string_view principal) {
    Lightweight::DataMapper mapper;
    std::vector<LimsRole> roles;
    for (auto const& row : assignmentsOf(mapper, principal)) {
        addOnce(roles, static_cast<LimsRole>(row.role.Value()));
    }
    return roles;
}

bool OrgModel::supervisorExists() {
    Lightweight::DataMapper mapper;
    return !mapper.Query<db::RoleAssignmentRecord>()
                .Where(::Lightweight::FieldNameOf<&db::RoleAssignmentRecord::role>, "=",
                       static_cast<int>(LimsRole::Supervisor))
                .All()
                .empty();
}
```
(The `ListUsers` handler calls `execute(ListRoleAssignments{})` on itself, which keeps one place that reads and orders assignments; Task 5 filters that one place.)

- [ ] **Step 6: Take role grants out of lab and point every reader at org**

In `examples/lims/lab/include/lims/lab/models/sample_model.hpp`: delete the `rolesOf` declaration and its doc comment, the `execute(const GrantRole&)` declaration and its doc comment, and the line `BRIDGE_REGISTER_ACTION(lims::SampleModel, lims::GrantRole, "GrantRole")`.

In `examples/lims/lab/src/models/sample_model.cpp`: add `#include "lims/org/models/org_model.hpp"`; delete `SampleModel::rolesOf` and `SampleModel::execute(const GrantRole&)`; in `auditStepFor` delete the two `else if (entry.actionType == "GrantRole") { … }` branches (the refused one and the decoded one); replace the body of `requireRole` with:
```cpp
void SampleModel::requireRole(LimsRole role) {
    const auto& principal = ::morph::session::current()->principal;
    const auto held = OrgModel::rolesAnywhere(principal);
    if (std::ranges::find(held, role) == held.end()) {
        throw Forbidden{"'" + principal + "' does not hold the " + std::string{roleName(role)} + " role"};
    }
}
```
A journal entry under the action id `GrantRole` that an older build recorded against a sample now reconstructs as an `Unreadable` step, which is the trail's rule for any entry this build cannot interpret (README decision 17).

In `examples/lims/app/include/lims/app/lims_authorizer.hpp`: replace `#include "lims/lab/dto/verification_dto.hpp"` with `#include "lims/org/dto/role_dto.hpp"`; make `requiresSupervisorRole` read
```cpp
[[nodiscard]] inline bool requiresSupervisorRole(std::string_view actionType) noexcept {
    return actionType == "AssignRole" || actionType == "RevokeRole";
}
```
and in its comments replace `` `lims_operators` table — the same table `SampleModel` reads`` with `` `org_role_assignments` table, through `OrgModel::rolesAnywhere` — the same table every model's own check reads`` and `` `SampleModel::execute(GrantRole)` `` with `` `OrgModel::execute(AssignRole)` ``, and `` `GrantRole` `` with `` `AssignRole` ``.

```bash
cd /home/yaraslau/repo/morph
perl -pi -e 's/lims::SampleModel::rolesOf\(/lims::OrgModel::rolesAnywhere(/g' \
    examples/lims/src/server/main.cpp examples/lims/tests/*.cpp examples/lims/screens/tests/*.cpp
grep -rn "rolesOf" examples/lims
```
Expected: no output. Add `#include "lims/org/models/org_model.hpp"` to `examples/lims/src/server/main.cpp`, `examples/lims/tests/test_backend_matrix.cpp`, `examples/lims/screens/tests/test_lims_screens.cpp` and `examples/lims/tests/test_verification_audit.cpp`.

- [ ] **Step 7: Update the lab suites**

In `examples/lims/tests/test_verification_audit.cpp`:
1. Delete the two test cases "The first grant bootstraps a lab with no supervisor -- later ones need one" and "Roles are not hierarchical" (both now in `test_org_roles.cpp`).
2. In "Every refused action kind is named in the trail, not lumped as unreadable", delete the block from the comment `// A supervisor has to exist before a grant can be refused at all` through the closing `}` of the `mallory` scope, and the line `CHECK(has(lims::AuditStepKind::RoleGranted));`.
3. In "Every state the sample was ever in is reconstructible from the journal alone", replace `bool sawGrant = false;` with `bool sawVerification = false;`, the `if (step.kind == lims::AuditStepKind::RoleGranted) { sawGrant = true; }` block with
```cpp
        if (step.kind == lims::AuditStepKind::ResultVerified) {
            sawVerification = true;
            CHECK(step.principal == "bob");
        }
```
and `CHECK(sawGrant);` with `CHECK(sawVerification);`.
4. In "The authorizer gates the role-requiring actions and nothing else" and "An authorizer with no role source fails closed", replace every `"SampleModel", "GrantRole"` with `"OrgModel", "AssignRole"`.

Then replace every remaining grant with the helper:
```bash
cd /home/yaraslau/repo/morph
perl -pi -e 's/\b[\w.]+\.execute\(lims::GrantRole\{\.principal = ("[^"]+"), \.role = (lims::LimsRole::\w+)\}\);/lims::test::grant($1, $2);/g' \
    examples/lims/tests/*.cpp examples/lims/screens/tests/*.cpp
grep -rn "GrantRole\|RoleGranted" examples/lims
```
Expected: no output.

- [ ] **Step 8: Build and run**

Run:
```
flock /tmp/claude-1000/-home-yaraslau-repo-morph/b1b73e49-c41d-4e27-bb68-b8c0cfacf145/scratchpad/morph-build.lock cmake --build build/linux-everything --target ladder_lims_tests ladder_lims_screen_tests ladder_lims_render_smoke ladder_lims_server morph_examples_client -j10
flock /tmp/claude-1000/-home-yaraslau-repo-morph/b1b73e49-c41d-4e27-bb68-b8c0cfacf145/scratchpad/morph-build.lock timeout 300 build/linux-everything/examples/lims/ladder_lims_tests "[roles]"
flock /tmp/claude-1000/-home-yaraslau-repo-morph/b1b73e49-c41d-4e27-bb68-b8c0cfacf145/scratchpad/morph-build.lock ctest --test-dir build/linux-everything -j6 -L ladder-lims --output-on-failure
```
Expected: `[roles]` passes (the seven org-role cases plus the verification suite's role cases); ctest `100% tests passed`. Then apply each `// Mutation:` in `test_org_roles.cpp` in turn, rebuild, and confirm the named case fails; revert each.

- [ ] **Step 9: Record the decision and commit**

Append to `examples/lims/README.md`'s resolved decisions:
```markdown
### 23. A role is held in a scope: a company or a site

A role assignment is `(principal, role, scope)`, kept by `lims/org` in `org_role_assignments`, and made with
`OrgModel`'s `AssignRole` and `RevokeRole`. A role held at a company holds at every site of the company; a role held
at a site holds there only. Roles themselves stay non-hierarchical (decision 16): the scope is the only thing that
nests. Grants made before scopes existed were for the whole lab, so the migration moves them to the seeded company.

The bootstrap carve-out moves with the action: the first assignment on a lab with no supervisor anywhere is allowed,
and the carve-out closes as soon as any supervisor exists. Revoking the last supervisor's assignment is refused,
because it would reopen that carve-out to whoever acted next.

Assignments are not a sample's history, so a sample's audit trail no longer shows them; they are journaled by the
framework's dispatcher like any other action of `OrgModel`. A `GrantRole` entry an older build wrote into a sample's
journal reconstructs as an `Unreadable` step, as any entry this build cannot interpret does (decision 17).
```
and to "Findings raised by this rung":
```markdown
- **A data migration that copies rows needs raw SQL.** Lightweight's migration builder inserts literal rows only, so
  moving `lims_operators` into `org_role_assignments` is one `RawSql` `INSERT ... SELECT`
  (`lab/src/db/schema.cpp`, migration `20261109000004`), under IMPLEMENTATION.md rule 4's escape tier. Upstream:
  <the Lightweight issue found or filed in Step 4>.
```
Then:
```bash
cd /home/yaraslau/repo/morph
git add -A examples/lims ':!examples/lims/design'
git commit -m "Scope lims role assignments to a company or a site

Role assignments move from lab's lims_operators to org_role_assignments as
(principal, role, scope); existing grants migrate to the seeded company.
OrgModel's AssignRole and RevokeRole replace SampleModel's GrantRole.

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01No1NznqxEBvjhZBaBRLCd6"
```

---
### Task 4: Declared authorization, with capture/verify as the first separation-of-duties pair (hypothesis 6, instance 1)

**Files:**
- Create: `examples/lims/org/include/lims/org/authorization/declared.hpp`
- Create: `examples/lims/org/src/authorization/declared.cpp`
- Create: `examples/lims/org/include/lims/org/authorization/org_authorization.hpp`
- Create: `examples/lims/org/src/authorization/org_authorization.cpp`
- Create: `examples/lims/lab/include/lims/lab/authorization/lab_authorization.hpp`
- Create: `examples/lims/lab/src/authorization/lab_authorization.cpp`
- Create: `examples/lims/app/include/lims/app/declared_requirements.hpp`
- Modify: `examples/lims/app/include/lims/app/lims_authorizer.hpp` (role lookup from the declared table)
- Modify: `examples/lims/org/src/models/org_model.cpp` (`CreateCompany`, `CreateSite`, `AssignRole`, `RevokeRole` check their declarations)
- Modify: `examples/lims/lab/include/lims/lab/models/sample_model.hpp`, `examples/lims/lab/src/models/sample_model.cpp` (`VerifyResult` checks its declaration; `requireRole` goes)
- Create: `examples/lims/tests/test_declared_authorization.cpp`
- Modify: `examples/lims/tests/lims_test_support.hpp` (`grant` takes a scope), `examples/lims/tests/test_org_companies.cpp`, `examples/lims/tests/test_sample_sites.cpp`

**Interfaces:**
- Consumes (Task 3): `lims::LimsRole`, `lims::roleName`, `lims::Scope`, `lims::scopeName`, `OrgModel::rolesIn`, `OrgModel::rolesAnywhere`, `OrgModel::supervisorExists`, `AssignRole`, `RevokeRole`; (Task 2) `CreateCompany`, `CreateSite`, `SampleRecord::siteId`; (lab) `VerifyResult`, `db::ResultRecord::capturedBy`.
- Produces:
  - `template <typename Action> struct lims::Authorization;` — specialised beside an action by the module that owns it, with `static constexpr LimsRole role`, optionally `static Scope scopeOf(Action const&)` (the scope its payload names), optionally `amountOf`/`limitOf` (a value limit), optionally `separatedFrom`/`sameActorRefusal`/`firstPerformerOf` (the second half of a separation-of-duties pair).
  - Concepts `lims::DeclaresRule<A>`, `DeclaresScope<A>`, `DeclaresValueLimit<A>`, `DeclaresSeparation<A>`.
  - `enum class lims::Verdict { Allowed, MissingRole, SameActor, NoLimit, OverLimit }`; `struct lims::AuthorizationFacts`; `lims::Verdict lims::authorizationVerdict(AuthorizationFacts const&)` (pure).
  - `template <typename A> void lims::requireAuthorized(A const& action)` — throws `lims::Forbidden`.
  - `struct lims::RoleRequirement { std::string_view actionType; LimsRole role; }`; `lims::requirementOf<A>()`.
  - `lims::app::declaredRequirements()` → `std::array<RoleRequirement, 5> const&`; `lims::app::requiredRole(std::string_view actionType)` → `std::optional<LimsRole>`.
  - Declarations: `VerifyResult` (Verifier, scope = its result's sample's site, separated from `CaptureConcentration`); `CreateCompany` (Supervisor, any scope); `CreateSite` (Supervisor at its company); `AssignRole` (Supervisor at its scope, after the bootstrap carve-out); `RevokeRole` (Supervisor at the assignment's scope).
  - `lims::test::grant(std::string principal, LimsRole role, Scope scope = Scope{seededCompany()})`.

- [ ] **Step 1: Write the failing tests**

Create `examples/lims/tests/test_declared_authorization.cpp`:
```cpp
// SPDX-License-Identifier: Apache-2.0
//
// Declared authorization (lims/org) and its first separation-of-duties pair, lab's capture and verify.
//
// This file is also the record of hypothesis 6's first instance (docs/superpowers/specs/2026-11-09-lims-erp-design.md,
// "Expected strain points"): `IAuthorizer::authorize` is handed the model and action ids and never the payload, so
// the server edge can check that a principal holds a role somewhere, and only the model, which has the payload, can
// check the scope and the pair. The "Hypothesis 6" cases below measure exactly that split: a verifier whose role is
// at another site passes the edge and is refused by the model, on every backend.

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <cstdint>
#include <memory>
#include <morph/core/backend.hpp>
#include <morph/session/session.hpp>
#include <morph/session/session_auth.hpp>
#include <morph/util/rational.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "lims/app/declared_requirements.hpp"
#include "lims/app/lims_authorizer.hpp"
#include "lims/lab/authorization/lab_authorization.hpp"
#include "lims/lab/models/analysis_catalog_model.hpp"
#include "lims/lab/models/sample_model.hpp"
#include "lims/org/authorization/declared.hpp"
#include "lims/org/core/errors.hpp"
#include "lims/org/models/org_model.hpp"
#include "lims_test_support.hpp"
#include "testkit/backend_rig.hpp"
#include "testkit/db_fixture.hpp"
#include "testkit/pump.hpp"

using lims::test::ScopedPrincipal;
using morph::ladder::testkit::awaitQt;
using morph::ladder::testkit::BackendRig;
using morph::ladder::testkit::DbFixture;
using morph::ladder::testkit::Mode;

namespace {

constexpr std::string_view kSecret = "lims-declared-authorization-test-secret-32b";

/// @brief Exact money-like amounts for the value-limit cases.
/// @param units Whole units.
/// @return The amount at two decimal places.
[[nodiscard]] morph::math::Rational amount(std::int64_t units) {
    return morph::math::Rational{units, morph::math::DecimalPlaces{2}};
}

/// @brief Facts for a principal holding @p held, asked for `Verifier`.
/// @param held The roles held in the scope.
/// @return The facts.
[[nodiscard]] lims::AuthorizationFacts verifierFacts(std::vector<lims::LimsRole> held) {
    return lims::AuthorizationFacts{.principal = "bob",
                                    .required = lims::LimsRole::Verifier,
                                    .held = std::move(held),
                                    .firstPerformer = std::nullopt,
                                    .amount = std::nullopt,
                                    .limit = std::nullopt};
}

/// @brief A signed session for @p principal.
/// @param issuer The issuer.
/// @param principal The principal.
/// @return The context, with the token and the principal set.
[[nodiscard]] morph::session::Context signedAs(morph::session::TokenIssuer const& issuer, std::string principal) {
    morph::session::Context ctx;
    ctx.principal = principal;
    ctx.token = issuer.issue(morph::session::SessionToken{
        .principal = std::move(principal), .issuedAtMs = 0, .expiresAtMs = 4102444800000, .roles = {}});
    return ctx;
}

/// @brief One sample at the seeded site with one reading captured by alice, awaiting verification, and a second
///        site in the same company.
struct TwoSites {
    lims::SiteId fieldLab;
    lims::SampleId sample;
    lims::ResultId result;
};

/// @brief Builds `TwoSites` through the models directly, as alice.
/// @return The ids.
[[nodiscard]] TwoSites twoSites() {
    lims::test::grant("alice", lims::LimsRole::Supervisor);
    ScopedPrincipal const alice{"alice"};
    lims::OrgModel org;
    auto const fieldLab = org.execute(lims::CreateSite{.companyId = lims::test::seededCompany(), .name = "Field lab"});
    lims::AnalysisCatalogModel catalog;
    auto const nitrate =
        catalog.execute(lims::DefineAnalysis{.name = "Nitrate", .canonicalUnit = "mg_per_L", .decimalPlaces = 3});
    lims::SampleModel bench;
    auto const client = bench.execute(lims::RegisterClient{.name = "Waterworks Ltd"});
    auto const sample = bench.execute(
        lims::RegisterSample{.siteId = lims::test::seededSite(), .clientId = client.clientId, .reference = "WW-1"});
    bench.execute(lims::ReceiveSample{});
    bench.execute(lims::StartWork{});
    auto const result = bench.execute(lims::CaptureConcentration{
        .analysisVersionId = nitrate.versionId,
        .value = lims::Concentration{morph::math::Rational{morph::math::Numerator{12}, morph::math::Denominator{5},
                                                           morph::math::DecimalPlaces{3}}}});
    bench.execute(lims::SubmitForVerification{});
    return TwoSites{.fieldLab = fieldLab.id, .sample = sample.id, .result = result.id};
}

}  // namespace

// ── The verdict, with no database ──────────────────────────────────────────

// Mutation: treat any held role as enough (an analyst is allowed to verify).
TEST_CASE("declared authorization: the role is required in the scope", "[lims][authorization]") {
    CHECK(lims::authorizationVerdict(verifierFacts({})) == lims::Verdict::MissingRole);
    CHECK(lims::authorizationVerdict(verifierFacts({lims::LimsRole::Analyst})) == lims::Verdict::MissingRole);
    CHECK(lims::authorizationVerdict(verifierFacts({lims::LimsRole::Analyst, lims::LimsRole::Verifier})) ==
          lims::Verdict::Allowed);
}

// Mutation: compare the first performer case-insensitively or not at all (bob verifies his own capture).
TEST_CASE("declared authorization: one principal may not perform both halves of a pair", "[lims][authorization]") {
    auto facts = verifierFacts({lims::LimsRole::Verifier});
    facts.firstPerformer = "bob";
    CHECK(lims::authorizationVerdict(facts) == lims::Verdict::SameActor);
    facts.firstPerformer = "alice";
    CHECK(lims::authorizationVerdict(facts) == lims::Verdict::Allowed);
    // Without the role the pair is not even reached: the role is the first question.
    facts = verifierFacts({});
    facts.firstPerformer = "bob";
    CHECK(lims::authorizationVerdict(facts) == lims::Verdict::MissingRole);
}

// Mutations: allow an amount when no limit is set (NoLimit reads Allowed); compare with >= (the amount at the
// limit is refused).
TEST_CASE("declared authorization: a value limit caps the amount, and no limit allows nothing", "[lims][authorization]") {
    auto facts = verifierFacts({lims::LimsRole::Verifier});
    facts.amount = amount(500);
    CHECK(lims::authorizationVerdict(facts) == lims::Verdict::NoLimit);
    facts.limit = amount(500);
    CHECK(lims::authorizationVerdict(facts) == lims::Verdict::Allowed);
    facts.limit = amount(499);
    CHECK(lims::authorizationVerdict(facts) == lims::Verdict::OverLimit);
}

TEST_CASE("declared authorization: lab declares capture and verify as a pair", "[lims][authorization]") {
    using Rule = lims::Authorization<lims::VerifyResult>;
    STATIC_REQUIRE(lims::DeclaresScope<lims::VerifyResult>);
    STATIC_REQUIRE(lims::DeclaresSeparation<lims::VerifyResult>);
    STATIC_REQUIRE_FALSE(lims::DeclaresValueLimit<lims::VerifyResult>);
    STATIC_REQUIRE_FALSE(lims::DeclaresRule<lims::CaptureConcentration>);
    CHECK(Rule::role == lims::LimsRole::Verifier);
    CHECK(Rule::separatedFrom == "CaptureConcentration");
}

TEST_CASE("declared authorization: the edge's table lists every declared role, and nothing else",
          "[lims][authorization]") {
    CHECK(lims::app::requiredRole("VerifyResult") == lims::LimsRole::Verifier);
    CHECK(lims::app::requiredRole("CreateCompany") == lims::LimsRole::Supervisor);
    CHECK(lims::app::requiredRole("CreateSite") == lims::LimsRole::Supervisor);
    CHECK(lims::app::requiredRole("AssignRole") == lims::LimsRole::Supervisor);
    CHECK(lims::app::requiredRole("RevokeRole") == lims::LimsRole::Supervisor);
    CHECK_FALSE(lims::app::requiredRole("CaptureConcentration").has_value());
    CHECK_FALSE(lims::app::requiredRole("GetSample").has_value());
}

// ── Through the models ─────────────────────────────────────────────────────

// Mutation: check the role anywhere instead of at the sample's site (bob, a verifier at the field lab, verifies).
TEST_CASE("declared authorization: verifying needs the verifier role at the sample's site", "[lims][authorization]") {
    DbFixture fixture;
    auto const lab = twoSites();
    lims::test::grant("bob", lims::LimsRole::Verifier, lims::Scope{lab.fieldLab});
    lims::test::grant("carol", lims::LimsRole::Verifier);
    {
        ScopedPrincipal const bob{"bob"};
        lims::SampleModel verifier;
        CHECK_THROWS_AS(verifier.execute(lims::VerifyResult{.resultId = lab.result}), lims::Forbidden);
    }
    {
        ScopedPrincipal const carol{"carol"};
        lims::SampleModel verifier;
        CHECK(verifier.execute(lims::VerifyResult{.resultId = lab.result}).verifiedBy == "carol");
    }
}

// Mutations: let a site-scoped supervisor create sites in the company (bob creates one); require CreateCompany's
// role at the seeded company (bob, a supervisor only at a site, is refused).
TEST_CASE("declared authorization: a site needs a supervisor at its company, a company a supervisor anywhere",
          "[lims][authorization]") {
    DbFixture fixture;
    lims::test::grant("bob", lims::LimsRole::Supervisor, lims::Scope{lims::test::seededSite()});
    ScopedPrincipal const bob{"bob"};
    lims::OrgModel org;
    CHECK_THROWS_AS(org.execute(lims::CreateSite{.companyId = lims::test::seededCompany(), .name = "Field lab"}),
                    lims::Forbidden);
    // A new company has no scope of its own yet, so CreateCompany declares its role in any scope.
    CHECK_NOTHROW(org.execute(lims::CreateCompany{
        .name = "Osaka branch", .baseCurrency = lims::CurrencyCode::JPY, .fiscalYearStart = lims::Month::April}));
    ScopedPrincipal const mallory{"mallory"};
    CHECK_THROWS_AS(org.execute(lims::CreateCompany{
                        .name = "Mallory Ltd", .baseCurrency = lims::CurrencyCode::EUR, .fiscalYearStart = lims::Month::January}),
                    lims::Forbidden);
}

// ── Hypothesis 6 ───────────────────────────────────────────────────────────

// The edge decides on (principal, model, action) and is right to allow bob: he does hold the verifier role. Only
// the model, which can read the result's sample and its site, can tell that he holds it somewhere else.
TEST_CASE("Hypothesis 6: the server edge allows a verifier whose role is at another site", "[lims][authorization][h6]") {
    DbFixture fixture;
    auto const lab = twoSites();
    lims::test::grant("bob", lims::LimsRole::Verifier, lims::Scope{lab.fieldLab});
    morph::session::TokenIssuer const issuer{std::string{kSecret}, morph::session::hmacSha256};
    lims::auth::LimsAuthorizer const authorizer{
        std::string{kSecret}, [](std::string_view principal) { return lims::OrgModel::rolesAnywhere(principal); }};
    CHECK(authorizer.authorize(signedAs(issuer, "bob"), "SampleModel", "VerifyResult"));
    CHECK_FALSE(authorizer.authorize(signedAs(issuer, "mallory"), "SampleModel", "VerifyResult"));
}

// Mutation: drop requireAuthorized from VerifyResult (bob's verification is recorded in every mode).
TEST_CASE("Hypothesis 6: on every backend, the model refuses what the edge let through", "[lims][authorization][h6]") {
    auto const mode = GENERATE(Mode::Local, Mode::LocalSingleThread, Mode::Socket);
    CAPTURE(mode);
    DbFixture fixture;
    auto const lab = twoSites();
    lims::test::grant("bob", lims::LimsRole::Verifier, lims::Scope{lab.fieldLab});
    lims::test::grant("carol", lims::LimsRole::Verifier);

    morph::session::TokenIssuer const issuer{std::string{kSecret}, morph::session::hmacSha256};
    BackendRig rig{mode, 1,
                   std::make_shared<lims::auth::LimsAuthorizer>(std::string{kSecret}, [](std::string_view principal) {
                       return lims::OrgModel::rolesAnywhere(principal);
                   })};
    rig.bridge(0).setDefaultSession(signedAs(issuer, "bob"));
    auto verifier = rig.client<lims::SampleModel>(0);
    CHECK_THROWS(awaitQt(verifier.execute(lims::VerifyResult{.resultId = lab.result})));
    {
        ScopedPrincipal const alice{"alice"};
        lims::SampleModel reader;
        reader.execute(lims::OpenSample{.sampleId = lab.sample});
        CHECK(reader.execute(lims::ListVerifications{}).verifications.empty());
    }

    // The control: the same call by a verifier at the sample's company goes through the same edge and the same model.
    rig.bridge(0).setDefaultSession(signedAs(issuer, "carol"));
    auto const recorded = awaitQt(verifier.execute(lims::VerifyResult{.resultId = lab.result}));
    CHECK(recorded.verifiedBy == "carol");
}
```
In `examples/lims/tests/lims_test_support.hpp`, make `grant` take a scope:
```cpp
/// @brief Assigns @p role to @p principal in @p scope.
///
/// Made by `lab-supervisor`, a supervisor at the seeded company the helper appoints itself while the lab has none. A
/// test that appoints its own supervisor first assigns through `OrgModel` as that supervisor instead.
/// @param principal Who gets the role.
/// @param role The role.
/// @param scope Where; the seeded company unless said.
/// @return The assignment.
inline lims::RoleAssignmentView grant(std::string principal, lims::LimsRole role,
                                      lims::Scope scope = lims::Scope{seededCompany()}) {
    constexpr std::string_view kGranter = "lab-supervisor";
    ScopedPrincipal const granter{std::string{kGranter}};
    lims::OrgModel org;
    auto const held = lims::OrgModel::rolesIn(kGranter, lims::Scope{seededCompany()});
    if (std::ranges::find(held, lims::LimsRole::Supervisor) == held.end()) {
        org.execute(lims::AssignRole{
            .principal = std::string{kGranter}, .role = lims::LimsRole::Supervisor, .company = seededCompany(), .site = {}});
    }
    auto const* company = std::get_if<lims::CompanyId>(&scope);
    return org.execute(lims::AssignRole{.principal = std::move(principal),
                                        .role = role,
                                        .company = company != nullptr ? *company : lims::CompanyId{},
                                        .site = company != nullptr ? lims::SiteId{} : std::get<lims::SiteId>(scope)});
}
```
(add `#include <variant>`).

In `examples/lims/tests/test_org_companies.cpp`, add `lims::test::grant("alice", lims::LimsRole::Supervisor);` as the first line after `DbFixture fixture;` in "a company keeps its base currency and fiscal-year start" and "a site belongs to a company that exists". In `examples/lims/tests/test_sample_sites.cpp`, add the same line after `DbFixture fixture;` in "a sample is registered at a site, and its view names the site".

- [ ] **Step 2: Run the build to see it fail**

Run: `flock /tmp/claude-1000/-home-yaraslau-repo-morph/b1b73e49-c41d-4e27-bb68-b8c0cfacf145/scratchpad/morph-build.lock cmake --build build/linux-everything --target ladder_lims_tests -j10`
Expected: FAIL — `fatal error: 'lims/app/declared_requirements.hpp' file not found`.

- [ ] **Step 3: Write the declared form and its evaluator**

Create `examples/lims/org/include/lims/org/authorization/declared.hpp`:
```cpp
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <concepts>
#include <cstdint>
#include <morph/core/registry.hpp>
#include <morph/session/session.hpp>
#include <morph/util/rational.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "lims/org/core/errors.hpp"
#include "lims/org/core/types.hpp"
#include "lims/org/dto/role_dto.hpp"
#include "lims/org/models/org_model.hpp"

/// @file
/// Declared authorization: per action, the role it needs, the scope its payload names, an optional value limit and
/// an optional separation-of-duties pair, declared once beside the action by the module that owns it and checked by
/// `requireAuthorized` inside the model.
///
/// The check runs in the model because only the model has the decoded payload: `IAuthorizer::authorize` at the
/// server edge is handed the model and action ids, never the payload, so the edge can ask whether a principal holds
/// the role somewhere (`lims::app::declaredRequirements`) and nothing more.

namespace lims {

/// @brief One action's declared authorization; an action with no specialisation declares none.
///
/// A specialisation, beside the action's registration in the module that owns it, provides:
/// - `static constexpr LimsRole role` — the role the action needs;
/// - optionally `static Scope scopeOf(Action const&)` — the scope the action's payload names, read from its scope
///   field; without it the role is needed in some scope, any;
/// - optionally `static ::morph::math::Rational amountOf(Action const&)` and
///   `static std::optional<::morph::math::Rational> limitOf(std::string_view principal, Scope const&)` — a value
///   limit: the principal may act on amounts up to their limit in the scope, and on none without one;
/// - optionally `static constexpr std::string_view separatedFrom` (the action id of the pair's first half),
///   `static constexpr std::string_view sameActorRefusal` (the refusal's words) and
///   `static std::optional<std::string> firstPerformerOf(Action const&)` (who performed the first half on the same
///   subject) — a separation-of-duties pair: the principal may not be that performer.
/// @tparam Action The action.
template <typename Action>
struct Authorization;

/// @brief Whether @p Action declares a rule.
/// @tparam Action The action.
template <typename Action>
concept DeclaresRule = requires {
    { Authorization<Action>::role } -> std::convertible_to<LimsRole>;
};

/// @brief Whether @p Action's rule names the scope its payload holds.
/// @tparam Action The action.
template <typename Action>
concept DeclaresScope = DeclaresRule<Action> && requires(Action const& action) {
    { Authorization<Action>::scopeOf(action) } -> std::convertible_to<Scope>;
};

/// @brief Whether @p Action's rule declares a value limit. A limit is judged in a scope, so it needs one.
/// @tparam Action The action.
template <typename Action>
concept DeclaresValueLimit =
    DeclaresScope<Action> && requires(Action const& action, std::string_view principal, Scope const& scope) {
        { Authorization<Action>::amountOf(action) } -> std::convertible_to<::morph::math::Rational>;
        {
            Authorization<Action>::limitOf(principal, scope)
        } -> std::convertible_to<std::optional<::morph::math::Rational>>;
    };

/// @brief Whether @p Action is the second half of a declared separation-of-duties pair.
/// @tparam Action The action.
template <typename Action>
concept DeclaresSeparation = DeclaresRule<Action> && requires(Action const& action) {
    { Authorization<Action>::separatedFrom } -> std::convertible_to<std::string_view>;
    { Authorization<Action>::sameActorRefusal } -> std::convertible_to<std::string_view>;
    { Authorization<Action>::firstPerformerOf(action) } -> std::convertible_to<std::optional<std::string>>;
};

/// @brief What a declared rule decides.
enum class Verdict : std::uint8_t {
    Allowed,      ///< Every declared condition holds.
    MissingRole,  ///< The principal does not hold the role in the scope.
    SameActor,    ///< The principal performed the pair's first half on the same subject.
    NoLimit,      ///< The action carries an amount and the principal has no limit in the scope.
    OverLimit,    ///< The amount is above the principal's limit in the scope.
};

/// @brief Everything a rule is judged on, gathered by `requireAuthorized` from the declaration and the payload.
struct AuthorizationFacts {
    /// @brief The authenticated principal.
    std::string principal;
    /// @brief The role the action needs.
    LimsRole required = LimsRole::Analyst;
    /// @brief The roles the principal holds in the scope (anywhere, for a rule without one).
    std::vector<LimsRole> held;
    /// @brief Who performed the pair's first half on the same subject; empty without a pair or a performer.
    std::optional<std::string> firstPerformer;
    /// @brief The amount the action acts on; empty without a value limit.
    std::optional<::morph::math::Rational> amount;
    /// @brief The principal's limit in the scope; empty when they have none.
    std::optional<::morph::math::Rational> limit;
};

/// @brief Judges @p facts. Pure: the same facts always give the same verdict.
///
/// The role is the first question, the pair the second, the limit the third, so a refusal names the first reason
/// that applies.
/// @param facts The facts.
/// @return The verdict.
[[nodiscard]] Verdict authorizationVerdict(AuthorizationFacts const& facts);

/// @brief Checks @p action against its declaration, as the authenticated principal.
///
/// Called by the model on every dispatch path, after the payload is validated and the rows it names are known to
/// exist, so a refusal is about permission and never a disguised missing row.
/// @tparam Action A declared action.
/// @param action The action.
/// @throws Forbidden with the reason, in words, when the verdict is not `Allowed`.
template <typename Action>
    requires DeclaresRule<Action>
void requireAuthorized(Action const& action) {
    using Rule = Authorization<Action>;
    std::string const principal = ::morph::session::current()->principal;
    AuthorizationFacts facts{.principal = principal,
                             .required = Rule::role,
                             .held = {},
                             .firstPerformer = std::nullopt,
                             .amount = std::nullopt,
                             .limit = std::nullopt};
    std::string where = "in any scope";
    if constexpr (DeclaresScope<Action>) {
        Scope const scope = Rule::scopeOf(action);
        facts.held = OrgModel::rolesIn(principal, scope);
        where = "at " + scopeName(scope);
        if constexpr (DeclaresValueLimit<Action>) {
            facts.amount = Rule::amountOf(action);
            facts.limit = Rule::limitOf(principal, scope);
        }
    } else {
        facts.held = OrgModel::rolesAnywhere(principal);
    }
    if constexpr (DeclaresSeparation<Action>) {
        facts.firstPerformer = Rule::firstPerformerOf(action);
    }
    std::string const actionType{::morph::model::ActionTraits<Action>::typeId()};
    switch (authorizationVerdict(facts)) {
        case Verdict::Allowed:
            return;
        case Verdict::MissingRole:
            throw Forbidden{actionType + ": '" + principal + "' does not hold the " + std::string{roleName(Rule::role)} +
                            " role " + where};
        case Verdict::SameActor:
            if constexpr (DeclaresSeparation<Action>) {
                throw Forbidden{actionType + ": '" + principal + "' " + std::string{Rule::sameActorRefusal}};
            }
            break;
        case Verdict::NoLimit:
            throw Forbidden{actionType + ": '" + principal + "' has no value limit " + where};
        case Verdict::OverLimit:
            throw Forbidden{actionType + ": the amount is above '" + principal + "''s value limit " + where};
    }
    throw Forbidden{actionType + ": refused"};
}

/// @brief One declared role as the server edge can use it: by action id, with no payload.
struct RoleRequirement {
    /// @brief The action id.
    std::string_view actionType;
    /// @brief The role it needs, somewhere.
    LimsRole role = LimsRole::Analyst;
};

/// @brief @p Action's declared role, for the edge's table.
/// @tparam Action A declared action.
/// @return Its action id and role.
template <typename Action>
    requires DeclaresRule<Action>
[[nodiscard]] constexpr RoleRequirement requirementOf() {
    return RoleRequirement{.actionType = ::morph::model::ActionTraits<Action>::typeId(),
                           .role = Authorization<Action>::role};
}

}  // namespace lims
```

Create `examples/lims/org/src/authorization/declared.cpp`:
```cpp
// SPDX-License-Identifier: Apache-2.0
#include "lims/org/authorization/declared.hpp"

#include <algorithm>

namespace lims {

Verdict authorizationVerdict(AuthorizationFacts const& facts) {
    if (std::ranges::find(facts.held, facts.required) == facts.held.end()) {
        return Verdict::MissingRole;
    }
    if (facts.firstPerformer.has_value() && *facts.firstPerformer == facts.principal) {
        return Verdict::SameActor;
    }
    if (facts.amount.has_value()) {
        if (!facts.limit.has_value()) {
            return Verdict::NoLimit;
        }
        if (*facts.amount > *facts.limit) {
            return Verdict::OverLimit;
        }
    }
    return Verdict::Allowed;
}

}  // namespace lims
```

- [ ] **Step 4: Declare org's and lab's rules, and the edge's table**

Create `examples/lims/org/include/lims/org/authorization/org_authorization.hpp`:
```cpp
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "lims/org/authorization/declared.hpp"
#include "lims/org/dto/company_dto.hpp"
#include "lims/org/dto/role_dto.hpp"

/// @file
/// The org module's declared authorization: administering the organisation is a supervisor's.

namespace lims {

/// @brief A new company has no scope yet, so creating one needs a supervisor anywhere.
template <>
struct Authorization<CreateCompany> {
    /// @brief The role.
    static constexpr LimsRole role = LimsRole::Supervisor;
};

/// @brief A site is created by a supervisor at its company.
template <>
struct Authorization<CreateSite> {
    /// @brief The role.
    static constexpr LimsRole role = LimsRole::Supervisor;

    /// @brief The company the payload names.
    /// @param action The action.
    /// @return Its company.
    [[nodiscard]] static Scope scopeOf(CreateSite const& action) { return Scope{action.companyId}; }
};

/// @brief A role is assigned by a supervisor in the scope it is assigned in. `OrgModel` lets the first assignment
///        through on a lab with no supervisor before it asks.
template <>
struct Authorization<AssignRole> {
    /// @brief The role.
    static constexpr LimsRole role = LimsRole::Supervisor;

    /// @brief The company or site the payload names.
    /// @param action The action.
    /// @return Its scope.
    [[nodiscard]] static Scope scopeOf(AssignRole const& action) {
        return action.company.hasValue() ? Scope{action.company} : Scope{action.site};
    }
};

/// @brief An assignment is revoked by a supervisor in the scope it was made in.
template <>
struct Authorization<RevokeRole> {
    /// @brief The role.
    static constexpr LimsRole role = LimsRole::Supervisor;

    /// @brief The scope of the assignment the payload names.
    /// @param action The action.
    /// @return Its scope; an empty company scope when there is no such assignment.
    [[nodiscard]] static Scope scopeOf(RevokeRole const& action);
};

}  // namespace lims
```

Create `examples/lims/org/src/authorization/org_authorization.cpp`:
```cpp
// SPDX-License-Identifier: Apache-2.0
#include "lims/org/authorization/org_authorization.hpp"

#include <Lightweight/DataMapper/DataMapper.hpp>

#include "lims/org/db/org_entity.hpp"

namespace lims {

Scope Authorization<RevokeRole>::scopeOf(RevokeRole const& action) {
    if (!action.assignmentId.hasValue()) {
        return Scope{CompanyId{}};
    }
    Lightweight::DataMapper mapper;
    auto const rows = mapper.Query<db::RoleAssignmentRecord>()
                          .Where(::Lightweight::FieldNameOf<&db::RoleAssignmentRecord::id>, "=", *action.assignmentId)
                          .All();
    if (rows.empty()) {
        return Scope{CompanyId{}};
    }
    auto const id = rows.front().scopeId.Value();
    return static_cast<ScopeKind>(rows.front().scopeKind.Value()) == ScopeKind::Company ? Scope{CompanyId{id}}
                                                                                        : Scope{SiteId{id}};
}

}  // namespace lims
```

Create `examples/lims/lab/include/lims/lab/authorization/lab_authorization.hpp`:
```cpp
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <optional>
#include <string>
#include <string_view>

#include "lims/lab/dto/verification_dto.hpp"
#include "lims/org/authorization/declared.hpp"

/// @file
/// The lab module's declared authorization: four eyes on a reading.

namespace lims {

/// @brief Verifying a reading needs the verifier role at the sample's site, and the verifier must not be the
///        analyst who captured it: capture and verify are a separation-of-duties pair.
template <>
struct Authorization<VerifyResult> {
    /// @brief The role.
    static constexpr LimsRole role = LimsRole::Verifier;

    /// @brief The pair's first half.
    static constexpr std::string_view separatedFrom = "CaptureConcentration";

    /// @brief The refusal when the verifier captured the reading.
    static constexpr std::string_view sameActorRefusal = "captured this result and cannot also verify it";

    /// @brief The site of the sample the payload's result belongs to.
    /// @param action The action.
    /// @return That site; an empty site when the result or its sample does not exist.
    [[nodiscard]] static Scope scopeOf(VerifyResult const& action);

    /// @brief Who captured the payload's result. A replayed or force-applied capture keeps its field operator here,
    ///        so the pair holds for those too.
    /// @param action The action.
    /// @return The capturing analyst; empty when the result does not exist.
    [[nodiscard]] static std::optional<std::string> firstPerformerOf(VerifyResult const& action);
};

}  // namespace lims
```

Create `examples/lims/lab/src/authorization/lab_authorization.cpp`:
```cpp
// SPDX-License-Identifier: Apache-2.0
#include "lims/lab/authorization/lab_authorization.hpp"

#include <Lightweight/DataMapper/DataMapper.hpp>
#include <cstdint>
#include <vector>

#include "lims/lab/db/lims_entity.hpp"

namespace lims {

namespace {

/// @brief The result row @p result names.
/// @param mapper The open mapper.
/// @param result The result.
/// @return Its row, at most one.
[[nodiscard]] std::vector<db::ResultRecord> resultRows(Lightweight::DataMapper& mapper, ResultId result) {
    if (!result.hasValue()) {
        return {};
    }
    return mapper.Query<db::ResultRecord>().Where(::Lightweight::FieldNameOf<&db::ResultRecord::id>, "=", *result).All();
}

}  // namespace

Scope Authorization<VerifyResult>::scopeOf(VerifyResult const& action) {
    Lightweight::DataMapper mapper;
    auto const results = resultRows(mapper, action.resultId);
    if (results.empty()) {
        return Scope{SiteId{}};
    }
    auto const samples = mapper.Query<db::SampleRecord>()
                             .Where(::Lightweight::FieldNameOf<&db::SampleRecord::id>, "=",
                                    static_cast<std::int64_t>(results.front().sample.Value()))
                             .All();
    return samples.empty() ? Scope{SiteId{}} : Scope{SiteId::fromOptional(samples.front().siteId.Value())};
}

std::optional<std::string> Authorization<VerifyResult>::firstPerformerOf(VerifyResult const& action) {
    Lightweight::DataMapper mapper;
    auto const results = resultRows(mapper, action.resultId);
    if (results.empty()) {
        return std::nullopt;
    }
    return std::string{results.front().capturedBy.Value().ToStringView()};
}

}  // namespace lims
```

Create `examples/lims/app/include/lims/app/declared_requirements.hpp`:
```cpp
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <array>
#include <optional>
#include <string_view>

#include "lims/lab/authorization/lab_authorization.hpp"
#include "lims/lab/models/sample_model.hpp"
#include "lims/org/authorization/org_authorization.hpp"
#include "lims/org/models/org_model.hpp"

/// @file
/// The declared roles of every module, as the server edge uses them: by action id, held anywhere. The app is the one
/// place that knows every module, so the table is composed here; each entry is read off its declaration, never
/// restated.

namespace lims::app {

/// @brief Every action a module declares a role for.
/// @return The table.
[[nodiscard]] inline std::array<RoleRequirement, 5> const& declaredRequirements() {
    static constexpr std::array<RoleRequirement, 5> requirements{
        requirementOf<VerifyResult>(), requirementOf<CreateCompany>(), requirementOf<CreateSite>(),
        requirementOf<AssignRole>(),   requirementOf<RevokeRole>(),
    };
    return requirements;
}

/// @brief The role @p actionType declares.
/// @param actionType The action id.
/// @return The role, or nothing for an action that declares none.
[[nodiscard]] inline std::optional<LimsRole> requiredRole(std::string_view actionType) {
    for (RoleRequirement const& requirement : declaredRequirements()) {
        if (requirement.actionType == actionType) {
            return requirement.role;
        }
    }
    return std::nullopt;
}

}  // namespace lims::app
```

- [ ] **Step 5: Check the declarations in the models and at the edge**

In `examples/lims/org/src/models/org_model.cpp`: add `#include "lims/org/authorization/org_authorization.hpp"`; delete the helper `requireSupervisorIn`; in `execute(const CreateCompany&)` add `requireAuthorized(action);` after the validation throw; in `execute(const CreateSite&)` add `requireAuthorized(action);` after the company `NotFound` throw; in `execute(const AssignRole&)` replace `requireSupervisorIn(scope);` with `requireAuthorized(action);`; in `execute(const RevokeRole&)` replace the `requireSupervisorIn(…)` line with `requireAuthorized(action);`.

In `examples/lims/lab/src/models/sample_model.cpp`: add `#include "lims/lab/authorization/lab_authorization.hpp"`; delete `SampleModel::requireRole`; in `execute(const VerifyResult&)` delete the `requireRole(LimsRole::Verifier);` line and the comment above it, and replace the block
```cpp
        // Half two: the second pair of eyes must belong to a second person.
        // This is the half `IAuthorizer` cannot express -- it is handed the
        // model and action type ids and never the row -- so it can only live
        // here.
        if (capturedBy == principal) {
            throw Forbidden{"VerifyResult: '" + principal + "' captured this result and cannot also verify it"};
        }
```
with
```cpp
        // Both halves of four eyes, as lab declares them: the verifier role at the sample's site, and not being the
        // analyst who captured the reading. The server edge can check only the role, held anywhere, because it
        // never sees the result; this check runs on every dispatch path.
        requireAuthorized(action);
```
In `examples/lims/lab/include/lims/lab/models/sample_model.hpp`, delete the `requireRole` declaration and its doc comment, and in `execute(const VerifyResult&)`'s doc replace
```cpp
    /// Both halves of the control are enforced here, because only one of them
    /// *can* be enforced anywhere else: the caller must hold
    /// `LimsRole::Verifier` (which `lims::auth::LimsAuthorizer` also checks at
    /// the `RemoteServer` edge, and which is re-checked here because no
    /// authorizer runs on the `Local` path), and the caller must not be the
    /// analyst who captured the result (which `IAuthorizer` structurally
    /// cannot express — it never sees the result).
```
with
```cpp
    /// Both halves of the control are checked here, as
    /// `lims::Authorization<VerifyResult>` declares them: the caller must hold
    /// `LimsRole::Verifier` at the sample's site, and must not be the analyst
    /// who captured the result. `lims::auth::LimsAuthorizer` can check only
    /// that the role is held somewhere, and runs on no `Local` path at all.
```
and its `@throws Forbidden if the caller lacks `Verifier`, or captured the result.` with `@throws Forbidden if the caller lacks `Verifier` at the sample's site, or captured the result.`

In `examples/lims/app/include/lims/app/lims_authorizer.hpp`: add `#include "lims/app/declared_requirements.hpp"`; delete `requiresVerifierRole` and `requiresSupervisorRole`; in `authorize`, replace
```cpp
        if (requiresVerifierRole(actionType)) {
            return holds(*principal, LimsRole::Verifier);
        }
        if (requiresSupervisorRole(actionType)) {
            return holds(*principal, LimsRole::Supervisor);
        }
        return true;
```
with
```cpp
        // The edge sees no payload, so it asks whether the principal holds the declared role anywhere; the scope,
        // a value limit and a separation-of-duties pair are checked by the model, which has the payload.
        if (auto const role = ::lims::app::requiredRole(actionType)) {
            return holds(*principal, *role);
        }
        return true;
```
and in the file comment replace
```cpp
/// @par What it can and cannot do, exactly
/// `IAuthorizer::authorize` is handed the caller's `Context` plus the target
/// **model type** and **action type** ids — and nothing else. It can answer
/// "may this principal ever verify a result?"; it structurally cannot answer
/// "may this principal verify *this* result?", because it never sees the
/// result. The second question is the one four-eyes actually turns on
/// (a verifier must not verify their own reading), so `SampleModel` enforces
/// it itself. Neither check stands in for the other.
```
with
```cpp
/// @par What it can and cannot do, exactly
/// `IAuthorizer::authorize` is handed the caller's `Context` plus the target
/// **model type** and **action type** ids — and nothing else. From the
/// declared roles (`lims::app::declaredRequirements`) it can answer "may this
/// principal ever verify a result?"; it cannot answer "may this principal
/// verify *this* result?", because it never sees the result, its site or who
/// captured it. `lims::requireAuthorized` answers that in the model. Neither
/// check stands in for the other.
```

- [ ] **Step 6: Build and run**

Run:
```
flock /tmp/claude-1000/-home-yaraslau-repo-morph/b1b73e49-c41d-4e27-bb68-b8c0cfacf145/scratchpad/morph-build.lock cmake --build build/linux-everything --target ladder_lims_tests ladder_lims_screen_tests ladder_lims_render_smoke ladder_lims_server morph_examples_client -j10
flock /tmp/claude-1000/-home-yaraslau-repo-morph/b1b73e49-c41d-4e27-bb68-b8c0cfacf145/scratchpad/morph-build.lock timeout 300 build/linux-everything/examples/lims/ladder_lims_tests "[authorization]"
flock /tmp/claude-1000/-home-yaraslau-repo-morph/b1b73e49-c41d-4e27-bb68-b8c0cfacf145/scratchpad/morph-build.lock ctest --test-dir build/linux-everything -j6 -L ladder-lims --output-on-failure
```
Expected: `[authorization]` passes, including both `[h6]` cases on all three modes; ctest `100% tests passed` (the verification suite's "A refused verification appears in the trail" still finds `cannot also verify` in the refusal's detail).

- [ ] **Step 7: Mutation checks**

Apply each `// Mutation:` in `test_declared_authorization.cpp` in turn (e.g. in `requireAuthorized` replace `OrgModel::rolesIn(principal, scope)` with `OrgModel::rolesAnywhere(principal)`; delete `requireAuthorized(action);` from `VerifyResult`; change `*facts.amount > *facts.limit` to `>=`), rebuild, run `… ladder_lims_tests "[authorization]"`, confirm the named case fails, and revert.
Expected: each mutation fails exactly the case naming it. The `rolesAnywhere` mutation must fail "verifying needs the verifier role at the sample's site" and the Socket leg of the second `[h6]` case — if it does not, the scope is not being measured.

- [ ] **Step 8: Record hypothesis 6's first outcome and commit**

Append to `examples/lims/README.md`'s resolved decisions:
```markdown
### 24. Authorization is declared per action and checked in the model (hypothesis 6, first instance)

An action's authorization is declared once, beside its registration, by the module that owns it
(`lims::Authorization<Action>`): the role it needs, the scope its payload names, an optional value limit, and an
optional separation-of-duties pair. Capture and verify are the first declared pair: verifying a reading needs the
verifier role at the sample's site, and the verifier must not be the analyst who captured it. `requireAuthorized`
checks a declaration inside the model, on every dispatch path; the server edge checks only that the principal holds
the declared role somewhere, from a table the app composes from every module's declarations.

**Hypothesis 6 — confirmed for its first instance.** `IAuthorizer::authorize` is handed the model and action ids and
never the payload, so the scope (the result's sample's site), the pair (who captured the result) and any value limit
can only be checked where the payload is decoded. Measured in `test_declared_authorization.cpp`: a verifier whose
role is at another site of the same company is allowed by `LimsAuthorizer` and refused by the model, in `Local`,
`LocalSingleThread` and `Socket` mode alike, and a verifier at the sample's company goes through the same edge and
model. What the first instance shows about a seam: the declaration is data apart from `scopeOf` and
`firstPerformerOf`, which read the module's own rows, so a framework check would have to run after decoding and call
back into the module for those; and the edge needs a second, app-composed table, because it cannot reach the
declarations through the action ids it is given. No framework seam is designed from one instance. The second
instance, billing's create-and-approve with an approver's value limit, decides between a framework seam and
"domain logic the models must own"; this decision is revisited then.
```
Then:
```bash
cd /home/yaraslau/repo/morph
git add -A examples/lims ':!examples/lims/design'
git commit -m "Declare lims authorization per action, with capture and verify as a pair

Each action declares its role, the scope its payload names, an optional
value limit and an optional separation-of-duties pair; the model checks the
declaration on every path, and the server edge checks the role only. Records
hypothesis 6's first instance: the edge allows a verifier whose role is at
another site, and only the model refuses.

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01No1NznqxEBvjhZBaBRLCd6"
```

---
### Task 5: Visibility — every list returns only records in the caller's scopes

**Files:**
- Modify: `examples/lims/org/include/lims/org/core/model_support.hpp` (`currentPrincipal()`)
- Modify: `examples/lims/org/include/lims/org/models/org_model.hpp`, `examples/lims/org/src/models/org_model.cpp` (`visibleSites`, `visibleCompanies`; lists filtered; a company's creator is its first supervisor; effects)
- Modify: `examples/lims/lab/include/lims/lab/dto/sample_dto.hpp` (`ListSamples`), `examples/lims/lab/include/lims/lab/models/sample_model.hpp`, `examples/lims/lab/src/models/sample_model.cpp` (`execute(const ListSamples&)`)
- Create: `examples/lims/tests/test_visibility.cpp`
- Modify: `examples/lims/tests/test_org_companies.cpp`, `examples/lims/screens/tests/test_lims_screens.cpp`

**Interfaces:**
- Consumes (Tasks 2–4): `OrgModel`, `RoleAssignmentRecord`, `assignmentsOf`/`toView` helpers in `org_model.cpp`, `requireAuthorized`, `lims::test::grant(principal, role, scope)`, `SampleView::siteId`, `SampleRecord::siteId`.
- Produces:
  - `std::string lims::currentPrincipal()` — the authenticated principal, empty when none.
  - `static std::vector<SiteId> OrgModel::visibleSites(std::string_view principal)` — sites where the principal holds a role at the site or at its company; `static std::vector<CompanyId> OrgModel::visibleCompanies(std::string_view principal)` — companies where they hold a role at the company or at one of its sites.
  - `ListCompanies`, `ListSites`, `ListRoleAssignments`, `ListUsers` answer only what the caller can see.
  - `struct lims::ListSamples { ::morph::table::TableQuery query; }` and `::morph::table::Page<SampleView> SampleModel::execute(const ListSamples&)` (registered `"ListSamples"`, a read).
  - `CreateCompany` assigns its creator `Supervisor` at the new company, in the same transaction.

- [ ] **Step 1: Write the failing visibility tests**

Create `examples/lims/tests/test_visibility.cpp`:
```cpp
// SPDX-License-Identifier: Apache-2.0
//
// Visibility: every list returns only records in scopes where the caller holds a role. A role at a company sees the
// company and all its sites; a role at a site sees that site and, through it, its company.

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <cstdint>
#include <memory>
#include <morph/session/session_auth.hpp>
#include <morph/table/query.hpp>
#include <string>
#include <string_view>
#include <vector>

#include "lims/app/lims_authorizer.hpp"
#include "lims/lab/models/sample_model.hpp"
#include "lims/org/models/org_model.hpp"
#include "lims_test_support.hpp"
#include "testkit/backend_rig.hpp"
#include "testkit/db_fixture.hpp"
#include "testkit/pump.hpp"

using lims::test::ScopedPrincipal;
using morph::ladder::testkit::awaitQt;
using morph::ladder::testkit::BackendRig;
using morph::ladder::testkit::DbFixture;
using morph::ladder::testkit::Mode;

namespace {

constexpr std::string_view kSecret = "lims-visibility-test-secret-at-least-32-b";

/// @brief The organisation every case here starts from.
struct Org {
    lims::CompanyId osaka;   ///< A second company, alice's.
    lims::SiteId fieldLab;   ///< A second site of the seeded company.
    lims::SampleId atMain;   ///< A sample at the seeded site.
    lims::SampleId atField;  ///< A sample at the field lab.
};

/// @brief alice supervises the seeded company and creates the rest; bob is an analyst at the field lab only.
/// @return The ids.
[[nodiscard]] Org setUp() {
    lims::test::grant("alice", lims::LimsRole::Supervisor);
    ScopedPrincipal const alice{"alice"};
    lims::OrgModel org;
    auto const osaka = org.execute(lims::CreateCompany{
        .name = "Osaka branch", .baseCurrency = lims::CurrencyCode::JPY, .fiscalYearStart = lims::Month::April});
    auto const fieldLab = org.execute(lims::CreateSite{.companyId = lims::test::seededCompany(), .name = "Field lab"});
    org.execute(lims::AssignRole{.principal = "bob", .role = lims::LimsRole::Analyst, .company = {}, .site = fieldLab.id});
    lims::SampleModel bench;
    auto const client = bench.execute(lims::RegisterClient{.name = "Waterworks Ltd"});
    auto const atMain = bench.execute(
        lims::RegisterSample{.siteId = lims::test::seededSite(), .clientId = client.clientId, .reference = "WW-MAIN"});
    lims::SampleModel other;
    auto const atField =
        other.execute(lims::RegisterSample{.siteId = fieldLab.id, .clientId = client.clientId, .reference = "WW-FIELD"});
    return Org{.osaka = osaka.id, .fieldLab = fieldLab.id, .atMain = atMain.id, .atField = atField.id};
}

/// @brief The ids of @p rows.
/// @tparam Row A view with an `id`.
/// @param rows The rows.
/// @return Their ids, in order.
template <typename Row>
[[nodiscard]] auto idsOf(std::vector<Row> const& rows) {
    std::vector<decltype(Row::id)> ids;
    for (Row const& row : rows) {
        ids.push_back(row.id);
    }
    return ids;
}

}  // namespace

// Mutation: let visibleSites ignore company-scoped roles (alice sees no site).
TEST_CASE("visibility: companies and sites are listed for the scopes the caller holds a role in",
          "[lims][visibility]") {
    DbFixture fixture;
    auto const org = setUp();
    lims::OrgModel model;
    {
        ScopedPrincipal const alice{"alice"};
        CHECK(idsOf(model.execute(lims::ListCompanies{}).companies) ==
              std::vector<lims::CompanyId>{lims::test::seededCompany(), org.osaka});
        CHECK(idsOf(model.execute(lims::ListSites{}).sites) ==
              std::vector<lims::SiteId>{lims::test::seededSite(), org.fieldLab});
    }
    {
        ScopedPrincipal const bob{"bob"};
        CHECK(idsOf(model.execute(lims::ListCompanies{}).companies) ==
              std::vector<lims::CompanyId>{lims::test::seededCompany()});
        CHECK(idsOf(model.execute(lims::ListSites{}).sites) == std::vector<lims::SiteId>{org.fieldLab});
        for (auto const& assignment : model.execute(lims::ListRoleAssignments{}).assignments) {
            CHECK(assignment.company != org.osaka);
        }
    }
    {
        ScopedPrincipal const carol{"carol"};
        CHECK(model.execute(lims::ListCompanies{}).companies.empty());
        CHECK(model.execute(lims::ListSites{}).sites.empty());
        CHECK(model.execute(lims::ListRoleAssignments{}).assignments.empty());
        CHECK(model.execute(lims::ListUsers{}).users.empty());
    }
}

// Mutation: skip the site filter in ListSamples (bob sees the main site's sample).
TEST_CASE("visibility: samples are listed for the sites the caller can see, and paged", "[lims][visibility]") {
    DbFixture fixture;
    auto const org = setUp();
    lims::SampleModel model;
    {
        ScopedPrincipal const bob{"bob"};
        auto const page = model.execute(lims::ListSamples{});
        CHECK(idsOf(page.rows) == std::vector<lims::SampleId>{org.atField});
    }
    {
        ScopedPrincipal const alice{"alice"};
        auto const page = model.execute(lims::ListSamples{});
        CHECK(idsOf(page.rows) == std::vector<lims::SampleId>{org.atMain, org.atField});
        lims::ListSamples firstOnly;
        firstOnly.query.page.limit = 1;
        auto const paged = model.execute(firstOnly);
        CHECK(idsOf(paged.rows) == std::vector<lims::SampleId>{org.atMain});
        CHECK(paged.total == std::optional<std::int64_t>{2});
    }
    {
        ScopedPrincipal const carol{"carol"};
        CHECK(model.execute(lims::ListSamples{}).rows.empty());
    }
    CHECK(model.execute(lims::ListSamples{}).rows.empty());
}

// Mutation: drop the creator's assignment from CreateCompany (alice cannot see the company she created).
TEST_CASE("visibility: a company's creator is its first supervisor", "[lims][visibility]") {
    DbFixture fixture;
    auto const org = setUp();
    auto const held = lims::OrgModel::rolesIn("alice", lims::Scope{org.osaka});
    REQUIRE(held.size() == 1);
    CHECK(held.front() == lims::LimsRole::Supervisor);
    CHECK(lims::OrgModel::rolesIn("bob", lims::Scope{org.osaka}).empty());
}

TEST_CASE("visibility: the sample list is scoped over every backend", "[lims][visibility][matrix]") {
    auto const mode = GENERATE(Mode::Local, Mode::LocalSingleThread, Mode::Socket);
    CAPTURE(mode);
    DbFixture fixture;
    auto const org = setUp();
    morph::session::TokenIssuer const issuer{std::string{kSecret}, morph::session::hmacSha256};
    BackendRig rig{mode, 1,
                   std::make_shared<lims::auth::LimsAuthorizer>(std::string{kSecret}, [](std::string_view principal) {
                       return lims::OrgModel::rolesAnywhere(principal);
                   })};
    morph::session::Context bob;
    bob.principal = "bob";
    bob.token = issuer.issue(
        morph::session::SessionToken{.principal = "bob", .issuedAtMs = 0, .expiresAtMs = 4102444800000, .roles = {}});
    rig.bridge(0).setDefaultSession(bob);
    auto lister = rig.client<lims::SampleModel>(0);
    auto const page = awaitQt(lister.execute(lims::ListSamples{}));
    CHECK(idsOf(page.rows) == std::vector<lims::SampleId>{org.atField});
}
```

In `examples/lims/tests/test_org_companies.cpp`, in "the first migration seeds one company with one site", add `lims::test::grant("alice", lims::LimsRole::Analyst);` as the first line after `DbFixture fixture;`.

In `examples/lims/screens/tests/test_lims_screens.cpp`, in "registration registers a client and a sample, then opens the sample's lifecycle", add `lims::test::grant("tech", lims::LimsRole::Analyst);` after `DbFixture fixture;` (the site table lists only the sites tech can see).

- [ ] **Step 2: Run the build to see it fail**

Run: `flock /tmp/claude-1000/-home-yaraslau-repo-morph/b1b73e49-c41d-4e27-bb68-b8c0cfacf145/scratchpad/morph-build.lock cmake --build build/linux-everything --target ladder_lims_tests -j10`
Expected: FAIL — `no member named 'ListSamples' in namespace 'lims'`.

- [ ] **Step 3: Scope the org lists**

In `examples/lims/org/include/lims/org/core/model_support.hpp`, add `#include <string>` and, after `requirePrincipal()`:
```cpp
/// @brief The authenticated principal, or empty when there is none. Lists answer an empty principal with nothing,
///        since nobody holds a role in any scope.
/// @return The principal.
[[nodiscard]] inline std::string currentPrincipal() {
    const auto* ctx = ::morph::session::current();
    return ctx == nullptr ? std::string{} : ctx->principal;
}
```

In `examples/lims/org/include/lims/org/models/org_model.hpp`, add after `supervisorExists()`:
```cpp
    /// @brief The sites @p principal can see: those where they hold a role at the site or at its company.
    /// @param principal The principal.
    /// @return The sites, by id.
    [[nodiscard]] static std::vector<SiteId> visibleSites(std::string_view principal);

    /// @brief The companies @p principal can see: those where they hold a role at the company or at one of its sites.
    /// @param principal The principal.
    /// @return The companies, by id.
    [[nodiscard]] static std::vector<CompanyId> visibleCompanies(std::string_view principal);
```
change the doc of each list: `ListCompanies` "@return The companies the caller can see, by id.", `ListSites` "@return The sites the caller can see, by id.", `ListRoleAssignments` "@return The assignments whose scope the caller can see, by id.", `ListUsers` "@return The principals holding an assignment the caller can see, by principal."; and in `CreateCompany`'s doc add "The creator is assigned `Supervisor` at the new company in the same transaction: without a role there, the company would be invisible to everyone, its creator included." Replace the effects lines:
```cpp
BRIDGE_REGISTER_EFFECTS(lims::CreateCompany, ::morph::effects::changes<lims::CompanyView>,
                        ::morph::effects::changes<lims::RoleAssignmentView>, ::morph::effects::changes<lims::UserView>)
```
```cpp
BRIDGE_REGISTER_EFFECTS(lims::AssignRole, ::morph::effects::changes<lims::RoleAssignmentView>,
                        ::morph::effects::changes<lims::UserView>, ::morph::effects::changes<lims::CompanyView>,
                        ::morph::effects::changes<lims::SiteView>)
```
```cpp
BRIDGE_REGISTER_EFFECTS(lims::RevokeRole, ::morph::effects::changes<lims::RoleAssignmentView>,
                        ::morph::effects::changes<lims::UserView>, ::morph::effects::changes<lims::CompanyView>,
                        ::morph::effects::changes<lims::SiteView>)
```

In `examples/lims/org/src/models/org_model.cpp`:
- In `execute(const CreateCompany&)`, replace the body after `requireAuthorized(action);` with:
```cpp
    Lightweight::DataMapper mapper;
    Lightweight::SqlTransaction sqlTxn{mapper.Connection(), Lightweight::SqlTransactionMode::ROLLBACK};
    db::CompanyRecord row;
    row.name = Lightweight::SqlAnsiString<128>{action.name};
    row.baseCurrency = static_cast<int>(action.baseCurrency);
    row.fiscalYearStart = static_cast<int>(action.fiscalYearStart);
    mapper.Create(row);
    db::RoleAssignmentRecord creator;
    creator.principal = Lightweight::SqlAnsiString<64>{::morph::session::current()->principal};
    creator.role = static_cast<int>(LimsRole::Supervisor);
    creator.scopeKind = static_cast<int>(ScopeKind::Company);
    creator.scopeId = static_cast<std::int64_t>(row.id.Value());
    creator.grantedBy = creator.principal;
    creator.grantedAt = nowMillis();
    mapper.Create(creator);
    sqlTxn.Commit();
    return toView(row);
```
- In `execute(const ListCompanies&)`, keep only visible companies:
```cpp
    auto const visible = visibleCompanies(currentPrincipal());
    Lightweight::DataMapper mapper;
    ListCompaniesResult result;
    for (auto const& row : mapper.Query<db::CompanyRecord>().All()) {
        auto view = toView(row);
        if (std::ranges::find(visible, view.id) != visible.end()) {
            result.companies.push_back(std::move(view));
        }
    }
```
(the sort after it stays).
- In `execute(const ListSites&)`, likewise with `auto const visible = visibleSites(currentPrincipal());` and `std::ranges::find(visible, view.id)`.
- In `execute(const ListRoleAssignments&)`, keep an assignment only when its scope is visible:
```cpp
    auto const principal = currentPrincipal();
    auto const sites = visibleSites(principal);
    auto const companies = visibleCompanies(principal);
    Lightweight::DataMapper mapper;
    ListRoleAssignmentsResult result;
    for (auto const& row : mapper.Query<db::RoleAssignmentRecord>().All()) {
        auto view = toView(row);
        bool const seen = view.scopeKind == ScopeKind::Company ? std::ranges::find(companies, view.company) != companies.end()
                                                               : std::ranges::find(sites, view.site) != sites.end();
        if (seen) {
            result.assignments.push_back(std::move(view));
        }
    }
```
(the sort stays; `ListUsers` builds on it and needs no change).
- Add the two lookups:
```cpp
std::vector<SiteId> OrgModel::visibleSites(std::string_view principal) {
    Lightweight::DataMapper mapper;
    auto const assignments = assignmentsOf(mapper, principal);
    std::vector<SiteId> visible;
    for (auto const& site : mapper.Query<db::SiteRecord>().All()) {
        auto const siteId = static_cast<std::int64_t>(site.id.Value());
        auto const companyId = static_cast<std::int64_t>(site.company.Value());
        bool const seen = std::ranges::any_of(assignments, [siteId, companyId](db::RoleAssignmentRecord const& row) {
            auto const kind = static_cast<ScopeKind>(row.scopeKind.Value());
            return (kind == ScopeKind::Site && row.scopeId.Value() == siteId) ||
                   (kind == ScopeKind::Company && row.scopeId.Value() == companyId);
        });
        if (seen) {
            visible.push_back(SiteId{siteId});
        }
    }
    std::ranges::sort(visible, {}, [](SiteId const& site) { return *site; });
    return visible;
}

std::vector<CompanyId> OrgModel::visibleCompanies(std::string_view principal) {
    Lightweight::DataMapper mapper;
    std::vector<CompanyId> visible;
    for (auto const& row : assignmentsOf(mapper, principal)) {
        if (static_cast<ScopeKind>(row.scopeKind.Value()) == ScopeKind::Company) {
            visible.emplace_back(row.scopeId.Value());
        }
    }
    for (SiteId const site : visibleSites(principal)) {
        if (auto const found = findSite(site)) {
            visible.push_back(found->companyId);
        }
    }
    std::ranges::sort(visible, {}, [](CompanyId const& company) { return *company; });
    auto const duplicates = std::ranges::unique(visible);
    visible.erase(duplicates.begin(), duplicates.end());
    return visible;
}
```

- [ ] **Step 4: Add the scoped sample list**

In `examples/lims/lab/include/lims/lab/dto/sample_dto.hpp`, add `#include <morph/table/query.hpp>` and after `struct GetSample`:
```cpp
/// @brief Lists the samples at the sites where the caller holds a role, through the table engine's query.
struct ListSamples {
    /// @brief The sort, filters and page.
    ::morph::table::TableQuery query;
};
```
In `examples/lims/lab/include/lims/lab/models/sample_model.hpp`, after `execute(const GetSample&)`:
```cpp
    /// @brief Lists the samples the caller can see: those at a site where they hold a role, there or at its company.
    /// @param action The sort, filters and page.
    /// @return The page.
    /// @throws ValidationError if the query is refused by the table engine.
    ::morph::table::Page<SampleView> execute(const ListSamples& action);
```
and after the `GetSample` registration:
```cpp
BRIDGE_REGISTER_ACTION(lims::SampleModel, lims::ListSamples, "ListSamples", ::morph::model::Loggable::No)
```
In `examples/lims/lab/src/models/sample_model.cpp`, add `#include <morph/table/query.hpp>` and after `execute(const GetSample&)`:
```cpp
// An action handler: the registry calls it on the instance, like every other.
// NOLINTNEXTLINE(readability-convert-member-functions-to-static)
::morph::table::Page<SampleView> SampleModel::execute(const ListSamples& action) {
    auto const visible = OrgModel::visibleSites(currentPrincipal());
    Lightweight::DataMapper mapper;
    std::vector<SampleView> rows;
    for (const auto& row : mapper.Query<db::SampleRecord>().All()) {
        auto view = toView(row);
        if (std::ranges::find(visible, view.siteId) != visible.end()) {
            rows.push_back(std::move(view));
        }
    }
    std::ranges::sort(rows, {}, [](SampleView const& sample) { return *sample.id; });
    auto page = ::morph::table::apply<SampleView>(
        rows, [](SampleView const& sample) { return ::morph::table::RowId{*sample.id}; }, action.query);
    if (!page) {
        throw ValidationError{"ListSamples: " + page.error().message};
    }
    return std::move(*page);
}
```

- [ ] **Step 5: Build and run**

Run:
```
flock /tmp/claude-1000/-home-yaraslau-repo-morph/b1b73e49-c41d-4e27-bb68-b8c0cfacf145/scratchpad/morph-build.lock cmake --build build/linux-everything --target ladder_lims_tests ladder_lims_screen_tests ladder_lims_render_smoke ladder_lims_server morph_examples_client -j10
flock /tmp/claude-1000/-home-yaraslau-repo-morph/b1b73e49-c41d-4e27-bb68-b8c0cfacf145/scratchpad/morph-build.lock timeout 300 build/linux-everything/examples/lims/ladder_lims_tests "[visibility]"
flock /tmp/claude-1000/-home-yaraslau-repo-morph/b1b73e49-c41d-4e27-bb68-b8c0cfacf145/scratchpad/morph-build.lock ctest --test-dir build/linux-everything -j6 -L ladder-lims --output-on-failure
```
Expected: `[visibility]` passes on all three modes; ctest `100% tests passed`. Then apply each `// Mutation:` in `test_visibility.cpp`, rebuild, confirm the named case fails, revert.

- [ ] **Step 6: Record the decision and commit**

Append to `examples/lims/README.md`'s resolved decisions:
```markdown
### 25. A list shows only what the caller's scopes reach

`ListSamples`, `ListCompanies`, `ListSites`, `ListRoleAssignments` and `ListUsers` answer only records in scopes where
the caller holds a role: a role at a company reaches the company and all its sites, a role at a site reaches that
site and, through it, its company. Nobody, and an unauthenticated call, sees nothing. A company's creator is assigned
`Supervisor` at it in the same transaction, since a company nobody holds a role in would be listed to nobody.

Reading one record by id (`OpenSample`, `GetSample`, the per-sample lists) is not scope-filtered in Part A: those are
not list queries, and an action that changes a record is authorized by its declaration (decision 24). Which records'
*changes* reach a principal of another company is hypothesis 7, measured with billing.

**Finding: a shared module's action cannot declare effects on another module's records.** Assigning or revoking a
role changes which samples a principal sees, so `AssignRole` changes the rows `ListSamples` answers. But `AssignRole`
belongs to `org`, which cannot name lab's `SampleView`, and a second `BRIDGE_REGISTER_EFFECTS` for the same action
from `app` is refused as "declares different effects in two registrations". So a screen listing samples does not
refresh when its viewer's roles change; it refreshes on the next sample change or reload. Observed once, by reading
`include/morph/core/effects.hpp` (`addEffects`, the two-declarations check); not reproduced on a screen, because no
Part A screen lists samples. A second module needing the same (billing's lines on a role change) makes it a framework
gap to file against `include/morph`.
```
Then:
```bash
cd /home/yaraslau/repo/morph
git add -A examples/lims ':!examples/lims/design'
git commit -m "Scope lims lists to the caller's companies and sites

ListSamples, ListCompanies, ListSites, ListRoleAssignments and ListUsers
answer only records in scopes where the caller holds a role; a company's
creator becomes its first supervisor.

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01No1NznqxEBvjhZBaBRLCd6"
```

---

### Task 6: Administration screens for companies, sites, users and role assignments

**Files:**
- Create: `examples/lims/screens/org_screens.hpp`
- Create: `examples/lims/screens/org_screens.cpp`
- Modify: `examples/lims/screens/lims_screens.hpp` (`kCompanies`, `kAccess`), `examples/lims/screens/lims_screens.cpp` (`bundle()` registers them)
- Create: `examples/lims/screens/tests/test_lims_org_screens.cpp`
- Modify: `examples/lims/screens/render_smoke/test_lims_render.cpp` (the mounted list)

**Interfaces:**
- Consumes (Tasks 2–5): `OrgModel` and its actions `CreateCompany`, `ListCompanies`, `CreateSite`, `ListSites`, `AssignRole`, `RevokeRole`, `ListRoleAssignments`, `ListUsers`; records `Org_Company`, `Org_Site`, `Org_RoleAssignment`, `Org_User`; `lims::test::grant`.
- Produces: screen ids `lims::screens::kCompanies == "lims.org.companies"` and `lims::screens::kAccess == "lims.org.access"`; functions `lims::screens::companiesScreen()` and `lims::screens::accessScreen()` returning `morph::ui::author::Screen`; test ids `companyCount`, `companies`, `companyForm`, `siteCount`, `sites`, `siteForm`, `userCount`, `users`, `assignmentCount`, `assignments`, `assignForm`, `revoke`, `error`.

- [ ] **Step 1: Write the failing screen tests**

Create `examples/lims/screens/tests/test_lims_org_screens.cpp`:
```cpp
// SPDX-License-Identifier: Apache-2.0
//
// lims' administration screens on the screen harness: companies and sites, and users and their role assignments.
// Every form is the action's own schema, and every list refreshes from the declared effects of the action that
// changed it.

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <string>
#include <testkit/db_fixture.hpp>
#include <testkit/screen_harness.hpp>

#include "lims/org/dto/role_dto.hpp"
#include "lims_screens.hpp"
#include "lims_test_support.hpp"

namespace {

using morph::client::LocalApp;
using morph::client::LocalSetup;
using morph::examples::testing::RigMode;
using morph::examples::testing::ScreenHarness;
using morph::ladder::testkit::DbFixture;

LocalApp limsApp() { return LocalApp{.name = "lims", .bundle = &lims::screens::bundle, .setUp = {}}; }

void reportProblems(ScreenHarness const& harness) {
    for (std::string const& problem : harness.problems()) {
        UNSCOPED_INFO(problem);
    }
}

}  // namespace

// Mutations: drop changes<CompanyView> from CreateCompany's effects (the count stays at 1 company); drop
// changes<SiteView> from CreateSite's (the count stays at 1 site).
TEST_CASE("lims org screens: a supervisor creates a company and a site, and both lists show them",
          "[lims-screens][org]") {
    DbFixture fixture;
    lims::test::grant("tech", lims::LimsRole::Supervisor);
    ScreenHarness harness{limsApp(), std::string{lims::screens::kCompanies}, RigMode::Local, {},
                          LocalSetup{.db = {}, .user = "tech"}};
    REQUIRE(harness.waitForText("companyCount", "1 companies"));
    REQUIRE(harness.waitForText("siteCount", "1 sites"));

    harness.editField("companyForm", "name", "Osaka branch");
    harness.chooseField("companyForm", "baseCurrency", R"("JPY")");
    harness.chooseField("companyForm", "fiscalYearStart", R"("April")");
    REQUIRE(harness.submitForm("companyForm"));
    CHECK(harness.waitForText("companyCount", "2 companies"));

    harness.editField("siteForm", "companyId", "1");
    harness.editField("siteForm", "name", "Field lab");
    REQUIRE(harness.submitForm("siteForm"));
    CHECK(harness.waitForText("siteCount", "2 sites"));
    reportProblems(harness);
    CHECK(harness.problems().empty());
}

// Mutations: drop changes<RoleAssignmentView> from AssignRole's effects (the count stays at 0); answer the last
// supervisor's revoke with success (no refusal is shown and the count drops).
TEST_CASE("lims org screens: the first supervisor assigns themselves, then a verifier, and the last supervisor stays",
          "[lims-screens][org]") {
    DbFixture fixture;
    ScreenHarness harness{limsApp(), std::string{lims::screens::kAccess}, RigMode::Local, {},
                          LocalSetup{.db = {}, .user = "tech"}};
    REQUIRE(harness.waitForText("assignmentCount", "0 role assignments"));

    harness.editField("assignForm", "principal", "tech");
    harness.chooseField("assignForm", "role", R"("Supervisor")");
    harness.editField("assignForm", "company", "1");
    REQUIRE(harness.submitForm("assignForm"));
    REQUIRE(harness.waitForText("assignmentCount", "1 role assignments"));
    CHECK(harness.waitForText("userCount", "1 users"));

    harness.editField("assignForm", "principal", "bob");
    harness.chooseField("assignForm", "role", R"("Verifier")");
    REQUIRE(harness.submitForm("assignForm"));
    REQUIRE(harness.waitForText("assignmentCount", "2 role assignments"));
    CHECK(harness.waitForText("userCount", "2 users"));

    // The first row is tech's supervisor assignment, the only one: revoking it is refused, in words.
    harness.clickIn("assignments", "revoke");
    CHECK(harness.waitFor([&harness] {
        auto const text = harness.text("error");
        return text && text->find("last supervisor") != std::string::npos;
    }));
    CHECK(harness.waitForText("assignmentCount", "2 role assignments"));
}
```

In `examples/lims/screens/render_smoke/test_lims_render.cpp`, make the expected mounted list:
```cpp
    CHECK(report.mounted ==
          std::vector<std::string>{std::string{lims::screens::kRegister}, std::string{lims::screens::kLifecycle},
                                   std::string{lims::screens::kResults},   std::string{lims::screens::kSample},
                                   std::string{lims::screens::kIgnition},  std::string{lims::screens::kCompanies},
                                   std::string{lims::screens::kAccess},    std::string{lims::screens::kLogin}});
```

- [ ] **Step 2: Run the build to see it fail**

Run: `flock /tmp/claude-1000/-home-yaraslau-repo-morph/b1b73e49-c41d-4e27-bb68-b8c0cfacf145/scratchpad/morph-build.lock cmake --build build/linux-everything --target ladder_lims_screen_tests ladder_lims_render_smoke -j10`
Expected: FAIL — `no member named 'kCompanies' in namespace 'lims::screens'`.

- [ ] **Step 3: Write the screens**

In `examples/lims/screens/lims_screens.hpp`, after `kIgnition`:
```cpp
/// @brief Companies and their sites: the lists, and a form to create each.
inline constexpr std::string_view kCompanies = "lims.org.companies";

/// @brief Users and their role assignments: the lists, a form to assign a role, and a revoke per assignment.
inline constexpr std::string_view kAccess = "lims.org.access";
```

Create `examples/lims/screens/org_screens.hpp`:
```cpp
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <morph/ui/author.hpp>

/// @file
/// The org module's administration screens, registered by `lims::screens::bundle()`.

namespace lims::screens {

/// @brief Companies and sites: what the caller can see of each, and a form to create one.
/// @return The screen.
[[nodiscard]] morph::ui::author::Screen companiesScreen();

/// @brief Users and role assignments: what the caller can see of each, a form to assign a role, and a revoke per
///        assignment.
/// @return The screen.
[[nodiscard]] morph::ui::author::Screen accessScreen();

}  // namespace lims::screens
```

Create `examples/lims/screens/org_screens.cpp`:
```cpp
// SPDX-License-Identifier: Apache-2.0

#include "org_screens.hpp"

#include <string>
#include <vector>

#include "lims/org/dto/company_dto.hpp"
#include "lims/org/dto/role_dto.hpp"
#include "lims/org/models/org_model.hpp"
#include "lims_screens.hpp"

namespace lims::screens {

namespace ui = morph::ui::author;

namespace {

/// @brief A cell showing one member of its row.
/// @param member The member's wire name.
/// @return The cell.
[[nodiscard]] ui::Node cell(std::string const& member) { return ui::text({.text = ui::row(member)}); }

}  // namespace

ui::Screen companiesScreen() {
    return ui::screen(std::string{kCompanies}, [](ui::Scope& screen) {
        screen.title(ui::t("lims.org.companies.title", "Companies and sites"));
        auto const org = screen.model<OrgModel>("org");
        auto const companies = screen.query<ListCompanies>("companies", org);
        auto const sites = screen.query<ListSites>("sites", org);
        auto const companyRows = companies.value().at(&ListCompaniesResult::companies);
        auto const siteRows = sites.value().at(&ListSitesResult::sites);
        return ui::column(
            {ui::text({.text = ui::coalesce({companies.error(), sites.error()}), .role = std::string{"error"}})
                 .testId("error"),
             ui::panel(
                 {ui::text({.text = ui::format("{0} companies", {ui::count(companyRows)})}).testId("companyCount"),
                  ui::table(companyRows,
                            {.key = ui::row(&CompanyView::id),
                             .columns = {{.name = "name", .label = "Name", .cell = cell("name")},
                                         {.name = "baseCurrency", .label = "Base currency", .cell = cell("baseCurrency")},
                                         {.name = "fiscalYearStart",
                                          .label = "Fiscal year starts",
                                          .cell = cell("fiscalYearStart")}}})
                      .a11y("Companies")
                      .testId("companies"),
                  ui::form<CreateCompany>(org, {.presentation = {.submitLabel = "Create company", .showStatus = true}})
                      .id(ui::element("companyForm"))
                      .testId("companyForm")},
                 {.title = ui::t("lims.org.companies", "Companies"), .padding = 2}),
             ui::panel(
                 {ui::text({.text = ui::format("{0} sites", {ui::count(siteRows)})}).testId("siteCount"),
                  ui::table(siteRows,
                            {.key = ui::row(&SiteView::id),
                             .columns = {{.name = "name", .label = "Name", .cell = cell("name")},
                                         {.name = "companyId", .label = "Company", .cell = cell("companyId")}}})
                      .a11y("Sites")
                      .testId("sites"),
                  ui::form<CreateSite>(org, {.presentation = {.submitLabel = "Create site", .showStatus = true}})
                      .id(ui::element("siteForm"))
                      .testId("siteForm")},
                 {.title = ui::t("lims.org.sites", "Sites"), .padding = 2})},
            {.gap = 3});
    });
}

ui::Screen accessScreen() {
    return ui::screen(std::string{kAccess}, [](ui::Scope& screen) {
        screen.title(ui::t("lims.org.access.title", "Users and roles"));
        auto const org = screen.model<OrgModel>("org");
        auto const users = screen.query<ListUsers>("users", org);
        auto const assignments = screen.query<ListRoleAssignments>("assignments", org);
        auto const revoke = screen.mutation<RevokeRole>("revoke", org, {.concurrency = ui::Mode::Serial});
        auto const userRows = users.value().at(&ListUsersResult::users);
        auto const assignmentRows = assignments.value().at(&ListRoleAssignmentsResult::assignments);
        return ui::column(
            {ui::text({.text = ui::coalesce({revoke.error(), users.error(), assignments.error()}),
                       .role = std::string{"error"}})
                 .testId("error"),
             ui::panel({ui::text({.text = ui::format("{0} users", {ui::count(userRows)})}).testId("userCount"),
                        ui::table(userRows, {.key = ui::row("id"),
                                             .columns = {{.name = "id", .label = "User", .cell = cell("id")},
                                                         {.name = "roles", .label = "Roles", .cell = cell("roles")}}})
                            .a11y("Users")
                            .testId("users")},
                       {.title = ui::t("lims.org.users", "Users"), .padding = 2}),
             ui::panel(
                 {ui::text({.text = ui::format("{0} role assignments", {ui::count(assignmentRows)})})
                      .testId("assignmentCount"),
                  ui::table(assignmentRows, {.key = ui::row(&RoleAssignmentView::id)},
                            [&](ui::DeclScope& /*row*/) {
                                return std::vector<ui::TableColumn>{
                                    {.name = "principal", .label = "User", .cell = cell("principal")},
                                    {.name = "role", .label = "Role", .cell = cell("role")},
                                    {.name = "scopeKind", .label = "Scope", .cell = cell("scopeKind")},
                                    {.name = "company", .label = "Company", .cell = cell("company")},
                                    {.name = "site", .label = "Site", .cell = cell("site")},
                                    {.name = "revoke",
                                     .label = "",
                                     .cell = ui::button({.label = "Revoke",
                                                         .onClick = ui::run(revoke, {{&RevokeRole::assignmentId,
                                                                                      ui::row(&RoleAssignmentView::id)}})})
                                                 .testId("revoke")},
                                };
                            })
                      .a11y("Role assignments")
                      .testId("assignments"),
                  ui::form<AssignRole>(org, {.presentation = {.submitLabel = "Assign role", .showStatus = true}})
                      .id(ui::element("assignForm"))
                      .testId("assignForm")},
                 {.title = ui::t("lims.org.assignments", "Role assignments"), .padding = 2})},
            {.gap = 3});
    });
}

}  // namespace lims::screens
```

In `examples/lims/screens/lims_screens.cpp`, add `#include "org_screens.hpp"` and, in `bundle()`, after the `kIgnition` entry:
```cpp
    listed.push_back({.id = std::string{kCompanies},
                      .title = "Companies and sites",
                      .registered = screens.add(companiesScreen())});
    listed.push_back(
        {.id = std::string{kAccess}, .title = "Users and roles", .registered = screens.add(accessScreen())});
```

- [ ] **Step 4: Build and run**

Run:
```
flock /tmp/claude-1000/-home-yaraslau-repo-morph/b1b73e49-c41d-4e27-bb68-b8c0cfacf145/scratchpad/morph-build.lock cmake --build build/linux-everything --target ladder_lims_tests ladder_lims_screen_tests ladder_lims_render_smoke ladder_lims_server morph_examples_client -j10
flock /tmp/claude-1000/-home-yaraslau-repo-morph/b1b73e49-c41d-4e27-bb68-b8c0cfacf145/scratchpad/morph-build.lock env QT_QPA_PLATFORM=offscreen timeout 300 build/linux-everything/ladder_lims_screen_tests "[org]"
flock /tmp/claude-1000/-home-yaraslau-repo-morph/b1b73e49-c41d-4e27-bb68-b8c0cfacf145/scratchpad/morph-build.lock ctest --test-dir build/linux-everything -j6 -L ladder-lims --output-on-failure
```
Expected: both `[org]` screen cases pass; ctest `100% tests passed`, including `lims_bundle_loads_without_warnings` (every new mutation declares its effects) and the render smoke with eight mounted screens. Then apply each `// Mutation:` in `test_lims_org_screens.cpp`, rebuild `ladder_lims_screen_tests`, confirm the named case fails, revert.

- [ ] **Step 5: Look at the screens**

Run the generic client over a scratch database and open both screens, as a person would:
```
flock /tmp/claude-1000/-home-yaraslau-repo-morph/b1b73e49-c41d-4e27-bb68-b8c0cfacf145/scratchpad/morph-build.lock timeout 300 build/linux-everything/examples/client/morph_examples_client --app lims --db /tmp/claude-1000/-home-yaraslau-repo-morph/b1b73e49-c41d-4e27-bb68-b8c0cfacf145/scratchpad/plan_a_lims.db
```
Expected: "Companies and sites" and "Users and roles" are in the catalog; assigning yourself `Supervisor` at company 1 from the form lists the seeded company and site on the other screen; a refused action shows its reason in words. These screens are functional stand-ins: Part D rebuilds them against `examples/lims/design/mockups/`.

- [ ] **Step 6: Record the decision and commit**

Append to `examples/lims/README.md`'s resolved decisions:
```markdown
### 26. The organisation is administered from two screens

`lims.org.companies` lists the companies and sites the viewer can see and creates either; `lims.org.access` lists
the users and role assignments the viewer can see, assigns a role and revokes one. Every form is its action's schema
and every list refreshes from the declared effects of the action that changed it, so neither screen says what to
refresh. A user is a principal holding an assignment: lims keeps no user store, and whoever signs in is a principal.
The screens are functional, not finished; the production GUI rebuilds them against the org mockups.
```
and add both screens to the README's "The client" list:
```markdown
- **`lims.org.companies`** lists the companies and sites the viewer's roles reach and creates either through its
  action's form.
- **`lims.org.access`** lists the users and role assignments the viewer's roles reach, assigns a role through
  `AssignRole`'s form (the first assignment on a lab with no supervisor needs none) and revokes one per row.
```
Then:
```bash
cd /home/yaraslau/repo/morph
git add -A examples/lims ':!examples/lims/design'
git commit -m "Add lims administration screens for companies, sites and roles

Two ui/1 screens over OrgModel: companies and sites, and users and their
role assignments, each list refreshing from the declared effects.

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01No1NznqxEBvjhZBaBRLCd6"
```

---

### Task 7: Part A's definition of done, measured

**Files:**
- Modify: `examples/lims/README.md` (Status line, "What to implement" model list), `codecov.yml` (lims component target if the measured ceiling moved)

**Interfaces:**
- Consumes: everything above. Produces: nothing new in code.

- [ ] **Step 1: Full rung run and the boundary**

Run:
```
flock /tmp/claude-1000/-home-yaraslau-repo-morph/b1b73e49-c41d-4e27-bb68-b8c0cfacf145/scratchpad/morph-build.lock cmake --preset linux-everything
flock /tmp/claude-1000/-home-yaraslau-repo-morph/b1b73e49-c41d-4e27-bb68-b8c0cfacf145/scratchpad/morph-build.lock cmake --build build/linux-everything --target ladder_lims_tests ladder_lims_screen_tests ladder_lims_render_smoke ladder_lims_server morph_examples_client -j10
flock /tmp/claude-1000/-home-yaraslau-repo-morph/b1b73e49-c41d-4e27-bb68-b8c0cfacf145/scratchpad/morph-build.lock ctest --test-dir build/linux-everything -j6 -L ladder-lims --output-on-failure
bash scripts/check_ctest_name_collisions.sh build/linux-everything
```
Expected: `100% tests passed`; the collision check reports no duplicate names (the new `lims.module_boundary.*` names included).

- [ ] **Step 2: Measure model coverage**

Follow the memory note `coverage-local-repro.md` (`scripts/coverage.sh` over the `clang-coverage` preset) with every build wrapped in the lock at `-j10`, and read the `lims` component's covered/total from `llvm-cov export`'s JSON for `examples/lims/{lab,org}/src/models/**` and `examples/lims/{lab,org}/include/lims/*/models/**`. Expected: at or above the README's recorded 98.96%; if below, every uncovered line is either tested now or listed in decision 20's style with the reason it is unreachable, and `codecov.yml`'s `target` is set a margin below the measured ceiling.

- [ ] **Step 3: Update the README's status and commit**

In `examples/lims/README.md`, change the "What to implement" model sentence to name the models as they are: `SampleModel`, `AnalysisCatalogModel`, `IgnitionModel` and `LimsAuthModel` in the lab and org modules, and `OrgModel` (companies, sites, role assignments) in org; and add to the Status paragraph: "Part A of the ERP design (`docs/superpowers/specs/2026-11-09-lims-erp-design.md`) is built: modules, companies and sites, scoped roles, declared authorization, scoped lists and the administration screens (decisions 21–26)." Then:
```bash
cd /home/yaraslau/repo/morph
git add examples/lims/README.md codecov.yml
git commit -m "Record lims ERP Part A as built, with its measured coverage

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01No1NznqxEBvjhZBaBRLCd6"
```

---

## Staging of Parts B–D

Parts B, C and D are **not** detailed here. Each gets its own plan file once Part A lands, because their shape depends on hypothesis outcomes: whether hypothesis 1 (atomic work across models) is confirmed decides whether posting is two calls in sequence or a framework seam; hypothesis 2 decides the event-delivery wiring; 3, 4 and 5 decide whether leases, scheduling and approvals are built in lims or against a new seam; 6's second instance decides the declared-authorization seam. The lines below are the expected task list, for sizing only.

**Part B — Ledger and billing** (`docs/superpowers/plans/<date>-lims-erp-part-b.md`)
1. `lims/posting` shared module: `PostingRequest`/`PostingResult` DTOs only — `examples/lims/posting/include/lims/posting/`, `morph_add_rung(… SHARED_MODULES org posting)`.
2. Money: currency units as `UnitTraits` over `CurrencyCode`, half-up rounding helpers for posters — `examples/lims/org/include/lims/org/core/` (or `posting/`), `IMPLEMENTATION.md` (the stale `DecimalPlaces >= 1` floor).
3. Ledger chart of accounts and periods — `examples/lims/ledger/{include,src}/…` (`ledger_` tables, `LedgerModel`).
4. Journal entries: validation, balancing per currency and in company currency, reversal — `ledger/src/models/`, `ledger/include/lims/ledger/dto/`.
5. Exchange rates and the reports (trial balance, account ledger, balance as of) — `ledger/`; the 10⁷-line trial balance test (hypothesis 8).
6. Billing price lists per company with effective dates and client overrides — `examples/lims/billing/{include,src}/…`.
7. Domain events: lab's outbox rows for `lab.AnalysisPublished`; billing's idempotent consumer creating billable lines; wiring in `lims/app` — `lab/src/models/sample_model.cpp`, `billing/`, `app/` (hypothesis 2, instance 1).
8. Invoices: draft → submitted → approved → posted, four-eyes approval with value limits as `Authorization<ApproveInvoice>` (hypothesis 6, instance 2 — designs the seam or dispositions it), escalation — `billing/`, `org/include/lims/org/authorization/` or `include/morph` per the outcome (hypothesis 5, instance 1).
9. Invoice edit lease and its lapse on disconnect — `billing/`, `app/` (hypothesis 3, instance 1).
10. Posting an invoice: gap-free numbering, `PostingRequest` to ledger, failure injection — `billing/`, `ledger/`, `app/` (hypothesis 1, instance 1).
11. Scheduled billing run with progress and resume after restart — `billing/`, `app/`, `src/server/main.cpp` (hypothesis 4, instance 1).
12. Two-company effects test — `tests/` (hypothesis 7, instance 1).

**Part C — Inventory and procurement** (`docs/superpowers/plans/<date>-lims-erp-part-c.md`)
1. Items with stock units, locations at a site, lots with expiry — `examples/lims/inventory/{include,src}/…`.
2. Requisitions with value-limit approval and escalation — `inventory/` (hypotheses 5 and 6, second instances).
3. Purchase orders to a supplier in its currency — `inventory/` (hypothesis 8, runtime currency).
4. Goods receipt: lots plus the GR/GRNI posting, atomic with the receipt — `inventory/`, `ledger/`, `app/` (hypothesis 1, instance 2).
5. Reagent consumption per analysis definition; `lab.ResultCaptured` consumer, first-expiring lot, cost posting — `lab/`, `inventory/`, `app/` (hypothesis 2, instance 2).
6. Lot reservation per sample, its race and its lapse — `inventory/`, `lab/` (hypothesis 3, instance 2).
7. Stock-movement list over 10⁶ rows — `inventory/`, `include/morph/table/` per the outcome (hypothesis 10).

**Part D — The production GUI** (`docs/superpowers/plans/<date>-lims-erp-part-d.md`; its mockups are made separately in `examples/lims/design/mockups/` and are not planned here)
1. Amend `IMPLEMENTATION.md` rule 2 and `LADDER.md`'s "no rung is a showcase" for lims — `examples/IMPLEMENTATION.md`, `examples/LADDER.md`.
2. Application shell: module navigation, global search, approvals inbox, company and site switcher — `examples/lims/screens/`, `include/morph/ui/` for vocabulary gaps.
3. Lab screens adapted from the gallery mockups — `examples/lims/screens/lims_screens.cpp`.
4. Org screens rebuilt against their mockups — `examples/lims/screens/org_screens.cpp`.
5. Billing screens including the invoice editor and print view — `examples/lims/screens/` (hypothesis 9).
6. Ledger screens — `examples/lims/screens/`.
7. Inventory screens including the goods-receipt dialog — `examples/lims/screens/` (hypothesis 9).
8. Production criteria across screens: states, locale (English, German), 10,000-row lists, keyboard, width classes — `examples/lims/screens/`, `src/qt_quick/`, `include/morph/ui/`.
9. Side-by-side grabs against the mockups on Qt Quick, and the terminal frontend pass — `scripts/shoot_screens.sh`, `examples/lims/screens/render_smoke/`.

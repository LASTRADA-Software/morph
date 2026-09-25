#!/usr/bin/env bash
# Usage: bash scripts/check_forms_qml_install.sh [REPO_ROOT]
#
# Fails unless an application outside the tree can render a DynamicForm from
# the *installed* MorphForms QML module: `find_package(morph CONFIG REQUIRED
# COMPONENTS forms_qml qt_forms)`, link `morph::forms_qmlplugin`, `import
# MorphForms`, instantiate a form.
#
# The sibling check_install_export.sh stops at C++ headers, and Qt is not on
# its job. This one needs Qt 6.5+ (found the usual way: Qt6_DIR or
# CMAKE_PREFIX_PATH in the environment) and measures the part a header check
# cannot see: a static QML module is a backing library, a plugin *and* the
# object libraries carrying its compiled resources and static initialiser.
# Leave the last out and everything links -- the module is then simply not
# there at run time. So the consumer is run, not just built, and a second
# consumer that links the backing library without the plugin must fail at run
# time, or this check is not measuring the plugin at all.
set -euo pipefail

repo_root="${1:-$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)}"
repo_root="$(cd "$repo_root" && pwd)"
if command -v cygpath >/dev/null 2>&1; then
    repo_root="$(cygpath -m "$repo_root")"
fi
readonly repo_root

failures=0
note() { printf 'ok: %s\n' "$*"; }
fail() { printf 'error: %s\n' "$*" >&2; failures=$((failures + 1)); }

run_step() {
    local description="$1"; shift
    local output
    if output="$("$@" 2>&1)"; then
        return 0
    fi
    fail "${description}"
    printf '%s\n' "$output" >&2
    return 1
}

finish() {
    if [ "$failures" -ne 0 ]; then
        printf '\n%d forms QML install check(s) failed.\n' "$failures" >&2
        exit 1
    fi
}

workspace="$(mktemp -d)"
trap 'rm -rf "$workspace" || true' EXIT
# Git Bash on Windows: hand CMake native paths, not /tmp/... ones.
if command -v cygpath >/dev/null 2>&1; then
    workspace="$(cygpath -m "$workspace")"
fi
readonly build_dir="${workspace}/build"
readonly prefix="${workspace}/prefix"

generator_args=()
if command -v ninja >/dev/null 2>&1; then
    generator_args=(-G Ninja)
fi

# ── 1. Build and install the module ─────────────────────────────────────────
run_step "the library did not configure with MORPH_BUILD_FORMS_QML=ON" \
    cmake -S "$repo_root" -B "$build_dir" ${generator_args[@]+"${generator_args[@]}"} \
        -DCMAKE_BUILD_TYPE=Release \
        -DMORPH_BUILD_TESTS=OFF \
        -DMORPH_BUILD_EXAMPLES=OFF \
        -DMORPH_BUILD_FORMS_QML=ON || finish
run_step "the forms QML module did not build" cmake --build "$build_dir" || finish
run_step "cmake --install failed" cmake --install "$build_dir" --prefix "$prefix" || finish

# ── 2. The prefix must hold the module, not just morph ──────────────────────
libdir="lib"
if [ ! -f "${prefix}/lib/cmake/morph/morphConfig.cmake" ] && [ -d "${prefix}/lib64/cmake/morph" ]; then
    libdir="lib64"
fi
for required in \
    "${libdir}/cmake/morph/morphTargets.cmake" \
    "${libdir}/qml/MorphForms/qmldir" \
    "${libdir}/qml/MorphForms/morph_forms_module.qmltypes" \
    "${libdir}/qml/MorphForms/qml/DynamicForm.qml" \
    "include/morph/qt/forms/forms_controller_core.hpp" \
    "include/morph/qt/qt_executor.hpp"; do
    if [ ! -f "${prefix}/${required}" ]; then
        fail "the install has no ${required}"
    fi
done

readonly targets_file="${prefix}/${libdir}/cmake/morph/morphTargets.cmake"
if [ -f "$targets_file" ]; then
    for exported in morph::forms_qml morph::forms_qmlplugin; do
        grep -q "^add_library(${exported} " "$targets_file" || fail "${exported} is not exported"
    done
    if grep -q '^add_library(morph::morph_' "$targets_file"; then
        fail "a target is exported as '$(grep -o 'morph::morph_[a-z0-9_]*' "$targets_file" | head -1)' -- give it an EXPORT_NAME"
    fi
fi
readonly qmldir_file="${prefix}/${libdir}/qml/MorphForms/qmldir"
if [ -f "$qmldir_file" ] && ! grep -q '^linktarget morph::forms_qmlplugin$' "$qmldir_file"; then
    fail "the installed qmldir does not name the exported plugin target: $(grep '^linktarget' "$qmldir_file" || echo 'no linktarget line')"
fi
finish
note "the prefix holds the MorphForms module, its plugin and the controller core headers"

# ── 3. A consumer renders a form from the installed module ──────────────────
write_consumer() {
    local dir="$1" link="$2"
    mkdir -p "$dir"
    cat > "${dir}/CMakeLists.txt" <<EOF
cmake_minimum_required(VERSION 3.25)
project(morph_forms_qml_consumer LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
find_package(Qt6 6.5 REQUIRED COMPONENTS Core Gui Qml Quick)
qt_standard_project_setup(REQUIRES 6.5)
find_package(morph CONFIG REQUIRED COMPONENTS forms_qml qt_forms)
qt_add_executable(consumer main.cpp)
target_link_libraries(consumer PRIVATE ${link} morph::qt_forms Qt6::Quick)
EOF
    cat > "${dir}/main.cpp" <<'EOF'
#include <morph/qt/forms/forms_controller_core.hpp>

#include <QGuiApplication>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QVariant>
#include <cstdio>
#include <memory>

int main(int argc, char** argv) {
    QGuiApplication app(argc, argv);
    QQmlEngine engine;
    QQmlComponent component(&engine);
    component.setData(R"(
        import QtQuick
        import MorphForms
        DynamicForm {
            actionType: "Probe"
            controller: null
            slotRegistry: SlotRegistry {}
            schema: ({ properties: { n: { type: "integer", "x-order": 0 } }, required: ["n"] })
            property int fieldCount: fields.length
        }
    )", QUrl{});
    std::unique_ptr<QObject> form{component.create()};
    if (!form) {
        std::fprintf(stderr, "%s\n", qPrintable(component.errorString()));
        return 1;
    }
    int const fieldCount = form->property("fieldCount").toInt();
    std::printf("DynamicForm from the installed MorphForms module: %d field(s)\n", fieldCount);
    return fieldCount == 1 ? 0 : 2;
}
EOF
}

build_consumer() {
    local dir="$1"
    cmake -S "$dir" -B "${dir}/build" ${generator_args[@]+"${generator_args[@]}"} \
        -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="$prefix" &&
        cmake --build "${dir}/build"
}

readonly consumer="${workspace}/consumer"
write_consumer "$consumer" morph::forms_qmlplugin
if run_step "a consumer linking morph::forms_qmlplugin did not build" build_consumer "$consumer"; then
    if output="$(QT_QPA_PLATFORM=offscreen "${consumer}/build/consumer" 2>&1)"; then
        note "$(printf '%s\n' "$output" | grep 'DynamicForm from the installed' || echo 'consumer ran')"
    else
        fail "the consumer built but could not instantiate DynamicForm from the installed module"
        printf '%s\n' "$output" >&2
    fi
fi

# ── 4. Vacuity guard: without the plugin, the module must be missing ────────
readonly unplugged="${workspace}/unplugged"
write_consumer "$unplugged" morph::forms_qml
if run_step "a consumer linking only morph::forms_qml did not build" build_consumer "$unplugged"; then
    if QT_QPA_PLATFORM=offscreen "${unplugged}/build/consumer" >/dev/null 2>&1; then
        fail "a consumer without morph::forms_qmlplugin still loaded MorphForms, so step 3 did not measure the plugin"
    else
        note "without the plugin the module is not found, so step 3 measured it"
    fi
fi

finish
note "an application renders DynamicForm from the installed MorphForms module"

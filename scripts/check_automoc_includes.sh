#!/usr/bin/env bash
# Usage: bash scripts/check_automoc_includes.sh [DIR...]
#
# Fails if any moc-generated source includes its class's header by a path that
# climbs out of its own directory -- see issue #372.
#
# moc writes the include for the header it was run on. Left to itself it writes
# a path relative to the generated file, and since the generated file lives
# deep inside the build tree that path is a long run of "..":
#
#     #include "../../../../../../examples/ledger/gui_lib/budget_presenter.hpp"
#
# A quoted include is resolved against the including file's own directory *and*
# against every -I entry, so the compiler attempts that same climb from each of
# the target's include directories too. Whether the escaped path resolves to a
# second, different file of the same name is arithmetic on how deep the
# checkout happens to sit inside its parent directories -- nothing about the
# code. When it does resolve, Clang reports
#
#     error: multiple candidates for header '...' found; ... ignoring others
#     including '.../examples/<rung>/include' [-Werror,-Wshadow-header]
#
# and the project's -Werror turns every AUTOMOC target into a build failure.
# That is not hypothetical: a git worktree under .claude/worktrees/<name>/
# (where the agent harness puts them) sits exactly six levels inside the
# checkout it was made from, which is exactly the climb, so every
# ladder_<rung>_gui_lib -- and every ladder_<rung>_tests that links one --
# stopped building at all.
#
# cmake/compiler_options.cmake sets CMAKE_AUTOMOC_PATH_PREFIX so moc emits the
# header path relative to the include directory it was found under
# ("budget_presenter.hpp", "ledger/app/app.hpp") instead, which resolves
# through the target's own -I set and cannot ascend. This gate asserts that it
# stayed that way: the failure it guards against is silent everywhere the
# arithmetic happens not to land, so a plain CI checkout would build green for
# years while the defect sat in the generated output.
#
# Scans each given DIR recursively for moc output (moc_*.cpp and *.moc; the
# mocs_compilation.cpp aggregators are skipped -- they only include files
# alongside them and carry none of moc's own header includes). Default DIR is
# "build", which covers every preset's binary directory.
#
# Exits 0 if every moc include is free of a ".." segment. Exits 1 if any
# ascends, printing one "file:line: include" diagnostic per offender, and also
# if no moc output was found at all -- a gate that scanned nothing must not
# report success (run it after a build configured with -DMORPH_BUILD_QT=ON).
set -euo pipefail

if [ "$#" -eq 0 ]; then
    dirs=("build")
else
    dirs=("$@")
fi

for dir in "${dirs[@]}"; do
    if [ ! -d "$dir" ]; then
        echo "error: $dir is not a directory" >&2
        exit 1
    fi
done

mapfile -d '' -t moc_files < <(
    find "${dirs[@]}" \( -name 'moc_*.cpp' -o -name '*.moc' \) -type f -print0
)

if [ "${#moc_files[@]}" -eq 0 ]; then
    echo "error: no moc-generated sources (moc_*.cpp, *.moc) found under: ${dirs[*]}" >&2
    echo "       This gate has nothing to check and must not report success. Build a" >&2
    echo "       tree configured with -DMORPH_BUILD_QT=ON first." >&2
    exit 1
fi

# A defect is a quoted include whose path carries a ".." segment. Angle-bracket
# includes (Qt's own headers) are never written this way and are not scanned.
offenders="$(
    grep -Hn '^#include[[:space:]]*"[^"]*"' "${moc_files[@]}" \
        | grep -E '"([^"]*/)?\.\./' \
        || true
)"

if [ -n "$offenders" ]; then
    printf '%s\n' "$offenders" >&2
    echo "" >&2
    echo "AUTOMOC include lint failed: the moc output above includes its header by a" >&2
    echo "path that climbs out of its own directory. That path is also resolved" >&2
    echo "against every -I entry, so it can pick up a same-named header from a" >&2
    echo "different checkout -- and Clang's -Wshadow-header makes it a hard error" >&2
    echo "under this project's -Werror (issue #372)." >&2
    echo "" >&2
    echo "cmake/compiler_options.cmake sets CMAKE_AUTOMOC_PATH_PREFIX to prevent" >&2
    echo "this; check that it is still set and still reaching the target that" >&2
    echo "produced the output above." >&2
    exit 1
fi

echo "AUTOMOC include lint OK: ${#moc_files[@]} generated moc source(s), none ascending."

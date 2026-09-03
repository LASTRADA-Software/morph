#!/usr/bin/env bash
# Usage: bash scripts/check_coverage_roots.sh [BUILD_DIR] [EXPORT_JSON]
#
# Fails if any file in the coverage mapping lies outside this checkout, so that
# a report cannot be silently computed over a subset of the tree.
#
# Why this gate exists (morph#426). A coverage build ran through the shared
# compiler cache, the cache served objects compiled in a *different* worktree,
# and clang had embedded that worktree's absolute source paths into their
# coverage mappings. scripts/coverage.sh filters by *relative* path
# (SOURCES=(include/morph), examples/<rung>/<sub>), so a record reading
# `/home/.../morph-wt/372/examples/crm/src/models/account_model.cpp` matches
# nothing named there and is dropped -- not mis-attributed, dropped. In the run
# that found this, 246 of 688 records were rooted in another worktree, and all
# eight of examples/crm/src/models/*.cpp appeared *only* under the foreign
# root, so the entire src side of codecov.yml's crm component was absent from
# the report while its tests ran and passed.
#
# The mechanism, demonstrated directly: compile one translation unit in
# worktree A through fastcache-cc, then compile the byte-identical unit in
# worktree B, and B's object carries A's paths --
#
#     SF:/home/.../T-tests-426-repro/wtA/lib.hpp     <- from B's build
#
# because a cache entry is keyed on content while the object it returns embeds
# an absolute path. Compiling the same unit with no launcher records B's own
# paths, which is what identifies the launcher as the cause.
#
# cmake/compiler_options.cmake now defaults USE_COMPILER_CACHE to OFF whenever
# AF_COVERAGE is ON, which removes the cause. This gate is the second half, and
# it is not redundant: the default is overridable (-DUSE_COMPILER_CACHE=ON is
# honoured, because on a single-checkout CI runner the cache is safe and worth
# having), the hazard returns the moment any cache is configured to key
# path-independently, and the failure it produces is a *silence* -- a report
# that shrinks while every command in the pipeline exits 0. That is the same
# failure mode scripts/coverage.sh's own comments record for morph#141 and
# morph#179, and it is the reason a derived list beat a hand-written one there.
#
# The check is on the *unfiltered* mapping, and it has to be: the filtered
# export contains only records that matched a relative SOURCES entry, so every
# path in it is under the checkout by construction and checking it would be
# vacuous. `-summary-only` gets the filename list without the per-region data,
# which keeps this to about two seconds over sixteen objects.
#
# One thing this gate deliberately does not fail on: llvm-cov's
# `N functions have mismatched data` warning, which the unfiltered export
# prints on stderr. It was read as evidence of this defect when the defect was
# first reported, and it is not. Measured in this tree, whose mapping contains
# zero foreign paths, against the same profile with a growing -object list:
#
#     objects   1     2     4     8    16
#     warning   0   183   413   998  2184
#
# It is zero with one object and scales with the *number of objects*, because a
# header-only template library instantiates the same function differently in
# each binary and llvm-cov reconciles function records by (name, structural
# hash). That is a property of measuring several binaries at once, not of where
# their sources came from, so a nonzero count here says nothing about morph#426
# either way. What it does mean is that some records are discarded, which is
# why coverage.sh's figure is not simply the sum of its parts; morph#403's
# object list is what decides which binaries are in the sum.
#
# EXPORT_JSON (second argument) supplies `llvm-cov export -summary-only` output
# from a file instead of running llvm-cov, which is what
# scripts/test_check_coverage_roots.sh uses to exercise this gate without a
# build.
set -euo pipefail

build_dir="${1:-build/clang-coverage}"
export_json_file="${2:-}"
readonly build_dir export_json_file

readonly manifest="${build_dir}/coverage_objects.txt"
readonly profdata="${build_dir}/merged.profdata"

# The source root every recorded path must be under. `pwd -P` rather than $PWD:
# clang writes the physical path, so comparing against a path reached through a
# symlink would report every file as foreign.
readonly source_root="$(pwd -P)"

if [ -n "$export_json_file" ]; then
    export_json="$(cat "$export_json_file")"
else
    if [ ! -s "$manifest" ]; then
        echo "check_coverage_roots: no coverage object manifest at ${manifest}." >&2
        echo "  Configure with the clang-coverage preset and build first." >&2
        exit 1
    fi
    if [ ! -s "$profdata" ]; then
        echo "check_coverage_roots: no merged profile at ${profdata}." >&2
        exit 1
    fi

    SUFFIX="${CLANG_VERSION:+-${CLANG_VERSION}}"
    objects=()
    while IFS= read -r _object; do
        [ -n "$_object" ] || continue
        objects+=("$_object")
    done < "$manifest"

    object_args=()
    for _object in "${objects[@]:1}"; do
        object_args+=(-object "$_object")
    done

    # No -ignore-filename-regex and no positional source filters on purpose:
    # both of those are what a foreign path escapes through, so applying them
    # here would hide exactly what is being looked for.
    export_json="$("llvm-cov${SUFFIX}" export "${objects[0]}" \
        "${object_args[@]}" \
        -instr-profile="$profdata" \
        -summary-only)"
fi

printf '%s' "$export_json" | python3 -c '
import json, os, sys

source_root = os.path.realpath(sys.argv[1]) + os.sep
document = json.load(sys.stdin)

filenames = [f["filename"] for export in document["data"] for f in export["files"]]
if not filenames:
    print("check_coverage_roots: the coverage mapping names no files at all.",
          file=sys.stderr)
    print("  A gate that passes over an empty mapping is the defect it exists",
          file=sys.stderr)
    print("  to find, committed by the detector (morph#426).", file=sys.stderr)
    raise SystemExit(1)

foreign = sorted({f for f in filenames if not f.startswith(source_root)})
if foreign:
    print("check_coverage_roots: %d of %d files in the coverage mapping are not"
          % (len(foreign), len(filenames)), file=sys.stderr)
    print("  under %s:" % source_root.rstrip(os.sep), file=sys.stderr)
    for name in foreign[:20]:
        print("    %s" % name, file=sys.stderr)
    if len(foreign) > 20:
        print("    ... and %d more" % (len(foreign) - 20), file=sys.stderr)
    print("", file=sys.stderr)
    print("  These records cannot match coverage.sh'"'"'s relative source filters, so", file=sys.stderr)
    print("  they are dropped and the report is computed over what is left.", file=sys.stderr)
    print("  Their most likely origin is a compiler cache serving objects built", file=sys.stderr)
    print("  in another checkout (morph#426). Reconfigure the coverage build with", file=sys.stderr)
    print("  -DUSE_COMPILER_CACHE=OFF, or delete the build tree and rebuild.", file=sys.stderr)
    raise SystemExit(1)

print("check_coverage_roots: %d files, all under the checkout." % len(filenames))
' "$source_root"

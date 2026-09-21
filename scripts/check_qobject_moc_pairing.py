#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Usage: python3 scripts/check_qobject_moc_pairing.py [ROOT] [--self-test]

Fails if a tracked header declaring a Q_OBJECT (or another AUTOMOC macro) is
separated from everything that would make AUTOMOC run moc on it -- see #659.

ROOT defaults to the repository this script lives in. `--self-test` runs the
gate's own fixtures, including a mutation of this repository's real
examples/common/CMakeLists.txt, and checks nothing else.

## Why this gate exists

AUTOMOC finds a `Q_OBJECT` header two ways, and only two:

  1. the header sits next to a translation unit of the same basename, in the
     same directory (`presenter.hpp` beside `presenter.cpp`); or
  2. the header is named in a target's own source list, which is why
     `cmake/morph_add_rung.cmake` globs `include/*.hpp` into each rung
     library and why `examples/common/CMakeLists.txt` lists
     `testkit/fault_proxy.hpp`.

When neither holds, no moc output is generated. Nothing complains. The
translation unit compiles, the static library archives, and the defect
surfaces only at the first link that needs the vtable:

    libmorph_ladder_testkit.a(fault_proxy.cpp.o): in function
      `morph::ladder::testkit::FaultProxy::FaultProxy(QUrl, QObject*)':
      undefined reference to `vtable for morph::ladder::testkit::FaultProxy'
      undefined reference to `...::FaultProxy::staticMetaObject'
      undefined reference to `typeinfo for ...::FaultProxy'

That is late and it is wide. On #657 one such split turned six CI legs red at
once -- `Application ladder`, its ASan+UBSan twin, `Kanban / ThreadSanitizer`,
`Linux / all optional features` for both compilers, and `clang-coverage` --
with the fastest at 4m03s.

The repository already knew. `cmake/morph_add_rung.cmake` describes this exact
failure, names the case it was diagnosed on (`pastebin::app::App`, "hit the
moment `ladder_pastebin_tests` linked it") and explains the remedy. #652 then
walked into it anyway, in a different CMakeLists, because nothing reads a
comment in a file you are not editing. A known failure mode with no control is
one that recurs on schedule; this is the control.

## Why this is a text scan and not a build-tree scan

The issue that filed this expected a gate over a configured build tree, as
`scripts/check_automoc_includes.sh` is, and worried about scoping: a build
tree only covers what the configure enabled, so "every `Q_OBJECT` header must
have moc output" would false-positive on everything behind an off-by-default
option -- the WASM shells, bank's GUI, every rung when
`MORPH_BUILD_LADDER=OFF`.

Checking the *pairing* instead of the *output* dissolves that. A header behind
an off-by-default option still has to be listed in its (conditionally added)
target, or sit beside its own .cpp; which options a given configure turned on
does not enter into it. So this gate needs no configure, no compiler and no Qt,
runs in well under a second, and lives in drift-guard.yml with the rest of the
fast dependency-free gates rather than behind the slow legs it is meant to
pre-empt.

## What it deliberately does NOT check

That the target whose source list names the header has AUTOMOC enabled. That
is a second way to get no moc output, and it has never happened here --
`morph_qt_impl` sets `AUTOMOC ON` explicitly and every Qt target the ladder
builds goes through `qt_add_*` or `morph_add_rung()`. Checking it means
resolving CMake target properties, which means a configure, which is the cost
this gate exists to avoid. A `FILE_SET HEADERS` entry is the one case of "named
but not scanned" that is cheap to recognise, and it is excluded below.

## The two vacuity traps, and how each is closed

A gate reporting "0 unmocced Q_OBJECT headers" is satisfied just as well by a
walk that found no headers at all, and this tree is clean today, so the gate
ships already green and would never announce a broken scan on its own.

  1. It reports what it examined -- headers walked, headers carrying an
     AUTOMOC macro, and how each covered one was covered -- and exits 1 if the
     macro-bearing set is empty. A scan that stops recognising `Q_OBJECT` is
     then a failure, not a pass.

  2. `--self-test` drives the mutation the issue named as its close condition:
     `testkit/fault_proxy.hpp` removed from `morph_ladder_testkit`'s source
     list in this repository's own `examples/common/CMakeLists.txt`. That is
     the exact #652 regression, replayed against the real file, and the gate
     must report it. Four synthetic fixtures cover the arms that mutation does
     not reach.

Exits 0 when every macro-bearing header is covered, printing the counts. Exits
1 naming each uncovered header, and also when the scan found no macro-bearing
header at all or when a coverage mechanism it credits has disappeared.
"""

import argparse
import os
import pathlib
import re
import shutil
import subprocess
import sys
import tempfile

# The macros that make AUTOMOC run moc on a header. Anchored at the start of a
# line so that prose naming one -- this file's own docstring, or a CMake
# comment -- is not mistaken for a declaration.
AUTOMOC_MACRO = re.compile(
    r"^[ \t]*(Q_OBJECT|Q_GADGET|Q_GADGET_EXPORT|Q_NAMESPACE|Q_NAMESPACE_EXPORT|Q_ENUM_NS)\b",
    re.M,
)

HEADER_SUFFIXES = (".h", ".hh", ".hpp", ".hxx", ".h++", ".hm")
SOURCE_SUFFIXES = (".cpp", ".cc", ".cxx", ".c++", ".mm", ".m", ".C")

# Commands whose arguments are a target's own source list -- the second of
# AUTOMOC's two ways of finding a header. `target_sources` is here for the
# `PRIVATE`/`PUBLIC` form; its `FILE_SET HEADERS` form is excluded per
# argument, below, because an installed-header set is not an AUTOMOC scan.
SOURCE_LIST_COMMANDS = {
    "add_library",
    "add_executable",
    "target_sources",
    "qt_add_executable",
    "qt_add_library",
    "qt6_add_executable",
    "qt6_add_library",
    "qt_add_qml_module",
    "qt6_add_qml_module",
}

# Substituted before a listed path is resolved. Anything still carrying a `${`
# after this is a variable this scan cannot follow, and is skipped -- see
# `unresolved` in audit(), which reports how many there were so that a listing
# this gate cannot see is visible rather than silently absent.
ROOT_VARIABLES = ("CMAKE_SOURCE_DIR", "PROJECT_SOURCE_DIR", "morph_SOURCE_DIR")
DIR_VARIABLES = ("CMAKE_CURRENT_SOURCE_DIR", "CMAKE_CURRENT_LIST_DIR")


def strip_cmake_comments(text: str) -> str:
    """Blank out `#` comments, leaving quoted `#` alone and line count intact.

    The comment half matters as much as the command half: examples/common's
    CMakeLists names `fault_proxy.hpp` five times in the paragraph explaining
    why it is listed, and a scan that counted those would report the header
    covered by the prose that describes the coverage.
    """
    out = []
    in_string = False
    index = 0
    while index < len(text):
        char = text[index]
        if in_string:
            if char == "\\" and index + 1 < len(text):
                out.append(text[index : index + 2])
                index += 2
                continue
            if char == '"':
                in_string = False
            out.append(char)
        elif char == '"':
            in_string = True
            out.append(char)
        elif char == "#":
            while index < len(text) and text[index] != "\n":
                out.append(" ")
                index += 1
            continue
        else:
            out.append(char)
        index += 1
    return "".join(out)


def cmake_commands(text: str):
    """Yield (name, argument-text) for every command invocation in `text`."""
    cleaned = strip_cmake_comments(text)
    for match in re.finditer(r"([A-Za-z_][A-Za-z0-9_]*)[ \t\r\n]*\(", cleaned):
        name = match.group(1).lower()
        depth = 1
        index = match.end()
        in_string = False
        while index < len(cleaned) and depth:
            char = cleaned[index]
            if in_string:
                if char == "\\":
                    index += 2
                    continue
                if char == '"':
                    in_string = False
            elif char == '"':
                in_string = True
            elif char == "(":
                depth += 1
            elif char == ")":
                depth -= 1
                if not depth:
                    break
            index += 1
        yield name, cleaned[match.end() : index]


def listed_headers(root: pathlib.Path, cmake_files):
    """Header paths named in a target's source list, repo-relative.

    Returns (covered, unresolved): the set of paths, and the count of listed
    header tokens that carry a variable this scan cannot expand.
    """
    covered = set()
    unresolved = 0
    for cmake_file in cmake_files:
        directory = (root / cmake_file).parent
        text = (root / cmake_file).read_text(errors="replace")
        for name, arguments in cmake_commands(text):
            if name not in SOURCE_LIST_COMMANDS:
                continue
            # A FILE_SET is an install/interface header set, not an AUTOMOC
            # scan: morph_qt lists include/morph/qt/qt_websocket_server.hpp
            # in one, and the listing that actually drives moc for it is
            # morph_qt_impl's. Everything from the keyword to the end of the
            # command is dropped, which is where a FILE_SET's FILES live.
            cut = re.search(r"\bFILE_SET\b", arguments)
            if cut:
                arguments = arguments[: cut.start()]
            for token in re.findall(r'"[^"]*"|\S+', arguments):
                token = token.strip('"')
                if not token.lower().endswith(HEADER_SUFFIXES):
                    continue
                for variable in ROOT_VARIABLES:
                    token = token.replace("${" + variable + "}", str(root))
                for variable in DIR_VARIABLES:
                    token = token.replace("${" + variable + "}", str(directory))
                if "${" in token:
                    unresolved += 1
                    continue
                path = pathlib.Path(token)
                if not path.is_absolute():
                    path = directory / path
                try:
                    covered.add(path.resolve().relative_to(root.resolve()).as_posix())
                except ValueError:
                    # Outside the tree: not something this repository owns.
                    continue
    return covered, unresolved


def rung_glob_is_intact(root: pathlib.Path):
    """Whether morph_add_rung() still globs a rung's include/ into its library.

    Four of this tree's six split headers are covered by nothing else, so if
    that glob is ever dropped or renamed this gate must stop crediting it
    rather than keep passing them.
    """
    recipe = root / "cmake" / "morph_add_rung.cmake"
    if not recipe.is_file():
        return False, f"{recipe.relative_to(root)} does not exist"
    text = strip_cmake_comments(recipe.read_text(errors="replace"))
    glob = re.search(
        r"file\s*\(\s*GLOB_RECURSE\s+_lib_headers\b[^)]*include/\*\.hpp", text, re.S
    )
    if not glob:
        return False, (
            "cmake/morph_add_rung.cmake no longer contains a "
            "`file(GLOB_RECURSE _lib_headers ... include/*.hpp)`"
        )
    if not re.search(r"add_library\s*\([^)]*\$\{_lib_headers\}", text, re.S):
        return False, (
            "cmake/morph_add_rung.cmake globs _lib_headers but no add_library() "
            "call passes ${_lib_headers} as a source"
        )
    return True, "morph_add_rung() globs include/*.hpp into ladder_<rung>_lib"


def read_rungs(root: pathlib.Path):
    listing = root / "examples" / "rungs.txt"
    if not listing.is_file():
        return []
    return [
        line.strip()
        for line in listing.read_text(errors="replace").splitlines()
        if line.strip() and not line.lstrip().startswith("#")
    ]


def tracked_files(root: pathlib.Path):
    result = subprocess.run(
        ["git", "-C", str(root), "ls-files", "-z"],
        capture_output=True,
        text=True,
        check=True,
    )
    return [name for name in result.stdout.split("\0") if name]


def audit(root: pathlib.Path):
    """Returns (report-lines, uncovered, macro_headers, headers_walked)."""
    files = tracked_files(root)
    headers = [f for f in files if f.lower().endswith(HEADER_SUFFIXES)]
    sources = {f for f in files if f.endswith(SOURCE_SUFFIXES)}
    cmake_files = [
        f
        for f in files
        if pathlib.PurePosixPath(f).name == "CMakeLists.txt" or f.endswith(".cmake")
    ]

    macro_headers = []
    for header in headers:
        text = (root / header).read_text(errors="replace")
        if AUTOMOC_MACRO.search(text):
            macro_headers.append(header)

    covered_by_listing, unresolved = listed_headers(root, cmake_files)
    rungs = read_rungs(root)
    rung_prefixes = tuple(f"examples/{rung}/include/" for rung in rungs)

    paired = []
    listed = []
    globbed = []
    uncovered = []
    for header in macro_headers:
        path = pathlib.PurePosixPath(header)
        neighbours = [
            f"{path.parent.as_posix()}/{path.stem}{suffix}" for suffix in SOURCE_SUFFIXES
        ]
        if any(neighbour in sources for neighbour in neighbours):
            paired.append(header)
        elif header in covered_by_listing:
            listed.append(header)
        elif rung_prefixes and header.startswith(rung_prefixes):
            globbed.append(header)
        else:
            uncovered.append(header)

    report = [
        f"walked {len(headers)} tracked header(s) across {len(cmake_files)} CMake file(s)",
        f"{len(macro_headers)} carry an AUTOMOC macro:",
        f"    {len(paired)} paired with a same-directory translation unit",
        f"    {len(listed)} named in a target's source list",
        f"    {len(globbed)} under a ladder rung's include/, globbed by morph_add_rung()",
        f"    {len(uncovered)} with no moc pairing at all",
    ]
    if unresolved:
        report.append(
            f"note: {unresolved} listed header path(s) carry a CMake variable this "
            f"scan cannot expand and were not credited as coverage"
        )
    return report, uncovered, macro_headers, headers, globbed


def check(root: pathlib.Path) -> int:
    report, uncovered, macro_headers, headers, globbed = audit(root)
    errors = []

    if not headers:
        errors.append(
            f"no tracked headers found under {root} -- this gate has nothing to "
            f"check and must not report success"
        )
    elif not macro_headers:
        errors.append(
            f"no tracked header carries an AUTOMOC macro "
            f"({'|'.join(['Q_OBJECT', 'Q_GADGET', 'Q_NAMESPACE', '...'])}), across "
            f"{len(headers)} header(s) walked. Either this repository has stopped "
            f"declaring QObjects in headers, or this scan has stopped recognising "
            f"them. A gate that examined nothing must not report success."
        )

    if globbed:
        intact, why = rung_glob_is_intact(root)
        if not intact:
            errors.append(
                f"{len(globbed)} header(s) are credited to morph_add_rung()'s "
                f"include/ glob, but that mechanism is gone: {why}. Those headers "
                f"now get no moc output at all:\n"
                + "\n".join(f"    {header}" for header in globbed)
            )
        else:
            report.append(f"ok: {why}")

    for header in uncovered:
        errors.append(
            f"{header} declares an AUTOMOC macro but AUTOMOC will never see it: "
            f"no translation unit of the same basename sits beside it, and no "
            f"target's source list names it. Nothing will fail until the first "
            f"link that needs its vtable, in every leg at once. Either move the "
            f".cpp back next to the header, or list the header among its target's "
            f"sources the way examples/common/CMakeLists.txt lists "
            f"testkit/fault_proxy.hpp -- header entries are not compiled, they "
            f"only join the AUTOMOC scan."
        )

    for line in report:
        print(line)
    if errors:
        for error in errors:
            print(f"::error::{error}", file=sys.stderr)
        print(f"\n{len(errors)} problem(s). See #659.", file=sys.stderr)
        return 1
    print("Q_OBJECT moc-pairing lint OK.")
    return 0


# ── self-test ───────────────────────────────────────────────────────────────


def _fixture(directory: pathlib.Path, files: dict) -> pathlib.Path:
    for name, body in files.items():
        path = directory / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(body)
    subprocess.run(["git", "-C", str(directory), "init", "-q"], check=True)
    subprocess.run(["git", "-C", str(directory), "add", "-A"], check=True)
    return directory


HEADER_WITH_MACRO = """#pragma once
#include <QObject>
class Thing : public QObject {
    Q_OBJECT
public:
    Thing();
};
"""


def self_test(repo_root: pathlib.Path) -> int:
    failures = []

    def expect(name, root, wanted, must_name=None):
        code = check(root)
        if code != wanted:
            failures.append(f"{name}: expected exit {wanted}, got {code}")
            return
        if must_name is not None:
            _, uncovered, _, _, _ = audit(root)
            if must_name not in uncovered:
                failures.append(f"{name}: expected {must_name} to be reported, got {uncovered}")

    with tempfile.TemporaryDirectory() as raw:
        tmp = pathlib.Path(raw)

        # 1. Paired: header beside its own .cpp. The arm that covers 34 of this
        #    repository's 40 macro-bearing headers.
        print("\n--- fixture 1: header beside its own translation unit ---")
        expect(
            "paired",
            _fixture(
                tmp / "paired",
                {
                    "src/thing.hpp": HEADER_WITH_MACRO,
                    "src/thing.cpp": '#include "thing.hpp"\nThing::Thing() {}\n',
                    "CMakeLists.txt": "add_library(t STATIC src/thing.cpp)\n",
                },
            ),
            0,
        )

        # 2. Split but listed: the shape morph_ladder_testkit and morph_qt_impl
        #    both have.
        print("\n--- fixture 2: split header, named in the source list ---")
        expect(
            "listed",
            _fixture(
                tmp / "listed",
                {
                    "inc/thing.hpp": HEADER_WITH_MACRO,
                    "src/thing.cpp": '#include "thing.hpp"\nThing::Thing() {}\n',
                    "CMakeLists.txt": "add_library(t STATIC src/thing.cpp inc/thing.hpp)\n",
                },
            ),
            0,
        )

        # 3. Split and not listed: #652's regression, synthetically.
        print("\n--- fixture 3: split header, not listed (must fail) ---")
        expect(
            "split",
            _fixture(
                tmp / "split",
                {
                    "inc/thing.hpp": HEADER_WITH_MACRO,
                    "src/thing.cpp": '#include "thing.hpp"\nThing::Thing() {}\n',
                    "CMakeLists.txt": "add_library(t STATIC src/thing.cpp)\n",
                },
            ),
            1,
            "inc/thing.hpp",
        )

        # 4. Named only by a comment, and only in a FILE_SET. Neither makes
        #    AUTOMOC scan the header, and both are how a passing gate would be
        #    faked by accident.
        print("\n--- fixture 4: comment + FILE_SET are not coverage (must fail) ---")
        expect(
            "not-really-listed",
            _fixture(
                tmp / "weak",
                {
                    "inc/thing.hpp": HEADER_WITH_MACRO,
                    "src/thing.cpp": '#include "thing.hpp"\nThing::Thing() {}\n',
                    "CMakeLists.txt": (
                        "# inc/thing.hpp is a QObject, remember to moc it\n"
                        "add_library(t STATIC src/thing.cpp)\n"
                        "target_sources(t INTERFACE FILE_SET HEADERS BASE_DIRS inc "
                        "FILES inc/thing.hpp)\n"
                    ),
                },
            ),
            1,
            "inc/thing.hpp",
        )

        # 5. A tree with no macro-bearing header at all. The anti-vacuity floor:
        #    this is what a scan that stopped recognising Q_OBJECT looks like.
        print("\n--- fixture 5: nothing to examine (must fail) ---")
        expect(
            "vacuous",
            _fixture(
                tmp / "vacuous",
                {
                    "inc/plain.hpp": "#pragma once\nint plain();\n",
                    "CMakeLists.txt": "add_library(t INTERFACE)\n",
                },
            ),
            1,
        )

        # 6. The close condition from #659, against the real file: this
        #    repository's examples/common/, with testkit/fault_proxy.hpp
        #    removed from morph_ladder_testkit's source list. Copied rather
        #    than mutated in place so the working tree is never touched.
        print("\n--- fixture 6: the real examples/common/, unmutated ---")
        real = tmp / "real"
        (real / "examples").mkdir(parents=True)
        shutil.copytree(repo_root / "examples" / "common", real / "examples" / "common")
        _fixture(real, {})
        expect("real-tree", real, 0)

        print("\n--- fixture 6b: fault_proxy.hpp removed from the source list ---")
        lists = real / "examples" / "common" / "CMakeLists.txt"
        before = lists.read_text()
        after = before.replace("    testkit/fault_proxy.hpp\n", "", 1)
        if after == before:
            failures.append(
                "real-tree mutation: `    testkit/fault_proxy.hpp` is no longer a "
                "line of examples/common/CMakeLists.txt, so this self-test mutated "
                "nothing. Re-point it at wherever that header is listed now."
            )
        lists.write_text(after)
        subprocess.run(["git", "-C", str(real), "add", "-A"], check=True)
        expect("real-tree-mutated", real, 1, "examples/common/testkit/fault_proxy.hpp")

        # 7. The rung-glob credit, and what happens when the mechanism behind
        #    it goes. Four of this tree's macro-bearing headers are covered by
        #    nothing but morph_add_rung()'s include/ glob, and this gate cannot
        #    resolve that glob -- so it asserts the glob instead. Mutating the
        #    real recipe is what proves that assertion is not decorative.
        print("\n--- fixture 7: a rung header, credited to the real glob ---")
        rung = tmp / "rung"
        recipe_source = repo_root / "cmake" / "morph_add_rung.cmake"
        _fixture(
            rung,
            {
                "examples/rungs.txt": "demo\n",
                "examples/demo/include/demo/app/app.hpp": HEADER_WITH_MACRO,
                "examples/demo/src/app/app.cpp": '#include "demo/app/app.hpp"\nThing::Thing() {}\n',
                "CMakeLists.txt": "include(cmake/morph_add_rung.cmake)\n",
                "cmake/morph_add_rung.cmake": recipe_source.read_text(),
            },
        )
        expect("rung-globbed", rung, 0)

        print("\n--- fixture 7b: the glob removed from morph_add_rung.cmake ---")
        recipe = rung / "cmake" / "morph_add_rung.cmake"
        before = recipe.read_text()
        after = re.sub(
            r"file\(GLOB_RECURSE _lib_headers[^\n]*\n", "", before, count=1
        )
        if after == before:
            failures.append(
                "rung-glob mutation: cmake/morph_add_rung.cmake no longer contains a "
                "one-line `file(GLOB_RECURSE _lib_headers ...)`, so this self-test "
                "mutated nothing."
            )
        recipe.write_text(after)
        subprocess.run(["git", "-C", str(rung), "add", "-A"], check=True)
        expect("rung-glob-gone", rung, 1)

    if failures:
        for failure in failures:
            print(f"::error::self-test: {failure}", file=sys.stderr)
        print(f"\n{len(failures)} self-test failure(s).", file=sys.stderr)
        return 1
    print(
        "\nself-test OK: 9 fixture(s), including the #652 mutation of the real "
        "examples/common/CMakeLists.txt and a mutation of the real "
        "cmake/morph_add_rung.cmake."
    )
    return 0


def main(argv) -> int:
    parser = argparse.ArgumentParser(add_help=True)
    parser.add_argument("root", nargs="?", default=None)
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args(argv)

    default_root = pathlib.Path(__file__).resolve().parent.parent
    root = pathlib.Path(args.root).resolve() if args.root else default_root
    if args.self_test:
        return self_test(default_root)
    return check(root)


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))

#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Fail when a dependency's pin is written twice and the two copies disagree.

Usage:
    python3 scripts/check_dep_pins.py [REPO_ROOT]
    python3 scripts/check_dep_pins.py --self-test

Why this exists (morph#693)
---------------------------
Every FetchContent dependency in this tree states its revision twice: once as
`morph_cache_dep(<name> <url> <tag>)` and once as the `FetchContent_Declare()`
that follows it.

    morph_cache_dep(glaze https://github.com/stephenberry/glaze.git v7.4.0)
    FetchContent_Declare(
        glaze
        GIT_REPOSITORY https://github.com/stephenberry/glaze.git
        GIT_TAG        v7.4.0
        GIT_SHALLOW    TRUE
    )

Nothing compared the two. cmake/DepCache.cmake was written against exactly this
failure class and says so in its own words -- `tag` is part of the cache
directory name, so "bumping a pin lands in a fresh directory instead of silently
reusing the old revision -- the failure mode a cache keyed on name alone would
have, and the one that is hardest to notice because everything still builds" --
but it keys on the tag it is *handed*, not the tag FetchContent is handed. Split
across two calls, the failure mode returns one level up.

The divergence is asymmetric in the worst way. With a warm cache, the build gets
`morph_cache_dep`'s tag (FETCHCONTENT_SOURCE_DIR_<NAME> points at a tree already
checked out at it, and FetchContent skips the download); with no cache -- an
ordinary local build, which opts out by default -- it gets `GIT_TAG`'s. So a
mismatch compiles a different revision in CI than on the author's machine, with
no diagnostic on either side and both builds succeeding.

All five paired sites currently agree, so this is a preventive gate rather than
a bug fix, and its acceptance test is therefore not "it passes on master" -- it
would pass on master if it parsed nothing at all. It is "skew one pin and watch
it fail, naming both lines". See the self-test, and the commit that added this
file for the same thing done on the real tree.

The alternative this does not do
--------------------------------
`morph_cache_dep` could emit the `FetchContent_Declare` itself, so the pin
cannot be written twice -- removing the failure mode instead of detecting it.
That is a larger change to a load-bearing macro used by five call sites across
four files, and it is not precluded by this gate. The gate is the cheap first
step.

Scope, and the vacuity rules
----------------------------
Reads CMake text only; configures nothing, clones nothing. A `morph_cache_dep`
call this script cannot parse is an error rather than a skip, a
`FetchContent_Declare` of a git repository with no `morph_cache_dep` beside it
is an error (its pin is then guarded by nothing, and it clones on every
configure -- the volume morph#552 was about), and finding zero pairs at all is
an error. A gate with nothing left to check reports green exactly as loudly as
one that checked everything.
"""

from __future__ import annotations

import collections
import os
import re
import sys

# `morph_cache_dep(name repository tag)` on one line, which is how all five call
# sites are written. Anything else is reported rather than skipped; see
# UNPARSED_CALL_RE below.
CACHE_DEP_RE = re.compile(r"^\s*morph_cache_dep\(\s*([^\s()]+)\s+([^\s()]+)\s+([^\s()]+)\s*\)")
UNPARSED_CALL_RE = re.compile(r"^\s*morph_cache_dep\(")
DECLARE_RE = re.compile(r"^\s*FetchContent_Declare\(\s*([^\s()]*)")
ARGUMENT_RE = re.compile(r"^\s*(GIT_REPOSITORY|GIT_TAG)\s+(\S+)")

SKIP_DIRECTORIES = {".git", "build", "out", "vcpkg_installed", "_deps", "node_modules"}

# One FetchContent_Declare block: where it opens, and where each half of its pin
# is written. The two line numbers are the payload of a failure message -- an
# author who is told "these two lines disagree" fixes it without reading the
# parser.
Declared = collections.namedtuple(
    "Declared", "declare_line url url_line tag tag_line")

# The definition of the function itself, not a call site.
DEFINITION = "cmake/DepCache.cmake"


def cmake_files(repo_root):
    """Every CMakeLists.txt / *.cmake in the tree, minus build output."""
    found = []
    for dirpath, dirnames, filenames in os.walk(repo_root):
        dirnames[:] = sorted(name for name in dirnames
                             if name not in SKIP_DIRECTORIES
                             and not name.startswith("build"))
        for filename in sorted(filenames):
            if filename == "CMakeLists.txt" or filename.endswith(".cmake"):
                rel = os.path.relpath(os.path.join(dirpath, filename), repo_root)
                found.append(rel.replace(os.sep, "/"))
    return found


def is_comment(line):
    return line.lstrip().startswith("#")


def parse_file(lines):
    """({name: (line, url, tag)}, {name: Declared}, [unparsed lines]).

    First mapping is the morph_cache_dep() calls, second the
    FetchContent_Declare() blocks. A block's `url`/`tag` are None when it does
    not state them, and each carries the line it was stated on so a failure can
    name the two lines that disagree rather than the block they live in.
    """
    cached, declared, unparsed = {}, {}, []
    index = 0
    while index < len(lines):
        line = lines[index]
        if is_comment(line):
            index += 1
            continue

        match = CACHE_DEP_RE.match(line)
        if match:
            cached[match.group(1)] = (index + 1, match.group(2), match.group(3))
            index += 1
            continue
        if UNPARSED_CALL_RE.match(line):
            unparsed.append(index + 1)
            index += 1
            continue

        match = DECLARE_RE.match(line)
        if match:
            start = index + 1
            name = match.group(1)
            url = tag = None
            url_line = tag_line = start
            depth = line.count("(") - line.count(")")
            index += 1
            while index < len(lines) and depth > 0:
                body = lines[index]
                if not is_comment(body):
                    if not name and body.strip():
                        name = body.split()[0]
                    argument = ARGUMENT_RE.match(body)
                    if argument:
                        if argument.group(1) == "GIT_REPOSITORY":
                            url, url_line = argument.group(2), index + 1
                        else:
                            tag, tag_line = argument.group(2), index + 1
                    depth += body.count("(") - body.count(")")
                index += 1
            declared[name] = Declared(start, url, url_line, tag, tag_line)
            continue

        index += 1
    return cached, declared, unparsed


def check(repo_root, out=sys.stdout):
    failures = []
    pairs = 0
    checked = []

    for path in cmake_files(repo_root):
        if path == DEFINITION:
            continue
        with open(os.path.join(repo_root, path), encoding="utf-8") as handle:
            lines = handle.read().splitlines()
        if "morph_cache_dep" not in "\n".join(lines) and "FetchContent_Declare" not in "\n".join(lines):
            continue

        cached, declared, unparsed = parse_file(lines)

        for line in unparsed:
            failures.append(
                f"{path}:{line} calls morph_cache_dep() in a form this gate cannot "
                f"parse, so its pin is compared against nothing. Write it as "
                f"`morph_cache_dep(<name> <url> <tag>)` on one line, or teach this "
                f"script the new form -- do not leave it unchecked.")

        for name, block in sorted(declared.items()):
            if block.url is None and block.tag is None:
                continue  # not a git dependency: URL/SOURCE_DIR/SVN and friends
            if name not in cached:
                failures.append(
                    f"{path}:{block.declare_line} declares FetchContent dependency "
                    f"{name!r} with GIT_TAG {block.tag!r} and no morph_cache_dep() "
                    f"beside it. Its pin is then guarded by nothing and every configure "
                    f"clones it again (morph#552). Add the matching morph_cache_dep() "
                    f"call.")
                continue
            cache_line, cache_url, cache_tag = cached[name]
            pairs += 1
            agreed = True
            if block.tag is None:
                agreed = False
                failures.append(
                    f"{path}:{block.declare_line} declares {name!r} with a "
                    f"GIT_REPOSITORY and no GIT_TAG, while {path}:{cache_line} caches "
                    f"it at {cache_tag!r}. The cache would serve that revision and an "
                    f"uncached configure would take the default branch, which is the "
                    f"same divergence with one side unwritten.")
            elif block.tag != cache_tag:
                agreed = False
                failures.append(
                    f"{name}'s pin is stated twice and the two disagree: "
                    f"{path}:{cache_line} caches {cache_tag!r}, {path}:{block.tag_line} "
                    f"fetches GIT_TAG {block.tag!r}. A configure with a warm cache "
                    f"builds the first and one without builds the second, both "
                    f"successfully and with no diagnostic (morph#693).")
            if block.url is not None and block.url != cache_url:
                agreed = False
                failures.append(
                    f"{name}'s repository is stated twice and the two disagree: "
                    f"{path}:{cache_line} caches from {cache_url!r}, "
                    f"{path}:{block.url_line} fetches GIT_REPOSITORY {block.url!r}. "
                    f"The cache key names only the tag, so the cached tree would be "
                    f"served for the other repository's revision.")
            if agreed:
                checked.append((path, name, cache_tag))

        for name, (cache_line, _url, cache_tag) in sorted(cached.items()):
            if name not in declared:
                failures.append(
                    f"{path}:{cache_line} caches {name!r} at {cache_tag!r}, but nothing "
                    f"in this file declares it to FetchContent. Either the declaration "
                    f"moved -- and the cache call should follow it -- or this call "
                    f"populates a cache directory nothing ever reads.")

    for path, name, tag in checked:
        print(f"ok: {path}: {name} pinned at {tag} in both places", file=out)

    if pairs == 0 and not failures:
        failures.append(
            "no morph_cache_dep()/FetchContent_Declare() pair was found anywhere in "
            "the tree. Either every dependency stopped using the cache, or this "
            "gate's parsing has stopped matching how they are written -- and a gate "
            "with nothing left to check reports green exactly as loudly as one that "
            "checked everything.")

    if failures:
        print(f"\nerror: {len(failures)} dependency pin problem(s):\n", file=out)
        for failure in failures:
            print(f"  - {failure}", file=out)
        return 1

    print(f"\nok: {pairs} dependency pin(s) agree with the FetchContent_Declare "
          f"beside them.", file=out)
    return 0


def self_test():
    import io
    import shutil
    import tempfile

    failures = 0

    def note(message):
        print(message)

    def fail(message, output=""):
        nonlocal failures
        failures += 1
        print(f"error: {message}", file=sys.stderr)
        if output:
            print(output, file=sys.stderr)

    work = tempfile.mkdtemp()
    try:
        root = os.path.join(work, "repo")
        os.makedirs(root)
        lists = os.path.join(root, "CMakeLists.txt")

        def write(text):
            with open(lists, "w", encoding="utf-8") as handle:
                handle.write(text)

        def run():
            buffer = io.StringIO()
            return check(root, out=buffer), buffer.getvalue()

        def tree(cache_tag="v7.4.0", declare_tag="v7.4.0",
                 cache_url="https://example.invalid/glaze.git",
                 declare_url="https://example.invalid/glaze.git"):
            return (
                f"morph_cache_dep(glaze {cache_url} {cache_tag})\n"
                f"FetchContent_Declare(\n"
                f"    glaze\n"
                f"    GIT_REPOSITORY {declare_url}\n"
                f"    GIT_TAG        {declare_tag}\n"
                f"    GIT_SHALLOW    TRUE\n"
                f")\n"
            )

        # 1. Agreeing pins pass, and the pair is reported so a reader can see
        #    the gate had something to compare.
        write(tree())
        code, output = run()
        if code == 0 and "1 dependency pin(s) agree" in output:
            note("ok: an agreeing pair passes, and is counted")
        else:
            fail("an agreeing pin pair was rejected", output)

        # 2. THE CASE. One character apart, both lines named. Without this the
        #    gate would pass on a tree where it cannot fire, which is the
        #    pattern morph#693 is about.
        write(tree(declare_tag="v7.4.1"))
        code, output = run()
        if code != 0 and "CMakeLists.txt:1 caches 'v7.4.0'" in output \
                and "CMakeLists.txt:5 fetches GIT_TAG 'v7.4.1'" in output:
            note("ok: a skewed tag fails, and the message names both lines")
        else:
            fail("a skewed tag was accepted, or the message did not name both lines",
                 output)

        # 3. The repository is the other half of the same pin, and the cache key
        #    does not contain it -- so a divergence there serves one project's
        #    tree for another project's revision.
        write(tree(declare_url="https://example.invalid/fork.git"))
        code, output = run()
        if code != 0 and "repository is stated twice" in output:
            note("ok: a skewed GIT_REPOSITORY fails too")
        else:
            fail("a skewed repository was accepted", output)

        # 4. A declaration with no cache call beside it: the pin is guarded by
        #    nothing, and it clones on every configure.
        write("FetchContent_Declare(Catch2\n"
              "    GIT_REPOSITORY https://example.invalid/catch2.git\n"
              "    GIT_TAG        v3.8.1\n"
              ")\n")
        code, output = run()
        if code != 0 and "no morph_cache_dep() beside it" in output:
            note("ok: a FetchContent_Declare with no cache call is reported")
        else:
            fail("an unguarded FetchContent_Declare was accepted", output)

        # 5. And the mirror: a cache call for something nothing declares, which
        #    is what stops rule 4 being satisfiable by deleting the declaration.
        write("morph_cache_dep(glaze https://example.invalid/glaze.git v7.4.0)\n")
        code, output = run()
        if code != 0 and "nothing in this file declares it to FetchContent" in output:
            note("ok: a cache call with no declaration is reported")
        else:
            fail("an orphan morph_cache_dep() was accepted", output)

        # 6. A GIT_REPOSITORY with no GIT_TAG is the same divergence with one
        #    side left unwritten: the cache serves a pinned tree, an uncached
        #    configure takes the default branch.
        write("morph_cache_dep(glaze https://example.invalid/glaze.git v7.4.0)\n"
              "FetchContent_Declare(glaze\n"
              "    GIT_REPOSITORY https://example.invalid/glaze.git\n"
              ")\n")
        code, output = run()
        if code != 0 and "no GIT_TAG" in output:
            note("ok: a declaration with no GIT_TAG is reported")
        else:
            fail("an unpinned declaration was accepted", output)

        # 7. A commented-out call is not a call. Without this the gate could be
        #    satisfied, or broken, by prose.
        write("# morph_cache_dep(glaze https://example.invalid/glaze.git v9.9.9)\n"
              + tree())
        code, output = run()
        if code == 0:
            note("ok: a commented-out call is not read as a pin")
        else:
            fail("a commented-out call was parsed as a real one", output)

        # 8. A morph_cache_dep() this script cannot parse is an error, not a
        #    silent skip -- a skip is how the gate would quietly stop covering a
        #    dependency someone reformatted.
        write("morph_cache_dep(\n"
              "    glaze https://example.invalid/glaze.git v7.4.0)\n"
              "FetchContent_Declare(glaze\n"
              "    GIT_REPOSITORY https://example.invalid/glaze.git\n"
              "    GIT_TAG        v7.4.0\n"
              ")\n")
        code, output = run()
        if code != 0 and "cannot parse" in output:
            note("ok: an unparsable morph_cache_dep() call fails rather than skipping")
        else:
            fail("an unparsable call was skipped silently", output)

        # 9. A tree with no dependencies at all fails. This gate has nothing to
        #    say about such a tree, and saying it in green would be a lie.
        write("project(morph)\n")
        code, output = run()
        if code != 0 and "reports green exactly as loudly" in output:
            note("ok: a tree with no pins at all is refused, not passed")
        else:
            fail("a tree with nothing to check reported success", output)

        # 10. A non-git FetchContent_Declare (URL, SOURCE_DIR) is not this
        #     gate's business and must not be forced into a pin it has no
        #     concept of.
        write(tree()
              + "FetchContent_Declare(data\n"
                "    URL https://example.invalid/data.tar.gz\n"
                ")\n")
        code, output = run()
        if code == 0:
            note("ok: a non-git FetchContent_Declare is left alone")
        else:
            fail("a URL-based dependency was treated as a git pin", output)

    finally:
        shutil.rmtree(work, ignore_errors=True)

    if failures:
        print(f"\n{failures} self-test check(s) failed", file=sys.stderr)
        return 1

    print("\nall self-test checks passed")
    return 0


def main(argv):
    if "--self-test" in argv:
        return self_test()
    return check(argv[0] if argv else os.getcwd())


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))

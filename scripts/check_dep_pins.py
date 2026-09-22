#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Fail when a dependency's pin can be written twice.

Usage:
    python3 scripts/check_dep_pins.py [REPO_ROOT]
    python3 scripts/check_dep_pins.py --self-test

What this checked before, and why it does not any more (morph#693, morph#712)
-----------------------------------------------------------------------------
Every FetchContent dependency in this tree used to state its revision twice:
once as `morph_cache_dep(<name> <url> <tag>)`, once as the `FetchContent_Declare`
beside it.

    morph_cache_dep(glaze https://github.com/stephenberry/glaze.git v7.4.0)
    FetchContent_Declare(
        glaze
        GIT_REPOSITORY https://github.com/stephenberry/glaze.git
        GIT_TAG        v7.4.0
        GIT_SHALLOW    TRUE
    )

Nothing compared the two, and the divergence was asymmetric in the worst way:
with a warm cache the build gets `morph_cache_dep`'s tag (it points
FETCHCONTENT_SOURCE_DIR_<NAME> at a tree already checked out at it, and
FetchContent skips the download); with no cache -- an ordinary local build,
which opts out by default -- it gets `GIT_TAG`'s. A mismatch therefore compiled
a different revision in CI than on the author's machine, with no diagnostic on
either side and both builds succeeding.

morph#693 added a gate that compared the two copies. morph#712 removed the
second copy instead: `morph_declare_dep(<name> <url> <tag> [args...])` in
cmake/DepCache.cmake takes the pin once and hands it to both the cache and
`FetchContent_Declare`, and refuses `GIT_REPOSITORY`/`GIT_TAG` in its forwarded
arguments so the second copy cannot be smuggled back into the one call that
ended it.

So the comparison this file used to make now has nothing to compare -- and a
gate left reporting green over an invariant body is this repository's named
failure mode. What replaced it is the invariant that makes the comparison
unnecessary, which is a *stricter* thing than the comparison was:

  1. No `FetchContent_Declare` of a git repository anywhere outside
     cmake/DepCache.cmake. That is the only shape in which a second pin can be
     written, so forbidding it is what makes the divergence inexpressible
     rather than merely undetected. It also keeps every dependency on the
     shared source cache (morph#552): a hand-written declaration clones again
     on every configure.
  2. No direct `morph_cache_dep` call outside cmake/DepCache.cmake. It is the
     caching half of the split and populates a directory nothing would read;
     a call site reaching for it is a call site about to declare by hand.
  3. No `GIT_REPOSITORY`/`GIT_TAG` among a `morph_declare_dep` call's forwarded
     arguments -- the same refusal DepCache.cmake makes at configure time,
     reported here without needing a configure.
  4. Two trees declaring the same dependency must name the same revision.
     examples/common and examples/bank both fetch Lightweight, either can be
     built without the other, and FetchContent keeps the *first* declaration of
     a name and ignores the rest -- so two disagreeing calls would build
     whichever tree configured first while the other pin sat there as inert
     text. That is the two-copy divergence one file apart, and it is the only
     form of it the wrapper cannot make unwritable, because both call sites are
     genuinely needed.

The vacuity rules are unchanged in spirit. A `morph_declare_dep` call this
script cannot parse is an error rather than a skip, and finding zero calls at
all is an error: a gate with nothing left to check reports green exactly as
loudly as one that checked everything.

Acceptance is not "it passes on master" -- it would pass on master if it parsed
nothing. It is "write the old two-pin shape and watch it fail, naming the line".
See the self-test, and the commit that narrowed this file for the same thing
done on the real tree.

Scope
-----
Reads CMake text only; configures nothing, clones nothing. A non-git
`FetchContent_Declare` (URL, SOURCE_DIR, SVN) is not this gate's business: it
carries no revision the cache keys on, and forcing it through a git-shaped
wrapper would be inventing a rule rather than enforcing one.
"""

from __future__ import annotations

import os
import re
import sys

# `morph_declare_dep(name repository tag [forwarded args...])`, which may span
# lines -- the real call sites put the forwarded arguments on the next one.
# Anything the token split cannot make sense of is reported, not skipped; see
# below.
DECLARE_DEP_OPEN_RE = re.compile(r"^\s*morph_declare_dep\(")
CACHE_DEP_OPEN_RE = re.compile(r"^\s*morph_cache_dep\(")
DECLARE_RE = re.compile(r"^\s*FetchContent_Declare\(\s*([^\s()]*)")

SKIP_DIRECTORIES = {".git", "build", "out", "vcpkg_installed", "_deps", "node_modules"}

# The definition of both functions, not a call site: it is the one place a
# FetchContent_Declare and a morph_cache_dep call are supposed to appear.
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


def read_call(lines, index):
    """The text of the call opening at `index`, and the line after it.

    Comment lines inside the call are dropped, so a reason written between the
    arguments is not mistaken for one. Returns (text, next_index), or
    (None, next_index) when the parentheses never balance.
    """
    depth = 0
    collected = []
    while index < len(lines):
        line = lines[index]
        if not is_comment(line):
            collected.append(line)
            depth += line.count("(") - line.count(")")
        index += 1
        if collected and depth <= 0:
            return " ".join(collected), index
    return None, index


def declare_dep_arguments(text):
    """The argument tokens of a morph_declare_dep call, or None if unparsable."""
    inside = text[text.index("(") + 1:]
    inside = inside[:inside.rindex(")")] if ")" in inside else None
    if inside is None:
        return None
    tokens = inside.split()
    if len(tokens) < 3:
        return None
    return tokens


def check(repo_root, out=sys.stdout):
    failures = []
    calls = 0
    checked = []
    # name -> [(path, line, url, tag)]. Two trees may each need the same
    # dependency -- examples/common and examples/bank both fetch Lightweight --
    # and FetchContent *ignores* the second declaration of a name it already
    # has. So the residue of the two-copy pin lives here: two calls naming one
    # dependency at different revisions would build the first one silently,
    # whichever tree configures first, and the second call's pin would be
    # inert text. Collected across files rather than within one, because that
    # is the only place this shape can occur.
    by_name = {}

    for path in cmake_files(repo_root):
        if path == DEFINITION:
            continue
        with open(os.path.join(repo_root, path), encoding="utf-8") as handle:
            lines = handle.read().splitlines()
        body = "\n".join(lines)
        if ("morph_declare_dep" not in body and "morph_cache_dep" not in body
                and "FetchContent_Declare" not in body):
            continue

        index = 0
        while index < len(lines):
            line = lines[index]
            if is_comment(line):
                index += 1
                continue

            if DECLARE_DEP_OPEN_RE.match(line):
                start = index + 1
                text, index = read_call(lines, index)
                if text is None:
                    failures.append(
                        f"{path}:{start} opens a morph_declare_dep() call whose "
                        f"parentheses never close, so this gate cannot see what it "
                        f"pins.")
                    continue
                tokens = declare_dep_arguments(text)
                if tokens is None:
                    failures.append(
                        f"{path}:{start} calls morph_declare_dep() in a form this gate "
                        f"cannot parse, so its pin is checked by nothing. Write it as "
                        f"`morph_declare_dep(<name> <url> <tag> [args...])`, or teach "
                        f"this script the new form -- do not leave it unchecked.")
                    continue
                name, url, tag = tokens[0], tokens[1], tokens[2]
                calls += 1
                by_name.setdefault(name, []).append((path, start, url, tag))
                offenders = [token for token in tokens[3:]
                             if token in ("GIT_REPOSITORY", "GIT_TAG")]
                if offenders:
                    failures.append(
                        f"{path}:{start} passes {offenders[0]} to morph_declare_dep() "
                        f"as a forwarded argument for {name!r}. The repository and the "
                        f"tag are that call's own second and third arguments; stating "
                        f"either twice re-creates the divergence the wrapper exists to "
                        f"make unwritable -- the cache keys on what it is handed and "
                        f"FetchContent fetches what it is handed, so a warm cache would "
                        f"build a different revision than a cold one, both successfully "
                        f"(morph#693, morph#712). cmake/DepCache.cmake refuses this at "
                        f"configure time too.")
                else:
                    checked.append((path, start, name, tag))
                continue

            if CACHE_DEP_OPEN_RE.match(line):
                start = index + 1
                _text, index = read_call(lines, index)
                failures.append(
                    f"{path}:{start} calls morph_cache_dep() directly. That is the "
                    f"caching half of the split in cmake/DepCache.cmake and declares "
                    f"nothing: on its own it populates a cache directory nothing reads, "
                    f"and beside a hand-written FetchContent_Declare it is the two-copy "
                    f"pin morph#712 removed. Call morph_declare_dep() instead.")
                continue

            match = DECLARE_RE.match(line)
            if match:
                start = index + 1
                text, index = read_call(lines, index)
                if text is None:
                    failures.append(
                        f"{path}:{start} opens a FetchContent_Declare() whose "
                        f"parentheses never close.")
                    continue
                name = match.group(1) or (text.split("(", 1)[1].split() or [""])[0]
                arguments = text[text.index("(") + 1:].replace(")", " ").split()
                if not any(argument in ("GIT_REPOSITORY", "GIT_TAG")
                           for argument in arguments):
                    continue  # not a git dependency: URL/SOURCE_DIR/SVN and friends
                failures.append(
                    f"{path}:{start} declares the git dependency {name!r} with a "
                    f"hand-written FetchContent_Declare. Its pin is then written here "
                    f"and, if it is to be cached at all, a second time beside it -- the "
                    f"two-copy shape whose halves could disagree with no diagnostic "
                    f"(morph#693). Every configure would also clone it again, which is "
                    f"the volume morph#552's shared source cache exists to cut. Use "
                    f"`morph_declare_dep(<name> <url> <tag> [args...])` from "
                    f"cmake/DepCache.cmake, which states the pin once and forwards the "
                    f"rest (GIT_SHALLOW and anything else) verbatim.")
                continue

            index += 1

    for name, sites in sorted(by_name.items()):
        if len(sites) < 2:
            continue
        pins = {(url, tag) for _path, _line, url, tag in sites}
        where = ", ".join(f"{path}:{line} at {tag!r}" for path, line, _u, tag in sites)
        if len(pins) > 1:
            failures.append(
                f"{name!r} is declared {len(sites)} times and the declarations disagree: "
                f"{where}. FetchContent keeps the first declaration of a name and ignores "
                f"the rest, so the build would take whichever tree configures first and "
                f"the other pin would be inert text -- the two-copy divergence morph#712 "
                f"removed, one file apart. Both call sites are needed (either tree can be "
                f"built alone), so make them agree rather than deleting one.")
        else:
            print(f"ok: {name} declared in {len(sites)} places, all at the same "
                  f"revision ({where})", file=out)

    for path, line, name, tag in checked:
        print(f"ok: {path}:{line}: {name} pinned at {tag}", file=out)

    if calls == 0 and not failures:
        failures.append(
            "no morph_declare_dep() call was found anywhere in the tree. Either every "
            "dependency stopped using the cache, or this gate's parsing has stopped "
            "matching how they are written -- and a gate with nothing left to check "
            "reports green exactly as loudly as one that checked everything.")

    if failures:
        print(f"\nerror: {len(failures)} dependency declaration problem(s):\n", file=out)
        for failure in failures:
            print(f"  - {failure}", file=out)
        return 1

    print(f"\nok: {calls} morph_declare_dep() call(s) over {len(by_name)} dependency/ies; "
          f"each call states its pin once, and every dependency declared from more than "
          f"one tree states the same revision in each.", file=out)
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

        # The real call shape: the forwarded arguments are on the next line, so
        # a one-line parser would see no pin at all and pass.
        multiline = (
            "morph_declare_dep(glaze https://example.invalid/glaze.git v7.4.0\n"
            "    GIT_SHALLOW TRUE)\n"
        )

        # 1. A single-line call passes and is counted, so a reader can see the
        #    gate had something to look at.
        write("morph_declare_dep(docs https://example.invalid/docs.git v2.3.4)\n")
        code, output = run()
        if code == 0 and "1 morph_declare_dep() call(s)" in output:
            note("ok: a single-line morph_declare_dep() passes, and is counted")
        else:
            fail("a well-formed call was rejected", output)

        # 2. And the shape every real call site actually uses.
        write(multiline)
        code, output = run()
        if code == 0 and "glaze pinned at v7.4.0" in output:
            note("ok: a call whose forwarded arguments wrap onto the next line is read")
        else:
            fail("a multi-line call was not parsed", output)

        # 3. THE CASE. The two-copy pin morph#712 removed, written back by hand.
        #    Without this the gate would pass on a tree where it cannot fire,
        #    which is the pattern it exists to prevent.
        write(multiline
              + "FetchContent_Declare(\n"
                "    glaze\n"
                "    GIT_REPOSITORY https://example.invalid/glaze.git\n"
                "    GIT_TAG        v7.4.1\n"
                ")\n")
        code, output = run()
        if code != 0 and "CMakeLists.txt:3 declares the git dependency" in output:
            note("ok: a hand-written git FetchContent_Declare fails, naming its line")
        else:
            fail("a hand-written git FetchContent_Declare was accepted", output)

        # 4. The same shape on one line, which is how examples/ used to write it.
        write(multiline
              + "FetchContent_Declare(Lightweight\n"
                "    GIT_REPOSITORY https://example.invalid/lw.git\n"
                "    GIT_TAG        abc123\n"
                ")\n")
        code, output = run()
        if code != 0 and "Lightweight" in output:
            note("ok: the name-on-the-open-line form is caught too")
        else:
            fail("a one-line-name FetchContent_Declare was accepted", output)

        # 5. The other half of the old shape: the caching call on its own. It
        #    declares nothing, so on its own it fills a directory nothing reads.
        write(multiline
              + "morph_cache_dep(glaze https://example.invalid/glaze.git v7.4.0)\n")
        code, output = run()
        if code != 0 and "calls morph_cache_dep() directly" in output:
            note("ok: a direct morph_cache_dep() call is reported")
        else:
            fail("a direct morph_cache_dep() call was accepted", output)

        # 6. The divergence smuggled into the wrapper itself. DepCache.cmake
        #    refuses this at configure time; this is the same refusal without a
        #    configure, so a reviewer sees it on the diff.
        write("morph_declare_dep(glaze https://example.invalid/glaze.git v7.4.0\n"
              "    GIT_SHALLOW TRUE GIT_TAG v9.9.9)\n")
        code, output = run()
        if code != 0 and "passes GIT_TAG to morph_declare_dep()" in output:
            note("ok: a second GIT_TAG inside the wrapper call is reported")
        else:
            fail("a second pin inside the wrapper call was accepted", output)

        # 7. A commented-out call is not a call, and a reason written between a
        #    call's arguments is not an argument. Without this the gate could be
        #    satisfied, or broken, by prose.
        write("# FetchContent_Declare(glaze GIT_TAG v9.9.9)\n"
              "# morph_cache_dep(glaze https://example.invalid/glaze.git v9.9.9)\n"
              "morph_declare_dep(glaze https://example.invalid/glaze.git v7.4.0\n"
              "    # GIT_SHALLOW FALSE would not resolve a SHA\n"
              "    GIT_SHALLOW TRUE)\n")
        code, output = run()
        if code == 0 and "glaze pinned at v7.4.0" in output:
            note("ok: comments are not read as calls or as arguments")
        else:
            fail("a comment was parsed as a call or an argument", output)

        # 8. A morph_declare_dep() this script cannot parse is an error, not a
        #    silent skip -- a skip is how the gate would quietly stop covering a
        #    dependency someone reformatted.
        write("morph_declare_dep(glaze)\n")
        code, output = run()
        if code != 0 and "cannot parse" in output:
            note("ok: an unparsable morph_declare_dep() call fails rather than skipping")
        else:
            fail("an unparsable call was skipped silently", output)

        # 9. A tree with no dependencies at all fails. This gate has nothing to
        #    say about such a tree, and saying it in green would be a lie.
        write("project(morph)\n")
        code, output = run()
        if code != 0 and "reports green exactly as loudly" in output:
            note("ok: a tree with no declarations at all is refused, not passed")
        else:
            fail("a tree with nothing to check reported success", output)

        # 10. A non-git FetchContent_Declare (URL, SOURCE_DIR) carries no
        #     revision the cache keys on, so it is not this gate's business and
        #     must not be forced through a git-shaped wrapper.
        write(multiline
              + "FetchContent_Declare(data\n"
                "    URL https://example.invalid/data.tar.gz\n"
                ")\n")
        code, output = run()
        if code == 0:
            note("ok: a non-git FetchContent_Declare is left alone")
        else:
            fail("a URL-based dependency was treated as a git pin", output)

        # 11. The residue the wrapper cannot remove: one dependency, two trees.
        #     FetchContent ignores the second declaration of a name, so two
        #     calls that disagree build the first silently.
        os.makedirs(os.path.join(root, "two"), exist_ok=True)
        write(multiline)
        with open(os.path.join(root, "two", "CMakeLists.txt"), "w",
                  encoding="utf-8") as handle:
            handle.write("morph_declare_dep(glaze https://example.invalid/glaze.git "
                         "v7.4.1)\n")
        code, output = run()
        if code != 0 and "declared 2 times and the declarations disagree" in output:
            note("ok: two trees pinning one dependency differently is reported")
        else:
            fail("a cross-file pin divergence was accepted", output)

        with open(os.path.join(root, "two", "CMakeLists.txt"), "w",
                  encoding="utf-8") as handle:
            handle.write("morph_declare_dep(glaze https://example.invalid/glaze.git "
                         "v7.4.0)\n")
        code, output = run()
        if code == 0 and "declared in 2 places, all at the same revision" in output:
            note("ok: two trees pinning it identically pass, and are reported as two")
        else:
            fail("two agreeing declarations of one dependency were rejected", output)
        os.remove(os.path.join(root, "two", "CMakeLists.txt"))

        # 12. cmake/DepCache.cmake is where the one legitimate
        #     FetchContent_Declare lives; exempting it must not exempt a file
        #     that merely looks like it.
        os.makedirs(os.path.join(root, "cmake"), exist_ok=True)
        write(multiline)
        with open(os.path.join(root, "cmake", "DepCache.cmake"), "w",
                  encoding="utf-8") as handle:
            handle.write("function(morph_declare_dep name repository tag)\n"
                         "    morph_cache_dep(${name} ${repository} ${tag})\n"
                         "    FetchContent_Declare(${name}\n"
                         "        GIT_REPOSITORY ${repository}\n"
                         "        GIT_TAG ${tag} ${ARGN})\n"
                         "endfunction()\n")
        code, output = run()
        if code == 0:
            note("ok: the definition itself is not read as a call site")
        else:
            fail("cmake/DepCache.cmake's own definition was reported", output)

        with open(os.path.join(root, "cmake", "Other.cmake"), "w",
                  encoding="utf-8") as handle:
            handle.write("FetchContent_Declare(sneaky\n"
                         "    GIT_REPOSITORY https://example.invalid/s.git\n"
                         "    GIT_TAG v1)\n")
        code, output = run()
        if code != 0 and "sneaky" in output:
            note("ok: the exemption is the one path, not any .cmake beside it")
        else:
            fail("a declaration in another cmake/ file was exempted too", output)

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

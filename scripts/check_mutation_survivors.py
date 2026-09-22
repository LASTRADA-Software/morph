#!/usr/bin/env python3
"""Audit scripts/mutation_survivors.json's citations against the code they name.

Usage:
    python3 scripts/check_mutation_survivors.py [SURVIVORS_JSON]
    python3 scripts/check_mutation_survivors.py --self-test

Why this exists (morph#608)
---------------------------
scripts/mutation_survivors.json records each triaged equivalent mutant as
`{file, line, mutator, source, reason}`. Nothing audited the `line` field, and
5 of its 7 line-carrying entries pointed at the wrong line by the time morph#608
measured them -- `include/morph/core/backend.hpp:749` was off by 479 lines and
landed in the middle of an unrelated function. Every `source` text still existed
and was still unique, so no *disposition* was wrong; only the coordinates were.
Re-checking the file's own history shows no `line` hint in it has ever been
edited since the file was created, so the two entries that are still right are
right by luck -- nothing above them happened to move -- not because anyone
refreshed them.

That is the same defect this repository has already found in
scripts/branch_partial_allowlist.json and scripts/error_path_allowlist.json
(morph#349, morph#355, morph#419): a citation keyed by line number alone rots
silently, because nothing reads it until a human follows it and finds
unrelated code. Both of those files are hardened by
check_branch_coverage.py's resolve_allowlist_source_line(), which resolves an
entry by its `source` text and fails the gate when the `line` hint has drifted.
mutation_survivors.json carried the same shape of citation with none of the
auditing. This script closes that, by calling the same resolver rather than
writing a second one -- it is check_error_path_coverage.py (morph#406) and this
script, three callers of one implementation, because an independently
maintained second copy of the fix is how this defect class gets a fourth
chance.

Why this gate is cheap, and where it runs
-----------------------------------------
Unlike the other two callers, this one reads no coverage data at all: its
inputs are a JSON file and the headers in the tree. So it runs as a *gate*, not
just as a self-test, in .github/workflows/drift-guard.yml's
`mutation-regression-lint` job -- on every pull request, in seconds, with no
build. The mutation campaign itself (.github/workflows/mutation.yml) is
scheduled rather than per-PR, but the citations in this file rot with ordinary
edits to include/morph, which happen per-PR, so that is where the audit belongs.

What it does NOT audit, stated rather than left to be discovered
----------------------------------------------------------------
Only the *structured* entries -- dicts carrying `file`, `line` and `source`.
The same document also carries citations as free text, inside prose strings:
`"site": "remote.hpp:1031"`, `"core/registry.hpp:54 -- PairKeyHash
hash-combine"`, and 18 more (20 in all, as this script's own
count_free_text_citations() reports on every run). Those have no verbatim
`source` text to resolve against -- the text after the `--` is a paraphrase,
and several use a bare filename with no directory -- so this resolver cannot
see them.

Every one of those 20 is *deliberately* out of reach, and morph#613 settled
why. They live in the file's dated campaign records -- `runs`,
`false_positive_finding`, `mechanism_confirmed` and
`classification_2026_09_09` -- whose line numbers describe the tree as it was
when the campaign ran, and each of those sections now carries a `revision` so
the citation resolves with `git show <revision>:<path>`. Auditing them against
HEAD would fail forever and correctly so: the record is not wrong, the reading
would be. The citations that *do* describe current code all sit under
`classes`, and morph#613 converted them to the structured shape above, which
this script picked up with no edit to it -- find_entries() walks the whole
document by design, exactly so that a new class of live citation is covered
without a second mechanism.

The count is printed on every run so this gate's coverage of the file is
visible rather than assumed: auditing the structured half of a file while the
prose half is exempt by design is a narrower claim than a green tick looks.

Vacuity
-------
The failure mode AGENTS.md names first is a check that reports success while
measuring nothing, so two things are refused rather than skipped:

  * **Finding no auditable entries at all** fails. The entries are located by
    walking the whole document for dicts carrying `file`/`line`/`source`,
    rather than by a hard-coded `classes.equivalent.entries` path, so a future
    triage pass that adds a new class gets audited for free -- but if a
    restructure ever moved them all out of reach, this gate would otherwise go
    green over zero entries, which is exactly the shape of failure it exists to
    prevent.
  * **An entry with `file` and `line` but no `source`** fails, rather than
    being skipped as "not one of mine". An entry with no source text is
    un-auditable by construction, and silently ignoring it would let the next
    contributor add exactly the unauditable citation this gate was written to
    forbid.
"""

from __future__ import annotations

import json
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import check_branch_coverage  # noqa: E402 -- resolve_allowlist_source_line(), see above

SURVIVORS = "scripts/mutation_survivors.json"

# `path/to/file.hpp:123` or `file.hpp:123` appearing inside a prose string --
# the free-text citations this resolver cannot audit. Counted and reported, not
# gated; see the module docstring and morph#613.
FREE_TEXT_CITATION = re.compile(r"[\w./-]+\.(?:hpp|cpp|h|cc):\d+")


def find_entries(document):
    """[(json path, entry dict)] for every dict carrying `file` and `line`.

    Walks the whole document rather than indexing a fixed key path, so a class
    added by a future triage pass is audited without editing this script, and
    a restructure that moved the entries cannot silently empty the gate (the
    caller fails on an empty result).
    """
    found = []

    def walk(node, where):
        if isinstance(node, dict):
            if "file" in node and "line" in node:
                found.append((where, node))
            for key, value in node.items():
                walk(value, f"{where}.{key}")
        elif isinstance(node, list):
            for index, value in enumerate(node):
                walk(value, f"{where}[{index}]")

    walk(document, "")
    return found


def count_free_text_citations(document):
    """How many `file.hpp:123` citations live in prose strings, not entries.

    Reported so this gate's own coverage of the file is visible. Strings that
    belong to an audited entry (its `file`, `source` or `reason`) are not
    counted -- only citations this resolver structurally cannot reach.
    """
    audited = {id(entry) for _, entry in find_entries(document)}
    count = 0

    def walk(node):
        nonlocal count
        if isinstance(node, dict):
            if id(node) in audited:
                # An audited entry's own fields are accounted for; only its
                # prose (`reason`) can still carry an unauditable citation.
                node = {"reason": node.get("reason", "")}
            for value in node.values():
                walk(value)
        elif isinstance(node, list):
            for value in node:
                walk(value)
        elif isinstance(node, str):
            count += len(FREE_TEXT_CITATION.findall(node))

    walk(document)
    return count


def check(survivors_path, repo_root, out=sys.stdout):
    if not os.path.exists(survivors_path):
        print(f"error: {survivors_path} does not exist. It is the triage record for "
              f"every mutation survivor this repository has dispositioned; without it "
              f"a survivor with a reason and one with none are indistinguishable.",
              file=sys.stderr)
        return 1

    with open(survivors_path, encoding="utf-8") as handle:
        document = json.load(handle)

    entries = find_entries(document)
    failures = []
    audited = 0

    for where, entry in entries:
        path, hint = entry["file"], entry["line"]
        location = f"{survivors_path}{where}"

        if not isinstance(hint, int):
            failures.append(f"{location} ({path}) has a non-integer `line` "
                            f"{hint!r}, which no resolver can check.")
            continue

        wanted = str(entry.get("source", "")).strip()
        if not wanted:
            failures.append(
                f"{path}:{hint} ({location}) cites a line with no `source` text. A "
                f"bare line number is the citation that rots (morph#349, morph#419) -- "
                f"add the verbatim source line so this entry can be re-resolved when "
                f"the code moves."
            )
            continue

        reason = str(entry.get("reason", "")).strip()
        if not reason:
            failures.append(f"{path}:{hint} ({location}) is recorded with no reason. "
                            f"A bare disposition is not a triage.")
            continue

        resolved = check_branch_coverage.resolve_allowlist_source_line(
            repo_root, path, hint, wanted, survivors_path, failures,
            context=entry.get("context"))
        if resolved is None:
            continue
        audited += 1

    free_text = count_free_text_citations(document)

    if not entries:
        failures.append(
            f"{survivors_path} carries no structured entries at all (nothing with "
            f"`file`, `line` and `source`). Either the file was restructured and this "
            f"gate now audits nothing -- which is the failure it exists to prevent -- "
            f"or the triage record was emptied. Neither is a state to pass."
        )

    if failures:
        print(f"{survivors_path}: {len(failures)} citation(s) no longer match the "
              f"code they name.\n", file=out)
        for failure in failures:
            print(f"  - {failure}", file=out)
        print(
            "\nThese are triage dispositions, not suppressions: the reasoning is "
            "recorded\nbecause someone checked it. Re-read the code at the resolved "
            "line, confirm the\nreason still holds, and then update the entry's "
            "`line` to the number printed\nabove. Do not delete an entry to silence "
            "this gate, and do not move a `source`\ntext to make it match -- either "
            "is a triage nobody performed.", file=out)
        return 1

    print(f"ok: {audited} structured citation(s) in {survivors_path} resolve to the "
          f"line they name.", file=out)
    print(f"note: {free_text} further citation(s) in this file are free text inside "
          f"prose\n      strings, which carry no verbatim `source` and so are NOT "
          f"audited here (morph#613).", file=out)
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
        os.makedirs(os.path.join(root, "include", "morph", "core"))
        header = os.path.join(root, "include", "morph", "core", "x.hpp")

        def write_header(text):
            with open(header, "w", encoding="utf-8") as handle:
                handle.write(text)

        survivors = os.path.join(root, "survivors.json")

        def write_survivors(document):
            with open(survivors, "w", encoding="utf-8") as handle:
                json.dump(document, handle)

        def run():
            buf = io.StringIO()
            return check(survivors, root, out=buf), buf.getvalue()

        def entry(**overrides):
            base = {
                "file": "include/morph/core/x.hpp",
                "line": 3,
                "mutator": "cxx_replace_scalar_call",
                "source": "out.reserve(text.size());",
                "reason": "fixture: a capacity hint, equivalent by construction",
            }
            base.update(overrides)
            return {"classes": {"equivalent": {"entries": [base]}}}

        original = (
            "void f() {\n"
            "    std::string out;\n"
            "    out.reserve(text.size());\n"   # line 3
            "}\n"
        )
        write_header(original)

        # 1. a citation that matches -> passes, and says how many it audited.
        write_survivors(entry())
        rc, output = run()
        if rc != 0:
            fail("a citation matching its line did not pass the gate", output)
        elif "ok: 1 structured citation" not in output:
            fail("a passing run did not report how many citations it audited", output)
        else:
            note("ok: a citation that matches its line passes, and is counted")

        # 2. the code moves, the hint does not -> fails, printing the new line.
        #    This is morph#608's own shape, and the case that proves this gate
        #    is not vacuous: nothing about the JSON changes between 1 and 2.
        write_header("// a new leading comment shifts everything down\n" + original)
        rc, output = run()
        if rc == 0:
            fail("a drifted `line` hint passed the gate", output)
        elif "has moved to line 4" not in output:
            fail("a drifted hint failed, but did not print the corrected line", output)
        elif "update the entry's `line`" not in output:
            fail("a drifted hint failed without telling the reader how to refresh it",
                 output)
        else:
            note("ok: a drifted `line` hint fails, and the corrected line is printed")

        # 3. the cited text is gone entirely -> fails differently, because the
        #    disposition itself may no longer apply.
        write_header("void f() {\n    std::string out;\n}\n")
        rc, output = run()
        if rc == 0:
            fail("a citation whose source text no longer exists passed the gate", output)
        elif "nowhere in the file any more" not in output:
            fail("a vanished source text failed with the wrong message", output)
        else:
            note("ok: a citation whose source text is gone fails, and says the code changed")

        # 4. the cited text now appears twice, at neither the hinted line ->
        #    ambiguous, and refused rather than guessed at. Since morph#701 the
        #    refusal does not depend on where the hint points (cases 11-13
        #    below), so the message is the one that asks for a `context`.
        write_header(
            "void f() {\n"
            "    out.reserve(text.size());\n"
            "    out.reserve(text.size());\n"
            "}\n"
        )
        write_survivors(entry(line=9))
        rc, output = run()
        if rc == 0:
            fail("an ambiguous citation passed the gate", output)
        elif "does not say which occurrence is meant" not in output:
            fail("an ambiguous citation failed with the wrong message", output)
        else:
            note("ok: a citation matching several lines is refused as ambiguous")

        # 5. an entry with no `source` -> refused, not skipped. Skipping it is
        #    how the gate would quietly stop covering new entries.
        write_header(original)
        document = entry()
        del document["classes"]["equivalent"]["entries"][0]["source"]
        write_survivors(document)
        rc, output = run()
        if rc == 0:
            fail("an entry with no `source` text passed the gate", output)
        elif "no `source` text" not in output:
            fail("a source-less entry failed with the wrong message", output)
        else:
            note("ok: an entry with no `source` text is refused, not skipped")

        # 6. an entry with no reason -> refused, on the same terms as every
        #    other allowlist in this repository.
        write_survivors(entry(reason=""))
        rc, output = run()
        if rc == 0:
            fail("an entry with no reason passed the gate", output)
        elif "not a triage" not in output:
            fail("a reason-less entry failed with the wrong message", output)
        else:
            note("ok: an entry with no reason is refused")

        # 7. a document with no entries at all -> refused. A gate that audits
        #    nothing must not report success (AGENTS.md's first failure mode).
        write_survivors({"classes": {"equivalent": {"entries": []}}})
        rc, output = run()
        if rc == 0:
            fail("a document with no auditable entries passed the gate", output)
        elif "audits nothing" not in output:
            fail("an empty document failed with the wrong message", output)
        else:
            note("ok: a document this gate can audit nothing in is refused")

        # 8. entries found outside `classes.equivalent` are audited too, so a
        #    future class does not have to remember to edit this script.
        write_survivors({"some_future_class": {"rows": [
            {"file": "include/morph/core/x.hpp", "line": 99,
             "source": "out.reserve(text.size());",
             "reason": "fixture: in a class this script has never heard of"}]}})
        rc, output = run()
        if rc == 0:
            fail("an entry outside classes.equivalent was not audited", output)
        elif "has moved to line 3" not in output:
            fail("an entry in an unknown class failed with the wrong message", output)
        else:
            note("ok: an entry in a class this script does not know about is audited")

        # 9. a file that no longer exists -> named, not ignored.
        write_survivors(entry(file="include/morph/core/gone.hpp"))
        rc, output = run()
        if rc == 0:
            fail("a citation naming a deleted file passed the gate", output)
        elif "does not exist" not in output:
            fail("a deleted file failed with the wrong message", output)
        else:
            note("ok: a citation naming a file that is gone fails, and names it")

        # 10. the free-text counter sees prose citations and does not gate on
        #     them -- the narrowness this gate reports about itself.
        write_header(original)
        document = entry()
        document["notes"] = ["include/morph/core/y.hpp:613 -- a prose citation",
                             "and remote.hpp:1031 as well"]
        write_survivors(document)
        rc, output = run()
        if rc != 0:
            fail("a prose citation wrongly failed the gate", output)
        elif "note: 2 further citation(s)" not in output:
            fail("the free-text citation count was wrong or missing", output)
        else:
            note("ok: free-text citations are counted and reported, not gated")

        # 11-13. The `context` disambiguator reaches this consumer too
        #     (morph#701). Case 4 above covers the hint matching *nothing*;
        #     these three cover the case that used to pass silently -- a hint
        #     that matches one of several occurrences, which this file has two
        #     live instances of (backend.hpp's registerCount and executeInFlight
        #     emissions, both with a twin the entry's prose excludes).
        ambiguous = (
            "void registerModel() {\n"           # 1
            "    emitMetric(registerCount);\n"   # 2
            "}\n"                                # 3
            + "// filler\n" * 50 +               # 4..53
            "void registerModelShared() {\n"     # 54
            "    emitMetric(registerCount);\n"   # 55
            "}\n"                                # 56
        )
        write_header(ambiguous)

        write_survivors(entry(line=55, source="emitMetric(registerCount);"))
        rc, output = run()
        if rc == 0:
            fail("a citation naming one of two identical arms passed with no `context`",
                 output)
        elif "does not say which occurrence is meant" not in output:
            fail("an ambiguous-but-matching citation failed with the wrong message", output)
        else:
            note("ok: a hint that matches one of several occurrences is refused")

        write_survivors(entry(line=55, source="emitMetric(registerCount);",
                              context="void registerModel() {"))
        rc, output = run()
        if rc == 0:
            fail("a `context` naming a different arm than `line` passed the gate", output)
        elif "resolves through its `context` to line 2" not in output:
            fail("a `context`/`line` disagreement failed with the wrong message", output)
        else:
            note("ok: a `context` that contradicts the `line` hint fails, and names the arm")

        write_survivors(entry(line=2, source="emitMetric(registerCount);",
                              context="void registerModel() {"))
        rc, output = run()
        if rc != 0:
            fail("a citation disambiguated by `context` did not pass", output)
        elif "ok: 1 structured citation" not in output:
            fail("a disambiguated citation was not counted as audited", output)
        else:
            note("ok: a `context` resolves an otherwise ambiguous citation, and it counts")

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
    survivors = argv[0] if argv else SURVIVORS
    return check(survivors, os.getcwd())


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))

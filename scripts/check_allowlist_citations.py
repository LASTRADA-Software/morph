#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Resolve every line-cited allowlist's citations, in one invocation.

Usage:
    python3 scripts/check_allowlist_citations.py [REPO_ROOT]
    python3 scripts/check_allowlist_citations.py --self-test

Why this exists (morph#705)
---------------------------
Three files in this repository cite source lines by `{file, line, source}` and
are audited by resolving that `source` text against the tree. Until this script
they were audited by *different CI jobs*, with no ordering between them:
`scripts/mutation_survivors.json` in drift-guard.yml's `mutation-regression-lint`
job, and `scripts/branch_partial_allowlist.json` inside ci.yml's coverage leg,
20 minutes into a build. So an author fixed the one CI told them about, pushed,
and learned about the other a cycle later.

That is not a hypothetical either. Three occurrences in one week:

  * morph#700 moved lines in two headers and discovered the two allowlists
    across two serial cycles.
  * morph#710's lane ran every allowlist locally *because it was told to*, then
    committed clang-tidy fixes that inserted **one line** above three citations
    and went red on two jobs. The gates were green when run and stale when
    pushed: running them was a manual act at a moment the author chose, and the
    commit came after.

The drift that produces this is an off-by-one, not a rewrite -- which is why
this script's self-test stales its fixtures by exactly one line. A checker
exercised only against large drifts would pass while handling only large
drifts.

What this aggregates, and what it deliberately does not
-------------------------------------------------------
Only the *citation resolution* -- "does this entry's `source` text still sit at
the line the entry names, and is that line decidable" -- which is common to all
three files and is check_branch_coverage.py's resolve_allowlist_source_line().

Every per-allowlist *site* check stays where it is, because each one needs
inputs this script deliberately has none of: whether a line is still a partial
branch needs the aggregated LCOV (check_branch_coverage.py), whether it is
still a throw/catch arm needs the site scan and the same LCOV
(check_error_path_coverage.py), and whether a survivor entry's document shape
is still auditable is check_mutation_survivors.py's own concern. This script
reads JSON and headers, nothing else, so it runs per-PR in drift-guard.yml in
seconds and reports on all three at once.

The registry, and why it is checked against the tree
-----------------------------------------------------
ALLOWLISTS below is a hand-written list, and a hand-written list of things to
audit is exactly what rots. So it is not trusted: `discover_allowlists()` walks
scripts/*.json for any document containing an entry shaped like a citation
(`file` + `line` + `source`) and fails when it finds one the registry does not
name. A fourth allowlist therefore cannot be added without this script
noticing -- which is the failure this script exists to prevent, one level up.

Finding zero citations across every registered allowlist is a failure, not a
pass, for the reason every other gate in drift-guard.yml states: a gate with
nothing left to check reports green exactly as loudly as one that checked
everything. An individual allowlist may legitimately be empty
(error_path_allowlist.json is, today) and is reported by name when it is.
"""

from __future__ import annotations

import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import check_branch_coverage  # noqa: E402 -- resolve_allowlist_source_line()
import check_mutation_survivors  # noqa: E402 -- find_entries()


def entries_array(document):
    """The `entries` array shape: branch_partial and error_path allowlists."""
    return [(f".entries[{index}]", entry)
            for index, entry in enumerate(document.get("entries", []))]


def walked_document(document):
    """The whole-document walk: mutation_survivors.json's triage classes.

    Shares check_mutation_survivors.find_entries() rather than reimplementing
    the walk, for the same reason this script shares the resolver: a second
    copy is how the defect gets another chance.
    """
    return check_mutation_survivors.find_entries(document)


# The line-cited allowlists this repository has, and how to find the citations
# in each. Audited against the tree by discover_allowlists(); see the docstring.
ALLOWLISTS = (
    ("scripts/branch_partial_allowlist.json", entries_array),
    ("scripts/error_path_allowlist.json", entries_array),
    ("scripts/mutation_survivors.json", walked_document),
)


def looks_like_a_citation(node):
    return (isinstance(node, dict)
            and "file" in node and "line" in node and "source" in node)


def document_cites_lines(document):
    """Does any node in this JSON document look like a line citation?"""
    if looks_like_a_citation(document):
        return True
    if isinstance(document, dict):
        return any(document_cites_lines(value) for value in document.values())
    if isinstance(document, list):
        return any(document_cites_lines(value) for value in document)
    return False


def discover_allowlists(repo_root):
    """Relative paths of every scripts/*.json that carries line citations."""
    found = []
    scripts_dir = os.path.join(repo_root, "scripts")
    for name in sorted(os.listdir(scripts_dir)):
        if not name.endswith(".json"):
            continue
        full = os.path.join(scripts_dir, name)
        try:
            with open(full, encoding="utf-8") as handle:
                document = json.load(handle)
        except (OSError, ValueError):
            continue
        if document_cites_lines(document):
            found.append(f"scripts/{name}")
    return found


def audit_pending_context(repo_root, cited_without_context, cited_at_all, failures):
    """Audit check_branch_coverage.PENDING_CONTEXT in both directions.

    A grandfathering list is a suppression unless it can only shrink, so each
    pin has to earn its place on every run:

      * its file must exist and its text must still appear more than once --
        otherwise the ambiguity it exempts is gone and the pin is noise;
      * some entry must still cite it -- otherwise the entry it was written for
        has been deleted or rewritten;
      * some entry citing it must still lack a `context` -- otherwise the
        migration this list exists to schedule has already happened.
    """
    for (path, wanted), reason in sorted(check_branch_coverage.PENDING_CONTEXT.items()):
        pin = f"{path} :: {wanted!r}"
        source_file = os.path.join(repo_root, path)
        if not os.path.exists(source_file):
            failures.append(
                f"PENDING_CONTEXT pins {pin}, but that file does not exist. Delete "
                f"the pin: it exempts nothing. ({reason})")
            continue
        with open(source_file, encoding="utf-8") as handle:
            occurrences = sum(1 for text in handle.read().splitlines()
                              if text.strip() == wanted)
        if occurrences < 2:
            failures.append(
                f"PENDING_CONTEXT pins {pin}, which now appears {occurrences} time(s) "
                f"in that file. The ambiguity it exempts is gone -- delete the pin "
                f"rather than leaving a carve-out nothing needs. ({reason})")
            continue
        if (path, wanted) not in cited_at_all:
            failures.append(
                f"PENDING_CONTEXT pins {pin}, which no allowlist entry cites any more. "
                f"Delete the pin. ({reason})")
            continue
        if (path, wanted) not in cited_without_context:
            failures.append(
                f"PENDING_CONTEXT pins {pin}, but every entry citing it now carries a "
                f"`context`. The migration is done -- delete the pin, so the next "
                f"ambiguous entry on this text is refused. ({reason})")


def check(repo_root, out=sys.stdout, allowlists=ALLOWLISTS, check_registry=True):
    failures = []
    audited = 0
    empty = []
    cited_at_all = set()
    cited_without_context = set()

    registered = {path for path, _ in allowlists}
    if check_registry:
        for path in discover_allowlists(repo_root):
            if path not in registered:
                failures.append(
                    f"{path} carries `{{file, line, source}}` citations and is not in "
                    f"this script's ALLOWLISTS registry, so nothing resolves them in "
                    f"one place -- which is the serial-discovery defect morph#705 "
                    f"closed. Add it.")

    for path, reader in allowlists:
        full = os.path.join(repo_root, path)
        if not os.path.exists(full):
            failures.append(
                f"{path} is in this script's registry and does not exist. Either it "
                f"was deleted -- remove it from ALLOWLISTS, deliberately -- or the "
                f"path is wrong and this gate has been auditing nothing.")
            continue
        with open(full, encoding="utf-8") as handle:
            document = json.load(handle)

        entries = reader(document)
        if not entries:
            empty.append(path)
            continue

        before = len(failures)
        for where, entry in entries:
            location = f"{path}{where}"
            file_path, hint = entry["file"], entry["line"]
            if not isinstance(hint, int):
                failures.append(f"{location} ({file_path}) has a non-integer `line` "
                                f"{hint!r}, which no resolver can check.")
                continue
            wanted = str(entry.get("source", "")).strip()
            if not wanted:
                failures.append(f"{location} ({file_path}:{hint}) cites a line with no "
                                f"`source` text, so it cannot be re-resolved when the "
                                f"code moves.")
                continue
            context = str(entry.get("context", "") or "").strip()
            cited_at_all.add((file_path, wanted))
            if not context:
                cited_without_context.add((file_path, wanted))
            resolved = check_branch_coverage.resolve_allowlist_source_line(
                repo_root, file_path, hint, wanted, path, failures, context=context)
            if resolved is not None:
                audited += 1
        print(f"{path}: {len(entries)} citation(s), "
              f"{len(failures) - before} unresolved", file=out)

    audit_pending_context(repo_root, cited_without_context, cited_at_all, failures)

    for path in empty:
        print(f"{path}: 0 citations (the file is present and its entry list is empty)",
              file=out)

    if audited == 0 and not failures:
        failures.append(
            "no allowlist in this repository carries a single resolvable citation. "
            "A gate with nothing left to check reports green exactly as loudly as one "
            "that checked everything.")

    if failures:
        print(f"\n{len(failures)} citation(s) across "
              f"{len(allowlists)} allowlist(s) do not resolve:\n", file=out)
        for failure in failures:
            print(f"  - {failure}", file=out)
        print(
            "\nEvery one of these is printed by this single invocation, deliberately: "
            "fixing\nthem one CI cycle at a time is the defect morph#705 closed. Update "
            "the `line`\nhints (or add the `context` a repeated `source` needs), then "
            "run this again\nbefore committing -- an edit that inserts one line above a "
            "citation stales it.",
            file=out)
        return 1

    print(f"\nok: {audited} citation(s) across {len(allowlists)} allowlist(s) resolve "
          f"to the line they name.", file=out)
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

    # The fixture tree is not this repository, so the real pins -- which name
    # real headers -- would every one of them fail their "the file exists"
    # direction here. Swap the whole constant for the duration and restore it,
    # rather than testing the gate against a tree it was not written about.
    saved_pins = dict(check_branch_coverage.PENDING_CONTEXT)
    check_branch_coverage.PENDING_CONTEXT.clear()

    work = tempfile.mkdtemp()
    try:
        root = os.path.join(work, "repo")
        os.makedirs(os.path.join(root, "scripts"))
        os.makedirs(os.path.join(root, "include", "morph", "core"))
        header = os.path.join(root, "include", "morph", "core", "x.hpp")

        # Two citations in one header, in two different allowlists -- the shape
        # morph#705 is about. Both are staled together below, by one line each.
        body = (
            "void f() {\n"
            "    out.reserve(text.size());\n"   # line 2  (allowlist A)
            "}\n"
            "void g() {\n"
            "    if (deadline && scheduler) {\n"  # line 5  (allowlist B)
            "    }\n"
            "}\n"
        )

        def write_header(text):
            with open(header, "w", encoding="utf-8") as handle:
                handle.write(text)

        alpha = os.path.join(root, "scripts", "alpha_allowlist.json")
        beta = os.path.join(root, "scripts", "beta_allowlist.json")

        def write(path, document):
            with open(path, "w", encoding="utf-8") as handle:
                json.dump(document, handle)

        def write_both(alpha_line=2, beta_line=5):
            write(alpha, {"entries": [{
                "file": "include/morph/core/x.hpp", "line": alpha_line,
                "source": "out.reserve(text.size());",
                "reason": "fixture"}]})
            write(beta, {"classes": {"equivalent": {"entries": [{
                "file": "include/morph/core/x.hpp", "line": beta_line,
                "source": "if (deadline && scheduler) {",
                "reason": "fixture"}]}}})

        registry = (("scripts/alpha_allowlist.json", entries_array),
                    ("scripts/beta_allowlist.json", walked_document))

        def run(check_registry=False):
            buffer = io.StringIO()
            code = check(root, out=buffer, allowlists=registry,
                         check_registry=check_registry)
            return code, buffer.getvalue()

        # 1. Both citations correct -> one invocation, one green answer.
        write_header(body)
        write_both()
        code, output = run()
        if code == 0 and "ok: 2 citation(s) across 2 allowlist(s)" in output:
            note("ok: two allowlists resolve in a single invocation")
        else:
            fail("a tree with two correct citations did not pass", output)

        # 2. THE CASE morph#705 IS ABOUT. One line inserted at the top stales a
        #    citation in *each* allowlist, by exactly one -- an off-by-one, which
        #    is what a real edit produces. One invocation must name both. A
        #    checker that reported only the first would leave the second for the
        #    next CI cycle, which is the defect.
        write_header("// one inserted line\n" + body)
        code, output = run()
        named_alpha = "include/morph/core/x.hpp:2 has moved to line 3" in output
        named_beta = "include/morph/core/x.hpp:5 has moved to line 6" in output
        if code != 0 and named_alpha and named_beta:
            note("ok: one-line drift in both allowlists is named by one invocation")
        elif code != 0 and (named_alpha or named_beta):
            fail("only one of two simultaneously-staled citations was reported -- "
                 "the other would surface a CI cycle later", output)
        else:
            fail("simultaneous one-line drift was not reported", output)

        # 3. The registry is checked against the tree: a fourth allowlist that
        #    nothing audits is an error, not a silent omission.
        write_header(body)
        write_both()
        write(os.path.join(root, "scripts", "gamma_allowlist.json"), {"entries": [{
            "file": "include/morph/core/x.hpp", "line": 2,
            "source": "out.reserve(text.size());", "reason": "fixture"}]})
        code, output = run(check_registry=True)
        if code != 0 and "not in this script's ALLOWLISTS registry" in output:
            note("ok: an unregistered line-cited allowlist fails the registry check")
        else:
            fail("an unregistered allowlist was not noticed", output)
        os.remove(os.path.join(root, "scripts", "gamma_allowlist.json"))

        # 4. A registered allowlist that has vanished is an error too -- the
        #    mirror direction, which is what stops rule 3 being satisfiable by
        #    deleting things.
        os.remove(beta)
        code, output = run()
        if code != 0 and "has been auditing nothing" in output:
            note("ok: a registered allowlist that no longer exists fails")
        else:
            fail("a missing registered allowlist was skipped", output)

        # 5. Every allowlist empty -> refused. An aggregate over nothing is the
        #    failure mode this repository names first.
        write(alpha, {"entries": []})
        write(beta, {"classes": {}})
        code, output = run()
        if code != 0 and "reports green exactly as loudly" in output:
            note("ok: an aggregate with no citations left to resolve is refused")
        else:
            fail("an empty aggregate reported success", output)

        # 6. One empty allowlist among populated ones is legitimate (this is
        #    error_path_allowlist.json today) and is reported by name rather
        #    than passing silently.
        write_header(body)
        write_both()
        write(alpha, {"entries": []})
        code, output = run()
        if code == 0 and "scripts/alpha_allowlist.json: 0 citations" in output:
            note("ok: an empty allowlist among populated ones is reported, not failed")
        else:
            fail("an empty allowlist was not reported by name", output)

        # ── PENDING_CONTEXT, audited in both directions (morph#701) ──────────
        write_both()
        ambiguous_body = (
            "void handler() {\n"
            "    out.reserve(text.size());\n"   # line 2
            "}\n"
            + "// filler\n" * 50 +
            "void fallback() {\n"
            "    out.reserve(text.size());\n"   # line 55
            "}\n"
            "void g() {\n"
            "    if (deadline && scheduler) {\n"
            "    }\n"
            "}\n"
        )
        pin = ("include/morph/core/x.hpp", "out.reserve(text.size());")

        def with_pin(reason="fixture pin"):
            check_branch_coverage.PENDING_CONTEXT[pin] = reason

        def without_pin():
            check_branch_coverage.PENDING_CONTEXT.pop(pin, None)

        try:
            # 7. A pin whose text is no longer ambiguous is an error: it exempts
            #    nothing, and a carve-out that outlives its cause is a
            #    suppression.
            write_header(body)
            write_both(beta_line=5)
            with_pin()
            code, output = run()
            if code != 0 and "The ambiguity it exempts is gone" in output:
                note("ok: a PENDING_CONTEXT pin whose text became unique is refused")
            else:
                fail("an obsolete PENDING_CONTEXT pin was accepted", output)

            # 8. A pin nothing cites is an error -- the direction that stops the
            #    list outliving the entries it was written for.
            write_header(ambiguous_body)
            write(alpha, {"entries": []})
            write(beta, {"classes": {"equivalent": {"entries": [{
                "file": "include/morph/core/x.hpp", "line": 58,
                "source": "if (deadline && scheduler) {",
                "reason": "fixture"}]}}})
            code, output = run()
            if code != 0 and "no allowlist entry cites any more" in output:
                note("ok: a PENDING_CONTEXT pin nothing cites is refused")
            else:
                fail("an uncited PENDING_CONTEXT pin was accepted", output)

            # 9. A pin every citing entry has already migrated past is an error
            #    too, so the list shrinks the moment the work is done rather
            #    than when someone remembers.
            write(alpha, {"entries": [{
                "file": "include/morph/core/x.hpp", "line": 2,
                "source": "out.reserve(text.size());",
                "context": "void handler() {",
                "reason": "fixture"}]})
            code, output = run()
            if code != 0 and "The migration is done" in output:
                note("ok: a PENDING_CONTEXT pin every entry has migrated past is refused")
            else:
                fail("a redundant PENDING_CONTEXT pin was accepted", output)

            # 10. And with the pin gone, the migrated entry stands on its own.
            without_pin()
            code, output = run()
            if code == 0:
                note("ok: a migrated entry resolves with no pin at all")
            else:
                fail("a migrated entry did not resolve once its pin was deleted", output)
        finally:
            without_pin()

    finally:
        shutil.rmtree(work, ignore_errors=True)
        check_branch_coverage.PENDING_CONTEXT.clear()
        check_branch_coverage.PENDING_CONTEXT.update(saved_pins)

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

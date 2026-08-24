#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Check that a scenario's assertions are load-bearing.

A scenario that passes proves nothing until you know it can fail. This flips
one assertion at a time — ``expect ok``↔``expect err``, ``==``↔``!=``,
``~``↔``!~`` — reruns the scenario against the same server, and reports any
mutant that still passed. A surviving mutant is an assertion that measures
nothing.

Standard library only. Requires a running server, exactly like the runner does.
"""

from __future__ import annotations

import argparse
import pathlib
import subprocess
import sys
import tempfile

RUNNER = pathlib.Path(__file__).with_name("morph_scenario.py")


def mutants(lines: list[str]) -> list[tuple[int, str, str]]:
    """Returns ``(line index, mutated line, description)`` for every flip."""
    out: list[tuple[int, str, str]] = []
    for index, line in enumerate(lines):
        stripped = line.strip()
        if not stripped.startswith("expect "):
            continue
        if stripped.startswith("expect ok"):
            out.append((index, line.replace("expect ok", "expect err", 1), "kind ok->err"))
        elif stripped.startswith("expect err"):
            out.append((index, line.replace("expect err", "expect ok", 1), "kind err->ok"))
        for original, flipped in ((" == ", " != "), (" !~ ", " ~ "), (" ~ ", " !~ ")):
            if original in line:
                out.append(
                    (index, line.replace(original, flipped, 1), f"op {original.strip()}->{flipped.strip()}")
                )
    return out


def main(argv: list[str] | None = None) -> int:
    """Runs every mutant and reports survivors. Returns a process exit code."""
    parser = argparse.ArgumentParser(
        prog="mutate_scenario.py",
        description="Flip one assertion at a time and confirm the scenario notices.",
    )
    parser.add_argument("scenario", help="path to the scenario file")
    parser.add_argument("--server", default="", help="ws://host:port passed through to the runner")
    parser.add_argument("--timeout", type=float, default=10.0, help="per-read timeout passed through")
    args = parser.parse_args(argv)

    source = pathlib.Path(args.scenario).read_text(encoding="utf-8").splitlines()
    todo = mutants(source)
    if not todo:
        print(f"{args.scenario}: no assertions to mutate", file=sys.stderr)
        return 2

    survivors: list[tuple[int, str, str]] = []
    with tempfile.TemporaryDirectory() as workdir:
        for number, (index, mutated, what) in enumerate(todo):
            lines = list(source)
            lines[index] = mutated
            path = pathlib.Path(workdir) / f"mutant_{number}.scenario"
            path.write_text("\n".join(lines) + "\n", encoding="utf-8")
            command = [sys.executable, str(RUNNER), str(path), "--timeout", str(args.timeout)]
            if args.server:
                command += ["--server", args.server]
            code = subprocess.run(command, capture_output=True, text=True).returncode
            if code == 1:
                verdict = "caught"
            else:
                verdict = f"SURVIVED (exit {code})"
                survivors.append((index + 1, what, mutated.strip()))
            print(f"  line {index + 1:>4}  {what:<18} {verdict}")

    print(f"\n{args.scenario}: {len(todo)} mutants, {len(todo) - len(survivors)} caught, {len(survivors)} survived")
    for line_no, what, text in survivors:
        print(f"  SURVIVOR line {line_no} ({what}): {text}")
    return 1 if survivors else 0


if __name__ == "__main__":
    sys.exit(main())

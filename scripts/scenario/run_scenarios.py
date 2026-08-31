#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Runs a whole rung's scenario directory against a server it starts itself.

`morph_scenario.py` owns no server lifecycle, by design: it drives a server
someone else started, and starting one, waiting for its port and tearing it
down belong to whatever runs it. That is the right boundary for one scenario
and the wrong one for a corpus. A directory of a dozen files, each of which
`mutate_scenario.py` must also rerun once per assertion, cannot be driven by
hand. This is the someone else.

**One server per rung directory, not per file.** Every scenario in the corpus
is required to be re-runnable against a database it has already run on -- no
assertion may depend on an empty database. A shared server is what that
requirement buys; `--twice` is what proves it was paid, by running the whole
directory a second time against the same database and demanding the same
result.

Standard library only: no build step, no dependencies, no `pip install`.

    python3 scripts/scenario/run_scenarios.py                  # every rung
    python3 scripts/scenario/run_scenarios.py --rung pastebin
    python3 scripts/scenario/run_scenarios.py --rung ledger --twice --mutate
"""

from __future__ import annotations

import argparse
import dataclasses
import os
import pathlib
import re
import shutil
import sqlite3
import signal
import subprocess
import sys
import tempfile
import time

RUNNER = pathlib.Path(__file__).with_name("morph_scenario.py")
MUTATOR = pathlib.Path(__file__).with_name("mutate_scenario.py")
SCENARIOS = pathlib.Path(__file__).with_name("scenarios")

#: Scenario files that are *meant* to fail. The driver inverts their verdict:
#: a run in which one of these passes is a failure, because the failure report
#: they exist to demonstrate has stopped being demonstrated.
EXPECTED_TO_FAIL = frozenset({"broken-on-purpose.scenario"})

#: How long to wait for a server to announce its port before giving up. Not a
#: wall-clock assertion -- nothing about a scenario's *result* depends on it;
#: it only bounds how long this tool hangs when a server fails to start.
STARTUP_TIMEOUT_SECONDS = 30.0

#: The one line of server output this tool has to understand. Four rungs print
#: `listening on ws://127.0.0.1:<port>`; pastebin prints `listening on port
#: <port>`. Anchored on those two forms specifically so kanban's *second*
#: line -- its attachment side channel, `listening on http://127.0.0.1:<port>`
#: -- cannot be mistaken for the WebSocket one and send every scenario to the
#: wrong socket.
PORT_LINE = re.compile(r"listening on (?:ws://[^\s:]+:|port )(\d+)\s*$")


def parse_port(line: str) -> int | None:
    """Reads a server's WebSocket port out of one line of its stdout.

    @param line One line of server output, newline optional.
    @return The port, or `None` if this is not the WebSocket announcement --
            including for kanban's attachment side channel, which announces an
            `http://` port on its own line.
    """
    match = PORT_LINE.search(line.rstrip("\n"))
    return int(match.group(1)) if match else None


@dataclasses.dataclass(frozen=True)
class RungSpec:
    """How to start one rung's server.

    Every field is read out of that rung's own `src/server/main.cpp`; nothing
    here is a convention this tool invented. `token_secret_var` is `None` for
    the two rungs that mint no bearer tokens.
    """

    binary: str
    port_var: str
    db_var: str
    token_secret_var: str | None = None
    extra_zero_ports: tuple[str, ...] = ()
    seed_sql: tuple[str, ...] = ()

    def environment(self, db_path: pathlib.Path) -> dict[str, str]:
        """Builds the process environment for one run.

        The port variable is bound to `0` so the OS picks a free port and the
        server prints it -- runs cannot collide, and nothing in this tool has
        to guess or reserve a port number.

        @param db_path Where this run's SQLite database should live.
        @return The full environment, inheriting the caller's.
        """
        env = dict(os.environ)
        env[self.port_var] = "0"
        env[self.db_var] = f"DRIVER=SQLite3;Database={db_path};Timeout=5000"
        env["QT_QPA_PLATFORM"] = "offscreen"
        if self.token_secret_var is not None:
            env[self.token_secret_var] = "scenario-corpus-secret"
        for name in self.extra_zero_ports:
            env[name] = "0"
        return env

    def seed(self, db_path: pathlib.Path) -> None:
        """Inserts the rows no wire action can create.

        Ledger is the one rung with a root entity its own action surface
        cannot produce: `ledgers` rows are created by no registered action, so
        `OpenAccount ledgerId=1` against a fresh database is refused with "no
        such ledger" and every ledger journey is unreachable. Seeding that one
        row is fixture setup, not a scenario step -- a scenario asserting it
        could exist would be asserting something no client can do.

        Runs *after* the server has started, so the migrations that create the
        tables have already run. `INSERT OR IGNORE` keeps this idempotent, so
        a rerun against a database that already has the row is a no-op rather
        than a constraint violation.

        @param db_path The SQLite file the server was pointed at.
        """
        if not self.seed_sql:
            return
        connection = sqlite3.connect(str(db_path))
        try:
            for statement in self.seed_sql:
                connection.execute(statement)
            connection.commit()
        finally:
            connection.close()


#: Exactly the rungs `scenario_coverage.py` measures (`SERVER_RUNGS`), which
#: is exactly the set with a `src/server/main.cpp`. The self-test asserts the
#: two lists agree, so a rung added there without being added here fails
#: loudly rather than being skipped in silence.
RUNGS: dict[str, RungSpec] = {
    "pastebin": RungSpec(
        binary="ladder_pastebin_server", port_var="PASTEBIN_PORT", db_var="PASTEBIN_DB"
    ),
    "bookmarks": RungSpec(
        binary="ladder_bookmarks_server",
        port_var="BOOKMARKS_PORT",
        db_var="BOOKMARKS_DB",
        token_secret_var="BOOKMARKS_TOKEN_SECRET",
    ),
    "polls": RungSpec(binary="ladder_polls_server", port_var="POLLS_PORT", db_var="POLLS_DB"),
    "kanban": RungSpec(
        binary="ladder_kanban_server",
        port_var="KANBAN_PORT",
        db_var="KANBAN_DB",
        token_secret_var="KANBAN_TOKEN_SECRET",
        extra_zero_ports=("KANBAN_ATTACHMENT_PORT",),
    ),
    "ledger": RungSpec(
        binary="ladder_ledger_server",
        port_var="LEDGER_PORT",
        db_var="LEDGER_DB",
        token_secret_var="LEDGER_TOKEN_SECRET",
        seed_sql=(
            "INSERT OR IGNORE INTO ledgers (id, name) VALUES (1, 'Scenario book')",
            "INSERT OR IGNORE INTO ledgers (id, name) VALUES (2, 'Second book')",
        ),
    ),
}


class DriverError(Exception):
    """A failure of this tool rather than of a scenario."""


def repo_root() -> pathlib.Path:
    """Returns the repository root, two directories above this file."""
    return pathlib.Path(__file__).resolve().parents[2]


def find_server(rung: str, build_dir: pathlib.Path | None) -> pathlib.Path:
    """Locates one rung's server binary.

    With an explicit `build_dir`, looks only there, so a caller who names a
    build gets that build or an error -- never a stale binary from a different
    one. Without it, searches every `build/*/examples/<rung>/` and refuses
    when more than one candidate exists rather than picking arbitrarily.

    @param rung      The rung name.
    @param build_dir An explicit build directory, or `None` to search.
    @return The path to `ladder_<rung>_server`.
    @throws DriverError if there is not exactly one candidate.
    """
    name = RUNGS[rung].binary
    if build_dir is not None:
        candidate = build_dir / "examples" / rung / name
        if not candidate.is_file():
            raise DriverError(f"{candidate} does not exist -- is {rung}'s server built?")
        return candidate
    found = sorted(repo_root().glob(f"build/*/examples/{rung}/{name}"))
    if not found:
        raise DriverError(
            f"no {name} under build/*/examples/{rung}/. Build it with:\n"
            f"  cmake --preset clang-release -B build/ladder-srv -DMORPH_BUILD_LADDER=ON "
            f"-DMORPH_LADDER_RUNGS=all -DMORPH_BUILD_NET=ON -DMORPH_BUILD_QT=ON "
            f"-DMORPH_BUILD_TESTS=ON\n"
            f"  cmake --build build/ladder-srv --target {name}"
        )
    if len(found) > 1:
        listed = "\n  ".join(str(path) for path in found)
        raise DriverError(f"several candidates for {name}; pass --build-dir:\n  {listed}")
    return found[0]


class ServerProcess:
    """A running rung server, with its port, as a context manager.

    Blocks on the server's stdout until it announces a WebSocket port, so
    there is no sleep-and-hope: the scenario run starts when the server says
    it is listening, and not before.
    """

    def __init__(self, rung: str, binary: pathlib.Path, db_path: pathlib.Path) -> None:
        self.rung = rung
        self.binary = binary
        self.db_path = db_path
        self.port = 0
        self._process: subprocess.Popen[str] | None = None
        self._log: list[str] = []

    def __enter__(self) -> "ServerProcess":
        self._process = subprocess.Popen(  # noqa: S603
            [str(self.binary)],
            env=RUNGS[self.rung].environment(self.db_path),
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            bufsize=1,
            start_new_session=True,
        )
        self.port = self._await_port()
        RUNGS[self.rung].seed(self.db_path)
        return self

    def __exit__(self, *_exc: object) -> None:
        process = self._process
        if process is None:
            return
        # The whole process group: a server that spawned helpers (kanban's
        # attachment side channel) must not outlive this block and hold its
        # port, which would make the next run fail for a reason that has
        # nothing to do with the scenario it was running.
        try:
            os.killpg(os.getpgid(process.pid), signal.SIGTERM)
        except (ProcessLookupError, PermissionError):
            process.terminate()
        try:
            process.wait(timeout=10)
        except subprocess.TimeoutExpired:
            try:
                os.killpg(os.getpgid(process.pid), signal.SIGKILL)
            except (ProcessLookupError, PermissionError):
                process.kill()
            process.wait(timeout=10)

    @property
    def url(self) -> str:
        """The `ws://` url scenarios should be pointed at."""
        return f"ws://127.0.0.1:{self.port}"

    def output(self) -> str:
        """Everything the server printed before it announced its port."""
        return "".join(self._log)

    def _await_port(self) -> int:
        process = self._process
        assert process is not None and process.stdout is not None
        deadline = time.monotonic() + STARTUP_TIMEOUT_SECONDS
        while time.monotonic() < deadline:
            line = process.stdout.readline()
            if not line:
                raise DriverError(
                    f"{self.rung} server exited before announcing a port:\n{self.output()}"
                )
            self._log.append(line)
            port = parse_port(line)
            if port is not None:
                return port
        raise DriverError(
            f"{self.rung} server printed no port within {STARTUP_TIMEOUT_SECONDS:.0f}s:\n"
            f"{self.output()}"
        )


def scenario_files(rung: str) -> list[pathlib.Path]:
    """Returns one rung's scenario files, in a stable order.

    @param rung The rung name.
    @return Every `*.scenario` directly under `scenarios/<rung>/`, sorted.
    @throws DriverError if the directory does not exist.
    """
    directory = SCENARIOS / rung
    if not directory.is_dir():
        raise DriverError(f"{directory} does not exist")
    return sorted(directory.glob("*.scenario"))


def _run(command: list[str], verbose: bool, show_failure: bool = True) -> bool:
    """Runs one subprocess, showing its output only when it matters.

    @param command      The argv to run.
    @param verbose      Let the child write straight to this tool's streams.
    @param show_failure Print a captured failure. `False` for a scenario that
                        is *meant* to fail, whose report is expected output
                        rather than a diagnostic anyone needs to read.
    """
    result = subprocess.run(  # noqa: S603
        command, capture_output=not verbose, text=True, check=False
    )
    if result.returncode != 0 and not verbose and show_failure:
        sys.stdout.write(result.stdout or "")
        sys.stdout.write(result.stderr or "")
    return result.returncode == 0


def run_one(path: pathlib.Path, url: str, verbose: bool, mutate: bool) -> bool:
    """Runs one scenario, and optionally proves its assertions can fail.

    A file named in `EXPECTED_TO_FAIL` has its verdict inverted: it exists to
    demonstrate a failure report, so it passing is the failure. Such a file is
    never mutated -- every mutant of a scenario that already fails also fails,
    which measures nothing.

    @param path    The scenario file.
    @param url     The `ws://` url of the running server.
    @param verbose Pass `--verbose` through to the runner.
    @param mutate  Also run `mutate_scenario.py` on a file that passed.
    @return Whether this file's verdict was the expected one.
    """
    inverted = path.name in EXPECTED_TO_FAIL
    command = [sys.executable, str(RUNNER), "--server", url]
    if verbose:
        command.append("--verbose")
    command.append(str(path))
    passed = _run(command, verbose, show_failure=not inverted)
    if inverted:
        if passed:
            print(f"  FAIL {path.name}: expected to fail, but passed")
            return False
        print(f"  ok   {path.name} (failed, as it is meant to)")
        return True
    if not passed:
        print(f"  FAIL {path.name}")
        return False
    if mutate:
        survived = not _run([sys.executable, str(MUTATOR), "--server", url, str(path)], verbose)
        if survived:
            print(f"  FAIL {path.name}: a mutant survived")
            return False
        print(f"  ok   {path.name} (+ mutants)")
        return True
    print(f"  ok   {path.name}")
    return True


def run_rung(
    rung: str,
    build_dir: pathlib.Path | None,
    verbose: bool,
    mutate: bool,
    twice: bool,
) -> bool:
    """Runs one rung's whole directory against one freshly started server.

    @param rung      The rung name.
    @param build_dir An explicit build directory, or `None` to search.
    @param verbose   Pass `--verbose` through to the runner.
    @param mutate    Mutation-test every file that passes.
    @param twice     Run the directory a second time against the same
                     database, which is what proves every file is re-runnable.
    @return Whether every file had its expected verdict, in every pass.
    """
    files = scenario_files(rung)
    if not files:
        print(f"{rung}: no scenario files")
        return True
    binary = find_server(rung, build_dir)
    workspace = pathlib.Path(tempfile.mkdtemp(prefix=f"morph-scenario-{rung}-"))
    try:
        with ServerProcess(rung, binary, workspace / f"{rung}.db") as server:
            print(f"{rung}: {len(files)} file(s) against {server.url}")
            ok = True
            for path in files:
                ok = run_one(path, server.url, verbose, mutate) and ok
            if twice and ok:
                print(f"{rung}: second pass, same database")
                for path in files:
                    ok = run_one(path, server.url, verbose, mutate=False) and ok
            return ok
    finally:
        shutil.rmtree(workspace, ignore_errors=True)


def main(argv: list[str] | None = None) -> int:
    """Command-line entry point.

    @param argv Arguments, defaulting to `sys.argv[1:]`.
    @return `0` if every scenario had its expected verdict, `1` if one did
            not, `2` if this tool could not run at all.
    """
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument(
        "--rung",
        action="append",
        choices=sorted(RUNGS),
        help="restrict to one rung; repeatable. Default: every rung.",
    )
    parser.add_argument(
        "--build-dir",
        type=pathlib.Path,
        default=None,
        help="where the ladder_<rung>_server binaries live; searched under build/*/ if omitted",
    )
    parser.add_argument(
        "--mutate",
        action="store_true",
        help="after a file passes, flip its assertions one at a time and fail on any survivor",
    )
    parser.add_argument(
        "--twice",
        action="store_true",
        help="run each directory a second time against the same database",
    )
    parser.add_argument(
        "-v", "--verbose", action="store_true", help="pass --verbose through to the runner"
    )
    args = parser.parse_args(argv)

    rungs = args.rung or [rung for rung in RUNGS if (SCENARIOS / rung).is_dir()]
    failed: list[str] = []
    try:
        for rung in rungs:
            if not run_rung(rung, args.build_dir, args.verbose, args.mutate, args.twice):
                failed.append(rung)
    except DriverError as error:
        print(f"run_scenarios: {error}", file=sys.stderr)
        return 2

    print()
    if failed:
        print(f"FAILED: {', '.join(failed)}")
        return 1
    print(f"every scenario passed in: {', '.join(rungs)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())

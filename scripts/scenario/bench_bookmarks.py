#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Concurrent-user load benchmark for the bookmarks server.

Starts a ``ladder_bookmarks_server`` on a fresh SQLite database and runs N
simulated users against it. Each user repeats one real-world **session**:

1. connect (TCP + WebSocket upgrade + ``hello``),
2. sign in (``Login`` on ``Bookmarks_AuthModel``, then ``BookmarkModel``
   registered under the returned token),
3. open the collection (``ListBookmarks``, the first page, as the client's
   main screen does),
4. a random 5-10 actions (``--actions``) drawn from a weighted mix, with a
   random think time between them (``--think-ms``),
5. disconnect.

and then starts the next session as a returning user. Each user signs in as one
of its own ``--accounts-per-user`` accounts, so collections persist across
sessions and no two concurrent users share one.

What it reports, per user count:

* **sessions/s and actions/s** completed in the measured window;
* **latency** per step, as the client saw it -- connect, sign-in, every action;
* **failures** -- refused requests and lost connections, by step;
* **server CPU** -- CPU seconds the server spent per wall second (``1.00`` is
  one core busy) and per action, read from ``/proc/<pid>/stat``;
* **server memory** -- RSS idle, at the window's start and end, and its peak,
  from ``/proc/<pid>/status``;
* **client CPU**, so a run where the load generator, not the server, was the
  bottleneck says so.

``--tracy DIR`` profiles the run: point ``--build-dir`` at a server built with
``-DMORPH_ENABLE_TRACY=ON -DMORPH_TRACY_ON_DEMAND=OFF``, and ``tracy-capture``
records it from start-up to exit into ``DIR/bookmarks-<users>.tracy``, whose
zone statistics ``tracy-csvexport`` then writes beside it and summarises.
Instrumentation costs time, so take throughput figures from an uninstrumented
build and the profile from this one.

``--sweep 16,64,256`` runs one fresh server per count and ends with a capacity
verdict: the largest count whose action p99 stays within ``--slo-p99-ms`` and
whose failure rate stays within ``--slo-failure-rate``.

Users are asyncio tasks, one event loop per process, spread over ``--procs``
processes; each speaks the wire protocol directly over a WebSocket of its own,
implemented here from the spec as ``morph_scenario.py``'s client is. Linux
only.

    python3 scripts/scenario/bench_bookmarks.py --users 32 --duration 30
    python3 scripts/scenario/bench_bookmarks.py --sweep 8,32,128,256,320 --json out.json
"""

from __future__ import annotations

import argparse
import asyncio
import base64
import dataclasses
import hashlib
import json
import multiprocessing
import os
import pathlib
import random
import resource
import shutil
import socket
import sqlite3
import struct
import subprocess
import sys
import tempfile
import threading
import time
from typing import Any

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))

from morph_scenario import Reply, TransportError, make_envelope, parse_ws_url  # noqa: E402
from run_scenarios import DriverError, ServerProcess, find_server  # noqa: E402

#: Relative weights of the in-session actions. Reads dominate, as they do for a
#: bookmark manager in use; every write is one a user makes by hand.
DEFAULT_MIX: dict[str, int] = {
    "CreateBookmark": 20,
    "GetBookmark": 20,
    "ListBookmarks": 25,
    "EditBookmark": 10,
    "ArchiveBookmark": 5,
    "UnarchiveBookmark": 5,
    "DeleteBookmark": 5,
    "GetChangesSince": 10,
}

#: Steps that are not in the mix but every session takes.
SESSION_STEPS = ("Connect", "SignIn", "Seed", "OpenCollection")

#: Actions that name an existing bookmark.
NEEDS_ID = frozenset({"GetBookmark", "EditBookmark", "ArchiveBookmark", "UnarchiveBookmark", "DeleteBookmark"})

TAGS = ("cpp", "python", "rust", "news", "recipes", "music", "reading", "work")

_CLK_TCK = os.sysconf("SC_CLK_TCK")


# ─────────────────────────────────────────────────────────────────────────────
# Server sampling (/proc)
# ─────────────────────────────────────────────────────────────────────────────


@dataclasses.dataclass
class ProcSample:
    """One reading of a process's CPU time and memory."""

    wall: float
    cpu_seconds: float
    rss_kib: int
    hwm_kib: int
    threads: int


def sample_process(pid: int) -> ProcSample:
    """Reads @p pid's user+system CPU time, RSS, peak RSS and thread count.

    @param pid The process to read.
    @return The reading, stamped with `time.time()`.
    """
    wall = time.time()
    with open(f"/proc/{pid}/stat", encoding="ascii") as stat:
        # Field 2 is the command name in parentheses and may contain spaces, so
        # split after its closing parenthesis; utime and stime are fields 14
        # and 15, i.e. indices 11 and 12 of what follows.
        fields = stat.read().rpartition(")")[2].split()
    cpu = (int(fields[11]) + int(fields[12])) / _CLK_TCK
    rss = hwm = threads = 0
    with open(f"/proc/{pid}/status", encoding="ascii") as status:
        for line in status:
            if line.startswith("VmRSS:"):
                rss = int(line.split()[1])
            elif line.startswith("VmHWM:"):
                hwm = int(line.split()[1])
            elif line.startswith("Threads:"):
                threads = int(line.split()[1])
    return ProcSample(wall=wall, cpu_seconds=cpu, rss_kib=rss, hwm_kib=hwm, threads=threads)


def host_busy_seconds() -> float:
    """CPU seconds every core of the host has spent busy since boot, from
    `/proc/stat`: everything but idle and iowait."""
    with open("/proc/stat", encoding="ascii") as stat:
        fields = [int(value) for value in stat.readline().split()[1:]]
    idle = fields[3] + (fields[4] if len(fields) > 4 else 0)
    return (sum(fields[:8]) - idle) / _CLK_TCK


def thread_cpu(pid: int) -> dict[str, float]:
    """CPU seconds per thread of @p pid, keyed by "name/tid"."""
    result = {}
    for task in pathlib.Path(f"/proc/{pid}/task").iterdir():
        try:
            name = (task / "comm").read_text().strip()
            fields = (task / "stat").read_text().rpartition(")")[2].split()
        except OSError:
            continue  # the thread exited between listing and reading
        result[f"{name}/{task.name}"] = (int(fields[11]) + int(fields[12])) / _CLK_TCK
    return result


class Sampler:
    """Samples a process on a fixed period in a background thread."""

    def __init__(self, pid: int, period: float) -> None:
        self.pid = pid
        self.period = period
        self.samples: list[ProcSample] = []
        self._stop = threading.Event()
        self._thread = threading.Thread(target=self._run, daemon=True)

    def __enter__(self) -> "Sampler":
        self._thread.start()
        return self

    def __exit__(self, *_exc: object) -> None:
        self._stop.set()
        self._thread.join()

    def _run(self) -> None:
        while not self._stop.is_set():
            try:
                self.samples.append(sample_process(self.pid))
            except (OSError, ValueError, IndexError):
                return
            self._stop.wait(self.period)


# ─────────────────────────────────────────────────────────────────────────────
# Tracy capture
# ─────────────────────────────────────────────────────────────────────────────


def free_port() -> int:
    with socket.socket() as probe:
        probe.bind(("127.0.0.1", 0))
        return int(probe.getsockname()[1])


class TracyCapture:
    """Records the server with `tracy-capture` for the whole of its life.

    The server must be built with `MORPH_TRACY_ON_DEMAND=OFF`: its Tracy client
    then queues events from the first instruction, and with `TRACY_NO_EXIT=1`
    it holds its exit until the capture has drained them, so the trace covers
    start-up, the run and shutdown. The port is chosen here and handed to the
    server through `TRACY_PORT`, so a stray Tracy client elsewhere on the
    machine cannot be the one recorded.
    """

    def __init__(self, tools: tuple[pathlib.Path, pathlib.Path], out_dir: pathlib.Path, users: int) -> None:
        self.capture_tool, self.export_tool = tools
        self.trace = out_dir / f"bookmarks-{users}.tracy"
        self.csv = out_dir / f"bookmarks-{users}-zones.csv"
        self.port = free_port()
        self._process: subprocess.Popen[bytes] | None = None

    def server_environment(self) -> dict[str, str]:
        return {"TRACY_PORT": str(self.port), "TRACY_NO_EXIT": "1"}

    def start(self) -> None:
        self._process = subprocess.Popen(  # noqa: S603
            [str(self.capture_tool), "-o", str(self.trace), "-a", "127.0.0.1", "-p", str(self.port), "-f"],
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

    def finish(self) -> list[dict[str, Any]]:
        """Waits for the capture to end (it ends when the server does), exports
        the zone statistics, and returns them ordered by total time."""
        if self._process is None:
            return []
        try:
            self._process.wait(timeout=120)
        except subprocess.TimeoutExpired:
            self._process.kill()
            raise DriverError(f"tracy-capture did not finish within 120s; {self.trace} is incomplete") from None
        if not self.trace.is_file():
            raise DriverError("tracy-capture wrote no trace -- was the server built with MORPH_ENABLE_TRACY=ON?")
        with self.csv.open("w") as out:
            subprocess.run([str(self.export_tool), str(self.trace)], stdout=out, check=True)  # noqa: S603
        rows = []
        with self.csv.open() as data:
            header = data.readline().strip().split(",")
            for line in data:
                # The source file column can hold commas; the numeric columns
                # are the last ones, so read the row from both ends.
                cells = line.rstrip("\n").split(",")
                head = dict(zip(header[:1], cells[:1]))
                tail = dict(zip(header[-8:], cells[-8:]))
                rows.append({"name": head.get("name", "?"), **{k: float(v) for k, v in tail.items()}})
        rows.sort(key=lambda row: row.get("total_ns", 0.0), reverse=True)
        if not rows:
            raise DriverError(f"{self.trace} holds no zones -- a profile that recorded nothing proves nothing")
        return rows


def resolve_tracy_tools(given: str) -> tuple[pathlib.Path, pathlib.Path]:
    found = []
    for name in ("tracy-capture", "tracy-csvexport"):
        located = shutil.which(name, path=given or None) or shutil.which(name)
        if located is None:
            raise DriverError(f"{name} not found on PATH{' or in ' + given if given else ''}; build it from "
                              f"Tracy's capture/ and csvexport/ directories and pass --tracy-tools")
        found.append(pathlib.Path(located))
    return found[0], found[1]


# ─────────────────────────────────────────────────────────────────────────────
# One simulated user
# ─────────────────────────────────────────────────────────────────────────────


@dataclasses.dataclass
class Plan:
    """Everything a client process needs; picklable for `spawn`."""

    url: str
    run_id: str
    start_at: float  # time.time() at which every user starts its first session
    measure_from: float  # the measured window, in time.time()
    measure_until: float
    mix: dict[str, int]
    actions: tuple[int, int]
    think_ms: tuple[float, float]
    accounts_per_user: int
    collection: int
    timeout: float
    seed: int
    workload: str
    ramp: float
    seed_bookmarks: tuple[int, int]


class Stats:
    """What one process measured inside the window. Shared by its users, which
    all run on the process's one event loop, so it needs no lock."""

    def __init__(self, plan: Plan) -> None:
        self.plan = plan
        self.latencies: dict[str, list[int]] = {}
        self.failures: dict[str, int] = {}
        self.failure_samples: list[str] = []
        self.sessions = 0
        self.session_seconds: list[float] = []

    def in_window(self) -> bool:
        return self.plan.measure_from <= time.time() < self.plan.measure_until

    def ok(self, step: str, elapsed_ns: int) -> None:
        if self.in_window():
            self.latencies.setdefault(step, []).append(elapsed_ns)

    def fail(self, step: str, message: str) -> None:
        if self.in_window():
            self.failures[step] = self.failures.get(step, 0) + 1
            if len(self.failure_samples) < 10:
                self.failure_samples.append(f"{step}: {message}")

    def session_done(self, seconds: float) -> None:
        if self.in_window():
            self.sessions += 1
            self.session_seconds.append(seconds)


class StepFailed(Exception):
    """A step was refused or its connection lost; the session ends here."""


# ─────────────────────────────────────────────────────────────────────────────
# An asyncio WebSocket client
# ─────────────────────────────────────────────────────────────────────────────
#
# The same RFC 6455 client as morph_scenario.WebSocket, on asyncio streams: one
# event loop per process carries thousands of connections, and a reply wakes
# exactly the user waiting for it. A thread per user cannot do that at scale --
# with a thousand threads in one process, the wait for the interpreter lock
# after a reply arrives grows into tens of milliseconds that show up as
# latency, without showing up as CPU.

_WS_GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"
_OP_CONT, _OP_TEXT, _OP_BINARY, _OP_CLOSE, _OP_PING, _OP_PONG = 0x0, 0x1, 0x2, 0x8, 0x9, 0xA


def _mask(payload: bytes, key: bytes) -> bytes:
    """XORs @p payload with the repeating 4-byte @p key, as one big integer
    rather than byte by byte: the per-byte loop dominates a Python client's
    CPU at these request rates."""
    if not payload:
        return payload
    length = len(payload)
    stream = (key * (length // 4 + 1))[:length]
    return (int.from_bytes(payload, "big") ^ int.from_bytes(stream, "big")).to_bytes(length, "big")


class AsyncWebSocket:
    """One client connection: handshake, masked text frames out, text frames in."""

    def __init__(self, reader: asyncio.StreamReader, writer: asyncio.StreamWriter, url: str) -> None:
        self._reader = reader
        self._writer = writer
        self.url = url

    @classmethod
    async def connect(cls, url: str, timeout: float) -> "AsyncWebSocket":
        host, port, path = parse_ws_url(url)
        try:
            reader, writer = await asyncio.wait_for(asyncio.open_connection(host, port), timeout)
        except (OSError, asyncio.TimeoutError) as exc:
            raise TransportError(f"cannot connect to {url}: {exc or 'timed out'}") from exc
        # One small frame per request, then a wait for its reply: without
        # TCP_NODELAY, Nagle plus delayed ACK can add tens of milliseconds to
        # a round trip -- measuring the kernel, not the server.
        sock = writer.get_extra_info("socket")
        if sock is not None:
            sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        ws = cls(reader, writer, url)
        await asyncio.wait_for(ws._handshake(host, port, path), timeout)
        return ws

    async def _handshake(self, host: str, port: int, path: str) -> None:
        key = base64.b64encode(os.urandom(16)).decode("ascii")
        self._writer.write((f"GET {path} HTTP/1.1\r\nHost: {host}:{port}\r\nUpgrade: websocket\r\n"
                            f"Connection: Upgrade\r\nSec-WebSocket-Key: {key}\r\n"
                            "Sec-WebSocket-Version: 13\r\n\r\n").encode("ascii"))
        try:
            head = (await self._reader.readuntil(b"\r\n\r\n")).decode("latin-1")
        except (asyncio.IncompleteReadError, asyncio.LimitOverrunError, OSError) as exc:
            raise TransportError(f"{self.url}: handshake failed: {exc}") from exc
        if "101" not in head.split("\r\n", 1)[0]:
            raise TransportError(f"{self.url}: websocket upgrade refused: {head.splitlines()[0]}")
        expected = base64.b64encode(hashlib.sha1((key + _WS_GUID).encode()).digest()).decode()
        accepted = ""
        for line in head.split("\r\n")[1:]:
            name, _, value = line.partition(":")
            if name.strip().lower() == "sec-websocket-accept":
                accepted = value.strip()
        if accepted != expected:
            raise TransportError(f"{self.url}: bad Sec-WebSocket-Accept (got {accepted!r}, want {expected!r})")

    def _send_frame(self, opcode: int, payload: bytes) -> None:
        key = os.urandom(4)
        length = len(payload)
        if length < 126:
            header = struct.pack("!BB", 0x80 | opcode, 0x80 | length)
        elif length < (1 << 16):
            header = struct.pack("!BBH", 0x80 | opcode, 0x80 | 126, length)
        else:
            header = struct.pack("!BBQ", 0x80 | opcode, 0x80 | 127, length)
        self._writer.write(header + key + _mask(payload, key))

    async def send_text(self, text: str) -> None:
        self._send_frame(_OP_TEXT, text.encode("utf-8"))
        try:
            await self._writer.drain()
        except OSError as exc:
            raise TransportError(f"{self.url}: write failed: {exc}") from exc

    async def _read_frame(self) -> tuple[bool, int, bytes]:
        first, second = await self._reader.readexactly(2)
        length = second & 0x7F
        if length == 126:
            length = struct.unpack("!H", await self._reader.readexactly(2))[0]
        elif length == 127:
            length = struct.unpack("!Q", await self._reader.readexactly(8))[0]
        key = await self._reader.readexactly(4) if second & 0x80 else None
        payload = await self._reader.readexactly(length)
        if key is not None:
            payload = _mask(payload, key)
        return bool(first & 0x80), first & 0x0F, payload

    async def recv_text(self, timeout: float) -> str:
        """Reads frames until one complete text message is assembled; answers
        pings and ignores pongs, and turns a close into a `TransportError`."""
        message = b""
        try:
            while True:
                fin, opcode, payload = await asyncio.wait_for(self._read_frame(), timeout)
                if opcode == _OP_PING:
                    self._send_frame(_OP_PONG, payload)
                elif opcode == _OP_CLOSE:
                    raise TransportError(f"{self.url}: server closed the connection")
                elif opcode in (_OP_TEXT, _OP_CONT):
                    message += payload
                    if fin:
                        return message.decode("utf-8")
                elif opcode == _OP_BINARY:
                    raise TransportError(f"{self.url}: unexpected binary frame")
        except asyncio.TimeoutError as exc:
            raise TransportError(f"{self.url}: no data within {timeout}s") from exc
        except (asyncio.IncompleteReadError, OSError) as exc:
            raise TransportError(f"{self.url}: server closed the connection") from exc

    def close(self) -> None:
        """Sends a close frame and drops the connection. Never raises."""
        try:
            self._send_frame(_OP_CLOSE, struct.pack("!H", 1000))
            self._writer.close()
        except (OSError, RuntimeError):
            pass


class AsyncClient:
    """One connection's session and registered model, and the request verb."""

    def __init__(self, ws: AsyncWebSocket, timeout: float) -> None:
        self.ws = ws
        self.timeout = timeout
        self.session: dict[str, Any] = {"principal": "", "token": "", "requestId": "", "locale": "", "metadata": {}}
        self.model_type = ""
        self.model_id = 0
        self._next_call_id = 1

    def next_call_id(self) -> int:
        call_id = self._next_call_id
        self._next_call_id += 1
        return call_id

    async def rpc(self, envelope: dict[str, Any]) -> Reply:
        """Sends one envelope and reads the reply with the same `callId`."""
        await self.ws.send_text(json.dumps(envelope))
        raw = await self.ws.recv_text(self.timeout)
        try:
            decoded = json.loads(raw)
        except json.JSONDecodeError as exc:
            raise TransportError(f"server sent non-JSON: {raw[:200]!r}") from exc
        if decoded.get("callId") != envelope["callId"]:
            raise TransportError(f"reply callId {decoded.get('callId')} does not match {envelope['callId']}")
        body_text = decoded.get("body", "")
        body: Any = None
        if isinstance(body_text, str) and body_text:
            try:
                body = json.loads(body_text)
            except json.JSONDecodeError:
                body = body_text
        return Reply(envelope=decoded, body=body)


# ─────────────────────────────────────────────────────────────────────────────
# One simulated user
# ─────────────────────────────────────────────────────────────────────────────


class User:
    """One simulated person, running sessions back to back."""

    def __init__(self, plan: Plan, index: int, stats: Stats) -> None:
        self.plan = plan
        self.index = index
        self.stats = stats
        self.rng = random.Random(plan.seed * 1_000_003 + index)
        self.client: AsyncClient | None = None
        self.ids: list[int] = []
        self.archived: set[int] = set()
        self.full = False
        self.changes_cursor: Any = None
        self.counter = 0

    # ── wire helpers ─────────────────────────────────────────────────────────

    async def _rpc(self, step: str, envelope: dict[str, Any], record: bool = True) -> Reply:
        assert self.client is not None
        started = time.perf_counter_ns()
        try:
            reply = await self.client.rpc(envelope)
        except TransportError as exc:
            self.stats.fail(step, f"connection lost: {exc}")
            raise StepFailed from exc
        if reply.kind != "ok":
            self.stats.fail(step, reply.message)
            raise StepFailed
        if record:
            self.stats.ok(step, time.perf_counter_ns() - started)
        return reply

    async def _execute(self, step: str, action: str, body: dict[str, Any], record: bool = True) -> Reply:
        assert self.client is not None
        return await self._rpc(
            step,
            make_envelope(
                kind="execute",
                callId=self.client.next_call_id(),
                modelId=self.client.model_id,
                modelType=self.client.model_type,
                actionType=action,
                body=json.dumps(body, separators=(",", ":")),
                session=self.client.session,
            ),
            record,
        )

    async def _register(self, step: str, model: str) -> None:
        assert self.client is not None
        reply = await self._rpc(step, make_envelope(kind="register", callId=self.client.next_call_id(), typeId=model),
                                False)
        self.client.model_type = model
        self.client.model_id = int(reply.envelope["modelId"])

    # ── the session ──────────────────────────────────────────────────────────

    async def run(self) -> None:
        # Spread first sessions over the ramp so N users do not all connect in
        # the same millisecond, which no real population does.
        await asyncio.sleep(max(0.0, self.plan.start_at - time.time()) + self.rng.random() * self.plan.ramp)
        while time.time() < self.plan.measure_until:
            started = time.perf_counter()
            try:
                await self.session()
                self.stats.session_done(time.perf_counter() - started)
            except StepFailed:
                await asyncio.sleep(0.05)  # a failed session retries, but not in a hot loop
            finally:
                if self.client is not None:
                    self.client.ws.close()
                    self.client = None

    async def session(self) -> None:
        account = f"bench-{self.plan.run_id}-u{self.index}-a{self.rng.randrange(self.plan.accounts_per_user)}"

        started = time.perf_counter_ns()
        try:
            ws = await AsyncWebSocket.connect(self.plan.url, self.plan.timeout)
        except TransportError as exc:
            self.stats.fail("Connect", str(exc))
            raise StepFailed from exc
        self.client = AsyncClient(ws, self.plan.timeout)
        await self._rpc("Connect", make_envelope(kind="hello", callId=self.client.next_call_id(), protocolVersion=1),
                        False)
        self.stats.ok("Connect", time.perf_counter_ns() - started)

        # Sign-in, timed as the user experiences it: from "log in" to having a
        # bookmark model to talk to. The auth model is deregistered once it
        # has answered, because live instances count against the server's cap
        # and a signed-in client needs only its BookmarkModel.
        started = time.perf_counter_ns()
        await self._register("SignIn", "Bookmarks_AuthModel")
        login = await self._execute("SignIn", "Login", {"username": account}, False)
        self.client.session["principal"] = login.body["principal"]
        self.client.session["token"] = login.body["token"]
        if self.plan.workload == "framework":
            # The session's actions are further Logins on this same model: a
            # full round trip through decode, token verification, the model's
            # strand, dispatch and encode that never reaches the database.
            self.stats.ok("SignIn", time.perf_counter_ns() - started)
            for _ in range(self.rng.randint(*self.plan.actions)):
                await self._think()
                await self._execute("Login", "Login", {"username": account})
            return
        # A deregister is authorized like any other request, so it carries the
        # token the login just returned.
        await self._rpc("SignIn", make_envelope(kind="deregister", callId=self.client.next_call_id(),
                                                modelId=self.client.model_id, session=self.client.session), False)
        await self._register("SignIn", "BookmarkModel")
        self.stats.ok("SignIn", time.perf_counter_ns() - started)

        # Under the memory store every session starts with an empty
        # collection, so it first saves a few bookmarks, as a user filling a
        # new account would; the actions after it then have something to act
        # on.
        low, high = self.plan.seed_bookmarks
        for _ in range(self.rng.randint(low, high)):
            await self._execute("Seed", "CreateBookmark", self.body_for("CreateBookmark"))

        listing = await self._execute("OpenCollection", "ListBookmarks", {})
        self._adopt_listing(listing.body)

        for _ in range(self.rng.randint(*self.plan.actions)):
            await self._think()
            action = self.choose()
            body = self.body_for(action)
            reply = await self._execute(action, action, body)
            self.record(action, body, reply)

    async def _think(self) -> None:
        low, high = self.plan.think_ms
        delay = self.rng.uniform(low, high) / 1000.0
        if delay > 0:
            await asyncio.sleep(delay)

    def _adopt_listing(self, body: Any) -> None:
        """The client's view of its collection is whatever its first page shows."""
        rows = body.get("bookmarks", []) if isinstance(body, dict) else []
        self.ids = [int(row["id"]) for row in rows]
        self.archived = set()
        self.full = bool(body.get("nextCursor")) if isinstance(body, dict) else False
        self.full = self.full or len(self.ids) >= self.plan.collection
        self.changes_cursor = None

    # ── the action mix ───────────────────────────────────────────────────────

    def choose(self) -> str:
        """Picks the next action by weight. A create turns into a delete once
        the collection is at its bound, and an action that names a bookmark
        turns into a create while the client knows of none."""
        action = self.rng.choices(list(self.plan.mix), weights=list(self.plan.mix.values()))[0]
        if action == "CreateBookmark" and self.full and self.ids:
            return "DeleteBookmark"
        if action in NEEDS_ID and not self.ids:
            return "CreateBookmark"
        return action

    def body_for(self, action: str) -> dict[str, Any]:
        self.counter += 1
        stem = f"https://example.com/{self.plan.run_id}/{self.index}/{self.counter}"
        if action == "CreateBookmark":
            return {"url": stem, "title": f"Bookmark {self.counter}",
                    "description": "A page saved by the load benchmark.", "tags": self.rng.sample(TAGS, 2)}
        if action == "EditBookmark":
            return {"id": self.rng.choice(self.ids), "url": stem + "/edited", "title": f"Edited {self.counter}",
                    "description": "Edited by the load benchmark.", "notes": "",
                    "tags": self.rng.sample(TAGS, 2), "visibility": "Private"}
        if action == "ListBookmarks":
            return {"tag": self.rng.choice(TAGS)} if self.rng.random() < 0.5 else {}
        if action == "GetChangesSince":
            return {"since": self.changes_cursor} if self.changes_cursor else {}
        if action == "ArchiveBookmark":
            active = [i for i in self.ids if i not in self.archived]
            return {"id": self.rng.choice(active or self.ids)}
        if action == "UnarchiveBookmark":
            return {"id": self.rng.choice(sorted(self.archived) or self.ids)}
        return {"id": self.rng.choice(self.ids)}  # GetBookmark, DeleteBookmark

    def record(self, action: str, body: dict[str, Any], reply: Reply) -> None:
        """Keeps the client's view of its collection in step with the server."""
        if action == "CreateBookmark":
            self.ids.append(int(reply.body["id"]))
            self.full = self.full or len(self.ids) >= self.plan.collection
        elif action == "DeleteBookmark":
            self.ids.remove(body["id"])
            self.archived.discard(body["id"])
            self.full = False
        elif action == "ArchiveBookmark":
            self.archived.add(body["id"])
        elif action == "UnarchiveBookmark":
            self.archived.discard(body["id"])
        elif action == "GetChangesSince" and isinstance(reply.body, dict):
            self.changes_cursor = reply.body.get("asOf") or self.changes_cursor


def run_process(plan: Plan, first_user: int, users: int, results: Any) -> None:
    """One client process: @p users users on one event loop, then what they measured."""
    results.put(asyncio.run(_run_users(plan, first_user, users)))


async def _run_users(plan: Plan, first_user: int, users: int) -> dict[str, Any]:
    stats = Stats(plan)
    tasks = [asyncio.create_task(User(plan, first_user + i, stats).run()) for i in range(users)]
    await asyncio.sleep(max(0.0, plan.measure_from - time.time()))
    usage = resource.getrusage(resource.RUSAGE_SELF)
    cpu_before = usage.ru_utime + usage.ru_stime
    await asyncio.sleep(max(0.0, plan.measure_until - time.time()))
    usage = resource.getrusage(resource.RUSAGE_SELF)
    cpu = usage.ru_utime + usage.ru_stime - cpu_before
    # Users finish the request in flight and stop; one still waiting on a reply
    # past the timeout is cancelled rather than holding up the report.
    _, pending = await asyncio.wait(tasks, timeout=plan.timeout + 5)
    for task in pending:
        task.cancel()
    return {"latencies": stats.latencies, "failures": stats.failures, "failure_samples": stats.failure_samples,
            "sessions": stats.sessions, "session_seconds": stats.session_seconds, "cpu_seconds": cpu}


# ─────────────────────────────────────────────────────────────────────────────
# One run: a fresh server, N users
# ─────────────────────────────────────────────────────────────────────────────


def percentile(sorted_values: list[float], fraction: float) -> float:
    """Nearest-rank percentile of an ascending list."""
    if not sorted_values:
        return float("nan")
    rank = max(0, min(len(sorted_values) - 1, int(round(fraction * len(sorted_values))) - 1))
    return sorted_values[rank]


def summarize(values_ns: list[int], window: float) -> dict[str, float]:
    values = sorted(v / 1e6 for v in values_ns)
    return {"count": len(values), "per_second": len(values) / window, "p50_ms": percentile(values, 0.50),
            "p95_ms": percentile(values, 0.95), "p99_ms": percentile(values, 0.99),
            "max_ms": values[-1] if values else float("nan")}


def run_once(binary: pathlib.Path, args: argparse.Namespace, users: int) -> dict[str, Any]:
    run_id = f"{os.getpid()}x{int(time.time()) % 100000}"
    ctx = multiprocessing.get_context("spawn")
    procs_wanted = min(users, args.procs)
    tracy = TracyCapture(args.tracy_tools, args.tracy, users) if args.tracy else None
    saved_env = dict(os.environ)
    # ServerProcess hands the server this process's environment.
    os.environ["BOOKMARKS_MAX_LIVE_MODELS"] = str(args.max_live_models)
    os.environ["BOOKMARKS_WORKERS"] = str(args.workers)
    os.environ["BOOKMARKS_TRANSPORT"] = args.transport
    os.environ["BOOKMARKS_STORE"] = args.store
    if tracy is not None:
        os.environ.update(tracy.server_environment())
    with tempfile.TemporaryDirectory(prefix="morph-bench-") as tmp:
        db_path = pathlib.Path(tmp) / "bookmarks.db"
        if args.sqlite_wal:
            # journal_mode=WAL is stored in the database file, so setting it on
            # the empty file before the server's migrations run makes every
            # connection the server opens use it.
            with sqlite3.connect(db_path) as db:
                db.execute("PRAGMA journal_mode=WAL")
        with ServerProcess("bookmarks", binary, db_path) as server:
            os.environ.clear()
            os.environ.update(saved_env)
            if tracy is not None:
                tracy.start()
            pid = server.pid
            idle = sample_process(pid)
            # Leave time for the client interpreters to start before the first
            # session, so a slow spawn does not eat into the warmup.
            start_at = time.time() + 2.0 + 0.1 * procs_wanted
            ramp = args.ramp if args.ramp is not None else max(1.0, users / 500)
            plan = Plan(url=server.url, run_id=run_id, start_at=start_at,
                        measure_from=start_at + ramp + args.warmup,
                        measure_until=start_at + ramp + args.warmup + args.duration,
                        mix=args.mix, actions=args.actions, think_ms=args.think_ms,
                        accounts_per_user=args.accounts_per_user, collection=args.collection,
                        timeout=args.timeout, seed=args.seed, workload=args.workload, ramp=ramp,
                        seed_bookmarks=args.seed_bookmarks)
            results = ctx.Queue()
            procs = []
            base, extra = divmod(users, procs_wanted)
            first = 0
            for p in range(procs_wanted):
                count = base + (1 if p < extra else 0)
                proc = ctx.Process(target=run_process, args=(plan, first, count, results), daemon=True)
                proc.start()
                procs.append(proc)
                first += count

            with Sampler(pid, args.sample_period) as sampler:
                time.sleep(max(0.0, plan.measure_from - time.time()))
                before = sample_process(pid)
                threads_before = thread_cpu(pid)
                host_before = host_busy_seconds()
                time.sleep(max(0.0, plan.measure_until - time.time()))
                after = sample_process(pid)
                threads_after = thread_cpu(pid)
                host_after = host_busy_seconds()
            collected = [results.get(timeout=args.timeout + 60) for _ in procs]
            for proc in procs:
                proc.join(timeout=10)
            db_bytes = sum(p.stat().st_size for p in pathlib.Path(tmp).glob("bookmarks.db*"))
            journal = pathlib.Path(tmp) / "bookmarks_actions.jsonl"
            journal_bytes = journal.stat().st_size if journal.exists() else 0
    zones = tracy.finish() if tracy is not None else []

    window = after.wall - before.wall
    action_names = ["Login"] if args.workload == "framework" else list(args.mix)
    session_steps = ("Connect", "SignIn") if args.workload == "framework" else SESSION_STEPS
    steps: dict[str, list[int]] = {name: [] for name in (*session_steps, *action_names)}
    failures: dict[str, int] = {}
    for r in collected:
        for name, values in r["latencies"].items():
            steps.setdefault(name, []).extend(values)
        for name, count in r["failures"].items():
            failures[name] = failures.get(name, 0) + count
    action_ns = [v for name in action_names for v in steps[name]]
    actions_ok = len(action_ns) + len(steps.get("OpenCollection", [])) + len(steps.get("Seed", []))
    failed = sum(failures.values())
    attempted = sum(len(v) for v in steps.values()) + failed
    session_seconds = sorted(s for r in collected for s in r["session_seconds"])
    sessions = sum(r["sessions"] for r in collected)
    per_thread = sorted(((name, (cpu - threads_before.get(name, 0.0)) / window)
                         for name, cpu in threads_after.items()), key=lambda item: item[1], reverse=True)
    in_window = [s for s in sampler.samples if before.wall <= s.wall <= after.wall] or [before, after]
    server_cpu = after.cpu_seconds - before.cpu_seconds

    return {
        "users": users,
        "workload": args.workload,
        "transport": args.transport,
        "store": args.store,
        "client_processes": procs_wanted,
        "window_seconds": window,
        "sessions": sessions,
        "sessions_per_second": sessions / window,
        "session_seconds_p50": percentile(session_seconds, 0.5),
        "session_seconds_p95": percentile(session_seconds, 0.95),
        "actions_per_second": actions_ok / window,
        "requests_attempted": attempted,
        "failures": failed,
        "failure_rate": failed / attempted if attempted else 0.0,
        "failure_by_step": failures,
        "failure_samples": [s for r in collected for s in r["failure_samples"]][:10],
        "action_latency": summarize(action_ns, window),
        "steps": {name: summarize(values, window) for name, values in steps.items()},
        "server": {
            "cpu_cores": server_cpu / window,
            "cpu_us_per_action": server_cpu / actions_ok * 1e6 if actions_ok else float("nan"),
            "rss_idle_mib": idle.rss_kib / 1024,
            "rss_start_mib": before.rss_kib / 1024,
            "rss_end_mib": after.rss_kib / 1024,
            "rss_peak_mib": max(s.rss_kib for s in in_window) / 1024,
            "hwm_mib": after.hwm_kib / 1024,
            "threads": after.threads,
            "thread_cores": dict(per_thread),
            "db_mib": db_bytes / 2**20,
            "journal_mib": journal_bytes / 2**20,
        },
        "clients_cpu_cores": sum(r["cpu_seconds"] for r in collected) / window,
        # Whatever else ran on the host during the window. A benchmark on a
        # shared machine is only as good as this number is small.
        "other_cpu_cores": max(0.0, (host_after - host_before - server_cpu
                                     - sum(r["cpu_seconds"] for r in collected)) / window),
        "tracy": {"trace": str(tracy.trace), "zones_csv": str(tracy.csv), "top_zones": zones[:25]}
        if tracy is not None else None,
        "host_cores": os.cpu_count(),
    }


# ─────────────────────────────────────────────────────────────────────────────
# Reporting
# ─────────────────────────────────────────────────────────────────────────────


def meets_slo(result: dict[str, Any], args: argparse.Namespace) -> bool:
    p99 = result["action_latency"]["p99_ms"]
    return p99 == p99 and p99 <= args.slo_p99_ms and result["failure_rate"] <= args.slo_failure_rate


def print_run(result: dict[str, Any], args: argparse.Namespace) -> None:
    s = result["server"]
    lat = result["action_latency"]
    verdict = "meets" if meets_slo(result, args) else "MISSES"
    print(f"\n== {result['users']} concurrent users ({result['client_processes']} client processes), "
          f"{result['window_seconds']:.1f}s measured -- {verdict} the SLO ==")
    print(f"  sessions       {result['sessions_per_second']:8.1f}/s   ({result['sessions']} completed; "
          f"duration p50 {result['session_seconds_p50']:.2f}s, p95 {result['session_seconds_p95']:.2f}s)")
    print(f"  actions        {result['actions_per_second']:8.1f}/s   p50 {lat['p50_ms']:.2f} ms, "
          f"p95 {lat['p95_ms']:.2f}, p99 {lat['p99_ms']:.2f}, max {lat['max_ms']:.2f}")
    print(f"  failures       {result['failures']} of {result['requests_attempted']} requests "
          f"({100 * result['failure_rate']:.2f}%)")
    print(f"  server CPU     {s['cpu_cores']:.2f} cores   ({s['cpu_us_per_action']:.0f} us CPU per action)")
    print(f"  server RSS     idle {s['rss_idle_mib']:.1f} MiB, window {s['rss_start_mib']:.1f} -> "
          f"{s['rss_end_mib']:.1f}, peak {s['rss_peak_mib']:.1f} (lifetime HWM {s['hwm_mib']:.1f})")
    print(f"  server         {s['threads']} threads, db {s['db_mib']:.1f} MiB, "
          f"action journal {s['journal_mib']:.1f} MiB")
    busiest = ", ".join(f"{name} {cores:.2f}" for name, cores in list(s["thread_cores"].items())[:6])
    print(f"  server threads {busiest}  (cores)")
    print(f"  clients CPU    {result['clients_cpu_cores']:.2f} cores of {result['host_cores']}")
    print(f"  other load     {result['other_cpu_cores']:.2f} cores used by other processes on the host")
    print(f"  {'step':<18}{'/s':>8}{'fail':>6}{'p50 ms':>9}{'p95':>9}{'p99':>9}{'max':>9}")
    for name, st in result["steps"].items():
        print(f"  {name:<18}{st['per_second']:8.1f}{result['failure_by_step'].get(name, 0):6d}{st['p50_ms']:9.2f}"
              f"{st['p95_ms']:9.2f}{st['p99_ms']:9.2f}{st['max_ms']:9.2f}")
    for sample in result["failure_samples"]:
        print(f"  failure: {sample}")
    if result["tracy"]:
        print(f"  tracy          {result['tracy']['trace']}")
        print(f"  {'zone (by total time)':<44}{'count':>10}{'total ms':>11}{'mean us':>10}{'max us':>10}")
        for zone in result["tracy"]["top_zones"][:15]:
            print(f"  {zone['name'][:43]:<44}{zone.get('counts', 0):>10.0f}{zone.get('total_ns', 0) / 1e6:>11.1f}"
                  f"{zone.get('mean_ns', 0) / 1e3:>10.1f}{zone.get('max_ns', 0) / 1e3:>10.1f}")
    # An idle desktop host runs at about half a core of background work; past
    # that, something else was competing for it.
    if result["other_cpu_cores"] > 1.5:
        print(f"  WARNING: other processes used {result['other_cpu_cores']:.1f} cores during the window -- "
              "this run competed for the host and its figures are not comparable with a quiet one")
    busy = result["clients_cpu_cores"] + s["cpu_cores"] + result["other_cpu_cores"]
    if busy > 0.85 * result["host_cores"]:
        print(f"  note: the host had {busy:.1f} of {result['host_cores']} cores busy -- it was saturated, so these "
              "figures are a floor, not the server's ceiling")


def print_sweep(results: list[dict[str, Any]], args: argparse.Namespace) -> None:
    print(f"\n== summary (SLO: action p99 <= {args.slo_p99_ms:g} ms, failures <= "
          f"{100 * args.slo_failure_rate:g}%) ==")
    print(f"  {'users':>6}{'sess/s':>8}{'act/s':>8}{'p50 ms':>8}{'p99 ms':>8}{'fail %':>8}"
          f"{'srv cores':>10}{'us/act':>8}{'peak MiB':>10}{'cli cores':>10}  SLO")
    for r in results:
        s = r["server"]
        print(f"  {r['users']:>6}{r['sessions_per_second']:>8.1f}{r['actions_per_second']:>8.1f}"
              f"{r['action_latency']['p50_ms']:>8.2f}{r['action_latency']['p99_ms']:>8.2f}"
              f"{100 * r['failure_rate']:>8.2f}{s['cpu_cores']:>10.2f}{s['cpu_us_per_action']:>8.0f}"
              f"{s['rss_peak_mib']:>10.1f}{r['clients_cpu_cores']:>10.2f}  {'ok' if meets_slo(r, args) else 'MISS'}")
    passing = [r["users"] for r in results if meets_slo(r, args)]
    if passing:
        print(f"\n  capacity: {max(passing)} concurrent users is the largest tested count that meets the SLO")
    else:
        print("\n  capacity: no tested count meets the SLO")


def parse_mix(text: str) -> dict[str, int]:
    mix = {}
    for part in text.split(","):
        name, _, weight = part.partition("=")
        if name not in DEFAULT_MIX or not weight.isdigit():
            raise argparse.ArgumentTypeError(f"bad mix entry {part!r}; actions: {', '.join(DEFAULT_MIX)}")
        if int(weight):
            mix[name] = int(weight)
    if "CreateBookmark" not in mix:
        raise argparse.ArgumentTypeError("the mix needs CreateBookmark: the other actions name a bookmark")
    return mix


def parse_range(text: str) -> tuple[float, float]:
    low, _, high = text.partition("-")
    try:
        bounds = (float(low), float(high or low))
    except ValueError as exc:
        raise argparse.ArgumentTypeError(f"expected LOW-HIGH, got {text!r}") from exc
    if bounds[0] < 0 or bounds[1] < bounds[0]:
        raise argparse.ArgumentTypeError(f"expected 0 <= LOW <= HIGH, got {text!r}")
    return bounds


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--users", type=int, default=32, help="concurrent users (default 32)")
    parser.add_argument("--sweep", default="", help="comma-separated user counts; one fresh server each")
    parser.add_argument("--duration", type=float, default=30.0, help="measured seconds per run (default 30)")
    parser.add_argument("--warmup", type=float, default=5.0, help="unmeasured seconds before it (default 5)")
    parser.add_argument("--actions", type=parse_range, default=(5, 10),
                        help="actions per session after opening the collection, LOW-HIGH (default 5-10)")
    parser.add_argument("--think-ms", type=parse_range, default=(100.0, 1000.0),
                        help="pause before each action, LOW-HIGH ms (default 100-1000); 0 for no pause")
    parser.add_argument("--accounts-per-user", type=int, default=4,
                        help="accounts each simulated user rotates through (default 4)")
    parser.add_argument("--collection", type=int, default=40,
                        help="bookmarks per account before creates turn into deletes (default 40)")
    parser.add_argument("--mix", type=parse_mix, default=DEFAULT_MIX,
                        help="action weights, e.g. CreateBookmark=1,ListBookmarks=4 (default: a read-heavy mix)")
    parser.add_argument("--procs", type=int, default=max(1, (os.cpu_count() or 2) // 2),
                        help="client processes the users are spread over (default: half the cores)")
    parser.add_argument("--slo-p99-ms", type=float, default=200.0, help="action p99 the SLO allows (default 200)")
    parser.add_argument("--slo-failure-rate", type=float, default=0.001,
                        help="failed fraction of requests the SLO allows (default 0.001)")
    parser.add_argument("--workload", choices=("bookmarks", "framework"), default="bookmarks",
                        help="bookmarks: the action mix against the database; framework: every in-session action "
                             "is a Login, a full morph round trip that never reaches the database")
    parser.add_argument("--max-live-models", type=int, default=0,
                        help="the server's cap on live model instances; 0 (default) is unbounded")
    parser.add_argument("--store", choices=("sql", "memory"), default="sql",
                        help="where the server keeps bookmarks: its SQL database (default) or each model "
                             "instance's memory, with no persistence")
    parser.add_argument("--seed-bookmarks", type=parse_range, default=None,
                        help="bookmarks each session creates before opening its collection, LOW-HIGH "
                             "(default: 5-10 with --store memory, 0 otherwise)")
    parser.add_argument("--transport", choices=("qt", "net"), default="qt",
                        help="what serves clients: Qt WebSockets (default) or morph::net's socket server")
    parser.add_argument("--workers", type=int, default=4, help="the server's model worker pool size (default 4)")
    parser.add_argument("--ramp", type=float, default=None,
                        help="seconds over which users start their first session (default: users/500, at least 1)")
    parser.add_argument("--sqlite-wal", action="store_true",
                        help="create the database in WAL mode instead of SQLite's default rollback journal")
    parser.add_argument("--seed", type=int, default=1)
    parser.add_argument("--timeout", type=float, default=30.0, help="per-reply timeout in seconds (default 30)")
    parser.add_argument("--sample-period", type=float, default=0.25, help="server sampling period (default 0.25s)")
    parser.add_argument("--build-dir", type=pathlib.Path, default=None,
                        help="where ladder_bookmarks_server lives (default: search build/*/)")
    parser.add_argument("--tracy", type=pathlib.Path, default=None,
                        help="record the server with tracy-capture into this directory (needs a Tracy build)")
    parser.add_argument("--tracy-tools", default="",
                        help="directories holding tracy-capture and tracy-csvexport, ':'-separated (default: PATH)")
    parser.add_argument("--json", type=pathlib.Path, default=None, help="also write every result here")
    args = parser.parse_args(argv)
    args.actions = (int(args.actions[0]), int(args.actions[1]))
    if args.seed_bookmarks is None:
        args.seed_bookmarks = (5, 10) if args.store == "memory" else (0, 0)
    args.seed_bookmarks = (int(args.seed_bookmarks[0]), int(args.seed_bookmarks[1]))

    if not sys.platform.startswith("linux"):
        print("bench_bookmarks: Linux only (the server is sampled through /proc)", file=sys.stderr)
        return 2
    counts = [int(c) for c in args.sweep.split(",") if c] if args.sweep else [args.users]
    if any(c < 1 for c in counts) or args.procs < 1:
        print("bench_bookmarks: user and process counts must be positive", file=sys.stderr)
        return 2

    try:
        binary = find_server("bookmarks", args.build_dir)
    except DriverError as exc:
        print(f"bench_bookmarks: {exc}", file=sys.stderr)
        return 2
    if args.tracy is not None:
        try:
            args.tracy_tools = resolve_tracy_tools(args.tracy_tools)
        except DriverError as exc:
            print(f"bench_bookmarks: {exc}", file=sys.stderr)
            return 2
        args.tracy.mkdir(parents=True, exist_ok=True)
    print(f"server: {binary}")
    if args.workload == "framework":
        print(f"session: connect, sign in, {args.actions[0]}-{args.actions[1]} Login round trips with "
              f"{args.think_ms[0]:g}-{args.think_ms[1]:g} ms think time, disconnect (no database)")
    else:
        seeded = (f"create {args.seed_bookmarks[0]}-{args.seed_bookmarks[1]} bookmarks, "
                  if args.seed_bookmarks[1] else "")
        print(f"session: connect, sign in, {seeded}open collection, {args.actions[0]}-{args.actions[1]} actions with "
              f"{args.think_ms[0]:g}-{args.think_ms[1]:g} ms think time, disconnect")
        print(f"mix: {', '.join(f'{k}={v}' for k, v in args.mix.items())}")
    print(f"server: {args.store} store, {args.transport} transport, {args.workers} workers, live-model cap {args.max_live_models or 'unbounded'}")

    results = []
    for count in counts:
        try:
            result = run_once(binary, args, count)
        except DriverError as exc:
            print(f"bench_bookmarks: {exc}", file=sys.stderr)
            return 1
        print_run(result, args)
        results.append(result)
    if len(results) > 1:
        print_sweep(results, args)
    if args.json is not None:
        settings = vars(args)
        args.json.write_text(json.dumps({"server": str(binary), "args": settings, "runs": results}, indent=2,
                                        default=str))
        print(f"\nwrote {args.json}")
    return 0


if __name__ == "__main__":
    sys.exit(main())

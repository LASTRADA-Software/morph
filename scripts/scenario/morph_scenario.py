#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Drive a running morph server from a plain-text scenario file.

An out-of-process client: it opens a real TCP connection to a running
``ladder_<rung>_server``, performs the RFC 6455 WebSocket handshake, and speaks
``morph::wire``'s JSON envelope protocol (``docs/spec/core/wire.md``) directly.
Nothing here links against morph or shares a process with it, and the protocol
is implemented from the spec rather than from morph's own C++ — which is the
point: a bug that is symmetric on both sides of morph's own client/server pair
is invisible to morph's own tests and visible here.

Standard library only; no build step, no dependencies.

See ``README.md`` next to this file for the scenario format.
"""

from __future__ import annotations

import argparse
import base64
import hashlib
import json
import os
import re
import socket
import struct
import sys
from dataclasses import dataclass, field
from typing import Any

# ─────────────────────────────────────────────────────────────────────────────
# Errors
# ─────────────────────────────────────────────────────────────────────────────


class ScenarioError(Exception):
    """A scenario file is malformed. Raised before anything connects."""


class TransportError(Exception):
    """The connection failed, closed, or timed out."""


# ─────────────────────────────────────────────────────────────────────────────
# WebSocket client (RFC 6455, client side, text frames only)
# ─────────────────────────────────────────────────────────────────────────────

_WS_GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"

_OP_CONT = 0x0
_OP_TEXT = 0x1
_OP_BINARY = 0x2
_OP_CLOSE = 0x8
_OP_PING = 0x9
_OP_PONG = 0xA


def parse_ws_url(url: str) -> tuple[str, int, str]:
    """Splits ``ws://host:port[/path]`` into its parts.

    Mirrors ``morph::net::detail::parseWsUrl``: ``wss://`` is not supported by
    the shipped servers, and the port must be explicit.
    """
    if url.startswith("wss://"):
        raise ScenarioError(f"wss:// is not supported by morph's servers: {url}")
    if not url.startswith("ws://"):
        raise ScenarioError(f"server url must start with ws:// : {url}")
    rest = url[len("ws://") :]
    path_at = rest.find("/")
    path = rest[path_at:] if path_at >= 0 else "/"
    hostport = rest[:path_at] if path_at >= 0 else rest
    if ":" not in hostport:
        raise ScenarioError(f"server url needs an explicit port: {url}")
    host, _, port_text = hostport.rpartition(":")
    if not port_text.isdigit():
        raise ScenarioError(f"server url has an invalid port: {url}")
    return host, int(port_text), path


class WebSocket:
    """A minimal RFC 6455 client connection carrying JSON text frames."""

    def __init__(self, url: str, timeout: float) -> None:
        host, port, path = parse_ws_url(url)
        self.url = url
        self._timeout = timeout
        try:
            self._sock = socket.create_connection((host, port), timeout=timeout)
        except OSError as exc:
            raise TransportError(f"cannot connect to {url}: {exc}") from exc
        self._sock.settimeout(timeout)
        self._buf = b""
        self._handshake(host, port, path)

    def _handshake(self, host: str, port: int, path: str) -> None:
        key = base64.b64encode(os.urandom(16)).decode("ascii")
        request = (
            f"GET {path} HTTP/1.1\r\n"
            f"Host: {host}:{port}\r\n"
            "Upgrade: websocket\r\n"
            "Connection: Upgrade\r\n"
            f"Sec-WebSocket-Key: {key}\r\n"
            "Sec-WebSocket-Version: 13\r\n"
            "\r\n"
        )
        self._sock.sendall(request.encode("ascii"))
        while b"\r\n\r\n" not in self._buf:
            self._buf += self._recv_some()
        head, self._buf = self._buf.split(b"\r\n\r\n", 1)
        text = head.decode("latin-1")
        status = text.split("\r\n", 1)[0]
        if "101" not in status:
            raise TransportError(f"{self.url}: websocket upgrade refused: {status}")
        expected = base64.b64encode(hashlib.sha1((key + _WS_GUID).encode()).digest()).decode()
        got = ""
        for line in text.split("\r\n")[1:]:
            name, _, value = line.partition(":")
            if name.strip().lower() == "sec-websocket-accept":
                got = value.strip()
        if got != expected:
            raise TransportError(f"{self.url}: bad Sec-WebSocket-Accept (got {got!r}, want {expected!r})")

    def _recv_some(self) -> bytes:
        try:
            chunk = self._sock.recv(65536)
        except socket.timeout as exc:
            raise TransportError(f"{self.url}: no data within {self._timeout}s") from exc
        except OSError as exc:
            raise TransportError(f"{self.url}: read failed: {exc}") from exc
        if not chunk:
            raise TransportError(f"{self.url}: server closed the connection")
        return chunk

    def _read_exactly(self, count: int) -> bytes:
        while len(self._buf) < count:
            self._buf += self._recv_some()
        head, self._buf = self._buf[:count], self._buf[count:]
        return head

    def _send_frame(self, opcode: int, payload: bytes) -> None:
        mask = os.urandom(4)
        length = len(payload)
        if length < 126:
            header = struct.pack("!BB", 0x80 | opcode, 0x80 | length)
        elif length < (1 << 16):
            header = struct.pack("!BBH", 0x80 | opcode, 0x80 | 126, length)
        else:
            header = struct.pack("!BBQ", 0x80 | opcode, 0x80 | 127, length)
        masked = bytes(byte ^ mask[i % 4] for i, byte in enumerate(payload))
        try:
            self._sock.sendall(header + mask + masked)
        except OSError as exc:
            raise TransportError(f"{self.url}: write failed: {exc}") from exc

    def send_text(self, text: str) -> None:
        """Sends one masked text frame."""
        self._send_frame(_OP_TEXT, text.encode("utf-8"))

    def _read_frame(self) -> tuple[bool, int, bytes]:
        first, second = self._read_exactly(2)
        fin = bool(first & 0x80)
        opcode = first & 0x0F
        length = second & 0x7F
        if length == 126:
            length = struct.unpack("!H", self._read_exactly(2))[0]
        elif length == 127:
            length = struct.unpack("!Q", self._read_exactly(8))[0]
        mask = self._read_exactly(4) if second & 0x80 else None
        payload = self._read_exactly(length)
        if mask is not None:
            payload = bytes(byte ^ mask[i % 4] for i, byte in enumerate(payload))
        return fin, opcode, payload

    def recv_text(self) -> str:
        """Reads frames until one complete text message is assembled.

        Answers pings, ignores pongs, and turns a close frame into a
        `TransportError` naming the server's close reason.
        """
        message = b""
        assembling = False
        while True:
            fin, opcode, payload = self._read_frame()
            if opcode == _OP_PING:
                self._send_frame(_OP_PONG, payload)
                continue
            if opcode == _OP_PONG:
                continue
            if opcode == _OP_CLOSE:
                code = struct.unpack("!H", payload[:2])[0] if len(payload) >= 2 else 0
                reason = payload[2:].decode("utf-8", "replace")
                raise TransportError(f"{self.url}: server closed the connection (code {code} {reason!r})")
            if opcode == _OP_BINARY:
                raise TransportError(f"{self.url}: unexpected binary frame")
            if opcode in (_OP_TEXT, _OP_CONT):
                if opcode == _OP_TEXT and assembling:
                    raise TransportError(f"{self.url}: interleaved text frames")
                message += payload
                assembling = not fin
                if fin:
                    return message.decode("utf-8")

    def close(self) -> None:
        """Sends a close frame and drops the socket. Never raises."""
        try:
            self._send_frame(_OP_CLOSE, struct.pack("!H", 1000))
        except TransportError:
            pass
        try:
            self._sock.close()
        except OSError:
            pass


# ─────────────────────────────────────────────────────────────────────────────
# The wire envelope (docs/spec/core/wire.md)
# ─────────────────────────────────────────────────────────────────────────────

PROTOCOL_VERSION = 1

#: Every field `morph::wire::Envelope` reflects, with its default. `encode`
#: writes all of them; `decode` ignores unknown keys and defaults absent ones,
#: so sending the full set is both valid and closest to what morph itself puts
#: on the wire. `primary`/`shared` are absent from wire.md's field table but
#: present in `include/morph/core/wire.hpp` (see README, "Spec drift").
ENVELOPE_DEFAULTS: dict[str, Any] = {
    "kind": "",
    "callId": 0,
    "typeId": "",
    "contextKey": "",
    "primary": "",
    "shared": False,
    "modelId": 0,
    "modelType": "",
    "actionType": "",
    "body": "",
    "message": "",
    "session": {"principal": "", "token": "", "requestId": "", "locale": "", "metadata": {}},
    "protocolVersion": 0,
}


def make_envelope(**fields: Any) -> dict[str, Any]:
    """Builds a full envelope, defaulting every field the caller omits."""
    envelope = json.loads(json.dumps(ENVELOPE_DEFAULTS))  # deep copy
    for name, value in fields.items():
        if name not in envelope:
            raise ScenarioError(f"unknown envelope field {name!r}")
        envelope[name] = value
    return envelope


@dataclass
class Reply:
    """A decoded reply envelope plus its parsed `body`."""

    envelope: dict[str, Any]
    body: Any

    @property
    def kind(self) -> str:
        return str(self.envelope.get("kind", ""))

    @property
    def message(self) -> str:
        return str(self.envelope.get("message", ""))

    def summary(self) -> str:
        if self.kind == "err":
            return f'err message="{self.message}"'
        body = self.envelope.get("body", "")
        parts = [f"modelId={self.envelope['modelId']}"] if self.envelope.get("modelId") else []
        if body:
            parts.append(f"body={body}")
        return "ok" + (" " + " ".join(parts) if parts else "")


# ─────────────────────────────────────────────────────────────────────────────
# Client — one connection, one session, optionally one registered model
# ─────────────────────────────────────────────────────────────────────────────


@dataclass
class Client:
    """One named connection: its socket, its session, its registered model."""

    name: str
    socket: WebSocket
    session: dict[str, Any]
    model_type: str = ""
    model_id: int = 0
    _next_call_id: int = 1

    def next_call_id(self) -> int:
        call_id = self._next_call_id
        self._next_call_id += 1
        return call_id

    def rpc(self, envelope: dict[str, Any]) -> Reply:
        """Sends one envelope and reads the reply carrying the same `callId`.

        `callId` correlation is the protocol's own matching rule; a reply for a
        different call is a protocol violation worth reporting rather than
        silently accepting.

        **One reply legitimately carries no `callId` at all.** When
        `morph::wire::decode` cannot parse the frame, the server has no
        envelope to read a `callId` out of, so it answers
        `err "envelope decode failed: ..."` with `callId` 0. That is a real
        reply to this request and the only one it will get -- refusing it as a
        mismatch would make the decode-failure path unassertable from a
        scenario, which is precisely the path
        `scenarios/pastebin/malformed-envelope.scenario` exists to pin. It is
        accepted only in that exact shape: an `err` whose message names a
        decode failure. A zero `callId` on anything else is still a violation.
        """
        self.socket.send_text(json.dumps(envelope))
        want = envelope["callId"]
        raw = self.socket.recv_text()
        try:
            decoded = json.loads(raw)
        except json.JSONDecodeError as exc:
            raise TransportError(f"{self.name}: server sent non-JSON: {raw[:200]!r}") from exc
        if not isinstance(decoded, dict):
            raise TransportError(f"{self.name}: server sent a non-object envelope: {raw[:200]!r}")
        got = decoded.get("callId")
        undecodable = (
            got == 0
            and decoded.get("kind") == "err"
            and str(decoded.get("message", "")).startswith("envelope decode failed")
        )
        if got != want and not undecodable:
            raise TransportError(f"{self.name}: reply callId {got} does not match request callId {want}")
        body_text = decoded.get("body", "")
        body: Any = None
        if isinstance(body_text, str) and body_text:
            try:
                body = json.loads(body_text)
            except json.JSONDecodeError:
                body = body_text
        return Reply(envelope=decoded, body=body)


# ─────────────────────────────────────────────────────────────────────────────
# Tokenizer and value syntax
# ─────────────────────────────────────────────────────────────────────────────


def tokenize(line: str) -> list[str]:
    """Splits a line on whitespace, keeping double-quoted runs (and their
    quotes, which the value syntax is sensitive to) intact and honouring
    backslash escapes inside them."""
    tokens: list[str] = []
    current = ""
    in_quotes = False
    index = 0
    started = False
    while index < len(line):
        char = line[index]
        if in_quotes:
            current += char
            if char == "\\" and index + 1 < len(line):
                current += line[index + 1]
                index += 2
                continue
            if char == '"':
                in_quotes = False
            index += 1
            continue
        if char == '"':
            in_quotes = True
            started = True
            current += char
            index += 1
            continue
        if char.isspace():
            if started:
                tokens.append(current)
                current = ""
                started = False
            index += 1
            continue
        if char == "#" and not started:
            break
        started = True
        current += char
        index += 1
    if in_quotes:
        raise ScenarioError("unterminated double quote")
    if started:
        tokens.append(current)
    return tokens


_NUMBER = re.compile(r"^-?(0|[1-9][0-9]*)(\.[0-9]+)?([eE][-+]?[0-9]+)?$")
_VARIABLE = re.compile(r"\$\{([A-Za-z_][A-Za-z0-9_]*)\}|\$([A-Za-z_][A-Za-z0-9_]*)")


def _stringify(value: Any) -> str:
    return value if isinstance(value, str) else json.dumps(value, separators=(",", ":"))


def _expand(text: str, captures: dict[str, Any]) -> str:
    def replace(match: re.Match[str]) -> str:
        name = match.group(1) or match.group(2)
        if name not in captures:
            raise ScenarioError(f"${name} is not captured (captured so far: {sorted(captures) or 'nothing'})")
        return _stringify(captures[name])

    return _VARIABLE.sub(replace, text)


def parse_value(token: str, captures: dict[str, Any]) -> Any:
    """Turns one scenario token into a JSON value.

    ``"text"`` is a string (escapes honoured), ``12``/``1.5`` a number,
    ``true``/``false``/``null`` the JSON literals, ``{...}``/``[...]`` raw JSON,
    a lone ``$name`` the captured value with its own type, and anything else a
    bare string. ``$name`` expands inside quoted strings and bare words too,
    stringified.
    """
    whole = _VARIABLE.fullmatch(token)
    if whole is not None:
        name = whole.group(1) or whole.group(2)
        if name not in captures:
            raise ScenarioError(f"${name} is not captured (captured so far: {sorted(captures) or 'nothing'})")
        return captures[name]
    if token.startswith('"'):
        try:
            return json.loads(_expand(token, captures))
        except json.JSONDecodeError as exc:
            raise ScenarioError(f"bad quoted string {token}: {exc}") from exc
    if token.startswith("{") or token.startswith("["):
        try:
            return json.loads(_expand(token, captures))
        except json.JSONDecodeError as exc:
            raise ScenarioError(f"bad inline JSON {token}: {exc}") from exc
    expanded = _expand(token, captures)
    if expanded in ("true", "false", "null"):
        return {"true": True, "false": False, "null": None}[expanded]
    if _NUMBER.match(expanded):
        return json.loads(expanded)
    return expanded


# The `client` options a scenario would plausibly capture: a principal and a
# token come out of a `Login` reply, and a contextKey out of whatever names the
# instance. All three are text on the wire, so a captured number is stringified
# rather than refused.
CAPTURED_CLIENT_OPTIONS = ("principal", "token", "contextKey")

# The rest name the connection itself, and are static in every shipped
# scenario. They are not expanded — a capture in one is refused by name rather
# than sent literally (morph#360).
STATIC_CLIENT_OPTIONS = ("url", "model", "protocol")


def resolve_client_options(options: dict[str, str], captures: dict[str, Any]) -> dict[str, str]:
    """Expands `$capture` references in a `client` step's options.

    `session` runs its values through `parse_value`, so `session token=$token`
    installs the captured token; `client` read its own options raw, so
    `client books token=$token` sent the six literal characters and the run
    failed several steps later with a bare `unauthorized` (morph#360). This is
    the one place both spellings now agree.

    Only the credential options are expanded. A capture in `url`, `model` or
    `protocol` is refused here, naming the option — those are static in every
    shipped scenario, and sending `$name` to a server as a model type is never
    what the author meant.
    """
    unknown = set(options) - set(CAPTURED_CLIENT_OPTIONS) - set(STATIC_CLIENT_OPTIONS)
    if unknown:
        raise ScenarioError(f"unknown client option(s): {', '.join(sorted(unknown))}")
    resolved = dict(options)
    for name in CAPTURED_CLIENT_OPTIONS:
        if name in resolved:
            resolved[name] = _stringify(parse_value(resolved[name], captures))
    for name in STATIC_CLIENT_OPTIONS:
        value = resolved.get(name, "")
        found = _VARIABLE.search(value)
        if found is not None:
            raise ScenarioError(
                f"client {name}={value} references the capture {found.group(0)}, and "
                f"{name} is never expanded — only {', '.join(CAPTURED_CLIENT_OPTIONS)} are"
            )
    return resolved


def split_assignment(token: str) -> tuple[str, str]:
    name, sep, value = token.partition("=")
    if not sep or not name:
        raise ScenarioError(f"expected name=value, got {token!r}")
    return name, value


_PATH_STEP = re.compile(r"([A-Za-z_][A-Za-z0-9_]*)|\[(-?[0-9]+)\]")

_MISSING = object()


def read_path(path: str, reply: Reply) -> Any:
    """Reads ``path`` out of a reply, or returns a sentinel when it is absent.

    ``@name`` reads an envelope field (``@modelId``, ``@message``, ``@kind``);
    anything else reads the parsed ``body``, with an optional ``$.`` prefix,
    dotted names and ``[index]`` subscripts.
    """
    if path.startswith("@"):
        name = path[1:]
        if name not in reply.envelope:
            return _MISSING
        return reply.envelope[name]
    rest = path[2:] if path.startswith("$.") else path
    current: Any = reply.body
    if rest in ("", "$"):
        return current
    position = 0
    while position < len(rest):
        if rest[position] == ".":
            position += 1
            continue
        match = _PATH_STEP.match(rest, position)
        if match is None:
            raise ScenarioError(f"bad field path {path!r} at offset {position}")
        position = match.end()
        if match.group(1) is not None:
            if not isinstance(current, dict) or match.group(1) not in current:
                return _MISSING
            current = current[match.group(1)]
        else:
            index = int(match.group(2))
            if not isinstance(current, list) or not -len(current) <= index < len(current):
                return _MISSING
            current = current[index]
    return current


# ─────────────────────────────────────────────────────────────────────────────
# Scenario model
# ─────────────────────────────────────────────────────────────────────────────


@dataclass
class Assertion:
    """One clause of an `expect` line."""

    line_no: int
    text: str
    kind: str  # "capture" | "compare"
    path: str = ""
    op: str = ""
    expected_token: str = ""
    capture_name: str = ""


@dataclass
class Step:
    """One action line plus every assertion attached to it."""

    line_no: int
    text: str
    verb: str
    args: list[str]
    assertions: list[Assertion] = field(default_factory=list)
    requires_expect: bool = True


@dataclass
class Scenario:
    """A parsed scenario: the settings block plus the ordered steps."""

    path: str
    server_url: str = ""
    default_model: str = ""
    steps: list[Step] = field(default_factory=list)


_ACTION_VERBS = {"client", "use", "session", "do", "send", "deregister", "close"}
_SETTING_VERBS = {"server", "model"}
_NEEDS_EXPECT = {"do", "send", "deregister"}


def parse_scenario(text: str, path: str) -> Scenario:
    """Parses a whole scenario file, raising `ScenarioError` on the first
    problem. Nothing connects until this has succeeded."""
    scenario = Scenario(path=path)
    for line_no, raw in enumerate(text.splitlines(), start=1):
        try:
            tokens = tokenize(raw)
            if not tokens:
                continue
            verb, args = tokens[0], tokens[1:]
            if verb in _SETTING_VERBS:
                if scenario.steps:
                    raise ScenarioError(f"'{verb}' must come before the first step")
                if len(args) != 1:
                    raise ScenarioError(f"'{verb}' takes exactly one argument")
                if verb == "server":
                    parse_ws_url(args[0])
                    scenario.server_url = args[0]
                else:
                    scenario.default_model = args[0]
            elif verb == "expect":
                if not scenario.steps:
                    raise ScenarioError("'expect' before any step")
                scenario.steps[-1].assertions.extend(parse_expect(args, line_no, raw.strip()))
            elif verb in _ACTION_VERBS:
                scenario.steps.append(
                    Step(
                        line_no=line_no,
                        text=raw.strip(),
                        verb=verb,
                        args=args,
                        requires_expect=verb in _NEEDS_EXPECT,
                    )
                )
            else:
                raise ScenarioError(f"unknown directive {verb!r}")
        except ScenarioError as exc:
            raise ScenarioError(f"{path}:{line_no}: {exc}") from None

    for step in scenario.steps:
        if step.requires_expect and not step.assertions:
            raise ScenarioError(
                f"{path}:{step.line_no}: '{step.text}' has no 'expect' line — "
                "every do/send/deregister must state its expected outcome"
            )
    if not scenario.steps:
        raise ScenarioError(f"{path}: scenario has no steps")
    return scenario


def parse_expect(args: list[str], line_no: int, text: str) -> list[Assertion]:
    """Parses one ``expect ok|err [clause ...]`` line into assertions."""
    if not args or args[0] not in ("ok", "err"):
        raise ScenarioError("'expect' must be followed by 'ok' or 'err'")
    assertions = [
        Assertion(line_no=line_no, text=text, kind="compare", path="@kind", op="==", expected_token=args[0])
    ]
    index = 1
    while index < len(args):
        clause = args[index]
        if clause == "capture":
            if index + 1 >= len(args):
                raise ScenarioError("'capture' needs <name>=<path>")
            name, path = split_assignment(args[index + 1])
            assertions.append(
                Assertion(line_no=line_no, text=text, kind="capture", path=path, capture_name=name)
            )
            index += 2
        elif clause in ("field", "message"):
            if clause == "message":
                path = "@message"
                if index + 2 >= len(args):
                    raise ScenarioError("'message' needs <op> <value>")
                op, expected = args[index + 1], args[index + 2]
                index += 3
            else:
                if index + 3 >= len(args):
                    raise ScenarioError("'field' needs <path> <op> <value>")
                path, op, expected = args[index + 1], args[index + 2], args[index + 3]
                index += 4
            if op not in ("==", "!=", "~", "!~"):
                raise ScenarioError(f"unknown comparison {op!r} (use ==, !=, ~ or !~)")
            assertions.append(
                Assertion(line_no=line_no, text=text, kind="compare", path=path, op=op, expected_token=expected)
            )
        else:
            raise ScenarioError(f"unknown expect clause {clause!r} (use capture, field or message)")
    return assertions


# ─────────────────────────────────────────────────────────────────────────────
# Runner
# ─────────────────────────────────────────────────────────────────────────────


class Failure(Exception):
    """A step or assertion did not hold. Carries the detail lines to print."""

    def __init__(self, headline: str, detail: list[str]) -> None:
        super().__init__(headline)
        self.headline = headline
        self.detail = detail


class Runner:
    """Executes a parsed scenario against a live server."""

    def __init__(self, scenario: Scenario, server_url: str, timeout: float, verbose: bool) -> None:
        self.scenario = scenario
        self.server_url = server_url
        self.timeout = timeout
        self.verbose = verbose
        self.clients: dict[str, Client] = {}
        self.current: Client | None = None
        self.captures: dict[str, Any] = {}
        self.last_reply: Reply | None = None
        self.assertion_count = 0

    # ── step dispatch ────────────────────────────────────────────────────

    def run(self) -> int:
        """Runs every step. Returns a process exit code."""
        failed_step = 0
        try:
            for number, step in enumerate(self.scenario.steps, start=1):
                self.run_step(number, step)
        except Failure as failure:
            failed_step = 1
            print(failure.headline)
            for line in failure.detail:
                print(line)
        except (TransportError, ScenarioError) as exc:
            failed_step = 1
            print(f"  ERROR {exc}")
        finally:
            for client in self.clients.values():
                client.socket.close()
        total = len(self.scenario.steps)
        print(
            f"\n{self.scenario.path}: {total} steps, {self.assertion_count} assertions, "
            f"{'1 failure' if failed_step else 'no failures'}"
        )
        return 1 if failed_step else 0

    def run_step(self, number: int, step: Step) -> None:
        handler = {
            "client": self.do_client,
            "use": self.do_use,
            "session": self.do_session,
            "do": self.do_execute,
            "send": self.do_send,
            "deregister": self.do_deregister,
            "close": self.do_close,
        }[step.verb]
        try:
            reply = handler(step)
        except ScenarioError as exc:
            raise Failure(
                f"FAIL step {number} (line {step.line_no}): {step.text}",
                [f"  {exc}", *self.state_lines()],
            ) from None
        except TransportError as exc:
            raise Failure(
                f"FAIL step {number} (line {step.line_no}): {step.text}",
                [f"  transport: {exc}", *self.state_lines()],
            ) from None
        if reply is not None:
            self.last_reply = reply
        who = self.current.name if self.current else "-"
        outcome = reply.summary() if reply is not None else "-"
        print(f"  ok   step {number} (line {step.line_no}) [{who}] {step.text}")
        if self.verbose and reply is not None:
            print(f"         -> {outcome}")
        for assertion in step.assertions:
            self.check(number, step, assertion)

    def state_lines(self) -> list[str]:
        lines = []
        if self.current is not None:
            lines.append(f"  client:   {self.current.name} (modelId={self.current.model_id})")
        if self.last_reply is not None:
            lines.append(f"  reply:    {self.last_reply.summary()}")
        captured = ", ".join(f"{k}={_stringify(v)}" for k, v in self.captures.items())
        lines.append(f"  captures: {captured if captured else '(none)'}")
        return lines

    # ── directives ───────────────────────────────────────────────────────

    def require_client(self) -> Client:
        if self.current is None:
            raise ScenarioError("no client is open — add a 'client <name>' step first")
        return self.current

    def do_client(self, step: Step) -> Reply | None:
        if not step.args:
            raise ScenarioError("'client' needs a name")
        name = step.args[0]
        if name in self.clients:
            raise ScenarioError(f"client {name!r} already exists")
        options = resolve_client_options(
            dict(split_assignment(token) for token in step.args[1:]), self.captures
        )
        url = options.get("url", self.server_url)
        if not url:
            raise ScenarioError("no server url — add a 'server ws://host:port' line or pass --server")
        session = dict(ENVELOPE_DEFAULTS["session"])
        session["principal"] = options.get("principal", "")
        session["token"] = options.get("token", "")
        client = Client(name=name, socket=WebSocket(url, self.timeout), session=session)
        self.clients[name] = client
        self.current = client

        protocol = options.get("protocol", str(PROTOCOL_VERSION))
        if protocol != "none":
            if not protocol.isdigit():
                raise ScenarioError(f"protocol must be a number or 'none', got {protocol!r}")
            reply = client.rpc(
                make_envelope(kind="hello", callId=client.next_call_id(), protocolVersion=int(protocol))
            )
            if reply.kind != "ok":
                raise ScenarioError(f"handshake refused by the server: {reply.message}")

        model = options.get("model", self.scenario.default_model)
        if model:
            reply = client.rpc(
                make_envelope(
                    kind="register",
                    callId=client.next_call_id(),
                    typeId=model,
                    contextKey=options.get("contextKey", ""),
                )
            )
            if reply.kind != "ok":
                raise ScenarioError(f"register {model} refused by the server: {reply.message}")
            client.model_type = model
            client.model_id = int(reply.envelope["modelId"])
        return None

    def do_use(self, step: Step) -> Reply | None:
        if len(step.args) != 1:
            raise ScenarioError("'use' takes exactly one client name")
        if step.args[0] not in self.clients:
            raise ScenarioError(f"no such client {step.args[0]!r}")
        self.current = self.clients[step.args[0]]
        return None

    def do_session(self, step: Step) -> Reply | None:
        """Replaces the current client's session — the step that turns a
        `Login` result into the credentials every later `execute` carries."""
        client = self.require_client()
        if not step.args:
            raise ScenarioError("'session' needs at least one principal=/token= assignment")
        for token in step.args:
            name, value = split_assignment(token)
            if name not in client.session:
                raise ScenarioError(f"unknown session field {name!r}")
            parsed = parse_value(value, self.captures)
            if not isinstance(parsed, (str, dict)):
                raise ScenarioError(f"session {name} must be text, got {_stringify(parsed)}")
            client.session[name] = parsed
        return None

    def do_execute(self, step: Step) -> Reply:
        client = self.require_client()
        if not step.args:
            raise ScenarioError("'do' needs an action type")
        if not client.model_id:
            raise ScenarioError(f"client {client.name!r} has no registered model — give it 'model=<TypeId>'")
        action_type = step.args[0]
        body: dict[str, Any] = {}
        for token in step.args[1:]:
            name, value = split_assignment(token)
            body[name] = parse_value(value, self.captures)
        return client.rpc(
            make_envelope(
                kind="execute",
                callId=client.next_call_id(),
                modelId=client.model_id,
                modelType=client.model_type,
                actionType=action_type,
                body=json.dumps(body, separators=(",", ":")),
                session=client.session,
            )
        )

    def do_send(self, step: Step) -> Reply:
        """Sends a hand-built envelope — the escape hatch for protocol-level
        hostility (a bogus `kind`, a wrong `protocolVersion`, someone else's
        `modelId`)."""
        client = self.require_client()
        if not step.args:
            raise ScenarioError("'send' needs an envelope kind")
        fields: dict[str, Any] = {"kind": step.args[0], "callId": client.next_call_id()}
        for token in step.args[1:]:
            name, value = split_assignment(token)
            if name == "callId":
                raise ScenarioError("callId is assigned by the runner and cannot be set")
            fields[name] = parse_value(value, self.captures)
        return client.rpc(make_envelope(**fields))

    def do_deregister(self, step: Step) -> Reply:
        client = self.require_client()
        if step.args:
            raise ScenarioError("'deregister' takes no arguments")
        if not client.model_id:
            raise ScenarioError(f"client {client.name!r} has no registered model")
        reply = client.rpc(
            make_envelope(kind="deregister", callId=client.next_call_id(), modelId=client.model_id)
        )
        if reply.kind == "ok":
            client.model_id = 0
        return reply

    def do_close(self, step: Step) -> Reply | None:
        name = step.args[0] if step.args else (self.current.name if self.current else "")
        if name not in self.clients:
            raise ScenarioError(f"no such client {name!r}")
        self.clients.pop(name).socket.close()
        if self.current is not None and self.current.name == name:
            self.current = next(iter(self.clients.values()), None)
        return None

    # ── assertions ───────────────────────────────────────────────────────

    def check(self, number: int, step: Step, assertion: Assertion) -> None:
        self.assertion_count += 1
        if self.last_reply is None:
            raise Failure(
                f"FAIL step {number} (line {assertion.line_no}): {assertion.text}",
                ["  nothing has been sent yet, so there is no reply to assert on", *self.state_lines()],
            )
        actual = read_path(assertion.path, self.last_reply)
        if assertion.kind == "capture":
            if actual is _MISSING:
                raise Failure(
                    f"FAIL step {number} (line {assertion.line_no}): {assertion.text}",
                    [f"  cannot capture {assertion.capture_name}: {assertion.path} is absent", *self.state_lines()],
                )
            self.captures[assertion.capture_name] = actual
            print(f"       capture {assertion.capture_name} = {_stringify(actual)}")
            return

        expected = parse_value(assertion.expected_token, self.captures)
        ok, shown = self.compare(assertion.op, actual, expected)
        if not ok:
            raise Failure(
                f"FAIL step {number} (line {assertion.line_no}): {step.text}",
                [
                    f"  expected: {assertion.path} {assertion.op} {_stringify(expected)}",
                    f"  actual:   {assertion.path} == {shown}",
                    *self.state_lines(),
                ],
            )
        if self.verbose:
            print(f"       {assertion.path} {assertion.op} {_stringify(expected)}")

    @staticmethod
    def compare(op: str, actual: Any, expected: Any) -> tuple[bool, str]:
        # An absent path fails every comparison, `!=` included: "the field is
        # not there" must never read as "the field differs from X", or a
        # renamed or dropped result field would quietly satisfy the assertion
        # that was watching it.
        if actual is _MISSING:
            return False, "(absent)"
        shown = _stringify(actual)
        if op == "==":
            return actual == expected, shown
        if op == "!=":
            return actual != expected, shown
        pattern = expected if isinstance(expected, str) else _stringify(expected)
        try:
            compiled = re.compile(pattern)
        except re.error as exc:
            raise ScenarioError(f"bad regex {pattern!r}: {exc}") from None
        found = compiled.search(_stringify(actual)) is not None
        return (found if op == "~" else not found), shown


# ─────────────────────────────────────────────────────────────────────────────
# Entry point
# ─────────────────────────────────────────────────────────────────────────────


def main(argv: list[str] | None = None) -> int:
    """Parses the command line, runs the scenario, returns the exit code."""
    parser = argparse.ArgumentParser(
        prog="morph_scenario.py",
        description="Drive a running morph server from a text scenario file.",
    )
    parser.add_argument("scenario", help="path to the scenario file")
    parser.add_argument("--server", default="", help="ws://host:port (overrides the file's 'server' line)")
    parser.add_argument("--timeout", type=float, default=10.0, help="per-read timeout in seconds (default 10)")
    parser.add_argument("-v", "--verbose", action="store_true", help="print every reply and every assertion")
    args = parser.parse_args(argv)

    try:
        with open(args.scenario, encoding="utf-8") as handle:
            scenario = parse_scenario(handle.read(), args.scenario)
    except OSError as exc:
        print(f"cannot read scenario: {exc}", file=sys.stderr)
        return 2
    except ScenarioError as exc:
        print(f"scenario error: {exc}", file=sys.stderr)
        return 2

    server_url = args.server or scenario.server_url
    print(f"{scenario.path} against {server_url or '(url from the scenario file)'}")
    return Runner(scenario, server_url, args.timeout, args.verbose).run()


if __name__ == "__main__":
    sys.exit(main())

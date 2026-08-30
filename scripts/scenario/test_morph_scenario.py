#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Self-test for the parts of the scenario runner that need no server:
the tokenizer, the value syntax, the field paths, the comparison rules and
the parser's own refusals.

    python3 scripts/scenario/test_morph_scenario.py
"""

from __future__ import annotations

import pathlib
import tempfile
import unittest

from morph_scenario import (
    Reply,
    Runner,
    ScenarioError,
    _MISSING,
    parse_expect,
    parse_scenario,
    parse_value,
    parse_ws_url,
    read_path,
    tokenize,
)

import scenario_coverage

from scenario_coverage import (
    DECODE_FUNCTION_MARKER,
    MIN_KINDS,
    MIN_MESSAGES,
    Allowlist,
    AllowlistError,
    Exercised,
    Surface,
    SurfaceError,
    _repo_root,
    allowlist_problems,
    covers,
    decode_body,
    exercised_by,
    extract_shipped_surface,
    extract_surface,
    floor_violations,
    load_allowlist,
    load_scenarios,
    main as coverage_main,
    server_half,
)


class TokenizerTest(unittest.TestCase):
    def test_keeps_quoted_runs_together_with_their_quotes(self) -> None:
        self.assertEqual(tokenize('do X a="two words" b=bare'), ["do", "X", 'a="two words"', "b=bare"])

    def test_strips_trailing_comments_but_not_hashes_inside_tokens(self) -> None:
        self.assertEqual(tokenize('do X a="c#1"  # trailing'), ["do", "X", 'a="c#1"'])

    def test_honours_backslash_escapes_inside_quotes(self) -> None:
        self.assertEqual(tokenize(r'do X a="he said \"hi\""'), ["do", "X", r'a="he said \"hi\""'])

    def test_rejects_an_unterminated_quote(self) -> None:
        with self.assertRaises(ScenarioError):
            tokenize('do X a="oops')


class ValueSyntaxTest(unittest.TestCase):
    def test_bare_words_are_strings_and_quoted_digits_stay_strings(self) -> None:
        self.assertEqual(parse_value("plaintext", {}), "plaintext")
        self.assertEqual(parse_value('"123"', {}), "123")

    def test_numbers_and_literals(self) -> None:
        self.assertEqual(parse_value("12", {}), 12)
        self.assertEqual(parse_value("1.5", {}), 1.5)
        self.assertIs(parse_value("true", {}), True)
        self.assertIsNone(parse_value("null", {}))

    def test_inline_json(self) -> None:
        self.assertEqual(parse_value('{"a":[1,2]}', {}), {"a": [1, 2]})

    def test_a_lone_variable_keeps_the_captured_type(self) -> None:
        self.assertEqual(parse_value("$n", {"n": 7}), 7)

    def test_a_variable_inside_a_string_is_stringified(self) -> None:
        self.assertEqual(parse_value('"id $n!"', {"n": 7}), "id 7!")

    def test_an_uncaptured_variable_is_an_error(self) -> None:
        with self.assertRaises(ScenarioError):
            parse_value("$nope", {})


class PathTest(unittest.TestCase):
    def setUp(self) -> None:
        self.reply = Reply(
            envelope={"kind": "ok", "modelId": 42, "message": "", "body": ""},
            body={"id": "a", "readCount": {"num": 1}, "pastes": [{"id": "p0"}, {"id": "p1"}]},
        )

    def test_reads_dotted_paths_indices_and_the_dollar_prefix(self) -> None:
        self.assertEqual(read_path("id", self.reply), "a")
        self.assertEqual(read_path("$.readCount.num", self.reply), 1)
        self.assertEqual(read_path("pastes[1].id", self.reply), "p1")
        self.assertEqual(read_path("pastes[-1].id", self.reply), "p1")

    def test_at_prefix_reads_the_envelope(self) -> None:
        self.assertEqual(read_path("@modelId", self.reply), 42)

    def test_absent_paths_report_missing_rather_than_none(self) -> None:
        self.assertIs(read_path("nope", self.reply), _MISSING)
        self.assertIs(read_path("pastes[9].id", self.reply), _MISSING)
        self.assertIs(read_path("@nope", self.reply), _MISSING)


class CompareTest(unittest.TestCase):
    def test_equality_and_regex(self) -> None:
        self.assertTrue(Runner.compare("==", "a", "a")[0])
        self.assertTrue(Runner.compare("!=", "a", "b")[0])
        self.assertTrue(Runner.compare("~", "no such paste", "such")[0])
        self.assertFalse(Runner.compare("!~", "no such paste", "such")[0])

    def test_regex_matches_the_json_text_of_a_non_string(self) -> None:
        self.assertTrue(Runner.compare("~", [{"id": "p0"}], "p0")[0])

    def test_an_absent_value_fails_every_operator(self) -> None:
        for op in ("==", "!=", "~", "!~"):
            held, shown = Runner.compare(op, _MISSING, "anything")
            self.assertFalse(held, f"{op} passed on an absent path")
            self.assertEqual(shown, "(absent)")


class ParserTest(unittest.TestCase):
    def test_a_step_without_an_expect_is_rejected(self) -> None:
        with self.assertRaises(ScenarioError) as caught:
            parse_scenario("model M\nclient a\ndo Thing x=1\n", "s")
        self.assertIn("has no 'expect'", str(caught.exception))

    def test_a_full_scenario_parses_into_steps_with_their_assertions(self) -> None:
        scenario = parse_scenario(
            "server ws://h:1\nmodel M\nclient a\ndo Thing x=1\nexpect ok capture id=$.id\nexpect ok field a == b\n",
            "s",
        )
        self.assertEqual(scenario.server_url, "ws://h:1")
        self.assertEqual(scenario.default_model, "M")
        self.assertEqual([step.verb for step in scenario.steps], ["client", "do"])
        # implicit @kind, capture, implicit @kind, field
        self.assertEqual(len(scenario.steps[1].assertions), 4)

    def test_expect_needs_ok_or_err(self) -> None:
        with self.assertRaises(ScenarioError):
            parse_expect(["field", "a", "==", "b"], 1, "expect field a == b")

    def test_unknown_directives_and_clauses_are_rejected(self) -> None:
        with self.assertRaises(ScenarioError):
            parse_scenario("wiggle a\n", "s")
        with self.assertRaises(ScenarioError):
            parse_expect(["ok", "wiggle"], 1, "expect ok wiggle")

    def test_settings_must_precede_the_first_step(self) -> None:
        with self.assertRaises(ScenarioError):
            parse_scenario("client a\nserver ws://h:1\n", "s")

    def test_an_empty_scenario_is_rejected(self) -> None:
        with self.assertRaises(ScenarioError):
            parse_scenario("# nothing but a comment\n", "s")


class UrlTest(unittest.TestCase):
    def test_splits_host_port_and_path(self) -> None:
        self.assertEqual(parse_ws_url("ws://127.0.0.1:8765/x"), ("127.0.0.1", 8765, "/x"))
        self.assertEqual(parse_ws_url("ws://h:1"), ("h", 1, "/"))

    def test_rejects_wss_and_a_missing_port(self) -> None:
        for bad in ("wss://h:1", "ws://h", "http://h:1", "ws://h:x"):
            with self.assertRaises(ScenarioError, msg=bad):
                parse_ws_url(bad)


_FIXTURE_HEADER = '''
    if (env.kind == "register") {
    } else if (env.kind == "execute") {
        reply(makeErr("model not found", env.callId));
    } else {
        reply(makeErr("unknown envelope kind: " + env.kind, env.callId));
    }
    rejectAndRelease("server busy");
    throw std::runtime_error("register requires a typeId");
    const std::string message = "payload missing required field(s): " + missing;
'''


def _real_header_text() -> str:
    """Reads the actual remote.hpp this repository ships."""
    return (_repo_root() / "include" / "morph" / "core" / "remote.hpp").read_text(encoding="utf-8")


def _real_wire_text() -> str:
    """Reads the actual wire.hpp this repository ships."""
    return (_repo_root() / "include" / "morph" / "core" / "wire.hpp").read_text(encoding="utf-8")


def _real_surface():
    """The surface of the shipped headers, with both scoping rules applied."""
    return extract_shipped_surface(_real_header_text(), _real_wire_text())


class SurfaceExtractionTest(unittest.TestCase):
    def test_finds_every_envelope_kind(self) -> None:
        surface = extract_surface(_FIXTURE_HEADER)
        self.assertEqual(surface.kinds, frozenset({"register", "execute"}))

    def test_separates_exact_messages_from_prefixes(self) -> None:
        surface = extract_surface(_FIXTURE_HEADER)
        self.assertIn("model not found", surface.exact_messages)
        self.assertIn("server busy", surface.exact_messages)
        self.assertIn("register requires a typeId", surface.exact_messages)
        # A message ending in ": " is a prefix -- the runtime value carries a
        # suffix the source cannot know.
        self.assertIn("unknown envelope kind: ", surface.message_prefixes)
        self.assertIn("payload missing required field(s): ", surface.message_prefixes)
        self.assertNotIn("unknown envelope kind: ", surface.exact_messages)

    def test_floor_rejects_an_implausibly_small_surface(self) -> None:
        # The whole point: a rename in remote.hpp must break this loudly rather
        # than report full coverage over an empty universe.
        violations = floor_violations(extract_surface(_FIXTURE_HEADER))
        self.assertTrue(violations)
        self.assertTrue(any("kind" in v for v in violations))

    def test_floor_accepts_the_real_header(self) -> None:
        surface = _real_surface()
        self.assertEqual(floor_violations(surface), [])
        self.assertGreaterEqual(len(surface.kinds), MIN_KINDS)
        self.assertGreaterEqual(
            len(surface.exact_messages) + len(surface.message_prefixes), MIN_MESSAGES
        )

    def test_extracts_a_message_from_a_call_split_across_lines(self) -> None:
        # `include/morph/core/remote.hpp` itself has a `makeErr(` call whose
        # string literal sits on the following line (the `handleInline`
        # refusal). Against the old tight pattern `makeErr\("([^"]*)"`, which
        # requires the literal to sit directly against the open paren, this
        # fixture would find nothing at all -- a clang-format reflow silently
        # drops a genuine refusal string. The widened pattern must still find
        # it.
        multiline_call = 'reply(makeErr(\n        "some message", env.callId));'
        surface = extract_surface(multiline_call)
        self.assertIn("some message", surface.exact_messages)

    def test_drops_an_exact_message_that_extends_a_known_prefix(self) -> None:
        # A literal that starts with a collected prefix is not a separate
        # refusal -- it is the first fragment of a `"prefix" + runtime_value`
        # concatenation that the regex also happened to match as a standalone
        # literal. No assertion could ever equal it, so it must not survive
        # into exact_messages, while the real prefix it extends still must.
        fixture = (
            'throw std::runtime_error("failed: " + reason);\n'
            'throw std::runtime_error("failed: with extra detail appended");\n'
        )
        surface = extract_surface(fixture)
        self.assertIn("failed: ", surface.message_prefixes)
        self.assertNotIn("failed: with extra detail appended", surface.exact_messages)

    def test_real_header_carries_the_kinds_the_spec_records(self) -> None:
        surface = _real_surface()
        self.assertEqual(
            surface.kinds,
            frozenset({"register", "execute", "deregister", "hello",
                       "attach", "assign", "instances", "schemas"}),
        )


class ServerHalfScopeTest(unittest.TestCase):
    def test_refusals_below_the_client_stub_are_not_part_of_the_universe(self) -> None:
        # SimulatedRemoteBackend throws these on the *client* when it receives
        # an err reply. They are C++ exceptions; no WebSocket peer can assert
        # them, so counting them would set unreachable coverage targets.
        surface = _real_surface()
        known = surface.exact_messages | surface.message_prefixes
        for client_side in (
            "register failed: ",
            "attach failed: ",
            "instances failed: ",
            "instances decode failed: ",
            "schemas request failed: ",
        ):
            self.assertNotIn(client_side, known)

    def test_the_kinds_are_unaffected_by_the_scope(self) -> None:
        # Every `env.kind ==` comparison lives in the server, so truncating at
        # the client stub must not lose one.
        self.assertEqual(_real_surface().kinds, extract_surface(_real_header_text()).kinds)

    def test_a_missing_marker_raises_rather_than_scanning_everything(self) -> None:
        # A scoping rule that silently degrades to "scan the whole file" is the
        # same class of bug as the one it exists to fix.
        with self.assertRaises(SurfaceError) as caught:
            server_half('reply(makeErr("only the server half here", id));\n')
        self.assertIn("SimulatedRemoteBackend", str(caught.exception))

    def test_the_wire_header_contributes_the_decode_refusal(self) -> None:
        # wire::decode throws this and dispatchMessage's catch turns it into an
        # err reply, so a scenario genuinely can assert it -- but it lives in
        # wire.hpp, which the tool used never to read.
        self.assertIn("envelope decode failed: ", _real_surface().message_prefixes)
        self.assertNotIn(
            "envelope decode failed: ", extract_surface(_real_header_text()).message_prefixes
        )


class DecodeBodyScopeTest(unittest.TestCase):
    def test_decode_body_excludes_encode_and_negotiation(self) -> None:
        # `encode`'s throw fires only when nothing has been sent yet, and
        # `interpretHelloReply`'s throw runs on the client inspecting a reply
        # it already received -- neither is wire-observable, so decode_body
        # must not carry either string into its scanned text.
        body = decode_body(_real_wire_text())
        self.assertNotIn("envelope encode failed: ", body)
        self.assertNotIn("protocol negotiation failed: ", body)
        self.assertIn("envelope decode failed: ", body)

    def test_wire_header_contributes_exactly_the_decode_refusal(self) -> None:
        # Scoped to decode's body, wire.hpp must yield exactly one entry: the
        # prefix `envelope decode failed: `. `envelope encode failed: ` and
        # `protocol negotiation failed: ` are not server refusals a scenario
        # could ever assert, and must appear in neither set.
        surface = _real_surface()
        for not_observable in ("envelope encode failed: ", "protocol negotiation failed: "):
            self.assertNotIn(not_observable, surface.exact_messages)
            self.assertNotIn(not_observable, surface.message_prefixes)

    def test_a_missing_decode_marker_raises_naming_the_marker(self) -> None:
        # A scoping rule that silently degrades to "scan the whole file" is
        # the same class of bug as the one it exists to fix: a rename of
        # decode's signature must stop the run, loudly, naming the marker it
        # could not find.
        with self.assertRaises(SurfaceError) as caught:
            decode_body("no decode function anywhere in this text\n")
        self.assertIn(DECODE_FUNCTION_MARKER, str(caught.exception))

    def test_unbalanced_braces_after_the_marker_also_raise(self) -> None:
        broken = DECODE_FUNCTION_MARKER + " {\n    if (true) {\n"
        with self.assertRaises(SurfaceError):
            decode_body(broken)


# The exact refusal universe the shipped headers yield. Unlike MIN_MESSAGES,
# which only catches the surface shrinking, this pins the *set*: adding a
# refusal to remote.hpp or wire.hpp fails this test, and a human then decides
# whether it needs a scenario or an allowlist entry.
_REAL_EXACT_MESSAGES = frozenset({
    "assign requires a typeId",
    "attach requires a typeId",
    "connection closed",
    "handleInline does not support execute (reply is asynchronous)",
    "instances requires a typeId",
    "model not found",
    "protocol version unsupported",
    "register requires a typeId",
    "schemas requires a typeId",
    "server busy",
    "server shutting down",
    "timeout",
    "too many models",
    "unauthorized",
})

_REAL_MESSAGE_PREFIXES = frozenset({
    "envelope decode failed: ",
    "payload missing required field(s): ",
    "unknown envelope kind: ",
})


class RealRefusalSetTest(unittest.TestCase):
    def test_real_headers_carry_exactly_these_refusals(self) -> None:
        surface = _real_surface()
        self.assertEqual(surface.exact_messages, _REAL_EXACT_MESSAGES)
        self.assertEqual(surface.message_prefixes, _REAL_MESSAGE_PREFIXES)

    def test_min_messages_sits_just_below_the_true_total(self) -> None:
        total = len(_REAL_EXACT_MESSAGES) + len(_REAL_MESSAGE_PREFIXES)
        self.assertEqual(total, 17)
        self.assertEqual(MIN_MESSAGES, total - 2)


_FIXTURE_SCENARIO = '''
model PasteModel
client alice
do CreatePaste content="x"
expect ok field id ~ .
expect ok message == "this must not be counted"
do GetPaste id=nope
expect err message == "model not found"
send instances typeId=PasteModel
expect err message == "unknown envelope kind: instances"
deregister
expect ok
'''


class ExercisedExtractionTest(unittest.TestCase):
    def _parsed(self):
        return [parse_scenario(_FIXTURE_SCENARIO, "fixture.scenario")]

    def test_client_counts_as_hello_and_register(self) -> None:
        used = exercised_by(self._parsed())
        self.assertIn("hello", used.kinds)
        self.assertIn("register", used.kinds)

    def test_do_counts_as_execute_and_send_counts_as_its_own_kind(self) -> None:
        used = exercised_by(self._parsed())
        self.assertIn("execute", used.kinds)
        self.assertIn("instances", used.kinds)
        self.assertIn("deregister", used.kinds)

    def test_collects_only_messages_asserted_on_an_err_reply(self) -> None:
        used = exercised_by(self._parsed())
        self.assertEqual(used.messages, frozenset({"model not found", "unknown envelope kind: instances"}))
        self.assertNotIn("this must not be counted", used.messages)

    def test_a_prefix_message_is_covered_by_an_assertion_carrying_a_suffix(self) -> None:
        surface = Surface(
            kinds=frozenset({"execute"}),
            exact_messages=frozenset({"model not found"}),
            message_prefixes=frozenset({"unknown envelope kind: "}),
        )
        uncovered_kinds, uncovered_messages = covers(surface, exercised_by(self._parsed()))
        self.assertEqual(uncovered_kinds, frozenset())
        self.assertEqual(uncovered_messages, frozenset())

    def test_reports_what_the_corpus_never_asserts(self) -> None:
        surface = Surface(
            kinds=frozenset({"execute", "assign"}),
            exact_messages=frozenset({"server busy"}),
            message_prefixes=frozenset(),
        )
        uncovered_kinds, uncovered_messages = covers(surface, exercised_by(self._parsed()))
        self.assertEqual(uncovered_kinds, frozenset({"assign"}))
        self.assertEqual(uncovered_messages, frozenset({"server busy"}))

    def test_a_protocol_none_client_does_not_credit_hello(self) -> None:
        # The runner skips the `hello` round-trip entirely when `protocol=none`
        # (morph_scenario.Runner.do_client). Crediting `hello` here would report
        # the handshake covered by a scenario that deliberately never sends it.
        scenario = parse_scenario(
            'client a protocol=none\nsend schemas typeId=X\nexpect err message == "x"\n', "f"
        )
        self.assertNotIn("hello", exercised_by([scenario]).kinds)

    def test_a_client_with_no_model_anywhere_does_not_credit_register(self) -> None:
        # No `model` setting and no `model=` option: the runner resolves an
        # empty model name and sends no `register` at all.
        scenario = parse_scenario(
            'client a\nsend schemas typeId=X\nexpect err message == "x"\n', "f"
        )
        self.assertNotIn("register", exercised_by([scenario]).kinds)

    def test_a_client_option_supplies_the_model_the_scenario_lacks(self) -> None:
        scenario = parse_scenario(
            'client a model=PasteModel\nsend schemas typeId=X\nexpect err message == "x"\n', "f"
        )
        self.assertIn("register", exercised_by([scenario]).kinds)

    def test_a_plain_client_in_a_scenario_with_a_model_credits_both(self) -> None:
        scenario = parse_scenario(
            'model PasteModel\nclient a\nsend schemas typeId=X\nexpect err message == "x"\n', "f"
        )
        used = exercised_by([scenario])
        self.assertIn("hello", used.kinds)
        self.assertIn("register", used.kinds)

    def test_load_scenarios_returns_scenarios_in_sorted_order(self) -> None:
        # Minimal valid scenario: every do/send/deregister step must be
        # followed by at least one expect line.
        minimal_scenario = (
            "model TestModel\n"
            "client test\n"
            "do GetPaste id=x\n"
            "expect ok field id == x\n"
        )
        with tempfile.TemporaryDirectory() as tmpdir:
            tmp_path = pathlib.Path(tmpdir)
            # Write files in reverse alphabetical order to verify sorting.
            (tmp_path / "b_second.scenario").write_text(minimal_scenario)
            (tmp_path / "a_first.scenario").write_text(minimal_scenario)
            scenarios = load_scenarios(tmp_path)
            self.assertEqual(len(scenarios), 2)
            # Verify they are sorted by path.
            self.assertEqual(scenarios[0].path, str(tmp_path / "a_first.scenario"))
            self.assertEqual(scenarios[1].path, str(tmp_path / "b_second.scenario"))


class AllowlistTest(unittest.TestCase):
    _SURFACE = Surface(
        kinds=frozenset({"execute", "assign"}),
        exact_messages=frozenset({"server busy", "model not found"}),
        message_prefixes=frozenset(),
    )
    _EXERCISED = Exercised(kinds=frozenset({"execute"}), messages=frozenset({"model not found"}))

    def test_an_entry_for_a_genuinely_uncovered_item_is_accepted(self) -> None:
        allowlist = Allowlist(kinds={"assign": "no rung uses shared instances yet"},
                              messages={"server busy": "no server sets a LimitPolicy"})
        self.assertEqual(allowlist_problems(allowlist, self._SURFACE, self._EXERCISED), [])

    def test_an_entry_for_something_now_covered_is_a_problem(self) -> None:
        # The exemption has outlived its reason: the corpus covers this now.
        allowlist = Allowlist(kinds={"assign": "r", "execute": "stale"},
                              messages={"server busy": "r"})
        problems = allowlist_problems(allowlist, self._SURFACE, self._EXERCISED)
        self.assertTrue(any("execute" in p and "already covered" in p for p in problems))

    def test_an_entry_naming_something_that_no_longer_exists_is_a_problem(self) -> None:
        # A rename in remote.hpp left this behind.
        allowlist = Allowlist(kinds={"assign": "r", "telepathy": "gone"},
                              messages={"server busy": "r"})
        problems = allowlist_problems(allowlist, self._SURFACE, self._EXERCISED)
        self.assertTrue(any("telepathy" in p and "no longer" in p for p in problems))

    def test_an_empty_reason_is_a_problem(self) -> None:
        allowlist = Allowlist(kinds={"assign": ""}, messages={"server busy": "r"})
        problems = allowlist_problems(allowlist, self._SURFACE, self._EXERCISED)
        self.assertTrue(any("reason" in p for p in problems))

    def test_an_empty_reason_on_a_message_entry_is_a_problem(self) -> None:
        allowlist = Allowlist(kinds={}, messages={"server busy": ""})
        problems = allowlist_problems(allowlist, self._SURFACE, self._EXERCISED)
        self.assertTrue(any("reason" in p for p in problems))

    def test_a_message_entry_naming_something_that_no_longer_exists_is_a_problem(self) -> None:
        # A rename in remote.hpp left this behind.
        allowlist = Allowlist(kinds={}, messages={"telepathic link severed": "gone"})
        problems = allowlist_problems(allowlist, self._SURFACE, self._EXERCISED)
        self.assertTrue(any("telepathic link severed" in p and "no longer" in p for p in problems))

    def test_a_message_entry_for_something_now_covered_is_a_problem(self) -> None:
        # The exemption has outlived its reason: the corpus covers this now.
        allowlist = Allowlist(kinds={}, messages={"model not found": "stale"})
        problems = allowlist_problems(allowlist, self._SURFACE, self._EXERCISED)
        self.assertTrue(any("model not found" in p and "already covered" in p for p in problems))

    def test_a_prefix_shaped_message_entry_genuinely_uncovered_is_accepted(self) -> None:
        surface = Surface(
            kinds=frozenset({"execute"}),
            exact_messages=frozenset(),
            message_prefixes=frozenset({"unknown envelope kind: "}),
        )
        exercised = Exercised(kinds=frozenset({"execute"}), messages=frozenset({"model not found"}))
        allowlist = Allowlist(kinds={}, messages={"unknown envelope kind: ": "no scenario sends a bad kind"})
        self.assertEqual(allowlist_problems(allowlist, surface, exercised), [])

    def test_a_prefix_shaped_message_entry_already_covered_is_a_problem(self) -> None:
        surface = Surface(
            kinds=frozenset({"execute"}),
            exact_messages=frozenset(),
            message_prefixes=frozenset({"unknown envelope kind: "}),
        )
        exercised = Exercised(
            kinds=frozenset({"execute"}),
            messages=frozenset({"unknown envelope kind: frobnicate"}),
        )
        allowlist = Allowlist(kinds={}, messages={"unknown envelope kind: ": "stale"})
        problems = allowlist_problems(allowlist, surface, exercised)
        self.assertTrue(
            any("unknown envelope kind: " in p and "already covered" in p for p in problems)
        )

    def test_malformed_json_raises_rather_than_escaping_as_a_gap(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            path = pathlib.Path(tmp) / "allow.json"
            path.write_text("{not json at all", encoding="utf-8")
            with self.assertRaises(AllowlistError) as caught:
                load_allowlist(path)
        self.assertIn("not valid JSON", str(caught.exception))

    def test_a_non_string_reason_raises(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            path = pathlib.Path(tmp) / "allow.json"
            path.write_text('{"kinds": {"assign": 7}}', encoding="utf-8")
            with self.assertRaises(AllowlistError):
                load_allowlist(path)

    def test_a_section_that_is_not_an_object_raises(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            path = pathlib.Path(tmp) / "allow.json"
            path.write_text('{"messages": ["server busy"]}', encoding="utf-8")
            with self.assertRaises(AllowlistError):
                load_allowlist(path)

    def test_the_shipped_allowlist_parses(self) -> None:
        allowlist = load_allowlist(_repo_root() / "scripts" / "scenario" / "coverage_allowlist.json")
        for reason in list(allowlist.kinds.values()) + list(allowlist.messages.values()):
            self.assertTrue(reason.strip(), "every entry needs a written reason")


class CoverageCliTest(unittest.TestCase):
    def _fixture_run(self, tmp: str, header_text: str, extra: list[str]) -> int:
        """Runs the CLI over a throwaway header, empty-bodied wire header and corpus."""
        root = pathlib.Path(tmp)
        header = root / "remote.hpp"
        # Every fixture header carries the marker: refusal extraction refuses
        # to guess a scope without it.
        header.write_text(header_text + "\n" + scenario_coverage.SERVER_HALF_MARKER + " {};\n",
                          encoding="utf-8")
        wire = root / "wire.hpp"
        # Likewise every fixture wire header carries the decode marker, with a
        # balanced, empty body: it contributes nothing to the surface, but its
        # absence would make decode_body raise instead of exercising what this
        # test actually means to check.
        wire.write_text(scenario_coverage.DECODE_FUNCTION_MARKER + " {}\n", encoding="utf-8")
        scenarios = root / "scenarios"
        scenarios.mkdir()
        return coverage_main([
            "--header", str(header), "--wire-header", str(wire),
            "--scenarios", str(scenarios), *extra,
        ])

    def test_exits_two_when_the_extractor_finds_an_implausible_surface(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            code = self._fixture_run(tmp, 'if (env.kind == "register") {}', [])
        self.assertEqual(code, 2)

    def test_exits_two_when_the_server_half_marker_is_gone(self) -> None:
        # Without the marker the tool has no honest scope, so it must stop --
        # not silently count the client stub's throws as wire refusals.
        with tempfile.TemporaryDirectory() as tmp:
            root = pathlib.Path(tmp)
            header = root / "remote.hpp"
            header.write_text(_FIXTURE_HEADER, encoding="utf-8")
            wire = root / "wire.hpp"
            wire.write_text("", encoding="utf-8")
            scenarios = root / "scenarios"
            scenarios.mkdir()
            code = coverage_main([
                "--header", str(header), "--wire-header", str(wire),
                "--scenarios", str(scenarios), "--no-floor",
            ])
        self.assertEqual(code, 2)

    def test_exits_one_when_something_is_uncovered_and_unexempt(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            allow = pathlib.Path(tmp) / "allow.json"
            allow.write_text('{"kinds": {}, "messages": {}}', encoding="utf-8")
            code = self._fixture_run(
                tmp, _FIXTURE_HEADER, ["--allowlist", str(allow), "--no-floor"]
            )
        self.assertEqual(code, 1)

    def test_exits_two_when_the_allowlist_is_malformed(self) -> None:
        # A broken allowlist is a broken tool. Exit 1 would tell CI there is a
        # real coverage gap.
        with tempfile.TemporaryDirectory() as tmp:
            allow = pathlib.Path(tmp) / "allow.json"
            allow.write_text("{oops", encoding="utf-8")
            code = self._fixture_run(
                tmp, _FIXTURE_HEADER, ["--allowlist", str(allow), "--no-floor"]
            )
        self.assertEqual(code, 2)

    def test_exits_zero_when_everything_uncovered_is_exempt(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            allow = pathlib.Path(tmp) / "allow.json"
            allow.write_text(
                '{"kinds": {"register": "fixture"}, "messages": {"nope": "fixture"}}', encoding="utf-8"
            )
            code = self._fixture_run(
                tmp,
                'if (env.kind == "register") {}\nreply(makeErr("nope", id));',
                ["--allowlist", str(allow), "--no-floor"],
            )
        self.assertEqual(code, 0)


if __name__ == "__main__":
    unittest.main(verbosity=2)

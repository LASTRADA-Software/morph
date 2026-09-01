#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Self-test for the parts of the scenario runner that need no server:
the tokenizer, the value syntax, the field paths, the comparison rules and
the parser's own refusals.

    python3 scripts/scenario/test_morph_scenario.py
"""

from __future__ import annotations

import json
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

import run_scenarios
import scenario_coverage

from scenario_coverage import (
    DECODE_FUNCTION_MARKER,
    MIN_ACTIONS,
    MIN_KINDS,
    MIN_MESSAGES,
    SERVER_RUNGS,
    WORKFLOW_FLOORS,
    WORKFLOW_MIN_ACTIONS,
    WORKFLOW_MIN_CHAINED,
    Allowlist,
    AllowlistError,
    Exercised,
    RungTally,
    ScenarioFacts,
    Surface,
    SurfaceError,
    _render,
    _repo_root,
    action_allowlist_problems,
    action_floor_violations,
    allowlist_problems,
    covers,
    decode_body,
    exercised_by,
    extract_actions,
    extract_shipped_surface,
    extract_surface,
    floor_violations,
    load_allowlist,
    load_scenarios,
    main as coverage_main,
    rung_tallies,
    scenario_facts,
    server_half,
    shipped_action_sources,
    workflow_problems,
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
                              messages={"server busy": "no server sets a LimitPolicy"}, actions={})
        self.assertEqual(allowlist_problems(allowlist, self._SURFACE, self._EXERCISED), [])

    def test_an_entry_for_something_now_covered_is_a_problem(self) -> None:
        # The exemption has outlived its reason: the corpus covers this now.
        allowlist = Allowlist(kinds={"assign": "r", "execute": "stale"},
                              messages={"server busy": "r"}, actions={})
        problems = allowlist_problems(allowlist, self._SURFACE, self._EXERCISED)
        self.assertTrue(any("execute" in p and "already covered" in p for p in problems))

    def test_an_entry_naming_something_that_no_longer_exists_is_a_problem(self) -> None:
        # A rename in remote.hpp left this behind.
        allowlist = Allowlist(kinds={"assign": "r", "telepathy": "gone"},
                              messages={"server busy": "r"}, actions={})
        problems = allowlist_problems(allowlist, self._SURFACE, self._EXERCISED)
        self.assertTrue(any("telepathy" in p and "no longer" in p for p in problems))

    def test_an_empty_reason_is_a_problem(self) -> None:
        allowlist = Allowlist(kinds={"assign": ""}, messages={"server busy": "r"}, actions={})
        problems = allowlist_problems(allowlist, self._SURFACE, self._EXERCISED)
        self.assertTrue(any("reason" in p for p in problems))

    def test_an_empty_reason_on_a_message_entry_is_a_problem(self) -> None:
        allowlist = Allowlist(kinds={}, messages={"server busy": ""}, actions={})
        problems = allowlist_problems(allowlist, self._SURFACE, self._EXERCISED)
        self.assertTrue(any("reason" in p for p in problems))

    def test_a_message_entry_naming_something_that_no_longer_exists_is_a_problem(self) -> None:
        # A rename in remote.hpp left this behind.
        allowlist = Allowlist(kinds={}, messages={"telepathic link severed": "gone"}, actions={})
        problems = allowlist_problems(allowlist, self._SURFACE, self._EXERCISED)
        self.assertTrue(any("telepathic link severed" in p and "no longer" in p for p in problems))

    def test_a_message_entry_for_something_now_covered_is_a_problem(self) -> None:
        # The exemption has outlived its reason: the corpus covers this now.
        allowlist = Allowlist(kinds={}, messages={"model not found": "stale"}, actions={})
        problems = allowlist_problems(allowlist, self._SURFACE, self._EXERCISED)
        self.assertTrue(any("model not found" in p and "already covered" in p for p in problems))

    def test_a_prefix_shaped_message_entry_genuinely_uncovered_is_accepted(self) -> None:
        surface = Surface(
            kinds=frozenset({"execute"}),
            exact_messages=frozenset(),
            message_prefixes=frozenset({"unknown envelope kind: "}),
        )
        exercised = Exercised(kinds=frozenset({"execute"}), messages=frozenset({"model not found"}))
        allowlist = Allowlist(kinds={}, messages={"unknown envelope kind: ": "no scenario sends a bad kind"}, actions={})
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
        allowlist = Allowlist(kinds={}, messages={"unknown envelope kind: ": "stale"}, actions={})
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
        for reason in list(allowlist.kinds.values()) + list(allowlist.messages.values()) + list(
            allowlist.actions.values()
        ):
            self.assertTrue(reason.strip(), "every entry needs a written reason")


class ActionAllowlistTest(unittest.TestCase):
    # One rung, three registered actions. `Alpha` is driven to success, `Beta`
    # is dispatched only to assert a refusal, `Gamma` is never dispatched --
    # the three states an `actions` entry can be audited against.
    _ACTIONS = {"demo": frozenset({"Alpha", "Beta", "Gamma"})}
    _FACTS = [
        ScenarioFacts(rung="demo", path="scenarios/demo/a.scenario",
                      actions=frozenset({"Alpha", "Beta"}), chained_steps=0,
                      is_workflow=False, succeeded=frozenset({"Alpha"})),
    ]

    def _problems(self, entries: dict) -> list:
        return action_allowlist_problems(
            Allowlist(kinds={}, messages={}, actions=entries), self._ACTIONS, self._FACTS
        )

    def test_an_entry_for_an_undispatched_action_is_accepted(self) -> None:
        self.assertEqual(self._problems({"demo/Gamma": "runner-only principal"}), [])

    def test_an_entry_dispatched_only_to_assert_a_refusal_is_accepted(self) -> None:
        # The shipped shape: the exemption says the action cannot be driven to
        # completion, and a scenario calling it to assert its refusal is that
        # claim's evidence, not its refutation.
        self.assertEqual(self._problems({"demo/Beta": "no wire action hands back its id"}), [])

    def test_an_action_driven_to_success_retires_the_entry(self) -> None:
        problems = self._problems({"demo/Alpha": "cannot be driven"})
        self.assertTrue(any("demo/Alpha" in p and "drop the exemption" in p for p in problems))

    def test_an_empty_reason_is_a_problem(self) -> None:
        problems = self._problems({"demo/Gamma": "   "})
        self.assertTrue(any("demo/Gamma" in p and "reason" in p for p in problems))

    def test_an_action_that_is_no_longer_registered_is_a_problem(self) -> None:
        # A rename in the rung's C++ left this entry behind.
        problems = self._problems({"demo/Telepathy": "gone"})
        self.assertTrue(any("demo/Telepathy" in p and "no longer registered" in p for p in problems))

    def test_a_key_with_no_rung_separator_is_a_problem(self) -> None:
        problems = self._problems({"Gamma": "malformed"})
        self.assertTrue(any("Gamma" in p and "<rung>/<Action>" in p for p in problems))

    def test_a_key_naming_an_unknown_rung_is_a_problem(self) -> None:
        problems = self._problems({"telepathy/Gamma": "wrong rung"})
        self.assertTrue(any("telepathy" in p and "rung" in p for p in problems))

    def test_an_empty_reason_is_caught_even_on_a_malformed_key(self) -> None:
        # Both halves are reported: a bad key must not shadow a missing reason.
        problems = self._problems({"Gamma": ""})
        self.assertTrue(any("reason" in p for p in problems))
        self.assertTrue(any("<rung>/<Action>" in p for p in problems))

    def test_the_shipped_allowlist_passes_its_own_audit(self) -> None:
        # The real entries must not need editing: both are dispatched, neither
        # is driven to success.
        root = _repo_root() / "scripts" / "scenario"
        allowlist = load_allowlist(root / "coverage_allowlist.json")
        actions = extract_actions(shipped_action_sources(_repo_root() / "examples"))
        facts = [scenario_facts(s) for s in load_scenarios(root / "scenarios", recursive=True)]
        self.assertEqual(action_allowlist_problems(allowlist, actions, facts), [])


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

    def test_exits_one_when_the_protocol_axis_is_exempt_but_the_real_workflow_axis_still_gaps(self) -> None:
        # `_fixture_run` isolates the *protocol* axis (its own throwaway header,
        # wire header and scenario corpus) but leaves `--examples` at its
        # default, so the workflow axis still reads the real `examples/` tree
        # and its real, known gap. (A fixture *can* isolate the workflow axis
        # too -- see `test_exits_zero_when_both_axes_are_fully_covered_or_exempt`
        # -- this test deliberately does not, to prove the two gates are
        # independent: exempting the protocol axis alone must not be enough
        # for a clean exit when the other axis still has a real gap.) This
        # exercises the claim the protocol axis makes on its own: fully
        # exempted kinds/messages stop protocol-side problems from being
        # reported. The real corpus's workflow gap (see
        # `test_the_real_run_exits_one_for_the_known_workflow_gap`) still
        # drives the overall exit code to 1 -- both gates must be clean for 0.
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
        self.assertEqual(code, 1)

    def _clean_two_axis_run(self, tmp: str, action_entries: dict) -> int:
        """Runs the CLI over a fixture in which both axes are clean.

        Isolates every input -- header, wire header, scenario corpus, examples
        tree and floors -- so the run's only impurity is @p action_entries,
        the `actions` section written into the fixture allowlist. With `{}` the
        run is clean and exits 0; a caller passing a dishonest entry is asking
        what the actions audit does with it.

        @param tmp           A throwaway directory to build the fixture in.
        @param action_entries The allowlist's `actions` section.
        @return The CLI's exit code.
        """
        root = pathlib.Path(tmp)

        # Protocol axis: a one-kind, one-message header. The kind
        # ("register") ends up genuinely covered below -- every
        # per-rung workflow scenario's `client` step sends it -- so only
        # the message ("nope", which no scenario asserts) needs an
        # exemption.
        allow = root / "allow.json"
        allow.write_text(
            json.dumps({"messages": {"nope": "fixture"}, "actions": action_entries}),
            encoding="utf-8",
        )
        header = root / "remote.hpp"
        header.write_text(
            'if (env.kind == "register") {}\nreply(makeErr("nope", id));\n'
            + scenario_coverage.SERVER_HALF_MARKER
            + " {};\n",
            encoding="utf-8",
        )
        wire = root / "wire.hpp"
        wire.write_text(scenario_coverage.DECODE_FUNCTION_MARKER + " {}\n", encoding="utf-8")

        # Workflow axis: one throwaway rung tree per SERVER_RUNGS entry,
        # each registering one action that a matching per-rung scenario
        # dispatches -- so every registered action is dispatched and
        # nothing needs an actions allowlist entry. Each scenario's
        # `client` step also happens to cover the protocol axis's one
        # kind, "register".
        examples = root / "examples"
        scenarios = root / "scenarios"
        for rung in scenario_coverage.SERVER_RUNGS:
            (examples / rung).mkdir(parents=True)
            (examples / rung / "x.cpp").write_text(
                'BRIDGE_REGISTER_ACTION(demo::M, demo::A, "AName")\n', encoding="utf-8"
            )
            (scenarios / rung).mkdir(parents=True)
            (scenarios / rung / "w.scenario").write_text(
                "model M\n\nclient alice\n\ndo AName\nexpect ok\n", encoding="utf-8"
            )

        # None of these fixture scenarios chains captured state, so none
        # is a workflow by `WORKFLOW_MIN_CHAINED`'s definition -- the real
        # per-rung floors (8-20) would fail every rung. Authoring five
        # rungs' worth of genuine multi-step workflows here would be far
        # more contrived than using the floors-injection path the brief
        # calls out as acceptable, so every floor is overridden to 0.
        floors = {rung: 0 for rung in scenario_coverage.SERVER_RUNGS}

        return coverage_main(
            [
                "--header", str(header),
                "--wire-header", str(wire),
                "--scenarios", str(scenarios),
                "--allowlist", str(allow),
                "--examples", str(examples),
                "--no-floor",
                "--floors", json.dumps(floors),
            ]
        )

    def test_exits_zero_when_both_axes_are_fully_covered_or_exempt(self) -> None:
        # The genuine exit-0 path. `shipped_action_sources` used to be
        # hardwired to the real `examples/` tree -- the one input to `main`
        # that could not be redirected -- so no fixture could ever produce a
        # fully clean run, and this tool's success path went untested. With
        # `--examples` (and the hidden, testing-only `--floors`) it can be
        # isolated like every other input: a throwaway header, wire header,
        # scenario corpus AND examples tree, with everything either covered
        # or allowlisted/floored away.
        with tempfile.TemporaryDirectory() as tmp:
            code = self._clean_two_axis_run(tmp, {})
        self.assertEqual(code, 0)

    def test_exits_two_when_an_actions_entry_has_no_written_reason(self) -> None:
        # An otherwise clean run: the only thing wrong is an actions entry
        # exempting a live registered action with an empty reason. That used to
        # pass silently -- the `actions` section was audited by nothing.
        with tempfile.TemporaryDirectory() as tmp:
            code = self._clean_two_axis_run(tmp, {"kanban/AName": ""})
        self.assertEqual(code, 2)

    def test_exits_two_when_an_actions_entry_names_a_nonexistent_action(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            code = self._clean_two_axis_run(tmp, {"kanban/NoSuchAction": "written reason"})
        self.assertEqual(code, 2)

    def test_exits_two_when_floors_is_malformed(self) -> None:
        # `--floors` is testing-only, but a bad value must still fail as a
        # broken tool (exit 2), not silently fall back to the shipped floors.
        with tempfile.TemporaryDirectory() as tmp:
            code = self._fixture_run(
                tmp, _FIXTURE_HEADER, ["--no-floor", "--floors", "not json"]
            )
        self.assertEqual(code, 2)

    def test_the_real_run_is_green_on_both_axes(self) -> None:
        # This replaces `test_the_real_run_exits_one_for_the_known_workflow_gap`,
        # which pinned exit 1 while the corpus was still being written and said
        # in its own comment that authoring the rest was later work. That work
        # is done: every registered action is dispatched, every rung meets its
        # floor, and every envelope kind and refusal is covered or exempt with
        # a written reason.
        #
        # The assertion is kept, with the number flipped, for the same reason
        # it existed: a change that silently makes the shipped corpus stop
        # covering its own surface should fail here rather than pass quietly.
        self.assertEqual(coverage_main([]), 0)


_FIXTURE_ACTIONS = '''
BRIDGE_REGISTER_ACTION(demo::PasteModel, demo::CreatePaste, "CreatePaste")
BRIDGE_REGISTER_ACTION(demo::PasteModel, demo::GetPaste, "GetPaste", ::morph::model::Loggable::No)
  BRIDGE_REGISTER_ACTION(
      demo::PasteModel, demo::ListPastes, "ListPastes")
// BRIDGE_REGISTER_ACTION(demo::PasteModel, demo::NotReal, "NotReal")
'''


class ActionExtractionTest(unittest.TestCase):
    def test_extracts_the_registered_action_names(self) -> None:
        actions = extract_actions({"demo": _FIXTURE_ACTIONS})
        self.assertEqual(actions["demo"], frozenset({"CreatePaste", "GetPaste", "ListPastes"}))

    def test_extraction_spans_a_registration_split_across_lines(self) -> None:
        # Same failure mode the message regexes had: a reflowed call site must
        # not silently vanish from the universe.
        actions = extract_actions({"demo": _FIXTURE_ACTIONS})
        self.assertIn("ListPastes", actions["demo"])

    def test_floor_rejects_an_implausibly_small_action_universe(self) -> None:
        problems = action_floor_violations({"demo": frozenset({"CreatePaste"})})
        self.assertTrue(problems)
        self.assertTrue(any("action" in p for p in problems))

    def test_floor_accepts_the_real_examples_tree(self) -> None:
        actions = extract_actions(shipped_action_sources(_repo_root() / "examples"))
        self.assertEqual(action_floor_violations(actions), [])
        self.assertEqual(set(actions), set(SERVER_RUNGS))
        for rung in SERVER_RUNGS:
            self.assertTrue(actions[rung], f"{rung} registers no actions")

    def test_real_tree_pins_the_known_action_names(self) -> None:
        # Pins the exact set of action names per rung: a count-only assertion
        # is blind to renames and to add-plus-remove pairs in the same rung.
        # The name-set pin catches a rename, an addition, or a swap. When this
        # fails, a human decides whether the changed action needs a workflow or
        # an allowlist entry.
        actions = extract_actions(shipped_action_sources(_repo_root() / "examples"))
        self.assertEqual(actions["pastebin"], frozenset({"CreatePaste", "DeletePaste", "EditPaste", "ExpirePaste", "GetPaste", "ListPastes"}))
        self.assertEqual(actions["polls"], frozenset({"AddComment", "CreatePoll", "FinalizePoll", "GetEventsSince", "GetPollState", "OpenPoll", "SubmitVotes", "UndoLastVoteChange", "UpdateVotes"}))
        self.assertEqual(actions["bookmarks"], frozenset({"ArchiveBookmark", "BulkEdit", "CreateBookmark", "DeleteBookmark", "EditBookmark", "ExportBookmarks", "GetBookmark", "GetChangesSince", "ImportBookmarks", "ListBookmarks", "ListSharedFeed", "ListTags", "Login", "MergeTags", "RecordMetadata", "RenameTag", "UnarchiveBookmark"}))
        self.assertEqual(actions["ledger"], frozenset({"CreateBudget", "CreateCategory", "CreateRule", "GetBudgetReport", "GetLedger", "GetReportStatus", "ImportLedgerChunk", "LinkAccountToCategory", "Login", "OpenAccount", "RunReportJob", "SetBudgetLimit", "SetCategory", "StoreTransaction", "SubmitReport", "UndoTransaction", "UpdateRule"}))
        self.assertEqual(actions["kanban"], frozenset({"AddAttachment", "AddComment", "ApplyTagMutation", "CreateColumn", "CreateProject", "CreateRule", "CreateSwimlane", "CreateTask", "DeleteRule", "GetActivity", "GetAttachments", "GetBoardState", "GetEventsSince", "GetMyProjects", "GetProjectRoles", "GetRules", "Login", "MoveTaskPosition", "OpenBoard", "RemoveAttachment", "RemoveMember", "SetMemberRole"}))
        # Total count provides a quick sanity check tied to MIN_ACTIONS.
        total = sum(len(names) for names in actions.values())
        self.assertEqual(total, 71)


_FLAT_LIST = '''
model PasteModel
client alice
do CreatePaste content="a"
expect ok capture one=$.id
do ListPastes
expect ok field pastes ~ .
do GetPaste id=fixed-not-captured
expect err message == "GetPaste: no such paste"
'''

_REAL_WORKFLOW = '''
model PasteModel
client alice
do CreatePaste content="a"
expect ok capture id=$.id
do GetPaste id=$id
expect ok field content == "a"
do EditPaste id=$id content="b"
expect ok
do ListPastes
expect ok field pastes ~ $id
do DeletePaste id=$id
expect ok
'''

# Four distinct actions and three chained steps -- but every chained step is a
# `session`, reinstalling one login's credentials on a second and third client,
# and the four `do` calls are mutually independent. Nothing threads a result
# from one action into the next, so this is a flat list wearing a workflow's
# arithmetic: exactly the file the do-only chaining rule exists to refuse.
_SESSION_CHAINED_FLAT_LIST = '''
model LedgerModel
client auth model=AuthModel
do Login username=alice
expect ok capture token=$.token
expect ok capture who=$.principal
client one model=LedgerModel
session principal=$who token=$token
do GetLedger ledgerId=1
expect ok field accounts ~ .
client two model=LedgerModel
session principal=$who token=$token
do OpenAccount ledgerId=1 name="Cash" kind=0 currency=1
expect ok field name == "Cash"
client three model=BudgetModel
session principal=$who token=$token
do CreateCategory ledgerId=1 name="Drinks"
expect ok
do GetBudgetReport budgetId=7 month="2026-08"
expect ok field limit.num == 5000
'''


class WorkflowClassificationTest(unittest.TestCase):
    def test_a_flat_list_of_calls_is_not_a_workflow(self) -> None:
        # Three distinct actions, but nothing consumes the captured id, so this
        # is three one-shot calls sharing a socket.
        facts = scenario_facts(parse_scenario(_FLAT_LIST, "scenarios/pastebin/flat.scenario"))
        self.assertEqual(facts.chained_steps, 0)
        self.assertFalse(facts.is_workflow)

    def test_a_threaded_sequence_is_a_workflow(self) -> None:
        facts = scenario_facts(parse_scenario(_REAL_WORKFLOW, "scenarios/pastebin/w.scenario"))
        self.assertGreaterEqual(facts.chained_steps, WORKFLOW_MIN_CHAINED)
        self.assertTrue(facts.is_workflow)

    def test_records_the_actions_and_the_rung(self) -> None:
        facts = scenario_facts(parse_scenario(_REAL_WORKFLOW, "scenarios/pastebin/w.scenario"))
        self.assertEqual(
            facts.actions,
            frozenset({"CreatePaste", "GetPaste", "EditPaste", "ListPastes", "DeletePaste"}),
        )
        self.assertEqual(facts.rung, "pastebin")

    def test_a_file_directly_in_scenarios_has_no_rung(self) -> None:
        facts = scenario_facts(parse_scenario(_REAL_WORKFLOW, "scenarios/loose.scenario"))
        self.assertEqual(facts.rung, "")

    def test_enough_chaining_but_too_few_actions_is_not_a_workflow(self) -> None:
        # Guards WORKFLOW_MIN_ACTIONS independently of WORKFLOW_MIN_CHAINED:
        # create-then-read-repeatedly threads state but goes nowhere.
        text = (
            'model PasteModel\nclient alice\n'
            'do CreatePaste content="a"\nexpect ok capture id=$.id\n'
            'do GetPaste id=$id\nexpect ok field content == "a"\n'
            'do GetPaste id=$id\nexpect ok field readCount.num == 2\n'
            'do GetPaste id=$id\nexpect ok field readCount.num == 3\n'
        )
        facts = scenario_facts(parse_scenario(text, "scenarios/pastebin/x.scenario"))
        self.assertGreaterEqual(facts.chained_steps, WORKFLOW_MIN_CHAINED)
        self.assertEqual(len(facts.actions), 2)
        self.assertFalse(facts.is_workflow)

    def test_chaining_only_on_non_do_steps_is_not_a_workflow(self) -> None:
        # Five actions, and three steps that reference an earlier capture --
        # but all three are `session` steps reusing one login's token, and the
        # `do` calls thread nothing between themselves. Counting a chained step
        # on any verb made this qualify; only `do` chaining counts now.
        facts = scenario_facts(
            parse_scenario(_SESSION_CHAINED_FLAT_LIST, "scenarios/ledger/fake.scenario")
        )
        self.assertGreaterEqual(len(facts.actions), WORKFLOW_MIN_ACTIONS)
        self.assertEqual(facts.chained_steps, 0)
        self.assertFalse(facts.is_workflow)

    def test_the_shipped_ledger_scenario_is_still_a_workflow(self) -> None:
        # The other half of the do-only rule: the corpus's one real journey
        # chains through `do` steps, so tightening the rule must not cost it.
        path = (_repo_root() / "scripts" / "scenario" / "scenarios" / "ledger"
                / "open-account-transact-report-close.scenario")
        facts = scenario_facts(parse_scenario(path.read_text(encoding="utf-8"), str(path)))
        self.assertTrue(facts.is_workflow)
        self.assertGreaterEqual(facts.chained_steps, WORKFLOW_MIN_CHAINED)

    def test_records_which_actions_a_scenario_drives_to_success(self) -> None:
        # `succeeded` is what the action allowlist is audited against: a `do`
        # asserted with `expect err` is dispatched but not driven to completion.
        text = (
            'model LedgerModel\nclient alice\n'
            'do GetLedger ledgerId=1\nexpect ok field accounts ~ .\n'
            'do UndoTransaction ledgerId=1 journalId=999999\nexpect err message ~ "journal"\n'
        )
        facts = scenario_facts(parse_scenario(text, "scenarios/ledger/x.scenario"))
        self.assertEqual(facts.actions, frozenset({"GetLedger", "UndoTransaction"}))
        self.assertEqual(facts.succeeded, frozenset({"GetLedger"}))

    def test_load_scenarios_recurses_when_asked(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = pathlib.Path(tmp)
            (root / "pastebin").mkdir()
            (root / "pastebin" / "a.scenario").write_text(_REAL_WORKFLOW, encoding="utf-8")
            (root / "b.scenario").write_text(_REAL_WORKFLOW, encoding="utf-8")
            self.assertEqual(len(load_scenarios(root)), 1)
            self.assertEqual(len(load_scenarios(root, recursive=True)), 2)


class CorpusLayoutTest(unittest.TestCase):
    def test_every_shipped_scenario_lives_under_a_rung_directory(self) -> None:
        # The rung is the parent directory name, and that is how per-rung action
        # coverage is attributed. A file loose in scenarios/ would be counted
        # against no rung at all and silently excluded from every floor.
        root = _repo_root() / "scripts" / "scenario" / "scenarios"
        loose = sorted(p.name for p in root.glob("*.scenario"))
        self.assertEqual(loose, [], "scenario files must live in scenarios/<rung>/")

    def test_every_rung_directory_names_a_real_server_rung(self) -> None:
        root = _repo_root() / "scripts" / "scenario" / "scenarios"
        found = sorted(p.name for p in root.iterdir() if p.is_dir())
        for name in found:
            self.assertIn(name, SERVER_RUNGS, f"scenarios/{name}/ is not a rung that ships a server")


class WorkflowGateTest(unittest.TestCase):
    _ACTIONS = {"demo": frozenset({"Alpha", "Beta", "Gamma"})}

    def _facts(self, count: int, actions: frozenset) -> list:
        return [
            ScenarioFacts(rung="demo", path=f"scenarios/demo/{i}.scenario",
                          actions=actions, chained_steps=2, is_workflow=True)
            for i in range(count)
        ]

    def test_reports_an_action_no_scenario_dispatches(self) -> None:
        facts = self._facts(1, frozenset({"Alpha", "Beta"}))
        problems = workflow_problems(self._ACTIONS, facts, Allowlist(kinds={}, messages={}, actions={}),
                                     floors={"demo": 1})
        self.assertTrue(any("Gamma" in p for p in problems))

    def test_an_allowlisted_action_is_not_reported(self) -> None:
        facts = self._facts(1, frozenset({"Alpha", "Beta"}))
        allow = Allowlist(kinds={}, messages={}, actions={"demo/Gamma": "runner-only principal"})
        problems = workflow_problems(self._ACTIONS, facts, allow, floors={"demo": 1})
        self.assertFalse(any("Gamma" in p for p in problems))

    def test_reports_a_rung_below_its_workflow_floor(self) -> None:
        facts = self._facts(2, frozenset({"Alpha", "Beta", "Gamma"}))
        problems = workflow_problems(self._ACTIONS, facts, Allowlist(kinds={}, messages={}, actions={}),
                                     floors={"demo": 5})
        self.assertTrue(any("floor" in p and "demo" in p for p in problems))

    def test_non_workflow_files_do_not_count_towards_the_floor(self) -> None:
        # Five files that are flat lists must not satisfy a floor of 5.
        flat = [
            ScenarioFacts(rung="demo", path=f"scenarios/demo/{i}.scenario",
                          actions=frozenset({"Alpha", "Beta", "Gamma"}),
                          chained_steps=0, is_workflow=False)
            for i in range(5)
        ]
        problems = workflow_problems(self._ACTIONS, flat, Allowlist(kinds={}, messages={}, actions={}),
                                     floors={"demo": 5})
        self.assertTrue(any("floor" in p for p in problems))

    def test_the_shipped_floors_name_only_real_rungs(self) -> None:
        for rung in WORKFLOW_FLOORS:
            self.assertIn(rung, SERVER_RUNGS)
        self.assertEqual(set(WORKFLOW_FLOORS), set(SERVER_RUNGS))


class RungTallyTest(unittest.TestCase):
    # One rung, three actions: one gets dispatched, one is undispatched but
    # allowlisted, one is undispatched and unexempt -- the three buckets
    # `rung_tallies` must partition `registered` into.
    _ACTIONS = {"demo": frozenset({"Alpha", "Beta", "Gamma"})}

    def test_covers_all_three_buckets_and_the_partition_identity(self) -> None:
        facts = [
            ScenarioFacts(rung="demo", path="scenarios/demo/a.scenario",
                          actions=frozenset({"Alpha"}), chained_steps=0, is_workflow=False),
        ]
        allow = Allowlist(kinds={}, messages={}, actions={"demo/Beta": "runner-only principal"})
        tally = rung_tallies(self._ACTIONS, facts, allow, floors={"demo": 0})["demo"]
        self.assertEqual(tally.registered, frozenset({"Alpha", "Beta", "Gamma"}))
        self.assertEqual(tally.dispatched, frozenset({"Alpha"}))
        self.assertEqual(tally.exempt, frozenset({"Beta"}))
        self.assertEqual(tally.undispatched, frozenset({"Gamma"}))
        self.assertEqual(
            len(tally.dispatched) + len(tally.exempt) + len(tally.undispatched),
            len(tally.registered),
        )

    def test_an_allowlisted_action_that_is_also_dispatched_counts_as_dispatched_not_exempt(self) -> None:
        # The double-count regression this whole tally exists to prevent: an
        # action can be both allowlisted and dispatched (a scenario may call
        # it only to assert its refusal, without driving it to completion).
        # It must land in `dispatched` alone -- counting it in `exempt` too
        # would put it in two buckets at once and break the partition
        # identity checked above.
        facts = [
            ScenarioFacts(rung="demo", path="scenarios/demo/a.scenario",
                          actions=frozenset({"Alpha", "Beta"}), chained_steps=0, is_workflow=False),
        ]
        allow = Allowlist(kinds={}, messages={}, actions={"demo/Beta": "runner-only principal"})
        tally = rung_tallies(self._ACTIONS, facts, allow, floors={"demo": 0})["demo"]
        self.assertIn("Beta", tally.dispatched)
        self.assertNotIn("Beta", tally.exempt)
        self.assertEqual(
            len(tally.dispatched) + len(tally.exempt) + len(tally.undispatched),
            len(tally.registered),
        )


class ReportGateAgreementTest(unittest.TestCase):
    # A protocol axis that is trivially fully covered (nothing in the
    # surface, nothing exercised, nothing to allowlist), so both cases below
    # isolate the workflow axis: whatever `_render` prints and whatever
    # `workflow_problems` returns must describe the same gap, or its absence.
    _SURFACE = Surface(kinds=frozenset(), exact_messages=frozenset(), message_prefixes=frozenset())
    _EXERCISED = Exercised(kinds=frozenset(), messages=frozenset())
    _ALLOWLIST = Allowlist(kinds={}, messages={}, actions={})
    _ACTIONS = {"demo": frozenset({"Alpha", "Beta"})}

    def test_a_real_gap_is_named_by_both_the_gate_and_the_report(self) -> None:
        facts: list[ScenarioFacts] = []
        problems = workflow_problems(self._ACTIONS, facts, self._ALLOWLIST, floors={"demo": 0})
        self.assertTrue(problems)
        text = _render(
            self._SURFACE, self._EXERCISED, self._ALLOWLIST, self._ACTIONS, facts, floors={"demo": 0}
        )
        self.assertIn("WORKFLOW GAPS", text)
        self.assertIn("demo", text)
        self.assertIn("Alpha", text)
        self.assertIn("Beta", text)

    def test_the_printed_exemption_list_agrees_with_the_per_rung_count(self) -> None:
        # `Beta` is dispatched (only to assert a refusal), so it grants no
        # exemption and the rung line says "(0 exempt)". Printing it under
        # "Actions exempt, with reasons" claimed an exemption the arithmetic
        # never granted; it is grouped apart now.
        facts = [
            ScenarioFacts(rung="demo", path="scenarios/demo/a.scenario",
                          actions=frozenset({"Alpha", "Beta"}), chained_steps=0,
                          is_workflow=False, succeeded=frozenset({"Alpha"})),
        ]
        allow = Allowlist(kinds={}, messages={}, actions={"demo/Beta": "only its refusal is assertable"})
        text = _render(
            self._SURFACE, self._EXERCISED, allow, self._ACTIONS, facts, floors={"demo": 0}
        )
        self.assertIn("(0 exempt)", text)
        self.assertNotIn("Actions exempt, with reasons:", text)
        self.assertIn("granting no exemption", text)
        self.assertIn("demo/Beta", text)

    def test_an_entry_that_does_grant_an_exemption_is_printed_as_exempt(self) -> None:
        # The other half: `Beta` is registered, dispatched by nobody and
        # allowlisted, so it is counted in the rung's "(1 exempt)" and belongs
        # under the exempt heading.
        facts = [
            ScenarioFacts(rung="demo", path="scenarios/demo/a.scenario",
                          actions=frozenset({"Alpha"}), chained_steps=0,
                          is_workflow=False, succeeded=frozenset({"Alpha"})),
        ]
        allow = Allowlist(kinds={}, messages={}, actions={"demo/Beta": "no wire action hands back its id"})
        text = _render(
            self._SURFACE, self._EXERCISED, allow, self._ACTIONS, facts, floors={"demo": 0}
        )
        self.assertIn("(1 exempt)", text)
        self.assertIn("Actions exempt, with reasons:", text)
        self.assertNotIn("granting no exemption", text)

    def test_no_gap_is_named_by_neither_the_gate_nor_the_report(self) -> None:
        facts = [
            ScenarioFacts(rung="demo", path="scenarios/demo/a.scenario",
                          actions=frozenset({"Alpha", "Beta"}), chained_steps=0, is_workflow=False),
        ]
        problems = workflow_problems(self._ACTIONS, facts, self._ALLOWLIST, floors={"demo": 0})
        self.assertEqual(problems, [])
        text = _render(
            self._SURFACE, self._EXERCISED, self._ALLOWLIST, self._ACTIONS, facts, floors={"demo": 0}
        )
        self.assertNotIn("WORKFLOW GAPS", text)
        self.assertIn("Every registered action is dispatched and every rung meets its floor.", text)


class PortLineTests(unittest.TestCase):
    """The one line of a server's output the driver has to understand.

    The five ladder servers do not agree on how to print it: four use
    `ws://127.0.0.1:<port>`, pastebin uses `port <port>`, and kanban prints a
    *second* line for its attachment side channel that must never be mistaken
    for the WebSocket one.
    """

    def test_reads_the_ws_form(self) -> None:
        self.assertEqual(
            run_scenarios.parse_port("bookmarks-server: listening on ws://127.0.0.1:46757"), 46757
        )

    def test_reads_the_bare_port_form(self) -> None:
        self.assertEqual(run_scenarios.parse_port("pastebin-server: listening on port 41221"), 41221)

    def test_ignores_kanbans_attachment_side_channel(self) -> None:
        self.assertIsNone(
            run_scenarios.parse_port(
                "kanban-server: attachment side channel listening on http://127.0.0.1:5001"
            )
        )

    def test_ignores_unrelated_output(self) -> None:
        self.assertIsNone(run_scenarios.parse_port("kanban-server: migrating schema"))


class RungSpecTests(unittest.TestCase):
    """The driver's rung table, checked against the tool it must agree with."""

    def test_covers_exactly_the_rungs_the_coverage_report_measures(self) -> None:
        self.assertEqual(set(run_scenarios.RUNGS), set(SERVER_RUNGS))

    def test_every_rung_names_its_port_variable(self) -> None:
        for rung, spec in run_scenarios.RUNGS.items():
            with self.subTest(rung=rung):
                self.assertTrue(spec.port_var.endswith("_PORT"), spec.port_var)
                self.assertEqual(spec.binary, f"ladder_{rung}_server")

    def test_only_ledger_seeds_and_it_seeds_ledgers(self) -> None:
        """Ledger is the one rung whose root entity no action can create.

        `ledgers` rows are produced by no registered action, so `OpenAccount
        ledgerId=1` against a fresh database is refused. Every other rung
        creates its own root entity over the wire (`CreatePoll`,
        `CreateProject`, `CreateBookmark`, `CreatePaste`) and must therefore
        seed nothing -- a rung that quietly gained a seed would be a scenario
        asserting state no client could have produced.
        """
        seeded = {rung for rung, spec in run_scenarios.RUNGS.items() if spec.seed_sql}
        self.assertEqual(seeded, {"ledger"})
        for statement in run_scenarios.RUNGS["ledger"].seed_sql:
            self.assertIn("INSERT OR IGNORE INTO ledgers", statement)

    def test_environment_binds_the_port_to_zero_and_names_the_database(self) -> None:
        spec = run_scenarios.RUNGS["ledger"]
        env = spec.environment(pathlib.Path("/tmp/x.db"))
        self.assertEqual(env["LEDGER_PORT"], "0")
        self.assertIn("Database=/tmp/x.db", env["LEDGER_DB"])
        self.assertEqual(env["QT_QPA_PLATFORM"], "offscreen")


if __name__ == "__main__":
    unittest.main(verbosity=2)

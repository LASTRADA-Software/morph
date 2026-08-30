#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Self-test for the parts of the scenario runner that need no server:
the tokenizer, the value syntax, the field paths, the comparison rules and
the parser's own refusals.

    python3 scripts/scenario/test_morph_scenario.py
"""

from __future__ import annotations

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

from scenario_coverage import (
    MIN_KINDS,
    MIN_MESSAGES,
    Exercised,
    Surface,
    _repo_root,
    covers,
    exercised_by,
    extract_surface,
    floor_violations,
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
        surface = extract_surface(_real_header_text())
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

    def test_real_header_carries_the_kinds_the_spec_records(self) -> None:
        surface = extract_surface(_real_header_text())
        self.assertEqual(
            surface.kinds,
            frozenset({"register", "execute", "deregister", "hello",
                       "attach", "assign", "instances", "schemas"}),
        )


_FIXTURE_SCENARIO = '''
model PasteModel
client alice
do CreatePaste content="x"
expect ok field id ~ .
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


if __name__ == "__main__":
    unittest.main(verbosity=2)

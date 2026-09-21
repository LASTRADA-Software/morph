// SPDX-License-Identifier: Apache-2.0
//
// Prose that NAMES the characters must not be flagged, or this gate would make
// its own documentation unwritable -- the same allowance
// scripts/check_nolint_directives.sh makes for the sentence in
// include/morph/detail/fixed_string.hpp that names NOLINTNEXTLINE.
//
// A raw U+200E LEFT-TO-RIGHT MARK in a string literal is a silently wrong
// assertion; U+202E RIGHT-TO-LEFT OVERRIDE and U+2066..U+2069, the isolates,
// are the ones that reorder rendered text. Write them as \u200E, \u202E and
// so on. U+061C, the ARABIC LETTER MARK, is the one morph#628 typed by
// accident.

#pragma once

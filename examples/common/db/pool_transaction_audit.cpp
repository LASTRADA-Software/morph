// SPDX-License-Identifier: Apache-2.0
#include "db/pool_transaction_audit.hpp"

/// @file
/// The self-containment check for `db/pool_transaction_audit.hpp`, and the
/// only translation unit in this directory.
///
/// Both of those are load-bearing, and the second is the less obvious one.
/// `clang-tidy` resolves a file's compile command from
/// `compile_commands.json`, and a header never has an entry of its own: for a
/// changed header, `clang-tidy-diff.py` interpolates the *nearest* entry and
/// analyses it with that. `.github/workflows/ci.yml`'s changed-source filter
/// deliberately does not skip headers, so the interpolation is what the
/// clang-tidy-diff gate actually runs. With no translation unit anywhere in
/// this directory the nearest entry was one that does not carry Lightweight's
/// include path, and analysing the header with it produced
/// `'Lightweight/SqlConnection.hpp' file not found` -- a
/// `clang-diagnostic-error` that fails the gate, plus a scatter of findings
/// from the broken parse (measured before this file existed: a
/// `cppcoreguidelines-pro-type-member-init` naming two fields the constructor
/// visibly initialises, and a `readability-identifier-naming` on a member
/// spelled exactly like every other member in the tree).
///
/// This file gives the directory an entry whose command is right, which is
/// what the interpolation then picks up. It earns its place on its own terms
/// too: compiling the header alone, first, is how a missing `#include` in it
/// is caught here rather than in whichever consumer happens to include it
/// second.
///
/// It is deliberately **not** a Catch2 translation unit and deliberately not
/// in `examples/common/testkit/`: that directory's `.clang-tidy` subtracts
/// `bugprone-chained-comparison` on an argument about Catch2's `REQUIRE`
/// expansion, and a library .cpp placed there would inherit that subtraction
/// on the strength of an argument that does not apply to it. Nothing enforces
/// the split, so it is kept by hand. The audit's behavioural tests live in
/// `testkit/test_pool_transaction_audit.cpp`, where they belong.

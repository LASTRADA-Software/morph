// SPDX-License-Identifier: Apache-2.0
//
// The regression fixture for route-count sensitivity: a domain model that is a *DAG*
// rather than a tree, compiled through `morph::forms::schemaJson<A>()`.
//
// A nested-aggregate recursion that carries the ancestor
// chain as a template parameter pack, so `annotateNestedAggregate<Leaf,
// Ancestors...>` was a distinct instantiation **per distinct root-to-node
// route** through the type graph. A tree has one route per node; a DAG has as
// many as the graph has paths, and that count grows exponentially in the
// graph's depth. The recursion then carried a depth counter instead, capping
// instantiations at one per (type, depth) pair, and carrying nothing at all
// keys them on the type alone,
// leaving one instantiation per type. This fixture measures neither directly
// -- it measures route sensitivity, which both changes remove.
//
// This file is compiled twice by tests/compile_checks/forms_dag_budget.cmake,
// and the whole point is that the two compilations differ *only* in route
// count:
//
//   MORPH_FORMS_DAG_PROBE_TREE defined   — every member of `A<K>_<I>` has type
//       `A<K-1>_<I>`, so exactly one chain of types is reachable from the root
//       and there is one route to each of them. The control.
//   MORPH_FORMS_DAG_PROBE_TREE undefined — every member of `A<K>_<I>` spans all
//       three types of the level below, so the route count from the root is
//       3^8 = 6,561. The fixture.
//
// Same number of members per struct, same nesting depth, same emitted schema
// size to within the extra `$defs` entries: the *only* thing that differs is
// how many routes reach a node. So a ratio between the two compile times
// measures route-count sensitivity and nothing else — not the machine's speed,
// not the cost of including forms.hpp, not glaze's own schema writer.
//
// Measured on this file at 3 members per struct and 8 levels (6,561 routes),
// g++ 16.2.1, `-std=c++23 -fsyntax-only`, best of 2 runs on a shared machine,
// CPU seconds:
//
//   revision                     control (tree)   fixture (DAG)   ratio
//   master @ a9cb5649 (before)        2.70            26.83         9.9
//   with a depth counter              2.69             2.98         1.11
//
// The spread between runs reached 50% on that machine, which is the other
// reason the budget script asserts the ratio rather than either absolute
// number, and takes the fastest of several runs.

#include <cstdio>
#include <morph/forms/forms.hpp>

namespace morph_forms_dag_probe {

// Three types per level, three members each. Widening the fan-out raises the
// route count as fan^levels while leaving the (type, depth) pair count — what
// the fixed recursion actually instantiates — at fan * levels.
#if defined(MORPH_FORMS_DAG_PROBE_TREE)
#define MORPH_FORMS_DAG_PROBE_MEMBERS(P, I) \
    A##P##_##I m0{};                        \
    A##P##_##I m1{};                        \
    A##P##_##I m2{};
#else
#define MORPH_FORMS_DAG_PROBE_MEMBERS(P, I) \
    A##P##_0 m0{};                          \
    A##P##_1 m1{};                          \
    A##P##_2 m2{};
#endif

// No NOLINT for the macros below: this file is never in compile_commands.json
// (the budget script invokes the compiler on it directly, and no target owns
// it), so clang-tidy never analyses it and a suppression here would suppress
// nothing. The shape under test is "many near-identical aggregate types",
// which is what a macro is for.
#define MORPH_FORMS_DAG_PROBE_LEVEL(K, P)             \
    struct A##K##_0 {                                 \
        MORPH_FORMS_DAG_PROBE_MEMBERS(P, 0) int v {}; \
    };                                                \
    struct A##K##_1 {                                 \
        MORPH_FORMS_DAG_PROBE_MEMBERS(P, 1) int v {}; \
    };                                                \
    struct A##K##_2 {                                 \
        MORPH_FORMS_DAG_PROBE_MEMBERS(P, 2) int v {}; \
    };

struct A0_0 {
    int v{};
};
struct A0_1 {
    int v{};
};
struct A0_2 {
    int v{};
};

MORPH_FORMS_DAG_PROBE_LEVEL(1, 0)
MORPH_FORMS_DAG_PROBE_LEVEL(2, 1)
MORPH_FORMS_DAG_PROBE_LEVEL(3, 2)
MORPH_FORMS_DAG_PROBE_LEVEL(4, 3)
MORPH_FORMS_DAG_PROBE_LEVEL(5, 4)
MORPH_FORMS_DAG_PROBE_LEVEL(6, 5)
MORPH_FORMS_DAG_PROBE_LEVEL(7, 6)
MORPH_FORMS_DAG_PROBE_LEVEL(8, 7)

/// The action type the schema is generated for: eight nested-aggregate levels
/// below it. That was inside `morph::forms::detail::kMaxNestDepth` when this
/// fixture was written, on purpose — it exists to stress route count and must
/// not double as a test of a depth limit. There is no such limit, so
/// the depth is now only a shape choice; the fixture is left unchanged so its
/// numbers stay comparable with the ones quoted above.
using RootAction = A8_0;

}  // namespace morph_forms_dag_probe

int main() {
    // The call, not just the instantiation: printing the size keeps the whole
    // schema pipeline reachable, so nothing here can be optimised away into a
    // compilation that never ran the generator.
    auto const& schema = morph::forms::schemaJson<morph_forms_dag_probe::RootAction>();
    std::printf("%zu\n", schema.size());
    return schema.empty() ? 1 : 0;
}

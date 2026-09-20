// SPDX-License-Identifier: Apache-2.0

#pragma once

/// @file detail/quantity_equation.hpp
/// @brief `Quantity::equation()` renderer (provenance builds only).
///
/// Included by `morph/util/quantity.hpp` when `MORPH_QUANTITY_PROVENANCE` is on.
/// Walks the shared derivation DAG and produces the print-ready `equation()`
/// lines: formula, substitution, result, and a `where` legend for reused
/// values. See `docs/spec/util/quantity_type.md` for the output contract.

#include <cstdint>
#include <format>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "../attributes.hpp"
// Included back deliberately, and not circular: `util/quantity.hpp` includes
// this file at the very end, `#pragma once` makes the inner visit a no-op, and
// nothing in that header past the include point needs anything defined here.
// It is what makes this file self-contained, which matters because a tool that
// opens a header on its own -- clang-tidy analysing a changed header, an IDE,
// include-what-you-use -- otherwise sees `unknown type name 'ASTNode'` on
// every line and reports a cascade of findings about code that compiles fine.
#include "../util/quantity.hpp"
#include "../util/rational.hpp"

namespace morph::units::detail {

/// @brief Formats an optional value as `equation()` prints numbers (decimals).
/// @param value The optional payload.
/// @return The formatted number, or `"N/A"` when empty.
[[nodiscard]] inline std::string formatOptional(const std::optional<morph::math::Rational>& value) {
    return formatOptionalDecimal(value);
}

/// @brief Whether a node is a plain leaf (no recorded operation).
/// @param node The node.
/// @return `true` for a leaf.
[[nodiscard]] inline bool isLeafNode(const ASTNode& node) { return node.current.operation.empty(); }

/// @brief Whether a node is a unit-conversion step.
/// @param node The node.
/// @return `true` for a conversion.
[[nodiscard]] inline bool isConversionNode(const ASTNode& node) {
    return node.current.operation.starts_with("convert");
}

/// @brief Whether a node renders as a single atom (leaf or conversion value).
/// @param node The node.
/// @return `true` when atomic.
[[nodiscard]] inline bool isAtomNode(const ASTNode& node) { return isLeafNode(node) || isConversionNode(node); }

/// @brief The value a node contributes (leaf value, else the step result).
/// @param node The node. Borrowed: the returned reference points into it, so it
///             must outlive every use of the result.
/// @return The node's optional value.
[[nodiscard]] inline const std::optional<morph::math::Rational>& nodeValue(const ASTNode& node MORPH_LIFETIMEBOUND) {
    return isLeafNode(node) ? node.current.lhs : node.current.result;
}

/// @brief A rendered subexpression plus the precedence of its top operator.
struct Rendered {
    /// @brief The rendered text.
    std::string text;

    /// @brief Precedence of the outermost operator (100 for an atom).
    int precedence{100};
};

/// @brief Which of the two renderings `EquationRenderer::render` produces.
enum class RenderMode : std::uint8_t {
    /// @brief Symbolic: a named node prints as its name and a reused node as
    ///        its `cK` placeholder, neither expanded.
    symbolic,

    /// @brief Substituted: a named node and a reused node both print as their
    ///        value.
    substituted,
};

/// @brief One suspended visit in the iterative renderer — what a recursive
///        `renderSymbolic` call would have kept in its stack frame.
struct RenderFrame {
    /// @brief The node being rendered.
    const ASTNode* node{nullptr};

    /// @brief Whether to expand @ref node itself rather than label it.
    bool expandThis{false};

    /// @brief 0 = not started, 1 = left operand rendered, 2 = both rendered.
    int stage{0};

    /// @brief The left operand's rendering, once stage 1 is reached.
    Rendered left;
};

/// @brief One suspended visit in the iterative `assignLabels` walk.
struct LabelFrame {
    /// @brief The node to label or descend through.
    const ASTNode* node{nullptr};

    /// @brief Whether to expand @ref node itself rather than label it.
    bool expandThis{false};
};

/// @brief Stateful renderer for one `equation()` call.
struct EquationRenderer {
    /// @brief Builds a renderer with a step limit.
    /// @param steps How many derivation steps may be written out in total.
    explicit EquationRenderer(std::size_t steps) : maxSteps(steps), stepBudget(steps) {}

    /// @brief The limit this render was asked for (quoted in the legend).
    std::size_t maxSteps;

    /// @brief How many steps `assignLabels` may still spend expanding.
    std::size_t stepBudget;

    /// @brief Placeholder number per reused, unnamed node (1-based).
    std::unordered_map<const ASTNode*, std::size_t> labelIndex;

    /// @brief In-edge count per node across displayed (non-opaque) paths.
    std::unordered_map<const ASTNode*, int> refCount;

    /// @brief Nodes visited during ref counting (dedup).
    std::unordered_set<const ASTNode*> seen;

    /// @brief Reused-node placeholders, in first-appearance order.
    std::vector<const ASTNode*> placeholderOrder;

    /// @brief Nodes `assignLabels` has already settled (label, elision, or
    ///        neither). A node's second visit can only repeat the first one's
    ///        verdict, so it is skipped.
    std::unordered_set<const ASTNode*> settled;

    /// @brief Elision number per node the step limit cut (1-based).
    std::unordered_map<const ASTNode*, std::size_t> elisionIndex;

    /// @brief Elided nodes, in first-appearance order.
    std::vector<const ASTNode*> elisionOrder;

    /// @brief Whether a node earns a placeholder (reused). Only ever called on
    ///        unnamed nodes recorded by `countRefs` (callers return early on
    ///        named nodes), so the lookup is total.
    /// @param node The node (never null, never named at any call site).
    /// @return `true` when it is a placeholder.
    [[nodiscard]] bool isPlaceholder(const ASTNode* node) const { return refCount.at(node) >= 2; }

    /// @brief Counts in-edges through displayed paths (stops at opaque nodes).
    ///
    /// Iterative, over an explicit worklist, for the reason spelled out on
    /// `render` below: the derivation of a running total is a linear chain and
    /// a recursive walk of it overflows the stack (morph#574). Each node is
    /// counted exactly once (the `seen` set), so the worklist order does not
    /// affect the counts.
    /// @param root The node to start from (may be null).
    void countRefs(const ASTNode* root) {
        std::vector<const ASTNode*> pending;
        pending.push_back(root);
        while (!pending.empty()) {
            const ASTNode* node = pending.back();
            pending.pop_back();
            // Children are pushed unconditionally and the null case handled
            // here, exactly as the recursive form handled it on entry: the
            // absent operand of a leaf, unary or scalar step is a null child.
            if (node == nullptr || seen.contains(node)) {
                continue;
            }
            seen.insert(node);
            if (node->name.has_value() || isAtomNode(*node)) {
                continue;
            }
            if (node->left) {
                ++refCount[node->left.get()];
            }
            if (node->right) {
                ++refCount[node->right.get()];
            }
            pending.push_back(node->left.get());
            pending.push_back(node->right.get());
        }
    }

    /// @brief Assigns placeholder labels, and decides where the step limit cuts.
    ///
    /// Iterative for the same reason as `countRefs`. First appearance is a
    /// left-before-right pre-order, so the right child is pushed first and the
    /// left one popped first — the order a recursive walk would have visited
    /// them in.
    ///
    /// This pass is also where `maxSteps` is spent, and it is spent **once**
    /// for the whole `equation()` call rather than per rendering. The two
    /// renderings and every legend line then consult the one `elisionIndex`
    /// they all share, so the formula, the substitution and the legend agree
    /// on which sub-derivations were written out — which a per-rendering
    /// budget could not guarantee, since the legend renders subtrees the
    /// formula stops short of.
    ///
    /// A step is one **operation node expanded**: atoms (leaves, conversions)
    /// and named nodes render as a single token and cost nothing. Past the
    /// budget a node is *elided* — it takes an `eK` label and its children are
    /// not walked — and elision wins over a `cK` placeholder, because a
    /// placeholder's legend line would expand the very subtree the limit just
    /// declined to render.
    /// @param root       The node to start from.
    /// @param expandRoot Whether to expand @p root itself (rather than label it).
    void assignLabels(const ASTNode* root, bool expandRoot) {
        std::vector<LabelFrame> pending;
        pending.push_back(LabelFrame{.node = root, .expandThis = expandRoot});
        while (!pending.empty()) {
            LabelFrame const frame = pending.back();
            pending.pop_back();
            const ASTNode* node = frame.node;
            if (node == nullptr || node->name.has_value()) {
                continue;
            }
            // A node reachable by several displayed paths is settled by its
            // first visit; re-walking it would assign nothing new and, on a
            // DAG, costs one walk per path rather than per node (morph#602:
            // 31 nodes built by repeated `q = q + q` have 2^30 paths and took
            // 10.3 s to render 33 short lines).
            if (!settled.insert(node).second) {
                continue;
            }
            bool const labelIt = !frame.expandThis && isPlaceholder(node);
            // An atom is a token in every rendering, so it is not a step and
            // the budget does not apply to it — only its placeholder does.
            if (isAtomNode(*node)) {
                if (labelIt) {
                    labelIndex.emplace(node, placeholderOrder.size() + 1);
                    placeholderOrder.push_back(node);
                }
                continue;
            }
            if (stepBudget == 0) {
                elisionIndex.emplace(node, elisionOrder.size() + 1);
                elisionOrder.push_back(node);
                continue;
            }
            --stepBudget;
            if (labelIt) {
                labelIndex.emplace(node, placeholderOrder.size() + 1);
                placeholderOrder.push_back(node);
            }
            pending.push_back(LabelFrame{.node = node->right.get(), .expandThis = false});
            pending.push_back(LabelFrame{.node = node->left.get(), .expandThis = false});
        }
    }

    /// @brief Parenthesises and joins a binary subexpression.
    ///
    /// Takes its left operand **by value** and appends to it rather than
    /// concatenating both sides into a fresh string. The left operand of a
    /// left-leaning chain — the shape `total = total + row` records — is the
    /// whole expression rendered so far, so copying it once per level made
    /// rendering quadratic in the depth (morph#582: 27.7 s and a
    /// 350,001-character line at 70,000 steps). Appending makes that shape
    /// linear, amortised. A **right**-leaning chain (`a + (b + (c + …))`) is
    /// still quadratic — the big operand is on the copied side — and so is a
    /// chain of unary negations, which has to prepend; `equation()`'s step
    /// limit is what bounds those, not this.
    /// @param op    The operator token.
    /// @param left  Rendered left operand; moved from, so callers pass an
    ///              operand they are done with.
    /// @param right Rendered right operand.
    /// @return The combined rendering.
    [[nodiscard]] static Rendered combine(const std::string& op, Rendered left, const Rendered& right) {
        int const precedence = (op == "*" || op == "/") ? 2 : 1;
        std::string text = std::move(left.text);
        if (left.precedence < precedence) {
            text.insert(0, 1, '(');
            text += ')';
        }
        text += ' ';
        text += op;
        text += ' ';
        bool const rightNeedsParens =
            (right.precedence < precedence) || (right.precedence == precedence && (op == "-" || op == "/"));
        if (rightNeedsParens) {
            text += '(';
            text += right.text;
            text += ')';
        } else {
            text += right.text;
        }
        return Rendered{.text = std::move(text), .precedence = precedence};
    }

    /// @brief Renders a unary-negation subexpression.
    /// @param operand Rendered operand.
    /// @return The combined rendering.
    [[nodiscard]] static Rendered combineUnary(const Rendered& operand) {
        std::string const text = (operand.precedence <= 1) ? "-(" + operand.text + ")" : "-" + operand.text;
        return Rendered{.text = text, .precedence = 3};
    }

    /// @brief The rendering of a node that stops the walk — a name, a `cK`
    ///        placeholder, a leaf value or a conversion result.
    /// @param node       The node.
    /// @param expandThis Whether to expand @p node even if named/placeholder.
    /// @param mode       Symbolic or substituted.
    /// @return The atom's rendering, or `std::nullopt` when @p node has
    ///         operands that must be rendered first.
    [[nodiscard]] std::optional<Rendered> atomRendering(const ASTNode* node, bool expandThis, RenderMode mode) const {
        // Checked first, and before `isPlaceholder`: a node can be both reused
        // and past the limit, and `assignLabels` gives such a node an `eK` and
        // no `cK` — so asking about the placeholder first would look up a
        // label that was deliberately never assigned. An elided node is never
        // named and never the root, so `expandThis` cannot be set on it.
        if (auto const elided = elisionIndex.find(node); elided != elisionIndex.end()) {
            if (mode == RenderMode::symbolic) {
                return Rendered{.text = "e" + std::to_string(elided->second), .precedence = 100};
            }
            return Rendered{.text = formatOptional(nodeValue(*node)), .precedence = 100};
        }
        if (mode == RenderMode::symbolic) {
            if (node->name.has_value()) {
                return Rendered{.text = "\"" + *node->name + "\"", .precedence = 100};
            }
            if (!expandThis && isPlaceholder(node)) {
                return Rendered{.text = "c" + std::to_string(labelIndex.at(node)), .precedence = 100};
            }
        } else if (!expandThis && (node->name.has_value() || isPlaceholder(node))) {
            return Rendered{.text = formatOptional(nodeValue(*node)), .precedence = 100};
        }
        if (isLeafNode(*node)) {
            return Rendered{.text = formatOptional(node->current.lhs), .precedence = 100};
        }
        if (isConversionNode(*node)) {
            return Rendered{.text = formatOptional(node->current.result), .precedence = 100};
        }
        return std::nullopt;
    }

    /// @brief Renders a node, iteratively.
    ///
    /// The walk is an explicit stack rather than recursion because the
    /// derivation of a running total (`total = total + x` in a loop) is a
    /// linear chain one node deep per iteration, and a recursive walk of it
    /// runs the stack out. Measured on the recursive code (morph#574, 8 MiB
    /// stack): `equation()` returned at 24,000 nodes and segfaulted inside
    /// `renderSymbolic` at 25,000 under clang `-O0`, returned at 50,000 and
    /// segfaulted at 60,000 under clang `-O2`, and segfaulted already at
    /// 40,000 under gcc `-O2`. Optimisation only moved the limit: unlike the
    /// operand destructor chain, this recursion cannot be rewritten into a
    /// loop, because the frames hold live `Rendered` strings across the call.
    ///
    /// Each frame resumes where its recursive twin would have: stage 0 renders
    /// the node or descends left, stage 1 takes the left result and descends
    /// right, stage 2 combines. `finished` carries the rendering of the frame
    /// that just popped, in place of a return value.
    /// @param root       The node to render.
    /// @param expandRoot Whether to expand @p root even if named/placeholder.
    /// @param mode       Symbolic or substituted.
    /// @return The rendering of @p root.
    [[nodiscard]] Rendered render(const ASTNode* root, bool expandRoot, RenderMode mode) const {
        std::vector<RenderFrame> stack;
        stack.push_back(RenderFrame{.node = root, .expandThis = expandRoot});
        Rendered finished;
        while (!stack.empty()) {
            RenderFrame& top = stack.back();
            if (top.stage == 0) {
                if (std::optional<Rendered> atom = atomRendering(top.node, top.expandThis, mode)) {
                    finished = *std::move(atom);
                    stack.pop_back();
                    continue;
                }
                top.stage = 1;
                if (top.node->left) {
                    const ASTNode* child = top.node->left.get();
                    stack.push_back(RenderFrame{.node = child, .expandThis = false});
                    continue;
                }
                finished = Rendered{.text = formatOptional(top.node->current.lhs), .precedence = 100};
                continue;
            }
            if (top.stage == 1) {
                top.left = std::move(finished);
                // Put the slot back into a known state rather than leaving it
                // moved-from: it is read again below, on the pop that follows.
                finished = Rendered{};
                bool const hasRight = top.node->right || top.node->current.rhs.has_value();
                if (!hasRight) {
                    finished = combineUnary(top.left);
                    stack.pop_back();
                    continue;
                }
                top.stage = 2;
                if (top.node->right) {
                    const ASTNode* child = top.node->right.get();
                    stack.push_back(RenderFrame{.node = child, .expandThis = false});
                    continue;
                }
                finished = Rendered{.text = formatOptional(top.node->current.rhs), .precedence = 100};
                continue;
            }
            finished = combine(top.node->current.operation, std::move(top.left), finished);
            stack.pop_back();
        }
        return finished;
    }

    /// @brief Renders a node symbolically (names, placeholders, inlined ops).
    /// @param node       The node.
    /// @param expandThis Whether to expand @p node even if it is a placeholder.
    /// @return The symbolic rendering.
    [[nodiscard]] Rendered renderSymbolic(const ASTNode* node, bool expandThis) const {
        return render(node, expandThis, RenderMode::symbolic);
    }

    /// @brief Renders a node with values substituted for symbols/placeholders.
    /// @param node       The node.
    /// @param expandThis Whether to expand @p node even if named/placeholder.
    /// @return The substituted rendering.
    [[nodiscard]] Rendered renderSubstituted(const ASTNode* node, bool expandThis) const {
        return render(node, expandThis, RenderMode::substituted);
    }

    /// @brief Builds one `where`-legend line for a placeholder.
    /// @param node The placeholder node.
    /// @return The legend body (`cK = ...`).
    [[nodiscard]] std::string legendLine(const ASTNode* node) const {
        std::string const label = "c" + std::to_string(labelIndex.at(node));
        if (isAtomNode(*node)) {
            return label + " = " + formatOptional(nodeValue(*node));
        }
        return label + " = " + renderSymbolic(node, true).text + " = " + renderSubstituted(node, true).text + " = " +
               formatOptional(node->current.result);
    }

    /// @brief Builds one `where`-legend line for an elided sub-derivation.
    ///
    /// Value only, and self-describing: an `eK` stands for work that was *not*
    /// written out, so the line says so and names the limit that cut it rather
    /// than leaving a caller to wonder why a number appeared where a formula
    /// was expected.
    /// @param node The elided node.
    /// @return The legend body (`eK = <value> (elided at the <N>-step limit)`).
    [[nodiscard]] std::string elisionLine(const ASTNode* node) const {
        return "e" + std::to_string(elisionIndex.at(node)) + " = " + formatOptional(nodeValue(*node)) +
               " (elided at the " + std::to_string(maxSteps) + "-step limit)";
    }
};

}  // namespace morph::units::detail

namespace morph::units {

template <auto U, std::uint32_t DeclaredDecimals>
    requires UnitEnum<decltype(U)>
std::vector<std::string> Quantity<U, DeclaredDecimals>::equation(std::size_t maxSteps) const {
    if (!payload) {
        return {detail::formatOptional(payload)};
    }
    // Asked for no steps at all: the same one-element answer a build with
    // tracing compiled out gives, since nothing of the derivation may be shown.
    if (maxSteps == 0) {
        return {detail::formatOptional(payload)};
    }
    const detail::ASTNode* root = _ctx.node.get();
    if (root == nullptr) {
        return {detail::formatOptional(payload)};
    }
    if (root->name.has_value()) {
        return {"\"" + *root->name + "\""};
    }
    if (detail::isLeafNode(*root)) {
        return {detail::formatOptional(root->current.lhs)};
    }
    if (detail::isConversionNode(*root)) {
        return {detail::formatOptional(root->current.result)};
    }

    // The reference count still walks the whole DAG: reuse is a property of
    // the derivation, not of how much of it gets printed, and a value shown
    // once must not be given a placeholder just because the limit hid its
    // other uses. That walk is linear and iterative; only the *rendering* is
    // what `maxSteps` bounds.
    detail::EquationRenderer renderer{maxSteps};
    renderer.countRefs(root);
    renderer.assignLabels(root, true);

    std::vector<std::string> lines;
    lines.push_back(renderer.renderSymbolic(root, true).text);
    lines.push_back("    = " + renderer.renderSubstituted(root, true).text);
    lines.push_back("    = " + detail::formatOptional(root->current.result));
    // Placeholders first, then elisions: both are things the formula referred
    // to by label, in the order the formula introduced them. The first legend
    // line of either kind carries the `where `, the rest align under it.
    bool firstLegendLine = true;
    auto const appendLegend = [&lines, &firstLegendLine](const std::string& body) {
        lines.push_back((firstLegendLine ? "where " : "      ") + body);
        firstLegendLine = false;
    };
    for (const detail::ASTNode* node : renderer.placeholderOrder) {
        appendLegend(renderer.legendLine(node));
    }
    for (const detail::ASTNode* node : renderer.elisionOrder) {
        appendLegend(renderer.elisionLine(node));
    }
    return lines;
}

}  // namespace morph::units

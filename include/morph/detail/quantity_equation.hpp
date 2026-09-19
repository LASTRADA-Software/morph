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
    /// @brief Placeholder number per reused, unnamed node (1-based).
    std::unordered_map<const ASTNode*, std::size_t> labelIndex;

    /// @brief In-edge count per node across displayed (non-opaque) paths.
    std::unordered_map<const ASTNode*, int> refCount;

    /// @brief Nodes visited during ref counting (dedup).
    std::unordered_set<const ASTNode*> seen;

    /// @brief Reused-node placeholders, in first-appearance order.
    std::vector<const ASTNode*> placeholderOrder;

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
        if (root != nullptr) {
            pending.push_back(root);
        }
        while (!pending.empty()) {
            const ASTNode* node = pending.back();
            pending.pop_back();
            if (seen.contains(node)) {
                continue;
            }
            seen.insert(node);
            if (node->name.has_value() || isAtomNode(*node)) {
                continue;
            }
            if (node->left) {
                ++refCount[node->left.get()];
                pending.push_back(node->left.get());
            }
            if (node->right) {
                ++refCount[node->right.get()];
                pending.push_back(node->right.get());
            }
        }
    }

    /// @brief Assigns placeholder labels in first-appearance order.
    ///
    /// Iterative for the same reason as `countRefs`. First appearance is a
    /// left-before-right pre-order, so the right child is pushed first and the
    /// left one popped first — the order a recursive walk would have visited
    /// them in.
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
            if (!frame.expandThis && isPlaceholder(node) && !labelIndex.contains(node)) {
                labelIndex.emplace(node, placeholderOrder.size() + 1);
                placeholderOrder.push_back(node);
            }
            // Both the labelled and the unlabelled arm descend exactly when the
            // node is not an atom, so the two cases share one exit.
            if (isAtomNode(*node)) {
                continue;
            }
            if (node->right) {
                pending.push_back(LabelFrame{.node = node->right.get(), .expandThis = false});
            }
            if (node->left) {
                pending.push_back(LabelFrame{.node = node->left.get(), .expandThis = false});
            }
        }
    }

    /// @brief Parenthesises and joins a binary subexpression.
    /// @param op    The operator token.
    /// @param left  Rendered left operand.
    /// @param right Rendered right operand.
    /// @return The combined rendering.
    [[nodiscard]] static Rendered combine(const std::string& op, const Rendered& left, const Rendered& right) {
        int const precedence = (op == "*" || op == "/") ? 2 : 1;
        std::string const leftText = (left.precedence < precedence) ? "(" + left.text + ")" : left.text;
        bool const rightNeedsParens =
            (right.precedence < precedence) || (right.precedence == precedence && (op == "-" || op == "/"));
        std::string const rightText = rightNeedsParens ? "(" + right.text + ")" : right.text;
        return Rendered{.text = leftText + " " + op + " " + rightText, .precedence = precedence};
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
            finished = combine(top.node->current.operation, top.left, finished);
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
};

}  // namespace morph::units::detail

namespace morph::units {

template <auto U, std::uint32_t DeclaredDecimals>
    requires UnitEnum<decltype(U)>
std::vector<std::string> Quantity<U, DeclaredDecimals>::equation() const {
    if (!payload) {
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

    detail::EquationRenderer renderer;
    renderer.countRefs(root);
    renderer.assignLabels(root, true);

    std::vector<std::string> lines;
    lines.push_back(renderer.renderSymbolic(root, true).text);
    lines.push_back("    = " + renderer.renderSubstituted(root, true).text);
    lines.push_back("    = " + detail::formatOptional(root->current.result));
    for (std::size_t i = 0; i < renderer.placeholderOrder.size(); ++i) {
        std::string const body = renderer.legendLine(renderer.placeholderOrder[i]);
        lines.push_back((i == 0 ? "where " : "      ") + body);
    }
    return lines;
}

}  // namespace morph::units

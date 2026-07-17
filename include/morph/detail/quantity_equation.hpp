// SPDX-License-Identifier: Apache-2.0

#pragma once

/// @file detail/quantity_equation.hpp
/// @brief `Quantity::equation()` renderer (provenance builds only).
///
/// Included by `morph/quantity.hpp` when `MORPH_QUANTITY_PROVENANCE` is on.
/// Walks the shared derivation DAG and produces the print-ready `equation()`
/// lines: formula, substitution, result, and a `where` legend for reused
/// values. See `docs/spec/quantity_type.md` for the output contract.

#include <format>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "../rational.hpp"

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
/// @param node The node.
/// @return The node's optional value.
[[nodiscard]] inline const std::optional<morph::math::Rational>& nodeValue(const ASTNode& node) {
    return isLeafNode(node) ? node.current.lhs : node.current.result;
}

/// @brief A rendered subexpression plus the precedence of its top operator.
struct Rendered {
    /// @brief The rendered text.
    std::string text;

    /// @brief Precedence of the outermost operator (100 for an atom).
    int precedence{100};
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
    ///        Recurses into children unconditionally; the null guard handles the
    ///        absent operands of leaf/unary/scalar steps.
    /// @param node The current node (may be null).
    void countRefs(const ASTNode* node) {
        if (node == nullptr) {
            return;
        }
        if (seen.contains(node)) {
            return;
        }
        seen.insert(node);
        if (node->name.has_value() || isAtomNode(*node)) {
            return;
        }
        if (node->left) {
            ++refCount[node->left.get()];
        }
        if (node->right) {
            ++refCount[node->right.get()];
        }
        countRefs(node->left.get());
        countRefs(node->right.get());
    }

    /// @brief Assigns placeholder labels in first-appearance order.
    /// @param node       The current node.
    /// @param expandThis Whether to expand @p node itself (rather than label it).
    void assignLabels(const ASTNode* node, bool expandThis) {
        if (node == nullptr || node->name.has_value()) {
            return;
        }
        if (!expandThis && isPlaceholder(node)) {
            if (!labelIndex.contains(node)) {
                labelIndex.emplace(node, placeholderOrder.size() + 1);
                placeholderOrder.push_back(node);
            }
            if (!isAtomNode(*node)) {
                assignLabels(node->left.get(), false);
                assignLabels(node->right.get(), false);
            }
            return;
        }
        if (isAtomNode(*node)) {
            return;
        }
        assignLabels(node->left.get(), false);
        assignLabels(node->right.get(), false);
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
        return Rendered{leftText + " " + op + " " + rightText, precedence};
    }

    /// @brief Renders a unary-negation subexpression.
    /// @param operand Rendered operand.
    /// @return The combined rendering.
    [[nodiscard]] static Rendered combineUnary(const Rendered& operand) {
        std::string const text = (operand.precedence <= 1) ? "-(" + operand.text + ")" : "-" + operand.text;
        return Rendered{text, 3};
    }

    /// @brief Renders a node symbolically (names, placeholders, inlined ops).
    /// @param node       The node.
    /// @param expandThis Whether to expand @p node even if it is a placeholder.
    /// @return The symbolic rendering.
    [[nodiscard]] Rendered renderSymbolic(const ASTNode* node, bool expandThis) {
        if (node->name.has_value()) {
            return Rendered{"\"" + *node->name + "\"", 100};
        }
        if (!expandThis && isPlaceholder(node)) {
            return Rendered{"c" + std::to_string(labelIndex.at(node)), 100};
        }
        if (isLeafNode(*node)) {
            return Rendered{formatOptional(node->current.lhs), 100};
        }
        if (isConversionNode(*node)) {
            return Rendered{formatOptional(node->current.result), 100};
        }
        Rendered const left = node->left ? renderSymbolic(node->left.get(), false)
                                         : Rendered{formatOptional(node->current.lhs), 100};
        bool const hasRight = node->right || node->current.rhs.has_value();
        if (!hasRight) {
            return combineUnary(left);
        }
        Rendered const right = node->right ? renderSymbolic(node->right.get(), false)
                                           : Rendered{formatOptional(node->current.rhs), 100};
        return combine(node->current.operation, left, right);
    }

    /// @brief Renders a node with values substituted for symbols/placeholders.
    /// @param node       The node.
    /// @param expandThis Whether to expand @p node even if named/placeholder.
    /// @return The substituted rendering.
    [[nodiscard]] Rendered renderSubstituted(const ASTNode* node, bool expandThis) {
        if (!expandThis && (node->name.has_value() || isPlaceholder(node))) {
            return Rendered{formatOptional(nodeValue(*node)), 100};
        }
        if (isLeafNode(*node)) {
            return Rendered{formatOptional(node->current.lhs), 100};
        }
        if (isConversionNode(*node)) {
            return Rendered{formatOptional(node->current.result), 100};
        }
        Rendered const left = node->left ? renderSubstituted(node->left.get(), false)
                                         : Rendered{formatOptional(node->current.lhs), 100};
        bool const hasRight = node->right || node->current.rhs.has_value();
        if (!hasRight) {
            return combineUnary(left);
        }
        Rendered const right = node->right ? renderSubstituted(node->right.get(), false)
                                           : Rendered{formatOptional(node->current.rhs), 100};
        return combine(node->current.operation, left, right);
    }

    /// @brief Builds one `where`-legend line for a placeholder.
    /// @param node The placeholder node.
    /// @return The legend body (`cK = ...`).
    [[nodiscard]] std::string legendLine(const ASTNode* node) {
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

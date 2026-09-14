#pragma once

#include "../lexer/lexer.hxx"
#include "../parser/parser.hxx"
#include <algorithm>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace kex::ast {

// The lossless syntax tree behind `Kex.AST.parseSyntax` (kexhq/kex#136).
//
// Every token of the source, as written and with the trivia in front of it
// (`tokenTexts`), nested under the nodes the parser completed
// (`Parser::syntaxSpans`). A node owns the tokens its span covers that no
// nested node claims, so printing a node's tokens and nested nodes in order —
// recursively, never slicing the original text — reproduces the source byte
// for byte.
//
// Written once against a builder, like `Converter`, so the interpreter
// (ValueBuilder) and `kex --emit-syntax` (TermBuilder) cannot drift apart.
//
// Answers nullopt only when the tokens cannot account for the source exactly.
// A span that does not nest inside its enclosing node is dropped rather than
// failing the tree: its tokens simply stay with the enclosing node, so the
// reprint stays exact and only that one level of structure is lost.
template<typename Builder>
auto buildSyntaxTree(const Builder& builder, const std::vector<Token>& tokens,
                     std::string_view source,
                     const std::vector<Parser::SyntaxSpan>& spans)
    -> std::optional<typename Builder::Value> {
    using Value = typename Builder::Value;

    auto texts = tokenTexts(tokens, source);
    if (!texts) return std::nullopt;

    // Eof is always last, spans nothing, and carries the trailing trivia; it
    // is the root's final token and never part of a nested node.
    const size_t tokenCount = tokens.size();
    const size_t realTokens =
        tokenCount > 0 && tokens.back().type == TokenType::Eof ? tokenCount - 1
                                                               : tokenCount;
    const auto realEnd = tokens.begin() + static_cast<std::ptrdiff_t>(realTokens);

    // Each span as the half-open token range [first, end) it covers.
    struct Range {
        std::string kind;
        size_t first;
        size_t end;
        size_t order;
    };
    std::vector<Range> ranges;
    for (size_t order = 0; order < spans.size(); ++order) {
        const auto& span = spans[order];
        if (span.startOffset < 0 || span.endOffset <= span.startOffset) continue;
        const auto firstIt = std::lower_bound(
            tokens.begin(), realEnd, span.startOffset,
            [](const Token& token, int offset) { return token.startOffset < offset; });
        const auto endIt = std::upper_bound(
            tokens.begin(), realEnd, span.endOffset,
            [](int offset, const Token& token) { return offset < token.endOffset; });
        const auto first = static_cast<size_t>(firstIt - tokens.begin());
        const auto end = static_cast<size_t>(endIt - tokens.begin());
        if (first >= end) continue;
        ranges.push_back({span.kind, first, end, order});
    }

    // Outer before inner: earlier start first, then the longer range. Two
    // nodes over the same tokens are a wrapper and what it wraps, and the
    // wrapper is the one the parser completed LATER.
    std::sort(ranges.begin(), ranges.end(), [](const Range& a, const Range& b) {
        if (a.first != b.first) return a.first < b.first;
        if (a.end != b.end) return a.end > b.end;
        return a.order > b.order;
    });

    struct Node {
        std::string kind;
        size_t first;
        size_t end;
        std::vector<size_t> children;
    };
    std::vector<Node> nodes;
    nodes.push_back({"Program", 0, realTokens, {}});
    std::vector<size_t> open{0};
    for (const auto& range : ranges) {
        while (open.size() > 1 && nodes[open.back()].end <= range.first)
            open.pop_back();
        const auto parent = open.back();
        if (range.end > nodes[parent].end) continue;  // straddles: not a tree
        // The same node recorded twice adds nothing.
        if (nodes[parent].kind == range.kind && nodes[parent].first == range.first &&
            nodes[parent].end == range.end)
            continue;
        nodes.push_back({range.kind, range.first, range.end, {}});
        const auto index = nodes.size() - 1;
        nodes[parent].children.push_back(index);
        open.push_back(index);
    }

    auto token = [&](size_t index) -> Value {
        return builder.variant(
            "TokenElement", "Kex.AST.SyntaxElement",
            {builder.record("SyntaxToken", {
                {"kind", builder.string(std::string(tokenTypeName(tokens[index].type)))},
                {"text", builder.string(std::string((*texts)[index].raw))},
                {"trivia", builder.string(std::string((*texts)[index].trivia))},
            })});
    };
    std::function<Value(size_t)> emit = [&](size_t index) -> Value {
        const auto& node = nodes[index];
        std::vector<Value> children;
        size_t next = node.first;
        for (const auto child : node.children) {
            for (; next < nodes[child].first; ++next)
                children.push_back(token(next));
            children.push_back(builder.variant("NodeElement", "Kex.AST.SyntaxElement",
                                               {emit(child)}));
            next = nodes[child].end;
        }
        for (; next < node.end; ++next)
            children.push_back(token(next));
        if (index == 0)
            for (size_t trailing = realTokens; trailing < tokenCount; ++trailing)
                children.push_back(token(trailing));
        return builder.record("SyntaxNode", {
            {"kind", builder.string(node.kind)},
            {"children", builder.list(std::move(children))},
        });
    };
    return emit(0);
}

} // namespace kex::ast

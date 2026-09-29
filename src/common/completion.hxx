#pragma once

#include <algorithm>
#include <cctype>
#include <string>
#include <vector>

namespace kex {

// Map the raw token before a dot to a logical type name for DB lookup.
// "[1,2,3]" -> "List", "42" -> "Integer", "'a'" -> "Char",
// "\"hi\"" -> "String", "SomeName" -> "SomeName" (identity).
inline auto qualifierFromBefore(const std::string& before) -> std::string {
    if (before.empty()) return "";
    if (before.back() == ']') return "List";
    if (before.back() == '}') return "Map";
    if (before[0] == '\'' || before[0] == '"') {
        // char vs string literal
        return (before[0] == '\'' && before.size() <= 3) ? "Char" : "String";
    }
    bool allDigits = !before.empty()
        && std::all_of(before.begin(), before.end(),
                       [](char c){ return std::isdigit((unsigned char)c); });
    if (allDigits) return "Integer";
    // Plain identifier / module name
    return before;
}

// Result of resolving a completion query from the readline context.
struct CompletionQuery {
    std::string dbQuery;     // passed to SemanticDB::completionsFor()
    std::string rewriteFrom; // prefix to remove from each result  (e.g. "List.")
    std::string rewriteTo;   // prefix to insert in its place       (e.g. "[1,2,3].")
};

// Given the raw line buffer and the word readline identified as `text`
// (which may or may not have been split at '.'), return a CompletionQuery
// describing:
//   - what to ask the DB
//   - how to rewrite results so readline inserts the right text
//
// Two scenarios:
//   A. readline DID split on '.':  text="ma", start=8, linebuf="[1,2,3].ma"
//      → detect ']' before dot → CompletionQuery{"List.ma", "List.", ""}
//      → completionsFor("List.ma") = ["List.map"]
//      → rewrite: strip "List.", prepend "" → "map"
//      → readline replaces buffer[8..10] with "map" → "[1,2,3].map" ✓
//
//   B. readline did NOT split:     text="[1,2,3].ma", start=0
//      → find '.' in text, before="]", logical="List"
//      → CompletionQuery{"List.ma", "List.", "[1,2,3]."}
//      → completionsFor("List.ma") = ["List.map"]
//      → rewrite: strip "List.", prepend "[1,2,3]." → "[1,2,3].map"
//      → readline replaces buffer[0..10] with "[1,2,3].map" ✓
inline auto resolveCompletionQuery(const char* linebuf, int start,
                                   const char* text) -> CompletionQuery
{
    std::string textStr(text);

    // The token before the dot, taken as a type or module name; literals
    // map to their type. Anything an analysis could type better — a
    // variable, a call, a lambda parameter — is `completeAt`'s job.
    auto resolveQualifier = [](const std::string& before,
                               const std::string& originalDot,
                               const std::string& memberSuffix)
        -> CompletionQuery
    {
        std::string qual = qualifierFromBefore(before);
        return {qual + "." + memberSuffix, qual + ".", originalDot};
    };

    // ── Case A: readline split on '.' ──────────────────────────────────────
    if (start > 0 && linebuf[start - 1] == '.') {
        int i = start - 2;
        if (i >= 0 && linebuf[i] == ']') {
            // List literal receiver → "List".
            return {"List." + textStr, "List.", ""};
        }
        while (i >= 0 && (std::isalnum((unsigned char)linebuf[i])
                           || linebuf[i] == '_'))
            i--;
        std::string before(linebuf + i + 1, linebuf + start - 1);
        if (!before.empty())
            return resolveQualifier(before, "", textStr);
        return {textStr, "", ""};
    }

    // ── Case B: readline did not split; text may contain '.' ───────────────
    auto dotPos = textStr.rfind('.');
    if (dotPos != std::string::npos) {
        std::string beforeDot   = textStr.substr(0, dotPos);
        std::string afterDot    = textStr.substr(dotPos + 1);
        std::string originalDot = beforeDot + ".";
        return resolveQualifier(beforeDot, originalDot, afterDot);
    }

    // ── No dot at all ───────────────────────────────────────────────────────
    return {textStr, "", ""};
}

// Apply a (rewriteFrom → rewriteTo) substitution on the leading prefix of
// each string in `raw`.  Used to turn DB results into what readline inserts.
inline auto rewriteCompletions(std::vector<std::string> raw,
                                const std::string& rewriteFrom,
                                const std::string& rewriteTo)
    -> std::vector<std::string>
{
    if (rewriteFrom.empty()) return raw;
    std::vector<std::string> out;
    out.reserve(raw.size());
    for (auto& s : raw) {
        if (s.rfind(rewriteFrom, 0) == 0)
            out.push_back(rewriteTo + s.substr(rewriteFrom.size()));
        else
            out.push_back(std::move(s));
    }
    return out;
}

// Convenience: strip a prefix (rewriteTo = "").
inline auto stripCompletionPrefix(std::vector<std::string> raw,
                                  const std::string& prefix)
    -> std::vector<std::string>
{
    return rewriteCompletions(std::move(raw), prefix, "");
}

} // namespace kex

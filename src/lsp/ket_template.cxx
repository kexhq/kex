#include "ket_template.hxx"

namespace kex::lsp::ket {
namespace {

// True at a real `<%` opener — not the `<%%` escape for a literal `<%`.
// Mirrors Template.atTagOpen?.
auto isTagOpen(const std::string& s, size_t i) -> bool {
    if (i + 1 >= s.size()) return false;
    if (s[i] != '<' || s[i + 1] != '%') return false;
    if (i + 2 < s.size() && s[i + 2] == '%') return false;
    return true;
}

auto trim(const std::string& s) -> std::string {
    const auto b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return "";
    const auto e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

struct Frontmatter {
    std::string paramsRaw;  // raw text of the `params: [...]` value, if any
    size_t bodyStart = 0;   // byte offset where the template body starts
};

// Mirrors Template.scanFrontmatter/atFrontmatterOpen?/parseFrontmatterLine,
// but only extracts the one key this translator needs (`params`) rather
// than building the full frontmatter map — an editor's hover/diagnostics
// have no other use for the rest of it yet.
auto scanFrontmatter(const std::string& s) -> Frontmatter {
    Frontmatter result;
    const bool opensFrontmatter =
        s.size() >= 3 && s[0] == '-' && s[1] == '-' && s[2] == '-' &&
        (s.size() == 3 || s[3] == '\n' || s[3] == '\r');
    if (!opensFrontmatter) return result;

    size_t firstNewline = s.find('\n', 0);
    size_t pos = firstNewline == std::string::npos ? s.size() : firstNewline + 1;
    while (pos < s.size()) {
        const auto lineEnd = s.find('\n', pos);
        const auto lineStop = lineEnd == std::string::npos ? s.size() : lineEnd;
        std::string line = s.substr(pos, lineStop - pos);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const auto nextPos = lineEnd == std::string::npos ? s.size() : lineEnd + 1;
        const auto trimmed = trim(line);
        if (trimmed == "---") {
            result.bodyStart = nextPos;
            return result;
        }
        if (!trimmed.empty()) {
            if (const auto colon = trimmed.find(':'); colon != std::string::npos) {
                if (trim(trimmed.substr(0, colon)) == "params")
                    result.paramsRaw = trim(trimmed.substr(colon + 1));
            }
        }
        pos = nextPos;
    }
    // Unterminated frontmatter: best-effort — treat the rest of the file as
    // having no body rather than propagating a TemplateError, since a
    // half-typed frontmatter block is exactly what an editor sees mid-edit.
    result.bodyStart = s.size();
    return result;
}

// Splits on commas not nested inside `[]`, `()`, `<>`, or `{}`. Mirrors
// Template.splitTopLevelCommas.
auto splitTopLevelCommas(const std::string& text) -> std::vector<std::string> {
    std::vector<std::string> parts;
    std::string current;
    int depth = 0;
    for (const char ch : text) {
        if (ch == '[' || ch == '(' || ch == '<' || ch == '{') {
            depth++;
            current += ch;
        } else if (ch == ']' || ch == ')' || ch == '>' || ch == '}') {
            depth--;
            current += ch;
        } else if (ch == ',' && depth == 0) {
            parts.push_back(current);
            current.clear();
        } else {
            current += ch;
        }
    }
    if (!current.empty()) parts.push_back(current);
    return parts;
}

struct Param {
    std::string name;
    std::string type;  // "" when the entry declared no `: Type`
};

// Mirrors Parsed#parameters/parseTemplateParams/parseTemplateParam: a
// `params: [name, library: Bool]` frontmatter value, split into declared
// parameters. Anything that is not a `[...]` list (a bare scalar, or the
// key absent) declares no parameters, same as the stdlib scanner.
auto parseParams(const std::string& raw) -> std::vector<Param> {
    std::vector<Param> params;
    if (raw.size() < 2 || raw.front() != '[' || raw.back() != ']') return params;
    const auto inner = trim(raw.substr(1, raw.size() - 2));
    if (inner.empty()) return params;
    for (const auto& item : splitTopLevelCommas(inner)) {
        const auto entry = trim(item);
        if (entry.empty()) continue;
        Param p;
        if (const auto colon = entry.find(':'); colon == std::string::npos) {
            p.name = entry;
        } else {
            p.name = trim(entry.substr(0, colon));
            p.type = trim(entry.substr(colon + 1));
        }
        if (!p.name.empty()) params.push_back(std::move(p));
    }
    return params;
}

}  // namespace

auto translate(const std::string& source) -> Translation {
    Translation result;
    const auto frontmatter = scanFrontmatter(source);
    const auto params = parseParams(frontmatter.paramsRaw);

    result.source = "let __ket_render(";
    for (size_t i = 0; i < params.size(); i++) {
        if (i) result.source += ", ";
        result.source += params[i].name;
        if (!params[i].type.empty()) result.source += ": " + params[i].type;
    }
    result.source += ") do\n";

    const size_t n = source.size();
    size_t pos = frontmatter.bodyStart;
    while (pos < n) {
        // Plain template text and `<%%` escapes carry no Kex and are simply
        // skipped — they never appear in the synthetic source.
        while (pos < n && !isTagOpen(source, pos)) {
            if (pos + 2 < n && source[pos] == '<' && source[pos + 1] == '%' &&
                source[pos + 2] == '%')
                pos += 3;
            else
                pos += 1;
        }
        if (pos >= n) break;

        pos += 2;  // past "<%"
        if (pos < n && source[pos] == '-') pos += 1;  // leftTrim marker

        // Classify: `<%=` interpolate, `<%==` raw, `<%#` comment, else
        // control. Mirrors Template.classifyTag.
        enum class Kind { Control, Interpolate, Raw, Comment } kind = Kind::Control;
        if (pos < n && source[pos] == '=') {
            if (pos + 1 < n && source[pos + 1] == '=') {
                kind = Kind::Raw;
                pos += 2;
            } else {
                kind = Kind::Interpolate;
                pos += 1;
            }
        } else if (pos < n && source[pos] == '#') {
            kind = Kind::Comment;
            pos += 1;
        }

        const size_t bodyStart = pos;
        bool closed = false;
        bool rightTrimsDash = false;
        while (pos < n) {
            if (pos + 2 < n && source[pos] == '-' && source[pos + 1] == '%' &&
                source[pos + 2] == '>') {
                closed = true;
                rightTrimsDash = true;
                break;
            }
            if (pos + 1 < n && source[pos] == '%' && source[pos + 1] == '>') {
                closed = true;
                break;
            }
            pos += 1;
        }
        if (!closed) break;  // UnterminatedTag: stop scanning, best-effort.
        const size_t bodyEnd = pos;
        const std::string body = source.substr(bodyStart, bodyEnd - bodyStart);

        switch (kind) {
            case Kind::Comment:
                // Emits nothing and is never type-checked, same as the
                // compiled-do pipeline: no synthetic text, no segment.
                break;
            case Kind::Interpolate:
            case Kind::Raw: {
                // An expression: wrapped in parens so it stands as its own
                // statement regardless of its own precedence (`a ? b : c`,
                // a trailing block, ...).
                const size_t synStart = result.source.size() + 1;
                result.source += "(";
                result.source += body;
                result.source += ")\n";
                result.segments.push_back({
                    static_cast<int>(synStart),
                    static_cast<int>(synStart + body.size()),
                    static_cast<int>(bodyStart),
                    static_cast<int>(bodyEnd),
                });
                break;
            }
            case Kind::Control: {
                // Already zero or more complete Kex statements (`if x`,
                // `elsif y`, `end`, `let z = ...`) — copied verbatim.
                const size_t synStart = result.source.size();
                result.source += body;
                result.source += "\n";
                result.segments.push_back({
                    static_cast<int>(synStart),
                    static_cast<int>(synStart + body.size()),
                    static_cast<int>(bodyStart),
                    static_cast<int>(bodyEnd),
                });
                break;
            }
        }
        pos = bodyEnd + (rightTrimsDash ? 3 : 2);
    }
    result.source += "end\n";
    return result;
}

auto mapOffset(const std::vector<Segment>& segments, int synOffset) -> int {
    for (const auto& seg : segments)
        if (synOffset >= seg.synStart && synOffset < seg.synEnd)
            return seg.origStart + (synOffset - seg.synStart);
    // A position exactly at a segment's end (exclusive) is common for a
    // diagnostic reported one-past-the-last-character — still resolvable
    // against that same segment.
    for (const auto& seg : segments)
        if (synOffset == seg.synEnd) return seg.origEnd;
    return -1;
}

}  // namespace kex::lsp::ket

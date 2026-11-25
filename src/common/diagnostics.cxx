#include "diagnostics.hxx"

#include "color.hxx"
#include "file_io.hxx"

#include "../compiled/expand.hxx"
#include "../lexer/lexer.hxx"
#include "../parser/parser.hxx"
#include "../semantic/analyzer.hxx"

#include <algorithm>
#include <filesystem>
#include <cctype>
#include <iostream>
#include <map>
#include <optional>
#include <sstream>
#include <string_view>

namespace kex::diagnostics {

// A file under the working directory, as relative to it: where a module was
// loaded from an absolute source root, its diagnostics read the same on every
// machine.
static auto displayPath(std::string_view file) -> std::string {
    std::string path(file);
    std::error_code failed;
    static const auto cwd = std::filesystem::current_path(failed).string() + "/";
    if (!failed && cwd.size() > 1 && path.starts_with(cwd)) {
        return path.substr(cwd.size());
    }
    return path;
}

auto jsonEscape(const std::string& s) -> std::string {
    std::string out;
    out.reserve(s.size() + 4);
    for (char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default: out += c;
        }
    }
    return out;
}

// Extract the "did you mean `X`?" portion from a diagnostic message, if any.
auto extractHint(const std::string& msg) -> std::string {
    const std::string needle = "did you mean `";
    auto pos = msg.find(needle);
    if (pos == std::string::npos) {
        return "";
    }
    pos += needle.size();
    auto end = msg.find('`', pos);
    if (end == std::string::npos) {
        return "";
    }
    return msg.substr(pos, end - pos);
}

namespace {

// How a byte of Kex is shown: in a snippet's source line, and in code a
// message quotes in backticks.
enum class Style : unsigned char { Plain, Keyword, Arrow, Type, Literal, Number, String, Comment };

auto styleOf(TokenType type) -> Style {
    switch (type) {
        case TokenType::True:
        case TokenType::False:
        case TokenType::None:
        case TokenType::Atom: return Style::Literal;
        case TokenType::UpperIdent: return Style::Type;
        case TokenType::Arrow:
        case TokenType::FatArrow: return Style::Arrow;
        case TokenType::Integer:
        case TokenType::Float: return Style::Number;
        case TokenType::String:
        case TokenType::RawString:
        case TokenType::InterpolatedRawString:
        case TokenType::Char: return Style::String;
        default:
            // The keywords come first in TokenType, After through While.
            return type >= TokenType::After && type <= TokenType::While ? Style::Keyword
                                                                        : Style::Plain;
    }
}

auto colorOf(Style style) -> const char* {
    using namespace kex::color;
    switch (style) {
        case Style::Keyword:
        case Style::Arrow: return magenta;
        case Style::Type: return cyan;
        case Style::Literal:
        case Style::Number: return yellow;
        case Style::String: return green;
        case Style::Comment: return gray;
        case Style::Plain: return nullptr;
    }
    return nullptr;
}

// A style for every byte of +text+, from the lexer: a token's bytes take its
// type's style, and a `#` between tokens starts a comment that runs to the end
// of its line. A file that does not lex cleanly is still styled as far as the
// lexer's tokens go, so a lexical error's own line shows what it can.
auto stylesOf(const std::string& text, const std::string& file) -> std::vector<Style> {
    std::vector<Style> styles(text.size(), Style::Plain);
    std::vector<Token> tokens;
    try {
        tokens = Lexer(text, file).tokenizeAll();
    } catch (...) {
        return styles;
    }
    std::size_t covered = 0;
    const auto commentsIn = [&](std::size_t from, std::size_t to) {
        for (auto i = from; i < to; ++i) {
            if (text[i] != '#') {
                continue;
            }
            const auto end = std::min(to, text.find('\n', i));
            std::fill(
                styles.begin() + static_cast<std::ptrdiff_t>(i),
                styles.begin() + static_cast<std::ptrdiff_t>(end), Style::Comment);
            i = end;
        }
    };
    for (const auto& token : tokens) {
        // The end of input carries no text of its own, but its range can
        // cover the whole of a text with no newline at its end: it must not
        // paint over what came before it.
        if (token.type == TokenType::Eof || token.startOffset < 0 ||
            token.endOffset < token.startOffset) {
            continue;
        }
        const auto start = std::min(static_cast<std::size_t>(token.startOffset), text.size());
        const auto end = std::min(static_cast<std::size_t>(token.endOffset), text.size());
        if (start >= covered) {
            commentsIn(covered, start);
        }
        const auto style = styleOf(token.type);
        if (style == Style::Plain) {
            covered = std::max(covered, end);
            continue;
        }
        std::fill(
            styles.begin() + static_cast<std::ptrdiff_t>(start),
            styles.begin() + static_cast<std::ptrdiff_t>(end), style);
        covered = std::max(covered, end);
    }
    commentsIn(covered, text.size());
    return styles;
}

// +text+ with +styles+ (one per byte) as colors. A byte with no style of its
// own takes +plain+, when there is one.
auto renderStyled(std::string_view text, const Style* styles, const char* plain) -> std::string {
    using namespace kex::color;
    if (!enabled) {
        return std::string(text);
    }
    std::string out;
    out.reserve(text.size() * 2);
    const char* current = nullptr;
    for (std::size_t i = 0; i < text.size(); ++i) {
        const char* color = colorOf(styles[i]);
        if (!color && text[i] != ' ') {
            color = plain;
        }
        if (color != current) {
            out += current ? reset : "";
            out += color ? color : "";
            current = color;
        }
        out += text[i];
    }
    if (current) {
        out += reset;
    }
    return out;
}

} // namespace

// Syntax-highlights a diagnostic message body using a palette kept distinct
// from Value::inspect's literal coloring: type names / type vars become cyan
// (matching the REPL/IO.inspect type suffix), function names become bold
// (signature heads), arrows (->) become magenta, and backtick delimiters
// become gray. Code in backticks is highlighted as Kex, as a snippet's source
// line is, with any other name in it bold: most often it names a function.
// Cyan-vs-bold keeps functions and types distinguishable. Honors
// kex::color::enabled.
auto colorizeMessage(const std::string& msg) -> std::string {
    using namespace kex::color;
    const auto isIdentChar = [](char c) -> bool {
        return std::isalnum(static_cast<unsigned char>(c)) || c == '_';
    };
    std::string out;
    out.reserve(msg.size() * 2);
    const auto n = msg.size();
    bool atLineStart = true;
    for (size_t i = 0; i < n;) {
        char c = msg[i];

        if (c == '`') {
            size_t end = msg.find('`', i + 1);
            if (end == std::string::npos) {
                out += c;
                i++;
                continue;
            }
            out += apply(gray);
            out += '`';
            out += apply(reset);
            // Code, highlighted as Kex; a name that is not a keyword or a type
            // is most often a function's, and stays bold.
            const std::string code = msg.substr(i + 1, end - i - 1);
            const auto styles = stylesOf(code, "<message>");
            out += renderStyled(code, styles.data(), apply(bold));
            out += apply(gray);
            out += '`';
            out += apply(reset);
            i = end + 1;
            atLineStart = false;
            continue;
        }

        if (atLineStart) {
            bool startsLower = std::isalpha(static_cast<unsigned char>(c)) &&
                               std::islower(static_cast<unsigned char>(c));
            bool identStart = std::isalpha(static_cast<unsigned char>(c)) || c == '_';
            if (startsLower || (c == '_' && identStart)) {
                size_t j = i + 1;
                while (j < n && isIdentChar(msg[j])) {
                    j++;
                }
                if (j < n && (msg[j] == '?' || msg[j] == '!')) {
                    j++;
                }
                size_t k = j;
                while (k < n && msg[k] == ' ') {
                    k++;
                }
                if (k < n && msg[k] == ':') {
                    out += apply(bold);
                    out.append(msg, i, j - i);
                    out += apply(reset);
                    i = j;
                    atLineStart = false;
                    continue;
                }
            }
        }

        if (c == '-' && i + 1 < n && msg[i + 1] == '>') {
            out += apply(magenta);
            out += "->";
            out += apply(reset);
            i += 2;
            atLineStart = false;
            continue;
        }

        if (std::isupper(static_cast<unsigned char>(c)) && (i == 0 || !isIdentChar(msg[i - 1]))) {
            size_t j = i + 1;
            while (j < n && isIdentChar(msg[j])) {
                j++;
            }
            out += apply(cyan);
            out.append(msg, i, j - i);
            out += apply(reset);
            i = j;
            atLineStart = false;
            continue;
        }

        if (c == '\n') {
            out += c;
            i++;
            atLineStart = true;
            continue;
        }
        out += c;
        i++;
        atLineStart = false;
    }
    return out;
}

// `--collapse-report`: what `compiled do` chain collapse folded away, and why
// the rest did not.
//
// Goes to stderr and says nothing at all for a program with no `compiled`
// block, so it composes with every other mode — you can ask about a run, a
// check or an emit without changing what any of them print.
auto printCollapseReport(const std::vector<kex::compiled::CollapseNote>& notes) -> void {
    if (notes.empty()) {
        return;
    }

    // Source order, not the order collapse worked in: a chain that failed and
    // was retried from the inside reports its outer attempt in a later round,
    // and reading the file top to bottom is what a user wants.
    auto ordered = notes;
    std::stable_sort(ordered.begin(), ordered.end(), [](const auto& a, const auto& b) {
        if (a.location.file != b.location.file) {
            return a.location.file < b.location.file;
        }
        if (a.location.line != b.location.line) {
            return a.location.line < b.location.line;
        }
        return a.location.column < b.location.column;
    });

    // Retrying a failed chain from the inside re-reports the parts it tries
    // again, and everything written inside one `"${...}"` shares that string's
    // position — so the same verdict can arrive several times over. Identical
    // lines say nothing extra. A location with BOTH verdicts keeps both: those
    // are genuinely different sub-expressions that happen to start together.
    ordered.erase(
        std::unique(
            ordered.begin(), ordered.end(),
            [](const auto& a, const auto& b) {
                return a.location.file == b.location.file && a.location.line == b.location.line &&
                       a.location.column == b.location.column && a.collapsed == b.collapsed &&
                       a.reason == b.reason;
            }),
        ordered.end());

    std::size_t collapsed = 0;
    for (const auto& note : ordered) {
        if (note.collapsed) {
            collapsed++;
        }
    }

    std::cerr << kex::color::apply(kex::color::bold)
              << "collapse:" << kex::color::apply(kex::color::reset) << " " << collapsed << " of "
              << ordered.size() << " evaluated at compile time\n";
    for (const auto& note : ordered) {
        std::cerr << "  " << kex::color::apply(kex::color::gray) << displayPath(note.location.file)
                  << ":" << note.location.line << ":" << note.location.column
                  << kex::color::apply(kex::color::reset) << " ";
        if (note.collapsed) {
            std::cerr << kex::color::apply(kex::color::green) << "collapsed"
                      << kex::color::apply(kex::color::reset) << "\n";
        } else {
            std::cerr << kex::color::apply(kex::color::magenta) << "kept"
                      << kex::color::apply(kex::color::reset) << " — " << note.reason << "\n";
        }
    }
}

// `[KEXET-0001]: Error,`: the code naming the kind of problem, then whether
// it is an error or a warning, spelled out rather than left to the code's E or
// W. A diagnostic with no code says `error:` or `warning:` instead. The color
// stops before the comma, which belongs to the sentence, and in a terminal
// that supports it the code links to its page on oops.kex.run.
static auto severityLabel(const kex::semantic::Diagnostic& diag) -> std::string {
    using namespace kex::color;
    const bool isError = diag.level == kex::semantic::Diagnostic::Level::Error;
    std::string out = std::string(apply(bold)) + apply(isError ? red : yellow);
    if (const auto id = kex::diag::idOf(diag.code); !id.empty()) {
        out += "[" + link(kex::diag::urlOf(diag.code), id) + "]: ";
        out += isError ? "Error" : "Warning";
        return out + apply(reset) + ",";
    }
    out += isError ? "error:" : "warning:";
    return out + apply(reset);
}

// --- Source snippets (kexhq/kex#657) ---------------------------------------
//
//   file.kex:2:20: [KEXET-0001]: Error, Type mismatch: expected Integer, got String
//     |
//   2 |   let x: Integer = "a"
//     |          -------   ^^^ this is a String
//     |          |
//     |          expected because of this annotation
//
// The primary span is underlined with `^` in the diagnostic's color, each
// secondary span with `-`; a span's label goes beside its underline when
// nothing lies to its right, and beneath it, joined by `|`, otherwise.

namespace {

// A source file split into lines, read once however many diagnostics name it.
struct SourceLines {
    std::string file;
    std::string text;
    std::vector<std::size_t> starts; // byte offset of each line, 0-based
    // How each byte is shown: worked out the first time a styled line is
    // printed, and only then.
    mutable std::optional<std::vector<Style>> styles;
};

auto sourceLines(const std::string& file) -> const SourceLines* {
    static std::map<std::string, std::optional<SourceLines>> cache;
    auto [it, inserted] = cache.try_emplace(file);
    if (inserted) {
        auto text = readFile(file);
        if (!text.empty()) {
            SourceLines lines{file, std::move(text), {0}, std::nullopt};
            for (std::size_t i = 0; i < lines.text.size(); ++i) {
                if (lines.text[i] == '\n') {
                    lines.starts.push_back(i + 1);
                }
            }
            it->second = std::move(lines);
        }
    }
    return it->second ? &*it->second : nullptr;
}

// The text of a 1-based line, without its newline.
auto lineText(const SourceLines& source, int line) -> std::string_view {
    const auto begin = source.starts[static_cast<std::size_t>(line - 1)];
    auto end = static_cast<std::size_t>(line) < source.starts.size()
                   ? source.starts[static_cast<std::size_t>(line)] - 1
                   : source.text.size();
    if (end > begin && source.text[end - 1] == '\r') {
        --end;
    }
    return std::string_view(source.text).substr(begin, end - begin);
}

// A line of +source+ as it is printed: syntax-highlighted when styling is on.
auto styledLine(const SourceLines& source, std::string_view text) -> std::string {
    if (!kex::color::enabled) {
        return std::string(text);
    }
    if (!source.styles) {
        source.styles = stylesOf(source.text, source.file);
    }
    const auto offset = static_cast<std::size_t>(text.data() - source.text.data());
    return renderStyled(text, source.styles->data() + offset, nullptr);
}

// One underlined span, on one line: byte columns [start, end), 0-based.
struct Mark {
    std::string file;
    int line = 0;
    std::size_t start = 0;
    std::size_t end = 0;
    bool primary = false;
    std::string label;
    // A span running past its first line ends here (1-based line, column).
    int throughLine = 0;
    int throughColumn = 0;
};

auto markOf(
    const SourceLocation& loc,
    const std::optional<SourceLocation>& endLoc,
    bool primary,
    std::string label) -> std::optional<Mark> {
    if (loc.file.empty() || loc.line < 1) {
        return std::nullopt;
    }
    const auto* source = sourceLines(std::string(loc.file));
    if (!source || static_cast<std::size_t>(loc.line) > source->starts.size()) {
        return std::nullopt;
    }
    const auto lineStart = source->starts[static_cast<std::size_t>(loc.line - 1)];
    const auto text = lineText(*source, loc.line);
    Mark mark{std::string(loc.file), loc.line, 0, 0, primary, std::move(label), 0, 0};

    // The byte range, when it lies where the line and column say: a location
    // whose offsets count in another buffer must not underline this one.
    const bool rangeHere = loc.startOffset >= 0 &&
                           static_cast<std::size_t>(loc.startOffset) >= lineStart &&
                           static_cast<std::size_t>(loc.startOffset) <= lineStart + text.size();
    mark.start = rangeHere ? static_cast<std::size_t>(loc.startOffset) - lineStart
                           : static_cast<std::size_t>(std::max(0, loc.column - 1));
    mark.start = std::min(mark.start, text.size());

    // Where the span ends, as a line and a byte offset into the file.
    int endLine = loc.line;
    std::size_t endByte = lineStart + mark.start + 1;
    if (endLoc && endLoc->line >= loc.line &&
        static_cast<std::size_t>(endLoc->line) <= source->starts.size()) {
        endLine = endLoc->line;
        endByte = source->starts[static_cast<std::size_t>(endLine - 1)] +
                  static_cast<std::size_t>(std::max(0, endLoc->column - 1));
    } else if (
        rangeHere && loc.endOffset > loc.startOffset &&
        static_cast<std::size_t>(loc.endOffset) <= source->text.size()) {
        endByte = static_cast<std::size_t>(loc.endOffset);
        const auto after =
            std::upper_bound(source->starts.begin(), source->starts.end(), endByte - 1);
        endLine = static_cast<int>(after - source->starts.begin());
    }

    if (endLine == loc.line) {
        mark.end = std::min(text.size(), endByte - lineStart);
    } else {
        // A node over several lines (a whole `match`) is underlined on its
        // first; only an explicit end (a validator's range) says where it stops.
        mark.end = text.size();
    }
    if (endLine != loc.line && endLoc) {
        mark.throughLine = endLine;
        mark.throughColumn =
            static_cast<int>(endByte - source->starts[static_cast<std::size_t>(endLine - 1)]) + 1;
    }
    if (mark.end <= mark.start) {
        // An empty span, or one at the end of its line: one cell, past the text.
        mark.end = mark.start + 1;
    }
    return mark;
}

// What the bytes before `upTo` of a line take on screen, as padding: a tab
// stays a tab so the underline lines up whatever the terminal's tab width,
// and a multi-byte character takes one cell.
auto padding(std::string_view text, std::size_t upTo) -> std::string {
    std::string out;
    for (std::size_t i = 0; i < upTo; ++i) {
        const auto c = i < text.size() ? static_cast<unsigned char>(text[i]) : ' ';
        if (c == '\t') {
            out += '\t';
        } else if ((c & 0xC0) != 0x80) {
            out += ' ';
        }
    }
    return out;
}

auto renderSnippet(std::vector<Mark> marks, const std::string& mainFile, const char* tone)
    -> std::string {
    using namespace kex::color;
    if (marks.empty()) {
        return {};
    }
    std::stable_sort(marks.begin(), marks.end(), [&](const Mark& a, const Mark& b) {
        const bool aMain = a.file == mainFile;
        const bool bMain = b.file == mainFile;
        if (aMain != bMain) {
            return aMain;
        }
        if (a.file != b.file) {
            return a.file < b.file;
        }
        if (a.line != b.line) {
            return a.line < b.line;
        }
        return a.start < b.start;
    });
    int widest = 0;
    for (const auto& mark : marks) {
        widest = std::max(widest, mark.line);
    }
    const std::string gutter(std::to_string(widest).size(), ' ');
    const auto bar = [&] { return std::string(apply(gray)) + gutter + " |" + apply(reset); };

    std::ostringstream out;
    std::string file;
    int previous = 0;
    for (std::size_t i = 0; i < marks.size();) {
        if (marks[i].file != file) {
            if (!file.empty() || marks[i].file != mainFile) {
                out << apply(gray) << gutter << "--> " << marks[i].file << ":" << marks[i].line
                    << ":" << (marks[i].start + 1) << apply(reset) << "\n";
            }
            file = marks[i].file;
            previous = 0;
            out << bar() << "\n";
        } else if (marks[i].line > previous + 1) {
            out << apply(gray) << gutter << "..." << apply(reset) << "\n";
        }
        const auto line = marks[i].line;
        std::vector<const Mark*> here;
        for (; i < marks.size() && marks[i].file == file && marks[i].line == line; ++i) {
            here.push_back(&marks[i]);
        }
        previous = line;
        const auto* source = sourceLines(file);
        const auto text = lineText(*source, line);
        const auto number = std::to_string(line);
        out << apply(gray) << std::string(gutter.size() - number.size(), ' ') << number << " |"
            << apply(reset) << (text.empty() ? "" : " ") << styledLine(*source, text) << "\n";

        // The underline row: secondary spans first, so a primary one over the
        // same columns shows on top.
        std::size_t width = 0;
        for (const auto* mark : here) {
            width = std::max(width, mark->end);
        }
        std::vector<char> under(width, ' ');
        std::vector<const char*> colorAt(width, nullptr);
        for (int pass = 0; pass < 2; ++pass) {
            for (const auto* mark : here) {
                if (mark->primary != (pass == 1)) {
                    continue;
                }
                for (auto c = mark->start; c < mark->end; ++c) {
                    under[c] = mark->primary ? '^' : '-';
                    colorAt[c] = mark->primary ? tone : cyan;
                }
            }
        }
        const auto colorOf = [&](const Mark* mark) { return mark->primary ? tone : cyan; };
        // The rightmost labelled span says its label inline, when nothing
        // starts after it; the others hang beneath, right to left.
        std::vector<const Mark*> hanging;
        const Mark* inlineLabel = nullptr;
        for (const auto* mark : here) {
            if (!mark->label.empty()) {
                hanging.push_back(mark);
            }
        }
        if (!hanging.empty()) {
            const auto* last = hanging.back();
            const bool nothingAfter = std::none_of(here.begin(), here.end(), [&](const Mark* m) {
                return m != last && m->start >= last->start;
            });
            if (nothingAfter) {
                inlineLabel = last;
                hanging.pop_back();
            }
        }

        std::string row = padding(text, 0);
        for (std::size_t c = 0; c < width;) {
            if (under[c] == ' ') {
                const auto byte = c < text.size() ? static_cast<unsigned char>(text[c]) : ' ';
                if (byte == '\t') {
                    row += '\t';
                } else if ((byte & 0xC0) != 0x80) {
                    row += ' ';
                }
                ++c;
                continue;
            }
            const auto* color = colorAt[c];
            std::string run;
            while (c < width && under[c] != ' ' && colorAt[c] == color) {
                if (c >= text.size() || (static_cast<unsigned char>(text[c]) & 0xC0) != 0x80) {
                    run += under[c];
                }
                ++c;
            }
            row += std::string(apply(color)) + run + apply(reset);
        }
        out << bar() << " " << row;
        if (inlineLabel) {
            out << " " << apply(colorOf(inlineLabel)) << inlineLabel->label << apply(reset);
        }
        out << "\n";

        // Right to left: a row of `|` down from every span still waiting,
        // then the same row with the rightmost one's label in place of its `|`.
        for (auto k = hanging.size(); k-- > 0;) {
            for (int step = 0; step < 2; ++step) {
                std::string row;
                std::size_t at = 0;
                for (std::size_t j = 0; j <= k; ++j) {
                    const auto* mark = hanging[j];
                    row += padding(text, mark->start).substr(padding(text, at).size());
                    at = mark->start + 1;
                    const bool labelHere = step == 1 && j == k;
                    row += std::string(apply(colorOf(mark))) + (labelHere ? mark->label : "|") +
                           apply(reset);
                }
                out << bar() << " " << row << "\n";
            }
        }
        for (const auto* mark : here) {
            if (mark->throughLine) {
                out << bar() << " ... through line " << mark->throughLine << ", column "
                    << mark->throughColumn << "\n";
            }
        }
    }
    return out.str();
}

auto noteHeader(const kex::semantic::DiagnosticNote& note) -> std::string {
    using namespace kex::color;
    std::ostringstream out;
    out << apply(gray) << displayPath(note.location.file) << ":" << note.location.line << ":"
        << note.location.column << ":" << apply(reset) << " " << apply(bold) << apply(cyan)
        << "note:" << apply(reset) << " " << colorizeMessage(note.message) << "\n";
    return out.str();
}

} // namespace

auto formatDiagnostic(const kex::semantic::Diagnostic& diag) -> std::string {
    using namespace kex::color;
    const bool isError = diag.level == kex::semantic::Diagnostic::Level::Error;
    const auto* tone = isError ? red : yellow;
    std::ostringstream out;
    out << apply(gray) << displayPath(diag.location.file) << ":" << diag.location.line << ":"
        << diag.location.column;
    if (diag.endLocation) {
        out << "-" << diag.endLocation->line << ":" << diag.endLocation->column;
    }
    out << ":" << apply(reset) << " " << severityLabel(diag) << " " << colorizeMessage(diag.message)
        << "\n";

    if (!snippets) {
        for (const auto& note : diag.notes) {
            out << noteHeader(note);
        }
        return out.str();
    }

    std::vector<Mark> marks;
    if (auto primary = markOf(diag.location, diag.endLocation, true, diag.label)) {
        marks.push_back(std::move(*primary));
    }
    for (const auto& label : diag.labels) {
        if (auto secondary = markOf(label.location, std::nullopt, false, label.message)) {
            marks.push_back(std::move(*secondary));
        }
    }
    out << renderSnippet(marks, std::string(diag.location.file), tone);

    for (const auto& note : diag.notes) {
        out << noteHeader(note);
        if (auto mark = markOf(note.location, std::nullopt, false, "")) {
            out << renderSnippet({std::move(*mark)}, std::string(note.location.file), cyan);
        }
    }
    return out.str();
}

auto formatBriefDiagnostic(const kex::semantic::Diagnostic& diag) -> std::string {
    return severityLabel(diag) + " " + colorizeMessage(diag.message);
}

auto formatFileDiagnostic(const std::string& path, const kex::semantic::Diagnostic& diag)
    -> std::string {
    using namespace kex::color;
    return std::string(apply(gray)) + path + ":" + apply(reset) + " " +
           formatBriefDiagnostic(diag) + "\n";
}

auto formatBriefException(const std::exception& e, const std::string& indent) -> std::string {
    if (const auto* coded = dynamic_cast<const kex::diag::CodedError*>(&e)) {
        kex::semantic::Diagnostic diag{
            .level = kex::semantic::Diagnostic::Level::Error,
            .message = coded->what(),
            .code = coded->code,
            .args = coded->args,
        };
        return formatBriefDiagnostic(diag);
    }
    const auto* failed = dynamic_cast<const kex::semantic::DiagnosticError*>(&e);
    if (!failed || failed->diagnostics.empty()) {
        return formatBriefError(e.what());
    }
    std::string out;
    for (const auto& diag : failed->diagnostics) {
        if (!out.empty()) {
            out += "\n" + indent;
        }
        out += formatBriefDiagnostic(diag);
    }
    return out;
}

auto formatBriefError(const std::string& message) -> std::string {
    using namespace kex::color;
    return std::string(apply(bold)) + apply(red) + "error:" + apply(reset) + " " +
           colorizeMessage(message);
}

auto printSemanticDiagnostic(const kex::semantic::Diagnostic& diag) -> void {
    std::cerr << formatDiagnostic(diag);
}

auto fromParseDiagnostic(const kex::ParseDiagnostic& diag) -> kex::semantic::Diagnostic {
    return kex::semantic::Diagnostic{
        .level = kex::semantic::Diagnostic::Level::Error,
        .location = diag.location,
        .message = diag.message,
        .code = diag.code,
        .args = diag.args,
    };
}

auto printParseDiagnostic(const kex::ParseDiagnostic& diag) -> void {
    printSemanticDiagnostic(fromParseDiagnostic(diag));
}

auto printAborted(const std::string& what) -> void {
    using namespace kex::color;
    std::cerr << apply(bold) << apply(magenta) << "Aborted:" << apply(reset) << " " << what << "\n";
}

auto errorCountPhrase(int count, const char* kind) -> std::string {
    return std::to_string(count) + " " + kind + (count == 1 ? " error" : " errors");
}

} // namespace kex::diagnostics
